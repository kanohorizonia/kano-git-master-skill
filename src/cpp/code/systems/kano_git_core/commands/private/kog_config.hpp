#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace kano::git::commands::kog_config {

auto HomeDirectory() -> std::filesystem::path;

auto ResolveConfigSearchPaths(const std::filesystem::path& InWorkspaceRoot,
                              const std::filesystem::path& InSkillRoot) -> std::vector<std::filesystem::path>;

auto NormalizeAiModelSelection(const std::string& InValue) -> std::string;

auto ResolveDefaultAiModelSelection(const std::string& InProvider,
                                    const std::filesystem::path& InWorkspaceRoot,
                                    const std::filesystem::path& InSkillRoot,
                                    const std::string& InFallback = "auto") -> std::string;

auto NormalizePlanCommitGenerationMode(const std::string& InValue) -> std::string;

auto ResolvePlanCommitGenerationMode(const std::filesystem::path& InWorkspaceRoot,
                                     const std::filesystem::path& InSkillRoot,
                                     const std::string& InFallback = "adaptive") -> std::string;

auto GetKnownModelsForProvider(const std::string& InProvider) -> std::vector<std::string>;

// --- Layer path resolution ---

auto SystemConfigPath(const std::filesystem::path& InSkillRoot) -> std::filesystem::path;
auto GlobalConfigPath() -> std::filesystem::path;
auto LocalConfigPath(const std::filesystem::path& InWorkspaceRoot) -> std::filesystem::path;

// --- Generic TOML key operations ---

auto ReadTomlValue(const std::filesystem::path& InConfigPath,
                   const std::string& InDottedKey) -> std::string;

auto ReadEffectiveValue(const std::filesystem::path& InWorkspaceRoot,
                        const std::filesystem::path& InSkillRoot,
                        const std::string& InDottedKey) -> std::string;

auto WriteTomlValue(const std::filesystem::path& InConfigPath,
                    const std::string& InDottedKey,
                    const std::string& InValue) -> bool;

auto UnsetTomlKey(const std::filesystem::path& InConfigPath,
                  const std::string& InDottedKey) -> bool;

auto ListTomlKeys(const std::filesystem::path& InConfigPath)
    -> std::vector<std::pair<std::string, std::string>>;

} // namespace kano::git::commands::kog_config