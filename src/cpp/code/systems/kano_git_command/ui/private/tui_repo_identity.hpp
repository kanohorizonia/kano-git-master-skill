#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace kano::git::commands {

/// Build a comparison key without touching the filesystem.
///
/// Use this only as a fallback or for already-resolved paths. Interactive
/// render code must consume a cached key instead of resolving paths again.
[[nodiscard]] auto NormalizeRepoIdentityKey(
    const std::filesystem::path& InPath) -> std::string;

/// Resolve existing path components once, then return a stable filesystem
/// object key. On POSIX hosts this uses device/inode identity, so symlink and
/// case aliases converge without folding distinct directories on a
/// case-sensitive volume. Failures fall back to lexical normalization.
[[nodiscard]] auto ResolveStableRepoIdentityKey(
    const std::filesystem::path& InPath) -> std::string;

/// Build a child path relative to an ancestor using host filesystem
/// equivalence for the ancestry boundary. This remains correct when the two
/// inputs spell a case-insensitive ancestor differently.
[[nodiscard]] auto ResolveRepoRelativePath(
    const std::filesystem::path& InAncestor,
    const std::filesystem::path& InChild) -> std::filesystem::path;

/// Return one index for each stable repository identity key, preferring the
/// lowest numeric representative priority.
[[nodiscard]] auto SelectPreferredRepoIdentityIndices(
    const std::vector<std::string>& InIdentityKeys,
    const std::vector<int>& InRepresentativePriorities)
    -> std::vector<std::size_t>;

}  // namespace kano::git::commands
