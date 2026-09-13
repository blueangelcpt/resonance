// SPDX-License-Identifier: GPL-3.0-or-later
// The only component in the application that produces a modified MP3.
//
// It never edits in place. It reads a source, assembles a new file at a
// destination the PathGuard allows, and copies the MPEG payload byte for byte
// from the source. That last point is structural rather than aspirational: the
// audio bytes are memcpy'd from the source range, so "tag-only operations retain
// MPEG stream bytes" is true by construction and then verified by hash.
//
// Padding is controlled here rather than left to the tag library's default,
// because FN-OPT-01 specifies a padding budget and a library default would
// silently decide it.
#pragma once

#include "mlcore/ChangePlan.hpp"
#include "mlcore/TagModel.hpp"
#include "mlcore/Types.hpp"
#include "mlinfra/Mp3Container.hpp"
#include "mlinfra/PathGuard.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace ml {

/// What to change. Anything not named here is preserved.
struct TagWriteRequest {
	/// Frame-level decisions from the privacy and gain policies. Only `Remove`
	/// and `Modify` actions are acted on; `Review` never writes.
	std::vector<FrameDecision> decisions;

	/// Replacement front-cover bytes. Other picture roles are left untouched.
	std::optional<std::vector<std::byte>> frontCover;
	std::string frontCoverMimeType = "image/jpeg";
	std::string frontCoverDescription;

	/// Unsynchronised lyrics to embed, with an ISO-639-2 language code.
	std::optional<std::string> lyrics;
	std::string lyricsLanguage = "eng";

	/// Integer BPM for the standards-compatible `TBPM` frame.
	std::optional<int> bpm;

	/// Arbitrary text frame updates, for accepted metadata corrections.
	/// Key is the frame id, for example "TIT2".
	std::vector<std::pair<std::string, std::string>> textFrames;

	/// The ID3v2 version to write. `Unknown` preserves the source's version,
	/// which is the default: the FRD forbids automatic version translation.
	TagContainer outputVersion = TagContainer::Unknown;

	/// Padding retained after the last frame (FN-OPT-01).
	std::size_t targetPaddingBytes = 2048;

	/// Keep the trailing ID3v1 block when the source had one.
	bool keepId3v1 = true;
	/// Blank the ID3v1 comment field, as the strict privacy profile offers.
	bool clearId3v1Comment = false;

	/// Keep the APEv2 tag when the source had one.
	bool keepApev2 = true;
	/// APEv2 item keys to drop. Used by the gain policy.
	std::vector<std::string> removeApeKeys;
};

struct TagWriteResult {
	std::int64_t bytesBefore = 0;
	std::int64_t bytesAfter = 0;
	std::int64_t newId3v2Bytes = 0;
	std::int64_t newPaddingBytes = 0;
	std::int64_t audioBytesCopied = 0;

	/// Hash of the MPEG payload as written. Compared with the source's audio
	/// hash to prove the stream was not altered.
	std::string writtenAudioSha256;
	std::string sourceAudioSha256;
	std::string writtenContentSha256;

	/// Keys of frames the write intended to change, for the preservation check.
	std::vector<std::string> intentionallyChangedKeys;

	SizeAccounting size;

	bool audioPreserved() const {
		return !sourceAudioSha256.empty() && sourceAudioSha256 == writtenAudioSha256;
	}
};

class TagWriter {
public:
	/// Reads `sourcePath`, applies `request`, and writes the result to
	/// `destinationPath`. The destination must not exist; the caller supplies an
	/// exclusively-created temporary path.
	///
	/// The source is opened read-only and is never modified.
	static Result<TagWriteResult> writeToNewFile(const std::filesystem::path& sourcePath,
		const std::filesystem::path& destinationPath, const TagWriteRequest& request, const PathGuard& guard);

	/// Renders the ID3v2 tag bytes that `request` would produce for `sourcePath`,
	/// without writing anything. Used by the review preview to show exact sizes.
	static Result<std::vector<std::byte>> renderTag(const std::filesystem::path& sourcePath,
		const TagWriteRequest& request, std::int64_t& paddingBytesOut);

	/// Maps a TagContainer to the ID3v2 major version number.
	static int majorVersionOf(TagContainer container);
};

} // namespace ml
