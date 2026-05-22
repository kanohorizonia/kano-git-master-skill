#include "repo_operation_scheduler.hpp"

#include "shell_executor.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cctype>
#include <deque>
#include <exception>
#include <format>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <unordered_set>

namespace kano::git::workspace {
namespace {

auto ToLower(std::string InValue) -> std::string {
    std::transform(InValue.begin(), InValue.end(), InValue.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return InValue;
}

auto Trim(std::string InValue) -> std::string {
    while (!InValue.empty() && (InValue.back() == '\n' || InValue.back() == '\r' || InValue.back() == ' ' || InValue.back() == '\t')) {
        InValue.pop_back();
    }
    std::size_t start = 0;
    while (start < InValue.size() && (InValue[start] == ' ' || InValue[start] == '\t')) {
        start += 1;
    }
    return InValue.substr(start);
}

auto NormalizePathForKey(const std::filesystem::path& InPath) -> std::string {
    std::error_code ec;
    auto path = std::filesystem::weakly_canonical(InPath, ec);
    if (ec) {
        path = std::filesystem::absolute(InPath, ec);
    }
    if (ec) {
        path = InPath;
    }
    auto key = path.lexically_normal().generic_string();
    while (key.size() > 1 && key.back() == '/') {
        key.pop_back();
    }
#if defined(_WIN32)
    return ToLower(key);
#else
    return key;
#endif
}

auto RepoPathKey(const RepoRecord& InRepo) -> std::string {
    return NormalizePathForKey(InRepo.path);
}

auto RepoPathKey(const RepoOperationInput& InRepo) -> std::string {
    return NormalizePathForKey(InRepo.path);
}

auto EscapeJson(std::string InValue) -> std::string {
    std::string out;
    out.reserve(InValue.size() + 8);
    for (const char ch : InValue) {
        switch (ch) {
        case '\\': out += "\\\\"; break;
        case '"': out += "\\\""; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default: out.push_back(ch); break;
        }
    }
    return out;
}

auto ResultSortKey(const RepoOperationResult& InResult) -> std::tuple<std::size_t, std::string> {
    return {InResult.phase, NormalizePathForKey(InResult.repoPath)};
}

auto StatusSucceeded(const RepoOperationStatus InStatus) -> bool {
    return InStatus == RepoOperationStatus::Succeeded || InStatus == RepoOperationStatus::Skipped;
}

auto StatusToFailureCategory(const RepoOperationStatus InStatus) -> std::string {
    switch (InStatus) {
    case RepoOperationStatus::Blocked: return "blocked";
    case RepoOperationStatus::Skipped: return "skipped";
    case RepoOperationStatus::Pending: return "pending";
    case RepoOperationStatus::Failed: return "operation-failed";
    case RepoOperationStatus::Succeeded: return {};
    }
    return "unknown";
}

auto MakePendingResult(const RepoOperationInput& InRepo,
                       const RepoOperationSchedulerOptions& InOptions,
                       const std::size_t InPhase,
                       std::string InLockKey) -> RepoOperationResult {
    RepoOperationResult out;
    out.repoId = InRepo.id.empty() ? RepoPathKey(InRepo) : InRepo.id;
    out.repoPath = InRepo.path.lexically_normal();
    out.operationName = InOptions.operationName;
    out.phase = InPhase;
    out.lockKey = std::move(InLockKey);
    return out;
}

auto MakeBlockedResult(const RepoOperationInput& InRepo,
                       const RepoOperationSchedulerOptions& InOptions,
                       const std::size_t InPhase,
                       std::string InLockKey,
                       std::string InBlockedBy,
                       std::string InReason) -> RepoOperationResult {
    auto out = MakePendingResult(InRepo, InOptions, InPhase, std::move(InLockKey));
    out.status = RepoOperationStatus::Blocked;
    out.exitCode = 1;
    out.blockedBy = std::move(InBlockedBy);
    out.blockReason = std::move(InReason);
    out.failureCategory = "blocked-by-dependency";
    out.message = out.blockReason;
    out.resumeRetryEligible = true;
    return out;
}

auto ApplyWorkerResult(RepoOperationResult InBase, const RepoOperationWorkerResult& InWorkerResult) -> RepoOperationResult {
    InBase.status = InWorkerResult.status;
    InBase.exitCode = InWorkerResult.exitCode;
    InBase.stdoutText = InWorkerResult.stdoutText;
    InBase.stderrText = InWorkerResult.stderrText;
    InBase.failureCategory = InWorkerResult.failureCategory.empty()
        ? StatusToFailureCategory(InWorkerResult.status)
        : InWorkerResult.failureCategory;
    InBase.message = InWorkerResult.message;
    InBase.skipReason = InWorkerResult.skipReason;
    InBase.resumeRetryEligible = InWorkerResult.resumeRetryEligible;
    if (InBase.status == RepoOperationStatus::Skipped && InBase.skipReason.empty()) {
        InBase.skipReason = InBase.message;
    }
    if (InBase.status == RepoOperationStatus::Failed && InBase.exitCode == 0) {
        InBase.exitCode = 1;
    }
    return InBase;
}

auto BuildAggregate(std::vector<RepoOperationResult> InResults) -> RepoOperationAggregate {
    std::sort(InResults.begin(), InResults.end(), [](const auto& A, const auto& B) {
        return ResultSortKey(A) < ResultSortKey(B);
    });

    RepoOperationAggregate out;
    out.results = std::move(InResults);
    for (const auto& result : out.results) {
        switch (result.status) {
        case RepoOperationStatus::Succeeded: out.succeeded += 1; break;
        case RepoOperationStatus::Failed: out.failed += 1; break;
        case RepoOperationStatus::Blocked: out.blocked += 1; break;
        case RepoOperationStatus::Skipped: out.skipped += 1; break;
        case RepoOperationStatus::Pending: out.pending += 1; break;
        }
    }
    out.hasFailure = out.failed > 0 || out.blocked > 0 || out.pending > 0;
    return out;
}

auto ResolveLockKey(const RepoOperationInput& InRepo, const RepoOperationSchedulerOptions& InOptions) -> std::string {
    if (!InRepo.explicitLockKey.empty()) {
        return InRepo.explicitLockKey;
    }
    if (InOptions.lockKeyResolver) {
        auto resolved = InOptions.lockKeyResolver(InRepo);
        if (!resolved.empty()) {
            return resolved;
        }
    }
    if (InOptions.resolveGitCommonDirLocks) {
        return ResolveRepoOperationLockKey(InRepo);
    }
    return NormalizePathForKey(InRepo.path);
}

auto BuildChildToParentEdges(const std::vector<RepoOperationInput>& InRepos) -> std::vector<std::vector<std::size_t>> {
    std::unordered_map<std::string, std::size_t> byPath;
    byPath.reserve(InRepos.size());
    for (std::size_t idx = 0; idx < InRepos.size(); ++idx) {
        byPath[RepoPathKey(InRepos[idx])] = idx;
    }

    std::vector<std::vector<std::size_t>> outgoing(InRepos.size());
    for (std::size_t idx = 0; idx < InRepos.size(); ++idx) {
        std::set<std::size_t> uniqueDeps;
        for (const auto& dep : InRepos[idx].dependencies) {
            const auto it = byPath.find(NormalizePathForKey(dep));
            if (it == byPath.end() || it->second == idx) {
                continue;
            }
            if (uniqueDeps.insert(it->second).second) {
                outgoing[it->second].push_back(idx);
            }
        }
    }
    return outgoing;
}

class LockCoordinator {
  public:
    explicit LockCoordinator(const int InJobs)
        : jobs_(std::max(1, InJobs)) {
    }

    void Acquire(const std::string& InLockKey) {
        std::unique_lock lock(mutex_);
        condition_.wait(lock, [&]() {
            return activeCount_ < jobs_ && activeLocks_.find(InLockKey) == activeLocks_.end();
        });
        activeCount_ += 1;
        activeLocks_.insert(InLockKey);
    }

    void Release(const std::string& InLockKey) {
        {
            std::lock_guard lock(mutex_);
            activeLocks_.erase(InLockKey);
            activeCount_ -= 1;
        }
        condition_.notify_all();
    }

  private:
    int jobs_ = 1;
    int activeCount_ = 0;
    std::mutex mutex_;
    std::condition_variable condition_;
    std::unordered_set<std::string> activeLocks_;
};

auto RunWave(const std::vector<RepoOperationInput>& InRepos,
             const std::vector<std::size_t>& InWave,
             const RepoOperationSchedulerOptions& InOptions,
             const RepoOperationWorker& InWorker,
             const std::size_t InPhase,
             const std::unordered_set<std::string>& InBlockedRepoIds,
             const std::unordered_map<std::string, std::string>& InBlockedBy,
             const std::unordered_map<std::string, std::string>& InLockKeys) -> std::vector<RepoOperationResult> {
    std::vector<RepoOperationResult> results;
    std::mutex resultsMutex;
    LockCoordinator coordinator(InOptions.jobs);
    std::vector<std::thread> workers;
    workers.reserve(InWave.size());

    for (const auto repoIndex : InWave) {
        if (repoIndex >= InRepos.size()) {
            continue;
        }
        const auto& repo = InRepos[repoIndex];
        const auto repoId = repo.id.empty() ? RepoPathKey(repo) : repo.id;
        const auto lockIt = InLockKeys.find(repoId);
        const auto lockKey = lockIt == InLockKeys.end() ? ResolveLockKey(repo, InOptions) : lockIt->second;

        if (InBlockedRepoIds.find(repoId) != InBlockedRepoIds.end()) {
            const auto blockedIt = InBlockedBy.find(repoId);
            const auto blockedBy = blockedIt == InBlockedBy.end() ? std::string{} : blockedIt->second;
            auto blocked = MakeBlockedResult(repo, InOptions, InPhase, lockKey, blockedBy, "dependency failed in an earlier phase");
            std::lock_guard lock(resultsMutex);
            results.push_back(std::move(blocked));
            continue;
        }

        workers.emplace_back([&, repo, repoId, lockKey]() {
            coordinator.Acquire(lockKey);
            auto base = MakePendingResult(repo, InOptions, InPhase, lockKey);
            RepoOperationResult result;
            try {
                const auto workerResult = InWorker(repo);
                result = ApplyWorkerResult(std::move(base), workerResult);
            } catch (const std::exception& ex) {
                RepoOperationWorkerResult workerResult;
                workerResult.status = RepoOperationStatus::Failed;
                workerResult.exitCode = 1;
                workerResult.failureCategory = "exception";
                workerResult.message = ex.what();
                result = ApplyWorkerResult(std::move(base), workerResult);
            } catch (...) {
                RepoOperationWorkerResult workerResult;
                workerResult.status = RepoOperationStatus::Failed;
                workerResult.exitCode = 1;
                workerResult.failureCategory = "exception";
                workerResult.message = "unknown exception";
                result = ApplyWorkerResult(std::move(base), workerResult);
            }
            coordinator.Release(lockKey);

            std::lock_guard lock(resultsMutex);
            results.push_back(std::move(result));
        });
    }

    for (auto& worker : workers) {
        worker.join();
    }

    std::sort(results.begin(), results.end(), [](const auto& A, const auto& B) {
        return ResultSortKey(A) < ResultSortKey(B);
    });
    return results;
}

} // namespace

auto BuildExecutionWaves(const std::vector<RepoRecord>& InRepos) -> WavePlan {
    return BuildRepoOperationWaves(MakeRepoOperationInputs(InRepos));
}

auto BuildRepoOperationWaves(const std::vector<RepoOperationInput>& InRepos) -> WavePlan {
    WavePlan plan;
    if (InRepos.empty()) {
        return plan;
    }

    std::unordered_map<std::string, std::size_t> byPath;
    byPath.reserve(InRepos.size());
    for (std::size_t idx = 0; idx < InRepos.size(); ++idx) {
        byPath[RepoPathKey(InRepos[idx])] = idx;
    }

    std::vector<std::vector<std::size_t>> outgoing(InRepos.size());
    std::vector<std::size_t> indegree(InRepos.size(), 0);
    for (std::size_t idx = 0; idx < InRepos.size(); ++idx) {
        std::set<std::size_t> uniqueDeps;
        for (const auto& dep : InRepos[idx].dependencies) {
            const auto it = byPath.find(NormalizePathForKey(dep));
            if (it == byPath.end() || it->second == idx) {
                continue;
            }
            const auto depIdx = it->second;
            if (uniqueDeps.insert(depIdx).second) {
                outgoing[depIdx].push_back(idx);
                indegree[idx] += 1;
            }
        }
    }

    std::vector<std::size_t> ready;
    for (std::size_t idx = 0; idx < InRepos.size(); ++idx) {
        if (indegree[idx] == 0) {
            ready.push_back(idx);
        }
    }

    auto byRepoPath = [&InRepos](const std::size_t A, const std::size_t B) {
        return RepoPathKey(InRepos[A]) < RepoPathKey(InRepos[B]);
    };
    std::sort(ready.begin(), ready.end(), byRepoPath);

    std::size_t processedCount = 0;
    while (!ready.empty()) {
        std::vector<std::size_t> wave = ready;
        plan.waves.push_back(wave);
        processedCount += wave.size();

        std::vector<std::size_t> next;
        for (const auto nodeIdx : wave) {
            for (const auto outIdx : outgoing[nodeIdx]) {
                if (indegree[outIdx] == 0) {
                    continue;
                }
                indegree[outIdx] -= 1;
                if (indegree[outIdx] == 0) {
                    next.push_back(outIdx);
                }
            }
        }

        std::sort(next.begin(), next.end(), byRepoPath);
        next.erase(std::unique(next.begin(), next.end()), next.end());
        ready = std::move(next);
    }

    if (processedCount != InRepos.size()) {
        plan.hasCycle = true;
        for (std::size_t idx = 0; idx < InRepos.size(); ++idx) {
            if (indegree[idx] > 0) {
                plan.cycleNodes.push_back(idx);
            }
        }
        std::sort(plan.cycleNodes.begin(), plan.cycleNodes.end(), byRepoPath);
    }

    return plan;
}

auto FlattenByWaves(const std::vector<RepoRecord>& InRepos, const WavePlan& InPlan) -> std::vector<RepoRecord> {
    std::vector<RepoRecord> ordered;
    for (const auto& wave : InPlan.waves) {
        for (const auto idx : wave) {
            if (idx < InRepos.size()) {
                ordered.push_back(InRepos[idx]);
            }
        }
    }
    return ordered;
}

auto WavesToJson(const std::vector<RepoRecord>& InRepos, const WavePlan& InPlan) -> std::string {
    std::string json;
    json += "{";
    json += std::format("\"has_cycle\":{},", InPlan.hasCycle ? "true" : "false");
    json += "\"cycle_nodes\":[";
    for (std::size_t i = 0; i < InPlan.cycleNodes.size(); ++i) {
        if (i > 0) {
            json += ",";
        }
        const auto idx = InPlan.cycleNodes[i];
        const auto value = idx < InRepos.size() ? RepoPathKey(InRepos[idx]) : std::format("invalid-index-{}", idx);
        json += std::format("\"{}\"", EscapeJson(value));
    }
    json += "],";
    json += "\"waves\":[";
    for (std::size_t waveIdx = 0; waveIdx < InPlan.waves.size(); ++waveIdx) {
        if (waveIdx > 0) {
            json += ",";
        }
        json += "[";
        const auto& wave = InPlan.waves[waveIdx];
        for (std::size_t i = 0; i < wave.size(); ++i) {
            if (i > 0) {
                json += ",";
            }
            const auto idx = wave[i];
            const auto value = idx < InRepos.size() ? RepoPathKey(InRepos[idx]) : std::format("invalid-index-{}", idx);
            json += std::format("\"{}\"", EscapeJson(value));
        }
        json += "]";
    }
    json += "]}";
    return json;
}

auto MakeRepoOperationInputs(const std::vector<RepoRecord>& InRepos) -> std::vector<RepoOperationInput> {
    std::vector<RepoOperationInput> out;
    out.reserve(InRepos.size());
    for (const auto& repo : InRepos) {
        out.push_back(RepoOperationInput{
            .id = RepoPathKey(repo),
            .path = repo.path,
            .type = repo.type,
            .dependencies = repo.dependencies,
        });
    }
    return out;
}

auto ResolveRepoOperationLockKey(const RepoOperationInput& InRepo) -> std::string {
    const auto result = shell::ExecuteCommand("git", {"rev-parse", "--git-common-dir"}, shell::ExecMode::Capture, InRepo.path);
    if (result.exitCode == 0) {
        const auto raw = Trim(result.stdoutStr);
        if (!raw.empty()) {
            std::filesystem::path commonDir(raw);
            if (commonDir.is_relative()) {
                commonDir = InRepo.path / commonDir;
            }
            return NormalizePathForKey(commonDir);
        }
    }
    return NormalizePathForKey(InRepo.path);
}

auto RunRepoOperationScheduler(
    const std::vector<RepoOperationInput>& InRepos,
    const RepoOperationSchedulerOptions& InOptions,
    const RepoOperationWorker& InWorker) -> RepoOperationAggregate {
    if (!InWorker) {
        return BuildAggregate({});
    }

    std::unordered_map<std::string, std::string> lockKeys;
    lockKeys.reserve(InRepos.size());
    for (const auto& repo : InRepos) {
        const auto repoId = repo.id.empty() ? RepoPathKey(repo) : repo.id;
        lockKeys[repoId] = ResolveLockKey(repo, InOptions);
    }

    std::vector<RepoOperationResult> allResults;
    auto plan = InOptions.mode == RepoOperationMode::MutatingDependencyWaves
        ? BuildRepoOperationWaves(InRepos)
        : WavePlan{.waves = {}};

    if (InOptions.mode == RepoOperationMode::ReadOnlyParallel) {
        std::vector<std::size_t> wave;
        wave.reserve(InRepos.size());
        for (std::size_t idx = 0; idx < InRepos.size(); ++idx) {
            wave.push_back(idx);
        }
        std::sort(wave.begin(), wave.end(), [&InRepos](const auto A, const auto B) {
            return RepoPathKey(InRepos[A]) < RepoPathKey(InRepos[B]);
        });
        plan.waves = {wave};
    }

    std::unordered_set<std::string> failedOrBlocked;
    std::unordered_map<std::string, std::string> blockedBy;
    const auto outgoing = BuildChildToParentEdges(InRepos);

    for (std::size_t phase = 0; phase < plan.waves.size(); ++phase) {
        const auto phaseResults = RunWave(InRepos, plan.waves[phase], InOptions, InWorker, phase, failedOrBlocked, blockedBy, lockKeys);
        for (const auto& result : phaseResults) {
            if (!StatusSucceeded(result.status)) {
                failedOrBlocked.insert(result.repoId);
                const auto failedIndexIt = std::find_if(InRepos.begin(), InRepos.end(), [&](const auto& repo) {
                    const auto repoId = repo.id.empty() ? RepoPathKey(repo) : repo.id;
                    return repoId == result.repoId;
                });
                if (failedIndexIt != InRepos.end()) {
                    const auto failedIndex = static_cast<std::size_t>(std::distance(InRepos.begin(), failedIndexIt));
                    for (const auto dependentIndex : outgoing[failedIndex]) {
                        if (dependentIndex < InRepos.size()) {
                            const auto& dependent = InRepos[dependentIndex];
                            const auto dependentId = dependent.id.empty() ? RepoPathKey(dependent) : dependent.id;
                            failedOrBlocked.insert(dependentId);
                            blockedBy.emplace(dependentId, result.repoId);
                        }
                    }
                }
            }
            allResults.push_back(result);
        }
    }

    if (plan.hasCycle) {
        for (const auto idx : plan.cycleNodes) {
            if (idx >= InRepos.size()) {
                continue;
            }
            const auto& repo = InRepos[idx];
            const auto repoId = repo.id.empty() ? RepoPathKey(repo) : repo.id;
            const auto lockIt = lockKeys.find(repoId);
            allResults.push_back(MakeBlockedResult(
                repo,
                InOptions,
                plan.waves.size(),
                lockIt == lockKeys.end() ? ResolveLockKey(repo, InOptions) : lockIt->second,
                "cycle",
                "dependency cycle prevents scheduling"));
        }
    }

    return BuildAggregate(std::move(allResults));
}

} // namespace kano::git::workspace
