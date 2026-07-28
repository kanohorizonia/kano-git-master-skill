#pragma once

#include <filesystem>
#include <string>

namespace kano::git::commands {

struct NativeAiConfig;

auto DefaultSharedPlanPath(const std::filesystem::path& InWorkspaceRoot) -> std::filesystem::path;
auto RunPlanNewViaSelf(const std::filesystem::path& InWorkspaceRoot,
                       const std::filesystem::path& InPlanPath) -> int;
auto RunCommitSeedViaSelf(const std::filesystem::path& InWorkspaceRoot,
                          const std::filesystem::path& InPlanPath) -> int;
auto RunCommitAutoPlanPipeline(const std::filesystem::path& InWorkspaceRoot,
                               const NativeAiConfig& InAi,
                               const std::string& InAiFillMode,
                               bool InProfile,
                               bool InAllowEmptyDirty) -> int;
auto RunAmendAutoPlanPipeline(const std::filesystem::path& InWorkspaceRoot,
                              const NativeAiConfig& InAi,
                              const std::string& InAiFillMode,
                              bool InProfile,
                              bool InAllowEmptyDirty) -> int;

} // namespace kano::git::commands
