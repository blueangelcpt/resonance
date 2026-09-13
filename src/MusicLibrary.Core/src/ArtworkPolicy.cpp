// SPDX-License-Identifier: GPL-3.0-or-later
#include "mlcore/ArtworkPolicy.hpp"

#include <algorithm>
#include <cmath>

namespace ml {

std::string_view toString(ArtworkSourceType t) {
	switch (t) {
		case ArtworkSourceType::Unknown: return "unknown";
		case ArtworkSourceType::EvidencedDigitalAsset: return "evidenced_digital_asset";
		case ArtworkSourceType::EvidencedRestoration: return "evidenced_restoration";
		case ArtworkSourceType::ScanOrPhoto: return "scan_or_photo";
		case ArtworkSourceType::UserSupplied: return "user_supplied";
		case ArtworkSourceType::ExistingEmbedded: return "existing_embedded";
	}
	return "unknown";
}

std::string_view toString(ArtworkDefect d) {
	switch (d) {
		case ArtworkDefect::ColourCast: return "colour_cast";
		case ArtworkDefect::Fading: return "fading";
		case ArtworkDefect::SleeveWear: return "sleeve_wear";
		case ArtworkDefect::RingWear: return "ring_wear";
		case ArtworkDefect::Crease: return "crease";
		case ArtworkDefect::Scratch: return "scratch";
		case ArtworkDefect::Sticker: return "sticker";
		case ArtworkDefect::UnwantedBorder: return "unwanted_border";
		case ArtworkDefect::Skew: return "skew";
		case ArtworkDefect::ScanMoire: return "scan_moire";
		case ArtworkDefect::CompressionBlocks: return "compression_blocks";
		case ArtworkDefect::Blur: return "blur";
		case ArtworkDefect::SharpeningHalo: return "sharpening_halo";
		case ArtworkDefect::Watermark: return "watermark";
		case ArtworkDefect::WrongImage: return "wrong_image";
		case ArtworkDefect::Upscaled: return "upscaled";
	}
	return "unknown";
}

std::string_view toString(CoverMatch m) {
	switch (m) {
		case CoverMatch::Unknown: return "unknown";
		case CoverMatch::IntendedCover: return "intended_cover";
		case CoverMatch::FaithfulRestoration: return "faithful_restoration";
		case CoverMatch::AlternateEdition: return "alternate_edition";
		case CoverMatch::DifferentRelease: return "different_release";
		case CoverMatch::WrongArtwork: return "wrong_artwork";
	}
	return "unknown";
}

std::string_view toString(ArtworkOutcome outcome) {
	switch (outcome) {
		case ArtworkOutcome::Selected: return "selected";
		case ArtworkOutcome::NeedsReview: return "needs_review";
		case ArtworkOutcome::NoAdequateSource: return "no_adequate_source";
		case ArtworkOutcome::LockedByUser: return "locked_by_user";
	}
	return "needs_review";
}

ArtworkPolicy::ArtworkPolicy(ArtworkOptions options) : m_options(std::move(options)) {}

bool ArtworkPolicy::isDisqualifying(ArtworkDefect d) {
	// A watermarked promotional substitute or the wrong image is rejected
	// outright (FRD section 6 point 6).
	return d == ArtworkDefect::Watermark || d == ArtworkDefect::WrongImage;
}

double ArtworkPolicy::conditionScore(const ArtworkCandidate& c) {
	// Weights reflect how much each defect harms a cover used at 600x600.
	// Physical damage dominates, because the FRD's decision cases turn on
	// preferring a clean small asset over a damaged large scan.
	double penalty = 0.0;
	for (ArtworkDefect d : c.defects) {
		switch (d) {
			case ArtworkDefect::WrongImage:
			case ArtworkDefect::Watermark:        penalty += 1.00; break;
			case ArtworkDefect::Fading:
			case ArtworkDefect::ColourCast:       penalty += 0.30; break;
			case ArtworkDefect::SleeveWear:
			case ArtworkDefect::RingWear:
			case ArtworkDefect::Crease:
			case ArtworkDefect::Scratch:
			case ArtworkDefect::Sticker:          penalty += 0.25; break;
			case ArtworkDefect::Skew:
			case ArtworkDefect::UnwantedBorder:   penalty += 0.20; break;
			case ArtworkDefect::ScanMoire:
			case ArtworkDefect::CompressionBlocks:penalty += 0.15; break;
			case ArtworkDefect::Blur:
			case ArtworkDefect::SharpeningHalo:   penalty += 0.15; break;
			case ArtworkDefect::Upscaled:         penalty += 0.35; break;
		}
	}
	return std::max(0.0, 1.0 - penalty);
}

int ArtworkPolicy::correctnessTier(const ArtworkCandidate& c) const {
	// Tier 1 of the fixed ranking order. Lower is better; -1 disqualifies.
	for (ArtworkDefect d : c.defects) {
		if (isDisqualifying(d)) return -1;
	}
	switch (c.coverMatch) {
		case CoverMatch::IntendedCover:       return 0;
		// A faithful restoration of the same design is desirable even when it was
		// supplied for a later digital release (FRD section 6 point 1).
		case CoverMatch::FaithfulRestoration: return 1;
		case CoverMatch::Unknown:             return 2;
		// An alternate edition must never silently replace the intended cover.
		case CoverMatch::AlternateEdition:    return 3;
		case CoverMatch::DifferentRelease:    return 4;
		case CoverMatch::WrongArtwork:        return -1;
	}
	return 2;
}

int ArtworkPolicy::conditionTier(const ArtworkCandidate& c) const {
	// Tier 2. Grouped into bands so that small scoring differences do not
	// pretend to be meaningful distinctions between two clean assets.
	const double score = conditionScore(c);
	if (score >= 0.95) return 0;   // clean
	if (score >= 0.80) return 1;   // minor observable defects
	if (score >= 0.55) return 2;   // visible damage
	return 3;                      // heavily damaged
}

ArtworkSelection ArtworkPolicy::select(const std::vector<ArtworkCandidate>& candidates) const {
	ArtworkSelection result;
	result.policyVersion = std::string(kPolicyVersion);
	result.rejectionReasons.assign(candidates.size(), std::string());

	if (candidates.empty()) {
		result.outcome = ArtworkOutcome::NoAdequateSource;
		result.reason = "no candidates were found; existing artwork is retained";
		return result;
	}

	// --- A manual lock decides, unconditionally (ART-003) ---------------------
	for (std::size_t i = 0; i < candidates.size(); ++i) {
		if (candidates[i].manuallyLocked && !candidates[i].manuallyRejected) {
			result.outcome = ArtworkOutcome::LockedByUser;
			result.selectedIndex = static_cast<int>(i);
			result.reason = "a manual lock selects this candidate; provider results cannot override it";
			for (std::size_t k = 0; k < candidates.size(); ++k) {
				if (k != i) result.rejectionReasons[k] = "a different candidate is locked by the user";
			}
			return result;
		}
	}

	// --- Build the eligible set ---------------------------------------------
	struct Ranked {
		std::size_t index;
		int correctness;
		int condition;
		int usableDetail;   ///< Shortest measured side; only compared within a tier.
		bool adequate;
	};
	std::vector<Ranked> eligible;

	for (std::size_t i = 0; i < candidates.size(); ++i) {
		const ArtworkCandidate& c = candidates[i];

		if (c.manuallyRejected) {
			result.rejectionReasons[i] = "rejected by the user";
			continue;
		}

		const int correctness = correctnessTier(c);
		if (correctness < 0) {
			std::string why = "disqualified: ";
			if (c.coverMatch == CoverMatch::WrongArtwork) {
				why += "not this album's artwork";
			} else {
				why += "carries a disqualifying defect (";
				bool first = true;
				for (ArtworkDefect d : c.defects) {
					if (!isDisqualifying(d)) continue;
					if (!first) why += ", ";
					why += toString(d);
					first = false;
				}
				why += ")";
			}
			result.rejectionReasons[i] = std::move(why);
			continue;
		}

		// Dimensions must have been measured by decoding. A CDN size request or a
		// filename claiming a size is not evidence (FRD section 6 point 5).
		if (!c.dimensionsMeasured) {
			result.rejectionReasons[i] =
				"dimensions have not been measured by decoding the received bytes; a claimed size is not evidence";
			continue;
		}

		// Non-square candidates go to review rather than being cropped or
		// stretched (FRD section 6 point 6, FN-ART-04).
		if (!c.isSquare()) {
			result.rejectionReasons[i] = "not square (" + std::to_string(c.measuredWidth) + "x"
				+ std::to_string(c.measuredHeight) + "); framing needs a review decision, never an automatic crop";
			continue;
		}

		Ranked r;
		r.index = i;
		r.correctness = correctness;
		r.condition = conditionTier(c);
		r.usableDetail = c.shortestSide();
		r.adequate = (r.usableDetail >= m_options.minimumAdequateSize);
		eligible.push_back(r);
	}

	if (eligible.empty()) {
		result.outcome = ArtworkOutcome::NoAdequateSource;
		result.reason = "no candidate passed correctness and framing checks; existing artwork is retained";
		return result;
	}

	// A source below the required output resolution is "source quality
	// unresolved": we never upscale to manufacture compliance.
	std::vector<Ranked> adequate;
	std::copy_if(eligible.begin(), eligible.end(), std::back_inserter(adequate),
		[](const Ranked& r) { return r.adequate; });

	if (adequate.empty()) {
		for (const auto& r : eligible) {
			result.rejectionReasons[r.index] = "source is " + std::to_string(candidates[r.index].measuredWidth)
				+ "x" + std::to_string(candidates[r.index].measuredHeight) + ", below the "
				+ std::to_string(m_options.minimumAdequateSize) + " px output; upscaling is not permitted";
		}
		result.outcome = ArtworkOutcome::NeedsReview;
		result.reason = "source quality unresolved: every candidate is smaller than the required "
			+ std::to_string(m_options.derivativeSize) + "x" + std::to_string(m_options.derivativeSize) + " output";
		return result;
	}

	// --- Rank: correctness, then condition, then usable detail ---------------
	std::sort(adequate.begin(), adequate.end(), [&](const Ranked& a, const Ranked& b) {
		if (a.correctness != b.correctness) return a.correctness < b.correctness;
		if (a.condition != b.condition) return a.condition < b.condition;
		// Only now does size matter, and only as "more genuine usable detail".
		if (a.usableDetail != b.usableDetail) return a.usableDetail > b.usableDetail;
		return a.index < b.index;
	});

	const Ranked& best = adequate.front();
	const ArtworkCandidate& winner = candidates[best.index];

	// --- Ambiguity that must not be resolved automatically -------------------
	// Equal on correctness and condition, and the same size: nothing distinguishes
	// them, so the choice is arbitrary and must be shown.
	std::vector<int> tied;
	for (const auto& r : adequate) {
		if (r.index == best.index) continue;
		if (r.correctness == best.correctness && r.condition == best.condition
			&& r.usableDetail == best.usableDetail) {
			tied.push_back(static_cast<int>(r.index));
		}
	}

	// Uncertain damage versus intentional texture: the FRD requires the
	// uncertainty to be surfaced rather than settled by a sharpness score.
	const bool uncertainCondition = (winner.matchConfidence == Confidence::Weak)
		|| (winner.coverMatch == CoverMatch::Unknown);

	for (const auto& r : adequate) {
		if (r.index == best.index) continue;
		if (result.rejectionReasons[r.index].empty()) {
			if (r.correctness > best.correctness) {
				result.rejectionReasons[r.index] = "a candidate matching the intended cover ranks higher ("
					+ std::string(toString(candidates[r.index].coverMatch)) + ")";
			} else if (r.condition > best.condition) {
				result.rejectionReasons[r.index] = "observed condition is worse than the selected candidate; "
					"resolution does not override condition";
			} else {
				result.rejectionReasons[r.index] = "equally faithful and clean, but carries less usable detail ("
					+ std::to_string(candidates[r.index].measuredWidth) + "px against "
					+ std::to_string(winner.measuredWidth) + "px)";
			}
		}
	}

	if (!tied.empty()) {
		result.outcome = ArtworkOutcome::NeedsReview;
		result.selectedIndex = static_cast<int>(best.index);
		result.tiedIndices = std::move(tied);
		result.reason = "several candidates are equal on correctness, condition and usable detail; "
			"choosing between them is a review decision";
		return result;
	}

	if (uncertainCondition) {
		result.outcome = ArtworkOutcome::NeedsReview;
		result.selectedIndex = static_cast<int>(best.index);
		result.reason = "the best candidate's relationship to the intended cover is not established ("
			+ std::string(toString(winner.coverMatch)) + ", confidence "
			+ std::string(toString(winner.matchConfidence)) + "); review before embedding";
		return result;
	}

	// An alternate-edition cover never replaces an adequate intended cover
	// automatically, even when it is cleaner or larger.
	if (winner.coverMatch == CoverMatch::AlternateEdition) {
		result.outcome = ArtworkOutcome::NeedsReview;
		result.selectedIndex = static_cast<int>(best.index);
		result.reason = "the best available candidate is an alternate edition's cover; "
			"an edition change must be reviewed, not applied silently";
		return result;
	}

	result.outcome = ArtworkOutcome::Selected;
	result.selectedIndex = static_cast<int>(best.index);

	std::string why = "selected: ";
	why += toString(winner.coverMatch);
	why += ", ";
	why += (best.condition == 0) ? "clean" : "condition tier " + std::to_string(best.condition);
	why += ", ";
	why += std::to_string(winner.measuredWidth) + "x" + std::to_string(winner.measuredHeight);
	why += " measured, source ";
	why += toString(winner.sourceType);
	if (winner.measuredWidth == m_options.minimumAdequateSize) {
		why += "; a clean asset at the output size is adequate and its resolution is not an unresolved issue";
	}
	result.reason = std::move(why);
	return result;
}

} // namespace ml
