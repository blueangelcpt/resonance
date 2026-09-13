// SPDX-License-Identifier: GPL-3.0-or-later
#include "mlinfra/PathGuard.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cerrno>
#include <cstring>
#include <random>
#include <system_error>

#ifdef _WIN32
#	include <windows.h>
#else
#	include <fcntl.h>
#	include <sys/stat.h>
#	include <sys/types.h>
#	include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace ml {

std::string_view toString(GuardVerdict v) {
	switch (v) {
		case GuardVerdict::Allowed: return "allowed";
		case GuardVerdict::InsideProtectedRoot: return "inside_protected_root";
		case GuardVerdict::ResolvesIntoProtectedRoot: return "resolves_into_protected_root";
		case GuardVerdict::OverlapsProtectedRoot: return "overlaps_protected_root";
		case GuardVerdict::OutsideAllowedOutput: return "outside_allowed_output";
		case GuardVerdict::IsSymlink: return "is_symlink";
		case GuardVerdict::NotAbsolute: return "not_absolute";
		case GuardVerdict::EmptyPath: return "empty_path";
		case GuardVerdict::ResolutionFailed: return "resolution_failed";
	}
	return "resolution_failed";
}

PathGuard::PathGuard() = default;

bool PathGuard::isLexicallyInside(const fs::path& child, const fs::path& parent) {
	// Compare component by component. A string prefix test would wrongly report
	// "/music-backup" as being inside "/music".
	auto childIt = child.begin();
	auto childEnd = child.end();
	auto parentIt = parent.begin();
	auto parentEnd = parent.end();

	for (; parentIt != parentEnd; ++parentIt, ++childIt) {
		if (childIt == childEnd) return false;
		// A trailing empty component appears when a path ends with a separator.
		if (parentIt->empty() && childIt->empty()) continue;
		if (*childIt != *parentIt) return false;
	}
	return true;
}

Result<fs::path> PathGuard::resolveAsFarAsPossible(const fs::path& path) {
	if (path.empty()) {
		return Error{ErrorCode::InvalidArgument, "empty path"};
	}

	std::error_code ec;
	fs::path absolute = path.is_absolute() ? path : fs::absolute(path, ec);
	if (ec) {
		return Error{ErrorCode::IoError, "cannot make absolute: " + path.string() + ": " + ec.message()};
	}
	absolute = absolute.lexically_normal();

	// Walk up to the nearest existing ancestor, resolve it fully (following
	// links), then re-append the components that do not exist yet. This catches a
	// destination whose *parent directory* is a symlink into a protected root.
	fs::path existing = absolute;
	std::vector<fs::path> trailing;

	while (!existing.empty()) {
		ec.clear();
		if (fs::exists(existing, ec) && !ec) break;
		const fs::path parent = existing.parent_path();
		if (parent == existing) {
			// Reached the root without finding anything that exists.
			return Error{ErrorCode::NotFound, "no existing ancestor for " + path.string()};
		}
		trailing.push_back(existing.filename());
		existing = parent;
	}

	ec.clear();
	fs::path resolved = fs::canonical(existing, ec);
	if (ec) {
		return Error{ErrorCode::IoError, "cannot resolve " + existing.string() + ": " + ec.message()};
	}

	for (auto it = trailing.rbegin(); it != trailing.rend(); ++it) {
		resolved /= *it;
	}
	return resolved.lexically_normal();
}

std::optional<FileIdentity> PathGuard::identityOf(const fs::path& path) {
	FileIdentity id;
#ifdef _WIN32
	HANDLE handle = ::CreateFileW(path.wstring().c_str(), 0,
		FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
		FILE_FLAG_BACKUP_SEMANTICS, nullptr);
	if (handle == INVALID_HANDLE_VALUE) return std::nullopt;

	BY_HANDLE_FILE_INFORMATION info{};
	const BOOL ok = ::GetFileInformationByHandle(handle, &info);
	::CloseHandle(handle);
	if (!ok) return std::nullopt;

	id.deviceId = info.dwVolumeSerialNumber;
	id.inode = (static_cast<std::uint64_t>(info.nFileIndexHigh) << 32) | info.nFileIndexLow;
	id.sizeBytes = static_cast<std::int64_t>(
		(static_cast<std::uint64_t>(info.nFileSizeHigh) << 32) | info.nFileSizeLow);
	const std::uint64_t ticks = (static_cast<std::uint64_t>(info.ftLastWriteTime.dwHighDateTime) << 32)
		| info.ftLastWriteTime.dwLowDateTime;
	// FILETIME is 100 ns intervals since 1601-01-01; convert to Unix milliseconds.
	id.modifiedUnixMs = static_cast<std::int64_t>(ticks / 10000ULL) - 11644473600000LL;
#else
	struct stat st {};
	if (::lstat(path.c_str(), &st) != 0) return std::nullopt;
	id.deviceId = static_cast<std::uint64_t>(st.st_dev);
	id.inode = static_cast<std::uint64_t>(st.st_ino);
	id.sizeBytes = static_cast<std::int64_t>(st.st_size);
	id.modifiedUnixMs = static_cast<std::int64_t>(st.st_mtime) * 1000
		+ static_cast<std::int64_t>(st.st_mtim.tv_nsec / 1000000);
#endif
	return id;
}

bool PathGuard::isReparsePoint(const fs::path& path) {
	std::error_code ec;
	const bool link = fs::is_symlink(path, ec);
	if (!ec && link) return true;

#ifdef _WIN32
	// Junctions are reparse points but not symlinks as far as std::filesystem is
	// concerned on every toolchain, so the attribute is checked directly.
	const DWORD attributes = ::GetFileAttributesW(path.wstring().c_str());
	if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
		return true;
	}
#endif
	return false;
}

std::optional<std::uintmax_t> PathGuard::hardLinkCount(const fs::path& path) {
	std::error_code ec;
	const std::uintmax_t count = fs::hard_link_count(path, ec);
	if (ec) return std::nullopt;
	return count;
}

Status PathGuard::addProtectedRoot(const fs::path& path, std::string label) {
	auto resolved = resolveAsFarAsPossible(path);
	if (!resolved) {
		return Status(Error{ErrorCode::InvalidArgument,
			"protected root cannot be resolved and must not be silently ignored: " + resolved.error().message});
	}

	std::error_code ec;
	if (!fs::exists(resolved.value(), ec) || ec) {
		return Status(Error{ErrorCode::NotFound, "protected root does not exist: " + path.string()});
	}
	if (!fs::is_directory(resolved.value(), ec) || ec) {
		return Status(Error{ErrorCode::InvalidArgument, "protected root is not a directory: " + path.string()});
	}

	std::lock_guard<std::mutex> lock(m_mutex);

	// An output root already configured inside this new protected root would make
	// the configuration unsafe as a whole.
	for (const auto& output : m_outputRoots) {
		if (isLexicallyInside(output, resolved.value())) {
			return Status(Error{ErrorCode::ProtectedRootViolation,
				"output root " + output.string() + " is inside the protected root being added"});
		}
	}

	ProtectedRoot root;
	root.id = RootId(m_nextRootId++);
	root.path = path;
	root.resolvedPath = resolved.value();
	root.label = label.empty() ? resolved.value().filename().string() : std::move(label);
	root.currentlyPresent = true;
	if (auto id = identityOf(resolved.value())) {
		root.deviceId = id->deviceId;
		root.inode = id->inode;
		root.volumeIdentity = std::to_string(id->deviceId);
	}

	m_protectedRoots.push_back(std::move(root));
	return Status::success();
}

Status PathGuard::addOutputRoot(const fs::path& path) {
	auto resolved = resolveAsFarAsPossible(path);
	if (!resolved) {
		return Status(Error{ErrorCode::InvalidArgument, "output root cannot be resolved: " + path.string()});
	}

	std::lock_guard<std::mutex> lock(m_mutex);

	for (const auto& root : m_protectedRoots) {
		// The output must not be inside a protected root...
		if (isLexicallyInside(resolved.value(), root.resolvedPath)) {
			return Status(Error{ErrorCode::ProtectedRootViolation,
				"output root " + resolved.value().string() + " resolves inside protected root "
					+ root.resolvedPath.string()});
		}
		// ...and must not contain one, which would let a recursive operation
		// reach the protected files from above.
		if (isLexicallyInside(root.resolvedPath, resolved.value())) {
			return Status(Error{ErrorCode::ProtectedRootViolation,
				"output root " + resolved.value().string() + " contains protected root "
					+ root.resolvedPath.string()});
		}
	}

	for (const auto& existing : m_outputRoots) {
		if (existing == resolved.value()) return Status::success();
	}
	m_outputRoots.push_back(resolved.value());
	return Status::success();
}

GuardDecision PathGuard::checkWriteLocked(const fs::path& path) const {
	GuardDecision decision;

	if (path.empty()) {
		decision.verdict = GuardVerdict::EmptyPath;
		decision.reason = "empty path";
		return decision;
	}

	auto resolved = resolveAsFarAsPossible(path);
	if (!resolved) {
		decision.verdict = GuardVerdict::ResolutionFailed;
		decision.reason = resolved.error().message;
		return decision;
	}
	decision.resolvedPath = resolved.value();

	// The resolved form is what actually gets written. Comparing it against the
	// resolved protected roots is what defeats symlinks, junctions and mounts.
	for (const auto& root : m_protectedRoots) {
		if (isLexicallyInside(decision.resolvedPath, root.resolvedPath)) {
			// Distinguish "the caller asked for a path inside the source" from
			// "the caller asked for an innocent-looking path that redirects there",
			// because the second is the case worth alarming about.
			const bool literal = isLexicallyInside(path.lexically_normal(), root.resolvedPath)
				|| isLexicallyInside(path.lexically_normal(), root.path.lexically_normal());
			decision.verdict = literal ? GuardVerdict::InsideProtectedRoot
									   : GuardVerdict::ResolvesIntoProtectedRoot;
			decision.offendingRoot = root.resolvedPath;
			decision.reason = literal
				? ("path is inside protected source root " + root.resolvedPath.string())
				: ("path \"" + path.string() + "\" resolves to \"" + decision.resolvedPath.string()
					+ "\", inside protected source root " + root.resolvedPath.string());
			return decision;
		}
		if (isLexicallyInside(root.resolvedPath, decision.resolvedPath)) {
			decision.verdict = GuardVerdict::OverlapsProtectedRoot;
			decision.offendingRoot = root.resolvedPath;
			decision.reason = "path contains protected source root " + root.resolvedPath.string();
			return decision;
		}
	}

	// Writing is confined to configured output roots. Without this, a bug in
	// destination computation could write anywhere the process has permission.
	if (!m_outputRoots.empty()) {
		bool inOutput = false;
		for (const auto& output : m_outputRoots) {
			if (isLexicallyInside(decision.resolvedPath, output)) {
				inOutput = true;
				break;
			}
		}
		if (!inOutput) {
			decision.verdict = GuardVerdict::OutsideAllowedOutput;
			decision.reason = "path is not under any configured output root";
			return decision;
		}
	}

	// Never write through a link. The link target may be anywhere, including
	// inside a protected root via a path we could not resolve.
	if (isReparsePoint(path)) {
		decision.verdict = GuardVerdict::IsSymlink;
		decision.reason = "destination is a symlink, junction or reparse point; writing through it is refused";
		return decision;
	}

	decision.verdict = GuardVerdict::Allowed;
	decision.reason = "allowed";
	return decision;
}

GuardDecision PathGuard::checkWrite(const fs::path& path) const {
	std::lock_guard<std::mutex> lock(m_mutex);
	return checkWriteLocked(path);
}

GuardDecision PathGuard::checkRead(const fs::path& path) const {
	GuardDecision decision;
	if (path.empty()) {
		decision.verdict = GuardVerdict::EmptyPath;
		decision.reason = "empty path";
		return decision;
	}
	auto resolved = resolveAsFarAsPossible(path);
	if (!resolved) {
		decision.verdict = GuardVerdict::ResolutionFailed;
		decision.reason = resolved.error().message;
		return decision;
	}
	decision.resolvedPath = resolved.value();
	decision.verdict = GuardVerdict::Allowed;
	decision.reason = "reading is permitted anywhere the process has access";
	return decision;
}

bool PathGuard::isInsideProtectedRoot(const fs::path& path) const {
	auto resolved = resolveAsFarAsPossible(path);
	if (!resolved) return false;

	std::lock_guard<std::mutex> lock(m_mutex);
	for (const auto& root : m_protectedRoots) {
		if (isLexicallyInside(resolved.value(), root.resolvedPath)) return true;
	}
	return false;
}

bool PathGuard::isSameFileAsProtectedSource(const fs::path& path) const {
	const auto identity = identityOf(path);
	if (!identity) return false;

	// A hardlinked file has more than one name. If this file is hardlinked and
	// any protected root is on the same device, we cannot prove the other name is
	// not inside the source, so we treat it as unsafe (SAFE-002).
	const auto links = hardLinkCount(path);

	std::lock_guard<std::mutex> lock(m_mutex);
	for (const auto& root : m_protectedRoots) {
		if (root.deviceId != 0 && identity->deviceId == root.deviceId) {
			if (links && *links > 1) return true;
		}
	}
	return false;
}

Status PathGuard::createDirectories(const fs::path& path) const {
	const GuardDecision decision = checkWrite(path);
	if (!decision.allowed()) {
		return Status(Error{ErrorCode::ProtectedRootViolation,
			"refusing to create directory: " + decision.reason});
	}

	std::error_code ec;
	fs::create_directories(decision.resolvedPath, ec);
	if (ec && !fs::is_directory(decision.resolvedPath)) {
		return Status(Error{ErrorCode::IoError,
			"cannot create directory " + decision.resolvedPath.string() + ": " + ec.message()});
	}
	return Status::success();
}

std::vector<ProtectedRoot> PathGuard::protectedRoots() const {
	std::lock_guard<std::mutex> lock(m_mutex);
	return m_protectedRoots;
}

std::vector<fs::path> PathGuard::outputRoots() const {
	std::lock_guard<std::mutex> lock(m_mutex);
	return m_outputRoots;
}

void PathGuard::refreshRootPresence() {
	std::lock_guard<std::mutex> lock(m_mutex);
	for (auto& root : m_protectedRoots) {
		std::error_code ec;
		const bool exists = fs::exists(root.resolvedPath, ec) && !ec && fs::is_directory(root.resolvedPath, ec);
		root.currentlyPresent = exists && !ec;

		// Volume identity is re-read so a different disk mounted at the same path
		// is not mistaken for the original collection.
		if (root.currentlyPresent) {
			if (auto id = identityOf(root.resolvedPath)) {
				root.volumeIdentity = std::to_string(id->deviceId);
			}
		}
	}
}

std::size_t PathGuard::absentRootCount() const {
	std::lock_guard<std::mutex> lock(m_mutex);
	std::size_t count = 0;
	for (const auto& root : m_protectedRoots) {
		if (!root.currentlyPresent) ++count;
	}
	return count;
}

// ---------------------------------------------------------------------------
// ScopedTempFile
// ---------------------------------------------------------------------------

ScopedTempFile::~ScopedTempFile() {
	remove();
}

ScopedTempFile::ScopedTempFile(ScopedTempFile&& other) noexcept : m_path(std::move(other.m_path)) {
	other.m_path.clear();
}

ScopedTempFile& ScopedTempFile::operator=(ScopedTempFile&& other) noexcept {
	if (this != &other) {
		remove();
		m_path = std::move(other.m_path);
		other.m_path.clear();
	}
	return *this;
}

std::filesystem::path ScopedTempFile::release() {
	fs::path p = m_path;
	m_path.clear();
	return p;
}

void ScopedTempFile::remove() {
	if (m_path.empty()) return;
	std::error_code ec;
	fs::remove(m_path, ec);   // Best effort: a missing file is not an error here.
	m_path.clear();
}

Result<ScopedTempFile> ScopedTempFile::createIn(const PathGuard& guard, const fs::path& directory,
	std::string_view prefix) {
	const GuardDecision decision = guard.checkWrite(directory);
	if (!decision.allowed()) {
		return Error{ErrorCode::ProtectedRootViolation,
			"refusing to create a temporary file: " + decision.reason};
	}

	std::error_code ec;
	if (!fs::exists(decision.resolvedPath, ec)) {
		fs::create_directories(decision.resolvedPath, ec);
		if (ec) {
			return Error{ErrorCode::IoError,
				"cannot create staging directory " + decision.resolvedPath.string() + ": " + ec.message()};
		}
	}

	// Exclusive creation, retried on collision. O_EXCL / CREATE_NEW guarantees we
	// never adopt a file that already existed.
	static std::atomic<std::uint64_t> counter{0};
	std::random_device rd;
	std::mt19937_64 rng(rd() ^ static_cast<std::uint64_t>(
		std::chrono::steady_clock::now().time_since_epoch().count()));

	for (int attempt = 0; attempt < 64; ++attempt) {
		std::string name(prefix);
		name += std::to_string(counter.fetch_add(1, std::memory_order_relaxed));
		name += "-";
		name += std::to_string(rng());
		name += ".tmp";

		const fs::path candidate = decision.resolvedPath / name;

#ifdef _WIN32
		HANDLE handle = ::CreateFileW(candidate.wstring().c_str(), GENERIC_WRITE, 0, nullptr,
			CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (handle == INVALID_HANDLE_VALUE) {
			if (::GetLastError() == ERROR_FILE_EXISTS) continue;
			return Error{ErrorCode::IoError, "cannot create temporary file in " + decision.resolvedPath.string()};
		}
		::CloseHandle(handle);
#else
		const int fd = ::open(candidate.c_str(), O_CREAT | O_EXCL | O_WRONLY, 0600);
		if (fd < 0) {
			if (errno == EEXIST) continue;
			return Error{ErrorCode::IoError,
				"cannot create temporary file in " + decision.resolvedPath.string() + ": " + std::strerror(errno)};
		}
		::close(fd);
#endif

		ScopedTempFile temp;
		temp.m_path = candidate;
		return temp;
	}

	return Error{ErrorCode::IoError, "exhausted attempts to create a unique temporary file"};
}

} // namespace ml
