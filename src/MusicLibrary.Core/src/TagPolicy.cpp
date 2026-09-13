// SPDX-License-Identifier: GPL-3.0-or-later
#include "mlcore/TagPolicy.hpp"
#include "mlcore/Text.hpp"

#include <algorithm>
#include <array>
#include <cctype>

namespace ml {

std::string_view toString(FrameAction a) {
	switch (a) {
		case FrameAction::Keep: return "keep";
		case FrameAction::Remove: return "remove";
		case FrameAction::Modify: return "modify";
		case FrameAction::Add: return "add";
		case FrameAction::Review: return "review";
	}
	return "keep";
}

std::string_view toString(PrivacyProfile p) {
	switch (p) {
		case PrivacyProfile::Selective: return "selective";
		case PrivacyProfile::StrictIDesiccate: return "strict_idesiccate";
		case PrivacyProfile::ReportOnly: return "report_only";
	}
	return "selective";
}

std::optional<PrivacyProfile> privacyProfileFromString(std::string_view s) {
	if (s == "selective") return PrivacyProfile::Selective;
	if (s == "strict_idesiccate" || s == "strict") return PrivacyProfile::StrictIDesiccate;
	if (s == "report_only" || s == "report") return PrivacyProfile::ReportOnly;
	return std::nullopt;
}

std::string_view toString(GainException e) {
	switch (e) {
		case GainException::None: return "none";
		case GainException::Mp3GainUndoPresent: return "mp3gain_undo_present";
		case GainException::AppliedGainEvidence: return "applied_gain_evidence";
		case GainException::UnknownRelativeVolume: return "unknown_relative_volume";
	}
	return "none";
}

// ---------------------------------------------------------------------------
// Privacy policy
// ---------------------------------------------------------------------------

namespace {

/// PRIV owners that carry purchase or account information. These come from
/// observed real-world tags; the list is additive and anything unmatched goes to
/// review rather than being removed on a guess.
constexpr std::array<std::string_view, 12> kPurchaseOwners = {
	"WM/MediaClassPrimaryID",       // Windows Media, but paired below with account data
	"WM/MediaClassSecondaryID",
	"WM/Provider",
	"WM/UniqueFileIdentifier",
	"PeakValue",
	"AverageLevel",
	"CDDB",
	"com.apple.iTunes:Account",
	"iTunes:Account",
	"Google/StoreId",
	"Google/AccountId",
	"AMGRating",
};

/// Owners documented as public identifiers. Mutagen documents MusicBrainz
/// recording identifiers in UFID; removing every UFID is too broad (FRD §2).
constexpr std::array<std::string_view, 8> kPublicIdentifierOwners = {
	"http://musicbrainz.org",
	"musicbrainz.org",
	"http://www.cdbaby.com/cdbaby.pid",
	"https://musicbrainz.org",
	"http://acoustid.org",
	"acoustid.org",
	"http://www.id3.org/dummy/ufid.html",
	"Discogs",
};

bool looksLikeEmail(std::string_view s) {
	const std::size_t at = s.find('@');
	if (at == std::string_view::npos || at == 0 || at + 2 >= s.size()) return false;
	const std::size_t dot = s.find('.', at + 1);
	if (dot == std::string_view::npos || dot + 1 >= s.size()) return false;
	// Reject an '@' that is part of ordinary prose, for example "Live @ Wembley".
	const char before = s[at - 1];
	const char after = s[at + 1];
	if (std::isspace(static_cast<unsigned char>(before)) || std::isspace(static_cast<unsigned char>(after))) {
		return false;
	}
	return true;
}

/// Apple purchase tags embed an account identifier in a decimal string. This
/// recognises the shape without asserting whose account it is.
bool looksLikeAccountId(std::string_view s) {
	if (s.size() < 6 || s.size() > 24) return false;
	return std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isdigit(c) != 0; });
}

/// Custom text fields with a documented public meaning.
///
/// Without this list the numeric-identifier heuristic flags every `CATALOG`,
/// `DISCID` and `BARCODE` value as account-shaped, which would bury the real
/// findings under hundreds of false positives. These are release identifiers
/// published on the sleeve, not personal data.
bool isKnownPublicTextField(std::string_view description) {
	static constexpr std::array<std::string_view, 30> kPublicFields = {
		"CATALOG", "CATALOGNUMBER", "CATALOGUENUMBER", "DISCID", "BARCODE", "ISRC",
		"ASIN", "LABEL", "MEDIA", "SCRIPT", "RELEASECOUNTRY", "RELEASETYPE",
		"RELEASESTATUS", "ORIGINALYEAR", "ORIGINALDATE", "ARTISTSORT", "ALBUMARTISTSORT",
		"CRC-32", "CRC32", "ACCURATERIPRESULT", "ACCURATERIPDISCID", "SOURCE",
		"ENCODER", "ENCODEDBY", "COMPILATION", "TOTALTRACKS", "TOTALDISCS",
		"MUSICBRAINZ ALBUM ID", "MUSICBRAINZ ARTIST ID", "MUSICBRAINZ TRACK ID",
	};
	for (auto field : kPublicFields) {
		if (text::equalsNoCase(description, field)) return true;
	}
	// Every MusicBrainz and AcoustID field is a public identifier.
	if (text::startsWithNoCase(description, "MUSICBRAINZ")) return true;
	if (text::startsWithNoCase(description, "ACOUSTID")) return true;
	if (text::startsWithNoCase(description, "REPLAYGAIN")) return true;
	return false;
}

std::string redact(std::string_view value, bool sensitive) {
	if (!sensitive) return std::string(value);
	if (value.size() <= 4) return "[redacted]";
	std::string out = "[redacted ";
	out += std::to_string(value.size());
	out += " bytes]";
	return out;
}

void push(PrivacyPreview& preview, FrameDecision d) {
	if (d.action == FrameAction::Remove) ++preview.removeCount;
	if (d.action == FrameAction::Review) ++preview.reviewCount;
	preview.byteDelta += d.byteDelta();
	preview.decisions.push_back(std::move(d));
}

FrameDecision makeDecision(const TagFrame& f, FrameAction action, std::string ruleId, std::string reason,
	bool sensitive) {
	FrameDecision d;
	d.action = action;
	d.frameKey = f.key();
	d.frameId = f.id;
	d.container = f.container;
	d.description = f.description.empty() ? f.owner : f.description;
	d.beforeBytes = f.rawSize;
	d.afterBytes = (action == FrameAction::Remove) ? 0 : f.rawSize;
	d.beforeValue = redact(f.value.empty() ? std::string("<binary>") : f.value, sensitive);
	d.ruleId = std::move(ruleId);
	d.reason = std::move(reason);
	d.sensitive = sensitive;
	return d;
}

} // namespace

PrivacyPolicy::PrivacyPolicy(PrivacyOptions options) : m_options(std::move(options)) {}

bool PrivacyPolicy::isKnownPublicIdentifier(std::string_view owner) {
	for (auto o : kPublicIdentifierOwners) {
		if (text::containsNoCase(owner, o)) return true;
	}
	return false;
}

bool PrivacyPolicy::isKnownPurchaseIdentifier(std::string_view owner) {
	for (auto o : kPurchaseOwners) {
		if (text::equalsNoCase(owner, o)) return true;
	}
	// Apple stores purchase data under owners beginning with these prefixes.
	if (text::startsWithNoCase(owner, "com.apple.iTunes")) return true;
	if (text::containsNoCase(owner, "purchase")) return true;
	if (text::containsNoCase(owner, "account")) return true;
	return false;
}

std::string PrivacyPolicy::detectPersonalData(std::string_view value) {
	if (looksLikeEmail(value)) return "contains an e-mail address";
	if (looksLikeAccountId(value)) return "contains a numeric identifier of account-id shape";
	if (text::containsNoCase(value, "ripped by")) return "contains a ripper attribution";
	if (text::containsNoCase(value, "encoded by ") && looksLikeEmail(value)) return "contains an encoder e-mail";
	return {};
}

PrivacyPreview PrivacyPolicy::evaluate(const TagSnapshot& snapshot) const {
	PrivacyPreview preview;
	const bool strict = (m_options.profile == PrivacyProfile::StrictIDesiccate);
	const bool reportOnly = (m_options.profile == PrivacyProfile::ReportOnly);

	const auto alwaysPreserve = [&](std::string_view owner) {
		for (const auto& o : m_options.alwaysPreserveOwners) {
			if (text::equalsNoCase(owner, o)) return true;
		}
		return false;
	};

	for (const auto& f : snapshot.frames) {
		// --- PRIV ----------------------------------------------------------
		if (f.id == "PRIV") {
			if (alwaysPreserve(f.owner)) {
				push(preview, makeDecision(f, FrameAction::Keep, "priv.preserve.configured",
					"owner is on the always-preserve list", true));
				continue;
			}
			if (isKnownPublicIdentifier(f.owner)) {
				push(preview, makeDecision(f, FrameAction::Keep, "priv.preserve.public_id",
					"owner \"" + f.owner + "\" is a documented public identifier", false));
				continue;
			}
			if (strict) {
				push(preview, makeDecision(f, reportOnly ? FrameAction::Review : FrameAction::Remove,
					"priv.strict.all_priv",
					"strict profile removes every PRIV frame (iDesiccate behaviour); owner was \"" + f.owner + "\"",
					true));
				continue;
			}
			if (isKnownPurchaseIdentifier(f.owner)) {
				push(preview, makeDecision(f, reportOnly ? FrameAction::Review : FrameAction::Remove,
					"priv.purchase",
					"owner \"" + f.owner + "\" is a recognised purchase or account record", true));
				continue;
			}
			// Unknown private frame: the FRD requires review, not removal.
			push(preview, makeDecision(f, FrameAction::Review, "priv.unknown_owner",
				"unrecognised PRIV owner \"" + f.owner + "\"; review before removing", true));
			continue;
		}

		// --- UFID ----------------------------------------------------------
		if (f.id == "UFID" || f.id == "UFI") {
			if (isKnownPublicIdentifier(f.owner) && !strict) {
				push(preview, makeDecision(f, FrameAction::Keep, "ufid.preserve.public_id",
					"owner \"" + f.owner + "\" is a documented public recording identifier", false));
				continue;
			}
			if (strict) {
				push(preview, makeDecision(f, reportOnly ? FrameAction::Review : FrameAction::Remove,
					"ufid.strict.all_ufid",
					"strict profile removes every UFID frame, including MusicBrainz recording identifiers", false));
				continue;
			}
			push(preview, makeDecision(f, FrameAction::Review, "ufid.unknown_owner",
				"unrecognised UFID owner \"" + f.owner + "\"; review before removing", true));
			continue;
		}

		// --- COMM ----------------------------------------------------------
		if (f.id == "COMM" || f.id == "COM") {
			if (strict) {
				push(preview, makeDecision(f, reportOnly ? FrameAction::Review : FrameAction::Remove,
					"comm.strict.all_comm",
					"strict profile removes every COMM frame; description was \"" + f.description + "\"", true));
				continue;
			}
			// Sound Check lives in a COMM frame and belongs to the gain policy,
			// not the privacy policy. Leave it to GainPolicy to decide.
			if (text::equalsNoCase(f.description, "iTunNORM") || text::equalsNoCase(f.description, "iTunSMPB")) {
				push(preview, makeDecision(f, FrameAction::Keep, "comm.defer.gain_policy",
					"\"" + f.description + "\" is handled by the gain policy, not the privacy policy", false));
				continue;
			}
			if (m_options.scanFreeTextForPersonalData) {
				const std::string finding = detectPersonalData(f.value);
				if (!finding.empty()) {
					push(preview, makeDecision(f, FrameAction::Review, "comm.personal_data",
						"comment " + finding + "; review before removing", true));
					continue;
				}
			}
			push(preview, makeDecision(f, FrameAction::Keep, "comm.preserve",
				"comment carries no recognised identifying data", false));
			continue;
		}

		// --- TXXX free text -------------------------------------------------
		if (f.id == "TXXX" || f.id == "TXX") {
			if (GainPolicy::isReplayGainField(f.description) || GainPolicy::isGaplessField(f.description)) {
				// Owned by the gain policy.
				continue;
			}
			if (isKnownPublicTextField(f.description)) {
				push(preview, makeDecision(f, FrameAction::Keep, "txxx.preserve.public_field",
					"\"" + f.description + "\" is a documented public release field", false));
				continue;
			}
			if (m_options.scanFreeTextForPersonalData) {
				const std::string finding = detectPersonalData(f.value);
				if (!finding.empty()) {
					push(preview, makeDecision(f, FrameAction::Review, "txxx.personal_data",
						"custom field \"" + f.description + "\" " + finding, true));
					continue;
				}
			}
			continue;
		}

		// --- APEv2 keys carrying purchase data --------------------------------
		if (f.container == TagContainer::Apev2) {
			if (GainPolicy::isMp3GainUndoField(f.id) || GainPolicy::isReplayGainField(f.id)) continue;
			if (isKnownPublicTextField(f.id)) continue;
			if (m_options.scanFreeTextForPersonalData) {
				const std::string finding = detectPersonalData(f.value);
				if (!finding.empty()) {
					push(preview, makeDecision(f, FrameAction::Review, "ape.personal_data",
						"APEv2 item \"" + f.id + "\" " + finding, true));
				}
			}
			continue;
		}

		// --- ID3v1 comment ---------------------------------------------------
		if (f.container == TagContainer::Id3v1 && f.id == "COMMENT" && m_options.clearId3v1Comment) {
			if (!f.value.empty()) {
				FrameDecision d = makeDecision(f, reportOnly ? FrameAction::Review : FrameAction::Modify,
					"id3v1.clear_comment", "ID3v1 comment cleared by configuration", true);
				d.afterValue.clear();
				d.afterBytes = f.rawSize;   // ID3v1 is fixed size; the field is blanked in place.
				push(preview, std::move(d));
			}
			continue;
		}
	}

	return preview;
}

// ---------------------------------------------------------------------------
// Gain policy
// ---------------------------------------------------------------------------

namespace {

constexpr std::array<std::string_view, 10> kReplayGainKeys = {
	"replaygain_track_gain",
	"replaygain_track_peak",
	"replaygain_album_gain",
	"replaygain_album_peak",
	"replaygain_reference_loudness",
	"replaygain_track_range",
	"replaygain_album_range",
	"r128_track_gain",
	"r128_album_gain",
	"mp3gain_album_minmax",
};

constexpr std::array<std::string_view, 5> kGaplessKeys = {
	"itunsmpb",
	"itunpgap",
	"gapless",
	"encoder delay",
	"lame_delay",
};

constexpr std::array<std::string_view, 4> kMp3GainUndoKeys = {
	"mp3gain_undo",
	"mp3gain_minmax",
	"mp3gain_album_undo",
	"mp3gain_album_minmax",
};

} // namespace

GainPolicy::GainPolicy(GainOptions options) : m_options(std::move(options)) {}

bool GainPolicy::isReplayGainField(std::string_view key) {
	const std::string lower = text::toLowerAscii(key);
	// mp3gain_album_minmax appears in both lists; the undo check runs first at the
	// call sites so recovery data always wins.
	if (isMp3GainUndoField(key)) return false;
	for (auto k : kReplayGainKeys) {
		if (lower == k) return true;
	}
	return false;
}

bool GainPolicy::isGaplessField(std::string_view key) {
	const std::string lower = text::toLowerAscii(key);
	for (auto k : kGaplessKeys) {
		if (lower == k) return true;
	}
	return false;
}

bool GainPolicy::isMp3GainUndoField(std::string_view key) {
	const std::string lower = text::toLowerAscii(key);
	for (auto k : kMp3GainUndoKeys) {
		if (lower == k) return true;
	}
	return false;
}

GainPreview GainPolicy::evaluate(const TagSnapshot& snapshot) const {
	GainPreview preview;

	const auto record = [&](const TagFrame& f, FrameAction action, std::string ruleId, std::string reason) {
		FrameDecision d;
		d.action = action;
		d.frameKey = f.key();
		d.frameId = f.id;
		d.container = f.container;
		d.description = f.description.empty() ? f.id : f.description;
		d.beforeValue = f.value;
		d.beforeBytes = f.rawSize;
		d.afterBytes = (action == FrameAction::Remove) ? 0 : f.rawSize;
		d.ruleId = std::move(ruleId);
		d.reason = std::move(reason);
		if (action == FrameAction::Remove) ++preview.removeCount;
		preview.byteDelta += d.byteDelta();
		preview.decisions.push_back(std::move(d));
	};

	// Pass 1: find recovery data and evidence of applied gain. These block the
	// removal pass entirely for this file (FRD section 8).
	bool undoPresent = false;
	for (const auto& f : snapshot.frames) {
		const std::string key = f.description.empty() ? f.id : f.description;
		if (isMp3GainUndoField(key)) {
			undoPresent = true;
			preview.preservedGaplessFields.push_back(key);
			record(f, FrameAction::Keep, "gain.preserve.mp3gain_undo",
				"MP3Gain recovery data; removing it would make a previous gain change irreversible");
		}
	}
	if (undoPresent) {
		preview.exceptions.push_back(GainException::Mp3GainUndoPresent);
	}

	// Pass 2: gapless information is preserved unconditionally.
	for (const auto& f : snapshot.frames) {
		const std::string key = f.description.empty() ? f.id : f.description;
		if (isGaplessField(key)) {
			preview.preservedGaplessFields.push_back(key);
			record(f, FrameAction::Keep, "gain.preserve.gapless",
				"\"" + key + "\" carries gapless information and is preserved by policy");
		}
	}

	// Pass 3: the removals themselves.
	for (const auto& f : snapshot.frames) {
		const std::string key = f.description.empty() ? f.id : f.description;

		if (m_options.removeReplayGain && isReplayGainField(key)) {
			if (undoPresent) {
				record(f, FrameAction::Review, "gain.blocked.undo_present",
					"ReplayGain field retained: this file carries MP3Gain recovery data and needs an explicit decision");
			} else {
				record(f, FrameAction::Remove, "gain.remove.replaygain",
					"\"" + key + "\" is a playback-gain coefficient, not audio content");
			}
			continue;
		}

		if (m_options.removeSoundCheck && text::equalsNoCase(key, "iTunNORM")) {
			if (undoPresent) {
				record(f, FrameAction::Review, "gain.blocked.undo_present",
					"Sound Check retained: this file carries MP3Gain recovery data");
			} else {
				record(f, FrameAction::Remove, "gain.remove.soundcheck",
					"iTunNORM is Apple Sound Check playback normalisation");
			}
			continue;
		}

		if (m_options.removeRelativeVolume && (f.id == "RVA2" || f.id == "RVAD" || f.id == "RVA")) {
			if (!f.interpreted) {
				// An RVA2 frame we could not parse might carry something other than
				// a simple playback adjustment. Do not guess.
				preview.exceptions.push_back(GainException::UnknownRelativeVolume);
				record(f, FrameAction::Review, "gain.review.unparsed_rva",
					"relative-volume frame could not be interpreted; review before removing");
			} else if (undoPresent) {
				record(f, FrameAction::Review, "gain.blocked.undo_present",
					"relative-volume frame retained: this file carries MP3Gain recovery data");
			} else {
				record(f, FrameAction::Remove, "gain.remove.relative_volume",
					f.id + " is a relative-volume playback adjustment");
			}
			continue;
		}
	}

	// Pass 4: evidence that the audio itself was gain-adjusted without undo data.
	// Detecting this is the LAME header's business; the flag is set by the reader.
	for (const auto& w : snapshot.readWarnings) {
		if (text::containsNoCase(w, "applied gain")) {
			preview.exceptions.push_back(GainException::AppliedGainEvidence);
			break;
		}
	}

	return preview;
}

} // namespace ml
