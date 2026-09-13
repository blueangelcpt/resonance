// SPDX-License-Identifier: GPL-3.0-or-later
// CAT-001 / SAFE-001 / SAFE-002 / FN-SAFE-02 / FN-TAG-01 / FN-TAG-02:
// end-to-end read, write and verify against generated fixtures.
//
// Fixtures are synthetic: a real MPEG frame header with a deterministic payload,
// plus tags this project writes with TagLib. No real music enters the
// repository, as the FRD requires.
#include "TestHarness.hpp"

#include "mlapp/Library.hpp"
#include "mlcore/TagPolicy.hpp"
#include "mlinfra/Hashing.hpp"
#include "mlinfra/Mp3Container.hpp"
#include "mlinfra/TagReader.hpp"
#include "mlinfra/TagWriter.hpp"

#include <taglib/attachedpictureframe.h>
#include <taglib/id3v1tag.h>
#include <taglib/id3v2tag.h>
#include <taglib/mpegfile.h>
#include <taglib/privateframe.h>
#include <taglib/textidentificationframe.h>
#include <taglib/unknownframe.h>
#include <taglib/unsynchronizedlyricsframe.h>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>

namespace fs = std::filesystem;
using namespace ml;

namespace {

class TempDirectory {
public:
	TempDirectory() {
		std::random_device rd;
		m_path = fs::temp_directory_path()
			/ ("resonance-preserve-" + std::to_string(rd()) + "-" + std::to_string(rd()));
		fs::create_directories(m_path);
	}
	~TempDirectory() {
		std::error_code ec;
		fs::remove_all(m_path, ec);
	}

	TempDirectory(const TempDirectory&) = delete;
	TempDirectory& operator=(const TempDirectory&) = delete;

	const fs::path& path() const { return m_path; }

private:
	fs::path m_path;
};

/// A deterministic MPEG 1 Layer III frame: valid header, varying payload so a
/// byte-level change anywhere in the stream alters the hash.
std::vector<std::uint8_t> mpegFrame(int index) {
	std::vector<std::uint8_t> frame(417, 0);
	frame[0] = 0xFF;
	frame[1] = 0xFB;
	frame[2] = 0x90;
	frame[3] = 0x00;
	for (std::size_t i = 4; i < frame.size(); ++i) {
		frame[i] = static_cast<std::uint8_t>((index * 31 + static_cast<int>(i) * 7) & 0xFF);
	}
	return frame;
}

/// Writes a bare MP3 (audio only), then lets TagLib attach the tags, so the
/// fixture is a file this toolchain genuinely produces.
fs::path makeFixture(const fs::path& directory, const std::string& name, int frameCount = 40) {
	fs::create_directories(directory);
	const fs::path path = directory / name;

	{
		std::ofstream out(path, std::ios::binary);
		for (int i = 0; i < frameCount; ++i) {
			const auto frame = mpegFrame(i);
			out.write(reinterpret_cast<const char*>(frame.data()),
				static_cast<std::streamsize>(frame.size()));
		}
	}
	return path;
}

/// Attaches a representative tag set: ordinary text, an unknown frame, a
/// private frame, a picture, ReplayGain and lyrics.
void attachTags(const fs::path& path) {
	TagLib::MPEG::File file(path.string().c_str(), false);
	TagLib::ID3v2::Tag* tag = file.ID3v2Tag(true);

	const auto addText = [&](const char* id, const std::string& value) {
		auto* frame = new TagLib::ID3v2::TextIdentificationFrame(
			TagLib::ByteVector(id, 4), TagLib::String::UTF8);
		frame->setText(TagLib::String(value, TagLib::String::UTF8));
		tag->addFrame(frame);
	};

	addText("TIT2", "Fixture Title");
	addText("TPE1", "Fixture Artist");
	addText("TPE2", "Fixture Album Artist");
	addText("TALB", "Fixture Album");
	addText("TRCK", "3/12");
	addText("TCON", "Electronic");
	addText("TCOM", "A Composer");

	// A frame this application does not interpret. It must survive a rewrite.
	auto* unknown = new TagLib::ID3v2::UnknownFrame(
		TagLib::ByteVector("ZZZZ\x00\x00\x00\x08\x00\x00opaque42", 18));
	tag->addFrame(unknown);

	// A public identifier that the selective privacy profile must keep.
	auto* publicPrivate = new TagLib::ID3v2::PrivateFrame();
	publicPrivate->setOwner("http://musicbrainz.org");
	publicPrivate->setData(TagLib::ByteVector("mbid-payload", 12));
	tag->addFrame(publicPrivate);

	// A purchase record the selective profile must remove.
	auto* purchase = new TagLib::ID3v2::PrivateFrame();
	purchase->setOwner("com.apple.iTunes:Account");
	purchase->setData(TagLib::ByteVector("account-payload", 15));
	tag->addFrame(purchase);

	// ReplayGain, which the gain policy removes.
	auto* gain = new TagLib::ID3v2::UserTextIdentificationFrame(TagLib::String::UTF8);
	gain->setDescription("replaygain_track_gain");
	gain->setText("-6.42 dB");
	tag->addFrame(gain);

	// Gapless information, which it must preserve.
	auto* gapless = new TagLib::ID3v2::UserTextIdentificationFrame(TagLib::String::UTF8);
	gapless->setDescription("iTunSMPB");
	gapless->setText("00000000 00000210 000008A4");
	tag->addFrame(gapless);

	auto* lyrics = new TagLib::ID3v2::UnsynchronizedLyricsFrame(TagLib::String::UTF8);
	lyrics->setLanguage(TagLib::ByteVector("eng", 3));
	lyrics->setText("existing lyrics that must not be replaced");
	tag->addFrame(lyrics);

	// A small picture, so the artwork path has something to preserve.
	auto* picture = new TagLib::ID3v2::AttachedPictureFrame();
	picture->setType(TagLib::ID3v2::AttachedPictureFrame::BackCover);
	picture->setMimeType("image/jpeg");
	picture->setPicture(TagLib::ByteVector("\xFF\xD8\xFF\xE0 not a real jpeg", 24));
	tag->addFrame(picture);

	file.save(TagLib::MPEG::File::ID3v2, TagLib::File::StripOthers, TagLib::ID3v2::v3);
}

/// PathGuard owns a mutex and is deliberately neither copyable nor movable, so
/// it is configured in place rather than returned by value.
void configureGuard(PathGuard& guard, const fs::path& source, const fs::path& output) {
	(void)guard.addProtectedRoot(source);
	(void)guard.addOutputRoot(output);
}

} // namespace

// ===========================================================================
// Reading
// ===========================================================================

TEST_CASE("FN-TAG-01: the source's ID3 version is reported, never translated on read") {
	TempDirectory temp;
	const fs::path source = makeFixture(temp.path() / "music", "track.mp3");
	attachTags(source);

	auto read = TagReader::read(source);
	CHECK(read.ok());
	// The fixture was written as v2.3 and must be reported as v2.3.
	CHECK_EQUAL(static_cast<int>(read.value().snapshot.primaryContainer),
		static_cast<int>(TagContainer::Id3v2_3));
}

TEST_CASE("CAT-001: an uninterpreted frame's payload is retained") {
	TempDirectory temp;
	const fs::path source = makeFixture(temp.path() / "music", "track.mp3");
	attachTags(source);

	TagReadOptions options;
	options.retainFramePayloads = true;
	auto read = TagReader::read(source, options);
	CHECK(read.ok());

	bool foundUnknown = false;
	for (const auto& frame : read.value().snapshot.frames) {
		if (frame.id != "ZZZZ") continue;
		foundUnknown = true;
		CHECK_MESSAGE(!frame.binary.empty(), "an uninterpreted frame must keep its payload");
	}
	CHECK_MESSAGE(foundUnknown, "the unknown frame was not catalogued at all");
}

TEST_CASE("FN-TAG-02: the raw inventory and the tag library agree on this fixture") {
	TempDirectory temp;
	const fs::path source = makeFixture(temp.path() / "music", "track.mp3");
	attachTags(source);

	auto read = TagReader::read(source);
	CHECK(read.ok());

	const auto discrepancies = TagReader::crossCheck(read.value().rawFrames, read.value().snapshot);
	for (const auto& discrepancy : discrepancies) {
		CHECK_MESSAGE(false, "cross-check discrepancy: " + discrepancy);
	}
	CHECK(!read.value().rawFrames.empty());
}

TEST_CASE("The audio payload hash covers the stream and excludes the tags") {
	TempDirectory temp;
	const fs::path bare = makeFixture(temp.path() / "music", "bare.mp3");

	auto before = Mp3Container::readLayout(bare);
	CHECK(before.ok());
	auto bareAudio = hashFileRange(bare, before.value().audioOffset, before.value().audioLength);
	CHECK(bareAudio.ok());

	// Adding a tag changes the file but must not change the audio hash.
	attachTags(bare);

	auto after = Mp3Container::readLayout(bare);
	CHECK(after.ok());
	auto taggedAudio = hashFileRange(bare, after.value().audioOffset, after.value().audioLength);
	CHECK(taggedAudio.ok());

	CHECK_MESSAGE(bareAudio.value() == taggedAudio.value(),
		"the audio-range hash changed when only tags were added");
	CHECK(after.value().audioOffset > before.value().audioOffset);
}

// ===========================================================================
// Writing
// ===========================================================================

TEST_CASE("SAFE-001: the writer refuses a destination inside the protected root") {
	TempDirectory temp;
	const fs::path sourceRoot = temp.path() / "music";
	const fs::path outputRoot = temp.path() / "output";
	fs::create_directories(outputRoot);

	const fs::path source = makeFixture(sourceRoot, "track.mp3");
	attachTags(source);

	PathGuard guard;
	configureGuard(guard, sourceRoot, outputRoot);

	TagWriteRequest request;
	request.bpm = 128;

	auto written = TagWriter::writeToNewFile(source, sourceRoot / "copy.mp3", request, guard);
	CHECK(!written.ok());
	CHECK_EQUAL(static_cast<int>(written.error().code),
		static_cast<int>(ErrorCode::ProtectedRootViolation));
	CHECK(!fs::exists(sourceRoot / "copy.mp3"));
}

TEST_CASE("SAFE-001: writing leaves the source byte-identical") {
	TempDirectory temp;
	const fs::path sourceRoot = temp.path() / "music";
	const fs::path outputRoot = temp.path() / "output";
	fs::create_directories(outputRoot);

	const fs::path source = makeFixture(sourceRoot, "track.mp3");
	attachTags(source);

	auto originalHash = hashFile(source);
	CHECK(originalHash.ok());

	PathGuard guard;
	configureGuard(guard, sourceRoot, outputRoot);

	TagWriteRequest request;
	request.bpm = 128;
	request.lyrics = std::string("replacement lyrics");

	auto written = TagWriter::writeToNewFile(source, outputRoot / "out.mp3", request, guard);
	CHECK(written.ok());

	auto afterHash = hashFile(source);
	CHECK(afterHash.ok());
	CHECK_MESSAGE(originalHash.value() == afterHash.value(),
		"the source file changed during a write to the output root");
}

TEST_CASE("The written MPEG payload is byte-identical to the source's") {
	TempDirectory temp;
	const fs::path sourceRoot = temp.path() / "music";
	const fs::path outputRoot = temp.path() / "output";
	fs::create_directories(outputRoot);

	const fs::path source = makeFixture(sourceRoot, "track.mp3");
	attachTags(source);

	PathGuard guard;
	configureGuard(guard, sourceRoot, outputRoot);

	TagWriteRequest request;
	request.bpm = 140;

	auto written = TagWriter::writeToNewFile(source, outputRoot / "out.mp3", request, guard);
	CHECK(written.ok());
	CHECK_MESSAGE(written.value().audioPreserved(),
		"the writer reported the audio was not preserved");
	CHECK_EQUAL(written.value().writtenAudioSha256, written.value().sourceAudioSha256);

	// Independently: hash the output's audio range from disk.
	auto layout = Mp3Container::readLayout(outputRoot / "out.mp3");
	CHECK(layout.ok());
	auto onDisk = hashFileRange(outputRoot / "out.mp3", layout.value().audioOffset,
		layout.value().audioLength);
	CHECK(onDisk.ok());
	CHECK_EQUAL(onDisk.value(), written.value().sourceAudioSha256);
}

TEST_CASE("PRIV-001 and GAIN-001 applied together: intended removals, nothing else") {
	TempDirectory temp;
	const fs::path sourceRoot = temp.path() / "music";
	const fs::path outputRoot = temp.path() / "output";
	fs::create_directories(outputRoot);

	const fs::path source = makeFixture(sourceRoot, "track.mp3");
	attachTags(source);

	TagReadOptions options;
	options.retainFramePayloads = true;
	options.hashAudio = true;
	auto before = TagReader::read(source, options);
	CHECK(before.ok());

	const PrivacyPolicy privacy;
	const GainPolicy gain;
	const PrivacyPreview privacyPreview = privacy.evaluate(before.value().snapshot);
	const GainPreview gainPreview = gain.evaluate(before.value().snapshot);

	// The purchase frame is removed; the MusicBrainz identifier is not.
	CHECK_EQUAL(privacyPreview.removeCount, std::size_t{1});
	// ReplayGain is removed; iTunSMPB is preserved.
	CHECK_EQUAL(gainPreview.removeCount, std::size_t{1});
	CHECK(!gainPreview.blocked());

	TagWriteRequest request;
	for (const auto& decision : privacyPreview.decisions) request.decisions.push_back(decision);
	for (const auto& decision : gainPreview.decisions) request.decisions.push_back(decision);

	const fs::path destination = outputRoot / "out.mp3";
	PathGuard guard;
	configureGuard(guard, sourceRoot, outputRoot);
	auto written = TagWriter::writeToNewFile(source, destination, request, guard);
	CHECK(written.ok());
	CHECK(written.value().audioPreserved());

	auto after = TagReader::read(destination, options);
	CHECK(after.ok());

	const TagSnapshot& result = after.value().snapshot;

	// Removed.
	bool purchaseSurvived = false;
	bool replayGainSurvived = false;
	// Preserved.
	bool musicBrainzSurvived = false;
	bool gaplessSurvived = false;
	bool unknownSurvived = false;
	bool composerSurvived = false;
	bool backCoverSurvived = false;
	bool lyricsSurvived = false;

	for (const auto& frame : result.frames) {
		if (frame.id == "PRIV" && frame.owner == "com.apple.iTunes:Account") purchaseSurvived = true;
		if (frame.id == "PRIV" && frame.owner.find("musicbrainz") != std::string::npos) {
			musicBrainzSurvived = true;
		}
		if (frame.id == "TXXX" && frame.description == "replaygain_track_gain") replayGainSurvived = true;
		if (frame.id == "TXXX" && frame.description == "iTunSMPB") gaplessSurvived = true;
		if (frame.id == "ZZZZ") unknownSurvived = true;
		if (frame.id == "TCOM") composerSurvived = true;
		if (frame.id == "USLT") lyricsSurvived = true;
	}
	for (const auto& picture : result.pictures) {
		if (picture.type == PictureType::BackCover) backCoverSurvived = true;
	}

	CHECK_MESSAGE(!purchaseSurvived, "the purchase frame was not removed");
	CHECK_MESSAGE(!replayGainSurvived, "the ReplayGain field was not removed");
	CHECK_MESSAGE(musicBrainzSurvived, "the MusicBrainz identifier was wrongly removed");
	CHECK_MESSAGE(gaplessSurvived, "gapless information was wrongly removed");
	CHECK_MESSAGE(unknownSurvived, "the uninterpreted frame was lost in the rewrite");
	CHECK_MESSAGE(composerSurvived, "an unrelated text frame was lost");
	CHECK_MESSAGE(lyricsSurvived, "existing lyrics were lost");
	CHECK_MESSAGE(backCoverSurvived, "a non-front-cover picture role was lost");
}

TEST_CASE("The preservation comparison reports no loss for a planned change") {
	TempDirectory temp;
	const fs::path sourceRoot = temp.path() / "music";
	const fs::path outputRoot = temp.path() / "output";
	fs::create_directories(outputRoot);

	const fs::path source = makeFixture(sourceRoot, "track.mp3");
	attachTags(source);

	TagReadOptions options;
	options.retainFramePayloads = true;
	options.hashAudio = true;

	auto before = TagReader::read(source, options);
	CHECK(before.ok());

	TagWriteRequest request;
	request.bpm = 128;

	const fs::path destination = outputRoot / "out.mp3";
	PathGuard guard;
	configureGuard(guard, sourceRoot, outputRoot);
	auto written = TagWriter::writeToNewFile(source, destination, request, guard);
	CHECK(written.ok());

	auto after = TagReader::read(destination, options);
	CHECK(after.ok());

	const auto report = TagReader::comparePreservation(before.value(), after.value(),
		written.value().intentionallyChangedKeys);

	for (const auto& lost : report.lostFrames) {
		CHECK_MESSAGE(false, "unexpectedly lost: " + lost);
	}
	for (const auto& altered : report.alteredFrames) {
		CHECK_MESSAGE(false, "unexpectedly altered: " + altered);
	}
	CHECK(report.preserved());
	CHECK(report.audioUnchanged);
}

TEST_CASE("FN-OPT-01: the padding budget is applied exactly") {
	TempDirectory temp;
	const fs::path sourceRoot = temp.path() / "music";
	const fs::path outputRoot = temp.path() / "output";
	fs::create_directories(outputRoot);

	const fs::path source = makeFixture(sourceRoot, "track.mp3");
	attachTags(source);

	TagWriteRequest request;
	request.targetPaddingBytes = 2048;

	const fs::path destination = outputRoot / "out.mp3";
	PathGuard guard;
	configureGuard(guard, sourceRoot, outputRoot);
	auto written = TagWriter::writeToNewFile(source, destination, request, guard);
	CHECK(written.ok());
	CHECK_EQUAL(written.value().newPaddingBytes, std::int64_t{2048});

	// Re-read the output and confirm the padding is really there.
	Mp3Layout layout = Mp3Container::readLayout(destination).valueOr(Mp3Layout{});
	auto frames = Mp3Container::readRawFrames(destination, layout);
	CHECK(frames.ok());
	CHECK_EQUAL(layout.id3v2PaddingBytes, std::int64_t{2048});
}

TEST_CASE("FN-OPT-01: a different padding budget is honoured, and frames still parse") {
	TempDirectory temp;
	const fs::path sourceRoot = temp.path() / "music";
	const fs::path outputRoot = temp.path() / "output";
	fs::create_directories(outputRoot);

	const fs::path source = makeFixture(sourceRoot, "track.mp3");
	attachTags(source);

	TagWriteRequest request;
	request.targetPaddingBytes = 0;

	const fs::path destination = outputRoot / "nopad.mp3";
	PathGuard guard;
	configureGuard(guard, sourceRoot, outputRoot);
	auto written = TagWriter::writeToNewFile(source, destination, request, guard);
	CHECK(written.ok());
	CHECK_EQUAL(written.value().newPaddingBytes, std::int64_t{0});

	// With no padding, a frame whose payload ends in 0x00 would be truncated by a
	// trailing-zero trim. The frames must still read back intact.
	auto reread = TagReader::read(destination);
	CHECK(reread.ok());
	bool unknownSurvived = false;
	for (const auto& frame : reread.value().snapshot.frames) {
		if (frame.id == "ZZZZ") unknownSurvived = true;
	}
	CHECK(unknownSurvived);
	for (const auto& warning : reread.value().snapshot.readWarnings) {
		CHECK_MESSAGE(warning.find("truncated") == std::string::npos,
			"a frame was truncated: " + warning);
	}
}

TEST_CASE("FN-TAG-01: the output keeps the source's ID3 version by default") {
	TempDirectory temp;
	const fs::path sourceRoot = temp.path() / "music";
	const fs::path outputRoot = temp.path() / "output";
	fs::create_directories(outputRoot);

	const fs::path source = makeFixture(sourceRoot, "track.mp3");
	attachTags(source);   // written as v2.3

	TagWriteRequest request;   // outputVersion left as Unknown == preserve
	request.bpm = 128;

	const fs::path destination = outputRoot / "out.mp3";
	PathGuard guard;
	configureGuard(guard, sourceRoot, outputRoot);
	auto written = TagWriter::writeToNewFile(source, destination, request, guard);
	CHECK(written.ok());

	auto after = TagReader::read(destination);
	CHECK(after.ok());
	CHECK_MESSAGE(after.value().snapshot.primaryContainer == TagContainer::Id3v2_3,
		"the tag library's v2.4 default silently translated the file");
}

TEST_CASE("SAFE-002: a copy is a real copy, never a hard link") {
	TempDirectory temp;
	const fs::path sourceRoot = temp.path() / "music";
	const fs::path outputRoot = temp.path() / "output";
	fs::create_directories(outputRoot);

	const fs::path source = makeFixture(sourceRoot, "track.mp3");
	attachTags(source);

	TagWriteRequest request;
	request.bpm = 128;

	const fs::path destination = outputRoot / "out.mp3";
	PathGuard guard;
	configureGuard(guard, sourceRoot, outputRoot);
	auto written = TagWriter::writeToNewFile(source, destination, request, guard);
	CHECK(written.ok());

	const auto sourceIdentity = PathGuard::identityOf(source);
	const auto destinationIdentity = PathGuard::identityOf(destination);
	CHECK(sourceIdentity.has_value());
	CHECK(destinationIdentity.has_value());
	CHECK_MESSAGE(!sourceIdentity->sameFile(*destinationIdentity),
		"the output is the same file as the source");

	const auto links = PathGuard::hardLinkCount(destination);
	CHECK(links.has_value());
	CHECK_EQUAL(*links, std::uintmax_t{1});
}

TEST_CASE("A write to a file with no MPEG payload is refused") {
	TempDirectory temp;
	const fs::path sourceRoot = temp.path() / "music";
	const fs::path outputRoot = temp.path() / "output";
	fs::create_directories(sourceRoot);
	fs::create_directories(outputRoot);

	const fs::path source = sourceRoot / "notaudio.mp3";
	{
		std::ofstream out(source, std::ios::binary);
		out << "this is not an MP3 at all";
	}

	TagWriteRequest request;
	request.bpm = 128;

	PathGuard guard;
	configureGuard(guard, sourceRoot, outputRoot);
	auto written = TagWriter::writeToNewFile(source, outputRoot / "out.mp3", request, guard);
	CHECK(!written.ok());
	CHECK_MESSAGE(!fs::exists(outputRoot / "out.mp3"),
		"a refused write must not leave an output behind");
}

// ===========================================================================
// The engine, end to end
// ===========================================================================

TEST_CASE("UI-001: scan, group, plan and export run through the shared engine") {
	TempDirectory temp;
	const fs::path sourceRoot = temp.path() / "music";
	const fs::path outputRoot = temp.path() / "output";
	const fs::path dataRoot = temp.path() / "data";
	fs::create_directories(outputRoot);
	fs::create_directories(dataRoot);

	// Three tracks of one album, tagged so the naming template can place them.
	for (int i = 1; i <= 3; ++i) {
		const fs::path track = makeFixture(sourceRoot / "Fixture Album Artist" / "Fixture Album",
			"track" + std::to_string(i) + ".mp3");
		attachTags(track);

		TagLib::MPEG::File file(track.string().c_str(), false);
		TagLib::ID3v2::Tag* tag = file.ID3v2Tag(true);
		tag->removeFrames("TRCK");
		auto* frame = new TagLib::ID3v2::TextIdentificationFrame("TRCK", TagLib::String::UTF8);
		frame->setText(TagLib::String(std::to_string(i) + "/3", TagLib::String::UTF8));
		tag->addFrame(frame);
		file.save(TagLib::MPEG::File::ID3v2, TagLib::File::StripOthers, TagLib::ID3v2::v3);
	}

	// Record every source hash before the engine touches anything.
	std::map<std::string, std::string> hashesBefore;
	for (const auto& entry : fs::recursive_directory_iterator(sourceRoot)) {
		if (!entry.is_regular_file()) continue;
		auto hash = hashFile(entry.path());
		CHECK(hash.ok());
		hashesBefore[entry.path().string()] = hash.value();
	}
	CHECK_EQUAL(hashesBefore.size(), std::size_t{3});

	LibraryConfig config;
	config.dataDirectory = dataRoot;
	config.sourceRoots.push_back(sourceRoot);
	config.outputRoot = outputRoot;
	config.offline = true;   // no network in the test suite

	Library library;
	auto opened = library.open(std::move(config));
	CHECK_MESSAGE(opened.ok(), opened.ok() ? "" : opened.error().describe());
	if (!opened.ok()) return;

	auto scan = library.scan();
	CHECK(scan.ok());
	CHECK_EQUAL(scan.value().filesSeen, std::int64_t{3});
	CHECK_EQUAL(scan.value().filesAdded, std::int64_t{3});
	CHECK_EQUAL(scan.value().filesUnreadable, std::int64_t{0});

	auto albums = library.resolveAlbums();
	CHECK(albums.ok());
	CHECK_EQUAL(albums.value(), std::int64_t{1});

	auto plan = library.plan();
	CHECK_MESSAGE(plan.ok(), plan.ok() ? "" : plan.error().describe());
	if (!plan.ok()) return;

	const auto summary = plan.value().summarise();
	CHECK_EQUAL(summary.total, std::size_t{3});
	CHECK_MESSAGE(summary.writable == 3,
		"expected 3 writable plans, got " + std::to_string(summary.writable));
	CHECK(plan.value().collisions.empty());

	// The destination follows the confirmed template.
	for (const auto& file : plan.value().files) {
		CHECK_EQUAL(file.destinationRelativePath.rfind("Fixture Album Artist/Fixture Album/",
			0), std::size_t{0});
	}

	auto exported = library.exportCopies(plan.value().id);
	CHECK(exported.ok());
	CHECK_MESSAGE(exported.value().written == 3,
		"wrote " + std::to_string(exported.value().written) + " of 3; failures: "
			+ (exported.value().failures.empty() ? "none" : exported.value().failures.front()));
	CHECK_EQUAL(exported.value().failed, std::int64_t{0});

	// SAFE-001: every source file is still byte-identical.
	for (const auto& [path, hash] : hashesBefore) {
		auto now = hashFile(path);
		CHECK(now.ok());
		CHECK_MESSAGE(now.value() == hash, "source changed during the run: " + path);
	}

	// The outputs exist, under the template path.
	CHECK(fs::exists(outputRoot / "Fixture Album Artist" / "Fixture Album"
		/ "01. Fixture Artist - Fixture Title.mp3"));

	// Verification agrees.
	auto verified = library.verify(plan.value().id);
	CHECK(verified.ok());
	CHECK_EQUAL(verified.value(), std::int64_t{3});
}

TEST_CASE("Idempotence: a second export writes nothing and overwrites nothing") {
	TempDirectory temp;
	const fs::path sourceRoot = temp.path() / "music";
	const fs::path outputRoot = temp.path() / "output";
	const fs::path dataRoot = temp.path() / "data";
	fs::create_directories(outputRoot);
	fs::create_directories(dataRoot);

	const fs::path track = makeFixture(sourceRoot / "Fixture Album Artist" / "Fixture Album",
		"track.mp3");
	attachTags(track);

	LibraryConfig config;
	config.dataDirectory = dataRoot;
	config.sourceRoots.push_back(sourceRoot);
	config.outputRoot = outputRoot;
	config.offline = true;

	Library library;
	CHECK(library.open(std::move(config)).ok());
	CHECK(library.scan().ok());
	CHECK(library.resolveAlbums().ok());

	auto firstPlan = library.plan();
	CHECK(firstPlan.ok());
	auto firstExport = library.exportCopies(firstPlan.value().id);
	CHECK(firstExport.ok());
	CHECK_EQUAL(firstExport.value().written, std::int64_t{1});

	const fs::path output = outputRoot / "Fixture Album Artist" / "Fixture Album"
		/ "03. Fixture Artist - Fixture Title.mp3";
	CHECK(fs::exists(output));
	auto firstHash = hashFile(output);
	CHECK(firstHash.ok());

	// A second run: the destination already exists and must not be overwritten.
	auto secondPlan = library.plan();
	CHECK(secondPlan.ok());
	auto secondExport = library.exportCopies(secondPlan.value().id);
	CHECK(secondExport.ok());
	CHECK_EQUAL(secondExport.value().written, std::int64_t{0});
	CHECK_EQUAL(secondExport.value().skipped, std::int64_t{1});

	auto secondHash = hashFile(output);
	CHECK(secondHash.ok());
	CHECK_MESSAGE(firstHash.value() == secondHash.value(),
		"the existing output was rewritten on the second run");
}

TEST_CASE("SAFE-001: an output root inside the source is refused when opening") {
	TempDirectory temp;
	const fs::path sourceRoot = temp.path() / "music";
	fs::create_directories(sourceRoot / "output");

	LibraryConfig config;
	config.dataDirectory = temp.path() / "data";
	config.sourceRoots.push_back(sourceRoot);
	config.outputRoot = sourceRoot / "output";

	Library library;
	auto status = library.open(std::move(config));
	CHECK(!status.ok());
	CHECK_EQUAL(static_cast<int>(status.error().code),
		static_cast<int>(ErrorCode::ProtectedRootViolation));
}

TEST_CASE("SAFE-001: a catalogue inside the source is refused when opening") {
	TempDirectory temp;
	const fs::path sourceRoot = temp.path() / "music";
	fs::create_directories(sourceRoot);

	LibraryConfig config;
	config.dataDirectory = sourceRoot / "catalogue";   // inside the music
	config.sourceRoots.push_back(sourceRoot);

	Library library;
	auto status = library.open(std::move(config));
	CHECK(!status.ok());
	CHECK_EQUAL(static_cast<int>(status.error().code),
		static_cast<int>(ErrorCode::ProtectedRootViolation));
}

TEST_CASE("FN-CLI-02: planning without an output root is refused") {
	TempDirectory temp;
	const fs::path sourceRoot = temp.path() / "music";
	const fs::path track = makeFixture(sourceRoot / "A" / "B", "t.mp3");
	attachTags(track);

	LibraryConfig config;
	config.dataDirectory = temp.path() / "data";
	config.sourceRoots.push_back(sourceRoot);
	// outputRoot deliberately empty.

	Library library;
	CHECK(library.open(std::move(config)).ok());
	CHECK(library.scan().ok());

	auto plan = library.plan();
	CHECK(!plan.ok());
	CHECK_EQUAL(static_cast<int>(plan.error().code), static_cast<int>(ErrorCode::InvalidArgument));
}

TEST_CASE("NAME-002: two files planned onto one destination do not both get written") {
	TempDirectory temp;
	const fs::path sourceRoot = temp.path() / "music";
	const fs::path outputRoot = temp.path() / "output";
	const fs::path dataRoot = temp.path() / "data";
	fs::create_directories(outputRoot);
	fs::create_directories(dataRoot);

	// Two files in different folders with identical tags: the template maps both
	// to the same destination.
	for (const char* folder : {"discOne", "discTwo"}) {
		const fs::path track = makeFixture(sourceRoot / folder, "track.mp3");
		attachTags(track);
	}

	LibraryConfig config;
	config.dataDirectory = dataRoot;
	config.sourceRoots.push_back(sourceRoot);
	config.outputRoot = outputRoot;
	config.offline = true;

	Library library;
	CHECK(library.open(std::move(config)).ok());
	CHECK(library.scan().ok());
	CHECK(library.resolveAlbums().ok());

	auto plan = library.plan();
	CHECK(plan.ok());

	const auto summary = plan.value().summarise();
	CHECK_MESSAGE(summary.writable <= 1,
		"both colliding files were planned as writable");
	CHECK_MESSAGE(!plan.value().collisions.empty(),
		"the collision was not reported");

	auto exported = library.exportCopies(plan.value().id);
	CHECK(exported.ok());
	CHECK_MESSAGE(exported.value().written <= 1, "a collision was written twice");
}

ML_TEST_MAIN("preservation and engine")
