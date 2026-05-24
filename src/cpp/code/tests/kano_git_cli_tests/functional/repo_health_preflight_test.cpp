#include "functional_test_support.hpp"

#include <catch2/catch_test_macros.hpp>

#include "repo_health.hpp"

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace kano::git::tests::functional {
namespace {

using kano::git::workspace::ParsedSubmoduleStatusLine;
using kano::git::workspace::RepoBlockerKind;
using kano::git::workspace::RepoHealthOptions;

auto RequireSuccess(const CommandResult& InResult, const std::string& InContext) -> void {
    INFO(InContext);
    INFO("exit=" << InResult.exitCode);
    INFO("stdout=" << InResult.stdoutText);
    INFO("stderr=" << InResult.stderrText);
    REQUIRE(InResult.exitCode == 0);
}

auto RequireFailure(const CommandResult& InResult, const std::string& InContext) -> void {
    INFO(InContext);
    INFO("exit=" << InResult.exitCode);
    INFO("stdout=" << InResult.stdoutText);
    INFO("stderr=" << InResult.stderrText);
    REQUIRE(InResult.exitCode != 0);
}

auto RequireContains(const std::string& InText, const std::string& InNeedle) -> void {
    INFO("missing needle=" << InNeedle);
    INFO(InText);
    REQUIRE(InText.find(InNeedle) != std::string::npos);
}

auto WriteTextFile(const std::filesystem::path& InPath, const std::string& InText) -> void {
    std::filesystem::create_directories(InPath.parent_path());
    std::ofstream out(InPath, std::ios::binary | std::ios::trunc);
    REQUIRE(out.good());
    out << InText;
}

auto ConfigureIdentity(const std::filesystem::path& InRepo) -> void {
    RequireSuccess(RunGit({"config", "user.name", "Kano Test"}, InRepo), "config user.name");
    RequireSuccess(RunGit({"config", "user.email", "kano-test@example.invalid"}, InRepo), "config user.email");
}

auto InitRepo(const std::filesystem::path& InRepo) -> void {
    std::filesystem::create_directories(InRepo);
    RequireSuccess(RunGit({"init", InRepo.string()}, InRepo.parent_path()), "init repo");
    ConfigureIdentity(InRepo);
    RequireSuccess(RunGit({"checkout", "-b", "main"}, InRepo), "checkout main");
    WriteTextFile(InRepo / "README.md", "seed\n");
    RequireSuccess(RunGit({"add", "README.md"}, InRepo), "add readme");
    RequireSuccess(RunGit({"commit", "-m", "seed"}, InRepo), "commit seed");
}

struct RemoteClone {
    SandboxContext sandbox;
    std::filesystem::path bare;
    std::filesystem::path seed;
    std::filesystem::path clone;
};

auto CreateRemoteWithClone(const std::string& InName) -> RemoteClone {
    RemoteClone out;
    out.sandbox = CreateSandboxWorkspace(InName);
    out.bare = (out.sandbox.root / "remote.git").lexically_normal();
    out.seed = (out.sandbox.root / "seed").lexically_normal();
    out.clone = (out.sandbox.root / "clone").lexically_normal();

    RequireSuccess(RunGit({"init", "--bare", out.bare.string()}, out.sandbox.root), "init bare");
    RequireSuccess(RunGit({"init", out.seed.string()}, out.sandbox.root), "init seed");
    ConfigureIdentity(out.seed);
    RequireSuccess(RunGit({"checkout", "-b", "main"}, out.seed), "seed checkout main");
    WriteTextFile(out.seed / "README.md", "seed\n");
    RequireSuccess(RunGit({"add", "README.md"}, out.seed), "seed add");
    RequireSuccess(RunGit({"commit", "-m", "seed"}, out.seed), "seed commit");
    RequireSuccess(RunGit({"remote", "add", "origin", out.bare.string()}, out.seed), "seed add remote");
    RequireSuccess(RunGit({"push", "-u", "origin", "main"}, out.seed), "seed push");
    RequireSuccess(RunGit({"symbolic-ref", "HEAD", "refs/heads/main"}, out.bare), "set bare head");
    RequireSuccess(RunGit({"clone", out.bare.string(), out.clone.string()}, out.sandbox.root), "clone");
    ConfigureIdentity(out.clone);
    return out;
}

auto HasBlocker(const kano::git::workspace::RepoHealth& InHealth, const RepoBlockerKind InKind) -> bool {
    for (const auto& blocker : InHealth.blockers) {
        if (blocker.kind == InKind) {
            return true;
        }
    }
    return false;
}

} // namespace

TEST_CASE("repo health scanner reports clean repository without blockers", "[functional][health][clean]") {
    const auto sandbox = CreateSandboxWorkspace("repo-health-clean");
    const auto repo = (sandbox.root / "repo").lexically_normal();
    InitRepo(repo);

    const auto health = kano::git::workspace::ScanRepoHealth(repo, RepoHealthOptions{});
    INFO("branch=" << health.branch);
    INFO("ahead=" << health.ahead << " behind=" << health.behind);
    REQUIRE(health.blockers.empty());

    RemoveSandboxWorkspace(sandbox);
}

TEST_CASE("repo health scanner detects detached head blocker", "[functional][health][detached]") {
    const auto sandbox = CreateSandboxWorkspace("repo-health-detached");
    const auto repo = (sandbox.root / "repo").lexically_normal();
    InitRepo(repo);
    RequireSuccess(RunGit({"checkout", "HEAD~0"}, repo), "detach head");

    const auto health = kano::git::workspace::ScanRepoHealth(repo, RepoHealthOptions{});
    REQUIRE(HasBlocker(health, RepoBlockerKind::DetachedHead));

    RemoveSandboxWorkspace(sandbox);
}

TEST_CASE("repo health scanner detects active merge and unmerged paths", "[functional][health][merge][unmerged]") {
    const auto sandbox = CreateSandboxWorkspace("repo-health-merge");
    const auto repo = (sandbox.root / "repo").lexically_normal();
    InitRepo(repo);

    RequireSuccess(RunGit({"checkout", "-b", "side"}, repo), "checkout side");
    WriteTextFile(repo / "README.md", "side\n");
    RequireSuccess(RunGit({"commit", "-am", "side"}, repo), "commit side");
    RequireSuccess(RunGit({"checkout", "main"}, repo), "checkout main");
    WriteTextFile(repo / "README.md", "main\n");
    RequireSuccess(RunGit({"commit", "-am", "main"}, repo), "commit main");
    const auto merge = RunGit({"merge", "side"}, repo);
    RequireFailure(merge, "merge should conflict");

    const auto health = kano::git::workspace::ScanRepoHealth(repo, RepoHealthOptions{});
    const bool hasActiveMerge = HasBlocker(health, RepoBlockerKind::ActiveMerge);
    const bool hasUnmerged = HasBlocker(health, RepoBlockerKind::UnmergedPaths);
    REQUIRE((hasActiveMerge || hasUnmerged));
    REQUIRE(HasBlocker(health, RepoBlockerKind::UnmergedPaths));

    RemoveSandboxWorkspace(sandbox);
}

TEST_CASE("repo health scanner detects fetch failure and branch divergence", "[functional][health][fetch][diverged]") {
    auto ctx = CreateRemoteWithClone("repo-health-fetch-diverged");
    WriteTextFile(ctx.clone / "local.txt", "local\n");
    RequireSuccess(RunGit({"add", "local.txt"}, ctx.clone), "add local");
    RequireSuccess(RunGit({"commit", "-m", "local"}, ctx.clone), "commit local");

    WriteTextFile(ctx.seed / "remote.txt", "remote\n");
    RequireSuccess(RunGit({"add", "remote.txt"}, ctx.seed), "add remote");
    RequireSuccess(RunGit({"commit", "-m", "remote"}, ctx.seed), "commit remote");
    RequireSuccess(RunGit({"push", "origin", "main"}, ctx.seed), "push remote");

    RequireSuccess(RunGit({"fetch", "origin"}, ctx.clone), "fetch origin");
    RequireSuccess(RunGit({"remote", "add", "dev-worktree", "file:///missing/path/for/fetch"}, ctx.clone), "add invalid remote");

    auto health = kano::git::workspace::ScanRepoHealth(ctx.clone, RepoHealthOptions{
        .checkFetchRemotes = true,
        .checkSubmoduleStatus = true,
        .checkGitlinkReachability = true,
        .fetchDryRun = true,
        .blockOnDetachedHead = true,
        .blockOnNoUpstream = true,
        .blockOnUnpushedCommits = true,
        .blockOnDirtyWorktree = false,
        .blockOnDirtySubmodule = false,
    });

    REQUIRE(HasBlocker(health, RepoBlockerKind::FetchFailed));
    REQUIRE(HasBlocker(health, RepoBlockerKind::BranchDiverged));

    RemoveSandboxWorkspace(ctx.sandbox);
}

TEST_CASE("repo health scanner fetches only selected remote when configured", "[functional][health][fetch][selected-remote]") {
    auto ctx = CreateRemoteWithClone("repo-health-fetch-selected-remote");
    RequireSuccess(RunGit({"remote", "add", "broken", "file:///missing/path/for/fetch"}, ctx.clone), "add invalid remote");

    auto health = kano::git::workspace::ScanRepoHealth(ctx.clone, RepoHealthOptions{
        .checkFetchRemotes = true,
        .checkSubmoduleStatus = true,
        .checkGitlinkReachability = true,
        .fetchDryRun = true,
        .fetchRemoteOnly = "origin",
        .blockOnDetachedHead = true,
        .blockOnNoUpstream = true,
        .blockOnUnpushedCommits = false,
        .blockOnDirtyWorktree = false,
        .blockOnDirtySubmodule = false,
    });

    REQUIRE_FALSE(HasBlocker(health, RepoBlockerKind::FetchFailed));
    RemoveSandboxWorkspace(ctx.sandbox);
}

TEST_CASE("submodule status parser flags U000 unresolved gitlink marker", "[functional][health][submodule][parser]") {
    const auto parsed = kano::git::workspace::ParseSubmoduleStatusLine(
        "U0000000000000000000000000000000000000000 src/cpp/shared/infra");
    REQUIRE(parsed.valid);
    REQUIRE(parsed.marker == 'U');
    REQUIRE(parsed.shaAllZero);
    REQUIRE(parsed.path == "src/cpp/shared/infra");
}

TEST_CASE("sync dry-run summary reports blockers instead of clean success", "[functional][sync][dry-run][blockers]") {
    auto ctx = CreateRemoteWithClone("sync-dry-run-blocked");
    RequireSuccess(RunGit({"checkout", "HEAD~0"}, ctx.clone), "detach clone head");
    RequireSuccess(RunGit({"remote", "add", "dev-worktree", "file:///missing/path/for/fetch"}, ctx.clone), "add invalid remote");

    const auto result = RunKog({"sync", "origin-latest", "--dry-run", "--jobs", "1"}, ctx.clone);
    const auto output = result.stdoutText + "\n" + result.stderrText;
    RequireFailure(result, "sync dry-run must fail on blockers");
    RequireContains(output, "DETACHED_HEAD");
    RequireContains(output, "FETCH_FAILED");
    RequireContains(output, "blocked=");
    REQUIRE(output.find("blocked=0") == std::string::npos);

    RemoveSandboxWorkspace(ctx.sandbox);
}

TEST_CASE("converge dry-run summary reports blockers from status preflight", "[functional][converge][dry-run][blockers]") {
    auto ctx = CreateRemoteWithClone("converge-dry-run-blocked");
    RequireSuccess(RunGit({"checkout", "HEAD~0"}, ctx.clone), "detach clone head");

    const auto result = RunKog({"converge", "--dry-run", "--jobs", "1"}, ctx.clone);
    const auto output = result.stdoutText + "\n" + result.stderrText;
    RequireFailure(result, "converge dry-run must fail on detached preflight");
    RequireContains(output, "Blocked repos");
    RequireContains(output, "DETACHED_HEAD");

    RemoveSandboxWorkspace(ctx.sandbox);
}

TEST_CASE("sync dry-run regression detects composite blockers for gate safety", "[functional][sync][dry-run][regression]") {
    auto ctx = CreateRemoteWithClone("sync-dry-run-regression");
    RequireSuccess(RunGit({"checkout", "-b", "feature"}, ctx.clone), "create feature branch");
    RequireSuccess(RunGit({"push", "-u", "origin", "feature"}, ctx.clone), "push feature branch");

    WriteTextFile(ctx.clone / "README.md", "feature local\n");
    RequireSuccess(RunGit({"commit", "-am", "feature local"}, ctx.clone), "commit feature local");
    RequireSuccess(RunGit({"checkout", "main"}, ctx.clone), "checkout main");
    WriteTextFile(ctx.clone / "README.md", "main local\n");
    RequireSuccess(RunGit({"commit", "-am", "main local"}, ctx.clone), "commit main local");
    RequireSuccess(RunGit({"checkout", "feature"}, ctx.clone), "checkout feature again");
    const auto rebase = RunGit({"rebase", "main"}, ctx.clone);
    RequireFailure(rebase, "rebase conflict should be active");

    RequireSuccess(RunGit({"remote", "add", "dev-worktree", "file:///missing/path/for/fetch"}, ctx.clone), "add invalid remote");

    const auto result = RunKogWithEnv(
        {"sync", "origin-latest", "--dry-run", "--jobs", "1"},
        ctx.clone,
        {
            {"KOG_TEST_HEALTH_SUBMODULE_STATUS", "U0000000000000000000000000000000000000000 src/cpp/shared/infra"},
        });
    const auto output = result.stdoutText + "\n" + result.stderrText;
    RequireFailure(result, "sync dry-run composite blockers");
    RequireContains(output, "ACTIVE_REBASE");
    RequireContains(output, "UNRESOLVED_GITLINK");
    RequireContains(output, "FETCH_FAILED");
    REQUIRE(output.find("blocked=0") == std::string::npos);

    RemoveSandboxWorkspace(ctx.sandbox);
}

} // namespace kano::git::tests::functional
