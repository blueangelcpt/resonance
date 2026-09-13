// SPDX-License-Identifier: GPL-3.0-or-later
// PRIV-001 and GAIN-001: selective, frame-aware metadata policy.
//
// Every decision is produced as a *preview* before anything is written. The
// policy never mutates a snapshot; it returns the list of actions a writer would
// take, each with the reason it was taken.
#pragma once

#include "mlcore/TagModel.hpp"
#include "mlcore/Types.hpp"

#include <string>
#include <vector>

namespace ml {

/// What a policy proposes to do with one frame.
enum class FrameAction {
	Keep = 0,
	Remove,
	Modify,
	Add,
	Review,     ///< Cannot be decided automatically; a human must choose.
};

std::string_view toString(FrameAction a);

/// One proposed frame-level change, with the rule that produced it.
struct FrameDecision {
	FrameAction action = FrameAction::Keep;
	std::string frameKey;          ///< TagFrame::key() of the affected frame.
	std::string frameId;
	TagContainer container = TagContainer::Unknown;
	std::string description;       ///< TXXX/COMM description or PRIV owner.
	std::string beforeValue;       ///< Redacted for display when sensitive.
	std::string afterValue;
	std::size_t beforeBytes = 0;
	std::size_t afterBytes = 0;
	std::string ruleId;            ///< Stable rule identifier, e.g. "priv.purchase.itunes".
	std::string reason;            ///< Human-readable justification.
	bool sensitive = false;        ///< Value must be redacted from routine logs.

	std::int64_t byteDelta() const {
		return static_cast<std::int64_t>(afterBytes) - static_cast<std::int64_t>(beforeBytes);
	}
};

// ---------------------------------------------------------------------------
// Privacy policy (PRIV-001, FN-TAG-03)
// ---------------------------------------------------------------------------

enum class PrivacyProfile {
	/// Removes recognised purchase and account information; preserves known
	/// public recording identifiers; routes unknown private frames to review.
	Selective = 0,
	/// Separately labelled profile matching iDesiccate's broad removal of every
	/// PRIV, COMM and UFID frame. Always previewed before use.
	StrictIDesiccate,
	/// Analysis only: decide nothing, report what a run would find.
	ReportOnly,
};

std::string_view toString(PrivacyProfile p);
std::optional<PrivacyProfile> privacyProfileFromString(std::string_view s);

struct PrivacyOptions {
	PrivacyProfile profile = PrivacyProfile::Selective;
	/// Remove the ID3v1 comment field when a trailing ID3v1 tag exists.
	bool clearId3v1Comment = false;
	/// Also scan free-text fields (COMM, TXXX) for e-mail addresses and account
	/// identifiers. Findings are routed to review, never removed silently.
	bool scanFreeTextForPersonalData = true;
	/// Owners that must always be preserved even under the strict profile.
	std::vector<std::string> alwaysPreserveOwners;
};

/// Result of a privacy evaluation over one snapshot.
struct PrivacyPreview {
	std::vector<FrameDecision> decisions;
	std::size_t removeCount = 0;
	std::size_t reviewCount = 0;
	std::int64_t byteDelta = 0;

	bool hasReviewItems() const { return reviewCount > 0; }
};

class PrivacyPolicy {
public:
	explicit PrivacyPolicy(PrivacyOptions options = {});

	PrivacyPreview evaluate(const TagSnapshot& snapshot) const;

	/// True when this owner string is a documented public identifier that must
	/// survive even a broad removal (for example MusicBrainz recording IDs).
	static bool isKnownPublicIdentifier(std::string_view owner);

	/// True when this PRIV owner is a recognised purchase or account record.
	static bool isKnownPurchaseIdentifier(std::string_view owner);

	/// Heuristic detection of personal data in free text. Returns a description
	/// of what was matched, or an empty string. Never used to remove silently.
	static std::string detectPersonalData(std::string_view text);

	const PrivacyOptions& options() const { return m_options; }

private:
	PrivacyOptions m_options;
};

// ---------------------------------------------------------------------------
// Gain policy (GAIN-001)
// ---------------------------------------------------------------------------

/// Why a file cannot have its gain metadata cleared without losing recovery
/// information. FRD section 8: MP3Gain undo data is not ordinary clutter.
enum class GainException {
	None = 0,
	Mp3GainUndoPresent,        ///< APEv2 MP3GAIN_UNDO / MP3GAIN_MINMAX present.
	AppliedGainEvidence,       ///< Evidence the audio itself was gain-adjusted.
	UnknownRelativeVolume,     ///< RVA2/RVAD present but not interpretable.
};

std::string_view toString(GainException e);

struct GainOptions {
	/// Remove ReplayGain track/album gain, peak and reference fields.
	bool removeReplayGain = true;
	/// Remove Apple Sound Check (`iTunNORM`).
	bool removeSoundCheck = true;
	/// Remove ID3v2 relative-volume frames (`RVA2`, legacy `RVAD`).
	bool removeRelativeVolume = true;
	/// Never touch gapless information (`iTunSMPB`, LAME header delay/padding).
	/// Present as a constant so the intent is explicit and testable.
	static constexpr bool preserveGapless = true;
};

struct GainPreview {
	std::vector<FrameDecision> decisions;
	std::vector<GainException> exceptions;
	std::vector<std::string> preservedGaplessFields;
	std::size_t removeCount = 0;
	std::int64_t byteDelta = 0;

	bool blocked() const { return !exceptions.empty(); }
};

class GainPolicy {
public:
	explicit GainPolicy(GainOptions options = {});

	GainPreview evaluate(const TagSnapshot& snapshot) const;

	/// True when this TXXX description or APEv2 key is a ReplayGain field.
	static bool isReplayGainField(std::string_view key);
	/// True when this key carries gapless information that must be preserved.
	static bool isGaplessField(std::string_view key);
	/// True when this key is MP3Gain recovery data.
	static bool isMp3GainUndoField(std::string_view key);

	const GainOptions& options() const { return m_options; }

private:
	GainOptions m_options;
};

// ---------------------------------------------------------------------------
// Padding and size policy (OPT-001, FN-OPT-01)
// ---------------------------------------------------------------------------

struct PaddingOptions {
	/// Padding budget retained so a future small edit does not force a rewrite.
	std::size_t targetPaddingBytes = 2048;
	/// Only rewrite for padding when the existing padding exceeds this.
	std::size_t rewriteThresholdBytes = 16384;
};

/// Size change, decomposed by cause.
///
/// FN-OPT-02 requires enrichment growth reported separately from optimisation
/// savings. Padding is a third cause: retaining a padding budget can make a file
/// larger without anything having been added to it, and reporting that as
/// negative "savings" would be misleading.
struct SizeAccounting {
	std::int64_t optimisationSavings = 0;   ///< Bytes removed by policy and compaction.
	std::int64_t enrichmentGrowth = 0;      ///< Bytes added by artwork and lyrics.
	std::int64_t paddingDelta = 0;          ///< Change in reserved padding.
	std::int64_t netDelta = 0;              ///< Actual file size change.
};

} // namespace ml
