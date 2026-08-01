#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace kano::git::commands {

struct TuiStartupRepoSnapshot {
    std::filesystem::path path;
    std::filesystem::path parentPath;
    std::string relativePath;
    std::string type;
    std::string branch;
    std::string upstream;
    std::string tracking;
    bool statusKnown = true;
    bool repoDirty = false;
    bool worktreeDirty = false;
};

struct TuiStartupSnapshotLoadOptions {
    std::string binaryCommand;
    std::filesystem::path workspaceRoot;
    unsigned int timeoutMs = 5000;
    std::size_t maxCaptureBytes = 4U * 1024U * 1024U;
};

struct TuiStartupSnapshotMetadata {
    std::string source;
    std::string completeness;
    std::string probeMode;
    bool statusKnown = true;
    std::vector<std::filesystem::path> allowedExternalRoots;
};

constexpr std::size_t kTuiStartupMaxRepos = 4096;
constexpr std::size_t kTuiStartupMaxFieldBytes = 16U * 1024U;
constexpr std::size_t kTuiStartupMaxExternalRoots = 256;

[[nodiscard]] auto TuiAuditBooleanLabel(
    bool bInKnown,
    bool bInValue) -> std::string_view;

[[nodiscard]] auto ParseTuiStartupSnapshotJson(
    std::string_view InJson,
    const std::filesystem::path& InWorkspaceRoot,
    std::string* OutError = nullptr,
    TuiStartupSnapshotMetadata* OutMetadata = nullptr)
    -> std::vector<TuiStartupRepoSnapshot>;

[[nodiscard]] auto LoadTuiStartupSnapshot(
    const TuiStartupSnapshotLoadOptions& InOptions,
    TuiStartupSnapshotMetadata* OutMetadata = nullptr)
    -> std::vector<TuiStartupRepoSnapshot>;

}  // namespace kano::git::commands
