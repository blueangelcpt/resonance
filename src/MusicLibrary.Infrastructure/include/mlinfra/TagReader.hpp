// SPDX-License-Identifier: GPL-3.0-or-later
// CAT-001 / FN-TAG-01 / FN-TAG-02: reading the complete observed tag state.
//
// Two independent readers cover one file:
//
//   * TagLib decodes frame semantics, text encodings and picture structure.
//   * Mp3Container enumerates the same frames from the raw bytes.
//
// Their results are cross-checked. A disagreement is recorded as a warning
// rather than silently resolved, because the whole point of the independent
// inventory is that a write can be verified against something other than the
// library that performed it.
//
// Tag versions are never translated on read. A v2.3 file is reported as v2.3.
#pragma once

#include "mlcore/TagModel.hpp"
#include "mlcore/Types.hpp"
#include "mlinfra/Mp3Container.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace ml {

struct TagReadOptions {
	/// Read picture payloads to compute their hashes and dimensions. Off during
	/// a bulk scan, because artwork can be a large fraction of a file.
	bool loadPictureBytes = false;
	/// Compute the whole-file SHA-256.
	bool hashContent = false;
	/// Compute the MPEG-payload-only SHA-256, which proves a tag edit did not
	/// touch the audio.
	bool hashAudio = false;
	/// Keep the exact payload of every frame. Required before writing, so
	/// uninterpreted frames survive a rewrite.
	bool retainFramePayloads = true;
};

struct TagReadResult {
	TagSnapshot snapshot;
	Mp3Layout layout;
	std::vector<RawFrame> rawFrames;
};

class TagReader {
public:
	static Result<TagReadResult> read(const std::filesystem::path& path, TagReadOptions options = {});

	/// Compares a raw frame inventory with the frames TagLib reported and returns
	/// the discrepancies. Used both on read and after a write (FN-TAG-02).
	static std::vector<std::string> crossCheck(const std::vector<RawFrame>& rawFrames,
		const TagSnapshot& snapshot);

	/// Compares two snapshots and returns every frame that is present in `before`
	/// but missing or altered in `after`, ignoring frames the plan intended to
	/// change. This is the preservation gate.
	struct PreservationReport {
		std::vector<std::string> lostFrames;
		std::vector<std::string> alteredFrames;
		std::vector<std::string> addedFrames;
		bool audioUnchanged = false;
		std::string beforeAudioSha256;
		std::string afterAudioSha256;

		bool preserved() const { return lostFrames.empty() && alteredFrames.empty(); }
	};
	static PreservationReport comparePreservation(const TagReadResult& before, const TagReadResult& after,
		const std::vector<std::string>& intentionallyChangedKeys);

	/// Decodes the dimensions of an image buffer without fully decoding it.
	/// Supports JPEG and PNG, the two formats that appear in `APIC` frames in
	/// practice. Returns false for anything else rather than guessing.
	static bool probeImageDimensions(const std::byte* data, std::size_t size, int& width, int& height,
		std::string& mimeType);
};

} // namespace ml
