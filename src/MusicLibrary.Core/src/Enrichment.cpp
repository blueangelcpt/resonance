// SPDX-License-Identifier: GPL-3.0-or-later
#include "mlcore/Enrichment.hpp"
#include "mlcore/Text.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>

namespace ml {

std::string_view toString(TempoCharacter c) {
	switch (c) {
		case TempoCharacter::Unknown: return "unknown";
		case TempoCharacter::SteadyBeat: return "steady_beat";
		case TempoCharacter::VariableTempo: return "variable_tempo";
		case TempoCharacter::Beatless: return "beatless";
		case TempoCharacter::TooShort: return "too_short";
		case TempoCharacter::LongForm: return "long_form";
	}
	return "unknown";
}

std::string_view toString(TempoDecisionKind k) {
	switch (k) {
		case TempoDecisionKind::NoChange: return "no_change";
		case TempoDecisionKind::Propose: return "propose";
		case TempoDecisionKind::NeedsReview: return "needs_review";
		case TempoDecisionKind::NotApplicable: return "not_applicable";
		case TempoDecisionKind::Failed: return "failed";
	}
	return "needs_review";
}

std::string_view toString(LyricsState s) {
	switch (s) {
		case LyricsState::NotAttempted: return "not_attempted";
		case LyricsState::Found: return "found";
		case LyricsState::Instrumental: return "instrumental";
		case LyricsState::NotFound: return "not_found";
		case LyricsState::NeedsReview: return "needs_review";
		case LyricsState::RateLimited: return "rate_limited";
		case LyricsState::Failed: return "failed";
		case LyricsState::ExistingPreserved: return "existing_preserved";
	}
	return "not_attempted";
}

std::optional<LyricsState> lyricsStateFromString(std::string_view s) {
	if (s == "not_attempted") return LyricsState::NotAttempted;
	if (s == "found") return LyricsState::Found;
	if (s == "instrumental") return LyricsState::Instrumental;
	if (s == "not_found") return LyricsState::NotFound;
	if (s == "needs_review") return LyricsState::NeedsReview;
	if (s == "rate_limited") return LyricsState::RateLimited;
	if (s == "failed") return LyricsState::Failed;
	if (s == "existing_preserved") return LyricsState::ExistingPreserved;
	return std::nullopt;
}

// ---------------------------------------------------------------------------
// Tempo
// ---------------------------------------------------------------------------

TempoPolicy::TempoPolicy(TempoOptions options) : m_options(std::move(options)) {}

int TempoPolicy::roundForTag(double bpm) {
	if (bpm <= 0.0) return 0;
	return static_cast<int>(std::llround(bpm));
}

TempoDecision TempoPolicy::decide(const TempoAnalysis& analysis, std::optional<double> existingBpm,
	std::int64_t durationMs) const {
	TempoDecision d;
	d.existingBpm = existingBpm;

	// --- Cases where no BPM should be written at all ------------------------
	if (durationMs > 0 && durationMs < m_options.minimumDurationMs) {
		d.kind = TempoDecisionKind::NotApplicable;
		d.reason = "track is " + text::formatDuration(durationMs) + ", below the "
			+ text::formatDuration(m_options.minimumDurationMs) + " minimum for tempo analysis";
		return d;
	}

	if (!analysis.valid) {
		d.kind = TempoDecisionKind::Failed;
		d.reason = "tempo analysis did not produce a result";
		return d;
	}

	if (analysis.character == TempoCharacter::Beatless) {
		d.kind = TempoDecisionKind::NotApplicable;
		d.reason = "no usable periodic onset structure was found; a BPM would be invented, not measured";
		return d;
	}
	if (analysis.character == TempoCharacter::TooShort) {
		d.kind = TempoDecisionKind::NotApplicable;
		d.reason = "analysable audio is too short for a reliable tempo estimate";
		return d;
	}

	// --- Alternatives: half and double tempo --------------------------------
	// Reported whenever they fall in the plausible range, because half/double
	// confusion is the dominant failure mode of every beat tracker.
	if (analysis.halfTempo >= m_options.minBpm) d.alternatives.push_back(analysis.halfTempo);
	if (analysis.doubleTempo <= m_options.maxBpm) d.alternatives.push_back(analysis.doubleTempo);

	d.preciseBpm = analysis.bpm;
	d.taggedBpm = roundForTag(analysis.bpm);

	// --- Existing trusted value ---------------------------------------------
	if (existingBpm && *existingBpm > 0.0 && m_options.preserveExistingTrusted) {
		const double relative = std::abs(analysis.bpm - *existingBpm) / *existingBpm;
		if (relative <= m_options.agreementTolerance) {
			d.kind = TempoDecisionKind::NoChange;
			d.confidence = Confidence::Strong;
			d.reason = "existing BPM " + std::to_string(static_cast<int>(*existingBpm))
				+ " agrees with the local analysis; preserved unchanged";
			return d;
		}

		// Disagreement that is exactly a half/double relationship is the classic
		// ambiguity and is always surfaced rather than silently "corrected".
		const double halfRatio = std::abs(analysis.bpm - *existingBpm * 2.0) / (*existingBpm * 2.0);
		const double doubleRatio = std::abs(analysis.bpm - *existingBpm / 2.0) / (*existingBpm / 2.0);
		if (halfRatio <= m_options.agreementTolerance || doubleRatio <= m_options.agreementTolerance) {
			d.kind = TempoDecisionKind::NeedsReview;
			d.confidence = Confidence::Weak;
			d.reason = "local analysis gives " + std::to_string(analysis.bpm) + " against an existing "
				+ std::to_string(*existingBpm) + "; these differ by a factor of two and the correct value "
				"is a musical judgement";
			return d;
		}

		d.kind = TempoDecisionKind::NeedsReview;
		d.confidence = Confidence::Weak;
		d.reason = "local analysis disagrees with the existing BPM; existing value preserved pending review";
		return d;
	}

	// --- No existing value ---------------------------------------------------
	if (analysis.character == TempoCharacter::VariableTempo) {
		d.kind = TempoDecisionKind::NeedsReview;
		d.confidence = Confidence::Weak;
		d.reason = "analysed sections disagree; a single BPM does not describe this track";
		return d;
	}
	if (analysis.character == TempoCharacter::LongForm || durationMs >= m_options.longFormDurationMs) {
		d.kind = TempoDecisionKind::NeedsReview;
		d.confidence = Confidence::Weak;
		d.reason = "long-form track (" + text::formatDuration(durationMs)
			+ "); one BPM may not describe the whole recording";
		return d;
	}
	if (analysis.stability < m_options.reviewStabilityThreshold) {
		d.kind = TempoDecisionKind::NeedsReview;
		d.confidence = Confidence::Weak;
		d.reason = "section agreement is low; the estimate is not stable enough to accept automatically";
		return d;
	}
	if (analysis.bpm < m_options.minBpm || analysis.bpm > m_options.maxBpm) {
		d.kind = TempoDecisionKind::NeedsReview;
		d.confidence = Confidence::Weak;
		d.reason = "estimate " + std::to_string(analysis.bpm) + " falls outside the plausible range";
		return d;
	}

	d.kind = TempoDecisionKind::Propose;
	d.confidence = (analysis.stability >= 0.9) ? Confidence::Strong : Confidence::Moderate;
	d.reason = "stable estimate across " + std::to_string(analysis.sectionsAnalysed)
		+ " analysed sections; integer value written to TBPM, precise value retained in the catalogue";
	return d;
}

// ---------------------------------------------------------------------------
// Lyrics
// ---------------------------------------------------------------------------

LyricsPolicy::LyricsPolicy(LyricsOptions options) : m_options(std::move(options)) {}

bool LyricsPolicy::isValidLanguageCode(std::string_view code) {
	// ID3v2 requires exactly three lowercase letters. "XXX" is the documented
	// "unknown" value and is accepted in either case.
	if (code.size() != 3) return false;
	for (char c : code) {
		if (!std::isalpha(static_cast<unsigned char>(c))) return false;
	}
	return true;
}

LyricsPolicy::MatchScore LyricsPolicy::score(const LyricsMatchInput& input, const LyricsCandidate& c) const {
	MatchScore s;
	s.titleSimilarity = text::similarity(input.localTitle, c.trackName);
	s.artistSimilarity = text::similarity(input.localArtist, c.artistName);
	s.albumSimilarity = input.localAlbum.empty() || c.albumName.empty()
		? -1.0
		: text::similarity(input.localAlbum, c.albumName);

	s.durationDeltaMs = (input.localDurationMs > 0 && c.durationMs > 0)
		? std::abs(input.localDurationMs - c.durationMs)
		: -1;
	s.durationWithinTolerance = (s.durationDeltaMs >= 0)
		&& (s.durationDeltaMs <= m_options.durationToleranceMs);

	// Version qualifiers are the difference between a radio edit and an extended
	// mix. A lyric set matching the wrong version is a wrong result.
	const auto localQ = text::stripQualifiers(input.localTitle);
	const auto remoteQ = text::stripQualifiers(c.trackName);
	const auto editionSet = [](const std::vector<std::string>& qs) {
		std::vector<std::string> out;
		for (const auto& q : qs) {
			const std::string n = text::normaliseForMatching(q);
			if (text::isEditionQualifier(n)) out.push_back(n);
		}
		std::sort(out.begin(), out.end());
		return out;
	};
	s.versionQualifiersAgree = (editionSet(localQ.qualifiers) == editionSet(remoteQ.qualifiers));

	s.evidence.push_back({"title_similarity",
		"title similarity " + std::to_string(s.titleSimilarity), s.titleSimilarity >= m_options.minimumTitleSimilarity});
	s.evidence.push_back({"artist_similarity",
		"artist similarity " + std::to_string(s.artistSimilarity),
		s.artistSimilarity >= m_options.minimumArtistSimilarity});
	if (s.durationDeltaMs >= 0) {
		s.evidence.push_back({"duration_delta",
			"duration differs by " + std::to_string(s.durationDeltaMs) + " ms", s.durationWithinTolerance});
	} else {
		s.evidence.push_back({"duration_unknown", "no duration available on one side to compare", false});
	}
	if (!s.versionQualifiersAgree) {
		s.evidence.push_back({"version_mismatch",
			"version qualifiers differ between \"" + input.localTitle + "\" and \"" + c.trackName + "\"", false});
	}

	// Confidence. A same-title result alone is explicitly insufficient.
	const bool titleOk = s.titleSimilarity >= m_options.minimumTitleSimilarity;
	const bool artistOk = s.artistSimilarity >= m_options.minimumArtistSimilarity;

	if (!titleOk || !artistOk || !s.versionQualifiersAgree) {
		s.confidence = Confidence::Weak;
	} else if (s.durationWithinTolerance && s.albumSimilarity >= 0.8) {
		s.confidence = Confidence::Strong;
	} else if (s.durationWithinTolerance) {
		s.confidence = Confidence::Moderate;
	} else if (s.durationDeltaMs < 0) {
		// No duration to compare: title and artist alone are not enough.
		s.confidence = Confidence::Weak;
	} else {
		s.confidence = Confidence::Weak;
	}
	return s;
}

LyricsDecision LyricsPolicy::decide(const LyricsMatchInput& input, const std::vector<LyricsCandidate>& candidates,
	bool fileHasExistingLyrics) const {
	LyricsDecision d;
	d.language = m_options.defaultLanguage;

	if (fileHasExistingLyrics && m_options.preserveExisting) {
		d.state = LyricsState::ExistingPreserved;
		d.reason = "the file already carries lyrics; preserved unless replacement is explicitly selected";
		d.confidence = Confidence::Strong;
		return d;
	}

	if (candidates.empty()) {
		d.state = LyricsState::NotFound;
		d.reason = "no provider returned a candidate for this recording; this is not evidence that the "
			"track is instrumental";
		return d;
	}

	int bestIndex = -1;
	MatchScore bestScore;
	for (std::size_t i = 0; i < candidates.size(); ++i) {
		const MatchScore s = score(input, candidates[i]);
		if (bestIndex < 0 || s.confidence > bestScore.confidence
			|| (s.confidence == bestScore.confidence && s.titleSimilarity > bestScore.titleSimilarity)) {
			bestIndex = static_cast<int>(i);
			bestScore = s;
		}
	}

	const LyricsCandidate& best = candidates[static_cast<std::size_t>(bestIndex)];
	d.selectedCandidate = bestIndex;
	d.providerId = best.providerId;
	d.sourceUrl = best.sourceUrl;
	d.evidence = bestScore.evidence;
	d.confidence = bestScore.confidence;

	if (bestScore.confidence <= Confidence::Weak) {
		d.state = LyricsState::NeedsReview;
		d.reason = "the best candidate does not match confidently on artist, title, version and duration together";
		return d;
	}

	// The provider's own instrumental flag is authoritative for that recording;
	// absence of lyrics is not.
	if (best.instrumental) {
		d.state = LyricsState::Instrumental;
		d.reason = "the provider marks this recording as instrumental";
		return d;
	}

	if (best.plainLyrics.empty()) {
		d.state = LyricsState::NotFound;
		d.reason = "the matched record carries no plain lyrics text";
		return d;
	}

	d.state = LyricsState::Found;
	d.text = best.plainLyrics;
	if (!isValidLanguageCode(d.language)) d.language = "eng";
	d.reason = "matched on artist, title, version and duration";
	return d;
}

} // namespace ml
