#pragma once

#include <filesystem>
#include <string>

namespace kano::git::commands::detail {

bool PlanSemanticallyCleanWorktreeRemoval(
    const std::filesystem::path& InWorktreePath,
    std::string& OutRemovalMode,
    std::string& OutMessage);

bool RemoveSemanticallyCleanWorktree(
    const std::filesystem::path& InRepoPath,
    const std::filesystem::path& InWorktreePath,
    std::string& OutNormalizedRemovalMode,
    std::string& OutMessage);

} // namespace kano::git::commands::detail
