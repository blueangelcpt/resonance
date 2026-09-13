// SPDX-License-Identifier: GPL-3.0-or-later
// SHA-256. Content hashes are the evidence behind every safety claim in this
// project: source protection, copy verification and idempotence all rest on
// them, so the implementation is vendored rather than depending on a TLS library
// that the CLI would otherwise not need.
#pragma once

#include "mlcore/Types.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace ml {

class Sha256 {
public:
	static constexpr std::size_t kDigestBytes = 32;
	using Digest = std::array<std::uint8_t, kDigestBytes>;

	Sha256();

	void update(const void* data, std::size_t length);
	void update(std::string_view text);

	/// Finalises and returns the raw digest. The object must not be reused.
	Digest finish();

	/// Finalises and returns the lowercase hexadecimal digest.
	std::string finishHex();

	static std::string toHex(const Digest& digest);

	static std::string hashBytes(const void* data, std::size_t length);
	static std::string hashString(std::string_view text);

private:
	void transform(const std::uint8_t block[64]);

	std::array<std::uint32_t, 8> m_state{};
	std::array<std::uint8_t, 64> m_buffer{};
	std::uint64_t m_bitCount = 0;
	std::size_t m_bufferLength = 0;
	bool m_finished = false;
};

/// Hashes a whole file in bounded memory.
Result<std::string> hashFile(const std::filesystem::path& path);

/// Hashes a byte range of a file. Used for the MPEG payload hash, which must
/// exclude tags so a tag-only edit can be proven not to have touched the audio.
Result<std::string> hashFileRange(const std::filesystem::path& path, std::int64_t offset, std::int64_t length);

/// Hashes a configuration description into a stable key for job identity
/// (JOB-001: jobs are keyed by entity, input hash, engine version and config).
std::string configHash(const std::vector<std::string>& components);

} // namespace ml
