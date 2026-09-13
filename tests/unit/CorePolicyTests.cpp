// SPDX-License-Identifier: GPL-3.0-or-later
// Policy tests for NAME-001/002, ART-002, PRIV-001, GAIN-001, ID-001, BPM-001
// and LYR-001.
//
// These are the FRD's own decision fixtures, written as executable assertions.
// Where the FRD states a required decision, the test asserts that exact decision
// rather than a convenient approximation of it.
#include "TestHarness.hpp"

#include "mlcore/AlbumGrouping.hpp"
#include "mlcore/ArtworkPolicy.hpp"
#include "mlcore/Enrichment.hpp"
#include "mlcore/NamingTemplate.hpp"
#include "mlcore/TagPolicy.hpp"
#include "mlcore/Text.hpp"

using namespace ml;

namespace {

TagFrame textFrame(std::string id, std::string value,
	TagContainer container = TagContainer::Id3v2_3) {
	TagFrame frame;
	frame.container = container;
	frame.id = std::move(id);
	frame.value = std::move(value);
	frame.interpreted = true;
	frame.rawSize = 10 + frame.value.size();
	return frame;
}

TagFrame userTextFrame(std::string description, std::string value) {
	TagFrame frame = textFrame("TXXX", std::move(value));
	frame.description = std::move(description);
	return frame;
}

TagSnapshot snapshotWith(std::vector<TagFrame> frames) {
	TagSnapshot snapshot;
	snapshot.primaryContainer = TagContainer::Id3v2_3;
	snapshot.containers.push_back(TagContainer::Id3v2_3);
	snapshot.frames = std::move(frames);
	return snapshot;
}

ArtworkCandidate makeCandidate(int size, CoverMatch match, ArtworkSourceType type,
	std::vector<ArtworkDefect> defects = {}) {
	ArtworkCandidate candidate;
	candidate.providerId = "test";
	candidate.measuredWidth = size;
	candidate.measuredHeight = size;
	candidate.dimensionsMeasured = true;
	candidate.coverMatch = match;
	candidate.sourceType = type;
	candidate.defects = std::move(defects);
	candidate.matchConfidence = Confidence::Strong;
	return candidate;
}

} // namespace

// ===========================================================================
// NAME-001: the confirmed template
// ===========================================================================

TEST_CASE("NAME-001: the user's own example produces the user's own path") {
	// FRD section 9 supplies this exact example:
	//   E:\Music\2 Unlimited\Get Ready!\01. 2 Unlimited - Get Ready for This (Orchestral Mix)
	NamingTemplate naming;
	NamingInput input;
	input.albumArtist = "2 Unlimited";
	input.album = "Get Ready!";
	input.artist = "2 Unlimited";
	input.title = "Get Ready for This (Orchestral Mix)";
	input.trackNumber = 1;

	const NamingResult result = naming.apply(input);
	CHECK(result.ok());
	CHECK_EQUAL(result.relativePath,
		std::string("2 Unlimited/Get Ready!/01. 2 Unlimited - Get Ready for This (Orchestral Mix).mp3"));
}

TEST_CASE("NAME-001: track 1 pads to 01 and track 100 stays 100") {
	NamingTemplate naming;
	NamingInput input;
	input.albumArtist = "A";
	input.album = "B";
	input.artist = "C";
	input.title = "D";

	input.trackNumber = 1;
	CHECK_EQUAL(naming.apply(input).relativePath, std::string("A/B/01. C - D.mp3"));

	input.trackNumber = 9;
	CHECK_EQUAL(naming.apply(input).relativePath, std::string("A/B/09. C - D.mp3"));

	input.trackNumber = 42;
	CHECK_EQUAL(naming.apply(input).relativePath, std::string("A/B/42. C - D.mp3"));

	// Three digits are not truncated to two.
	input.trackNumber = 100;
	CHECK_EQUAL(naming.apply(input).relativePath, std::string("A/B/100. C - D.mp3"));
}

TEST_CASE("NAME-001: accents and punctuation survive") {
	NamingTemplate naming;
	NamingInput input;
	input.albumArtist = "Sigur Rós";
	input.album = "Ágætis byrjun";
	input.artist = "Sigur Rós";
	input.title = "Svefn-g-englar";
	input.trackNumber = 2;

	const NamingResult result = naming.apply(input);
	CHECK(result.ok());
	CHECK_EQUAL(result.relativePath,
		std::string("Sigur Rós/Ágætis byrjun/02. Sigur Rós - Svefn-g-englar.mp3"));
}

TEST_CASE("NAME-001: a missing album artist is never filled from the track artist") {
	// FRD section 9 forbids this specific substitution. It must become a review
	// exception rather than a silently-invented folder name.
	NamingTemplate naming;
	NamingInput input;
	input.album = "Album";
	input.artist = "Track Artist";
	input.title = "Title";
	input.trackNumber = 1;
	// albumArtist deliberately empty.

	const NamingResult result = naming.apply(input);
	CHECK(result.requiresReview);
	CHECK(result.relativePath.empty());

	bool sawMissingAlbumArtist = false;
	for (const auto& exception : result.exceptions) {
		if (exception.kind == NamingExceptionKind::MissingAlbumArtist) sawMissingAlbumArtist = true;
	}
	CHECK(sawMissingAlbumArtist);

	// And specifically: the track artist did not leak into the path.
	CHECK(result.albumArtistComponent.find("Track Artist") == std::string::npos);
}

TEST_CASE("NAME-001: a missing or non-numeric track number needs review") {
	NamingTemplate naming;
	NamingInput input;
	input.albumArtist = "A";
	input.album = "B";
	input.artist = "C";
	input.title = "D";

	const NamingResult missing = naming.apply(input);
	CHECK(missing.requiresReview);

	input.trackNumber = 0;
	const NamingResult zero = naming.apply(input);
	CHECK(zero.requiresReview);
}

TEST_CASE("NAME-001: multiple tag values are a review decision, not a silent pick") {
	NamingTemplate naming;
	NamingInput input;
	input.albumArtist = "A";
	input.album = "B";
	input.artist = "C";
	input.title = "D";
	input.trackNumber = 1;
	input.artistHadMultipleValues = true;

	const NamingResult result = naming.apply(input);
	CHECK(result.requiresReview);
}

TEST_CASE("FN-NAME-01: Windows illegal characters are replaced and reported") {
	NamingTemplate naming;
	NamingInput input;
	input.albumArtist = "AC/DC";
	input.album = "Back: In Black";
	input.artist = "AC/DC";
	input.title = "What Do You Do for Money \"Honey\"?";
	input.trackNumber = 3;

	const NamingResult result = naming.apply(input);
	// Sanitisation is advisory: the result is still deterministic and usable.
	CHECK(result.ok());
	CHECK(result.relativePath.find('/') != std::string::npos);   // separators remain
	CHECK_EQUAL(result.albumArtistComponent, std::string("AC_DC"));
	CHECK_EQUAL(result.albumComponent, std::string("Back_ In Black"));

	bool sawIllegal = false;
	for (const auto& exception : result.exceptions) {
		if (exception.kind == NamingExceptionKind::IllegalCharacters) {
			sawIllegal = true;
			CHECK(exception.advisory);
		}
	}
	CHECK(sawIllegal);
}

TEST_CASE("FN-NAME-01: Windows reserved device names need review") {
	NamingTemplate naming;
	NamingInput input;
	input.albumArtist = "CON";
	input.album = "Album";
	input.artist = "Artist";
	input.title = "Title";
	input.trackNumber = 1;

	const NamingResult result = naming.apply(input);
	CHECK(result.requiresReview);

	bool sawReserved = false;
	for (const auto& exception : result.exceptions) {
		if (exception.kind == NamingExceptionKind::ReservedName) sawReserved = true;
	}
	CHECK(sawReserved);
}

TEST_CASE("FN-NAME-01: trailing spaces and dots are trimmed, not left to Windows") {
	NamingTemplate naming;
	NamingInput input;
	input.albumArtist = "Artist ";
	input.album = "Album.";
	input.artist = "Artist";
	input.title = "Title";
	input.trackNumber = 1;

	const NamingResult result = naming.apply(input);
	CHECK_EQUAL(result.albumArtistComponent, std::string("Artist"));
	CHECK_EQUAL(result.albumComponent, std::string("Album"));
}

TEST_CASE("FN-NAME-01: an over-long path is reported rather than truncated") {
	NamingOptions options;
	options.maxPathLength = 60;
	NamingTemplate naming(options);

	NamingInput input;
	input.albumArtist = std::string(30, 'A');
	input.album = std::string(30, 'B');
	input.artist = std::string(30, 'C');
	input.title = std::string(30, 'D');
	input.trackNumber = 1;

	const NamingResult result = naming.apply(input);
	CHECK(result.requiresReview);
	CHECK(result.relativePath.empty());

	bool sawTooLong = false;
	for (const auto& exception : result.exceptions) {
		if (exception.kind == NamingExceptionKind::PathTooLong) sawTooLong = true;
	}
	CHECK(sawTooLong);
}

// ===========================================================================
// NAME-002: collisions
// ===========================================================================

TEST_CASE("NAME-002: an identical destination is reported as a collision") {
	CollisionDetector detector;
	CHECK(detector.add("A/B/01. C - D.mp3", FileId(1)));
	CHECK(!detector.add("A/B/01. C - D.mp3", FileId(2)));
	CHECK_EQUAL(detector.collisions().size(), std::size_t{1});
	CHECK_EQUAL(detector.collisions()[0].files.size(), std::size_t{2});
}

TEST_CASE("NAME-002: paths differing only by case collide") {
	// They are distinct on ext4 and identical on NTFS and case-insensitive APFS.
	// The library's home is Windows, so this must be caught before copying.
	CollisionDetector detector;
	CHECK(detector.add("Artist/Album/01. A - Song.mp3", FileId(1)));
	CHECK(!detector.add("artist/album/01. a - song.mp3", FileId(2)));
	CHECK_EQUAL(detector.collisions().size(), std::size_t{1});
	CHECK(detector.collisions()[0].caseOnly);
}

TEST_CASE("NAME-002: different paths do not collide") {
	CollisionDetector detector;
	CHECK(detector.add("A/B/01. C - D.mp3", FileId(1)));
	CHECK(detector.add("A/B/02. C - E.mp3", FileId(2)));
	CHECK(detector.add("A/C/01. C - D.mp3", FileId(3)));
	CHECK(detector.collisions().empty());
}

// ===========================================================================
// ART-002: the FRD section 6 decision fixtures
// ===========================================================================

TEST_CASE("ART-002: a clean 600x600 asset beats a damaged 1451x1367 scan") {
	// FRD section 6, case 1. The scan is non-square as well as damaged, which is
	// itself grounds for review rather than an automatic crop.
	ArtworkPolicy policy;

	ArtworkCandidate clean = makeCandidate(600, CoverMatch::IntendedCover,
		ArtworkSourceType::EvidencedDigitalAsset);

	ArtworkCandidate scan;
	scan.providerId = "test";
	scan.measuredWidth = 1451;
	scan.measuredHeight = 1367;
	scan.dimensionsMeasured = true;
	scan.coverMatch = CoverMatch::IntendedCover;
	scan.sourceType = ArtworkSourceType::ScanOrPhoto;
	scan.matchConfidence = Confidence::Strong;
	scan.defects = {ArtworkDefect::Fading, ArtworkDefect::SleeveWear,
		ArtworkDefect::CompressionBlocks};

	const ArtworkSelection selection = policy.select({clean, scan});
	CHECK_EQUAL(static_cast<int>(selection.outcome), static_cast<int>(ArtworkOutcome::Selected));
	CHECK_EQUAL(selection.selectedIndex, 0);
	// And its resolution is explicitly not treated as an unresolved issue.
	CHECK(selection.reason.find("not an unresolved issue") != std::string::npos);
}

TEST_CASE("ART-002: a clean 600x600 beats a damaged square scan of any size") {
	ArtworkPolicy policy;

	ArtworkCandidate clean = makeCandidate(600, CoverMatch::IntendedCover,
		ArtworkSourceType::EvidencedDigitalAsset);
	ArtworkCandidate damaged = makeCandidate(3000, CoverMatch::IntendedCover,
		ArtworkSourceType::ScanOrPhoto,
		{ArtworkDefect::Fading, ArtworkDefect::RingWear, ArtworkDefect::Crease});

	const ArtworkSelection selection = policy.select({clean, damaged});
	CHECK_EQUAL(static_cast<int>(selection.outcome), static_cast<int>(ArtworkOutcome::Selected));
	CHECK_MESSAGE(selection.selectedIndex == 0,
		"resolution must not override observed condition");
}

TEST_CASE("ART-002: among equally clean assets, 2000 beats 1200") {
	// FRD section 6, case 2: divisibility by 600 carries no advantage.
	ArtworkPolicy policy;

	ArtworkCandidate large = makeCandidate(2000, CoverMatch::IntendedCover,
		ArtworkSourceType::EvidencedDigitalAsset);
	ArtworkCandidate small = makeCandidate(1200, CoverMatch::IntendedCover,
		ArtworkSourceType::EvidencedDigitalAsset);

	const ArtworkSelection selection = policy.select({small, large});
	CHECK_EQUAL(static_cast<int>(selection.outcome), static_cast<int>(ArtworkOutcome::Selected));
	CHECK_EQUAL(selection.selectedIndex, 1);
}

TEST_CASE("ART-002: provider priority does not override observed quality") {
	// FRD section 6, case 3.
	ArtworkPolicy policy;

	ArtworkCandidate streamingDamaged = makeCandidate(1500, CoverMatch::IntendedCover,
		ArtworkSourceType::EvidencedDigitalAsset,
		{ArtworkDefect::Fading, ArtworkDefect::ScanMoire, ArtworkDefect::SleeveWear});
	streamingDamaged.providerId = "itunes";

	ArtworkCandidate otherClean = makeCandidate(1000, CoverMatch::IntendedCover,
		ArtworkSourceType::EvidencedRestoration);
	otherClean.providerId = "coverartarchive";

	const ArtworkSelection selection = policy.select({streamingDamaged, otherClean});
	CHECK_EQUAL(static_cast<int>(selection.outcome), static_cast<int>(ArtworkOutcome::Selected));
	CHECK_EQUAL(selection.selectedIndex, 1);
}

TEST_CASE("ART-002: an alternate edition never silently replaces the intended cover") {
	// FRD section 6, case 4.
	ArtworkPolicy policy;

	ArtworkCandidate alternate = makeCandidate(3000, CoverMatch::AlternateEdition,
		ArtworkSourceType::EvidencedDigitalAsset);
	ArtworkCandidate intended = makeCandidate(900, CoverMatch::IntendedCover,
		ArtworkSourceType::EvidencedDigitalAsset);

	const ArtworkSelection selection = policy.select({alternate, intended});
	CHECK_EQUAL(static_cast<int>(selection.outcome), static_cast<int>(ArtworkOutcome::Selected));
	CHECK_MESSAGE(selection.selectedIndex == 1,
		"the intended cover must be preserved over a larger alternate edition");
}

TEST_CASE("ART-002: an alternate edition alone is shown for review, not applied") {
	ArtworkPolicy policy;
	ArtworkCandidate alternate = makeCandidate(3000, CoverMatch::AlternateEdition,
		ArtworkSourceType::EvidencedDigitalAsset);

	const ArtworkSelection selection = policy.select({alternate});
	CHECK_EQUAL(static_cast<int>(selection.outcome), static_cast<int>(ArtworkOutcome::NeedsReview));
}

TEST_CASE("ART-002: uncertainty is surfaced rather than settled by a score") {
	// FRD section 6, case 5.
	ArtworkPolicy policy;
	ArtworkCandidate uncertain = makeCandidate(1200, CoverMatch::Unknown,
		ArtworkSourceType::Unknown);
	uncertain.matchConfidence = Confidence::Weak;

	const ArtworkSelection selection = policy.select({uncertain});
	CHECK_EQUAL(static_cast<int>(selection.outcome), static_cast<int>(ArtworkOutcome::NeedsReview));
}

TEST_CASE("ART-002: a manual lock outranks every provider result") {
	ArtworkPolicy policy;

	ArtworkCandidate best = makeCandidate(3000, CoverMatch::IntendedCover,
		ArtworkSourceType::EvidencedDigitalAsset);
	ArtworkCandidate locked = makeCandidate(600, CoverMatch::Unknown,
		ArtworkSourceType::UserSupplied);
	locked.manuallyLocked = true;

	const ArtworkSelection selection = policy.select({best, locked});
	CHECK_EQUAL(static_cast<int>(selection.outcome), static_cast<int>(ArtworkOutcome::LockedByUser));
	CHECK_EQUAL(selection.selectedIndex, 1);
}

TEST_CASE("FN-ART-04: a non-square candidate goes to review, never to an automatic crop") {
	ArtworkPolicy policy;
	ArtworkCandidate wide;
	wide.providerId = "test";
	wide.measuredWidth = 1600;
	wide.measuredHeight = 900;
	wide.dimensionsMeasured = true;
	wide.coverMatch = CoverMatch::IntendedCover;
	wide.matchConfidence = Confidence::Strong;
	wide.sourceType = ArtworkSourceType::EvidencedDigitalAsset;

	const ArtworkSelection selection = policy.select({wide});
	CHECK_EQUAL(static_cast<int>(selection.outcome),
		static_cast<int>(ArtworkOutcome::NoAdequateSource));
	CHECK(selection.rejectionReasons[0].find("not square") != std::string::npos);
	CHECK(selection.rejectionReasons[0].find("never an automatic crop") != std::string::npos);
}

TEST_CASE("FN-ART-04: a source below the output size is unresolved, never upscaled") {
	ArtworkPolicy policy;
	ArtworkCandidate small = makeCandidate(300, CoverMatch::IntendedCover,
		ArtworkSourceType::EvidencedDigitalAsset);

	const ArtworkSelection selection = policy.select({small});
	CHECK_EQUAL(static_cast<int>(selection.outcome), static_cast<int>(ArtworkOutcome::NeedsReview));
	CHECK(selection.reason.find("source quality unresolved") != std::string::npos);
	CHECK(selection.rejectionReasons[0].find("upscaling is not permitted") != std::string::npos);
}

TEST_CASE("ART-002: a claimed size is not evidence; unmeasured candidates are ineligible") {
	ArtworkPolicy policy;
	ArtworkCandidate claimed;
	claimed.providerId = "test";
	claimed.claimedWidth = 3000;
	claimed.claimedHeight = 3000;
	claimed.dimensionsMeasured = false;   // never decoded
	claimed.coverMatch = CoverMatch::IntendedCover;
	claimed.matchConfidence = Confidence::Strong;

	const ArtworkSelection selection = policy.select({claimed});
	CHECK_EQUAL(static_cast<int>(selection.outcome),
		static_cast<int>(ArtworkOutcome::NoAdequateSource));
	CHECK(selection.rejectionReasons[0].find("claimed size is not evidence") != std::string::npos);
}

TEST_CASE("ART-002: watermarked and wrong images are rejected outright") {
	ArtworkPolicy policy;

	ArtworkCandidate watermarked = makeCandidate(3000, CoverMatch::IntendedCover,
		ArtworkSourceType::EvidencedDigitalAsset, {ArtworkDefect::Watermark});
	ArtworkCandidate wrong = makeCandidate(3000, CoverMatch::WrongArtwork,
		ArtworkSourceType::EvidencedDigitalAsset);

	const ArtworkSelection selection = policy.select({watermarked, wrong});
	CHECK_EQUAL(static_cast<int>(selection.outcome),
		static_cast<int>(ArtworkOutcome::NoAdequateSource));
	CHECK(selection.rejectionReasons[0].find("disqualified") != std::string::npos);
	CHECK(selection.rejectionReasons[1].find("not this album") != std::string::npos);
}

TEST_CASE("ART-003: no candidates retains the existing artwork rather than clearing it") {
	ArtworkPolicy policy;
	const ArtworkSelection selection = policy.select({});
	CHECK_EQUAL(static_cast<int>(selection.outcome),
		static_cast<int>(ArtworkOutcome::NoAdequateSource));
	CHECK(selection.reason.find("existing artwork is retained") != std::string::npos);
}

// ===========================================================================
// PRIV-001
// ===========================================================================

TEST_CASE("PRIV-001: a recognised purchase frame is removed") {
	TagFrame purchase;
	purchase.container = TagContainer::Id3v2_3;
	purchase.id = "PRIV";
	purchase.owner = "com.apple.iTunes:Account";
	purchase.binary = {std::byte{1}, std::byte{2}};
	purchase.rawSize = 64;

	PrivacyPolicy policy;
	const PrivacyPreview preview = policy.evaluate(snapshotWith({purchase}));
	CHECK_EQUAL(preview.removeCount, std::size_t{1});
	CHECK_EQUAL(preview.decisions.size(), std::size_t{1});
	CHECK_EQUAL(static_cast<int>(preview.decisions[0].action), static_cast<int>(FrameAction::Remove));
	// The value is treated as sensitive and redacted from routine output.
	CHECK(preview.decisions[0].sensitive);
}

TEST_CASE("PRIV-001: a MusicBrainz UFID is preserved -- removing every UFID is too broad") {
	TagFrame ufid;
	ufid.container = TagContainer::Id3v2_3;
	ufid.id = "UFID";
	ufid.owner = "http://musicbrainz.org";
	ufid.value = "d2c1d2ba-1111-2222-3333-444455556666";
	ufid.rawSize = 50;

	PrivacyPolicy policy;
	const PrivacyPreview preview = policy.evaluate(snapshotWith({ufid}));
	CHECK_EQUAL(preview.removeCount, std::size_t{0});
	CHECK_EQUAL(static_cast<int>(preview.decisions[0].action), static_cast<int>(FrameAction::Keep));
}

TEST_CASE("PRIV-001: an unknown private frame goes to review, not to removal") {
	TagFrame unknown;
	unknown.container = TagContainer::Id3v2_3;
	unknown.id = "PRIV";
	unknown.owner = "some.unrecognised.owner";
	unknown.binary = {std::byte{9}};
	unknown.rawSize = 32;

	PrivacyPolicy policy;
	const PrivacyPreview preview = policy.evaluate(snapshotWith({unknown}));
	CHECK_EQUAL(preview.removeCount, std::size_t{0});
	CHECK_EQUAL(preview.reviewCount, std::size_t{1});
	CHECK(preview.hasReviewItems());
}

TEST_CASE("FN-TAG-03: the strict profile removes every PRIV, COMM and UFID, and says so") {
	std::vector<TagFrame> frames;

	TagFrame priv;
	priv.container = TagContainer::Id3v2_3;
	priv.id = "PRIV";
	priv.owner = "anything";
	priv.rawSize = 20;
	frames.push_back(priv);

	TagFrame comment = textFrame("COMM", "a perfectly ordinary comment");
	frames.push_back(comment);

	TagFrame ufid;
	ufid.container = TagContainer::Id3v2_3;
	ufid.id = "UFID";
	ufid.owner = "http://musicbrainz.org";
	ufid.rawSize = 40;
	frames.push_back(ufid);

	PrivacyOptions options;
	options.profile = PrivacyProfile::StrictIDesiccate;
	PrivacyPolicy policy(options);

	const PrivacyPreview preview = policy.evaluate(snapshotWith(frames));
	CHECK_EQUAL(preview.removeCount, std::size_t{3});

	// The strict profile is explicit that it removes MusicBrainz identifiers too.
	bool sawUfidWarning = false;
	for (const auto& decision : preview.decisions) {
		if (decision.ruleId == "ufid.strict.all_ufid"
			&& decision.reason.find("MusicBrainz") != std::string::npos) {
			sawUfidWarning = true;
		}
	}
	CHECK(sawUfidWarning);
}

TEST_CASE("PRIV-001: the report-only profile decides nothing") {
	TagFrame purchase;
	purchase.container = TagContainer::Id3v2_3;
	purchase.id = "PRIV";
	purchase.owner = "com.apple.iTunes:Account";
	purchase.rawSize = 64;

	PrivacyOptions options;
	options.profile = PrivacyProfile::ReportOnly;
	PrivacyPolicy policy(options);

	const PrivacyPreview preview = policy.evaluate(snapshotWith({purchase}));
	CHECK_EQUAL(preview.removeCount, std::size_t{0});
	CHECK_EQUAL(preview.reviewCount, std::size_t{1});
}

TEST_CASE("PRIV-001: public release fields are not mistaken for personal data") {
	// Regression: a numeric-identifier heuristic flagged 252 CATALOG, DISCID and
	// CRC-32 fields across the test collection before the allowlist existed.
	PrivacyPolicy policy;
	const PrivacyPreview preview = policy.evaluate(snapshotWith({
		userTextFrame("CATALOG", "7243 8 12345 2 9"),
		userTextFrame("DISCID", "8c0b4c0d"),
		userTextFrame("BARCODE", "0724381234529"),
		userTextFrame("CRC-32", "1234567890"),
	}));
	CHECK_EQUAL(preview.reviewCount, std::size_t{0});
	CHECK_EQUAL(preview.removeCount, std::size_t{0});
}

TEST_CASE("PRIV-001: an e-mail address in a comment is flagged for review") {
	PrivacyPolicy policy;
	const PrivacyPreview preview = policy.evaluate(snapshotWith({
		textFrame("COMM", "ripped by someone@example.com"),
	}));
	CHECK_EQUAL(preview.reviewCount, std::size_t{1});
	CHECK_EQUAL(preview.removeCount, std::size_t{0});
}

TEST_CASE("PRIV-001: '@' in ordinary prose is not an e-mail address") {
	PrivacyPolicy policy;
	const PrivacyPreview preview = policy.evaluate(snapshotWith({
		textFrame("COMM", "Recorded live @ Wembley Arena, 1992"),
	}));
	CHECK_EQUAL(preview.reviewCount, std::size_t{0});
}

// ===========================================================================
// GAIN-001
// ===========================================================================

TEST_CASE("GAIN-001: ReplayGain fields are removed") {
	GainPolicy policy;
	const GainPreview preview = policy.evaluate(snapshotWith({
		userTextFrame("replaygain_track_gain", "-3.21 dB"),
		userTextFrame("replaygain_track_peak", "0.988"),
		userTextFrame("replaygain_album_gain", "-2.10 dB"),
	}));
	CHECK_EQUAL(preview.removeCount, std::size_t{3});
	CHECK(!preview.blocked());
}

TEST_CASE("GAIN-001: Sound Check is removed but gapless information is preserved") {
	TagFrame soundCheck = textFrame("COMM", "00000A00 00000A00");
	soundCheck.description = "iTunNORM";

	TagFrame gapless = textFrame("COMM", "00000000 00000210 000008A4");
	gapless.description = "iTunSMPB";

	GainPolicy policy;
	const GainPreview preview = policy.evaluate(snapshotWith({soundCheck, gapless}));

	CHECK_EQUAL(preview.removeCount, std::size_t{1});
	CHECK_EQUAL(preview.preservedGaplessFields.size(), std::size_t{1});
	CHECK_EQUAL(preview.preservedGaplessFields[0], std::string("iTunSMPB"));
}

TEST_CASE("GAIN-001: MP3Gain undo data blocks removal and is itself preserved") {
	// FRD section 8: undo information is not ordinary clutter. Deleting it would
	// make a previous gain change irreversible.
	TagFrame undo = textFrame("MP3GAIN_UNDO", "+004,+004,N", TagContainer::Apev2);
	TagFrame replayGain = userTextFrame("replaygain_track_gain", "-3.21 dB");

	GainPolicy policy;
	const GainPreview preview = policy.evaluate(snapshotWith({undo, replayGain}));

	CHECK(preview.blocked());
	CHECK_EQUAL(preview.exceptions.size(), std::size_t{1});
	CHECK_EQUAL(static_cast<int>(preview.exceptions[0]),
		static_cast<int>(GainException::Mp3GainUndoPresent));

	// Nothing is removed while the exception stands.
	CHECK_EQUAL(preview.removeCount, std::size_t{0});

	bool undoKept = false;
	for (const auto& decision : preview.decisions) {
		if (decision.ruleId == "gain.preserve.mp3gain_undo") undoKept = true;
	}
	CHECK(undoKept);
}

TEST_CASE("GAIN-001: an uninterpretable RVA2 frame goes to review") {
	TagFrame rva;
	rva.container = TagContainer::Id3v2_4;
	rva.id = "RVA2";
	rva.interpreted = false;
	rva.binary = {std::byte{0xFF}, std::byte{0x00}};
	rva.rawSize = 30;

	GainPolicy policy;
	const GainPreview preview = policy.evaluate(snapshotWith({rva}));
	CHECK(preview.blocked());
	CHECK_EQUAL(preview.removeCount, std::size_t{0});
}

// ===========================================================================
// ID-001 / FN-ALB-01 / FN-ALB-02
// ===========================================================================

TEST_CASE("FN-ALB-01: a compilation is not split by track artist") {
	AlbumResolverCore resolver;

	std::vector<GroupingInput> inputs;
	for (int i = 1; i <= 4; ++i) {
		GroupingInput input;
		input.fileId = FileId(i);
		input.relativeDirectory = "Various Artists/Now 42";
		input.album = "Now 42";
		input.albumArtist = "Various Artists";
		input.artist = "Performer " + std::to_string(i);   // four different artists
		input.trackNumber = i;
		input.trackTotal = 4;
		inputs.push_back(input);
	}

	const auto albums = resolver.group(inputs);
	CHECK_EQUAL(albums.size(), std::size_t{1});
	CHECK_EQUAL(albums[0].files.size(), std::size_t{4});
	CHECK(albums[0].isCompilation);
}

TEST_CASE("FN-ALB-02: editions differing only by a qualifier are not merged") {
	AlbumResolverCore resolver;

	GroupingInput standard;
	standard.fileId = FileId(1);
	standard.relativeDirectory = "Artist/Album";
	standard.album = "Album";
	standard.albumArtist = "Artist";
	standard.artist = "Artist";
	standard.trackNumber = 1;

	GroupingInput deluxe = standard;
	deluxe.fileId = FileId(2);
	deluxe.album = "Album (Deluxe Edition)";

	const auto albums = resolver.group({standard, deluxe});
	CHECK_MESSAGE(albums.size() == 2,
		"a deluxe edition must not merge into the standard release");
}

TEST_CASE("ID-001: duplicate track numbers are flagged and lower the confidence") {
	AlbumResolverCore resolver;

	GroupingInput first;
	first.fileId = FileId(1);
	first.relativeDirectory = "Artist/Album";
	first.album = "Album";
	first.albumArtist = "Artist";
	first.artist = "Artist";
	first.trackNumber = 1;

	GroupingInput second = first;
	second.fileId = FileId(2);
	// Same track number: two files claim track 1.

	const auto albums = resolver.group({first, second});
	CHECK_EQUAL(albums.size(), std::size_t{1});
	CHECK(albums[0].needsReview());

	bool sawDuplicate = false;
	for (auto flag : albums[0].flags) {
		if (flag == AlbumFlag::DuplicateTrackNumbers) sawDuplicate = true;
	}
	CHECK(sawDuplicate);
	CHECK_EQUAL(static_cast<int>(albums[0].identityConfidence), static_cast<int>(Confidence::Weak));
}

TEST_CASE("ID-001: an incomplete track run against a declared total is flagged") {
	AlbumResolverCore resolver;

	std::vector<GroupingInput> inputs;
	for (int i = 1; i <= 3; ++i) {
		GroupingInput input;
		input.fileId = FileId(i);
		input.relativeDirectory = "Artist/Album";
		input.album = "Album";
		input.albumArtist = "Artist";
		input.artist = "Artist";
		input.trackNumber = i;
		input.trackTotal = 12;   // nine missing
		inputs.push_back(input);
	}

	const auto albums = resolver.group(inputs);
	bool sawIncomplete = false;
	for (auto flag : albums[0].flags) {
		if (flag == AlbumFlag::IncompleteTrackRun) sawIncomplete = true;
	}
	CHECK(sawIncomplete);
}

TEST_CASE("ID-001: a consistent single-artist album reaches moderate confidence") {
	AlbumResolverCore resolver;

	std::vector<GroupingInput> inputs;
	for (int i = 1; i <= 10; ++i) {
		GroupingInput input;
		input.fileId = FileId(i);
		input.relativeDirectory = "Artist/Album";
		input.album = "Album";
		input.albumArtist = "Artist";
		input.artist = "Artist";
		input.date = "1994";
		input.trackNumber = i;
		input.trackTotal = 10;
		inputs.push_back(input);
	}

	const auto albums = resolver.group(inputs);
	CHECK_EQUAL(albums.size(), std::size_t{1});
	CHECK_EQUAL(static_cast<int>(albums[0].identityConfidence),
		static_cast<int>(Confidence::Moderate));
	CHECK(albums[0].flags.empty());
}

// ===========================================================================
// BPM-001
// ===========================================================================

TEST_CASE("BPM-001: a stable estimate agreeing with an existing tag changes nothing") {
	TempoPolicy policy;
	TempoAnalysis analysis;
	analysis.valid = true;
	analysis.bpm = 128.2;
	analysis.stability = 0.95;
	analysis.sectionsAnalysed = 8;
	analysis.character = TempoCharacter::SteadyBeat;

	const TempoDecision decision = policy.decide(analysis, 128.0, 240000);
	CHECK_EQUAL(static_cast<int>(decision.kind), static_cast<int>(TempoDecisionKind::NoChange));
	CHECK(!decision.writesTag());
}

TEST_CASE("BPM-001: a half or double disagreement is surfaced, never auto-corrected") {
	TempoPolicy policy;
	TempoAnalysis analysis;
	analysis.valid = true;
	analysis.bpm = 140.0;
	analysis.stability = 0.98;
	analysis.sectionsAnalysed = 8;
	analysis.character = TempoCharacter::SteadyBeat;

	const TempoDecision decision = policy.decide(analysis, 70.0, 240000);
	CHECK_EQUAL(static_cast<int>(decision.kind), static_cast<int>(TempoDecisionKind::NeedsReview));
	CHECK(!decision.writesTag());
	CHECK(decision.reason.find("factor of two") != std::string::npos);
}

TEST_CASE("BPM-001: a beatless track receives no invented BPM") {
	TempoPolicy policy;
	TempoAnalysis analysis;
	analysis.valid = true;
	analysis.character = TempoCharacter::Beatless;

	const TempoDecision decision = policy.decide(analysis, std::nullopt, 240000);
	CHECK_EQUAL(static_cast<int>(decision.kind), static_cast<int>(TempoDecisionKind::NotApplicable));
	CHECK(!decision.writesTag());
}

TEST_CASE("FN-BPM-02: variable tempo and long-form tracks go to review") {
	TempoPolicy policy;

	TempoAnalysis variable;
	variable.valid = true;
	variable.bpm = 120.0;
	variable.stability = 0.4;
	variable.character = TempoCharacter::VariableTempo;
	CHECK_EQUAL(static_cast<int>(policy.decide(variable, std::nullopt, 240000).kind),
		static_cast<int>(TempoDecisionKind::NeedsReview));

	TempoAnalysis longForm;
	longForm.valid = true;
	longForm.bpm = 128.0;
	longForm.stability = 0.95;
	longForm.character = TempoCharacter::SteadyBeat;
	const TempoDecision decision = policy.decide(longForm, std::nullopt, 90ll * 60 * 1000);
	CHECK_EQUAL(static_cast<int>(decision.kind), static_cast<int>(TempoDecisionKind::NeedsReview));
	CHECK(decision.reason.find("long-form") != std::string::npos);
}

TEST_CASE("BPM-001: a very short track is not analysed") {
	TempoPolicy policy;
	TempoAnalysis analysis;
	analysis.valid = true;
	analysis.bpm = 120.0;

	const TempoDecision decision = policy.decide(analysis, std::nullopt, 5000);
	CHECK_EQUAL(static_cast<int>(decision.kind), static_cast<int>(TempoDecisionKind::NotApplicable));
}

TEST_CASE("FN-BPM-01: an accepted estimate reports half and double alternatives") {
	TempoPolicy policy;
	TempoAnalysis analysis;
	analysis.valid = true;
	analysis.bpm = 128.4;
	analysis.halfTempo = 64.2;
	analysis.doubleTempo = 256.8;
	analysis.stability = 0.95;
	analysis.sectionsAnalysed = 8;
	analysis.character = TempoCharacter::SteadyBeat;

	const TempoDecision decision = policy.decide(analysis, std::nullopt, 240000);
	CHECK_EQUAL(static_cast<int>(decision.kind), static_cast<int>(TempoDecisionKind::Propose));
	CHECK(decision.writesTag());
	// The integer goes into TBPM; the precise value is kept alongside it.
	CHECK_EQUAL(decision.taggedBpm, 128);
	CHECK(decision.preciseBpm > 128.3 && decision.preciseBpm < 128.5);
	// 256.8 exceeds the plausible range, so only the half tempo is offered.
	CHECK_EQUAL(decision.alternatives.size(), std::size_t{1});
}

// ===========================================================================
// LYR-001
// ===========================================================================

TEST_CASE("LYR-001: existing lyrics are preserved by default") {
	LyricsPolicy policy;
	LyricsMatchInput input;
	input.localTitle = "Song";
	input.localArtist = "Artist";

	const LyricsDecision decision = policy.decide(input, {}, true);
	CHECK_EQUAL(static_cast<int>(decision.state), static_cast<int>(LyricsState::ExistingPreserved));
	CHECK(!decision.writesTag());
}

TEST_CASE("LYR-001: no candidates means not_found, never instrumental") {
	LyricsPolicy policy;
	LyricsMatchInput input;
	input.localTitle = "Song";
	input.localArtist = "Artist";

	const LyricsDecision decision = policy.decide(input, {}, false);
	CHECK_EQUAL(static_cast<int>(decision.state), static_cast<int>(LyricsState::NotFound));
	CHECK(decision.reason.find("not evidence that the track is instrumental") != std::string::npos);
}

TEST_CASE("FN-LYR-01: a same-title match alone is insufficient") {
	LyricsPolicy policy;
	LyricsMatchInput input;
	input.localTitle = "Song";
	input.localArtist = "The Right Artist";
	input.localDurationMs = 200000;

	LyricsCandidate candidate;
	candidate.trackName = "Song";
	candidate.artistName = "A Completely Different Artist";
	candidate.durationMs = 200000;
	candidate.plainLyrics = "some words";

	const LyricsDecision decision = policy.decide(input, {candidate}, false);
	CHECK_EQUAL(static_cast<int>(decision.state), static_cast<int>(LyricsState::NeedsReview));
	CHECK(!decision.writesTag());
}

TEST_CASE("FN-LYR-01: a version mismatch is not a confident match") {
	LyricsPolicy policy;
	LyricsMatchInput input;
	input.localTitle = "Song (Extended Mix)";
	input.localArtist = "Artist";
	input.localDurationMs = 400000;

	LyricsCandidate candidate;
	candidate.trackName = "Song (Radio Edit)";
	candidate.artistName = "Artist";
	candidate.durationMs = 400000;
	candidate.plainLyrics = "some words";

	const LyricsDecision decision = policy.decide(input, {candidate}, false);
	CHECK_EQUAL(static_cast<int>(decision.state), static_cast<int>(LyricsState::NeedsReview));
}

TEST_CASE("LYR-001: a provider instrumental flag is respected and distinct from not_found") {
	LyricsPolicy policy;
	LyricsMatchInput input;
	input.localTitle = "Theme";
	input.localArtist = "Artist";
	input.localAlbum = "Album";
	input.localDurationMs = 180000;

	LyricsCandidate candidate;
	candidate.trackName = "Theme";
	candidate.artistName = "Artist";
	candidate.albumName = "Album";
	candidate.durationMs = 180000;
	candidate.instrumental = true;

	const LyricsDecision decision = policy.decide(input, {candidate}, false);
	CHECK_EQUAL(static_cast<int>(decision.state), static_cast<int>(LyricsState::Instrumental));
	CHECK(!decision.writesTag());
}

TEST_CASE("LYR-001: a confident match is accepted with a valid language code") {
	LyricsPolicy policy;
	LyricsMatchInput input;
	input.localTitle = "Song";
	input.localArtist = "Artist";
	input.localAlbum = "Album";
	input.localDurationMs = 200000;

	LyricsCandidate candidate;
	candidate.trackName = "Song";
	candidate.artistName = "Artist";
	candidate.albumName = "Album";
	candidate.durationMs = 200500;   // within tolerance
	candidate.plainLyrics = "the actual lyrics";

	const LyricsDecision decision = policy.decide(input, {candidate}, false);
	CHECK_EQUAL(static_cast<int>(decision.state), static_cast<int>(LyricsState::Found));
	CHECK(decision.writesTag());
	CHECK(LyricsPolicy::isValidLanguageCode(decision.language));
	CHECK_EQUAL(decision.language.size(), std::size_t{3});
}

TEST_CASE("FN-LYR-02: language codes are validated before reaching USLT") {
	CHECK(LyricsPolicy::isValidLanguageCode("eng"));
	CHECK(LyricsPolicy::isValidLanguageCode("afr"));
	CHECK(LyricsPolicy::isValidLanguageCode("XXX"));
	CHECK(!LyricsPolicy::isValidLanguageCode("en"));
	CHECK(!LyricsPolicy::isValidLanguageCode("english"));
	CHECK(!LyricsPolicy::isValidLanguageCode("e1g"));
	CHECK(!LyricsPolicy::isValidLanguageCode(""));
}

ML_TEST_MAIN("core policies")
