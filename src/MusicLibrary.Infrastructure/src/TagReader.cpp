// SPDX-License-Identifier: GPL-3.0-or-later
#include "mlinfra/TagReader.hpp"
#include "mlcore/Text.hpp"
#include "mlinfra/Hashing.hpp"

#include <taglib/apefile.h>
#include <taglib/apeitem.h>
#include <taglib/apetag.h>
#include <taglib/attachedpictureframe.h>
#include <taglib/commentsframe.h>
#include <taglib/id3v1tag.h>
#include <taglib/id3v2frame.h>
#include <taglib/id3v2header.h>
#include <taglib/id3v2tag.h>
#include <taglib/mpegfile.h>
#include <taglib/mpegproperties.h>
#include <taglib/popularimeterframe.h>
#include <taglib/privateframe.h>
#include <taglib/relativevolumeframe.h>
#include <taglib/synchronizedlyricsframe.h>
#include <taglib/textidentificationframe.h>
#include <taglib/tfile.h>
#include <taglib/tpropertymap.h>
#include <taglib/uniquefileidentifierframe.h>
#include <taglib/unknownframe.h>
#include <taglib/unsynchronizedlyricsframe.h>

#include <algorithm>
#include <map>
#include <set>

namespace fs = std::filesystem;

namespace ml {

namespace {

std::string toStd(const TagLib::String& s) {
	return s.to8Bit(true);   // true == UTF-8
}

std::string toStd(const TagLib::ByteVector& v) {
	return std::string(v.data(), v.size());
}

std::vector<std::byte> toBytes(const TagLib::ByteVector& v) {
	std::vector<std::byte> out(v.size());
	for (unsigned int i = 0; i < v.size(); ++i) {
		out[i] = static_cast<std::byte>(static_cast<unsigned char>(v[static_cast<int>(i)]));
	}
	return out;
}

TextEncoding mapEncoding(TagLib::String::Type type) {
	switch (type) {
		case TagLib::String::Latin1: return TextEncoding::Latin1;
		case TagLib::String::UTF16: return TextEncoding::Utf16;
		case TagLib::String::UTF16BE: return TextEncoding::Utf16Be;
		case TagLib::String::UTF8: return TextEncoding::Utf8;
		case TagLib::String::UTF16LE: return TextEncoding::Utf16;
		default: return TextEncoding::Unknown;
	}
}

PictureType mapPictureType(TagLib::ID3v2::AttachedPictureFrame::Type type) {
	return static_cast<PictureType>(static_cast<int>(type));
}

TagContainer mapId3v2Version(unsigned int major) {
	switch (major) {
		case 2: return TagContainer::Id3v2_2;
		case 3: return TagContainer::Id3v2_3;
		case 4: return TagContainer::Id3v2_4;
		default: return TagContainer::Unknown;
	}
}

/// Joins a multi-value text frame with NUL, preserving the fact that the frame
/// carried several values. The naming layer treats an embedded NUL as "this is a
/// decision, not a value".
std::string joinValues(const TagLib::StringList& values) {
	std::string out;
	// TagLib's List indexes with an unsigned type; staying unsigned throughout
	// avoids a signed/unsigned round trip on every element.
	for (unsigned int i = 0; i < values.size(); ++i) {
		if (i > 0) out.push_back('\0');
		out += toStd(values[i]);
	}
	return out;
}

} // namespace

bool TagReader::probeImageDimensions(const std::byte* data, std::size_t size, int& width, int& height,
	std::string& mimeType) {
	width = 0;
	height = 0;
	mimeType.clear();
	if (!data || size < 24) return false;

	const auto at = [data](std::size_t i) { return static_cast<unsigned char>(data[i]); };

	// --- PNG: IHDR is always the first chunk -------------------------------
	static constexpr unsigned char kPngMagic[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
	bool isPng = true;
	for (std::size_t i = 0; i < 8; ++i) {
		if (at(i) != kPngMagic[i]) { isPng = false; break; }
	}
	if (isPng && size >= 24) {
		mimeType = "image/png";
		width = static_cast<int>((static_cast<std::uint32_t>(at(16)) << 24) | (static_cast<std::uint32_t>(at(17)) << 16)
			| (static_cast<std::uint32_t>(at(18)) << 8) | at(19));
		height = static_cast<int>((static_cast<std::uint32_t>(at(20)) << 24) | (static_cast<std::uint32_t>(at(21)) << 16)
			| (static_cast<std::uint32_t>(at(22)) << 8) | at(23));
		return width > 0 && height > 0;
	}

	// --- JPEG: walk the marker segments to the first SOF --------------------
	if (at(0) == 0xFF && at(1) == 0xD8) {
		mimeType = "image/jpeg";
		std::size_t i = 2;
		while (i + 4 <= size) {
			if (at(i) != 0xFF) { ++i; continue; }     // Skip fill bytes.
			const unsigned char marker = at(i + 1);
			if (marker == 0xFF) { ++i; continue; }
			// Markers without a payload.
			if (marker == 0xD8 || marker == 0xD9 || (marker >= 0xD0 && marker <= 0xD7) || marker == 0x01) {
				i += 2;
				continue;
			}
			if (i + 4 > size) break;
			const std::size_t segmentLength = (static_cast<std::size_t>(at(i + 2)) << 8) | at(i + 3);
			if (segmentLength < 2 || i + 2 + segmentLength > size) break;

			// SOF0..SOF15 except the DHT/JPG/DAC markers, which are not frame headers.
			const bool isSof = (marker >= 0xC0 && marker <= 0xCF)
				&& marker != 0xC4 && marker != 0xC8 && marker != 0xCC;
			if (isSof) {
				if (i + 9 > size) break;
				height = static_cast<int>((static_cast<std::uint32_t>(at(i + 5)) << 8) | at(i + 6));
				width = static_cast<int>((static_cast<std::uint32_t>(at(i + 7)) << 8) | at(i + 8));
				return width > 0 && height > 0;
			}
			i += 2 + segmentLength;
		}
		return false;
	}

	// --- GIF and WebP appear occasionally; identify but do not claim support --
	if (size >= 10 && at(0) == 'G' && at(1) == 'I' && at(2) == 'F') {
		mimeType = "image/gif";
		width = static_cast<int>(at(6) | (static_cast<std::uint32_t>(at(7)) << 8));
		height = static_cast<int>(at(8) | (static_cast<std::uint32_t>(at(9)) << 8));
		return width > 0 && height > 0;
	}
	if (size >= 16 && at(0) == 'R' && at(1) == 'I' && at(2) == 'F' && at(3) == 'F'
		&& at(8) == 'W' && at(9) == 'E' && at(10) == 'B' && at(11) == 'P') {
		mimeType = "image/webp";
		return false;   // Dimensions need a real WebP parser; report unmeasured.
	}

	return false;
}

Result<TagReadResult> TagReader::read(const fs::path& path, TagReadOptions options) {
	TagReadResult result;

	// --- Pass 1: raw container layout and frame inventory -------------------
	auto layout = Mp3Container::readLayout(path);
	if (!layout) return layout.error();
	result.layout = layout.value();

	auto rawFrames = Mp3Container::readRawFrames(path, result.layout);
	if (!rawFrames) return rawFrames.error();
	result.rawFrames = std::move(rawFrames.value());

	TagSnapshot& snapshot = result.snapshot;
	snapshot.id3v2TagBytes = static_cast<std::size_t>(result.layout.id3v2TotalSize);
	snapshot.id3v2PaddingBytes = static_cast<std::size_t>(result.layout.id3v2PaddingBytes);
	snapshot.apeTagBytes = static_cast<std::size_t>(result.layout.apev2TotalSize);
	snapshot.hasId3v1 = result.layout.hasId3v1;
	snapshot.id3v2Unsynchronised = result.layout.id3v2Unsynchronised;
	snapshot.id3v2ExtendedHeader = result.layout.id3v2ExtendedHeader;
	snapshot.readWarnings = result.layout.warnings;

	// --- Pass 2: TagLib for decoded semantics -------------------------------
	// readAudioProperties is false: the container parser already measured them,
	// and doing it twice on 3000 files is wasted I/O.
	TagLib::MPEG::File file(path.string().c_str(), false);
	if (!file.isValid()) {
		snapshot.readWarnings.push_back("TagLib could not open the file; only the raw inventory is available");
	}

	// Ordinals disambiguate frames that are otherwise identical, and they are
	// counted per *discriminated* key rather than per frame id.
	//
	// Counting per id makes the ordinal positional: removing the first of two
	// TXXX frames renumbers the survivor from 1 to 0, its key changes, and a
	// preservation check then reports an untouched frame as lost and re-added.
	// Keying on the discriminator keeps the survivor's identity stable.
	std::map<std::string, int> ordinals;
	const auto assignOrdinal = [&ordinals](TagFrame& frame) {
		std::string discriminator = std::string(toString(frame.container)) + "/" + frame.id;
		discriminator += "|" + frame.owner;
		discriminator += "|" + frame.description;
		discriminator += "|" + frame.language;
		frame.ordinal = ordinals[discriminator]++;
	};
	const auto pushFrame = [&](TagFrame& frame) {
		assignOrdinal(frame);
		snapshot.frames.push_back(std::move(frame));
	};

	if (file.isValid() && file.hasID3v2Tag()) {
		TagLib::ID3v2::Tag* tag = file.ID3v2Tag();
		const unsigned int major = tag->header() ? tag->header()->majorVersion() : 0;
		snapshot.primaryContainer = mapId3v2Version(major);
		snapshot.containers.push_back(snapshot.primaryContainer);

		for (const auto* frame : tag->frameList()) {
			if (!frame) continue;

			TagFrame out;
			out.container = snapshot.primaryContainer;
			out.id = toStd(frame->frameID());
			out.rawSize = frame->size() + frame->headerSize();

			// --- Attached picture ------------------------------------------
			if (const auto* picture = dynamic_cast<const TagLib::ID3v2::AttachedPictureFrame*>(frame)) {
				out.interpreted = true;
				out.description = toStd(picture->description());
				out.value = toStd(picture->mimeType());
				out.encoding = mapEncoding(picture->textEncoding());

				EmbeddedPicture embedded;
				embedded.type = mapPictureType(picture->type());
				embedded.mimeType = toStd(picture->mimeType());
				embedded.description = toStd(picture->description());
				embedded.byteLength = picture->picture().size();

				const TagLib::ByteVector& bytes = picture->picture();
				if (bytes.size() > 0) {
					int width = 0;
					int height = 0;
					std::string mime;
					const auto* raw = reinterpret_cast<const std::byte*>(bytes.data());
					if (probeImageDimensions(raw, bytes.size(), width, height, mime)) {
						embedded.width = width;
						embedded.height = height;
						if (!mime.empty() && mime != embedded.mimeType) {
							// A frame whose declared MIME type disagrees with the
							// actual bytes is worth surfacing, not correcting.
							snapshot.readWarnings.push_back("APIC declares \"" + embedded.mimeType
								+ "\" but the payload is " + mime);
						}
					}
					if (options.loadPictureBytes) {
						embedded.contentSha256 = Sha256::hashBytes(raw, bytes.size());
					}
				}
				if (options.retainFramePayloads) out.binary = toBytes(picture->picture());

				// The ordinal is assigned here, not by pushFrame, because the
				// picture record has to carry the same value. Calling both would
				// count this frame twice and leave the two records disagreeing,
				// which breaks looking a picture's bytes up by its frame ordinal.
				assignOrdinal(out);
				embedded.frameOrdinal = out.ordinal;
				snapshot.pictures.push_back(std::move(embedded));
				snapshot.frames.push_back(std::move(out));
				continue;
			}

			// --- User-defined text (TXXX) ----------------------------------
			if (const auto* userText = dynamic_cast<const TagLib::ID3v2::UserTextIdentificationFrame*>(frame)) {
				out.interpreted = true;
				out.description = toStd(userText->description());
				out.encoding = mapEncoding(userText->textEncoding());
				// fieldList()[0] is the description; the values follow it.
				const TagLib::StringList values = userText->fieldList();
				TagLib::StringList tail;
				for (unsigned int i = 1; i < values.size(); ++i) tail.append(values[i]);
				out.value = joinValues(tail);
				pushFrame(out);
				continue;
			}

			// --- Plain text identification ---------------------------------
			if (const auto* text = dynamic_cast<const TagLib::ID3v2::TextIdentificationFrame*>(frame)) {
				out.interpreted = true;
				out.encoding = mapEncoding(text->textEncoding());
				out.value = joinValues(text->fieldList());
				pushFrame(out);
				continue;
			}

			// --- Comments ---------------------------------------------------
			if (const auto* comment = dynamic_cast<const TagLib::ID3v2::CommentsFrame*>(frame)) {
				out.interpreted = true;
				out.description = toStd(comment->description());
				out.language = toStd(comment->language());
				out.encoding = mapEncoding(comment->textEncoding());
				out.value = toStd(comment->text());
				pushFrame(out);
				continue;
			}

			// --- Unsynchronised lyrics -------------------------------------
			if (const auto* lyrics = dynamic_cast<const TagLib::ID3v2::UnsynchronizedLyricsFrame*>(frame)) {
				out.interpreted = true;
				out.description = toStd(lyrics->description());
				out.language = toStd(lyrics->language());
				out.encoding = mapEncoding(lyrics->textEncoding());
				out.value = toStd(lyrics->text());
				pushFrame(out);
				continue;
			}

			// --- Private frames --------------------------------------------
			if (const auto* priv = dynamic_cast<const TagLib::ID3v2::PrivateFrame*>(frame)) {
				out.interpreted = true;
				out.owner = toStd(priv->owner());
				if (options.retainFramePayloads) out.binary = toBytes(priv->data());
				pushFrame(out);
				continue;
			}

			// --- Unique file identifier ------------------------------------
			if (const auto* ufid = dynamic_cast<const TagLib::ID3v2::UniqueFileIdentifierFrame*>(frame)) {
				out.interpreted = true;
				out.owner = toStd(ufid->owner());
				out.value = toStd(ufid->identifier());
				if (options.retainFramePayloads) out.binary = toBytes(ufid->identifier());
				pushFrame(out);
				continue;
			}

			// --- Relative volume -------------------------------------------
			if (const auto* rva = dynamic_cast<const TagLib::ID3v2::RelativeVolumeFrame*>(frame)) {
				out.interpreted = true;
				out.description = toStd(rva->identification());
				// The gain value itself is not decoded here: the gain policy only
				// needs to know the frame exists and could be interpreted.
				out.value = "relative volume adjustment";
				if (options.retainFramePayloads) out.binary = toBytes(rva->render());
				pushFrame(out);
				continue;
			}

			// --- Popularimeter ---------------------------------------------
			if (const auto* popm = dynamic_cast<const TagLib::ID3v2::PopularimeterFrame*>(frame)) {
				out.interpreted = true;
				out.owner = toStd(popm->email());
				out.value = std::to_string(popm->rating()) + "/" + std::to_string(popm->counter());
				pushFrame(out);
				continue;
			}

			// --- Anything else ----------------------------------------------
			// The payload is retained exactly so it can be written back unchanged.
			// This is the requirement that "retain unknown fields" actually means.
			out.interpreted = false;
			snapshot.uninterpretedFrameIds.push_back(out.id);
			if (options.retainFramePayloads) {
				if (const auto* unknown = dynamic_cast<const TagLib::ID3v2::UnknownFrame*>(frame)) {
					out.binary = toBytes(unknown->data());
				} else {
					out.binary = toBytes(frame->render());
				}
			}
			out.value = toStd(frame->toString());
			pushFrame(out);
		}
	}

	// --- APEv2 --------------------------------------------------------------
	if (file.isValid() && file.hasAPETag()) {
		snapshot.containers.push_back(TagContainer::Apev2);
		TagLib::APE::Tag* ape = file.APETag();
		for (const auto& [key, item] : ape->itemListMap()) {
			TagFrame out;
			out.container = TagContainer::Apev2;
			out.id = toStd(key);
			out.interpreted = true;
			out.rawSize = static_cast<std::size_t>(item.size());
			if (item.type() == TagLib::APE::Item::Binary) {
				out.interpreted = false;
				if (options.retainFramePayloads) out.binary = toBytes(item.binaryData());
			} else {
				out.value = joinValues(item.values());
			}
			pushFrame(out);
		}
		if (snapshot.primaryContainer == TagContainer::Unknown) {
			snapshot.primaryContainer = TagContainer::Apev2;
		}
	}

	// --- ID3v1 --------------------------------------------------------------
	// Read last and never promoted over ID3v2: it is a 128-byte truncated
	// remnant, not a source of truth.
	if (file.isValid() && file.hasID3v1Tag()) {
		snapshot.containers.push_back(TagContainer::Id3v1);
		TagLib::ID3v1::Tag* v1 = file.ID3v1Tag();

		const auto addV1 = [&](std::string id, std::string value, std::size_t size) {
			if (value.empty()) return;
			TagFrame out;
			out.container = TagContainer::Id3v1;
			out.id = std::move(id);
			out.value = std::move(value);
			out.encoding = TextEncoding::Latin1;
			out.interpreted = true;
			out.rawSize = size;
			pushFrame(out);
		};
		addV1("TITLE", toStd(v1->title()), 30);
		addV1("ARTIST", toStd(v1->artist()), 30);
		addV1("ALBUM", toStd(v1->album()), 30);
		addV1("YEAR", toStd(v1->year() ? std::to_string(v1->year()) : std::string()), 4);
		addV1("COMMENT", toStd(v1->comment()), 30);
		if (v1->track() > 0) addV1("TRACK", std::to_string(v1->track()), 1);

		if (snapshot.primaryContainer == TagContainer::Unknown) {
			snapshot.primaryContainer = TagContainer::Id3v1;
		}
	}

	// --- LAME header gain evidence ------------------------------------------
	// Recorded as a warning so the gain policy can raise its exception without
	// this layer having to know about gain rules.
	if (result.layout.audio.hasLameHeader) {
		TagFrame out;
		out.container = TagContainer::LameHeader;
		out.id = "LAME_GAPLESS";
		out.interpreted = true;
		out.value = std::to_string(result.layout.audio.encoderDelay) + "/"
			+ std::to_string(result.layout.audio.encoderPadding);
		out.description = "encoder delay/padding";
		pushFrame(out);
		snapshot.containers.push_back(TagContainer::LameHeader);
	}

	// --- Hashes -------------------------------------------------------------
	if (options.hashContent) {
		if (auto hash = hashFile(path)) snapshot.contentSha256 = hash.value();
	}
	if (options.hashAudio && result.layout.audioLength > 0) {
		if (auto hash = hashFileRange(path, result.layout.audioOffset, result.layout.audioLength)) {
			snapshot.audioSha256 = hash.value();
		}
	}

	// --- Cross-check --------------------------------------------------------
	for (auto& warning : crossCheck(result.rawFrames, snapshot)) {
		snapshot.readWarnings.push_back(std::move(warning));
	}

	return result;
}

std::vector<std::string> TagReader::crossCheck(const std::vector<RawFrame>& rawFrames,
	const TagSnapshot& snapshot) {
	std::vector<std::string> warnings;

	std::multiset<std::string> rawIds;
	for (const auto& f : rawFrames) {
		if (f.truncated) {
			warnings.push_back("raw reader saw a truncated frame \"" + f.id + "\"");
			continue;
		}
		rawIds.insert(f.id);
	}

	std::multiset<std::string> libIds;
	for (const auto& f : snapshot.frames) {
		if (!isId3v2(f.container)) continue;
		libIds.insert(f.id);
	}

	// Frames the raw reader saw but TagLib did not surface. This is the case the
	// independent inventory exists to catch: a frame that would be dropped by a
	// naive save.
	for (const auto& id : rawIds) {
		if (libIds.count(id) < rawIds.count(id)) {
			const auto message = "frame \"" + id + "\" appears " + std::to_string(rawIds.count(id))
				+ " time(s) in the raw tag but " + std::to_string(libIds.count(id))
				+ " time(s) via the tag library";
			if (std::find(warnings.begin(), warnings.end(), message) == warnings.end()) {
				warnings.push_back(message);
			}
		}
	}

	return warnings;
}

TagReader::PreservationReport TagReader::comparePreservation(const TagReadResult& before,
	const TagReadResult& after, const std::vector<std::string>& intentionallyChangedKeys) {
	PreservationReport report;
	report.beforeAudioSha256 = before.snapshot.audioSha256;
	report.afterAudioSha256 = after.snapshot.audioSha256;
	report.audioUnchanged = !before.snapshot.audioSha256.empty()
		&& before.snapshot.audioSha256 == after.snapshot.audioSha256;

	// A planned change is named either by its full frame key ("id3v2.3/TPE1")
	// or by the bare frame id ("TPE1"), depending on which layer recorded it.
	// Both spellings must count as intended, or every deliberate edit would be
	// reported as a preservation failure.
	const auto intended = [&](const std::string& key) {
		for (const auto& changed : intentionallyChangedKeys) {
			if (changed == key) return true;
			const std::size_t slash = key.find('/');
			const std::string bare = (slash == std::string::npos) ? key : key.substr(slash + 1);
			if (changed == bare) return true;
			// Keys carry ":owner=", ":desc=" and ":#n" discriminators; a change
			// recorded against the bare id covers all of its instances.
			const std::size_t colon = bare.find(':');
			if (colon != std::string::npos && changed == bare.substr(0, colon)) return true;
			// "APIC:front_cover" covers the front-cover picture frame.
			if (changed.rfind("APIC:", 0) == 0 && bare.rfind("APIC", 0) == 0) return true;
		}
		return false;
	};

	std::map<std::string, const TagFrame*> beforeByKey;
	for (const auto& f : before.snapshot.frames) beforeByKey[f.key()] = &f;

	std::map<std::string, const TagFrame*> afterByKey;
	for (const auto& f : after.snapshot.frames) afterByKey[f.key()] = &f;

	for (const auto& [key, frame] : beforeByKey) {
		if (intended(key)) continue;
		auto it = afterByKey.find(key);
		if (it == afterByKey.end()) {
			report.lostFrames.push_back(key);
			continue;
		}
		// Compare the decoded value and the retained payload. Either changing
		// without being planned is a preservation failure.
		if (frame->value != it->second->value || frame->binary != it->second->binary) {
			report.alteredFrames.push_back(key);
		}
	}

	for (const auto& [key, frame] : afterByKey) {
		(void)frame;
		if (intended(key)) continue;
		if (beforeByKey.find(key) == beforeByKey.end()) {
			report.addedFrames.push_back(key);
		}
	}

	return report;
}

} // namespace ml
