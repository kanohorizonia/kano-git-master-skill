#include "functional_test_support.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace kano::git::tests::functional {
namespace {

struct RemoteCloneContext {
    SandboxContext sandbox;
    std::filesystem::path bareRemote;
    std::filesystem::path seedRepo;
    std::filesystem::path cloneRepo;
    std::string branch;
};

struct NestedWorkspaceContext {
    SandboxContext sandbox;
    std::filesystem::path nestedBareRemote;
    std::filesystem::path nestedSeedRepo;
    std::filesystem::path rootBareRemote;
    std::filesystem::path rootSeedRepo;
    std::filesystem::path cloneRootRepo;
    std::filesystem::path cloneNestedRepo;
    std::string branch;
    std::string nestedRepoPath;
};

auto StripAnsi(const std::string& InText) -> std::string {
    std::string out;
    out.reserve(InText.size());
    bool inEsc = false;
    for (const char ch : InText) {
        if (!inEsc) {
            if (ch == '\x1b') {
                inEsc = true;
                continue;
            }
            out.push_back(ch);
            continue;
        }
        if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z')) {
            inEsc = false;
        }
    }
    return out;
}

auto RequireSuccess(const CommandResult& InResult, const std::string& InContext) -> void {
    INFO(InContext);
    INFO("exit=" << InResult.exitCode);
    INFO("stdout=" << InResult.stdoutText);
    INFO("stderr=" << InResult.stderrText);
    REQUIRE(InResult.exitCode == 0);
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

auto CurrentHeadSha(const std::filesystem::path& InRepo) -> std::string {
    const auto result = RunGit({"rev-parse", "HEAD"}, InRepo);
    RequireSuccess(result, "rev-parse HEAD");
    std::istringstream iss(result.stdoutText);
    std::string sha;
    iss >> sha;
    return sha;
}

auto CurrentBranch(const std::filesystem::path& InRepo) -> std::string {
    const auto result = RunGit({"symbolic-ref", "--quiet", "--short", "HEAD"}, InRepo);
    if (result.exitCode != 0) {
        return {};
    }
    std::istringstream iss(result.stdoutText);
    std::string branch;
    iss >> branch;
    return branch;
}

auto StatusPorcelain(const std::filesystem::path& InRepo) -> std::string {
    const auto result = RunGit({"status", "--porcelain"}, InRepo);
    RequireSuccess(result, "git status --porcelain");
    return result.stdoutText;
}

auto InitPlainGitRepo(const std::filesystem::path& InRepo) -> void {
    std::filesystem::create_directories(InRepo);
    RequireSuccess(RunGit({"init", InRepo.string()}, InRepo.parent_path()), "init plain git repo");
    ConfigureIdentity(InRepo);
    WriteTextFile(InRepo / "README.md", "repo\n");
    RequireSuccess(RunGit({"add", "README.md"}, InRepo), "add plain repo readme");
    RequireSuccess(RunGit({"commit", "-m", "seed repo"}, InRepo), "commit plain repo readme");
}

auto CreateRemoteWithClone(const std::string& InName, const std::string& InBranch = "main") -> RemoteCloneContext {
    RemoteCloneContext ctx;
    ctx.sandbox = CreateSandboxWorkspace(InName);
    ctx.bareRemote = (ctx.sandbox.root / "remote.git").lexically_normal();
    ctx.seedRepo = (ctx.sandbox.root / "seed").lexically_normal();
    ctx.cloneRepo = (ctx.sandbox.root / "clone").lexically_normal();
    ctx.branch = InBranch;

    RequireSuccess(RunGit({"init", "--bare", ctx.bareRemote.string()}, ctx.sandbox.root), "init bare remote");
    RequireSuccess(RunGit({"init", ctx.seedRepo.string()}, ctx.sandbox.root), "init seed repo");
    ConfigureIdentity(ctx.seedRepo);
    RequireSuccess(RunGit({"checkout", "-b", ctx.branch}, ctx.seedRepo), "checkout seed branch");
    WriteTextFile(ctx.seedRepo / "README.md", "seed\n");
    RequireSuccess(RunGit({"add", "README.md"}, ctx.seedRepo), "seed add");
    RequireSuccess(RunGit({"commit", "-m", "seed commit"}, ctx.seedRepo), "seed commit");
    RequireSuccess(RunGit({"remote", "add", "origin", ctx.bareRemote.string()}, ctx.seedRepo), "seed add remote");
    RequireSuccess(RunGit({"push", "-u", "origin", ctx.branch}, ctx.seedRepo), "seed push");
    RequireSuccess(RunGit({"symbolic-ref", "HEAD", ("refs/heads/" + ctx.branch)}, ctx.bareRemote), "set bare HEAD");
    RequireSuccess(RunGit({"clone", ctx.bareRemote.string(), ctx.cloneRepo.string()}, ctx.sandbox.root), "clone repo");
    ConfigureIdentity(ctx.cloneRepo);
    return ctx;
}

auto CreateRemoteWithNestedRepoClone(const std::string& InName, const std::string& InBranch = "main") -> NestedWorkspaceContext {
    NestedWorkspaceContext ctx;
    ctx.sandbox = CreateSandboxWorkspace(InName);
    ctx.nestedBareRemote = (ctx.sandbox.root / "nested-remote.git").lexically_normal();
    ctx.nestedSeedRepo = (ctx.sandbox.root / "nested-seed").lexically_normal();
    ctx.rootBareRemote = (ctx.sandbox.root / "root-remote.git").lexically_normal();
    ctx.rootSeedRepo = (ctx.sandbox.root / "root-seed").lexically_normal();
    ctx.cloneRootRepo = (ctx.sandbox.root / "root-clone").lexically_normal();
    ctx.branch = InBranch;
    ctx.nestedRepoPath = "nested/tool";

    RequireSuccess(RunGit({"init", "--bare", ctx.nestedBareRemote.string()}, ctx.sandbox.root), "init nested bare");
    RequireSuccess(RunGit({"init", ctx.nestedSeedRepo.string()}, ctx.sandbox.root), "init nested seed");
    ConfigureIdentity(ctx.nestedSeedRepo);
    RequireSuccess(RunGit({"checkout", "-b", ctx.branch}, ctx.nestedSeedRepo), "checkout nested branch");
    WriteTextFile(ctx.nestedSeedRepo / "nested.txt", "nested seed\n");
    RequireSuccess(RunGit({"add", "nested.txt"}, ctx.nestedSeedRepo), "nested add");
    RequireSuccess(RunGit({"commit", "-m", "nested seed"}, ctx.nestedSeedRepo), "nested commit");
    RequireSuccess(RunGit({"remote", "add", "origin", ctx.nestedBareRemote.string()}, ctx.nestedSeedRepo), "nested add remote");
    RequireSuccess(RunGit({"push", "-u", "origin", ctx.branch}, ctx.nestedSeedRepo), "nested push");
    RequireSuccess(RunGit({"symbolic-ref", "HEAD", ("refs/heads/" + ctx.branch)}, ctx.nestedBareRemote), "nested bare HEAD");

    RequireSuccess(RunGit({"init", "--bare", ctx.rootBareRemote.string()}, ctx.sandbox.root), "init root bare");
    RequireSuccess(RunGit({"init", ctx.rootSeedRepo.string()}, ctx.sandbox.root), "init root seed");
    ConfigureIdentity(ctx.rootSeedRepo);
    RequireSuccess(RunGit({"checkout", "-b", ctx.branch}, ctx.rootSeedRepo), "checkout root branch");
    WriteTextFile(ctx.rootSeedRepo / "README.md", "root seed\n");
    RequireSuccess(RunGit({"add", "README.md"}, ctx.rootSeedRepo), "root add");
    RequireSuccess(RunGit({"commit", "-m", "root seed"}, ctx.rootSeedRepo), "root commit");
    RequireSuccess(RunGit({"remote", "add", "origin", ctx.rootBareRemote.string()}, ctx.rootSeedRepo), "root add remote");
    RequireSuccess(RunGit({"push", "-u", "origin", ctx.branch}, ctx.rootSeedRepo), "root push");
    RequireSuccess(RunGit({"symbolic-ref", "HEAD", ("refs/heads/" + ctx.branch)}, ctx.rootBareRemote), "root bare HEAD");

    RequireSuccess(RunGit({"clone", ctx.rootBareRemote.string(), ctx.cloneRootRepo.string()}, ctx.sandbox.root), "clone root");
    ConfigureIdentity(ctx.cloneRootRepo);

    std::filesystem::create_directories((ctx.cloneRootRepo / std::filesystem::path(ctx.nestedRepoPath)).parent_path());
    RequireSuccess(
        RunGit({"clone", ctx.nestedBareRemote.string(), (ctx.cloneRootRepo / std::filesystem::path(ctx.nestedRepoPath)).string()}, ctx.sandbox.root),
        "clone nested repo");
    ctx.cloneNestedRepo = (ctx.cloneRootRepo / std::filesystem::path(ctx.nestedRepoPath)).lexically_normal();
    ConfigureIdentity(ctx.cloneNestedRepo);
    return ctx;
}

} // namespace

TEST_CASE("fetch help is registered", "[functional][fetch][help]") {
    const auto ctx = CreateRemoteWithClone("fetch-help");
    const auto result = RunKog({"fetch", "--help"}, ctx.cloneRepo);
    const auto merged = StripAnsi(result.stdoutText + "\n" + result.stderrText);
    INFO(merged);
    REQUIRE(result.exitCode == 0);
    REQUIRE(merged.find("Recursively fetch discovered repositories in parallel") != std::string::npos);
    RemoveSandboxWorkspace(ctx.sandbox);
}

TEST_CASE("fetch dry-run lists root and nested repo", "[functional][fetch][dry-run]") {
    const auto ctx = CreateRemoteWithNestedRepoClone("fetch-dry-run");
    const auto result = RunKog(
        {"fetch", "--repo-root", ctx.cloneRootRepo.string(), "--max-depth", "8", "--no-cache", "--dry-run"},
        ctx.cloneRootRepo);
    const auto merged = StripAnsi(result.stdoutText + "\n" + result.stderrText);
    INFO(merged);
    REQUIRE(result.exitCode == 0);
    REQUIRE(merged.find("[DRY RUN] git fetch --all --prune --tags") != std::string::npos);
    REQUIRE(merged.find("nested/tool") != std::string::npos);
    RemoveSandboxWorkspace(ctx.sandbox);
}

TEST_CASE("fetch repo without remotes is skip", "[functional][fetch][no-remotes]") {
    const auto sandbox = CreateSandboxWorkspace("fetch-no-remotes");
    const auto repo = (sandbox.root / "repo").lexically_normal();
    InitPlainGitRepo(repo);

    const auto result = RunKog({"fetch", "--repo-root", repo.string(), "--no-recursive", "--no-cache"}, repo);
    const auto merged = StripAnsi(result.stdoutText + "\n" + result.stderrText);
    INFO(merged);
    REQUIRE(result.exitCode == 0);
    REQUIRE(merged.find("SKIP") != std::string::npos);
    REQUIRE(merged.find("no-remotes") != std::string::npos);

    RemoveSandboxWorkspace(sandbox);
}

TEST_CASE("fetch missing explicit remote fails", "[functional][fetch][remote]") {
    const auto ctx = CreateRemoteWithClone("fetch-missing-remote");
    const auto result = RunKog(
        {"fetch", "--repo-root", ctx.cloneRepo.string(), "--no-recursive", "--remote", "missing"},
        ctx.cloneRepo);
    const auto merged = StripAnsi(result.stdoutText + "\n" + result.stderrText);
    INFO(merged);
    REQUIRE(result.exitCode != 0);
    REQUIRE(merged.find("requested remote not found: missing") != std::string::npos);
    RemoveSandboxWorkspace(ctx.sandbox);
}

TEST_CASE("fetch jobs validation rejects zero", "[functional][fetch][jobs]") {
    const auto ctx = CreateRemoteWithClone("fetch-jobs-zero");
    const auto result = RunKog({"fetch", "--jobs", "0"}, ctx.cloneRepo);
    const auto merged = StripAnsi(result.stdoutText + "\n" + result.stderrText);
    INFO(merged);
    REQUIRE(result.exitCode != 0);
    REQUIRE(merged.find("--jobs must be 'auto' or an integer >= 1") != std::string::npos);
    RemoveSandboxWorkspace(ctx.sandbox);
}

TEST_CASE("fetch dry-run does not mutate worktree", "[functional][fetch][dry-run]") {
    const auto ctx = CreateRemoteWithClone("fetch-dry-no-mutate");
    WriteTextFile(ctx.cloneRepo / "local.txt", "uncommitted change\n");

    const auto branchBefore = CurrentBranch(ctx.cloneRepo);
    const auto headBefore = CurrentHeadSha(ctx.cloneRepo);
    const auto statusBefore = StatusPorcelain(ctx.cloneRepo);

    const auto result = RunKog(
        {"fetch", "--repo-root", ctx.cloneRepo.string(), "--no-recursive", "--dry-run"},
        ctx.cloneRepo);
    INFO(result.stdoutText);
    INFO(result.stderrText);
    REQUIRE(result.exitCode == 0);

    REQUIRE(CurrentBranch(ctx.cloneRepo) == branchBefore);
    REQUIRE(CurrentHeadSha(ctx.cloneRepo) == headBefore);
    REQUIRE(StatusPorcelain(ctx.cloneRepo) == statusBefore);
    RemoveSandboxWorkspace(ctx.sandbox);
}

} // namespace kano::git::tests::functional
