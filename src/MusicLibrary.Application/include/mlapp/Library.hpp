// SPDX-License-Identifier: GPL-3.0-or-later
// UI-001: the single engine behind both the CLI and the desktop application.
//
// Neither front end contains policy. Both call this class, which owns the
// catalogue, the path guard, the providers and the writer. A capability the CLI
// does not have is one the desktop does not have either.
#pragma once

#include "mlapp/Catalogue.hpp"
#include "mlcore/AlbumGrouping.hpp"
#include "mlcore/ArtworkPolicy.hpp"
#include "mlcore/ChangePlan.hpp"
#include "mlcore/Enrichment.hpp"
#include "mlcore/NamingTemplate.hpp"
#include "mlcore/TagPolicy.hpp"
#include "mlinfra/AudioAnalysis.hpp"
#include "mlinfra/Http.hpp"
#include "mlinfra/ImagePipeline.hpp"
#include "mlinfra/PathGuard.hpp"
#include "mlinfra/Providers.hpp"
#include "mlinfra/Sqlite.hpp"

#include <atomic>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace ml {

/// Progress callback. Returning false requests cancellation.
using ProgressCallback = std::function<bool(std::int64_t done, std::int64_t total,
	const std::string& message)>;

/// Everything the engine needs to start. Paths are validated before use.
struct LibraryConfig {
	/// Where the catalogue, asset store, caches and logs live. Never inside a
	/// protected source root.
	std::filesystem::path dataDirectory;
	/// Protected source roots. Read-only to every operation.
	std::vector<std::filesystem::path> sourceRoots;
	/// Output root for exported copies. Required before any modifying command.
	std::filesystem::path outputRoot;

	NamingOptions naming;
	PrivacyOptions privacy;
	GainOptions gain;
	ArtworkOptions artwork;
	DerivativeConfig derivative;
	TempoOptions tempo;
	TempoAnalyzerOptions tempoAnalyzer;
	LyricsOptions lyrics;
	PaddingOptions padding;
	GroupingOptions grouping;

	/// Storefront for the Apple adapter. South Africa by default.
	std::string appleStorefront = "za";
	/// Disable all network access. Local operations continue.
	bool offline = false;
	/// Analysis worker count. 0 means "choose from the hardware".
	int workerCount = 0;

	/// Default data directory for this platform's per-user data location.
	static std::filesystem::path defaultDataDirectory();

	/// A stable hash of the policy configuration, used for job identity.
	std::string hash() const;
};

/// Outcome of a scan.
struct ScanResult {
	ScanRunId runId;
	std::int64_t filesSeen = 0;
	std::int64_t filesAdded = 0;
	std::int64_t filesChanged = 0;
	std::int64_t filesUnchanged = 0;
	std::int64_t filesUnreadable = 0;
	std::int64_t bytesRead = 0;
	double elapsedSeconds = 0.0;
	bool cancelled = false;
	std::vector<std::string> warnings;
};

/// Outcome of an export run.
struct ExportResult {
	ChangeSetId changeSetId;
	std::int64_t attempted = 0;
	std::int64_t written = 0;
	std::int64_t skipped = 0;
	std::int64_t failed = 0;
	std::int64_t bytesWritten = 0;
	std::int64_t optimisationSavings = 0;
	std::int64_t enrichmentGrowth = 0;
	double elapsedSeconds = 0.0;
	bool cancelled = false;
	std::vector<std::string> failures;
};

/// One album with everything the review view needs, assembled once.
struct AlbumReview {
	ProvisionalAlbum album;
	std::vector<ArtworkCandidate> artworkCandidates;
	ArtworkSelection selection;
	std::vector<FileRecord> tracks;
	std::string googleImagesUrl;
	std::string appleSearchUrl;
	std::string musicBrainzUrl;
};

class Library {
public:
	Library();
	~Library();

	Library(const Library&) = delete;
	Library& operator=(const Library&) = delete;

	/// Opens or creates the catalogue and validates the configured roots.
	///
	/// Fails when an output root is inside a protected source root, when a
	/// protected root cannot be resolved, or when the catalogue schema is newer
	/// than this build understands.
	Status open(LibraryConfig config);

	bool isOpen() const { return m_database.valid(); }
	const LibraryConfig& config() const { return m_config; }
	Catalogue& catalogue() { return *m_catalogue; }
	const PathGuard& guard() const { return m_guard; }

	/// Cancels the running operation at the next checkpoint.
	void requestCancel() { m_cancelled.store(true, std::memory_order_relaxed); }
	void clearCancel() { m_cancelled.store(false, std::memory_order_relaxed); }
	bool cancelRequested() const { return m_cancelled.load(std::memory_order_relaxed); }

	// --- Commands ------------------------------------------------------------

	/// Read-only inventory of the protected source roots (FN-SCAN-01..04).
	Result<ScanResult> scan(ProgressCallback progress = {});

	/// Rebuilds provisional album grouping from the catalogue (ID-001).
	Result<std::int64_t> resolveAlbums(ProgressCallback progress = {});

	/// Creates an independent sample of copies for testing, outside every
	/// protected root (FRD section 13).
	Result<std::int64_t> createSample(const std::filesystem::path& destination, int albumCount,
		int trackLimit, ProgressCallback progress = {});

	/// Local analysis: tempo. Touches no network and writes no music.
	Result<std::int64_t> analyseTempo(ProgressCallback progress = {});

	/// Remote enrichment: artwork and lyrics. Degrades cleanly when offline.
	Result<std::int64_t> fetchArtwork(ProgressCallback progress = {});
	Result<std::int64_t> fetchLyrics(ProgressCallback progress = {});

	/// Builds a change plan. Structurally incapable of writing music: it returns
	/// data and touches only the catalogue (FN-CLI-02).
	Result<ChangeSet> plan(ProgressCallback progress = {});

	/// Executes a change plan, writing copies to the output root.
	Result<ExportResult> exportCopies(ChangeSetId set, ProgressCallback progress = {});

	/// Re-reads written outputs and verifies them against their plans.
	Result<std::int64_t> verify(ChangeSetId set, ProgressCallback progress = {});

	/// Reconciles operations interrupted by a crash (FN-SAFE-03).
	Result<std::int64_t> recover(ChangeSetId set, ProgressCallback progress = {});

	// --- Reports -------------------------------------------------------------
	Result<CoverageReport> coverage() const;
	Result<NamingConformity> namingReport() const;
	/// Writes docs/naming-convention.md style evidence to a file.
	Status writeNamingConventionReport(const std::filesystem::path& destination) const;

	// --- Review --------------------------------------------------------------
	Result<AlbumReview> reviewAlbum(AlbumId album);
	Status lockArtwork(AlbumId album, ArtworkId asset, std::string_view note);
	Status rejectArtwork(AlbumId album, ArtworkId asset, std::string_view note);
	Status importArtworkFile(AlbumId album, const std::filesystem::path& path);

	/// Previews the exact frame-level changes for one file, without writing.
	Result<FilePlan> previewFile(FileId file);

	// --- Diagnostics ---------------------------------------------------------
	struct Diagnostics {
		std::string catalogueJournalMode;
		std::string catalogueVersion;
		std::int64_t catalogueBytes = 0;
		bool integrityOk = false;
		std::size_t protectedRootCount = 0;
		std::size_t absentRootCount = 0;
		std::string derivativeConfig;
		std::string tempoEngine;
		bool offline = false;
	};
	Result<Diagnostics> diagnostics();

	/// Backs the catalogue up using SQLite's own backup API.
	Status backupCatalogue(const std::filesystem::path& destination);

private:
	Result<FilePlan> buildPlan(const FileRecord& record, CollisionDetector& collisions,
		std::optional<AlbumId> albumId);
	bool reportProgress(const ProgressCallback& progress, std::int64_t done, std::int64_t total,
		const std::string& message);

	LibraryConfig m_config;
	Database m_database;
	std::unique_ptr<Catalogue> m_catalogue;
	PathGuard m_guard;

	std::unique_ptr<HttpClient> m_http;
	std::unique_ptr<ITunesProvider> m_itunes;
	std::unique_ptr<CoverArtArchiveProvider> m_coverArtArchive;
	std::unique_ptr<MusicBrainzProvider> m_musicBrainz;
	std::unique_ptr<LrclibProvider> m_lrclib;
	std::unique_ptr<AssetStore> m_assets;

	std::atomic<bool> m_cancelled{false};
};

} // namespace ml
