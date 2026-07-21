#include "functional_test_support.hpp"

#include <catch2/catch_test_macros.hpp>

#include "repo_health.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <unordered_set>
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

auto RequireNotContains(const std::string& InText, const std::string& InNeedle) -> void {
    INFO("unexpected needle=" << InNeedle);
    INFO(InText);
    REQUIRE(InText.find(InNeedle) == std::string::npos);
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

auto IsolatedGitConfigEnv(const SandboxContext& InSandbox) -> std::vector<std::pair<std::string, std::string>> {
    return {
        {"GIT_CONFIG_NOSYSTEM", "1"},
        {"GIT_CONFIG_GLOBAL", (InSandbox.root / "global.gitconfig").string()},
        {"KOG_PROCESS_DIAGNOSTICS", "0"},
    };
}

auto AddGitmodulesEntry(const std::filesystem::path& InRepo, const std::string& InName, const std::filesystem::path& InRelativePath) -> void {
    WriteTextFile(
        InRepo / ".gitmodules",
        "[submodule \"" + InName + "\"]\n"
        "\tpath = " + InRelativePath.generic_string() + "\n"
        "\turl = ../" + InName + "\n");
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

TEST_CASE("repo health ignores KOG internal artifacts but retains user dirtiness", "[functional][health][dirty][internal-artifacts]") {
    const auto sandbox = CreateSandboxWorkspace("repo-health-internal-artifacts");
    const auto repo = (sandbox.root / "repo").lexically_normal();
    InitRepo(repo);
    WriteTextFile(repo / ".kano" / "kog_config.toml", "[sync]\nauto_stash = false\n");
    RequireSuccess(RunGit({"add", ".kano/kog_config.toml"}, repo), "add tracked KOG config");
    RequireSuccess(RunGit({"commit", "-m", "add tracked KOG config"}, repo), "commit tracked KOG config");
    WriteTextFile(repo / ".kano" / "cache" / "git" / "workspace-manifest.json", "{}\n");
    WriteTextFile(repo / ".sisyphus" / "tmp" / "state.json", "{}\n");

    RepoHealthOptions options;
    options.checkSubmoduleStatus = false;
    options.checkGitlinkReachability = false;
    options.blockOnDetachedHead = false;
    const auto internalOnly = kano::git::workspace::ScanRepoHealth(repo, options);
    REQUIRE_FALSE(internalOnly.hasDirtyWorktree);

    WriteTextFile(repo / ".kano" / "kog_config.toml", "[sync]\nauto_stash = true\n");
    const auto withTrackedConfigChange = kano::git::workspace::ScanRepoHealth(repo, options);
    REQUIRE(withTrackedConfigChange.hasDirtyWorktree);

    WriteTextFile(repo / "user-change.txt", "user work\n");
    const auto withUserChange = kano::git::workspace::ScanRepoHealth(repo, options);
    REQUIRE(withUserChange.hasDirtyWorktree);

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

TEST_CASE("repo health skips unregistered gitlink when submodule mapping is missing", "[functional][health][submodule][unregistered]") {
    const auto sandbox = CreateSandboxWorkspace("repo-health-unregistered-gitlink-skip");
    const auto root = (sandbox.root / "root").lexically_normal();
    const auto child = (root / "HorizonDialogueDemo").lexically_normal();
    InitRepo(root);
    InitRepo(child);

    RequireSuccess(RunGit({"add", "HorizonDialogueDemo"}, root), "stage unregistered gitlink");
    RequireSuccess(RunGit({"commit", "-m", "add unregistered gitlink"}, root), "commit unregistered gitlink");

    WriteTextFile(child / "README.md", "seed\nchild moved\n");
    RequireSuccess(RunGit({"commit", "-am", "child moved"}, child), "advance child head");

    const auto health = kano::git::workspace::ScanRepoHealth(root, RepoHealthOptions{
        .checkFetchRemotes = false,
        .checkSubmoduleStatus = true,
        .checkGitlinkReachability = false,
        .fetchDryRun = true,
        .blockOnDetachedHead = false,
        .blockOnNoUpstream = false,
        .blockOnUnpushedCommits = false,
        .blockOnDirtyWorktree = false,
        .blockOnDirtySubmodule = false,
        .strictSubmoduleMappings = false,
        .managedSubmodulePaths = {},
    });

    REQUIRE_FALSE(HasBlocker(health, RepoBlockerKind::KogPlanUnauditable));
    REQUIRE_FALSE(HasBlocker(health, RepoBlockerKind::SubmoduleMappingMissing));
    REQUIRE(std::find(health.statusFlags.begin(), health.statusFlags.end(), "UNREGISTERED_GITLINK_SKIPPED") != health.statusFlags.end());

    RemoveSandboxWorkspace(sandbox);
}

TEST_CASE("repo health blocks managed submodule when mapping is missing", "[functional][health][submodule][managed-missing-mapping]") {
    const auto sandbox = CreateSandboxWorkspace("repo-health-managed-gitlink-missing-mapping");
    const auto root = (sandbox.root / "root").lexically_normal();
    const auto child = (root / "HorizonDialogueDemo").lexically_normal();
    InitRepo(root);
    InitRepo(child);

    RequireSuccess(RunGit({"add", "HorizonDialogueDemo"}, root), "stage managed gitlink fixture");
    RequireSuccess(RunGit({"commit", "-m", "add managed gitlink fixture"}, root), "commit managed gitlink fixture");

    WriteTextFile(child / "README.md", "seed\nchild moved\n");
    RequireSuccess(RunGit({"commit", "-am", "child moved"}, child), "advance child head");

    std::unordered_set<std::string> managedPaths;
    managedPaths.insert("HorizonDialogueDemo");
    const auto health = kano::git::workspace::ScanRepoHealth(root, RepoHealthOptions{
        .checkFetchRemotes = false,
        .checkSubmoduleStatus = true,
        .checkGitlinkReachability = false,
        .fetchDryRun = true,
        .blockOnDetachedHead = false,
        .blockOnNoUpstream = false,
        .blockOnUnpushedCommits = false,
        .blockOnDirtyWorktree = false,
        .blockOnDirtySubmodule = false,
        .strictSubmoduleMappings = false,
        .managedSubmodulePaths = managedPaths,
    });

    REQUIRE(HasBlocker(health, RepoBlockerKind::SubmoduleMappingMissing));

    RemoveSandboxWorkspace(sandbox);
}

TEST_CASE("sync dry-run summary reports blockers instead of clean success", "[functional][sync][dry-run][blockers]") {
    auto ctx = CreateRemoteWithClone("sync-dry-run-blocked");
    RequireSuccess(RunGit({"checkout", "HEAD~0"}, ctx.clone), "detach clone head");
    RequireSuccess(RunGit({"remote", "set-url", "origin", "file:///missing/path/for/fetch"}, ctx.clone), "invalidate selected remote");

    const auto result = RunKogWithEnv(
        {"sync", "origin-latest", "--dry-run", "--jobs", "1"},
        ctx.clone,
        {{"KOG_PROCESS_DIAGNOSTICS_LOG", (ctx.sandbox.root / "sync-dry-run-blocked-process-diag.log").string()}});
    const auto output = result.stdoutText + "\n" + result.stderrText;
    RequireFailure(result, "sync dry-run must fail on blockers");
    RequireContains(output, "DETACHED_HEAD");
    RequireContains(output, "FETCH_FAILED");
    RequireContains(output, "blocked=");
    REQUIRE(output.find("blocked=0") == std::string::npos);

    RemoveSandboxWorkspace(ctx.sandbox);
}

TEST_CASE("doctor fix-safe-directory repairs Git dubious ownership blockers", "[functional][doctor][safe-directory][fix]") {
    const auto sandbox = CreateSandboxWorkspace("doctor-safe-directory-fix");
    const auto repo = (sandbox.root / "repo").lexically_normal();
    const auto child = (repo / "child").lexically_normal();
    const auto loose = (repo / "loose").lexically_normal();
    InitRepo(repo);
    InitRepo(child);
    InitRepo(loose);
    AddGitmodulesEntry(repo, "child", std::filesystem::path("child"));
    RequireSuccess(RunGit({"add", ".gitmodules"}, repo), "add gitmodules");
    RequireSuccess(RunGit({"commit", "-m", "register child"}, repo), "commit gitmodules");

    auto env = IsolatedGitConfigEnv(sandbox);
    for (auto& pair : env) {
        if (pair.first == "GIT_CONFIG_GLOBAL") {
            pair.second = (sandbox.root / "missing-global-parent" / "global.gitconfig").string();
        }
    }
    env.push_back({"GIT_TEST_ASSUME_DIFFERENT_OWNER", "1"});

    const auto before = RunKogWithEnv(
        {"status", "--recursive", "--json", "--repo-root", repo.string(), "--no-unregistered-scan", "--no-fetch-health"},
        repo,
        env);
    const auto beforeOutput = before.stdoutText + "\n" + before.stderrText;
    RequireSuccess(before, "status should report unsafe ownership without crashing");
    RequireContains(beforeOutput, "UNSAFE_OWNERSHIP");
    RequireContains(beforeOutput, "kog doctor --fix-safe-directory");

    const auto doctor = RunKogWithEnv({"doctor", "--repo", repo.string(), "--safe-directory", "--fix-safe-directory"}, repo, env);
    const auto doctorOutput = doctor.stdoutText + "\n" + doctor.stderrText;
    RequireSuccess(doctor, "doctor --fix-safe-directory should repair test-owned repos");
    RequireContains(doctorOutput, "fixed safe.directory");
    RequireContains(doctorOutput, "workspace safe.directory config");
    RequireContains(doctorOutput, "loose");

    const auto after = RunKogWithEnv(
        {"status", "--recursive", "--json", "--repo-root", repo.string(), "--no-unregistered-scan", "--no-fetch-health"},
        repo,
        env);
    const auto afterOutput = after.stdoutText + "\n" + after.stderrText;
    RequireSuccess(after, "status should succeed after safe.directory repair");
    RequireNotContains(afterOutput, "UNSAFE_OWNERSHIP");

    RemoveSandboxWorkspace(sandbox);
}

TEST_CASE("auth doctor redacts credentials in explicit URLs", "[functional][auth][doctor][redaction]") {
    const auto sandbox = CreateSandboxWorkspace("auth-doctor-redaction");
    const auto repo = (sandbox.root / "repo").lexically_normal();
    InitRepo(repo);
    RequireSuccess(RunGit({"config", "--local", "--add", "credential.helper", "manager"}, repo), "config local GCM helper");

    const auto result = RunKogWithEnv({
        "auth",
        "doctor",
        "--repo",
        repo.string(),
        "--url",
        "https://user:super-secret@example.invalid/org/repo.git",
    }, repo, IsolatedGitConfigEnv(sandbox));
    const auto output = result.stdoutText + "\n" + result.stderrText;
    RequireSuccess(result, "auth doctor explicit url should succeed");
    RequireContains(output, "<redacted>@example.invalid");
    RequireNotContains(output, "super-secret");

    RemoveSandboxWorkspace(sandbox);
}

TEST_CASE("auth doctor fails when stale manager-core helper is configured", "[functional][auth][doctor][gcm][manager-core]") {
    const auto sandbox = CreateSandboxWorkspace("auth-doctor-manager-core");
    const auto repo = (sandbox.root / "repo").lexically_normal();
    InitRepo(repo);
    RequireSuccess(RunGit({"remote", "add", "origin", "https://example.invalid/org/repo.git"}, repo), "add https remote");
    RequireSuccess(RunGit({"config", "--local", "--add", "credential.helper", "manager"}, repo), "config valid GCM helper");
    RequireSuccess(RunGit({"config", "--local", "--add", "credential.helper", "manager-core"}, repo), "config stale GCM helper");

    const auto result = RunKogWithEnv({"auth", "doctor", "--repo", repo.string()}, repo, IsolatedGitConfigEnv(sandbox));
    const auto output = result.stdoutText + "\n" + result.stderrText;
    RequireFailure(result, "auth doctor should fail on stale manager-core");
    RequireContains(output, "stale credential.helper=manager-core");
    RequireContains(output, "kog auth doctor --fix");

    RemoveSandboxWorkspace(sandbox);
}

TEST_CASE("auth doctor fix removes stale manager-core helper", "[functional][auth][doctor][gcm][fix]") {
    const auto sandbox = CreateSandboxWorkspace("auth-doctor-manager-core-fix");
    const auto repo = (sandbox.root / "repo").lexically_normal();
    InitRepo(repo);
    RequireSuccess(RunGit({"remote", "add", "origin", "https://example.invalid/org/repo.git"}, repo), "add https remote");
    RequireSuccess(RunGit({"config", "--local", "--add", "credential.helper", "manager"}, repo), "config valid GCM helper");
    RequireSuccess(RunGit({"config", "--local", "--add", "credential.helper", "manager-core"}, repo), "config stale GCM helper");

    const auto result = RunKogWithEnv({"auth", "doctor", "--repo", repo.string(), "--fix"}, repo, IsolatedGitConfigEnv(sandbox));
    const auto output = result.stdoutText + "\n" + result.stderrText;
    RequireSuccess(result, "auth doctor --fix should remove stale manager-core");
    RequireContains(output, "removing stale credential.helper=manager-core");

    const auto helpers = RunGit({"config", "--local", "--get-all", "credential.helper"}, repo);
    RequireSuccess(helpers, "read local credential helpers");
    RequireContains(helpers.stdoutText, "manager");
    RequireNotContains(helpers.stdoutText, "manager-core");

    RemoveSandboxWorkspace(sandbox);
}

TEST_CASE("auth test selected-remotes ignores non-selected broken remotes", "[functional][auth][test][selected-remotes]") {
    auto ctx = CreateRemoteWithClone("auth-test-selected-remotes");
    RequireSuccess(RunGit({"remote", "add", "broken", "file:///missing/path/for/fetch"}, ctx.clone), "add invalid remote");

    const auto result = RunKog({"auth", "test", "--repo", ctx.clone.string(), "--selected-remotes"}, ctx.clone);
    const auto output = result.stdoutText + "\n" + result.stderrText;
    RequireSuccess(result, "auth test selected-remotes should only inspect the selected origin remote");
    RequireContains(output, "AUTH_TEST_SKIPPED");
    RequireContains(output, "failed=0");

    RemoveSandboxWorkspace(ctx.sandbox);
}

TEST_CASE("sync dry-run blocks early on HTTP auth preflight failures", "[functional][sync][auth-preflight][connection]") {
    const auto sandbox = CreateSandboxWorkspace("sync-auth-preflight-http");
    const auto repo = (sandbox.root / "repo").lexically_normal();
    InitRepo(repo);
    RequireSuccess(RunGit({"remote", "add", "origin", "http://127.0.0.1:1/repo.git"}, repo), "add http origin");

    const auto result = RunKog({"sync", "origin-latest", "--dry-run", "--jobs", "1", "--no-recursive"}, repo);
    const auto output = result.stdoutText + "\n" + result.stderrText;
    RequireFailure(result, "sync dry-run should fail before fetch when auth preflight cannot reach selected HTTP remote");
    RequireContains(output, "AUTH_TEST_FAILED");
    RequireContains(output, "FAILED_CONNECTION");

    RemoveSandboxWorkspace(sandbox);
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

    RequireSuccess(RunGit({"remote", "set-url", "origin", "file:///missing/path/for/fetch"}, ctx.clone), "invalidate selected remote");

    const auto result = RunKogWithEnv(
        {"sync", "origin-latest", "--dry-run", "--jobs", "1"},
        ctx.clone,
        {
            {"KOG_TEST_HEALTH_SUBMODULE_STATUS", "U0000000000000000000000000000000000000000 src/cpp/shared/infra"},
            {"KOG_PROCESS_DIAGNOSTICS_LOG", (ctx.sandbox.root / "sync-dry-run-regression-process-diag.log").string()},
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
