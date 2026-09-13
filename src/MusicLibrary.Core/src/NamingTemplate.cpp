// SPDX-License-Identifier: GPL-3.0-or-later
#include "mlcore/NamingTemplate.hpp"
#include "mlcore/Text.hpp"

#include <algorithm>
#include <array>
#include <cctype>

namespace ml {

std::string_view toString(NamingExceptionKind k) {
	switch (k) {
		case NamingExceptionKind::None: return "none";
		case NamingExceptionKind::MissingAlbumArtist: return "missing_album_artist";
		case NamingExceptionKind::MissingAlbum: return "missing_album";
		case NamingExceptionKind::MissingArtist: return "missing_artist";
		case NamingExceptionKind::MissingTitle: return "missing_title";
		case NamingExceptionKind::MissingTrackNumber: return "missing_track_number";
		case NamingExceptionKind::InvalidTrackNumber: return "invalid_track_number";
		case NamingExceptionKind::MultipleValues: return "multiple_values";
		case NamingExceptionKind::ReservedName: return "reserved_name";
		case NamingExceptionKind::IllegalCharacters: return "illegal_characters";
		case NamingExceptionKind::TrailingSpaceOrDot: return "trailing_space_or_dot";
		case NamingExceptionKind::PathTooLong: return "path_too_long";
		case NamingExceptionKind::EmptyComponent: return "empty_component";
		case NamingExceptionKind::MultiDiscCollision: return "multi_disc_collision";
	}
	return "none";
}

namespace {

/// Windows device names, reserved with or without an extension.
bool isWindowsReservedName(std::string_view component) {
	// Compare only the stem: "COM1.mp3" is still reserved.
	std::size_t dot = component.find('.');
	std::string_view stem = (dot == std::string_view::npos) ? component : component.substr(0, dot);

	std::string upper;
	upper.reserve(stem.size());
	for (char c : stem) upper.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));

	static constexpr std::array<std::string_view, 4> kSimple = {"CON", "PRN", "AUX", "NUL"};
	for (auto r : kSimple) {
		if (upper == r) return true;
	}
	for (int i = 1; i <= 9; ++i) {
		if (upper == std::string("COM") + char('0' + i)) return true;
		if (upper == std::string("LPT") + char('0' + i)) return true;
	}
	return false;
}

/// Characters Windows forbids in a path component. POSIX only forbids '/' and
/// NUL, but the library's home is Windows, so the strict set is the default.
bool isWindowsIllegal(unsigned char c) {
	if (c < 0x20) return true;
	switch (c) {
		case '<': case '>': case ':': case '"': case '/':
		case '\\': case '|': case '?': case '*':
			return true;
		default:
			return false;
	}
}

void addException(std::vector<NamingException>& out, NamingExceptionKind kind, std::string detail, bool advisory) {
	// Collapse repeats of the same advisory kind so a title with five illegal
	// characters produces one review item, not five.
	for (auto& e : out) {
		if (e.kind == kind && e.advisory == advisory) {
			e.detail += "; ";
			e.detail += detail;
			return;
		}
	}
	out.push_back(NamingException{kind, std::move(detail), advisory});
}

} // namespace

NamingTemplate::NamingTemplate(NamingOptions options) : m_options(std::move(options)) {}

std::string NamingTemplate::sanitiseComponent(std::string_view raw, std::vector<NamingException>& exceptions) const {
	std::string out;
	out.reserve(raw.size());

	bool replacedAny = false;
	std::string replacedList;

	for (std::size_t i = 0; i < raw.size(); ++i) {
		const unsigned char c = static_cast<unsigned char>(raw[i]);
		// Bytes >= 0x80 belong to a UTF-8 sequence. They are copied through so
		// accents and non-Latin scripts survive (FRD section 9).
		if (c >= 0x80) {
			out.push_back(raw[i]);
			continue;
		}
		const bool illegal = m_options.enforceWindowsRules ? isWindowsIllegal(c) : (c == '/' || c == 0);
		if (illegal) {
			out += m_options.replacementChar;
			if (!replacedAny) replacedAny = true;
			if (replacedList.find(static_cast<char>(c)) == std::string::npos && c >= 0x20) {
				replacedList.push_back(static_cast<char>(c));
			}
			continue;
		}
		out.push_back(raw[i]);
	}

	if (replacedAny) {
		std::string detail = "replaced ";
		detail += replacedList.empty() ? std::string("control characters") : ("'" + replacedList + "'");
		detail += " in \"";
		detail += std::string(raw);
		detail += "\"";
		// Advisory: the result is still deterministic and usable.
		addException(exceptions, NamingExceptionKind::IllegalCharacters, std::move(detail), true);
	}

	// Windows silently strips trailing spaces and dots, which would make two
	// different names collide. Trim them ourselves and say so.
	std::size_t end = out.size();
	while (end > 0 && (out[end - 1] == ' ' || out[end - 1] == '.')) --end;
	if (end != out.size()) {
		addException(exceptions, NamingExceptionKind::TrailingSpaceOrDot,
			"trimmed trailing space or dot from \"" + out + "\"", true);
		out.resize(end);
	}

	// Leading spaces are legal but produce confusing directory listings.
	std::size_t begin = 0;
	while (begin < out.size() && out[begin] == ' ') ++begin;
	if (begin > 0) out.erase(0, begin);

	if (m_options.enforceWindowsRules && !out.empty() && isWindowsReservedName(out)) {
		addException(exceptions, NamingExceptionKind::ReservedName,
			"\"" + out + "\" is a reserved device name on Windows", false);
	}

	return out;
}

NamingResult NamingTemplate::apply(const NamingInput& input) const {
	NamingResult result;

	// --- Required values ---------------------------------------------------
	// A missing album artist is never filled from the track artist: FRD section 9
	// explicitly forbids that substitution.
	if (input.albumArtist.empty()) {
		addException(result.exceptions, NamingExceptionKind::MissingAlbumArtist,
			"no TPE2 album artist; template requires one and substituting TPE1 is not permitted", false);
	}
	if (input.album.empty()) {
		addException(result.exceptions, NamingExceptionKind::MissingAlbum, "no TALB album title", false);
	}
	if (input.artist.empty()) {
		addException(result.exceptions, NamingExceptionKind::MissingArtist, "no TPE1 track artist", false);
	}
	if (input.title.empty()) {
		addException(result.exceptions, NamingExceptionKind::MissingTitle, "no TIT2 title", false);
	}
	if (!input.trackNumber.has_value()) {
		addException(result.exceptions, NamingExceptionKind::MissingTrackNumber,
			"no numeric TRCK track number", false);
	} else if (*input.trackNumber <= 0) {
		addException(result.exceptions, NamingExceptionKind::InvalidTrackNumber,
			"track number " + std::to_string(*input.trackNumber) + " is not positive", false);
	}

	if (input.albumArtistHadMultipleValues) {
		addException(result.exceptions, NamingExceptionKind::MultipleValues,
			"album artist tag carried multiple values; choosing one is a review decision", false);
	}
	if (input.artistHadMultipleValues) {
		addException(result.exceptions, NamingExceptionKind::MultipleValues,
			"artist tag carried multiple values; choosing one is a review decision", false);
	}

	// --- Component assembly -------------------------------------------------
	result.albumArtistComponent = sanitiseComponent(input.albumArtist, result.exceptions);
	result.albumComponent = sanitiseComponent(input.album, result.exceptions);

	const std::string artist = sanitiseComponent(input.artist, result.exceptions);
	const std::string title = sanitiseComponent(input.title, result.exceptions);

	std::string trackPrefix;
	if (input.trackNumber && *input.trackNumber > 0) {
		trackPrefix = std::to_string(*input.trackNumber);
		while (static_cast<int>(trackPrefix.size()) < m_options.trackDigits) {
			trackPrefix.insert(trackPrefix.begin(), '0');
		}
	}

	// Sanitisation can empty a component entirely, for example a title that was
	// only illegal characters. That is a review case, not a silent "Unknown".
	if (!input.albumArtist.empty() && result.albumArtistComponent.empty()) {
		addException(result.exceptions, NamingExceptionKind::EmptyComponent,
			"album artist became empty after sanitisation", false);
	}
	if (!input.album.empty() && result.albumComponent.empty()) {
		addException(result.exceptions, NamingExceptionKind::EmptyComponent,
			"album became empty after sanitisation", false);
	}
	if (!input.title.empty() && title.empty()) {
		addException(result.exceptions, NamingExceptionKind::EmptyComponent,
			"title became empty after sanitisation", false);
	}

	// The extension is preserved from the actual file, not assumed.
	std::string extension = input.extension.empty() ? std::string(".mp3") : input.extension;
	if (extension.front() != '.') extension.insert(extension.begin(), '.');

	// "{track:02}. {artist} - {title}.mp3"
	result.fileComponent = trackPrefix + ". " + artist + " - " + title + extension;

	// Re-check the assembled filename: the separators cannot create a reserved
	// name, but a trailing dot can appear when the title ended in one.
	if (m_options.enforceWindowsRules) {
		const std::size_t dot = result.fileComponent.rfind('.');
		if (dot != std::string::npos) {
			std::string stem = result.fileComponent.substr(0, dot);
			if (!stem.empty() && (stem.back() == ' ' || stem.back() == '.')) {
				addException(result.exceptions, NamingExceptionKind::TrailingSpaceOrDot,
					"filename stem ends with a space or dot", true);
				while (!stem.empty() && (stem.back() == ' ' || stem.back() == '.')) stem.pop_back();
				result.fileComponent = stem + extension;
			}
		}
	}

	// --- Decide whether the result is usable --------------------------------
	for (const auto& e : result.exceptions) {
		if (!e.advisory) result.requiresReview = true;
	}

	if (!result.requiresReview) {
		result.relativePath = result.albumArtistComponent + "/" + result.albumComponent + "/" + result.fileComponent;

		if (result.relativePath.size() > m_options.maxPathLength) {
			addException(result.exceptions, NamingExceptionKind::PathTooLong,
				"relative path is " + std::to_string(result.relativePath.size()) + " characters, limit is "
					+ std::to_string(m_options.maxPathLength), false);
			result.requiresReview = true;
			result.relativePath.clear();
		}
	}

	return result;
}

NamingInput namingInputFromSnapshot(const TagSnapshot& snapshot, std::string_view sourceExtension) {
	NamingInput in;
	in.albumArtist = snapshot.albumArtist();
	in.album = snapshot.album();
	in.artist = snapshot.artist();
	in.title = snapshot.title();
	in.trackNumber = snapshot.trackNumber();
	in.discNumber = snapshot.discNumber();
	in.discTotal = snapshot.discTotal();
	in.extension = std::string(sourceExtension);

	// ID3v2.4 separates multiple values with NUL. TagLib joins them; either way a
	// surviving NUL or a null-joined value means the decision is not ours.
	const auto hasMultiple = [](const std::string& v) {
		return v.find('\0') != std::string::npos;
	};
	in.albumArtistHadMultipleValues = hasMultiple(in.albumArtist);
	in.artistHadMultipleValues = hasMultiple(in.artist);
	if (const TagFrame* f = snapshot.find("TPE2")) {
		if (f->value.find('\0') != std::string::npos) in.albumArtistHadMultipleValues = true;
	}
	if (const TagFrame* f = snapshot.find("TPE1")) {
		if (f->value.find('\0') != std::string::npos) in.artistHadMultipleValues = true;
	}
	return in;
}

// ---------------------------------------------------------------------------
// Collision detection
// ---------------------------------------------------------------------------

std::string foldPathForComparison(std::string_view path) {
	// Normalise separators, then case-fold. Unicode case folding beyond ASCII is
	// handled by Text::foldCase, which covers the Latin-1 supplement and common
	// accented forms that actually collide in practice.
	std::string normalised;
	normalised.reserve(path.size());
	for (char c : path) normalised.push_back(c == '\\' ? '/' : c);
	return text::foldCase(normalised);
}

bool CollisionDetector::add(const std::string& relativePath, FileId file) {
	const std::string folded = foldPathForComparison(relativePath);
	auto it = m_byFoldedPath.find(folded);
	if (it == m_byFoldedPath.end()) {
		m_byFoldedPath.emplace(folded, std::make_pair(relativePath, std::vector<FileId>{file}));
		return true;
	}

	it->second.second.push_back(file);

	const bool caseOnly = (it->second.first != relativePath);
	for (auto& c : m_collisions) {
		if (foldPathForComparison(c.relativePath) == folded) {
			c.files.push_back(file);
			c.caseOnly = c.caseOnly || caseOnly;
			return false;
		}
	}
	Collision c;
	c.relativePath = it->second.first;
	c.files = it->second.second;
	c.caseOnly = caseOnly;
	m_collisions.push_back(std::move(c));
	return false;
}

void CollisionDetector::clear() {
	m_byFoldedPath.clear();
	m_collisions.clear();
}

} // namespace ml
