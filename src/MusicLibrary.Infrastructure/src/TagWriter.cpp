// SPDX-License-Identifier: GPL-3.0-or-later
#include "mlinfra/TagWriter.hpp"
#include "mlcore/Text.hpp"
#include "mlinfra/Hashing.hpp"
#include "mlinfra/TagReader.hpp"

#include <taglib/apetag.h>
#include <taglib/attachedpictureframe.h>
#include <taglib/commentsframe.h>
#include <taglib/id3v1tag.h>
#include <taglib/id3v2frame.h>
#include <taglib/id3v2header.h>
#include <taglib/id3v2tag.h>
#include <taglib/mpegfile.h>
#include <taglib/privateframe.h>
#include <taglib/textidentificationframe.h>
#include <taglib/uniquefileidentifierframe.h>
#include <taglib/unsynchronizedlyricsframe.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <vector>

#ifndef _WIN32
#	include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace ml {

namespace {

// TagLib::FileName takes a wchar_t* on Windows (exact) or a char* elsewhere
// (assumed UTF-8, matching fs::path::string() on POSIX). fs::path::string()
// on Windows instead converts through the system ANSI codepage and silently
// mangles any character it cannot represent — routine for a real music
// library. wstring() keeps the exact Unicode path TagLib actually opens.
//
// Returned by value as a properly-owned string (not TagLib::FileName itself):
// on POSIX, FileName is just `const char*`, so a helper handing back a
// FileName built from a local std::string's c_str() would return a pointer
// into an already-destroyed temporary.
#ifdef _WIN32
std::wstring nativeTagPath(const fs::path& path) { return path.wstring(); }
#else
std::string nativeTagPath(const fs::path& path) { return path.string(); }
#endif

constexpr std::size_t kCopyChunk = 1u << 20;

class OutFile {
public:
	explicit OutFile(const fs::path& path) {
#ifdef _WIN32
		_wfopen_s(&m_file, path.wstring().c_str(), L"wb");
#else
		m_file = std::fopen(path.c_str(), "wb");
#endif
	}
	~OutFile() { close(); }

	OutFile(const OutFile&) = delete;
	OutFile& operator=(const OutFile&) = delete;

	bool write(const void* data, std::size_t size) {
		if (!m_file || size == 0) return m_file != nullptr;
		return std::fwrite(data, 1, size, m_file) == size;
	}

	/// Flushes user-space and OS buffers so the data is durable before the
	/// caller renames the file into place (FN-SAFE-02 step 5).
	bool flushToDisk() {
		if (!m_file) return false;
		if (std::fflush(m_file) != 0) return false;
#ifndef _WIN32
		if (::fsync(::fileno(m_file)) != 0) return false;
#endif
		return true;
	}

	void close() {
		if (m_file) {
			std::fclose(m_file);
			m_file = nullptr;
		}
	}

	explicit operator bool() const { return m_file != nullptr; }

private:
	std::FILE* m_file = nullptr;
};

class InFile {
public:
	explicit InFile(const fs::path& path) {
#ifdef _WIN32
		_wfopen_s(&m_file, path.wstring().c_str(), L"rb");
#else
		m_file = std::fopen(path.c_str(), "rb");
#endif
	}
	~InFile() { if (m_file) std::fclose(m_file); }

	InFile(const InFile&) = delete;
	InFile& operator=(const InFile&) = delete;

	bool seek(std::int64_t offset) {
#ifdef _WIN32
		return _fseeki64(m_file, offset, SEEK_SET) == 0;
#else
		return std::fseek(m_file, static_cast<long>(offset), SEEK_SET) == 0;
#endif
	}
	std::size_t read(void* buffer, std::size_t count) { return std::fread(buffer, 1, count, m_file); }
	explicit operator bool() const { return m_file != nullptr; }

private:
	std::FILE* m_file = nullptr;
};

TagLib::String toTagLib(const std::string& s) {
	return TagLib::String(s, TagLib::String::UTF8);
}

TagLib::ByteVector toByteVector(const std::vector<std::byte>& bytes) {
	return TagLib::ByteVector(reinterpret_cast<const char*>(bytes.data()),
		static_cast<unsigned int>(bytes.size()));
}

/// Matches one of this file's frames against a policy decision.
///
/// The decision carries the frame id plus the discriminator that made it unique
/// (owner for PRIV/UFID, description for TXXX/COMM). Matching on the id alone
/// would remove sibling frames the policy chose to keep.
bool frameMatchesDecision(const TagLib::ID3v2::Frame* frame, const FrameDecision& decision) {
	const std::string id(frame->frameID().data(), frame->frameID().size());
	if (id != decision.frameId) return false;

	if (const auto* priv = dynamic_cast<const TagLib::ID3v2::PrivateFrame*>(frame)) {
		return priv->owner().to8Bit(true) == decision.description;
	}
	if (const auto* ufid = dynamic_cast<const TagLib::ID3v2::UniqueFileIdentifierFrame*>(frame)) {
		return ufid->owner().to8Bit(true) == decision.description;
	}
	if (const auto* userText = dynamic_cast<const TagLib::ID3v2::UserTextIdentificationFrame*>(frame)) {
		return text::equalsNoCase(userText->description().to8Bit(true), decision.description);
	}
	if (const auto* comment = dynamic_cast<const TagLib::ID3v2::CommentsFrame*>(frame)) {
		const std::string description = comment->description().to8Bit(true);
		// A decision on a comment with an empty description targets the plain
		// comment frame, which is how most writers store it.
		return description == decision.description
			|| (decision.description == decision.frameId && description.empty());
	}
	// Frames with no discriminator: the id is the whole identity.
	return true;
}

} // namespace

int TagWriter::majorVersionOf(TagContainer container) {
	switch (container) {
		case TagContainer::Id3v2_2: return 2;
		case TagContainer::Id3v2_3: return 3;
		case TagContainer::Id3v2_4: return 4;
		default: return 0;
	}
}

namespace {

/// Applies a request to an in-memory ID3v2 tag. The tag belongs to a TagLib file
/// object that is never saved, so the source file is not touched.
std::vector<std::string> applyToTag(TagLib::ID3v2::Tag& tag, const TagWriteRequest& request) {
	std::vector<std::string> changedKeys;

	// --- Removals and modifications ----------------------------------------
	for (const auto& decision : request.decisions) {
		if (decision.action != FrameAction::Remove && decision.action != FrameAction::Modify) continue;
		// APEv2 and ID3v1 decisions are handled elsewhere.
		if (!isId3v2(decision.container)) continue;

		// Collect first: removeFrame() mutates the list being iterated.
		std::vector<TagLib::ID3v2::Frame*> matches;
		for (auto* frame : tag.frameList()) {
			if (frame && frameMatchesDecision(frame, decision)) matches.push_back(frame);
		}
		for (auto* frame : matches) {
			if (decision.action == FrameAction::Remove) {
				tag.removeFrame(frame, true);
				changedKeys.push_back(decision.frameKey);
			}
		}
	}

	// --- Front cover --------------------------------------------------------
	if (request.frontCover) {
		// Replace only the front-cover role. Back covers, band logos and
		// illustrations are inventoried and preserved (FN-ART-05).
		std::vector<TagLib::ID3v2::Frame*> oldCovers;
		for (auto* frame : tag.frameList("APIC")) {
			auto* picture = dynamic_cast<TagLib::ID3v2::AttachedPictureFrame*>(frame);
			if (!picture) continue;
			if (picture->type() == TagLib::ID3v2::AttachedPictureFrame::FrontCover) {
				oldCovers.push_back(frame);
			}
		}
		// A file whose only picture has type Other is treated as carrying its
		// cover there, so replacing the cover must replace that frame too.
		if (oldCovers.empty() && tag.frameList("APIC").size() == 1) {
			oldCovers.push_back(tag.frameList("APIC").front());
		}
		for (auto* frame : oldCovers) tag.removeFrame(frame, true);

		auto* picture = new TagLib::ID3v2::AttachedPictureFrame();
		picture->setType(TagLib::ID3v2::AttachedPictureFrame::FrontCover);
		picture->setMimeType(toTagLib(request.frontCoverMimeType));
		picture->setDescription(toTagLib(request.frontCoverDescription));
		picture->setPicture(toByteVector(*request.frontCover));
		tag.addFrame(picture);   // Ownership passes to the tag.
		changedKeys.push_back("APIC:front_cover");
	}

	// --- Lyrics -------------------------------------------------------------
	if (request.lyrics) {
		tag.removeFrames("USLT");
		if (!request.lyrics->empty()) {
			auto* lyrics = new TagLib::ID3v2::UnsynchronizedLyricsFrame(TagLib::String::UTF8);
			// ID3v2 requires a three-character language code; the policy layer
			// validated it before we get here.
			lyrics->setLanguage(TagLib::ByteVector(request.lyricsLanguage.c_str(), 3));
			lyrics->setDescription(TagLib::String());
			lyrics->setText(toTagLib(*request.lyrics));
			tag.addFrame(lyrics);
		}
		changedKeys.push_back("USLT");
	}

	// --- BPM ----------------------------------------------------------------
	if (request.bpm && *request.bpm > 0) {
		tag.removeFrames("TBPM");
		auto* bpm = new TagLib::ID3v2::TextIdentificationFrame("TBPM", TagLib::String::Latin1);
		bpm->setText(TagLib::String(std::to_string(*request.bpm), TagLib::String::Latin1));
		tag.addFrame(bpm);
		changedKeys.push_back("TBPM");
	}

	// --- Accepted text corrections ------------------------------------------
	for (const auto& [id, value] : request.textFrames) {
		if (id.size() != 4) continue;
		tag.removeFrames(TagLib::ByteVector(id.c_str(), 4));
		if (!value.empty()) {
			auto* frame = new TagLib::ID3v2::TextIdentificationFrame(
				TagLib::ByteVector(id.c_str(), 4), TagLib::String::UTF8);
			frame->setText(toTagLib(value));
			tag.addFrame(frame);
		}
		changedKeys.push_back(id);
	}

	return changedKeys;
}

/// Rewrites the 4-byte syncsafe size field of a rendered ID3v2 tag so it covers
/// the tag plus our chosen padding.
void setTagSize(std::vector<std::byte>& tag, std::size_t contentBytesAfterHeader) {
	const std::uint32_t value = static_cast<std::uint32_t>(contentBytesAfterHeader);
	tag[6] = static_cast<std::byte>((value >> 21) & 0x7F);
	tag[7] = static_cast<std::byte>((value >> 14) & 0x7F);
	tag[8] = static_cast<std::byte>((value >> 7) & 0x7F);
	tag[9] = static_cast<std::byte>(value & 0x7F);
}

} // namespace

Result<std::vector<std::byte>> TagWriter::renderTag(const fs::path& sourcePath,
	const TagWriteRequest& request, std::int64_t& paddingBytesOut) {
	auto layout = Mp3Container::readLayout(sourcePath);
	if (!layout) return layout.error();

	const auto nativeSourcePath = nativeTagPath(sourcePath);
	TagLib::MPEG::File file(nativeSourcePath.c_str(), false);
	if (!file.isValid()) {
		return Error{ErrorCode::ParseError, "cannot parse tags in " + text::pathToUtf8(sourcePath)};
	}

	// create = true so a file with no ID3v2 tag can still receive artwork.
	TagLib::ID3v2::Tag* tag = file.ID3v2Tag(true);
	if (!tag) {
		return Error{ErrorCode::ParseError, "cannot access an ID3v2 tag for " + text::pathToUtf8(sourcePath)};
	}

	(void)applyToTag(*tag, request);

	// Preserve the source version unless the caller asked for a specific one.
	// The tag library defaults to v2.4, which would silently translate the whole
	// library; FN-TAG-01 forbids that.
	TagContainer version = request.outputVersion;
	if (version == TagContainer::Unknown) {
		version = layout.value().hasId3v2 ? layout.value().id3v2Version : TagContainer::Id3v2_3;
	}
	if (!isId3v2(version)) version = TagContainer::Id3v2_3;

	// ID3v2.2 is read but not written: it cannot represent several frames this
	// application produces, and writing it would lose information.
	const int major = (majorVersionOf(version) == 2) ? 3 : majorVersionOf(version);
	const auto rendered = tag->render(major == 4 ? TagLib::ID3v2::v4 : TagLib::ID3v2::v3);

	std::vector<std::byte> bytes(rendered.size());
	std::memcpy(bytes.data(), rendered.data(), rendered.size());

	if (bytes.size() < 10) {
		return Error{ErrorCode::Internal, "rendered ID3v2 tag is impossibly small"};
	}

	// TagLib renders its own padding. Trim it back to the configured budget so
	// the size policy is ours, not the library's.
	//
	// The end of the frame data is found by parsing the frame headers, NOT by
	// scanning back over trailing zero bytes: a frame payload can legitimately
	// end in 0x00 (UTF-16 text, or image data), and trimming into it would
	// silently truncate the last frame.
	const TagContainer renderedVersion = (major == 4) ? TagContainer::Id3v2_4 : TagContainer::Id3v2_3;
	std::vector<std::string> parseWarnings;
	const auto renderedFrames = Mp3Container::parseFramesFromBuffer(
		bytes.data(), bytes.size(), renderedVersion, false, parseWarnings);

	std::size_t contentEnd = 10;
	if (!renderedFrames.empty()) {
		const RawFrame& last = renderedFrames.back();
		contentEnd = static_cast<std::size_t>(last.headerOffset) + last.totalSize;
	}
	if (contentEnd > bytes.size()) {
		return Error{ErrorCode::Internal, "rendered ID3v2 frame data overruns the rendered tag"};
	}

	const std::size_t padding = request.targetPaddingBytes;
	std::vector<std::byte> out(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(contentEnd));
	out.resize(contentEnd + padding, std::byte{0});
	setTagSize(out, out.size() - 10);

	paddingBytesOut = static_cast<std::int64_t>(padding);
	return out;
}

Result<TagWriteResult> TagWriter::writeToNewFile(const fs::path& sourcePath, const fs::path& destinationPath,
	const TagWriteRequest& request, const PathGuard& guard) {
	// --- Guard --------------------------------------------------------------
	const GuardDecision decision = guard.checkWrite(destinationPath);
	if (!decision.allowed()) {
		return Error{ErrorCode::ProtectedRootViolation,
			"refusing to write " + text::pathToUtf8(destinationPath) + ": " + decision.reason};
	}
	if (guard.isInsideProtectedRoot(destinationPath)) {
		return Error{ErrorCode::ProtectedRootViolation,
			"destination resolves inside a protected source root"};
	}

	const auto nativeSourcePath = nativeTagPath(sourcePath);

	auto layoutResult = Mp3Container::readLayout(sourcePath);
	if (!layoutResult) return layoutResult.error();
	Mp3Layout& layout = layoutResult.value();

	// readLayout does not walk the frame headers, so the source padding is only
	// known after the raw frame pass. The size accounting needs it.
	(void)Mp3Container::readRawFrames(sourcePath, layout);

	// `valid` means a confirmed MPEG frame sync was found. Checking only the
	// length is not enough: for a file that is not an MP3 at all, the "audio"
	// range is simply the whole file, and the writer would happily wrap arbitrary
	// bytes in an ID3 tag and call the result a track.
	if (!layout.valid || layout.audioLength <= 0) {
		return Error{ErrorCode::Unsupported,
			"no confirmed MPEG audio frame was located in " + text::pathToUtf8(sourcePath)
				+ "; refusing to rewrite it"};
	}

	TagWriteResult result;
	result.bytesBefore = layout.fileSize;

	// The source's audio hash is computed before writing so the comparison is
	// against the actual source, not against a value carried in from a plan.
	if (auto hash = hashFileRange(sourcePath, layout.audioOffset, layout.audioLength)) {
		result.sourceAudioSha256 = hash.value();
	} else {
		return hash.error();
	}

	// --- Build the new ID3v2 tag -------------------------------------------
	std::int64_t paddingBytes = 0;
	auto tagBytes = renderTag(sourcePath, request, paddingBytes);
	if (!tagBytes) return tagBytes.error();

	result.newId3v2Bytes = static_cast<std::int64_t>(tagBytes.value().size());
	result.newPaddingBytes = paddingBytes;

	// --- Collect the changed keys, for the preservation check ---------------
	std::vector<EmbeddedPicture> sourcePictures;
	{
		TagLib::MPEG::File probe(nativeSourcePath.c_str(), false);
		if (probe.isValid()) {
			if (TagLib::ID3v2::Tag* tag = probe.ID3v2Tag(true)) {
				for (auto* frame : tag->frameList("APIC")) {
					const auto* picture = dynamic_cast<const TagLib::ID3v2::AttachedPictureFrame*>(frame);
					if (!picture) continue;
					EmbeddedPicture p;
					p.type = static_cast<PictureType>(static_cast<int>(picture->type()));
					p.byteLength = picture->picture().size();
					sourcePictures.push_back(p);
				}
				result.intentionallyChangedKeys = applyToTag(*tag, request);
			}
		}
	}

	// --- Assemble the output ------------------------------------------------
	InFile source(sourcePath);
	if (!source) {
		return Error{ErrorCode::IoError, "cannot open source " + text::pathToUtf8(sourcePath)};
	}

	OutFile out(destinationPath);
	if (!out) {
		return Error{ErrorCode::IoError, "cannot create " + text::pathToUtf8(destinationPath)};
	}

	Sha256 contentHasher;
	Sha256 audioHasher;

	// 1. The new ID3v2 tag.
	if (!out.write(tagBytes.value().data(), tagBytes.value().size())) {
		return Error{ErrorCode::IoError, "write failed on the ID3v2 tag"};
	}
	contentHasher.update(tagBytes.value().data(), tagBytes.value().size());

	// 2. The MPEG payload, copied verbatim from the source range.
	//    This is what makes audio preservation structural rather than hoped for.
	if (!source.seek(layout.audioOffset)) {
		return Error{ErrorCode::IoError, "cannot seek to the audio payload"};
	}
	{
		std::vector<std::byte> buffer(kCopyChunk);
		std::int64_t remaining = layout.audioLength;
		while (remaining > 0) {
			const std::size_t want = static_cast<std::size_t>(
				std::min<std::int64_t>(remaining, static_cast<std::int64_t>(buffer.size())));
			const std::size_t read = source.read(buffer.data(), want);
			if (read == 0) {
				return Error{ErrorCode::IoError, "source ended before the planned audio length"};
			}
			if (!out.write(buffer.data(), read)) {
				return Error{ErrorCode::IoError, "write failed on the audio payload"};
			}
			contentHasher.update(buffer.data(), read);
			audioHasher.update(buffer.data(), read);
			remaining -= static_cast<std::int64_t>(read);
			result.audioBytesCopied += static_cast<std::int64_t>(read);
		}
	}

	// 3. APEv2, copied verbatim when kept. Item removal re-renders the tag.
	if (layout.hasApev2 && request.keepApev2) {
		if (request.removeApeKeys.empty()) {
			if (!source.seek(layout.apev2Offset)) {
				return Error{ErrorCode::IoError, "cannot seek to the APEv2 tag"};
			}
			std::vector<std::byte> buffer(static_cast<std::size_t>(layout.apev2TotalSize));
			const std::size_t read = source.read(buffer.data(), buffer.size());
			if (read != buffer.size()) {
				return Error{ErrorCode::IoError, "short read on the APEv2 tag"};
			}
			if (!out.write(buffer.data(), read)) {
				return Error{ErrorCode::IoError, "write failed on the APEv2 tag"};
			}
			contentHasher.update(buffer.data(), read);
		} else {
			TagLib::MPEG::File probe(nativeSourcePath.c_str(), false);
			if (probe.isValid() && probe.hasAPETag()) {
				TagLib::APE::Tag* ape = probe.APETag();
				for (const auto& key : request.removeApeKeys) {
					ape->removeItem(TagLib::String(key, TagLib::String::UTF8));
				}
				const auto rendered = ape->render();
				if (rendered.size() > 0) {
					if (!out.write(rendered.data(), rendered.size())) {
						return Error{ErrorCode::IoError, "write failed on the rebuilt APEv2 tag"};
					}
					contentHasher.update(rendered.data(), rendered.size());
				}
			}
		}
	}

	// 4. ID3v1, copied verbatim or with its comment field blanked.
	if (layout.hasId3v1 && request.keepId3v1) {
		if (!source.seek(layout.id3v1Offset)) {
			return Error{ErrorCode::IoError, "cannot seek to the ID3v1 tag"};
		}
		std::array<std::byte, 128> block{};
		const std::size_t read = source.read(block.data(), block.size());
		if (read != block.size()) {
			return Error{ErrorCode::IoError, "short read on the ID3v1 tag"};
		}
		if (request.clearId3v1Comment) {
			// Comment occupies bytes 97..126. Byte 125 holds the track number in
			// ID3v1.1, and byte 127 is the genre; neither is a comment.
			const bool isV11 = (block[125] == std::byte{0}) && (block[126] != std::byte{0});
			const std::size_t commentLength = isV11 ? 28 : 30;
			for (std::size_t i = 0; i < commentLength; ++i) block[97 + i] = std::byte{0};
		}
		if (!out.write(block.data(), block.size())) {
			return Error{ErrorCode::IoError, "write failed on the ID3v1 tag"};
		}
		contentHasher.update(block.data(), block.size());
	}

	if (!out.flushToDisk()) {
		return Error{ErrorCode::IoError, "cannot flush " + text::pathToUtf8(destinationPath) + " to disk"};
	}
	out.close();

	result.writtenAudioSha256 = audioHasher.finishHex();
	result.writtenContentSha256 = contentHasher.finishHex();

	std::error_code ec;
	result.bytesAfter = static_cast<std::int64_t>(fs::file_size(destinationPath, ec));
	if (ec) result.bytesAfter = 0;

	// --- Verify the audio survived -----------------------------------------
	if (!result.audioPreserved()) {
		std::error_code removeEc;
		fs::remove(destinationPath, removeEc);
		return Error{ErrorCode::VerificationFailed,
			"the written MPEG payload does not match the source; output discarded"};
	}

	// --- Size accounting, kept separate by cause (FN-OPT-02) ---------------
	//
	// Three independent numbers, because netting them into one would hide
	// enrichment growth behind optimisation savings, which is exactly what the
	// requirement forbids.
	{
		std::int64_t removedByPolicy = 0;
		for (const auto& d : request.decisions) {
			if (d.action == FrameAction::Remove) removedByPolicy += static_cast<std::int64_t>(d.beforeBytes);
		}

		std::int64_t enrichmentAdded = 0;
		if (request.frontCover) {
			enrichmentAdded += static_cast<std::int64_t>(request.frontCover->size());
			// The cover being replaced is not new bytes.
			for (const auto& picture : sourcePictures) {
				if (picture.type == PictureType::FrontCover) {
					enrichmentAdded -= static_cast<std::int64_t>(picture.byteLength);
					break;
				}
			}
		}
		if (request.lyrics) enrichmentAdded += static_cast<std::int64_t>(request.lyrics->size());

		result.size.optimisationSavings = removedByPolicy;
		result.size.enrichmentGrowth = std::max<std::int64_t>(enrichmentAdded, 0);
		result.size.paddingDelta = result.newPaddingBytes - layout.id3v2PaddingBytes;
		result.size.netDelta = result.bytesAfter - result.bytesBefore;
	}

	return result;
}

} // namespace ml
