// SPDX-License-Identifier: GPL-3.0-or-later
// UTF-8 helpers used by matching, collision detection and reporting.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace ml::text {

/// Converts a filesystem path to UTF-8.
///
/// Never use `path.string()` or `path.generic_string()` for this: on Windows
/// they convert through the system's native ANSI codepage, and for a
/// filename containing a character outside it — Cyrillic, CJK, and other
/// ordinary real-world cases, not just exotica — the MSVC STL's conversion
/// does not degrade gracefully. It crashes (a stack buffer overrun caught by
/// /GS), not merely mangles the text. `u8string()` sidesteps the ANSI
/// conversion entirely. On POSIX the native encoding is already UTF-8, so
/// this is a no-op there.
std::string pathToUtf8(const std::filesystem::path& path);

/// Case-folds ASCII plus the Latin-1 supplement and Latin Extended-A. Enough to
/// catch the case collisions that actually occur in music metadata without
/// linking a full Unicode library.
std::string foldCase(std::string_view utf8);

/// Strips diacritics where a simple decomposition exists, folds case, collapses
/// whitespace and removes punctuation. Used only for *matching* candidates; the
/// canonical tag values are never normalised (FRD section 9).
std::string normaliseForMatching(std::string_view utf8);

/// Removes bracketed qualifiers such as "(Remastered 2011)" or "[Deluxe]" for a
/// secondary comparison pass. The qualifier itself is returned separately so the
/// caller can treat an edition difference as evidence rather than noise.
struct StrippedTitle {
	std::string base;
	std::vector<std::string> qualifiers;
};
StrippedTitle stripQualifiers(std::string_view utf8);

/// Recognised edition/version qualifiers. Presence changes album identity, so
/// two titles differing only by one of these must not be merged (FN-ALB-02).
bool isEditionQualifier(std::string_view lowercaseQualifier);

/// Compares two release titles, tolerating the suffixes catalogues add.
///
/// "Desire" and "Desire - Single" denote the same release; a raw edit-distance
/// similarity scores that pair at 0.46 and would discard a correct match. The
/// trailing marker is removed for the comparison and returned separately, so it
/// stays available as edition evidence rather than being silently ignored.
struct TitleComparison {
	double similarity = 0.0;     ///< Similarity of the base titles.
	std::string leftMarker;      ///< "Single", "EP", "Deluxe"... found on the left.
	std::string rightMarker;
	bool markersAgree = true;    ///< False when one side declares an edition the other does not.
};
TitleComparison compareTitles(std::string_view a, std::string_view b);

/// Compares artist credits, tolerating collaboration and featuring forms.
///
/// "Calvin Harris" and "Calvin Harris & Sam Smith" are the same primary artist
/// with a collaborator. Plain similarity scores that pair low; containment of
/// the shorter credit's tokens in the longer one is the signal that matters.
struct ArtistComparison {
	double similarity = 0.0;       ///< Best of direct similarity and containment.
	bool oneContainsTheOther = false;
	std::vector<std::string> extraCredits;   ///< Names present on only one side.
};
ArtistComparison compareArtists(std::string_view a, std::string_view b);

/// Splits an artist credit into individual names on "&", ",", "feat.", "with",
/// "vs" and similar separators.
std::vector<std::string> splitArtistCredit(std::string_view credit);

/// Levenshtein distance over Unicode code points, capped for performance.
std::size_t editDistance(std::string_view a, std::string_view b, std::size_t cap = 64);

/// Similarity in [0,1] using the capped edit distance over normalised text.
double similarity(std::string_view a, std::string_view b);

/// Decodes UTF-8 into code points, replacing invalid sequences with U+FFFD so
/// malformed tag text can never crash the matcher (FRD section 15).
std::vector<char32_t> decodeUtf8(std::string_view utf8);
std::string encodeUtf8(const std::vector<char32_t>& codepoints);

/// True when the string is well-formed UTF-8.
bool isValidUtf8(std::string_view s);

/// Replaces invalid UTF-8 sequences with U+FFFD, returning valid UTF-8.
std::string sanitiseUtf8(std::string_view s);

/// Trims ASCII whitespace and control characters from both ends.
std::string trim(std::string_view s);

/// Lowercases ASCII only. Cheap path for machine keys.
std::string toLowerAscii(std::string_view s);

/// Splits on a single-character delimiter without allocating a regex.
std::vector<std::string> split(std::string_view s, char delimiter);

/// Joins with a separator.
std::string join(const std::vector<std::string>& parts, std::string_view separator);

/// True when `haystack` contains `needle`, both compared case-insensitively in
/// ASCII. Used for policy matching on frame descriptions.
bool containsNoCase(std::string_view haystack, std::string_view needle);

/// True when `s` equals `other` ignoring ASCII case.
bool equalsNoCase(std::string_view s, std::string_view other);

/// True when `s` starts with `prefix`, ignoring ASCII case.
bool startsWithNoCase(std::string_view s, std::string_view prefix);

/// Escapes a string for inclusion in a JSON document.
std::string jsonEscape(std::string_view s);

/// Escapes a string for inclusion in a CSV field, adding quotes when needed.
std::string csvEscape(std::string_view s);

/// Formats a byte count for display, for example "4.2 MiB".
std::string formatBytes(std::int64_t bytes);

/// Formats milliseconds as "m:ss" or "h:mm:ss".
std::string formatDuration(std::int64_t milliseconds);

} // namespace ml::text
