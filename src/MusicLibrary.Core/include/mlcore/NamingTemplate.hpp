// SPDX-License-Identifier: GPL-3.0-or-later
// NAME-001 / NAME-002: the user-confirmed organisation template from FRD section 9.
//
//   {album_artist}/{album}/{track:02}. {artist} - {title}.mp3
//
// The separator after the track number is a period and one space; the
// artist/title separator is space-hyphen-space. Track 1 becomes "01" and track
// 100 stays "100". Punctuation, accents and version qualifiers are preserved
// subject only to documented filesystem sanitisation.
#pragma once

#include "mlcore/TagModel.hpp"
#include "mlcore/Types.hpp"

#include <map>
#include <string>
#include <vector>

namespace ml {

/// Target filesystem rules to preflight against. Planning always preflights the
/// strictest enabled target so a library prepared on Linux still copies to an
/// NTFS volume without a late failure (FN-NAME-01).
enum class FilesystemTarget { Posix, Windows };

struct NamingOptions {
	/// Preflight Windows rules even when running on POSIX. Default on: the user's
	/// library lives on Windows.
	bool enforceWindowsRules = true;
	/// Maximum full path length to accept before reporting an exception.
	std::size_t maxPathLength = 255;
	/// Replacement for characters the target filesystem forbids.
	std::string replacementChar = "_";
	/// Minimum digits for the track number prefix.
	int trackDigits = 2;
};

/// Why a track could not be placed automatically. The FRD forbids silently
/// resolving these by altering the template, so each becomes a review item.
enum class NamingExceptionKind {
	None = 0,
	MissingAlbumArtist,
	MissingAlbum,
	MissingArtist,
	MissingTitle,
	MissingTrackNumber,
	InvalidTrackNumber,
	MultipleValues,        ///< A tag carried several values; picking one is a decision.
	ReservedName,          ///< CON, PRN, AUX, NUL, COM1-9, LPT1-9 on Windows.
	IllegalCharacters,     ///< Characters replaced by sanitisation.
	TrailingSpaceOrDot,    ///< Windows silently strips these; we refuse instead.
	PathTooLong,
	EmptyComponent,
	MultiDiscCollision,    ///< Same track number on different discs, no disc folder allowed.
};

std::string_view toString(NamingExceptionKind k);

struct NamingException {
	NamingExceptionKind kind = NamingExceptionKind::None;
	std::string detail;
	/// True when the result is still usable and the exception is informational
	/// (for example a character was sanitised); false when review is required.
	bool advisory = false;
};

/// The proposed relative destination for one track.
struct NamingResult {
	std::string relativePath;              ///< Empty when `requiresReview` is true.
	std::string albumArtistComponent;
	std::string albumComponent;
	std::string fileComponent;
	std::vector<NamingException> exceptions;
	bool requiresReview = false;

	bool ok() const { return !requiresReview && !relativePath.empty(); }
};

/// Values the template consumes. Kept separate from TagSnapshot so review edits
/// and manual locks can supply corrected values without mutating observed state.
struct NamingInput {
	std::string albumArtist;
	std::string album;
	std::string artist;
	std::string title;
	std::optional<int> trackNumber;
	std::optional<int> discNumber;
	std::optional<int> discTotal;
	std::string extension = ".mp3";
	bool albumArtistHadMultipleValues = false;
	bool artistHadMultipleValues = false;
};

/// Builds the template input from observed tags. Missing album artist is NOT
/// substituted with the track artist: FRD section 9 forbids that specific
/// automatic fallback, so it becomes a review exception instead.
NamingInput namingInputFromSnapshot(const TagSnapshot& snapshot, std::string_view sourceExtension = ".mp3");

class NamingTemplate {
public:
	explicit NamingTemplate(NamingOptions options = {});

	/// The literal pattern this class implements, for display and for the
	/// generated naming-convention report.
	static std::string_view pattern() { return "{album_artist}/{album}/{track:02}. {artist} - {title}.mp3"; }

	NamingResult apply(const NamingInput& input) const;

	/// Sanitises one path component. Returns the sanitised text and appends any
	/// exceptions raised. Exposed for tests and for the review preview.
	std::string sanitiseComponent(std::string_view raw, std::vector<NamingException>& exceptions) const;

	const NamingOptions& options() const { return m_options; }

private:
	NamingOptions m_options;
};

/// Detects destination collisions across a whole planned change set.
///
/// NAME-002 forbids overwriting a collision. Comparison is case-insensitive and
/// Unicode-aware enough to catch the cases that actually collide on Windows and
/// on case-insensitive macOS volumes.
class CollisionDetector {
public:
	struct Collision {
		std::string relativePath;
		std::vector<FileId> files;
		bool caseOnly = false;   ///< Paths differ only by case.
	};

	/// Returns false when `path` collides with one already registered.
	bool add(const std::string& relativePath, FileId file);

	const std::vector<Collision>& collisions() const { return m_collisions; }
	std::size_t registeredCount() const { return m_byFoldedPath.size(); }
	void clear();

private:
	std::map<std::string, std::pair<std::string, std::vector<FileId>>> m_byFoldedPath;
	std::vector<Collision> m_collisions;
};

/// Case-folds and normalises a path for collision comparison. Not a security
/// boundary; that is `PathGuard` in the infrastructure layer.
std::string foldPathForComparison(std::string_view path);

} // namespace ml
