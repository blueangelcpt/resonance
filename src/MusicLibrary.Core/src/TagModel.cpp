// SPDX-License-Identifier: GPL-3.0-or-later
#include "mlcore/TagModel.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>

namespace ml {

std::string_view toString(ErrorCode code) {
	switch (code) {
		case ErrorCode::Ok: return "ok";
		case ErrorCode::NotFound: return "not_found";
		case ErrorCode::InvalidArgument: return "invalid_argument";
		case ErrorCode::IoError: return "io_error";
		case ErrorCode::ParseError: return "parse_error";
		case ErrorCode::Unsupported: return "unsupported";
		case ErrorCode::PermissionDenied: return "permission_denied";
		case ErrorCode::ProtectedRootViolation: return "protected_root_violation";
		case ErrorCode::Collision: return "collision";
		case ErrorCode::Conflict: return "conflict";
		case ErrorCode::NetworkError: return "network_error";
		case ErrorCode::RateLimited: return "rate_limited";
		case ErrorCode::Cancelled: return "cancelled";
		case ErrorCode::VerificationFailed: return "verification_failed";
		case ErrorCode::DatabaseError: return "database_error";
		case ErrorCode::Internal: return "internal";
	}
	return "internal";
}

std::string Error::describe() const {
	std::string out(toString(code));
	if (!message.empty()) {
		out += ": ";
		out += message;
	}
	return out;
}

std::string_view toString(Confidence c) {
	switch (c) {
		case Confidence::Unknown: return "unknown";
		case Confidence::Weak: return "weak";
		case Confidence::Moderate: return "moderate";
		case Confidence::Strong: return "strong";
		case Confidence::Locked: return "locked";
	}
	return "unknown";
}

std::optional<Confidence> confidenceFromString(std::string_view s) {
	if (s == "unknown") return Confidence::Unknown;
	if (s == "weak") return Confidence::Weak;
	if (s == "moderate") return Confidence::Moderate;
	if (s == "strong") return Confidence::Strong;
	if (s == "locked") return Confidence::Locked;
	return std::nullopt;
}

std::string_view toString(BitrateMode m) {
	switch (m) {
		case BitrateMode::Unknown: return "unknown";
		case BitrateMode::Cbr: return "cbr";
		case BitrateMode::Vbr: return "vbr";
		case BitrateMode::Abr: return "abr";
	}
	return "unknown";
}

std::string_view toString(TagContainer c) {
	switch (c) {
		case TagContainer::Unknown: return "unknown";
		case TagContainer::Id3v1: return "id3v1";
		case TagContainer::Id3v2_2: return "id3v2.2";
		case TagContainer::Id3v2_3: return "id3v2.3";
		case TagContainer::Id3v2_4: return "id3v2.4";
		case TagContainer::Apev2: return "apev2";
		case TagContainer::LameHeader: return "lame_header";
	}
	return "unknown";
}

std::optional<TagContainer> tagContainerFromString(std::string_view s) {
	if (s == "id3v1") return TagContainer::Id3v1;
	if (s == "id3v2.2") return TagContainer::Id3v2_2;
	if (s == "id3v2.3") return TagContainer::Id3v2_3;
	if (s == "id3v2.4") return TagContainer::Id3v2_4;
	if (s == "apev2") return TagContainer::Apev2;
	if (s == "lame_header") return TagContainer::LameHeader;
	if (s == "unknown") return TagContainer::Unknown;
	return std::nullopt;
}

bool isId3v2(TagContainer c) {
	return c == TagContainer::Id3v2_2 || c == TagContainer::Id3v2_3 || c == TagContainer::Id3v2_4;
}

std::string_view toString(TextEncoding e) {
	switch (e) {
		case TextEncoding::Unknown: return "unknown";
		case TextEncoding::Latin1: return "latin1";
		case TextEncoding::Utf16: return "utf16";
		case TextEncoding::Utf16Be: return "utf16be";
		case TextEncoding::Utf8: return "utf8";
	}
	return "unknown";
}

std::string_view toString(PictureType t) {
	switch (t) {
		case PictureType::Other: return "other";
		case PictureType::FileIcon32: return "file_icon_32";
		case PictureType::OtherFileIcon: return "other_file_icon";
		case PictureType::FrontCover: return "front_cover";
		case PictureType::BackCover: return "back_cover";
		case PictureType::LeafletPage: return "leaflet_page";
		case PictureType::Media: return "media";
		case PictureType::LeadArtist: return "lead_artist";
		case PictureType::Artist: return "artist";
		case PictureType::Conductor: return "conductor";
		case PictureType::Band: return "band";
		case PictureType::Composer: return "composer";
		case PictureType::Lyricist: return "lyricist";
		case PictureType::RecordingLocation: return "recording_location";
		case PictureType::DuringRecording: return "during_recording";
		case PictureType::DuringPerformance: return "during_performance";
		case PictureType::ScreenCapture: return "screen_capture";
		case PictureType::Illustration: return "illustration";
		case PictureType::BandLogo: return "band_logo";
		case PictureType::PublisherLogo: return "publisher_logo";
	}
	return "other";
}

std::string TagFrame::key() const {
	std::string k(toString(container));
	k += '/';
	k += id;
	if (!owner.empty()) {
		k += ":owner=";
		k += owner;
	}
	if (!description.empty()) {
		k += ":desc=";
		k += description;
	}
	if (!language.empty()) {
		k += ":lang=";
		k += language;
	}
	if (ordinal > 0) {
		k += ":#";
		k += std::to_string(ordinal);
	}
	return k;
}

namespace {

/// ID3v2.2 uses three-character identifiers. Comparisons accept the v2.3/v2.4
/// spelling so policies do not have to enumerate both.
bool frameIdMatches(const std::string& actual, std::string_view wanted) {
	if (actual == wanted) return true;
	static const struct { std::string_view three; std::string_view four; } kAliases[] = {
		{"TT2", "TIT2"}, {"TP1", "TPE1"}, {"TP2", "TPE2"}, {"TAL", "TALB"},
		{"TRK", "TRCK"}, {"TPA", "TPOS"}, {"TYE", "TYER"}, {"TCO", "TCON"},
		{"TBP", "TBPM"}, {"COM", "COMM"}, {"PIC", "APIC"}, {"ULT", "USLT"},
		{"TXX", "TXXX"}, {"TCM", "TCOM"}, {"TCP", "TCMP"}, {"TDA", "TDAT"},
	};
	for (const auto& a : kAliases) {
		if (actual == a.three && wanted == a.four) return true;
		if (actual == a.four && wanted == a.three) return true;
	}
	return false;
}

/// APEv2 and ID3v1 store the same semantics under different keys.
bool alternateContainerMatches(const TagFrame& f, std::string_view frameId) {
	if (f.container != TagContainer::Apev2) return false;
	struct Map { std::string_view ape; std::string_view id3; };
	static const Map kMap[] = {
		{"Title", "TIT2"}, {"Artist", "TPE1"}, {"Album", "TALB"},
		{"Album Artist", "TPE2"}, {"AlbumArtist", "TPE2"}, {"Genre", "TCON"},
		{"Year", "TYER"}, {"Track", "TRCK"}, {"Disc", "TPOS"}, {"BPM", "TBPM"},
		{"Comment", "COMM"}, {"Lyrics", "USLT"}, {"Composer", "TCOM"},
	};
	for (const auto& m : kMap) {
		if (f.id == m.ape && m.id3 == frameId) return true;
	}
	return false;
}

std::string trimmed(std::string_view s) {
	std::size_t b = 0;
	std::size_t e = s.size();
	while (b < e && (static_cast<unsigned char>(s[b]) <= 0x20)) ++b;
	while (e > b && (static_cast<unsigned char>(s[e - 1]) <= 0x20)) --e;
	return std::string(s.substr(b, e - b));
}

} // namespace

const TagFrame* TagSnapshot::find(std::string_view frameId) const {
	// Prefer the primary container so an ID3v1 remnant never shadows ID3v2.
	const TagFrame* fallback = nullptr;
	for (const auto& f : frames) {
		if (!frameIdMatches(f.id, frameId) && !alternateContainerMatches(f, frameId)) continue;
		if (f.value.empty() && f.binary.empty()) continue;
		if (f.container == primaryContainer) return &f;
		if (f.container == TagContainer::Id3v1) {
			if (!fallback) fallback = &f;
			continue;
		}
		if (!fallback || fallback->container == TagContainer::Id3v1) fallback = &f;
	}
	return fallback;
}

std::vector<const TagFrame*> TagSnapshot::findAll(std::string_view frameId) const {
	std::vector<const TagFrame*> out;
	for (const auto& f : frames) {
		if (frameIdMatches(f.id, frameId) || alternateContainerMatches(f, frameId)) out.push_back(&f);
	}
	return out;
}

const TagFrame* TagSnapshot::findUserText(std::string_view description) const {
	for (const auto& f : frames) {
		if (!frameIdMatches(f.id, "TXXX")) continue;
		// TXXX descriptions are compared case-insensitively: ReplayGain writers
		// disagree about capitalisation.
		if (f.description.size() != description.size()) continue;
		bool equal = true;
		for (std::size_t i = 0; i < description.size(); ++i) {
			if (std::tolower(static_cast<unsigned char>(f.description[i]))
				!= std::tolower(static_cast<unsigned char>(description[i]))) {
				equal = false;
				break;
			}
		}
		if (equal) return &f;
	}
	return nullptr;
}

namespace {
std::string frameValue(const TagSnapshot& s, std::string_view id) {
	const TagFrame* f = s.find(id);
	return f ? trimmed(f->value) : std::string();
}
} // namespace

std::string TagSnapshot::title() const { return frameValue(*this, "TIT2"); }
std::string TagSnapshot::artist() const { return frameValue(*this, "TPE1"); }
std::string TagSnapshot::albumArtist() const { return frameValue(*this, "TPE2"); }
std::string TagSnapshot::album() const { return frameValue(*this, "TALB"); }
std::string TagSnapshot::genre() const { return frameValue(*this, "TCON"); }

std::string TagSnapshot::date() const {
	// ID3v2.4 uses TDRC; v2.3 splits TYER and TDAT. Report what exists without
	// synthesising a date that was never written.
	std::string v = frameValue(*this, "TDRC");
	if (!v.empty()) return v;
	v = frameValue(*this, "TYER");
	if (!v.empty()) return v;
	return frameValue(*this, "TDOR");
}

std::string TagSnapshot::comment() const { return frameValue(*this, "COMM"); }
std::string TagSnapshot::lyrics() const { return frameValue(*this, "USLT"); }

std::optional<int> TagSnapshot::trackNumber() const { return parsePositionField(frameValue(*this, "TRCK")); }
std::optional<int> TagSnapshot::trackTotal() const { return parsePositionField(frameValue(*this, "TRCK"), true); }
std::optional<int> TagSnapshot::discNumber() const { return parsePositionField(frameValue(*this, "TPOS")); }
std::optional<int> TagSnapshot::discTotal() const { return parsePositionField(frameValue(*this, "TPOS"), true); }

std::optional<double> TagSnapshot::bpm() const {
	const std::string v = frameValue(*this, "TBPM");
	if (v.empty()) return std::nullopt;
	try {
		std::size_t consumed = 0;
		const double d = std::stod(v, &consumed);
		if (consumed == 0 || d <= 0.0) return std::nullopt;
		return d;
	} catch (...) {
		return std::nullopt;
	}
}

bool TagSnapshot::isCompilation() const {
	const std::string v = frameValue(*this, "TCMP");
	if (!v.empty() && v != "0") return true;
	if (const TagFrame* f = findUserText("COMPILATION")) {
		return !f->value.empty() && f->value != "0";
	}
	return false;
}

const EmbeddedPicture* TagSnapshot::frontCover() const {
	const EmbeddedPicture* other = nullptr;
	for (const auto& p : pictures) {
		if (p.type == PictureType::FrontCover) return &p;
		if (!other) other = &p;
	}
	// A file with exactly one picture of type "Other" is treated as its cover for
	// display, but the writer still records that the role was not declared.
	return (pictures.size() == 1) ? other : nullptr;
}

std::optional<int> parsePositionField(std::string_view text, bool wantTotal) {
	const std::size_t slash = text.find('/');
	std::string_view part;
	if (wantTotal) {
		if (slash == std::string_view::npos) return std::nullopt;
		part = text.substr(slash + 1);
	} else {
		part = (slash == std::string_view::npos) ? text : text.substr(0, slash);
	}

	std::size_t b = 0;
	while (b < part.size() && std::isspace(static_cast<unsigned char>(part[b]))) ++b;
	std::size_t e = b;
	while (e < part.size() && std::isdigit(static_cast<unsigned char>(part[e]))) ++e;
	if (e == b) return std::nullopt;

	int value = 0;
	const auto res = std::from_chars(part.data() + b, part.data() + e, value);
	if (res.ec != std::errc()) return std::nullopt;
	return value;
}

} // namespace ml
