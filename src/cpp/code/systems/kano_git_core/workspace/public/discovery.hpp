#pragma once

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace kano::git::workspace {

enum class DiscoverScope {
    RegisteredOnly,
    Full,
};

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
    DiscoverScope scope = DiscoverScope::RegisteredOnly;
    std::function<void(const std::string&)> progressCallback;
};

struct DiscoveryResult {
    std::vector<RepoRecord> repos;
    std::string mode;
    std::filesystem::path cacheFile;
    std::string marker;
};

struct WorkspaceManifest {
    std::filesystem::path workspaceRoot;
    std::vector<RepoRecord> repos;
    std::unordered_map<std::string, std::string> gitmodulesFingerprints;
    std::filesystem::path manifestFile;
};

struct CacheLockInfo {
    std::filesystem::path lockPath;
    std::filesystem::path targetPath;
    bool exists = false;
    bool activeProcessDetected = false;
    bool staleCandidate = false;
    long long ageSeconds = -1;
    long long ownerPid = -1;
    std::string ownerCommand;
};

auto DiscoverRepos(const DiscoverOptions& InOptions) -> DiscoveryResult;
auto ReposToJson(const std::vector<RepoRecord>& InRepos) -> std::string;
auto ManifestToJson(const std::filesystem::path& InWorkspaceRoot, const std::vector<RepoRecord>& InRepos) -> std::string;
auto DiscoverRegisteredPathsRecursive(const std::filesystem::path& InWorkspaceRoot) -> std::vector<std::filesystem::path>;
auto WorkspaceManifestFilePath(const std::filesystem::path& InWorkspaceRoot) -> std::filesystem::path;
auto BuildWorkspaceManifest(const std::filesystem::path& InWorkspaceRoot, const std::vector<RepoRecord>& InRepos) -> WorkspaceManifest;
auto SaveWorkspaceManifest(const WorkspaceManifest& InManifest) -> bool;
auto LoadTrustedWorkspaceManifest(const std::filesystem::path& InWorkspaceRoot,
                                  std::string* OutReason = nullptr) -> std::optional<WorkspaceManifest>;
auto UpsertUnregisteredRepoIntoWorkspaceManifest(const std::filesystem::path& InWorkspaceRoot,
                                                 const std::filesystem::path& InRepoPath) -> bool;
auto RefreshWorkspaceManifestAfterRegisteredChange(const std::filesystem::path& InWorkspaceRoot) -> bool;
auto WriteCacheFileText(const std::filesystem::path& InFile,
                        const std::string& InText,
                        std::string* OutError = nullptr) -> bool;
auto InspectCacheLocks(const std::filesystem::path& InCacheRoot) -> std::vector<CacheLockInfo>;
auto CleanupStaleCacheLocks(const std::filesystem::path& InCacheRoot,
                            int InMinAgeSeconds = 30) -> std::vector<CacheLockInfo>;
auto GetSubmoduleConfig(const std::filesystem::path& InRoot, const std::filesystem::path& InSubmodulePath, const std::string& InKey) -> std::string;

} // namespace kano::git::workspace
