// SPDX-License-Identifier: GPL-3.0-or-later
#include "mlinfra/Mp3Container.hpp"
#include "mlcore/Text.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace fs = std::filesystem;

namespace ml {

namespace {

/// Bitrate table for MPEG 1/2/2.5 Layer I/II/III, in kbps. Index 0 is "free"
/// and index 15 is invalid; both are rejected by parseMpegFrameHeader.
constexpr int kBitrateV1L3[16] = {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0};
constexpr int kBitrateV1L2[16] = {0, 32, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384, 0};
constexpr int kBitrateV1L1[16] = {0, 32, 64, 96, 128, 160, 192, 224, 256, 288, 320, 352, 384, 416, 448, 0};
constexpr int kBitrateV2L1[16] = {0, 32, 48, 56, 64, 80, 96, 112, 128, 144, 160, 176, 192, 224, 256, 0};
constexpr int kBitrateV2L23[16] = {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0};

constexpr int kSampleRateV1[4] = {44100, 48000, 32000, 0};
constexpr int kSampleRateV2[4] = {22050, 24000, 16000, 0};
constexpr int kSampleRateV25[4] = {11025, 12000, 8000, 0};

constexpr std::size_t kId3v2HeaderBytes = 10;
constexpr std::size_t kId3v1Bytes = 128;
constexpr std::size_t kApeFooterBytes = 32;

/// How much of the file tail to read when looking for APEv2 and ID3v1.
constexpr std::size_t kTailProbeBytes = 64 * 1024;

/// How far to search for the first MPEG frame after the tags. Some files carry
/// junk between the tag and the audio.
constexpr std::size_t kSyncSearchBytes = 256 * 1024;

std::uint8_t byteAt(const std::byte* data, std::size_t index) {
	return static_cast<std::uint8_t>(data[index]);
}

class FileHandle {
public:
	explicit FileHandle(const fs::path& path) {
#ifdef _WIN32
		_wfopen_s(&m_file, path.wstring().c_str(), L"rb");
#else
		m_file = std::fopen(path.c_str(), "rb");
#endif
	}
	~FileHandle() { if (m_file) std::fclose(m_file); }

	FileHandle(const FileHandle&) = delete;
	FileHandle& operator=(const FileHandle&) = delete;

	std::FILE* get() const { return m_file; }
	explicit operator bool() const { return m_file != nullptr; }

	bool seek(std::int64_t offset) {
#ifdef _WIN32
		return _fseeki64(m_file, offset, SEEK_SET) == 0;
#else
		return std::fseek(m_file, static_cast<long>(offset), SEEK_SET) == 0;
#endif
	}

	std::size_t read(void* buffer, std::size_t count) {
		return std::fread(buffer, 1, count, m_file);
	}

private:
	std::FILE* m_file = nullptr;
};

/// Reads `count` bytes at `offset`. Returns fewer bytes at end of file.
std::vector<std::byte> readRange(FileHandle& file, std::int64_t offset, std::size_t count) {
	std::vector<std::byte> buffer;
	if (!file.seek(offset)) return buffer;
	buffer.resize(count);
	const std::size_t read = file.read(buffer.data(), count);
	buffer.resize(read);
	return buffer;
}

} // namespace

std::optional<std::uint32_t> Mp3Container::decodeSyncsafe(const std::byte* bytes) {
	for (std::size_t i = 0; i < 4; ++i) {
		// A syncsafe integer has the high bit of every byte clear. Writers that
		// get this wrong produce tags whose size cannot be trusted.
		if ((byteAt(bytes, i) & 0x80u) != 0) return std::nullopt;
	}
	return (static_cast<std::uint32_t>(byteAt(bytes, 0)) << 21)
		 | (static_cast<std::uint32_t>(byteAt(bytes, 1)) << 14)
		 | (static_cast<std::uint32_t>(byteAt(bytes, 2)) << 7)
		 | (static_cast<std::uint32_t>(byteAt(bytes, 3)));
}

std::uint32_t Mp3Container::decodeBigEndian32(const std::byte* bytes) {
	return (static_cast<std::uint32_t>(byteAt(bytes, 0)) << 24)
		 | (static_cast<std::uint32_t>(byteAt(bytes, 1)) << 16)
		 | (static_cast<std::uint32_t>(byteAt(bytes, 2)) << 8)
		 | (static_cast<std::uint32_t>(byteAt(bytes, 3)));
}

std::vector<std::byte> Mp3Container::undoUnsynchronisation(const std::byte* data, std::size_t size) {
	std::vector<std::byte> out;
	out.reserve(size);
	for (std::size_t i = 0; i < size; ++i) {
		out.push_back(data[i]);
		if (byteAt(data, i) == 0xFF && i + 1 < size && byteAt(data, i + 1) == 0x00) {
			++i;   // Drop the inserted zero.
		}
	}
	return out;
}

bool Mp3Container::isMpegFrameHeader(const std::byte* bytes) {
	AudioProperties properties;
	int length = 0;
	return parseMpegFrameHeader(bytes, properties, length);
}

bool Mp3Container::parseMpegFrameHeader(const std::byte* bytes, AudioProperties& out, int& frameLengthBytes) {
	const std::uint8_t b0 = byteAt(bytes, 0);
	const std::uint8_t b1 = byteAt(bytes, 1);
	const std::uint8_t b2 = byteAt(bytes, 2);
	const std::uint8_t b3 = byteAt(bytes, 3);

	// Frame sync: 11 set bits.
	if (b0 != 0xFF || (b1 & 0xE0u) != 0xE0u) return false;

	const int versionBits = (b1 >> 3) & 0x03;
	const int layerBits = (b1 >> 1) & 0x03;
	if (versionBits == 1) return false;   // Reserved.
	if (layerBits == 0) return false;     // Reserved.

	const int bitrateIndex = (b2 >> 4) & 0x0F;
	const int sampleRateIndex = (b2 >> 2) & 0x03;
	if (bitrateIndex == 0 || bitrateIndex == 15) return false;
	if (sampleRateIndex == 3) return false;

	const int padding = (b2 >> 1) & 0x01;
	const int channelMode = (b3 >> 6) & 0x03;

	// version: 0 = MPEG 2.5, 2 = MPEG 2, 3 = MPEG 1
	// layer:   1 = Layer III, 2 = Layer II, 3 = Layer I
	int bitrate = 0;
	if (versionBits == 3) {
		bitrate = (layerBits == 3) ? kBitrateV1L1[bitrateIndex]
				: (layerBits == 2) ? kBitrateV1L2[bitrateIndex]
								   : kBitrateV1L3[bitrateIndex];
	} else {
		bitrate = (layerBits == 3) ? kBitrateV2L1[bitrateIndex] : kBitrateV2L23[bitrateIndex];
	}
	if (bitrate == 0) return false;

	int sampleRate = 0;
	if (versionBits == 3) sampleRate = kSampleRateV1[sampleRateIndex];
	else if (versionBits == 2) sampleRate = kSampleRateV2[sampleRateIndex];
	else sampleRate = kSampleRateV25[sampleRateIndex];
	if (sampleRate == 0) return false;

	// Frame length. Layer I uses 4-byte slots; Layers II and III use 1-byte slots.
	if (layerBits == 3) {
		frameLengthBytes = ((12 * bitrate * 1000 / sampleRate) + padding) * 4;
	} else {
		const int samplesPerFrame = (versionBits == 3) ? 144 : 72;
		frameLengthBytes = (samplesPerFrame * bitrate * 1000 / sampleRate) + padding;
	}
	if (frameLengthBytes <= 4) return false;

	out.sampleRateHz = sampleRate;
	out.bitrateKbps = bitrate;
	out.channels = (channelMode == 3) ? 1 : 2;
	return true;
}

Mp3Layout Mp3Container::parseLayoutFromBuffer(const std::byte* data, std::size_t size) {
	Mp3Layout layout;
	layout.fileSize = static_cast<std::int64_t>(size);

	if (size < kId3v2HeaderBytes) {
		layout.warnings.push_back("file is smaller than an ID3v2 header");
		return layout;
	}

	std::size_t cursor = 0;

	// --- ID3v2 header -------------------------------------------------------
	if (byteAt(data, 0) == 'I' && byteAt(data, 1) == 'D' && byteAt(data, 2) == '3') {
		const std::uint8_t major = byteAt(data, 3);
		const std::uint8_t revision = byteAt(data, 4);
		const std::uint8_t flags = byteAt(data, 5);

		// Revision 0xFF is invalid, as is a major version this format never had.
		if (major == 0xFF || revision == 0xFF) {
			layout.warnings.push_back("ID3v2 version bytes are invalid");
		} else {
			const auto declared = decodeSyncsafe(data + 6);
			if (!declared) {
				layout.warnings.push_back("ID3v2 size field is not syncsafe; tag length cannot be trusted");
			} else if (static_cast<std::size_t>(*declared) > kMaxTagBytes) {
				layout.warnings.push_back("ID3v2 declares an implausibly large tag ("
					+ std::to_string(*declared) + " bytes); treated as malformed");
			} else {
				layout.hasId3v2 = true;
				layout.id3v2Offset = 0;
				layout.id3v2Unsynchronised = (flags & 0x80u) != 0;
				layout.id3v2ExtendedHeader = (flags & 0x40u) != 0;
				layout.id3v2Experimental = (flags & 0x20u) != 0;
				layout.id3v2HasFooter = (flags & 0x10u) != 0;

				switch (major) {
					case 2: layout.id3v2Version = TagContainer::Id3v2_2; break;
					case 3: layout.id3v2Version = TagContainer::Id3v2_3; break;
					case 4: layout.id3v2Version = TagContainer::Id3v2_4; break;
					default:
						layout.id3v2Version = TagContainer::Unknown;
						layout.warnings.push_back("unsupported ID3v2.\" " + std::to_string(major) + "\" version");
						break;
				}

				std::int64_t total = static_cast<std::int64_t>(kId3v2HeaderBytes) + *declared;
				if (layout.id3v2HasFooter) total += static_cast<std::int64_t>(kId3v2HeaderBytes);

				// Clamp to the file: a tag claiming to extend past the end is
				// malformed, and trusting it would read out of bounds.
				if (total > layout.fileSize) {
					layout.warnings.push_back("ID3v2 tag extends past end of file; clamped");
					total = layout.fileSize;
				}
				layout.id3v2TotalSize = total;
				cursor = static_cast<std::size_t>(total);
			}
		}
	}

	// --- Trailing containers, from the end backwards ------------------------
	std::size_t tailEnd = size;

	if (size >= kId3v1Bytes) {
		const std::byte* tag = data + (size - kId3v1Bytes);
		if (byteAt(tag, 0) == 'T' && byteAt(tag, 1) == 'A' && byteAt(tag, 2) == 'G') {
			layout.hasId3v1 = true;
			layout.id3v1Offset = static_cast<std::int64_t>(size - kId3v1Bytes);
			tailEnd = size - kId3v1Bytes;
		}
	}

	if (tailEnd >= kApeFooterBytes) {
		const std::byte* footer = data + (tailEnd - kApeFooterBytes);
		if (std::memcmp(footer, "APETAGEX", 8) == 0) {
			// Footer layout: magic(8) version(4) size(4) items(4) flags(4) reserved(8).
			// `size` covers the footer plus all items, but not the 32-byte header
			// when one is present.
			const std::uint32_t tagSize = static_cast<std::uint32_t>(byteAt(footer, 12))
				| (static_cast<std::uint32_t>(byteAt(footer, 13)) << 8)
				| (static_cast<std::uint32_t>(byteAt(footer, 14)) << 16)
				| (static_cast<std::uint32_t>(byteAt(footer, 15)) << 24);
			const std::uint32_t items = static_cast<std::uint32_t>(byteAt(footer, 16))
				| (static_cast<std::uint32_t>(byteAt(footer, 17)) << 8)
				| (static_cast<std::uint32_t>(byteAt(footer, 18)) << 16)
				| (static_cast<std::uint32_t>(byteAt(footer, 19)) << 24);
			const std::uint32_t apeFlags = static_cast<std::uint32_t>(byteAt(footer, 20))
				| (static_cast<std::uint32_t>(byteAt(footer, 21)) << 8)
				| (static_cast<std::uint32_t>(byteAt(footer, 22)) << 16)
				| (static_cast<std::uint32_t>(byteAt(footer, 23)) << 24);

			const bool hasHeader = (apeFlags & 0x80000000u) != 0;
			std::int64_t total = static_cast<std::int64_t>(tagSize) + (hasHeader ? 32 : 0);

			if (tagSize >= kApeFooterBytes && total <= static_cast<std::int64_t>(tailEnd)) {
				layout.hasApev2 = true;
				layout.apev2ItemCount = items;
				layout.apev2TotalSize = total;
				layout.apev2Offset = static_cast<std::int64_t>(tailEnd) - total;
				tailEnd = static_cast<std::size_t>(layout.apev2Offset);
			} else {
				layout.warnings.push_back("APEv2 footer declares an implausible size; ignored");
			}
		}
	}

	// --- MPEG payload -------------------------------------------------------
	// Search forward from the end of the ID3v2 tag for a frame sync that is
	// confirmed by a second frame at the computed distance. A single sync
	// pattern occurs by chance in ordinary data.
	std::size_t searchEnd = std::min(size, cursor + kSyncSearchBytes);
	bool found = false;

	for (std::size_t i = cursor; i + 4 <= searchEnd; ++i) {
		AudioProperties properties;
		int frameLength = 0;
		if (!parseMpegFrameHeader(data + i, properties, frameLength)) continue;

		// Confirm with the next frame where the file is long enough to check.
		const std::size_t next = i + static_cast<std::size_t>(frameLength);
		if (next + 4 <= size) {
			AudioProperties second;
			int secondLength = 0;
			if (!parseMpegFrameHeader(data + next, second, secondLength)) continue;
			if (second.sampleRateHz != properties.sampleRateHz) continue;
		}

		if (i > cursor) {
			layout.warnings.push_back("skipped " + std::to_string(i - cursor)
				+ " bytes of non-MPEG data before the first audio frame");
		}

		layout.audioOffset = static_cast<std::int64_t>(i);
		layout.audio = properties;
		layout.audio.audioOffset = layout.audioOffset;
		layout.audio.valid = true;
		found = true;

		// --- Xing / Info / VBRI header ------------------------------------
		// Its position depends on the MPEG version and channel mode.
		const std::uint8_t b1 = byteAt(data + i, 1);
		const std::uint8_t b3 = byteAt(data + i, 3);
		const int versionBits = (b1 >> 3) & 0x03;
		const int channelMode = (b3 >> 6) & 0x03;
		std::size_t sideInfo = 0;
		if (versionBits == 3) {
			sideInfo = (channelMode == 3) ? 17 : 32;
		} else {
			sideInfo = (channelMode == 3) ? 9 : 17;
		}
		const std::size_t xingOffset = i + 4 + sideInfo;

		if (xingOffset + 8 <= size) {
			const bool isXing = std::memcmp(data + xingOffset, "Xing", 4) == 0;
			const bool isInfo = std::memcmp(data + xingOffset, "Info", 4) == 0;
			if (isXing || isInfo) {
				layout.audio.hasXingHeader = true;
				// "Info" marks a CBR file written with the Xing frame layout.
				layout.audio.bitrateMode = isXing ? BitrateMode::Vbr : BitrateMode::Cbr;

				const std::uint32_t xingFlags = decodeBigEndian32(data + xingOffset + 4);
				std::size_t p = xingOffset + 8;
				std::uint32_t frameCount = 0;
				if ((xingFlags & 0x0001u) && p + 4 <= size) {
					frameCount = decodeBigEndian32(data + p);
					p += 4;
				}
				if ((xingFlags & 0x0002u) && p + 4 <= size) p += 4;    // byte count
				if ((xingFlags & 0x0004u) && p + 100 <= size) p += 100; // TOC
				if ((xingFlags & 0x0008u) && p + 4 <= size) p += 4;    // quality

				// LAME/Info tail: 9-byte encoder string, then the gapless fields.
				if (p + 36 <= size) {
					const bool lame = std::memcmp(data + p, "LAME", 4) == 0
						|| std::memcmp(data + p, "Lavf", 4) == 0
						|| std::memcmp(data + p, "Lavc", 4) == 0;
					if (lame) {
						layout.audio.hasLameHeader = true;
						// Encoder delay and padding are 12-bit fields packed into
						// three bytes at offset 21 of the LAME tag. They are
						// gapless information and are preserved, never rewritten.
						const std::size_t gapless = p + 21;
						if (gapless + 3 <= size) {
							const std::uint32_t packed = (static_cast<std::uint32_t>(byteAt(data, gapless)) << 16)
								| (static_cast<std::uint32_t>(byteAt(data, gapless + 1)) << 8)
								| static_cast<std::uint32_t>(byteAt(data, gapless + 2));
							layout.audio.encoderDelay = (packed >> 12) & 0x0FFF;
							layout.audio.encoderPadding = packed & 0x0FFF;
						}
					}
				}

				if (frameCount > 0 && properties.sampleRateHz > 0) {
					// Samples per frame: 1152 for MPEG 1 Layer III, 576 for MPEG 2/2.5.
					const int layerBits = (b1 >> 1) & 0x03;
					int samplesPerFrame = 1152;
					if (layerBits == 3) samplesPerFrame = 384;
					else if (versionBits != 3) samplesPerFrame = 576;

					const std::int64_t totalSamples =
						static_cast<std::int64_t>(frameCount) * samplesPerFrame;
					layout.audio.durationMs =
						(totalSamples * 1000) / properties.sampleRateHz;
				}
			}
		}
		break;
	}

	if (!found) {
		layout.warnings.push_back("no confirmed MPEG frame sync found");
		layout.audioOffset = static_cast<std::int64_t>(cursor);
	}

	layout.audioLength = static_cast<std::int64_t>(tailEnd) - layout.audioOffset;
	if (layout.audioLength < 0) {
		layout.warnings.push_back("tag containers overlap the audio payload");
		layout.audioLength = 0;
	}
	layout.audio.audioLength = layout.audioLength;

	// Duration from the bitrate when there was no Xing frame count.
	if (layout.audio.durationMs == 0 && layout.audio.bitrateKbps > 0 && layout.audioLength > 0) {
		layout.audio.durationMs = (layout.audioLength * 8) / layout.audio.bitrateKbps;
		if (layout.audio.bitrateMode == BitrateMode::Unknown) {
			layout.audio.bitrateMode = BitrateMode::Cbr;
		}
	}

	layout.valid = found;
	return layout;
}

Result<Mp3Layout> Mp3Container::readLayout(const fs::path& path) {
	FileHandle file(path);
	if (!file) {
		return Error{ErrorCode::IoError, "cannot open " + text::pathToUtf8(path)};
	}

	std::error_code ec;
	const std::uintmax_t size = fs::file_size(path, ec);
	if (ec) {
		return Error{ErrorCode::IoError, "cannot stat " + text::pathToUtf8(path) + ": " + ec.message()};
	}
	if (size == 0) {
		Mp3Layout empty;
		empty.warnings.push_back("file is empty");
		return empty;
	}

	// A small file is read whole. A large one is read as a head region big enough
	// to cover the ID3v2 tag and the first audio frames, plus a tail region for
	// APEv2 and ID3v1.
	const std::size_t fileSize = static_cast<std::size_t>(size);

	if (fileSize <= kTailProbeBytes * 2) {
		std::vector<std::byte> whole = readRange(file, 0, fileSize);
		if (whole.size() != fileSize) {
			return Error{ErrorCode::IoError, "short read on " + text::pathToUtf8(path)};
		}
		return parseLayoutFromBuffer(whole.data(), whole.size());
	}

	// Read the head. The ID3v2 size is known after 10 bytes, so the head only
	// needs to cover the tag plus the sync search window.
	std::vector<std::byte> head = readRange(file, 0, std::min<std::size_t>(fileSize, kId3v2HeaderBytes));
	if (head.size() < kId3v2HeaderBytes) {
		return Error{ErrorCode::IoError, "short read on " + text::pathToUtf8(path)};
	}

	std::size_t headBytes = kSyncSearchBytes;
	if (byteAt(head.data(), 0) == 'I' && byteAt(head.data(), 1) == 'D' && byteAt(head.data(), 2) == '3') {
		if (const auto declared = decodeSyncsafe(head.data() + 6)) {
			const std::size_t tagBytes = std::min<std::size_t>(
				static_cast<std::size_t>(*declared) + kId3v2HeaderBytes * 2, kMaxTagBytes);
			headBytes = tagBytes + kSyncSearchBytes;
		}
	}
	headBytes = std::min(headBytes, fileSize);

	// Build a buffer that is the head followed by the tail, with the gap zeroed.
	// Offsets in the buffer then match offsets in the file, which keeps the
	// parser's arithmetic honest without loading a 40 MB file into memory.
	const std::size_t tailBytes = std::min(kTailProbeBytes, fileSize - headBytes);

	std::vector<std::byte> buffer(fileSize, std::byte{0});
	{
		std::vector<std::byte> headData = readRange(file, 0, headBytes);
		if (headData.size() != headBytes) {
			return Error{ErrorCode::IoError, "short read on head of " + text::pathToUtf8(path)};
		}
		std::memcpy(buffer.data(), headData.data(), headBytes);
	}
	if (tailBytes > 0) {
		const std::int64_t tailOffset = static_cast<std::int64_t>(fileSize - tailBytes);
		std::vector<std::byte> tailData = readRange(file, tailOffset, tailBytes);
		if (tailData.size() != tailBytes) {
			return Error{ErrorCode::IoError, "short read on tail of " + text::pathToUtf8(path)};
		}
		std::memcpy(buffer.data() + (fileSize - tailBytes), tailData.data(), tailBytes);
	}

	return parseLayoutFromBuffer(buffer.data(), buffer.size());
}

std::vector<RawFrame> Mp3Container::parseFramesFromBuffer(const std::byte* tag, std::size_t size,
	TagContainer version, bool tagUnsynchronised, std::vector<std::string>& warnings) {
	std::vector<RawFrame> frames;

	if (size <= kId3v2HeaderBytes) return frames;

	const std::size_t idLength = (version == TagContainer::Id3v2_2) ? 3 : 4;
	const std::size_t sizeLength = (version == TagContainer::Id3v2_2) ? 3 : 4;
	const std::size_t flagLength = (version == TagContainer::Id3v2_2) ? 0 : 2;
	const std::size_t headerLength = idLength + sizeLength + flagLength;

	std::size_t cursor = kId3v2HeaderBytes;

	// --- Extended header ----------------------------------------------------
	// Skipped, not interpreted. Its size field is encoded differently in v2.3
	// and v2.4, and getting that wrong would shift every frame boundary.
	{
		const std::byte* header = tag;
		const bool extended = (byteAt(header, 5) & 0x40u) != 0;
		if (extended && cursor + 4 <= size) {
			std::uint32_t extendedSize = 0;
			if (version == TagContainer::Id3v2_4) {
				// v2.4: syncsafe, and the size includes itself.
				if (const auto s = decodeSyncsafe(tag + cursor)) extendedSize = *s;
				else warnings.push_back("ID3v2.4 extended header size is not syncsafe");
			} else {
				// v2.3: plain integer, excluding its own four bytes.
				extendedSize = decodeBigEndian32(tag + cursor) + 4;
			}
			if (extendedSize > 0 && cursor + extendedSize <= size) {
				cursor += extendedSize;
			} else {
				warnings.push_back("extended header size is implausible; frame parsing stopped");
				return frames;
			}
		}
	}

	// --- Frames -------------------------------------------------------------
	while (cursor + headerLength <= size) {
		// A zero byte where a frame identifier should be marks the start of the
		// padding region.
		if (byteAt(tag, cursor) == 0x00) break;

		RawFrame frame;
		frame.headerOffset = cursor;
		frame.id.assign(reinterpret_cast<const char*>(tag + cursor), idLength);

		// Frame identifiers are A-Z and 0-9. Anything else means we have lost
		// synchronisation with the frame boundaries.
		bool validId = true;
		for (char c : frame.id) {
			const unsigned char uc = static_cast<unsigned char>(c);
			if (!((uc >= 'A' && uc <= 'Z') || (uc >= '0' && uc <= '9'))) {
				validId = false;
				break;
			}
		}
		if (!validId) {
			warnings.push_back("frame identifier at offset " + std::to_string(cursor)
				+ " is not valid; frame parsing stopped");
			break;
		}

		std::uint32_t declared = 0;
		if (version == TagContainer::Id3v2_2) {
			declared = (static_cast<std::uint32_t>(byteAt(tag, cursor + 3)) << 16)
				| (static_cast<std::uint32_t>(byteAt(tag, cursor + 4)) << 8)
				| static_cast<std::uint32_t>(byteAt(tag, cursor + 5));
		} else if (version == TagContainer::Id3v2_4) {
			const auto s = decodeSyncsafe(tag + cursor + 4);
			if (s) {
				declared = *s;
			} else {
				// A common writer bug: v2.4 frames whose sizes are plain integers.
				// Fall back, but record that the file is non-conforming.
				declared = decodeBigEndian32(tag + cursor + 4);
				warnings.push_back("ID3v2.4 frame \"" + frame.id
					+ "\" has a non-syncsafe size; read as a plain integer");
			}
		} else {
			declared = decodeBigEndian32(tag + cursor + 4);
		}

		if (flagLength == 2) {
			frame.flags = static_cast<std::uint16_t>(
				(static_cast<std::uint16_t>(byteAt(tag, cursor + 8)) << 8)
				| static_cast<std::uint16_t>(byteAt(tag, cursor + 9)));
		}

		frame.declaredSize = declared;

		if (version == TagContainer::Id3v2_4) {
			frame.compressed = (frame.flags & 0x0008u) != 0;
			frame.encrypted = (frame.flags & 0x0004u) != 0;
			frame.unsynchronised = (frame.flags & 0x0002u) != 0;
			frame.hasDataLengthIndicator = (frame.flags & 0x0001u) != 0;
		} else if (version == TagContainer::Id3v2_3) {
			frame.compressed = (frame.flags & 0x0080u) != 0;
			frame.encrypted = (frame.flags & 0x0040u) != 0;
		}

		// Checked arithmetic: the declared size must fit inside the remaining tag.
		const std::size_t payloadStart = cursor + headerLength;
		if (declared > size || payloadStart > size || declared > size - payloadStart) {
			frame.truncated = true;
			frame.totalSize = static_cast<std::uint32_t>(size - cursor);
			const std::size_t available = size - payloadStart;
			frame.payload.assign(tag + payloadStart, tag + payloadStart + available);
			warnings.push_back("frame \"" + frame.id + "\" declares " + std::to_string(declared)
				+ " bytes but only " + std::to_string(available) + " remain; truncated");
			frames.push_back(std::move(frame));
			break;
		}

		frame.totalSize = static_cast<std::uint32_t>(headerLength + declared);
		frame.payload.assign(tag + payloadStart, tag + payloadStart + declared);

		// Unsynchronisation is undone per frame in v2.4, or for the whole tag in
		// earlier versions. Either way the stored payload is the real bytes.
		if (frame.unsynchronised || (tagUnsynchronised && version != TagContainer::Id3v2_4)) {
			frame.payload = undoUnsynchronisation(frame.payload.data(), frame.payload.size());
		}

		cursor += frame.totalSize;
		frames.push_back(std::move(frame));
	}

	return frames;
}

Result<std::vector<RawFrame>> Mp3Container::readRawFrames(const fs::path& path, Mp3Layout& layout) {
	if (!layout.hasId3v2 || layout.id3v2TotalSize <= 0) {
		return std::vector<RawFrame>{};
	}

	FileHandle file(path);
	if (!file) {
		return Error{ErrorCode::IoError, "cannot open " + text::pathToUtf8(path)};
	}

	const std::size_t tagBytes = static_cast<std::size_t>(layout.id3v2TotalSize);
	std::vector<std::byte> tag = readRange(file, layout.id3v2Offset, tagBytes);
	if (tag.size() < kId3v2HeaderBytes) {
		return Error{ErrorCode::IoError, "cannot read ID3v2 tag from " + text::pathToUtf8(path)};
	}

	auto frames = parseFramesFromBuffer(tag.data(), tag.size(), layout.id3v2Version,
		layout.id3v2Unsynchronised, layout.warnings);

	// Padding is whatever remains between the last frame and the end of the tag,
	// minus a footer if one is declared.
	std::size_t consumed = kId3v2HeaderBytes;
	if (!frames.empty()) {
		const RawFrame& last = frames.back();
		consumed = static_cast<std::size_t>(last.headerOffset) + last.totalSize;
	}
	std::size_t tagEnd = tag.size();
	if (layout.id3v2HasFooter && tagEnd >= kId3v2HeaderBytes) tagEnd -= kId3v2HeaderBytes;
	layout.id3v2PaddingBytes = (tagEnd > consumed) ? static_cast<std::int64_t>(tagEnd - consumed) : 0;

	return frames;
}

} // namespace ml
