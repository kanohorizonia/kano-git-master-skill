#pragma once

#include <filesystem>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace kano::git::commands {

struct PostSyncRepoPathScope {
    std::vector<std::string> include;
    std::vector<std::string> exclude;
};

struct PostSyncPlanPathScope {
    bool scoped = false;
    std::unordered_map<std::string, PostSyncRepoPathScope> repos;
};

enum class PostSyncDeltaKind {
    None,
    GitlinkOnly,
    SemanticDrift,
};

struct PostSyncDeltaSummary {
    PostSyncDeltaKind kind = PostSyncDeltaKind::None;
    int gitlinkOnlyRepoCount = 0;
    std::vector<std::filesystem::path> semanticRepos;
};

auto NormalizeCommitPushGitPath(std::string InPath) -> std::string;
auto CommitPushPathspecCoversPath(std::string InPathspec, std::string InPath) -> bool;
auto CommitPushRepoScopeKey(const std::filesystem::path& InRepoRoot) -> std::string;

auto RepoHasPostSyncWorkingTreeChanges(const std::filesystem::path& InRepoRoot) -> bool;
auto AnyRepoHasPostSyncWorkingTreeChanges(const std::vector<std::filesystem::path>& InRepos,
                                          const PostSyncPlanPathScope* InScope) -> bool;
auto ClassifyPostSyncDelta(const std::vector<std::filesystem::path>& InCandidateRepos,
                           const PostSyncPlanPathScope* InScope = nullptr) -> PostSyncDeltaSummary;
auto AutoAmendGitlinkOnlyPostSyncRepos(const std::filesystem::path& InWorkspaceRoot,
                                       const std::vector<std::filesystem::path>& InCandidateRepos,
                                       const PostSyncPlanPathScope* InScope = nullptr) -> std::pair<int, int>;

} // namespace kano::git::commands
