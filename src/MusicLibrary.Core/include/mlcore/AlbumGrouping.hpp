// SPDX-License-Identifier: GPL-3.0-or-later
// ID-001 / FN-ALB-01 / FN-ALB-02: provisional album grouping.
//
// Grouping uses folder structure, album title, album artist, disc numbering,
// track count, dates and release identifiers. Track artist plus album title is
// explicitly NOT used as the only key, because compilations and guest
// performers would split incorrectly. Two editions whose titles match are not
// merged on that basis alone.
#pragma once

#include "mlcore/TagModel.hpp"
#include "mlcore/Types.hpp"

#include <map>
#include <string>
#include <vector>

namespace ml {

/// The facts about one file that grouping is allowed to use.
struct GroupingInput {
	FileId fileId;
	std::string relativeDirectory;   ///< Directory of the file, relative to the root.
	std::string album;
	std::string albumArtist;
	std::string artist;
	std::string date;
	std::optional<int> trackNumber;
	std::optional<int> trackTotal;
	std::optional<int> discNumber;
	std::optional<int> discTotal;
	bool compilationFlag = false;
	std::string musicBrainzAlbumId;
	std::string musicBrainzReleaseGroupId;
	std::string catalogNumber;
	std::string barcode;
	std::int64_t durationMs = 0;
};

/// Why a group was flagged for human attention.
enum class AlbumFlag {
	MissingTrackNumbers,
	DuplicateTrackNumbers,
	IncompleteTrackRun,       ///< Gaps against a declared total.
	ConflictingAlbumArtist,
	ConflictingDate,
	ConflictingReleaseId,
	MixedDiscNumbering,
	SingleTrackGroup,
	LikelyCompilation,
	FolderTitleMismatch,      ///< Folder name and album tag disagree.
	NoAlbumTag,
};

std::string_view toString(AlbumFlag f);

/// A provisional album. "Provisional" is deliberate: identity is proposed, and
/// confirming a specific release or edition requires provider evidence.
struct ProvisionalAlbum {
	AlbumId id;
	std::string groupKey;           ///< Stable key this grouping produced.
	std::string album;
	std::string albumArtist;
	std::string date;
	std::string musicBrainzAlbumId;
	std::string editionQualifier;   ///< "Deluxe", "2 CD Expanded Edition", etc.
	std::vector<FileId> files;
	std::vector<AlbumFlag> flags;
	std::vector<Evidence> evidence;

	int discCount = 1;
	int observedTrackCount = 0;
	std::optional<int> declaredTrackTotal;
	bool isCompilation = false;
	Confidence identityConfidence = Confidence::Unknown;

	bool needsReview() const { return !flags.empty() || identityConfidence <= Confidence::Weak; }
};

struct GroupingOptions {
	/// Treat a directory as an album boundary. Folder structure is strong
	/// evidence in a library organised by the user's template.
	bool useDirectoryAsPrimaryKey = true;
	/// Similarity above which two album titles are considered the same title.
	double titleSimilarityThreshold = 0.92;
	/// Flag a group whose observed count is below its declared total.
	bool flagIncompleteRuns = true;
};

class AlbumResolverCore {
public:
	explicit AlbumResolverCore(GroupingOptions options = {});

	/// Groups files into provisional albums. Pure: no I/O, deterministic given
	/// the same inputs in the same order.
	std::vector<ProvisionalAlbum> group(const std::vector<GroupingInput>& inputs) const;

	/// The key a single file contributes. Exposed for tests and for explaining a
	/// grouping decision in the UI.
	std::string groupKeyFor(const GroupingInput& input) const;

	/// True when two album titles denote the same *edition*. Titles differing
	/// only by an edition qualifier are NOT the same edition (FN-ALB-02).
	bool sameEdition(std::string_view titleA, std::string_view titleB) const;

	const GroupingOptions& options() const { return m_options; }

private:
	void analyseGroup(ProvisionalAlbum& album, const std::vector<const GroupingInput*>& members) const;

	GroupingOptions m_options;
};

} // namespace ml
