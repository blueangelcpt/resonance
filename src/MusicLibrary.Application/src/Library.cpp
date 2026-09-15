// SPDX-License-Identifier: GPL-3.0-or-later
#include "mlapp/Library.hpp"
#include "mlcore/Text.hpp"
#include "mlinfra/Hashing.hpp"
#include "mlinfra/Mp3Container.hpp"
#include "mlinfra/TagReader.hpp"
#include "mlinfra/TagWriter.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <set>
#include <thread>

namespace fs = std::filesystem;

namespace ml {

namespace {

/// A stopwatch that reports elapsed seconds.
class Stopwatch {
public:
	double elapsed() const {
		return std::chrono::duration<double>(std::chrono::steady_clock::now() - m_start).count();
	}

private:
	std::chrono::steady_clock::time_point m_start = std::chrono::steady_clock::now();
};

bool isMp3(const fs::path& path) {
	std::string extension = text::pathToUtf8(path.extension());
	std::transform(extension.begin(), extension.end(), extension.begin(),
		[](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	return extension == ".mp3";
}

/// Turns an absolute path into a root-relative one with forward slashes, so the
/// catalogue is portable between Windows and POSIX.
std::string relativePathOf(const fs::path& path, const fs::path& root) {
	std::error_code ec;
	fs::path relative = fs::relative(path, root, ec);
	if (ec) relative = path.filename();

	// generic_u8string(), not generic_string(): both force forward slashes,
	// but generic_string() converts through the native encoding the same
	// unsafe way string() does (see pathToUtf8's comment).
	const auto u8 = relative.generic_u8string();
	return std::string(u8.begin(), u8.end());
}

} // namespace

fs::path LibraryConfig::defaultDataDirectory() {
	// OS-standard per-user data directories (FRD section 16). The catalogue must
	// survive an application update, so it never lives beside the executable.
#ifdef _WIN32
	if (const char* appData = std::getenv("LOCALAPPDATA")) {
		return fs::path(appData) / "Resonance";
	}
	return fs::current_path() / "resonance-data";
#elif defined(__APPLE__)
	if (const char* home = std::getenv("HOME")) {
		return fs::path(home) / "Library" / "Application Support" / "Resonance";
	}
	return fs::current_path() / "resonance-data";
#else
	if (const char* dataHome = std::getenv("XDG_DATA_HOME")) {
		return fs::path(dataHome) / "resonance";
	}
	if (const char* home = std::getenv("HOME")) {
		return fs::path(home) / ".local" / "share" / "resonance";
	}
	return fs::current_path() / "resonance-data";
#endif
}

std::string LibraryConfig::hash() const {
	return configHash({
		"naming.windows=" + std::string(naming.enforceWindowsRules ? "1" : "0"),
		"naming.maxPath=" + std::to_string(naming.maxPathLength),
		"naming.trackDigits=" + std::to_string(naming.trackDigits),
		"privacy.profile=" + std::string(toString(privacy.profile)),
		"privacy.id3v1Comment=" + std::string(privacy.clearId3v1Comment ? "1" : "0"),
		"privacy.scanFreeText=" + std::string(privacy.scanFreeTextForPersonalData ? "1" : "0"),
		"gain.replaygain=" + std::string(gain.removeReplayGain ? "1" : "0"),
		"gain.soundcheck=" + std::string(gain.removeSoundCheck ? "1" : "0"),
		"gain.rva=" + std::string(gain.removeRelativeVolume ? "1" : "0"),
		"artwork.minAdequate=" + std::to_string(artwork.minimumAdequateSize),
		"derivative=" + derivative.hash(),
		"tempo.preserve=" + std::string(tempo.preserveExistingTrusted ? "1" : "0"),
		"lyrics.preserve=" + std::string(lyrics.preserveExisting ? "1" : "0"),
		"lyrics.language=" + lyrics.defaultLanguage,
		"padding=" + std::to_string(padding.targetPaddingBytes),
		"policy_version=" + std::string(ArtworkPolicy::kPolicyVersion),
	});
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

Library::Library() = default;
Library::~Library() = default;

Status Library::open(LibraryConfig config) {
	m_config = std::move(config);

	if (m_config.dataDirectory.empty()) {
		m_config.dataDirectory = LibraryConfig::defaultDataDirectory();
	}

	std::error_code ec;
	fs::create_directories(m_config.dataDirectory, ec);
	if (ec && !fs::is_directory(m_config.dataDirectory)) {
		return Status(Error{ErrorCode::IoError,
			"cannot create the data directory " + text::pathToUtf8(m_config.dataDirectory) + ": " + ec.message()});
	}

	// --- Protected roots first ---------------------------------------------
	// Registering these before anything else means a later output-root
	// registration is checked against them rather than the other way round.
	for (const auto& root : m_config.sourceRoots) {
		if (auto status = m_guard.addProtectedRoot(root); !status) return status;
	}

	// The catalogue, assets, caches and logs must live outside the source roots.
	if (m_guard.isInsideProtectedRoot(m_config.dataDirectory)) {
		return Status(Error{ErrorCode::ProtectedRootViolation,
			"the data directory " + text::pathToUtf8(m_config.dataDirectory)
				+ " is inside a protected source root; the catalogue must live outside the music"});
	}

	if (!m_config.outputRoot.empty()) {
		fs::create_directories(m_config.outputRoot, ec);
		if (auto status = m_guard.addOutputRoot(m_config.outputRoot); !status) return status;
	}
	// The staging directory is inside the data directory, never beside the music.
	if (auto status = m_guard.addOutputRoot(m_config.dataDirectory); !status) return status;

	m_guard.refreshRootPresence();

	// --- Catalogue -----------------------------------------------------------
	auto database = Database::open(m_config.dataDirectory / "catalogue.mlcat");
	if (!database) return Status(database.error());
	m_database = std::move(database.value());

	if (auto status = SchemaMigrator::migrate(m_database); !status) return status;

	m_catalogue = std::make_unique<Catalogue>(m_database);

	// A second, read-only connection to the same file (WAL keeps this safe
	// concurrently with the writer above). Every catalogue read that can happen
	// from the UI thread goes through this one instead, because the primary
	// connection is opened SQLITE_OPEN_NOMUTEX: it may only ever be touched by
	// one thread at a time, and background commands (scan, enrichment, export)
	// run it from a worker thread while the window is still open and responding
	// to clicks. Reading through the primary connection from the UI thread at
	// the same time is a data race that can crash the process outright.
	DatabaseOpenOptions readOptions;
	readOptions.readOnly = true;
	readOptions.createIfMissing = false;
	auto readDatabase = Database::open(m_config.dataDirectory / "catalogue.mlcat", readOptions);
	if (!readDatabase) return Status(readDatabase.error());
	m_readDatabase = std::move(readDatabase.value());
	m_readCatalogue = std::make_unique<Catalogue>(m_readDatabase);

	for (const auto& root : m_guard.protectedRoots()) {
		auto id = m_catalogue->upsertRoot(root.path, root.resolvedPath, "protected_source", root.label,
			root.volumeIdentity, root.deviceId);
		if (!id) return Status(id.error());
	}
	if (!m_config.outputRoot.empty()) {
		auto id = m_catalogue->upsertRoot(m_config.outputRoot, m_config.outputRoot, "output", "output",
			{}, 0);
		if (!id) return Status(id.error());
	}

	// --- Assets and providers -------------------------------------------------
	m_assets = std::make_unique<AssetStore>(m_config.dataDirectory / "assets");

	m_http = std::make_unique<HttpClient>();
	m_http->setOfflineMode(m_config.offline);
	m_itunes = std::make_unique<ITunesProvider>(*m_http, m_config.appleStorefront);
	m_coverArtArchive = std::make_unique<CoverArtArchiveProvider>(*m_http);
	m_musicBrainz = std::make_unique<MusicBrainzProvider>(*m_http);
	m_lrclib = std::make_unique<LrclibProvider>(*m_http);

	(void)m_catalogue->setSetting("config_hash", m_config.hash());
	(void)m_catalogue->setSetting("last_opened", nowIso8601());

	return Status::success();
}

bool Library::reportProgress(const ProgressCallback& progress, std::int64_t done, std::int64_t total,
	const std::string& message) {
	if (cancelRequested()) return false;
	if (!progress) return true;
	return progress(done, total, message);
}

// ---------------------------------------------------------------------------
// Scan
// ---------------------------------------------------------------------------

Result<ScanResult> Library::scan(ProgressCallback progress) {
	if (!isOpen()) return Error{ErrorCode::Internal, "the library is not open"};
	std::lock_guard<std::mutex> lock(m_primaryMutex);

	Stopwatch stopwatch;
	ScanResult result;

	m_guard.refreshRootPresence();
	const auto roots = m_guard.protectedRoots();
	if (roots.empty()) {
		return Error{ErrorCode::InvalidArgument, "no source root is configured"};
	}

	for (const auto& root : roots) {
		if (!root.currentlyPresent) {
			// FN-SCAN-04: an absent root is reported, never treated as deletion
			// of everything it contained.
			result.warnings.push_back("source root \"" + text::pathToUtf8(root.path)
				+ "\" is not currently reachable; its catalogued files are left untouched rather than "
				  "being recorded as deleted");
			continue;
		}

		auto rootId = m_catalogue->findRoot(root.path);
		if (!rootId) return rootId.error();
		if (!rootId.value()) {
			return Error{ErrorCode::Internal, "source root is not registered in the catalogue"};
		}

		auto scanRun = m_catalogue->beginScan(*rootId.value(), true);
		if (!scanRun) return scanRun.error();
		result.runId = scanRun.value();

		// Count first so progress is meaningful. On a cold cache this costs one
		// extra directory traversal, which is cheap next to reading every tag.
		std::int64_t total = 0;
		{
			std::error_code ec;
			fs::recursive_directory_iterator it(root.resolvedPath,
				fs::directory_options::skip_permission_denied, ec);
			const fs::recursive_directory_iterator end;
			for (; it != end; it.increment(ec)) {
				if (ec) { ec.clear(); continue; }
				if (it->is_regular_file(ec) && isMp3(it->path())) ++total;
			}
		}

		std::error_code ec;
		fs::recursive_directory_iterator it(root.resolvedPath,
			fs::directory_options::skip_permission_denied, ec);
		const fs::recursive_directory_iterator end;

		if (ec) {
			(void)m_catalogue->finishScan(result.runId, "failed", 0, 0, 0, 0, ec.message());
			return Error{ErrorCode::IoError, "cannot walk " + text::pathToUtf8(root.resolvedPath)};
		}

		// One transaction per batch: small transactions, as the FRD requires,
		// but not one per file across 70,000 files.
		constexpr std::int64_t kBatchSize = 200;
		std::optional<Transaction> batch;
		std::int64_t inBatch = 0;

		const auto commitBatch = [&]() -> Status {
			if (!batch) return Status::success();
			auto status = batch->commit();
			batch.reset();
			inBatch = 0;
			return status;
		};

		for (; it != end; it.increment(ec)) {
			if (ec) {
				ec.clear();
				continue;
			}
			if (!it->is_regular_file(ec) || ec) { ec.clear(); continue; }

			const fs::path& path = it->path();
			if (!isMp3(path)) continue;

			++result.filesSeen;

			if (!batch) {
				auto begun = m_database.begin(Transaction::Kind::Immediate);
				if (!begun) return begun.error();
				batch.emplace(std::move(begun.value()));
			}

			const std::string relative = relativePathOf(path, root.resolvedPath);

			if (!reportProgress(progress, result.filesSeen, total, relative)) {
				(void)commitBatch();
				(void)m_catalogue->finishScan(result.runId, "cancelled", result.filesSeen,
					result.filesAdded, result.filesChanged, result.filesUnreadable);
				result.cancelled = true;
				result.elapsedSeconds = stopwatch.elapsed();
				return result;
			}

			// --- Change detection ------------------------------------------
			const auto identity = PathGuard::identityOf(path);
			auto existing = m_catalogue->findFile(*rootId.value(), relative);
			if (!existing) return existing.error();

			if (existing.value() && identity
				&& existing.value()->identity.looksUnchanged(*identity)
				&& existing.value()->readStatus == "ok"
				&& !existing.value()->contentSha256.empty()) {
				// Size and mtime match a record that was read successfully before.
				// FN-SCAN-02: this is grounds for skipping a rescan, but never for
				// skipping revalidation before a write.
				++result.filesUnchanged;
				if (++inBatch >= kBatchSize) {
					if (auto status = commitBatch(); !status) return status.error();
				}
				continue;
			}

			// --- Read ---------------------------------------------------------
			TagReadOptions options;
			options.hashContent = true;
			options.hashAudio = true;
			options.loadPictureBytes = false;
			options.retainFramePayloads = false;   // Bulk scan: payloads on demand.

			auto read = TagReader::read(path, options);
			if (!read) {
				++result.filesUnreadable;
				(void)m_catalogue->markFileUnreadable(*rootId.value(), result.runId, relative,
					"unreadable", read.error().message);
				if (++inBatch >= kBatchSize) {
					if (auto status = commitBatch(); !status) return status.error();
				}
				continue;
			}

			const TagSnapshot& snapshot = read.value().snapshot;

			FileRecord record;
			record.rootId = *rootId.value();
			record.relativePath = relative;
			// Sliced from the already-UTF-8 `relative` string rather than round
			// tripped back through fs::path(relative).…: on Windows, constructing
			// a path from a narrow string and reading one back both go through
			// the native ANSI codepage, which is exactly the unsafe conversion
			// pathToUtf8() exists to avoid (see its comment). fileName/extension
			// come straight from `path`, which has never left wide-string form.
			{
				const std::size_t slash = relative.find_last_of('/');
				record.relativeDirectory = (slash == std::string::npos)
					? std::string() : relative.substr(0, slash);
			}
			record.fileName = text::pathToUtf8(path.filename());
			record.extension = text::pathToUtf8(path.extension());
			if (identity) record.identity = *identity;
			record.audio = read.value().layout.audio;
			record.contentSha256 = snapshot.contentSha256;
			record.audioSha256 = snapshot.audioSha256;
			record.readStatus = read.value().layout.valid ? "ok" : "malformed";
			if (!read.value().layout.valid) {
				record.readError = "no confirmed MPEG frame sync was found";
				++result.filesUnreadable;
			}

			record.title = snapshot.title();
			record.artist = snapshot.artist();
			record.albumArtist = snapshot.albumArtist();
			record.album = snapshot.album();
			record.genre = snapshot.genre();
			record.date = snapshot.date();
			record.trackNumber = snapshot.trackNumber();
			record.discNumber = snapshot.discNumber();
			record.bpm = snapshot.bpm();
			record.primaryContainer = std::string(toString(snapshot.primaryContainer));

			if (const EmbeddedPicture* cover = snapshot.frontCover()) {
				record.hasArtwork = true;
				record.artworkWidth = cover->width;
				record.artworkHeight = cover->height;
			}
			record.hasLyrics = !snapshot.lyrics().empty();

			// Gain and privacy findings are evaluated here so the filter chips in
			// the track table work straight after a scan, with no second pass.
			const GainPolicy gainPolicy(m_config.gain);
			const GainPreview gainPreview = gainPolicy.evaluate(snapshot);
			record.hasGainFields = gainPreview.removeCount > 0 || !gainPreview.exceptions.empty();

			const PrivacyPolicy privacyPolicy(m_config.privacy);
			const PrivacyPreview privacyPreview = privacyPolicy.evaluate(snapshot);
			record.hasPrivacyFindings = privacyPreview.removeCount > 0 || privacyPreview.reviewCount > 0;

			auto fileId = m_catalogue->upsertFile(*rootId.value(), result.runId, record);
			if (!fileId) return fileId.error();

			// The full frame inventory, including uninterpreted payloads, needs
			// the payloads; re-read only for files that will be written later is
			// wasteful, so the inventory is stored without them during a bulk scan
			// and the writer re-reads the file it is about to copy.
			auto snapshotId = m_catalogue->saveSnapshot(fileId.value(), snapshot, "observed");
			if (!snapshotId) return snapshotId.error();

			if (existing.value()) ++result.filesChanged;
			else ++result.filesAdded;

			result.bytesRead += record.identity.sizeBytes;

			if (++inBatch >= kBatchSize) {
				if (auto status = commitBatch(); !status) return status.error();
			}
		}

		if (auto status = commitBatch(); !status) return status.error();

		(void)m_catalogue->finishScan(result.runId, "completed", result.filesSeen, result.filesAdded,
			result.filesChanged, result.filesUnreadable);
	}

	result.elapsedSeconds = stopwatch.elapsed();
	return result;
}

// ---------------------------------------------------------------------------
// Albums
// ---------------------------------------------------------------------------

Result<std::int64_t> Library::resolveAlbums(ProgressCallback progress) {
	if (!isOpen()) return Error{ErrorCode::Internal, "the library is not open"};
	std::lock_guard<std::mutex> lock(m_primaryMutex);

	TrackFilter filter;
	filter.readStatus = "ok";

	auto total = m_catalogue->countFiles(filter);
	if (!total) return total.error();

	std::vector<GroupingInput> inputs;
	inputs.reserve(static_cast<std::size_t>(total.value()));

	constexpr std::int64_t kPage = 2000;
	for (std::int64_t offset = 0; offset < total.value(); offset += kPage) {
		auto page = m_catalogue->queryFiles(filter, kPage, offset);
		if (!page) return page.error();

		for (const auto& record : page.value()) {
			GroupingInput input;
			input.fileId = record.id;
			input.relativeDirectory = record.relativeDirectory;
			input.album = record.album;
			input.albumArtist = record.albumArtist;
			input.artist = record.artist;
			input.date = record.date;
			input.trackNumber = record.trackNumber;
			input.discNumber = record.discNumber;
			input.durationMs = record.audio.durationMs;
			inputs.push_back(std::move(input));
		}

		if (!reportProgress(progress, offset, total.value(), "grouping albums")) {
			return Error{ErrorCode::Cancelled, "cancelled"};
		}
	}

	const AlbumResolverCore resolver(m_config.grouping);
	const auto albums = resolver.group(inputs);

	if (auto status = m_catalogue->replaceAlbums(albums); !status) return status.error();
	return static_cast<std::int64_t>(albums.size());
}

// ---------------------------------------------------------------------------
// Sample
// ---------------------------------------------------------------------------

Result<std::int64_t> Library::createSample(const fs::path& destination, int albumCount, int trackLimit,
	ProgressCallback progress) {
	if (!isOpen()) return Error{ErrorCode::Internal, "the library is not open"};
	std::lock_guard<std::mutex> lock(m_primaryMutex);

	// The sample destination must be outside every protected root: the whole
	// point is that transformation runs never touch the original collection.
	if (m_guard.isInsideProtectedRoot(destination)) {
		return Error{ErrorCode::ProtectedRootViolation,
			"the sample destination is inside a protected source root"};
	}

	PathGuard sampleGuard;
	for (const auto& root : m_guard.protectedRoots()) {
		if (auto status = sampleGuard.addProtectedRoot(root.path); !status) return status.error();
	}
	std::error_code ec;
	fs::create_directories(destination, ec);
	if (auto status = sampleGuard.addOutputRoot(destination); !status) return status.error();

	auto albums = m_catalogue->listAlbums(false, albumCount > 0 ? albumCount : 0, 0);
	if (!albums) return albums.error();

	std::int64_t copied = 0;
	std::int64_t attempted = 0;

	for (const auto& album : albums.value()) {
		int fromThisAlbum = 0;
		for (FileId fileId : album.files) {
			if (trackLimit > 0 && fromThisAlbum >= trackLimit) break;

			auto record = m_catalogue->loadFile(fileId);
			if (!record || !record.value()) continue;

			auto roots = m_catalogue->listRoots("protected_source");
			if (!roots || roots.value().empty()) break;

			// Resolve the source path from its root.
			fs::path sourcePath;
			for (const auto& root : m_guard.protectedRoots()) {
				const fs::path candidate = root.resolvedPath / record.value()->relativePath;
				if (fs::exists(candidate, ec) && !ec) {
					sourcePath = candidate;
					break;
				}
			}
			if (sourcePath.empty()) continue;

			++attempted;

			// Mirror the source layout inside the sample so the copy is
			// recognisable, and make an ordinary byte copy -- never a hardlink.
			const fs::path target = destination / record.value()->relativePath;
			const GuardDecision decision = sampleGuard.checkWrite(target);
			if (!decision.allowed()) continue;

			fs::create_directories(target.parent_path(), ec);
			if (ec) { ec.clear(); continue; }
			if (fs::exists(target, ec)) continue;

			fs::copy_file(sourcePath, target, fs::copy_options::none, ec);
			if (ec) { ec.clear(); continue; }

			// Verify the copy by hash before counting it.
			auto sourceHash = hashFile(sourcePath);
			auto targetHash = hashFile(target);
			if (!sourceHash || !targetHash || sourceHash.value() != targetHash.value()) {
				fs::remove(target, ec);
				continue;
			}

			++copied;
			++fromThisAlbum;

			if (!reportProgress(progress, copied, attempted, record.value()->relativePath)) {
				return Error{ErrorCode::Cancelled, "cancelled"};
			}
		}
	}

	return copied;
}

// ---------------------------------------------------------------------------
// Tempo
// ---------------------------------------------------------------------------

Result<std::int64_t> Library::analyseTempo(ProgressCallback progress) {
	if (!isOpen()) return Error{ErrorCode::Internal, "the library is not open"};
	std::lock_guard<std::mutex> lock(m_primaryMutex);

	TrackFilter filter;
	filter.readStatus = "ok";
	auto total = m_catalogue->countFiles(filter);
	if (!total) return total.error();

	const TempoAnalyzer analyzer(m_config.tempoAnalyzer);
	const TempoPolicy policy(m_config.tempo);

	// Bounded worker pool. Each worker decodes and analyses; the catalogue write
	// happens on this thread, because SQLite has one writer.
	unsigned workers = m_config.workerCount > 0
		? static_cast<unsigned>(m_config.workerCount)
		: std::max(1u, std::min(4u, std::thread::hardware_concurrency()));

	std::int64_t analysed = 0;
	constexpr std::int64_t kPage = 256;

	for (std::int64_t offset = 0; offset < total.value(); offset += kPage) {
		auto page = m_catalogue->queryFiles(filter, kPage, offset);
		if (!page) return page.error();
		if (page.value().empty()) break;

		struct Job {
			FileRecord record;
			fs::path path;
			TempoAnalysis analysis;
			TempoDecision decision;
			bool done = false;
		};
		std::vector<Job> jobs;

		for (const auto& record : page.value()) {
			// Idempotence: a completed analysis under the same engine and settings
			// is not repeated (JOB-001).
			auto existing = m_catalogue->loadTempo(record.id);
			if (existing && existing.value()) continue;

			fs::path sourcePath;
			std::error_code ec;
			for (const auto& root : m_guard.protectedRoots()) {
				const fs::path candidate = root.resolvedPath / record.relativePath;
				if (fs::exists(candidate, ec) && !ec) { sourcePath = candidate; break; }
			}
			if (sourcePath.empty()) continue;

			Job job;
			job.record = record;
			job.path = sourcePath;
			jobs.push_back(std::move(job));
		}

		if (jobs.empty()) continue;

		std::atomic<std::size_t> next{0};
		const auto worker = [&]() {
			while (true) {
				const std::size_t index = next.fetch_add(1, std::memory_order_relaxed);
				if (index >= jobs.size()) return;
				if (cancelRequested()) return;

				Job& job = jobs[index];
				auto audio = Mp3Decoder::decode(job.path);
				if (!audio) {
					job.decision.kind = TempoDecisionKind::Failed;
					job.decision.reason = audio.error().message;
					job.done = true;
					continue;
				}
				job.analysis = analyzer.analyse(audio.value());
				job.decision = policy.decide(job.analysis, job.record.bpm, audio.value().durationMs);
				job.done = true;
			}
		};

		std::vector<std::thread> pool;
		pool.reserve(workers);
		for (unsigned i = 0; i < workers; ++i) pool.emplace_back(worker);
		for (auto& thread : pool) thread.join();

		if (cancelRequested()) return Error{ErrorCode::Cancelled, "cancelled"};

		auto transaction = m_database.begin(Transaction::Kind::Immediate);
		if (!transaction) return transaction.error();
		for (const auto& job : jobs) {
			if (!job.done) continue;
			if (auto status = m_catalogue->saveTempo(job.record.id, job.analysis, job.decision);
				!status) {
				return status.error();
			}
			++analysed;
		}
		if (auto status = transaction.value().commit(); !status) return status.error();

		if (!reportProgress(progress, offset + static_cast<std::int64_t>(page.value().size()),
				total.value(), "analysing tempo")) {
			return Error{ErrorCode::Cancelled, "cancelled"};
		}
	}

	return analysed;
}

} // namespace ml
