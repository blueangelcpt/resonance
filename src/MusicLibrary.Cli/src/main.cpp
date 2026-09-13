// SPDX-License-Identifier: GPL-3.0-or-later
// FN-CLI-01 / FN-CLI-02: the command line application.
//
// The CLI links no Qt Widgets and requires no display server. Analysis and
// planning commands are structurally incapable of writing music: they call
// Library methods that have no writer. Every modifying command requires an
// explicit --output.
#include "mlapp/Library.hpp"
#include "mlcore/Text.hpp"
#include "mlinfra/Providers.hpp"

#include <mlversion/Version.hpp>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace ml;

namespace {

// ---------------------------------------------------------------------------
// Exit codes. Meaningful, so a script can act on them (FRD section 11).
// ---------------------------------------------------------------------------
constexpr int kExitOk = 0;
constexpr int kExitUsage = 2;
constexpr int kExitConfiguration = 3;
constexpr int kExitRuntimeError = 4;
constexpr int kExitReviewRequired = 5;
constexpr int kExitCancelled = 130;

struct Options {
	std::string command;
	std::vector<fs::path> sources;
	fs::path output;
	fs::path data;
	fs::path destination;
	std::string format = "text";       // text | json | csv
	std::string privacyProfile = "selective";
	std::string storefront = "za";
	bool offline = false;
	bool quiet = false;
	bool yes = false;
	int workers = 0;
	int albumLimit = 0;
	int trackLimit = 0;
	std::int64_t changeSet = 0;
	std::int64_t albumId = 0;
	std::int64_t fileId = 0;
	bool clearId3v1Comment = false;
	bool help = false;
	bool version = false;
};

void printUsage() {
	std::cout <<
R"(Resonance -- MP3 library catalogue, enrichment and safe organisation

USAGE
  resonance <command> [options]

COMMANDS
  scan             Read-only inventory of the source roots. Writes nothing to
                   the music.
  albums           Rebuild provisional album grouping from the catalogue.
  sample           Copy an independent sample to a destination outside every
                   protected root, for testing.
  analyse          Local analysis: tempo. No network, no writes to music.
  artwork          Fetch and rank artwork candidates. Degrades cleanly offline.
  lyrics           Look up lyrics. Distinguishes unavailable, instrumental and
                   failed lookups.
  plan             Build a change plan. Cannot modify music.
  review           Show albums and files that need a human decision.
  export-copies    Execute a change plan, writing copies to the output root.
  verify           Re-read written outputs and verify them against their plans.
  resume           Reconcile operations interrupted by a crash.
  report           Coverage, naming conformity and duplicate reports.
  diagnostics      Catalogue and engine status.

OPTIONS
  --source <path>        Protected source root. Repeatable. Never written to.
  --output <path>        Output root for copies. Required by modifying commands.
  --data <path>          Catalogue and cache directory. Defaults to the
                         platform per-user data directory.
  --destination <path>   Destination for 'sample' and for report files.
  --format <text|json|csv>  Report format. Default text.
  --privacy <selective|strict|report>   Privacy profile. Default selective.
  --storefront <code>    Apple storefront. Default za.
  --clear-id3v1-comment  Blank the trailing ID3v1 comment field.
  --offline              Disable all network access.
  --workers <n>          Analysis workers. Default: from the hardware.
  --albums <n>           Album limit for 'sample'.
  --tracks <n>           Per-album track limit for 'sample'.
  --change-set <id>      Change set for export-copies, verify and resume.
  --album <id>           Album id for 'review'.
  --file <id>            File id for 'review'.
  --quiet                Suppress progress output.
  --yes                  Do not prompt for confirmation.
  -h, --help             This text.
  --version              Version and build information.

EXIT CODES
  0  success
  2  usage error
  3  configuration error (for example an output root inside a source root)
  4  runtime error
  5  completed, but items need review before they can be written
  130 cancelled

SAFETY
  Source roots are read-only to every operation. The writer refuses any
  destination that resolves inside a source root, including through a symlink
  or junction. Version 1 always exports copies; it never modifies the original
  collection.
)";
}

bool parseArguments(int argc, char** argv, Options& options, std::string& error) {
	if (argc < 2) {
		options.help = true;
		return true;
	}

	int start = 1;
	const std::string first = argv[1];
	if (!first.empty() && first[0] != '-') {
		options.command = first;
		start = 2;
	}

	for (int i = start; i < argc; ++i) {
		const std::string argument = argv[i];
		const auto next = [&](const char* name) -> const char* {
			if (i + 1 >= argc) {
				error = std::string("option ") + name + " needs a value";
				return nullptr;
			}
			return argv[++i];
		};

		if (argument == "-h" || argument == "--help") {
			options.help = true;
		} else if (argument == "--version") {
			options.version = true;
		} else if (argument == "--source") {
			const char* value = next("--source");
			if (!value) return false;
			options.sources.emplace_back(value);
		} else if (argument == "--output") {
			const char* value = next("--output");
			if (!value) return false;
			options.output = value;
		} else if (argument == "--data") {
			const char* value = next("--data");
			if (!value) return false;
			options.data = value;
		} else if (argument == "--destination") {
			const char* value = next("--destination");
			if (!value) return false;
			options.destination = value;
		} else if (argument == "--format") {
			const char* value = next("--format");
			if (!value) return false;
			options.format = value;
		} else if (argument == "--privacy") {
			const char* value = next("--privacy");
			if (!value) return false;
			options.privacyProfile = value;
		} else if (argument == "--storefront") {
			const char* value = next("--storefront");
			if (!value) return false;
			options.storefront = value;
		} else if (argument == "--clear-id3v1-comment") {
			options.clearId3v1Comment = true;
		} else if (argument == "--offline") {
			options.offline = true;
		} else if (argument == "--quiet") {
			options.quiet = true;
		} else if (argument == "--yes") {
			options.yes = true;
		} else if (argument == "--workers") {
			const char* value = next("--workers");
			if (!value) return false;
			options.workers = std::atoi(value);
		} else if (argument == "--albums") {
			const char* value = next("--albums");
			if (!value) return false;
			options.albumLimit = std::atoi(value);
		} else if (argument == "--tracks") {
			const char* value = next("--tracks");
			if (!value) return false;
			options.trackLimit = std::atoi(value);
		} else if (argument == "--change-set") {
			const char* value = next("--change-set");
			if (!value) return false;
			options.changeSet = std::atoll(value);
		} else if (argument == "--album") {
			const char* value = next("--album");
			if (!value) return false;
			options.albumId = std::atoll(value);
		} else if (argument == "--file") {
			const char* value = next("--file");
			if (!value) return false;
			options.fileId = std::atoll(value);
		} else {
			error = "unrecognised option: " + argument;
			return false;
		}
	}
	return true;
}

/// Progress printer. Rewrites one line so a long run does not fill the scrollback.
class ProgressPrinter {
public:
	explicit ProgressPrinter(bool quiet) : m_quiet(quiet) {}

	bool operator()(std::int64_t done, std::int64_t total, const std::string& message) {
		if (m_quiet) return true;

		const auto now = std::chrono::steady_clock::now();
		if (now - m_last < std::chrono::milliseconds(100) && done != total) return true;
		m_last = now;

		std::string line;
		if (total > 0) {
			const int percent = static_cast<int>((done * 100) / std::max<std::int64_t>(total, 1));
			line = "  [" + std::to_string(percent) + "%] " + std::to_string(done) + "/"
				+ std::to_string(total) + "  ";
		} else {
			line = "  [" + std::to_string(done) + "]  ";
		}
		line += message.substr(0, 60);

		// Pad to erase the previous, possibly longer, line.
		if (line.size() < m_previousLength) line.append(m_previousLength - line.size(), ' ');
		m_previousLength = line.size();

		std::cout << '\r' << line << std::flush;
		return true;
	}

	void finish() {
		if (m_quiet || m_previousLength == 0) return;
		std::cout << '\r' << std::string(m_previousLength, ' ') << '\r' << std::flush;
		m_previousLength = 0;
	}

private:
	bool m_quiet;
	std::size_t m_previousLength = 0;
	std::chrono::steady_clock::time_point m_last{};
};

Status buildConfig(const Options& options, LibraryConfig& config) {
	config.dataDirectory = options.data;
	config.sourceRoots = options.sources;
	config.outputRoot = options.output;
	config.offline = options.offline;
	config.workerCount = options.workers;
	config.appleStorefront = options.storefront;
	config.privacy.clearId3v1Comment = options.clearId3v1Comment;

	if (auto profile = privacyProfileFromString(options.privacyProfile)) {
		config.privacy.profile = *profile;
	} else {
		return Status(Error{ErrorCode::InvalidArgument,
			"unknown privacy profile \"" + options.privacyProfile + "\""});
	}
	return Status::success();
}

void printCoverage(const CoverageReport& report) {
	std::cout << "Coverage\n";
	std::cout << "  files ................. " << report.totalFiles
		<< "  (" << report.readable << " readable, " << report.unreadable << " unreadable)\n";
	std::cout << "  total size ............ " << text::formatBytes(report.totalBytes) << "\n";
	std::cout << "  total duration ........ " << text::formatDuration(report.totalDurationMs) << "\n";
	std::cout << "  albums ................ " << report.albumCount
		<< "  (" << report.albumsNeedingReview << " need review)\n";
	std::cout << "  with artwork .......... " << report.withArtwork << "\n";
	std::cout << "  with lyrics ........... " << report.withLyrics << "\n";
	std::cout << "  with BPM .............. " << report.withBpm << "\n";
	std::cout << "  with gain fields ...... " << report.withGainFields << "\n";
	std::cout << "  with privacy findings . " << report.withPrivacyFindings << "\n";
	std::cout << "  duplicate audio groups  " << report.likelyDuplicateGroups << "\n";

	const auto section = [](std::string_view title,
		const std::vector<std::pair<std::string, std::int64_t>>& rows) {
		if (rows.empty()) return;
		std::cout << "\n" << title << "\n";
		for (const auto& [label, count] : rows) {
			std::cout << "  " << label;
			const std::size_t pad = label.size() < 24 ? 24 - label.size() : 1;
			std::cout << std::string(pad, ' ') << count << "\n";
		}
	};

	section("Tag versions", report.tagVersionDistribution);
	section("Artwork sizes", report.artworkSizeDistribution);
	section("Bitrate modes", report.bitrateModeDistribution);
	section("Missing fields", report.missingFieldCounts);
	section("Read errors", report.readErrorCounts);
}

void printNaming(const NamingConformity& conformity) {
	std::cout << "\nNaming conformity (measured, not assumed)\n";
	std::cout << "  template .............. " << NamingTemplate::pattern() << "\n";
	std::cout << "  files examined ........ " << conformity.totalFiles << "\n";
	std::cout << "  matching the template . " << conformity.matchingTemplate
		<< "  (" << static_cast<int>(conformity.matchPercentage()) << "%)\n";
	std::cout << "  would need review ..... " << conformity.wouldNeedReview << "\n";

	if (!conformity.exceptionCounts.empty()) {
		std::cout << "\n  Exception classes\n";
		for (const auto& [kind, count] : conformity.exceptionCounts) {
			std::cout << "    " << kind;
			const std::size_t pad = kind.size() < 28 ? 28 - kind.size() : 1;
			std::cout << std::string(pad, ' ') << count << "\n";
		}
	}
	if (!conformity.observedGrammars.empty()) {
		std::cout << "\n  Observed path grammars\n";
		for (const auto& [grammar, count] : conformity.observedGrammars) {
			std::cout << "    " << grammar;
			const std::size_t pad = grammar.size() < 40 ? 40 - grammar.size() : 1;
			std::cout << std::string(pad, ' ') << count << "\n";
		}
	}
}

int runScan(Library& library, const Options& options) {
	ProgressPrinter printer(options.quiet);
	auto result = library.scan([&](std::int64_t d, std::int64_t t, const std::string& m) {
		return printer(d, t, m);
	});
	printer.finish();

	if (!result) {
		std::cerr << "scan failed: " << result.error().describe() << "\n";
		return kExitRuntimeError;
	}

	const ScanResult& scan = result.value();
	std::cout << "Scan complete in " << static_cast<int>(scan.elapsedSeconds) << " s\n";
	std::cout << "  seen ........ " << scan.filesSeen << "\n";
	std::cout << "  added ....... " << scan.filesAdded << "\n";
	std::cout << "  changed ..... " << scan.filesChanged << "\n";
	std::cout << "  unchanged ... " << scan.filesUnchanged << "\n";
	std::cout << "  unreadable .. " << scan.filesUnreadable << "\n";
	std::cout << "  read ........ " << text::formatBytes(scan.bytesRead) << "\n";
	if (scan.elapsedSeconds > 0.5 && scan.filesSeen > 0) {
		std::cout << "  rate ........ "
			<< static_cast<int>(static_cast<double>(scan.filesSeen) / scan.elapsedSeconds)
			<< " files/s\n";
	}
	for (const auto& warning : scan.warnings) {
		std::cout << "  warning: " << warning << "\n";
	}
	if (scan.cancelled) return kExitCancelled;
	return kExitOk;
}

int runPlan(Library& library, const Options& options) {
	ProgressPrinter printer(options.quiet);
	auto result = library.plan([&](std::int64_t d, std::int64_t t, const std::string& m) {
		return printer(d, t, m);
	});
	printer.finish();

	if (!result) {
		std::cerr << "plan failed: " << result.error().describe() << "\n";
		return result.error().code == ErrorCode::InvalidArgument ? kExitUsage : kExitRuntimeError;
	}

	const ChangeSet& set = result.value();
	const auto summary = set.summarise();

	if (options.format == "json") {
		std::cout << toJson(set) << "\n";
	} else if (options.format == "csv") {
		std::cout << toCsv(set);
	} else {
		std::cout << "Change set " << set.id.value << "\n";
		std::cout << "  output root ........... " << set.outputRootPath << "\n";
		std::cout << "  files planned ......... " << summary.total << "\n";
		std::cout << "  writable .............. " << summary.writable << "\n";
		std::cout << "  blocked ............... " << summary.blocked << "\n";
		std::cout << "  needing review ........ " << summary.needingReview << "\n";
		std::cout << "  artwork replacements .. " << summary.artworkReplacements << "\n";
		std::cout << "  lyrics to add ......... " << summary.lyricsAdded << "\n";
		std::cout << "  BPM to write .......... " << summary.bpmProposed << "\n";
		std::cout << "  privacy removals ...... " << summary.privacyRemovals << "\n";
		std::cout << "  gain removals ......... " << summary.gainRemovals << "\n";
		std::cout << "  optimisation savings .. " << text::formatBytes(summary.optimisationSavings) << "\n";
		std::cout << "  enrichment growth ..... " << text::formatBytes(summary.enrichmentGrowth) << "\n";
		if (!set.collisions.empty()) {
			std::cout << "\n  Destination collisions (" << set.collisions.size() << ")\n";
			for (std::size_t i = 0; i < set.collisions.size() && i < 20; ++i) {
				std::cout << "    " << set.collisions[i].relativePath
					<< "  (" << set.collisions[i].files.size() << " files"
					<< (set.collisions[i].caseOnly ? ", differing only by case" : "") << ")\n";
			}
		}
		std::cout << "\nRun 'resonance export-copies --change-set " << set.id.value
			<< "' to write the copies.\n";
	}

	return summary.needingReview > 0 ? kExitReviewRequired : kExitOk;
}

int runExport(Library& library, const Options& options) {
	std::int64_t setId = options.changeSet;
	if (setId == 0) {
		auto latest = library.catalogue().latestChangeSet();
		if (!latest || !latest.value()) {
			std::cerr << "no change set found; run 'resonance plan' first\n";
			return kExitUsage;
		}
		setId = latest.value()->value;
	}

	if (options.output.empty()) {
		std::cerr << "export-copies requires --output\n";
		return kExitUsage;
	}

	if (!options.yes) {
		std::cout << "About to write copies into " << options.output.string() << ".\n";
		std::cout << "The source collection is not modified. Continue? [y/N] ";
		std::string answer;
		std::getline(std::cin, answer);
		if (answer != "y" && answer != "Y" && answer != "yes") {
			std::cout << "Cancelled.\n";
			return kExitCancelled;
		}
	}

	ProgressPrinter printer(options.quiet);
	auto result = library.exportCopies(ChangeSetId(setId),
		[&](std::int64_t d, std::int64_t t, const std::string& m) { return printer(d, t, m); });
	printer.finish();

	if (!result) {
		std::cerr << "export failed: " << result.error().describe() << "\n";
		return kExitRuntimeError;
	}

	const ExportResult& e = result.value();
	std::cout << "Export complete in " << static_cast<int>(e.elapsedSeconds) << " s\n";
	std::cout << "  attempted ............. " << e.attempted << "\n";
	std::cout << "  written ............... " << e.written << "\n";
	std::cout << "  skipped ............... " << e.skipped << "\n";
	std::cout << "  failed ................ " << e.failed << "\n";
	std::cout << "  bytes written ......... " << text::formatBytes(e.bytesWritten) << "\n";
	std::cout << "  optimisation savings .. " << text::formatBytes(e.optimisationSavings) << "\n";
	std::cout << "  enrichment growth ..... " << text::formatBytes(e.enrichmentGrowth) << "\n";

	if (!e.failures.empty()) {
		std::cout << "\n  Failures and skips\n";
		for (std::size_t i = 0; i < e.failures.size() && i < 40; ++i) {
			std::cout << "    " << e.failures[i] << "\n";
		}
		if (e.failures.size() > 40) {
			std::cout << "    ... and " << (e.failures.size() - 40) << " more\n";
		}
	}

	if (e.cancelled) return kExitCancelled;
	return e.failed > 0 ? kExitRuntimeError : kExitOk;
}

int runReview(Library& library, const Options& options) {
	if (options.fileId > 0) {
		auto plan = library.previewFile(FileId(options.fileId));
		if (!plan) {
			std::cerr << "preview failed: " << plan.error().describe() << "\n";
			return kExitRuntimeError;
		}
		const FilePlan& p = plan.value();
		std::cout << "File " << p.fileId.value << "\n";
		std::cout << "  source ........ " << p.sourcePath << "\n";
		std::cout << "  destination ... "
			<< (p.destinationRelativePath.empty() ? "(review required)" : p.destinationRelativePath) << "\n";
		std::cout << "  container ..... " << toString(p.outputContainer) << "\n";

		if (!p.naming.exceptions.empty()) {
			std::cout << "\n  Naming exceptions\n";
			for (const auto& exception : p.naming.exceptions) {
				std::cout << "    [" << (exception.advisory ? "advisory" : "review  ") << "] "
					<< toString(exception.kind) << ": " << exception.detail << "\n";
			}
		}

		const auto changing = p.changingDecisions();
		std::cout << "\n  Tag changes (" << changing.size() << ")\n";
		for (const auto* decision : changing) {
			std::cout << "    " << toString(decision->action) << "  " << decision->frameId;
			if (!decision->description.empty()) std::cout << " [" << decision->description << "]";
			std::cout << "\n      rule: " << decision->ruleId << "\n      " << decision->reason << "\n";
		}

		std::cout << "\n  Artwork ..... " << toString(p.artwork.outcome);
		if (!p.artwork.selectionReason.empty()) std::cout << " -- " << p.artwork.selectionReason;
		std::cout << "\n  Tempo ....... " << toString(p.tempo.kind) << " -- " << p.tempo.reason << "\n";
		std::cout << "  Lyrics ...... " << toString(p.lyrics.state) << " -- " << p.lyrics.reason << "\n";

		if (!p.blockers.empty()) {
			std::cout << "\n  Blockers\n";
			for (auto blocker : p.blockers) std::cout << "    " << toString(blocker) << "\n";
		}
		for (const auto& note : p.notes) std::cout << "    note: " << note << "\n";
		return p.writable() ? kExitOk : kExitReviewRequired;
	}

	if (options.albumId > 0) {
		auto review = library.reviewAlbum(AlbumId(options.albumId));
		if (!review) {
			std::cerr << "review failed: " << review.error().describe() << "\n";
			return kExitRuntimeError;
		}
		const AlbumReview& r = review.value();
		std::cout << r.album.albumArtist << " - " << r.album.album << "\n";
		std::cout << "  tracks .......... " << r.album.observedTrackCount << "\n";
		std::cout << "  confidence ...... " << toString(r.album.identityConfidence) << "\n";
		if (!r.album.editionQualifier.empty()) {
			std::cout << "  edition ......... " << r.album.editionQualifier << "\n";
		}
		if (!r.album.flags.empty()) {
			std::cout << "  flags ...........";
			for (auto flag : r.album.flags) std::cout << " " << toString(flag);
			std::cout << "\n";
		}
		std::cout << "\n  Evidence\n";
		for (const auto& e : r.album.evidence) {
			std::cout << "    [" << (e.supporting ? '+' : '-') << "] " << e.detail << "\n";
		}

		std::cout << "\n  Artwork candidates (" << r.artworkCandidates.size() << ")\n";
		for (std::size_t i = 0; i < r.artworkCandidates.size(); ++i) {
			const auto& c = r.artworkCandidates[i];
			std::cout << "    [" << c.id.value << "] " << c.providerName << "  ";
			if (c.dimensionsMeasured) {
				std::cout << c.measuredWidth << "x" << c.measuredHeight << " measured";
			} else {
				std::cout << "dimensions unmeasured";
			}
			std::cout << "  " << toString(c.coverMatch) << "  " << toString(c.sourceType) << "\n";
			if (static_cast<int>(i) == r.selection.selectedIndex) {
				std::cout << "        SELECTED: " << r.selection.reason << "\n";
			} else if (i < r.selection.rejectionReasons.size()
				&& !r.selection.rejectionReasons[i].empty()) {
				std::cout << "        rejected: " << r.selection.rejectionReasons[i] << "\n";
			}
		}
		std::cout << "\n  Outcome: " << toString(r.selection.outcome) << "\n";
		std::cout << "    " << r.selection.reason << "\n";

		std::cout << "\n  Review aids\n";
		std::cout << "    Google Images: " << r.googleImagesUrl << "\n";
		std::cout << "    Apple Music:   " << r.appleSearchUrl << "\n";
		std::cout << "    MusicBrainz:   " << r.musicBrainzUrl << "\n";
		return r.selection.outcome == ArtworkOutcome::NeedsReview ? kExitReviewRequired : kExitOk;
	}

	// No target: list what needs attention.
	auto albums = library.catalogue().listAlbums(true, 50, 0);
	if (!albums) {
		std::cerr << "review failed: " << albums.error().describe() << "\n";
		return kExitRuntimeError;
	}
	auto count = library.catalogue().countAlbums(true);

	std::cout << "Albums needing review: " << (count.ok() ? count.value() : 0) << "\n\n";
	for (const auto& album : albums.value()) {
		std::cout << "  [" << album.id.value << "] " << album.albumArtist << " - " << album.album
			<< "  (" << album.observedTrackCount << " tracks, "
			<< toString(album.identityConfidence) << ")\n";
		for (auto flag : album.flags) std::cout << "      " << toString(flag) << "\n";
	}
	if (albums.value().empty()) std::cout << "  Nothing needs review.\n";
	std::cout << "\nUse 'resonance review --album <id>' for detail.\n";
	return kExitOk;
}

int runReport(Library& library, const Options& options) {
	auto coverage = library.coverage();
	if (!coverage) {
		std::cerr << "report failed: " << coverage.error().describe() << "\n";
		return kExitRuntimeError;
	}
	auto naming = library.namingReport();
	if (!naming) {
		std::cerr << "report failed: " << naming.error().describe() << "\n";
		return kExitRuntimeError;
	}

	if (options.format == "json") {
		const CoverageReport& c = coverage.value();
		const NamingConformity& n = naming.value();
		std::cout << "{\"coverage\":{"
			<< "\"total_files\":" << c.totalFiles
			<< ",\"readable\":" << c.readable
			<< ",\"unreadable\":" << c.unreadable
			<< ",\"with_artwork\":" << c.withArtwork
			<< ",\"with_lyrics\":" << c.withLyrics
			<< ",\"with_bpm\":" << c.withBpm
			<< ",\"with_gain_fields\":" << c.withGainFields
			<< ",\"with_privacy_findings\":" << c.withPrivacyFindings
			<< ",\"total_bytes\":" << c.totalBytes
			<< ",\"total_duration_ms\":" << c.totalDurationMs
			<< ",\"album_count\":" << c.albumCount
			<< ",\"albums_needing_review\":" << c.albumsNeedingReview
			<< ",\"duplicate_audio_groups\":" << c.likelyDuplicateGroups
			<< "},\"naming\":{"
			<< "\"template\":\"" << text::jsonEscape(NamingTemplate::pattern()) << "\""
			<< ",\"total_files\":" << n.totalFiles
			<< ",\"matching_template\":" << n.matchingTemplate
			<< ",\"match_percentage\":" << n.matchPercentage()
			<< ",\"would_need_review\":" << n.wouldNeedReview
			<< "}}\n";
	} else {
		printCoverage(coverage.value());
		printNaming(naming.value());
	}

	if (!options.destination.empty()) {
		if (auto status = library.writeNamingConventionReport(options.destination); !status) {
			std::cerr << "could not write the naming report: " << status.error().describe() << "\n";
			return kExitRuntimeError;
		}
		std::cout << "\nNaming evidence written to " << options.destination.string() << "\n";
	}
	return kExitOk;
}

int runDiagnostics(Library& library) {
	auto diagnostics = library.diagnostics();
	if (!diagnostics) {
		std::cerr << "diagnostics failed: " << diagnostics.error().describe() << "\n";
		return kExitRuntimeError;
	}
	const auto& d = diagnostics.value();
	std::cout << "Resonance " << version::kVersion << "\n";
	std::cout << "  catalogue schema ...... " << d.catalogueVersion << "\n";
	std::cout << "  catalogue journal ..... " << d.catalogueJournalMode << "\n";
	std::cout << "  catalogue size ........ " << text::formatBytes(d.catalogueBytes) << "\n";
	std::cout << "  integrity ............. " << (d.integrityOk ? "ok" : "FAILED") << "\n";
	std::cout << "  protected roots ....... " << d.protectedRootCount
		<< "  (" << d.absentRootCount << " currently absent)\n";
	std::cout << "  artwork derivative .... " << d.derivativeConfig << "\n";
	std::cout << "  tempo engine .......... " << d.tempoEngine << "\n";
	std::cout << "  network ............... " << (d.offline ? "offline" : "enabled") << "\n";
	return d.integrityOk ? kExitOk : kExitRuntimeError;
}

} // namespace

int main(int argc, char** argv) {
	Options options;
	std::string parseError;

	if (!parseArguments(argc, argv, options, parseError)) {
		std::cerr << "resonance: " << parseError << "\n\n";
		printUsage();
		return kExitUsage;
	}

	if (options.version) {
		std::cout << version::kProjectName << " " << version::kVersion << "\n";
		std::cout << version::kDescription << "\n";
		std::cout << version::kHomepage << "\n";
		return kExitOk;
	}

	if (options.help || options.command.empty()) {
		printUsage();
		return options.command.empty() && !options.help ? kExitUsage : kExitOk;
	}

	LibraryConfig config;
	if (auto status = buildConfig(options, config); !status) {
		std::cerr << "resonance: " << status.error().describe() << "\n";
		return kExitUsage;
	}

	// Commands that need no catalogue.
	if (options.command == "help") {
		printUsage();
		return kExitOk;
	}

	if (config.sourceRoots.empty() && options.command != "diagnostics") {
		std::cerr << "resonance: at least one --source is required\n";
		return kExitUsage;
	}

	Library library;
	if (auto status = library.open(std::move(config)); !status) {
		std::cerr << "resonance: " << status.error().describe() << "\n";
		return status.error().code == ErrorCode::ProtectedRootViolation
			? kExitConfiguration : kExitRuntimeError;
	}

	ProgressPrinter printer(options.quiet);
	const auto progress = [&](std::int64_t d, std::int64_t t, const std::string& m) {
		return printer(d, t, m);
	};

	if (options.command == "scan") {
		return runScan(library, options);
	}

	if (options.command == "albums") {
		auto count = library.resolveAlbums(progress);
		printer.finish();
		if (!count) {
			std::cerr << "albums failed: " << count.error().describe() << "\n";
			return kExitRuntimeError;
		}
		std::cout << "Grouped " << count.value() << " provisional albums.\n";
		auto needingReview = library.catalogue().countAlbums(true);
		if (needingReview) {
			std::cout << needingReview.value() << " need review.\n";
			return needingReview.value() > 0 ? kExitReviewRequired : kExitOk;
		}
		return kExitOk;
	}

	if (options.command == "sample") {
		if (options.destination.empty()) {
			std::cerr << "sample requires --destination\n";
			return kExitUsage;
		}
		auto copied = library.createSample(options.destination,
			options.albumLimit > 0 ? options.albumLimit : 20,
			options.trackLimit > 0 ? options.trackLimit : 10, progress);
		printer.finish();
		if (!copied) {
			std::cerr << "sample failed: " << copied.error().describe() << "\n";
			return copied.error().code == ErrorCode::ProtectedRootViolation
				? kExitConfiguration : kExitRuntimeError;
		}
		std::cout << "Copied " << copied.value() << " files to " << options.destination.string()
			<< ".\nEvery copy was verified by hash against its source; the source collection is "
			   "unchanged.\n";
		return kExitOk;
	}

	if (options.command == "analyse") {
		auto analysed = library.analyseTempo(progress);
		printer.finish();
		if (!analysed) {
			std::cerr << "analyse failed: " << analysed.error().describe() << "\n";
			return analysed.error().code == ErrorCode::Cancelled ? kExitCancelled : kExitRuntimeError;
		}
		std::cout << "Analysed tempo for " << analysed.value() << " files.\n";
		return kExitOk;
	}

	if (options.command == "artwork") {
		auto decided = library.fetchArtwork(progress);
		printer.finish();
		if (!decided) {
			std::cerr << "artwork failed: " << decided.error().describe() << "\n";
			return decided.error().code == ErrorCode::Cancelled ? kExitCancelled : kExitRuntimeError;
		}
		std::cout << "Processed artwork for " << decided.value() << " albums.\n";
		return kExitOk;
	}

	if (options.command == "lyrics") {
		auto decided = library.fetchLyrics(progress);
		printer.finish();
		if (!decided) {
			std::cerr << "lyrics failed: " << decided.error().describe() << "\n";
			return decided.error().code == ErrorCode::Cancelled ? kExitCancelled : kExitRuntimeError;
		}
		std::cout << "Resolved lyrics state for " << decided.value() << " files.\n";
		return kExitOk;
	}

	if (options.command == "plan") {
		return runPlan(library, options);
	}

	if (options.command == "review") {
		return runReview(library, options);
	}

	if (options.command == "export-copies") {
		return runExport(library, options);
	}

	if (options.command == "verify") {
		std::int64_t setId = options.changeSet;
		if (setId == 0) {
			auto latest = library.catalogue().latestChangeSet();
			if (!latest || !latest.value()) {
				std::cerr << "no change set to verify\n";
				return kExitUsage;
			}
			setId = latest.value()->value;
		}
		auto verified = library.verify(ChangeSetId(setId), progress);
		printer.finish();
		if (!verified) {
			std::cerr << "verify failed: " << verified.error().describe() << "\n";
			return kExitRuntimeError;
		}
		std::cout << "Verified " << verified.value() << " written files against their plans.\n";
		return kExitOk;
	}

	if (options.command == "resume") {
		std::int64_t setId = options.changeSet;
		if (setId == 0) {
			auto latest = library.catalogue().latestChangeSet();
			if (!latest || !latest.value()) {
				std::cerr << "no change set to resume\n";
				return kExitUsage;
			}
			setId = latest.value()->value;
		}
		auto reconciled = library.recover(ChangeSetId(setId), progress);
		printer.finish();
		if (!reconciled) {
			std::cerr << "resume failed: " << reconciled.error().describe() << "\n";
			return kExitRuntimeError;
		}
		std::cout << "Reconciled " << reconciled.value() << " interrupted operations.\n";
		return kExitOk;
	}

	if (options.command == "report") {
		return runReport(library, options);
	}

	if (options.command == "diagnostics") {
		return runDiagnostics(library);
	}

	std::cerr << "resonance: unknown command \"" << options.command << "\"\n\n";
	printUsage();
	return kExitUsage;
}
