// SPDX-License-Identifier: GPL-3.0-or-later
// Frame-level tag model. The catalogue stores every discovered field, including
// fields this application cannot interpret (CAT-001, FN-TAG-01).
#pragma once

#include "mlcore/Types.hpp"

#include <cstddef>
#include <map>
#include <string>
#include <vector>

namespace ml {

/// Which physical tag container a field came from. Kept distinct so a write can
/// preserve the original container layout instead of silently upgrading it.
enum class TagContainer {
	Unknown = 0,
	Id3v1,
	Id3v2_2,
	Id3v2_3,
	Id3v2_4,
	Apev2,
	LameHeader,   ///< Encoder header fields (gapless, replay gain), never a normal frame.
};

std::string_view toString(TagContainer c);
std::optional<TagContainer> tagContainerFromString(std::string_view s);
bool isId3v2(TagContainer c);

/// Text encoding as declared by the source frame. Recorded so a rewrite can keep
/// the original declaration rather than adopting a library default.
enum class TextEncoding { Unknown = 0, Latin1, Utf16, Utf16Be, Utf8 };

std::string_view toString(TextEncoding e);

/// One tag field exactly as found.
///
/// `id` is the raw frame identifier (`TIT2`, `TXXX`, `PRIV`, `APIC`, or an APEv2
/// key). `description` holds the `TXXX`/`COMM`/`APIC` description, `owner` the
/// `PRIV`/`UFID` owner identifier. `value` holds the decoded text when the frame
/// is textual; `binary` holds the exact original payload whenever the frame is
/// binary or could not be interpreted, so it can be written back unchanged.
struct TagFrame {
	TagContainer container = TagContainer::Unknown;
	std::string id;
	std::string description;
	std::string owner;
	std::string language;       ///< ISO-639-2 for COMM/USLT/SYLT.
	std::string value;
	std::vector<std::byte> binary;
	TextEncoding encoding = TextEncoding::Unknown;
	int ordinal = 0;            ///< Position among same-id frames; duplicates are preserved.
	std::size_t rawSize = 0;    ///< Original on-disk frame size including its header.
	bool interpreted = false;   ///< False when only the opaque payload is understood.

	bool isText() const { return !value.empty() || (interpreted && binary.empty()); }
	bool isPicture() const { return id == "APIC" || id == "PIC" || id == "Cover Art (Front)"; }

	/// Stable key used for preview, diffing and policy matching.
	std::string key() const;
};

/// Roles defined by ID3v2 for attached pictures. Only the front cover is
/// replaced; other roles are inventoried and preserved (FN-ART-05).
enum class PictureType {
	Other = 0x00,
	FileIcon32 = 0x01,
	OtherFileIcon = 0x02,
	FrontCover = 0x03,
	BackCover = 0x04,
	LeafletPage = 0x05,
	Media = 0x06,
	LeadArtist = 0x07,
	Artist = 0x08,
	Conductor = 0x09,
	Band = 0x0A,
	Composer = 0x0B,
	Lyricist = 0x0C,
	RecordingLocation = 0x0D,
	DuringRecording = 0x0E,
	DuringPerformance = 0x0F,
	ScreenCapture = 0x10,
	Illustration = 0x12,
	BandLogo = 0x13,
	PublisherLogo = 0x14,
};

std::string_view toString(PictureType t);

/// An embedded picture, described without holding a decoded image.
struct EmbeddedPicture {
	PictureType type = PictureType::Other;
	std::string mimeType;
	std::string description;
	std::size_t byteLength = 0;
	int width = 0;              ///< 0 when not yet decoded.
	int height = 0;
	std::string contentSha256;
	int frameOrdinal = 0;
};

/// The complete observed tag state of one file at one point in time.
///
/// This is *observed* state. Proposed and written state live in separate
/// structures so a failed write can never make the catalogue claim success
/// (CAT-001).
struct TagSnapshot {
	SnapshotId id;
	FileId fileId;
	std::vector<TagFrame> frames;
	std::vector<EmbeddedPicture> pictures;

	/// Containers actually present on disk, in discovery order.
	std::vector<TagContainer> containers;
	TagContainer primaryContainer = TagContainer::Unknown;

	std::size_t id3v2TagBytes = 0;     ///< Whole ID3v2 tag including header and padding.
	std::size_t id3v2PaddingBytes = 0;
	std::size_t apeTagBytes = 0;
	bool hasId3v1 = false;
	bool id3v2Unsynchronised = false;
	bool id3v2ExtendedHeader = false;

	/// Frames the reader could not interpret. Their payloads are still retained.
	std::vector<std::string> uninterpretedFrameIds;

	/// Non-fatal problems found while reading, for example a truncated frame.
	std::vector<std::string> readWarnings;

	std::string contentSha256;  ///< Hash of the whole file at read time.
	std::string audioSha256;    ///< Hash of the MPEG payload only.

	const TagFrame* find(std::string_view frameId) const;
	std::vector<const TagFrame*> findAll(std::string_view frameId) const;
	const TagFrame* findUserText(std::string_view description) const;

	/// Convenience accessors. These read from the observed frames; they never
	/// invent a value and return an empty string when absent.
	std::string title() const;
	std::string artist() const;
	std::string albumArtist() const;
	std::string album() const;
	std::string genre() const;
	std::string date() const;
	std::string comment() const;
	std::string lyrics() const;
	std::optional<int> trackNumber() const;
	std::optional<int> trackTotal() const;
	std::optional<int> discNumber() const;
	std::optional<int> discTotal() const;
	std::optional<double> bpm() const;
	bool isCompilation() const;

	const EmbeddedPicture* frontCover() const;
};

/// Parses `"3/12"` style ID3 position fields. Returns nullopt for absent or
/// non-numeric leading components rather than guessing.
std::optional<int> parsePositionField(std::string_view text, bool wantTotal = false);

} // namespace ml
