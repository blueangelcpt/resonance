// SPDX-License-Identifier: GPL-3.0-or-later
// FN-TAG-01 / FN-TAG-02: container-level MP3 inspection.
//
// This is deliberately NOT a full MP3 parser. It locates the boundaries of the
// tag containers and the MPEG payload, and enumerates ID3v2 frame headers,
// because three requirements need exactly that and nothing more:
//
//   * the audio-only hash that proves a tag edit did not touch the stream,
//   * an ID3v2 frame inventory captured independently of TagLib, so a write can
//     be verified against something other than the library that performed it,
//   * padding accounting for the size policy.
//
// Every length is parsed with checked arithmetic against the actual buffer
// bounds. Untrusted tag buffers are never reinterpreted as structs.
#pragma once

#include "mlcore/TagModel.hpp"
#include "mlcore/Types.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace ml {

/// One ID3v2 frame as it physically appears on disk.
struct RawFrame {
	std::string id;                  ///< 3 characters for v2.2, 4 for v2.3/v2.4.
	std::uint64_t headerOffset = 0;  ///< Offset within the file.
	std::uint32_t declaredSize = 0;  ///< Size field value (payload only).
	std::uint32_t totalSize = 0;     ///< Header plus payload.
	std::uint16_t flags = 0;
	std::vector<std::byte> payload;  ///< Exact bytes, unsynchronisation undone if applied.
	bool compressed = false;
	bool encrypted = false;
	bool unsynchronised = false;
	bool hasDataLengthIndicator = false;
	bool truncated = false;          ///< Declared size ran past the tag.
};

/// The physical layout of one MP3 file.
struct Mp3Layout {
	std::int64_t fileSize = 0;

	// --- ID3v2 -------------------------------------------------------------
	bool hasId3v2 = false;
	TagContainer id3v2Version = TagContainer::Unknown;
	std::int64_t id3v2Offset = 0;        ///< Always 0 when present, kept explicit.
	std::int64_t id3v2TotalSize = 0;     ///< Header + extended header + frames + padding (+ footer).
	std::int64_t id3v2PaddingBytes = 0;
	bool id3v2Unsynchronised = false;
	bool id3v2ExtendedHeader = false;
	bool id3v2HasFooter = false;
	bool id3v2Experimental = false;

	// --- APEv2 -------------------------------------------------------------
	bool hasApev2 = false;
	std::int64_t apev2Offset = 0;
	std::int64_t apev2TotalSize = 0;
	std::uint32_t apev2ItemCount = 0;

	// --- ID3v1 -------------------------------------------------------------
	bool hasId3v1 = false;
	std::int64_t id3v1Offset = 0;

	// --- MPEG payload ------------------------------------------------------
	/// Byte range of the MPEG audio, excluding every tag container. Hashing this
	/// range is what proves a tag-only operation left the stream untouched.
	std::int64_t audioOffset = 0;
	std::int64_t audioLength = 0;

	AudioProperties audio;

	/// Non-fatal problems. A truncated frame or a bogus length is recorded and
	/// the scan continues (FN-SCAN-01).
	std::vector<std::string> warnings;

	bool valid = false;
};

/// Reads container layout and raw ID3v2 frames.
class Mp3Container {
public:
	/// Maximum ID3v2 tag size accepted. A larger declared size is treated as a
	/// malformed file rather than a reason to allocate it.
	static constexpr std::size_t kMaxTagBytes = 64u * 1024u * 1024u;

	/// Inspects layout only. Reads the header region and the file tail; it does
	/// not read the whole file.
	static Result<Mp3Layout> readLayout(const std::filesystem::path& path);

	/// Enumerates raw ID3v2 frames. Returns an empty vector when there is no
	/// ID3v2 tag. Malformed frames terminate enumeration and add a warning
	/// rather than throwing.
	static Result<std::vector<RawFrame>> readRawFrames(const std::filesystem::path& path, Mp3Layout& layout);

	/// Parses a layout from an in-memory buffer. Used by fuzz targets and tests
	/// so malformed inputs never need to touch the filesystem.
	static Mp3Layout parseLayoutFromBuffer(const std::byte* data, std::size_t size);

	/// Parses raw frames from an in-memory ID3v2 tag buffer (including its
	/// 10-byte header).
	static std::vector<RawFrame> parseFramesFromBuffer(const std::byte* tag, std::size_t size,
		TagContainer version, bool tagUnsynchronised, std::vector<std::string>& warnings);

	/// Decodes a syncsafe 28-bit integer. Returns nullopt when any byte has its
	/// high bit set, which means the value is not syncsafe.
	static std::optional<std::uint32_t> decodeSyncsafe(const std::byte* bytes);

	/// Decodes a plain 32-bit big-endian integer.
	static std::uint32_t decodeBigEndian32(const std::byte* bytes);

	/// Reverses ID3v2 unsynchronisation: every 0xFF 0x00 pair becomes 0xFF.
	static std::vector<std::byte> undoUnsynchronisation(const std::byte* data, std::size_t size);

	/// True when the four bytes look like a valid MPEG audio frame header.
	static bool isMpegFrameHeader(const std::byte* bytes);

	/// Parses one MPEG frame header into audio properties. Returns false when the
	/// header is not valid.
	static bool parseMpegFrameHeader(const std::byte* bytes, AudioProperties& out, int& frameLengthBytes);
};

} // namespace ml
