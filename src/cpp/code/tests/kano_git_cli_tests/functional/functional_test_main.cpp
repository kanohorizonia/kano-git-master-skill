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

struct RecursiveSubmoduleUpdateContext {
    SandboxContext sandbox;
    std::filesystem::path nestedBareRemote;
    std::filesystem::path nestedSeedRepo;
    std::filesystem::path healthyBareRemote;
    std::filesystem::path healthySeedRepo;
    std::filesystem::path brokenBareRemote;
    std::filesystem::path brokenSeedRepo;
    std::filesystem::path rootBareRemote;
    std::filesystem::path rootSeedRepo;
    std::filesystem::path cloneRootRepo;
    std::string branch;
    std::string healthyPath;
    std::string brokenPath;
    std::string nestedPathWithinHealthy;
    std::string nestedPathFromRoot;
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


auto RequireContainsText(const std::string& InText, const std::string& InNeedle) -> void {
    INFO("missing needle=" << InNeedle);
    INFO(InText);
    REQUIRE(InText.find(InNeedle) != std::string::npos);
}

auto RequireNotContainsText(const std::string& InText, const std::string& InNeedle) -> void {
    INFO("unexpected needle=" << InNeedle);
    INFO(InText);
    REQUIRE(InText.find(InNeedle) == std::string::npos);
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
    RequireSuccess(RunGit({"config", "core.hooksPath", "/dev/null"}, InRepo), "disable hooks in fixture repo");
}

auto SeedSelfBuildScaffolding(const RemoteCloneContext& InCtx) -> void {
    WriteTextFile(InCtx.seedRepo / "scripts/kano-git", "#!/usr/bin/env bash\nset -euo pipefail\n");

    const std::string buildScript = "#!/usr/bin/env bash\nset -euo pipefail\nprintf 'built\\n' > .kano-self-build-ran\nexit 0\n";
    WriteTextFile(InCtx.seedRepo / "src/cpp/scripts/self/build.sh", buildScript);
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
    const auto batPath = (stubDir / "codex.bat").lexically_normal();
    const auto shPath = (stubDir / "codex").lexically_normal();

    std::ostringstream script;
    script << "$capture = " << '"' << InCapturePath.string() << "\"\n";
    script << "$logPath = [Environment]::GetEnvironmentVariable('KOG_TEST_AI_STUB_LOG')\n";
    script << "if ([string]::IsNullOrWhiteSpace($logPath)) { $logPath = '" << (stubDir / "provider-invocations.log").string() << "' }\n";
    script << "$output = $null\n";
    script << "Add-Content -LiteralPath $logPath -Value ('codex ' + ($args -join ' '))\n";
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
    WriteTextFile(batPath, cmd.str());

    std::ostringstream sh;
    sh << "#!/usr/bin/env bash\n";
    sh << "log_path=\"${KOG_TEST_AI_STUB_LOG-}\"\n";
    sh << "if [[ -z \"$log_path\" ]]; then log_path='" << (stubDir / "provider-invocations.log").generic_string() << "'; fi\n";
    sh << "printf '%s\n' \"codex $*\" >> \"$log_path\"\n";
    sh << "output=\"\"\n";
    sh << "for ((i=1; i<=$#; i++)); do\n";
    sh << "  if [[ \"${!i}\" == \"-o\" ]]; then\n";
    sh << "    j=$((i+1))\n";
    sh << "    if [[ $j -le $# ]]; then output=\"${!j}\"; fi\n";
    sh << "  fi\n";
    sh << "done\n";
    sh << "if [[ $# -gt 0 ]]; then printf '%s' \"${!#}\" > '" << InCapturePath.generic_string() << "'; fi\n";
    sh << "if [[ -n \"$output\" ]]; then printf '[]' > \"$output\"; fi\n";
    sh << "exit 0\n";
    WriteTextFile(shPath, sh.str());
    return stubDir;
}

auto ResolveProviderCommands(const std::filesystem::path& InRepo) -> CommandResult {
    const std::string probe =
        "which copilot || true; where.exe copilot || true; command -v copilot || true; "
        "which gh || true; where.exe gh || true; command -v gh || true; "
        "which codex || true; where.exe codex || true; command -v codex || true";
    return RunCommand("bash", {"-lc", probe}, InRepo);
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

auto InstallCopilotStub(const std::filesystem::path& InDir) -> std::filesystem::path {
    const auto stubDir = (InDir / "fake-copilot-bin").lexically_normal();
    std::filesystem::create_directories(stubDir);
    const auto scriptPath = (stubDir / "copilot-stub.ps1").lexically_normal();
    const auto cmdPath = (stubDir / "copilot.cmd").lexically_normal();
    const auto batPath = (stubDir / "copilot.bat").lexically_normal();
    const auto shPath = (stubDir / "copilot").lexically_normal();
    const auto ghScriptPath = (stubDir / "gh-stub.ps1").lexically_normal();
    const auto ghCmdPath = (stubDir / "gh.cmd").lexically_normal();
    const auto ghBatPath = (stubDir / "gh.bat").lexically_normal();
    const auto ghShPath = (stubDir / "gh").lexically_normal();

    std::ostringstream script;
    script << "$logPath = [Environment]::GetEnvironmentVariable('KOG_TEST_AI_STUB_LOG')\n";
    script << "if ([string]::IsNullOrWhiteSpace($logPath)) { $logPath = '" << (stubDir / "provider-invocations.log").string() << "' }\n";
    script << "Add-Content -LiteralPath $logPath -Value ('copilot ' + ($args -join ' '))\n";
    script << "$stdout = [Environment]::GetEnvironmentVariable('KOG_TEST_AI_STDOUT')\n";
    script << "if ($null -ne $stdout) { [Console]::Out.Write($stdout) }\n";
    script << "$exitCode = [Environment]::GetEnvironmentVariable('KOG_TEST_AI_EXIT_CODE')\n";
    script << "if ([string]::IsNullOrWhiteSpace($exitCode)) { exit 0 }\n";
    script << "exit [int]$exitCode\n";
    WriteTextFile(scriptPath, script.str());

    std::ostringstream cmd;
    cmd << "@echo off\r\n";
    cmd << "powershell -NoProfile -ExecutionPolicy Bypass -File \"%~dp0copilot-stub.ps1\" %*\r\n";
    WriteTextFile(cmdPath, cmd.str());
    WriteTextFile(batPath, cmd.str());

    std::ostringstream sh;
    sh << "#!/usr/bin/env bash\n";
    sh << "log_path=\"${KOG_TEST_AI_STUB_LOG-}\"\n";
    sh << "if [[ -z \"$log_path\" ]]; then log_path='" << (stubDir / "provider-invocations.log").generic_string() << "'; fi\n";
    sh << "printf '%s\n' \"copilot $*\" >> \"$log_path\"\n";
    sh << "stdout=\"${KOG_TEST_AI_STDOUT-}\"\n";
    sh << "if [[ -n \"$stdout\" ]]; then printf '%s' \"$stdout\"; fi\n";
    sh << "exit_code=\"${KOG_TEST_AI_EXIT_CODE-}\"\n";
    sh << "if [[ -z \"$exit_code\" ]]; then exit 0; fi\n";
    sh << "exit \"$exit_code\"\n";
    WriteTextFile(shPath, sh.str());

    std::ostringstream ghScript;
    ghScript << "$argList = @($args)\n";
    ghScript << "$logPath = [Environment]::GetEnvironmentVariable('KOG_TEST_AI_STUB_LOG')\n";
    ghScript << "if ([string]::IsNullOrWhiteSpace($logPath)) { $logPath = '" << (stubDir / "provider-invocations.log").string() << "' }\n";
    ghScript << "Add-Content -LiteralPath $logPath -Value ('gh ' + ($argList -join ' '))\n";
    ghScript << "if ($argList.Length -ge 2 -and $argList[0] -eq 'copilot' -and $argList[1] -eq '--version') {\n";
    ghScript << "  [Console]::Out.Write('gh-copilot-stub 1.0')\n";
    ghScript << "  exit 0\n";
    ghScript << "}\n";
    ghScript << "if ($argList.Length -ge 1 -and $argList[0] -eq 'copilot') {\n";
    ghScript << "  $stdout = [Environment]::GetEnvironmentVariable('KOG_TEST_AI_STDOUT')\n";
    ghScript << "  if ($null -ne $stdout) { [Console]::Out.Write($stdout) }\n";
    ghScript << "  $exitCode = [Environment]::GetEnvironmentVariable('KOG_TEST_AI_EXIT_CODE')\n";
    ghScript << "  if ([string]::IsNullOrWhiteSpace($exitCode)) { exit 0 }\n";
    ghScript << "  exit [int]$exitCode\n";
    ghScript << "}\n";
    ghScript << "exit 1\n";
    WriteTextFile(ghScriptPath, ghScript.str());

    std::ostringstream ghCmd;
    ghCmd << "@echo off\r\n";
    ghCmd << "powershell -NoProfile -ExecutionPolicy Bypass -File \"%~dp0gh-stub.ps1\" %*\r\n";
    WriteTextFile(ghCmdPath, ghCmd.str());
    WriteTextFile(ghBatPath, ghCmd.str());

    std::ostringstream ghSh;
    ghSh << "#!/usr/bin/env bash\n";
    ghSh << "log_path=\"${KOG_TEST_AI_STUB_LOG-}\"\n";
    ghSh << "if [[ -z \"$log_path\" ]]; then log_path='" << (stubDir / "provider-invocations.log").generic_string() << "'; fi\n";
    ghSh << "printf '%s\n' \"gh $*\" >> \"$log_path\"\n";
    ghSh << "if [[ \"${1-}\" == \"copilot\" && \"${2-}\" == \"--version\" ]]; then\n";
    ghSh << "  printf 'gh-copilot-stub 1.0'\n";
    ghSh << "  exit 0\n";
    ghSh << "fi\n";
    ghSh << "if [[ \"${1-}\" == \"copilot\" ]]; then\n";
    ghSh << "  stdout=\"${KOG_TEST_AI_STDOUT-}\"\n";
    ghSh << "  if [[ -n \"$stdout\" ]]; then printf '%s' \"$stdout\"; fi\n";
    ghSh << "  exit_code=\"${KOG_TEST_AI_EXIT_CODE-}\"\n";
    ghSh << "  if [[ -z \"$exit_code\" ]]; then exit 0; fi\n";
    ghSh << "  exit \"$exit_code\"\n";
    ghSh << "fi\n";
    ghSh << "exit 1\n";
    WriteTextFile(ghShPath, ghSh.str());
    return stubDir;
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
    WriteTextFile(ctx.seedRepo / ".gitattributes", "*.sh text eol=lf\n");
    WriteTextFile(ctx.seedRepo / ".gitignore", ".kano/\n");
    WriteTextFile(ctx.seedRepo / "README.md", "seed\n");
    WriteTextFile(ctx.seedRepo / "src/shell/test/pre-commit-quality-gate.sh", "#!/usr/bin/env bash\nset -euo pipefail\nexit 0\n");
    RequireSuccess(
        RunGit({"add", ".gitattributes", ".gitignore", "README.md", "src/shell/test/pre-commit-quality-gate.sh"}, ctx.seedRepo),
        "seed add");
    RequireSuccess(
        RunGit({"update-index", "--chmod=+x", "src/shell/test/pre-commit-quality-gate.sh"}, ctx.seedRepo),
        "seed mark quality gate executable");
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

auto CreateRecursiveSubmoduleUpdateClone(const std::string& InName, const std::string& InBranch = "main") -> RecursiveSubmoduleUpdateContext {
    RecursiveSubmoduleUpdateContext ctx;
    ctx.sandbox = CreateSandboxWorkspace(InName);
    ctx.nestedBareRemote = (ctx.sandbox.root / "nested-remote.git").lexically_normal();
    ctx.nestedSeedRepo = (ctx.sandbox.root / "nested-seed").lexically_normal();
    ctx.healthyBareRemote = (ctx.sandbox.root / "healthy-remote.git").lexically_normal();
    ctx.healthySeedRepo = (ctx.sandbox.root / "healthy-seed").lexically_normal();
    ctx.brokenBareRemote = (ctx.sandbox.root / "broken-remote.git").lexically_normal();
    ctx.brokenSeedRepo = (ctx.sandbox.root / "broken-seed").lexically_normal();
    ctx.rootBareRemote = (ctx.sandbox.root / "root-remote.git").lexically_normal();
    ctx.rootSeedRepo = (ctx.sandbox.root / "root-seed").lexically_normal();
    ctx.cloneRootRepo = (ctx.sandbox.root / "root-clone").lexically_normal();
    ctx.branch = InBranch;
    ctx.healthyPath = "deps/healthy";
    ctx.brokenPath = "deps/broken";
    ctx.nestedPathWithinHealthy = "vendor/grandchild";
    ctx.nestedPathFromRoot = ctx.healthyPath + "/" + ctx.nestedPathWithinHealthy;

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

    RequireSuccess(RunGit({"init", "--bare", ctx.healthyBareRemote.string()}, ctx.sandbox.root), "init healthy bare");
    RequireSuccess(RunGit({"init", ctx.healthySeedRepo.string()}, ctx.sandbox.root), "init healthy seed");
    ConfigureIdentity(ctx.healthySeedRepo);
    RequireSuccess(RunGit({"checkout", "-b", ctx.branch}, ctx.healthySeedRepo), "checkout healthy branch");
    WriteTextFile(ctx.healthySeedRepo / "healthy.txt", "healthy seed\n");
    RequireSuccess(RunGit({"add", "healthy.txt"}, ctx.healthySeedRepo), "healthy add base");
    RequireSuccess(RunGit({"commit", "-m", "healthy seed"}, ctx.healthySeedRepo), "healthy base commit");
    RequireSuccess(
        RunGit({"-c", "protocol.file.allow=always", "submodule", "add", "-b", ctx.branch, ctx.nestedBareRemote.string(), ctx.nestedPathWithinHealthy},
               ctx.healthySeedRepo),
        "healthy add nested submodule");
    RequireSuccess(RunGit({"commit", "-am", "add nested submodule"}, ctx.healthySeedRepo), "healthy commit nested submodule");
    RequireSuccess(RunGit({"remote", "add", "origin", ctx.healthyBareRemote.string()}, ctx.healthySeedRepo), "healthy add remote");
    RequireSuccess(RunGit({"push", "-u", "origin", ctx.branch}, ctx.healthySeedRepo), "healthy push");
    RequireSuccess(RunGit({"symbolic-ref", "HEAD", ("refs/heads/" + ctx.branch)}, ctx.healthyBareRemote), "healthy bare HEAD");

    RequireSuccess(RunGit({"init", "--bare", ctx.brokenBareRemote.string()}, ctx.sandbox.root), "init broken bare");
    RequireSuccess(RunGit({"init", ctx.brokenSeedRepo.string()}, ctx.sandbox.root), "init broken seed");
    ConfigureIdentity(ctx.brokenSeedRepo);
    RequireSuccess(RunGit({"checkout", "-b", ctx.branch}, ctx.brokenSeedRepo), "checkout broken branch");
    WriteTextFile(ctx.brokenSeedRepo / "broken.txt", "broken seed\n");
    RequireSuccess(RunGit({"add", "broken.txt"}, ctx.brokenSeedRepo), "broken add");
    RequireSuccess(RunGit({"commit", "-m", "broken seed"}, ctx.brokenSeedRepo), "broken commit");
    RequireSuccess(RunGit({"remote", "add", "origin", ctx.brokenBareRemote.string()}, ctx.brokenSeedRepo), "broken add remote");
    RequireSuccess(RunGit({"push", "-u", "origin", ctx.branch}, ctx.brokenSeedRepo), "broken push");
    RequireSuccess(RunGit({"symbolic-ref", "HEAD", ("refs/heads/" + ctx.branch)}, ctx.brokenBareRemote), "broken bare HEAD");

    RequireSuccess(RunGit({"init", "--bare", ctx.rootBareRemote.string()}, ctx.sandbox.root), "init root bare");
    RequireSuccess(RunGit({"init", ctx.rootSeedRepo.string()}, ctx.sandbox.root), "init root seed");
    ConfigureIdentity(ctx.rootSeedRepo);
    RequireSuccess(RunGit({"checkout", "-b", ctx.branch}, ctx.rootSeedRepo), "checkout root branch");
    WriteTextFile(ctx.rootSeedRepo / ".gitignore", ".kano/\n");
    WriteTextFile(ctx.rootSeedRepo / "README.md", "root seed\n");
    RequireSuccess(RunGit({"add", ".gitignore", "README.md"}, ctx.rootSeedRepo), "root add base");
    RequireSuccess(RunGit({"commit", "-m", "root seed"}, ctx.rootSeedRepo), "root base commit");
    RequireSuccess(
        RunGit({"-c", "protocol.file.allow=always", "submodule", "add", "-b", ctx.branch, ctx.healthyBareRemote.string(), ctx.healthyPath},
               ctx.rootSeedRepo),
        "root add healthy submodule");
    RequireSuccess(
        RunGit({"-c", "protocol.file.allow=always", "submodule", "add", "-b", ctx.branch, ctx.brokenBareRemote.string(), ctx.brokenPath},
               ctx.rootSeedRepo),
        "root add broken submodule");
    RequireSuccess(RunGit({"commit", "-am", "add submodules"}, ctx.rootSeedRepo), "root commit submodules");

    const auto missingBrokenRemote = (ctx.sandbox.root / "missing-broken-remote.git").lexically_normal();
    RequireSuccess(
        RunGit({"config", "-f", ".gitmodules", ("submodule." + ctx.brokenPath + ".url"), missingBrokenRemote.string()}, ctx.rootSeedRepo),
        "rewrite broken submodule url");
    RequireSuccess(RunGit({"add", ".gitmodules"}, ctx.rootSeedRepo), "stage broken submodule url");
    RequireSuccess(RunGit({"commit", "-m", "break broken submodule url"}, ctx.rootSeedRepo), "commit broken submodule url");
    RequireSuccess(RunGit({"remote", "add", "origin", ctx.rootBareRemote.string()}, ctx.rootSeedRepo), "root add remote");
    RequireSuccess(RunGit({"push", "-u", "origin", ctx.branch}, ctx.rootSeedRepo), "root push");
    RequireSuccess(RunGit({"symbolic-ref", "HEAD", ("refs/heads/" + ctx.branch)}, ctx.rootBareRemote), "root bare HEAD");

    RequireSuccess(
        RunGit({"clone", ctx.rootBareRemote.string(), ctx.cloneRootRepo.string()}, ctx.sandbox.root),
        "clone root without submodules");
    ConfigureIdentity(ctx.cloneRootRepo);
    RequireSuccess(
        RunGit({"config", "kano.cache.local-dir", (ctx.sandbox.root / "_cache").string()}, ctx.cloneRootRepo),
        "configure root external kano cache");
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

TEST_CASE("commit_push_plan_file_keeps_exact_include_scope", "[functional][commit-push][plan-file][pathspec]") {
    const auto ctx = CreateRemoteWithClone("plan-file-exact-include");
    WriteTextFile(ctx.cloneRepo / "included.txt", "include me\n");
    WriteTextFile(ctx.cloneRepo / "unrelated.txt", "password: \"supersecretvalue\"\n");

    const auto planPath = (ctx.cloneRepo / ".kano" / "cache" / "git" / "plans" / "exact-include.json").lexically_normal();
    RequireSuccess(RunKog({"plan", "new", "--force", "--output", planPath.string()}, ctx.cloneRepo), "plan new");
    RequireSuccess(
        RunKog({
            "plan", "prepare", "add-commit-entry",
            "--plan-file", planPath.string(),
            "--repo", ".",
            "--commit-message", "test(functional): exact include",
            "--commit-include", "included.txt",
            "--commit-review-verdict", "pass",
            "--commit-review-reason", "functional regression for plan-file exact include staging"
        }, ctx.cloneRepo),
        "plan add commit entry");
    RequireSuccess(
        RunKog({"plan", "verify", "pre-apply", "--stage", "commit", "--plan-file", planPath.string()}, ctx.cloneRepo),
        "plan verify pre-apply");

    const auto result = RunKog({"commit-push", "--plan-file", planPath.string()}, ctx.cloneRepo);
    INFO(result.stdoutText);
    INFO(result.stderrText);
    REQUIRE(result.exitCode == 0);
    RequireContainsText(result.stdoutText, "pre-commit skipped for explicit plan-file");
    RequireContainsText(result.stdoutText, "scoped safety gates checked files=1");

    const auto includedStatus = RunGit({"status", "--short", "--", "included.txt"}, ctx.cloneRepo);
    RequireSuccess(includedStatus, "included status");
    REQUIRE(TrimCopy(includedStatus.stdoutText).empty());

    const auto unrelatedStatus = RunGit({"status", "--short", "--", "unrelated.txt"}, ctx.cloneRepo);
    RequireSuccess(unrelatedStatus, "unrelated status");
    REQUIRE(TrimCopy(unrelatedStatus.stdoutText) == "?? unrelated.txt");

    const auto cached = RunGit({"diff", "--cached", "--name-only"}, ctx.cloneRepo);
    RequireSuccess(cached, "cached diff after plan commit-push");
    REQUIRE(TrimCopy(cached.stdoutText).empty());

    const auto [behind, ahead] = AheadBehindCounts(ctx.cloneRepo);
    REQUIRE(behind == 0);
    REQUIRE(ahead == 0);
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
    const auto stubDir = InstallCopilotStub(ctx.sandbox.root);
    WriteTextFile(ctx.cloneRepo / "README.md", "seed\namend ai reword\n");
    RequireSuccess(RunGit({"add", "README.md"}, ctx.cloneRepo), "stage amend ai change");
    RequireSuccess(RunGit({"commit", "-m", "chore: placeholder amend subject"}, ctx.cloneRepo), "seed amend target commit");
    const auto beforeHead = CurrentHeadSha(ctx.cloneRepo);

    std::vector<std::pair<std::string, std::string>> env{
        {"KOG_TEST_AI_STDOUT", "docs(readme): refine amend ai subject\n"},
        {"KOG_TEST_AI_EXIT_CODE", "0"},
    };
    env.emplace_back("PATH", stubDir.string() + ";" + (std::getenv("PATH") != nullptr ? std::getenv("PATH") : ""));

    const auto result = RunKogWithEnv(
        {"amend", "--ai-auto", "--ai-provider", "copilot", "--no-ai-review"},
        ctx.cloneRepo,
        env);
    INFO(result.stdoutText);
    INFO(result.stderrText);
    REQUIRE(result.exitCode == 0);
    REQUIRE(result.stdoutText.find("=== Native Amend Summary ===") != std::string::npos);
    REQUIRE(result.stdoutText.find("amended") != std::string::npos);
    REQUIRE(CurrentHeadSha(ctx.cloneRepo) != beforeHead);

    const auto subject = RunGit({"log", "-1", "--pretty=%s"}, ctx.cloneRepo);
    RequireSuccess(subject, "read amended subject");
    REQUIRE(TrimCopy(subject.stdoutText) == "docs(readme): refine amend ai subject");

    RemoveSandboxWorkspace(ctx.sandbox);
}

TEST_CASE("amend_ai_auto_rejects_status_only_ai_output", "[functional][amend][ai]") {
    const auto ctx = CreateRemoteWithClone("amend-ai-status-only-output");
    const auto stubDir = InstallCopilotStub(ctx.sandbox.root);
    WriteTextFile(ctx.cloneRepo / "README.md", "seed\namend ai status-only\n");
    RequireSuccess(RunGit({"add", "README.md"}, ctx.cloneRepo), "stage amend ai status-only change");
    RequireSuccess(RunGit({"commit", "-m", "chore: placeholder amend status-only"}, ctx.cloneRepo), "seed amend status-only target");
    const auto beforeHead = CurrentHeadSha(ctx.cloneRepo);

    std::vector<std::pair<std::string, std::string>> env{
        {"KOG_TEST_AI_STDOUT", "Reading\n"},
        {"KOG_TEST_AI_EXIT_CODE", "0"},
    };
    env.emplace_back("PATH", stubDir.string() + ";" + (std::getenv("PATH") != nullptr ? std::getenv("PATH") : ""));

    const auto result = RunKogWithEnv(
        {"amend", "--ai-auto", "--ai-provider", "copilot", "--no-ai-review"},
        ctx.cloneRepo,
        env);
    INFO(result.stdoutText);
    INFO(result.stderrText);
    REQUIRE(result.exitCode != 0);

    const auto merged = result.stdoutText + "\n" + result.stderrText;
    REQUIRE(merged.find("ai message generation failed: ai provider returned empty message") != std::string::npos);
    REQUIRE(CurrentHeadSha(ctx.cloneRepo) == beforeHead);

    const auto subject = RunGit({"log", "-1", "--pretty=%s"}, ctx.cloneRepo);
    RequireSuccess(subject, "read unchanged subject");
    REQUIRE(TrimCopy(subject.stdoutText) == "chore: placeholder amend status-only");

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

TEST_CASE("agent_mode_commit_push_ai_auto_requires_prepared_plan_boundary",
          "[functional][commit-push][agent-mode]") {
    const auto ctx = CreateRemoteWithClone("agent-mode-cpa-boundary");
    WriteTextFile(ctx.cloneRepo / "README.md", "seed\nagent mode boundary\n");

    const auto result = RunKogWithEnv(
        {"commit-push", "--ai-auto"},
        ctx.cloneRepo,
        {{"KANO_AGENT_MODE", "1"}});
    INFO(result.stdoutText);
    INFO(result.stderrText);

    REQUIRE(result.exitCode != 0);
    const auto merged = result.stdoutText + "\n" + result.stderrText;
    REQUIRE(merged.find("agent mode cpa/commit-push cannot invoke internal AI auto-plan") != std::string::npos);

    RemoveSandboxWorkspace(ctx.sandbox);
}

TEST_CASE("repo_hygiene_archive_safe_prereq_failure_skips_fix_mutations",
          "[functional][repo-hygiene][archive-safe]") {
    const auto ctx = CreateRemoteWithClone("repo-hygiene-prereq-ordering");
    const std::string scriptPath = "src/shell/test/pre-commit-quality-gate.sh";

    RequireSuccess(
        RunGit({"update-index", "--chmod=-x", scriptPath}, ctx.cloneRepo),
        "mark quality gate non-executable");
    std::error_code ec;
    std::filesystem::remove((ctx.cloneRepo / scriptPath).lexically_normal(), ec);

    const auto result = RunKog(
        {"repo-hygiene", "--repo", ctx.cloneRepo.string(), "fix", "--archive-safe"},
        ctx.cloneRepo);
    INFO(result.stdoutText);
    INFO(result.stderrText);
    REQUIRE(result.exitCode != 0);

    const auto modeCheck = RunGit({"ls-files", "-s", "--", scriptPath}, ctx.cloneRepo);
    RequireSuccess(modeCheck, "check script mode after failed archive-safe prereq");
    REQUIRE(modeCheck.stdoutText.find("100644") != std::string::npos);

    const auto merged = result.stdoutText + "\n" + result.stderrText;
    REQUIRE(merged.find("Fixes skipped due to archive-safe prerequisite failures") != std::string::npos);
    REQUIRE(merged.find("tracked script missing from working tree") != std::string::npos);

    RemoveSandboxWorkspace(ctx.sandbox);
}

TEST_CASE("repo_hygiene_archive_safe_skips_untracked_optional_quality_gate",
          "[functional][repo-hygiene][archive-safe]") {
    const auto ctx = CreateRemoteWithClone("repo-hygiene-optional-quality-gate");
    const std::string scriptPath = "src/shell/test/pre-commit-quality-gate.sh";

    RequireSuccess(RunGit({"rm", scriptPath}, ctx.cloneRepo), "remove optional quality gate from repo");
    RequireSuccess(
        RunGit({"commit", "-m", "remove optional quality gate"}, ctx.cloneRepo),
        "commit optional quality gate removal");

    const auto result = RunKog(
        {"repo-hygiene", "--repo", ctx.cloneRepo.string(), "fix", "--archive-safe"},
        ctx.cloneRepo);
    INFO(result.stdoutText);
    INFO(result.stderrText);
    REQUIRE(result.exitCode == 0);

    const auto merged = result.stdoutText + "\n" + result.stderrText;
    REQUIRE(merged.find("[SKIP] pre-commit-quality-gate optional script not tracked in this repo") != std::string::npos);
    REQUIRE(merged.find("Archive-safe prerequisites failed") == std::string::npos);

    RemoveSandboxWorkspace(ctx.sandbox);
}

TEST_CASE("commit_push_ai_auto_single_human_mode_fails_without_deterministic_fallback",
          "[.][functional][commit-push][ai][single]") {
    const auto ctx = CreateRemoteWithClone("commit-push-ai-single-fail-fast");
    WriteTextFile(ctx.cloneRepo / "README.md", "seed\nai single fail-fast\n");
    const auto beforeHead = CurrentHeadSha(ctx.cloneRepo);
    const auto providerLogPath = (ctx.sandbox.root / "provider-invocations.log").lexically_normal();

    std::vector<std::pair<std::string, std::string>> env{
        {"KOG_TEST_AI_STDOUT", "not-json"},
        {"KOG_TEST_AI_EXIT_CODE", "0"},
        {"KANO_AGENT_MODE", ""},
        {"KOG_TEST_AI_STUB_LOG", providerLogPath.string()},
    };
    const auto stubDir = InstallCopilotStub(ctx.sandbox.root);
    env.emplace_back("PATH", stubDir.string() + ";" + (std::getenv("PATH") != nullptr ? std::getenv("PATH") : ""));
    const auto providerProbe = ResolveProviderCommands(ctx.cloneRepo);
    INFO(providerProbe.stdoutText);
    INFO(providerProbe.stderrText);

    const auto result = RunKogWithEnv(
        {"commit-push", "--ai-auto", "--ai-provider", "copilot", "--ai-fill-mode", "single", "--no-ai-review"},
        ctx.cloneRepo,
        env);
    INFO(result.stdoutText);
    INFO(result.stderrText);
    REQUIRE(result.exitCode != 0);

    const auto merged = result.stdoutText + "\n" + result.stderrText;
    const bool hasExpectedFailure =
        merged.find("AI edited working plan could not be normalized") != std::string::npos ||
        merged.find("AI commit runbook failed via native binary") != std::string::npos;
    REQUIRE(hasExpectedFailure);
    REQUIRE(merged.find("Filled plan commit entries with deterministic fallback ops") == std::string::npos);
    REQUIRE(merged.find("using deterministic local fallback") == std::string::npos);
    REQUIRE(std::filesystem::exists(providerLogPath));
    const auto providerLogText = ReadTextFile(providerLogPath);
    INFO(providerLogText);
    const bool invokedCopilotProvider = providerLogText.find("copilot") != std::string::npos ||
                                       providerLogText.find("gh copilot") != std::string::npos;
    REQUIRE(invokedCopilotProvider);
    REQUIRE(CurrentHeadSha(ctx.cloneRepo) == beforeHead);
    const auto [behind, ahead] = AheadBehindCounts(ctx.cloneRepo);
    REQUIRE(behind == 0);
    REQUIRE(ahead == 0);

    RemoveSandboxWorkspace(ctx.sandbox);
}

TEST_CASE("commit_push_ai_auto_codex_uses_explicit_workspace_relative_prompt_reference",
          "[.][functional][commit-push][ai][codex]") {
#if defined(_WIN32)
    const auto ctx = CreateRemoteWithClone("commit-push-ai-codex-prompt-ref");
    WriteTextFile(ctx.cloneRepo / "README.md", "seed\ncodex prompt ref\n");

    const auto capturePath = (ctx.sandbox.root / "codex-prompt.txt").lexically_normal();
    const auto providerLogPath = (ctx.sandbox.root / "provider-invocations.log").lexically_normal();
    const auto stubDir = InstallCodexCaptureStub(ctx.sandbox.root, capturePath);

    std::vector<std::pair<std::string, std::string>> env{
        {"KANO_AGENT_MODE", ""},
        {"KOG_TEST_AI_STUB_LOG", providerLogPath.string()},
        {"PATH", stubDir.string() + ";" + (std::getenv("PATH") != nullptr ? std::getenv("PATH") : "")},
    };
    const auto providerProbe = ResolveProviderCommands(ctx.cloneRepo);
    INFO(providerProbe.stdoutText);
    INFO(providerProbe.stderrText);

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
    REQUIRE(capturedPrompt.find("provider-prompts/plan-fill-") != std::string::npos);
    REQUIRE(capturedPrompt.find("Read @./") == std::string::npos);
    REQUIRE(std::filesystem::exists(providerLogPath));
    const auto providerLogText = ReadTextFile(providerLogPath);
    REQUIRE(providerLogText.find("codex") != std::string::npos);

    RemoveSandboxWorkspace(ctx.sandbox);
#else
    SUCCEED("Windows-only regression");
#endif
}

TEST_CASE("plan_runbook_commit_per_commit_accepts_flat_review_fill_ops_aliases",
          "[.][functional][plan][ai][per-commit]") {
    const auto ctx = CreateRemoteWithClone("plan-runbook-per-commit-flat-review-aliases");
    WriteTextFile(ctx.cloneRepo / "README.md", "seed\nper-commit alias fill\n");
    const auto planPath = (ctx.cloneRepo / ".kano" / "tmp" / "git" / "plans" / "per-commit-plan.json").lexically_normal();
    const auto providerLogPath = (ctx.sandbox.root / "provider-invocations.log").lexically_normal();

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
        {"KOG_TEST_AI_STUB_LOG", providerLogPath.string()},
    };
    const auto stubDir = InstallCopilotStub(ctx.sandbox.root);
    env.emplace_back("PATH", stubDir.string() + ";" + (std::getenv("PATH") != nullptr ? std::getenv("PATH") : ""));
    const auto providerProbe = ResolveProviderCommands(ctx.cloneRepo);
    INFO(providerProbe.stdoutText);
    INFO(providerProbe.stderrText);

    const auto result = RunKogWithEnv(
        {"plan", "runbook", "commit", "--plan-file", planPath.string(), "--ai-provider", "copilot", "--ai-fill-mode", "per-commit"},
        ctx.cloneRepo,
        env);
    INFO(result.stdoutText);
    INFO(result.stderrText);
    REQUIRE(result.exitCode == 0);
    REQUIRE(std::filesystem::exists(planPath));

    const auto planText = ReadTextFile(planPath);
    REQUIRE(planText.find("The README change is self-contained and the alias-based fill payload is semantically complete.") != std::string::npos);
    REQUIRE(planText.find("\"verdict\":\"pass\"") != std::string::npos);
    const auto mergedOutput = result.stdoutText + "\n" + result.stderrText;
    const bool hasPerCommitFillSignal =
        mergedOutput.find("Filled plan commit entries with AI-safe ops") != std::string::npos ||
        mergedOutput.find("entry 0 (.) is deterministic; skipping AI fill") != std::string::npos;
    REQUIRE(hasPerCommitFillSignal);
    REQUIRE(std::filesystem::exists(providerLogPath));
    const auto providerLogText = ReadTextFile(providerLogPath);
    INFO(providerLogText);
    const bool invokedCopilotProvider = providerLogText.find("copilot") != std::string::npos ||
                                       providerLogText.find("gh copilot") != std::string::npos;
    REQUIRE(invokedCopilotProvider);

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
    const auto plainOutput = StripAnsi(result.stdoutText);
    REQUIRE(result.exitCode == 0);
    REQUIRE(plainOutput.find("Branch source: registered .gitmodules branch (refreshed)") != std::string::npos);
    REQUIRE(plainOutput.find("Auto-stashed local changes for deps/child") != std::string::npos);
    REQUIRE(plainOutput.find("Restored auto-stash for deps/child") != std::string::npos);

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
    auto childText = ReadTextFile(ctx.cloneChildRepo / "child.txt");
    childText.erase(std::remove(childText.begin(), childText.end(), '\r'), childText.end());
    REQUIRE(childText == "child seed\nlocal dirty change\n");

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

TEST_CASE("submodule_update_recursive_continues_past_failed_direct_submodule", "[functional][submodule][update][recursive]") {
    const auto ctx = CreateRecursiveSubmoduleUpdateClone("submodule-update-recursive-continue");

    const auto result = RunKogAllowingFileProtocol({"submodule", "update", "--recursive"}, ctx.cloneRootRepo);
    INFO(result.stdoutText);
    INFO(result.stderrText);
    REQUIRE(result.exitCode != 0);

    REQUIRE(std::filesystem::exists(ctx.cloneRootRepo / std::filesystem::path(ctx.healthyPath) / "healthy.txt"));
    REQUIRE(std::filesystem::exists(ctx.cloneRootRepo / std::filesystem::path(ctx.nestedPathFromRoot) / "nested.txt"));

    const auto brokenStatus = RunGit({"submodule", "status", "--", ctx.brokenPath}, ctx.cloneRootRepo);
    RequireSuccess(brokenStatus, "probe broken submodule status");
    REQUIRE_FALSE(TrimCopy(brokenStatus.stdoutText).empty());
    REQUIRE(TrimCopy(brokenStatus.stdoutText).front() == '-');

    const auto merged = result.stdoutText + "\n" + result.stderrText;
    REQUIRE(merged.find(ctx.brokenPath) != std::string::npos);
    REQUIRE(merged.find("All submodules updated successfully") == std::string::npos);
    REQUIRE(merged.find("=== Submodule Update Complete ===") != std::string::npos);
    REQUIRE(merged.find("Failed:") != std::string::npos);

    RemoveSandboxWorkspace(ctx.sandbox);
}

TEST_CASE("submodule_update_clean_summary_reports_clean_counts", "[functional][submodule][update][summary]") {
    const auto ctx = CreateRemoteWithSubmoduleClone("submodule-update-clean-summary");

    const auto result = RunKogAllowingFileProtocol({"submodule", "update", "--recursive"}, ctx.cloneRootRepo);
    INFO(result.stdoutText);
    INFO(result.stderrText);
    REQUIRE(result.exitCode == 0);

    const auto merged = result.stdoutText + "\n" + result.stderrText;
    REQUIRE(merged.find("=== Submodule Update Complete ===") != std::string::npos);
    REQUIRE(merged.find("Updated cleanly:") != std::string::npos);
    REQUIRE(merged.find("Failed: 0") != std::string::npos);
    REQUIRE(merged.find("Blocked: 0") != std::string::npos);
    REQUIRE(merged.find("All submodules updated successfully") == std::string::npos);

    RemoveSandboxWorkspace(ctx.sandbox);
}

TEST_CASE("submodule_update_repairs_invalid_gitdir_state_when_safe", "[functional][submodule][update][repair]") {
    const auto ctx = CreateRemoteWithSubmoduleClone("submodule-update-repair-safe");

    const auto submodulePath = (ctx.cloneRootRepo / std::filesystem::path(ctx.submodulePath)).lexically_normal();
    const auto modulePathResult = RunGit({"-C", ctx.cloneRootRepo.string(), "rev-parse", "--git-path", "modules/" + ctx.submodulePath}, ctx.cloneRootRepo);
    RequireSuccess(modulePathResult, "resolve module path");
    const auto modulePath = std::filesystem::path(TrimCopy(modulePathResult.stdoutText)).lexically_normal();

    std::error_code ec;
    std::filesystem::remove_all(modulePath, ec);
    REQUIRE(!ec);
    WriteTextFile(submodulePath / ".git", "gitdir: ../../.git/modules/" + ctx.submodulePath + "\n");

    const auto result = RunKogAllowingFileProtocol({"submodule", "update", ctx.submodulePath}, ctx.cloneRootRepo);
    INFO(result.stdoutText);
    INFO(result.stderrText);
    REQUIRE(result.exitCode == 0);

    const auto merged = result.stdoutText + "\n" + result.stderrText;
    REQUIRE(merged.find("Repaired and updated: 1") != std::string::npos);
    REQUIRE(merged.find("Failed: 0") != std::string::npos);
    REQUIRE(merged.find("Blocked: 0") != std::string::npos);

    const auto childHead = CurrentHeadSha(submodulePath);
    REQUIRE(childHead == GitlinkHeadSha(ctx.cloneRootRepo, ctx.submodulePath));

    RemoveSandboxWorkspace(ctx.sandbox);
}

TEST_CASE("submodule_update_blocks_unsafe_repair_for_local_user_files", "[functional][submodule][update][repair][unsafe]") {
    const auto ctx = CreateRemoteWithSubmoduleClone("submodule-update-repair-unsafe");

    const auto submodulePath = (ctx.cloneRootRepo / std::filesystem::path(ctx.submodulePath)).lexically_normal();
    const auto modulePathResult = RunGit({"-C", ctx.cloneRootRepo.string(), "rev-parse", "--git-path", "modules/" + ctx.submodulePath}, ctx.cloneRootRepo);
    RequireSuccess(modulePathResult, "resolve module path");
    const auto modulePath = std::filesystem::path(TrimCopy(modulePathResult.stdoutText)).lexically_normal();

    std::error_code ec;
    std::filesystem::remove_all(modulePath, ec);
    REQUIRE(!ec);
    WriteTextFile(submodulePath / "user-note.txt", "keep me\n");

    const auto result = RunKogAllowingFileProtocol({"submodule", "update", ctx.submodulePath}, ctx.cloneRootRepo);
    INFO(result.stdoutText);
    INFO(result.stderrText);
    REQUIRE(result.exitCode != 0);

    const auto merged = result.stdoutText + "\n" + result.stderrText;
    REQUIRE(merged.find("BLOCKED_SUBMODULE_REPAIR_UNSAFE") != std::string::npos);
    REQUIRE(merged.find("Blocked: 1") != std::string::npos);
    REQUIRE(std::filesystem::exists(submodulePath / "user-note.txt"));

    RemoveSandboxWorkspace(ctx.sandbox);
}

TEST_CASE("submodule_update_classifies_lfs_pointer_mismatch_warning", "[functional][submodule][update][lfs]") {
    const auto ctx = CreateRemoteWithSubmoduleClone("submodule-update-lfs-warning");

    const auto result = RunKogWithEnv(
        {"submodule", "update", ctx.submodulePath},
        ctx.cloneRootRepo,
        {
            {"GIT_ALLOW_PROTOCOL", "file:https:ssh:git"},
            {"KOG_TEST_SUBMODULE_UPDATE_STDERR", "Encountered 2 files that should have been pointers, but weren't:\n  a.bin\n  b.bin\n"},
        });
    INFO(result.stdoutText);
    INFO(result.stderrText);
    REQUIRE(result.exitCode == 0);

    const auto merged = result.stdoutText + "\n" + result.stderrText;
    REQUIRE(merged.find("Updated with warnings: 1") != std::string::npos);
    REQUIRE(merged.find("LFS_POINTER_MISMATCH") != std::string::npos);
    REQUIRE(merged.find("All submodules updated successfully") == std::string::npos);

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
    REQUIRE(result.stdoutText.find("SKIPPED_BY_POLICY: .gitmodules policy kog-push-policy=skip") != std::string::npos);

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
    REQUIRE(result.stdoutText.find("SKIPPED_BY_POLICY (origin") != std::string::npos);
    REQUIRE(result.stdoutText.find("local non-bare remote has checked-out branch") != std::string::npos);
    REQUIRE(CurrentHeadSha(localWorkingRemote) == beforeRemoteHead);

    RemoveSandboxWorkspace(ctx.sandbox);
}

TEST_CASE("push_recursive_skips_workspace_root_without_remote_but_pushes_children", "[functional][push][recursive]") {
    const auto ctx = CreateRemoteWithSubmoduleClone("push-root-no-remote-skip");

    RequireSuccess(RunGit({"remote", "remove", "origin"}, ctx.cloneRootRepo), "remove root origin remote");
    RequireSuccess(RunGit({"checkout", ctx.branch}, ctx.cloneChildRepo), "checkout child branch before recursive push");

    WriteTextFile(ctx.cloneChildRepo / "child.txt", "child update for root-no-remote case\n");
    RequireSuccess(RunGit({"add", "child.txt"}, ctx.cloneChildRepo), "child add root-no-remote update");
    RequireSuccess(RunGit({"commit", "-m", "child update root no remote"}, ctx.cloneChildRepo), "child commit root-no-remote update");

    const auto childLocalHead = CurrentHeadSha(ctx.cloneChildRepo);
    const auto result = RunKog({"push"}, ctx.cloneRootRepo);
    INFO(result.stdoutText);
    INFO(result.stderrText);
    REQUIRE(result.exitCode == 0);

    const auto merged = result.stdoutText + "\n" + result.stderrText;
    REQUIRE(merged.find("Push skipped: no pushable remote on workspace root container repo") != std::string::npos);
    REQUIRE(RefSha(ctx.childBareRemote, "refs/heads/" + ctx.branch) == childLocalHead);

    RemoveSandboxWorkspace(ctx.sandbox);
}

TEST_CASE("push_recursive_blocks_parent_when_child_push_fails", "[functional][push][recursive]") {
    const auto ctx = CreateRemoteWithSubmoduleClone("push-child-failure-block-parent");
    RequireSuccess(RunGit({"checkout", ctx.branch}, ctx.cloneChildRepo), "checkout child branch before failure push");

    WriteTextFile(ctx.cloneChildRepo / "child.txt", "child update that cannot be pushed\n");
    RequireSuccess(RunGit({"add", "child.txt"}, ctx.cloneChildRepo), "child add failure case");
    RequireSuccess(RunGit({"commit", "-m", "child update for failure case"}, ctx.cloneChildRepo), "child commit failure case");
    RequireSuccess(
        RunGit({"remote", "set-url", "origin", (ctx.sandbox.root / "missing-child-remote.git").string()}, ctx.cloneChildRepo),
        "break child remote");

    WriteTextFile(ctx.cloneRootRepo / "README.md", "root seed\nroot should be blocked when child fails\n");
    RequireSuccess(RunGit({"add", "README.md"}, ctx.cloneRootRepo), "root add blocked update");
    RequireSuccess(RunGit({"commit", "-m", "root update should be blocked"}, ctx.cloneRootRepo), "root commit blocked update");

    const auto rootRemoteBefore = RefSha(ctx.rootBareRemote, "refs/heads/" + ctx.branch);
    const auto rootLocalHead = CurrentHeadSha(ctx.cloneRootRepo);
    REQUIRE(rootRemoteBefore != rootLocalHead);

    const auto result = RunKog({"push"}, ctx.cloneRootRepo);
    INFO(result.stdoutText);
    INFO(result.stderrText);
    REQUIRE(result.exitCode != 0);

    const auto merged = result.stdoutText + "\n" + result.stderrText;
    const bool hasPushFailure = merged.find("Push failed") != std::string::npos || merged.find("fatal:") != std::string::npos;
    REQUIRE(hasPushFailure);
    REQUIRE(merged.find("Push blocked: one or more nested repositories failed in earlier wave") != std::string::npos);
    REQUIRE(RefSha(ctx.rootBareRemote, "refs/heads/" + ctx.branch) == rootRemoteBefore);

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
        {"workspace", "discover", "--full", "--format", "json", "--repo-root", ctx.cloneRootRepo.string(), "--no-cache"},
        ctx.cloneRootRepo);
    INFO(baseline.stdoutText);
    INFO(baseline.stderrText);
    REQUIRE(baseline.exitCode == 0);
    REQUIRE(ContainsPathEntry(baseline.stdoutText, ctx.cloneNestedRepo));

    const auto excluded = RunKog(
        {"workspace", "discover", "--full", "--format", "json", "--repo-root", ctx.cloneRootRepo.string(), "--no-cache",
         "--exclude", "nested/"},
        ctx.cloneRootRepo);
    INFO(excluded.stdoutText);
    INFO(excluded.stderrText);
    REQUIRE(excluded.exitCode == 0);
    REQUIRE_FALSE(ContainsPathEntry(excluded.stdoutText, ctx.cloneNestedRepo));

    const auto after = RunKog(
        {"workspace", "discover", "--full", "--format", "json", "--repo-root", ctx.cloneRootRepo.string(), "--no-cache"},
        ctx.cloneRootRepo);
    INFO(after.stdoutText);
    INFO(after.stderrText);
    REQUIRE(after.exitCode == 0);
    REQUIRE(ContainsPathEntry(after.stdoutText, ctx.cloneNestedRepo));

    RemoveSandboxWorkspace(ctx.sandbox);
}

TEST_CASE("kog_discover_registered_recursion_ignores_unregistered_depth", "[functional][discover][registry]") {
    const auto ctx = CreateRecursiveSubmoduleUpdateClone("discover-registered-recursion");

    const auto result = RunKogAllowingFileProtocol(
        {"submodule", "update", "--recursive", ctx.healthyPath},
        ctx.cloneRootRepo);
    INFO(result.stdoutText);
    INFO(result.stderrText);
    REQUIRE(result.exitCode == 0);
    const auto mergedUpdateOutput = result.stdoutText + "\n" + result.stderrText;
    REQUIRE(mergedUpdateOutput.find(ctx.brokenPath) == std::string::npos);

    const auto discover = RunKog(
        {"discover", "--format", "json", "--repo-root", ctx.cloneRootRepo.string(), "--no-cache",
         "--unregistered-depth", "0", "--no-unregistered-scan"},
        ctx.cloneRootRepo);
    INFO(discover.stdoutText);
    INFO(discover.stderrText);
    REQUIRE(discover.exitCode == 0);
    REQUIRE(ContainsPathEntry(discover.stdoutText, ctx.cloneRootRepo));
    REQUIRE(ContainsPathEntry(discover.stdoutText, ctx.cloneRootRepo / std::filesystem::path(ctx.healthyPath)));
    REQUIRE(ContainsPathEntry(discover.stdoutText, ctx.cloneRootRepo / std::filesystem::path(ctx.nestedPathFromRoot)));
    REQUIRE(discover.stdoutText.find("\"type\":\"registered\"") != std::string::npos);
    REQUIRE(discover.stdoutText.find("registered-uninit") == std::string::npos);

    RemoveSandboxWorkspace(ctx.sandbox);
}

TEST_CASE("kog_discover_bounded_unregistered_scan_and_no_unregistered_scan", "[functional][discover][registry]") {
    const auto ctx = CreateRemoteWithNestedRepoClone("discover-bounded-unregistered");
    const auto shallowRepo = (ctx.cloneRootRepo / "shallow-tool").lexically_normal();
    const auto deepRepo = (ctx.cloneRootRepo / "level1" / "level2" / "deep-tool").lexically_normal();
    InitPlainGitRepo(shallowRepo);
    InitPlainGitRepo(deepRepo);

    const auto bounded = RunKog(
        {"discover", "--full", "--unregistered-depth", "1", "--format", "json", "--repo-root", ctx.cloneRootRepo.string(), "--no-cache"},
        ctx.cloneRootRepo);
    INFO(bounded.stdoutText);
    INFO(bounded.stderrText);
    REQUIRE(bounded.exitCode == 0);
    REQUIRE(ContainsPathEntry(bounded.stdoutText, shallowRepo));
    REQUIRE_FALSE(ContainsPathEntry(bounded.stdoutText, deepRepo));

    const auto registeredOnly = RunKog(
        {"discover", "--full", "--unregistered-depth", "3", "--no-unregistered-scan", "--format", "json", "--repo-root", ctx.cloneRootRepo.string(), "--no-cache"},
        ctx.cloneRootRepo);
    INFO(registeredOnly.stdoutText);
    INFO(registeredOnly.stderrText);
    REQUIRE(registeredOnly.exitCode == 0);
    REQUIRE(ContainsPathEntry(registeredOnly.stdoutText, shallowRepo));
    REQUIRE_FALSE(ContainsPathEntry(registeredOnly.stdoutText, ctx.cloneNestedRepo));

    RemoveSandboxWorkspace(ctx.sandbox);
}

TEST_CASE("kog_discover_registered_child_is_discovery_root_with_policy_metadata", "[functional][discover][registry]") {
    const auto ctx = CreateRemoteWithSubmoduleClone("discover-child-root-policy");
    RequireSuccess(RunGit({"config", "-f", ".gitmodules", "submodule." + ctx.submodulePath + ".kog-sync", "true"}, ctx.cloneRootRepo), "set kog-sync policy");
    RequireSuccess(RunGit({"config", "-f", ".gitmodules", "submodule." + ctx.submodulePath + ".kog-commit", "false"}, ctx.cloneRootRepo), "set kog-commit policy");
    RequireSuccess(RunGit({"config", "-f", ".gitmodules", "submodule." + ctx.submodulePath + ".kog-push", "false"}, ctx.cloneRootRepo), "set kog-push policy");
    RequireSuccess(RunGit({"config", "-f", ".gitmodules", "submodule." + ctx.submodulePath + ".kog-hygiene", "false"}, ctx.cloneRootRepo), "set kog-hygiene policy");

    const auto rootDiscover = RunKog(
        {"discover", "--format", "json", "--repo-root", ctx.cloneRootRepo.string(), "--no-cache", "--no-unregistered-scan"},
        ctx.cloneRootRepo);
    INFO(rootDiscover.stdoutText);
    INFO(rootDiscover.stderrText);
    REQUIRE(rootDiscover.exitCode == 0);
    REQUIRE(ContainsPathEntry(rootDiscover.stdoutText, ctx.cloneChildRepo));
    REQUIRE(rootDiscover.stdoutText.find("\"registrationRelativeTo\":\"" + ctx.cloneRootRepo.generic_string()) != std::string::npos);
    REQUIRE(rootDiscover.stdoutText.find("\"kogSync\":\"true\"") != std::string::npos);
    REQUIRE(rootDiscover.stdoutText.find("\"kogCommit\":\"false\"") != std::string::npos);
    REQUIRE(rootDiscover.stdoutText.find("\"kogPush\":\"false\"") != std::string::npos);
    REQUIRE(rootDiscover.stdoutText.find("\"kogHygiene\":\"false\"") != std::string::npos);

    const auto childDiscover = RunKog(
        {"discover", "--format", "json", "--repo-root", ctx.cloneChildRepo.string(), "--no-cache", "--no-unregistered-scan"},
        ctx.cloneChildRepo);
    INFO(childDiscover.stdoutText);
    INFO(childDiscover.stderrText);
    REQUIRE(childDiscover.exitCode == 0);
    REQUIRE(childDiscover.stdoutText.find("\"path\":\"" + ctx.cloneChildRepo.generic_string()) != std::string::npos);
    REQUIRE(childDiscover.stdoutText.find("\"type\":\"root\"") != std::string::npos);

    RemoveSandboxWorkspace(ctx.sandbox);
}

TEST_CASE("kog_discover_skips_ignored_directories_and_preserves_trusted_unregistered_manifest", "[functional][discover][registry]") {
    const auto ctx = CreateRemoteWithClone("discover-trusted-unregistered");
    const auto trustedRepo = (ctx.cloneRepo / "trusted-tool").lexically_normal();
    const auto ignoredRepo = (ctx.cloneRepo / "build" / "ignored-tool").lexically_normal();
    const auto newUntrustedRepo = (ctx.cloneRepo / "new-untrusted-tool").lexically_normal();
    InitPlainGitRepo(trustedRepo);
    InitPlainGitRepo(ignoredRepo);
    WriteTextFile(ctx.cloneRepo / ".gitignore", ".kano/\nbuild/\n");

    const auto seedManifest = RunKog(
        {"discover", "--full", "--unregistered-depth", "1", "--format", "json", "--repo-root", ctx.cloneRepo.string()},
        ctx.cloneRepo);
    INFO(seedManifest.stdoutText);
    INFO(seedManifest.stderrText);
    REQUIRE(seedManifest.exitCode == 0);
    REQUIRE(ContainsPathEntry(seedManifest.stdoutText, trustedRepo));
    REQUIRE_FALSE(ContainsPathEntry(seedManifest.stdoutText, ignoredRepo));

    const auto trustedOnly = RunKog(
        {"discover", "--format", "json", "--repo-root", ctx.cloneRepo.string(), "--no-refresh-cache"},
        ctx.cloneRepo);
    INFO(trustedOnly.stdoutText);
    INFO(trustedOnly.stderrText);
    REQUIRE(trustedOnly.exitCode == 0);
    REQUIRE(ContainsPathEntry(trustedOnly.stdoutText, trustedRepo));
    REQUIRE_FALSE(ContainsPathEntry(trustedOnly.stdoutText, ignoredRepo));

    InitPlainGitRepo(newUntrustedRepo);

    const auto strictRegistered = RunKog(
        {"discover", "--format", "json", "--repo-root", ctx.cloneRepo.string(), "--no-cache", "--no-unregistered-scan"},
        ctx.cloneRepo);
    INFO(strictRegistered.stdoutText);
    INFO(strictRegistered.stderrText);
    REQUIRE(strictRegistered.exitCode == 0);
    REQUIRE(ContainsPathEntry(strictRegistered.stdoutText, trustedRepo));
    REQUIRE_FALSE(ContainsPathEntry(strictRegistered.stdoutText, newUntrustedRepo));
    REQUIRE_FALSE(ContainsPathEntry(strictRegistered.stdoutText, ignoredRepo));

    RemoveSandboxWorkspace(ctx.sandbox);
}

TEST_CASE("kog_discover_trusted_unregistered_child_is_registered_discovery_root", "[functional][discover][registry]") {
    const auto sandbox = CreateSandboxWorkspace("discover-trusted-child-root");
    const auto grandchildBareRemote = (sandbox.root / "grandchild-remote.git").lexically_normal();
    const auto grandchildSeedRepo = (sandbox.root / "grandchild-seed").lexically_normal();
    const auto rootRepo = (sandbox.root / "root").lexically_normal();
    const auto childRepo = (rootRepo / "child-tool").lexically_normal();
    const auto grandchildRepo = (childRepo / "deps" / "grandchild").lexically_normal();

    RequireSuccess(RunGit({"init", "--bare", grandchildBareRemote.string()}, sandbox.root), "init grandchild bare");
    RequireSuccess(RunGit({"init", grandchildSeedRepo.string()}, sandbox.root), "init grandchild seed");
    ConfigureIdentity(grandchildSeedRepo);
    RequireSuccess(RunGit({"checkout", "-b", "main"}, grandchildSeedRepo), "checkout grandchild main");
    WriteTextFile(grandchildSeedRepo / "grandchild.txt", "grandchild seed\n");
    RequireSuccess(RunGit({"add", "grandchild.txt"}, grandchildSeedRepo), "grandchild add");
    RequireSuccess(RunGit({"commit", "-m", "grandchild seed"}, grandchildSeedRepo), "grandchild commit");
    RequireSuccess(RunGit({"remote", "add", "origin", grandchildBareRemote.string()}, grandchildSeedRepo), "grandchild add remote");
    RequireSuccess(RunGit({"push", "-u", "origin", "main"}, grandchildSeedRepo), "grandchild push");
    RequireSuccess(RunGit({"symbolic-ref", "HEAD", "refs/heads/main"}, grandchildBareRemote), "grandchild bare HEAD");

    InitPlainGitRepo(rootRepo);
    RequireSuccess(RunGit({"config", "kano.cache.local-dir", (sandbox.root / "_cache").string()}, rootRepo), "configure root external kano cache");
    InitPlainGitRepo(childRepo);
    RequireSuccess(RunGit({"config", "kano.cache.local-dir", (sandbox.root / "_cache-child").string()}, childRepo), "configure child external kano cache");
    RequireSuccess(
        RunGit({"-c", "protocol.file.allow=always", "submodule", "add", "-b", "main", grandchildBareRemote.string(), "deps/grandchild"}, childRepo),
        "child add registered grandchild submodule");
    RequireSuccess(RunGit({"commit", "-am", "add registered grandchild"}, childRepo), "child commit registered grandchild");

    const auto seedManifest = RunKog(
        {"discover", "--full", "--unregistered-depth", "1", "--format", "json", "--repo-root", rootRepo.string()},
        rootRepo);
    INFO(seedManifest.stdoutText);
    INFO(seedManifest.stderrText);
    REQUIRE(seedManifest.exitCode == 0);
    REQUIRE(ContainsPathEntry(seedManifest.stdoutText, childRepo));
    REQUIRE_FALSE(ContainsPathEntry(seedManifest.stdoutText, grandchildRepo));

    const auto trustedDefault = RunKog(
        {"discover", "--format", "json", "--repo-root", rootRepo.string(), "--no-cache", "--no-unregistered-scan"},
        rootRepo);
    INFO(trustedDefault.stdoutText);
    INFO(trustedDefault.stderrText);
    REQUIRE(trustedDefault.exitCode == 0);
    REQUIRE(ContainsPathEntry(trustedDefault.stdoutText, childRepo));
    REQUIRE(ContainsPathEntry(trustedDefault.stdoutText, grandchildRepo));
    REQUIRE(trustedDefault.stdoutText.find("\"path\":\"" + childRepo.generic_string()) != std::string::npos);
    REQUIRE(trustedDefault.stdoutText.find("\"type\":\"unregistered\"") != std::string::npos);
    REQUIRE(trustedDefault.stdoutText.find("\"path\":\"" + grandchildRepo.generic_string()) != std::string::npos);
    REQUIRE(trustedDefault.stdoutText.find("\"type\":\"registered\"") != std::string::npos);
    REQUIRE(trustedDefault.stdoutText.find("\"registrationRelativeTo\":\"" + childRepo.generic_string()) != std::string::npos);

    RemoveSandboxWorkspace(sandbox);
}

TEST_CASE("workspace_discover_honors_gitignore_reinclude_under_cpp_scripts", "[functional][workspace][discovery]") {
    const auto ctx = CreateRemoteWithClone("discover-cpp-scripts-reinclude");
    WriteTextFile(
        ctx.cloneRepo / ".gitignore",
        ".kano/\n"
        "src/cpp/**\n"
        "!src/cpp/\n"
        "!src/cpp/scripts/\n"
        "!src/cpp/scripts/**\n");

    const auto scriptRepo = (ctx.cloneRepo / "src" / "cpp" / "scripts" / "tooling-repo").lexically_normal();
    const auto intermediateRepo = (ctx.cloneRepo / "src" / "cpp" / "build" / "_intermediate" / "cache-repo").lexically_normal();

    std::filesystem::create_directories(scriptRepo.parent_path());
    std::filesystem::create_directories(intermediateRepo.parent_path());
    RequireSuccess(RunGit({"init", scriptRepo.string()}, ctx.cloneRepo), "init script nested repo");
    RequireSuccess(RunGit({"init", intermediateRepo.string()}, ctx.cloneRepo), "init intermediate nested repo");

    const auto result = RunKog(
        {"discover", "--full", "--unregistered-depth", "8", "--format", "json", "--repo-root", ctx.cloneRepo.string(), "--no-cache"},
        ctx.cloneRepo);
    INFO(result.stdoutText);
    INFO(result.stderrText);
    REQUIRE(result.exitCode == 0);
    REQUIRE(ContainsPathEntry(result.stdoutText, ctx.cloneRepo));
    REQUIRE(ContainsPathEntry(result.stdoutText, scriptRepo));
    REQUIRE_FALSE(ContainsPathEntry(result.stdoutText, intermediateRepo));

    RemoveSandboxWorkspace(ctx.sandbox);
}

TEST_CASE("workspace_discover_honors_kogignore_reinclude_under_cpp_scripts", "[functional][workspace][discovery]") {
    const auto ctx = CreateRemoteWithClone("discover-cpp-scripts-kogignore-reinclude");
    WriteTextFile(
        ctx.cloneRepo / ".kogignore",
        "src/cpp/**\n"
        "!src/cpp/\n"
        "!src/cpp/scripts/\n"
        "!src/cpp/scripts/**\n");

    const auto scriptRepo = (ctx.cloneRepo / "src" / "cpp" / "scripts" / "tooling-repo").lexically_normal();
    const auto intermediateRepo = (ctx.cloneRepo / "src" / "cpp" / "build" / "_intermediate" / "cache-repo").lexically_normal();

    std::filesystem::create_directories(scriptRepo.parent_path());
    std::filesystem::create_directories(intermediateRepo.parent_path());
    RequireSuccess(RunGit({"init", scriptRepo.string()}, ctx.cloneRepo), "init script nested repo via kogignore");
    RequireSuccess(RunGit({"init", intermediateRepo.string()}, ctx.cloneRepo), "init intermediate nested repo via kogignore");

    const auto result = RunKog(
        {"discover", "--full", "--unregistered-depth", "8", "--format", "json", "--repo-root", ctx.cloneRepo.string(), "--no-cache"},
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
    const auto systemConfigDir = (skillRoot / ".kano").lexically_normal();
    const auto agentsRoot = (ctx.sandbox.root / "agents-skills-kano").lexically_normal();
    const auto codexRoot = (ctx.sandbox.root / "codex-skills-kano").lexically_normal();
    const auto agentRepo = (agentsRoot / "alpha-skill").lexically_normal();
    const auto codexRepo = (codexRoot / "beta-skill").lexically_normal();

    std::filesystem::create_directories(systemConfigDir);
    WriteTextFile(systemConfigDir / "kog_config.toml",
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
        {"discover", "--full", "--unregistered-depth", "8", "--format", "json", "--repo-root", ctx.cloneRepo.string(), "--no-cache"},
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
    const auto systemConfigDir = (skillRoot / ".kano").lexically_normal();
    const auto agentsRoot = (ctx.sandbox.root / "agents-skills-kano").lexically_normal();
    const auto codexRoot = (ctx.sandbox.root / "codex-skills-kano").lexically_normal();
    const auto sharedName = std::string("shared-skill");
    const auto agentRepo = (agentsRoot / sharedName).lexically_normal();
    const auto codexRepo = (codexRoot / sharedName).lexically_normal();

    std::filesystem::create_directories(systemConfigDir);
    WriteTextFile(systemConfigDir / "kog_config.toml",
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
        {"discover", "--full", "--unregistered-depth", "8", "--format", "json", "--repo-root", ctx.cloneRepo.string(), "--no-cache"},
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
        {"status", longRepo.string(), "--repo-root", sandbox.root.string()},
        sandbox.root,
        {{"COLUMNS", "80"}});
    INFO(narrow.stdoutText);
    INFO(narrow.stderrText);
    REQUIRE(narrow.exitCode == 0);

    const auto wide = RunKogWithEnv(
        {"status", longRepo.string(), "--repo-root", sandbox.root.string()},
        sandbox.root,
        {{"COLUMNS", "140"}});
    INFO(wide.stdoutText);
    INFO(wide.stderrText);
    REQUIRE(wide.exitCode == 0);

    const auto repoName = longRepo.filename().string();
    REQUIRE(wide.stdoutText.find(repoName) != std::string::npos);

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

TEST_CASE("commit_lock_recovery_cleans_stale_lock_and_retries_original_commit", "[functional][commit][locks]") {
    const auto ctx = CreateRemoteWithClone("commit-lock-recovery");
    const std::string message = "test(functional): recover stale commit lock";
    WriteTextFile(ctx.cloneRepo / "README.md", "seed\ncommit lock recovery\n");
    const auto lockPath = (ctx.cloneRepo / ".git" / "index.lock").lexically_normal();
    TouchFile(lockPath);
    SetFileAgeSeconds(lockPath, 10);

    const auto result = RunKogWithEnv(
        {"commit", "-m", message, "--no-recursive"},
        ctx.cloneRepo,
        {{"KOG_COMMIT_LOCK_RECOVERY_TEST_ACTIVE_PROCESS", "0"},
         {"KOG_PROCESS_DIAGNOSTICS", "0"},
         {"KOG_SHELL_TIMEOUT_MS", "120000"}});
    INFO(result.stdoutText);
    INFO(result.stderrText);
    REQUIRE(result.exitCode == 0);

    const auto merged = result.stdoutText + "\n" + result.stderrText;
    REQUIRE(merged.find("[native-commit][lock-recovery] lock failure detected") != std::string::npos);
    REQUIRE(merged.find("removed stale index.lock") != std::string::npos);
    REQUIRE(merged.find("converge probe passed") != std::string::npos);
    REQUIRE(merged.find("retrying original commit once") != std::string::npos);
    REQUIRE_FALSE(std::filesystem::exists(lockPath));
    REQUIRE(StatusPorcelain(ctx.cloneRepo).empty());

    const auto subject = RunGit({"log", "-1", "--pretty=%s"}, ctx.cloneRepo);
    RequireSuccess(subject, "read recovered commit subject");
    REQUIRE(TrimCopy(subject.stdoutText) == message);

    RemoveSandboxWorkspace(ctx.sandbox);
}

TEST_CASE("commit_lock_recovery_blocks_when_active_process_detected", "[functional][commit][locks]") {
    const auto ctx = CreateRemoteWithClone("commit-lock-active-guard");
    WriteTextFile(ctx.cloneRepo / "README.md", "seed\ncommit lock active guard\n");
    const auto lockPath = (ctx.cloneRepo / ".git" / "index.lock").lexically_normal();
    TouchFile(lockPath);
    SetFileAgeSeconds(lockPath, 10);

    const auto result = RunKogWithEnv(
        {"commit", "-m", "test(functional): active lock guard", "--no-recursive"},
        ctx.cloneRepo,
        {{"KOG_COMMIT_LOCK_RECOVERY_TEST_ACTIVE_PROCESS", "1"},
         {"KOG_PROCESS_DIAGNOSTICS", "0"},
         {"KOG_SHELL_TIMEOUT_MS", "120000"}});
    INFO(result.stdoutText);
    INFO(result.stderrText);
    REQUIRE(result.exitCode != 0);

    const auto merged = result.stdoutText + "\n" + result.stderrText;
    REQUIRE(merged.find("[native-commit][lock-recovery] lock failure detected") != std::string::npos);
    REQUIRE(merged.find("lock recovery blocked: active git/kog/coding-agent process detected") != std::string::npos);
    REQUIRE(merged.find("retrying original commit once") == std::string::npos);
    REQUIRE(std::filesystem::exists(lockPath));

    RemoveSandboxWorkspace(ctx.sandbox);
}

TEST_CASE("commit_lock_recovery_does_not_run_for_non_lock_commit_path", "[functional][commit][locks]") {
    const auto ctx = CreateRemoteWithClone("commit-lock-non-lock-path");

    const auto result = RunKogWithEnv(
        {"commit", "-m", "test(functional): clean no recovery", "--no-recursive"},
        ctx.cloneRepo,
        {{"KOG_COMMIT_LOCK_RECOVERY_TEST_ACTIVE_PROCESS", "1"},
         {"KOG_PROCESS_DIAGNOSTICS", "0"},
         {"KOG_SHELL_TIMEOUT_MS", "120000"}});
    INFO(result.stdoutText);
    INFO(result.stderrText);
    REQUIRE(result.exitCode == 0);

    const auto merged = result.stdoutText + "\n" + result.stderrText;
    REQUIRE(merged.find("[native-commit][lock-recovery]") == std::string::npos);

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

TEST_CASE("sync_runs_self_cpp_build_when_self_repo_cpp_changes_arrive", "[.][functional][sync][self-build]") {
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
