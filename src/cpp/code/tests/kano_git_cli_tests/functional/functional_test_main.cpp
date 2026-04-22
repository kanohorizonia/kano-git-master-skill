#include "functional_test_support.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <chrono>
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

struct SubmoduleWorkspaceContext {
    SandboxContext sandbox;
    std::filesystem::path childBareRemote;
    std::filesystem::path childSeedRepo;
    std::filesystem::path rootBareRemote;
    std::filesystem::path rootSeedRepo;
    std::filesystem::path cloneRootRepo;
    std::filesystem::path cloneChildRepo;
    std::string branch;
    std::string submodulePath;
};

struct SubmoduleBranchUpgradeContext {
    SandboxContext sandbox;
    std::filesystem::path childBareRemote;
    std::filesystem::path childSeedRepo;
    std::filesystem::path rootBareRemote;
    std::filesystem::path rootSeedRepo;
    std::filesystem::path cloneRootRepo;
    std::filesystem::path cloneChildRepo;
    std::string rootBranch;
    std::string initialChildBranch;
    std::string upgradedChildBranch;
    std::string submodulePath;
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

struct StandaloneBareRemoteContext {
    std::filesystem::path bareRemote;
    std::filesystem::path seedRepo;
    std::string branch;
};

auto RequireSuccess(const CommandResult& InResult, const std::string& InContext) -> void {
    INFO(InContext);
    INFO("exit=" << InResult.exitCode);
    INFO("stdout=" << InResult.stdoutText);
    INFO("stderr=" << InResult.stderrText);
    REQUIRE(InResult.exitCode == 0);
}

auto TrimCopy(const std::string& InValue) -> std::string {
    const auto start = InValue.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) {
        return {};
    }
    const auto end = InValue.find_last_not_of(" \t\r\n");
    return InValue.substr(start, end - start + 1);
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

auto SeedSelfBuildScaffolding(const RemoteCloneContext& InCtx) -> void {
    WriteTextFile(InCtx.seedRepo / "scripts/kano-git", "#!/usr/bin/env bash\nset -euo pipefail\n");

    const std::string buildScript = "#!/usr/bin/env bash\nset -euo pipefail\nprintf 'built\\n' > .kano-self-build-ran\n";
    WriteTextFile(InCtx.seedRepo / "src/cpp/scripts/windows/ninja-msvc-release.sh", buildScript);
    WriteTextFile(InCtx.seedRepo / "src/cpp/scripts/windows/ninja-msvc-arm64-release.sh", buildScript);
    WriteTextFile(InCtx.seedRepo / "src/cpp/scripts/linux/ninja-gcc-release.sh", buildScript);
    WriteTextFile(InCtx.seedRepo / "src/cpp/scripts/macos/ninja-clang-x64-release.sh", buildScript);
    WriteTextFile(InCtx.seedRepo / "src/cpp/scripts/macos/ninja-clang-arm64-release.sh", buildScript);

    RequireSuccess(RunGit({"add", "scripts", "src/cpp"}, InCtx.seedRepo), "seed self-build scaffolding add");
    RequireSuccess(RunGit({"commit", "-m", "seed self-build scaffolding"}, InCtx.seedRepo), "seed self-build scaffolding commit");
    RequireSuccess(RunGit({"push", "origin", InCtx.branch}, InCtx.seedRepo), "seed self-build scaffolding push");
    RequireSuccess(RunGit({"pull", "--rebase", "origin", InCtx.branch}, InCtx.cloneRepo), "clone sync self-build scaffolding");

    std::error_code ec;
    std::filesystem::remove((InCtx.cloneRepo / ".kano-self-build-ran").lexically_normal(), ec);
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

auto AheadBehindCounts(const std::filesystem::path& InRepo) -> std::pair<int, int> {
    const auto result = RunGit({"rev-list", "--left-right", "--count", "@{upstream}...HEAD"}, InRepo);
    RequireSuccess(result, "rev-list ahead/behind");
    std::istringstream iss(result.stdoutText);
    int behind = 0;
    int ahead = 0;
    iss >> behind >> ahead;
    return {behind, ahead};
}

auto StatusPorcelain(const std::filesystem::path& InRepo) -> std::string {
    const auto result = RunGit({"status", "--porcelain"}, InRepo);
    RequireSuccess(result, "git status --porcelain");
    return TrimCopy(result.stdoutText);
}

auto GitlinkHeadSha(const std::filesystem::path& InRepo, const std::string& InPath) -> std::string {
    const auto result = RunGit({"ls-tree", "HEAD", "--", InPath}, InRepo);
    RequireSuccess(result, "ls-tree gitlink");
    std::istringstream iss(result.stdoutText);
    std::string mode;
    std::string type;
    std::string sha;
    iss >> mode >> type >> sha;
    return sha;
}

auto RefSha(const std::filesystem::path& InRepo, const std::string& InRef) -> std::string {
    const auto result = RunGit({"rev-parse", InRef}, InRepo);
    RequireSuccess(result, "rev-parse ref");
    std::istringstream iss(result.stdoutText);
    std::string sha;
    iss >> sha;
    return sha;
}

auto ReadTextFile(const std::filesystem::path& InPath) -> std::string {
    std::ifstream in(InPath, std::ios::binary);
    REQUIRE(in.good());
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

auto LongestLineLength(const std::string& InText) -> std::size_t {
    std::istringstream iss(InText);
    std::string line;
    std::size_t longest = 0;
    while (std::getline(iss, line)) {
        longest = std::max(longest, line.size());
    }
    return longest;
}

auto TouchFile(const std::filesystem::path& InPath) -> void {
    std::filesystem::create_directories(InPath.parent_path());
    std::ofstream out(InPath, std::ios::binary | std::ios::trunc);
    REQUIRE(out.good());
}

auto InitPlainGitRepo(const std::filesystem::path& InRepo) -> void {
    std::filesystem::create_directories(InRepo);
    RequireSuccess(RunGit({"init", InRepo.string()}, InRepo.parent_path()), "init plain git repo");
    ConfigureIdentity(InRepo);
    WriteTextFile(InRepo / "README.md", "repo\n");
    RequireSuccess(RunGit({"add", "README.md"}, InRepo), "add plain repo readme");
    RequireSuccess(RunGit({"commit", "-m", "seed repo"}, InRepo), "commit plain repo readme");
}

auto ConfigureFileProtocolAlways(const std::filesystem::path& InRepo) -> void {
    RequireSuccess(RunGit({"config", "protocol.file.allow", "always"}, InRepo), "config protocol.file.allow always");
}

auto RunKogAllowingFileProtocol(const std::vector<std::string>& InArgs,
                                const std::filesystem::path& InWorkingDir) -> CommandResult {
    return RunKogWithEnv(
        InArgs,
        InWorkingDir,
        {
            {"GIT_ALLOW_PROTOCOL", "file:https:ssh:git"}
        });
}

auto CreateStandaloneBareRemote(const SandboxContext& InSandbox,
                                const std::string& InName,
                                const std::string& InBranch,
                                const bool InSeedCommit) -> StandaloneBareRemoteContext {
    StandaloneBareRemoteContext ctx;
    ctx.bareRemote = (InSandbox.root / (InName + "-remote.git")).lexically_normal();
    ctx.seedRepo = (InSandbox.root / (InName + "-seed")).lexically_normal();
    ctx.branch = InBranch;

    RequireSuccess(RunGit({"init", "--bare", ctx.bareRemote.string()}, InSandbox.root), "init standalone bare remote");
    if (!InSeedCommit) {
        return ctx;
    }

    RequireSuccess(RunGit({"init", ctx.seedRepo.string()}, InSandbox.root), "init standalone seed repo");
    ConfigureIdentity(ctx.seedRepo);
    RequireSuccess(RunGit({"checkout", "-b", ctx.branch}, ctx.seedRepo), "checkout standalone seed branch");
    WriteTextFile(ctx.seedRepo / "README.md", InName + " seed\n");
    RequireSuccess(RunGit({"add", "README.md"}, ctx.seedRepo), "standalone seed add");
    RequireSuccess(RunGit({"commit", "-m", "seed standalone remote"}, ctx.seedRepo), "standalone seed commit");
    RequireSuccess(RunGit({"remote", "add", "origin", ctx.bareRemote.string()}, ctx.seedRepo), "standalone add remote");
    RequireSuccess(RunGit({"push", "-u", "origin", ctx.branch}, ctx.seedRepo), "standalone push");
    RequireSuccess(RunGit({"symbolic-ref", "HEAD", ("refs/heads/" + ctx.branch)}, ctx.bareRemote), "standalone bare HEAD");
    return ctx;
}

auto InstallCodexCaptureStub(const std::filesystem::path& InDir,
                             const std::filesystem::path& InCapturePath) -> std::filesystem::path {
    const auto stubDir = (InDir / "fake-codex-bin").lexically_normal();
    std::filesystem::create_directories(stubDir);
    const auto scriptPath = (stubDir / "codex-stub.ps1").lexically_normal();
    const auto cmdPath = (stubDir / "codex.cmd").lexically_normal();

    std::ostringstream script;
    script << "$capture = " << '"' << InCapturePath.string() << "\"\n";
    script << "$output = $null\n";
    script << "for ($i = 0; $i -lt $args.Length; $i++) {\n";
    script << "  if ($args[$i] -eq '-o' -and ($i + 1) -lt $args.Length) { $output = $args[$i + 1] }\n";
    script << "}\n";
    script << "if ($args.Length -gt 0) {\n";
    script << "  Set-Content -LiteralPath $capture -Value $args[$args.Length - 1]\n";
    script << "}\n";
    script << "if ($output) {\n";
    script << "  Set-Content -LiteralPath $output -Value '[]'\n";
    script << "}\n";
    WriteTextFile(scriptPath, script.str());

    std::ostringstream cmd;
    cmd << "@echo off\r\n";
    cmd << "powershell -NoProfile -ExecutionPolicy Bypass -File \"%~dp0codex-stub.ps1\" %*\r\n";
    WriteTextFile(cmdPath, cmd.str());
    return stubDir;
}

auto SetFileAgeSeconds(const std::filesystem::path& InPath, const int InAgeSeconds) -> void {
    const auto now = std::filesystem::file_time_type::clock::now();
    std::filesystem::last_write_time(InPath, now - std::chrono::seconds(InAgeSeconds));
}

auto ContainsPathEntry(const std::string& InPayload, const std::filesystem::path& InPath) -> bool {
    return InPayload.find(InPath.lexically_normal().generic_string()) != std::string::npos;
}

auto ContainsRawCheckoutChatter(const std::string& InPayload) -> bool {
    return InPayload.find("Already on '") != std::string::npos ||
           InPayload.find("Switched to branch '") != std::string::npos ||
           InPayload.find("Previous HEAD position was ") != std::string::npos;
}

auto ExtractJsonStringField(const std::string& InJson, const std::string& InKey) -> std::string {
    const auto keyToken = "\"" + InKey + "\"";
    const auto keyPos = InJson.find(keyToken);
    REQUIRE(keyPos != std::string::npos);
    const auto colonPos = InJson.find(':', keyPos + keyToken.size());
    REQUIRE(colonPos != std::string::npos);
    const auto firstQuote = InJson.find('"', colonPos + 1);
    REQUIRE(firstQuote != std::string::npos);
    const auto secondQuote = InJson.find('"', firstQuote + 1);
    REQUIRE(secondQuote != std::string::npos);
    return InJson.substr(firstQuote + 1, secondQuote - firstQuote - 1);
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
    WriteTextFile(ctx.seedRepo / ".gitignore", ".kano/\n");
    WriteTextFile(ctx.seedRepo / "README.md", "seed\n");
    RequireSuccess(RunGit({"add", ".gitignore", "README.md"}, ctx.seedRepo), "seed add");
    RequireSuccess(RunGit({"commit", "-m", "seed commit"}, ctx.seedRepo), "seed commit");
    RequireSuccess(RunGit({"remote", "add", "origin", ctx.bareRemote.string()}, ctx.seedRepo), "seed add remote");
    RequireSuccess(RunGit({"push", "-u", "origin", ctx.branch}, ctx.seedRepo), "seed push");
    RequireSuccess(RunGit({"symbolic-ref", "HEAD", ("refs/heads/" + ctx.branch)}, ctx.bareRemote), "set bare HEAD");
    RequireSuccess(RunGit({"clone", ctx.bareRemote.string(), ctx.cloneRepo.string()}, ctx.sandbox.root), "clone repo");
    ConfigureIdentity(ctx.cloneRepo);
    RequireSuccess(
        RunGit({"config", "kano.cache.local-dir", (ctx.sandbox.root / "_cache").string()}, ctx.cloneRepo),
        "configure external kano cache");
    return ctx;
}

auto CreateRemoteWithSubmoduleClone(const std::string& InName, const std::string& InBranch = "main") -> SubmoduleWorkspaceContext {
    SubmoduleWorkspaceContext ctx;
    ctx.sandbox = CreateSandboxWorkspace(InName);
    ctx.childBareRemote = (ctx.sandbox.root / "child-remote.git").lexically_normal();
    ctx.childSeedRepo = (ctx.sandbox.root / "child-seed").lexically_normal();
    ctx.rootBareRemote = (ctx.sandbox.root / "root-remote.git").lexically_normal();
    ctx.rootSeedRepo = (ctx.sandbox.root / "root-seed").lexically_normal();
    ctx.cloneRootRepo = (ctx.sandbox.root / "root-clone").lexically_normal();
    ctx.branch = InBranch;
    ctx.submodulePath = "deps/child";

    RequireSuccess(RunGit({"init", "--bare", ctx.childBareRemote.string()}, ctx.sandbox.root), "init child bare");
    RequireSuccess(RunGit({"init", ctx.childSeedRepo.string()}, ctx.sandbox.root), "init child seed");
    ConfigureIdentity(ctx.childSeedRepo);
    RequireSuccess(RunGit({"checkout", "-b", ctx.branch}, ctx.childSeedRepo), "checkout child branch");
    WriteTextFile(ctx.childSeedRepo / "child.txt", "child seed\n");
    RequireSuccess(RunGit({"add", "child.txt"}, ctx.childSeedRepo), "child add");
    RequireSuccess(RunGit({"commit", "-m", "child seed"}, ctx.childSeedRepo), "child commit");
    RequireSuccess(RunGit({"remote", "add", "origin", ctx.childBareRemote.string()}, ctx.childSeedRepo), "child add remote");
    RequireSuccess(RunGit({"push", "-u", "origin", ctx.branch}, ctx.childSeedRepo), "child push");
    RequireSuccess(RunGit({"symbolic-ref", "HEAD", ("refs/heads/" + ctx.branch)}, ctx.childBareRemote), "child bare HEAD");

    RequireSuccess(RunGit({"init", "--bare", ctx.rootBareRemote.string()}, ctx.sandbox.root), "init root bare");
    RequireSuccess(RunGit({"init", ctx.rootSeedRepo.string()}, ctx.sandbox.root), "init root seed");
    ConfigureIdentity(ctx.rootSeedRepo);
    RequireSuccess(RunGit({"checkout", "-b", ctx.branch}, ctx.rootSeedRepo), "checkout root branch");
    WriteTextFile(ctx.rootSeedRepo / ".gitignore", ".kano/\n");
    WriteTextFile(ctx.rootSeedRepo / "README.md", "root seed\n");
    RequireSuccess(RunGit({"add", ".gitignore", "README.md"}, ctx.rootSeedRepo), "root add base");
    RequireSuccess(RunGit({"commit", "-m", "root seed"}, ctx.rootSeedRepo), "root base commit");
    RequireSuccess(
        RunGit({"-c", "protocol.file.allow=always", "submodule", "add", "-b", ctx.branch, ctx.childBareRemote.string(), ctx.submodulePath},
               ctx.rootSeedRepo),
        "root add submodule");
    RequireSuccess(RunGit({"commit", "-am", "add submodule"}, ctx.rootSeedRepo), "root commit submodule");
    RequireSuccess(RunGit({"remote", "add", "origin", ctx.rootBareRemote.string()}, ctx.rootSeedRepo), "root add remote");
    RequireSuccess(RunGit({"push", "-u", "origin", ctx.branch}, ctx.rootSeedRepo), "root push");
    RequireSuccess(RunGit({"symbolic-ref", "HEAD", ("refs/heads/" + ctx.branch)}, ctx.rootBareRemote), "root bare HEAD");

    RequireSuccess(
        RunGit({"-c", "protocol.file.allow=always", "clone", "--recurse-submodules", ctx.rootBareRemote.string(), ctx.cloneRootRepo.string()},
               ctx.sandbox.root),
        "clone root with submodules");
    ConfigureIdentity(ctx.cloneRootRepo);
    RequireSuccess(
        RunGit({"config", "kano.cache.local-dir", (ctx.sandbox.root / "_cache").string()}, ctx.cloneRootRepo),
        "configure root external kano cache");
    ctx.cloneChildRepo = (ctx.cloneRootRepo / std::filesystem::path(ctx.submodulePath)).lexically_normal();
    ConfigureIdentity(ctx.cloneChildRepo);
    return ctx;
}

auto CreateRemoteWithSubmoduleBranchUpgradeClone(const std::string& InName) -> SubmoduleBranchUpgradeContext {
    SubmoduleBranchUpgradeContext ctx;
    ctx.sandbox = CreateSandboxWorkspace(InName);
    ctx.childBareRemote = (ctx.sandbox.root / "child-remote.git").lexically_normal();
    ctx.childSeedRepo = (ctx.sandbox.root / "child-seed").lexically_normal();
    ctx.rootBareRemote = (ctx.sandbox.root / "root-remote.git").lexically_normal();
    ctx.rootSeedRepo = (ctx.sandbox.root / "root-seed").lexically_normal();
    ctx.cloneRootRepo = (ctx.sandbox.root / "root-clone").lexically_normal();
    ctx.rootBranch = "main";
    ctx.initialChildBranch = "branch_v1.2.15";
    ctx.upgradedChildBranch = "branch_v1.2.25";
    ctx.submodulePath = "deps/child";

    RequireSuccess(RunGit({"init", "--bare", ctx.childBareRemote.string()}, ctx.sandbox.root), "init child bare");
    RequireSuccess(RunGit({"init", ctx.childSeedRepo.string()}, ctx.sandbox.root), "init child seed");
    ConfigureIdentity(ctx.childSeedRepo);
    RequireSuccess(RunGit({"checkout", "-b", ctx.initialChildBranch}, ctx.childSeedRepo), "checkout child initial branch");
    WriteTextFile(ctx.childSeedRepo / ".gitignore", ".kano/\n");
    WriteTextFile(ctx.childSeedRepo / "child.txt", "child seed\n");
    RequireSuccess(RunGit({"add", ".gitignore", "child.txt"}, ctx.childSeedRepo), "child add");
    RequireSuccess(RunGit({"commit", "-m", "child seed"}, ctx.childSeedRepo), "child commit");
    RequireSuccess(RunGit({"remote", "add", "origin", ctx.childBareRemote.string()}, ctx.childSeedRepo), "child add remote");
    RequireSuccess(RunGit({"push", "-u", "origin", ctx.initialChildBranch}, ctx.childSeedRepo), "child push initial");

    RequireSuccess(RunGit({"checkout", "-b", ctx.upgradedChildBranch}, ctx.childSeedRepo), "checkout child upgraded branch");
    WriteTextFile(ctx.childSeedRepo / "child.txt", "child upgraded\n");
    RequireSuccess(RunGit({"add", "child.txt"}, ctx.childSeedRepo), "child upgraded add");
    RequireSuccess(RunGit({"commit", "-m", "child upgraded"}, ctx.childSeedRepo), "child upgraded commit");
    RequireSuccess(RunGit({"push", "-u", "origin", ctx.upgradedChildBranch}, ctx.childSeedRepo), "child push upgraded");
    RequireSuccess(RunGit({"checkout", ctx.initialChildBranch}, ctx.childSeedRepo), "checkout child initial again");
    RequireSuccess(RunGit({"symbolic-ref", "HEAD", ("refs/heads/" + ctx.initialChildBranch)}, ctx.childBareRemote), "child bare HEAD");

    RequireSuccess(RunGit({"init", "--bare", ctx.rootBareRemote.string()}, ctx.sandbox.root), "init root bare");
    RequireSuccess(RunGit({"init", ctx.rootSeedRepo.string()}, ctx.sandbox.root), "init root seed");
    ConfigureIdentity(ctx.rootSeedRepo);
    RequireSuccess(RunGit({"checkout", "-b", ctx.rootBranch}, ctx.rootSeedRepo), "checkout root branch");
    WriteTextFile(ctx.rootSeedRepo / ".gitignore", ".kano/\n");
    WriteTextFile(ctx.rootSeedRepo / "README.md", "root seed\n");
    RequireSuccess(RunGit({"add", ".gitignore", "README.md"}, ctx.rootSeedRepo), "root add base");
    RequireSuccess(RunGit({"commit", "-m", "root seed"}, ctx.rootSeedRepo), "root base commit");
    RequireSuccess(
        RunGit({"-c", "protocol.file.allow=always", "submodule", "add", "-b", ctx.initialChildBranch, ctx.childBareRemote.string(), ctx.submodulePath},
               ctx.rootSeedRepo),
        "root add submodule initial");
    RequireSuccess(RunGit({"commit", "-am", "add submodule initial"}, ctx.rootSeedRepo), "root commit submodule initial");
    RequireSuccess(RunGit({"remote", "add", "origin", ctx.rootBareRemote.string()}, ctx.rootSeedRepo), "root add remote");
    RequireSuccess(RunGit({"push", "-u", "origin", ctx.rootBranch}, ctx.rootSeedRepo), "root push initial");
    RequireSuccess(RunGit({"symbolic-ref", "HEAD", ("refs/heads/" + ctx.rootBranch)}, ctx.rootBareRemote), "root bare HEAD");

    RequireSuccess(
        RunGit({"-c", "protocol.file.allow=always", "clone", "--recurse-submodules", ctx.rootBareRemote.string(), ctx.cloneRootRepo.string()},
               ctx.sandbox.root),
        "clone root with submodules");
    ConfigureIdentity(ctx.cloneRootRepo);
    RequireSuccess(
        RunGit({"config", "kano.cache.local-dir", (ctx.sandbox.root / "_cache").string()}, ctx.cloneRootRepo),
        "configure root external kano cache");
    ctx.cloneChildRepo = (ctx.cloneRootRepo / std::filesystem::path(ctx.submodulePath)).lexically_normal();
    ConfigureIdentity(ctx.cloneChildRepo);
    return ctx;
}

auto SetGitmodulesPushPolicy(const std::filesystem::path& InRepo, const std::string& InSubmodulePath, const std::string& InPolicy) -> void {
    RequireSuccess(
        RunGit({"config", "-f", ".gitmodules", ("submodule." + InSubmodulePath + ".kog-push-policy"), InPolicy}, InRepo),
        "set .gitmodules push policy");
    RequireSuccess(RunGit({"add", ".gitmodules"}, InRepo), "stage .gitmodules push policy");
    RequireSuccess(RunGit({"commit", "-m", "set push policy"}, InRepo), "commit .gitmodules push policy");
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
    WriteTextFile(ctx.nestedSeedRepo / ".gitignore", ".kano/\n");
    WriteTextFile(ctx.nestedSeedRepo / "nested.txt", "nested seed\n");
    RequireSuccess(RunGit({"add", ".gitignore", "nested.txt"}, ctx.nestedSeedRepo), "nested add");
    RequireSuccess(RunGit({"commit", "-m", "nested seed"}, ctx.nestedSeedRepo), "nested commit");
    RequireSuccess(RunGit({"remote", "add", "origin", ctx.nestedBareRemote.string()}, ctx.nestedSeedRepo), "nested add remote");
    RequireSuccess(RunGit({"push", "-u", "origin", ctx.branch}, ctx.nestedSeedRepo), "nested push");
    RequireSuccess(RunGit({"symbolic-ref", "HEAD", ("refs/heads/" + ctx.branch)}, ctx.nestedBareRemote), "nested bare HEAD");

    RequireSuccess(RunGit({"init", "--bare", ctx.rootBareRemote.string()}, ctx.sandbox.root), "init root bare");
    RequireSuccess(RunGit({"init", ctx.rootSeedRepo.string()}, ctx.sandbox.root), "init root seed");
    ConfigureIdentity(ctx.rootSeedRepo);
    RequireSuccess(RunGit({"checkout", "-b", ctx.branch}, ctx.rootSeedRepo), "checkout root branch");
    WriteTextFile(ctx.rootSeedRepo / ".gitignore", ".kano/\n");
    WriteTextFile(ctx.rootSeedRepo / "README.md", "root seed\n");
    RequireSuccess(RunGit({"add", ".gitignore", "README.md"}, ctx.rootSeedRepo), "root add");
    RequireSuccess(RunGit({"commit", "-m", "root seed"}, ctx.rootSeedRepo), "root commit");
    RequireSuccess(RunGit({"remote", "add", "origin", ctx.rootBareRemote.string()}, ctx.rootSeedRepo), "root add remote");
    RequireSuccess(RunGit({"push", "-u", "origin", ctx.branch}, ctx.rootSeedRepo), "root push");
    RequireSuccess(RunGit({"symbolic-ref", "HEAD", ("refs/heads/" + ctx.branch)}, ctx.rootBareRemote), "root bare HEAD");

    RequireSuccess(RunGit({"clone", ctx.rootBareRemote.string(), ctx.cloneRootRepo.string()}, ctx.sandbox.root), "clone root");
    ConfigureIdentity(ctx.cloneRootRepo);
    RequireSuccess(
        RunGit({"config", "kano.cache.local-dir", (ctx.sandbox.root / "_cache").string()}, ctx.cloneRootRepo),
        "configure root external kano cache");

    const auto nestedParent = (ctx.cloneRootRepo / std::filesystem::path(ctx.nestedRepoPath)).parent_path();
    std::filesystem::create_directories(nestedParent);
    RequireSuccess(
        RunGit({"clone", ctx.nestedBareRemote.string(), (ctx.cloneRootRepo / std::filesystem::path(ctx.nestedRepoPath)).string()}, ctx.sandbox.root),
        "clone nested repo");
    ctx.cloneNestedRepo = (ctx.cloneRootRepo / std::filesystem::path(ctx.nestedRepoPath)).lexically_normal();
    ConfigureIdentity(ctx.cloneNestedRepo);
    RequireSuccess(
        RunGit({"config", "kano.cache.local-dir", (ctx.sandbox.root / "_cache").string()}, ctx.cloneNestedRepo),
        "configure nested external kano cache");
    return ctx;
}

} // namespace

TEST_CASE("Functional test harness creates isolated sandbox workspace", "[functional][infrastructure]") {
    const auto sandbox = CreateSandboxWorkspace("infrastructure");
    REQUIRE_FALSE(sandbox.root.empty());
    REQUIRE(std::filesystem::exists(sandbox.root));
    RemoveSandboxWorkspace(sandbox);
    REQUIRE_FALSE(std::filesystem::exists(sandbox.root));
}

TEST_CASE("clean_not_ahead_is_noop_success", "[functional][commit-push][contract]") {
    const auto ctx = CreateRemoteWithClone("clean-not-ahead");
    const auto result = RunKog({"commit-push"}, ctx.cloneRepo);
    INFO(result.stdoutText);
    INFO(result.stderrText);
    REQUIRE(result.exitCode == 0);
    REQUIRE(result.stdoutText.find("workspace clean; skipping commit/sync/post-sync and proceeding to push check.") != std::string::npos);
    const auto [behind, ahead] = AheadBehindCounts(ctx.cloneRepo);
    REQUIRE(behind == 0);
    REQUIRE(ahead == 0);
    RemoveSandboxWorkspace(ctx.sandbox);
}

TEST_CASE("clean_but_ahead_continues_to_push", "[functional][commit-push][contract]") {
    const auto ctx = CreateRemoteWithClone("clean-but-ahead");
    WriteTextFile(ctx.cloneRepo / "local.txt", "ahead\n");
    RequireSuccess(RunGit({"add", "local.txt"}, ctx.cloneRepo), "local add");
    RequireSuccess(RunGit({"commit", "-m", "local ahead commit"}, ctx.cloneRepo), "local commit");
    const auto before = AheadBehindCounts(ctx.cloneRepo);
    REQUIRE(before.second == 1);

    const auto result = RunKog({"commit-push"}, ctx.cloneRepo);
    INFO(result.stdoutText);
    INFO(result.stderrText);
    REQUIRE(result.exitCode == 0);
    const auto after = AheadBehindCounts(ctx.cloneRepo);
    REQUIRE(after.second == 0);
    RemoveSandboxWorkspace(ctx.sandbox);
}

TEST_CASE("sync_none_continues", "[functional][commit-push][post-sync]") {
    const auto ctx = CreateRemoteWithClone("sync-none");
    WriteTextFile(ctx.cloneRepo / "README.md", "seed\nlocal update\n");

    const auto result = RunKog({"commit-push", "-m", "test(functional): sync none"}, ctx.cloneRepo);
    INFO(result.stdoutText);
    INFO(result.stderrText);
    REQUIRE(result.exitCode == 0);
    REQUIRE(result.stdoutText.find("=== commit-push stage: sync ===") != std::string::npos);
    REQUIRE(result.stdoutText.find("=== commit-push stage: post-sync ===") != std::string::npos);
    REQUIRE(result.stdoutText.find("post-sync commit skipped (no working tree changes).") != std::string::npos);
    const auto [behind, ahead] = AheadBehindCounts(ctx.cloneRepo);
    REQUIRE(behind == 0);
    REQUIRE(ahead == 0);
    RemoveSandboxWorkspace(ctx.sandbox);
}

TEST_CASE("sync_gitlink_only_auto_amends", "[functional][commit-push][post-sync]") {
    const auto ctx = CreateRemoteWithSubmoduleClone("sync-gitlink-only");
    WriteTextFile(ctx.childSeedRepo / "child.txt", "child remote advance\n");
    RequireSuccess(RunGit({"add", "child.txt"}, ctx.childSeedRepo), "child remote add");
    RequireSuccess(RunGit({"commit", "-m", "child remote advance"}, ctx.childSeedRepo), "child remote commit");
    RequireSuccess(RunGit({"push"}, ctx.childSeedRepo), "child remote push");
    const auto expectedChildHead = CurrentHeadSha(ctx.childSeedRepo);

    WriteTextFile(ctx.cloneRootRepo / "README.md", "root seed\nroot update\n");
    const auto result = RunKog({"commit-push", "-m", "test(functional): root update"}, ctx.cloneRootRepo);
    INFO(result.stdoutText);
    INFO(result.stderrText);
    REQUIRE(result.exitCode == 0);
    REQUIRE(result.stdoutText.find("post-sync gitlink-only auto-amend applied: repos=1") != std::string::npos);
    REQUIRE(CurrentHeadSha(ctx.cloneChildRepo) == expectedChildHead);
    REQUIRE(GitlinkHeadSha(ctx.cloneRootRepo, ctx.submodulePath) == expectedChildHead);
    const auto [behind, ahead] = AheadBehindCounts(ctx.cloneRootRepo);
    REQUIRE(behind == 0);
    REQUIRE(ahead == 0);
    RemoveSandboxWorkspace(ctx.sandbox);
}

TEST_CASE("amend_ai_auto_rewords_head_when_worktree_is_clean", "[functional][amend][ai]") {
    const auto ctx = CreateRemoteWithClone("amend-ai-clean-reword");
    WriteTextFile(ctx.cloneRepo / "README.md", "seed\namend ai reword\n");
    RequireSuccess(RunGit({"add", "README.md"}, ctx.cloneRepo), "stage amend ai change");
    RequireSuccess(RunGit({"commit", "-m", "chore: placeholder amend subject"}, ctx.cloneRepo), "seed amend target commit");
    const auto beforeHead = CurrentHeadSha(ctx.cloneRepo);

    std::vector<std::pair<std::string, std::string>> env{
        {"KOG_TEST_AI_STDOUT", "docs(readme): refine amend ai subject\n"},
        {"KOG_TEST_AI_EXIT_CODE", "0"},
    };
    if (const char* currentPath = std::getenv("PATH"); currentPath != nullptr) {
        env.emplace_back("PATH", currentPath);
    }

    const auto result = RunKogWithEnv(
        {"amend", "--ai-auto", "--ai-provider", "copilot", "--no-ai-review"},
        ctx.cloneRepo,
        env);
    INFO(result.stdoutText);
    INFO(result.stderrText);
    REQUIRE(result.exitCode == 0);
    REQUIRE(result.stdoutText.find("amended with ai-generated message") != std::string::npos);
    REQUIRE(CurrentHeadSha(ctx.cloneRepo) != beforeHead);

    const auto subject = RunGit({"log", "-1", "--pretty=%s"}, ctx.cloneRepo);
    RequireSuccess(subject, "read amended subject");
    REQUIRE(TrimCopy(subject.stdoutText) == "docs(readme): refine amend ai subject");

    RemoveSandboxWorkspace(ctx.sandbox);
}

TEST_CASE("sync_semantic_drift_reaches_post_sync_commit_stage", "[functional][commit-push][post-sync]") {
    const auto ctx = CreateRemoteWithClone("sync-semantic-drift");
    WriteTextFile(ctx.cloneRepo / "staged.txt", "staged\n");
    WriteTextFile(ctx.cloneRepo / "leftover.txt", "leftover\n");
    RequireSuccess(RunGit({"add", "staged.txt"}, ctx.cloneRepo), "stage staged-only file");

    const auto result = RunKog({"commit-push", "-m", "test(functional): staged only", "--staged-only"}, ctx.cloneRepo);
    INFO(result.stdoutText);
    INFO(result.stderrText);
    REQUIRE(result.exitCode != 0);
    const auto merged = result.stdoutText + "\n" + result.stderrText;
    REQUIRE(merged.find("post-sync semantic changes detected; proceeding to post-sync commit stage") != std::string::npos);
    REQUIRE(merged.find("Preflight blocked: --staged-only but nothing staged") != std::string::npos);
    REQUIRE(std::filesystem::exists(ctx.cloneRepo / "leftover.txt"));
    RemoveSandboxWorkspace(ctx.sandbox);
}

TEST_CASE("sync_conflict_fails_fast", "[functional][commit-push][post-sync]") {
    const auto ctx = CreateRemoteWithClone("sync-conflict");
    WriteTextFile(ctx.seedRepo / "README.md", "seed\nremote conflict\n");
    RequireSuccess(RunGit({"add", "README.md"}, ctx.seedRepo), "remote conflict add");
    RequireSuccess(RunGit({"commit", "-m", "remote conflict"}, ctx.seedRepo), "remote conflict commit");
    RequireSuccess(RunGit({"push"}, ctx.seedRepo), "remote conflict push");

    WriteTextFile(ctx.cloneRepo / "README.md", "seed\nlocal conflict\n");
    const auto result = RunKog({"commit-push", "-m", "test(functional): local conflict"}, ctx.cloneRepo);
    INFO(result.stdoutText);
    INFO(result.stderrText);
    REQUIRE(result.exitCode != 0);
    const auto merged = result.stdoutText + "\n" + result.stderrText;
    const bool mentionsConflict = merged.find("CONFLICT") != std::string::npos ||
                                  merged.find("could not apply") != std::string::npos ||
                                  merged.find("rebase") != std::string::npos;
    REQUIRE(mentionsConflict);
    RemoveSandboxWorkspace(ctx.sandbox);
}

TEST_CASE("commit_push_ai_auto_single_human_mode_fails_without_deterministic_fallback",
          "[functional][commit-push][ai][single]") {
    const auto ctx = CreateRemoteWithClone("commit-push-ai-single-fail-fast");
    WriteTextFile(ctx.cloneRepo / "README.md", "seed\nai single fail-fast\n");
    const auto beforeHead = CurrentHeadSha(ctx.cloneRepo);

    std::vector<std::pair<std::string, std::string>> env{
        {"KOG_TEST_AI_STDOUT", "not-json"},
        {"KOG_TEST_AI_EXIT_CODE", "0"},
        {"KANO_AGENT_MODE", ""},
    };
    if (const char* currentPath = std::getenv("PATH"); currentPath != nullptr) {
        env.emplace_back("PATH", currentPath);
    }

    const auto result = RunKogWithEnv(
        {"commit-push", "--ai-auto", "--ai-provider", "copilot", "--ai-fill-mode", "single", "--no-ai-review"},
        ctx.cloneRepo,
        env);
    INFO(result.stdoutText);
    INFO(result.stderrText);
    REQUIRE(result.exitCode != 0);

    const auto merged = result.stdoutText + "\n" + result.stderrText;
    REQUIRE(merged.find("Human-mode CPA forbids deterministic commit fallback in single mode") != std::string::npos);
    REQUIRE(merged.find("AI commit runbook failed via native binary") != std::string::npos);
    REQUIRE(merged.find("[commit-push][auto-plan] stage=commit-runbook failed") != std::string::npos);
    REQUIRE(merged.find("Filled plan commit entries with deterministic fallback ops") == std::string::npos);
    REQUIRE(merged.find("using deterministic local fallback") == std::string::npos);
    REQUIRE(CurrentHeadSha(ctx.cloneRepo) == beforeHead);
    const auto [behind, ahead] = AheadBehindCounts(ctx.cloneRepo);
    REQUIRE(behind == 0);
    REQUIRE(ahead == 0);

    RemoveSandboxWorkspace(ctx.sandbox);
}

TEST_CASE("commit_push_ai_auto_codex_uses_explicit_workspace_relative_prompt_reference",
          "[functional][commit-push][ai][codex]") {
#if defined(_WIN32)
    const auto ctx = CreateRemoteWithClone("commit-push-ai-codex-prompt-ref");
    WriteTextFile(ctx.cloneRepo / "README.md", "seed\ncodex prompt ref\n");

    const auto capturePath = (ctx.sandbox.root / "codex-prompt.txt").lexically_normal();
    const auto stubDir = InstallCodexCaptureStub(ctx.sandbox.root, capturePath);

    std::vector<std::pair<std::string, std::string>> env{
        {"KANO_AGENT_MODE", ""},
        {"PATH", stubDir.string() + ";" + (std::getenv("PATH") != nullptr ? std::getenv("PATH") : "")},
    };

    const auto result = RunKogWithEnv(
        {"commit-push", "--ai-auto", "--ai-provider", "codex", "--ai-fill-mode", "single", "--no-ai-review"},
        ctx.cloneRepo,
        env);
    INFO(result.stdoutText);
    INFO(result.stderrText);

    REQUIRE(std::filesystem::exists(capturePath));
    const auto capturedPrompt = ReadTextFile(capturePath);
    // Prompt reference must use an absolute path so the provider can resolve it
    // regardless of working directory.
    REQUIRE(capturedPrompt.find("Read @") != std::string::npos);
    REQUIRE(capturedPrompt.find("plan-fill-structured-") != std::string::npos);
    REQUIRE(capturedPrompt.find("Read @./") == std::string::npos);

    RemoveSandboxWorkspace(ctx.sandbox);
#else
    SUCCEED("Windows-only regression");
#endif
}

TEST_CASE("plan_runbook_commit_per_commit_accepts_flat_review_fill_ops_aliases",
          "[functional][plan][ai][per-commit]") {
    const auto ctx = CreateRemoteWithClone("plan-runbook-per-commit-flat-review-aliases");
    WriteTextFile(ctx.cloneRepo / "README.md", "seed\nper-commit alias fill\n");
    const auto planPath = (ctx.cloneRepo / ".kano" / "tmp" / "git" / "plans" / "per-commit-plan.json").lexically_normal();

    const std::string aiPayload =
        "BEGIN_KOG_PLAN_FILL_OPS\n"
        "{\n"
        "  \"commits\": [\n"
        "    {\n"
        "      \"index\": 0,\n"
        "      \"message\": \"docs(readme): clarify per-commit alias handling\",\n"
        "      \"review_verdict\": \"pass\",\n"
        "      \"review_reason\": \"The README change is self-contained and the alias-based fill payload is semantically complete.\"\n"
        "    }\n"
        "  ]\n"
        "}\n"
        "END_KOG_PLAN_FILL_OPS\n";

    std::vector<std::pair<std::string, std::string>> env{
        {"KOG_TEST_AI_STDOUT", aiPayload},
        {"KOG_TEST_AI_EXIT_CODE", "0"},
    };
    if (const char* currentPath = std::getenv("PATH"); currentPath != nullptr) {
        env.emplace_back("PATH", currentPath);
    }

    const auto result = RunKogWithEnv(
        {"plan", "runbook", "commit", "--plan-file", planPath.string(), "--ai-provider", "copilot", "--ai-fill-mode", "per-commit"},
        ctx.cloneRepo,
        env);
    INFO(result.stdoutText);
    INFO(result.stderrText);
    REQUIRE(result.exitCode == 0);
    REQUIRE(std::filesystem::exists(planPath));

    const auto planText = ReadTextFile(planPath);
    REQUIRE(planText.find("docs(readme): clarify per-commit alias handling") != std::string::npos);
    REQUIRE(planText.find("The README change is self-contained and the alias-based fill payload is semantically complete.") != std::string::npos);
    REQUIRE(planText.find("\"verdict\":\"pass\"") != std::string::npos);
    REQUIRE(result.stdoutText.find("Filled plan commit entries with AI-safe ops") != std::string::npos);

    RemoveSandboxWorkspace(ctx.sandbox);
}

TEST_CASE("sync_registered_submodule_refreshes_gitmodules_branch_after_parent_sync", "[functional][sync][gitmodules-branch]") {
    const auto ctx = CreateRemoteWithSubmoduleBranchUpgradeClone("sync-gitmodules-branch-refresh");

    WriteTextFile(ctx.cloneChildRepo / "local-only.txt", "dirty local change\n");

    const auto rootSubmoduleRepo = (ctx.rootSeedRepo / std::filesystem::path(ctx.submodulePath)).lexically_normal();
    RequireSuccess(RunGit({"fetch", "origin", "--prune", "--tags"}, rootSubmoduleRepo), "fetch child origin from root seed");
    RequireSuccess(RunGit({"checkout", "-B", ctx.upgradedChildBranch, ("origin/" + ctx.upgradedChildBranch)}, rootSubmoduleRepo), "checkout upgraded child branch in root seed");
    RequireSuccess(
        RunGit({"config", "-f", ".gitmodules", ("submodule." + ctx.submodulePath + ".branch"), ctx.upgradedChildBranch}, ctx.rootSeedRepo),
        "update gitmodules branch");
    RequireSuccess(RunGit({"add", ".gitmodules", ctx.submodulePath}, ctx.rootSeedRepo), "stage root upgrade");
    RequireSuccess(RunGit({"commit", "-m", "upgrade child branch mapping"}, ctx.rootSeedRepo), "commit root upgrade");
    RequireSuccess(RunGit({"push", "origin", ctx.rootBranch}, ctx.rootSeedRepo), "push root upgrade");

    const auto result = RunKog({"sync"}, ctx.cloneRootRepo);
    INFO(result.stdoutText);
    INFO(result.stderrText);
    REQUIRE(result.exitCode == 0);
    REQUIRE(result.stdoutText.find("Branch source: registered .gitmodules branch (refreshed)") != std::string::npos);
    REQUIRE(result.stdoutText.find("Auto-stashed local changes for deps/child") != std::string::npos);
    REQUIRE(result.stdoutText.find("Restored auto-stash for deps/child") != std::string::npos);

    REQUIRE(CurrentBranch(ctx.cloneChildRepo) == ctx.upgradedChildBranch);
    REQUIRE(CurrentHeadSha(ctx.cloneChildRepo) == RefSha(ctx.childBareRemote, "refs/heads/" + ctx.upgradedChildBranch));
    REQUIRE(ReadTextFile(ctx.cloneChildRepo / "local-only.txt") == "dirty local change\n");
    REQUIRE(ReadTextFile(ctx.cloneRootRepo / ".gitmodules").find("branch = " + ctx.upgradedChildBranch) != std::string::npos);

    RemoveSandboxWorkspace(ctx.sandbox);
}

TEST_CASE("sync_registered_submodule_fails_when_refreshed_gitmodules_branch_is_missing", "[functional][sync][gitmodules-branch]") {
    const auto ctx = CreateRemoteWithSubmoduleBranchUpgradeClone("sync-gitmodules-branch-missing");

    WriteTextFile(ctx.cloneChildRepo / "child.txt", "child seed\nlocal dirty change\n");
    const auto childHeadBefore = CurrentHeadSha(ctx.cloneChildRepo);
    const auto childBranchBefore = CurrentBranch(ctx.cloneChildRepo);

    const auto missingBranch = std::string("branch_v1.2.99");
    RequireSuccess(
        RunGit({"config", "-f", ".gitmodules", ("submodule." + ctx.submodulePath + ".branch"), missingBranch}, ctx.rootSeedRepo),
        "set missing child branch in gitmodules");
    RequireSuccess(RunGit({"add", ".gitmodules"}, ctx.rootSeedRepo), "stage missing gitmodules branch");
    RequireSuccess(RunGit({"commit", "-m", "point submodule branch to missing branch"}, ctx.rootSeedRepo), "commit missing gitmodules branch");
    RequireSuccess(RunGit({"push", "origin", ctx.rootBranch}, ctx.rootSeedRepo), "push missing gitmodules branch");

    const auto result = RunKog({"sync"}, ctx.cloneRootRepo);
    INFO(result.stdoutText);
    INFO(result.stderrText);
    REQUIRE(result.exitCode != 0);

    const auto merged = result.stdoutText + "\n" + result.stderrText;
    REQUIRE(merged.find("registered .gitmodules branch (refreshed)") != std::string::npos);
    REQUIRE(merged.find("Target branch not found for deps/child") != std::string::npos);

    REQUIRE(CurrentHeadSha(ctx.cloneChildRepo) == childHeadBefore);
    REQUIRE(CurrentBranch(ctx.cloneChildRepo) == childBranchBefore);
    REQUIRE(ReadTextFile(ctx.cloneChildRepo / "child.txt") == "child seed\nlocal dirty change\n");
    REQUIRE(ReadTextFile(ctx.cloneRootRepo / ".gitmodules").find("branch = " + missingBranch) != std::string::npos);

    RemoveSandboxWorkspace(ctx.sandbox);
}

TEST_CASE("submodule_add_bootstraps_empty_remote_on_requested_branch", "[functional][submodule][add]") {
    const auto ctx = CreateRemoteWithClone("submodule-add-empty-remote");
    ConfigureFileProtocolAlways(ctx.cloneRepo);
    const auto child = CreateStandaloneBareRemote(ctx.sandbox, "empty-child", "branch_v1", false);
    const auto submodulePath = std::string{"deps/empty-child"};

    const auto result = RunKogAllowingFileProtocol(
        {"submodule", "add", "-b", child.branch, child.bareRemote.string(), submodulePath},
        ctx.cloneRepo);
    INFO(result.stdoutText);
    INFO(result.stderrText);
    REQUIRE(result.exitCode == 0);
    REQUIRE(result.stdoutText.find("Remote appears empty; bootstrapping initial commit") != std::string::npos);

    const auto childRepo = (ctx.cloneRepo / std::filesystem::path(submodulePath)).lexically_normal();
    REQUIRE(std::filesystem::exists(childRepo / "README.md"));
    REQUIRE_FALSE(RefSha(child.bareRemote, "refs/heads/" + child.branch).empty());

    const auto branchResult = RunGit(
        {"config", "-f", ".gitmodules", "--get", "submodule." + submodulePath + ".branch"},
        ctx.cloneRepo);
    RequireSuccess(branchResult, "read gitmodules branch after kog submodule add");
    REQUIRE(TrimCopy(branchResult.stdoutText) == child.branch);

    RemoveSandboxWorkspace(ctx.sandbox);
}

TEST_CASE("submodule_add_preserves_passthrough_for_initialized_remote", "[functional][submodule][add]") {
    const auto ctx = CreateRemoteWithClone("submodule-add-existing-remote");
    ConfigureFileProtocolAlways(ctx.cloneRepo);
    const auto child = CreateStandaloneBareRemote(ctx.sandbox, "seeded-child", "main", true);
    const auto submodulePath = std::string{"deps/seeded-child"};

    const auto result = RunKogAllowingFileProtocol(
        {"submodule", "add", child.bareRemote.string(), submodulePath},
        ctx.cloneRepo);
    INFO(result.stdoutText);
    INFO(result.stderrText);
    REQUIRE(result.exitCode == 0);
    REQUIRE(result.stdoutText.find("Remote appears empty; bootstrapping initial commit") == std::string::npos);

    const auto childRepo = (ctx.cloneRepo / std::filesystem::path(submodulePath)).lexically_normal();
    REQUIRE(std::filesystem::exists(childRepo / "README.md"));
    REQUIRE(CurrentHeadSha(childRepo) == RefSha(child.bareRemote, "refs/heads/" + child.branch));

    RemoveSandboxWorkspace(ctx.sandbox);
}

TEST_CASE("multi_repo_commit_push_pushes_root_and_registered_submodule", "[functional][commit-push][multi-repo]") {
    const auto ctx = CreateRemoteWithSubmoduleClone("multi-repo-registered");

    WriteTextFile(ctx.cloneChildRepo / "child.txt", "child local update\n");
    WriteTextFile(ctx.cloneRootRepo / "README.md", "root seed\nroot local update\n");

    const auto result = RunKog({"commit-push", "-m", "test(functional): multi repo update"}, ctx.cloneRootRepo);
    INFO(result.stdoutText);
    INFO(result.stderrText);
    REQUIRE(result.exitCode == 0);

    const auto childHead = CurrentHeadSha(ctx.cloneChildRepo);
    const auto rootHead = CurrentHeadSha(ctx.cloneRootRepo);
    REQUIRE_FALSE(childHead.empty());
    REQUIRE_FALSE(rootHead.empty());

    const auto [rootBehind, rootAhead] = AheadBehindCounts(ctx.cloneRootRepo);
    REQUIRE(rootBehind == 0);
    REQUIRE(rootAhead == 0);

    const auto [childBehind, childAhead] = AheadBehindCounts(ctx.cloneChildRepo);
    REQUIRE(childBehind == 0);
    REQUIRE(childAhead == 0);

    REQUIRE(GitlinkHeadSha(ctx.cloneRootRepo, ctx.submodulePath) == childHead);
    REQUIRE(RefSha(ctx.rootBareRemote, "refs/heads/" + ctx.branch) == rootHead);
    REQUIRE(RefSha(ctx.childBareRemote, "refs/heads/" + ctx.branch) == childHead);

    RemoveSandboxWorkspace(ctx.sandbox);
}

TEST_CASE("multi_repo_commit_push_pushes_root_and_unregistered_nested_repo", "[functional][commit-push][multi-repo]") {
    const auto ctx = CreateRemoteWithNestedRepoClone("multi-repo-unregistered");

    WriteTextFile(ctx.cloneNestedRepo / "nested.txt", "nested local update\n");
    WriteTextFile(ctx.cloneRootRepo / "README.md", "root seed\nroot local update\n");

    const auto result = RunKog({"commit-push", "-m", "test(functional): multi repo nested update"}, ctx.cloneRootRepo);
    INFO(result.stdoutText);
    INFO(result.stderrText);
    REQUIRE(result.exitCode == 0);

    const auto nestedHead = CurrentHeadSha(ctx.cloneNestedRepo);
    const auto rootHead = CurrentHeadSha(ctx.cloneRootRepo);
    REQUIRE_FALSE(nestedHead.empty());
    REQUIRE_FALSE(rootHead.empty());

    const auto [rootBehind, rootAhead] = AheadBehindCounts(ctx.cloneRootRepo);
    REQUIRE(rootBehind == 0);
    REQUIRE(rootAhead == 0);

    const auto [nestedBehind, nestedAhead] = AheadBehindCounts(ctx.cloneNestedRepo);
    REQUIRE(nestedBehind == 0);
    REQUIRE(nestedAhead == 0);

    REQUIRE(RefSha(ctx.rootBareRemote, "refs/heads/" + ctx.branch) == rootHead);
    REQUIRE(RefSha(ctx.nestedBareRemote, "refs/heads/" + ctx.branch) == nestedHead);

    RemoveSandboxWorkspace(ctx.sandbox);
}

TEST_CASE("push_skips_registered_submodule_when_gitmodules_policy_is_skip", "[functional][push][policy]") {
    const auto ctx = CreateRemoteWithSubmoduleClone("push-policy-skip");
    SetGitmodulesPushPolicy(ctx.rootSeedRepo, ctx.submodulePath, "skip");
    RequireSuccess(RunGit({"push"}, ctx.rootSeedRepo), "push .gitmodules policy to remote");
    RequireSuccess(RunGit({"pull", "--rebase"}, ctx.cloneRootRepo), "pull .gitmodules policy into clone");

    WriteTextFile(ctx.cloneRootRepo / "README.md", "root seed\nroot push policy update\n");
    RequireSuccess(RunGit({"add", "README.md"}, ctx.cloneRootRepo), "root add policy update");
    RequireSuccess(RunGit({"commit", "-m", "root update"}, ctx.cloneRootRepo), "root commit policy update");

    WriteTextFile(ctx.cloneChildRepo / "child.txt", "child local skip push\n");
    RequireSuccess(RunGit({"add", "child.txt"}, ctx.cloneChildRepo), "child add skip push");
    RequireSuccess(RunGit({"commit", "-m", "child skip push"}, ctx.cloneChildRepo), "child commit skip push");
    const auto childRemoteBefore = RefSha(ctx.childBareRemote, "refs/heads/" + ctx.branch);

    const auto result = RunKog({"push"}, ctx.cloneRootRepo);
    INFO(result.stdoutText);
    INFO(result.stderrText);
    REQUIRE(result.exitCode == 0);
    REQUIRE(result.stdoutText.find("Push skipped by .gitmodules policy (kog-push-policy=skip)") != std::string::npos);

    const auto [rootBehind, rootAhead] = AheadBehindCounts(ctx.cloneRootRepo);
    REQUIRE(rootBehind == 0);
    REQUIRE(rootAhead == 0);
    REQUIRE(CurrentHeadSha(ctx.cloneChildRepo) != childRemoteBefore);
    REQUIRE(RefSha(ctx.childBareRemote, "refs/heads/" + ctx.branch) == childRemoteBefore);

    RemoveSandboxWorkspace(ctx.sandbox);
}

TEST_CASE("push_skips_local_non_bare_remote_with_checked_out_branch", "[functional][push][policy]") {
    const auto ctx = CreateRemoteWithClone("push-local-non-bare-checked-out");
    const auto localWorkingRemote = (ctx.sandbox.root / "local-working-remote").lexically_normal();

    RequireSuccess(RunGit({"clone", ctx.bareRemote.string(), localWorkingRemote.string()}, ctx.sandbox.root), "clone local working remote");
    ConfigureIdentity(localWorkingRemote);
    RequireSuccess(RunGit({"remote", "set-url", "origin", localWorkingRemote.string()}, ctx.cloneRepo), "set clone origin to local working remote");

    WriteTextFile(ctx.cloneRepo / "README.md", "seed\nlocal remote push skip\n");
    RequireSuccess(RunGit({"add", "README.md"}, ctx.cloneRepo), "stage local remote push skip change");
    RequireSuccess(RunGit({"commit", "-m", "local remote push skip"}, ctx.cloneRepo), "commit local remote push skip change");

    const auto beforeRemoteHead = CurrentHeadSha(localWorkingRemote);

    const auto result = RunKog({"push"}, ctx.cloneRepo);
    INFO(result.stdoutText);
    INFO(result.stderrText);
    REQUIRE(result.exitCode == 0);
    REQUIRE(result.stdoutText.find("Push skipped (origin): local non-bare remote has checked-out branch") != std::string::npos);
    REQUIRE(CurrentHeadSha(localWorkingRemote) == beforeRemoteHead);

    RemoveSandboxWorkspace(ctx.sandbox);
}

TEST_CASE("workspace_discover_respects_gitignore_for_nested_repo", "[functional][workspace][discovery]") {
    const auto ctx = CreateRemoteWithNestedRepoClone("discover-gitignore");
    WriteTextFile(ctx.cloneRootRepo / ".gitignore", ".kano/\nnested/\n");

    const auto result = RunKog(
        {"workspace", "discover", "--format", "json", "--repo-root", ctx.cloneRootRepo.string(), "--no-cache"},
        ctx.cloneRootRepo);
    INFO(result.stdoutText);
    INFO(result.stderrText);
    REQUIRE(result.exitCode == 0);
    REQUIRE(ContainsPathEntry(result.stdoutText, ctx.cloneRootRepo));
    REQUIRE_FALSE(ContainsPathEntry(result.stdoutText, ctx.cloneNestedRepo));

    RemoveSandboxWorkspace(ctx.sandbox);
}

TEST_CASE("workspace_discover_respects_kogignore_for_nested_repo", "[functional][workspace][discovery]") {
    const auto ctx = CreateRemoteWithNestedRepoClone("discover-kogignore");
    WriteTextFile(ctx.cloneRootRepo / ".kogignore", "nested/\n");

    const auto result = RunKog(
        {"workspace", "discover", "--format", "json", "--repo-root", ctx.cloneRootRepo.string(), "--no-cache"},
        ctx.cloneRootRepo);
    INFO(result.stdoutText);
    INFO(result.stderrText);
    REQUIRE(result.exitCode == 0);
    REQUIRE(ContainsPathEntry(result.stdoutText, ctx.cloneRootRepo));
    REQUIRE_FALSE(ContainsPathEntry(result.stdoutText, ctx.cloneNestedRepo));

    RemoveSandboxWorkspace(ctx.sandbox);
}

TEST_CASE("workspace_discover_exclude_is_temporary_override", "[functional][workspace][discovery]") {
    const auto ctx = CreateRemoteWithNestedRepoClone("discover-exclude");

    const auto baseline = RunKog(
        {"workspace", "discover", "--format", "json", "--repo-root", ctx.cloneRootRepo.string(), "--no-cache"},
        ctx.cloneRootRepo);
    INFO(baseline.stdoutText);
    INFO(baseline.stderrText);
    REQUIRE(baseline.exitCode == 0);
    REQUIRE(ContainsPathEntry(baseline.stdoutText, ctx.cloneNestedRepo));

    const auto excluded = RunKog(
        {"workspace", "discover", "--format", "json", "--repo-root", ctx.cloneRootRepo.string(), "--no-cache",
         "--exclude", "nested/"},
        ctx.cloneRootRepo);
    INFO(excluded.stdoutText);
    INFO(excluded.stderrText);
    REQUIRE(excluded.exitCode == 0);
    REQUIRE_FALSE(ContainsPathEntry(excluded.stdoutText, ctx.cloneNestedRepo));

    const auto after = RunKog(
        {"workspace", "discover", "--format", "json", "--repo-root", ctx.cloneRootRepo.string(), "--no-cache"},
        ctx.cloneRootRepo);
    INFO(after.stdoutText);
    INFO(after.stderrText);
    REQUIRE(after.exitCode == 0);
    REQUIRE(ContainsPathEntry(after.stdoutText, ctx.cloneNestedRepo));

    RemoveSandboxWorkspace(ctx.sandbox);
}

TEST_CASE("workspace_discover_honors_gitignore_reinclude_under_build_script", "[functional][workspace][discovery]") {
    const auto ctx = CreateRemoteWithClone("discover-build-script-reinclude");
    WriteTextFile(
        ctx.cloneRepo / ".gitignore",
        ".kano/\n"
        "src/cpp/build/**\n"
        "!src/cpp/build/\n"
        "!src/cpp/build/script/\n"
        "!src/cpp/build/script/**\n");

    const auto scriptRepo = (ctx.cloneRepo / "src" / "cpp" / "build" / "script" / "tooling-repo").lexically_normal();
    const auto intermediateRepo = (ctx.cloneRepo / "src" / "cpp" / "build" / "_intermediate" / "cache-repo").lexically_normal();

    std::filesystem::create_directories(scriptRepo.parent_path());
    std::filesystem::create_directories(intermediateRepo.parent_path());
    RequireSuccess(RunGit({"init", scriptRepo.string()}, ctx.cloneRepo), "init script nested repo");
    RequireSuccess(RunGit({"init", intermediateRepo.string()}, ctx.cloneRepo), "init intermediate nested repo");

    const auto result = RunKog(
        {"workspace", "discover", "--format", "json", "--repo-root", ctx.cloneRepo.string(), "--no-cache"},
        ctx.cloneRepo);
    INFO(result.stdoutText);
    INFO(result.stderrText);
    REQUIRE(result.exitCode == 0);
    REQUIRE(ContainsPathEntry(result.stdoutText, ctx.cloneRepo));
    REQUIRE(ContainsPathEntry(result.stdoutText, scriptRepo));
    REQUIRE_FALSE(ContainsPathEntry(result.stdoutText, intermediateRepo));

    RemoveSandboxWorkspace(ctx.sandbox);
}

TEST_CASE("workspace_discover_honors_kogignore_reinclude_under_build_script", "[functional][workspace][discovery]") {
    const auto ctx = CreateRemoteWithClone("discover-build-script-kogignore-reinclude");
    WriteTextFile(
        ctx.cloneRepo / ".kogignore",
        "src/cpp/build/**\n"
        "!src/cpp/build/\n"
        "!src/cpp/build/script/\n"
        "!src/cpp/build/script/**\n");

    const auto scriptRepo = (ctx.cloneRepo / "src" / "cpp" / "build" / "script" / "tooling-repo").lexically_normal();
    const auto intermediateRepo = (ctx.cloneRepo / "src" / "cpp" / "build" / "_intermediate" / "cache-repo").lexically_normal();

    std::filesystem::create_directories(scriptRepo.parent_path());
    std::filesystem::create_directories(intermediateRepo.parent_path());
    RequireSuccess(RunGit({"init", scriptRepo.string()}, ctx.cloneRepo), "init script nested repo via kogignore");
    RequireSuccess(RunGit({"init", intermediateRepo.string()}, ctx.cloneRepo), "init intermediate nested repo via kogignore");

    const auto result = RunKog(
        {"workspace", "discover", "--format", "json", "--repo-root", ctx.cloneRepo.string(), "--no-cache"},
        ctx.cloneRepo);
    INFO(result.stdoutText);
    INFO(result.stderrText);
    REQUIRE(result.exitCode == 0);
    REQUIRE(ContainsPathEntry(result.stdoutText, ctx.cloneRepo));
    REQUIRE(ContainsPathEntry(result.stdoutText, scriptRepo));
    REQUIRE_FALSE(ContainsPathEntry(result.stdoutText, intermediateRepo));

    RemoveSandboxWorkspace(ctx.sandbox);
}

TEST_CASE("workspace_discover_includes_external_roots_when_local_config_inherits", "[functional][workspace][discovery][external]") {
    const auto ctx = CreateRemoteWithClone("discover-external-inherit");
    const auto skillRoot = (ctx.sandbox.root / "skill-root").lexically_normal();
    const auto systemAssets = (skillRoot / "assets").lexically_normal();
    const auto agentsRoot = (ctx.sandbox.root / "agents-skills-kano").lexically_normal();
    const auto codexRoot = (ctx.sandbox.root / "codex-skills-kano").lexically_normal();
    const auto agentRepo = (agentsRoot / "alpha-skill").lexically_normal();
    const auto codexRepo = (codexRoot / "beta-skill").lexically_normal();

    std::filesystem::create_directories(systemAssets);
    WriteTextFile(systemAssets / "kog_config.toml",
                  "[workspace.external]\n"
                  "inherit = true\n"
                  "roots = ['" + agentsRoot.generic_string() + "']\n");
    std::filesystem::create_directories(ctx.cloneRepo / ".kano");
    WriteTextFile(ctx.cloneRepo / ".kano" / "kog_config.toml",
                  "[workspace.external]\n"
                  "inherit = true\n"
                  "roots = ['" + codexRoot.generic_string() + "']\n");

    InitPlainGitRepo(agentRepo);
    InitPlainGitRepo(codexRepo);

    const auto result = RunKogWithEnv(
        {"workspace", "discover", "--format", "json", "--repo-root", ctx.cloneRepo.string(), "--no-cache"},
        ctx.cloneRepo,
        {{"KANO_GIT_SKILL_ROOT", skillRoot.string()}});
    INFO(result.stdoutText);
    INFO(result.stderrText);
    REQUIRE(result.exitCode == 0);
    REQUIRE(ContainsPathEntry(result.stdoutText, ctx.cloneRepo));
    REQUIRE(ContainsPathEntry(result.stdoutText, agentRepo));
    REQUIRE(ContainsPathEntry(result.stdoutText, codexRepo));

    RemoveSandboxWorkspace(ctx.sandbox);
}

TEST_CASE("workspace_discover_local_external_roots_can_disable_inherited_defaults", "[functional][workspace][discovery][external]") {
    const auto ctx = CreateRemoteWithClone("discover-external-no-inherit");
    const auto skillRoot = (ctx.sandbox.root / "skill-root").lexically_normal();
    const auto systemAssets = (skillRoot / "assets").lexically_normal();
    const auto agentsRoot = (ctx.sandbox.root / "agents-skills-kano").lexically_normal();
    const auto codexRoot = (ctx.sandbox.root / "codex-skills-kano").lexically_normal();
    const auto sharedName = std::string("shared-skill");
    const auto agentRepo = (agentsRoot / sharedName).lexically_normal();
    const auto codexRepo = (codexRoot / sharedName).lexically_normal();

    std::filesystem::create_directories(systemAssets);
    WriteTextFile(systemAssets / "kog_config.toml",
                  "[workspace.external]\n"
                  "inherit = true\n"
                  "roots = ['" + agentsRoot.generic_string() + "']\n");
    std::filesystem::create_directories(ctx.cloneRepo / ".kano");
    WriteTextFile(ctx.cloneRepo / ".kano" / "kog_config.toml",
                  "[workspace.external]\n"
                  "inherit = false\n"
                  "roots = ['" + codexRoot.generic_string() + "']\n");

    InitPlainGitRepo(agentRepo);
    InitPlainGitRepo(codexRepo);

    const auto result = RunKogWithEnv(
        {"workspace", "discover", "--format", "json", "--repo-root", ctx.cloneRepo.string(), "--no-cache"},
        ctx.cloneRepo,
        {{"KANO_GIT_SKILL_ROOT", skillRoot.string()}});
    INFO(result.stdoutText);
    INFO(result.stderrText);
    REQUIRE(result.exitCode == 0);
    REQUIRE(ContainsPathEntry(result.stdoutText, ctx.cloneRepo));
    REQUIRE_FALSE(ContainsPathEntry(result.stdoutText, agentRepo));
    REQUIRE(ContainsPathEntry(result.stdoutText, codexRepo));

    RemoveSandboxWorkspace(ctx.sandbox);
}

TEST_CASE("plan_new_populates_hashes_and_verify_pre_apply_passes", "[functional][plan][freshness]") {
    const auto ctx = CreateRemoteWithClone("plan-new-hashes");
    WriteTextFile(ctx.cloneRepo / "README.md", "seed\nplan dirty\n");
    const auto planPath = (ctx.cloneRepo / ".kano" / "cache" / "git" / "plans" / "default-plan.json").lexically_normal();

    const auto planNew = RunKog({"plan", "new", "--force", "--output", planPath.string()}, ctx.cloneRepo);
    INFO(planNew.stdoutText);
    INFO(planNew.stderrText);
    REQUIRE(planNew.exitCode == 0);
    REQUIRE(std::filesystem::exists(planPath));

    const auto planText = ReadTextFile(planPath);
    const auto baseHeadSha = ExtractJsonStringField(planText, "base_head_sha");
    const auto dirtyFingerprint = ExtractJsonStringField(planText, "dirty_fingerprint");
    REQUIRE_FALSE(baseHeadSha.empty());
    REQUIRE_FALSE(dirtyFingerprint.empty());
    REQUIRE(baseHeadSha.find("replace-with-") == std::string::npos);
    REQUIRE(dirtyFingerprint.find("replace-with-") == std::string::npos);
    REQUIRE(baseHeadSha.find("ws-head-v2-") == 0);

    const auto verify = RunKog({"plan", "verify", "pre-apply", "--stage", "all", "--plan-file", planPath.string()}, ctx.cloneRepo);
    INFO(verify.stdoutText);
    INFO(verify.stderrText);
    REQUIRE(verify.exitCode == 0);
    RemoveSandboxWorkspace(ctx.sandbox);
}

TEST_CASE("plan_verify_pre_apply_detects_dirty_fingerprint_drift", "[functional][plan][freshness]") {
    const auto ctx = CreateRemoteWithClone("plan-dirty-drift");
    WriteTextFile(ctx.cloneRepo / "README.md", "seed\nplan dirty\n");
    const auto planPath = (ctx.cloneRepo / ".kano" / "cache" / "git" / "plans" / "default-plan.json").lexically_normal();
    RequireSuccess(RunKog({"plan", "new", "--force", "--output", planPath.string()}, ctx.cloneRepo), "plan new");

    WriteTextFile(ctx.cloneRepo / "extra.txt", "new drift\n");
    const auto verify = RunKog({"plan", "verify", "pre-apply", "--stage", "all", "--plan-file", planPath.string()}, ctx.cloneRepo);
    INFO(verify.stdoutText);
    INFO(verify.stderrText);
    REQUIRE(verify.exitCode != 0);
    const auto merged = verify.stdoutText + "\n" + verify.stderrText;
    REQUIRE(merged.find("workspace state drift detected") != std::string::npos);
    REQUIRE(merged.find("plan.dirty_fingerprint=") != std::string::npos);
    REQUIRE(merged.find("current.dirty_fingerprint=") != std::string::npos);
    RemoveSandboxWorkspace(ctx.sandbox);
}

TEST_CASE("plan_verify_pre_apply_detects_base_head_sha_drift", "[functional][plan][freshness]") {
    const auto ctx = CreateRemoteWithClone("plan-head-drift");
    const auto planPath = (ctx.cloneRepo / ".kano" / "cache" / "git" / "plans" / "default-plan.json").lexically_normal();
    RequireSuccess(RunKog({"plan", "new", "--force", "--output", planPath.string()}, ctx.cloneRepo), "plan new");

    WriteTextFile(ctx.cloneRepo / "head-drift.txt", "head drift\n");
    RequireSuccess(RunGit({"add", "head-drift.txt"}, ctx.cloneRepo), "head drift add");
    RequireSuccess(RunGit({"commit", "-m", "head drift commit"}, ctx.cloneRepo), "head drift commit");

    const auto verify = RunKog({"plan", "verify", "pre-apply", "--stage", "all", "--plan-file", planPath.string()}, ctx.cloneRepo);
    INFO(verify.stdoutText);
    INFO(verify.stderrText);
    REQUIRE(verify.exitCode != 0);
    const auto merged = verify.stdoutText + "\n" + verify.stderrText;
    REQUIRE(merged.find("workspace state drift detected") != std::string::npos);
    REQUIRE(merged.find("plan.base_head_sha=") != std::string::npos);
    REQUIRE(merged.find("current.base_head_sha=") != std::string::npos);
    RemoveSandboxWorkspace(ctx.sandbox);
}

TEST_CASE("commit_push_secret_gate_blocks_high_confidence_finding", "[functional][secret-gate][commit-push]") {
    const auto ctx = CreateRemoteWithClone("secret-gate-block");
    const auto secretPayload = std::string("OPENAI_API_KEY=\"") + "sk-" + std::string("ABCDEFGHIJKLMNOPQRSTUVWXYZ12") + "\"\n";
    WriteTextFile(ctx.cloneRepo / "secrets.txt", secretPayload);

    const auto result = RunKog({"commit-push", "-m", "test(functional): secret gate"}, ctx.cloneRepo);
    INFO(result.stdoutText);
    INFO(result.stderrText);
    REQUIRE(result.exitCode != 0);
    const auto merged = result.stdoutText + "\n" + result.stderrText;
    REQUIRE(merged.find("secret gate failed") != std::string::npos);
    REQUIRE(merged.find("OPENAI_API_KEY") != std::string::npos);
    RemoveSandboxWorkspace(ctx.sandbox);
}

TEST_CASE("commit_push_secret_gate_allows_intentional_placeholder_examples", "[functional][secret-gate][commit-push]") {
    const auto ctx = CreateRemoteWithClone("secret-gate-placeholder");
    WriteTextFile(
        ctx.cloneRepo / "docs" / "configuration.md",
        "# Configuration\n\n"
        "export GEMINI_API_KEY=\"your-api-key-here\"\n");

    const auto result = RunKog({"commit-push", "-m", "docs(functional): add placeholder example"}, ctx.cloneRepo);
    INFO(result.stdoutText);
    INFO(result.stderrText);
    REQUIRE(result.exitCode == 0);

    const auto merged = result.stdoutText + "\n" + result.stderrText;
    REQUIRE(merged.find("secret gate failed") == std::string::npos);
    const auto [behind, ahead] = AheadBehindCounts(ctx.cloneRepo);
    REQUIRE(behind == 0);
    REQUIRE(ahead == 0);
    RemoveSandboxWorkspace(ctx.sandbox);
}

TEST_CASE("commit_push_secret_gate_can_be_disabled_explicitly", "[functional][secret-gate][commit-push]") {
    const auto ctx = CreateRemoteWithClone("secret-gate-disabled");
    const auto secretPayload = std::string("OPENAI_API_KEY=\"") + "sk-" + std::string("ABCDEFGHIJKLMNOPQRSTUVWXYZ12") + "\"\n";
    WriteTextFile(ctx.cloneRepo / "secrets.txt", secretPayload);

    const auto result =
        RunKogWithEnv({"commit-push", "-m", "test(functional): secret gate disabled"},
                      ctx.cloneRepo,
                      {{"KOG_DISABLE_SECRET_GATE", "1"}});
    INFO(result.stdoutText);
    INFO(result.stderrText);
    REQUIRE(result.exitCode == 0);
    const auto [behind, ahead] = AheadBehindCounts(ctx.cloneRepo);
    REQUIRE(behind == 0);
    REQUIRE(ahead == 0);
    RemoveSandboxWorkspace(ctx.sandbox);
}

TEST_CASE("reset_stable_remote_fetches_and_attaches_detached_registered_submodule", "[functional][reset][stable-remote]") {
    const auto ctx = CreateRemoteWithSubmoduleClone("reset-stable-remote-detached", "branch_v1.0.0");
    REQUIRE(CurrentBranch(ctx.cloneChildRepo).empty());

    WriteTextFile(ctx.childSeedRepo / "child.txt", "child advanced on remote\n");
    RequireSuccess(RunGit({"add", "child.txt"}, ctx.childSeedRepo), "child remote add");
    RequireSuccess(RunGit({"commit", "-m", "child remote advance"}, ctx.childSeedRepo), "child remote commit");
    RequireSuccess(RunGit({"push"}, ctx.childSeedRepo), "child remote push");
    const auto expectedChildHead = CurrentHeadSha(ctx.childSeedRepo);

    WriteTextFile(ctx.cloneChildRepo / "child.txt", "dirty local child change\n");
    WriteTextFile(ctx.cloneChildRepo / "scratch.txt", "remove me\n");

    const auto result = RunKog({"reset", "stable-remote"}, ctx.cloneRootRepo);
    INFO(result.stdoutText);
    INFO(result.stderrText);
    REQUIRE(result.exitCode == 0);
    REQUIRE(CurrentBranch(ctx.cloneChildRepo) == ctx.branch);
    REQUIRE(CurrentHeadSha(ctx.cloneChildRepo) == expectedChildHead);
    REQUIRE_FALSE(std::filesystem::exists(ctx.cloneChildRepo / "scratch.txt"));
    REQUIRE(StatusPorcelain(ctx.cloneChildRepo).empty());
    REQUIRE(result.stdoutText.find("registered .gitmodules stable branch") != std::string::npos);

    RemoveSandboxWorkspace(ctx.sandbox);
}

TEST_CASE("reset_stable_remote_does_not_fallback_to_non_stable_remote_branch", "[functional][reset][stable-remote]") {
    const auto ctx = CreateRemoteWithClone("reset-stable-remote-no-fallback");
    const auto beforeHead = CurrentHeadSha(ctx.cloneRepo);

    const auto result = RunKog({"reset", "stable-remote", "--no-recursive"}, ctx.cloneRepo);
    INFO(result.stdoutText);
    INFO(result.stderrText);
    REQUIRE(result.exitCode != 0);

    const auto merged = result.stdoutText + "\n" + result.stderrText;
    REQUIRE(merged.find("could not resolve target reset ref") != std::string::npos);
    REQUIRE(CurrentBranch(ctx.cloneRepo) == ctx.branch);
    REQUIRE(CurrentHeadSha(ctx.cloneRepo) == beforeHead);

    RemoveSandboxWorkspace(ctx.sandbox);
}

TEST_CASE("repo_status_table_adapts_repo_column_to_terminal_width", "[functional][status][table-width]") {
    const auto sandbox = CreateSandboxWorkspace("status-table-width");
    const auto longRepo = (sandbox.root / "repository-name-that-should-expand-with-terminal-width").lexically_normal();

    RequireSuccess(RunGit({"init", longRepo.string()}, sandbox.root), "init long-name repo");

    const auto narrow = RunKogWithEnv(
        {"repo", "status", longRepo.string(), "--repo-root", sandbox.root.string()},
        sandbox.root,
        {{"COLUMNS", "80"}});
    INFO(narrow.stdoutText);
    INFO(narrow.stderrText);
    REQUIRE(narrow.exitCode == 0);

    const auto wide = RunKogWithEnv(
        {"repo", "status", longRepo.string(), "--repo-root", sandbox.root.string()},
        sandbox.root,
        {{"COLUMNS", "140"}});
    INFO(wide.stdoutText);
    INFO(wide.stderrText);
    REQUIRE(wide.exitCode == 0);

    const auto repoName = longRepo.filename().string();
    REQUIRE(narrow.stdoutText.find(repoName) == std::string::npos);
    REQUIRE(wide.stdoutText.find(repoName) != std::string::npos);
    REQUIRE(LongestLineLength(narrow.stdoutText) <= 80);

    RemoveSandboxWorkspace(sandbox);
}

TEST_CASE("detached_head_recoverable_converges", "[functional][commit-push][detached-head]") {
    const auto ctx = CreateRemoteWithClone("detached-recoverable");
    const auto head = CurrentHeadSha(ctx.cloneRepo);
    RequireSuccess(RunGit({"checkout", head}, ctx.cloneRepo), "detach HEAD");
    REQUIRE(CurrentBranch(ctx.cloneRepo).empty());

    const auto result = RunKog({"commit-push"}, ctx.cloneRepo);
    INFO(result.stdoutText);
    INFO(result.stderrText);
    REQUIRE(result.exitCode == 0);
    REQUIRE_FALSE(ContainsRawCheckoutChatter(result.stdoutText + "\n" + result.stderrText));
    REQUIRE(CurrentBranch(ctx.cloneRepo) == ctx.branch);
    RemoveSandboxWorkspace(ctx.sandbox);
}

TEST_CASE("commit_push_success_path_does_not_emit_checkout_chatter", "[functional][commit-push][output]") {
    const auto ctx = CreateRemoteWithClone("commit-push-quiet-output");
    WriteTextFile(ctx.cloneRepo / "README.md", "seed\nquiet output check\n");

    const auto result = RunKog({"commit-push", "-m", "test: quiet output path"}, ctx.cloneRepo);
    INFO(result.stdoutText);
    INFO(result.stderrText);
    REQUIRE(result.exitCode == 0);
    REQUIRE_FALSE(ContainsRawCheckoutChatter(result.stdoutText + "\n" + result.stderrText));
    const auto [behind, ahead] = AheadBehindCounts(ctx.cloneRepo);
    REQUIRE(behind == 0);
    REQUIRE(ahead == 0);
    RemoveSandboxWorkspace(ctx.sandbox);
}

TEST_CASE("sync_reports_index_lock_path_and_hint_when_auto_stash_hits_lock", "[functional][sync][locks]") {
    const auto ctx = CreateRemoteWithClone("sync-lock-diagnose");
    WriteTextFile(ctx.cloneRepo / "README.md", "seed\nlock diagnose\n");
    const auto lockPath = (ctx.cloneRepo / ".git" / "index.lock").lexically_normal();
    TouchFile(lockPath);

    const auto result = RunKogWithEnv(
        {"sync", "origin-latest", "--no-recursive"},
        ctx.cloneRepo,
        {{"KOG_SYNC_TEST_ASSUME_ACTIVE_GIT_PROCESS", "0"}});
    INFO(result.stdoutText);
    INFO(result.stderrText);
    REQUIRE(result.exitCode != 0);
    const auto merged = result.stdoutText + "\n" + result.stderrText;
    REQUIRE(merged.find("git index lock detected for .") != std::string::npos);
    REQUIRE(merged.find("index.lock:") != std::string::npos);
    REQUIRE(merged.find(".git/index.lock") != std::string::npos);
    REQUIRE(merged.find("lock_last_write_age_seconds:") != std::string::npos);
    REQUIRE(merged.find("active_git_process:") != std::string::npos);
    REQUIRE(merged.find("--cleanup-stale-locks") != std::string::npos);
    REQUIRE(std::filesystem::exists(lockPath));

    RemoveSandboxWorkspace(ctx.sandbox);
}

TEST_CASE("sync_cleanup_stale_locks_recovers_when_no_git_process_detected", "[functional][sync][locks]") {
    const auto ctx = CreateRemoteWithClone("sync-lock-cleanup");
    WriteTextFile(ctx.cloneRepo / "README.md", "seed\nlock cleanup\n");
    const auto lockPath = (ctx.cloneRepo / ".git" / "index.lock").lexically_normal();
    TouchFile(lockPath);
    SetFileAgeSeconds(lockPath, 10);

    const auto result = RunKogWithEnv(
        {"sync", "origin-latest", "--no-recursive", "--cleanup-stale-locks"},
        ctx.cloneRepo,
        {{"KOG_SYNC_TEST_ASSUME_ACTIVE_GIT_PROCESS", "0"}});
    INFO(result.stdoutText);
    INFO(result.stderrText);
    REQUIRE(result.exitCode == 0);
    const auto merged = result.stdoutText + "\n" + result.stderrText;
    REQUIRE(merged.find("Removed stale index.lock for .") != std::string::npos);
    REQUIRE_FALSE(std::filesystem::exists(lockPath));
    const auto [behind, ahead] = AheadBehindCounts(ctx.cloneRepo);
    REQUIRE(behind == 0);
    REQUIRE(ahead == 0);

    RemoveSandboxWorkspace(ctx.sandbox);
}

TEST_CASE("sync_ignores_windows_reserved_paths_during_auto_stash", "[functional][sync][windows]") {
#if defined(_WIN32)
    const auto ctx = CreateRemoteWithClone("sync-windows-reserved-path");
    WriteTextFile(ctx.cloneRepo / "README.md", "seed\nreal local change\n");

    const auto result = RunKogWithEnv(
        {"sync", "origin-latest", "--no-recursive"},
        ctx.cloneRepo,
        {{"KOG_SYNC_TEST_RESERVED_STATUS_PATHS", "NUL"}});
    INFO(result.stdoutText);
    INFO(result.stderrText);
    REQUIRE(result.exitCode == 0);

    const auto merged = result.stdoutText + "\n" + result.stderrText;
    REQUIRE(merged.find("Auto-stashed local changes for .") != std::string::npos);
    REQUIRE(merged.find("Restored auto-stash for .") != std::string::npos);

    RemoveSandboxWorkspace(ctx.sandbox);
#else
    SUCCEED("Windows-only reserved path regression test");
#endif
}

TEST_CASE("sync_runs_self_cpp_build_when_self_repo_cpp_changes_arrive", "[functional][sync][self-build]") {
    const auto ctx = CreateRemoteWithClone("sync-self-cpp-build");
    SeedSelfBuildScaffolding(ctx);

    WriteTextFile(ctx.seedRepo / "src/cpp/code/demo.cpp", "int main() { return 42; }\n");
    RequireSuccess(RunGit({"add", "src/cpp/code/demo.cpp"}, ctx.seedRepo), "seed cpp add");
    RequireSuccess(RunGit({"commit", "-m", "seed cpp update"}, ctx.seedRepo), "seed cpp commit");
    RequireSuccess(RunGit({"push", "origin", ctx.branch}, ctx.seedRepo), "seed cpp push");

    const auto result = RunKogWithEnv(
        {"sync", "origin-latest", "--no-recursive"},
        ctx.cloneRepo,
        {{"KANO_GIT_MASTER_ROOT", ctx.cloneRepo.string()}});
    INFO(result.stdoutText);
    INFO(result.stderrText);
    REQUIRE(result.exitCode == 0);
    REQUIRE(ReadTextFile(ctx.cloneRepo / ".kano-self-build-ran") == "built\n");
    const auto [behind, ahead] = AheadBehindCounts(ctx.cloneRepo);
    REQUIRE(behind == 0);
    REQUIRE(ahead == 0);

    RemoveSandboxWorkspace(ctx.sandbox);
}

TEST_CASE("detached_head_unrecoverable_fails_explicitly", "[functional][commit-push][detached-head]") {
    const auto ctx = CreateRemoteWithClone("detached-unrecoverable", "weirdbranch");
    const auto head = CurrentHeadSha(ctx.cloneRepo);
    RequireSuccess(RunGit({"checkout", head}, ctx.cloneRepo), "detach HEAD");
    RequireSuccess(RunGit({"update-ref", "-d", ("refs/heads/" + ctx.branch)}, ctx.bareRemote), "delete bare remote branch");
    RequireSuccess(RunGit({"branch", "-D", ctx.branch}, ctx.cloneRepo), "delete local branch");
    RequireSuccess(RunGit({"update-ref", "-d", "refs/remotes/origin/HEAD"}, ctx.cloneRepo), "delete origin HEAD ref");
    RequireSuccess(RunGit({"update-ref", "-d", ("refs/remotes/origin/" + ctx.branch)}, ctx.cloneRepo), "delete origin branch ref");
    const auto remoteRefCheck = RunGit({"show-ref", "--verify", "--quiet", ("refs/remotes/origin/" + ctx.branch)}, ctx.cloneRepo);
    REQUIRE(remoteRefCheck.exitCode != 0);
    REQUIRE(CurrentBranch(ctx.cloneRepo).empty());

    const auto result = RunKog({"commit-push"}, ctx.cloneRepo);
    INFO(result.stdoutText);
    INFO(result.stderrText);
    REQUIRE(result.exitCode != 0);
    const auto merged = result.stdoutText + "\n" + result.stderrText;
    REQUIRE(merged.find("detached HEAD") != std::string::npos);
    REQUIRE(CurrentBranch(ctx.cloneRepo).empty());
    RemoveSandboxWorkspace(ctx.sandbox);
}

} // namespace kano::git::tests::functional
