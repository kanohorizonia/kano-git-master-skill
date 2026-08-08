#include "functional_test_support.hpp"
#include "audit_contract.hpp"
#include "shell_executor.hpp"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <future>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <thread>

#if !defined(_WIN32)
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

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

auto ResolveFixtureGitPath(const std::filesystem::path& InRepo,
                           const std::string& InGitPath) -> std::filesystem::path {
    std::error_code ec;
    const auto repoRoot = std::filesystem::weakly_canonical(InRepo, ec);
    REQUIRE_FALSE(ec);
    const std::filesystem::path rawPath(InGitPath);
    const auto candidate = std::filesystem::weakly_canonical(
        rawPath.is_absolute() ? rawPath : repoRoot / rawPath,
        ec);
    REQUIRE_FALSE(ec);
    const auto relative = std::filesystem::relative(candidate, repoRoot, ec);
    REQUIRE_FALSE(ec);
    REQUIRE_FALSE(relative.empty());
    REQUIRE_FALSE(relative.is_absolute());
    for (const auto& component : relative) {
        REQUIRE(component != "..");
    }
    return candidate;
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

auto ConfigureReceivePackWrapper(const RemoteCloneContext& InContext, const bool InRejectAfterReceive) -> void {
#if defined(_WIN32)
    const auto wrapperPath = (InContext.sandbox.root / "receive-pack-wrapper.cmd").lexically_normal();
    std::ostringstream script;
    script << "@echo off\r\n";
    if (InRejectAfterReceive) {
        script << "git-receive-pack %*\r\n";
        script << "echo remote: forced ambiguous push result 1>&2\r\n";
    } else {
        script << "echo fatal: Authentication failed 1>&2\r\n";
    }
    script << "exit /b 1\r\n";
#else
    const auto wrapperPath = (InContext.sandbox.root / "receive-pack-wrapper.sh").lexically_normal();
    std::ostringstream script;
    script << "#!/usr/bin/env bash\n";
    if (InRejectAfterReceive) {
        script << "git-receive-pack \"$@\"\n";
        script << "printf '%s\\n' 'remote: forced ambiguous push result' >&2\n";
    } else {
        script << "printf '%s\\n' 'fatal: Authentication failed' >&2\n";
    }
    script << "exit 1\n";
#endif
    WriteTextFile(wrapperPath, script.str());
#if !defined(_WIN32)
    std::error_code permissionError;
    std::filesystem::permissions(
        wrapperPath,
        std::filesystem::perms::owner_exec |
            std::filesystem::perms::group_exec |
            std::filesystem::perms::others_exec,
        std::filesystem::perm_options::add,
        permissionError);
    REQUIRE_FALSE(permissionError);
#endif
    RequireSuccess(
        RunGit({"config", "remote.origin.receivepack", wrapperPath.generic_string()}, InContext.cloneRepo),
        "configure receive-pack wrapper");
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

auto PrependPathEntry(const std::filesystem::path& InEntry) -> std::string {
#if defined(_WIN32)
    constexpr auto separator = ";";
#else
    constexpr auto separator = ":";
#endif
    return InEntry.string() + separator + (std::getenv("PATH") != nullptr ? std::getenv("PATH") : "");
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
#if !defined(_WIN32)
    std::error_code permissionError;
    std::filesystem::permissions(
        shPath,
        std::filesystem::perms::owner_exec | std::filesystem::perms::group_exec | std::filesystem::perms::others_exec,
        std::filesystem::perm_options::add,
        permissionError);
    REQUIRE_FALSE(permissionError);
    std::filesystem::permissions(
        ghShPath,
        std::filesystem::perms::owner_exec | std::filesystem::perms::group_exec | std::filesystem::perms::others_exec,
        std::filesystem::perm_options::add,
        permissionError);
    REQUIRE_FALSE(permissionError);
#endif
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

auto ReplaceJsonStringField(std::string InJson,
                            const std::string& InKey,
                            const std::string& InValue) -> std::string {
    const auto keyToken = "\"" + InKey + "\"";
    const auto keyPos = InJson.find(keyToken);
    REQUIRE(keyPos != std::string::npos);
    const auto colonPos = InJson.find(':', keyPos + keyToken.size());
    REQUIRE(colonPos != std::string::npos);
    const auto firstQuote = InJson.find('"', colonPos + 1);
    REQUIRE(firstQuote != std::string::npos);
    const auto secondQuote = InJson.find('"', firstQuote + 1);
    REQUIRE(secondQuote != std::string::npos);
    InJson.replace(firstQuote + 1, secondQuote - firstQuote - 1, InValue);
    return InJson;
}

auto ReplaceJsonUnsignedField(std::string InJson,
                              const std::string& InKey,
                              const std::uint32_t InValue) -> std::string {
    const auto keyToken = "\"" + InKey + "\"";
    const auto keyPos = InJson.find(keyToken);
    REQUIRE(keyPos != std::string::npos);
    const auto colonPos = InJson.find(':', keyPos + keyToken.size());
    REQUIRE(colonPos != std::string::npos);
    const auto valueStart = InJson.find_first_of("0123456789", colonPos + 1);
    REQUIRE(valueStart != std::string::npos);
    const auto valueEnd = InJson.find_first_not_of("0123456789", valueStart);
    InJson.replace(valueStart, valueEnd - valueStart, std::to_string(InValue));
    return InJson;
}

auto ResolvePlanAuditRoot(const std::filesystem::path& InPlanPath)
    -> std::filesystem::path {
    auto auditRoot = InPlanPath.parent_path() / (InPlanPath.filename().string() + ".audit");
    const auto gitDirectory = RunGit({"rev-parse", "--absolute-git-dir"}, InPlanPath.parent_path());
    if (gitDirectory.exitCode == 0 && !TrimCopy(gitDirectory.stdoutText).empty()) {
        std::error_code ec;
        const auto canonicalPlan = std::filesystem::weakly_canonical(InPlanPath, ec);
        REQUIRE_FALSE(ec);
        auditRoot = std::filesystem::path(TrimCopy(gitDirectory.stdoutText)) / "kog" / "audit" /
            ("plan-" + kano::git::audit::Sha256Hex(canonicalPlan.generic_string()));
    }
    return auditRoot;
}

auto FindAuditAttemptRoots(const std::filesystem::path& InPlanPath,
                           const std::uint32_t InAttempt)
    -> std::vector<std::filesystem::path> {
    const auto auditRoot = ResolvePlanAuditRoot(InPlanPath);
    const auto attemptName = "attempt-" + std::to_string(InAttempt);
    std::vector<std::filesystem::path> matches;
    std::error_code ec;
    for (std::filesystem::recursive_directory_iterator it(auditRoot, ec), end; !ec && it != end; it.increment(ec)) {
        if (it->is_directory(ec) && it->path().filename() == attemptName) matches.push_back(it->path());
    }
    REQUIRE_FALSE(ec);
    return matches;
}

auto FindAuditAttemptRoot(const std::filesystem::path& InPlanPath,
                          const std::uint32_t InAttempt) -> std::filesystem::path {
    const auto matches = FindAuditAttemptRoots(InPlanPath, InAttempt);
    REQUIRE(matches.size() == 1);
    return matches.front();
}

auto RequireAuditAttemptRoot(
    const std::filesystem::path& InRoot,
    const kano::git::audit::OutcomeState InOutcome,
    const std::vector<std::string>& InActions) -> void {
    const auto events = kano::git::audit::ParseAuditEventsJsonl(
        ReadTextFile(InRoot / "events.jsonl"));
    INFO("audit event parse issues=" << events.validation.issues.size());
    REQUIRE(events.ok());
    const auto receipt = kano::git::audit::ParseRunReceiptJson(
        ReadTextFile(InRoot / "receipt.json"));
    INFO("audit receipt parse issues=" << receipt.validation.issues.size());
    REQUIRE(receipt.ok());
    REQUIRE(receipt.value->terminalOutcome.status == InOutcome);
    REQUIRE(kano::git::audit::ValidateRunTrace(*receipt.value, events.values).ok());
    for (const auto& action : InActions) {
        REQUIRE(std::any_of(events.values.begin(), events.values.end(),
                            [&](const auto& event) {
                                return event.action == action;
                            }));
    }
}

auto RunKogWithPostReserveSameInodeRewrite(
    const std::vector<std::string>& InArgs,
    const std::filesystem::path& InRepo,
    const std::filesystem::path& InPlanPath,
    const std::string& InChangedBytes) -> CommandResult {
    const auto auditRoot = ResolvePlanAuditRoot(InPlanPath);
    std::atomic<bool> stop{false};
    std::atomic<bool> changed{false};
    std::thread mutator([&]() {
        while (!stop.load(std::memory_order_relaxed) &&
               !changed.load(std::memory_order_relaxed)) {
            std::error_code ec;
            if (std::filesystem::exists(auditRoot, ec) && !ec) {
                for (std::filesystem::recursive_directory_iterator it(
                         auditRoot, ec), end;
                     !ec && it != end; it.increment(ec)) {
                    if (it->path().filename() != "publication-pending.json")
                        continue;
                    std::ofstream output(
                        InPlanPath, std::ios::binary | std::ios::trunc);
                    if (output.good()) {
                        output << InChangedBytes;
                        output.flush();
                        changed.store(output.good(), std::memory_order_relaxed);
                    }
                    break;
                }
            }
            if (!changed.load(std::memory_order_relaxed))
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    });
    const auto result = RunKogWithEnv(
        InArgs, InRepo,
        {{"KOG_TEST_MODE", "1"},
         {"KOG_TEST_ONLY_AUDIT_INPUT_POST_STAT_DELAY_MS", "750"},
         {"KOG_PROCESS_DIAGNOSTICS", "0"}});
    stop.store(true, std::memory_order_relaxed);
    mutator.join();
    REQUIRE(changed.load(std::memory_order_relaxed));
    return result;
}

auto WaitForPath(const std::filesystem::path& InPath,
                 const std::chrono::milliseconds InTimeout) -> bool {
    const auto deadline = std::chrono::steady_clock::now() + InTimeout;
    while (std::chrono::steady_clock::now() < deadline) {
        std::error_code ec;
        if (std::filesystem::exists(InPath, ec) && !ec) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

#if !defined(_WIN32)
auto RunKogInIsolatedChild(
    const std::vector<std::string>& InArgs,
    const std::filesystem::path& InRepo,
    const std::vector<std::pair<std::string, std::string>>& InEnv)
    -> CommandResult {
    auto skillRoot = ResolveKogBinaryPath().parent_path();
    for (int index = 0; index < 6 && !skillRoot.empty(); ++index)
        skillRoot = skillRoot.parent_path();
    std::vector<std::string> childArgs = {
        "-c", "cd \"$1\" && shift && exec \"$@\"", "kog-lock-test",
        InRepo.string(), "/usr/bin/env", "KOG_GIT_INTERACTIVE=0",
        "GIT_TERMINAL_PROMPT=0", "GCM_INTERACTIVE=never",
        "GIT_ASKPASS=true", "SSH_ASKPASS=true",
        "KOG_PROCESS_DIAGNOSTICS=0", "KOG_SHELL_TIMEOUT_MS=120000",
        "KANO_GIT_SKILL_ROOT=" + skillRoot.string()};
    for (const auto& [key, value] : InEnv)
        childArgs.push_back(key + "=" + value);
    childArgs.push_back(ResolveKogBinaryPath().string());
    childArgs.insert(childArgs.end(), InArgs.begin(), InArgs.end());

    const auto started = std::chrono::steady_clock::now();
    const auto executed = kano::git::shell::ExecuteCommand(
        "/bin/sh", childArgs, kano::git::shell::ExecMode::Capture,
        std::nullopt);
    CommandResult result;
    result.exitCode = executed.exitCode;
    result.stdoutText = executed.stdoutStr;
    result.stderrText = executed.stderrStr;
    result.elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - started)
                           .count();
    return result;
}
#endif

auto RequireAuditAttempt(const std::filesystem::path& InPlanPath,
                         const std::uint32_t InAttempt,
                         const kano::git::audit::OutcomeState InOutcome,
                         const std::vector<std::string>& InActions) -> void {
    const auto root = FindAuditAttemptRoot(InPlanPath, InAttempt);
    RequireAuditAttemptRoot(root, InOutcome, InActions);
}

auto ResolveGitMetadataPath(const std::filesystem::path& InRepo,
                            const std::string& InSelector)
    -> std::filesystem::path {
    const auto result = RunGit({"rev-parse", InSelector}, InRepo);
    RequireSuccess(result, "resolve Git metadata path " + InSelector);
    auto candidate = std::filesystem::path(TrimCopy(result.stdoutText));
    if (candidate.is_relative()) candidate = InRepo / candidate;
    std::error_code ec;
    const auto resolved = std::filesystem::weakly_canonical(candidate, ec);
    REQUIRE_FALSE(ec);
    REQUIRE(resolved.is_absolute());
    return resolved;
}

auto FindOnlyOperationAuditAttempt(const std::filesystem::path& InRepo)
    -> std::filesystem::path {
    const auto auditRoot = ResolveGitMetadataPath(InRepo, "--git-common-dir") /
        "kog" / "audit";
    std::vector<std::filesystem::path> matches;
    std::error_code ec;
    for (std::filesystem::recursive_directory_iterator it(auditRoot, ec), end;
         !ec && it != end; it.increment(ec)) {
        if (it->is_regular_file(ec) && it->path().filename() == "receipt.json" &&
            it->path().parent_path().filename().string().rfind("attempt-", 0) == 0 &&
            it->path().parent_path().parent_path().parent_path().filename().string().rfind(
                "operation-", 0) == 0) {
            matches.push_back(it->path().parent_path());
        }
    }
    REQUIRE_FALSE(ec);
    REQUIRE(matches.size() == 1);
    return matches.front();
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

auto RequirePlanPushFailureThenCleanRetry(const std::string& InFixtureName,
                                          const std::vector<std::string>& InCommitPushExtraArgs,
                                          const bool InUseBasenamePlanPath = false) -> void {
    const auto ctx = CreateRemoteWithClone(InFixtureName);
    WriteTextFile(ctx.cloneRepo / "README.md", "seed\ntruthful plan push result\n");

    const auto planPath = InUseBasenamePlanPath
        ? (ctx.cloneRepo / "truthful-push-result.json").lexically_normal()
        : (ctx.cloneRepo / ".kano" / "cache" / "git" / "plans" / "truthful-push-result.json").lexically_normal();
    if (InUseBasenamePlanPath) {
        WriteTextFile(ctx.cloneRepo / ".git" / "info" / "exclude", "/truthful-push-result.json\n");
    }
    const auto planArgument = InUseBasenamePlanPath
        ? planPath.filename().string()
        : planPath.string();
    RequireSuccess(
        RunKog({"plan", "new", "--force", "--output", planArgument}, ctx.cloneRepo),
        "plan new for truthful push result");
    RequireSuccess(
        RunKog({
            "plan", "prepare", "add-commit-entry",
            "--plan-file", planArgument,
            "--repo", ".",
            "--commit-message", "test(functional): preserve truthful push result",
            "--commit-include", "README.md",
            "--commit-review-verdict", "pass",
            "--commit-review-reason", "same-scenario regression for plan push failure and clean retry"
        }, ctx.cloneRepo),
        "plan add commit entry for truthful push result");
    RequireSuccess(
        RunKog({"plan", "verify", "pre-apply", "--stage", "commit", "--plan-file", planArgument}, ctx.cloneRepo),
        "plan verify pre-apply for truthful push result");
    REQUIRE(ExtractJsonStringField(ReadTextFile(planPath), "schema_version") == "3");
    WriteTextFile(
        planPath,
        ReplaceJsonStringField(
            ReadTextFile(planPath),
            "executed_at_utc",
            "2026-07-30T00:00:00Z"));
    REQUIRE(ExtractJsonStringField(ReadTextFile(planPath), "executed_at_utc") == "2026-07-30T00:00:00Z");

    const auto remoteHeadBefore = RefSha(ctx.bareRemote, "refs/heads/" + ctx.branch);
    std::vector<std::string> commitPushArgs = {"commit-push", "--plan-file", planArgument};
    commitPushArgs.insert(
        commitPushArgs.end(),
        InCommitPushExtraArgs.begin(),
        InCommitPushExtraArgs.end());

    const auto missingPushRemote = (ctx.sandbox.root / "missing-push-remote.git").lexically_normal();
    RequireSuccess(
        RunGit({"remote", "set-url", "--push", "origin", missingPushRemote.string()}, ctx.cloneRepo),
        "configure missing push-only remote");

    const auto failedPush = RunKogWithEnv(
        commitPushArgs,
        ctx.cloneRepo,
        {{"KOG_EXACT_PLAN_COMMIT_MODE", "plumbing"}});
    INFO(failedPush.stdoutText);
    INFO(failedPush.stderrText);
    REQUIRE(failedPush.exitCode != 0);
    RequireNotContainsText(failedPush.stdoutText, "=== plan summary ===");
    RequireContainsText(
        failedPush.stdoutText + "\n" + failedPush.stderrText,
        missingPushRemote.filename().string());
    const bool profiledPath =
        std::find(InCommitPushExtraArgs.begin(), InCommitPushExtraArgs.end(), "--profile") !=
        InCommitPushExtraArgs.end();
    if (profiledPath) {
        RequireContainsText(failedPush.stdoutText, "=== commit-push stage: push ===");
        RequireNotContainsText(failedPush.stdoutText, "[commit-push][plan-pipeline] stage=push start");
    } else {
        RequireContainsText(failedPush.stdoutText, "[commit-push][plan-pipeline] stage=push start");
    }

    const auto localHeadAfterFailure = CurrentHeadSha(ctx.cloneRepo);
    REQUIRE(localHeadAfterFailure != remoteHeadBefore);
    REQUIRE(RefSha(ctx.bareRemote, "refs/heads/" + ctx.branch) == remoteHeadBefore);
    REQUIRE(StatusPorcelain(ctx.cloneRepo).empty());
    REQUIRE(ExtractJsonStringField(ReadTextFile(planPath),
                                   "executed_at_utc")
                .empty());
    RequireAuditAttempt(
        planPath,
        1,
        kano::git::audit::OutcomeState::Failed,
        {"audit.reserve", "plan.execution-lock", "plan.stage",
         "plan.stamp.clear", "commit.apply", "push"});

    const auto failedPostApply = RunKog(
        {"plan", "verify", "post-apply", "--stage", "commit", "--plan-file", planArgument},
        ctx.cloneRepo);
    INFO(failedPostApply.stdoutText);
    INFO(failedPostApply.stderrText);
    REQUIRE(failedPostApply.exitCode != 0);
    RequireContainsText(
        failedPostApply.stdoutText + "\n" + failedPostApply.stderrText,
        "meta.executed_at_utc is empty");

    const auto [behindAfterFailure, aheadAfterFailure] = AheadBehindCounts(ctx.cloneRepo);
    REQUIRE(behindAfterFailure == 0);
    REQUIRE(aheadAfterFailure == 1);
    RequireSuccess(
        RunGit({"remote", "set-url", "--push", "origin", ctx.bareRemote.string()}, ctx.cloneRepo),
        "restore push-only remote");
    std::uint32_t nextAttempt = 2;
    WriteTextFile(
        planPath,
        ReplaceJsonUnsignedField(ReadTextFile(planPath), "attempt", nextAttempt++));

    if (InUseBasenamePlanPath) {
        const auto failedStamp = RunKogWithEnv(
            commitPushArgs,
            ctx.cloneRepo,
            {{"KOG_EXACT_PLAN_COMMIT_MODE", "plumbing"},
             {"KOG_TEST_MODE", "1"},
             {"KOG_TEST_ONLY_PLAN_SOURCE_REWRITE_PHASE", "stamp"},
             {"KOG_TEST_ONLY_PLAN_SOURCE_REWRITE_FAIL_AFTER_FIRST_WRITE", "1"},
             {"KOG_PROCESS_DIAGNOSTICS", "0"}});

        INFO(failedStamp.stdoutText);
        INFO(failedStamp.stderrText);
        REQUIRE(failedStamp.exitCode != 0);
        RequireContainsText(
            failedStamp.stdoutText + "\n" + failedStamp.stderrText,
            "failed to stamp plan executed_at_utc after successful push");
        RequireContainsText(
            failedStamp.stdoutText + "\n" + failedStamp.stderrText,
            "injected failure after first plan source write");
        RequireNotContainsText(failedStamp.stdoutText, "=== plan summary ===");
        REQUIRE(CurrentHeadSha(ctx.cloneRepo) == localHeadAfterFailure);
        REQUIRE(RefSha(ctx.bareRemote, "refs/heads/" + ctx.branch) == localHeadAfterFailure);
        REQUIRE(ExtractJsonStringField(ReadTextFile(planPath),
                                       "executed_at_utc")
                    .empty());
        RequireAuditAttempt(
            planPath,
            2,
            kano::git::audit::OutcomeState::Failed,
            {"audit.reserve", "plan.execution-lock", "plan.stamp.clear",
             "push", "plan.stamp"});
        WriteTextFile(
            planPath,
            ReplaceJsonUnsignedField(ReadTextFile(planPath), "attempt", nextAttempt++));
    }

    const auto successfulRetry = RunKogWithEnv(
        commitPushArgs,
        ctx.cloneRepo,
        {{"KOG_EXACT_PLAN_COMMIT_MODE", "plumbing"}});
    INFO(successfulRetry.stdoutText);
    INFO(successfulRetry.stderrText);
    REQUIRE(successfulRetry.exitCode == 0);
    RequireContainsText(successfulRetry.stdoutText, "workspace clean; skipping commit/sync/post-sync");
    RequireContainsText(successfulRetry.stdoutText, "=== plan summary ===");
    REQUIRE(CurrentHeadSha(ctx.cloneRepo) == localHeadAfterFailure);
    REQUIRE(RefSha(ctx.bareRemote, "refs/heads/" + ctx.branch) == localHeadAfterFailure);
    REQUIRE(StatusPorcelain(ctx.cloneRepo).empty());

    const auto executedAt = ExtractJsonStringField(ReadTextFile(planPath), "executed_at_utc");
    const std::regex utcTimestampPattern(R"(^[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{2}:[0-9]{2}:[0-9]{2}Z$)");
    REQUIRE(std::regex_match(executedAt, utcTimestampPattern));
    REQUIRE(ExtractJsonStringField(ReadTextFile(planPath), "schema_version") == "3");
    RequireAuditAttempt(
        planPath,
        nextAttempt - 1,
        kano::git::audit::OutcomeState::Succeeded,
        {"audit.reserve", "plan.execution-lock", "plan.stamp.clear", "push",
         "plan.stamp"});

    const auto successfulPostApply = RunKog(
        {"plan", "verify", "post-apply", "--stage", "commit", "--plan-file", planArgument},
        ctx.cloneRepo);
    RequireSuccess(successfulPostApply, "post-apply verify after successful clean retry");
    RemoveSandboxWorkspace(ctx.sandbox);
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

TEST_CASE("audit capability CLI publishes the exact closed route and input pairs",
          "[functional][audit][capability][KG-TSK-0125]") {
    const auto ctx = CreateRemoteWithClone("audit-capability-closed-pairs");
    const auto result =
        RunKog({"audit", "capability", "--json"}, ctx.cloneRepo);
    INFO(result.stdoutText);
    INFO(result.stderrText);
    REQUIRE(result.exitCode == 0);
    const auto capability = nlohmann::json::parse(result.stdoutText);
    const std::set<std::string> exactKeys = {
        "schemaName", "schemaVersion", "protocolVersion",
        "correlationEnvelopeVersions", "auditEventVersions",
        "runReceiptVersions", "auditVerificationVersions",
        "supportedInputs", "provenanceGrantsAuthority", "durability",
    };
    std::set<std::string> actualKeys;
    for (const auto& [key, value] : capability.items()) {
        static_cast<void>(value);
        actualKeys.insert(key);
    }
    REQUIRE(actualKeys == exactKeys);
    REQUIRE(capability.at("provenanceGrantsAuthority") == false);
    const nlohmann::json expectedPairs = nlohmann::json::array({
        {{"route", "commit.plan"}, {"inputKind", "commit-plan"}},
        {{"route", "commit-push.plan"}, {"inputKind", "commit-plan"}},
        {{"route", "plan.apply"}, {"inputKind", "commit-plan"}},
        {{"route", "converge.repos"}, {"inputKind", "operation-descriptor"}},
        {{"route", "converge.branches.apply"}, {"inputKind", "operation-descriptor"}},
        {{"route", "converge.branches.recover"}, {"inputKind", "operation-descriptor"}},
        {{"route", "converge.branches.retire"}, {"inputKind", "operation-descriptor"}},
    });
    REQUIRE(capability.at("supportedInputs") == expectedPairs);
    RequireNotContainsText(result.stdoutText, ctx.cloneRepo.generic_string());
    RequireNotContainsText(result.stdoutText, "plan-file");

    RemoveSandboxWorkspace(ctx.sandbox);
}

TEST_CASE("converge abort reserves before state deletion and receipts deletion failure",
          "[functional][converge][audit][abort][failure][KG-TSK-0125]") {
    const auto ctx = CreateRemoteWithClone("converge-audited-abort-delete-failure");
    const auto statePath = (ctx.cloneRepo / ".kano" / "tmp" / "workflows" /
                            "converge" / "state.json").lexically_normal();
    // A non-empty directory at the fixed state-file path gives a portable,
    // deterministic deletion failure without permission-dependent fixtures.
    WriteTextFile(statePath / "sentinel", "preserve me\n");

    const auto malformedCorrelation =
        (ctx.cloneRepo / ".kano" / "tmp" / "malformed-correlation.json").lexically_normal();
    WriteTextFile(malformedCorrelation, R"({"mode":"koa"})");
    const auto rejected = RunKog(
        {"converge", "--no-recursive", "--abort", "--correlation-file",
         malformedCorrelation.string()},
        ctx.cloneRepo);
    INFO(rejected.stdoutText);
    INFO(rejected.stderrText);
    REQUIRE(rejected.exitCode != 0);
    REQUIRE(std::filesystem::exists(statePath / "sentinel"));
    RequireContainsText(rejected.stdoutText + "\n" + rejected.stderrText,
                        "audit preflight failed");

    const auto failedDelete =
        RunKog({"converge", "--no-recursive", "--abort"}, ctx.cloneRepo);
    INFO(failedDelete.stdoutText);
    INFO(failedDelete.stderrText);
    REQUIRE(failedDelete.exitCode != 0);
    REQUIRE(std::filesystem::exists(statePath / "sentinel"));
    RequireContainsText(failedDelete.stdoutText + "\n" + failedDelete.stderrText,
                        "failed to remove converge state file");

    const auto attemptRoot = FindOnlyOperationAuditAttempt(ctx.cloneRepo);
    const auto events = kano::git::audit::ParseAuditEventsJsonl(
        ReadTextFile(attemptRoot / "events.jsonl"));
    REQUIRE(events.ok());
    const auto receipt = kano::git::audit::ParseRunReceiptJson(
        ReadTextFile(attemptRoot / "receipt.json"));
    REQUIRE(receipt.ok());
    REQUIRE(receipt.value->terminalOutcome.status ==
            kano::git::audit::OutcomeState::Failed);
    REQUIRE(kano::git::audit::ValidateRunTrace(*receipt.value, events.values).ok());
    const auto deletion = std::find_if(
        events.values.begin(), events.values.end(), [](const auto& event) {
            return event.action == "converge.repos.state.delete";
        });
    REQUIRE(deletion != events.values.end());
    REQUIRE(deletion->outcome.status == kano::git::audit::OutcomeState::Failed);
    REQUIRE(deletion->outcome.exitCode == 1);

    RemoveSandboxWorkspace(ctx.sandbox);
}

TEST_CASE("converge operation descriptor persists only opaque remote selectors",
          "[functional][converge][audit][redaction][KG-TSK-0125]") {
    const auto ctx = CreateRemoteWithClone("converge-audit-selector-redaction");
    const std::vector<std::string> sensitiveSelectors = {
        "token-host.example",
        "private-secret-workspace",
        "windows-secret-repo",
    };
    for (const auto& selector : sensitiveSelectors) {
        const auto result = RunKog(
            {"converge", "--no-recursive", "--abort", "--remote", selector},
            ctx.cloneRepo);
        INFO(result.stdoutText);
        INFO(result.stderrText);
        REQUIRE(result.exitCode == 0);
    }

    const auto auditRoot =
        ResolveGitMetadataPath(ctx.cloneRepo, "--git-common-dir") /
        "kog" / "audit";
    std::size_t descriptors = 0;
    std::error_code ec;
    for (std::filesystem::recursive_directory_iterator it(auditRoot, ec), end;
         !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec) ||
            it->path().filename() != "frozen-operation.json") {
            continue;
        }
        ++descriptors;
        const auto bytes = ReadTextFile(it->path());
        for (const auto& selector : sensitiveSelectors) {
            REQUIRE(bytes.find(selector) == std::string::npos);
        }
        const auto descriptor = nlohmann::json::parse(bytes);
        REQUIRE(descriptor.at("route") == "converge.repos");
        const auto& options = descriptor.at("options");
        REQUIRE(options.size() == 9);
        REQUIRE(options.contains("remoteSelectorSha256"));
        REQUIRE_FALSE(options.contains("remote"));
        REQUIRE(options.at("remoteSelectorSha256").is_string());
        REQUIRE(options.at("remoteSelectorSha256").get<std::string>().size() == 64);
    }
    REQUIRE_FALSE(ec);
    REQUIRE(descriptors == sensitiveSelectors.size());

    RemoveSandboxWorkspace(ctx.sandbox);
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

TEST_CASE("push_reconciles_nonzero_transport_result_with_remote_head",
          "[functional][push][transport][KG-BUG-0096]") {
    const auto ctx = CreateRemoteWithClone("push-remote-head-reconciliation");
    ConfigureReceivePackWrapper(ctx, true);

    WriteTextFile(ctx.cloneRepo / "ambiguous.txt", "remote accepted before transport error\n");
    RequireSuccess(RunGit({"add", "ambiguous.txt"}, ctx.cloneRepo), "stage ambiguous push commit");
    RequireSuccess(RunGit({"commit", "-m", "ambiguous push commit"}, ctx.cloneRepo), "commit ambiguous push");
    const auto ambiguousHead = CurrentHeadSha(ctx.cloneRepo);

    const auto ambiguousResult = RunKog({"push"}, ctx.cloneRepo);
    INFO(ambiguousResult.stdoutText);
    INFO(ambiguousResult.stderrText);
    REQUIRE(ambiguousResult.exitCode == 0);
    const auto ambiguousOutput = ambiguousResult.stdoutText + "\n" + ambiguousResult.stderrText;
    RequireContainsText(ambiguousOutput, "PUSHED_REMOTE_CONFIRMED");
    RequireContainsText(ambiguousOutput, "remote_probe=exact_match");
    REQUIRE(RefSha(ctx.bareRemote, "refs/heads/" + ctx.branch) == ambiguousHead);

    ConfigureReceivePackWrapper(ctx, false);
    WriteTextFile(ctx.cloneRepo / "auth-failure.txt", "remote rejected before receive\n");
    RequireSuccess(RunGit({"add", "auth-failure.txt"}, ctx.cloneRepo), "stage auth failure commit");
    RequireSuccess(RunGit({"commit", "-m", "auth failure push commit"}, ctx.cloneRepo), "commit auth failure push");
    const auto authFailureHead = CurrentHeadSha(ctx.cloneRepo);

    const auto authFailureResult = RunKog({"push"}, ctx.cloneRepo);
    INFO(authFailureResult.stdoutText);
    INFO(authFailureResult.stderrText);
    REQUIRE(authFailureResult.exitCode != 0);
    const auto authFailureOutput = authFailureResult.stdoutText + "\n" + authFailureResult.stderrText;
    RequireContainsText(authFailureOutput, "FAILED_AUTH");
    RequireContainsText(authFailureOutput, "remote_probe=remote_head_differs");
    REQUIRE(RefSha(ctx.bareRemote, "refs/heads/" + ctx.branch) != authFailureHead);

    RequireSuccess(
        RunGit({"config", "--unset", "remote.origin.receivepack"}, ctx.cloneRepo),
        "restore default receive-pack");
    const auto missingRemote = (ctx.sandbox.root / "missing-remote.git").lexically_normal();
    RequireSuccess(
        RunGit({"remote", "set-url", "origin", missingRemote.string()}, ctx.cloneRepo),
        "set missing push remote");
    WriteTextFile(ctx.cloneRepo / "missing-remote.txt", "remote probe unavailable\n");
    RequireSuccess(RunGit({"add", "missing-remote.txt"}, ctx.cloneRepo), "stage missing remote commit");
    RequireSuccess(RunGit({"commit", "-m", "missing remote push commit"}, ctx.cloneRepo), "commit missing remote push");

    const auto missingRemoteResult = RunKog({"push"}, ctx.cloneRepo);
    INFO(missingRemoteResult.stdoutText);
    INFO(missingRemoteResult.stderrText);
    REQUIRE(missingRemoteResult.exitCode != 0);
    const auto missingRemoteOutput = missingRemoteResult.stdoutText + "\n" + missingRemoteResult.stderrText;
    RequireContainsText(missingRemoteOutput, "FAILED_MISSING_REMOTE");
    RequireContainsText(missingRemoteOutput, "remote_probe=probe_failed");

    RemoveSandboxWorkspace(ctx.sandbox);
}

TEST_CASE("plan_file_clean_but_ahead_continues_to_push",
          "[functional][commit-push][plan-file][contract]") {
    const auto ctx = CreateRemoteWithClone("plan-file-clean-but-ahead");
    WriteTextFile(ctx.cloneRepo / "local.txt", "ahead through plan pipeline\n");
    RequireSuccess(RunGit({"add", "local.txt"}, ctx.cloneRepo), "local add");
    RequireSuccess(RunGit({"commit", "-m", "local plan pipeline ahead commit"}, ctx.cloneRepo), "local commit");
    const auto before = AheadBehindCounts(ctx.cloneRepo);
    REQUIRE(before.second == 1);

    const auto planPath =
        (ctx.cloneRepo / ".kano" / "cache" / "git" / "plans" / "clean-but-ahead.json").lexically_normal();
    RequireSuccess(
        RunKog({"plan", "new", "--force", "--output", planPath.string()}, ctx.cloneRepo),
        "plan new for clean ahead workspace");
    const auto result = RunKog({"commit-push", "--plan-file", planPath.string()}, ctx.cloneRepo);
    INFO(result.stdoutText);
    INFO(result.stderrText);
    REQUIRE(result.exitCode == 0);
    RequireContainsText(result.stdoutText, "[commit-push][plan-pipeline] stage=push start");
    RequireContainsText(
        result.stdoutText,
        "workspace clean; skipping commit/sync/post-sync and proceeding to push check.");
    const auto after = AheadBehindCounts(ctx.cloneRepo);
    REQUIRE(after.second == 0);
    RemoveSandboxWorkspace(ctx.sandbox);
}

TEST_CASE("plan_file_push_failure_leaves_execution_unstamped_and_retry_stamps_after_convergence",
          "[functional][commit-push][plan-file][contract][failure][output][KG-BUG-0089]") {
    RequirePlanPushFailureThenCleanRetry("plan-file-push-failure-retry", {}, true);
}

TEST_CASE("plan_file_profile_path_push_failure_leaves_execution_unstamped_and_retry_stamps_after_convergence",
          "[functional][commit-push][plan-file][contract][failure][output][profile][KG-BUG-0089]") {
    RequirePlanPushFailureThenCleanRetry("plan-file-profile-push-failure-retry", {"--profile"});
}

TEST_CASE("both commit-push plan routes reject a failed stale-stamp clear before mutation",
          "[functional][commit-push][plan-file][contract][failure][stamp][rollback][KG-BUG-0089][KG-TSK-0125]") {
    for (const bool profiled : {false, true}) {
        const auto ctx = CreateRemoteWithClone(
            profiled ? "plan-file-general-clear-failure"
                     : "plan-file-fast-clear-failure");
        WriteTextFile(ctx.cloneRepo / "README.md",
                      "seed\nstale completion clear failure\n");
        const auto planPath = ctx.cloneRepo /
            ".kano/cache/git/plans/stale-clear-failure.json";
        RequireSuccess(
            RunKog({"plan", "new", "--force", "--output",
                    planPath.string()},
                   ctx.cloneRepo),
            "create stale clear failure plan");
        RequireSuccess(
            RunKog({
                "plan", "prepare", "add-commit-entry", "--plan-file",
                planPath.string(), "--repo", ".", "--commit-message",
                "test(functional): reject failed stale completion clear",
                "--commit-include", "README.md", "--commit-review-verdict",
                "pass", "--commit-review-reason",
                "the stale completion stamp must clear before mutation"},
                ctx.cloneRepo),
            "prepare stale clear failure plan");
        WriteTextFile(
            planPath,
            ReplaceJsonStringField(
                ReadTextFile(planPath), "executed_at_utc",
                "2026-07-30T00:00:00Z"));

        const auto admittedBytes = ReadTextFile(planPath);
        const auto headBefore = CurrentHeadSha(ctx.cloneRepo);
        const auto remoteHeadBefore =
            RefSha(ctx.bareRemote, "refs/heads/" + ctx.branch);
        const auto statusBefore = StatusPorcelain(ctx.cloneRepo);
        std::vector<std::string> args = {
            "commit-push", "--plan-file", planPath.string()};
        if (profiled) args.push_back("--profile");
        const auto result = RunKogWithEnv(
            args, ctx.cloneRepo,
            {{"KOG_EXACT_PLAN_COMMIT_MODE", "plumbing"},
             {"KOG_TEST_MODE", "1"},
             {"KOG_TEST_ONLY_PLAN_SOURCE_REWRITE_PHASE", "clear"},
             {"KOG_TEST_ONLY_PLAN_SOURCE_REWRITE_FAIL_AFTER_FIRST_WRITE", "1"},
             {"KOG_PROCESS_DIAGNOSTICS", "0"}});

        INFO("profiled=" << profiled);
        INFO(result.stdoutText);
        INFO(result.stderrText);
        REQUIRE(result.exitCode == 2);
        RequireContainsText(
            result.stdoutText + "\n" + result.stderrText,
            "failed to clear plan executed_at_utc before execution");
        RequireContainsText(
            result.stdoutText + "\n" + result.stderrText,
            "injected failure after first plan source write");
        REQUIRE(ReadTextFile(planPath) == admittedBytes);
        REQUIRE(CurrentHeadSha(ctx.cloneRepo) == headBefore);
        REQUIRE(RefSha(ctx.bareRemote, "refs/heads/" + ctx.branch) ==
                remoteHeadBefore);
        REQUIRE(StatusPorcelain(ctx.cloneRepo) == statusBefore);
        RequireAuditAttempt(
            planPath, 1, kano::git::audit::OutcomeState::Failed,
            {"audit.reserve", "plan.execution-lock", "plan.stage",
             "plan.source.revalidate", "plan.stamp.clear"});
        const auto events = kano::git::audit::ParseAuditEventsJsonl(
            ReadTextFile(FindAuditAttemptRoot(planPath, 1) / "events.jsonl"));
        REQUIRE(events.ok());
        REQUIRE(std::any_of(
            events.values.begin(), events.values.end(), [](const auto& event) {
                return event.action == "plan.stamp.clear" &&
                    event.outcome.status ==
                    kano::git::audit::OutcomeState::Failed;
            }));
        REQUIRE(std::none_of(
            events.values.begin(), events.values.end(), [](const auto& event) {
                return event.action == "pre-commit.sync" ||
                    event.action == "commit.apply" ||
                    event.action == "sync.apply" ||
                    event.action == "post-sync.commit" ||
                    event.action == "push" || event.action == "plan.stamp";
            }));
        RemoveSandboxWorkspace(ctx.sandbox);
    }
}

TEST_CASE("plan_file_post_apply_rejects_non_utc_execution_stamp",
          "[functional][plan][post-apply][contract][failure][output][KG-BUG-0089]") {
    const auto ctx = CreateRemoteWithClone("plan-file-invalid-execution-stamp");
    const auto planPath =
        (ctx.cloneRepo / ".kano" / "cache" / "git" / "plans" / "invalid-execution-stamp.json").lexically_normal();
    RequireSuccess(
        RunKog({"plan", "new", "--force", "--output", planPath.string()}, ctx.cloneRepo),
        "plan new for invalid execution stamp");
    WriteTextFile(
        planPath,
        ReplaceJsonStringField(ReadTextFile(planPath), "executed_at_utc", "failed"));

    const auto result = RunKog(
        {"plan", "verify", "post-apply", "--stage", "commit", "--plan-file", planPath.string()},
        ctx.cloneRepo);
    INFO(result.stdoutText);
    INFO(result.stderrText);
    REQUIRE(result.exitCode != 0);
    RequireContainsText(
        result.stdoutText + "\n" + result.stderrText,
        "meta.executed_at_utc is not a valid UTC timestamp");
    RemoveSandboxWorkspace(ctx.sandbox);
}

TEST_CASE("clean_unpublished_commit_without_push_remote_fails_explicitly",
          "[functional][commit-push][contract][failure]") {
    const auto ctx = CreateRemoteWithClone("clean-unpublished-missing-remote");
    WriteTextFile(ctx.cloneRepo / "local.txt", "cannot publish\n");
    RequireSuccess(RunGit({"add", "local.txt"}, ctx.cloneRepo), "local add");
    RequireSuccess(RunGit({"commit", "-m", "local unpublished commit"}, ctx.cloneRepo), "local commit");
    const auto unpublishedHead = CurrentHeadSha(ctx.cloneRepo);
    REQUIRE(RefSha(ctx.bareRemote, "refs/heads/main") != unpublishedHead);
    RequireSuccess(RunGit({"remote", "remove", "origin"}, ctx.cloneRepo), "remove push remote");

    const auto result = RunKog({"commit-push"}, ctx.cloneRepo);
    INFO(result.stdoutText);
    INFO(result.stderrText);
    REQUIRE(result.exitCode != 0);
    const auto merged = result.stdoutText + "\n" + result.stderrText;
    RequireContainsText(merged, "FAILED_MISSING_REMOTE");
    RequireContainsText(merged, "no usable push remote found");
    REQUIRE(CurrentHeadSha(ctx.cloneRepo) == unpublishedHead);
    REQUIRE(RefSha(ctx.bareRemote, "refs/heads/main") != unpublishedHead);
    REQUIRE(TrimCopy(RunGit({"status", "--porcelain"}, ctx.cloneRepo).stdoutText).empty());
    RemoveSandboxWorkspace(ctx.sandbox);
}

TEST_CASE("commit_push_plan_file_keeps_exact_include_scope", "[functional][commit-push][plan-file][pathspec]") {
    const auto ctx = CreateRemoteWithClone("plan-file-exact-include");
    const std::string includedPath = "PARA/2026-07-11 - Daily Concept.md";
    WriteTextFile(ctx.cloneRepo / includedPath, "staged draft\n");
    RequireSuccess(RunGit({"add", includedPath}, ctx.cloneRepo), "stage included draft");
    WriteTextFile(ctx.cloneRepo / includedPath, "include me\n");
    WriteTextFile(ctx.cloneRepo / "unrelated.txt", "password: \"supersecretvalue\"\n");

    const auto planPath = (ctx.cloneRepo / ".kano" / "cache" / "git" / "plans" / "exact-include.json").lexically_normal();
    RequireSuccess(RunKog({"plan", "new", "--force", "--output", planPath.string()}, ctx.cloneRepo), "plan new");
    RequireSuccess(
        RunKog({
            "plan", "prepare", "add-commit-entry",
            "--plan-file", planPath.string(),
            "--repo", ".",
            "--commit-message", "test(functional): exact include",
            "--commit-include", includedPath,
            "--commit-review-verdict", "pass",
            "--commit-review-reason", "functional regression for plan-file exact include staging"
        }, ctx.cloneRepo),
        "plan add commit entry");
    RequireSuccess(
        RunKog({"plan", "verify", "pre-apply", "--stage", "commit", "--plan-file", planPath.string()}, ctx.cloneRepo),
        "plan verify pre-apply");

    const auto result = RunKogWithEnv(
        {"commit-push", "--plan-file", planPath.string()},
        ctx.cloneRepo,
        {{"KOG_EXACT_PLAN_COMMIT_MODE", "plumbing"}});
    INFO(result.stdoutText);
    INFO(result.stderrText);
    REQUIRE(result.exitCode == 0);
    RequireContainsText(result.stdoutText, "pre-commit skipped for explicit plan-file");
    RequireContainsText(result.stdoutText, "scoped safety gates checked files=1");
    RequireContainsText(result.stdoutText, "exact cacheinfo staging paths=1");
    RequireContainsText(result.stdoutText, "exact plumbing commit=true");
    RequireContainsText(result.stdoutText, "exact plan working changes=true");
    RequireNotContainsText(result.stdoutText, "workspace clean; skipping commit/sync/post-sync");

    const auto includedStatus = RunGit({"status", "--short", "--", includedPath}, ctx.cloneRepo);
    RequireSuccess(includedStatus, "included status");
    REQUIRE(TrimCopy(includedStatus.stdoutText).empty());

    const auto committedContent = RunGit({"show", "HEAD:" + includedPath}, ctx.cloneRepo);
    RequireSuccess(committedContent, "read committed included content");
    REQUIRE(committedContent.stdoutText == "include me\n");

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

TEST_CASE("commit_push_plan_file_uses_git_add_for_tracked_ignored_exact_path",
          "[functional][commit-push][plan-file][pathspec][ignored][KG-TSK-0125]") {
    const auto ctx = CreateRemoteWithClone("plan-file-tracked-ignored");
    WriteTextFile(ctx.cloneRepo / "generated.cache", "tracked seed\n");
    WriteTextFile(ctx.cloneRepo / ".gitignore", ".kano/\n*.cache\n");
    RequireSuccess(RunGit({"add", ".gitignore"}, ctx.cloneRepo), "stage ignore rule");
    RequireSuccess(RunGit({"add", "-f", "generated.cache"}, ctx.cloneRepo), "force stage tracked ignored seed");
    RequireSuccess(RunGit({"commit", "-m", "seed tracked ignored path"}, ctx.cloneRepo), "commit tracked ignored seed");
    RequireSuccess(RunGit({"push", "origin", "HEAD:" + ctx.branch}, ctx.cloneRepo), "push tracked ignored seed");
    WriteTextFile(ctx.cloneRepo / "generated.cache", "tracked updated through git add\n");

    const auto planPath = ctx.cloneRepo / ".kano/cache/git/plans/tracked-ignored.json";
    RequireSuccess(RunKog({"plan", "new", "--force", "--output", planPath.string()}, ctx.cloneRepo), "plan new");
    RequireSuccess(RunKog({
        "plan", "prepare", "add-commit-entry", "--plan-file", planPath.string(),
        "--repo", ".", "--commit-message", "test(functional): stage tracked ignored path",
        "--commit-include", "generated.cache", "--commit-review-verdict", "pass",
        "--commit-review-reason", "tracked ignored exact paths require real git add"
    }, ctx.cloneRepo), "plan tracked ignored commit");

    const auto result = RunKogWithEnv(
        {"commit-push", "--plan-file", planPath.string()}, ctx.cloneRepo,
        {{"KOG_EXACT_PLAN_COMMIT_MODE", "plumbing"}});
    INFO(result.stdoutText); INFO(result.stderrText);
    REQUIRE(result.exitCode == 0);
    RequireNotContainsText(result.stdoutText, "exact cacheinfo staging paths=1");
    const auto committed = RunGit({"show", "HEAD:generated.cache"}, ctx.cloneRepo);
    RequireSuccess(committed, "read tracked ignored commit");
    REQUIRE(committed.stdoutText == "tracked updated through git add\n");
    REQUIRE(StatusPorcelain(ctx.cloneRepo).empty());
    RemoveSandboxWorkspace(ctx.sandbox);
}

TEST_CASE("commit_push_plan_file_scoped_reset_preserves_staged_rename_pair",
          "[functional][commit-push][plan-file][pathspec][rename][KG-TSK-0125]") {
    const auto ctx = CreateRemoteWithClone("plan-file-staged-rename");
    WriteTextFile(ctx.cloneRepo / "old-name.md", "rename payload\n");
    RequireSuccess(RunGit({"add", "old-name.md"}, ctx.cloneRepo), "stage rename seed");
    RequireSuccess(RunGit({"commit", "-m", "seed rename path"}, ctx.cloneRepo), "commit rename seed");
    RequireSuccess(RunGit({"push", "origin", "HEAD:" + ctx.branch}, ctx.cloneRepo), "push rename seed");
    RequireSuccess(RunGit({"mv", "old-name.md", "new-name.md"}, ctx.cloneRepo), "stage rename");

    const auto planPath = ctx.cloneRepo / ".kano/cache/git/plans/staged-rename.json";
    RequireSuccess(RunKog({"plan", "new", "--force", "--output", planPath.string()}, ctx.cloneRepo), "plan new");
    RequireSuccess(RunKog({
        "plan", "prepare", "add-commit-entry", "--plan-file", planPath.string(),
        "--repo", ".", "--commit-message", "test(functional): preserve staged rename",
        "--commit-include", "old-name.md", "--commit-review-verdict", "pass",
        "--commit-review-reason", "old and new rename paths share the scoped reset"
    }, ctx.cloneRepo), "plan staged rename commit");

    const auto result = RunKogWithEnv(
        {"commit-push", "--plan-file", planPath.string()}, ctx.cloneRepo,
        {{"KOG_EXACT_PLAN_COMMIT_MODE", "plumbing"}});
    INFO(result.stdoutText); INFO(result.stderrText);
    REQUIRE(result.exitCode == 0);
    REQUIRE_FALSE(std::filesystem::exists(ctx.cloneRepo / "old-name.md"));
    REQUIRE(std::filesystem::exists(ctx.cloneRepo / "new-name.md"));
    const auto changed = RunGit({"diff-tree", "--no-commit-id", "--name-status", "-r", "HEAD"}, ctx.cloneRepo);
    RequireSuccess(changed, "inspect rename commit");
    RequireContainsText(changed.stdoutText, "old-name.md");
    RequireContainsText(changed.stdoutText, "new-name.md");
    REQUIRE(StatusPorcelain(ctx.cloneRepo).empty());
    RemoveSandboxWorkspace(ctx.sandbox);
}

#if !defined(_WIN32)
TEST_CASE("commit_push_plan_file_secret_gate_preserves_tab_newline_filename bytes",
          "[functional][commit-push][plan-file][pathspec][nul][KG-TSK-0125]") {
    const auto ctx = CreateRemoteWithClone("plan-file-nul-safe-secret");
    const auto unusualRelative = std::string{"odd\tname\nsecret.txt"};
    WriteTextFile(ctx.cloneRepo / unusualRelative,
                  "AKIAABCDEFGHIJKLMNOP\n");
    const auto headBefore = CurrentHeadSha(ctx.cloneRepo);

    const auto planPath =
        ctx.cloneRepo / ".kano/cache/git/plans/nul-safe-secret.json";
    RequireSuccess(
        RunKog({"plan", "new", "--force", "--output", planPath.string()},
               ctx.cloneRepo),
        "plan new");
    RequireSuccess(RunKog({
        "plan", "prepare", "add-commit-entry", "--plan-file", planPath.string(),
        "--repo", ".", "--commit-message",
        "test(functional): exercise NUL-safe path discovery",
        "--commit-include", ".", "--commit-review-verdict", "pass",
        "--commit-review-reason", "path discovery must not split control bytes"
    }, ctx.cloneRepo), "plan unusual filename commit");

    const auto result = RunKog(
        {"commit-push", "--plan-file", planPath.string()}, ctx.cloneRepo);
    INFO(result.stdoutText);
    INFO(result.stderrText);
    REQUIRE(result.exitCode == 3);
    RequireContainsText(result.stdoutText + "\n" + result.stderrText,
                        "secret gate failed");
    REQUIRE(CurrentHeadSha(ctx.cloneRepo) == headBefore);
    REQUIRE(std::filesystem::exists(ctx.cloneRepo / unusualRelative));
    RequireAuditAttempt(
        planPath, 1, kano::git::audit::OutcomeState::Failed,
        {"audit.reserve", "plan.execution-lock", "plan.stage"});
    RemoveSandboxWorkspace(ctx.sandbox);
}
#endif

TEST_CASE("commit-push malformed correlation creates no execution lock or mutation",
          "[functional][commit-push][plan-file][admission][lock][KG-TSK-0125]") {
    const auto ctx = CreateRemoteWithClone("commit-push-malformed-correlation-no-lock");
    WriteTextFile(ctx.cloneRepo / "README.md", "seed\nmalformed correlation must not mutate\n");
    const auto planPath =
        ctx.cloneRepo / ".kano/cache/git/plans/malformed-correlation.json";
    RequireSuccess(
        RunKog({"plan", "new", "--force", "--output", planPath.string()},
               ctx.cloneRepo),
        "create malformed-correlation plan fixture");
    RequireSuccess(RunKog({
        "plan", "prepare", "add-commit-entry", "--plan-file", planPath.string(),
        "--repo", ".", "--commit-message",
        "test(functional): malformed correlation must not execute",
        "--commit-include", "README.md", "--commit-review-verdict", "pass",
        "--commit-review-reason", "admission must precede execution lock creation"
    }, ctx.cloneRepo), "prepare malformed-correlation plan fixture");

    auto malformed = nlohmann::json::parse(ReadTextFile(planPath));
    malformed["meta"]["correlation"]["mode"] = "koa";
    malformed["meta"]["correlation"]["product_id"] =
        "legacy/" + std::string(140, 'a') + "#id";
    malformed["meta"]["correlation"]["item_id"] = "item";
    malformed["meta"]["correlation"]["work_order_id"] = "work";
    malformed["meta"]["correlation"]["request_id"] = "request";
    malformed["meta"]["correlation"]["run_id"] = "run";
    malformed["meta"]["correlation"]["producer_id"] = "producer";
    malformed["meta"]["correlation"]["route_id"] = "commit-push.plan";
    WriteTextFile(planPath, malformed.dump(2) + "\n");

    const auto admittedBytes = ReadTextFile(planPath);
    const auto headBefore = CurrentHeadSha(ctx.cloneRepo);
    const auto statusBefore = StatusPorcelain(ctx.cloneRepo);
    const auto lockRoot =
        ctx.cloneRepo / ".kano/tmp/git/plan-execution-locks";
    REQUIRE_FALSE(std::filesystem::exists(lockRoot));

    for (const bool profiled : {false, true}) {
        std::vector<std::string> args = {
            "commit-push", "--plan-file", planPath.string()};
        if (profiled) args.push_back("--profile");
        const auto result = RunKog(args, ctx.cloneRepo);
        INFO("profiled=" << profiled);
        INFO(result.stdoutText);
        INFO(result.stderrText);
        REQUIRE(result.exitCode == 2);
        RequireContainsText(result.stdoutText + "\n" + result.stderrText,
                            "stable");
        REQUIRE(ReadTextFile(planPath) == admittedBytes);
        REQUIRE(CurrentHeadSha(ctx.cloneRepo) == headBefore);
        REQUIRE(StatusPorcelain(ctx.cloneRepo) == statusBefore);
        REQUIRE_FALSE(std::filesystem::exists(lockRoot));
        REQUIRE_FALSE(std::filesystem::exists(ctx.cloneRepo / ".git/kog/audit"));
    }

    RemoveSandboxWorkspace(ctx.sandbox);
}

TEST_CASE("both commit-push plan routes reject source changes after reservation",
          "[functional][commit-push][plan-file][admission][lock][toctou][KG-TSK-0125]") {
    for (const bool profiled : {false, true}) {
        const auto ctx = CreateRemoteWithClone(
            profiled ? "commit-push-general-post-reserve-race"
                     : "commit-push-fast-post-reserve-race");
        WriteTextFile(ctx.cloneRepo / "README.md",
                      "seed\npost-reservation source race\n");
        const auto planPath = ctx.cloneRepo / ".kano/cache/git/plans/racing-plan.json";
        RequireSuccess(
            RunKog({"plan", "new", "--force", "--output", planPath.string()},
                   ctx.cloneRepo),
            "create post-reservation race plan");
        RequireSuccess(RunKog({
            "plan", "prepare", "add-commit-entry", "--plan-file", planPath.string(),
            "--repo", ".", "--commit-message",
            "test(functional): reject post-reservation source race",
            "--commit-include", "README.md", "--commit-review-verdict", "pass",
            "--commit-review-reason", "exact admitted bytes must survive lock acquisition"
        }, ctx.cloneRepo), "prepare post-reservation race plan");

        auto changedPlan = nlohmann::json::parse(ReadTextFile(planPath));
        changedPlan["post_reservation_test_mutation"] = true;
        const auto changedBytes = changedPlan.dump(2) + "\n";
        std::error_code canonicalError;
        const auto canonicalPlan = std::filesystem::weakly_canonical(
            planPath, canonicalError);
        REQUIRE_FALSE(canonicalError);
        const auto auditRoot =
            ResolveGitMetadataPath(ctx.cloneRepo, "--git-common-dir") /
            "kog" / "audit" /
            ("plan-" + kano::git::audit::Sha256Hex(
                canonicalPlan.generic_string()));

        std::atomic<bool> stop{false};
        std::atomic<bool> changed{false};
        std::thread mutator([&]() {
            while (!stop.load(std::memory_order_relaxed) &&
                   !changed.load(std::memory_order_relaxed)) {
                std::error_code ec;
                if (std::filesystem::exists(auditRoot, ec) && !ec) {
                    for (std::filesystem::recursive_directory_iterator it(
                             auditRoot, ec), end;
                         !ec && it != end; it.increment(ec)) {
                        if (it->path().filename() != "publication-pending.json")
                            continue;
                        std::ofstream output(
                            planPath, std::ios::binary | std::ios::trunc);
                        if (output.good()) {
                            output << changedBytes;
                            output.flush();
                            changed.store(output.good(),
                                          std::memory_order_relaxed);
                        }
                        break;
                    }
                }
                if (!changed.load(std::memory_order_relaxed))
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
        });

        std::vector<std::string> args = {
            "commit-push", "--plan-file", planPath.string()};
        if (profiled) args.push_back("--profile");
        const auto headBefore = CurrentHeadSha(ctx.cloneRepo);
        const auto statusBefore = StatusPorcelain(ctx.cloneRepo);
        const auto result = RunKogWithEnv(
            args, ctx.cloneRepo,
            {{"KOG_TEST_MODE", "1"},
             {"KOG_TEST_ONLY_AUDIT_INPUT_POST_STAT_DELAY_MS", "750"},
             {"KOG_PROCESS_DIAGNOSTICS", "0"}});
        stop.store(true, std::memory_order_relaxed);
        mutator.join();

        INFO("profiled=" << profiled);
        INFO(result.stdoutText);
        INFO(result.stderrText);
        REQUIRE(changed.load(std::memory_order_relaxed));
        REQUIRE(result.exitCode == 2);
        RequireContainsText(result.stdoutText + "\n" + result.stderrText,
                            "lock/source admission");
        REQUIRE(CurrentHeadSha(ctx.cloneRepo) == headBefore);
        REQUIRE(StatusPorcelain(ctx.cloneRepo) == statusBefore);
        REQUIRE(ReadTextFile(planPath) == changedBytes);
        RequireAuditAttempt(
            planPath, 1, kano::git::audit::OutcomeState::Failed,
            {"audit.reserve", "plan.execution-lock", "plan.source.revalidate"});
        const auto attemptRoot = FindAuditAttemptRoot(planPath, 1);
        const auto events = kano::git::audit::ParseAuditEventsJsonl(
            ReadTextFile(attemptRoot / "events.jsonl"));
        REQUIRE(events.ok());
        REQUIRE(std::none_of(
            events.values.begin(), events.values.end(), [](const auto& event) {
                return event.action == "plan.stage";
            }));
        RemoveSandboxWorkspace(ctx.sandbox);
    }
}

TEST_CASE("both commit-push plan routes preserve a final pre-stamp source rewrite",
          "[functional][commit-push][plan-file][audit][stamp][toctou][KG-TSK-0125]") {
    for (const bool profiled : {false, true}) {
        const auto ctx = CreateRemoteWithClone(
            profiled ? "commit-push-general-final-stamp-race"
                     : "commit-push-fast-final-stamp-race");
        WriteTextFile(ctx.cloneRepo / "README.md",
                      "seed\nfinal execution stamp race\n");
        const auto planPath = ctx.cloneRepo /
            ".kano/cache/git/plans/final-execution-stamp-race.json";
        RequireSuccess(
            RunKog({"plan", "new", "--force", "--output",
                    planPath.string()},
                   ctx.cloneRepo),
            "create final execution stamp race plan");
        RequireSuccess(
            RunKog({
                "plan", "prepare", "add-commit-entry", "--plan-file",
                planPath.string(), "--repo", ".", "--commit-message",
                "test(functional): preserve final stamp source race",
                "--commit-include", "README.md", "--commit-review-verdict",
                "pass", "--commit-review-reason",
                "execution stamp must conditionally rewrite admitted bytes"},
                ctx.cloneRepo),
            "prepare final execution stamp race plan");

        auto changedPlan = nlohmann::json::parse(ReadTextFile(planPath));
        changedPlan["final_execution_stamp_test_mutation"] = true;
        const auto changedBytes = changedPlan.dump(2) + "\n";
        const auto hookRoot = ctx.cloneRepo /
            (".kano/tmp/git/plan-source-rewrite-test-hooks/commit-push-" +
             std::string(profiled ? "general" : "fast"));
        const auto readyPath = hookRoot / "ready";
        const auto releasePath = hookRoot / "release";
        std::vector<std::string> args = {
            "commit-push", "--plan-file", planPath.string()};
        if (profiled) args.push_back("--profile");
        const auto headBefore = CurrentHeadSha(ctx.cloneRepo);
        auto commitPushFuture = std::async(std::launch::async, [&]() {
            return RunKogWithEnv(
                args, ctx.cloneRepo,
                {{"KOG_TEST_MODE", "1"},
                 {"KOG_TEST_ONLY_PLAN_SOURCE_REWRITE_PHASE", "stamp"},
                 {"KOG_TEST_ONLY_PLAN_SOURCE_REWRITE_READY_FILE",
                  readyPath.string()},
                 {"KOG_TEST_ONLY_PLAN_SOURCE_REWRITE_RELEASE_FILE",
                  releasePath.string()},
                 {"KOG_PROCESS_DIAGNOSTICS", "0"}});
        });

        const bool rewriteGapReached =
            WaitForPath(readyPath, std::chrono::seconds(30));
        if (rewriteGapReached) WriteTextFile(planPath, changedBytes);
        WriteTextFile(releasePath, "release\n");
        const auto result = commitPushFuture.get();

        INFO("profiled=" << profiled);
        INFO(result.stdoutText);
        INFO(result.stderrText);
        REQUIRE(rewriteGapReached);
        REQUIRE(result.exitCode == 2);
        RequireContainsText(result.stdoutText + "\n" + result.stderrText,
                            "failed to stamp plan executed_at_utc");
        REQUIRE(ReadTextFile(planPath) == changedBytes);
        const auto headAfter = CurrentHeadSha(ctx.cloneRepo);
        REQUIRE(headAfter != headBefore);
        REQUIRE(RefSha(ctx.bareRemote, "refs/heads/" + ctx.branch) ==
                headAfter);
        REQUIRE(StatusPorcelain(ctx.cloneRepo).empty());
        RequireAuditAttempt(
            planPath, 1, kano::git::audit::OutcomeState::Failed,
            {"audit.reserve", "plan.execution-lock", "plan.stage",
             "commit.apply", "push", "plan.source.revalidate",
             "plan.stamp"});
        const auto events = kano::git::audit::ParseAuditEventsJsonl(
            ReadTextFile(FindAuditAttemptRoot(planPath, 1) / "events.jsonl"));
        REQUIRE(events.ok());
        const auto stamp = std::find_if(
            events.values.begin(), events.values.end(),
            [](const auto& event) { return event.action == "plan.stamp"; });
        REQUIRE(stamp != events.values.end());
        REQUIRE(stamp->outcome.status ==
                kano::git::audit::OutcomeState::Failed);
        RemoveSandboxWorkspace(ctx.sandbox);
    }
}

TEST_CASE("both commit-push plan routes restore exact source after audit finalization failure",
          "[functional][commit-push][plan-file][audit][stamp][restore][KG-TSK-0125]") {
    for (const bool profiled : {false, true}) {
        const auto ctx = CreateRemoteWithClone(
            profiled ? "commit-push-general-audit-restore"
                     : "commit-push-fast-audit-restore");
        WriteTextFile(ctx.cloneRepo / "README.md",
                      "seed\naudit finalization restore\n");
        const auto planPath = ctx.cloneRepo /
            ".kano/cache/git/plans/audit-finalization-restore.json";
        RequireSuccess(
            RunKog({"plan", "new", "--force", "--output",
                    planPath.string()},
                   ctx.cloneRepo),
            "create audit finalization restore plan");
        RequireSuccess(
            RunKog({
                "plan", "prepare", "add-commit-entry", "--plan-file",
                planPath.string(), "--repo", ".", "--commit-message",
                "test(functional): exact audit failure restore",
                "--commit-include", "README.md", "--commit-review-verdict",
                "pass", "--commit-review-reason",
                "audit failure must restore exact admitted source bytes"},
                ctx.cloneRepo),
            "prepare audit finalization restore plan");

        const auto admittedBytes = ReadTextFile(planPath);
        const auto headBefore = CurrentHeadSha(ctx.cloneRepo);
        std::vector<std::string> args = {
            "commit-push", "--plan-file", planPath.string()};
        if (profiled) args.push_back("--profile");
        const auto result = RunKogWithEnv(
            args, ctx.cloneRepo,
            {{"KOG_TEST_MODE", "1"},
             {"KOG_TEST_ONLY_AUDIT_FAIL_POST_PUBLISH_DIR_SYNC", "1"},
             {"KOG_PROCESS_DIAGNOSTICS", "0"}});

        INFO("profiled=" << profiled);
        INFO(result.stdoutText);
        INFO(result.stderrText);
        REQUIRE(result.exitCode == 2);
        RequireContainsText(result.stdoutText + "\n" + result.stderrText,
                            "audit terminalization failed");
        REQUIRE(ReadTextFile(planPath) == admittedBytes);
        const auto headAfter = CurrentHeadSha(ctx.cloneRepo);
        REQUIRE(headAfter != headBefore);
        REQUIRE(RefSha(ctx.bareRemote, "refs/heads/" + ctx.branch) ==
                headAfter);
        REQUIRE(StatusPorcelain(ctx.cloneRepo).empty());
        const auto attemptRoot = FindAuditAttemptRoot(planPath, 1);
        REQUIRE(std::filesystem::exists(attemptRoot / "receipt.json"));
        REQUIRE(std::filesystem::exists(
            attemptRoot / "publication-pending.json"));
        REQUIRE(std::filesystem::exists(attemptRoot / "incomplete.json"));
        const auto marker = nlohmann::json::parse(
            ReadTextFile(attemptRoot / "incomplete.json"));
        REQUIRE(marker.at("reasonCode") == "receipt-durability-uncertain");
        const auto events = kano::git::audit::ParseAuditEventsJsonl(
            ReadTextFile(attemptRoot / "events.jsonl"));
        REQUIRE(events.ok());
        REQUIRE(std::any_of(events.values.begin(), events.values.end(),
                            [](const auto& event) {
                                return event.action == "plan.stamp" &&
                                    event.outcome.status ==
                                    kano::git::audit::OutcomeState::Succeeded;
                            }));
        RemoveSandboxWorkspace(ctx.sandbox);
    }
}

TEST_CASE("direct commit rejects same-inode source rewrite after reservation",
          "[functional][commit][plan-file][admission][lock][toctou][KG-TSK-0125]") {
    const auto ctx = CreateRemoteWithClone("direct-commit-post-reserve-race");
    WriteTextFile(ctx.cloneRepo / "README.md",
                  "seed\ndirect post-reservation source race\n");
    const auto planPath =
        ctx.cloneRepo / ".kano/cache/git/plans/direct-racing-plan.json";
    RequireSuccess(
        RunKog({"plan", "new", "--force", "--output", planPath.string()},
               ctx.cloneRepo),
        "create direct post-reservation race plan");
    RequireSuccess(RunKog({
        "plan", "prepare", "add-commit-entry", "--plan-file", planPath.string(),
        "--repo", ".", "--commit-message",
        "test(functional): reject direct source race",
        "--commit-include", "README.md", "--commit-review-verdict", "pass",
        "--commit-review-reason", "direct commit must preserve admitted bytes"
    }, ctx.cloneRepo), "prepare direct post-reservation race plan");

    auto changedPlan = nlohmann::json::parse(ReadTextFile(planPath));
    changedPlan["direct_post_reservation_test_mutation"] = true;
    const auto changedBytes = changedPlan.dump(2) + "\n";
    const auto headBefore = CurrentHeadSha(ctx.cloneRepo);
    const auto statusBefore = StatusPorcelain(ctx.cloneRepo);
    const auto result = RunKogWithPostReserveSameInodeRewrite(
        {"commit", "--plan-file", planPath.string(), "--plan-stage", "commit"},
        ctx.cloneRepo, planPath, changedBytes);

    INFO(result.stdoutText);
    INFO(result.stderrText);
    REQUIRE(result.exitCode == 2);
    RequireContainsText(result.stdoutText + "\n" + result.stderrText,
                        "lock/source admission");
    REQUIRE(CurrentHeadSha(ctx.cloneRepo) == headBefore);
    REQUIRE(StatusPorcelain(ctx.cloneRepo) == statusBefore);
    REQUIRE(ReadTextFile(planPath) == changedBytes);
    RequireAuditAttempt(
        planPath, 1, kano::git::audit::OutcomeState::Failed,
        {"audit.reserve", "plan.execution-lock", "plan.source.revalidate"});
    const auto events = kano::git::audit::ParseAuditEventsJsonl(
        ReadTextFile(FindAuditAttemptRoot(planPath, 1) / "events.jsonl"));
    REQUIRE(events.ok());
    REQUIRE(std::none_of(events.values.begin(), events.values.end(),
                         [](const auto& event) {
                             return event.action == "plan.safety" ||
                                    event.action == "commit.apply";
                         }));
    RemoveSandboxWorkspace(ctx.sandbox);
}

TEST_CASE("plan apply rejects same-inode source rewrite after reservation",
          "[functional][plan][apply][admission][lock][toctou][KG-TSK-0125]") {
    const auto ctx = CreateRemoteWithClone("plan-apply-post-reserve-race");
    WriteTextFile(ctx.cloneRepo / "GeneratedProject.slnx", "generated\n");
    const auto planPath =
        ctx.cloneRepo / ".kano/cache/git/plans/apply-racing-plan.json";
    RequireSuccess(
        RunKog({"plan", "new", "--force", "--output", planPath.string()},
               ctx.cloneRepo),
        "create plan-apply post-reservation race plan");
    RequireSuccess(
        RunKog({"plan", "ignore-init", "--plan-file", planPath.string(),
                "--force"},
               ctx.cloneRepo),
        "prepare plan-apply ignore race plan");

    auto changedPlan = nlohmann::json::parse(ReadTextFile(planPath));
    changedPlan["apply_post_reservation_test_mutation"] = true;
    const auto changedBytes = changedPlan.dump(2) + "\n";
    const auto ignoreBefore = ReadTextFile(ctx.cloneRepo / ".gitignore");
    const auto statusBefore = StatusPorcelain(ctx.cloneRepo);
    const auto result = RunKogWithPostReserveSameInodeRewrite(
        {"plan", "apply", "--stage", "ignore", "--plan-file",
         planPath.string()},
        ctx.cloneRepo, planPath, changedBytes);

    INFO(result.stdoutText);
    INFO(result.stderrText);
    REQUIRE(result.exitCode == 2);
    RequireContainsText(result.stdoutText + "\n" + result.stderrText,
                        "lock/source admission");
    REQUIRE(ReadTextFile(ctx.cloneRepo / ".gitignore") == ignoreBefore);
    REQUIRE(StatusPorcelain(ctx.cloneRepo) == statusBefore);
    REQUIRE(ReadTextFile(planPath) == changedBytes);
    RequireAuditAttempt(
        planPath, 1, kano::git::audit::OutcomeState::Failed,
        {"audit.reserve", "plan.execution-lock", "plan.source.revalidate"});
    const auto events = kano::git::audit::ParseAuditEventsJsonl(
        ReadTextFile(FindAuditAttemptRoot(planPath, 1) / "events.jsonl"));
    REQUIRE(events.ok());
    REQUIRE(std::none_of(events.values.begin(), events.values.end(),
                         [](const auto& event) {
                             return event.action == "plan.ignore.apply" ||
                                    event.action == "plan.ignore.stamp";
                         }));
    RemoveSandboxWorkspace(ctx.sandbox);
}

TEST_CASE("plan apply preserves a final pre-stamp source rewrite",
          "[functional][plan][apply][audit][stamp][toctou][KG-TSK-0125]") {
    const auto ctx = CreateRemoteWithClone("plan-apply-final-pre-stamp-race");
    WriteTextFile(ctx.cloneRepo / "GeneratedProject.slnx", "generated\n");
    const auto planPath =
        ctx.cloneRepo / ".kano/cache/git/plans/final-stamp-racing-plan.json";
    RequireSuccess(
        RunKog({"plan", "new", "--force", "--output", planPath.string()},
               ctx.cloneRepo),
        "create final pre-stamp race plan");
    RequireSuccess(
        RunKog({"plan", "ignore-init", "--plan-file", planPath.string(),
                "--force"},
               ctx.cloneRepo),
        "prepare final pre-stamp ignore plan");

    auto changedPlan = nlohmann::json::parse(ReadTextFile(planPath));
    changedPlan["final_pre_stamp_test_mutation"] = true;
    const auto changedBytes = changedPlan.dump(2) + "\n";
    const auto ignoreBefore = ReadTextFile(ctx.cloneRepo / ".gitignore");
    const auto hookRoot = ctx.cloneRepo /
        ".kano/tmp/git/plan-source-rewrite-test-hooks/final-pre-stamp";
    const auto readyPath = hookRoot / "ready";
    const auto releasePath = hookRoot / "release";
    auto applyFuture = std::async(std::launch::async, [&]() {
        return RunKogWithEnv(
            {"plan", "apply", "--stage", "ignore", "--plan-file",
             planPath.string()},
            ctx.cloneRepo,
            {{"KOG_TEST_MODE", "1"},
             {"KOG_TEST_ONLY_PLAN_SOURCE_REWRITE_PHASE", "stamp"},
             {"KOG_TEST_ONLY_PLAN_SOURCE_REWRITE_READY_FILE",
              readyPath.string()},
             {"KOG_TEST_ONLY_PLAN_SOURCE_REWRITE_RELEASE_FILE",
              releasePath.string()},
             {"KOG_PROCESS_DIAGNOSTICS", "0"}});
    });

    const bool rewriteGapReached =
        WaitForPath(readyPath, std::chrono::seconds(5));
    if (rewriteGapReached) WriteTextFile(planPath, changedBytes);
    WriteTextFile(releasePath, "release\n");
    const auto result = applyFuture.get();

    INFO(result.stdoutText);
    INFO(result.stderrText);
    REQUIRE(rewriteGapReached);
    REQUIRE(result.exitCode == 2);
    RequireContainsText(result.stdoutText + "\n" + result.stderrText,
                        "plan source changed before applied_at_utc stamp");
    REQUIRE(ReadTextFile(planPath) == changedBytes);
    const auto ignoreAfter = ReadTextFile(ctx.cloneRepo / ".gitignore");
    REQUIRE(ignoreAfter != ignoreBefore);
    RequireContainsText(ignoreAfter, "*.slnx");
    RequireAuditAttempt(
        planPath, 1, kano::git::audit::OutcomeState::Failed,
        {"audit.reserve", "plan.execution-lock", "plan.ignore.apply",
         "plan.source.revalidate"});
    const auto events = kano::git::audit::ParseAuditEventsJsonl(
        ReadTextFile(FindAuditAttemptRoot(planPath, 1) / "events.jsonl"));
    REQUIRE(events.ok());
    REQUIRE(std::none_of(events.values.begin(), events.values.end(),
                         [](const auto& event) {
                             return event.action == "plan.ignore.stamp";
                         }));
    RemoveSandboxWorkspace(ctx.sandbox);
}

TEST_CASE("plan apply restores admitted source bytes after a partial stamp write",
          "[functional][plan][apply][audit][stamp][rollback][KG-TSK-0125]") {
    const auto ctx = CreateRemoteWithClone("plan-apply-partial-stamp-rollback");
    WriteTextFile(ctx.cloneRepo / "GeneratedProject.slnx", "generated\n");
    const auto planPath = ctx.cloneRepo /
        ".kano/cache/git/plans/partial-stamp-rollback-plan.json";
    RequireSuccess(
        RunKog({"plan", "new", "--force", "--output", planPath.string()},
               ctx.cloneRepo),
        "create partial stamp rollback plan");
    RequireSuccess(
        RunKog({"plan", "ignore-init", "--plan-file", planPath.string(),
                "--force"},
               ctx.cloneRepo),
        "prepare partial stamp rollback ignore plan");

    const auto admittedBytes = ReadTextFile(planPath);
    const auto ignoreBefore = ReadTextFile(ctx.cloneRepo / ".gitignore");
    const auto result = RunKogWithEnv(
        {"plan", "apply", "--stage", "ignore", "--plan-file",
         planPath.string()},
        ctx.cloneRepo,
        {{"KOG_TEST_MODE", "1"},
         {"KOG_TEST_ONLY_PLAN_SOURCE_REWRITE_PHASE", "stamp"},
         {"KOG_TEST_ONLY_PLAN_SOURCE_REWRITE_FAIL_AFTER_FIRST_WRITE", "1"},
         {"KOG_PROCESS_DIAGNOSTICS", "0"}});

    INFO(result.stdoutText);
    INFO(result.stderrText);
    REQUIRE(result.exitCode == 2);
    RequireContainsText(result.stdoutText + "\n" + result.stderrText,
                        "injected failure after first plan source write");
    REQUIRE(ReadTextFile(planPath) == admittedBytes);
    const auto ignoreAfter = ReadTextFile(ctx.cloneRepo / ".gitignore");
    REQUIRE(ignoreAfter != ignoreBefore);
    RequireContainsText(ignoreAfter, "*.slnx");
    RequireAuditAttempt(
        planPath, 1, kano::git::audit::OutcomeState::Failed,
        {"audit.reserve", "plan.execution-lock", "plan.ignore.apply",
         "plan.source.revalidate"});
    const auto events = kano::git::audit::ParseAuditEventsJsonl(
        ReadTextFile(FindAuditAttemptRoot(planPath, 1) / "events.jsonl"));
    REQUIRE(events.ok());
    REQUIRE(std::none_of(events.values.begin(), events.values.end(),
                         [](const auto& event) {
                             return event.action == "plan.ignore.stamp";
                         }));
    RemoveSandboxWorkspace(ctx.sandbox);
}

#if !defined(_WIN32)
TEST_CASE("plan apply fails promptly when the plan source lock is contended",
          "[functional][plan][apply][audit][stamp][lock][KG-TSK-0125]") {
    const auto ctx = CreateRemoteWithClone("plan-apply-source-lock-contention");
    WriteTextFile(ctx.cloneRepo / "GeneratedProject.slnx", "generated\n");
    const auto planPath = ctx.cloneRepo /
        ".kano/cache/git/plans/source-lock-contention-plan.json";
    RequireSuccess(
        RunKog({"plan", "new", "--force", "--output", planPath.string()},
               ctx.cloneRepo),
        "create source lock contention plan");
    RequireSuccess(
        RunKog({"plan", "ignore-init", "--plan-file", planPath.string(),
                "--force"},
               ctx.cloneRepo),
        "prepare source lock contention ignore plan");

    const auto admittedBytes = ReadTextFile(planPath);
    const auto ignoreBefore = ReadTextFile(ctx.cloneRepo / ".gitignore");
    const int sourceHandle = ::open(planPath.c_str(), O_RDWR);
    REQUIRE(sourceHandle >= 0);
    REQUIRE(::flock(sourceHandle, LOCK_EX | LOCK_NB) == 0);
    const auto result = RunKogWithEnv(
        {"plan", "apply", "--stage", "ignore", "--plan-file",
         planPath.string()},
        ctx.cloneRepo, {{"KOG_PROCESS_DIAGNOSTICS", "0"}});
    REQUIRE(::flock(sourceHandle, LOCK_UN) == 0);
    REQUIRE(::close(sourceHandle) == 0);

    INFO(result.stdoutText);
    INFO(result.stderrText);
    REQUIRE(result.exitCode == 2);
    REQUIRE(result.elapsedMs < 5000);
    RequireContainsText(result.stdoutText + "\n" + result.stderrText,
                        "cannot lock plan source handle");
    REQUIRE(ReadTextFile(planPath) == admittedBytes);
    const auto ignoreAfter = ReadTextFile(ctx.cloneRepo / ".gitignore");
    REQUIRE(ignoreAfter != ignoreBefore);
    RequireContainsText(ignoreAfter, "*.slnx");
    RequireAuditAttempt(
        planPath, 1, kano::git::audit::OutcomeState::Failed,
        {"audit.reserve", "plan.execution-lock", "plan.ignore.apply",
         "plan.source.revalidate"});
    const auto events = kano::git::audit::ParseAuditEventsJsonl(
        ReadTextFile(FindAuditAttemptRoot(planPath, 1) / "events.jsonl"));
    REQUIRE(events.ok());
    REQUIRE(std::none_of(events.values.begin(), events.values.end(),
                         [](const auto& event) {
                             return event.action == "plan.ignore.stamp";
                         }));
    RemoveSandboxWorkspace(ctx.sandbox);
}
#endif

TEST_CASE("plan apply commit and all reuse execution admission",
          "[functional][plan][apply][admission][lock][reentrant][KG-TSK-0125]") {
    for (const std::string stage : {"commit", "all"}) {
        INFO("stage=" << stage);
        const auto ctx =
            CreateRemoteWithClone("plan-apply-reentrant-" + stage);
        WriteTextFile(ctx.cloneRepo / "README.md",
                      "seed\nreentrant plan apply " + stage + "\n");
        if (stage == "all")
            WriteTextFile(ctx.cloneRepo / "GeneratedProject.slnx",
                          "generated\n");
        const auto planPath = ctx.cloneRepo /
            (".kano/cache/git/plans/reentrant-" + stage + "-plan.json");
        RequireSuccess(
            RunKog({"plan", "new", "--force", "--output",
                    planPath.string()},
                   ctx.cloneRepo),
            "create reentrant plan");
        if (stage == "all") {
            RequireSuccess(
                RunKog({"plan", "ignore-init", "--plan-file",
                        planPath.string(), "--force"},
                       ctx.cloneRepo),
                "prepare reentrant all-stage ignore plan");
        }
        std::vector<std::string> prepareArgs = {
            "plan", "prepare", "add-commit-entry", "--plan-file",
            planPath.string(), "--repo", ".", "--commit-message",
            "test(functional): reentrant plan apply " + stage,
            "--commit-include", "README.md"};
        if (stage == "all") {
            prepareArgs.push_back("--commit-include");
            prepareArgs.push_back(".gitignore");
        }
        prepareArgs.insert(
            prepareArgs.end(),
            {"--commit-review-verdict", "pass", "--commit-review-reason",
             "nested commit pipeline must reuse plan execution admission"});
        RequireSuccess(RunKog(prepareArgs, ctx.cloneRepo),
                       "prepare reentrant commit entry");

        const auto headBefore = CurrentHeadSha(ctx.cloneRepo);
        const auto result = RunKog(
            {"plan", "apply", "--stage", stage, "--plan-file",
             planPath.string()},
            ctx.cloneRepo);
        INFO(result.stdoutText);
        INFO(result.stderrText);
        REQUIRE(result.exitCode == 0);
        const auto headAfter = CurrentHeadSha(ctx.cloneRepo);
        REQUIRE(headAfter != headBefore);
        REQUIRE(RefSha(ctx.bareRemote, "refs/heads/" + ctx.branch) ==
                headAfter);
        REQUIRE(StatusPorcelain(ctx.cloneRepo).empty());
        const auto lockRoot =
            ResolveGitMetadataPath(ctx.cloneRepo, "--git-common-dir") /
            "kog" / "plan-execution-locks";
        std::error_code lockError;
        REQUIRE(std::filesystem::is_directory(lockRoot, lockError));
        REQUIRE_FALSE(lockError);
        REQUIRE(std::any_of(
            std::filesystem::directory_iterator(lockRoot),
            std::filesystem::directory_iterator{},
            [](const auto& entry) {
                std::error_code ec;
                return entry.is_regular_file(ec) && !ec &&
                    entry.path().extension() == ".lock";
            }));
        REQUIRE_FALSE(std::filesystem::exists(
            ctx.cloneRepo / ".kano/tmp/git/plan-execution-locks"));
        const auto executedAt =
            ExtractJsonStringField(ReadTextFile(planPath), "executed_at_utc");
        REQUIRE_FALSE(executedAt.empty());

        const auto attemptRoots = FindAuditAttemptRoots(planPath, 1);
        REQUIRE(attemptRoots.size() == 1);
        const auto events = kano::git::audit::ParseAuditEventsJsonl(
            ReadTextFile(attemptRoots.front() / "events.jsonl"));
        REQUIRE(events.ok());
        const auto receipt = kano::git::audit::ParseRunReceiptJson(
            ReadTextFile(attemptRoots.front() / "receipt.json"));
        REQUIRE(receipt.ok());
        REQUIRE(receipt.value->terminalOutcome.status ==
                kano::git::audit::OutcomeState::Succeeded);
        REQUIRE(kano::git::audit::ValidateRunTrace(
                    *receipt.value, events.values)
                    .ok());
        const auto actionCount = [&](const std::string_view action) {
            return static_cast<std::size_t>(std::count_if(
                events.values.begin(), events.values.end(),
                [&](const auto& event) { return event.action == action; }));
        };
        REQUIRE(actionCount("audit.reserve") == 1);
        REQUIRE(actionCount("plan.execution-lock") >= 2);
        REQUIRE(actionCount("commit.apply") >= 1);
        REQUIRE(actionCount("push") == 1);
        REQUIRE(actionCount("plan.stamp") == 1);
        REQUIRE(actionCount("plan.verify") == 1);
        RemoveSandboxWorkspace(ctx.sandbox);
    }
}

TEST_CASE("Git plan execution lock resolution fails closed before mutation",
          "[functional][commit-push][plan-file][admission][lock][failure][KG-TSK-0125]") {
    const auto ctx =
        CreateRemoteWithClone("plan-execution-common-dir-probe-failure");
    WriteTextFile(ctx.cloneRepo / "README.md",
                  "seed\ncommon Git metadata probe must fail closed\n");
    const auto planPath =
        ctx.cloneRepo / ".kano/cache/git/plans/common-dir-failure.json";
    RequireSuccess(
        RunKog({"plan", "new", "--force", "--output", planPath.string()},
               ctx.cloneRepo),
        "create common-dir failure plan");
    RequireSuccess(
        RunKog({
            "plan", "prepare", "add-commit-entry", "--plan-file",
            planPath.string(), "--repo", ".", "--commit-message",
            "test(functional): fail closed on Git metadata probe failure",
            "--commit-include", "README.md", "--commit-review-verdict",
            "pass", "--commit-review-reason",
            "execution locks must never fall back into a Git worktree"},
            ctx.cloneRepo),
        "prepare common-dir failure plan");

    const auto admittedBytes = ReadTextFile(planPath);
    const auto headBefore = CurrentHeadSha(ctx.cloneRepo);
    const auto statusBefore = StatusPorcelain(ctx.cloneRepo);
    const auto result = RunKogWithEnv(
        {"commit-push", "--plan-file", planPath.string()}, ctx.cloneRepo,
        {{"KOG_TEST_MODE", "1"},
         {"KOG_TEST_ONLY_PLAN_EXECUTION_COMMON_DIR_FAILURE", "1"}});
    INFO(result.stdoutText);
    INFO(result.stderrText);
    REQUIRE(result.exitCode == 2);
    RequireContainsText(result.stdoutText + "\n" + result.stderrText,
                        "cannot resolve common Git metadata directory");
    REQUIRE(ReadTextFile(planPath) == admittedBytes);
    REQUIRE(CurrentHeadSha(ctx.cloneRepo) == headBefore);
    REQUIRE(StatusPorcelain(ctx.cloneRepo) == statusBefore);
    REQUIRE_FALSE(std::filesystem::exists(
        ctx.cloneRepo / ".kano/tmp/git/plan-execution-locks"));
    REQUIRE_FALSE(std::filesystem::exists(
        ctx.cloneRepo / ".git/kog/plan-execution-locks"));
    RequireAuditAttempt(
        planPath, 1, kano::git::audit::OutcomeState::Failed,
        {"audit.reserve", "plan.execution-lock"});
    RemoveSandboxWorkspace(ctx.sandbox);
}

TEST_CASE("linked worktree plan locks persist only in common Git metadata",
          "[functional][plan][apply][admission][lock][worktree][KG-TSK-0125]") {
    const auto ctx = CreateRemoteWithClone("plan-execution-linked-worktree");
    const auto linkedWorktree = ctx.sandbox.root / "linked";
    RequireSuccess(
        RunGit({"worktree", "add", "-b", "linked-lock-test",
                linkedWorktree.string()},
               ctx.cloneRepo),
        "create linked worktree for execution lock test");
    WriteTextFile(linkedWorktree / "GeneratedProject.slnx", "generated\n");
    const auto planPath = ctx.sandbox.root / "linked-plan.json";
    RequireSuccess(
        RunKog({"plan", "new", "--force", "--output", planPath.string()},
               linkedWorktree),
        "create linked-worktree plan");
    RequireSuccess(
        RunKog({"plan", "ignore-init", "--plan-file", planPath.string(),
                "--force"},
               linkedWorktree),
        "prepare linked-worktree ignore plan");

    const auto gitDirectory =
        ResolveGitMetadataPath(linkedWorktree, "--git-dir");
    const auto commonDirectory =
        ResolveGitMetadataPath(linkedWorktree, "--git-common-dir");
    REQUIRE(gitDirectory != commonDirectory);

    const auto applyIgnore = [&]() {
        const auto result = RunKog(
            {"plan", "apply", "--stage", "ignore", "--plan-file",
             planPath.string()},
            linkedWorktree);
        INFO(result.stdoutText);
        INFO(result.stderrText);
        REQUIRE(result.exitCode == 0);
    };
    applyIgnore();

    const auto lockRoot =
        commonDirectory / "kog" / "plan-execution-locks";
    const auto countLocks = [&]() {
        std::size_t count = 0;
        std::error_code ec;
        for (std::filesystem::directory_iterator it(lockRoot, ec), end;
             !ec && it != end; it.increment(ec)) {
            if (it->is_regular_file(ec) && !ec &&
                it->path().extension() == ".lock") {
                ++count;
            }
        }
        REQUIRE_FALSE(ec);
        return count;
    };
    REQUIRE(countLocks() == 1);
    REQUIRE_FALSE(std::filesystem::exists(
        gitDirectory / "kog/plan-execution-locks"));
    REQUIRE_FALSE(std::filesystem::exists(
        linkedWorktree / ".kano/tmp/git/plan-execution-locks"));
    RequireNotContainsText(StatusPorcelain(linkedWorktree),
                           "plan-execution-locks");

    applyIgnore();
    REQUIRE(countLocks() == 1);
    REQUIRE_FALSE(std::filesystem::exists(
        gitDirectory / "kog/plan-execution-locks"));
    REQUIRE_FALSE(std::filesystem::exists(
        linkedWorktree / ".kano/tmp/git/plan-execution-locks"));
    RemoveSandboxWorkspace(ctx.sandbox);
}

#if !defined(_WIN32)
TEST_CASE("plan execution lock rejects a Git metadata ancestor symlink",
          "[functional][commit-push][plan-file][admission][lock][symlink][KG-TSK-0125]") {
    const auto ctx =
        CreateRemoteWithClone("plan-execution-lock-ancestor-symlink");
    WriteTextFile(ctx.cloneRepo / "README.md",
                  "seed\nlock ancestors must not follow symlinks\n");
    const auto planPath =
        ctx.cloneRepo / ".kano/cache/git/plans/lock-symlink.json";
    RequireSuccess(
        RunKog({"plan", "new", "--force", "--output", planPath.string()},
               ctx.cloneRepo),
        "create lock ancestor symlink plan");
    RequireSuccess(
        RunKog({
            "plan", "prepare", "add-commit-entry", "--plan-file",
            planPath.string(), "--repo", ".", "--commit-message",
            "test(functional): reject lock ancestor symlink",
            "--commit-include", "README.md", "--commit-review-verdict",
            "pass", "--commit-review-reason",
            "Git metadata lock traversal must remain directory anchored"},
            ctx.cloneRepo),
        "prepare lock ancestor symlink plan");

    const auto outsideRoot = ctx.sandbox.root / "outside-lock-root";
    std::filesystem::create_directories(outsideRoot);
    const auto commonDirectory =
        ResolveGitMetadataPath(ctx.cloneRepo, "--git-common-dir");
    std::filesystem::create_directories(commonDirectory / "kog");
    std::error_code linkError;
    std::filesystem::create_directory_symlink(
        outsideRoot, commonDirectory / "kog/plan-execution-locks",
        linkError);
    REQUIRE_FALSE(linkError);

    const auto admittedBytes = ReadTextFile(planPath);
    const auto headBefore = CurrentHeadSha(ctx.cloneRepo);
    const auto statusBefore = StatusPorcelain(ctx.cloneRepo);
    const auto result = RunKog(
        {"commit-push", "--plan-file", planPath.string()}, ctx.cloneRepo);
    INFO(result.stdoutText);
    INFO(result.stderrText);
    REQUIRE(result.exitCode == 2);
    RequireContainsText(result.stdoutText + "\n" + result.stderrText,
                        "cannot open plan execution lock directory");
    REQUIRE(ReadTextFile(planPath) == admittedBytes);
    REQUIRE(CurrentHeadSha(ctx.cloneRepo) == headBefore);
    REQUIRE(StatusPorcelain(ctx.cloneRepo) == statusBefore);
    REQUIRE(std::filesystem::directory_iterator(outsideRoot) ==
            std::filesystem::directory_iterator{});
    RequireAuditAttempt(
        planPath, 1, kano::git::audit::OutcomeState::Failed,
        {"audit.reserve", "plan.execution-lock"});
    RemoveSandboxWorkspace(ctx.sandbox);
}

TEST_CASE("plan execution lock rejects a non-Git fallback directory symlink",
          "[functional][commit-push][plan-file][admission][lock][non-git][symlink][KG-TSK-0125]") {
    const auto ctx =
        CreateRemoteWithClone("plan-execution-lock-non-git-symlink");
    const auto sourcePlan =
        ctx.cloneRepo / ".kano/cache/git/plans/non-git-lock.json";
    RequireSuccess(
        RunKog({"plan", "new", "--force", "--output",
                sourcePlan.string()},
               ctx.cloneRepo),
        "create valid source plan for non-Git lock fallback");

    const auto nonGitWorkspace = ctx.sandbox.root / "non-git-workspace";
    std::filesystem::create_directories(nonGitWorkspace);
    const auto planPath = nonGitWorkspace / "non-git-lock.json";
    WriteTextFile(planPath, ReadTextFile(sourcePlan));

    const auto outsideRoot = ctx.sandbox.root / "outside-non-git-lock-root";
    std::filesystem::create_directories(outsideRoot);
    std::filesystem::create_directories(nonGitWorkspace / ".kano/tmp/git");
    std::error_code linkError;
    std::filesystem::create_directory_symlink(
        outsideRoot,
        nonGitWorkspace / ".kano/tmp/git/plan-execution-locks",
        linkError);
    REQUIRE_FALSE(linkError);

    const auto admittedBytes = ReadTextFile(planPath);
    const auto result = RunKog(
        {"commit-push", "--plan-file", planPath.string()},
        nonGitWorkspace);
    INFO(result.stdoutText);
    INFO(result.stderrText);
    REQUIRE(result.exitCode == 2);
    RequireContainsText(result.stdoutText + "\n" + result.stderrText,
                        "cannot open plan execution lock directory");
    REQUIRE(ReadTextFile(planPath) == admittedBytes);
    REQUIRE(std::filesystem::directory_iterator(outsideRoot) ==
            std::filesystem::directory_iterator{});
    RequireAuditAttempt(
        planPath, 1, kano::git::audit::OutcomeState::Failed,
        {"audit.reserve", "plan.execution-lock"});
    RemoveSandboxWorkspace(ctx.sandbox);
}

TEST_CASE("plan apply holds execution lock against a competing KOG writer",
          "[functional][plan][apply][admission][lock][concurrency][KG-TSK-0125]") {
    const auto ctx = CreateRemoteWithClone("plan-apply-competing-writer-lock");
    WriteTextFile(ctx.cloneRepo / "GeneratedProject.slnx", "generated\n");
    const auto planPath =
        ctx.cloneRepo / ".kano/cache/git/plans/locked-apply-plan.json";
    RequireSuccess(
        RunKog({"plan", "new", "--force", "--output", planPath.string()},
               ctx.cloneRepo),
        "create competing-writer plan");
    RequireSuccess(
        RunKog({"plan", "ignore-init", "--plan-file", planPath.string(),
                "--force"},
               ctx.cloneRepo),
        "prepare competing-writer ignore plan");
    const auto aliasPath =
        planPath.parent_path() / "locked-apply-plan-alias.json";
    std::error_code aliasError;
    std::filesystem::create_hard_link(planPath, aliasPath, aliasError);
    REQUIRE_FALSE(aliasError);

    const auto hookRoot = ctx.cloneRepo /
        ".kano/tmp/git/plan-execution-test-hooks/competing-writer";
    const auto readyPath = hookRoot / "ready";
    const auto releasePath = hookRoot / "release";
    const std::vector<std::string> applyArgs = {
        "plan", "apply", "--stage", "ignore", "--plan-file",
        planPath.string()};
    const std::vector<std::string> aliasApplyArgs = {
        "plan", "apply", "--stage", "ignore", "--plan-file",
        aliasPath.string()};
    auto firstFuture = std::async(std::launch::async, [&]() {
        return RunKogInIsolatedChild(
            applyArgs, ctx.cloneRepo,
            {{"KOG_TEST_MODE", "1"},
             {"KOG_TEST_ONLY_PLAN_EXECUTION_LOCK_READY_FILE",
              readyPath.string()},
             {"KOG_TEST_ONLY_PLAN_EXECUTION_LOCK_RELEASE_FILE",
              releasePath.string()}});
    });

    const bool firstHoldsLock =
        WaitForPath(readyPath, std::chrono::seconds(5));
    std::optional<CommandResult> competing;
    if (firstHoldsLock) {
        competing = RunKogInIsolatedChild(
            aliasApplyArgs, ctx.cloneRepo,
            {{"KOG_TEST_MODE", "1"},
             {"KOG_TEST_ONLY_PLAN_EXECUTION_LOCK_TIMEOUT_MS", "250"}});
    }
    WriteTextFile(releasePath, "release\n");
    const auto first = firstFuture.get();

    INFO(first.stdoutText);
    INFO(first.stderrText);
    REQUIRE(firstHoldsLock);
    REQUIRE(competing.has_value());
    REQUIRE(first.exitCode == 0);
    INFO(competing->stdoutText);
    INFO(competing->stderrText);
    REQUIRE(competing->exitCode == 2);
    RequireContainsText(competing->stdoutText + "\n" + competing->stderrText,
                        "plan execution lock busy");
    RequireContainsText(ReadTextFile(ctx.cloneRepo / ".gitignore"), "*.slnx");
    auto attemptRoots = FindAuditAttemptRoots(planPath, 1);
    const auto aliasAttemptRoots = FindAuditAttemptRoots(aliasPath, 1);
    attemptRoots.insert(attemptRoots.end(), aliasAttemptRoots.begin(),
                        aliasAttemptRoots.end());
    REQUIRE(attemptRoots.size() == 2);
    std::size_t succeeded = 0;
    std::size_t failed = 0;
    for (const auto& attemptRoot : attemptRoots) {
        const auto receipt = kano::git::audit::ParseRunReceiptJson(
            ReadTextFile(attemptRoot / "receipt.json"));
        REQUIRE(receipt.ok());
        if (receipt.value->terminalOutcome.status ==
            kano::git::audit::OutcomeState::Succeeded) {
            ++succeeded;
            RequireAuditAttemptRoot(
                attemptRoot, kano::git::audit::OutcomeState::Succeeded,
                {"audit.reserve", "plan.execution-lock", "plan.ignore.apply",
                 "plan.ignore.stamp"});
        } else {
            ++failed;
            RequireAuditAttemptRoot(
                attemptRoot, kano::git::audit::OutcomeState::Failed,
                {"audit.reserve", "plan.execution-lock"});
            const auto events = kano::git::audit::ParseAuditEventsJsonl(
                ReadTextFile(attemptRoot / "events.jsonl"));
            REQUIRE(events.ok());
            REQUIRE(std::none_of(
                events.values.begin(), events.values.end(),
                [](const auto& event) {
                    return event.action == "plan.ignore.apply" ||
                           event.action == "plan.ignore.stamp";
                }));
        }
    }
    REQUIRE(succeeded == 1);
    REQUIRE(failed == 1);
    RemoveSandboxWorkspace(ctx.sandbox);
}
#endif

TEST_CASE("amend rejects KOA-correlated plans before repository mutation",
          "[functional][amend][plan-file][audit][failure][KG-TSK-0125]") {
    const auto ctx = CreateRemoteWithClone("amend-rejects-koa-plan");
    WriteTextFile(ctx.cloneRepo / "README.md", "seed\nshould remain unamended\n");
    const auto planPath =
        ctx.cloneRepo / ".kano/cache/git/plans/koa-amend-unsupported.json";
    RequireSuccess(
        RunKog({"plan", "new", "--force", "--output", planPath.string()},
               ctx.cloneRepo),
        "plan new for rejected KOA amend");
    RequireSuccess(RunKog({
        "plan", "prepare", "add-commit-entry", "--plan-file", planPath.string(),
        "--repo", ".", "--commit-message",
        "test(functional): must not amend through unsupported route",
        "--commit-include", "README.md", "--commit-review-verdict", "pass",
        "--commit-review-reason", "amend.plan is not an audited capability"
    }, ctx.cloneRepo), "prepare rejected KOA amend plan");

    auto plan = nlohmann::json::parse(ReadTextFile(planPath));
    plan["meta"]["correlation"] = {
        {"mode", "koa"}, {"product_id", "product-demo"},
        {"topic_id", "topic-demo"}, {"item_id", "item-demo"},
        {"work_order_id", "work-order-demo"},
        {"request_id", "request-demo"}, {"run_id", "run-demo"},
        {"parent_run_id", nullptr}, {"producer_id", "koa"},
        {"route_id", "amend.plan"}, {"attempt", 1},
    };
    WriteTextFile(planPath, plan.dump(2) + "\n");

    const auto headBefore = CurrentHeadSha(ctx.cloneRepo);
    const auto statusBefore = StatusPorcelain(ctx.cloneRepo);
    const auto result =
        RunKog({"amend", "--plan-file", planPath.string()}, ctx.cloneRepo);
    INFO(result.stdoutText);
    INFO(result.stderrText);
    REQUIRE(result.exitCode == 2);
    RequireContainsText(result.stdoutText + "\n" + result.stderrText,
                        "KOA-correlated plans are unsupported");
    REQUIRE(CurrentHeadSha(ctx.cloneRepo) == headBefore);
    REQUIRE(StatusPorcelain(ctx.cloneRepo) == statusBefore);

    RemoveSandboxWorkspace(ctx.sandbox);
}

TEST_CASE("plan prepare and verify preserve exact KOA correlation envelope",
          "[functional][plan][correlation][prepare][verify][KG-TSK-0125]") {
    const auto ctx = CreateRemoteWithClone("plan-preserves-koa-correlation");
    WriteTextFile(ctx.cloneRepo / "README.md", "seed\ncorrelated change\n");
    const auto planPath =
        ctx.cloneRepo / ".kano/cache/git/plans/preserve-correlation.json";
    RequireSuccess(
        RunKog({"plan", "new", "--force", "--output", planPath.string()},
               ctx.cloneRepo),
        "plan new for correlation preservation");

    auto plan = nlohmann::json::parse(ReadTextFile(planPath));
    const nlohmann::json correlation = {
        {"mode", "koa"}, {"product_id", "product-demo"},
        {"topic_id", "topic-demo"}, {"item_id", "item-demo"},
        {"work_order_id", "work-order-demo"},
        {"request_id", "request-demo"}, {"run_id", "run-demo"},
        {"parent_run_id", "run-parent"}, {"producer_id", "koa"},
        {"route_id", "commit.plan"}, {"attempt", 3},
    };
    plan["meta"]["correlation"] = correlation;
    WriteTextFile(planPath, plan.dump(2) + "\n");

    RequireSuccess(RunKog({
        "plan", "prepare", "add-commit-entry", "--plan-file", planPath.string(),
        "--repo", ".", "--commit-message",
        "test(functional): preserve KOA correlation",
        "--commit-include", "README.md", "--commit-review-verdict", "pass",
        "--commit-review-reason", "preparation must retain the caller envelope"
    }, ctx.cloneRepo), "plan prepare with KOA correlation");
    REQUIRE(nlohmann::json::parse(ReadTextFile(planPath))
                .at("meta").at("correlation") == correlation);

    RequireSuccess(
        RunKog({"plan", "verify", "pre-apply", "--stage", "commit",
                "--plan-file", planPath.string()},
               ctx.cloneRepo),
        "plan verify with KOA correlation");
    REQUIRE(nlohmann::json::parse(ReadTextFile(planPath))
                .at("meta").at("correlation") == correlation);

    RemoveSandboxWorkspace(ctx.sandbox);
}

TEST_CASE("plan new rejects symlinked correlation envelope input",
          "[functional][plan][correlation][input-boundary][symlink][KG-TSK-0125]") {
    const auto ctx = CreateRemoteWithClone("plan-correlation-symlink");
    const auto realEnvelope = ctx.cloneRepo / ".kano/tmp/correlation-real.json";
    const auto linkedEnvelope = ctx.cloneRepo / ".kano/tmp/correlation-link.json";
    const auto planPath = ctx.cloneRepo / ".kano/cache/git/plans/symlink-correlation.json";
    WriteTextFile(realEnvelope,
        R"({"mode":"koa","product_id":"product","topic_id":"topic","item_id":"item","work_order_id":"work","request_id":"request","run_id":"run","parent_run_id":"parent","producer_id":"producer","route_id":"commit.plan","attempt":1})");
    std::error_code symlinkError;
    std::filesystem::create_symlink(realEnvelope, linkedEnvelope, symlinkError);
    if (symlinkError) {
        RemoveSandboxWorkspace(ctx.sandbox);
        SKIP("filesystem symlink creation is unavailable: " << symlinkError.message());
    }

    const auto result = RunKog({
        "plan", "new", "--force", "--output", planPath.string(),
        "--correlation-file", linkedEnvelope.string()
    }, ctx.cloneRepo);
    INFO(result.stdoutText);
    INFO(result.stderrText);
    REQUIRE(result.exitCode == 2);
    RequireContainsText(result.stdoutText + "\n" + result.stderrText,
                        "bounded input");
    REQUIRE_FALSE(std::filesystem::exists(planPath));
    RemoveSandboxWorkspace(ctx.sandbox);
}

TEST_CASE("plan new rejects oversized correlation envelope input",
          "[functional][plan][correlation][input-boundary][oversized][KG-TSK-0125]") {
    const auto ctx = CreateRemoteWithClone("plan-correlation-oversized");
    const auto envelopePath = ctx.cloneRepo / ".kano/tmp/correlation-oversized.json";
    const auto planPath = ctx.cloneRepo / ".kano/cache/git/plans/oversized-correlation.json";
    WriteTextFile(envelopePath, std::string((64U << 10U) + 1U, 'x'));

    const auto result = RunKog({
        "plan", "new", "--force", "--output", planPath.string(),
        "--correlation-file", envelopePath.string()
    }, ctx.cloneRepo);
    INFO(result.stdoutText);
    INFO(result.stderrText);
    REQUIRE(result.exitCode == 2);
    RequireContainsText(result.stdoutText + "\n" + result.stderrText,
                        "bounded input");
    REQUIRE_FALSE(std::filesystem::exists(planPath));
    RemoveSandboxWorkspace(ctx.sandbox);
}

#if !defined(_WIN32)
TEST_CASE("plan new rejects correlation input changed during bounded read",
          "[functional][plan][correlation][input-boundary][toctou][KG-TSK-0125]") {
    const auto ctx = CreateRemoteWithClone("plan-correlation-toctou");
    const auto envelopePath = ctx.cloneRepo / ".kano/tmp/correlation-racing.json";
    const auto planPath = ctx.cloneRepo / ".kano/cache/git/plans/racing-correlation.json";
    const std::string envelope =
        R"({"mode":"koa","product_id":"product","topic_id":"topic","item_id":"item","work_order_id":"work","request_id":"request","run_id":"run","parent_run_id":"parent","producer_id":"producer","route_id":"commit.plan","attempt":1})";
    WriteTextFile(envelopePath, envelope);

    std::atomic<bool> stopMutating{false};
    std::thread mutator([&]() {
        std::size_t generation = 0;
        while (!stopMutating.load(std::memory_order_relaxed)) {
            std::ofstream output(envelopePath, std::ios::binary | std::ios::trunc);
            if (output.good()) {
                output << envelope << std::string((generation % 2U) + 1U, ' ');
                output.flush();
            }
            ++generation;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    });
    const auto result = RunKogWithEnv({
        "plan", "new", "--force", "--output", planPath.string(),
        "--correlation-file", envelopePath.string()
    }, ctx.cloneRepo,
       {{"KOG_TEST_MODE", "1"},
        {"KOG_TEST_ONLY_AUDIT_INPUT_POST_STAT_DELAY_MS", "500"}});
    stopMutating.store(true, std::memory_order_relaxed);
    mutator.join();

    INFO(result.stdoutText);
    INFO(result.stderrText);
    REQUIRE(result.exitCode == 2);
    RequireContainsText(result.stdoutText + "\n" + result.stderrText,
                        "bounded input");
    REQUIRE_FALSE(std::filesystem::exists(planPath));
    RemoveSandboxWorkspace(ctx.sandbox);
}
#endif

TEST_CASE("direct plan commit is auditable and native verify rejects same-path replacement",
          "[functional][commit][plan-file][audit][verify][KG-TSK-0125]") {
    const auto ctx = CreateRemoteWithClone("direct-commit-native-audit-verify");
    WriteTextFile(ctx.cloneRepo / "README.md", "seed\ndirect audited commit\n");
    const auto planPath =
        ctx.cloneRepo / ".kano/cache/git/plans/direct-audit-verify.json";
    RequireSuccess(
        RunKog({"plan", "new", "--force", "--output", planPath.string()},
               ctx.cloneRepo),
        "plan new for direct audited commit");
    RequireSuccess(RunKog({
        "plan", "prepare", "add-commit-entry", "--plan-file", planPath.string(),
        "--repo", ".", "--commit-message",
        "test(functional): direct audited commit",
        "--commit-include", "README.md", "--commit-review-verdict", "pass",
        "--commit-review-reason", "native verification binding regression"
    }, ctx.cloneRepo), "prepare direct audited commit");
    auto plan = nlohmann::json::parse(ReadTextFile(planPath));
    plan["meta"]["correlation"] = {
        {"mode", "koa"}, {"product_id", "product-demo"},
        {"topic_id", "topic-demo"}, {"item_id", "item-demo"},
        {"work_order_id", "work-order-demo"},
        {"request_id", "request-demo"},
        {"run_id", "run-native-verify"},
        {"parent_run_id", "run-parent"}, {"producer_id", "koa"},
        {"route_id", "commit.plan"}, {"attempt", 1},
    };
    WriteTextFile(planPath, plan.dump(2) + "\n");
    const auto admittedBytes = ReadTextFile(planPath);

    const auto commit = RunKog(
        {"commit", "--plan-file", planPath.string(), "--plan-stage", "commit"},
        ctx.cloneRepo);
    INFO(commit.stdoutText);
    INFO(commit.stderrText);
    REQUIRE(commit.exitCode == 0);
    REQUIRE(ReadTextFile(planPath) == admittedBytes);
    RequireAuditAttempt(
        planPath, 1, kano::git::audit::OutcomeState::Succeeded,
        {"audit.reserve", "plan.safety", "commit.apply"});

    const auto capability = RunKogWithEnv(
        {"audit", "capability", "--json"}, ctx.cloneRepo,
        {{"KANO_AGENT_MODE", "1"}});
    INFO(capability.stdoutText);
    INFO(capability.stderrText);
    REQUIRE(capability.exitCode == 0);
    REQUIRE(nlohmann::json::parse(capability.stdoutText)
                .at("schemaName") == "kog.auditCapability");
    RequireNotContainsText(capability.stdoutText, "[run]");
    RequireNotContainsText(capability.stdoutText,
                           ctx.cloneRepo.generic_string());

    const auto verify = RunKogWithEnv({
        "audit", "verify", "--plan-file", planPath.string(),
        "--run-id", "run-native-verify", "--attempt", "1", "--json"
    }, ctx.cloneRepo, {{"KANO_AGENT_MODE", "1"}});
    INFO(verify.stdoutText);
    INFO(verify.stderrText);
    REQUIRE(verify.exitCode == 0);
    RequireNotContainsText(verify.stdoutText, "[run]");
    RequireNotContainsText(verify.stdoutText,
                           ctx.cloneRepo.generic_string());
    const auto verified = nlohmann::json::parse(verify.stdoutText);
    REQUIRE(verified.size() == 21);
    REQUIRE(verified.at("ok") == true);
    REQUIRE(verified.at("traceValid") == true);
    REQUIRE(verified.at("receiptSha256").get<std::string>().size() == 64);

    auto replaced = nlohmann::json::parse(admittedBytes);
    replaced["unbound"] = true;
    WriteTextFile(planPath, replaced.dump(2) + "\n");
    const auto contentRejected = RunKog({
        "audit", "verify", "--plan-file", planPath.string(),
        "--run-id", "run-native-verify", "--attempt", "1", "--json"
    }, ctx.cloneRepo);
    REQUIRE(contentRejected.exitCode != 0);
    REQUIRE(nlohmann::json::parse(contentRejected.stdoutText).at("ok") == false);

    replaced = nlohmann::json::parse(admittedBytes);
    replaced["meta"]["plan_id"] = "different-plan";
    WriteTextFile(planPath, replaced.dump(2) + "\n");
    const auto identityRejected = RunKog({
        "audit", "verify", "--plan-file", planPath.string(),
        "--run-id", "run-native-verify", "--attempt", "1", "--json"
    }, ctx.cloneRepo);
    REQUIRE(identityRejected.exitCode != 0);
    REQUIRE(nlohmann::json::parse(identityRejected.stdoutText).at("ok") == false);

    RemoveSandboxWorkspace(ctx.sandbox);
}

TEST_CASE("commit_push_plan_file_preserves_unrelated_staged_paths", "[functional][commit-push][plan-file][pathspec][index]") {
    const auto ctx = CreateRemoteWithClone("plan-file-preserve-staged");
    WriteTextFile(ctx.cloneRepo / "included.txt", "include me\n");
    WriteTextFile(ctx.cloneRepo / "unrelated.txt", "keep staged\n");
    RequireSuccess(RunGit({"add", "unrelated.txt"}, ctx.cloneRepo), "stage unrelated path");

    const auto planPath = (ctx.cloneRepo / ".kano" / "cache" / "git" / "plans" / "preserve-staged.json").lexically_normal();
    RequireSuccess(RunKog({"plan", "new", "--force", "--output", planPath.string()}, ctx.cloneRepo), "plan new");
    RequireSuccess(
        RunKog({
            "plan", "prepare", "add-commit-entry",
            "--plan-file", planPath.string(),
            "--repo", ".",
            "--commit-message", "test(functional): exact include preserves index",
            "--commit-include", "included.txt",
            "--commit-review-verdict", "pass",
            "--commit-review-reason", "functional regression for preserving unrelated staged paths"
        }, ctx.cloneRepo),
        "plan add commit entry");

    const auto result = RunKog({"commit-push", "--plan-file", planPath.string()}, ctx.cloneRepo);
    INFO(result.stdoutText);
    INFO(result.stderrText);
    REQUIRE(result.exitCode != 0);
    RequireContainsText(result.stdoutText + "\n" + result.stderrText,
                        "plan commit blocked by pre-existing staged path outside plan include/exclude scope: unrelated.txt");

    const auto cached = RunGit({"diff", "--cached", "--name-only"}, ctx.cloneRepo);
    RequireSuccess(cached, "cached diff after blocked plan commit-push");
    REQUIRE(TrimCopy(cached.stdoutText) == "unrelated.txt");

    const auto includedStatus = RunGit({"status", "--short", "--", "included.txt"}, ctx.cloneRepo);
    RequireSuccess(includedStatus, "included status after blocked plan commit-push");
    REQUIRE(TrimCopy(includedStatus.stdoutText) == "?? included.txt");

    RemoveSandboxWorkspace(ctx.sandbox);
}

TEST_CASE("commit_push_plan_file_ignores_out_of_scope_post_sync_gitlinks", "[functional][commit-push][plan-file][post-sync][pathspec]") {
    const auto ctx = CreateRemoteWithSubmoduleClone("plan-file-post-sync-scope");
    const auto originalGitlinkHead = GitlinkHeadSha(ctx.cloneRootRepo, ctx.submodulePath);

    WriteTextFile(ctx.cloneChildRepo / "child.txt", "child local out-of-scope update\n");
    RequireSuccess(RunGit({"add", "child.txt"}, ctx.cloneChildRepo), "child add out-of-scope update");
    RequireSuccess(RunGit({"commit", "-m", "child out-of-scope update"}, ctx.cloneChildRepo), "child commit out-of-scope update");
    REQUIRE(CurrentHeadSha(ctx.cloneChildRepo) != originalGitlinkHead);

    WriteTextFile(ctx.cloneRootRepo / "README.md", "root seed\nroot scoped update\n");
    const auto planPath = (ctx.cloneRootRepo / ".kano" / "cache" / "git" / "plans" / "post-sync-scope.json").lexically_normal();
    RequireSuccess(RunKog({"plan", "new", "--force", "--output", planPath.string()}, ctx.cloneRootRepo), "plan new");
    const auto freshPlanText = ReadTextFile(planPath);
    const auto baseHeadSha = ExtractJsonStringField(freshPlanText, "base_head_sha");
    const auto dirtyFingerprint = ExtractJsonStringField(freshPlanText, "dirty_fingerprint");
    auto scopedPlanText = std::string(R"json({
  "meta": {
    "schema_version": "2",
    "plan_id": "functional-post-sync-scope",
    "generated_at_utc": "2026-07-08T00:00:00Z",
    "executed_at_utc": "",
    "base_head_sha": "KOG_BASE_HEAD_SHA",
    "dirty_fingerprint": "KOG_DIRTY_FINGERPRINT",
    "planner": { "provider": "human", "ai-model": "deterministic" },
    "review": { "verdict": "pass", "reason": "functional regression for scoped post-sync gitlinks" },
    "correlation": {"mode":"standalone","product_id":null,"topic_id":null,"item_id":null,"work_order_id":null,"request_id":null,"run_id":null,"parent_run_id":null,"producer_id":null,"route_id":null,"attempt":1}
  },
  "stages": {
    "commit": [
      {
        "repo": ".",
        "commits": [
          {
            "message": "test(functional): root scoped update",
            "include": ["README.md"],
            "exclude": [],
            "review": { "verdict": "pass", "reason": "commit only the scoped root update" }
          }
        ]
      }
    ],
    "post_sync": [
      {
        "repo": ".",
        "commits": [
          {
            "message": "test(functional): scoped post-sync update",
            "include": ["README.md"],
            "exclude": [],
            "review": { "verdict": "pass", "reason": "post-sync scope excludes dirty submodule gitlinks" }
          }
        ]
      }
    ]
  }
}
)json");
    const auto baseMarker = std::string("KOG_BASE_HEAD_SHA");
    const auto dirtyMarker = std::string("KOG_DIRTY_FINGERPRINT");
    const auto baseMarkerPos = scopedPlanText.find(baseMarker);
    REQUIRE(baseMarkerPos != std::string::npos);
    scopedPlanText.replace(baseMarkerPos, baseMarker.size(), baseHeadSha);
    const auto dirtyMarkerPos = scopedPlanText.find(dirtyMarker);
    REQUIRE(dirtyMarkerPos != std::string::npos);
    scopedPlanText.replace(dirtyMarkerPos, dirtyMarker.size(), dirtyFingerprint);
    WriteTextFile(planPath, scopedPlanText);

    const auto result = RunKog({"commit-push", "--plan-file", planPath.string()}, ctx.cloneRootRepo);
    INFO(result.stdoutText);
    INFO(result.stderrText);
    REQUIRE(result.exitCode == 0);
    RequireContainsText(result.stdoutText, "post-sync plan commit skipped (no working tree changes).");
    RequireNotContainsText(result.stdoutText, "post-sync gitlink-only auto-amend applied");
    REQUIRE(GitlinkHeadSha(ctx.cloneRootRepo, ctx.submodulePath) == originalGitlinkHead);
    REQUIRE(StatusPorcelain(ctx.cloneRootRepo).find(ctx.submodulePath) != std::string::npos);
    const auto [behind, ahead] = AheadBehindCounts(ctx.cloneRootRepo);
    REQUIRE(behind == 0);
    REQUIRE(ahead == 0);
    RemoveSandboxWorkspace(ctx.sandbox);
}

TEST_CASE("commit_push_plan_file_auto_amends_in_scope_post_sync_gitlinks",
          "[functional][commit-push][plan-file][post-sync][pathspec]") {
    const auto ctx = CreateRemoteWithSubmoduleClone("plan-file-post-sync-auto-amend");

    WriteTextFile(ctx.cloneChildRepo / "child.txt", "child published in-scope update\n");
    RequireSuccess(RunGit({"add", "child.txt"}, ctx.cloneChildRepo), "child add in-scope update");
    RequireSuccess(RunGit({"commit", "-m", "child in-scope update"}, ctx.cloneChildRepo), "child commit in-scope update");
    RequireSuccess(
        RunGit({"push", "origin", "HEAD:" + ctx.branch}, ctx.cloneChildRepo),
        "publish child in-scope update");
    const auto expectedChildHead = CurrentHeadSha(ctx.cloneChildRepo);

    WriteTextFile(ctx.cloneRootRepo / "README.md", "root seed\nroot scoped update\n");
    const auto planPath =
        (ctx.cloneRootRepo / ".kano" / "cache" / "git" / "plans" / "post-sync-auto-amend.json").lexically_normal();
    RequireSuccess(RunKog({"plan", "new", "--force", "--output", planPath.string()}, ctx.cloneRootRepo), "plan new");
    const auto freshPlanText = ReadTextFile(planPath);
    const auto baseHeadSha = ExtractJsonStringField(freshPlanText, "base_head_sha");
    const auto dirtyFingerprint = ExtractJsonStringField(freshPlanText, "dirty_fingerprint");
    auto scopedPlanText = std::string(R"json({
  "meta": {
    "schema_version": "2",
    "plan_id": "functional-post-sync-auto-amend",
    "generated_at_utc": "2026-07-26T00:00:00Z",
    "executed_at_utc": "",
    "base_head_sha": "KOG_BASE_HEAD_SHA",
    "dirty_fingerprint": "KOG_DIRTY_FINGERPRINT",
    "planner": { "provider": "human", "ai-model": "deterministic" },
    "review": { "verdict": "pass", "reason": "functional regression for in-scope post-sync gitlinks" },
    "correlation": {"mode":"standalone","product_id":null,"topic_id":null,"item_id":null,"work_order_id":null,"request_id":null,"run_id":null,"parent_run_id":null,"producer_id":null,"route_id":null,"attempt":1}
  },
  "stages": {
    "commit": [
      {
        "repo": ".",
        "commits": [
          {
            "message": "test(functional): root scoped auto-amend update",
            "include": ["README.md"],
            "exclude": [],
            "review": { "verdict": "pass", "reason": "commit only the scoped root update" }
          }
        ]
      }
    ],
    "post_sync": [
      {
        "repo": ".",
        "commits": [
          {
            "message": "test(functional): in-scope post-sync gitlink",
            "include": ["deps/child"],
            "exclude": [],
            "review": { "verdict": "pass", "reason": "auto-amend the published submodule pointer" }
          }
        ]
      }
    ]
  }
}
)json");
    const auto baseMarker = std::string("KOG_BASE_HEAD_SHA");
    const auto dirtyMarker = std::string("KOG_DIRTY_FINGERPRINT");
    const auto baseMarkerPos = scopedPlanText.find(baseMarker);
    REQUIRE(baseMarkerPos != std::string::npos);
    scopedPlanText.replace(baseMarkerPos, baseMarker.size(), baseHeadSha);
    const auto dirtyMarkerPos = scopedPlanText.find(dirtyMarker);
    REQUIRE(dirtyMarkerPos != std::string::npos);
    scopedPlanText.replace(dirtyMarkerPos, dirtyMarker.size(), dirtyFingerprint);
    WriteTextFile(planPath, scopedPlanText);

    const auto result = RunKog({"commit-push", "--plan-file", planPath.string()}, ctx.cloneRootRepo);
    INFO(result.stdoutText);
    INFO(result.stderrText);
    REQUIRE(result.exitCode == 0);
    RequireContainsText(result.stdoutText, "post-sync gitlink-only auto-amend applied: repos=1");
    RequireNotContainsText(result.stdoutText, "post-sync semantic changes detected");
    REQUIRE(GitlinkHeadSha(ctx.cloneRootRepo, ctx.submodulePath) == expectedChildHead);
    REQUIRE(TrimCopy(StatusPorcelain(ctx.cloneRootRepo)).empty());
    const auto [behind, ahead] = AheadBehindCounts(ctx.cloneRootRepo);
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
    env.emplace_back("PATH", PrependPathEntry(stubDir));

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
    env.emplace_back("PATH", PrependPathEntry(stubDir));

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

TEST_CASE("sync_mixed_semantic_and_gitlink_drift_never_auto_amends", "[functional][commit-push][post-sync]") {
    const auto ctx = CreateRemoteWithSubmoduleClone("sync-mixed-semantic-gitlink");
    const auto originalGitlinkHead = GitlinkHeadSha(ctx.cloneRootRepo, ctx.submodulePath);

    WriteTextFile(ctx.cloneChildRepo / "child.txt", "child published mixed-drift update\n");
    RequireSuccess(RunGit({"add", "child.txt"}, ctx.cloneChildRepo), "child add mixed-drift update");
    RequireSuccess(RunGit({"commit", "-m", "child mixed-drift update"}, ctx.cloneChildRepo), "child commit mixed-drift update");
    RequireSuccess(
        RunGit({"push", "origin", "HEAD:" + ctx.branch}, ctx.cloneChildRepo),
        "publish child mixed-drift update");
    REQUIRE(CurrentHeadSha(ctx.cloneChildRepo) != originalGitlinkHead);

    WriteTextFile(ctx.cloneRootRepo / "staged.txt", "staged\n");
    WriteTextFile(ctx.cloneRootRepo / "leftover.txt", "leftover\n");
    RequireSuccess(RunGit({"add", "staged.txt"}, ctx.cloneRootRepo), "stage mixed-drift staged-only file");

    const auto result =
        RunKog({"commit-push", "-m", "test(functional): mixed staged only", "--staged-only"}, ctx.cloneRootRepo);
    INFO(result.stdoutText);
    INFO(result.stderrText);
    REQUIRE(result.exitCode != 0);
    const auto merged = result.stdoutText + "\n" + result.stderrText;
    RequireContainsText(merged, "post-sync semantic changes detected; proceeding to post-sync commit stage");
    RequireContainsText(merged, "Preflight blocked: --staged-only but nothing staged");
    RequireNotContainsText(merged, "post-sync gitlink-only auto-amend applied");
    REQUIRE(GitlinkHeadSha(ctx.cloneRootRepo, ctx.submodulePath) == originalGitlinkHead);
    const auto status = StatusPorcelain(ctx.cloneRootRepo);
    RequireContainsText(status, ctx.submodulePath);
    RequireContainsText(status, "leftover.txt");
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

TEST_CASE("agent mode CPA fails closed on a missing shared plan without mutation",
          "[functional][commit-push][agent-mode][cpa][preparation][KG-TSK-0125]") {
    const auto ctx = CreateRemoteWithClone("agent-mode-cpa-missing-plan");
    WriteTextFile(ctx.cloneRepo / "README.md", "seed\nagent mode missing shared plan\n");
    const auto headBefore = CurrentHeadSha(ctx.cloneRepo);
    const auto planPath = (ctx.cloneRepo / ".kano" / "tmp" / "git" / "plans" / "default-plan.json").lexically_normal();
    const auto providerLogPath = (ctx.sandbox.root / "provider-invocations.log").lexically_normal();
    const auto stubDir = InstallCopilotStub(ctx.sandbox.root);

    const auto result = RunKogWithEnv(
        {"cpa"},
        ctx.cloneRepo,
        {
            {"KANO_AGENT_MODE", "1"},
            {"KOG_TEST_MODE", "1"},
            {"KOG_TEST_AVAILABLE_COMMANDS", "copilot"},
            {"KOG_TEST_COPILOT_COMMAND", (stubDir / "copilot").string()},
            {"KOG_TEST_AI_STUB_LOG", providerLogPath.string()},
            {"PATH", PrependPathEntry(stubDir)},
        });
    INFO(result.stdoutText);
    INFO(result.stderrText);
    REQUIRE(result.exitCode == 3);

    const auto merged = result.stdoutText + "\n" + result.stderrText;
    RequireContainsText(merged, "agent mode + --plan-file detected; using plan-driven flow");
    RequireContainsText(merged, "refresh-needed: missing-or-unreadable");
    RequireContainsText(merged, "[AGENT_PLAN_PREPARATION_REQUIRED]");
    RequireNotContainsText(merged, "[commit-push][auto-plan] stage=commit-runbook");
    REQUIRE_FALSE(std::filesystem::exists(providerLogPath));
    REQUIRE_FALSE(std::filesystem::exists(planPath));
    REQUIRE_FALSE(std::filesystem::exists(
        ctx.cloneRepo / ".kano/tmp/git/plan-execution-locks"));
    REQUIRE(CurrentHeadSha(ctx.cloneRepo) == headBefore);

    RemoveSandboxWorkspace(ctx.sandbox);
}

TEST_CASE("agent mode CPA rejects a stale shared plan without rewriting it",
          "[functional][commit-push][agent-mode][cpa][preparation][stale][KG-TSK-0125]") {
    const auto ctx = CreateRemoteWithClone("agent-mode-cpa-stale-plan");
    WriteTextFile(ctx.cloneRepo / "README.md", "seed\nagent mode initial plan\n");
    const auto headBefore = CurrentHeadSha(ctx.cloneRepo);
    const auto planPath = (ctx.cloneRepo / ".kano" / "tmp" / "git" / "plans" / "default-plan.json").lexically_normal();
    RequireSuccess(RunKog({"plan", "new", "--force", "--output", planPath.string()}, ctx.cloneRepo), "create shared plan");
    RequireSuccess(
        RunKog({
            "plan", "prepare", "add-commit-entry",
            "--plan-file", planPath.string(),
            "--repo", ".",
            "--commit-message", "test(functional): ready plan before drift",
            "--commit-include", "README.md",
            "--commit-review-verdict", "pass",
            "--commit-review-reason", "ready shared plan before intentional workspace drift",
        }, ctx.cloneRepo),
        "fill shared plan before drift");
    const auto readyCheck = RunKog({"plan", "refresh-check", "--plan-file", planPath.string()}, ctx.cloneRepo);
    INFO(readyCheck.stdoutText);
    INFO(readyCheck.stderrText);
    REQUIRE(readyCheck.exitCode == 1);
    const auto staleFingerprint = ExtractJsonStringField(ReadTextFile(planPath), "dirty_fingerprint");

    WriteTextFile(ctx.cloneRepo / "extra.txt", "intentional workspace drift\n");
    const auto stalePlanText = ReadTextFile(planPath);
    const auto providerLogPath = (ctx.sandbox.root / "provider-invocations.log").lexically_normal();
    const auto stubDir = InstallCopilotStub(ctx.sandbox.root);
    const auto result = RunKogWithEnv(
        {"cpa"},
        ctx.cloneRepo,
        {
            {"KANO_AGENT_MODE", "1"},
            {"KOG_TEST_MODE", "1"},
            {"KOG_TEST_AVAILABLE_COMMANDS", "copilot"},
            {"KOG_TEST_COPILOT_COMMAND", (stubDir / "copilot").string()},
            {"KOG_TEST_AI_STUB_LOG", providerLogPath.string()},
            {"PATH", PrependPathEntry(stubDir)},
        });
    INFO(result.stdoutText);
    INFO(result.stderrText);
    REQUIRE(result.exitCode == 3);

    const auto merged = result.stdoutText + "\n" + result.stderrText;
    RequireContainsText(merged, "workspace-state-changed");
    RequireContainsText(merged, "[AGENT_PLAN_PREPARATION_REQUIRED]");
    RequireNotContainsText(merged, "[commit-push][auto-plan] stage=commit-runbook");
    REQUIRE_FALSE(std::filesystem::exists(providerLogPath));
    REQUIRE(ReadTextFile(planPath) == stalePlanText);
    REQUIRE(ExtractJsonStringField(ReadTextFile(planPath), "dirty_fingerprint") ==
            staleFingerprint);
    REQUIRE_FALSE(std::filesystem::exists(
        ctx.cloneRepo / ".kano/tmp/git/plan-execution-locks"));
    REQUIRE(CurrentHeadSha(ctx.cloneRepo) == headBefore);

    RemoveSandboxWorkspace(ctx.sandbox);
}

TEST_CASE("agent_mode_cpa_with_message_does_not_inject_shared_plan",
          "[functional][commit-push][agent-mode][cpa][alias]") {
    const auto ctx = CreateRemoteWithClone("agent-mode-cpa-message");
    WriteTextFile(ctx.cloneRepo / "README.md", "seed\nagent cpa message dry-run\n");
    const auto headBefore = CurrentHeadSha(ctx.cloneRepo);

    const auto result = RunKogWithEnv(
        {"cpa", "-m", "test(functional): agent cpa dry-run", "--dry-run"},
        ctx.cloneRepo,
        {{"KANO_AGENT_MODE", "1"}});
    INFO(result.stdoutText);
    INFO(result.stderrText);
    REQUIRE(result.exitCode == 0);
    const auto merged = result.stdoutText + "\n" + result.stderrText;
    RequireNotContainsText(merged, "--plan-file cannot be combined");
    RequireNotContainsText(merged, "default-plan.json");
    REQUIRE(CurrentHeadSha(ctx.cloneRepo) == headBefore);

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
    env.emplace_back("PATH", PrependPathEntry(stubDir));
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
        {"PATH", PrependPathEntry(stubDir)},
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

TEST_CASE("plan_runbook_commit_per_commit_accepts_direct_plan_writeback",
          "[.][functional][plan][ai][per-commit]") {
    const auto ctx = CreateRemoteWithClone("plan-runbook-per-commit-direct-writeback");
    WriteTextFile(ctx.cloneRepo / "README.md", "seed\nper-commit direct writeback\n");
    const auto planPath = (ctx.cloneRepo / ".kano" / "tmp" / "git" / "plans" / "per-commit-plan.json").lexically_normal();
    const auto providerLogPath = (ctx.sandbox.root / "provider-invocations.log").lexically_normal();

    RequireSuccess(RunKog({"plan", "new", "--force", "--output", planPath.string()}, ctx.cloneRepo), "plan new");
    RequireSuccess(
        RunKog({"plan", "commit-seed", "--force", "--plan-file", planPath.string()}, ctx.cloneRepo),
        "plan commit-seed");
    const auto seededPlanText = ReadTextFile(planPath);
    RequireSuccess(
        RunKog({
            "plan", "fill-commit", "0",
            "--plan-file", planPath.string(),
            "--commit-message", "docs(readme): clarify per-commit direct writeback",
            "--review-verdict", "pass",
            "--review-reason", "The README change is self-contained and the direct working-plan writeback is semantically complete.",
        }, ctx.cloneRepo),
        "prepare simulated AI plan writeback");
    const auto aiPayload = ReadTextFile(planPath);
    WriteTextFile(planPath, seededPlanText);

    std::vector<std::pair<std::string, std::string>> env{
        {"KOG_TEST_AI_STDOUT", aiPayload},
        {"KOG_TEST_AI_EXIT_CODE", "0"},
        {"KOG_TEST_AI_STUB_LOG", providerLogPath.string()},
    };
    const auto stubDir = InstallCopilotStub(ctx.sandbox.root);
    env.emplace_back("PATH", PrependPathEntry(stubDir));
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
    REQUIRE(planText.find("The README change is self-contained and the direct working-plan writeback is semantically complete.") != std::string::npos);
    REQUIRE(planText.find("\"verdict\":\"pass\"") != std::string::npos);
    const auto mergedOutput = result.stdoutText + "\n" + result.stderrText;
    const bool hasPerCommitFillSignal =
        mergedOutput.find("filled entry 0 message: docs(readme): clarify per-commit direct writeback") != std::string::npos ||
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

    RequireSuccess(
        RunGit({"checkout", "-B", ctx.initialChildBranch, ("origin/" + ctx.initialChildBranch)}, ctx.cloneChildRepo),
        "attach child initial branch before dirty auto-stash regression");
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

    RequireSuccess(
        RunGit({"checkout", "-B", ctx.initialChildBranch, ("origin/" + ctx.initialChildBranch)}, ctx.cloneChildRepo),
        "attach child initial branch before missing target regression");
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

TEST_CASE("submodule_named_remotes_are_versioned_and_rehydrated", "[functional][submodule][remote-set][KG-FTR-0006]") {
    const auto ctx = CreateRemoteWithSubmoduleClone("submodule-versioned-named-remotes");
    const auto upstream = CreateStandaloneBareRemote(ctx.sandbox, "upstream-child", ctx.branch, true);
    const auto upstreamUrl = upstream.bareRemote.string();

    const auto setResult = RunKog(
        {"submodule", "remote-set", ctx.submodulePath, "upstream", upstreamUrl},
        ctx.cloneRootRepo);
    RequireSuccess(setResult, "persist named submodule remote");

    const auto declared = RunGit(
        {"config", "-f", ".gitmodules", "--get", "submodule." + ctx.submodulePath + ".kog-remote-upstream"},
        ctx.cloneRootRepo);
    RequireSuccess(declared, "read version-controlled named remote");
    REQUIRE(TrimCopy(declared.stdoutText) == upstreamUrl);
    REQUIRE(TrimCopy(RunGit({"remote", "get-url", "upstream"}, ctx.cloneChildRepo).stdoutText) == upstreamUrl);

    RequireSuccess(RunGit({"remote", "remove", "upstream"}, ctx.cloneChildRepo), "remove local named remote before rehydration");
    const auto syncResult = RunKog({"submodule", "sync-urls", "--no-origin"}, ctx.cloneRootRepo);
    RequireSuccess(syncResult, "rehydrate named remote from gitmodules");
    REQUIRE(syncResult.stdoutText.find("add upstream") != std::string::npos);
    REQUIRE(TrimCopy(RunGit({"remote", "get-url", "upstream"}, ctx.cloneChildRepo).stdoutText) == upstreamUrl);

    const auto invalidName = RunKog(
        {"submodule", "remote-set", ctx.submodulePath, "../unsafe", upstreamUrl},
        ctx.cloneRootRepo);
    REQUIRE(invalidName.exitCode != 0);

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

TEST_CASE("submodule_update_remote_accepts_child_head_ahead_of_parent_gitlink", "[functional][submodule][update][remote][KG-BUG-0086]") {
    const auto ctx = CreateRemoteWithSubmoduleClone("submodule-update-remote-ahead");

    WriteTextFile(ctx.childSeedRepo / "remote-update.txt", "remote update\n");
    RequireSuccess(RunGit({"add", "remote-update.txt"}, ctx.childSeedRepo), "stage child remote update");
    RequireSuccess(RunGit({"commit", "-m", "advance child remote"}, ctx.childSeedRepo), "commit child remote update");
    RequireSuccess(RunGit({"push", "origin", ctx.branch}, ctx.childSeedRepo), "push child remote update");
    const auto remoteHead = CurrentHeadSha(ctx.childSeedRepo);
    const auto parentGitlink = GitlinkHeadSha(ctx.cloneRootRepo, ctx.submodulePath);
    REQUIRE(remoteHead != parentGitlink);

    const auto result = RunKogAllowingFileProtocol(
        {"submodule", "update", "--remote", ctx.submodulePath},
        ctx.cloneRootRepo);
    INFO(result.stdoutText);
    INFO(result.stderrText);
    REQUIRE(result.exitCode == 0);
    REQUIRE(CurrentHeadSha(ctx.cloneChildRepo) == remoteHead);
    REQUIRE(GitlinkHeadSha(ctx.cloneRootRepo, ctx.submodulePath) == parentGitlink);

    const auto merged = result.stdoutText + "\n" + result.stderrText;
    REQUIRE(merged.find("Updated cleanly: 1") != std::string::npos);
    REQUIRE(merged.find("SUBMODULE_HEAD_MISMATCH") == std::string::npos);

    RemoveSandboxWorkspace(ctx.sandbox);
}

TEST_CASE("submodule_update_repairs_invalid_gitdir_state_when_safe", "[functional][submodule][update][repair]") {
    const auto ctx = CreateRemoteWithSubmoduleClone("submodule-update-repair-safe");

    const auto submodulePath = (ctx.cloneRootRepo / std::filesystem::path(ctx.submodulePath)).lexically_normal();
    const auto modulePathResult = RunGit({"-C", ctx.cloneRootRepo.string(), "rev-parse", "--git-path", "modules/" + ctx.submodulePath}, ctx.cloneRootRepo);
    RequireSuccess(modulePathResult, "resolve module path");
    const auto modulePath = ResolveFixtureGitPath(ctx.cloneRootRepo, TrimCopy(modulePathResult.stdoutText));

    std::error_code ec;
    std::filesystem::remove_all(modulePath, ec);
    REQUIRE(!ec);
    std::filesystem::remove_all(submodulePath, ec);
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

TEST_CASE("submodule_update_repairs_empty_malformed_gitdir_state_when_safe", "[functional][submodule][update][repair][KG-BUG-0067]") {
    const auto ctx = CreateRemoteWithSubmoduleClone("submodule-update-repair-malformed-gitdir");

    const auto submodulePath = (ctx.cloneRootRepo / std::filesystem::path(ctx.submodulePath)).lexically_normal();
    const auto modulePathResult = RunGit({"-C", ctx.cloneRootRepo.string(), "rev-parse", "--git-path", "modules/" + ctx.submodulePath}, ctx.cloneRootRepo);
    RequireSuccess(modulePathResult, "resolve malformed module path");
    const auto modulePath = ResolveFixtureGitPath(ctx.cloneRootRepo, TrimCopy(modulePathResult.stdoutText));

    std::error_code ec;
    std::filesystem::remove_all(modulePath, ec);
    REQUIRE(!ec);
    std::filesystem::create_directories(modulePath / "objects");
    WriteTextFile(modulePath / "objects" / "stale", "stale\n");
    std::filesystem::remove_all(submodulePath, ec);
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
    REQUIRE(CurrentHeadSha(submodulePath) == GitlinkHeadSha(ctx.cloneRootRepo, ctx.submodulePath));

    RemoveSandboxWorkspace(ctx.sandbox);
}
TEST_CASE("submodule_update_blocks_unsafe_repair_for_local_user_files", "[functional][submodule][update][repair][unsafe]") {
    const auto ctx = CreateRemoteWithSubmoduleClone("submodule-update-repair-unsafe");

    const auto submodulePath = (ctx.cloneRootRepo / std::filesystem::path(ctx.submodulePath)).lexically_normal();
    const auto modulePathResult = RunGit({"-C", ctx.cloneRootRepo.string(), "rev-parse", "--git-path", "modules/" + ctx.submodulePath}, ctx.cloneRootRepo);
    RequireSuccess(modulePathResult, "resolve module path");
    const auto modulePath = ResolveFixtureGitPath(ctx.cloneRootRepo, TrimCopy(modulePathResult.stdoutText));

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

TEST_CASE("plan_file_commit_push_converges_registered_submodule_child_before_parent",
          "[functional][commit-push][plan-file][multi-repo][waves]") {
    const auto ctx = CreateRemoteWithSubmoduleClone("plan-file-multi-repo-waves");
    RequireSuccess(
        RunGit({"checkout", "-B", ctx.branch, "origin/" + ctx.branch}, ctx.cloneChildRepo),
        "attach child branch for plan commit");

    WriteTextFile(ctx.cloneChildRepo / "child.txt", "child plan update\n");
    WriteTextFile(ctx.cloneRootRepo / "README.md", "root seed\nroot plan update\n");

    const auto planPath =
        (ctx.cloneRootRepo / ".kano" / "cache" / "git" / "plans" / "multi-repo-waves.json").lexically_normal();
    RequireSuccess(
        RunKog({"plan", "new", "--force", "--output", planPath.string()}, ctx.cloneRootRepo),
        "plan new for multi-repo waves");
    RequireSuccess(
        RunKog({
            "plan", "prepare", "add-commit-entry",
            "--plan-file", planPath.string(),
            "--repo", ctx.submodulePath,
            "--commit-message", "test(functional): child plan update",
            "--commit-include", "child.txt",
            "--commit-review-verdict", "pass",
            "--commit-review-reason", "commit child before parent pointer"
        }, ctx.cloneRootRepo),
        "plan add child commit entry");
    RequireSuccess(
        RunKog({
            "plan", "prepare", "add-commit-entry",
            "--plan-file", planPath.string(),
            "--repo", ".",
            "--commit-message", "test(functional): parent plan update",
            "--commit-include", "README.md",
            "--commit-include", ctx.submodulePath,
            "--commit-review-verdict", "pass",
            "--commit-review-reason", "commit parent semantic change and materialized gitlink"
        }, ctx.cloneRootRepo),
        "plan add parent commit entry");
    RequireSuccess(
        RunKog({"plan", "verify", "pre-apply", "--stage", "commit", "--plan-file", planPath.string()}, ctx.cloneRootRepo),
        "verify multi-repo wave plan");

    const auto result = RunKog({"commit-push", "--plan-file", planPath.string()}, ctx.cloneRootRepo);
    INFO(result.stdoutText);
    INFO(result.stderrText);
    REQUIRE(result.exitCode == 0);

    const auto mergedOutput = result.stdoutText + "\n" + result.stderrText;
    RequireContainsText(mergedOutput, "[native-commit] plan: repos=2");
    RequireContainsText(mergedOutput, "commit_waves=2 order=child-first");
    const auto childCommitPosition = mergedOutput.find("[commit] deps/child");
    const auto rootCommitMarker = "[commit] " + ctx.cloneRootRepo.filename().generic_string() + " (.)";
    const auto rootCommitPosition = mergedOutput.find(rootCommitMarker);
    REQUIRE(childCommitPosition != std::string::npos);
    REQUIRE(rootCommitPosition != std::string::npos);
    REQUIRE(childCommitPosition < rootCommitPosition);

    const auto childHead = CurrentHeadSha(ctx.cloneChildRepo);
    const auto rootHead = CurrentHeadSha(ctx.cloneRootRepo);
    REQUIRE(GitlinkHeadSha(ctx.cloneRootRepo, ctx.submodulePath) == childHead);
    REQUIRE(RefSha(ctx.childBareRemote, "refs/heads/" + ctx.branch) == childHead);
    REQUIRE(RefSha(ctx.rootBareRemote, "refs/heads/" + ctx.branch) == rootHead);
    const auto [childBehind, childAhead] = AheadBehindCounts(ctx.cloneChildRepo);
    REQUIRE(childBehind == 0);
    REQUIRE(childAhead == 0);
    const auto [rootBehind, rootAhead] = AheadBehindCounts(ctx.cloneRootRepo);
    REQUIRE(rootBehind == 0);
    REQUIRE(rootAhead == 0);

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

    const auto mergedOutput = result.stdoutText + "\n" + result.stderrText;
    RequireContainsText(mergedOutput, "[native-commit] plan: repos=2");
    RequireContainsText(mergedOutput, "commit_waves=2 order=child-first");
    const auto childCommitPosition = mergedOutput.find("[commit] deps/child");
    const auto rootCommitMarker = "[commit] " + ctx.cloneRootRepo.filename().generic_string() + " (.)";
    const auto rootCommitPosition = mergedOutput.find(rootCommitMarker);
    REQUIRE(childCommitPosition != std::string::npos);
    REQUIRE(rootCommitPosition != std::string::npos);
    REQUIRE(childCommitPosition < rootCommitPosition);

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
    const auto nestedBeforeHead = CurrentHeadSha(ctx.cloneNestedRepo);

    WriteTextFile(ctx.cloneNestedRepo / "nested.txt", "nested local update\n");
    WriteTextFile(ctx.cloneRootRepo / "README.md", "root seed\nroot local update\n");

    const auto result = RunKog({"commit-push", "-m", "test(functional): multi repo nested update"}, ctx.cloneRootRepo);
    INFO(result.stdoutText);
    INFO(result.stderrText);
    REQUIRE(result.exitCode == 0);

    const auto mergedOutput = result.stdoutText + "\n" + result.stderrText;
    RequireContainsText(mergedOutput, "[native-commit] plan: repos=2");
    RequireContainsText(mergedOutput, "commit_waves=2 order=child-first");
    const auto nestedCommitPosition = mergedOutput.find("[commit] nested/tool");
    const auto rootCommitMarker = "[commit] " + ctx.cloneRootRepo.filename().generic_string() + " (.)";
    const auto rootCommitPosition = mergedOutput.find(rootCommitMarker);
    REQUIRE(nestedCommitPosition != std::string::npos);
    REQUIRE(rootCommitPosition != std::string::npos);
    REQUIRE(nestedCommitPosition < rootCommitPosition);

    const auto nestedHead = CurrentHeadSha(ctx.cloneNestedRepo);
    const auto rootHead = CurrentHeadSha(ctx.cloneRootRepo);
    REQUIRE_FALSE(nestedHead.empty());
    REQUIRE_FALSE(rootHead.empty());
    REQUIRE(nestedHead != nestedBeforeHead);
    const auto nestedContent = RunGit({"show", "HEAD:nested.txt"}, ctx.cloneNestedRepo);
    RequireSuccess(nestedContent, "show committed nested content");
    REQUIRE(nestedContent.stdoutText == "nested local update\n");
    REQUIRE(GitlinkHeadSha(ctx.cloneRootRepo, ctx.nestedRepoPath) == nestedHead);

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

TEST_CASE("multi_repo_commit_push_preserves_push_only_registered_repo_policy", "[functional][commit-push][multi-repo][policy]") {
    const auto ctx = CreateRemoteWithSubmoduleClone("multi-repo-push-only-policy");

    RequireSuccess(
        RunGit({"config", "-f", ".gitmodules", ("submodule." + ctx.submodulePath + ".kog-commit-policy"), "false"},
               ctx.cloneRootRepo),
        "disable child commit policy");
    RequireSuccess(RunGit({"add", ".gitmodules"}, ctx.cloneRootRepo), "stage child commit policy");
    RequireSuccess(RunGit({"commit", "-m", "disable child commit policy"}, ctx.cloneRootRepo), "commit child commit policy");
    RequireSuccess(RunGit({"push"}, ctx.cloneRootRepo), "push child commit policy");

    WriteTextFile(ctx.cloneChildRepo / "child.txt", "child push-only update\n");
    RequireSuccess(RunGit({"add", "child.txt"}, ctx.cloneChildRepo), "stage push-only child update");
    RequireSuccess(RunGit({"commit", "-m", "child push-only update"}, ctx.cloneChildRepo), "commit push-only child update");
    const auto childHead = CurrentHeadSha(ctx.cloneChildRepo);
    REQUIRE(RefSha(ctx.childBareRemote, "refs/heads/" + ctx.branch) != childHead);

    const auto result = RunKog({"commit-push", "-m", "test(functional): preserve push-only child"}, ctx.cloneRootRepo);
    INFO(result.stdoutText);
    INFO(result.stderrText);
    REQUIRE(result.exitCode == 0);

    const auto [childBehind, childAhead] = AheadBehindCounts(ctx.cloneChildRepo);
    REQUIRE(childBehind == 0);
    REQUIRE(childAhead == 0);
    REQUIRE(RefSha(ctx.childBareRemote, "refs/heads/" + ctx.branch) == childHead);

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

TEST_CASE("discover_respects_gitignore_for_nested_repo", "[functional][discover][discovery]") {
    const auto ctx = CreateRemoteWithNestedRepoClone("discover-gitignore");
    WriteTextFile(ctx.cloneRootRepo / ".gitignore", ".kano/\nnested/\n");

    const auto result = RunKog(
        {"discover", "--format", "json", "--repo-root", ctx.cloneRootRepo.string(), "--no-cache"},
        ctx.cloneRootRepo);
    INFO(result.stdoutText);
    INFO(result.stderrText);
    REQUIRE(result.exitCode == 0);
    REQUIRE(result.stdoutText.starts_with("{"));
    REQUIRE(result.stdoutText.find("Discovery mode:") == std::string::npos);
    REQUIRE(ContainsPathEntry(result.stdoutText, ctx.cloneRootRepo));
    REQUIRE_FALSE(ContainsPathEntry(result.stdoutText, ctx.cloneNestedRepo));

    RemoveSandboxWorkspace(ctx.sandbox);
}

TEST_CASE("discover_respects_kogignore_for_nested_repo", "[functional][discover][discovery]") {
    const auto ctx = CreateRemoteWithNestedRepoClone("discover-kogignore");
    WriteTextFile(ctx.cloneRootRepo / ".kogignore", "nested/\n");

    const auto result = RunKog(
        {"discover", "--format", "json", "--repo-root", ctx.cloneRootRepo.string(), "--no-cache"},
        ctx.cloneRootRepo);
    INFO(result.stdoutText);
    INFO(result.stderrText);
    REQUIRE(result.exitCode == 0);
    REQUIRE(ContainsPathEntry(result.stdoutText, ctx.cloneRootRepo));
    REQUIRE_FALSE(ContainsPathEntry(result.stdoutText, ctx.cloneNestedRepo));

    RemoveSandboxWorkspace(ctx.sandbox);
}

TEST_CASE("discover_exclude_is_temporary_override", "[functional][discover][discovery]") {
    const auto ctx = CreateRemoteWithNestedRepoClone("discover-exclude");

    const auto baseline = RunKog(
        {"discover", "--full", "--format", "json", "--repo-root", ctx.cloneRootRepo.string(), "--no-cache"},
        ctx.cloneRootRepo);
    INFO(baseline.stdoutText);
    INFO(baseline.stderrText);
    REQUIRE(baseline.exitCode == 0);
    REQUIRE(ContainsPathEntry(baseline.stdoutText, ctx.cloneNestedRepo));

    const auto excluded = RunKog(
        {"discover", "--full", "--format", "json", "--repo-root", ctx.cloneRootRepo.string(), "--no-cache",
         "--exclude", "nested/"},
        ctx.cloneRootRepo);
    INFO(excluded.stdoutText);
    INFO(excluded.stderrText);
    REQUIRE(excluded.exitCode == 0);
    REQUIRE_FALSE(ContainsPathEntry(excluded.stdoutText, ctx.cloneNestedRepo));

    const auto after = RunKog(
        {"discover", "--full", "--format", "json", "--repo-root", ctx.cloneRootRepo.string(), "--no-cache"},
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

TEST_CASE("commit plan apply rejects dirty fingerprint drift before mutation", "[functional][commit][plan-file][freshness][KG-BUG-0038]") {
    const auto ctx = CreateRemoteWithClone("commit-plan-rejects-dirty-drift");
    WriteTextFile(ctx.cloneRepo / "included.txt", "planned content\n");
    const auto planPath = (ctx.cloneRepo / ".kano" / "cache" / "git" / "plans" / "stale-commit.json").lexically_normal();
    RequireSuccess(RunKog({"plan", "new", "--force", "--output", planPath.string()}, ctx.cloneRepo), "plan new");
    RequireSuccess(
        RunKog({
            "plan", "prepare", "add-commit-entry",
            "--plan-file", planPath.string(),
            "--repo", ".",
            "--commit-message", "[Commit][Test] Reject stale plan (KG-BUG-0038)",
            "--commit-include", "included.txt",
            "--commit-review-verdict", "pass",
            "--commit-review-reason", "functional stale plan rejection"
        }, ctx.cloneRepo),
        "add stale commit entry");
    const auto headBefore = TrimCopy(RunGit({"rev-parse", "HEAD"}, ctx.cloneRepo).stdoutText);

    WriteTextFile(ctx.cloneRepo / "concurrent.txt", "concurrent agent drift\n");
    const auto apply = RunKog({"commit", "--plan-file", planPath.string(), "--plan-stage", "commit"}, ctx.cloneRepo);
    INFO(apply.stdoutText);
    INFO(apply.stderrText);
    REQUIRE(apply.exitCode != 0);
    const auto merged = apply.stdoutText + "\n" + apply.stderrText;
    RequireContainsText(merged, "workspace state drift detected; commit apply refused");
    RequireContainsText(merged, "plan.dirty_fingerprint=");
    RequireContainsText(merged, "current.dirty_fingerprint=");
    RequireContainsText(merged, "regenerate/refill plan before commit apply");
    REQUIRE(TrimCopy(RunGit({"rev-parse", "HEAD"}, ctx.cloneRepo).stdoutText) == headBefore);
    REQUIRE(TrimCopy(RunGit({"diff", "--cached", "--name-only"}, ctx.cloneRepo).stdoutText).empty());
    const auto status = RunGit({"status", "--short"}, ctx.cloneRepo);
    RequireSuccess(status, "status after stale plan rejection");
    RequireContainsText(status.stdoutText, "?? concurrent.txt");
    RequireContainsText(status.stdoutText, "?? included.txt");

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

TEST_CASE("plan_ignore_init_coalesces_unreal_artifacts_in_root_and_submodule", "[functional][ignore-gate][KG-BUG-0016]") {
    const auto ctx = CreateRemoteWithSubmoduleClone("ignore-unreal-artifacts");
    const auto planPath = (ctx.cloneRootRepo / ".kano" / "tmp" / "git" / "plans" / "kg-bug-0016-plan.json").lexically_normal();

    WriteTextFile(ctx.cloneRootRepo / "KTOGame.slnx", "solution\n");
    WriteTextFile(ctx.cloneRootRepo / "Automation_KTOGame.slnx", "automation solution\n");
    WriteTextFile(ctx.cloneRootRepo / "Build" / "Windows" / "FileOpenOrder" / "CookerOpenOrder.log", "cook order\n");
    WriteTextFile(ctx.cloneRootRepo / "Build" / "Windows" / "ChunkLayerInfo" / "pakchunklayers.txt", "layers\n");
    WriteTextFile(ctx.cloneChildRepo / "Binaries" / "Win64" / "UnrealEditor-KanoAgentAuthoringEditor.dll", "dll\n");
    WriteTextFile(ctx.cloneChildRepo / "Intermediate" / "Build" / "Win64" / "x64" / "UnrealEditor" / "Development" /
                      "KanoAgentAuthoringEditor" / "KanoAgentAuthoringEditor.cpp.obj",
                  "obj\n");
    WriteTextFile(ctx.cloneChildRepo / "Intermediate" / "Build" / "Win64" / "x64" / "UnrealEditor" / "Development" /
                      "KanoAgentAuthoringEditor" / "KanoAgentAuthoringEditor.cpp.sarif",
                  "sarif\n");

    RequireSuccess(RunKog({"plan", "new", "--force", "--output", planPath.string()}, ctx.cloneRootRepo), "plan new");

    const auto ignoreInit = RunKog({"plan", "ignore-init", "--plan-file", planPath.string(), "--force"}, ctx.cloneRootRepo);
    INFO(ignoreInit.stdoutText);
    INFO(ignoreInit.stderrText);
    REQUIRE(ignoreInit.exitCode == 0);
    const auto initMerged = ignoreInit.stdoutText + "\n" + ignoreInit.stderrText;
    RequireContainsText(initMerged, "auto-ignoring: *.slnx (in .)");
    RequireContainsText(initMerged, "auto-ignoring: Build/Windows/FileOpenOrder/ (in .)");
    RequireContainsText(initMerged, "auto-ignoring: Build/Windows/ChunkLayerInfo/ (in .)");
    RequireContainsText(initMerged, std::string("auto-ignoring: Binaries/ (in ") + ctx.submodulePath + ")");
    RequireContainsText(initMerged, std::string("auto-ignoring: Intermediate/ (in ") + ctx.submodulePath + ")");

    const auto apply = RunKog({"plan", "apply", "--stage", "ignore", "--plan-file", planPath.string()}, ctx.cloneRootRepo);
    INFO(apply.stdoutText);
    INFO(apply.stderrText);
    REQUIRE(apply.exitCode == 0);

    const auto rootIgnore = ReadTextFile(ctx.cloneRootRepo / ".gitignore");
    RequireContainsText(rootIgnore, "*.slnx");
    RequireContainsText(rootIgnore, "Build/Windows/FileOpenOrder/");
    RequireContainsText(rootIgnore, "Build/Windows/ChunkLayerInfo/");

    const auto childIgnore = ReadTextFile(ctx.cloneChildRepo / ".gitignore");
    RequireContainsText(childIgnore, "Binaries/");
    RequireContainsText(childIgnore, "Intermediate/");

    RequireSuccess(RunGit({"check-ignore", "--", "KTOGame.slnx", "Build/Windows/FileOpenOrder/CookerOpenOrder.log",
                           "Build/Windows/ChunkLayerInfo/pakchunklayers.txt"},
                          ctx.cloneRootRepo),
                   "root unreal artifacts ignored");
    RequireSuccess(RunGit({"check-ignore", "--", "Binaries/Win64/UnrealEditor-KanoAgentAuthoringEditor.dll",
                           "Intermediate/Build/Win64/x64/UnrealEditor/Development/KanoAgentAuthoringEditor/"
                           "KanoAgentAuthoringEditor.cpp.obj"},
                          ctx.cloneChildRepo),
                   "submodule unreal artifacts ignored");
    RequireAuditAttempt(
        planPath, 1, kano::git::audit::OutcomeState::Succeeded,
        {"audit.reserve", "plan.ignore.apply", "plan.ignore.stamp"});

    RemoveSandboxWorkspace(ctx.sandbox);
}

TEST_CASE("commit_auto_ignores_unreal_artifacts_in_root_and_submodule", "[functional][commit][ignore-gate][KG-BUG-0016]") {
    const auto ctx = CreateRemoteWithSubmoduleClone("commit-ignore-unreal-artifacts");
    WriteTextFile(ctx.cloneRootRepo / "KTOGame.slnx", "solution\n");
    WriteTextFile(ctx.cloneRootRepo / "Build" / "Windows" / "FileOpenOrder" / "CookerOpenOrder.log", "cook order\n");
    WriteTextFile(ctx.cloneChildRepo / "Binaries" / "Win64" / "UnrealEditor-KanoAgentAuthoringEditor.dll", "dll\n");
    WriteTextFile(ctx.cloneChildRepo / "Intermediate" / "Build" / "Win64" / "x64" / "UnrealEditor" / "Development" /
                      "KanoAgentAuthoringEditor" / "KanoAgentAuthoringEditor.cpp.obj",
                  "obj\n");

    const auto result = RunKog({"commit", "-m", "test(functional): ignore generated Unreal artifacts"}, ctx.cloneRootRepo);
    INFO(result.stdoutText);
    INFO(result.stderrText);
    REQUIRE(result.exitCode == 0);

    const auto merged = result.stdoutText + "\n" + result.stderrText;
    RequireContainsText(merged, "[native-commit][ignore] generated artifacts detected; applying ignore rules before staging.");
    RequireContainsText(merged, "[native-commit][ignore] applied: repo=. rule=*.slnx");
    RequireContainsText(merged, "[native-commit][ignore] applied: repo=. rule=Build/Windows/FileOpenOrder/");
    RequireContainsText(merged, std::string("[native-commit][ignore] applied: repo=") + ctx.submodulePath + " rule=Binaries/");
    RequireContainsText(merged, std::string("[native-commit][ignore] applied: repo=") + ctx.submodulePath + " rule=Intermediate/");

    const auto rootIgnore = ReadTextFile(ctx.cloneRootRepo / ".gitignore");
    RequireContainsText(rootIgnore, "*.slnx");
    RequireContainsText(rootIgnore, "Build/Windows/FileOpenOrder/");

    const auto childIgnore = ReadTextFile(ctx.cloneChildRepo / ".gitignore");
    RequireContainsText(childIgnore, "Binaries/");
    RequireContainsText(childIgnore, "Intermediate/");

    RequireSuccess(RunGit({"check-ignore", "--", "KTOGame.slnx", "Build/Windows/FileOpenOrder/CookerOpenOrder.log"},
                          ctx.cloneRootRepo),
                   "root generated artifacts ignored after commit");
    RequireSuccess(RunGit({"check-ignore", "--", "Binaries/Win64/UnrealEditor-KanoAgentAuthoringEditor.dll",
                           "Intermediate/Build/Win64/x64/UnrealEditor/Development/KanoAgentAuthoringEditor/"
                           "KanoAgentAuthoringEditor.cpp.obj"},
                          ctx.cloneChildRepo),
                   "submodule generated artifacts ignored after commit");

    const auto staged = RunGit({"diff", "--cached", "--name-only"}, ctx.cloneRootRepo);
    RequireSuccess(staged, "cached diff after auto-ignore commit");
    REQUIRE(TrimCopy(staged.stdoutText).empty());

    RemoveSandboxWorkspace(ctx.sandbox);
}

TEST_CASE("workspace-relative ignore policy is consistent across plan commit and commit-push",
          "[functional][ignore-gate][nested-repo][KG-TSK-0057]") {
    const auto ctx = CreateRemoteWithSubmoduleClone("workspace-relative-ignore-policy");
    const auto skillRoot = ctx.sandbox.root / "packaged-skill";
    const auto policyPath =
        skillRoot / "assets" / "ignore" / "policy" / "ignore-gate-allowlist.txt";
    const auto policyPattern = ctx.submodulePath + "/src/cpp/scripts/**";
    WriteTextFile(policyPath, policyPattern + "\n");
    const std::vector<std::pair<std::string, std::string>> env = {
        {"KANO_GIT_SKILL_ROOT", skillRoot.string()},
        {"KOG_DISABLE_SECRET_GATE", "1"},
    };

    WriteTextFile(
        ctx.cloneChildRepo / "src" / "cpp" / "scripts" / "build" / "plan-tool.exe",
        "legitimate source fixture\n");
    const auto planVerify = RunKogWithEnv(
        {
            "plan", "verify", "ignore",
            "--workspace-root", ctx.cloneRootRepo.string(),
            "--context", "plan",
        },
        ctx.cloneRootRepo,
        env);
    INFO(planVerify.stdoutText);
    INFO(planVerify.stderrText);
    REQUIRE(planVerify.exitCode == 0);

    const auto preparePlan = [&](const std::filesystem::path& planPath,
                                 const std::string& childInclude,
                                 const std::string& message) {
        RequireSuccess(
            RunKogWithEnv(
                {"plan", "new", "--force", "--output", planPath.string()},
                ctx.cloneRootRepo,
                env),
            "create workspace-relative policy plan");
        RequireSuccess(
            RunKogWithEnv(
                {
                    "plan", "prepare", "add-commit-entry",
                    "--plan-file", planPath.string(),
                    "--repo", ctx.submodulePath,
                    "--commit-message", message + " child",
                    "--commit-include", childInclude,
                    "--commit-review-verdict", "pass",
                    "--commit-review-reason", "workspace-relative allowlist regression",
                },
                ctx.cloneRootRepo,
                env),
            "add nested-repository plan entry");
        RequireSuccess(
            RunKogWithEnv(
                {
                    "plan", "prepare", "add-commit-entry",
                    "--plan-file", planPath.string(),
                    "--repo", ".",
                    "--commit-message", message + " gitlink",
                    "--commit-include", ctx.submodulePath,
                    "--commit-review-verdict", "pass",
                    "--commit-review-reason", "record nested-repository gitlink",
                },
                ctx.cloneRootRepo,
                env),
            "add root gitlink plan entry");
    };

    const auto commitPlan = ctx.sandbox.root / "workspace-relative-commit-plan.json";
    preparePlan(
        commitPlan,
        "src/cpp/scripts/build/plan-tool.exe",
        "test(functional): workspace-relative commit allowlist");
    const auto commit = RunKogWithEnv(
        {"commit", "--plan-file", commitPlan.string()},
        ctx.cloneRootRepo,
        env);
    INFO(commit.stdoutText);
    INFO(commit.stderrText);
    REQUIRE(commit.exitCode == 0);
    RequireNotContainsText(commit.stdoutText + "\n" + commit.stderrText, "ignore gate failed");

    WriteTextFile(
        ctx.cloneChildRepo / "src" / "cpp" / "scripts" / "build" / "push-tool.exe",
        "second legitimate source fixture\n");
    const auto pushPlan = ctx.sandbox.root / "workspace-relative-push-plan.json";
    preparePlan(
        pushPlan,
        "src/cpp/scripts/build/push-tool.exe",
        "test(functional): workspace-relative commit-push allowlist");
    const auto commitPush = RunKogWithEnv(
        {"commit-push", "--plan-file", pushPlan.string()},
        ctx.cloneRootRepo,
        env);
    INFO(commitPush.stdoutText);
    INFO(commitPush.stderrText);
    REQUIRE(commitPush.exitCode == 0);
    RequireNotContainsText(
        commitPush.stdoutText + "\n" + commitPush.stderrText,
        "ignore gate failed");

    const auto [behind, ahead] = AheadBehindCounts(ctx.cloneRootRepo);
    REQUIRE(behind == 0);
    REQUIRE(ahead == 0);
    RemoveSandboxWorkspace(ctx.sandbox);
}

TEST_CASE("message shorthand commit coalesces registered child and parent gitlink work",
          "[functional][commit][message-plan][nested-repo][KG-BUG-0087]") {
    const auto ctx = CreateRemoteWithSubmoduleClone("message-plan-registered-commit");
    const auto skillRoot = ctx.sandbox.root / "packaged-skill";
    const auto policyPath =
        skillRoot / "assets" / "ignore" / "policy" / "ignore-gate-allowlist.txt";
    const auto policyPattern = ctx.submodulePath + "/src/cpp/scripts/**";
    const std::vector<std::pair<std::string, std::string>> env = {
        {"KANO_GIT_SKILL_ROOT", skillRoot.string()},
        {"KOG_DISABLE_SECRET_GATE", "1"},
        {"KOG_PROCESS_DIAGNOSTICS", "0"},
    };
    const std::string childPath = "src/cpp/scripts/build/message-commit-tool.exe";
    const std::string message = "test(functional): commit registered child shorthand";
    WriteTextFile(policyPath, policyPattern + "\n");

    const auto unrelatedRepo = (ctx.sandbox.root / "unrelated-commit-repo").lexically_normal();
    RequireSuccess(RunGit({"init", unrelatedRepo.string()}, ctx.sandbox.root), "init unrelated commit repo");
    ConfigureIdentity(unrelatedRepo);
    WriteTextFile(unrelatedRepo / "unrelated.txt", "unrelated seed\n");
    RequireSuccess(RunGit({"add", "unrelated.txt"}, unrelatedRepo), "add unrelated commit seed");
    RequireSuccess(RunGit({"commit", "-m", "seed unrelated commit repo"}, unrelatedRepo),
                   "commit unrelated seed");
    const auto unrelatedHeadBefore = CurrentHeadSha(unrelatedRepo);
    WriteTextFile(unrelatedRepo / "unrelated.txt", "unrelated seed\nremain dirty\n");

    const auto childHeadBefore = CurrentHeadSha(ctx.cloneChildRepo);
    const auto rootHeadBefore = CurrentHeadSha(ctx.cloneRootRepo);
    WriteTextFile(ctx.cloneChildRepo / childPath, "message shorthand child content\n");

    const auto result = RunKogWithEnv(
        {"commit", "-m", message},
        ctx.cloneRootRepo,
        env);
    INFO(result.stdoutText);
    INFO(result.stderrText);
    REQUIRE(result.exitCode == 0);

    const auto mergedOutput = result.stdoutText + "\n" + result.stderrText;
    RequireContainsText(mergedOutput, "[native-commit] synthesized plan file:");
    RequireContainsText(mergedOutput, "commit_waves=2 order=child-first");
    const auto childCommitPosition = mergedOutput.find("[commit] " + ctx.submodulePath);
    const auto rootCommitPosition =
        mergedOutput.find("[commit] " + ctx.cloneRootRepo.filename().generic_string() + " (.)");
    REQUIRE(childCommitPosition != std::string::npos);
    REQUIRE(rootCommitPosition != std::string::npos);
    REQUIRE(childCommitPosition < rootCommitPosition);

    const auto childHead = CurrentHeadSha(ctx.cloneChildRepo);
    const auto rootHead = CurrentHeadSha(ctx.cloneRootRepo);
    REQUIRE(childHead != childHeadBefore);
    REQUIRE(rootHead != rootHeadBefore);
    const auto childContent = RunGit({"show", "HEAD:" + childPath}, ctx.cloneChildRepo);
    RequireSuccess(childContent, "show message shorthand child content");
    REQUIRE(childContent.stdoutText == "message shorthand child content\n");
    REQUIRE(GitlinkHeadSha(ctx.cloneRootRepo, ctx.submodulePath) == childHead);

    const auto childPaths =
        RunGit({"diff-tree", "--no-commit-id", "--name-only", "-r", "HEAD"}, ctx.cloneChildRepo);
    RequireSuccess(childPaths, "list child shorthand commit paths");
    REQUIRE(TrimCopy(childPaths.stdoutText) == childPath);
    const auto rootPaths =
        RunGit({"diff-tree", "--no-commit-id", "--name-only", "-r", "HEAD"}, ctx.cloneRootRepo);
    RequireSuccess(rootPaths, "list parent shorthand commit paths");
    REQUIRE(TrimCopy(rootPaths.stdoutText) == ctx.submodulePath);
    REQUIRE(CurrentHeadSha(unrelatedRepo) == unrelatedHeadBefore);
    const auto unrelatedStatus =
        RunGit({"status", "--porcelain", "--", "unrelated.txt"}, unrelatedRepo);
    RequireSuccess(unrelatedStatus, "verify unrelated commit repo remains dirty");
    REQUIRE(TrimCopy(unrelatedStatus.stdoutText) == "M unrelated.txt");
    const auto trackedInternal =
        RunGit({"ls-files", "--", ".kano"}, ctx.cloneChildRepo);
    RequireSuccess(trackedInternal, "verify child pipeline cache is not tracked");
    REQUIRE(TrimCopy(trackedInternal.stdoutText).empty());
    const auto intendedStatus =
        RunGit({"status", "--porcelain", "--", childPath}, ctx.cloneChildRepo);
    RequireSuccess(intendedStatus, "verify intended child path is clean");
    REQUIRE(TrimCopy(intendedStatus.stdoutText).empty());

    const auto [childBehind, childAhead] = AheadBehindCounts(ctx.cloneChildRepo);
    REQUIRE(childBehind == 0);
    REQUIRE(childAhead == 1);
    const auto [rootBehind, rootAhead] = AheadBehindCounts(ctx.cloneRootRepo);
    REQUIRE(rootBehind == 0);
    REQUIRE(rootAhead == 1);

    RemoveSandboxWorkspace(ctx.sandbox);
}

TEST_CASE("message shorthand commit-push converges registered child before parent",
          "[functional][commit-push][message-plan][nested-repo][KG-BUG-0087]") {
    const auto ctx = CreateRemoteWithSubmoduleClone("message-plan-registered-commit-push");
    const auto skillRoot = ctx.sandbox.root / "packaged-skill";
    const auto policyPath =
        skillRoot / "assets" / "ignore" / "policy" / "ignore-gate-allowlist.txt";
    const auto policyPattern = ctx.submodulePath + "/src/cpp/scripts/**";
    const std::vector<std::pair<std::string, std::string>> env = {
        {"KANO_GIT_SKILL_ROOT", skillRoot.string()},
        {"KOG_DISABLE_SECRET_GATE", "1"},
        {"KOG_PROCESS_DIAGNOSTICS", "0"},
    };
    const std::string childPath = "src/cpp/scripts/build/message-push-tool.exe";
    const std::string message = "test(functional): push registered child shorthand";
    WriteTextFile(policyPath, policyPattern + "\n");
    RequireSuccess(
        RunGit(
            {"config", "kano.cache.local-dir", (ctx.sandbox.root / "_cache").string()},
            ctx.cloneChildRepo),
        "configure child external kano cache");

    const auto unrelatedRepo = (ctx.sandbox.root / "unrelated-push-repo").lexically_normal();
    RequireSuccess(RunGit({"init", unrelatedRepo.string()}, ctx.sandbox.root), "init unrelated push repo");
    ConfigureIdentity(unrelatedRepo);
    WriteTextFile(unrelatedRepo / "unrelated.txt", "unrelated seed\n");
    RequireSuccess(RunGit({"add", "unrelated.txt"}, unrelatedRepo), "add unrelated push seed");
    RequireSuccess(RunGit({"commit", "-m", "seed unrelated push repo"}, unrelatedRepo),
                   "commit unrelated push seed");
    const auto unrelatedHeadBefore = CurrentHeadSha(unrelatedRepo);
    WriteTextFile(unrelatedRepo / "unrelated.txt", "unrelated seed\nremain dirty\n");

    const auto childHeadBefore = CurrentHeadSha(ctx.cloneChildRepo);
    const auto rootHeadBefore = CurrentHeadSha(ctx.cloneRootRepo);
    WriteTextFile(ctx.cloneChildRepo / childPath, "message shorthand pushed child content\n");

    const auto result = RunKogWithEnv(
        {"commit-push", "-m", message},
        ctx.cloneRootRepo,
        env);
    INFO(result.stdoutText);
    INFO(result.stderrText);
    REQUIRE(result.exitCode == 0);

    const auto mergedOutput = result.stdoutText + "\n" + result.stderrText;
    RequireContainsText(mergedOutput, "commit_waves=2 order=child-first");
    const auto childCommitPosition = mergedOutput.find("[commit] " + ctx.submodulePath);
    const auto rootCommitPosition =
        mergedOutput.find("[commit] " + ctx.cloneRootRepo.filename().generic_string() + " (.)");
    REQUIRE(childCommitPosition != std::string::npos);
    REQUIRE(rootCommitPosition != std::string::npos);
    REQUIRE(childCommitPosition < rootCommitPosition);

    const auto childHead = CurrentHeadSha(ctx.cloneChildRepo);
    const auto rootHead = CurrentHeadSha(ctx.cloneRootRepo);
    REQUIRE(childHead != childHeadBefore);
    REQUIRE(rootHead != rootHeadBefore);
    const auto childContent = RunGit({"show", "HEAD:" + childPath}, ctx.cloneChildRepo);
    RequireSuccess(childContent, "show pushed shorthand child content");
    REQUIRE(childContent.stdoutText == "message shorthand pushed child content\n");
    REQUIRE(GitlinkHeadSha(ctx.cloneRootRepo, ctx.submodulePath) == childHead);

    const auto childPaths =
        RunGit({"diff-tree", "--no-commit-id", "--name-only", "-r", "HEAD"}, ctx.cloneChildRepo);
    RequireSuccess(childPaths, "list pushed child shorthand commit paths");
    REQUIRE(TrimCopy(childPaths.stdoutText) == childPath);
    const auto rootPaths =
        RunGit({"diff-tree", "--no-commit-id", "--name-only", "-r", "HEAD"}, ctx.cloneRootRepo);
    RequireSuccess(rootPaths, "list pushed parent shorthand commit paths");
    REQUIRE(TrimCopy(rootPaths.stdoutText) == ctx.submodulePath);
    REQUIRE(CurrentHeadSha(unrelatedRepo) == unrelatedHeadBefore);
    const auto unrelatedStatus =
        RunGit({"status", "--porcelain", "--", "unrelated.txt"}, unrelatedRepo);
    RequireSuccess(unrelatedStatus, "verify unrelated push repo remains dirty");
    REQUIRE(TrimCopy(unrelatedStatus.stdoutText) == "M unrelated.txt");
    REQUIRE(StatusPorcelain(ctx.cloneChildRepo).empty());
    REQUIRE(StatusPorcelain(ctx.cloneRootRepo).empty());

    const auto [childBehind, childAhead] = AheadBehindCounts(ctx.cloneChildRepo);
    REQUIRE(childBehind == 0);
    REQUIRE(childAhead == 0);
    const auto [rootBehind, rootAhead] = AheadBehindCounts(ctx.cloneRootRepo);
    REQUIRE(rootBehind == 0);
    REQUIRE(rootAhead == 0);
    REQUIRE(RefSha(ctx.childBareRemote, "refs/heads/" + ctx.branch) == childHead);
    REQUIRE(RefSha(ctx.rootBareRemote, "refs/heads/" + ctx.branch) == rootHead);

    RemoveSandboxWorkspace(ctx.sandbox);
}

TEST_CASE("commit no-recursive message plan avoids child probes and falls back from an unusable global cache",
          "[functional][commit][no-recursive][staged-only][KG-BUG-0043][KG-BUG-0081]") {
    const auto ctx = CreateRemoteWithSubmoduleClone("commit-no-recursive-repo-freshness");
    const auto diagPath = (ctx.sandbox.root / "commit-no-recursive-process.log").lexically_normal();
    std::filesystem::remove(diagPath);
    const auto discover = RunKog(
        {"discover", "--format", "json", "--repo-root", ctx.cloneRootRepo.string(), "--no-unregistered-scan"},
        ctx.cloneRootRepo);
    INFO(discover.stdoutText);
    INFO(discover.stderrText);
    REQUIRE(discover.exitCode == 0);
    WriteTextFile(ctx.cloneRootRepo / "README.md", "root seed\nroot staged change\n");
    RequireSuccess(RunGit({"add", "README.md"}, ctx.cloneRootRepo), "stage root-only change");
    WriteTextFile(ctx.cloneChildRepo / "child.txt", "child seed\nunrelated child change\n");

    const auto preflightDiagPath = (ctx.sandbox.root / "commit-no-recursive-preflight-process.log").lexically_normal();
    std::filesystem::remove(preflightDiagPath);
    const auto preflight = RunKogWithEnv(
        {"commit", "--no-recursive", "--staged-only", "--preflight-only", "--profile"},
        ctx.cloneRootRepo,
        {{"KOG_PROCESS_DIAGNOSTICS_LOG", preflightDiagPath.string()}});
    INFO(preflight.stdoutText);
    INFO(preflight.stderrText);
    REQUIRE(preflight.exitCode == 0);
    RequireNotContainsText(
        preflight.stdoutText + "\n" + preflight.stderrText,
        ctx.cloneChildRepo.lexically_normal().generic_string());
    REQUIRE(std::filesystem::exists(preflightDiagPath));
    RequireNotContainsText(
        ReadTextFile(preflightDiagPath),
        "[process-diag] cwd=" + ctx.cloneChildRepo.lexically_normal().generic_string());

    const auto blockedGlobalCache = (ctx.sandbox.root / "blocked-global-cache").lexically_normal();
    WriteTextFile(blockedGlobalCache, "not a directory\n");
    RequireSuccess(
        RunGit({"config", "kano.cache.global-dir", blockedGlobalCache.string()}, ctx.cloneRootRepo),
        "configure unusable global cache root");

    const auto result = RunKogWithEnv(
        {"commit", "--no-recursive", "--staged-only", "-m",
         "test(functional): repo-only plan freshness"},
        ctx.cloneRootRepo,
        {
            {"KOG_PLAN_FRESHNESS_SCOPE", "registered-only"},
            {"KOG_PROCESS_DIAGNOSTICS_LOG", diagPath.string()},
        });
    INFO(result.stdoutText);
    INFO(result.stderrText);
    REQUIRE(result.exitCode == 0);

    const auto merged = result.stdoutText + "\n" + result.stderrText;
    RequireContainsText(merged, "mode=repo");
    REQUIRE(merged.find("mode=registered-only") == std::string::npos);
    RequireContainsText(merged, "Repo: . already on branch");
    const auto workspacePlanDir =
        (ctx.cloneRootRepo / ".kano" / "cache" / "git" / "plans").lexically_normal();
    RequireContainsText(merged, workspacePlanDir.generic_string());
    REQUIRE(std::filesystem::is_directory(workspacePlanDir));
    REQUIRE(merged.find("Repo: " + ctx.submodulePath) == std::string::npos);
    RequireNotContainsText(merged, ctx.cloneChildRepo.lexically_normal().generic_string());
    REQUIRE(StatusPorcelain(ctx.cloneChildRepo).find("child.txt") != std::string::npos);
    REQUIRE(StatusPorcelain(ctx.cloneRootRepo).find(".kano") == std::string::npos);
    REQUIRE(std::filesystem::exists(diagPath));
    const auto diagnostics = ReadTextFile(diagPath);
    RequireNotContainsText(
        diagnostics,
        "[process-diag] cwd=" + ctx.cloneChildRepo.lexically_normal().generic_string());

    const auto committedPaths = RunGit({"show", "--pretty=format:", "--name-only", "HEAD"}, ctx.cloneRootRepo);
    RequireSuccess(committedPaths, "read root-only commit paths");
    REQUIRE(TrimCopy(committedPaths.stdoutText) == "README.md");

    RemoveSandboxWorkspace(ctx.sandbox);
}

TEST_CASE("commit_plan_scoped_ignore_gate_allows_tracked_source_under_build_segment", "[functional][commit][ignore-gate][KG-BUG-0016]") {
    const auto ctx = CreateRemoteWithClone("commit-plan-build-source-delete");
    const auto sourcePath = std::filesystem::path("src") / "cpp" / "build" / "script" / "common" / "tool.sh";
    WriteTextFile(ctx.cloneRepo / sourcePath, "#!/usr/bin/env bash\necho source\n");
    RequireSuccess(RunGit({"add", sourcePath.generic_string()}, ctx.cloneRepo), "add tracked build source");
    RequireSuccess(RunGit({"commit", "-m", "test(functional): seed tracked build source"}, ctx.cloneRepo), "commit tracked build source");
    RequireSuccess(RunGit({"push"}, ctx.cloneRepo), "push tracked build source");

    std::error_code ec;
    REQUIRE(std::filesystem::remove(ctx.cloneRepo / sourcePath, ec));
    REQUIRE(!ec);

    const auto planPath = (ctx.cloneRepo / ".kano" / "cache" / "git" / "plans" / "build-source-delete.json").lexically_normal();
    RequireSuccess(RunKog({"plan", "new", "--force", "--output", planPath.string()}, ctx.cloneRepo), "plan new");
    RequireSuccess(
        RunKog({
            "plan", "prepare", "add-commit-entry",
            "--plan-file", planPath.string(),
            "--repo", ".",
            "--commit-message", "test(functional): remove tracked build source",
            "--commit-include", sourcePath.generic_string(),
            "--commit-review-verdict", "pass",
            "--commit-review-reason", "functional regression for tracked source under build segment"
        }, ctx.cloneRepo),
        "plan add commit entry");
    RequireSuccess(
        RunKog({"plan", "verify", "pre-apply", "--stage", "commit", "--plan-file", planPath.string()}, ctx.cloneRepo),
        "plan verify pre-apply");

    const auto result = RunKog({"commit-push", "--plan-file", planPath.string()}, ctx.cloneRepo);
    INFO(result.stdoutText);
    INFO(result.stderrText);
    REQUIRE(result.exitCode == 0);
    const auto merged = result.stdoutText + "\n" + result.stderrText;
    REQUIRE(merged.find("ignore gate failed") == std::string::npos);
    RequireContainsText(result.stdoutText, "scoped safety gates checked files=1");

    const auto status = RunGit({"status", "--short"}, ctx.cloneRepo);
    RequireSuccess(status, "status after tracked build source delete commit");
    REQUIRE(TrimCopy(status.stdoutText).empty());

    const auto subject = RunGit({"log", "-1", "--pretty=%s"}, ctx.cloneRepo);
    RequireSuccess(subject, "read tracked build source delete commit subject");
    REQUIRE(TrimCopy(subject.stdoutText) == "test(functional): remove tracked build source");

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
        {"status", longRepo.string(), "--repo-root", sandbox.root.string(), "--all"},
        sandbox.root,
        {{"COLUMNS", "80"}});
    INFO(narrow.stdoutText);
    INFO(narrow.stderrText);
    REQUIRE(narrow.exitCode == 0);

    // The first cache write creates the repository-local .kano directory after
    // computing its marker. Warm it once more so the wide run deterministically
    // exercises a cache hit and the persisted root path stored as ".".
    const auto warmCache = RunKogWithEnv(
        {"status", longRepo.string(), "--repo-root", sandbox.root.string(), "--all"},
        sandbox.root,
        {{"COLUMNS", "80"}});
    INFO(warmCache.stdoutText);
    INFO(warmCache.stderrText);
    REQUIRE(warmCache.exitCode == 0);

    const auto wide = RunKogWithEnv(
        {"status", longRepo.string(), "--repo-root", sandbox.root.string(), "--all"},
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

TEST_CASE("commit_plan_stage_lock_recovery_cleans_stale_lock_and_retries_original_commit", "[functional][commit][locks][plan-file]") {
    const auto ctx = CreateRemoteWithClone("commit-plan-stage-lock-recovery");
    const std::string message = "test(functional): recover stale plan-stage commit lock";
    WriteTextFile(ctx.cloneRepo / "README.md", "seed\ncommit plan-stage lock recovery\n");

    const auto planPath = (ctx.cloneRepo / ".kano" / "cache" / "git" / "plans" / "lock-recovery.json").lexically_normal();
    RequireSuccess(RunKog({"plan", "new", "--force", "--output", planPath.string()}, ctx.cloneRepo), "plan new");
    RequireSuccess(
        RunKog({
            "plan", "prepare", "add-commit-entry",
            "--plan-file", planPath.string(),
            "--repo", ".",
            "--commit-message", message,
            "--commit-include", "README.md",
            "--commit-review-verdict", "pass",
            "--commit-review-reason", "functional regression for plan-stage lock recovery"
        }, ctx.cloneRepo),
        "plan add commit entry");
    RequireSuccess(
        RunKog({"plan", "verify", "pre-apply", "--stage", "commit", "--plan-file", planPath.string()}, ctx.cloneRepo),
        "plan verify pre-apply");

    const auto lockPath = (ctx.cloneRepo / ".git" / "index.lock").lexically_normal();
    TouchFile(lockPath);
    SetFileAgeSeconds(lockPath, 10);

    const auto result = RunKogWithEnv(
        {"commit", "--plan-file", planPath.string(), "--plan-stage", "commit"},
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
    RequireSuccess(subject, "read recovered plan-stage commit subject");
    REQUIRE(TrimCopy(subject.stdoutText) == message);
    RequireAuditAttempt(
        planPath, 1, kano::git::audit::OutcomeState::Succeeded,
        {"audit.reserve", "plan.safety", "commit.apply"});

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

TEST_CASE("sync origin-latest no-recursive does not probe nested repositories", "[functional][sync][discovery][KG-BUG-0028]") {
    const auto ctx = CreateRemoteWithClone("sync-no-recursive-root-only");
    const auto nestedRepo = (ctx.cloneRepo / "products" / "deep" / "nested-repo").lexically_normal();
    InitPlainGitRepo(nestedRepo);

    const auto result = RunKog(
        {"sync", "origin-latest", "--no-recursive"},
        ctx.cloneRepo);
    INFO(result.stdoutText);
    INFO(result.stderrText);
    REQUIRE(result.exitCode == 0);
    RequireContainsText(result.stdoutText, "Syncing current repository only (non-recursive mode)");
    RequireContainsText(result.stdoutText, "Repo: .");

    const auto diagPath = (ctx.cloneRepo / ".kano" / "tmp" / "functional-process-diag.log").lexically_normal();
    const auto diagnostics = ReadTextFile(diagPath);
    RequireNotContainsText(diagnostics, "[process-diag] cwd=" + nestedRepo.generic_string());

    RemoveSandboxWorkspace(ctx.sandbox);
}

TEST_CASE("sync detects target repo rebase before restoring auto-stash", "[functional][sync][conflict][stash][KG-BUG-0030]") {
    const auto ctx = CreateRemoteWithClone("sync-target-repo-rebase-state");

    WriteTextFile(ctx.seedRepo / "README.md", "remote conflicting line\n");
    RequireSuccess(RunGit({"commit", "-am", "remote conflicting change"}, ctx.seedRepo), "commit remote conflict");
    RequireSuccess(RunGit({"push", "origin", ctx.branch}, ctx.seedRepo), "push remote conflict");

    WriteTextFile(ctx.cloneRepo / "README.md", "local conflicting line\n");
    RequireSuccess(RunGit({"commit", "-am", "local conflicting change"}, ctx.cloneRepo), "commit local conflict");
    const auto localHead = CurrentHeadSha(ctx.cloneRepo);
    WriteTextFile(ctx.cloneRepo / "local-dirty.txt", "restore after failed sync\n");

    const auto result = RunKog(
        {"sync", "origin-latest", "--repo", ctx.cloneRepo.string(), "--no-recursive"},
        ctx.sandbox.root);
    INFO(result.stdoutText);
    INFO(result.stderrText);
    REQUIRE(result.exitCode != 0);
    const auto output = result.stdoutText + "\n" + result.stderrText;
    RequireContainsText(output, "SYNC_CONFLICT: rebase conflict detected; aborting rebase for manual review");
    RequireNotContainsText(output, "could not write index");
    REQUIRE(CurrentHeadSha(ctx.cloneRepo) == localHead);
    const auto status = RunGit({"status", "--short"}, ctx.cloneRepo);
    RequireSuccess(status, "read status after conflict recovery");
    RequireContainsText(status.stdoutText, "?? local-dirty.txt");

    const auto stashList = RunGit({"stash", "list"}, ctx.cloneRepo);
    RequireSuccess(stashList, "read stash list after conflict recovery");
    REQUIRE(TrimCopy(stashList.stdoutText).empty());
    REQUIRE_FALSE(std::filesystem::exists(ctx.cloneRepo / ".git" / "rebase-merge"));
    REQUIRE_FALSE(std::filesystem::exists(ctx.cloneRepo / ".git" / "rebase-apply"));

    RemoveSandboxWorkspace(ctx.sandbox);
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
