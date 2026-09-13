// SPDX-License-Identifier: GPL-3.0-or-later
// SAFE-001 / SAFE-002 / FN-SAFE-01: the filesystem safety boundary.
//
// This is the single place that decides whether a path may be written. Every
// writer in the application asks this class first, and there is deliberately no
// other way to obtain write permission.
//
// Path-string comparison alone is not sufficient. A symlink, a bind mount, a
// junction or a reparse point can make a destination that *looks* outside a
// protected root resolve inside it. The guard therefore resolves paths and also
// compares filesystem identity (device and inode) where the platform provides it.
#pragma once

#include "mlcore/Types.hpp"

#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace ml {

/// Why a path was refused. Each maps to a specific FRD safety rule.
enum class GuardVerdict {
	Allowed = 0,
	InsideProtectedRoot,        ///< Resolves inside a protected source root.
	ResolvesIntoProtectedRoot,  ///< A link or mount redirects it into one.
	OverlapsProtectedRoot,      ///< A protected root sits inside this path.
	OutsideAllowedOutput,       ///< Not under any configured output root.
	IsSymlink,                  ///< The target itself is a link; we never follow it to write.
	NotAbsolute,
	EmptyPath,
	ResolutionFailed,
};

std::string_view toString(GuardVerdict v);

struct GuardDecision {
	GuardVerdict verdict = GuardVerdict::ResolutionFailed;
	std::string reason;
	std::filesystem::path resolvedPath;
	/// The protected root that caused a refusal, when applicable.
	std::filesystem::path offendingRoot;

	bool allowed() const { return verdict == GuardVerdict::Allowed; }
};

/// A root the application may read but must never write.
struct ProtectedRoot {
	RootId id;
	std::filesystem::path path;          ///< As configured.
	std::filesystem::path resolvedPath;  ///< Fully resolved, links followed.
	std::uint64_t deviceId = 0;
	std::uint64_t inode = 0;
	std::string label;
	/// Volume identity, so a disconnected drive is distinguishable from a
	/// deleted collection (FN-SCAN-04).
	std::string volumeIdentity;
	bool currentlyPresent = false;
};

/// Thread-safe. The guard is consulted from every worker, so its state is
/// protected and its roots are effectively immutable after configuration.
class PathGuard {
public:
	PathGuard();

	/// Registers a source root as protected. Returns an error when the path does
	/// not exist or cannot be resolved; a protected root that cannot be resolved
	/// must not be silently ignored.
	Status addProtectedRoot(const std::filesystem::path& path, std::string label = {});

	/// Registers an output root. Fails when the output root is inside, contains,
	/// or resolves into a protected root.
	Status addOutputRoot(const std::filesystem::path& path);

	/// May the application write to this path?
	///
	/// The path need not exist yet; its nearest existing ancestor is resolved so
	/// a symlinked parent directory is still detected.
	GuardDecision checkWrite(const std::filesystem::path& path) const;

	/// May the application read this path? Reading inside a protected root is
	/// allowed; this checks only that the path is well-formed and resolvable.
	GuardDecision checkRead(const std::filesystem::path& path) const;

	/// Verifies that a file is not the same file as anything inside a protected
	/// root, comparing device and inode rather than path text.
	bool isSameFileAsProtectedSource(const std::filesystem::path& path) const;

	/// True when this path is inside a protected root after resolution.
	bool isInsideProtectedRoot(const std::filesystem::path& path) const;

	/// Creates a directory tree, refusing anything checkWrite() would refuse.
	Status createDirectories(const std::filesystem::path& path) const;

	/// Returns the configured roots. Copies, so callers cannot mutate state.
	std::vector<ProtectedRoot> protectedRoots() const;
	std::vector<std::filesystem::path> outputRoots() const;

	/// Re-checks whether each protected root is currently present. A root that
	/// has become absent is reported, never treated as an empty collection.
	void refreshRootPresence();

	/// Number of protected roots that were configured but are not currently
	/// reachable. The scanner refuses to record deletions when this is non-zero.
	std::size_t absentRootCount() const;

	/// Resolves a path as far as it exists, returning the resolved form of the
	/// nearest existing ancestor joined with the remaining components. Never
	/// throws; errors become a failed decision.
	static Result<std::filesystem::path> resolveAsFarAsPossible(const std::filesystem::path& path);

	/// True when `child` is lexically inside `parent`. Both must already be
	/// resolved and absolute. Component-wise, so "/musicX" is not inside "/music".
	static bool isLexicallyInside(const std::filesystem::path& child, const std::filesystem::path& parent);

	/// Reads filesystem identity. Returns nullopt when the path does not exist.
	static std::optional<FileIdentity> identityOf(const std::filesystem::path& path);

	/// True when the path is a symlink, junction or other reparse point.
	static bool isReparsePoint(const std::filesystem::path& path);

	/// Number of hard links to this file. Used to refuse a hardlinked "copy"
	/// (SAFE-002), which would let a write reach the protected original.
	static std::optional<std::uintmax_t> hardLinkCount(const std::filesystem::path& path);

private:
	GuardDecision checkWriteLocked(const std::filesystem::path& path) const;

	mutable std::mutex m_mutex;
	std::vector<ProtectedRoot> m_protectedRoots;
	std::vector<std::filesystem::path> m_outputRoots;
	std::int64_t m_nextRootId = 1;
};

/// RAII temporary file created exclusively in a directory the guard allows.
///
/// The file is removed on destruction unless `release()` was called, so an
/// interrupted write never leaves a partial output behind (FN-SAFE-02).
class ScopedTempFile {
public:
	ScopedTempFile() = default;
	~ScopedTempFile();

	ScopedTempFile(const ScopedTempFile&) = delete;
	ScopedTempFile& operator=(const ScopedTempFile&) = delete;
	ScopedTempFile(ScopedTempFile&& other) noexcept;
	ScopedTempFile& operator=(ScopedTempFile&& other) noexcept;

	/// Creates an exclusively-owned temporary file in `directory`. The directory
	/// must be on the same filesystem as the final destination so publication can
	/// be an atomic same-filesystem rename.
	static Result<ScopedTempFile> createIn(const PathGuard& guard, const std::filesystem::path& directory,
		std::string_view prefix = "resonance-");

	const std::filesystem::path& path() const { return m_path; }
	bool valid() const { return !m_path.empty(); }

	/// Gives up ownership: the file survives destruction.
	std::filesystem::path release();

	/// Deletes the file now.
	void remove();

private:
	std::filesystem::path m_path;
};

} // namespace ml
