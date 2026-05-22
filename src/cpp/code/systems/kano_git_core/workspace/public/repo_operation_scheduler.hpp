#pragma once

#include "discovery.hpp"

#include <cstddef>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace kano::git::workspace {

enum class RepoOperationMode {
    ReadOnlyParallel,
    MutatingDependencyWaves,
};

enum class RepoOperationStatus {
    Pending,
    Succeeded,
    Failed,
    Blocked,
    Skipped,
};

struct WavePlan {
    std::vector<std::vector<std::size_t>> waves;
    std::vector<std::size_t> cycleNodes;
    bool hasCycle = false;
};

struct RepoOperationInput {
    std::string id;
    std::filesystem::path path;
    std::string type;
    std::vector<std::filesystem::path> dependencies;
    std::string explicitLockKey;
};

struct RepoOperationWorkerResult {
    RepoOperationStatus status = RepoOperationStatus::Succeeded;
    int exitCode = 0;
    std::string stdoutText;
    std::string stderrText;
    std::string failureCategory;
    std::string message;
    std::string skipReason;
    bool resumeRetryEligible = true;
};

struct RepoOperationResult {
    std::string repoId;
    std::filesystem::path repoPath;
    std::string operationName;
    RepoOperationStatus status = RepoOperationStatus::Pending;
    int exitCode = 0;
    std::string skipReason;
    std::string blockReason;
    std::string blockedBy;
    std::string failureCategory;
    std::string message;
    std::string stdoutText;
    std::string stderrText;
    std::size_t phase = 0;
    std::string lockKey;
    bool resumeRetryEligible = true;
};

struct RepoOperationAggregate {
    std::vector<RepoOperationResult> results;
    std::size_t succeeded = 0;
    std::size_t failed = 0;
    std::size_t blocked = 0;
    std::size_t skipped = 0;
    std::size_t pending = 0;
    bool hasFailure = false;
};

struct RepoOperationSchedulerOptions {
    std::string operationName = "repo-operation";
    RepoOperationMode mode = RepoOperationMode::ReadOnlyParallel;
    int jobs = 1;
    bool resolveGitCommonDirLocks = true;
    std::function<std::string(const RepoOperationInput&)> lockKeyResolver;
};

using RepoOperationWorker = std::function<RepoOperationWorkerResult(const RepoOperationInput&)>;

auto BuildExecutionWaves(const std::vector<RepoRecord>& InRepos) -> WavePlan;
auto BuildRepoOperationWaves(const std::vector<RepoOperationInput>& InRepos) -> WavePlan;
auto FlattenByWaves(const std::vector<RepoRecord>& InRepos, const WavePlan& InPlan) -> std::vector<RepoRecord>;
auto WavesToJson(const std::vector<RepoRecord>& InRepos, const WavePlan& InPlan) -> std::string;
auto MakeRepoOperationInputs(const std::vector<RepoRecord>& InRepos) -> std::vector<RepoOperationInput>;
auto ResolveRepoOperationLockKey(const RepoOperationInput& InRepo) -> std::string;
auto RunRepoOperationScheduler(
    const std::vector<RepoOperationInput>& InRepos,
    const RepoOperationSchedulerOptions& InOptions,
    const RepoOperationWorker& InWorker) -> RepoOperationAggregate;

} // namespace kano::git::workspace
