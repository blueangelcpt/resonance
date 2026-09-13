// SPDX-License-Identifier: GPL-3.0-or-later
#include "mlinfra/Hashing.hpp"

#include <cstdio>
#include <cstring>
#include <vector>

namespace ml {

namespace {

constexpr std::uint32_t kRoundConstants[64] = {
	0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
	0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
	0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
	0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
	0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
	0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
	0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
	0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
};

constexpr std::uint32_t rotr(std::uint32_t x, unsigned n) {
	return (x >> n) | (x << (32u - n));
}

/// Read buffer for whole-file hashing. 1 MiB keeps memory bounded while still
/// amortising syscalls over a 13 GB library.
constexpr std::size_t kReadChunk = 1u << 20;

} // namespace

Sha256::Sha256() {
	m_state = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
			   0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};
}

void Sha256::transform(const std::uint8_t block[64]) {
	std::uint32_t w[64];
	for (std::size_t i = 0; i < 16; ++i) {
		w[i] = (static_cast<std::uint32_t>(block[i * 4]) << 24)
			 | (static_cast<std::uint32_t>(block[i * 4 + 1]) << 16)
			 | (static_cast<std::uint32_t>(block[i * 4 + 2]) << 8)
			 | (static_cast<std::uint32_t>(block[i * 4 + 3]));
	}
	for (std::size_t i = 16; i < 64; ++i) {
		const std::uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
		const std::uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
		w[i] = w[i - 16] + s0 + w[i - 7] + s1;
	}

	std::uint32_t a = m_state[0], b = m_state[1], c = m_state[2], d = m_state[3];
	std::uint32_t e = m_state[4], f = m_state[5], g = m_state[6], h = m_state[7];

	for (std::size_t i = 0; i < 64; ++i) {
		const std::uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
		const std::uint32_t ch = (e & f) ^ (~e & g);
		const std::uint32_t temp1 = h + s1 + ch + kRoundConstants[i] + w[i];
		const std::uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
		const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
		const std::uint32_t temp2 = s0 + maj;

		h = g; g = f; f = e;
		e = d + temp1;
		d = c; c = b; b = a;
		a = temp1 + temp2;
	}

	m_state[0] += a; m_state[1] += b; m_state[2] += c; m_state[3] += d;
	m_state[4] += e; m_state[5] += f; m_state[6] += g; m_state[7] += h;
}

void Sha256::update(const void* data, std::size_t length) {
	if (m_finished || data == nullptr) return;

	const auto* bytes = static_cast<const std::uint8_t*>(data);
	m_bitCount += static_cast<std::uint64_t>(length) * 8u;

	// Fill any partial block first.
	if (m_bufferLength > 0) {
		const std::size_t needed = 64 - m_bufferLength;
		const std::size_t take = (length < needed) ? length : needed;
		std::memcpy(m_buffer.data() + m_bufferLength, bytes, take);
		m_bufferLength += take;
		bytes += take;
		length -= take;
		if (m_bufferLength == 64) {
			transform(m_buffer.data());
			m_bufferLength = 0;
		}
	}

	while (length >= 64) {
		transform(bytes);
		bytes += 64;
		length -= 64;
	}

	if (length > 0) {
		std::memcpy(m_buffer.data(), bytes, length);
		m_bufferLength = length;
	}
}

void Sha256::update(std::string_view text) {
	update(text.data(), text.size());
}

Sha256::Digest Sha256::finish() {
	Digest digest{};
	if (m_finished) return digest;

	const std::uint64_t bitCount = m_bitCount;

	// Padding: 0x80, then zeros, then the 64-bit big-endian length.
	std::uint8_t pad = 0x80;
	m_bitCount = bitCount;   // update() would otherwise count the padding
	{
		// Inline the padding append without going through update(), so the bit
		// count is not disturbed.
		m_buffer[m_bufferLength++] = pad;
		if (m_bufferLength > 56) {
			while (m_bufferLength < 64) m_buffer[m_bufferLength++] = 0;
			transform(m_buffer.data());
			m_bufferLength = 0;
		}
		while (m_bufferLength < 56) m_buffer[m_bufferLength++] = 0;
		for (int i = 7; i >= 0; --i) {
			m_buffer[m_bufferLength++] = static_cast<std::uint8_t>((bitCount >> (i * 8)) & 0xFFu);
		}
		transform(m_buffer.data());
	}

	for (std::size_t i = 0; i < 8; ++i) {
		digest[i * 4] = static_cast<std::uint8_t>((m_state[i] >> 24) & 0xFFu);
		digest[i * 4 + 1] = static_cast<std::uint8_t>((m_state[i] >> 16) & 0xFFu);
		digest[i * 4 + 2] = static_cast<std::uint8_t>((m_state[i] >> 8) & 0xFFu);
		digest[i * 4 + 3] = static_cast<std::uint8_t>(m_state[i] & 0xFFu);
	}

	m_finished = true;
	return digest;
}

std::string Sha256::toHex(const Digest& digest) {
	static constexpr char kHex[] = "0123456789abcdef";
	std::string out;
	out.resize(kDigestBytes * 2);
	for (std::size_t i = 0; i < kDigestBytes; ++i) {
		out[i * 2] = kHex[(digest[i] >> 4) & 0x0Fu];
		out[i * 2 + 1] = kHex[digest[i] & 0x0Fu];
	}
	return out;
}

std::string Sha256::finishHex() {
	return toHex(finish());
}

std::string Sha256::hashBytes(const void* data, std::size_t length) {
	Sha256 h;
	h.update(data, length);
	return h.finishHex();
}

std::string Sha256::hashString(std::string_view text) {
	return hashBytes(text.data(), text.size());
}

namespace {

/// RAII wrapper for a C file handle. Every file handle in this project is
/// owned by a scope guard (FRD section 15 C++ rules).
class FileHandle {
public:
	FileHandle() = default;
	explicit FileHandle(std::FILE* f) : m_file(f) {}
	~FileHandle() { reset(); }

	FileHandle(const FileHandle&) = delete;
	FileHandle& operator=(const FileHandle&) = delete;
	FileHandle(FileHandle&& other) noexcept : m_file(other.m_file) { other.m_file = nullptr; }
	FileHandle& operator=(FileHandle&& other) noexcept {
		if (this != &other) {
			reset();
			m_file = other.m_file;
			other.m_file = nullptr;
		}
		return *this;
	}

	void reset() {
		if (m_file) {
			std::fclose(m_file);
			m_file = nullptr;
		}
	}

	std::FILE* get() const { return m_file; }
	explicit operator bool() const { return m_file != nullptr; }

private:
	std::FILE* m_file = nullptr;
};

FileHandle openForRead(const std::filesystem::path& path) {
#ifdef _WIN32
	std::FILE* f = nullptr;
	_wfopen_s(&f, path.wstring().c_str(), L"rb");
	return FileHandle(f);
#else
	return FileHandle(std::fopen(path.c_str(), "rb"));
#endif
}

} // namespace

Result<std::string> hashFile(const std::filesystem::path& path) {
	FileHandle file = openForRead(path);
	if (!file) {
		return Error{ErrorCode::IoError, "cannot open for reading: " + path.string()};
	}

	Sha256 hasher;
	std::vector<std::uint8_t> buffer(kReadChunk);
	while (true) {
		const std::size_t read = std::fread(buffer.data(), 1, buffer.size(), file.get());
		if (read > 0) hasher.update(buffer.data(), read);
		if (read < buffer.size()) {
			if (std::ferror(file.get())) {
				return Error{ErrorCode::IoError, "read error: " + path.string()};
			}
			break;
		}
	}
	return hasher.finishHex();
}

Result<std::string> hashFileRange(const std::filesystem::path& path, std::int64_t offset, std::int64_t length) {
	if (offset < 0 || length < 0) {
		return Error{ErrorCode::InvalidArgument, "negative offset or length"};
	}

	FileHandle file = openForRead(path);
	if (!file) {
		return Error{ErrorCode::IoError, "cannot open for reading: " + path.string()};
	}

#ifdef _WIN32
	if (_fseeki64(file.get(), offset, SEEK_SET) != 0) {
#else
	if (std::fseek(file.get(), static_cast<long>(offset), SEEK_SET) != 0) {
#endif
		return Error{ErrorCode::IoError, "cannot seek to offset in " + path.string()};
	}

	Sha256 hasher;
	std::vector<std::uint8_t> buffer(kReadChunk);
	std::int64_t remaining = length;
	while (remaining > 0) {
		const std::size_t want = static_cast<std::size_t>(
			std::min<std::int64_t>(remaining, static_cast<std::int64_t>(buffer.size())));
		const std::size_t read = std::fread(buffer.data(), 1, want, file.get());
		if (read == 0) {
			if (std::ferror(file.get())) {
				return Error{ErrorCode::IoError, "read error: " + path.string()};
			}
			// Fewer bytes than the caller expected means the file is shorter than
			// the plan assumed. That is a verification failure, not a silent pass.
			return Error{ErrorCode::VerificationFailed,
				"file is shorter than the requested range: " + path.string()};
		}
		hasher.update(buffer.data(), read);
		remaining -= static_cast<std::int64_t>(read);
	}
	return hasher.finishHex();
}

std::string configHash(const std::vector<std::string>& components) {
	Sha256 hasher;
	for (const auto& c : components) {
		// Length-prefix each component so ["ab","c"] and ["a","bc"] differ.
		const std::uint32_t length = static_cast<std::uint32_t>(c.size());
		hasher.update(&length, sizeof(length));
		hasher.update(c);
	}
	return hasher.finishHex();
}

} // namespace ml
