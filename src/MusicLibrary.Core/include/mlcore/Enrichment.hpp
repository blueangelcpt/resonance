// SPDX-License-Identifier: GPL-3.0-or-later
// BPM-001 / LYR-001: tempo and lyrics result models and their decision rules.
//
// Both share one principle from the FRD: an unknown result stays explicitly
// unknown. Filling a field with an uncertain guess makes the collection worse.
#pragma once

#include "mlcore/Types.hpp"

#include <string>
#include <vector>

namespace ml {

// ---------------------------------------------------------------------------
// Tempo (BPM-001, FN-BPM-01, FN-BPM-02)
// ---------------------------------------------------------------------------

/// Classification of the track's tempo behaviour, decided before a single BPM
/// value is proposed. FRD section 7 requires these handled explicitly.
enum class TempoCharacter {
	Unknown = 0,
	SteadyBeat,       ///< A stable tempo across the analysed sections.
	VariableTempo,    ///< Sections disagree beyond the stability tolerance.
	Beatless,         ///< No usable periodic onset structure.
	TooShort,         ///< Below the minimum analysable length.
	LongForm,         ///< Long mix or set; one BPM may not describe it.
};

std::string_view toString(TempoCharacter c);

enum class TempoDecisionKind {
	NoChange = 0,       ///< Existing trusted value preserved.
	Propose,            ///< A new value is proposed.
	NeedsReview,        ///< Analysis and existing value disagree, or confidence is low.
	NotApplicable,      ///< Beatless or too short; no BPM should be written.
	Failed,             ///< Analysis could not run.
};

std::string_view toString(TempoDecisionKind k);

/// Raw output of the tempo analyser, before any policy is applied.
struct TempoAnalysis {
	double bpm = 0.0;                  ///< Best estimate, fractional.
	double halfTempo = 0.0;            ///< bpm / 2, when musically plausible.
	double doubleTempo = 0.0;          ///< bpm * 2, when musically plausible.
	/// Agreement across analysed sections in [0,1]. An ordering aid, explicitly
	/// not a calibrated probability (FRD section 7).
	double stability = 0.0;
	int sectionsAnalysed = 0;
	std::vector<double> sectionBpms;
	TempoCharacter character = TempoCharacter::Unknown;
	std::string engine;                ///< Engine identifier and version.
	std::string settings;              ///< Hash or description of analysis settings.
	std::vector<Evidence> evidence;
	bool valid = false;
};

struct TempoOptions {
	/// Preserve an existing BPM tag by default; only replace on explicit request.
	bool preserveExistingTrusted = true;
	/// Relative difference below which an analysis agrees with an existing value.
	double agreementTolerance = 0.02;
	/// Stability below which a result always needs review rather than acceptance.
	double reviewStabilityThreshold = 0.70;
	/// Minimum track length worth analysing.
	std::int64_t minimumDurationMs = 20000;
	/// Above this, the track is treated as long-form and reported as such.
	std::int64_t longFormDurationMs = 15 * 60 * 1000;
	/// Plausible BPM range for the detector to report within.
	double minBpm = 50.0;
	double maxBpm = 220.0;
};

struct TempoDecision {
	TempoDecisionKind kind = TempoDecisionKind::NeedsReview;
	/// Precise value retained in the catalogue.
	double preciseBpm = 0.0;
	/// Integer written to the standards-compatible `TBPM` frame.
	int taggedBpm = 0;
	std::optional<double> existingBpm;
	std::string reason;
	std::vector<double> alternatives;   ///< Half/double tempo candidates.
	Confidence confidence = Confidence::Unknown;

	bool writesTag() const { return kind == TempoDecisionKind::Propose && taggedBpm > 0; }
};

class TempoPolicy {
public:
	explicit TempoPolicy(TempoOptions options = {});

	/// Decides what to do given an analysis and whatever BPM the file already had.
	TempoDecision decide(const TempoAnalysis& analysis, std::optional<double> existingBpm,
		std::int64_t durationMs) const;

	/// Rounds a fractional BPM for the integer `TBPM` frame.
	static int roundForTag(double bpm);

	const TempoOptions& options() const { return m_options; }

private:
	TempoOptions m_options;
};

// ---------------------------------------------------------------------------
// Lyrics (LYR-001, FN-LYR-01..03)
// ---------------------------------------------------------------------------

/// The six states FRD section 7 requires to stay distinct. Missing lyrics are
/// not proof of an instrumental, and a failed lookup is not a "not found".
enum class LyricsState {
	NotAttempted = 0,
	Found,
	Instrumental,
	NotFound,
	NeedsReview,      ///< A candidate was returned but did not match confidently.
	RateLimited,
	Failed,
	ExistingPreserved,
};

std::string_view toString(LyricsState s);
std::optional<LyricsState> lyricsStateFromString(std::string_view s);

struct LyricsCandidate {
	std::string providerId;
	std::string trackName;
	std::string artistName;
	std::string albumName;
	std::int64_t durationMs = 0;
	std::string plainLyrics;
	std::string syncedLyrics;   ///< LRC text when the provider supplied it.
	bool instrumental = false;
	std::string sourceUrl;
	std::string retrievedAtIso8601;
};

/// Scoring inputs for matching a candidate to the local recording. A same-title
/// result alone is insufficient (FRD section 7).
struct LyricsMatchInput {
	std::string localTitle;
	std::string localArtist;
	std::string localAlbum;
	std::int64_t localDurationMs = 0;
};

struct LyricsOptions {
	/// Preserve existing lyrics unless replacement is explicitly selected.
	bool preserveExisting = true;
	/// Duration difference beyond which a candidate cannot be a confident match.
	std::int64_t durationToleranceMs = 3000;
	/// Minimum title similarity for any match at all.
	double minimumTitleSimilarity = 0.85;
	/// Minimum artist similarity for a confident match.
	double minimumArtistSimilarity = 0.80;
	/// ISO-639-2 language code written into `USLT`.
	std::string defaultLanguage = "eng";
};

struct LyricsDecision {
	LyricsState state = LyricsState::NotAttempted;
	std::string text;               ///< Plain lyrics to embed, when state == Found.
	std::string language;
	std::string providerId;
	std::string sourceUrl;
	std::string reason;
	Confidence confidence = Confidence::Unknown;
	std::vector<Evidence> evidence;
	int selectedCandidate = -1;

	bool writesTag() const { return state == LyricsState::Found && !text.empty(); }
};

class LyricsPolicy {
public:
	explicit LyricsPolicy(LyricsOptions options = {});

	/// Chooses among provider candidates. Never generates or translates text.
	LyricsDecision decide(const LyricsMatchInput& input, const std::vector<LyricsCandidate>& candidates,
		bool fileHasExistingLyrics) const;

	/// Match quality for one candidate, with its evidence.
	struct MatchScore {
		double titleSimilarity = 0.0;
		double artistSimilarity = 0.0;
		double albumSimilarity = 0.0;
		std::int64_t durationDeltaMs = 0;
		bool durationWithinTolerance = false;
		bool versionQualifiersAgree = true;
		Confidence confidence = Confidence::Unknown;
		std::vector<Evidence> evidence;
	};
	MatchScore score(const LyricsMatchInput& input, const LyricsCandidate& candidate) const;

	/// Validates an ISO-639-2 language code shape before it reaches `USLT`.
	static bool isValidLanguageCode(std::string_view code);

	const LyricsOptions& options() const { return m_options; }

private:
	LyricsOptions m_options;
};

} // namespace ml
