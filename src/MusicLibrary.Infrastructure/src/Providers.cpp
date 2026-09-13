// SPDX-License-Identifier: GPL-3.0-or-later
#include "mlinfra/Providers.hpp"
#include "mlcore/Text.hpp"
#include "mlinfra/ImagePipeline.hpp"
#include "mlinfra/Json.hpp"

#include <algorithm>
#include <cstdio>
#include <fstream>

namespace fs = std::filesystem;

namespace ml {

std::string urlEncode(std::string_view text) {
	static constexpr char kHex[] = "0123456789ABCDEF";
	std::string out;
	out.reserve(text.size() * 3);
	for (unsigned char c : text) {
		if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
			out.push_back(static_cast<char>(c));
		} else {
			out.push_back('%');
			out.push_back(kHex[c >> 4]);
			out.push_back(kHex[c & 0x0F]);
		}
	}
	return out;
}

// ---------------------------------------------------------------------------
// ITunesProvider
// ---------------------------------------------------------------------------

ITunesProvider::ITunesProvider(HttpClient& http, std::string storefront)
	: m_http(http), m_storefront(std::move(storefront)) {}

std::string ITunesProvider::rewriteArtworkUrl(const std::string& url, int requestedEdge) {
	// iTunes returns URLs ending in "/100x100bb.jpg". Substituting a larger size
	// usually works, but this is an undocumented convention: the caller must
	// verify by decoding the downloaded bytes, and record the measured size.
	const std::size_t lastSlash = url.find_last_of('/');
	if (lastSlash == std::string::npos) return url;

	const std::string tail = url.substr(lastSlash + 1);
	const std::size_t x = tail.find('x');
	if (x == std::string::npos) return url;

	// The tail must look like "<digits>x<digits><suffix>".
	bool leadingDigits = x > 0;
	for (std::size_t i = 0; i < x; ++i) {
		if (!std::isdigit(static_cast<unsigned char>(tail[i]))) { leadingDigits = false; break; }
	}
	if (!leadingDigits) return url;

	std::size_t i = x + 1;
	while (i < tail.size() && std::isdigit(static_cast<unsigned char>(tail[i]))) ++i;
	if (i == x + 1) return url;

	const std::string suffix = tail.substr(i);
	const std::string size = std::to_string(requestedEdge);
	return url.substr(0, lastSlash + 1) + size + "x" + size + suffix;
}

Result<std::vector<ReleaseCandidate>> ITunesProvider::findReleases(const AlbumQuery& query) {
	if (query.album.empty()) {
		return Error{ErrorCode::InvalidArgument, "an album title is required for an iTunes lookup"};
	}

	std::vector<std::string> storefronts{m_storefront};
	for (const auto& fallback : m_fallbackStorefronts) {
		if (fallback != m_storefront) storefronts.push_back(fallback);
	}

	std::vector<ReleaseCandidate> all;
	std::string lastError;

	for (const auto& storefront : storefronts) {
		const std::string term = urlEncode(text::trim(query.albumArtist + " " + query.album));
		HttpRequest request;
		request.providerId = "itunes";
		request.url = "https://itunes.apple.com/search?term=" + term
			+ "&entity=album&limit=12&country=" + urlEncode(storefront);

		auto response = m_http.get(request);
		if (!response) {
			lastError = response.error().message;
			// A rate limit or outage must not silently become "no results".
			if (response.error().code == ErrorCode::RateLimited) return response.error();
			continue;
		}
		if (!response.value().ok()) {
			lastError = "HTTP " + std::to_string(response.value().statusCode);
			continue;
		}

		auto parsed = Json::parse(response.value().body);
		if (!parsed) {
			lastError = parsed.error().message;
			continue;
		}

		for (const auto& item : parsed.value()["results"].asArray()) {
			ReleaseCandidate candidate;
			candidate.providerId = id();
			candidate.providerRef = item["collectionId"].asString();
			candidate.album = item["collectionName"].asString();
			candidate.albumArtist = item["artistName"].asString();
			candidate.date = item["releaseDate"].asString().substr(0, 10);
			candidate.trackCount = static_cast<int>(item["trackCount"].asInt());
			candidate.pageUrl = item["collectionViewUrl"].asString();
			candidate.artworkUrlTemplate = item["artworkUrl100"].asString();

			// Evidence, gathered before any confidence is claimed.
			//
			// Comparison uses the edition-aware helpers, not raw edit distance:
			// a catalogue writes "Desire - Single" and credits
			// "Calvin Harris & Sam Smith" where the local tags say "Desire" and
			// "Calvin Harris". Raw similarity scores those pairs at 0.46 and 0.62
			// and would discard the correct release.
			const text::TitleComparison titleMatch = text::compareTitles(query.album, candidate.album);
			const bool haveArtist = !query.albumArtist.empty();
			const text::ArtistComparison artistMatch = haveArtist
				? text::compareArtists(query.albumArtist, candidate.albumArtist)
				: text::ArtistComparison{};

			candidate.evidence.push_back({"title_similarity",
				"album title similarity " + std::to_string(titleMatch.similarity)
					+ " comparing \"" + query.album + "\" with \"" + candidate.album + "\"",
				titleMatch.similarity > 0.85});
			if (!candidate.edition.empty() || !titleMatch.rightMarker.empty()) {
				candidate.edition = titleMatch.rightMarker;
				candidate.evidence.push_back({"release_marker",
					"catalogue labels this release \"" + titleMatch.rightMarker + "\"",
					titleMatch.markersAgree});
			}
			if (haveArtist) {
				candidate.evidence.push_back({"artist_similarity",
					"artist credit similarity " + std::to_string(artistMatch.similarity),
					artistMatch.similarity > 0.8});
				if (!artistMatch.extraCredits.empty()) {
					candidate.evidence.push_back({"extra_credits",
						"catalogue credits also name " + text::join(artistMatch.extraCredits, ", "),
						artistMatch.oneContainsTheOther});
				}
			}
			if (query.trackCount > 0 && candidate.trackCount > 0) {
				const bool match = query.trackCount == candidate.trackCount;
				candidate.evidence.push_back({"track_count",
					"local group has " + std::to_string(query.trackCount) + " tracks, release declares "
						+ std::to_string(candidate.trackCount), match});
			}
			candidate.evidence.push_back({"storefront", "matched in the " + storefront + " storefront", true});

			const bool titleOk = titleMatch.similarity > 0.85;
			const bool artistOk = !haveArtist || artistMatch.similarity > 0.8;
			const bool countOk = query.trackCount <= 0 || candidate.trackCount <= 0
				|| query.trackCount == candidate.trackCount;

			if (titleOk && artistOk && countOk && titleMatch.markersAgree) {
				candidate.confidence = Confidence::Moderate;
			} else if (titleOk && artistOk) {
				// The release exists but something disagrees: a different edition,
				// or a track count that does not match. Weak means "show this for
				// review", not "discard it".
				candidate.confidence = Confidence::Weak;
			} else {
				candidate.confidence = Confidence::Unknown;
			}

			// The same release is listed in every storefront. Deduplicate on the
			// catalogue's own identifier so a fallback sweep does not present the
			// reviewer with four copies of each release.
			const bool alreadyPresent = std::any_of(all.begin(), all.end(),
				[&](const ReleaseCandidate& existing) {
					return !candidate.providerRef.empty() && existing.providerRef == candidate.providerRef;
				});
			if (!alreadyPresent) all.push_back(std::move(candidate));
		}

		// Stop at the first storefront that produced a usable match; the FRD asks
		// for controlled fallback, not for querying every storefront every time.
		const bool usable = std::any_of(all.begin(), all.end(),
			[](const ReleaseCandidate& c) { return c.confidence >= Confidence::Moderate; });
		if (usable) break;
	}

	if (all.empty() && !lastError.empty()) {
		return Error{ErrorCode::NetworkError, "iTunes lookup failed: " + lastError};
	}
	return all;
}

Result<std::vector<ArtworkCandidate>> ITunesProvider::findArtwork(const AlbumQuery& query) {
	auto releases = findReleases(query);
	if (!releases) return releases.error();

	std::vector<ArtworkCandidate> candidates;
	for (const auto& release : releases.value()) {
		if (release.artworkUrlTemplate.empty()) continue;
		// Weak candidates are kept. Discarding them here would hide a real
		// release from review and leave the album looking like "nothing found";
		// the artwork policy is what decides between selecting and reviewing.
		if (release.confidence == Confidence::Unknown) continue;

		ArtworkCandidate candidate;
		candidate.providerId = id();
		candidate.providerName = displayName();
		candidate.pageUrl = release.pageUrl;
		// Ask for a large size. The measured size, not this request, is what the
		// selection policy is allowed to use.
		candidate.imageUrl = rewriteArtworkUrl(release.artworkUrlTemplate, 3000);
		candidate.matchConfidence = release.confidence;

		// A streaming catalogue's asset is an original digital asset in the usual
		// case, but that is a claim until the image itself supports it.
		candidate.sourceType = ArtworkSourceType::EvidencedDigitalAsset;
		candidate.evidence = release.evidence;
		candidate.evidence.push_back({"provider_type",
			"supplied by a streaming catalogue, which is evidence of a digital asset but not proof of "
			"its condition or of the intended edition", true});

		// Whether this is the *intended* cover for the local release cannot be
		// established from a search result alone.
		const bool editionMatches = release.edition.empty()
			|| text::normaliseForMatching(release.edition) == text::normaliseForMatching(query.album);
		candidate.coverMatch = (release.confidence >= Confidence::Moderate && editionMatches)
			? CoverMatch::IntendedCover
			: CoverMatch::Unknown;

		candidates.push_back(std::move(candidate));
	}

	// Best evidence first, and bounded: a review screen showing fifty covers is
	// not a review screen.
	std::stable_sort(candidates.begin(), candidates.end(),
		[](const ArtworkCandidate& a, const ArtworkCandidate& b) {
			return a.matchConfidence > b.matchConfidence;
		});
	if (candidates.size() > 12) candidates.resize(12);
	return candidates;
}

Result<std::vector<std::uint8_t>> ITunesProvider::fetchImage(const ArtworkCandidate& candidate,
	std::string& contentTypeOut) {
	HttpRequest request;
	request.providerId = "cdn";
	request.url = candidate.imageUrl;
	request.maxBodyBytes = 24u * 1024u * 1024u;

	auto bytes = m_http.download(request, contentTypeOut);
	if (bytes) return bytes;

	// The high-resolution rewrite is undocumented and can 404. Fall back through
	// smaller requested sizes rather than giving up on the candidate.
	for (int edge : {2400, 1800, 1200, 600}) {
		request.url = rewriteArtworkUrl(candidate.imageUrl, edge);
		if (request.url == candidate.imageUrl) continue;
		auto retry = m_http.download(request, contentTypeOut);
		if (retry) return retry;
	}
	return bytes.error();
}

// ---------------------------------------------------------------------------
// CoverArtArchiveProvider
// ---------------------------------------------------------------------------

CoverArtArchiveProvider::CoverArtArchiveProvider(HttpClient& http) : m_http(http) {}

Result<std::vector<ArtworkCandidate>> CoverArtArchiveProvider::findArtwork(const AlbumQuery& query) {
	if (query.musicBrainzAlbumId.empty()) {
		// Not an error: this provider simply has no way in without a release id.
		return std::vector<ArtworkCandidate>{};
	}

	HttpRequest request;
	request.providerId = "coverartarchive";
	request.url = "https://coverartarchive.org/release/" + urlEncode(query.musicBrainzAlbumId);

	auto response = m_http.get(request);
	if (!response) {
		if (response.error().code == ErrorCode::RateLimited) return response.error();
		return response.error();
	}
	if (response.value().statusCode == 404) {
		return std::vector<ArtworkCandidate>{};
	}
	if (!response.value().ok()) {
		return Error{ErrorCode::NetworkError,
			"Cover Art Archive returned HTTP " + std::to_string(response.value().statusCode)};
	}

	auto parsed = Json::parse(response.value().body);
	if (!parsed) return parsed.error();

	std::vector<ArtworkCandidate> candidates;
	for (const auto& image : parsed.value()["images"].asArray()) {
		const bool isFront = image["front"].asBool();
		if (!isFront) continue;   // Other roles are inventoried elsewhere.

		ArtworkCandidate candidate;
		candidate.providerId = id();
		candidate.providerName = displayName();
		candidate.pageUrl = "https://musicbrainz.org/release/" + query.musicBrainzAlbumId;
		candidate.imageUrl = image["image"].asString();
		if (const auto& thumbnails = image["thumbnails"]; thumbnails.contains("1200")) {
			candidate.imageUrl = thumbnails["1200"].asString();
		}

		// The archive holds both label-supplied assets and user scans. Which one
		// this is cannot be told from the metadata, so it stays unknown and the
		// image itself has to answer it.
		candidate.sourceType = ArtworkSourceType::Unknown;
		candidate.coverMatch = CoverMatch::IntendedCover;
		candidate.matchConfidence = Confidence::Moderate;
		candidate.evidence.push_back({"release_id_match",
			"attached to the MusicBrainz release this album already claims", true});
		candidate.evidence.push_back({"source_unknown",
			"the archive does not distinguish a label asset from a user scan; source type is unresolved",
			false});

		for (const auto& type : image["types"].asArray()) {
			candidate.evidence.push_back({"declared_type", "declared as " + type.asString(), true});
		}

		candidates.push_back(std::move(candidate));
	}
	return candidates;
}

Result<std::vector<std::uint8_t>> CoverArtArchiveProvider::fetchImage(const ArtworkCandidate& candidate,
	std::string& contentTypeOut) {
	HttpRequest request;
	request.providerId = "coverartarchive";
	request.url = candidate.imageUrl;
	request.maxBodyBytes = 32u * 1024u * 1024u;
	return m_http.download(request, contentTypeOut);
}

// ---------------------------------------------------------------------------
// LocalArtworkProvider
// ---------------------------------------------------------------------------

void LocalArtworkProvider::addEmbeddedCandidate(std::vector<std::uint8_t> bytes,
	std::string sourceDescription) {
	if (bytes.empty()) return;
	m_held.push_back({std::move(bytes), std::move(sourceDescription), {}});
}

Status LocalArtworkProvider::addImportedFile(const fs::path& path) {
	std::ifstream file(path, std::ios::binary);
	if (!file) {
		return Status(Error{ErrorCode::IoError, "cannot read " + path.string()});
	}
	std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
	if (bytes.empty()) {
		return Status(Error{ErrorCode::IoError, "imported image is empty: " + path.string()});
	}
	if (!ImagePipeline::isDecodable(bytes.data(), bytes.size())) {
		return Status(Error{ErrorCode::Unsupported,
			"imported file is not a JPEG or PNG: " + path.string()});
	}
	m_held.push_back({std::move(bytes), "imported by the user", path.string()});
	return Status::success();
}

Result<std::vector<ArtworkCandidate>> LocalArtworkProvider::findArtwork(const AlbumQuery&) {
	std::vector<ArtworkCandidate> candidates;

	// Held candidates: embedded covers and user imports.
	for (std::size_t i = 0; i < m_held.size(); ++i) {
		const Held& held = m_held[i];
		ArtworkCandidate candidate;
		candidate.providerId = id();
		candidate.providerName = displayName();
		candidate.localPath = held.path;
		candidate.imageUrl = "local:" + std::to_string(i);
		candidate.byteLength = static_cast<std::int64_t>(held.bytes.size());
		candidate.sourceType = held.path.empty()
			? ArtworkSourceType::ExistingEmbedded
			: ArtworkSourceType::UserSupplied;
		// An existing embedded cover is, by definition, the cover this library
		// already uses for this album.
		candidate.coverMatch = CoverMatch::IntendedCover;
		candidate.matchConfidence = held.path.empty() ? Confidence::Strong : Confidence::Locked;
		candidate.evidence.push_back({"local_source", held.description, true});
		candidates.push_back(std::move(candidate));
	}

	// Loose files beside the album.
	if (!m_directory.empty()) {
		static constexpr std::array<std::string_view, 8> kNames = {
			"cover.jpg", "cover.png", "folder.jpg", "folder.png",
			"front.jpg", "front.png", "album.jpg", "albumart.jpg",
		};
		std::error_code ec;
		for (auto name : kNames) {
			const fs::path path = m_directory / std::string(name);
			if (!fs::exists(path, ec) || ec) continue;

			ArtworkCandidate candidate;
			candidate.providerId = id();
			candidate.providerName = displayName();
			candidate.localPath = path.string();
			candidate.imageUrl = "file://" + path.string();
			candidate.byteLength = static_cast<std::int64_t>(fs::file_size(path, ec));
			candidate.sourceType = ArtworkSourceType::UserSupplied;
			candidate.coverMatch = CoverMatch::Unknown;
			candidate.matchConfidence = Confidence::Moderate;
			candidate.evidence.push_back({"local_file",
				"found \"" + std::string(name) + "\" beside the album", true});
			candidates.push_back(std::move(candidate));
		}
	}

	return candidates;
}

Result<std::vector<std::uint8_t>> LocalArtworkProvider::fetchImage(const ArtworkCandidate& candidate,
	std::string& contentTypeOut) {
	if (candidate.imageUrl.rfind("local:", 0) == 0) {
		const std::string indexText = candidate.imageUrl.substr(6);
		try {
			const std::size_t index = static_cast<std::size_t>(std::stoul(indexText));
			if (index < m_held.size()) {
				contentTypeOut = "application/octet-stream";
				return m_held[index].bytes;
			}
		} catch (...) {
		}
		return Error{ErrorCode::NotFound, "held artwork candidate is no longer available"};
	}

	if (!candidate.localPath.empty()) {
		std::ifstream file(candidate.localPath, std::ios::binary);
		if (!file) {
			return Error{ErrorCode::IoError, "cannot read " + candidate.localPath};
		}
		std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(file)),
			std::istreambuf_iterator<char>());
		contentTypeOut = "application/octet-stream";
		return bytes;
	}

	return Error{ErrorCode::NotFound, "local candidate has no readable source"};
}

// ---------------------------------------------------------------------------
// MusicBrainzProvider
// ---------------------------------------------------------------------------

MusicBrainzProvider::MusicBrainzProvider(HttpClient& http) : m_http(http) {}

Result<std::vector<ReleaseCandidate>> MusicBrainzProvider::findReleases(const AlbumQuery& query) {
	std::string lucene;
	if (!query.album.empty()) {
		lucene += "release:\"" + query.album + "\"";
	}
	if (!query.albumArtist.empty()) {
		if (!lucene.empty()) lucene += " AND ";
		lucene += "artist:\"" + query.albumArtist + "\"";
	}
	if (!query.barcode.empty()) {
		if (!lucene.empty()) lucene += " AND ";
		lucene += "barcode:" + query.barcode;
	}
	if (lucene.empty()) {
		return Error{ErrorCode::InvalidArgument, "a MusicBrainz lookup needs an album or artist"};
	}

	HttpRequest request;
	request.providerId = "musicbrainz";
	request.url = "https://musicbrainz.org/ws/2/release?query=" + urlEncode(lucene) + "&fmt=json&limit=10";
	request.headers["Accept"] = "application/json";

	auto response = m_http.get(request);
	if (!response) return response.error();
	if (!response.value().ok()) {
		return Error{ErrorCode::NetworkError,
			"MusicBrainz returned HTTP " + std::to_string(response.value().statusCode)};
	}

	auto parsed = Json::parse(response.value().body);
	if (!parsed) return parsed.error();

	std::vector<ReleaseCandidate> candidates;
	for (const auto& release : parsed.value()["releases"].asArray()) {
		ReleaseCandidate candidate;
		candidate.providerId = id();
		candidate.providerRef = release["id"].asString();
		candidate.album = release["title"].asString();
		candidate.date = release["date"].asString();
		candidate.trackCount = static_cast<int>(release["track-count"].asInt());
		candidate.pageUrl = "https://musicbrainz.org/release/" + candidate.providerRef;

		// MusicBrainz distinguishes recordings, releases and release groups. The
		// disambiguation field is where the edition lives.
		candidate.edition = release["disambiguation"].asString();

		std::string artistName;
		for (const auto& credit : release["artist-credit"].asArray()) {
			artistName += credit["name"].asString();
			artistName += credit["joinphrase"].asString();
		}
		candidate.albumArtist = artistName;

		const double titleSimilarity = text::similarity(query.album, candidate.album);
		const double artistSimilarity = query.albumArtist.empty()
			? -1.0
			: text::similarity(query.albumArtist, candidate.albumArtist);

		candidate.evidence.push_back({"title_similarity",
			"release title similarity " + std::to_string(titleSimilarity), titleSimilarity > 0.85});
		if (artistSimilarity >= 0.0) {
			candidate.evidence.push_back({"artist_similarity",
				"artist credit similarity " + std::to_string(artistSimilarity), artistSimilarity > 0.8});
		}
		if (!candidate.edition.empty()) {
			candidate.evidence.push_back({"edition",
				"release is disambiguated as \"" + candidate.edition + "\"", true});
		}
		if (query.trackCount > 0 && candidate.trackCount > 0) {
			candidate.evidence.push_back({"track_count",
				"local group has " + std::to_string(query.trackCount) + ", release declares "
					+ std::to_string(candidate.trackCount),
				query.trackCount == candidate.trackCount});
		}

		// The search score is MusicBrainz's own relevance number, not a
		// probability that this is the right edition.
		const std::int64_t score = release["score"].asInt();
		candidate.evidence.push_back({"search_score",
			"MusicBrainz relevance score " + std::to_string(score)
				+ " (a text-match score, not a probability that this is the correct edition)", score > 80});

		if (titleSimilarity > 0.9 && (artistSimilarity < 0.0 || artistSimilarity > 0.85)
			&& query.trackCount > 0 && candidate.trackCount == query.trackCount) {
			candidate.confidence = Confidence::Moderate;
		} else if (titleSimilarity > 0.85) {
			candidate.confidence = Confidence::Weak;
		} else {
			candidate.confidence = Confidence::Unknown;
		}

		candidates.push_back(std::move(candidate));
	}
	return candidates;
}

// ---------------------------------------------------------------------------
// LrclibProvider
// ---------------------------------------------------------------------------

LrclibProvider::LrclibProvider(HttpClient& http) : m_http(http) {}

namespace {

LyricsCandidate candidateFromJson(const JsonValue& record, const std::string& providerId) {
	LyricsCandidate candidate;
	candidate.providerId = providerId;
	candidate.trackName = record["trackName"].asString();
	candidate.artistName = record["artistName"].asString();
	candidate.albumName = record["albumName"].asString();
	candidate.durationMs = static_cast<std::int64_t>(record["duration"].asNumber() * 1000.0);
	candidate.plainLyrics = record["plainLyrics"].asString();
	candidate.syncedLyrics = record["syncedLyrics"].asString();
	candidate.instrumental = record["instrumental"].asBool();
	const std::string recordId = record["id"].asString();
	if (!recordId.empty()) {
		candidate.sourceUrl = "https://lrclib.net/api/get/" + recordId;
	}
	return candidate;
}

} // namespace

Result<std::vector<LyricsCandidate>> LrclibProvider::findLyrics(const TrackQuery& query) {
	if (query.artist.empty() || query.title.empty()) {
		return Error{ErrorCode::InvalidArgument, "a lyrics lookup needs both an artist and a title"};
	}

	// LRCLIB's /api/get requires every supplied field to match exactly. Passing
	// the local album name therefore *reduces* the hit rate: the same recording
	// is catalogued there under whichever compilation it was ingested from, so
	// "Desire" on album "Desire" misses a record filed under "Now That's What I
	// Call Music! 116".
	//
	// The album is consequently omitted from the request and compared afterwards
	// by the matching policy, which weighs it alongside artist, title, version
	// and duration. That keeps the FRD's requirement -- a same-title result alone
	// is never sufficient -- while not throwing away correct matches.
	const auto attempt = [&](bool includeDuration) -> Result<std::vector<LyricsCandidate>> {
		std::string url = "https://lrclib.net/api/get?artist_name=" + urlEncode(query.artist)
			+ "&track_name=" + urlEncode(query.title);
		if (includeDuration && query.durationMs > 0) {
			url += "&duration=" + std::to_string(query.durationMs / 1000);
		}

		HttpRequest request;
		request.providerId = "lrclib";
		request.url = url;
		request.headers["Accept"] = "application/json";

		auto response = m_http.get(request);
		if (!response) return response.error();

		if (response.value().statusCode == 404) {
			// A genuine miss, distinct from a failure. The caller turns this into
			// `not_found`, never into `instrumental`.
			return std::vector<LyricsCandidate>{};
		}
		if (!response.value().ok()) {
			return Error{ErrorCode::NetworkError,
				"LRCLIB returned HTTP " + std::to_string(response.value().statusCode)};
		}

		auto parsed = Json::parse(response.value().body);
		if (!parsed) return parsed.error();

		std::vector<LyricsCandidate> candidates;
		candidates.push_back(candidateFromJson(parsed.value(), id()));
		return candidates;
	};

	// Duration first, because an exact duration match is the strongest signal
	// that this is the same recording rather than a different edit.
	auto withDuration = attempt(true);
	if (!withDuration) return withDuration;
	if (!withDuration.value().empty()) return withDuration;

	if (query.durationMs > 0) {
		auto withoutDuration = attempt(false);
		if (!withoutDuration) return withoutDuration;
		if (!withoutDuration.value().empty()) return withoutDuration;
	}

	// Fall back to the broader search; its results still face the same policy.
	return searchLyrics(query);
}

Result<std::vector<LyricsCandidate>> LrclibProvider::searchLyrics(const TrackQuery& query) {
	if (query.title.empty()) {
		return Error{ErrorCode::InvalidArgument, "a lyrics search needs a title"};
	}

	std::string url = "https://lrclib.net/api/search?track_name=" + urlEncode(query.title);
	if (!query.artist.empty()) url += "&artist_name=" + urlEncode(query.artist);

	HttpRequest request;
	request.providerId = "lrclib";
	request.url = url;
	request.headers["Accept"] = "application/json";

	auto response = m_http.get(request);
	if (!response) return response.error();
	if (response.value().statusCode == 404) return std::vector<LyricsCandidate>{};
	if (!response.value().ok()) {
		return Error{ErrorCode::NetworkError,
			"LRCLIB search returned HTTP " + std::to_string(response.value().statusCode)};
	}

	auto parsed = Json::parse(response.value().body);
	if (!parsed) return parsed.error();

	std::vector<LyricsCandidate> candidates;
	for (const auto& record : parsed.value().asArray()) {
		candidates.push_back(candidateFromJson(record, id()));
		if (candidates.size() >= 10) break;
	}
	return candidates;
}

// ---------------------------------------------------------------------------
// ReviewSearchUrls
// ---------------------------------------------------------------------------

const std::vector<int>& ReviewSearchUrls::sizePresets() {
	// The user's existing presets, retained as shortcuts. They are search
	// conveniences, not eligibility rules: a clean 600x600 asset is acceptable.
	static const std::vector<int> kPresets = {1200, 1800, 2400, 3000};
	return kPresets;
}

std::string ReviewSearchUrls::googleImages(const std::string& artist, const std::string& album,
	int imageSizePreset) {
	std::string query = artist + " " + album;
	if (imageSizePreset > 0) {
		query += " imagesize:" + std::to_string(imageSizePreset) + "x" + std::to_string(imageSizePreset);
	}
	return "https://www.google.com/search?tbm=isch&q=" + urlEncode(query);
}

std::string ReviewSearchUrls::appleMusicSiteSearch(const std::string& artist, const std::string& album) {
	// The user's existing query shape, without a size filter that would exclude
	// a clean 600x600 source.
	return "https://www.google.com/search?tbm=isch&q="
		+ urlEncode(artist + " " + album + " site:music.apple.com");
}

std::string ReviewSearchUrls::musicBrainzSearch(const std::string& artist, const std::string& album) {
	return "https://musicbrainz.org/search?type=release&query="
		+ urlEncode("release:\"" + album + "\" AND artist:\"" + artist + "\"");
}

std::string ReviewSearchUrls::discogsSearch(const std::string& artist, const std::string& album) {
	return "https://www.discogs.com/search/?type=release&q=" + urlEncode(artist + " " + album);
}

} // namespace ml
