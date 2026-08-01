#include "tui_repo_identity.hpp"

#include <cstdint>
#include <system_error>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/stat.h>
#endif

namespace kano::git::commands {
namespace {

auto StripTrailingSeparators(std::string InValue) -> std::string {
    while (InValue.size() > 1 && InValue.back() == '/') {
        InValue.pop_back();
    }
    return InValue;
}

}  // namespace

auto NormalizeRepoIdentityKey(const std::filesystem::path& InPath)
    -> std::string {
    return StripTrailingSeparators(
        InPath.lexically_normal().generic_string());
}

auto ResolveStableRepoIdentityKey(const std::filesystem::path& InPath)
    -> std::string {
    std::error_code error;
    const auto resolved = std::filesystem::weakly_canonical(InPath, error);
    const auto representativePath =
        error || resolved.empty() ? InPath.lexically_normal() : resolved;
    const auto candidateKey =
        NormalizeRepoIdentityKey(representativePath);

#ifdef _WIN32
    const auto handle = ::CreateFileW(
        representativePath.c_str(),
        0,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS,
        nullptr);
    if (handle != INVALID_HANDLE_VALUE) {
        BY_HANDLE_FILE_INFORMATION metadata {};
        const bool bHasMetadata =
            ::GetFileInformationByHandle(handle, &metadata) != 0;
        ::CloseHandle(handle);
        if (bHasMetadata) {
            const auto fileIndex =
                (static_cast<std::uint64_t>(
                     metadata.nFileIndexHigh)
                 << 32U) |
                static_cast<std::uint64_t>(
                    metadata.nFileIndexLow);
            return "win:" +
                std::to_string(metadata.dwVolumeSerialNumber) +
                ":" + std::to_string(fileIndex);
        }
    }
#else
    // weakly_canonical resolves symlinks but preserves caller casing on
    // case-insensitive APFS. A POSIX filesystem object identity is stable
    // across symlink, case, and Unicode spelling aliases without folding two
    // distinct directories on a case-sensitive volume.
    struct stat metadata {};
    if (::stat(representativePath.c_str(), &metadata) == 0) {
        return "posix:" +
            std::to_string(static_cast<std::uintmax_t>(metadata.st_dev)) +
            ":" +
            std::to_string(static_cast<std::uintmax_t>(metadata.st_ino));
    }
#endif
    return candidateKey;
}

auto ResolveRepoRelativePath(
    const std::filesystem::path& InAncestor,
    const std::filesystem::path& InChild) -> std::filesystem::path {
    const auto normalizedAncestor = InAncestor.lexically_normal();
    const auto normalizedChild = InChild.lexically_normal();
    auto cursor = normalizedChild;
    std::vector<std::filesystem::path> reversedComponents;
    while (!cursor.empty()) {
        std::error_code equivalentError;
        if (std::filesystem::equivalent(
                normalizedAncestor,
                cursor,
                equivalentError) &&
            !equivalentError) {
            std::filesystem::path relative;
            for (auto component = reversedComponents.rbegin();
                 component != reversedComponents.rend();
                 ++component) {
                relative /= *component;
            }
            return relative.empty()
                ? std::filesystem::path(".")
                : relative;
        }

        const auto parent = cursor.parent_path();
        if (parent == cursor) {
            break;
        }
        reversedComponents.push_back(cursor.filename());
        cursor = parent;
    }

    const auto lexical =
        normalizedChild.lexically_relative(normalizedAncestor);
    return lexical.empty() ? normalizedChild : lexical;
}

auto SelectPreferredRepoIdentityIndices(
    const std::vector<std::string>& InIdentityKeys,
    const std::vector<int>& InRepresentativePriorities)
    -> std::vector<std::size_t> {
    if (InIdentityKeys.size() !=
        InRepresentativePriorities.size()) {
        return {};
    }

    std::unordered_map<std::string, std::size_t> selectedPositions;
    std::vector<std::size_t> selectedIndices;
    selectedIndices.reserve(InIdentityKeys.size());
    for (std::size_t index = 0;
         index < InIdentityKeys.size();
         ++index) {
        const auto [selected, bInserted] =
            selectedPositions.emplace(
                InIdentityKeys[index],
                selectedIndices.size());
        if (bInserted) {
            selectedIndices.push_back(index);
            continue;
        }
        auto& currentIndex =
            selectedIndices[selected->second];
        if (InRepresentativePriorities[index] <
            InRepresentativePriorities[currentIndex]) {
            currentIndex = index;
        }
    }
    return selectedIndices;
}

}  // namespace kano::git::commands
