// SPDX-License-Identifier: GPL-3.0-or-later
// FN-JOB-02: HTTP access with global per-provider rate limits.
//
// Rate limits are per provider and global to the process, not per worker: the
// FRD is explicit that a worker pool must not multiply a provider's documented
// request rate. MusicBrainz documents at most one request per second per client,
// and `Retry-After` is honoured.
#pragma once

#include "mlcore/Types.hpp"

#include <atomic>
#include <chrono>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace ml {

struct HttpResponse {
	long statusCode = 0;
	std::string body;
	std::map<std::string, std::string> headers;
	std::string effectiveUrl;
	std::string contentType;
	/// Seconds the server asked us to wait, from `Retry-After`.
	int retryAfterSeconds = 0;

	bool ok() const { return statusCode >= 200 && statusCode < 300; }
	bool rateLimited() const { return statusCode == 429 || statusCode == 503; }
};

struct HttpRequest {
	std::string url;
	std::map<std::string, std::string> headers;
	/// Provider key for rate limiting, for example "musicbrainz".
	std::string providerId;
	int timeoutSeconds = 20;
	/// Maximum response size. A provider returning something enormous is a fault,
	/// not a reason to exhaust memory.
	std::size_t maxBodyBytes = 32u * 1024u * 1024u;
	bool followRedirects = true;
};

/// Token-bucket limiter, one bucket per provider, shared across all callers.
class RateLimiter {
public:
	struct Limit {
		/// Minimum interval between requests.
		std::chrono::milliseconds minimumInterval{0};
		/// Burst allowance.
		int burst = 1;
	};

	void configure(const std::string& providerId, Limit limit);

	/// Blocks until a request to this provider is permitted. Returns false if
	/// the wait was cancelled.
	bool acquire(const std::string& providerId);

	/// Records a server-instructed backoff. Subsequent acquisitions for this
	/// provider wait until it expires.
	void applyRetryAfter(const std::string& providerId, int seconds);

	/// How long a caller would currently wait, for progress display.
	std::chrono::milliseconds estimatedWait(const std::string& providerId) const;

	void cancelAll();
	void reset();

private:
	struct Bucket {
		Limit limit;
		std::chrono::steady_clock::time_point nextAllowed{};
		std::chrono::steady_clock::time_point backoffUntil{};
	};

	mutable std::mutex m_mutex;
	std::map<std::string, Bucket> m_buckets;
	bool m_cancelled = false;
};

/// Thin libcurl wrapper. Thread-safe; one easy handle per call.
class HttpClient {
public:
	HttpClient();
	~HttpClient();

	HttpClient(const HttpClient&) = delete;
	HttpClient& operator=(const HttpClient&) = delete;

	/// Performs a GET, waiting for the provider's rate limit first.
	Result<HttpResponse> get(const HttpRequest& request);

	/// Downloads binary content, enforcing `maxBodyBytes`.
	Result<std::vector<std::uint8_t>> download(const HttpRequest& request, std::string& contentTypeOut);

	RateLimiter& rateLimiter() { return m_rateLimiter; }

	/// The User-Agent sent with every request. MusicBrainz requires a meaningful
	/// one that identifies the application and a contact route.
	static std::string userAgent();

	/// Applies the documented limits for the providers this application uses.
	void configureDefaultLimits();

	/// Disables network access entirely. Every request then fails with
	/// NetworkError, which is how the offline-operation test runs.
	void setOfflineMode(bool offline) { m_offline = offline; }
	bool offlineMode() const { return m_offline; }

	void cancel();

private:
	RateLimiter m_rateLimiter;
	bool m_offline = false;
	std::atomic<bool> m_cancelled{false};
};

} // namespace ml
