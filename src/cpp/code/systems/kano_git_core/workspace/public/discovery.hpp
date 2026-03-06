#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace kano::git::workspace {

struct RepoRecord {
    std::filesystem::path path;
    std::string type;
    std::string currentBranch;
    std::string remotes;
    bool hasChanges = false;
    std::vector<std::filesystem::path> dependencies;
};

struct DiscoverOptions {
    std::filesystem::path rootDir = ".";
    int maxDepth = 3;
    // Temporary operator/debug scan-scope override only.
    // Shared workspace exclusion policy should live in .gitignore / .kogignore.
    std::vector<std::string> excludePatterns;
    bool useCache = true;
    int cacheTtlSeconds = 60;
    bool refreshCache = false;
    bool incremental = true;
    int maxStaleSeconds = 900;
    std::string metadataLevel = "full";
};

struct DiscoveryResult {
    std::vector<RepoRecord> repos;
    std::string mode;
    std::filesystem::path cacheFile;
    std::string marker;
};

auto DiscoverRepos(const DiscoverOptions& InOptions) -> DiscoveryResult;
auto ReposToJson(const std::vector<RepoRecord>& InRepos) -> std::string;
auto ManifestToJson(const std::filesystem::path& InWorkspaceRoot, const std::vector<RepoRecord>& InRepos) -> std::string;

} // namespace kano::git::workspace
