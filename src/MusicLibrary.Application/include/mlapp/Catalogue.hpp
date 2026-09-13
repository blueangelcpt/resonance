// SPDX-License-Identifier: GPL-3.0-or-later
// CAT-001: the catalogue repository.
//
// Observed, proposed and written state are stored in separate tables and are
// written by separate methods. There is deliberately no call that records "this
// file was updated" as a side effect of planning one.
#pragma once

#include "mlcore/AlbumGrouping.hpp"
#include "mlcore/ArtworkPolicy.hpp"
#include "mlcore/ChangePlan.hpp"
#include "mlcore/Enrichment.hpp"
#include "mlcore/TagModel.hpp"
#include "mlcore/Types.hpp"
#include "mlinfra/Sqlite.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace ml {

/// One catalogued file, as the browsing views need it.
struct FileRecord {
	FileId id;
	RootId rootId;
	std::string relativePath;
	std::string relativeDirectory;
	std::string fileName;
	std::string extension;

	FileIdentity identity;
	AudioProperties audio;

	std::string contentSha256;
	std::string audioSha256;

	std::string readStatus = "ok";
	std::string readError;

	// Denormalised display fields, filled from the latest observed snapshot so
	// the track table can page without joining four tables per row.
	std::string title;
	std::string artist;
	std::string albumArtist;
	std::string album;
	std::string genre;
	std::string date;
	std::optional<int> trackNumber;
	std::optional<int> discNumber;
	std::optional<double> bpm;
	bool hasArtwork = false;
	int artworkWidth = 0;
	int artworkHeight = 0;
	bool hasLyrics = false;
	bool hasGainFields = false;
	bool hasPrivacyFindings = false;
	std::string primaryContainer;
};

/// Filters for the track table (FN-UI-02).
struct TrackFilter {
	std::string searchText;
	std::optional<bool> missingArtwork;
	std::optional<bool> missingLyrics;
	std::optional<bool> missingBpm;
	std::optional<bool> hasGainFields;
	std::optional<bool> hasPrivacyFindings;
	std::optional<bool> needsReview;
	std::optional<AlbumId> albumId;
	std::string readStatus;

	bool isEmpty() const {
		return searchText.empty() && !missingArtwork && !missingLyrics && !missingBpm
			&& !hasGainFields && !hasPrivacyFindings && !needsReview && !albumId && readStatus.empty();
	}
};

/// Aggregate counts for the scan report (FN-SCAN-03).
struct CoverageReport {
	std::int64_t totalFiles = 0;
	std::int64_t readable = 0;
	std::int64_t unreadable = 0;
	std::int64_t withArtwork = 0;
	std::int64_t withLyrics = 0;
	std::int64_t withBpm = 0;
	std::int64_t withGainFields = 0;
	std::int64_t withPrivacyFindings = 0;
	std::int64_t totalBytes = 0;
	std::int64_t totalDurationMs = 0;

	std::vector<std::pair<std::string, std::int64_t>> tagVersionDistribution;
	std::vector<std::pair<std::string, std::int64_t>> artworkSizeDistribution;
	std::vector<std::pair<std::string, std::int64_t>> bitrateModeDistribution;
	std::vector<std::pair<std::string, std::int64_t>> missingFieldCounts;
	std::vector<std::pair<std::string, std::int64_t>> readErrorCounts;

	std::int64_t albumCount = 0;
	std::int64_t albumsNeedingReview = 0;
	std::int64_t likelyDuplicateGroups = 0;
};

/// Naming conformity measured against the confirmed template (NAME-001).
struct NamingConformity {
	std::int64_t totalFiles = 0;
	std::int64_t matchingTemplate = 0;
	std::int64_t wouldNeedReview = 0;
	std::vector<std::pair<std::string, std::int64_t>> exceptionCounts;
	std::vector<std::pair<std::string, std::int64_t>> observedGrammars;
	std::vector<std::string> representativeMatches;
	std::vector<std::string> representativeExceptions;

	double matchPercentage() const {
		return totalFiles > 0 ? (100.0 * static_cast<double>(matchingTemplate)
			/ static_cast<double>(totalFiles)) : 0.0;
	}
};

class Catalogue {
public:
	explicit Catalogue(Database& database);

	// --- Roots ---------------------------------------------------------------
	Result<RootId> upsertRoot(const std::filesystem::path& path, const std::filesystem::path& resolved,
		std::string_view kind, std::string_view label, std::string_view volumeIdentity,
		std::uint64_t deviceId);
	Result<std::optional<RootId>> findRoot(const std::filesystem::path& path) const;
	Result<std::vector<std::tuple<RootId, std::string, std::string>>> listRoots(std::string_view kind) const;

	// --- Scan runs -----------------------------------------------------------
	Result<ScanRunId> beginScan(RootId root, bool rootWasPresent);
	Status finishScan(ScanRunId scan, std::string_view status, std::int64_t seen, std::int64_t added,
		std::int64_t changed, std::int64_t unreadable, std::string_view error = {});

	// --- Files (observed state) ----------------------------------------------
	Result<FileId> upsertFile(RootId root, ScanRunId scan, const FileRecord& record);
	Status markFileUnreadable(RootId root, ScanRunId scan, std::string_view relativePath,
		std::string_view status, std::string_view error);
	Result<std::optional<FileRecord>> findFile(RootId root, std::string_view relativePath) const;
	Result<std::optional<FileRecord>> loadFile(FileId id) const;

	Result<std::vector<FileRecord>> queryFiles(const TrackFilter& filter, std::int64_t limit,
		std::int64_t offset) const;
	Result<std::int64_t> countFiles(const TrackFilter& filter) const;

	/// Files whose identity suggests they changed since the last scan.
	Result<std::vector<FileId>> filesNeedingRevalidation(RootId root) const;

	// --- Tag snapshots -------------------------------------------------------
	Result<SnapshotId> saveSnapshot(FileId file, const TagSnapshot& snapshot, std::string_view kind);
	Result<std::optional<TagSnapshot>> latestSnapshot(FileId file, std::string_view kind = "observed") const;

	// --- Albums --------------------------------------------------------------
	Status replaceAlbums(const std::vector<ProvisionalAlbum>& albums);
	Result<std::vector<ProvisionalAlbum>> listAlbums(bool onlyNeedingReview = false,
		std::int64_t limit = 0, std::int64_t offset = 0) const;
	Result<std::optional<ProvisionalAlbum>> loadAlbum(AlbumId id) const;
	Result<std::optional<AlbumId>> albumForFile(FileId file) const;
	Result<std::int64_t> countAlbums(bool onlyNeedingReview = false) const;

	// --- Artwork -------------------------------------------------------------
	Result<ArtworkId> saveArtworkCandidate(AlbumId album, const ArtworkCandidate& candidate,
		std::string_view selectionState, std::string_view reason, std::string_view policyVersion);
	Result<std::vector<ArtworkCandidate>> artworkCandidates(AlbumId album) const;
	Status setArtworkSelection(ArtworkId asset, std::string_view state, std::string_view reason);
	Result<std::optional<ArtworkCandidate>> selectedArtwork(AlbumId album) const;

	Status saveDerivative(ArtworkId asset, std::string_view configHash, std::string_view localPath,
		std::string_view sha256, int width, int height, std::int64_t bytes, int quality,
		std::string_view subsampling);
	Result<std::optional<std::string>> findDerivative(ArtworkId asset, std::string_view configHash) const;

	// --- Tempo and lyrics ----------------------------------------------------
	Status saveTempo(FileId file, const TempoAnalysis& analysis, const TempoDecision& decision);
	Result<std::optional<TempoDecision>> loadTempo(FileId file) const;

	Status saveLyrics(FileId file, const LyricsDecision& decision);
	Result<std::optional<LyricsDecision>> loadLyrics(FileId file) const;

	// --- Manual decisions (survive rescans) ----------------------------------
	Status recordManualDecision(std::string_view scope, std::int64_t scopeId, std::string_view field,
		std::string_view decision, std::string_view value, std::optional<std::int64_t> referenceId,
		std::string_view note);
	Result<std::vector<std::tuple<std::string, std::string, std::string>>> manualDecisions(
		std::string_view scope, std::int64_t scopeId) const;
	Status clearManualDecision(std::string_view scope, std::int64_t scopeId, std::string_view field);

	// --- Change sets ---------------------------------------------------------
	Result<ChangeSetId> createChangeSet(std::string_view outputRoot, std::string_view configHash,
		std::string_view note);
	Status saveFilePlan(ChangeSetId set, const FilePlan& plan);
	Result<std::vector<FilePlan>> loadFilePlans(ChangeSetId set, bool onlyWritable = false) const;
	Result<std::optional<ChangeSetId>> latestChangeSet() const;
	Status setChangeSetState(ChangeSetId set, std::string_view state);

	/// Reserves a destination path. Fails with ErrorCode::Collision when the
	/// path is already reserved in this change set (NAME-002).
	Status reserveDestination(ChangeSetId set, std::string_view relativePath, FileId file);

	// --- Operation journal (FN-SAFE-02, FN-SAFE-03) --------------------------
	Status setOperationState(ChangeSetId set, FileId file, std::string_view state,
		std::string_view tempPath = {}, std::string_view error = {});
	Status completeOperation(ChangeSetId set, FileId file, std::string_view writtenSha256,
		std::string_view writtenAudioSha256, std::int64_t bytesBefore, std::int64_t bytesAfter,
		std::int64_t optimisationSavings, std::int64_t enrichmentGrowth);
	/// Operations left mid-flight by a crash, for the recovery pass.
	Result<std::vector<std::tuple<FileId, std::string, std::string>>> incompleteOperations(
		ChangeSetId set) const;

	// --- Reports -------------------------------------------------------------
	Result<CoverageReport> coverageReport() const;
	Result<NamingConformity> namingConformity(const NamingTemplate& naming) const;
	Result<std::vector<std::pair<std::string, std::int64_t>>> duplicateAudioGroups(
		std::int64_t limit = 100) const;

	// --- Settings ------------------------------------------------------------
	Status setSetting(std::string_view key, std::string_view value);
	Result<std::optional<std::string>> setting(std::string_view key) const;

	Database& database() { return m_database; }

private:
	Status refreshSearchIndex(FileId file, const FileRecord& record);

	Database& m_database;
};

/// ISO-8601 timestamp in UTC, used for every stored time.
std::string nowIso8601();

} // namespace ml
