// SPDX-License-Identifier: GPL-3.0-or-later
// ART-001 / FN-ART-02: the artwork derivative pipeline.
//
// The output configuration is fixed by the requirement: sRGB, Lanczos resize to
// 600x600, JPEG quality 75, optimised Huffman tables, baseline (non-progressive),
// 4:4:4 subsampling for fine lettering, EXIF and XMP stripped.
//
// The derivative is deterministic: the same source bytes and the same encoder
// configuration always produce the same output bytes, so an album can be given
// identical cover bytes throughout and a plan can be repeated (FN-ART-03).
//
// FRD section 3 suggested a bundled ImageMagick helper. This implements the same
// specified output with libjpeg-turbo and libpng directly; see
// docs/adr/0002-native-image-pipeline.md for the reasoning and the obligations
// that decision carries.
#pragma once

#include "mlcore/Types.hpp"

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace ml {

/// A decoded image in 8-bit sRGB.
struct RgbImage {
	int width = 0;
	int height = 0;
	/// Row-major, three bytes per pixel.
	std::vector<std::uint8_t> pixels;

	bool valid() const {
		return width > 0 && height > 0
			&& pixels.size() == static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 3u;
	}
	std::size_t stride() const { return static_cast<std::size_t>(width) * 3u; }
};

/// The encoder configuration. Every field is part of the derivative's identity
/// and is hashed into the config hash stored alongside it.
struct DerivativeConfig {
	int edgeSize = 600;
	int jpegQuality = 75;
	bool optimiseHuffman = true;
	bool progressive = false;
	/// 4:4:4 keeps fine lettering legible; 4:2:0 would blur coloured text.
	bool chromaSubsampling = false;
	std::string colourSpace = "sRGB";
	std::string resampler = "lanczos3";

	/// Stable identity of this configuration, stored with every derivative so a
	/// configuration change regenerates rather than silently reusing.
	std::string hash() const;
	std::string describe() const;
};

/// Result of generating a derivative.
struct DerivativeResult {
	std::vector<std::uint8_t> jpegBytes;
	int width = 0;
	int height = 0;
	std::string sha256;
	std::string configHash;
	/// Dimensions measured by decoding the source, never a claimed size.
	int sourceWidth = 0;
	int sourceHeight = 0;
	bool sourceWasUpscaled = false;
};

class ImagePipeline {
public:
	/// Decodes JPEG or PNG into sRGB. Returns an error for anything else rather
	/// than guessing; an undecodable candidate must not be selected.
	static Result<RgbImage> decode(const std::uint8_t* data, std::size_t size);
	static Result<RgbImage> decodeFile(const std::filesystem::path& path);

	/// Resizes with a separable Lanczos-3 kernel in linear light.
	///
	/// Resampling sRGB values directly darkens edges; converting to linear,
	/// resampling, and converting back is what "colour-managed" means here.
	static RgbImage resizeLanczos(const RgbImage& source, int targetWidth, int targetHeight);

	/// Encodes to JPEG with the exact configuration in `config`.
	static Result<std::vector<std::uint8_t>> encodeJpeg(const RgbImage& image, const DerivativeConfig& config);

	/// The whole pipeline: decode, check, resize, encode, hash.
	///
	/// Refuses to upscale: a source smaller than the target edge returns an
	/// error, because manufacturing compliance by upscaling is forbidden.
	static Result<DerivativeResult> makeDerivative(const std::uint8_t* data, std::size_t size,
		const DerivativeConfig& config = {});

	/// Reads an image's dimensions without decoding the pixels.
	static Result<std::pair<int, int>> probeDimensions(const std::uint8_t* data, std::size_t size);

	/// True when the buffer is a format this pipeline can decode.
	static bool isDecodable(const std::uint8_t* data, std::size_t size);
};

/// Content-addressed storage for received assets and derivatives.
///
/// FRD section 4: image bytes are stored once in an asset directory and
/// referenced, rather than repeated as database blobs.
class AssetStore {
public:
	explicit AssetStore(std::filesystem::path root);

	/// Stores bytes under their own SHA-256 and returns the relative path.
	/// Storing the same bytes twice is a no-op.
	Result<std::string> put(const std::uint8_t* data, std::size_t size, std::string_view extension);

	/// Absolute path for a stored asset.
	std::filesystem::path resolve(std::string_view relativePath) const;

	Result<std::vector<std::uint8_t>> get(std::string_view relativePath) const;

	bool contains(std::string_view relativePath) const;

	const std::filesystem::path& root() const { return m_root; }

private:
	std::filesystem::path m_root;
};

} // namespace ml
