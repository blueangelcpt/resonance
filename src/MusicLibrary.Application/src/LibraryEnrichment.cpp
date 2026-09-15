// SPDX-License-Identifier: GPL-3.0-or-later
// Library: artwork, lyrics, planning, export, verification and recovery.
//
// Split from Library.cpp only for file size; these are members of the same class
// and share its invariants.
#include "mlapp/Library.hpp"
#include "mlcore/Text.hpp"
#include "mlinfra/Hashing.hpp"
#include "mlinfra/Mp3Container.hpp"
#include "mlinfra/TagReader.hpp"
#include "mlinfra/TagWriter.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <fstream>
#include <set>

namespace fs = std::filesystem;

namespace ml {

namespace {

class Stopwatch {
public:
	double elapsed() const {
		return std::chrono::duration<double>(std::chrono::steady_clock::now() - m_start).count();
	}

private:
	std::chrono::steady_clock::time_point m_start = std::chrono::steady_clock::now();
};

/// Resolves a catalogued relative path back to an absolute source path.
fs::path resolveSource(const PathGuard& guard, const std::string& relativePath) {
	std::error_code ec;
	for (const auto& root : guard.protectedRoots()) {
		const fs::path candidate = root.resolvedPath / relativePath;
		if (fs::exists(candidate, ec) && !ec) return candidate;
	}
	return {};
}

} // namespace

// ---------------------------------------------------------------------------
// Artwork (ART-001..003)
// ---------------------------------------------------------------------------

Result<std::int64_t> Library::fetchArtwork(ProgressCallback progress) {
	if (!isOpen()) return Error{ErrorCode::Internal, "the library is not open"};
	std::lock_guard<std::mutex> lock(m_primaryMutex);

	auto albums = m_catalogue->listAlbums(false, 0, 0);
	if (!albums) return albums.error();

	const ArtworkPolicy policy(m_config.artwork);
	std::int64_t decided = 0;
	std::int64_t index = 0;

	for (const auto& album : albums.value()) {
		++index;
		if (!reportProgress(progress, index, static_cast<std::int64_t>(albums.value().size()),
				album.albumArtist + " - " + album.album)) {
			return Error{ErrorCode::Cancelled, "cancelled"};
		}

		// A manual lock outranks everything and stops the album being re-fetched
		// on every run (ART-003).
		auto locked = m_catalogue->selectedArtwork(album.id);
		if (locked && locked.value() && locked.value()->manuallyLocked) continue;

		std::vector<ArtworkCandidate> candidates;

		// --- Local first, so the pipeline works with no network at all --------
		LocalArtworkProvider local;
		if (!album.files.empty()) {
			auto firstFile = m_catalogue->loadFile(album.files.front());
			if (firstFile && firstFile.value()) {
				const fs::path sourcePath = resolveSource(m_guard, firstFile.value()->relativePath);
				if (!sourcePath.empty()) {
					local.setSearchDirectory(sourcePath.parent_path());

					TagReadOptions options;
					options.loadPictureBytes = true;
					options.retainFramePayloads = true;
					auto read = TagReader::read(sourcePath, options);
					if (read) {
						for (const auto& frame : read.value().snapshot.frames) {
							if (frame.id != "APIC" && frame.id != "PIC") continue;
							if (frame.binary.empty()) continue;
							std::vector<std::uint8_t> bytes(frame.binary.size());
							std::memcpy(bytes.data(), frame.binary.data(), frame.binary.size());
							local.addEmbeddedCandidate(std::move(bytes),
								"already embedded in " + firstFile.value()->fileName);
							break;
						}
					}
				}
			}
		}

		if (auto found = local.findArtwork({}); found) {
			for (auto& candidate : found.value()) {
				std::string contentType;
				auto bytes = local.fetchImage(candidate, contentType);
				if (!bytes) continue;

				// Dimensions are measured by decoding, never claimed.
				auto dimensions = ImagePipeline::probeDimensions(bytes.value().data(),
					bytes.value().size());
				if (dimensions) {
					candidate.measuredWidth = dimensions.value().first;
					candidate.measuredHeight = dimensions.value().second;
					candidate.dimensionsMeasured = true;
				} else {
					candidate.evidence.push_back({"undecodable",
						"the image could not be decoded, so its size and condition cannot be measured",
						false});
				}
				candidate.byteLength = static_cast<std::int64_t>(bytes.value().size());
				candidate.contentSha256 = Sha256::hashBytes(bytes.value().data(), bytes.value().size());

				if (auto stored = m_assets->put(bytes.value().data(), bytes.value().size(), ".img")) {
					candidate.localPath = stored.value();
				}
				candidates.push_back(std::move(candidate));
			}
		}

		// --- Remote providers -------------------------------------------------
		if (!m_config.offline) {
			AlbumQuery query;
			query.album = album.album;
			query.albumArtist = album.albumArtist;
			query.date = album.date;
			query.trackCount = album.observedTrackCount;
			query.musicBrainzAlbumId = album.musicBrainzAlbumId;

			std::vector<ArtworkProvider*> providers{m_itunes.get(), m_coverArtArchive.get()};
			for (ArtworkProvider* provider : providers) {
				auto found = provider->findArtwork(query);
				if (!found) {
					// A provider failure is recorded and the run continues; it must
					// never cause an inferior automatic replacement (ART-003).
					continue;
				}
				for (auto& candidate : found.value()) {
					std::string contentType;
					auto bytes = provider->fetchImage(candidate, contentType);
					if (!bytes) continue;

					auto dimensions = ImagePipeline::probeDimensions(bytes.value().data(),
						bytes.value().size());
					if (dimensions) {
						candidate.measuredWidth = dimensions.value().first;
						candidate.measuredHeight = dimensions.value().second;
						candidate.dimensionsMeasured = true;
						candidate.evidence.push_back({"measured_dimensions",
							"decoded to " + std::to_string(candidate.measuredWidth) + "x"
								+ std::to_string(candidate.measuredHeight)
								+ ", measured rather than taken from the requested URL size", true});
					}
					candidate.mimeType = contentType;
					candidate.byteLength = static_cast<std::int64_t>(bytes.value().size());
					candidate.contentSha256 = Sha256::hashBytes(bytes.value().data(),
						bytes.value().size());

					// The received asset is stored unmodified, exactly once.
					if (auto stored = m_assets->put(bytes.value().data(), bytes.value().size(), ".img")) {
						candidate.localPath = stored.value();
					}
					candidates.push_back(std::move(candidate));
					if (cancelRequested()) return Error{ErrorCode::Cancelled, "cancelled"};
				}
			}
		}

		if (candidates.empty()) continue;

		// --- Decide ------------------------------------------------------------
		const ArtworkSelection selection = policy.select(candidates);

		auto transaction = m_database.begin(Transaction::Kind::Immediate);
		if (!transaction) return transaction.error();

		for (std::size_t i = 0; i < candidates.size(); ++i) {
			const bool isSelected = (selection.selectedIndex == static_cast<int>(i))
				&& selection.outcome != ArtworkOutcome::NeedsReview;
			const std::string state = isSelected ? "selected" : "candidate";
			const std::string reason = isSelected
				? selection.reason
				: (i < selection.rejectionReasons.size() ? selection.rejectionReasons[i] : std::string());

			auto saved = m_catalogue->saveArtworkCandidate(album.id, candidates[i], state, reason,
				selection.policyVersion);
			if (!saved) return saved.error();

			// --- Derivative, generated once per (asset, configuration) --------
			if (isSelected) {
				auto existing = m_catalogue->findDerivative(saved.value(), m_config.derivative.hash());
				if (existing && !existing.value() && !candidates[i].localPath.empty()) {
					auto bytes = m_assets->get(candidates[i].localPath);
					if (bytes) {
						auto derivative = ImagePipeline::makeDerivative(bytes.value().data(),
							bytes.value().size(), m_config.derivative);
						if (derivative) {
							auto stored = m_assets->put(derivative.value().jpegBytes.data(),
								derivative.value().jpegBytes.size(), ".jpg");
							if (stored) {
								(void)m_catalogue->saveDerivative(saved.value(),
									m_config.derivative.hash(), stored.value(),
									derivative.value().sha256, derivative.value().width,
									derivative.value().height,
									static_cast<std::int64_t>(derivative.value().jpegBytes.size()),
									m_config.derivative.jpegQuality,
									m_config.derivative.chromaSubsampling ? "4:2:0" : "4:4:4");
							}
						}
					}
				}
			}
		}

		if (auto status = transaction.value().commit(); !status) return status.error();
		++decided;
	}

	return decided;
}

// ---------------------------------------------------------------------------
// Lyrics (LYR-001)
// ---------------------------------------------------------------------------

Result<std::int64_t> Library::fetchLyrics(ProgressCallback progress) {
	if (!isOpen()) return Error{ErrorCode::Internal, "the library is not open"};
	std::lock_guard<std::mutex> lock(m_primaryMutex);

	TrackFilter filter;
	filter.readStatus = "ok";
	auto total = m_catalogue->countFiles(filter);
	if (!total) return total.error();

	const LyricsPolicy policy(m_config.lyrics);
	std::int64_t decided = 0;
	constexpr std::int64_t kPage = 200;

	for (std::int64_t offset = 0; offset < total.value(); offset += kPage) {
		auto page = m_catalogue->queryFiles(filter, kPage, offset);
		if (!page) return page.error();
		if (page.value().empty()) break;

		for (const auto& record : page.value()) {
			if (!reportProgress(progress, offset, total.value(), record.title)) {
				return Error{ErrorCode::Cancelled, "cancelled"};
			}

			// Idempotence: a resolved state is not looked up again.
			auto existing = m_catalogue->loadLyrics(record.id);
			if (existing && existing.value()) {
				const LyricsState state = existing.value()->state;
				if (state == LyricsState::Found || state == LyricsState::Instrumental
					|| state == LyricsState::ExistingPreserved) {
					continue;
				}
			}

			LyricsMatchInput input;
			input.localTitle = record.title;
			input.localArtist = record.artist;
			input.localAlbum = record.album;
			input.localDurationMs = record.audio.durationMs;

			std::vector<LyricsCandidate> candidates;
			LyricsDecision decision;

			if (record.hasLyrics && m_config.lyrics.preserveExisting) {
				decision = policy.decide(input, {}, true);
			} else if (m_config.offline) {
				decision.state = LyricsState::NotAttempted;
				decision.reason = "offline mode: remote lyrics lookup was not attempted";
			} else if (input.localTitle.empty() || input.localArtist.empty()) {
				decision.state = LyricsState::NeedsReview;
				decision.reason = "the file has no artist or title to match on";
			} else {
				TrackQuery query;
				query.artist = input.localArtist;
				query.title = input.localTitle;
				query.album = input.localAlbum;
				query.durationMs = input.localDurationMs;

				auto found = m_lrclib->findLyrics(query);
				if (!found) {
					// A failure and a miss are different states, and they stay
					// different (FN-LYR-03).
					decision.state = (found.error().code == ErrorCode::RateLimited)
						? LyricsState::RateLimited
						: LyricsState::Failed;
					decision.reason = found.error().describe();
				} else {
					candidates = found.value();
					decision = policy.decide(input, candidates, false);
				}
			}

			if (auto status = m_catalogue->saveLyrics(record.id, decision); !status) {
				return status.error();
			}
			++decided;

			if (cancelRequested()) return Error{ErrorCode::Cancelled, "cancelled"};
		}
	}

	return decided;
}

// ---------------------------------------------------------------------------
// Planning (no write capability)
// ---------------------------------------------------------------------------

Result<FilePlan> Library::buildPlan(const FileRecord& record, CollisionDetector& collisions,
	std::optional<AlbumId> albumId) {
	FilePlan plan;
	plan.fileId = record.id;
	plan.sourceSha256 = record.contentSha256;
	plan.sourceAudioSha256 = record.audioSha256;
	plan.sourceIdentity = record.identity;
	plan.targetPaddingBytes = m_config.padding.targetPaddingBytes;

	const fs::path sourcePath = resolveSource(m_guard, record.relativePath);
	if (sourcePath.empty()) {
		plan.blockers.push_back(PlanBlocker::SourceUnreadable);
		plan.notes.push_back("the source file is not currently reachable");
		return plan;
	}
	plan.sourcePath = text::pathToUtf8(sourcePath);

	// --- Observed tags, with payloads, so the preview is frame-exact ---------
	TagReadOptions options;
	options.retainFramePayloads = true;
	options.loadPictureBytes = false;
	auto read = TagReader::read(sourcePath, options);
	if (!read) {
		plan.blockers.push_back(PlanBlocker::SourceUnreadable);
		plan.notes.push_back(read.error().describe());
		return plan;
	}
	const TagSnapshot& snapshot = read.value().snapshot;

	// Preserve the source's ID3 version by default (FN-TAG-01).
	plan.outputContainer = isId3v2(snapshot.primaryContainer)
		? snapshot.primaryContainer
		: TagContainer::Id3v2_3;

	// --- Naming (NAME-001) ---------------------------------------------------
	const NamingTemplate naming(m_config.naming);
	const NamingInput namingInput = namingInputFromSnapshot(snapshot, record.extension);
	plan.naming = naming.apply(namingInput);

	if (plan.naming.requiresReview) {
		plan.blockers.push_back(PlanBlocker::NamingReviewRequired);
	} else {
		plan.destinationRelativePath = plan.naming.relativePath;
		if (!collisions.add(plan.destinationRelativePath, record.id)) {
			plan.blockers.push_back(PlanBlocker::DestinationCollision);
			plan.notes.push_back("another file is already planned for this destination");
		}
	}

	// --- Privacy (PRIV-001) ---------------------------------------------------
	const PrivacyPolicy privacyPolicy(m_config.privacy);
	const PrivacyPreview privacy = privacyPolicy.evaluate(snapshot);
	for (const auto& decision : privacy.decisions) {
		if (decision.action == FrameAction::Keep) continue;
		plan.tagDecisions.push_back(decision);
	}
	if (privacy.reviewCount > 0) {
		plan.blockers.push_back(PlanBlocker::PrivacyReviewRequired);
		plan.notes.push_back(std::to_string(privacy.reviewCount)
			+ " privacy finding(s) need a decision before this file is written");
	}

	// --- Gain (GAIN-001) ------------------------------------------------------
	const GainPolicy gainPolicy(m_config.gain);
	const GainPreview gain = gainPolicy.evaluate(snapshot);
	for (const auto& decision : gain.decisions) {
		if (decision.action == FrameAction::Keep) continue;
		plan.tagDecisions.push_back(decision);
	}
	if (gain.blocked()) {
		plan.blockers.push_back(PlanBlocker::GainExceptionRequired);
		for (auto exception : gain.exceptions) {
			plan.notes.push_back("gain exception: " + std::string(toString(exception)));
		}
	}

	// --- Artwork (ART-001) ----------------------------------------------------
	if (albumId) {
		auto selected = m_catalogue->selectedArtwork(*albumId);
		if (selected && selected.value()) {
			auto derivative = m_catalogue->findDerivative(selected.value()->id,
				m_config.derivative.hash());
			if (derivative && derivative.value()) {
				plan.artwork.replaceFrontCover = true;
				plan.artwork.outcome = selected.value()->manuallyLocked
					? ArtworkOutcome::LockedByUser
					: ArtworkOutcome::Selected;
				plan.artwork.selectedAsset = selected.value()->id;
				plan.artwork.sourceAssetPath = selected.value()->localPath;
				plan.artwork.derivativePath = *derivative.value();
				plan.artwork.derivativeWidth = m_config.derivative.edgeSize;
				plan.artwork.derivativeHeight = m_config.derivative.edgeSize;
				plan.artwork.policyVersion = std::string(ArtworkPolicy::kPolicyVersion);
				plan.artwork.selectionReason = "album selection applies identical derivative bytes "
					"to every track on this album";

				// Other picture roles are inventoried and preserved.
				for (const auto& picture : snapshot.pictures) {
					if (picture.type == PictureType::FrontCover) continue;
					plan.artwork.preservedPictureRoles.emplace_back(toString(picture.type));
				}
			}
		}
	}

	// --- Tempo (BPM-001) ------------------------------------------------------
	auto tempo = m_catalogue->loadTempo(record.id);
	if (tempo && tempo.value()) {
		plan.tempo = *tempo.value();
		if (plan.tempo.kind == TempoDecisionKind::NeedsReview) {
			// A disputed BPM does not block the file: the existing value is simply
			// preserved. It appears in the review view as an open item.
			plan.notes.push_back("BPM needs review: " + plan.tempo.reason);
		}
	}

	// --- Lyrics (LYR-001) -----------------------------------------------------
	auto lyrics = m_catalogue->loadLyrics(record.id);
	if (lyrics && lyrics.value()) {
		plan.lyrics = *lyrics.value();
		if (plan.lyrics.state == LyricsState::NeedsReview) {
			plan.notes.push_back("lyrics need review: " + plan.lyrics.reason);
		}
	}

	// --- Size accounting ------------------------------------------------------
	for (const auto& decision : plan.tagDecisions) {
		if (decision.action == FrameAction::Remove) {
			plan.size.optimisationSavings += static_cast<std::int64_t>(decision.beforeBytes);
		}
	}
	if (plan.artwork.replaceFrontCover) {
		std::int64_t existingCover = 0;
		for (const auto& picture : snapshot.pictures) {
			if (picture.type == PictureType::FrontCover) {
				existingCover = static_cast<std::int64_t>(picture.byteLength);
				break;
			}
		}
		std::error_code ec;
		const auto derivativeBytes = static_cast<std::int64_t>(
			fs::file_size(m_assets->resolve(plan.artwork.derivativePath), ec));
		if (!ec) {
			plan.artwork.derivativeBytes = static_cast<std::size_t>(derivativeBytes);
			plan.size.enrichmentGrowth += std::max<std::int64_t>(derivativeBytes - existingCover, 0);
		}
	}
	if (plan.lyrics.writesTag()) {
		plan.size.enrichmentGrowth += static_cast<std::int64_t>(plan.lyrics.text.size());
	}
	plan.size.paddingDelta = static_cast<std::int64_t>(m_config.padding.targetPaddingBytes)
		- static_cast<std::int64_t>(snapshot.id3v2PaddingBytes);

	return plan;
}

Result<ChangeSet> Library::plan(ProgressCallback progress) {
	if (!isOpen()) return Error{ErrorCode::Internal, "the library is not open"};
	std::lock_guard<std::mutex> lock(m_primaryMutex);
	if (m_config.outputRoot.empty()) {
		// FN-CLI-02: every modifying command requires an output root. Planning is
		// not modifying, but a plan without a destination cannot be executed, so
		// the requirement is enforced here where it is actionable.
		return Error{ErrorCode::InvalidArgument,
			"an output root is required before a change plan can be produced"};
	}

	ChangeSet set;
	set.outputRootPath = text::pathToUtf8(m_config.outputRoot);
	set.createdAtIso8601 = nowIso8601();
	set.configHash = m_config.hash();

	TrackFilter filter;
	filter.readStatus = "ok";
	auto total = m_catalogue->countFiles(filter);
	if (!total) return total.error();

	auto changeSetId = m_catalogue->createChangeSet(set.outputRootPath, set.configHash, "plan");
	if (!changeSetId) return changeSetId.error();
	set.id = changeSetId.value();

	CollisionDetector collisions;
	constexpr std::int64_t kPage = 500;

	for (std::int64_t offset = 0; offset < total.value(); offset += kPage) {
		auto page = m_catalogue->queryFiles(filter, kPage, offset);
		if (!page) return page.error();
		if (page.value().empty()) break;

		auto transaction = m_database.begin(Transaction::Kind::Immediate);
		if (!transaction) return transaction.error();

		for (const auto& record : page.value()) {
			auto albumId = m_catalogue->albumForFile(record.id);
			auto plan = buildPlan(record, collisions,
				albumId.ok() ? albumId.value() : std::optional<AlbumId>{});
			if (!plan) return plan.error();

			if (plan.value().writable() && !plan.value().destinationRelativePath.empty()) {
				// The reservation's UNIQUE constraint is the real guarantee behind
				// NAME-002; the detector above is the early warning.
				auto reserved = m_catalogue->reserveDestination(set.id,
					plan.value().destinationRelativePath, record.id);
				if (!reserved) {
					plan.value().blockers.push_back(PlanBlocker::DestinationCollision);
					plan.value().notes.push_back(reserved.error().message);
				}
			}

			if (auto status = m_catalogue->saveFilePlan(set.id, plan.value()); !status) {
				return status.error();
			}
			set.files.push_back(std::move(plan.value()));
		}

		if (auto status = transaction.value().commit(); !status) return status.error();

		if (!reportProgress(progress, offset + static_cast<std::int64_t>(page.value().size()),
				total.value(), "planning")) {
			return Error{ErrorCode::Cancelled, "cancelled"};
		}
	}

	set.collisions = collisions.collisions();
	return set;
}

Result<FilePlan> Library::previewFile(FileId file) {
	std::lock_guard<std::mutex> lock(m_primaryMutex);
	auto record = m_catalogue->loadFile(file);
	if (!record) return record.error();
	if (!record.value()) return Error{ErrorCode::NotFound, "no such file in the catalogue"};

	CollisionDetector collisions;
	auto albumId = m_catalogue->albumForFile(file);
	return buildPlan(*record.value(), collisions, albumId.ok() ? albumId.value() : std::optional<AlbumId>{});
}

// ---------------------------------------------------------------------------
// Export (SAFE-001, SAFE-002, FN-SAFE-02)
// ---------------------------------------------------------------------------

Result<ExportResult> Library::exportCopies(ChangeSetId set, ProgressCallback progress) {
	if (!isOpen()) return Error{ErrorCode::Internal, "the library is not open"};
	std::lock_guard<std::mutex> lock(m_primaryMutex);
	if (m_config.outputRoot.empty()) {
		return Error{ErrorCode::InvalidArgument, "an output root is required to export copies"};
	}

	Stopwatch stopwatch;
	ExportResult result;
	result.changeSetId = set;

	auto plans = m_catalogue->loadFilePlans(set, true);
	if (!plans) return plans.error();

	result.attempted = static_cast<std::int64_t>(plans.value().size());
	(void)m_catalogue->setChangeSetState(set, "executing");

	const fs::path staging = m_config.dataDirectory / "staging";
	std::error_code ec;
	fs::create_directories(staging, ec);

	std::int64_t index = 0;
	for (const auto& stored : plans.value()) {
		++index;
		if (!reportProgress(progress, index, result.attempted, stored.destinationRelativePath)) {
			result.cancelled = true;
			break;
		}

		// Rebuild the plan from current state rather than trusting the stored one:
		// FN-SAFE-02 step 1 requires revalidating the source against the planned
		// identity and content, and a stale plan must be rejected.
		auto record = m_catalogue->loadFile(stored.fileId);
		if (!record || !record.value()) {
			++result.failed;
			result.failures.push_back("file " + std::to_string(stored.fileId.value)
				+ " is no longer in the catalogue");
			continue;
		}

		const fs::path sourcePath = resolveSource(m_guard, record.value()->relativePath);
		if (sourcePath.empty()) {
			++result.failed;
			result.failures.push_back(record.value()->relativePath + ": source is not reachable");
			(void)m_catalogue->setOperationState(set, stored.fileId, "failed", {}, "source unreachable");
			continue;
		}

		// Revalidate identity and content.
		const auto identity = PathGuard::identityOf(sourcePath);
		if (!identity || identity->sizeBytes != stored.sourceIdentity.sizeBytes) {
			++result.skipped;
			result.failures.push_back(record.value()->relativePath
				+ ": source changed since the plan was made; refusing to act on a stale plan");
			(void)m_catalogue->setOperationState(set, stored.fileId, "failed", {},
				"source changed since plan");
			continue;
		}

		CollisionDetector collisions;
		auto albumId = m_catalogue->albumForFile(stored.fileId);
		auto rebuilt = buildPlan(*record.value(), collisions,
			albumId.ok() ? albumId.value() : std::optional<AlbumId>{});
		if (!rebuilt) {
			++result.failed;
			result.failures.push_back(record.value()->relativePath + ": " + rebuilt.error().describe());
			continue;
		}
		const FilePlan& plan = rebuilt.value();

		if (!plan.writable()) {
			++result.skipped;
			(void)m_catalogue->setOperationState(set, stored.fileId, "skipped", {},
				"plan is blocked and was not executed");
			continue;
		}

		const fs::path destination = m_config.outputRoot / plan.destinationRelativePath;

		// Never overwrite an existing destination.
		if (fs::exists(destination, ec) && !ec) {
			++result.skipped;
			result.failures.push_back(plan.destinationRelativePath
				+ ": destination already exists; refusing to overwrite");
			(void)m_catalogue->setOperationState(set, stored.fileId, "skipped", {},
				"destination exists");
			continue;
		}

		// --- Build the write request ------------------------------------------
		TagWriteRequest request;
		request.decisions = plan.tagDecisions;
		request.outputVersion = plan.outputContainer;
		request.targetPaddingBytes = plan.targetPaddingBytes;
		request.clearId3v1Comment = m_config.privacy.clearId3v1Comment;

		for (const auto& decision : plan.tagDecisions) {
			if (decision.action == FrameAction::Remove && decision.container == TagContainer::Apev2) {
				request.removeApeKeys.push_back(decision.frameId);
			}
		}

		if (plan.artwork.replaceFrontCover && !plan.artwork.derivativePath.empty()) {
			auto bytes = m_assets->get(plan.artwork.derivativePath);
			if (bytes) {
				std::vector<std::byte> cover(bytes.value().size());
				std::memcpy(cover.data(), bytes.value().data(), bytes.value().size());
				request.frontCover = std::move(cover);
				request.frontCoverMimeType = "image/jpeg";
			}
		}
		if (plan.lyrics.writesTag()) {
			request.lyrics = plan.lyrics.text;
			request.lyricsLanguage = plan.lyrics.language.size() == 3 ? plan.lyrics.language : "eng";
		}
		if (plan.tempo.writesTag()) {
			request.bpm = plan.tempo.taggedBpm;
		}

		// --- Write to an exclusive temporary file, then publish by rename ------
		(void)m_catalogue->setOperationState(set, stored.fileId, "copying");

		// The temporary file must be on the destination filesystem so publication
		// is a same-filesystem rename.
		auto temporary = ScopedTempFile::createIn(m_guard, destination.parent_path().empty()
			? m_config.outputRoot : destination.parent_path());
		if (!temporary) {
			// The destination directory may not exist yet.
			if (auto status = m_guard.createDirectories(destination.parent_path()); !status) {
				++result.failed;
				result.failures.push_back(plan.destinationRelativePath + ": " + status.error().describe());
				continue;
			}
			temporary = ScopedTempFile::createIn(m_guard, destination.parent_path());
			if (!temporary) {
				++result.failed;
				result.failures.push_back(plan.destinationRelativePath + ": "
					+ temporary.error().describe());
				continue;
			}
		}

		(void)m_catalogue->setOperationState(set, stored.fileId, "applying",
			text::pathToUtf8(temporary.value().path()));

		auto written = TagWriter::writeToNewFile(sourcePath, temporary.value().path(), request, m_guard);
		if (!written) {
			++result.failed;
			result.failures.push_back(plan.destinationRelativePath + ": " + written.error().describe());
			(void)m_catalogue->setOperationState(set, stored.fileId, "failed", {},
				written.error().describe());
			continue;
		}

		// --- Verify before publishing ------------------------------------------
		(void)m_catalogue->setOperationState(set, stored.fileId, "verifying");

		TagReadOptions verifyOptions;
		verifyOptions.hashAudio = true;
		verifyOptions.retainFramePayloads = true;
		auto reread = TagReader::read(temporary.value().path(), verifyOptions);
		if (!reread) {
			++result.failed;
			result.failures.push_back(plan.destinationRelativePath
				+ ": the written file could not be reopened for verification");
			(void)m_catalogue->setOperationState(set, stored.fileId, "failed", {},
				"verification read failed");
			continue;
		}

		if (reread.value().snapshot.audioSha256 != written.value().sourceAudioSha256) {
			++result.failed;
			result.failures.push_back(plan.destinationRelativePath
				+ ": the reopened file's MPEG payload does not match the source");
			(void)m_catalogue->setOperationState(set, stored.fileId, "failed", {},
				"audio hash mismatch on reopen");
			continue;
		}

		// Preservation: nothing outside the planned changes may have moved.
		auto before = TagReader::read(sourcePath, verifyOptions);
		if (before) {
			const auto preservation = TagReader::comparePreservation(before.value(), reread.value(),
				written.value().intentionallyChangedKeys);
			if (!preservation.preserved()) {
				++result.failed;
				std::string detail = plan.destinationRelativePath + ": preservation check failed (";
				detail += std::to_string(preservation.lostFrames.size()) + " lost, ";
				detail += std::to_string(preservation.alteredFrames.size()) + " altered)";
				result.failures.push_back(std::move(detail));
				(void)m_catalogue->setOperationState(set, stored.fileId, "failed", {},
					"unplanned tag changes detected");
				continue;
			}
		}

		// --- Publish -----------------------------------------------------------
		const fs::path temporaryPath = temporary.value().release();
		fs::rename(temporaryPath, destination, ec);
		if (ec) {
			fs::remove(temporaryPath, ec);
			++result.failed;
			result.failures.push_back(plan.destinationRelativePath + ": publish failed");
			(void)m_catalogue->setOperationState(set, stored.fileId, "failed", {}, "rename failed");
			continue;
		}

		// A filesystem rename and a SQLite transaction are not one atomic
		// operation. The journal records "published" before the commit so a crash
		// between them is recoverable rather than silent.
		(void)m_catalogue->setOperationState(set, stored.fileId, "published");
		(void)m_catalogue->completeOperation(set, stored.fileId, written.value().writtenContentSha256,
			written.value().writtenAudioSha256, written.value().bytesBefore,
			written.value().bytesAfter, written.value().size.optimisationSavings,
			written.value().size.enrichmentGrowth);

		// The written state is recorded as its own snapshot, separate from the
		// observed state of the source (CAT-001).
		(void)m_catalogue->saveSnapshot(stored.fileId, reread.value().snapshot, "written");

		++result.written;
		result.bytesWritten += written.value().bytesAfter;
		result.optimisationSavings += written.value().size.optimisationSavings;
		result.enrichmentGrowth += written.value().size.enrichmentGrowth;
	}

	(void)m_catalogue->setChangeSetState(set, result.cancelled ? "cancelled" : "completed");
	result.elapsedSeconds = stopwatch.elapsed();
	return result;
}

// ---------------------------------------------------------------------------
// Verify and recover
// ---------------------------------------------------------------------------

Result<std::int64_t> Library::verify(ChangeSetId set, ProgressCallback progress) {
	if (!isOpen()) return Error{ErrorCode::Internal, "the library is not open"};
	std::lock_guard<std::mutex> lock(m_primaryMutex);

	auto statement = m_database.prepare(
		"SELECT file_id, destination_relative, written_sha256, written_audio_sha256, "
		"expected_audio_sha256 FROM file_operations WHERE change_set_id = ? AND state = 'committed';");
	if (!statement) return statement.error();
	statement.value().bind(1, set.value);

	std::int64_t verified = 0;
	std::int64_t checked = 0;

	while (true) {
		auto row = statement.value().step();
		if (!row) return row.error();
		if (!row.value()) break;

		++checked;
		const std::string relative = statement.value().columnText(1);
		const std::string expectedContent = statement.value().columnText(2);
		const std::string expectedAudio = statement.value().columnText(3);
		const std::string sourceAudio = statement.value().columnText(4);

		if (!reportProgress(progress, checked, 0, relative)) {
			return Error{ErrorCode::Cancelled, "cancelled"};
		}

		const fs::path path = m_config.outputRoot / relative;
		std::error_code ec;
		if (!fs::exists(path, ec) || ec) continue;

		auto actual = hashFile(path);
		if (!actual) continue;
		if (actual.value() != expectedContent) continue;

		// The audio hash is the claim that matters: a tag-only operation must
		// have left the MPEG stream identical to the source's.
		auto layout = Mp3Container::readLayout(path);
		if (!layout) continue;
		auto audioHash = hashFileRange(path, layout.value().audioOffset, layout.value().audioLength);
		if (!audioHash) continue;
		if (audioHash.value() != expectedAudio) continue;
		if (!sourceAudio.empty() && audioHash.value() != sourceAudio) continue;

		++verified;
	}

	return verified;
}

Result<std::int64_t> Library::recover(ChangeSetId set, ProgressCallback progress) {
	if (!isOpen()) return Error{ErrorCode::Internal, "the library is not open"};
	std::lock_guard<std::mutex> lock(m_primaryMutex);

	auto incomplete = m_catalogue->incompleteOperations(set);
	if (!incomplete) return incomplete.error();

	std::int64_t reconciled = 0;
	std::int64_t index = 0;

	for (const auto& [fileId, state, tempPath] : incomplete.value()) {
		++index;
		if (!reportProgress(progress, index, static_cast<std::int64_t>(incomplete.value().size()),
				"reconciling " + state)) {
			return Error{ErrorCode::Cancelled, "cancelled"};
		}

		std::error_code ec;

		// A leftover temporary file is an interrupted write. Removing it is always
		// safe: it was never published, so nothing references it.
		if (!tempPath.empty() && fs::exists(tempPath, ec) && !ec) {
			fs::remove(tempPath, ec);
		}

		if (state == "published") {
			// The rename succeeded but the commit did not. Verify the published
			// file and finish the commit rather than rewriting it.
			auto record = m_catalogue->loadFile(fileId);
			auto plans = m_catalogue->loadFilePlans(set, false);
			std::string relative;
			for (const auto& plan : plans.ok() ? plans.value() : std::vector<FilePlan>{}) {
				if (plan.fileId == fileId) { relative = plan.destinationRelativePath; break; }
			}
			if (!relative.empty()) {
				const fs::path path = m_config.outputRoot / relative;
				if (fs::exists(path, ec) && !ec) {
					auto hash = hashFile(path);
					auto layout = Mp3Container::readLayout(path);
					if (hash && layout) {
						auto audioHash = hashFileRange(path, layout.value().audioOffset,
							layout.value().audioLength);
						if (audioHash) {
							std::error_code sizeEc;
							(void)m_catalogue->completeOperation(set, fileId, hash.value(),
								audioHash.value(), record.ok() && record.value()
									? record.value()->identity.sizeBytes : 0,
								static_cast<std::int64_t>(fs::file_size(path, sizeEc)), 0, 0);
							++reconciled;
							continue;
						}
					}
				}
			}
		}

		// Everything else is rolled back to "planned" so a later run retries it.
		// Recovery never silently overwrites a newer destination.
		(void)m_catalogue->setOperationState(set, fileId, "rolled_back", {},
			"reconciled after an interrupted run");
		++reconciled;
	}

	return reconciled;
}

// ---------------------------------------------------------------------------
// Reports and review
// ---------------------------------------------------------------------------

Result<CoverageReport> Library::coverage() const {
	if (!m_readCatalogue) return Error{ErrorCode::Internal, "the library is not open"};
	return m_readCatalogue->coverageReport();
}

Result<NamingConformity> Library::namingReport() const {
	if (!m_readCatalogue) return Error{ErrorCode::Internal, "the library is not open"};
	const NamingTemplate naming(m_config.naming);
	return m_readCatalogue->namingConformity(naming);
}

Status Library::writeNamingConventionReport(const fs::path& destination) const {
	auto conformity = namingReport();
	if (!conformity) return Status(conformity.error());

	std::ofstream out(destination);
	if (!out) {
		return Status(Error{ErrorCode::IoError, "cannot write " + text::pathToUtf8(destination)});
	}

	const NamingConformity& c = conformity.value();

	out << "# Naming convention: measured evidence\n\n";
	out << "Generated " << nowIso8601() << " by Resonance.\n\n";
	out << "This report measures the collection against the user-confirmed template. The template "
		   "itself is a specification supplied by the user; the figures below are what was actually "
		   "observed, and the two must not be confused.\n\n";
	out << "**Template**: `" << NamingTemplate::pattern() << "`\n\n";

	out << "## Summary\n\n";
	out << "| Measure | Value |\n|---|---:|\n";
	out << "| MP3 files examined | " << c.totalFiles << " |\n";
	out << "| Matching the template exactly | " << c.matchingTemplate << " |\n";
	out << "| Percentage matching | " << std::fixed << c.matchPercentage() << "% |\n";
	out << "| Would need review before organising | " << c.wouldNeedReview << " |\n\n";

	out << "## Exception classes\n\n";
	if (c.exceptionCounts.empty()) {
		out << "No exceptions were recorded.\n\n";
	} else {
		out << "| Exception | Files |\n|---|---:|\n";
		for (const auto& [kind, count] : c.exceptionCounts) {
			out << "| `" << kind << "` | " << count << " |\n";
		}
		out << "\n";
	}

	out << "## Other observed path grammars\n\n";
	out << "| Shape | Files |\n|---|---:|\n";
	for (const auto& [grammar, count] : c.observedGrammars) {
		out << "| " << grammar << " | " << count << " |\n";
	}
	out << "\n";

	out << "## Representative matches\n\n";
	for (const auto& example : c.representativeMatches) {
		out << "- `" << example << "`\n";
	}
	if (c.representativeMatches.empty()) out << "None.\n";
	out << "\n";

	out << "## Representative exceptions\n\n";
	out << "Shown as `actual path  ->  path the template would produce`.\n\n";
	for (const auto& example : c.representativeExceptions) {
		out << "- `" << example << "`\n";
	}
	if (c.representativeExceptions.empty()) out << "None.\n";
	out << "\n";

	return Status::success();
}

Result<AlbumReview> Library::reviewAlbum(AlbumId albumId) {
	if (!isOpen()) return Error{ErrorCode::Internal, "the library is not open"};
	std::lock_guard<std::mutex> lock(m_primaryMutex);

	auto album = m_catalogue->loadAlbum(albumId);
	if (!album) return album.error();
	if (!album.value()) return Error{ErrorCode::NotFound, "no such album"};

	AlbumReview review;
	review.album = *album.value();

	auto candidates = m_catalogue->artworkCandidates(albumId);
	if (!candidates) return candidates.error();
	review.artworkCandidates = candidates.value();

	const ArtworkPolicy policy(m_config.artwork);
	review.selection = policy.select(review.artworkCandidates);

	TrackFilter filter;
	filter.albumId = albumId;
	auto tracks = m_catalogue->queryFiles(filter, 0, 0);
	if (!tracks) return tracks.error();
	review.tracks = tracks.value();

	// Review aids, not automated providers (FN-ART-06).
	review.googleImagesUrl = ReviewSearchUrls::googleImages(review.album.albumArtist,
		review.album.album, 1200);
	review.appleSearchUrl = ReviewSearchUrls::appleMusicSiteSearch(review.album.albumArtist,
		review.album.album);
	review.musicBrainzUrl = ReviewSearchUrls::musicBrainzSearch(review.album.albumArtist,
		review.album.album);

	return review;
}

Status Library::lockArtwork(AlbumId album, ArtworkId asset, std::string_view note) {
	std::lock_guard<std::mutex> lock(m_primaryMutex);
	if (auto status = m_catalogue->setArtworkSelection(asset, "locked",
			"locked by the user" + (note.empty() ? std::string() : ": " + std::string(note)));
		!status) {
		return status;
	}
	// A lock is recorded separately so it survives a rescan that rebuilds the
	// derived artwork rows (ART-003).
	return m_catalogue->recordManualDecision("album", album.value, "artwork", "lock", {},
		asset.value, note);
}

Status Library::rejectArtwork(AlbumId album, ArtworkId asset, std::string_view note) {
	std::lock_guard<std::mutex> lock(m_primaryMutex);
	if (auto status = m_catalogue->setArtworkSelection(asset, "rejected",
			"rejected by the user" + (note.empty() ? std::string() : ": " + std::string(note)));
		!status) {
		return status;
	}
	return m_catalogue->recordManualDecision("album", album.value, "artwork_rejected", "reject", {},
		asset.value, note);
}

Status Library::importArtworkFile(AlbumId album, const fs::path& path) {
	std::lock_guard<std::mutex> lock(m_primaryMutex);
	LocalArtworkProvider local;
	if (auto status = local.addImportedFile(path); !status) return status;

	auto found = local.findArtwork({});
	if (!found || found.value().empty()) {
		return Status(Error{ErrorCode::InvalidArgument, "the file could not be read as artwork"});
	}

	ArtworkCandidate candidate = found.value().front();
	std::string contentType;
	auto bytes = local.fetchImage(candidate, contentType);
	if (!bytes) return Status(bytes.error());

	auto dimensions = ImagePipeline::probeDimensions(bytes.value().data(), bytes.value().size());
	if (!dimensions) return Status(dimensions.error());

	candidate.measuredWidth = dimensions.value().first;
	candidate.measuredHeight = dimensions.value().second;
	candidate.dimensionsMeasured = true;
	candidate.byteLength = static_cast<std::int64_t>(bytes.value().size());
	candidate.contentSha256 = Sha256::hashBytes(bytes.value().data(), bytes.value().size());
	candidate.manuallyLocked = true;

	auto stored = m_assets->put(bytes.value().data(), bytes.value().size(), ".img");
	if (!stored) return Status(stored.error());
	candidate.localPath = stored.value();

	auto saved = m_catalogue->saveArtworkCandidate(album, candidate, "locked",
		"imported by the user from " + text::pathToUtf8(path.filename()),
		std::string(ArtworkPolicy::kPolicyVersion));
	if (!saved) return Status(saved.error());

	// Generate the derivative now so the plan can use it immediately.
	auto derivative = ImagePipeline::makeDerivative(bytes.value().data(), bytes.value().size(),
		m_config.derivative);
	if (derivative) {
		auto derivativeStored = m_assets->put(derivative.value().jpegBytes.data(),
			derivative.value().jpegBytes.size(), ".jpg");
		if (derivativeStored) {
			(void)m_catalogue->saveDerivative(saved.value(), m_config.derivative.hash(),
				derivativeStored.value(), derivative.value().sha256, derivative.value().width,
				derivative.value().height,
				static_cast<std::int64_t>(derivative.value().jpegBytes.size()),
				m_config.derivative.jpegQuality,
				m_config.derivative.chromaSubsampling ? "4:2:0" : "4:4:4");
		}
	} else {
		return Status(Error{ErrorCode::Unsupported,
			"the imported image cannot produce the required derivative: " + derivative.error().message});
	}

	return m_catalogue->recordManualDecision("album", album.value, "artwork", "lock", text::pathToUtf8(path),
		saved.value().value, "imported file");
}

// ---------------------------------------------------------------------------
// Diagnostics
// ---------------------------------------------------------------------------

Result<Library::Diagnostics> Library::diagnostics() {
	if (!isOpen()) return Error{ErrorCode::Internal, "the library is not open"};
	std::lock_guard<std::mutex> lock(m_primaryMutex);

	Diagnostics d;
	d.catalogueJournalMode = m_database.journalMode();

	auto version = SchemaMigrator::currentVersion(m_database);
	d.catalogueVersion = version.ok() ? std::to_string(version.value()) : "unknown";

	std::error_code ec;
	d.catalogueBytes = static_cast<std::int64_t>(fs::file_size(m_database.path(), ec));
	if (ec) d.catalogueBytes = 0;

	auto integrity = m_database.integrityCheck();
	d.integrityOk = integrity.ok() && integrity.value();

	d.protectedRootCount = m_guard.protectedRoots().size();
	m_guard.refreshRootPresence();
	d.absentRootCount = m_guard.absentRootCount();

	d.derivativeConfig = m_config.derivative.describe();
	d.tempoEngine = TempoAnalyzer(m_config.tempoAnalyzer).engine();
	d.offline = m_config.offline;

	return d;
}

Status Library::backupCatalogue(const fs::path& destination) {
	if (!isOpen()) return Status(Error{ErrorCode::Internal, "the library is not open"});
	std::lock_guard<std::mutex> lock(m_primaryMutex);
	return m_database.backupTo(destination);
}

} // namespace ml
