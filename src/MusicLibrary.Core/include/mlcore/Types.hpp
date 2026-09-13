// SPDX-License-Identifier: GPL-3.0-or-later
// Core value types shared by every layer. No I/O, no Qt, no platform headers.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace ml {

// ---------------------------------------------------------------------------
// Strong identifiers. Row identifiers from the catalogue are not interchangeable.
// ---------------------------------------------------------------------------
template <typename Tag>
struct Id {
	std::int64_t value = 0;

	constexpr Id() = default;
	constexpr explicit Id(std::int64_t v) : value(v) {}

	constexpr bool valid() const { return value > 0; }
	friend constexpr bool operator==(Id a, Id b) { return a.value == b.value; }
	friend constexpr bool operator!=(Id a, Id b) { return a.value != b.value; }
	friend constexpr bool operator<(Id a, Id b) { return a.value < b.value; }
};

struct FileTag {};
struct AlbumTag {};
struct ScanRunTag {};
struct SnapshotTag {};
struct ArtworkTag {};
struct JobTag {};
struct ChangeSetTag {};
struct OperationTag {};
struct RootTag {};

using FileId = Id<FileTag>;
using AlbumId = Id<AlbumTag>;
using ScanRunId = Id<ScanRunTag>;
using SnapshotId = Id<SnapshotTag>;
using ArtworkId = Id<ArtworkTag>;
using JobId = Id<JobTag>;
using ChangeSetId = Id<ChangeSetTag>;
using OperationId = Id<OperationTag>;
using RootId = Id<RootTag>;

// ---------------------------------------------------------------------------
// Error handling. Errors are values; exceptions are converted at job and UI
// boundaries into durable failure records (FRD section 15).
// ---------------------------------------------------------------------------
enum class ErrorCode {
	Ok = 0,
	NotFound,
	InvalidArgument,
	IoError,
	ParseError,
	Unsupported,
	PermissionDenied,
	ProtectedRootViolation,
	Collision,
	Conflict,
	NetworkError,
	RateLimited,
	Cancelled,
	VerificationFailed,
	DatabaseError,
	Internal,
};

std::string_view toString(ErrorCode code);

struct Error {
	ErrorCode code = ErrorCode::Internal;
	std::string message;

	Error() = default;
	Error(ErrorCode c, std::string m) : code(c), message(std::move(m)) {}

	std::string describe() const;
};

/// Minimal result type. Deliberately not std::expected so the code builds on the
/// oldest toolchain in the support matrix.
template <typename T>
class Result {
public:
	Result(T value) : m_data(std::move(value)) {}
	Result(Error error) : m_data(std::move(error)) {}

	bool ok() const { return std::holds_alternative<T>(m_data); }
	explicit operator bool() const { return ok(); }

	T& value() { return std::get<T>(m_data); }
	const T& value() const { return std::get<T>(m_data); }
	const Error& error() const { return std::get<Error>(m_data); }

	T valueOr(T fallback) const { return ok() ? std::get<T>(m_data) : std::move(fallback); }

private:
	std::variant<T, Error> m_data;
};

/// Result of an operation that yields no value.
class Status {
public:
	Status() = default;
	Status(Error error) : m_error(std::move(error)) {}

	static Status success() { return Status(); }

	bool ok() const { return !m_error.has_value(); }
	explicit operator bool() const { return ok(); }
	const Error& error() const { return *m_error; }
	std::string describe() const { return m_error ? m_error->describe() : std::string("ok"); }

private:
	std::optional<Error> m_error;
};

// ---------------------------------------------------------------------------
// Confidence. The FRD forbids presenting an arbitrary score as a calibrated
// probability, so confidence is an ordered label carrying its own evidence.
// ---------------------------------------------------------------------------
enum class Confidence {
	Unknown = 0,   ///< No usable evidence either way.
	Weak,          ///< Suggestive evidence only; requires review before acting.
	Moderate,      ///< Consistent evidence; acceptable for a reviewed proposal.
	Strong,        ///< Multiple independent agreeing signals.
	Locked,        ///< A human decision. Survives rescans and provider refreshes.
};

std::string_view toString(Confidence c);
std::optional<Confidence> confidenceFromString(std::string_view s);

/// A single piece of supporting or contradicting evidence, retained so a
/// decision can be explained later (FRD sections 6 and 15).
struct Evidence {
	std::string kind;     ///< Stable machine key, for example "duration_match".
	std::string detail;   ///< Human-readable observation.
	bool supporting = true;
};

// ---------------------------------------------------------------------------
// Technical audio properties observed from the stream, never from tags.
// ---------------------------------------------------------------------------
enum class BitrateMode { Unknown = 0, Cbr, Vbr, Abr };

std::string_view toString(BitrateMode m);

struct AudioProperties {
	std::int64_t durationMs = 0;
	int sampleRateHz = 0;
	int channels = 0;
	int bitrateKbps = 0;
	BitrateMode bitrateMode = BitrateMode::Unknown;
	std::int64_t audioOffset = 0;      ///< Byte offset of the first MPEG frame.
	std::int64_t audioLength = 0;      ///< Byte length of the MPEG payload.
	bool hasXingHeader = false;
	bool hasLameHeader = false;
	std::int64_t encoderDelay = 0;     ///< Gapless information, preserved not rewritten.
	std::int64_t encoderPadding = 0;
	bool valid = false;
};

// ---------------------------------------------------------------------------
// Filesystem identity. Path strings alone cannot establish identity
// (FRD section 9): device and inode are compared as well.
// ---------------------------------------------------------------------------
struct FileIdentity {
	std::uint64_t deviceId = 0;
	std::uint64_t inode = 0;
	std::int64_t sizeBytes = 0;
	std::int64_t modifiedUnixMs = 0;

	bool sameFile(const FileIdentity& other) const {
		return deviceId != 0 && deviceId == other.deviceId && inode != 0 && inode == other.inode;
	}
	bool looksUnchanged(const FileIdentity& other) const {
		return sizeBytes == other.sizeBytes && modifiedUnixMs == other.modifiedUnixMs;
	}
};

} // namespace ml
