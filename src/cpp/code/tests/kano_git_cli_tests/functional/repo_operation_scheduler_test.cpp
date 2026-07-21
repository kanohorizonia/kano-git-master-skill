#include "repo_operation_scheduler.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace kano::git::tests::functional {
namespace {

using kano::git::workspace::RepoOperationInput;
using kano::git::workspace::RepoOperationMode;
using kano::git::workspace::RepoOperationSchedulerOptions;
using kano::git::workspace::RepoOperationStatus;
using kano::git::workspace::RepoOperationWorkerResult;

auto MakeRepo(std::string InId, std::string InPath, std::string InLockKey = {}) -> RepoOperationInput {
    RepoOperationInput out;
    out.id = std::move(InId);
    out.path = std::filesystem::path(InPath);
    out.explicitLockKey = std::move(InLockKey);
    return out;
}

auto Success(std::string InStdout = {}) -> RepoOperationWorkerResult {
    RepoOperationWorkerResult out;
    out.status = RepoOperationStatus::Succeeded;
    out.stdoutText = std::move(InStdout);
    return out;
}

} // namespace

TEST_CASE("repo operation scheduler serializes identical common-dir locks", "[tdd][unit][feature:repo-operation-scheduler][functional][scheduler]") {
    std::vector<RepoOperationInput> repos{
        MakeRepo("repo-a", "workspace/repo-a", "common-dir/shared"),
        MakeRepo("repo-b", "workspace/repo-b", "common-dir/shared"),
    };

    RepoOperationSchedulerOptions options;
    options.operationName = "lock-test";
    options.mode = RepoOperationMode::ReadOnlyParallel;
    options.jobs = 2;
    options.resolveGitCommonDirLocks = false;

    std::atomic<int> active{0};
    std::atomic<int> maxActive{0};
    const auto aggregate = kano::git::workspace::RunRepoOperationScheduler(repos, options, [&](const RepoOperationInput& repo) {
        const auto nowActive = active.fetch_add(1) + 1;
        maxActive.store(std::max(maxActive.load(), nowActive));
        std::this_thread::sleep_for(std::chrono::milliseconds(40));
        active.fetch_sub(1);
        return Success(repo.id + "\n");
    });

    REQUIRE(aggregate.succeeded == 2);
    REQUIRE(aggregate.failed == 0);
    REQUIRE(maxActive.load() == 1);
}

TEST_CASE("repo operation scheduler runs independent repos concurrently with deterministic buffered results", "[tdd][unit][feature:repo-operation-scheduler][functional][scheduler]") {
    std::vector<RepoOperationInput> repos{
        MakeRepo("repo-b", "workspace/b", "lock-b"),
        MakeRepo("repo-a", "workspace/a", "lock-a"),
    };

    RepoOperationSchedulerOptions options;
    options.operationName = "read-only-status";
    options.mode = RepoOperationMode::ReadOnlyParallel;
    options.jobs = 2;
    options.resolveGitCommonDirLocks = false;

    std::mutex mutex;
    std::condition_variable condition;
    int active = 0;
    int maxActive = 0;

    const auto aggregate = kano::git::workspace::RunRepoOperationScheduler(repos, options, [&](const RepoOperationInput& repo) {
        {
            std::unique_lock lock(mutex);
            active += 1;
            maxActive = std::max(maxActive, active);
            condition.notify_all();
            condition.wait_for(lock, std::chrono::milliseconds(250), [&]() { return active >= 2; });
            active -= 1;
        }
        return Success("stdout:" + repo.id + "\n");
    });

    REQUIRE(aggregate.succeeded == 2);
    REQUIRE(maxActive == 2);
    REQUIRE(aggregate.results.size() == 2);
    REQUIRE(aggregate.results[0].repoId == "repo-a");
    REQUIRE(aggregate.results[1].repoId == "repo-b");
    REQUIRE(aggregate.results[0].stdoutText == "stdout:repo-a\n");
    REQUIRE(aggregate.results[1].stdoutText == "stdout:repo-b\n");
}

TEST_CASE("repo operation scheduler jobs one executes inline in deterministic path order", "[tdd][unit][feature:repo-operation-scheduler][functional][scheduler][serial]") {
    std::vector<RepoOperationInput> repos{
        MakeRepo("repo-b", "workspace/b", "lock-b"),
        MakeRepo("repo-a", "workspace/a", "lock-a"),
    };

    RepoOperationSchedulerOptions options;
    options.operationName = "serial-sync";
    options.mode = RepoOperationMode::ReadOnlyParallel;
    options.jobs = 1;
    options.resolveGitCommonDirLocks = false;

    const auto callerThread = std::this_thread::get_id();
    std::vector<std::string> invocationOrder;
    std::vector<std::thread::id> invocationThreads;
    const auto aggregate = kano::git::workspace::RunRepoOperationScheduler(repos, options, [&](const RepoOperationInput& repo) {
        invocationOrder.push_back(repo.id);
        invocationThreads.push_back(std::this_thread::get_id());
        return Success(repo.id + "\n");
    });

    REQUIRE(aggregate.succeeded == 2);
    REQUIRE(invocationOrder == std::vector<std::string>{"repo-a", "repo-b"});
    REQUIRE(std::all_of(invocationThreads.begin(), invocationThreads.end(), [&](const auto threadId) {
        return threadId == callerThread;
    }));
}

TEST_CASE("repo operation scheduler mutating waves order children before parents", "[tdd][unit][feature:repo-operation-scheduler][functional][scheduler]") {
    auto child = MakeRepo("child", "workspace/child", "lock-child");
    auto parent = MakeRepo("parent", "workspace/parent", "lock-parent");
    parent.dependencies.push_back(child.path);
    std::vector<RepoOperationInput> repos{parent, child};

    RepoOperationSchedulerOptions options;
    options.operationName = "mutating-sync";
    options.mode = RepoOperationMode::MutatingDependencyWaves;
    options.jobs = 2;
    options.resolveGitCommonDirLocks = false;

    std::mutex mutex;
    std::vector<std::string> invocationOrder;
    const auto aggregate = kano::git::workspace::RunRepoOperationScheduler(repos, options, [&](const RepoOperationInput& repo) {
        std::lock_guard lock(mutex);
        invocationOrder.push_back(repo.id);
        return Success(repo.id + " done\n");
    });

    REQUIRE(aggregate.succeeded == 2);
    REQUIRE(invocationOrder == std::vector<std::string>{"child", "parent"});
    REQUIRE(aggregate.results.size() == 2);
    REQUIRE(aggregate.results[0].repoId == "child");
    REQUIRE(aggregate.results[0].phase == 0);
    REQUIRE(aggregate.results[1].repoId == "parent");
    REQUIRE(aggregate.results[1].phase == 1);
}

TEST_CASE("repo operation scheduler aggregates partial failures and blocks only dependent ancestors", "[tdd][unit][feature:repo-operation-scheduler][functional][scheduler]") {
    auto child = MakeRepo("child", "workspace/child", "lock-child");
    auto parent = MakeRepo("parent", "workspace/parent", "lock-parent");
    parent.dependencies.push_back(child.path);
    auto sibling = MakeRepo("sibling", "workspace/sibling", "lock-sibling");
    std::vector<RepoOperationInput> repos{parent, child, sibling};

    RepoOperationSchedulerOptions options;
    options.operationName = "partial-sync";
    options.mode = RepoOperationMode::MutatingDependencyWaves;
    options.jobs = 3;
    options.resolveGitCommonDirLocks = false;

    const auto aggregate = kano::git::workspace::RunRepoOperationScheduler(repos, options, [&](const RepoOperationInput& repo) {
        if (repo.id == "child") {
            RepoOperationWorkerResult out;
            out.status = RepoOperationStatus::Failed;
            out.exitCode = 23;
            out.failureCategory = "repo-local-failure";
            out.message = "child failed";
            out.stderrText = "boom\n";
            return out;
        }
        return Success(repo.id + " ok\n");
    });

    REQUIRE(aggregate.succeeded == 1);
    REQUIRE(aggregate.failed == 1);
    REQUIRE(aggregate.blocked == 1);
    REQUIRE(aggregate.hasFailure);
    REQUIRE(aggregate.results.size() == 3);

    const auto childIt = std::find_if(aggregate.results.begin(), aggregate.results.end(), [](const auto& result) { return result.repoId == "child"; });
    const auto parentIt = std::find_if(aggregate.results.begin(), aggregate.results.end(), [](const auto& result) { return result.repoId == "parent"; });
    const auto siblingIt = std::find_if(aggregate.results.begin(), aggregate.results.end(), [](const auto& result) { return result.repoId == "sibling"; });
    REQUIRE(childIt != aggregate.results.end());
    REQUIRE(parentIt != aggregate.results.end());
    REQUIRE(siblingIt != aggregate.results.end());
    REQUIRE(childIt->status == RepoOperationStatus::Failed);
    REQUIRE(parentIt->status == RepoOperationStatus::Blocked);
    REQUIRE(parentIt->blockedBy == "child");
    REQUIRE(siblingIt->status == RepoOperationStatus::Succeeded);
}

} // namespace kano::git::tests::functional
