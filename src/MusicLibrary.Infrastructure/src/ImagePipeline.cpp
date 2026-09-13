// SPDX-License-Identifier: GPL-3.0-or-later
#include "mlinfra/ImagePipeline.hpp"
#include "mlinfra/Hashing.hpp"

#include <jpeglib.h>
#include <png.h>

#include <algorithm>
#include <cmath>
#include <csetjmp>
#include <cstring>
#include <fstream>

namespace fs = std::filesystem;

namespace ml {

namespace {

/// Guard against a decompression bomb. A cover art frame has no business
/// decoding to more than this.
constexpr std::size_t kMaxPixels = 64u * 1024u * 1024u;

// ---------------------------------------------------------------------------
// sRGB transfer function. Resampling happens in linear light.
// ---------------------------------------------------------------------------

const std::array<float, 256>& srgbToLinearTable() {
	static const std::array<float, 256> table = [] {
		std::array<float, 256> t{};
		for (int i = 0; i < 256; ++i) {
			const float c = static_cast<float>(i) / 255.0f;
			t[static_cast<std::size_t>(i)] = (c <= 0.04045f)
				? (c / 12.92f)
				: std::pow((c + 0.055f) / 1.055f, 2.4f);
		}
		return t;
	}();
	return table;
}

std::uint8_t linearToSrgb(float linear) {
	if (linear <= 0.0f) return 0;
	if (linear >= 1.0f) return 255;
	const float encoded = (linear <= 0.0031308f)
		? (linear * 12.92f)
		: (1.055f * std::pow(linear, 1.0f / 2.4f) - 0.055f);
	const int value = static_cast<int>(std::lround(encoded * 255.0f));
	return static_cast<std::uint8_t>(std::clamp(value, 0, 255));
}

// ---------------------------------------------------------------------------
// Lanczos-3
// ---------------------------------------------------------------------------

constexpr float kLanczosRadius = 3.0f;

float sinc(float x) {
	if (std::abs(x) < 1e-6f) return 1.0f;
	const float pix = 3.14159265358979323846f * x;
	return std::sin(pix) / pix;
}

float lanczos(float x) {
	x = std::abs(x);
	if (x >= kLanczosRadius) return 0.0f;
	return sinc(x) * sinc(x / kLanczosRadius);
}

/// One output sample's contributing input samples and their weights.
struct Contribution {
	int start = 0;
	std::vector<float> weights;
};

/// Builds the filter for one axis. When downscaling, the kernel is widened by
/// the scale factor so the filter averages over the whole source footprint;
/// without that, downscaling a 3000 px cover to 600 px would alias badly.
std::vector<Contribution> buildContributions(int sourceSize, int targetSize) {
	std::vector<Contribution> result(static_cast<std::size_t>(targetSize));

	const float scale = static_cast<float>(targetSize) / static_cast<float>(sourceSize);
	const float support = (scale < 1.0f) ? (kLanczosRadius / scale) : kLanczosRadius;
	const float filterScale = (scale < 1.0f) ? scale : 1.0f;

	for (int i = 0; i < targetSize; ++i) {
		// Centre of this output pixel, in source coordinates.
		const float centre = (static_cast<float>(i) + 0.5f) / scale - 0.5f;
		int left = static_cast<int>(std::ceil(centre - support));
		int right = static_cast<int>(std::floor(centre + support));
		left = std::max(left, 0);
		right = std::min(right, sourceSize - 1);

		Contribution c;
		c.start = left;
		c.weights.reserve(static_cast<std::size_t>(right - left + 1));

		float total = 0.0f;
		for (int j = left; j <= right; ++j) {
			const float w = lanczos((static_cast<float>(j) - centre) * filterScale);
			c.weights.push_back(w);
			total += w;
		}
		// Normalise so the filter is energy-preserving; an unnormalised Lanczos
		// shifts overall brightness.
		if (std::abs(total) > 1e-9f) {
			for (float& w : c.weights) w /= total;
		} else {
			// Degenerate case: fall back to nearest neighbour for this sample.
			c.weights.assign(c.weights.size(), 0.0f);
			if (!c.weights.empty()) c.weights[0] = 1.0f;
		}
		result[static_cast<std::size_t>(i)] = std::move(c);
	}
	return result;
}

// ---------------------------------------------------------------------------
// libjpeg error handling. libjpeg calls exit() by default, which is not an
// acceptable response to a malformed APIC frame.
// ---------------------------------------------------------------------------

struct JpegErrorManager {
	jpeg_error_mgr base;
	std::jmp_buf escape;
	char message[JMSG_LENGTH_MAX] = {};
};

void jpegErrorExit(j_common_ptr info) {
	auto* manager = reinterpret_cast<JpegErrorManager*>(info->err);
	(*info->err->format_message)(info, manager->message);
	std::longjmp(manager->escape, 1);
}

void jpegEmitMessage(j_common_ptr, int) {
	// Warnings are suppressed: a slightly non-conforming JPEG still decodes, and
	// the condition assessment is the artwork policy's business, not libjpeg's.
}

// ---------------------------------------------------------------------------
// libpng reading from memory
// ---------------------------------------------------------------------------

struct PngReadState {
	const std::uint8_t* data = nullptr;
	std::size_t size = 0;
	std::size_t offset = 0;
};

void pngReadFromMemory(png_structp png, png_bytep out, png_size_t count) {
	auto* state = static_cast<PngReadState*>(png_get_io_ptr(png));
	if (!state || state->offset + count > state->size) {
		png_error(png, "read past end of buffer");
		return;
	}
	std::memcpy(out, state->data + state->offset, count);
	state->offset += count;
}

void pngErrorHandler(png_structp png, png_const_charp message) {
	// Converted into a longjmp by libpng's own setjmp mechanism.
	png_longjmp(png, 1);
	(void)message;
}

void pngWarningHandler(png_structp, png_const_charp) {}

bool looksLikeJpeg(const std::uint8_t* data, std::size_t size) {
	return size >= 3 && data[0] == 0xFF && data[1] == 0xD8 && data[2] == 0xFF;
}

bool looksLikePng(const std::uint8_t* data, std::size_t size) {
	static constexpr std::uint8_t kMagic[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
	return size >= 8 && std::memcmp(data, kMagic, 8) == 0;
}

Result<RgbImage> decodeJpeg(const std::uint8_t* data, std::size_t size) {
	jpeg_decompress_struct info{};
	JpegErrorManager errorManager{};

	info.err = jpeg_std_error(&errorManager.base);
	errorManager.base.error_exit = jpegErrorExit;
	errorManager.base.emit_message = jpegEmitMessage;

	RgbImage image;

	if (setjmp(errorManager.escape)) {
		jpeg_destroy_decompress(&info);
		return Error{ErrorCode::ParseError, std::string("JPEG decode failed: ") + errorManager.message};
	}

	jpeg_create_decompress(&info);
	jpeg_mem_src(&info, data, static_cast<unsigned long>(size));

	if (jpeg_read_header(&info, TRUE) != JPEG_HEADER_OK) {
		jpeg_destroy_decompress(&info);
		return Error{ErrorCode::ParseError, "JPEG header is not readable"};
	}

	// Always produce 24-bit RGB, whatever the source colour space. CMYK JPEGs
	// from print workflows do appear in cover art.
	info.out_color_space = JCS_RGB;

	const std::size_t pixels = static_cast<std::size_t>(info.image_width)
		* static_cast<std::size_t>(info.image_height);
	if (pixels == 0 || pixels > kMaxPixels) {
		jpeg_destroy_decompress(&info);
		return Error{ErrorCode::Unsupported,
			"JPEG dimensions are implausible for cover art (" + std::to_string(info.image_width) + "x"
				+ std::to_string(info.image_height) + ")"};
	}

	jpeg_start_decompress(&info);

	image.width = static_cast<int>(info.output_width);
	image.height = static_cast<int>(info.output_height);
	image.pixels.resize(static_cast<std::size_t>(image.width) * static_cast<std::size_t>(image.height) * 3u);

	if (info.output_components != 3) {
		jpeg_abort_decompress(&info);
		jpeg_destroy_decompress(&info);
		return Error{ErrorCode::Unsupported, "JPEG did not decode to three components"};
	}

	while (info.output_scanline < info.output_height) {
		JSAMPROW row = image.pixels.data()
			+ static_cast<std::size_t>(info.output_scanline) * image.stride();
		if (jpeg_read_scanlines(&info, &row, 1) != 1) break;
	}

	jpeg_finish_decompress(&info);
	jpeg_destroy_decompress(&info);

	if (!image.valid()) {
		return Error{ErrorCode::ParseError, "JPEG decode produced an incomplete image"};
	}
	return image;
}

Result<RgbImage> decodePng(const std::uint8_t* data, std::size_t size) {
	png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING, nullptr,
		pngErrorHandler, pngWarningHandler);
	if (!png) {
		return Error{ErrorCode::Internal, "cannot create a PNG reader"};
	}
	png_infop info = png_create_info_struct(png);
	if (!info) {
		png_destroy_read_struct(&png, nullptr, nullptr);
		return Error{ErrorCode::Internal, "cannot create a PNG info struct"};
	}

	RgbImage image;
	std::vector<png_bytep> rowPointers;
	PngReadState state{data, size, 0};

	if (setjmp(png_jmpbuf(png))) {
		png_destroy_read_struct(&png, &info, nullptr);
		return Error{ErrorCode::ParseError, "PNG decode failed"};
	}

	png_set_read_fn(png, &state, pngReadFromMemory);
	png_read_info(png, info);

	const png_uint_32 width = png_get_image_width(png, info);
	const png_uint_32 height = png_get_image_height(png, info);
	const int bitDepth = png_get_bit_depth(png, info);
	const int colourType = png_get_color_type(png, info);

	const std::size_t pixels = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
	if (pixels == 0 || pixels > kMaxPixels) {
		png_destroy_read_struct(&png, &info, nullptr);
		return Error{ErrorCode::Unsupported, "PNG dimensions are implausible for cover art"};
	}

	// Normalise every variant to 8-bit RGB.
	if (colourType == PNG_COLOR_TYPE_PALETTE) png_set_palette_to_rgb(png);
	if (colourType == PNG_COLOR_TYPE_GRAY && bitDepth < 8) png_set_expand_gray_1_2_4_to_8(png);
	if (png_get_valid(png, info, PNG_INFO_tRNS)) png_set_tRNS_to_alpha(png);
	if (bitDepth == 16) png_set_strip_16(png);
	if (colourType == PNG_COLOR_TYPE_GRAY || colourType == PNG_COLOR_TYPE_GRAY_ALPHA) {
		png_set_gray_to_rgb(png);
	}
	// Transparency is composited onto white: an embedded cover is opaque, and
	// discarding alpha without compositing would show artefacts.
	png_set_filler(png, 0xFF, PNG_FILLER_AFTER);
	png_read_update_info(png, info);

	const std::size_t rowBytes = png_get_rowbytes(png, info);
	std::vector<std::uint8_t> rgba(rowBytes * height);
	rowPointers.resize(height);
	for (png_uint_32 y = 0; y < height; ++y) {
		rowPointers[y] = rgba.data() + static_cast<std::size_t>(y) * rowBytes;
	}
	png_read_image(png, rowPointers.data());
	png_read_end(png, nullptr);
	png_destroy_read_struct(&png, &info, nullptr);

	image.width = static_cast<int>(width);
	image.height = static_cast<int>(height);
	image.pixels.resize(pixels * 3u);

	const std::size_t channels = rowBytes / width;
	for (std::size_t i = 0; i < pixels; ++i) {
		const std::uint8_t* src = rgba.data() + i * channels;
		std::uint8_t r = src[0];
		std::uint8_t g = (channels > 1) ? src[1] : src[0];
		std::uint8_t b = (channels > 2) ? src[2] : src[0];
		if (channels > 3) {
			const float alpha = static_cast<float>(src[3]) / 255.0f;
			const auto composite = [alpha](std::uint8_t c) {
				const float value = static_cast<float>(c) * alpha + 255.0f * (1.0f - alpha);
				return static_cast<std::uint8_t>(std::clamp(static_cast<int>(std::lround(value)), 0, 255));
			};
			r = composite(r);
			g = composite(g);
			b = composite(b);
		}
		image.pixels[i * 3] = r;
		image.pixels[i * 3 + 1] = g;
		image.pixels[i * 3 + 2] = b;
	}

	if (!image.valid()) {
		return Error{ErrorCode::ParseError, "PNG decode produced an incomplete image"};
	}
	return image;
}

} // namespace

std::string DerivativeConfig::hash() const {
	return configHash({
		"edge=" + std::to_string(edgeSize),
		"quality=" + std::to_string(jpegQuality),
		"optimise=" + std::string(optimiseHuffman ? "1" : "0"),
		"progressive=" + std::string(progressive ? "1" : "0"),
		"subsampling=" + std::string(chromaSubsampling ? "420" : "444"),
		"colourspace=" + colourSpace,
		"resampler=" + resampler,
		// The encoder implementation is part of the derivative's identity:
		// "quality 75" is encoder-specific, as the FRD notes.
		"encoder=libjpeg-turbo",
	});
}

std::string DerivativeConfig::describe() const {
	return std::to_string(edgeSize) + "x" + std::to_string(edgeSize) + " JPEG q" + std::to_string(jpegQuality)
		+ ", " + (chromaSubsampling ? "4:2:0" : "4:4:4")
		+ ", " + (progressive ? "progressive" : "baseline")
		+ ", " + (optimiseHuffman ? "optimised" : "standard") + " Huffman"
		+ ", " + colourSpace + ", " + resampler;
}

bool ImagePipeline::isDecodable(const std::uint8_t* data, std::size_t size) {
	return looksLikeJpeg(data, size) || looksLikePng(data, size);
}

Result<RgbImage> ImagePipeline::decode(const std::uint8_t* data, std::size_t size) {
	if (!data || size < 16) {
		return Error{ErrorCode::InvalidArgument, "image buffer is too small to identify"};
	}
	if (looksLikeJpeg(data, size)) return decodeJpeg(data, size);
	if (looksLikePng(data, size)) return decodePng(data, size);
	return Error{ErrorCode::Unsupported,
		"unsupported image format; only JPEG and PNG are decoded, and a candidate that cannot be "
		"decoded cannot have its dimensions or condition measured"};
}

Result<RgbImage> ImagePipeline::decodeFile(const fs::path& path) {
	std::ifstream file(path, std::ios::binary);
	if (!file) {
		return Error{ErrorCode::IoError, "cannot open image " + path.string()};
	}
	std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
	if (bytes.empty()) {
		return Error{ErrorCode::IoError, "image file is empty: " + path.string()};
	}
	return decode(bytes.data(), bytes.size());
}

Result<std::pair<int, int>> ImagePipeline::probeDimensions(const std::uint8_t* data, std::size_t size) {
	auto decoded = decode(data, size);
	if (!decoded) return decoded.error();
	return std::make_pair(decoded.value().width, decoded.value().height);
}

RgbImage ImagePipeline::resizeLanczos(const RgbImage& source, int targetWidth, int targetHeight) {
	RgbImage output;
	if (!source.valid() || targetWidth <= 0 || targetHeight <= 0) return output;

	if (source.width == targetWidth && source.height == targetHeight) return source;

	const auto& toLinear = srgbToLinearTable();

	// Horizontal pass into a linear-light intermediate.
	const auto horizontal = buildContributions(source.width, targetWidth);
	std::vector<float> intermediate(
		static_cast<std::size_t>(targetWidth) * static_cast<std::size_t>(source.height) * 3u, 0.0f);

	for (int y = 0; y < source.height; ++y) {
		const std::uint8_t* srcRow = source.pixels.data() + static_cast<std::size_t>(y) * source.stride();
		float* dstRow = intermediate.data()
			+ static_cast<std::size_t>(y) * static_cast<std::size_t>(targetWidth) * 3u;

		for (int x = 0; x < targetWidth; ++x) {
			const Contribution& c = horizontal[static_cast<std::size_t>(x)];
			float r = 0.0f;
			float g = 0.0f;
			float b = 0.0f;
			for (std::size_t k = 0; k < c.weights.size(); ++k) {
				const std::size_t sx = static_cast<std::size_t>(c.start + static_cast<int>(k)) * 3u;
				const float w = c.weights[k];
				r += toLinear[srcRow[sx]] * w;
				g += toLinear[srcRow[sx + 1]] * w;
				b += toLinear[srcRow[sx + 2]] * w;
			}
			dstRow[static_cast<std::size_t>(x) * 3u] = r;
			dstRow[static_cast<std::size_t>(x) * 3u + 1] = g;
			dstRow[static_cast<std::size_t>(x) * 3u + 2] = b;
		}
	}

	// Vertical pass back to sRGB.
	const auto vertical = buildContributions(source.height, targetHeight);
	output.width = targetWidth;
	output.height = targetHeight;
	output.pixels.resize(static_cast<std::size_t>(targetWidth) * static_cast<std::size_t>(targetHeight) * 3u);

	for (int y = 0; y < targetHeight; ++y) {
		const Contribution& c = vertical[static_cast<std::size_t>(y)];
		std::uint8_t* dstRow = output.pixels.data() + static_cast<std::size_t>(y) * output.stride();

		for (int x = 0; x < targetWidth; ++x) {
			float r = 0.0f;
			float g = 0.0f;
			float b = 0.0f;
			for (std::size_t k = 0; k < c.weights.size(); ++k) {
				const std::size_t sy = static_cast<std::size_t>(c.start + static_cast<int>(k));
				const float* srcRow = intermediate.data()
					+ sy * static_cast<std::size_t>(targetWidth) * 3u
					+ static_cast<std::size_t>(x) * 3u;
				const float w = c.weights[k];
				r += srcRow[0] * w;
				g += srcRow[1] * w;
				b += srcRow[2] * w;
			}
			dstRow[static_cast<std::size_t>(x) * 3u] = linearToSrgb(r);
			dstRow[static_cast<std::size_t>(x) * 3u + 1] = linearToSrgb(g);
			dstRow[static_cast<std::size_t>(x) * 3u + 2] = linearToSrgb(b);
		}
	}

	return output;
}

Result<std::vector<std::uint8_t>> ImagePipeline::encodeJpeg(const RgbImage& image,
	const DerivativeConfig& config) {
	if (!image.valid()) {
		return Error{ErrorCode::InvalidArgument, "cannot encode an invalid image"};
	}

	jpeg_compress_struct info{};
	JpegErrorManager errorManager{};
	info.err = jpeg_std_error(&errorManager.base);
	errorManager.base.error_exit = jpegErrorExit;
	errorManager.base.emit_message = jpegEmitMessage;

	unsigned char* buffer = nullptr;
	unsigned long bufferSize = 0;

	if (setjmp(errorManager.escape)) {
		jpeg_destroy_compress(&info);
		if (buffer) std::free(buffer);
		return Error{ErrorCode::Internal, std::string("JPEG encode failed: ") + errorManager.message};
	}

	jpeg_create_compress(&info);
	jpeg_mem_dest(&info, &buffer, &bufferSize);

	info.image_width = static_cast<JDIMENSION>(image.width);
	info.image_height = static_cast<JDIMENSION>(image.height);
	info.input_components = 3;
	info.in_color_space = JCS_RGB;

	jpeg_set_defaults(&info);
	jpeg_set_quality(&info, config.jpegQuality, TRUE);   // TRUE == baseline-safe tables

	info.optimize_coding = config.optimiseHuffman ? TRUE : FALSE;

	// 4:4:4 keeps coloured lettering readable at 600 px. jpeg_set_defaults
	// chooses 4:2:0, which would blur it.
	if (!config.chromaSubsampling) {
		info.comp_info[0].h_samp_factor = 1;
		info.comp_info[0].v_samp_factor = 1;
		info.comp_info[1].h_samp_factor = 1;
		info.comp_info[1].v_samp_factor = 1;
		info.comp_info[2].h_samp_factor = 1;
		info.comp_info[2].v_samp_factor = 1;
	}

	if (config.progressive) jpeg_simple_progression(&info);

	// No JFIF density block beyond the default, no Adobe marker, and crucially
	// no EXIF or XMP: jpeg_start_compress writes none of those unless asked, so
	// the derivative carries no metadata from the source (FN-ART-02).
	info.write_JFIF_header = TRUE;
	info.write_Adobe_marker = FALSE;

	jpeg_start_compress(&info, TRUE);

	while (info.next_scanline < info.image_height) {
		JSAMPROW row = const_cast<JSAMPROW>(
			image.pixels.data() + static_cast<std::size_t>(info.next_scanline) * image.stride());
		jpeg_write_scanlines(&info, &row, 1);
	}

	jpeg_finish_compress(&info);
	jpeg_destroy_compress(&info);

	std::vector<std::uint8_t> out;
	if (buffer && bufferSize > 0) {
		out.assign(buffer, buffer + bufferSize);
	}
	if (buffer) std::free(buffer);

	if (out.empty()) {
		return Error{ErrorCode::Internal, "JPEG encoder produced no output"};
	}
	return out;
}

Result<DerivativeResult> ImagePipeline::makeDerivative(const std::uint8_t* data, std::size_t size,
	const DerivativeConfig& config) {
	auto decoded = decode(data, size);
	if (!decoded) return decoded.error();

	const RgbImage& source = decoded.value();

	DerivativeResult result;
	result.sourceWidth = source.width;
	result.sourceHeight = source.height;
	result.configHash = config.hash();

	// Never upscale to manufacture compliance (FRD section 6 point 5).
	if (source.width < config.edgeSize || source.height < config.edgeSize) {
		return Error{ErrorCode::Unsupported,
			"source is " + std::to_string(source.width) + "x" + std::to_string(source.height)
				+ ", smaller than the " + std::to_string(config.edgeSize)
				+ " px output; upscaling is not permitted"};
	}

	// Non-square sources are a review decision, not something to crop silently.
	if (source.width != source.height) {
		return Error{ErrorCode::Unsupported,
			"source is not square (" + std::to_string(source.width) + "x" + std::to_string(source.height)
				+ "); framing requires a review decision rather than an automatic crop"};
	}

	const RgbImage resized = resizeLanczos(source, config.edgeSize, config.edgeSize);
	if (!resized.valid()) {
		return Error{ErrorCode::Internal, "resampling produced an invalid image"};
	}

	auto encoded = encodeJpeg(resized, config);
	if (!encoded) return encoded.error();

	result.jpegBytes = std::move(encoded.value());
	result.width = resized.width;
	result.height = resized.height;
	result.sha256 = Sha256::hashBytes(result.jpegBytes.data(), result.jpegBytes.size());
	return result;
}

// ---------------------------------------------------------------------------
// AssetStore
// ---------------------------------------------------------------------------

AssetStore::AssetStore(fs::path root) : m_root(std::move(root)) {}

Result<std::string> AssetStore::put(const std::uint8_t* data, std::size_t size, std::string_view extension) {
	if (!data || size == 0) {
		return Error{ErrorCode::InvalidArgument, "refusing to store an empty asset"};
	}

	const std::string hash = Sha256::hashBytes(data, size);
	// Two-level fan-out keeps directory sizes reasonable at library scale.
	const std::string relative = hash.substr(0, 2) + "/" + hash.substr(2, 2) + "/" + hash
		+ std::string(extension);

	const fs::path absolute = m_root / relative;

	std::error_code ec;
	if (fs::exists(absolute, ec)) return relative;   // Content-addressed: already stored.

	fs::create_directories(absolute.parent_path(), ec);
	if (ec) {
		return Error{ErrorCode::IoError, "cannot create asset directory: " + ec.message()};
	}

	// Write to a temporary name and rename, so a crash never leaves a truncated
	// file under a hash that claims to describe complete content.
	const fs::path temporary = absolute.string() + ".partial";
	{
		std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
		if (!file) {
			return Error{ErrorCode::IoError, "cannot write asset " + temporary.string()};
		}
		file.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
		if (!file) {
			return Error{ErrorCode::IoError, "short write on asset " + temporary.string()};
		}
	}
	fs::rename(temporary, absolute, ec);
	if (ec) {
		fs::remove(temporary, ec);
		return Error{ErrorCode::IoError, "cannot publish asset: " + ec.message()};
	}

	return relative;
}

fs::path AssetStore::resolve(std::string_view relativePath) const {
	return m_root / fs::path(std::string(relativePath));
}

Result<std::vector<std::uint8_t>> AssetStore::get(std::string_view relativePath) const {
	const fs::path path = resolve(relativePath);
	std::ifstream file(path, std::ios::binary);
	if (!file) {
		return Error{ErrorCode::NotFound, "asset not found: " + path.string()};
	}
	std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
	return bytes;
}

bool AssetStore::contains(std::string_view relativePath) const {
	std::error_code ec;
	return fs::exists(resolve(relativePath), ec) && !ec;
}

} // namespace ml
