#pragma once

#include <filesystem>
#include <string>

namespace kano::git::commands {

auto FetchCommitDetail(const std::filesystem::path& InRepo,
                       const std::string& InSha,
                       int InMode) -> std::string;
auto FetchCommitFilePatch(const std::filesystem::path& InRepo,
                          const std::string& InSha,
                          const std::string& InPatchPath,
                          const std::string& InPatchPathAlt = {}) -> std::string;
auto FetchWorkingTreeFilePatch(const std::filesystem::path& InRepo,
                               const std::string& InPatchPath,
                               const std::string& InPatchPathAlt = {}) -> std::string;
auto FetchWorkingTreeDetail(const std::filesystem::path& InRepo, int InMode) -> std::string;

}
