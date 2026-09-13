// SPDX-License-Identifier: GPL-3.0-or-later
// Provider adapters.
//
// FRD section 3: provider adapters return *proposals*. They cannot save tags or
// rename music, and that is enforced structurally: nothing in this header has
// access to a PathGuard, a TagWriter or the catalogue.
//
// Every adapter is replaceable. An inaccessible provider degrades the result to
// "unresolved"; it never blocks the run and never causes an inferior automatic
// replacement (ART-003).
#pragma once

#include "mlcore/ArtworkPolicy.hpp"
#include "mlcore/Enrichment.hpp"
#include "mlcore/Types.hpp"
#include "mlinfra/Http.hpp"

#include <array>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace ml {

/// What we know locally about an album when asking a provider about it.
struct AlbumQuery {
	std::string albumArtist;
	std::string album;
	std::string date;
	int trackCount = 0;
	std::string musicBrainzAlbumId;
	std::string barcode;
	std::string catalogNumber;
};

/// What we know about one recording.
struct TrackQuery {
	std::string artist;
	std::string title;
	std::string album;
	std::int64_t durationMs = 0;
};

/// A release a provider proposed, with the evidence for the match.
struct ReleaseCandidate {
	std::string providerId;
	std::string providerRef;      ///< Provider's own identifier.
	std::string album;
	std::string albumArtist;
	std::string date;
	int trackCount = 0;
	std::string edition;
	std::string pageUrl;
	std::string artworkUrlTemplate;
	int artworkMaxWidth = 0;
	int artworkMaxHeight = 0;
	Confidence confidence = Confidence::Unknown;
	std::vector<Evidence> evidence;
};

/// Base interface. Implementations are network-bound and cancellable.
class ArtworkProvider {
public:
	virtual ~ArtworkProvider() = default;

	virtual std::string id() const = 0;
	virtual std::string displayName() const = 0;

	/// Finds artwork candidates for an album. Returns an empty vector when the
	/// provider has nothing; an error only when the lookup itself failed, so a
	/// caller can distinguish "no cover" from "provider unreachable".
	virtual Result<std::vector<ArtworkCandidate>> findArtwork(const AlbumQuery& query) = 0;

	/// Fetches the bytes of a candidate. Dimensions are measured by the caller
	/// from these bytes, never taken from the provider's claim.
	virtual Result<std::vector<std::uint8_t>> fetchImage(const ArtworkCandidate& candidate,
		std::string& contentTypeOut) = 0;
};

/// Apple's iTunes Search API.
///
/// The storefront defaults to South Africa, as the FRD specifies, with
/// controlled fallbacks for misses. The high-resolution URL rewrite is isolated
/// here and its result is always verified by decoding the downloaded image,
/// because the rewrite is undocumented.
class ITunesProvider : public ArtworkProvider {
public:
	explicit ITunesProvider(HttpClient& http, std::string storefront = "za");

	std::string id() const override { return "itunes"; }
	std::string displayName() const override { return "Apple / iTunes Search"; }

	Result<std::vector<ArtworkCandidate>> findArtwork(const AlbumQuery& query) override;
	Result<std::vector<std::uint8_t>> fetchImage(const ArtworkCandidate& candidate,
		std::string& contentTypeOut) override;

	/// Looks up releases without asking for artwork.
	Result<std::vector<ReleaseCandidate>> findReleases(const AlbumQuery& query);

	/// Rewrites a 100x100 artwork URL to a larger size.
	///
	/// This is an undocumented CDN convention. The rewritten URL is a *request*,
	/// not a guarantee: the caller must decode the result and record the measured
	/// size rather than the requested one.
	static std::string rewriteArtworkUrl(const std::string& url, int requestedEdge);

	void setStorefront(std::string storefront) { m_storefront = std::move(storefront); }
	void setFallbackStorefronts(std::vector<std::string> storefronts) {
		m_fallbackStorefronts = std::move(storefronts);
	}

private:
	HttpClient& m_http;
	std::string m_storefront;
	std::vector<std::string> m_fallbackStorefronts{"us", "gb", "de"};
};

/// Cover Art Archive, reachable when a MusicBrainz release id is known.
class CoverArtArchiveProvider : public ArtworkProvider {
public:
	explicit CoverArtArchiveProvider(HttpClient& http);

	std::string id() const override { return "coverartarchive"; }
	std::string displayName() const override { return "Cover Art Archive"; }

	Result<std::vector<ArtworkCandidate>> findArtwork(const AlbumQuery& query) override;
	Result<std::vector<std::uint8_t>> fetchImage(const ArtworkCandidate& candidate,
		std::string& contentTypeOut) override;

private:
	HttpClient& m_http;
};

/// Local artwork: an embedded cover, a folder image, or a file the user chose.
///
/// Needs no network, so it is the provider that keeps the artwork pipeline
/// working offline (ART-003).
class LocalArtworkProvider : public ArtworkProvider {
public:
	LocalArtworkProvider() = default;

	std::string id() const override { return "local"; }
	std::string displayName() const override { return "Local files"; }

	Result<std::vector<ArtworkCandidate>> findArtwork(const AlbumQuery& query) override;
	Result<std::vector<std::uint8_t>> fetchImage(const ArtworkCandidate& candidate,
		std::string& contentTypeOut) override;

	/// Directory searched for cover.jpg, folder.jpg and similar.
	void setSearchDirectory(std::filesystem::path directory) { m_directory = std::move(directory); }

	/// Registers an already-embedded cover as a candidate.
	void addEmbeddedCandidate(std::vector<std::uint8_t> bytes, std::string sourceDescription);

	/// Registers a file the user imported.
	Status addImportedFile(const std::filesystem::path& path);

private:
	std::filesystem::path m_directory;
	struct Held {
		std::vector<std::uint8_t> bytes;
		std::string description;
		std::string path;
	};
	std::vector<Held> m_held;
};

/// MusicBrainz release lookup.
class MusicBrainzProvider {
public:
	explicit MusicBrainzProvider(HttpClient& http);

	std::string id() const { return "musicbrainz"; }

	Result<std::vector<ReleaseCandidate>> findReleases(const AlbumQuery& query);

private:
	HttpClient& m_http;
};

/// LRCLIB lyrics lookup (FN-LYR-01).
class LrclibProvider {
public:
	explicit LrclibProvider(HttpClient& http);

	std::string id() const { return "lrclib"; }

	/// Signature lookup: artist, title, album and duration together. A same-title
	/// search alone is explicitly insufficient.
	Result<std::vector<LyricsCandidate>> findLyrics(const TrackQuery& query);

	/// Broader search, used only when the signature lookup misses. Its results
	/// still have to pass the matching policy.
	Result<std::vector<LyricsCandidate>> searchLyrics(const TrackQuery& query);

private:
	HttpClient& m_http;
};

/// Builds the review URLs the FRD asks to preserve (FN-ART-06).
///
/// These open a browser for a human to look at. No Google API is called, and
/// none is required: Google's Custom Search JSON API is closed to new customers
/// and must not become a dependency.
struct ReviewSearchUrls {
	static std::string googleImages(const std::string& artist, const std::string& album,
		int imageSizePreset = 0);
	static std::string appleMusicSiteSearch(const std::string& artist, const std::string& album);
	static std::string musicBrainzSearch(const std::string& artist, const std::string& album);
	static std::string discogsSearch(const std::string& artist, const std::string& album);

	/// The size presets the user already uses.
	static const std::vector<int>& sizePresets();
};

/// Percent-encodes a string for use in a query parameter.
std::string urlEncode(std::string_view text);

} // namespace ml
