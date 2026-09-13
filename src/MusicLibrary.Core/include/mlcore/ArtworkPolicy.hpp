// SPDX-License-Identifier: GPL-3.0-or-later
// ART-001 / ART-002 / ART-003: artwork selection.
//
// The ranking order is fixed by FRD section 6 and is NOT negotiable:
//   1. correctness  -- is this the intended cover for this release/edition?
//   2. fidelity and condition -- is it clean and faithful?
//   3. resolution   -- only among candidates equal on 1 and 2.
//
// A clean 600x600 digital asset is an acceptable final source. Resolution alone
// never wins, and provider priority never overrides observed quality.
#pragma once

#include "mlcore/Types.hpp"

#include <string>
#include <vector>

namespace ml {

/// What kind of asset a candidate is believed to be. "Evidenced" means the
/// evidence is recorded and inspectable; it is never asserted from a provider's
/// own description alone (FRD section 6 point 4).
enum class ArtworkSourceType {
	Unknown = 0,
	EvidencedDigitalAsset,   ///< Original digital cover from a catalogue or label.
	EvidencedRestoration,    ///< A careful restoration of the same design.
	ScanOrPhoto,             ///< A scan or photograph of a physical sleeve.
	UserSupplied,            ///< Imported by the user from a file or URL.
	ExistingEmbedded,        ///< Already embedded in the file.
};

std::string_view toString(ArtworkSourceType t);

/// Observable condition defects. These describe the image, not its provenance.
enum class ArtworkDefect {
	ColourCast,
	Fading,
	SleeveWear,
	RingWear,
	Crease,
	Scratch,
	Sticker,
	UnwantedBorder,
	Skew,
	ScanMoire,
	CompressionBlocks,
	Blur,
	SharpeningHalo,
	Watermark,
	WrongImage,
	Upscaled,           ///< Evidence of prior upscaling: detail does not match size.
};

std::string_view toString(ArtworkDefect d);

/// How the candidate relates to the intended cover for this release.
enum class CoverMatch {
	Unknown = 0,        ///< Not established. Routes to review.
	IntendedCover,      ///< The cover of this release/edition.
	FaithfulRestoration,///< The same design, restored or supplied for a reissue.
	AlternateEdition,   ///< A different edition's cover.
	DifferentRelease,   ///< A different release entirely.
	WrongArtwork,       ///< Not this album.
};

std::string_view toString(CoverMatch m);

/// One artwork candidate under consideration.
struct ArtworkCandidate {
	ArtworkId id;
	std::string providerId;         ///< "itunes", "coverartarchive", "local", "embedded", "manual".
	std::string providerName;
	std::string pageUrl;
	std::string imageUrl;
	std::string localPath;          ///< Set once the asset bytes are stored.
	std::string contentSha256;

	/// Dimensions actually measured by decoding the received bytes. The FRD
	/// requires these to be distinguished from a requested CDN size or a
	/// filename claiming "3000x3000".
	int measuredWidth = 0;
	int measuredHeight = 0;
	bool dimensionsMeasured = false;

	/// A provider's claim about native resolution. Evidence, never proof.
	int claimedWidth = 0;
	int claimedHeight = 0;

	std::string mimeType;
	std::int64_t byteLength = 0;

	ArtworkSourceType sourceType = ArtworkSourceType::Unknown;
	CoverMatch coverMatch = CoverMatch::Unknown;
	Confidence matchConfidence = Confidence::Unknown;

	std::vector<ArtworkDefect> defects;
	std::vector<Evidence> evidence;

	/// Set when the user locked this choice. A lock outranks everything else and
	/// survives rescans and provider refreshes (ART-003).
	bool manuallyLocked = false;
	/// Set when the user explicitly rejected this candidate.
	bool manuallyRejected = false;

	bool isSquare() const { return dimensionsMeasured && measuredWidth == measuredHeight && measuredWidth > 0; }
	int shortestSide() const {
		if (!dimensionsMeasured) return 0;
		return (measuredWidth < measuredHeight) ? measuredWidth : measuredHeight;
	}
};

/// The outcome of ranking candidates for one album.
enum class ArtworkOutcome {
	Selected = 0,          ///< A candidate was chosen automatically.
	NeedsReview,           ///< Evidence insufficient; a human must decide.
	NoAdequateSource,      ///< Nothing usable found; existing artwork is retained.
	LockedByUser,          ///< A manual lock decided it.
};

std::string_view toString(ArtworkOutcome outcome);

struct ArtworkSelection {
	ArtworkOutcome outcome = ArtworkOutcome::NeedsReview;
	/// Index into the candidate vector, or -1.
	int selectedIndex = -1;
	std::string reason;
	/// Per-candidate rejection reasons, parallel to the input vector.
	std::vector<std::string> rejectionReasons;
	/// Candidates that could not be decided between.
	std::vector<int> tiedIndices;
	/// Policy version, stored so a decision can be explained and repeated.
	std::string policyVersion;
};

struct ArtworkOptions {
	/// Output derivative edge length. Fixed at 600 by ART-001.
	int derivativeSize = 600;
	/// JPEG quality for the derivative. Fixed at 75 by ART-001.
	int jpegQuality = 75;
	/// A source at least this size on its shortest side is adequate. 600 means a
	/// clean 600x600 asset is accepted, exactly as FRD section 6 requires.
	int minimumAdequateSize = 600;
	/// Never upscale a smaller source to manufacture compliance.
	static constexpr bool allowUpscaling = false;
	/// Route non-square candidates to review instead of cropping.
	static constexpr bool allowAutomaticCrop = false;
	/// Treat a scan or photo as acceptable when nothing better exists.
	bool acceptScansWhenNothingBetter = true;
};

class ArtworkPolicy {
public:
	static constexpr std::string_view kPolicyVersion = "artwork-policy/1";

	explicit ArtworkPolicy(ArtworkOptions options = {});

	/// Ranks candidates and returns the decision with its reasons.
	ArtworkSelection select(const std::vector<ArtworkCandidate>& candidates) const;

	/// Condition score in [0,1] derived from observed defects. Exposed for the
	/// review UI. This is an ordering aid, not a claim of provenance, and it can
	/// never by itself promote a candidate past a correctness failure.
	static double conditionScore(const ArtworkCandidate& c);

	/// True when the candidate's defects make it unusable regardless of size.
	static bool isDisqualifying(ArtworkDefect d);

	const ArtworkOptions& options() const { return m_options; }

private:
	/// Tier 1: correctness. Lower is better; -1 means disqualified.
	int correctnessTier(const ArtworkCandidate& c) const;
	/// Tier 2: fidelity and condition. Lower is better.
	int conditionTier(const ArtworkCandidate& c) const;

	ArtworkOptions m_options;
};

} // namespace ml
