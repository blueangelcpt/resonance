// SPDX-License-Identifier: GPL-3.0-or-later
// Infrastructure tests: SHA-256 vectors, path protection (SAFE-001/002),
// container parsing against malformed input, the image pipeline (ART-001),
// JSON hardening and the catalogue schema.
#include "TestHarness.hpp"

#include "mlcore/Text.hpp"
#include "mlinfra/AudioAnalysis.hpp"
#include "mlinfra/Hashing.hpp"
#include "mlinfra/ImagePipeline.hpp"
#include "mlinfra/Json.hpp"
#include "mlinfra/Mp3Container.hpp"
#include "mlinfra/PathGuard.hpp"
#include "mlinfra/Providers.hpp"
#include "mlinfra/Sqlite.hpp"

#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>

namespace fs = std::filesystem;
using namespace ml;

namespace {

/// A scratch directory removed on destruction, so a failing test cannot leave
/// state behind for the next one.
class TempDirectory {
public:
	TempDirectory() {
		std::random_device rd;
		m_path = fs::temp_directory_path()
			/ ("resonance-test-" + std::to_string(rd()) + "-" + std::to_string(rd()));
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

void writeFile(const fs::path& path, const std::vector<std::uint8_t>& bytes) {
	fs::create_directories(path.parent_path());
	std::ofstream out(path, std::ios::binary);
	out.write(reinterpret_cast<const char*>(bytes.data()),
		static_cast<std::streamsize>(bytes.size()));
}

/// Builds a syncsafe 28-bit size field.
void appendSyncsafe(std::vector<std::uint8_t>& out, std::uint32_t value) {
	out.push_back(static_cast<std::uint8_t>((value >> 21) & 0x7F));
	out.push_back(static_cast<std::uint8_t>((value >> 14) & 0x7F));
	out.push_back(static_cast<std::uint8_t>((value >> 7) & 0x7F));
	out.push_back(static_cast<std::uint8_t>(value & 0x7F));
}

/// A valid MPEG 1 Layer III, 128 kbps, 44100 Hz, stereo frame header plus a
/// zero-filled payload. Not decodable music, but a real frame boundary.
std::vector<std::uint8_t> mpegFrame() {
	std::vector<std::uint8_t> frame(417, 0);
	frame[0] = 0xFF;
	frame[1] = 0xFB;   // MPEG1 Layer III, no CRC
	frame[2] = 0x90;   // 128 kbps, 44100 Hz, no padding
	frame[3] = 0x00;   // stereo
	return frame;
}

/// A synthetic MP3: ID3v2.3 tag with one text frame, then several MPEG frames.
/// Synthetic by construction -- no real music enters the repository.
std::vector<std::uint8_t> syntheticMp3(int frameCount = 8, std::uint32_t padding = 100) {
	std::vector<std::uint8_t> frameData;

	// One TIT2 frame: id(4) size(4) flags(2) encoding(1) text
	const std::string title = "Synthetic Fixture";
	frameData.insert(frameData.end(), {'T', 'I', 'T', '2'});
	const std::uint32_t payloadSize = static_cast<std::uint32_t>(title.size() + 1);
	frameData.push_back(static_cast<std::uint8_t>((payloadSize >> 24) & 0xFF));
	frameData.push_back(static_cast<std::uint8_t>((payloadSize >> 16) & 0xFF));
	frameData.push_back(static_cast<std::uint8_t>((payloadSize >> 8) & 0xFF));
	frameData.push_back(static_cast<std::uint8_t>(payloadSize & 0xFF));
	frameData.push_back(0);
	frameData.push_back(0);
	frameData.push_back(0);   // ISO-8859-1
	frameData.insert(frameData.end(), title.begin(), title.end());

	std::vector<std::uint8_t> out;
	out.insert(out.end(), {'I', 'D', '3', 3, 0, 0});
	appendSyncsafe(out, static_cast<std::uint32_t>(frameData.size()) + padding);
	out.insert(out.end(), frameData.begin(), frameData.end());
	out.insert(out.end(), padding, 0);

	for (int i = 0; i < frameCount; ++i) {
		const auto frame = mpegFrame();
		out.insert(out.end(), frame.begin(), frame.end());
	}
	return out;
}

} // namespace

// ===========================================================================
// SHA-256
// ===========================================================================

TEST_CASE("SHA-256 matches the published vectors") {
	CHECK_EQUAL(Sha256::hashString(""),
		std::string("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
	CHECK_EQUAL(Sha256::hashString("abc"),
		std::string("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
	CHECK_EQUAL(Sha256::hashString(
		"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"),
		std::string("248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"));

	// One million 'a', fed in chunks to exercise the buffering path.
	Sha256 hasher;
	const std::string chunk(1000, 'a');
	for (int i = 0; i < 1000; ++i) hasher.update(chunk);
	CHECK_EQUAL(hasher.finishHex(),
		std::string("cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"));
}

TEST_CASE("configHash distinguishes differently-split component lists") {
	// Without length prefixing, ["ab","c"] and ["a","bc"] would hash alike and a
	// configuration change could be missed.
	CHECK(configHash({"ab", "c"}) != configHash({"a", "bc"}));
	CHECK_EQUAL(configHash({"a", "b"}), configHash({"a", "b"}));
}

TEST_CASE("hashFileRange refuses a range longer than the file") {
	TempDirectory temp;
	const fs::path path = temp.path() / "short.bin";
	writeFile(path, {1, 2, 3, 4});

	auto result = hashFileRange(path, 0, 1000);
	CHECK(!result.ok());
	CHECK_EQUAL(static_cast<int>(result.error().code),
		static_cast<int>(ErrorCode::VerificationFailed));
}

// ===========================================================================
// SAFE-001 / SAFE-002: path protection
// ===========================================================================

TEST_CASE("SAFE-001: a write inside a protected root is refused") {
	TempDirectory temp;
	const fs::path source = temp.path() / "music";
	const fs::path output = temp.path() / "output";
	fs::create_directories(source / "album");
	fs::create_directories(output);

	PathGuard guard;
	CHECK(guard.addProtectedRoot(source).ok());
	CHECK(guard.addOutputRoot(output).ok());

	const GuardDecision decision = guard.checkWrite(source / "album" / "track.mp3");
	CHECK(!decision.allowed());
	CHECK_EQUAL(static_cast<int>(decision.verdict),
		static_cast<int>(GuardVerdict::InsideProtectedRoot));
}

TEST_CASE("SAFE-001: a sibling directory with a shared prefix is not inside the root") {
	// "/music-backup" must not be treated as inside "/music". A string prefix
	// test would get this wrong.
	TempDirectory temp;
	const fs::path source = temp.path() / "music";
	const fs::path sibling = temp.path() / "music-backup";
	fs::create_directories(source);
	fs::create_directories(sibling);

	PathGuard guard;
	CHECK(guard.addProtectedRoot(source).ok());
	CHECK(guard.addOutputRoot(sibling).ok());

	CHECK(guard.checkWrite(sibling / "track.mp3").allowed());
}

TEST_CASE("SAFE-001: a symlink that resolves into the protected root is refused") {
	TempDirectory temp;
	const fs::path source = temp.path() / "music";
	const fs::path output = temp.path() / "output";
	fs::create_directories(source / "album");
	fs::create_directories(output);

	std::error_code ec;
	fs::create_directory_symlink(source, output / "sneaky", ec);
	if (ec) {
		// Symlink creation is not permitted here; the case cannot be exercised,
		// and reporting a pass would be dishonest.
		CHECK_MESSAGE(false, "symlinks could not be created in the test environment");
		return;
	}

	PathGuard guard;
	CHECK(guard.addProtectedRoot(source).ok());
	CHECK(guard.addOutputRoot(output).ok());

	// This path is lexically under the output root, but resolves into the source.
	const GuardDecision decision = guard.checkWrite(output / "sneaky" / "album" / "track.mp3");
	CHECK_MESSAGE(!decision.allowed(), "a symlink into the source must not be writable");
	CHECK_EQUAL(static_cast<int>(decision.verdict),
		static_cast<int>(GuardVerdict::ResolvesIntoProtectedRoot));
}

TEST_CASE("SAFE-001: an output root inside a protected root is refused at configuration time") {
	TempDirectory temp;
	const fs::path source = temp.path() / "music";
	fs::create_directories(source / "output");

	PathGuard guard;
	CHECK(guard.addProtectedRoot(source).ok());

	auto status = guard.addOutputRoot(source / "output");
	CHECK(!status.ok());
	CHECK_EQUAL(static_cast<int>(status.error().code),
		static_cast<int>(ErrorCode::ProtectedRootViolation));
}

TEST_CASE("SAFE-001: an output root containing a protected root is refused") {
	TempDirectory temp;
	const fs::path outer = temp.path() / "everything";
	const fs::path source = outer / "music";
	fs::create_directories(source);

	PathGuard guard;
	CHECK(guard.addProtectedRoot(source).ok());

	auto status = guard.addOutputRoot(outer);
	CHECK(!status.ok());
	CHECK_EQUAL(static_cast<int>(status.error().code),
		static_cast<int>(ErrorCode::ProtectedRootViolation));
}

TEST_CASE("SAFE-001: writing outside every configured output root is refused") {
	TempDirectory temp;
	const fs::path source = temp.path() / "music";
	const fs::path output = temp.path() / "output";
	const fs::path elsewhere = temp.path() / "elsewhere";
	fs::create_directories(source);
	fs::create_directories(output);
	fs::create_directories(elsewhere);

	PathGuard guard;
	CHECK(guard.addProtectedRoot(source).ok());
	CHECK(guard.addOutputRoot(output).ok());

	const GuardDecision decision = guard.checkWrite(elsewhere / "track.mp3");
	CHECK(!decision.allowed());
	CHECK_EQUAL(static_cast<int>(decision.verdict),
		static_cast<int>(GuardVerdict::OutsideAllowedOutput));
}

TEST_CASE("FN-SCAN-04: an absent protected root is reported, not treated as empty") {
	TempDirectory temp;
	const fs::path source = temp.path() / "removable";
	fs::create_directories(source);

	PathGuard guard;
	CHECK(guard.addProtectedRoot(source).ok());
	guard.refreshRootPresence();
	CHECK_EQUAL(guard.absentRootCount(), std::size_t{0});

	std::error_code ec;
	fs::remove_all(source, ec);
	guard.refreshRootPresence();
	CHECK_EQUAL(guard.absentRootCount(), std::size_t{1});
}

TEST_CASE("SAFE-002: a temporary file is created exclusively and removed on destruction") {
	TempDirectory temp;
	const fs::path source = temp.path() / "music";
	const fs::path output = temp.path() / "output";
	fs::create_directories(source);
	fs::create_directories(output);

	PathGuard guard;
	CHECK(guard.addProtectedRoot(source).ok());
	CHECK(guard.addOutputRoot(output).ok());

	fs::path leaked;
	{
		auto temporary = ScopedTempFile::createIn(guard, output);
		CHECK(temporary.ok());
		leaked = temporary.value().path();
		CHECK(fs::exists(leaked));
	}
	CHECK_MESSAGE(!fs::exists(leaked), "an unreleased temporary file must not survive its scope");
}

TEST_CASE("SAFE-002: a released temporary file survives, as publication requires") {
	TempDirectory temp;
	const fs::path output = temp.path() / "output";
	fs::create_directories(output);

	PathGuard guard;
	CHECK(guard.addOutputRoot(output).ok());

	fs::path kept;
	{
		auto temporary = ScopedTempFile::createIn(guard, output);
		CHECK(temporary.ok());
		kept = temporary.value().release();
	}
	CHECK(fs::exists(kept));
	std::error_code ec;
	fs::remove(kept, ec);
}

TEST_CASE("SAFE-001: a temporary file cannot be created inside a protected root") {
	TempDirectory temp;
	const fs::path source = temp.path() / "music";
	fs::create_directories(source);

	PathGuard guard;
	CHECK(guard.addProtectedRoot(source).ok());

	auto temporary = ScopedTempFile::createIn(guard, source);
	CHECK(!temporary.ok());
}

// ===========================================================================
// Container parsing, including malformed input
// ===========================================================================

TEST_CASE("Mp3Container locates the tag and the audio payload") {
	const auto bytes = syntheticMp3();
	const Mp3Layout layout = Mp3Container::parseLayoutFromBuffer(
		reinterpret_cast<const std::byte*>(bytes.data()), bytes.size());

	CHECK(layout.valid);
	CHECK(layout.hasId3v2);
	CHECK_EQUAL(static_cast<int>(layout.id3v2Version), static_cast<int>(TagContainer::Id3v2_3));
	CHECK(layout.audioOffset > 0);
	CHECK(layout.audioLength > 0);
	CHECK_EQUAL(layout.audioOffset + layout.audioLength, layout.fileSize);
	CHECK_EQUAL(layout.audio.sampleRateHz, 44100);
	CHECK_EQUAL(layout.audio.bitrateKbps, 128);
}

TEST_CASE("Mp3Container enumerates frames and measures padding") {
	const auto bytes = syntheticMp3(4, 250);
	Mp3Layout layout = Mp3Container::parseLayoutFromBuffer(
		reinterpret_cast<const std::byte*>(bytes.data()), bytes.size());

	std::vector<std::string> warnings;
	const auto frames = Mp3Container::parseFramesFromBuffer(
		reinterpret_cast<const std::byte*>(bytes.data()),
		static_cast<std::size_t>(layout.id3v2TotalSize), layout.id3v2Version, false, warnings);

	CHECK_EQUAL(frames.size(), std::size_t{1});
	CHECK_EQUAL(frames[0].id, std::string("TIT2"));
	CHECK(!frames[0].truncated);
	CHECK(warnings.empty());
}

TEST_CASE("Mp3Container survives an empty buffer") {
	const Mp3Layout layout = Mp3Container::parseLayoutFromBuffer(nullptr, 0);
	CHECK(!layout.valid);
}

TEST_CASE("Mp3Container survives a truncated ID3 header") {
	const std::vector<std::uint8_t> bytes = {'I', 'D', '3', 3, 0, 0, 0};
	const Mp3Layout layout = Mp3Container::parseLayoutFromBuffer(
		reinterpret_cast<const std::byte*>(bytes.data()), bytes.size());
	CHECK(!layout.valid);
	CHECK(!layout.warnings.empty());
}

TEST_CASE("Mp3Container rejects a tag claiming to extend past the end of the file") {
	std::vector<std::uint8_t> bytes;
	bytes.insert(bytes.end(), {'I', 'D', '3', 3, 0, 0});
	appendSyncsafe(bytes, 0x0FFFFFFF);   // an enormous declared tag
	bytes.resize(100, 0);

	const Mp3Layout layout = Mp3Container::parseLayoutFromBuffer(
		reinterpret_cast<const std::byte*>(bytes.data()), bytes.size());

	// Either the size is rejected as implausible or it is clamped to the file;
	// what must not happen is a read beyond the buffer.
	CHECK(!layout.warnings.empty());
	CHECK(layout.id3v2TotalSize <= layout.fileSize);
}

TEST_CASE("Mp3Container reports a frame whose declared size overruns the tag") {
	std::vector<std::uint8_t> frameData;
	frameData.insert(frameData.end(), {'T', 'I', 'T', '2'});
	frameData.insert(frameData.end(), {0x00, 0x00, 0x7F, 0xFF});   // far too large
	frameData.insert(frameData.end(), {0, 0});
	frameData.insert(frameData.end(), {0, 'x'});

	std::vector<std::uint8_t> bytes;
	bytes.insert(bytes.end(), {'I', 'D', '3', 3, 0, 0});
	appendSyncsafe(bytes, static_cast<std::uint32_t>(frameData.size()));
	bytes.insert(bytes.end(), frameData.begin(), frameData.end());

	std::vector<std::string> warnings;
	const auto frames = Mp3Container::parseFramesFromBuffer(
		reinterpret_cast<const std::byte*>(bytes.data()), bytes.size(),
		TagContainer::Id3v2_3, false, warnings);

	CHECK_EQUAL(frames.size(), std::size_t{1});
	CHECK(frames[0].truncated);
	CHECK(!warnings.empty());
}

TEST_CASE("Mp3Container stops on a frame identifier that is not valid") {
	std::vector<std::uint8_t> frameData;
	frameData.insert(frameData.end(), {0x01, 0x02, 0x03, 0x04});   // not A-Z0-9
	frameData.insert(frameData.end(), {0, 0, 0, 4, 0, 0});
	frameData.insert(frameData.end(), {1, 2, 3, 4});

	std::vector<std::uint8_t> bytes;
	bytes.insert(bytes.end(), {'I', 'D', '3', 3, 0, 0});
	appendSyncsafe(bytes, static_cast<std::uint32_t>(frameData.size()));
	bytes.insert(bytes.end(), frameData.begin(), frameData.end());

	std::vector<std::string> warnings;
	const auto frames = Mp3Container::parseFramesFromBuffer(
		reinterpret_cast<const std::byte*>(bytes.data()), bytes.size(),
		TagContainer::Id3v2_3, false, warnings);

	CHECK(frames.empty());
	CHECK(!warnings.empty());
}

TEST_CASE("Mp3Container survives arbitrary random input without crashing") {
	// A crude fuzz pass over the tag reader. The assertion is simply that every
	// input terminates and stays inside its buffer; ASan and UBSan turn a
	// violation here into a failure.
	std::mt19937 rng(20260913);
	std::uniform_int_distribution<int> byteValue(0, 255);
	std::uniform_int_distribution<std::size_t> lengthValue(0, 4096);

	for (int iteration = 0; iteration < 400; ++iteration) {
		std::vector<std::uint8_t> bytes(lengthValue(rng));
		for (auto& byte : bytes) byte = static_cast<std::uint8_t>(byteValue(rng));

		// Half the inputs start with a plausible ID3 header, so the frame parser
		// is actually reached rather than rejected at the first byte.
		if (iteration % 2 == 0 && bytes.size() > 10) {
			bytes[0] = 'I'; bytes[1] = 'D'; bytes[2] = '3';
			bytes[3] = 3; bytes[4] = 0; bytes[5] = 0;
			bytes[6] &= 0x7F; bytes[7] &= 0x7F; bytes[8] &= 0x7F; bytes[9] &= 0x7F;
		}

		const Mp3Layout layout = Mp3Container::parseLayoutFromBuffer(
			reinterpret_cast<const std::byte*>(bytes.data()), bytes.size());
		CHECK(layout.id3v2TotalSize <= layout.fileSize);

		std::vector<std::string> warnings;
		(void)Mp3Container::parseFramesFromBuffer(
			reinterpret_cast<const std::byte*>(bytes.data()), bytes.size(),
			TagContainer::Id3v2_3, false, warnings);
		(void)Mp3Container::parseFramesFromBuffer(
			reinterpret_cast<const std::byte*>(bytes.data()), bytes.size(),
			TagContainer::Id3v2_4, true, warnings);
	}
	CHECK(true);
}

TEST_CASE("A syncsafe integer with a high bit set is rejected") {
	const std::uint8_t good[4] = {0x00, 0x00, 0x02, 0x01};
	CHECK(Mp3Container::decodeSyncsafe(reinterpret_cast<const std::byte*>(good)).has_value());
	CHECK_EQUAL(*Mp3Container::decodeSyncsafe(reinterpret_cast<const std::byte*>(good)),
		std::uint32_t{257});

	const std::uint8_t bad[4] = {0x00, 0x00, 0x82, 0x01};
	CHECK(!Mp3Container::decodeSyncsafe(reinterpret_cast<const std::byte*>(bad)).has_value());
}

TEST_CASE("Unsynchronisation is reversed correctly") {
	const std::uint8_t input[] = {0xFF, 0x00, 0xE0, 0x12, 0xFF, 0x00, 0x00};
	const auto output = Mp3Container::undoUnsynchronisation(
		reinterpret_cast<const std::byte*>(input), sizeof(input));

	CHECK_EQUAL(output.size(), std::size_t{5});
	CHECK_EQUAL(static_cast<int>(output[0]), 0xFF);
	CHECK_EQUAL(static_cast<int>(output[1]), 0xE0);
	CHECK_EQUAL(static_cast<int>(output[2]), 0x12);
	CHECK_EQUAL(static_cast<int>(output[3]), 0xFF);
	CHECK_EQUAL(static_cast<int>(output[4]), 0x00);
}

// ===========================================================================
// ART-001: the image pipeline
// ===========================================================================

namespace {

/// A deterministic test image with sharp edges and saturated colour, so
/// resampling and subsampling artefacts would be visible in the output hash.
RgbImage testImage(int size) {
	RgbImage image;
	image.width = size;
	image.height = size;
	image.pixels.resize(static_cast<std::size_t>(size) * static_cast<std::size_t>(size) * 3u);

	for (int y = 0; y < size; ++y) {
		for (int x = 0; x < size; ++x) {
			const std::size_t i = (static_cast<std::size_t>(y) * static_cast<std::size_t>(size)
				+ static_cast<std::size_t>(x)) * 3u;
			const bool checker = ((x / 16) + (y / 16)) % 2 == 0;
			image.pixels[i] = static_cast<std::uint8_t>(checker ? 220 : 20);
			image.pixels[i + 1] = static_cast<std::uint8_t>((x * 255) / size);
			image.pixels[i + 2] = static_cast<std::uint8_t>((y * 255) / size);
		}
	}
	return image;
}

} // namespace

TEST_CASE("ART-001: the derivative is exactly 600x600 and decodes back to that size") {
	const RgbImage source = testImage(1200);
	DerivativeConfig config;

	auto encoded = ImagePipeline::encodeJpeg(source, config);
	CHECK(encoded.ok());

	auto derivative = ImagePipeline::makeDerivative(encoded.value().data(), encoded.value().size(),
		config);
	CHECK(derivative.ok());
	CHECK_EQUAL(derivative.value().width, 600);
	CHECK_EQUAL(derivative.value().height, 600);

	auto dimensions = ImagePipeline::probeDimensions(derivative.value().jpegBytes.data(),
		derivative.value().jpegBytes.size());
	CHECK(dimensions.ok());
	CHECK_EQUAL(dimensions.value().first, 600);
	CHECK_EQUAL(dimensions.value().second, 600);
}

TEST_CASE("FN-ART-03: the derivative is deterministic, so an album gets identical bytes") {
	const RgbImage source = testImage(900);
	DerivativeConfig config;

	auto encoded = ImagePipeline::encodeJpeg(source, config);
	CHECK(encoded.ok());

	auto first = ImagePipeline::makeDerivative(encoded.value().data(), encoded.value().size(), config);
	auto second = ImagePipeline::makeDerivative(encoded.value().data(), encoded.value().size(), config);
	CHECK(first.ok());
	CHECK(second.ok());
	CHECK_EQUAL(first.value().sha256, second.value().sha256);
	CHECK(first.value().jpegBytes == second.value().jpegBytes);
}

TEST_CASE("ART-001: a 600x600 source is accepted without upscaling") {
	const RgbImage source = testImage(600);
	DerivativeConfig config;

	auto encoded = ImagePipeline::encodeJpeg(source, config);
	CHECK(encoded.ok());

	auto derivative = ImagePipeline::makeDerivative(encoded.value().data(), encoded.value().size(),
		config);
	CHECK_MESSAGE(derivative.ok(), "a clean 600x600 source must be acceptable");
	CHECK_EQUAL(derivative.value().width, 600);
}

TEST_CASE("ART-001: a smaller source is refused rather than upscaled") {
	const RgbImage source = testImage(400);
	DerivativeConfig config;

	auto encoded = ImagePipeline::encodeJpeg(source, config);
	CHECK(encoded.ok());

	auto derivative = ImagePipeline::makeDerivative(encoded.value().data(), encoded.value().size(),
		config);
	CHECK(!derivative.ok());
	CHECK(derivative.error().message.find("upscaling is not permitted") != std::string::npos);
}

TEST_CASE("FN-ART-04: a non-square source is refused rather than cropped") {
	RgbImage source;
	source.width = 1200;
	source.height = 800;
	source.pixels.assign(static_cast<std::size_t>(1200) * 800 * 3, std::uint8_t{128});

	DerivativeConfig config;
	auto encoded = ImagePipeline::encodeJpeg(source, config);
	CHECK(encoded.ok());

	auto derivative = ImagePipeline::makeDerivative(encoded.value().data(), encoded.value().size(),
		config);
	CHECK(!derivative.ok());
	CHECK(derivative.error().message.find("review decision") != std::string::npos);
}

TEST_CASE("ART-001: a configuration change changes the derivative's identity") {
	DerivativeConfig baseline;
	DerivativeConfig other = baseline;
	other.jpegQuality = 90;
	CHECK(baseline.hash() != other.hash());

	DerivativeConfig subsampled = baseline;
	subsampled.chromaSubsampling = true;
	CHECK(baseline.hash() != subsampled.hash());

	CHECK_EQUAL(baseline.hash(), DerivativeConfig{}.hash());
}

TEST_CASE("ART-001: an undecodable buffer is an error, never a guess") {
	const std::vector<std::uint8_t> garbage(64, 0xAB);
	auto result = ImagePipeline::decode(garbage.data(), garbage.size());
	CHECK(!result.ok());
	CHECK_EQUAL(static_cast<int>(result.error().code), static_cast<int>(ErrorCode::Unsupported));
}

TEST_CASE("The Lanczos resampler preserves overall brightness") {
	// An unnormalised kernel shifts the mean. This catches that.
	RgbImage flat;
	flat.width = 1000;
	flat.height = 1000;
	flat.pixels.assign(static_cast<std::size_t>(1000) * 1000 * 3, std::uint8_t{128});

	const RgbImage resized = ImagePipeline::resizeLanczos(flat, 600, 600);
	CHECK(resized.valid());

	double sum = 0.0;
	for (std::uint8_t value : resized.pixels) sum += value;
	const double mean = sum / static_cast<double>(resized.pixels.size());
	CHECK_MESSAGE(std::abs(mean - 128.0) < 1.5,
		"mean drifted to " + std::to_string(mean));
}

TEST_CASE("AssetStore is content-addressed and idempotent") {
	TempDirectory temp;
	AssetStore store(temp.path() / "assets");

	const std::vector<std::uint8_t> bytes = {1, 2, 3, 4, 5};
	auto first = store.put(bytes.data(), bytes.size(), ".bin");
	auto second = store.put(bytes.data(), bytes.size(), ".bin");

	CHECK(first.ok());
	CHECK(second.ok());
	CHECK_EQUAL(first.value(), second.value());
	CHECK(store.contains(first.value()));

	auto read = store.get(first.value());
	CHECK(read.ok());
	CHECK(read.value() == bytes);
}

// ===========================================================================
// JSON hardening
// ===========================================================================

TEST_CASE("JSON parses provider-shaped documents") {
	auto parsed = Json::parse(R"({"resultCount":2,"results":[{"a":1},{"a":2}]})");
	CHECK(parsed.ok());
	CHECK_EQUAL(parsed.value()["resultCount"].asInt(), std::int64_t{2});
	CHECK_EQUAL(parsed.value()["results"].size(), std::size_t{2});
	CHECK_EQUAL(parsed.value().at("results.1.a").asInt(), std::int64_t{2});
}

TEST_CASE("JSON rejects deeply nested input rather than overflowing the stack") {
	std::string deep;
	for (int i = 0; i < 5000; ++i) deep += "[";
	for (int i = 0; i < 5000; ++i) deep += "]";

	auto parsed = Json::parse(deep);
	CHECK(!parsed.ok());
	CHECK_EQUAL(static_cast<int>(parsed.error().code), static_cast<int>(ErrorCode::ParseError));
}

TEST_CASE("JSON rejects malformed documents and trailing content") {
	CHECK(!Json::parse("{").ok());
	CHECK(!Json::parse("").ok());
	CHECK(!Json::parse("{\"a\":1} trailing").ok());
	CHECK(!Json::parse("{\"a\":}").ok());
}

TEST_CASE("JSON decodes surrogate pairs and tolerates lone surrogates") {
	auto parsed = Json::parse(R"({"s":"🎵"})");
	CHECK(parsed.ok());
	CHECK(text::isValidUtf8(parsed.value()["s"].asString()));

	auto lone = Json::parse(R"({"s":"\uD83C"})");
	CHECK(lone.ok());
	CHECK(text::isValidUtf8(lone.value()["s"].asString()));
}

TEST_CASE("Missing JSON fields return fallbacks rather than throwing") {
	auto parsed = Json::parse(R"({"a":1})");
	CHECK(parsed.ok());
	CHECK_EQUAL(parsed.value()["missing"].asString("fallback"), std::string("fallback"));
	CHECK_EQUAL(parsed.value()["missing"].asInt(7), std::int64_t{7});
	CHECK(parsed.value()["missing"]["deeper"].isNull());
	CHECK(parsed.value()["missing"][3].isNull());
}

// ===========================================================================
// Catalogue schema
// ===========================================================================

TEST_CASE("The schema migrates, passes an integrity check and is idempotent") {
	auto database = Database::openInMemory();
	CHECK(database.ok());

	CHECK(SchemaMigrator::migrate(database.value()).ok());
	auto version = SchemaMigrator::currentVersion(database.value());
	CHECK(version.ok());
	CHECK_EQUAL(version.value(), SchemaMigrator::kCurrentVersion);

	auto integrity = database.value().integrityCheck();
	CHECK(integrity.ok());
	CHECK(integrity.value());

	// Running it again changes nothing.
	CHECK(SchemaMigrator::migrate(database.value()).ok());
}

TEST_CASE("A newer schema is refused rather than downgraded") {
	auto database = Database::openInMemory();
	CHECK(database.ok());
	CHECK(database.value().executeScript("PRAGMA user_version = 9999;").ok());

	auto status = SchemaMigrator::migrate(database.value());
	CHECK(!status.ok());
	CHECK_EQUAL(static_cast<int>(status.error().code), static_cast<int>(ErrorCode::Conflict));
}

TEST_CASE("Nested transactions become savepoints and unwind independently") {
	auto database = Database::openInMemory();
	CHECK(database.ok());
	CHECK(database.value().executeScript("CREATE TABLE t (v INTEGER);").ok());

	auto outer = database.value().begin(Transaction::Kind::Immediate);
	CHECK(outer.ok());
	CHECK(database.value().executeScript("INSERT INTO t VALUES (1);").ok());

	{
		auto inner = database.value().begin();
		CHECK(inner.ok());
		CHECK(inner.value().isNested());
		CHECK(database.value().executeScript("INSERT INTO t VALUES (2);").ok());
		// Destroyed without committing: rolls back to the savepoint only.
	}

	CHECK(database.value().executeScript("INSERT INTO t VALUES (3);").ok());
	CHECK(outer.value().commit().ok());

	auto statement = database.value().prepare("SELECT COUNT(*), SUM(v) FROM t;");
	CHECK(statement.ok());
	auto row = statement.value().step();
	CHECK(row.ok());
	CHECK(row.value());
	CHECK_EQUAL(statement.value().columnInt(0), 2);   // rows 1 and 3 survive
	CHECK_EQUAL(statement.value().columnInt(1), 4);
}

TEST_CASE("Parameterised statements treat SQL metacharacters as data") {
	auto database = Database::openInMemory();
	CHECK(database.ok());
	CHECK(database.value().executeScript("CREATE TABLE t (v TEXT);").ok());

	auto insert = database.value().prepare("INSERT INTO t (v) VALUES (?);");
	CHECK(insert.ok());
	const std::string hostile = "'; DROP TABLE t; --";
	insert.value().bind(1, hostile);
	CHECK(insert.value().execute().ok());

	auto select = database.value().prepare("SELECT v FROM t;");
	CHECK(select.ok());
	auto row = select.value().step();
	CHECK(row.ok());
	CHECK(row.value());
	CHECK_EQUAL(select.value().columnText(0), hostile);
}

// ===========================================================================
// Providers: pure functions, no network
// ===========================================================================

TEST_CASE("The iTunes artwork URL rewrite only fires on a size-shaped tail") {
	CHECK_EQUAL(ITunesProvider::rewriteArtworkUrl(
		"https://is1-ssl.mzstatic.com/image/thumb/abc/100x100bb.jpg", 3000),
		std::string("https://is1-ssl.mzstatic.com/image/thumb/abc/3000x3000bb.jpg"));

	// Not size-shaped: returned unchanged rather than mangled.
	CHECK_EQUAL(ITunesProvider::rewriteArtworkUrl("https://example.com/cover.jpg", 3000),
		std::string("https://example.com/cover.jpg"));
	CHECK_EQUAL(ITunesProvider::rewriteArtworkUrl("https://example.com/", 3000),
		std::string("https://example.com/"));
	CHECK_EQUAL(ITunesProvider::rewriteArtworkUrl("", 3000), std::string(""));
}

TEST_CASE("FN-ART-06: the review URLs keep the user's own query shapes") {
	const std::string google = ReviewSearchUrls::googleImages("Artist", "Album", 1200);
	CHECK(google.find("imagesize%3A1200x1200") != std::string::npos);

	// The Apple query has no size filter, so a clean 600x600 source is not
	// excluded from what the reviewer sees.
	const std::string apple = ReviewSearchUrls::appleMusicSiteSearch("Artist", "Album");
	CHECK(apple.find("site%3Amusic.apple.com") != std::string::npos);
	CHECK(apple.find("imagesize") == std::string::npos);

	// The user's existing presets are retained.
	const auto& presets = ReviewSearchUrls::sizePresets();
	CHECK_EQUAL(presets.size(), std::size_t{4});
	CHECK_EQUAL(presets[0], 1200);
	CHECK_EQUAL(presets[3], 3000);
}

TEST_CASE("URL encoding escapes what it must and leaves unreserved characters alone") {
	CHECK_EQUAL(urlEncode("a b"), std::string("a%20b"));
	CHECK_EQUAL(urlEncode("a&b=c"), std::string("a%26b%3Dc"));
	CHECK_EQUAL(urlEncode("Sigur Rós"), std::string("Sigur%20R%C3%B3s"));
	CHECK_EQUAL(urlEncode("abc-_.~"), std::string("abc-_.~"));
}

// ===========================================================================
// FFT and tempo internals
// ===========================================================================

TEST_CASE("The FFT round-trips through its inverse") {
	std::vector<float> real(256);
	std::vector<float> imaginary(256, 0.0f);
	for (std::size_t i = 0; i < real.size(); ++i) {
		real[i] = std::sin(static_cast<float>(i) * 0.3f) + 0.5f * std::cos(static_cast<float>(i) * 0.05f);
	}
	const std::vector<float> original = real;

	fftRadix2(real, imaginary, false);
	fftRadix2(real, imaginary, true);

	double worst = 0.0;
	for (std::size_t i = 0; i < real.size(); ++i) {
		worst = std::max(worst, static_cast<double>(std::abs(real[i] - original[i])));
	}
	CHECK_MESSAGE(worst < 1e-3, "worst round-trip error was " + std::to_string(worst));
}

TEST_CASE("The tempo detector recovers a known pulse rate") {
	// A synthetic click track at exactly 120 BPM: a known answer, unlike real
	// music where the tagged value is itself only a baseline.
	AnalysisAudio audio;
	audio.sampleRateHz = 11025;
	audio.durationMs = 60000;
	audio.samples.assign(static_cast<std::size_t>(11025) * 60, 0.0f);

	const double beatsPerSecond = 120.0 / 60.0;
	const auto samplesPerBeat = static_cast<std::size_t>(11025.0 / beatsPerSecond);

	std::mt19937 rng(7);
	std::uniform_real_distribution<float> noise(-0.01f, 0.01f);
	for (auto& sample : audio.samples) sample = noise(rng);

	for (std::size_t position = 0; position + 200 < audio.samples.size();
		position += samplesPerBeat) {
		// A short decaying burst: an onset with energy across the spectrum.
		for (std::size_t i = 0; i < 200; ++i) {
			const float envelope = 1.0f - static_cast<float>(i) / 200.0f;
			audio.samples[position + i] += envelope * ((i % 2 == 0) ? 0.8f : -0.8f);
		}
	}

	TempoAnalyzer analyzer;
	const TempoAnalysis analysis = analyzer.analyse(audio);

	CHECK(analysis.valid);
	CHECK_MESSAGE(std::abs(analysis.bpm - 120.0) < 3.0,
		"detected " + std::to_string(analysis.bpm) + " BPM, expected 120");
	CHECK_EQUAL(static_cast<int>(analysis.character), static_cast<int>(TempoCharacter::SteadyBeat));
}

TEST_CASE("Silence is reported as beatless, not given an invented tempo") {
	AnalysisAudio audio;
	audio.sampleRateHz = 11025;
	audio.durationMs = 40000;
	audio.samples.assign(static_cast<std::size_t>(11025) * 40, 0.0f);

	TempoAnalyzer analyzer;
	const TempoAnalysis analysis = analyzer.analyse(audio);

	CHECK(analysis.valid);
	CHECK_EQUAL(static_cast<int>(analysis.character), static_cast<int>(TempoCharacter::Beatless));
	CHECK_EQUAL(analysis.bpm, 0.0);
}

TEST_CASE("A too-short input is reported as too short") {
	AnalysisAudio audio;
	audio.sampleRateHz = 11025;
	audio.durationMs = 3000;
	audio.samples.assign(static_cast<std::size_t>(11025) * 3, 0.1f);

	TempoAnalyzer analyzer;
	const TempoAnalysis analysis = analyzer.analyse(audio);
	CHECK_EQUAL(static_cast<int>(analysis.character), static_cast<int>(TempoCharacter::TooShort));
}

ML_TEST_MAIN("infrastructure")
