#include "commit_lock_recovery.hpp"

#include "plan_utils.hpp"
#include "shell_executor.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <process.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace kano::git::commands {
namespace {

struct ActiveProcessProbe {
    bool blocked = false;
    std::string detail;
};

struct CommitGitLockDiagnosis {
    std::string lockName;
    std::filesystem::path lockPath;
    bool exists = false;
    long long ageSeconds = -1;
};

auto DisplayRepoLabel(
    const std::filesystem::path& InWorkspaceRoot,
    const std::filesystem::path& InRepo) -> std::string {
    const auto rootNorm = NormalizePath(InWorkspaceRoot);
    const auto repoNorm = NormalizePath(InRepo);
    if (ToGeneric(rootNorm) == ToGeneric(repoNorm)) {
        auto rootName = rootNorm.filename().generic_string();
        if (rootName.empty()) {
            rootName = rootNorm.generic_string();
        }
        return rootName + " (.)";
    }
    const auto rel = repoNorm.lexically_relative(rootNorm);
    if (!rel.empty() && rel != ".") {
        return rel.generic_string();
    }
    return repoNorm.generic_string();
}

auto CurrentBranch(const std::filesystem::path& InRepo) -> std::string {
    const auto out = GitCapture(InRepo, {"rev-parse", "--abbrev-ref", "HEAD"});
    if (out.exitCode != 0) {
        return {};
    }
    const auto value = Trim(out.stdoutStr);
    if (value == "HEAD") {
        return {};
    }
    return value;
}

auto ParseOptionalBoolEnv(const char* InValue) -> std::optional<bool> {
    if (InValue == nullptr) {
        return std::nullopt;
    }
    const auto normalized = ToLower(Trim(std::string(InValue)));
    if (normalized == "1" || normalized == "true" ||
        normalized == "yes" || normalized == "on") {
        return true;
    }
    if (normalized == "0" || normalized == "false" ||
        normalized == "no" || normalized == "off") {
        return false;
    }
    return std::nullopt;
}

auto CurrentProcessIdForCommitLockRecovery() -> long long {
#if defined(_WIN32)
    return static_cast<long long>(_getpid());
#else
    return static_cast<long long>(getpid());
#endif
}

auto ResolveSelfBinaryForCommitLockRecovery() -> std::string {
    if (const char* path = std::getenv("KANO_GIT_BINARY_PATH");
        path != nullptr && *path != '\0') {
        return std::filesystem::path(path).lexically_normal().string();
    }
#if defined(_WIN32)
    std::string buffer(MAX_PATH, '\0');
    const auto written =
        GetModuleFileNameA(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (written > 0) {
        buffer.resize(written);
        return std::filesystem::path(buffer).lexically_normal().string();
    }
    return "kano-git.exe";
#else
    std::string buffer(4096, '\0');
    const auto written = readlink("/proc/self/exe", buffer.data(), buffer.size() - 1);
    if (written > 0) {
        buffer.resize(static_cast<std::size_t>(written));
        return std::filesystem::path(buffer).lexically_normal().string();
    }
    return "kano-git";
#endif
}

auto SetProcessEnvOverride(
    const std::string& InKey,
    const std::string& InValue) -> void {
#if defined(_WIN32)
    _putenv_s(InKey.c_str(), InValue.c_str());
#else
    setenv(InKey.c_str(), InValue.c_str(), 1);
#endif
}

auto UnsetProcessEnvOverride(const std::string& InKey) -> void {
#if defined(_WIN32)
    _putenv_s(InKey.c_str(), "");
#else
    unsetenv(InKey.c_str());
#endif
}

class ScopedProcessEnvOverride {
  public:
    ScopedProcessEnvOverride(std::string InKey, std::string InValue)
        : key_(std::move(InKey)) {
        if (const char* previous = std::getenv(key_.c_str()); previous != nullptr) {
            previous_ = std::string(previous);
        }
        SetProcessEnvOverride(key_, InValue);
    }

    ~ScopedProcessEnvOverride() {
        if (previous_.has_value()) {
            SetProcessEnvOverride(key_, *previous_);
        } else {
            UnsetProcessEnvOverride(key_);
        }
    }

    ScopedProcessEnvOverride(const ScopedProcessEnvOverride&) = delete;
    auto operator=(const ScopedProcessEnvOverride&)
        -> ScopedProcessEnvOverride& = delete;

  private:
    std::string key_;
    std::optional<std::string> previous_;
};

auto DetectActiveCommitLockRecoveryProcess() -> ActiveProcessProbe {
    if (const auto forced = ParseOptionalBoolEnv(
            std::getenv("KOG_COMMIT_LOCK_RECOVERY_TEST_ACTIVE_PROCESS"));
        forced.has_value()) {
        return ActiveProcessProbe{
            .blocked = *forced,
            .detail = *forced
                ? "test override KOG_COMMIT_LOCK_RECOVERY_TEST_ACTIVE_PROCESS=1"
                : ""
        };
    }
    if (const auto forcedGit = ParseOptionalBoolEnv(
            std::getenv("KOG_SYNC_TEST_ASSUME_ACTIVE_GIT_PROCESS"));
        forcedGit.has_value() && *forcedGit) {
        return ActiveProcessProbe{
            .blocked = true,
            .detail = "test override KOG_SYNC_TEST_ASSUME_ACTIVE_GIT_PROCESS=1"
        };
    }

#if defined(_WIN32)
    const auto result = shell::ExecuteCommand(
        "powershell",
        {"-NoLogo", "-NoProfile", "-Command",
         std::format(
             "$self={}; "
             "$names=@('git.exe','kano-git.exe','kog.exe','opencode.exe','claude.exe'); "
             "$p=Get-CimInstance Win32_Process -ErrorAction SilentlyContinue | "
             "Where-Object {{ $_.ProcessId -ne $self -and "
             "$names -contains $_.Name.ToLowerInvariant() -and "
             "(($_.CommandLine -as [string]) -notmatch 'fsmonitor--daemon') }} | "
             "Select-Object -First 8; "
             "if ($null -eq $p) {{ exit 1 }}; "
             "$p | ForEach-Object {{ Write-Output (\"{{0}}:{{1}}:{{2}}\" -f $_.ProcessId,$_.Name,(($_.CommandLine -as [string]) -replace '\\s+',' ')) }}; "
             "exit 0",
             CurrentProcessIdForCommitLockRecovery())},
        shell::ExecMode::Capture,
        std::filesystem::current_path());
    if (result.exitCode == 0) {
        return ActiveProcessProbe{
            .blocked = true,
            .detail = Trim(result.stdoutStr)
        };
    }
    return {};
#else
    const auto result = shell::ExecuteCommand(
        "ps",
        {"-axo", "pid=,args="},
        shell::ExecMode::Capture,
        std::filesystem::current_path());
    if (result.exitCode != 0) {
        return {};
    }
    std::istringstream iss(result.stdoutStr);
    std::string line;
    std::vector<std::string> active;
    const auto selfPid = CurrentProcessIdForCommitLockRecovery();
    while (std::getline(iss, line)) {
        line = Trim(line);
        if (line.empty()) {
            continue;
        }
        std::istringstream ls(line);
        long long pid = -1;
        std::string command;
        ls >> pid;
        std::getline(ls, command);
        command = Trim(command);
        if (pid <= 0 || pid == selfPid) {
            continue;
        }
        const auto lowerCommand = ToLower(command);
        if (lowerCommand.find("fsmonitor--daemon") != std::string::npos) {
            continue;
        }
        std::istringstream commandStream(command);
        std::string executable;
        commandStream >> executable;
        const auto base =
            ToLower(std::filesystem::path(executable).filename().string());
        if (base == "git" || base == "kano-git" || base == "kog" ||
            base == "opencode" || base == "claude") {
            active.push_back(std::format("{}:{}", pid, base));
            if (active.size() >= 8) {
                break;
            }
        }
    }
    if (!active.empty()) {
        std::ostringstream joined;
        for (std::size_t index = 0; index < active.size(); ++index) {
            if (index > 0) {
                joined << ", ";
            }
            joined << active[index];
        }
        return ActiveProcessProbe{
            .blocked = true,
            .detail = joined.str()
        };
    }
    return {};
#endif
}

auto IsCommitLockFailureText(const std::string& InText) -> bool {
    const auto merged = ToLower(InText);
    const bool mentionsLock =
        merged.find("index.lock") != std::string::npos ||
        merged.find("config.lock") != std::string::npos ||
        merged.find(".lock") != std::string::npos;
    if (!mentionsLock) {
        return false;
    }
    return merged.find("unable to create") != std::string::npos ||
           merged.find("file exists") != std::string::npos ||
           merged.find("another git process seems to be running") !=
               std::string::npos ||
           merged.find("could not write index") != std::string::npos ||
           merged.find("could not lock") != std::string::npos ||
           merged.find("failed to lock") != std::string::npos;
}

auto IsCommitLockFailure(const RepoCommitResult& InResult) -> bool {
    if (!InResult.failed) {
        return false;
    }
    return IsCommitLockFailureText(
        InResult.note + "\n" + InResult.stdoutText + "\n" +
        InResult.stderrText);
}

auto ResolveCommitGitLockPath(
    const std::filesystem::path& InRepo,
    const std::string& InLockName) -> std::filesystem::path {
    const auto result =
        GitCapture(InRepo, {"rev-parse", "--git-path", InLockName});
    if (result.exitCode != 0) {
        return {};
    }
    auto path = std::filesystem::path(Trim(result.stdoutStr));
    if (path.empty()) {
        return {};
    }
    if (path.is_relative()) {
        path = std::filesystem::absolute((InRepo / path).lexically_normal());
    }
    return path.lexically_normal();
}

auto DiagnoseCommitGitLock(
    const std::filesystem::path& InRepo,
    const std::string& InLockName) -> CommitGitLockDiagnosis {
    CommitGitLockDiagnosis out;
    out.lockName = InLockName;
    out.lockPath = ResolveCommitGitLockPath(InRepo, InLockName);
    if (out.lockPath.empty()) {
        return out;
    }
    std::error_code ec;
    out.exists = std::filesystem::exists(out.lockPath, ec) && !ec;
    if (!out.exists) {
        return out;
    }
    const auto writeTime = std::filesystem::last_write_time(out.lockPath, ec);
    if (!ec) {
        const auto now = decltype(writeTime)::clock::now();
        out.ageSeconds =
            std::chrono::duration_cast<std::chrono::seconds>(now - writeTime)
                .count();
        if (out.ageSeconds < 0) {
            out.ageSeconds = 0;
        }
    }
    return out;
}

auto CleanupStaleCommitLocksForRepo(
    const std::filesystem::path& InWorkspaceRoot,
    const std::filesystem::path& InRepo,
    std::string* OutReason) -> bool {
    const auto label = DisplayRepoLabel(InWorkspaceRoot, InRepo);
    bool sawExistingLock = false;
    for (const auto& lockName :
         {std::string{"index.lock"}, std::string{"config.lock"}}) {
        const auto diagnosis = DiagnoseCommitGitLock(InRepo, lockName);
        if (!diagnosis.exists) {
            continue;
        }
        sawExistingLock = true;
        std::cout << "[native-commit][lock-recovery] detected " << lockName
                  << " repo=" << label
                  << " path=" << diagnosis.lockPath.generic_string();
        if (diagnosis.ageSeconds >= 0) {
            std::cout << " age_seconds=" << diagnosis.ageSeconds;
        }
        std::cout << "\n";

        if (diagnosis.ageSeconds >= 0 && diagnosis.ageSeconds < 2) {
            if (OutReason != nullptr) {
                *OutReason = std::format(
                    "{} is too new to remove automatically for {}",
                    lockName,
                    label);
            }
            return false;
        }

        std::error_code ec;
        const bool removed = std::filesystem::remove(diagnosis.lockPath, ec);
        if (!removed || ec) {
            if (OutReason != nullptr) {
                *OutReason = std::format(
                    "failed to remove {} for {}: {}",
                    lockName,
                    label,
                    ec.message());
            }
            return false;
        }
        std::cout << "[native-commit][lock-recovery] removed stale "
                  << lockName << " repo=" << label << "\n";
    }

    if (!sawExistingLock) {
        std::cout << "[native-commit][lock-recovery] no git lock file remains for repo="
                  << label << "\n";
    }
    return true;
}

auto RunCommitLockRecoveryConvergeProbe(
    const std::filesystem::path& InWorkspaceRoot,
    const std::filesystem::path& InRepo,
    std::string* OutReason) -> bool {
    std::vector<std::string> args{
        "converge", "--dry-run", "--no-recursive", "--jobs", "1"};
    if (const auto branch = CurrentBranch(InRepo); !branch.empty()) {
        args.push_back("--target");
        args.push_back(branch);
    }
    std::ostringstream commandText;
    commandText << ResolveSelfBinaryForCommitLockRecovery();
    for (const auto& arg : args) {
        commandText << ' ' << arg;
    }
    std::cout << "[native-commit][lock-recovery] running bounded converge probe: "
              << commandText.str() << "\n";
    ScopedProcessEnvOverride depth("KOG_COMMIT_LOCK_RECOVERY_DEPTH", "1");
    const auto result = shell::ExecuteCommand(
        ResolveSelfBinaryForCommitLockRecovery(),
        args,
        shell::ExecMode::Capture,
        InRepo);
    if (result.exitCode == 0) {
        std::cout << "[native-commit][lock-recovery] converge probe passed for repo="
                  << DisplayRepoLabel(InWorkspaceRoot, InRepo) << "\n";
        return true;
    }
    if (!result.stdoutStr.empty()) {
        std::cout << result.stdoutStr;
        if (!result.stdoutStr.ends_with('\n')) {
            std::cout << '\n';
        }
    }
    if (!result.stderrStr.empty()) {
        std::cerr << result.stderrStr;
        if (!result.stderrStr.ends_with('\n')) {
            std::cerr << '\n';
        }
    }
    if (OutReason != nullptr) {
        *OutReason = std::format(
            "bounded converge probe failed with exit code {}",
            result.exitCode);
    }
    return false;
}

auto AttemptCommitLockRecoveryOnce(
    const std::filesystem::path& InWorkspaceRoot,
    const std::filesystem::path& InRepo,
    CommitLockRecoveryState& InState) -> bool {
    std::lock_guard lock(InState.mutex);
    if (InState.attempted) {
        return InState.succeeded;
    }

    InState.attempted = true;
    if (IsTruthyEnv(std::getenv("KOG_DISABLE_COMMIT_LOCK_RECOVERY"))) {
        InState.reason = "disabled by KOG_DISABLE_COMMIT_LOCK_RECOVERY";
        std::cerr << "[native-commit][lock-recovery] blocked: "
                  << InState.reason << "\n";
        return false;
    }
    if (IsTruthyEnv(std::getenv("KOG_COMMIT_LOCK_RECOVERY_DEPTH"))) {
        InState.reason = "already inside commit lock recovery";
        std::cerr << "[native-commit][lock-recovery] blocked: "
                  << InState.reason << "\n";
        return false;
    }

    const auto active = DetectActiveCommitLockRecoveryProcess();
    if (active.blocked) {
        InState.reason = active.detail.empty()
            ? "active git/kog/coding-agent process detected"
            : "active git/kog/coding-agent process detected: " + active.detail;
        std::cerr << "[native-commit][lock-recovery] blocked: "
                  << InState.reason << "\n";
        return false;
    }

    std::string reason;
    if (!CleanupStaleCommitLocksForRepo(
            InWorkspaceRoot, InRepo, &reason)) {
        InState.reason =
            reason.empty() ? "stale lock cleanup failed" : reason;
        std::cerr << "[native-commit][lock-recovery] blocked: "
                  << InState.reason << "\n";
        return false;
    }
    if (!RunCommitLockRecoveryConvergeProbe(
            InWorkspaceRoot, InRepo, &reason)) {
        InState.reason =
            reason.empty() ? "bounded converge probe failed" : reason;
        std::cerr << "[native-commit][lock-recovery] blocked: "
                  << InState.reason << "\n";
        return false;
    }

    InState.succeeded = true;
    InState.reason = "stale lock cleanup and converge probe completed";
    return true;
}

} // namespace

auto MaybeRecoverCommitLockFailure(
    const std::filesystem::path& InWorkspaceRoot,
    RepoCommitResult InResult,
    CommitLockRecoveryState& InState,
    const std::function<RepoCommitResult()>& InRetryFn) -> RepoCommitResult {
    if (!IsCommitLockFailure(InResult)) {
        return InResult;
    }

    std::cout << "[native-commit][lock-recovery] lock failure detected repo="
              << DisplayRepoLabel(InWorkspaceRoot, InResult.repo)
              << " note=" << InResult.note << "\n";
    if (!AttemptCommitLockRecoveryOnce(
            InWorkspaceRoot, InResult.repo, InState)) {
        if (!InState.reason.empty()) {
            InResult.note +=
                " (lock recovery blocked: " + InState.reason + ")";
        }
        return InResult;
    }

    std::cout << "[native-commit][lock-recovery] retrying original commit once repo="
              << DisplayRepoLabel(InWorkspaceRoot, InResult.repo) << "\n";
    auto retried = InRetryFn();
    if (!retried.failed && retried.note.empty()) {
        retried.note = "committed after lock recovery";
    } else if (!retried.failed) {
        retried.note += " after lock recovery";
    } else if (!InState.reason.empty()) {
        retried.note += " after lock recovery retry";
    }
    return retried;
}

} // namespace kano::git::commands
