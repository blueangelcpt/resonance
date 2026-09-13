// SPDX-License-Identifier: GPL-3.0-or-later
// The exact before/after plan for one file, and for a whole run.
//
// A plan is data. Producing one requires no write capability at all, which is
// what lets the `plan` and `analyse` CLI commands be structurally incapable of
// modifying a file (FN-CLI-02).
#pragma once

#include "mlcore/ArtworkPolicy.hpp"
#include "mlcore/Enrichment.hpp"
#include "mlcore/NamingTemplate.hpp"
#include "mlcore/TagPolicy.hpp"
#include "mlcore/Types.hpp"

#include <string>
#include <vector>

namespace ml {

/// Why a planned file cannot be written yet.
enum class PlanBlocker {
	None = 0,
	NamingReviewRequired,
	DestinationCollision,
	ArtworkReviewRequired,
	PrivacyReviewRequired,
	GainExceptionRequired,
	TempoReviewRequired,
	LyricsReviewRequired,
	SourceUnreadable,
	SourceChangedSincePlan,
	DestinationInsideProtectedRoot,
};

std::string_view toString(PlanBlocker b);

/// The artwork change proposed for one file.
struct ArtworkPlan {
	bool replaceFrontCover = false;
	ArtworkOutcome outcome = ArtworkOutcome::NeedsReview;
	ArtworkId selectedAsset;
	std::string sourceAssetPath;        ///< Received asset, stored unmodified.
	std::string derivativePath;         ///< 600x600 JPEG q75 derivative.
	std::string derivativeSha256;
	int derivativeWidth = 0;
	int derivativeHeight = 0;
	std::size_t derivativeBytes = 0;
	std::string selectionReason;
	std::string policyVersion;
	/// Image roles other than the front cover that are inventoried and preserved.
	std::vector<std::string> preservedPictureRoles;
};

/// Everything planned for one file.
struct FilePlan {
	FileId fileId;
	std::string sourcePath;             ///< Absolute source path. Read-only.
	std::string sourceSha256;           ///< Expected content at plan time.
	std::string sourceAudioSha256;      ///< Expected MPEG payload hash.
	FileIdentity sourceIdentity;

	std::string destinationRelativePath;
	NamingResult naming;

	std::vector<FrameDecision> tagDecisions;   ///< Privacy, gain and enrichment merged.
	ArtworkPlan artwork;
	TempoDecision tempo;
	LyricsDecision lyrics;

	SizeAccounting size;
	std::vector<PlanBlocker> blockers;
	std::vector<std::string> notes;

	/// The tag container the output will use. Defaults to preserving the source's
	/// existing supported version (FN-TAG-01).
	TagContainer outputContainer = TagContainer::Unknown;
	std::size_t targetPaddingBytes = 2048;

	bool writable() const { return blockers.empty(); }

	/// Frames that will actually change. Used by the review view.
	std::vector<const FrameDecision*> changingDecisions() const;
};

/// A complete planned run.
struct ChangeSet {
	ChangeSetId id;
	std::string outputRootPath;
	std::string createdAtIso8601;
	std::string configHash;             ///< Hash of the policy configuration used.
	std::vector<FilePlan> files;

	/// Destination collisions found across the whole set (NAME-002).
	std::vector<CollisionDetector::Collision> collisions;

	struct Summary {
		std::size_t total = 0;
		std::size_t writable = 0;
		std::size_t blocked = 0;
		std::size_t needingReview = 0;
		std::int64_t optimisationSavings = 0;
		std::int64_t enrichmentGrowth = 0;
		std::size_t artworkReplacements = 0;
		std::size_t lyricsAdded = 0;
		std::size_t bpmProposed = 0;
		std::size_t privacyRemovals = 0;
		std::size_t gainRemovals = 0;
	};

	Summary summarise() const;
};

/// Renders a plan as JSON or CSV for the `report` command.
std::string toJson(const ChangeSet& set, bool includeUnchanged = false);
std::string toCsv(const ChangeSet& set);

} // namespace ml
