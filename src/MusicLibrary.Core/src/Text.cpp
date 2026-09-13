// SPDX-License-Identifier: GPL-3.0-or-later
#include "mlcore/Text.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <numeric>

namespace ml::text {

namespace {

constexpr char32_t kReplacement = 0xFFFD;

/// Maps a code point to its folded form. Covers ASCII, Latin-1 supplement and
/// Latin Extended-A, which is the range that appears in this collection's
/// metadata. Outside those ranges the code point is returned unchanged.
char32_t foldCodepoint(char32_t cp) {
	if (cp < 0x80) {
		return (cp >= U'A' && cp <= U'Z') ? cp + 32 : cp;
	}
	// Latin-1 supplement: C0-DE lowercase to E0-FE, excluding D7 (multiplication).
	if (cp >= 0xC0 && cp <= 0xDE && cp != 0xD7) return cp + 0x20;
	// Latin Extended-A: even code points are uppercase in most of the block.
	if (cp >= 0x100 && cp <= 0x137) return (cp % 2 == 0) ? cp + 1 : cp;
	if (cp >= 0x139 && cp <= 0x148) return (cp % 2 == 1) ? cp + 1 : cp;
	if (cp >= 0x14A && cp <= 0x177) return (cp % 2 == 0) ? cp + 1 : cp;
	if (cp == 0x178) return 0xFF;                       // Y with diaeresis
	if (cp >= 0x179 && cp <= 0x17E) return (cp % 2 == 1) ? cp + 1 : cp;
	// Greek and Cyrillic basic ranges.
	if (cp >= 0x391 && cp <= 0x3AB) return cp + 0x20;
	if (cp >= 0x410 && cp <= 0x42F) return cp + 0x20;
	if (cp >= 0x400 && cp <= 0x40F) return cp + 0x50;
	return cp;
}

/// Strips a diacritic where an unambiguous ASCII equivalent exists.
char32_t stripDiacritic(char32_t cp) {
	struct Entry { char32_t from; char32_t to; };
	static constexpr std::array<Entry, 62> kMap = {{
		{0xE0, U'a'}, {0xE1, U'a'}, {0xE2, U'a'}, {0xE3, U'a'}, {0xE4, U'a'}, {0xE5, U'a'},
		{0xE7, U'c'}, {0xE8, U'e'}, {0xE9, U'e'}, {0xEA, U'e'}, {0xEB, U'e'},
		{0xEC, U'i'}, {0xED, U'i'}, {0xEE, U'i'}, {0xEF, U'i'},
		{0xF1, U'n'}, {0xF2, U'o'}, {0xF3, U'o'}, {0xF4, U'o'}, {0xF5, U'o'}, {0xF6, U'o'},
		{0xF9, U'u'}, {0xFA, U'u'}, {0xFB, U'u'}, {0xFC, U'u'}, {0xFD, U'y'}, {0xFF, U'y'},
		{0x101, U'a'}, {0x103, U'a'}, {0x105, U'a'}, {0x107, U'c'}, {0x109, U'c'},
		{0x10B, U'c'}, {0x10D, U'c'}, {0x10F, U'd'}, {0x111, U'd'}, {0x113, U'e'},
		{0x115, U'e'}, {0x117, U'e'}, {0x119, U'e'}, {0x11B, U'e'}, {0x11D, U'g'},
		{0x11F, U'g'}, {0x121, U'g'}, {0x123, U'g'}, {0x125, U'h'}, {0x127, U'h'},
		{0x129, U'i'}, {0x12B, U'i'}, {0x12D, U'i'}, {0x12F, U'i'}, {0x131, U'i'},
		{0x13A, U'l'}, {0x13C, U'l'}, {0x13E, U'l'}, {0x142, U'l'}, {0x144, U'n'},
		{0x146, U'n'}, {0x148, U'n'}, {0x14D, U'o'}, {0x159, U'r'}, {0x161, U's'},
	}};
	for (const auto& e : kMap) {
		if (e.from == cp) return e.to;
	}
	if (cp == 0xDF) return U's';   // sharp s folds to a single s for matching only
	if (cp == 0x17E) return U'z';
	if (cp == 0x17C) return U'z';
	if (cp == 0x17A) return U'z';
	if (cp == 0x16B || cp == 0x16D || cp == 0x16F) return U'u';
	if (cp == 0x163 || cp == 0x165) return U't';
	if (cp == 0x15B || cp == 0x15D || cp == 0x15F) return U's';
	return cp;
}

bool isMatchingPunctuation(char32_t cp) {
	if (cp < 0x80) {
		return std::ispunct(static_cast<int>(cp)) != 0;
	}
	// Typographic quotes and dashes commonly differ between providers.
	return cp == 0x2018 || cp == 0x2019 || cp == 0x201C || cp == 0x201D
		|| cp == 0x2013 || cp == 0x2014 || cp == 0x2026 || cp == 0x00B4;
}

} // namespace

std::vector<char32_t> decodeUtf8(std::string_view utf8) {
	std::vector<char32_t> out;
	out.reserve(utf8.size());

	std::size_t i = 0;
	while (i < utf8.size()) {
		const unsigned char c = static_cast<unsigned char>(utf8[i]);
		std::size_t length = 0;
		char32_t cp = 0;

		if (c < 0x80) { length = 1; cp = c; }
		else if ((c & 0xE0) == 0xC0) { length = 2; cp = c & 0x1F; }
		else if ((c & 0xF0) == 0xE0) { length = 3; cp = c & 0x0F; }
		else if ((c & 0xF8) == 0xF0) { length = 4; cp = c & 0x07; }
		else { out.push_back(kReplacement); ++i; continue; }

		if (i + length > utf8.size()) { out.push_back(kReplacement); ++i; continue; }

		bool valid = true;
		for (std::size_t k = 1; k < length; ++k) {
			const unsigned char cc = static_cast<unsigned char>(utf8[i + k]);
			if ((cc & 0xC0) != 0x80) { valid = false; break; }
			cp = (cp << 6) | (cc & 0x3F);
		}
		// Reject overlong forms, surrogates and out-of-range code points: these
		// are the inputs a fuzzer will produce from a malformed tag.
		if (!valid
			|| (length == 2 && cp < 0x80)
			|| (length == 3 && cp < 0x800)
			|| (length == 4 && cp < 0x10000)
			|| (cp >= 0xD800 && cp <= 0xDFFF)
			|| cp > 0x10FFFF) {
			out.push_back(kReplacement);
			++i;
			continue;
		}

		out.push_back(cp);
		i += length;
	}
	return out;
}

std::string encodeUtf8(const std::vector<char32_t>& codepoints) {
	std::string out;
	out.reserve(codepoints.size() * 2);
	for (char32_t cp : codepoints) {
		if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) cp = kReplacement;
		if (cp < 0x80) {
			out.push_back(static_cast<char>(cp));
		} else if (cp < 0x800) {
			out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
			out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
		} else if (cp < 0x10000) {
			out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
			out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
			out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
		} else {
			out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
			out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
			out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
			out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
		}
	}
	return out;
}

bool isValidUtf8(std::string_view s) {
	std::size_t i = 0;
	while (i < s.size()) {
		const unsigned char c = static_cast<unsigned char>(s[i]);
		std::size_t length = 0;
		char32_t cp = 0;
		if (c < 0x80) { ++i; continue; }
		else if ((c & 0xE0) == 0xC0) { length = 2; cp = c & 0x1F; }
		else if ((c & 0xF0) == 0xE0) { length = 3; cp = c & 0x0F; }
		else if ((c & 0xF8) == 0xF0) { length = 4; cp = c & 0x07; }
		else return false;

		if (i + length > s.size()) return false;
		for (std::size_t k = 1; k < length; ++k) {
			const unsigned char cc = static_cast<unsigned char>(s[i + k]);
			if ((cc & 0xC0) != 0x80) return false;
			cp = (cp << 6) | (cc & 0x3F);
		}
		if ((length == 2 && cp < 0x80) || (length == 3 && cp < 0x800) || (length == 4 && cp < 0x10000)) return false;
		if (cp >= 0xD800 && cp <= 0xDFFF) return false;
		if (cp > 0x10FFFF) return false;
		i += length;
	}
	return true;
}

std::string sanitiseUtf8(std::string_view s) {
	if (isValidUtf8(s)) return std::string(s);
	return encodeUtf8(decodeUtf8(s));
}

std::string foldCase(std::string_view utf8) {
	auto cps = decodeUtf8(utf8);
	for (auto& cp : cps) cp = foldCodepoint(cp);
	return encodeUtf8(cps);
}

std::string normaliseForMatching(std::string_view utf8) {
	const auto cps = decodeUtf8(utf8);
	std::vector<char32_t> out;
	out.reserve(cps.size());

	bool pendingSpace = false;
	for (char32_t cp : cps) {
		cp = foldCodepoint(cp);
		cp = stripDiacritic(cp);

		if (cp < 0x80 && std::isspace(static_cast<int>(cp))) {
			pendingSpace = !out.empty();
			continue;
		}
		if (isMatchingPunctuation(cp)) {
			// Punctuation becomes a word break rather than disappearing, so
			// "re-mix" and "remix" stay distinguishable from "re mix".
			pendingSpace = !out.empty();
			continue;
		}
		if (pendingSpace) {
			out.push_back(U' ');
			pendingSpace = false;
		}
		out.push_back(cp);
	}
	return encodeUtf8(out);
}

StrippedTitle stripQualifiers(std::string_view utf8) {
	StrippedTitle result;
	std::string base;
	base.reserve(utf8.size());

	int depth = 0;
	std::string current;
	for (std::size_t i = 0; i < utf8.size(); ++i) {
		const char c = utf8[i];
		if (c == '(' || c == '[' || c == '{') {
			if (depth == 0) {
				current.clear();
			} else {
				current.push_back(c);
			}
			++depth;
			continue;
		}
		if ((c == ')' || c == ']' || c == '}') && depth > 0) {
			--depth;
			if (depth == 0) {
				std::string q = trim(current);
				if (!q.empty()) result.qualifiers.push_back(std::move(q));
				current.clear();
			} else {
				current.push_back(c);
			}
			continue;
		}
		if (depth > 0) {
			current.push_back(c);
		} else {
			base.push_back(c);
		}
	}
	// An unterminated bracket means the text was truncated; keep what we read as
	// a qualifier rather than losing it.
	if (depth > 0 && !trim(current).empty()) result.qualifiers.push_back(trim(current));

	result.base = trim(base);
	return result;
}

bool isEditionQualifier(std::string_view lowercaseQualifier) {
	static constexpr std::array<std::string_view, 24> kMarkers = {
		"remix", "mix", "edit", "version", "live", "acoustic", "instrumental",
		"remaster", "remastered", "deluxe", "expanded", "anniversary", "extended",
		"radio", "club", "dub", "reprise", "demo", "bonus", "single", "album",
		"original", "reissue", "special",
	};
	for (auto m : kMarkers) {
		if (lowercaseQualifier.find(m) != std::string_view::npos) return true;
	}
	return false;
}

namespace {

/// Trailing markers catalogues append to a release title.
const std::array<std::string_view, 8> kTrailingMarkers = {
	"single", "ep", "deluxe edition", "deluxe", "remixes", "remix ep", "bonus track version", "explicit",
};

/// Separators that join several artists into one credit string.
const std::array<std::string_view, 10> kArtistSeparators = {
	" & ", " and ", ", ", " feat. ", " feat ", " featuring ", " ft. ", " ft ", " with ", " vs ",
};

} // namespace

std::vector<std::string> splitArtistCredit(std::string_view credit) {
	std::string working = std::string(credit);
	std::vector<std::string> parts{working};

	for (auto separator : kArtistSeparators) {
		std::vector<std::string> next;
		for (const auto& part : parts) {
			const std::string lowerPart = toLowerAscii(part);
			std::size_t start = 0;
			while (true) {
				const std::size_t position = lowerPart.find(separator, start);
				if (position == std::string::npos) {
					next.push_back(trim(part.substr(start)));
					break;
				}
				next.push_back(trim(part.substr(start, position - start)));
				start = position + separator.size();
			}
		}
		parts.clear();
		for (auto& p : next) {
			if (!p.empty()) parts.push_back(std::move(p));
		}
	}
	return parts;
}

TitleComparison compareTitles(std::string_view a, std::string_view b) {
	TitleComparison result;

	// Split a trailing " - Marker" form, which is how catalogues label singles
	// and EPs, and also collect bracketed qualifiers.
	const auto peel = [](std::string_view text, std::string& markerOut) {
		std::string base = trim(text);
		const std::size_t dash = base.rfind(" - ");
		if (dash != std::string::npos) {
			const std::string tail = toLowerAscii(trim(base.substr(dash + 3)));
			for (auto marker : kTrailingMarkers) {
				if (tail == marker) {
					markerOut = trim(base.substr(dash + 3));
					base = trim(base.substr(0, dash));
					break;
				}
			}
		}
		const auto stripped = stripQualifiers(base);
		for (const auto& q : stripped.qualifiers) {
			const std::string lower = toLowerAscii(q);
			for (auto marker : kTrailingMarkers) {
				if (lower == marker && markerOut.empty()) markerOut = q;
			}
		}
		return stripped.base.empty() ? base : stripped.base;
	};

	const std::string baseA = peel(a, result.leftMarker);
	const std::string baseB = peel(b, result.rightMarker);

	result.similarity = similarity(baseA, baseB);

	// An edition marker on one side only is worth surfacing: it is the
	// difference between a standard release and a deluxe one.
	const std::string lowerLeft = toLowerAscii(result.leftMarker);
	const std::string lowerRight = toLowerAscii(result.rightMarker);
	const bool leftIsEdition = !lowerLeft.empty() && lowerLeft != "single" && lowerLeft != "ep";
	const bool rightIsEdition = !lowerRight.empty() && lowerRight != "single" && lowerRight != "ep";
	result.markersAgree = (leftIsEdition == rightIsEdition) && (lowerLeft == lowerRight || !leftIsEdition);

	return result;
}

ArtistComparison compareArtists(std::string_view a, std::string_view b) {
	ArtistComparison result;
	result.similarity = similarity(a, b);

	const auto partsA = splitArtistCredit(a);
	const auto partsB = splitArtistCredit(b);
	if (partsA.empty() || partsB.empty()) return result;

	const auto matches = [](const std::vector<std::string>& haystack, const std::string& needle) {
		for (const auto& candidate : haystack) {
			if (similarity(candidate, needle) > 0.9) return true;
		}
		return false;
	};

	// Containment: every name on the shorter side appears on the longer side.
	const auto& shorter = (partsA.size() <= partsB.size()) ? partsA : partsB;
	const auto& longer = (partsA.size() <= partsB.size()) ? partsB : partsA;

	std::size_t found = 0;
	for (const auto& name : shorter) {
		if (matches(longer, name)) ++found;
	}
	result.oneContainsTheOther = (found == shorter.size());

	for (const auto& name : longer) {
		if (!matches(shorter, name)) result.extraCredits.push_back(name);
	}

	if (result.oneContainsTheOther) {
		// A shared primary artist with extra collaborators is a strong signal,
		// but not identity: the extra credits are reported so a caller can decide.
		const double containmentScore = 0.95 - 0.05 * static_cast<double>(result.extraCredits.size());
		result.similarity = std::max(result.similarity, std::max(containmentScore, 0.8));
	}

	return result;
}

std::size_t editDistance(std::string_view a, std::string_view b, std::size_t cap) {
	const auto ca = decodeUtf8(a);
	const auto cb = decodeUtf8(b);

	if (ca.empty()) return std::min(cb.size(), cap);
	if (cb.empty()) return std::min(ca.size(), cap);

	// Bail out early on lengths that cannot come within the cap.
	const std::size_t diff = (ca.size() > cb.size()) ? ca.size() - cb.size() : cb.size() - ca.size();
	if (diff > cap) return cap;

	std::vector<std::size_t> previous(cb.size() + 1);
	std::vector<std::size_t> current(cb.size() + 1);
	std::iota(previous.begin(), previous.end(), std::size_t{0});

	for (std::size_t i = 1; i <= ca.size(); ++i) {
		current[0] = i;
		std::size_t rowMin = current[0];
		for (std::size_t j = 1; j <= cb.size(); ++j) {
			const std::size_t cost = (ca[i - 1] == cb[j - 1]) ? 0 : 1;
			current[j] = std::min({previous[j] + 1, current[j - 1] + 1, previous[j - 1] + cost});
			rowMin = std::min(rowMin, current[j]);
		}
		if (rowMin > cap) return cap;
		previous.swap(current);
	}
	return std::min(previous[cb.size()], cap);
}

double similarity(std::string_view a, std::string_view b) {
	const std::string na = normaliseForMatching(a);
	const std::string nb = normaliseForMatching(b);
	if (na.empty() && nb.empty()) return 1.0;
	if (na.empty() || nb.empty()) return 0.0;
	if (na == nb) return 1.0;

	const std::size_t longest = std::max(decodeUtf8(na).size(), decodeUtf8(nb).size());
	const std::size_t cap = std::min<std::size_t>(longest, 256);
	const std::size_t distance = editDistance(na, nb, cap);
	if (longest == 0) return 1.0;
	const double s = 1.0 - static_cast<double>(distance) / static_cast<double>(longest);
	return std::max(0.0, std::min(1.0, s));
}

std::string trim(std::string_view s) {
	std::size_t b = 0;
	std::size_t e = s.size();
	while (b < e && static_cast<unsigned char>(s[b]) <= 0x20) ++b;
	while (e > b && static_cast<unsigned char>(s[e - 1]) <= 0x20) --e;
	return std::string(s.substr(b, e - b));
}

std::string toLowerAscii(std::string_view s) {
	std::string out(s);
	std::transform(out.begin(), out.end(), out.begin(),
		[](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	return out;
}

std::vector<std::string> split(std::string_view s, char delimiter) {
	std::vector<std::string> out;
	std::size_t start = 0;
	while (true) {
		const std::size_t pos = s.find(delimiter, start);
		if (pos == std::string_view::npos) {
			out.emplace_back(s.substr(start));
			break;
		}
		out.emplace_back(s.substr(start, pos - start));
		start = pos + 1;
	}
	return out;
}

std::string join(const std::vector<std::string>& parts, std::string_view separator) {
	std::string out;
	for (std::size_t i = 0; i < parts.size(); ++i) {
		if (i > 0) out += separator;
		out += parts[i];
	}
	return out;
}

bool equalsNoCase(std::string_view s, std::string_view other) {
	if (s.size() != other.size()) return false;
	for (std::size_t i = 0; i < s.size(); ++i) {
		if (std::tolower(static_cast<unsigned char>(s[i])) != std::tolower(static_cast<unsigned char>(other[i]))) {
			return false;
		}
	}
	return true;
}

bool startsWithNoCase(std::string_view s, std::string_view prefix) {
	if (s.size() < prefix.size()) return false;
	return equalsNoCase(s.substr(0, prefix.size()), prefix);
}

bool containsNoCase(std::string_view haystack, std::string_view needle) {
	if (needle.empty()) return true;
	if (haystack.size() < needle.size()) return false;
	const std::size_t limit = haystack.size() - needle.size();
	for (std::size_t i = 0; i <= limit; ++i) {
		if (equalsNoCase(haystack.substr(i, needle.size()), needle)) return true;
	}
	return false;
}

std::string jsonEscape(std::string_view s) {
	std::string out;
	out.reserve(s.size() + 8);
	for (char c : s) {
		switch (c) {
			case '"': out += "\\\""; break;
			case '\\': out += "\\\\"; break;
			case '\n': out += "\\n"; break;
			case '\r': out += "\\r"; break;
			case '\t': out += "\\t"; break;
			case '\b': out += "\\b"; break;
			case '\f': out += "\\f"; break;
			default:
				if (static_cast<unsigned char>(c) < 0x20) {
					char buf[8];
					std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned char>(c));
					out += buf;
				} else {
					out.push_back(c);
				}
		}
	}
	return out;
}

std::string csvEscape(std::string_view s) {
	const bool needsQuotes = s.find_first_of(",\"\n\r") != std::string_view::npos;
	if (!needsQuotes) return std::string(s);
	std::string out = "\"";
	for (char c : s) {
		if (c == '"') out += "\"\"";
		else out.push_back(c);
	}
	out += "\"";
	return out;
}

std::string formatBytes(std::int64_t bytes) {
	const char* units[] = {"B", "KiB", "MiB", "GiB", "TiB"};
	double value = static_cast<double>(bytes);
	int unit = 0;
	const bool negative = value < 0;
	if (negative) value = -value;
	while (value >= 1024.0 && unit < 4) {
		value /= 1024.0;
		++unit;
	}
	char buf[64];
	if (unit == 0) {
		std::snprintf(buf, sizeof(buf), "%s%lld B", negative ? "-" : "", static_cast<long long>(negative ? -bytes : bytes));
	} else {
		std::snprintf(buf, sizeof(buf), "%s%.1f %s", negative ? "-" : "", value, units[unit]);
	}
	return buf;
}

std::string formatDuration(std::int64_t milliseconds) {
	if (milliseconds < 0) milliseconds = 0;
	const std::int64_t totalSeconds = milliseconds / 1000;
	const std::int64_t hours = totalSeconds / 3600;
	const std::int64_t minutes = (totalSeconds % 3600) / 60;
	const std::int64_t seconds = totalSeconds % 60;
	char buf[32];
	if (hours > 0) {
		std::snprintf(buf, sizeof(buf), "%lld:%02lld:%02lld",
			static_cast<long long>(hours), static_cast<long long>(minutes), static_cast<long long>(seconds));
	} else {
		std::snprintf(buf, sizeof(buf), "%lld:%02lld",
			static_cast<long long>(minutes), static_cast<long long>(seconds));
	}
	return buf;
}

} // namespace ml::text
