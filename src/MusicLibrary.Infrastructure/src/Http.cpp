// SPDX-License-Identifier: GPL-3.0-or-later
#include "mlinfra/Http.hpp"

#include <curl/curl.h>
#include <mlversion/Version.hpp>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <thread>

namespace ml {

namespace {

/// libcurl global initialisation, done exactly once and torn down at exit.
struct CurlGlobal {
	CurlGlobal() { curl_global_init(CURL_GLOBAL_DEFAULT); }
	~CurlGlobal() { curl_global_cleanup(); }
};

void ensureCurlInitialised() {
	static CurlGlobal global;
	(void)global;
}

std::size_t writeToString(char* data, std::size_t size, std::size_t count, void* userData) {
	auto* out = static_cast<std::string*>(userData);
	out->append(data, size * count);
	return size * count;
}

struct BoundedBuffer {
	std::vector<std::uint8_t> bytes;
	std::size_t limit = 0;
	bool exceeded = false;
};

std::size_t writeToBuffer(char* data, std::size_t size, std::size_t count, void* userData) {
	auto* buffer = static_cast<BoundedBuffer*>(userData);
	const std::size_t total = size * count;
	if (buffer->bytes.size() + total > buffer->limit) {
		buffer->exceeded = true;
		return 0;   // Aborts the transfer.
	}
	const auto* begin = reinterpret_cast<const std::uint8_t*>(data);
	buffer->bytes.insert(buffer->bytes.end(), begin, begin + total);
	return total;
}

std::size_t collectHeader(char* data, std::size_t size, std::size_t count, void* userData) {
	auto* headers = static_cast<std::map<std::string, std::string>*>(userData);
	const std::size_t total = size * count;
	std::string line(data, total);

	const std::size_t colon = line.find(':');
	if (colon != std::string::npos) {
		std::string key = line.substr(0, colon);
		std::string value = line.substr(colon + 1);
		std::transform(key.begin(), key.end(), key.begin(),
			[](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		// Trim surrounding whitespace and the trailing CRLF.
		const auto trim = [](std::string& s) {
			while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' ')) s.pop_back();
			std::size_t begin = 0;
			while (begin < s.size() && s[begin] == ' ') ++begin;
			s.erase(0, begin);
		};
		trim(key);
		trim(value);
		(*headers)[key] = value;
	}
	return total;
}

} // namespace

// ---------------------------------------------------------------------------
// RateLimiter
// ---------------------------------------------------------------------------

void RateLimiter::configure(const std::string& providerId, Limit limit) {
	std::lock_guard<std::mutex> lock(m_mutex);
	m_buckets[providerId].limit = limit;
}

bool RateLimiter::acquire(const std::string& providerId) {
	// The wait happens outside the lock so one slow provider does not block
	// requests to a different one.
	while (true) {
		std::chrono::steady_clock::time_point waitUntil;
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			if (m_cancelled) return false;

			auto& bucket = m_buckets[providerId];
			const auto now = std::chrono::steady_clock::now();

			auto ready = bucket.nextAllowed;
			if (bucket.backoffUntil > ready) ready = bucket.backoffUntil;

			if (ready <= now) {
				bucket.nextAllowed = now + bucket.limit.minimumInterval;
				return true;
			}
			waitUntil = ready;
		}

		// Sleep in short slices so cancellation is responsive.
		const auto now = std::chrono::steady_clock::now();
		auto remaining = waitUntil - now;
		if (remaining > std::chrono::milliseconds(100)) remaining = std::chrono::milliseconds(100);
		if (remaining > std::chrono::milliseconds(0)) std::this_thread::sleep_for(remaining);
	}
}

void RateLimiter::applyRetryAfter(const std::string& providerId, int seconds) {
	if (seconds <= 0) return;
	std::lock_guard<std::mutex> lock(m_mutex);
	auto& bucket = m_buckets[providerId];
	const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
	if (until > bucket.backoffUntil) bucket.backoffUntil = until;
}

std::chrono::milliseconds RateLimiter::estimatedWait(const std::string& providerId) const {
	std::lock_guard<std::mutex> lock(m_mutex);
	auto it = m_buckets.find(providerId);
	if (it == m_buckets.end()) return std::chrono::milliseconds(0);

	const auto now = std::chrono::steady_clock::now();
	auto ready = it->second.nextAllowed;
	if (it->second.backoffUntil > ready) ready = it->second.backoffUntil;
	if (ready <= now) return std::chrono::milliseconds(0);
	return std::chrono::duration_cast<std::chrono::milliseconds>(ready - now);
}

void RateLimiter::cancelAll() {
	std::lock_guard<std::mutex> lock(m_mutex);
	m_cancelled = true;
}

void RateLimiter::reset() {
	std::lock_guard<std::mutex> lock(m_mutex);
	m_cancelled = false;
	m_buckets.clear();
}

// ---------------------------------------------------------------------------
// HttpClient
// ---------------------------------------------------------------------------

HttpClient::HttpClient() {
	ensureCurlInitialised();
	configureDefaultLimits();
}

HttpClient::~HttpClient() = default;

std::string HttpClient::userAgent() {
	// MusicBrainz requires an application name, version and contact route.
	return std::string(version::kProjectName) + "/" + std::string(version::kVersion)
		+ " ( " + std::string(version::kHomepage) + " )";
}

void HttpClient::configureDefaultLimits() {
	// MusicBrainz: at most one request per second per client, documented.
	m_rateLimiter.configure("musicbrainz", {std::chrono::milliseconds(1100), 1});
	// iTunes Search: approximately 20 calls per minute, documented as subject to
	// change. 3.2 seconds keeps a margin under that.
	m_rateLimiter.configure("itunes", {std::chrono::milliseconds(3200), 1});
	// AcoustID publishes usage limits; three per second is its documented cap.
	m_rateLimiter.configure("acoustid", {std::chrono::milliseconds(350), 1});
	// Cover Art Archive is a redirect service in front of the Internet Archive.
	m_rateLimiter.configure("coverartarchive", {std::chrono::milliseconds(1100), 1});
	// LRCLIB publishes no hard limit; this is a courtesy rate.
	m_rateLimiter.configure("lrclib", {std::chrono::milliseconds(400), 1});
	// Image downloads from a CDN.
	m_rateLimiter.configure("cdn", {std::chrono::milliseconds(100), 1});
}

void HttpClient::cancel() {
	m_cancelled.store(true, std::memory_order_relaxed);
	m_rateLimiter.cancelAll();
}

namespace {

int parseRetryAfter(const std::map<std::string, std::string>& headers) {
	auto it = headers.find("retry-after");
	if (it == headers.end()) return 0;
	try {
		// The header can also carry an HTTP date; a non-numeric value falls back
		// to a conservative fixed wait rather than being ignored.
		return std::max(0, std::stoi(it->second));
	} catch (...) {
		return 60;
	}
}

} // namespace

Result<HttpResponse> HttpClient::get(const HttpRequest& request) {
	if (m_offline) {
		return Error{ErrorCode::NetworkError,
			"offline mode is enabled; remote enrichment is unavailable while local operations continue"};
	}
	if (m_cancelled.load(std::memory_order_relaxed)) {
		return Error{ErrorCode::Cancelled, "cancelled"};
	}
	if (request.url.rfind("https://", 0) != 0 && request.url.rfind("http://", 0) != 0) {
		return Error{ErrorCode::InvalidArgument, "only http and https URLs are permitted"};
	}

	if (!m_rateLimiter.acquire(request.providerId)) {
		return Error{ErrorCode::Cancelled, "cancelled while waiting for the provider rate limit"};
	}

	CURL* handle = curl_easy_init();
	if (!handle) {
		return Error{ErrorCode::Internal, "cannot create an HTTP handle"};
	}

	HttpResponse response;
	curl_slist* headerList = nullptr;

	// Everything below is released by this guard, including on an early return.
	struct Cleanup {
		CURL* handle;
		curl_slist** headers;
		~Cleanup() {
			if (*headers) curl_slist_free_all(*headers);
			if (handle) curl_easy_cleanup(handle);
		}
	} cleanup{handle, &headerList};

	const std::string agent = userAgent();
	headerList = curl_slist_append(headerList, ("User-Agent: " + agent).c_str());
	for (const auto& [key, value] : request.headers) {
		headerList = curl_slist_append(headerList, (key + ": " + value).c_str());
	}

	curl_easy_setopt(handle, CURLOPT_URL, request.url.c_str());
	curl_easy_setopt(handle, CURLOPT_HTTPHEADER, headerList);
	curl_easy_setopt(handle, CURLOPT_USERAGENT, agent.c_str());
	curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, writeToString);
	curl_easy_setopt(handle, CURLOPT_WRITEDATA, &response.body);
	curl_easy_setopt(handle, CURLOPT_HEADERFUNCTION, collectHeader);
	curl_easy_setopt(handle, CURLOPT_HEADERDATA, &response.headers);
	curl_easy_setopt(handle, CURLOPT_TIMEOUT, static_cast<long>(request.timeoutSeconds));
	curl_easy_setopt(handle, CURLOPT_CONNECTTIMEOUT, 10L);
	curl_easy_setopt(handle, CURLOPT_FOLLOWLOCATION, request.followRedirects ? 1L : 0L);
	curl_easy_setopt(handle, CURLOPT_MAXREDIRS, 5L);
	curl_easy_setopt(handle, CURLOPT_ACCEPT_ENCODING, "");
	curl_easy_setopt(handle, CURLOPT_NOSIGNAL, 1L);
	// TLS verification is never relaxed.
	curl_easy_setopt(handle, CURLOPT_SSL_VERIFYPEER, 1L);
	curl_easy_setopt(handle, CURLOPT_SSL_VERIFYHOST, 2L);
	// Only these two schemes, including after a redirect.
	curl_easy_setopt(handle, CURLOPT_PROTOCOLS_STR, "http,https");
	curl_easy_setopt(handle, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");

	const CURLcode code = curl_easy_perform(handle);
	if (code != CURLE_OK) {
		return Error{ErrorCode::NetworkError,
			std::string("request to ") + request.url + " failed: " + curl_easy_strerror(code)};
	}

	curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &response.statusCode);

	char* effective = nullptr;
	if (curl_easy_getinfo(handle, CURLINFO_EFFECTIVE_URL, &effective) == CURLE_OK && effective) {
		response.effectiveUrl = effective;
	}
	char* contentType = nullptr;
	if (curl_easy_getinfo(handle, CURLINFO_CONTENT_TYPE, &contentType) == CURLE_OK && contentType) {
		response.contentType = contentType;
	}

	response.retryAfterSeconds = parseRetryAfter(response.headers);
	if (response.rateLimited()) {
		// Honour the server's instruction rather than retrying on our own schedule.
		m_rateLimiter.applyRetryAfter(request.providerId,
			response.retryAfterSeconds > 0 ? response.retryAfterSeconds : 30);
		return Error{ErrorCode::RateLimited,
			"provider " + request.providerId + " returned " + std::to_string(response.statusCode)};
	}

	return response;
}

Result<std::vector<std::uint8_t>> HttpClient::download(const HttpRequest& request, std::string& contentTypeOut) {
	if (m_offline) {
		return Error{ErrorCode::NetworkError, "offline mode is enabled; downloads are unavailable"};
	}
	if (!m_rateLimiter.acquire(request.providerId)) {
		return Error{ErrorCode::Cancelled, "cancelled while waiting for the provider rate limit"};
	}

	CURL* handle = curl_easy_init();
	if (!handle) {
		return Error{ErrorCode::Internal, "cannot create an HTTP handle"};
	}

	BoundedBuffer buffer;
	buffer.limit = request.maxBodyBytes;
	std::map<std::string, std::string> headers;
	curl_slist* headerList = nullptr;

	struct Cleanup {
		CURL* handle;
		curl_slist** headers;
		~Cleanup() {
			if (*headers) curl_slist_free_all(*headers);
			if (handle) curl_easy_cleanup(handle);
		}
	} cleanup{handle, &headerList};

	const std::string agent = userAgent();
	headerList = curl_slist_append(headerList, ("User-Agent: " + agent).c_str());
	for (const auto& [key, value] : request.headers) {
		headerList = curl_slist_append(headerList, (key + ": " + value).c_str());
	}

	curl_easy_setopt(handle, CURLOPT_URL, request.url.c_str());
	curl_easy_setopt(handle, CURLOPT_HTTPHEADER, headerList);
	curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, writeToBuffer);
	curl_easy_setopt(handle, CURLOPT_WRITEDATA, &buffer);
	curl_easy_setopt(handle, CURLOPT_HEADERFUNCTION, collectHeader);
	curl_easy_setopt(handle, CURLOPT_HEADERDATA, &headers);
	curl_easy_setopt(handle, CURLOPT_TIMEOUT, static_cast<long>(request.timeoutSeconds));
	curl_easy_setopt(handle, CURLOPT_CONNECTTIMEOUT, 10L);
	curl_easy_setopt(handle, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(handle, CURLOPT_MAXREDIRS, 5L);
	curl_easy_setopt(handle, CURLOPT_NOSIGNAL, 1L);
	curl_easy_setopt(handle, CURLOPT_SSL_VERIFYPEER, 1L);
	curl_easy_setopt(handle, CURLOPT_SSL_VERIFYHOST, 2L);
	curl_easy_setopt(handle, CURLOPT_PROTOCOLS_STR, "http,https");
	curl_easy_setopt(handle, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");

	const CURLcode code = curl_easy_perform(handle);

	if (buffer.exceeded) {
		return Error{ErrorCode::Unsupported,
			"response exceeded the " + std::to_string(request.maxBodyBytes) + " byte limit"};
	}
	if (code != CURLE_OK) {
		return Error{ErrorCode::NetworkError,
			std::string("download from ") + request.url + " failed: " + curl_easy_strerror(code)};
	}

	long status = 0;
	curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &status);
	if (status < 200 || status >= 300) {
		if (status == 429 || status == 503) {
			m_rateLimiter.applyRetryAfter(request.providerId, std::max(parseRetryAfter(headers), 30));
			return Error{ErrorCode::RateLimited, "download rate limited (" + std::to_string(status) + ")"};
		}
		return Error{ErrorCode::NetworkError, "download returned HTTP " + std::to_string(status)};
	}

	char* contentType = nullptr;
	if (curl_easy_getinfo(handle, CURLINFO_CONTENT_TYPE, &contentType) == CURLE_OK && contentType) {
		contentTypeOut = contentType;
	}

	if (buffer.bytes.empty()) {
		return Error{ErrorCode::NetworkError, "download returned an empty body"};
	}
	return buffer.bytes;
}

} // namespace ml
