// sync command — Repository synchronization workflows
// Uses native Git synchronization and kog-managed workflows

#include <CLI/CLI.hpp>
#include "discovery.hpp"
#include "shell_executor.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <format>
#include <cstdlib>
#include <iostream>
#include <regex>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>
#include <fstream>

#if defined(_WIN32)
#include <windows.h>
#include <io.h>
#include <process.h>
#else
#include <unistd.h>
#endif

namespace kano::git::commands {
namespace {

struct SyncPlan {
    std::filesystem::path path;
    std::string type;
    std::string remote;
    std::string targetBranch;
    std::string branchSource;
};

enum class BranchMode {
    Default,
    StableDev,
};

struct GitmodulesBinding {
    std::filesystem::path root;
    std::string prefix;
};

struct WorkingTreeFileSnapshot {
    std::string relativePath;
    std::string contents;
};

enum class StableDevReportFormat {
    Compact,
    Table,
    Tsv,
    Json,
    Markdown,
};

struct StableDevSummaryRow {
    std::string repo;
    std::string result;
    std::string currentBranch;
    bool sameCommit{false};
    std::string latestUpstreamCommit;
    std::string latestStableCommit;
    std::string reason;
};

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

auto GitCapture(const std::filesystem::path& InRepo, const std::vector<std::string>& InArgs) -> shell::ExecResult {
    std::vector<std::string> args;
    args.reserve(InArgs.size() + 4);
    args.insert(args.end(), {"-c", "submodule.recurse=false", "-c", "checkout.recurseSubmodules=false"});
    args.insert(args.end(), InArgs.begin(), InArgs.end());
    return shell::ExecuteCommand("git", args, shell::ExecMode::Capture, InRepo);
}

auto GitPassThrough(const std::filesystem::path& InRepo, const std::vector<std::string>& InArgs) -> shell::ExecResult {
    std::vector<std::string> args;
    args.reserve(InArgs.size() + 4);
    args.insert(args.end(), {"-c", "submodule.recurse=false", "-c", "checkout.recurseSubmodules=false"});
    args.insert(args.end(), InArgs.begin(), InArgs.end());
    return shell::ExecuteCommand("git", args, shell::ExecMode::PassThrough, InRepo);
}

auto IsGitRepo(const std::filesystem::path& InRepo) -> bool {
    return GitCapture(InRepo, {"rev-parse", "--is-inside-work-tree"}).exitCode == 0;
}

auto LooksLikeSelfRepoRoot(const std::filesystem::path& InRepo) -> bool {
    return std::filesystem::exists((InRepo / "src/cpp/scripts").lexically_normal()) &&
           std::filesystem::exists((InRepo / "scripts/kano-git").lexically_normal());
}

auto ResolveSelfRepoRoot(const std::filesystem::path& InSyncRoot) -> std::optional<std::filesystem::path> {
    if (const char* rootEnv = std::getenv("KANO_GIT_MASTER_ROOT"); rootEnv != nullptr && std::string(rootEnv).size() > 0) {
        const auto envPath = std::filesystem::weakly_canonical(std::filesystem::path(rootEnv));
        if (IsGitRepo(envPath) && LooksLikeSelfRepoRoot(envPath)) {
            return envPath;
        }
    }

    const auto syncRoot = std::filesystem::weakly_canonical(InSyncRoot);
    if (IsGitRepo(syncRoot) && LooksLikeSelfRepoRoot(syncRoot)) {
        return syncRoot;
    }

    return std::nullopt;
}

auto CurrentHeadCommit(const std::filesystem::path& InRepo) -> std::string {
    const auto result = GitCapture(InRepo, {"rev-parse", "HEAD"});
    if (result.exitCode != 0) {
        return {};
    }
    return Trim(result.stdoutStr);
}

auto HeadRangeTouchesSelfCpp(const std::filesystem::path& InRepo,
                             const std::string& InBeforeHead,
                             const std::string& InAfterHead) -> bool {
    const auto beforeHead = Trim(InBeforeHead);
    const auto afterHead = Trim(InAfterHead);
    if (beforeHead.empty() || afterHead.empty() || beforeHead == afterHead) {
        return false;
    }

    const auto diff = GitCapture(InRepo, {"diff", "--name-only", beforeHead, afterHead, "--"});
    if (diff.exitCode != 0) {
        return false;
    }

    std::istringstream iss(diff.stdoutStr);
    std::string line;
    while (std::getline(iss, line)) {
        const auto path = Trim(line);
        if (path.rfind("src/cpp/", 0) == 0) {
            return true;
        }
    }
    return false;
}

auto ResolveSelfBuildScript(const std::filesystem::path& InSelfRepoRoot) -> std::filesystem::path {
    const auto base = (InSelfRepoRoot / "src/cpp/scripts").lexically_normal();
#if defined(_WIN32)
#if defined(_M_ARM64)
    return (base / "windows/build_windows_ninja_msvc_arm64_release.sh").lexically_normal();
#else
    return (base / "windows/build_windows_ninja_msvc_release.sh").lexically_normal();
#endif
#elif defined(__APPLE__)
#if defined(__aarch64__) || defined(__arm64__) || defined(_M_ARM64)
    return (base / "macos/build_macos_ninja_clang_arm64_release.sh").lexically_normal();
#else
    return (base / "macos/build_macos_ninja_clang_x64_release.sh").lexically_normal();
#endif
#else
    return (base / "linux/build_linux_ninja_gcc_release.sh").lexically_normal();
#endif
}

auto RunSelfCppBuild(const std::filesystem::path& InSelfRepoRoot) -> int {
    const auto buildScript = ResolveSelfBuildScript(InSelfRepoRoot);
    if (!std::filesystem::exists(buildScript)) {
        std::cerr << "ERROR: self C++ build script not found: " << buildScript.generic_string() << "\n";
        return 1;
    }

    std::cout << "[sync] self repo C++ changes detected; running build: " << buildScript.generic_string() << "\n";
    const auto run = shell::ExecuteCommand(
        "bash",
        {buildScript.generic_string()},
        shell::ExecMode::PassThrough,
        InSelfRepoRoot);
    if (run.exitCode != 0) {
        std::cerr << "ERROR: self C++ build failed after sync\n";
    }
    return run.exitCode;
}

auto TruncateText(const std::string& InText, std::size_t InMax) -> std::string {
    if (InText.size() <= InMax) {
        return InText;
    }
    if (InMax <= 3) {
        return InText.substr(0, InMax);
    }
    return InText.substr(0, InMax - 3) + "...";
}

auto TrimTrailingWindowsPathChars(std::string InValue) -> std::string {
    while (!InValue.empty() && (InValue.back() == ' ' || InValue.back() == '.')) {
        InValue.pop_back();
    }
    return InValue;
}

auto ToLower(std::string InValue) -> std::string {
    std::transform(InValue.begin(), InValue.end(), InValue.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return InValue;
}

auto IsWindowsReservedDeviceComponent(std::string InComponent) -> bool {
#if defined(_WIN32)
    InComponent = TrimTrailingWindowsPathChars(ToLower(Trim(std::move(InComponent))));
    if (InComponent.empty() || InComponent == "." || InComponent == "..") {
        return false;
    }

    const auto colon = InComponent.find(':');
    if (colon != std::string::npos) {
        InComponent = InComponent.substr(0, colon);
    }

    const auto dot = InComponent.find('.');
    const auto stem = dot == std::string::npos ? InComponent : InComponent.substr(0, dot);
    static const std::unordered_set<std::string> reserved = {
        "con", "prn", "aux", "nul",
        "com1", "com2", "com3", "com4", "com5", "com6", "com7", "com8", "com9",
        "lpt1", "lpt2", "lpt3", "lpt4", "lpt5", "lpt6", "lpt7", "lpt8", "lpt9"
    };
    return reserved.contains(stem);
#else
    (void)InComponent;
    return false;
#endif
}

auto PathHasWindowsReservedDeviceComponent(const std::string& InPath) -> bool {
#if defined(_WIN32)
    if (InPath.empty()) {
        return false;
    }

    std::string normalized = InPath;
    for (auto& ch : normalized) {
        if (ch == '\\') {
            ch = '/';
        }
    }

    std::size_t start = 0;
    while (start <= normalized.size()) {
        const auto end = normalized.find('/', start);
        auto component = end == std::string::npos ? normalized.substr(start) : normalized.substr(start, end - start);
        if (IsWindowsReservedDeviceComponent(component)) {
            return true;
        }
        if (end == std::string::npos) {
            break;
        }
        start = end + 1;
    }

    return false;
#else
    (void)InPath;
    return false;
#endif
}

auto ParseStatusPaths(const std::string& InStatusText) -> std::vector<std::string> {
    std::vector<std::string> paths;
    std::istringstream iss(InStatusText);
    std::string line;
    while (std::getline(iss, line)) {
        if (line.size() < 4) {
            continue;
        }
        auto path = line.substr(3);
        const auto renameMarker = path.find(" -> ");
        if (renameMarker != std::string::npos) {
            path = path.substr(renameMarker + 4);
        }
        path = Trim(path);
        if (!path.empty()) {
            paths.push_back(std::move(path));
        }
    }
    return paths;
}

auto CollectWindowsReservedStatusPaths(const std::string& InStatusText) -> std::vector<std::string> {
    std::vector<std::string> reserved;
#if defined(_WIN32)
    if (const char* overrideValue = std::getenv("KOG_SYNC_TEST_RESERVED_STATUS_PATHS"); overrideValue != nullptr) {
        std::istringstream overrideStream(overrideValue);
        std::string line;
        std::set<std::string> seen;
        while (std::getline(overrideStream, line, ';')) {
            line = Trim(line);
            if (line.empty()) {
                continue;
            }
            if (!seen.insert(line).second) {
                continue;
            }
            reserved.push_back(std::move(line));
        }
        return reserved;
    }
    std::set<std::string> seen;
    for (const auto& path : ParseStatusPaths(InStatusText)) {
        if (!PathHasWindowsReservedDeviceComponent(path)) {
            continue;
        }
        if (!seen.insert(path).second) {
            continue;
        }
        reserved.push_back(path);
    }
#else
    (void)InStatusText;
#endif
    return reserved;
}

auto BuildSyncStashArgs(const std::vector<std::string>& InExcluded) -> std::vector<std::string> {
    std::vector<std::string> args{"stash", "push", "-u", "-m", "kano-native-sync-autostash"};
#if defined(_WIN32)
    if (!InExcluded.empty()) {
        args.push_back("--");
        args.push_back(".");
        for (const auto& path : InExcluded) {
            args.push_back(std::string(":(exclude)") + path);
        }
    }
#else
    (void)InExcluded;
#endif
    return args;
}

auto MaybeWarnAboutReservedSyncPaths(const std::filesystem::path& InRepo,
                                     const std::vector<std::string>& InExcluded) -> void {
    if (InExcluded.empty()) {
        return;
    }

    std::ostringstream stream;
    for (std::size_t index = 0; index < InExcluded.size(); ++index) {
        if (index > 0) {
            stream << ", ";
        }
        stream << InExcluded[index];
    }
    std::cerr << "[kog sync] warning: skipped Windows reserved path(s) in "
              << InRepo.generic_string()
              << ": "
              << stream.str()
              << '\n';
}

auto HasRemote(const std::filesystem::path& InRepo, const std::string& InRemote) -> bool {
    if (InRemote.empty()) {
        return false;
    }
    const auto result = GitCapture(InRepo, {"remote", "get-url", InRemote});
    return result.exitCode == 0;
}

auto ResolveRemote(const std::filesystem::path& InRepo, const std::string& InPreferredRemote) -> std::string {
    if (!InPreferredRemote.empty() && HasRemote(InRepo, InPreferredRemote)) {
        return InPreferredRemote;
    }
    if (HasRemote(InRepo, "origin")) {
        return "origin";
    }
    if (HasRemote(InRepo, "upstream")) {
        return "upstream";
    }
    const auto remotes = GitCapture(InRepo, {"remote"});
    if (remotes.exitCode != 0) {
        return {};
    }
    std::istringstream iss(remotes.stdoutStr);
    std::string line;
    if (std::getline(iss, line)) {
        return Trim(line);
    }
    return {};
}

auto DetectRemoteDefaultBranch(const std::filesystem::path& InRepo, const std::string& InRemote) -> std::string {
    const auto remoteHead = GitCapture(InRepo, {"symbolic-ref", "--quiet", std::format("refs/remotes/{}/HEAD", InRemote)});
    if (remoteHead.exitCode == 0) {
        const auto ref = Trim(remoteHead.stdoutStr);
        const auto marker = std::format("refs/remotes/{}/", InRemote);
        if (ref.starts_with(marker) && ref.size() > marker.size()) {
            return ref.substr(marker.size());
        }
    }

    for (const std::string branch : {"main", "master", "dev", "develop", "trunk"}) {
        const auto exists = GitCapture(InRepo, {"show-ref", "--verify", "--quiet", std::format("refs/remotes/{}/{}", InRemote, branch)});
        if (exists.exitCode == 0) {
            return branch;
        }
    }
    return {};
}

auto CurrentBranch(const std::filesystem::path& InRepo) -> std::string {
    const auto result = GitCapture(InRepo, {"symbolic-ref", "--quiet", "--short", "HEAD"});
    if (result.exitCode != 0) {
        return {};
    }
    return Trim(result.stdoutStr);
}

auto HasRebaseInProgress(const std::filesystem::path& InRepo) -> bool {
    const auto rebaseMergePath = GitCapture(InRepo, {"rev-parse", "--git-path", "rebase-merge"});
    if (rebaseMergePath.exitCode == 0) {
        const auto path = Trim(rebaseMergePath.stdoutStr);
        if (!path.empty() && std::filesystem::exists(path)) {
            return true;
        }
    }

    const auto rebaseApplyPath = GitCapture(InRepo, {"rev-parse", "--git-path", "rebase-apply"});
    if (rebaseApplyPath.exitCode == 0) {
        const auto path = Trim(rebaseApplyPath.stdoutStr);
        if (!path.empty() && std::filesystem::exists(path)) {
            return true;
        }
    }

    return false;
}

auto RecoverRebaseState(const std::filesystem::path& InRepo, const std::string& InRepoName, bool InDryRun) -> bool {
    if (!HasRebaseInProgress(InRepo)) {
        return true;
    }

    if (InDryRun) {
        std::cout << "[DRY RUN] Detected in-progress rebase in " << InRepoName << "; would run: git rebase --abort\n";
        return true;
    }

    std::cout << "WARN: Detected in-progress rebase in " << InRepoName << "; attempting auto-recovery via rebase --abort\n";
    const auto abortRebase = GitPassThrough(InRepo, {"rebase", "--abort"});
    if (abortRebase.exitCode != 0) {
        std::cerr << "ERROR: Failed to recover rebase state for " << InRepoName << "\n";
        return false;
    }

    std::cout << "Recovered rebase state for " << InRepoName << "\n";
    return true;
}

auto RestoreAutoStashIfNeeded(const std::filesystem::path& InRepo, const std::string& InRepoName, bool InStashCreated) -> bool {
    if (!InStashCreated) {
        return true;
    }

    const auto stashPop = GitPassThrough(InRepo, {"stash", "pop"});
    if (stashPop.exitCode != 0) {
        std::cerr << "WARN: failed to restore auto-stash for " << InRepoName << "\n";
        return false;
    }

    std::cout << "Restored auto-stash for " << InRepoName << "\n";
    return true;
}

struct IndexLockDiagnosis {
    bool lockExists{false};
    std::filesystem::path lockPath;
    bool activeGitProcessDetected{false};
    long long ageSeconds{-1};
};

auto ResolveIndexLockPath(const std::filesystem::path& InRepo) -> std::filesystem::path {
    const auto result = GitCapture(InRepo, {"rev-parse", "--git-path", "index.lock"});
    if (result.exitCode != 0) {
        return {};
    }
    const auto pathText = Trim(result.stdoutStr);
    if (pathText.empty()) {
        return {};
    }
    auto path = std::filesystem::path(pathText);
    if (path.is_relative()) {
        path = std::filesystem::absolute((InRepo / path).lexically_normal());
    }
    return path;
}

auto DetectActiveGitProcesses() -> bool {
    if (const char* overrideValue = std::getenv("KOG_SYNC_TEST_ASSUME_ACTIVE_GIT_PROCESS"); overrideValue != nullptr) {
        const auto normalized = Trim(overrideValue);
        if (normalized == "1" || normalized == "true" || normalized == "TRUE") {
            return true;
        }
        if (normalized == "0" || normalized == "false" || normalized == "FALSE") {
            return false;
        }
    }
    const auto selfPid =
#if defined(_WIN32)
        _getpid();
#else
        getpid();
#endif
#if defined(_WIN32)
    const auto result = shell::ExecuteCommand(
        "powershell",
        {"-NoLogo", "-NoProfile", "-Command",
         std::format("$self={}; $p = Get-Process git -ErrorAction SilentlyContinue | Where-Object {{ $_.Id -ne $self }}; if ($null -eq $p) {{ exit 1 }} else {{ exit 0 }}",
                     selfPid)},
        shell::ExecMode::Capture,
        std::filesystem::current_path());
    return result.exitCode == 0;
#else
    const auto result = shell::ExecuteCommand(
        "sh",
        {"-lc", std::format("self={}; pgrep -f '(^|/)git(\\.exe)?$' | grep -v \"^$self$\" >/dev/null 2>&1", selfPid)},
        shell::ExecMode::Capture,
        std::filesystem::current_path());
    return result.exitCode == 0;
#endif
}

auto DiagnoseIndexLock(const std::filesystem::path& InRepo) -> IndexLockDiagnosis {
    IndexLockDiagnosis out;
    out.lockPath = ResolveIndexLockPath(InRepo);
    if (out.lockPath.empty()) {
        return out;
    }
    std::error_code ec;
    out.lockExists = std::filesystem::exists(out.lockPath, ec) && !ec;
    out.activeGitProcessDetected = DetectActiveGitProcesses();
    if (out.lockExists) {
        const auto writeTime = std::filesystem::last_write_time(out.lockPath, ec);
        if (!ec) {
            const auto now = decltype(writeTime)::clock::now();
            out.ageSeconds =
                std::chrono::duration_cast<std::chrono::seconds>(now - writeTime).count();
            if (out.ageSeconds < 0) {
                out.ageSeconds = 0;
            }
        }
    }
    return out;
}

auto PrintIndexLockDiagnosis(const std::string& InRepoName, const IndexLockDiagnosis& InDiagnosis) -> void {
    if (!InDiagnosis.lockExists) {
        return;
    }
    std::cerr << "ERROR: git index lock detected for " << InRepoName << "\n";
    std::cerr << "  index.lock: " << InDiagnosis.lockPath.generic_string() << "\n";
    if (InDiagnosis.ageSeconds >= 0) {
        std::cerr << "  lock_last_write_age_seconds: " << InDiagnosis.ageSeconds << "\n";
    }
    std::cerr << "  active_git_process: " << (InDiagnosis.activeGitProcessDetected ? "yes" : "no") << "\n";
}

auto TryCleanupStaleIndexLock(const std::string& InRepoName, const IndexLockDiagnosis& InDiagnosis) -> bool {
    if (!InDiagnosis.lockExists || InDiagnosis.lockPath.empty()) {
        return false;
    }
    if (InDiagnosis.activeGitProcessDetected) {
        std::cerr << "Hint: active git process detected; not removing index.lock automatically for " << InRepoName << "\n";
        return false;
    }
    if (InDiagnosis.ageSeconds >= 0 && InDiagnosis.ageSeconds < 2) {
        std::cerr << "Hint: index.lock is too new to treat as stale automatically for " << InRepoName << "\n";
        return false;
    }

    std::error_code ec;
    bool removed = std::filesystem::remove(InDiagnosis.lockPath, ec);
#if defined(_WIN32)
    if ((!removed || ec) && std::filesystem::exists(InDiagnosis.lockPath)) {
        ec.clear();
        const auto nativePath = InDiagnosis.lockPath.native();
        ::SetFileAttributesW(nativePath.c_str(), FILE_ATTRIBUTE_NORMAL);
        removed = ::DeleteFileW(nativePath.c_str()) != 0;
        if (removed) {
            ec.clear();
        }
    }
    if ((!removed || ec) && std::filesystem::exists(InDiagnosis.lockPath)) {
        const auto deleteCommand = std::format("del /f /q \"{}\"", InDiagnosis.lockPath.string());
        const auto deleteResult = shell::ExecuteCommand(
            "cmd",
            {"/C", deleteCommand},
            shell::ExecMode::Capture,
            std::filesystem::current_path());
        if (deleteResult.exitCode == 0 && !std::filesystem::exists(InDiagnosis.lockPath)) {
            ec.clear();
            removed = true;
        }
    }
#endif
    if (!removed || ec) {
        std::cerr << "ERROR: failed to remove stale index.lock for " << InRepoName << ": " << ec.message() << "\n";
        return false;
    }

    std::cout << "Removed stale index.lock for " << InRepoName << ": " << InDiagnosis.lockPath.generic_string() << "\n";
    return true;
}

auto IsIndexLockFailure(const shell::ExecResult& InResult) -> bool {
    const auto merged = InResult.stdoutStr + "\n" + InResult.stderrStr;
    return merged.find("index.lock") != std::string::npos &&
           (merged.find("File exists") != std::string::npos ||
            merged.find("could not write index") != std::string::npos ||
            merged.find("Unable to create") != std::string::npos ||
            merged.find("another git process seems to be running") != std::string::npos ||
            merged.find("Unable to create '") != std::string::npos);
}

auto DescribeIndexLockFailure(const IndexLockDiagnosis& InDiagnosis, const bool InCleanupEnabled) -> std::string {
    if (!InDiagnosis.lockExists) {
        return "auto-stash failed";
    }
    if (InDiagnosis.activeGitProcessDetected) {
        return "auto-stash blocked by index.lock (active git process detected)";
    }
    if (InCleanupEnabled) {
        return "auto-stash blocked by stale index.lock (cleanup failed or lock too new)";
    }
    return "auto-stash blocked by stale index.lock (rerun with --cleanup-stale-locks)";
}

auto LocalBranchExists(const std::filesystem::path& InRepo, const std::string& InBranch) -> bool {
    if (InBranch.empty()) {
        return false;
    }
    return GitCapture(InRepo, {"show-ref", "--verify", "--quiet", std::format("refs/heads/{}", InBranch)}).exitCode == 0;
}

auto RemoteBranchExists(const std::filesystem::path& InRepo, const std::string& InRemote, const std::string& InBranch) -> bool {
    if (InRemote.empty() || InBranch.empty()) {
        return false;
    }
    return GitCapture(InRepo, {"show-ref", "--verify", "--quiet", std::format("refs/remotes/{}/{}", InRemote, InBranch)}).exitCode == 0;
}

auto ParseStableDevReportFormat(const std::string& InValue) -> std::optional<StableDevReportFormat> {
    if (InValue == "compact") {
        return StableDevReportFormat::Compact;
    }
    if (InValue == "table") {
        return StableDevReportFormat::Table;
    }
    if (InValue == "tsv") {
        return StableDevReportFormat::Tsv;
    }
    if (InValue == "json") {
        return StableDevReportFormat::Json;
    }
    if (InValue == "markdown") {
        return StableDevReportFormat::Markdown;
    }
    return std::nullopt;
}

auto ParseBranchMode(const std::string& InValue) -> std::optional<BranchMode> {
    if (InValue == "default") {
        return BranchMode::Default;
    }
    if (InValue == "stable-dev") {
        return BranchMode::StableDev;
    }
    return std::nullopt;
}

auto IsAgentModeEnabled() -> bool {
    const char* value = std::getenv("KANO_AGENT_MODE");
    if (value == nullptr) {
        return false;
    }
    const std::string normalized = Trim(value);
    return normalized == "1" || normalized == "true" || normalized == "TRUE";
}

auto IsInteractiveTerminal() -> bool {
#if defined(_WIN32)
    return _isatty(_fileno(stdin)) != 0 && _isatty(_fileno(stdout)) != 0;
#else
    return isatty(fileno(stdin)) != 0 && isatty(fileno(stdout)) != 0;
#endif
}

auto PromptYesNo(const std::string& InPrompt) -> bool {
    std::cout << InPrompt << " [y/N]: " << std::flush;
    std::string answer;
    if (!std::getline(std::cin, answer)) {
        return false;
    }
    answer = Trim(answer);
    std::transform(answer.begin(), answer.end(), answer.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return answer == "y" || answer == "yes";
}

auto HasLongFlag(const std::vector<std::string>& InArgs, const std::string& InFlag) -> bool {
    for (const auto& arg : InArgs) {
        if (arg == InFlag || arg.starts_with(InFlag + "=")) {
            return true;
        }
    }
    return false;
}

auto JsonEscape(const std::string& InValue) -> std::string {
    std::string out;
    out.reserve(InValue.size() + 16);
    for (const char c : InValue) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out.push_back(c); break;
        }
    }
    return out;
}

auto ResolveLatestStableTag(const std::filesystem::path& InRepo) -> std::string {
    static const std::regex stableTagPattern(R"((release[-_/])?(v)?[0-9]+(\.[0-9]+){1,3}(\+[0-9A-Za-z.-]+)?)", std::regex::icase);
    const auto tags = GitCapture(InRepo, {"tag", "--list", "--sort=-version:refname"});
    if (tags.exitCode != 0) {
        return {};
    }

    std::istringstream iss(tags.stdoutStr);
    std::string line;
    while (std::getline(iss, line)) {
        line = Trim(line);
        if (line.empty()) {
            continue;
        }
        if (std::regex_match(line, stableTagPattern)) {
            return line;
        }
    }
    return {};
}

auto ResolveCommitSha(const std::filesystem::path& InRepo, const std::string& InRev) -> std::string {
    if (InRev.empty()) {
        return {};
    }
    const auto result = GitCapture(InRepo, {"rev-parse", InRev});
    if (result.exitCode != 0) {
        return {};
    }
    return Trim(result.stdoutStr);
}

auto ResolveCommitLine(const std::filesystem::path& InRepo, const std::string& InRev) -> std::string {
    if (InRev.empty()) {
        return "N/A";
    }
    const auto result = GitCapture(InRepo, {"show", "-s", "--format=%h | %cI | %an | %s", InRev});
    if (result.exitCode != 0) {
        return "N/A";
    }
    const auto line = Trim(result.stdoutStr);
    return line.empty() ? "N/A" : line;
}

auto ResolveGitmodulesBranch(const std::filesystem::path& InRoot, const std::string& InRelPath) -> std::string {
    const auto paths = GitCapture(InRoot, {"config", "-f", ".gitmodules", "--get-regexp", "^submodule\\..*\\.path$"});
    if (paths.exitCode != 0) {
        return {};
    }

    std::istringstream iss(paths.stdoutStr);
    std::string line;
    while (std::getline(iss, line)) {
        line = Trim(line);
        if (line.empty()) {
            continue;
        }
        const auto sp = line.find(' ');
        if (sp == std::string::npos || sp + 1 >= line.size()) {
            continue;
        }
        const auto key = line.substr(0, sp);
        const auto value = line.substr(sp + 1);
        if (value != InRelPath || !key.ends_with(".path")) {
            continue;
        }
        const auto prefix = key.substr(0, key.size() - 5);
        const auto branch = GitCapture(InRoot, {"config", "-f", ".gitmodules", "--get", prefix + ".branch"});
        if (branch.exitCode == 0) {
            return Trim(branch.stdoutStr);
        }
        return {};
    }

    return {};
}

auto RelativePathDepth(const std::filesystem::path& InRoot, const std::filesystem::path& InPath) -> std::size_t {
    std::error_code ec;
    const auto rel = std::filesystem::relative(InPath, InRoot, ec);
    if (ec) {
        return static_cast<std::size_t>(std::distance(InPath.begin(), InPath.end()));
    }
    return static_cast<std::size_t>(std::distance(rel.begin(), rel.end()));
}

auto ResolveStableRef(const std::filesystem::path& InRoot, const std::filesystem::path& InRepo, const std::string& InCurrentBranch, const std::string& InRelPath) -> std::string {
    if (InCurrentBranch.starts_with("branch_")) {
        return InCurrentBranch;
    }

    const auto gmBranch = ResolveGitmodulesBranch(InRoot, InRelPath);
    if (!gmBranch.empty()) {
        if (GitCapture(InRepo, {"show-ref", "--verify", "--quiet", "refs/heads/" + gmBranch}).exitCode == 0) {
            return gmBranch;
        }
        if (GitCapture(InRepo, {"show-ref", "--verify", "--quiet", "refs/remotes/origin/" + gmBranch}).exitCode == 0) {
            return "origin/" + gmBranch;
        }
    }

    return InCurrentBranch;
}

auto CollectSrcSubmodulePaths(const std::filesystem::path& InRoot) -> std::vector<std::string> {
    std::vector<std::string> out;
    const auto paths = GitCapture(InRoot, {"config", "-f", ".gitmodules", "--get-regexp", "^submodule\\..*\\.path$"});
    if (paths.exitCode != 0) {
        return out;
    }

    std::istringstream iss(paths.stdoutStr);
    std::string line;
    while (std::getline(iss, line)) {
        line = Trim(line);
        if (line.empty()) {
            continue;
        }
        const auto sp = line.find(' ');
        if (sp == std::string::npos || sp + 1 >= line.size()) {
            continue;
        }
        const auto rel = line.substr(sp + 1);
        if (rel.starts_with("src/")) {
            out.push_back(rel);
        }
    }
    return out;
}

auto TryResolveTagRefForBranch(const std::filesystem::path& InRepo, const std::string& InBranch, std::string* OutTagRef) -> bool {
    if (!InBranch.starts_with("branch_") || InBranch.size() <= 7) {
        return false;
    }

    const auto tag = InBranch.substr(7);
    if (tag.empty()) {
        return false;
    }

    if (GitCapture(InRepo, {"show-ref", "--verify", "--quiet", std::format("refs/tags/{}", tag)}).exitCode != 0) {
        return false;
    }

    if (OutTagRef != nullptr) {
        *OutTagRef = std::format("refs/tags/{}", tag);
    }
    return true;
}

void PrintStableDevSummary(const std::vector<StableDevSummaryRow>& InRows, StableDevReportFormat InFormat) {
    switch (InFormat) {
        case StableDevReportFormat::Compact: {
            for (const auto& row : InRows) {
                std::cout << "[" << row.repo << "] RESULT=" << row.result << " | branch=" << row.currentBranch << "\n";
                if (!row.reason.empty()) {
                    std::cout << "  reason: " << row.reason << "\n";
                }
                if (row.sameCommit) {
                    std::cout << "  commit: " << row.latestUpstreamCommit << "\n";
                } else {
                    std::cout << "  upstream: " << row.latestUpstreamCommit << "\n";
                    std::cout << "  stable:   " << row.latestStableCommit << "\n";
                }
            }
            break;
        }
        case StableDevReportFormat::Table: {
            const std::string sep = "+------------------------------+---------+------------------------+------------------------------------------------------------+------------------------------------------------------------+------------------------------+";
            std::cout << sep << "\n";
            std::cout << std::format("| {:<28} | {:<7} | {:<22} | {:<58} | {:<58} | {:<28} |\n",
                "Repo", "Result", "Current Branch", "Latest Upstream Commit (sha|time|author|title)", "Latest Stable Commit (sha|time|author|title)", "Reason");
            std::cout << sep << "\n";
            for (const auto& row : InRows) {
                std::cout << std::format("| {:<28} | {:<7} | {:<22} | {:<58} | {:<58} | {:<28} |\n",
                    TruncateText(row.repo, 28),
                    TruncateText(row.result, 7),
                    TruncateText(row.currentBranch, 22),
                    TruncateText(row.latestUpstreamCommit, 58),
                    TruncateText(row.latestStableCommit, 58),
                    TruncateText(row.reason, 28));
            }
            std::cout << sep << "\n";
            break;
        }
        case StableDevReportFormat::Tsv: {
            std::cout << "repo\tresult\tcurrent_branch\tsame_commit\tlatest_upstream_commit\tlatest_stable_commit\treason\n";
            for (const auto& row : InRows) {
                std::cout << row.repo << "\t" << row.result << "\t" << row.currentBranch << "\t"
                          << (row.sameCommit ? "1" : "0") << "\t" << row.latestUpstreamCommit << "\t"
                          << row.latestStableCommit << "\t" << row.reason << "\n";
            }
            break;
        }
        case StableDevReportFormat::Json: {
            std::cout << "[\n";
            for (std::size_t i = 0; i < InRows.size(); ++i) {
                const auto& row = InRows[i];
                std::cout << std::format(
                    "  {{\"repo\":\"{}\",\"result\":\"{}\",\"current_branch\":\"{}\",\"same_commit\":{},\"latest_upstream_commit\":\"{}\",\"latest_stable_commit\":\"{}\",\"reason\":\"{}\"}}{}\n",
                    JsonEscape(row.repo),
                    JsonEscape(row.result),
                    JsonEscape(row.currentBranch),
                    row.sameCommit ? "true" : "false",
                    JsonEscape(row.latestUpstreamCommit),
                    JsonEscape(row.latestStableCommit),
                    JsonEscape(row.reason),
                    (i + 1 < InRows.size()) ? "," : "");
            }
            std::cout << "]\n";
            break;
        }
        case StableDevReportFormat::Markdown: {
            std::cout << "| Repo | Result | Current Branch | Latest Upstream Commit | Latest Stable Commit | Reason |\n";
            std::cout << "| --- | --- | --- | --- | --- | --- |\n";
            for (const auto& row : InRows) {
                auto esc = [](std::string value) {
                    std::size_t pos = 0;
                    while ((pos = value.find('|', pos)) != std::string::npos) {
                        value.replace(pos, 1, "\\|");
                        pos += 2;
                    }
                    return value;
                };
                std::cout << "| " << row.repo << " | " << row.result << " | " << row.currentBranch << " | "
                          << esc(row.latestUpstreamCommit) << " | " << esc(row.latestStableCommit) << " | " << esc(row.reason) << " |\n";
            }
            break;
        }
    }
}

auto RunNativeStableDevSync(
    const std::filesystem::path& InRoot,
    const std::filesystem::path& InRepo,
    const std::string& InRel,
    bool InDryRun,
    StableDevSummaryRow& OutRow) -> int {
    OutRow.repo = InRel;
    OutRow.currentBranch = CurrentBranch(InRepo);

    if (OutRow.currentBranch.empty()) {
        OutRow.result = "FAILED";
        OutRow.sameCommit = false;
        OutRow.latestUpstreamCommit = "N/A";
        OutRow.latestStableCommit = "N/A";
        OutRow.reason = "detached HEAD";
        return 1;
    }

    if (InDryRun) {
        std::cout << "[DRY RUN] " << InRel << ": git fetch upstream --tags --prune\n";
    } else {
        const auto fetch = GitPassThrough(InRepo, {"fetch", "upstream", "--tags", "--prune"});
        if (fetch.exitCode != 0) {
            OutRow.result = "FAILED";
            OutRow.sameCommit = false;
            OutRow.latestUpstreamCommit = "N/A";
            OutRow.latestStableCommit = "N/A";
            OutRow.reason = "fetch upstream --tags failed";
            return fetch.exitCode;
        }
    }

    const auto latestTag = ResolveLatestStableTag(InRepo);
    if (latestTag.empty()) {
        OutRow.result = "FAILED";
        OutRow.sameCommit = false;
        OutRow.latestUpstreamCommit = "N/A";
        OutRow.latestStableCommit = "N/A";
        OutRow.reason = "no stable tag found";
        return 1;
    }

    const auto sourceBranch = OutRow.currentBranch;
    const auto targetBranch = std::string{"branch_"} + latestTag;

    const auto upstreamSha = ResolveCommitSha(InRepo, std::format("refs/tags/{}^{{commit}}", latestTag));
    const auto stableRef = ResolveStableRef(InRoot, InRepo, sourceBranch, InRel);
    const auto stableShaBefore = ResolveCommitSha(InRepo, stableRef);

    OutRow.latestUpstreamCommit = ResolveCommitLine(InRepo, upstreamSha);
    OutRow.latestStableCommit = ResolveCommitLine(InRepo, stableShaBefore);

    if (upstreamSha.empty()) {
        OutRow.result = "FAILED";
        OutRow.sameCommit = false;
        OutRow.reason = "cannot resolve stable tag commit";
        return 1;
    }

    if (stableShaBefore.empty()) {
        OutRow.result = "FAILED";
        OutRow.sameCommit = false;
        OutRow.reason = "cannot resolve stable branch ref";
        return 1;
    }

    if (upstreamSha == stableShaBefore) {
        OutRow.result = "SUCCESS";
        OutRow.sameCommit = true;
        OutRow.currentBranch = sourceBranch;
        OutRow.latestStableCommit = "(same as upstream)";
        OutRow.reason.clear();
        return 0;
    }

    if (InDryRun) {
        std::cout << "[DRY RUN] " << InRel << ": git rebase refs/tags/" << latestTag << "\n";
        if (sourceBranch != targetBranch) {
            std::cout << "[DRY RUN] " << InRel << ": git checkout -B " << targetBranch << " " << sourceBranch << "\n";
        }
        OutRow.result = "SUCCESS";
        OutRow.sameCommit = false;
        OutRow.currentBranch = (sourceBranch == targetBranch) ? sourceBranch : targetBranch;
        OutRow.reason = "dry-run";
        return 0;
    }

    const auto rebase = GitPassThrough(InRepo, {"rebase", std::format("refs/tags/{}", latestTag)});
    if (rebase.exitCode != 0) {
        OutRow.result = "FAILED";
        OutRow.sameCommit = false;
        OutRow.reason = "rebase onto latest stable tag failed";
        return rebase.exitCode;
    }

    if (sourceBranch != targetBranch) {
        const auto moveToTarget = GitPassThrough(InRepo, {"checkout", "-B", targetBranch, sourceBranch});
        if (moveToTarget.exitCode != 0) {
            OutRow.result = "FAILED";
            OutRow.sameCommit = false;
            OutRow.reason = "failed to move onto target stable branch";
            return moveToTarget.exitCode;
        }
    }

    const auto stableShaAfter = ResolveCommitSha(InRepo, "HEAD");
    OutRow.result = "SUCCESS";
    OutRow.sameCommit = (stableShaAfter == upstreamSha);
    OutRow.currentBranch = (sourceBranch == targetBranch) ? sourceBranch : targetBranch;
    OutRow.latestStableCommit = OutRow.sameCommit
        ? "(same as upstream)"
        : ResolveCommitLine(InRepo, stableShaAfter);
    OutRow.reason.clear();
    return 0;
}

auto RunStableDevWorkspace(
    const std::filesystem::path& InRoot,
    StableDevReportFormat InFormat,
    std::vector<std::string> InForwardArgs) -> int {
    if (HasLongFlag(InForwardArgs, "--repo")) {
        std::cerr << "ERROR: --repo is not allowed with --workspace\n";
        return 1;
    }

    const bool dryRun = HasLongFlag(InForwardArgs, "--dry-run");

    const auto gitmodules = InRoot / ".gitmodules";
    if (!std::filesystem::exists(gitmodules)) {
        std::cerr << "ERROR: .gitmodules not found at project root: " << gitmodules.generic_string() << "\n";
        return 1;
    }

    const auto submodulePaths = CollectSrcSubmodulePaths(InRoot);
    if (submodulePaths.empty()) {
        std::cout << "INFO: No src/* submodules found. Nothing to do.\n";
        return 0;
    }

    int success = 0;
    int skipped = 0;
    int failed = 0;
    std::vector<StableDevSummaryRow> summary;
    summary.reserve(submodulePaths.size());

    for (const auto& rel : submodulePaths) {
        const auto repoPath = InRoot / rel;
        if (GitCapture(repoPath, {"rev-parse", "--is-inside-work-tree"}).exitCode != 0) {
            std::cout << "SKIP: " << rel << " (not initialized git repo)\n";
            skipped += 1;
            summary.push_back({rel, "SKIPPED", "N/A", false, "N/A", "N/A", "not initialized git repo"});
            continue;
        }

        if (!HasRemote(repoPath, "upstream")) {
            std::cout << "SKIP: " << rel << " (no upstream remote)\n";
            skipped += 1;
            summary.push_back({rel, "SKIPPED", CurrentBranch(repoPath), false, "N/A", "N/A", "no upstream remote"});
            continue;
        }

        std::cout << "RUN: " << rel << "\n";
        StableDevSummaryRow row;
        const auto runExitCode = RunNativeStableDevSync(InRoot, repoPath, rel, dryRun, row);

        if (runExitCode == 0) {
            success += 1;
            summary.push_back(row);
        } else {
            failed += 1;
            std::cerr << "FAIL: " << rel << " (" << row.reason << ")\n";
            summary.push_back(row);
        }
    }

    std::cout << "=== upstream-stable-dev wrapper summary ===\n";
    std::cout << "success: " << success << "\n";
    std::cout << "skipped: " << skipped << "\n";
    std::cout << "failed: " << failed << "\n";
    std::cout << "OVERALL RESULT: " << ((failed > 0) ? "FAILED" : "SUCCESS") << "\n";
    std::cout << "=== upstream-stable-dev branch report ===\n";
    PrintStableDevSummary(summary, InFormat);

    return failed > 0 ? 1 : 0;
}

auto DiscoverRegisteredPathsRecursive(const std::filesystem::path& InWorkspaceRoot) -> std::set<std::string> {
    std::set<std::string> out;
    std::vector<std::filesystem::path> queue{InWorkspaceRoot};

    while (!queue.empty()) {
        const auto current = queue.back();
        queue.pop_back();

        const auto gitmodules = current / ".gitmodules";
        if (!std::filesystem::exists(gitmodules)) {
            continue;
        }

        const auto pathsResult = GitCapture(current, {"config", "-f", ".gitmodules", "--get-regexp", "^submodule\\..*\\.path$"});
        if (pathsResult.exitCode != 0) {
            continue;
        }

        std::istringstream iss(pathsResult.stdoutStr);
        std::string line;
        while (std::getline(iss, line)) {
            line = Trim(line);
            if (line.empty()) {
                continue;
            }
            const auto sp = line.find(' ');
            if (sp == std::string::npos || sp + 1 >= line.size()) {
                continue;
            }
            const auto relPath = line.substr(sp + 1);
            const auto full = std::filesystem::weakly_canonical(current / relPath).generic_string();
            if (out.insert(full).second) {
                queue.push_back(full);
            }
        }
    }

    return out;
}

auto GitmodulesBranchForPath(const std::filesystem::path& InWorkspaceRoot, const std::filesystem::path& InRepoPath) -> std::optional<std::string> {
    auto current = InRepoPath.parent_path();
    const auto workspace = std::filesystem::weakly_canonical(InWorkspaceRoot);
    const auto target = std::filesystem::weakly_canonical(InRepoPath);

    while (!current.empty()) {
        const auto candidateGitmodules = current / ".gitmodules";
        if (std::filesystem::exists(candidateGitmodules)) {
            std::error_code ec;
            auto rel = std::filesystem::relative(target, current, ec);
            if (!ec) {
                const auto relPath = rel.generic_string();
                const auto pathResult = GitCapture(current, {"config", "-f", ".gitmodules", "--get-regexp", "^submodule\\..*\\.path$"});
                if (pathResult.exitCode == 0) {
                    std::istringstream iss(pathResult.stdoutStr);
                    std::string line;
                    while (std::getline(iss, line)) {
                        line = Trim(line);
                        if (line.empty()) {
                            continue;
                        }
                        const auto sp = line.find(' ');
                        if (sp == std::string::npos || sp + 1 >= line.size()) {
                            continue;
                        }
                        const auto key = line.substr(0, sp);
                        const auto value = line.substr(sp + 1);
                        if (value != relPath) {
                            continue;
                        }

                        const std::string suffix = ".path";
                        if (!key.ends_with(suffix)) {
                            continue;
                        }
                        const auto prefix = key.substr(0, key.size() - suffix.size());
                        const auto branchResult = GitCapture(current, {"config", "-f", ".gitmodules", "--get", prefix + ".branch"});
                        if (branchResult.exitCode == 0) {
                            const auto branch = Trim(branchResult.stdoutStr);
                            if (!branch.empty()) {
                                return branch;
                            }
                        }
                        return std::nullopt;
                    }
                }
            }
        }

        if (std::filesystem::weakly_canonical(current) == workspace) {
            break;
        }
        const auto next = current.parent_path();
        if (next == current) {
            break;
        }
        current = next;
    }

    return std::nullopt;
}

auto FindGitmodulesBindingForPath(const std::filesystem::path& InWorkspaceRoot, const std::filesystem::path& InRepoPath) -> std::optional<GitmodulesBinding> {
    auto current = InRepoPath.parent_path();
    const auto workspace = std::filesystem::weakly_canonical(InWorkspaceRoot);
    const auto target = std::filesystem::weakly_canonical(InRepoPath);

    while (!current.empty()) {
        const auto candidateGitmodules = current / ".gitmodules";
        if (std::filesystem::exists(candidateGitmodules)) {
            std::error_code ec;
            auto rel = std::filesystem::relative(target, current, ec);
            if (!ec) {
                const auto relPath = rel.generic_string();
                const auto pathResult = GitCapture(current, {"config", "-f", ".gitmodules", "--get-regexp", "^submodule\\..*\\.path$"});
                if (pathResult.exitCode == 0) {
                    std::istringstream iss(pathResult.stdoutStr);
                    std::string line;
                    while (std::getline(iss, line)) {
                        line = Trim(line);
                        if (line.empty()) {
                            continue;
                        }
                        const auto sp = line.find(' ');
                        if (sp == std::string::npos || sp + 1 >= line.size()) {
                            continue;
                        }
                        const auto key = line.substr(0, sp);
                        const auto value = line.substr(sp + 1);
                        if (value != relPath || !key.ends_with(".path")) {
                            continue;
                        }
                        return GitmodulesBinding{
                            .root = std::filesystem::weakly_canonical(current),
                            .prefix = key.substr(0, key.size() - 5),
                        };
                    }
                }
            }
        }

        if (std::filesystem::weakly_canonical(current) == workspace) {
            break;
        }
        const auto next = current.parent_path();
        if (next == current) {
            break;
        }
        current = next;
    }

    return std::nullopt;
}

auto WriteGitmodulesBranch(const GitmodulesBinding& InBinding, const std::string& InBranch, bool InDryRun) -> bool {
    if (InBranch.empty()) {
        return false;
    }

    if (InDryRun) {
        std::cout << "[DRY RUN] Would run: git -C " << InBinding.root.generic_string()
                  << " config -f .gitmodules --replace-all " << InBinding.prefix << ".branch " << InBranch << "\n";
        return true;
    }

    const auto write = GitCapture(InBinding.root, {"config", "-f", ".gitmodules", "--replace-all", InBinding.prefix + ".branch", InBranch});
    return write.exitCode == 0;
}

auto ResolveDetachedTargetBranch(const std::filesystem::path& InRepo, const std::string& InRemote, const BranchMode InMode) -> std::pair<std::string, std::string> {
    if (InMode == BranchMode::StableDev) {
        const auto latestTag = ResolveLatestStableTag(InRepo);
        if (latestTag.empty()) {
            return {{}, "stable-dev mode: no release tag found"};
        }
        return {"branch_" + latestTag, "stable-dev inferred branch from latest tag"};
    }

    const auto remoteDefault = DetectRemoteDefaultBranch(InRepo, InRemote);
    if (remoteDefault.empty()) {
        return {{}, "default mode: cannot resolve remote default branch"};
    }
    return {remoteDefault, "default mode inferred remote default branch"};
}

auto CheckoutRecoveredBranch(
    const std::filesystem::path& InRepo,
    const std::string& InRemote,
    const std::string& InBranch,
    const BranchMode InMode,
    bool InDryRun,
    std::string* OutDetail) -> bool {
    if (InBranch.empty()) {
        return false;
    }

    if (LocalBranchExists(InRepo, InBranch)) {
        if (OutDetail != nullptr) {
            *OutDetail = "checkout existing local branch";
        }
        if (InDryRun) {
            std::cout << "[DRY RUN] Would run: git checkout -q " << InBranch << "\n";
            return true;
        }
        const auto checkout = GitPassThrough(InRepo, {"checkout", "-q", InBranch});
        if (checkout.exitCode == 0) {
            return true;
        }
        const auto forceCheckout = GitPassThrough(InRepo, {"checkout", "-q", "-f", InBranch});
        if (forceCheckout.exitCode == 0) {
            if (OutDetail != nullptr) {
                *OutDetail = "force checkout existing local branch";
            }
            return true;
        }
        return false;
    }

    if (RemoteBranchExists(InRepo, InRemote, InBranch)) {
        if (OutDetail != nullptr) {
            *OutDetail = "create local branch from remote";
        }
        if (InDryRun) {
            std::cout << "[DRY RUN] Would run: git checkout -q -b " << InBranch << " " << InRemote << "/" << InBranch << "\n";
            return true;
        }
        const auto checkout = GitPassThrough(InRepo, {"checkout", "-q", "-b", InBranch, std::format("{}/{}", InRemote, InBranch)});
        if (checkout.exitCode == 0) {
            return true;
        }
        const auto forceCheckout = GitPassThrough(InRepo, {"checkout", "-q", "-B", InBranch, std::format("{}/{}", InRemote, InBranch)});
        if (forceCheckout.exitCode == 0) {
            if (OutDetail != nullptr) {
                *OutDetail = "force recreate local branch from remote";
            }
            return true;
        }
        return false;
    }

    if (InMode == BranchMode::StableDev) {
        const auto latestTag = ResolveLatestStableTag(InRepo);
        if (!latestTag.empty()) {
            if (OutDetail != nullptr) {
                *OutDetail = "create stable-dev branch from latest tag";
            }
            if (InDryRun) {
                std::cout << "[DRY RUN] Would run: git checkout -q -b " << InBranch << " refs/tags/" << latestTag << "\n";
                return true;
            }
            return GitPassThrough(InRepo, {"checkout", "-q", "-b", InBranch, std::format("refs/tags/{}", latestTag)}).exitCode == 0;
        }
    }

    std::string tagRef;
    if (TryResolveTagRefForBranch(InRepo, InBranch, &tagRef)) {
        if (OutDetail != nullptr) {
            *OutDetail = "create local branch from matching tag";
        }
        if (InDryRun) {
            std::cout << "[DRY RUN] Would run: git checkout -q -b " << InBranch << " " << tagRef << "\n";
            return true;
        }
        return GitPassThrough(InRepo, {"checkout", "-q", "-b", InBranch, tagRef}).exitCode == 0;
    }

    return false;
}

auto CollectUnmergedPaths(const std::filesystem::path& InRepo) -> std::vector<std::string> {
    std::vector<std::string> paths;
    const auto unmerged = GitCapture(InRepo, {"ls-files", "-u"});
    if (unmerged.exitCode != 0) {
        return paths;
    }

    std::unordered_set<std::string> seen;
    std::istringstream iss(unmerged.stdoutStr);
    std::string line;
    while (std::getline(iss, line)) {
        line = Trim(line);
        if (line.empty()) {
            continue;
        }
        const auto tab = line.find('\t');
        if (tab == std::string::npos || tab + 1 >= line.size()) {
            continue;
        }
        const auto path = line.substr(tab + 1);
        if (seen.insert(path).second) {
            paths.push_back(path);
        }
    }

    return paths;
}

auto ResolveUnmergedByTheirs(const std::filesystem::path& InRepo, bool InDryRun) -> bool {
    const auto paths = CollectUnmergedPaths(InRepo);
    if (paths.empty()) {
        return false;
    }

    for (const auto& path : paths) {
        if (InDryRun) {
            std::cout << "[DRY RUN] Would run: git checkout --theirs -- " << path << "\n";
            std::cout << "[DRY RUN] Would run: git add -- " << path << "\n";
            continue;
        }

        const auto checkoutTheirs = GitPassThrough(InRepo, {"checkout", "--theirs", "--", path});
        if (checkoutTheirs.exitCode != 0) {
            return false;
        }

        const auto addPath = GitPassThrough(InRepo, {"add", "--", path});
        if (addPath.exitCode != 0) {
            return false;
        }
    }

    if (InDryRun) {
        return true;
    }

    return CollectUnmergedPaths(InRepo).empty();
}

auto BuildSyncPlans(
    const std::filesystem::path& InRoot,
    const std::string& InPreferredRemote,
    int InMaxDepth,
    bool InNoCache,
    bool InRefreshCache) -> std::pair<std::vector<SyncPlan>, std::string> {
    const auto root = std::filesystem::weakly_canonical(InRoot);
    const auto registeredPaths = DiscoverRegisteredPathsRecursive(root);

    std::vector<std::filesystem::path> discoveredPaths;
    discoveredPaths.reserve(registeredPaths.size() + 1);
    discoveredPaths.push_back(root);
    for (const auto& registeredPath : registeredPaths) {
        discoveredPaths.push_back(std::filesystem::path(registeredPath));
    }

    if (!InNoCache) {
        std::string manifestReason;
        if (const auto manifest = workspace::LoadTrustedWorkspaceManifest(root, &manifestReason); manifest.has_value()) {
            for (const auto& repo : manifest->repos) {
                discoveredPaths.push_back(repo.path.lexically_normal());
            }
        }
    }

    std::sort(discoveredPaths.begin(), discoveredPaths.end(), [](const auto& A, const auto& B) {
        return A.generic_string() < B.generic_string();
    });
    discoveredPaths.erase(std::unique(discoveredPaths.begin(), discoveredPaths.end(), [](const auto& A, const auto& B) {
        return A.generic_string() == B.generic_string();
    }), discoveredPaths.end());

    std::vector<SyncPlan> plans;
    plans.reserve(discoveredPaths.size());

    for (const auto& discoveredPath : discoveredPaths) {
        const auto repoPath = std::filesystem::weakly_canonical(discoveredPath);
        const auto remote = ResolveRemote(repoPath, InPreferredRemote);
        if (remote.empty()) {
            std::cerr << "WARN: Skip repo without remotes: " << repoPath.generic_string() << "\n";
            continue;
        }

        const auto current = CurrentBranch(repoPath);
        const bool isRoot = (repoPath == root);
        const bool isRegistered = registeredPaths.contains(repoPath.generic_string());

        std::string branchSource;
        std::string targetBranch;
        if (isRoot) {
            if (!current.empty()) {
                targetBranch = current;
                branchSource = "root current branch";
            } else {
                targetBranch = DetectRemoteDefaultBranch(repoPath, remote);
                branchSource = "root detached -> remote default";
            }
        } else if (isRegistered) {
            const auto configured = GitmodulesBranchForPath(root, repoPath);
            if (configured.has_value() && !configured->empty()) {
                targetBranch = *configured;
                branchSource = "registered .gitmodules branch";
            } else {
                targetBranch = DetectRemoteDefaultBranch(repoPath, remote);
                branchSource = "registered remote default branch";
            }
        } else {
            if (!current.empty()) {
                targetBranch = current;
                branchSource = "unregistered current branch";
            } else {
                targetBranch = DetectRemoteDefaultBranch(repoPath, remote);
                branchSource = "unregistered detached -> remote default";
            }
        }

        if (targetBranch.empty()) {
            std::cerr << "ERROR: Could not determine target branch for repo: " << repoPath.generic_string() << "\n";
            continue;
        }

        plans.push_back(SyncPlan{
            .path = repoPath,
            .type = isRoot ? "root" : (isRegistered ? "registered" : "unregistered"),
            .remote = remote,
            .targetBranch = targetBranch,
            .branchSource = branchSource,
        });
    }

    std::sort(plans.begin(), plans.end(), [&](const SyncPlan& A, const SyncPlan& B) {
        const auto depthA = RelativePathDepth(root, A.path);
        const auto depthB = RelativePathDepth(root, B.path);
        if (depthA != depthB) {
            return depthA < depthB;
        }
        return A.path.generic_string() < B.path.generic_string();
    });

    return {plans, "registered-only-scan"};
}

auto CaptureWorkingTreeFileSnapshots(const std::filesystem::path& InRepo,
                                     const std::string& InStatusText) -> std::vector<WorkingTreeFileSnapshot> {
    std::vector<WorkingTreeFileSnapshot> snapshots;
    std::set<std::string> seen;
    for (const auto& relativePath : ParseStatusPaths(InStatusText)) {
        if (!seen.insert(relativePath).second) {
            continue;
        }
        const auto absolutePath = (InRepo / std::filesystem::path(relativePath)).lexically_normal();
        std::error_code ec;
        if (!std::filesystem::exists(absolutePath, ec) || std::filesystem::is_directory(absolutePath, ec)) {
            continue;
        }
        std::ifstream in(absolutePath, std::ios::binary);
        if (!in) {
            continue;
        }
        std::ostringstream buffer;
        buffer << in.rdbuf();
        snapshots.push_back(WorkingTreeFileSnapshot{
            .relativePath = relativePath,
            .contents = buffer.str(),
        });
    }
    return snapshots;
}

auto RestoreWorkingTreeFileSnapshots(const std::filesystem::path& InRepo,
                                     const std::vector<WorkingTreeFileSnapshot>& InSnapshots,
                                     const std::string& InRepoName) -> bool {
    for (const auto& snapshot : InSnapshots) {
        const auto absolutePath = (InRepo / std::filesystem::path(snapshot.relativePath)).lexically_normal();
        std::error_code ec;
        std::filesystem::create_directories(absolutePath.parent_path(), ec);
        if (ec) {
            std::cerr << "WARN: failed to restore local snapshot parent path for " << InRepoName
                      << ": " << snapshot.relativePath << "\n";
            return false;
        }
        std::ofstream out(absolutePath, std::ios::binary | std::ios::trunc);
        if (!out) {
            std::cerr << "WARN: failed to restore local snapshot for " << InRepoName
                      << ": " << snapshot.relativePath << "\n";
            return false;
        }
        out << snapshot.contents;
        if (!out.good()) {
            std::cerr << "WARN: failed to write restored local snapshot for " << InRepoName
                      << ": " << snapshot.relativePath << "\n";
            return false;
        }
    }
    return true;
}

auto RunNativeOriginLatestSync(
    const std::filesystem::path& InRepoRoot,
    const std::string& InRemote,
    int InMaxDepth,
    bool InDryRun,
    bool InNoCache,
    bool InRefreshCache,
    bool InRecursive,
    bool InAutoStashLocalChanges,
    bool InCleanupStaleLocks) -> int {
    std::vector<SyncPlan> plans;
    std::string mode;
    try {
        auto planResult = BuildSyncPlans(InRepoRoot, InRemote, InMaxDepth, InNoCache, InRefreshCache);
        plans = std::move(planResult.first);
        mode = std::move(planResult.second);
    } catch (const std::exception& ex) {
        if (!InNoCache) {
            std::cerr << "WARN: native discovery failed with cache enabled, retrying without cache: " << ex.what() << "\n";
            try {
                auto planResult = BuildSyncPlans(InRepoRoot, InRemote, InMaxDepth, true, InRefreshCache);
                plans = std::move(planResult.first);
                mode = std::move(planResult.second);
            } catch (const std::exception& exNoCache) {
                std::cerr << "ERROR: native discovery failed without cache: " << exNoCache.what() << "\n";
                return 1;
            }
        } else {
            std::cerr << "ERROR: native discovery failed: " << ex.what() << "\n";
            return 1;
        }
    }

    if (!InRecursive) {
        const auto root = std::filesystem::weakly_canonical(InRepoRoot);
        plans.erase(
            std::remove_if(plans.begin(), plans.end(), [&](const SyncPlan& InPlan) {
                return std::filesystem::weakly_canonical(InPlan.path) != root;
            }),
            plans.end());
    }

    std::cout << (InRecursive
        ? "Syncing workspace repos with recursive branch rules\n"
        : "Syncing current repository only (non-recursive mode)\n");
    std::cout << "Discover mode: " << (mode.empty() ? "unknown" : mode) << "\n";

    const auto root = std::filesystem::weakly_canonical(InRepoRoot);
    const auto selfRepoRoot = ResolveSelfRepoRoot(root);
    int failures = 0;
    int succeeded = 0;
    std::vector<std::pair<std::string, std::string>> failureDetails;
    bool shouldBuildSelfCpp = false;
    for (const auto& plan : plans) {
        const auto rel = std::filesystem::relative(plan.path, InRepoRoot).generic_string();
        const auto name = (rel.empty() || rel == ".") ? "." : rel;
        std::string targetBranch = plan.targetBranch;
        std::string branchSource = plan.branchSource;
        const bool isSelfRepo = selfRepoRoot.has_value() && std::filesystem::weakly_canonical(plan.path) == *selfRepoRoot;
        const auto headBeforeSync = isSelfRepo ? CurrentHeadCommit(plan.path) : std::string{};
        if (plan.type == "registered") {
            const auto refreshed = GitmodulesBranchForPath(root, plan.path);
            if (refreshed.has_value() && !refreshed->empty() && *refreshed != targetBranch) {
                targetBranch = *refreshed;
                branchSource = "registered .gitmodules branch (refreshed)";
            }
        }
        const auto status = GitCapture(plan.path, {"status", "--porcelain"});
        const bool hasLocalChanges = status.exitCode == 0 && !Trim(status.stdoutStr).empty();
        bool stashCreated = false;
        bool performedRebase = false;
        const auto workingTreeSnapshots = status.exitCode == 0
            ? CaptureWorkingTreeFileSnapshots(plan.path, status.stdoutStr)
            : std::vector<WorkingTreeFileSnapshot>{};
        const auto reservedPaths = status.exitCode == 0 ? CollectWindowsReservedStatusPaths(status.stdoutStr)
                                                        : std::vector<std::string>{};
        const auto stashArgs = BuildSyncStashArgs(reservedPaths);

        std::cout << (InDryRun ? "[DRY RUN] " : "") << "Repo: " << name << "\n";
        std::cout << (InDryRun ? "[DRY RUN] " : "") << "Branch source: " << branchSource << "\n";

        if (hasLocalChanges) {
            if (InAutoStashLocalChanges) {
                if (InDryRun) {
                    std::cout << "[DRY RUN] Would run: git stash push -u -m kano-native-sync-autostash";
                    if (!reservedPaths.empty()) {
                        std::cout << " -- .";
                        for (const auto& path : reservedPaths) {
                            std::cout << " :(exclude)" << path;
                        }
                    }
                    std::cout << "\n";
                    stashCreated = true;
                } else {
                    MaybeWarnAboutReservedSyncPaths(plan.path, reservedPaths);
                    auto stash = GitCapture(plan.path, stashArgs);
                    std::optional<IndexLockDiagnosis> indexLockDiagnosis;
                    if (stash.exitCode != 0 && IsIndexLockFailure(stash)) {
                        const auto diagnosis = DiagnoseIndexLock(plan.path);
                        indexLockDiagnosis = diagnosis;
                        PrintIndexLockDiagnosis(name, diagnosis);
                        if (InCleanupStaleLocks) {
                            if (TryCleanupStaleIndexLock(name, diagnosis)) {
                                stash = GitCapture(plan.path, stashArgs);
                            }
                        } else if (diagnosis.lockExists && !diagnosis.activeGitProcessDetected) {
                            std::cerr << "Hint: rerun with --cleanup-stale-locks to remove the stale index.lock automatically.\n";
                        }
                    }
                    if (stash.exitCode != 0 && !indexLockDiagnosis.has_value()) {
                        const auto diagnosis = DiagnoseIndexLock(plan.path);
                        if (diagnosis.lockExists) {
                            indexLockDiagnosis = diagnosis;
                            PrintIndexLockDiagnosis(name, diagnosis);
                            if (InCleanupStaleLocks && !diagnosis.activeGitProcessDetected) {
                                if (TryCleanupStaleIndexLock(name, diagnosis)) {
                                    stash = GitCapture(plan.path, stashArgs);
                                }
                            } else if (!diagnosis.activeGitProcessDetected) {
                                std::cerr << "Hint: rerun with --cleanup-stale-locks to remove the stale index.lock automatically.\n";
                            }
                        }
                    }
                    if (stash.exitCode != 0) {
                        std::cerr << "ERROR: failed to auto-stash local changes for " << name << "\n";
                        failures += 1;
                        if (indexLockDiagnosis.has_value()) {
                            failureDetails.emplace_back(name, DescribeIndexLockFailure(*indexLockDiagnosis, InCleanupStaleLocks));
                        } else {
                            failureDetails.emplace_back(name, "auto-stash failed");
                        }
                        continue;
                    }

                    const auto stashOut = Trim(stash.stdoutStr + "\n" + stash.stderrStr);
                    stashCreated = stashOut.find("No local changes to save") == std::string::npos;
                    if (stashCreated) {
                        std::cout << "Auto-stashed local changes for " << name << "\n";
                    }
                }
            } else {
                std::cerr << "ERROR: local changes detected for " << name << " (auto-stash disabled)\n";
                failures += 1;
                failureDetails.emplace_back(name, "local changes present and auto-stash disabled");
                continue;
            }
        }

        const auto fetch = GitCapture(plan.path, {"fetch", plan.remote, "--prune", "--tags"});
        if (!InDryRun && fetch.exitCode != 0) {
            std::cerr << "WARN: fetch failed for " << name << "\n";
        }

        const auto hasLocal = GitCapture(plan.path, {"show-ref", "--verify", "--quiet", std::format("refs/heads/{}", targetBranch)}).exitCode == 0;
        const auto hasRemote = GitCapture(plan.path, {"show-ref", "--verify", "--quiet", std::format("refs/remotes/{}/{}", plan.remote, targetBranch)}).exitCode == 0;

        std::vector<std::string> checkoutArgs;
        if (hasLocal) {
            checkoutArgs = {"checkout", "-q", targetBranch};
        } else if (hasRemote) {
            checkoutArgs = {"checkout", "-q", "-B", targetBranch, std::format("{}/{}", plan.remote, targetBranch)};
        } else {
            if (plan.type == "unregistered") {
                checkoutArgs = {"checkout", "-q", targetBranch};
                std::cout << "WARN: Unregistered repo branch has no remote ref, keeping local branch: " << name << "\n";
            } else {
                std::string tagRef;
                if (TryResolveTagRefForBranch(plan.path, targetBranch, &tagRef)) {
                    checkoutArgs = {"checkout", "-q", "-B", targetBranch, tagRef};
                    std::cout << "INFO: Target branch missing for " << name << "; creating from tag " << tagRef << "\n";
                } else {
                    std::cerr << "ERROR: Target branch not found for " << name << "\n";
                    failures += 1;
                    failureDetails.emplace_back(name, "target branch and matching tag not found");
                    if (!RestoreAutoStashIfNeeded(plan.path, name, stashCreated)) {
                        failureDetails.emplace_back(name, "stash pop failed after target-branch lookup failure");
                    } else if (stashCreated && !workingTreeSnapshots.empty()) {
                        (void)RestoreWorkingTreeFileSnapshots(plan.path, workingTreeSnapshots, name);
                    }
                    continue;
                }
            }
        }

        if (InDryRun) {
            std::cout << "[DRY RUN] Would run: git";
            for (const auto& arg : checkoutArgs) {
                std::cout << " " << arg;
            }
            std::cout << "\n";
            if (hasRemote) {
                const auto rebaseTarget = std::format("{}/{}", plan.remote, targetBranch);
                const auto aheadBehind = GitCapture(plan.path, {"rev-list", "--left-right", "--count", std::format("HEAD...{}", rebaseTarget)});
                bool shouldRebase = true;
                if (aheadBehind.exitCode == 0) {
                    std::istringstream iss(aheadBehind.stdoutStr);
                    int aheadCount = 0;
                    int behindCount = 0;
                    if (iss >> aheadCount >> behindCount) {
                        shouldRebase = behindCount > 0;
                        if (!shouldRebase) {
                            std::cout << "[DRY RUN] Skip rebase: local branch is not behind " << rebaseTarget << " (ahead=" << aheadCount << ", behind=" << behindCount << ")\n";
                        }
                    }
                }
                if (shouldRebase) {
                    std::cout << "[DRY RUN] Would run: git rebase " << rebaseTarget << "\n";
                }
            } else {
                std::cout << "[DRY RUN] Skip pull: missing remote branch " << plan.remote << "/" << plan.targetBranch << "\n";
            }
            if (stashCreated) {
                std::cout << "[DRY RUN] Would run: git stash pop\n";
            }
            continue;
        }

        const auto checkout = GitPassThrough(plan.path, checkoutArgs);
        if (checkout.exitCode != 0) {
            std::cerr << "ERROR: checkout failed for " << name << "\n";
            failures += 1;
            failureDetails.emplace_back(name, "checkout failed");
            if (!RestoreAutoStashIfNeeded(plan.path, name, stashCreated)) {
                failureDetails.emplace_back(name, "stash pop failed after checkout failure");
            } else if (stashCreated && !workingTreeSnapshots.empty()) {
                (void)RestoreWorkingTreeFileSnapshots(plan.path, workingTreeSnapshots, name);
            }
            continue;
        }

        if (hasRemote) {
            if (!RecoverRebaseState(plan.path, name, InDryRun)) {
                failures += 1;
                failureDetails.emplace_back(name, "rebase state recovery failed");
                continue;
            }

            const auto rebaseTarget = std::format("{}/{}", plan.remote, targetBranch);
            bool shouldRebase = true;
            const auto aheadBehind = GitCapture(plan.path, {"rev-list", "--left-right", "--count", std::format("HEAD...{}", rebaseTarget)});
            if (aheadBehind.exitCode == 0) {
                std::istringstream iss(aheadBehind.stdoutStr);
                int aheadCount = 0;
                int behindCount = 0;
                if (iss >> aheadCount >> behindCount) {
                    shouldRebase = behindCount > 0;
                    if (!shouldRebase) {
                        std::cout << "Skip rebase for " << name << ": local branch is not behind " << rebaseTarget
                                  << " (ahead=" << aheadCount << ", behind=" << behindCount << ")\n";
                    }
                }
            }

            if (shouldRebase) {
                const auto rebase = GitPassThrough(plan.path, {"rebase", rebaseTarget});
                if (rebase.exitCode != 0) {
                    if (HasRebaseInProgress(plan.path)) {
                        std::cerr << "ERROR: rebase conflict detected for " << name
                                  << "; stopping sync and aborting rebase for manual review\n";
                        const auto abortRebase = GitPassThrough(plan.path, {"rebase", "--abort"});
                        if (abortRebase.exitCode != 0) {
                            std::cerr << "WARN: failed to abort rebase after conflict for " << name << "\n";
                        }
                        failures += 1;
                        failureDetails.emplace_back(name, "rebase conflict");
                        if (!RestoreAutoStashIfNeeded(plan.path, name, stashCreated)) {
                            failureDetails.emplace_back(name, "stash pop failed after rebase conflict");
                        } else if (stashCreated && !workingTreeSnapshots.empty()) {
                            (void)RestoreWorkingTreeFileSnapshots(plan.path, workingTreeSnapshots, name);
                        }
                        continue;
                    }
                    std::cerr << "ERROR: rebase failed for " << name << "\n";
                    failures += 1;
                    failureDetails.emplace_back(name, "rebase failed");
                    if (!RestoreAutoStashIfNeeded(plan.path, name, stashCreated)) {
                        failureDetails.emplace_back(name, "stash pop failed after rebase failure");
                    } else if (stashCreated && !workingTreeSnapshots.empty()) {
                        (void)RestoreWorkingTreeFileSnapshots(plan.path, workingTreeSnapshots, name);
                    }
                    continue;
                }
                performedRebase = true;
            }
        }

        if (!RestoreAutoStashIfNeeded(plan.path, name, stashCreated)) {
            failures += 1;
            failureDetails.emplace_back(name, "stash pop failed after sync");
            continue;
        }
        if (stashCreated && !performedRebase && !workingTreeSnapshots.empty()) {
            (void)RestoreWorkingTreeFileSnapshots(plan.path, workingTreeSnapshots, name);
        }

        succeeded += 1;
        if (isSelfRepo) {
            const auto headAfterSync = CurrentHeadCommit(plan.path);
            if (HeadRangeTouchesSelfCpp(plan.path, headBeforeSync, headAfterSync)) {
                shouldBuildSelfCpp = true;
            }
        }
    }

    if (failures == 0 && shouldBuildSelfCpp && selfRepoRoot.has_value() && !InDryRun) {
        const auto buildCode = RunSelfCppBuild(*selfRepoRoot);
        if (buildCode != 0) {
            failures += 1;
            failureDetails.emplace_back(
                std::filesystem::relative(*selfRepoRoot, InRepoRoot).generic_string().empty()
                    ? std::string{"."}
                    : std::filesystem::relative(*selfRepoRoot, InRepoRoot).generic_string(),
                "self C++ build failed after sync");
        }
    }

    std::cout << "=== Sync Complete ===\n";
    std::cout << "Succeeded: " << succeeded << "\n";
    std::cout << "Failed: " << failures << "\n";
    if (!failureDetails.empty()) {
        std::cout << "\n=== FAILED REPOS ===\n";
        for (const auto& [repo, reason] : failureDetails) {
            std::cout << "[ERROR] " << repo << " | " << reason << "\n";
        }
    }
    return failures > 0 ? 1 : 0;
}

auto RunNativePreCommitRepair(
    const std::filesystem::path& InRepoRoot,
    const std::string& InRemote,
    int InMaxDepth,
    bool InDryRun,
    bool InNoCache,
    bool InRefreshCache,
    bool InRecursive,
    BranchMode InBranchMode) -> int {
    workspace::DiscoverOptions options;
    options.rootDir = InRepoRoot;
    options.maxDepth = InMaxDepth;
    options.useCache = !InNoCache;
    options.refreshCache = InRefreshCache;
    options.metadataLevel = "full";

    const auto discovery = workspace::DiscoverRepos(options);
    const auto root = std::filesystem::weakly_canonical(InRepoRoot);
    const auto registeredPaths = DiscoverRegisteredPathsRecursive(root);

    int failures = 0;

    std::cout << (InRecursive
        ? "Pre-commit detached-HEAD repair for workspace repos\n"
        : "Pre-commit detached-HEAD repair for current repository only\n");

    for (const auto& repo : discovery.repos) {
        const auto repoPath = std::filesystem::weakly_canonical(repo.path);
        if (!InRecursive && repoPath != root) {
            continue;
        }

        const auto rel = std::filesystem::relative(repoPath, InRepoRoot).generic_string();
        const auto name = (rel.empty() || rel == ".") ? "." : rel;

        const auto remote = ResolveRemote(repoPath, InRemote);
        if (remote.empty()) {
            std::cerr << "WARN: Skip repo without remotes: " << name << "\n";
            continue;
        }

        const auto current = CurrentBranch(repoPath);
        if (!current.empty()) {
            std::cout << (InDryRun ? "[DRY RUN] " : "") << "Repo: " << name << " already on branch " << current << "\n";
            continue;
        }

        const bool isRegistered = registeredPaths.contains(repoPath.generic_string());
        std::string branchSource;
        std::string targetBranch;

        if (isRegistered) {
            const auto configured = GitmodulesBranchForPath(root, repoPath);
            if (configured.has_value() && !configured->empty()) {
                targetBranch = *configured;
                branchSource = "registered .gitmodules branch";
            }
        }

        if (targetBranch.empty()) {
            auto [inferredBranch, inferredSource] = ResolveDetachedTargetBranch(repoPath, remote, InBranchMode);
            targetBranch = std::move(inferredBranch);
            branchSource = std::move(inferredSource);
        }

        if (targetBranch.empty()) {
            std::cerr << "ERROR: " << name << " detached HEAD with no resolvable target branch\n";
            failures += 1;
            continue;
        }

        std::cout << (InDryRun ? "[DRY RUN] " : "") << "Repo: " << name << " | source=" << branchSource << " | target=" << targetBranch << "\n";

        if (!InDryRun) {
            const auto fetch = GitCapture(repoPath, {"fetch", remote, "--prune", "--tags"});
            if (fetch.exitCode != 0) {
                std::cerr << "WARN: fetch failed before detached recovery: " << name << "\n";
            }
        }

        std::string checkoutDetail;
        bool checkedOut = CheckoutRecoveredBranch(
            repoPath,
            remote,
            targetBranch,
            InBranchMode,
            InDryRun,
            &checkoutDetail);

        if (!checkedOut) {
            const auto unmergedPaths = CollectUnmergedPaths(repoPath);
            if (!unmergedPaths.empty()) {
                std::cerr << "WARN: " << name << " has " << unmergedPaths.size()
                          << " unmerged index entr" << (unmergedPaths.size() == 1 ? "y" : "ies")
                          << "; auto-resolving with --theirs and retrying branch recovery\n";
                const auto resolved = ResolveUnmergedByTheirs(repoPath, InDryRun);
                if (resolved) {
                    checkedOut = CheckoutRecoveredBranch(
                        repoPath,
                        remote,
                        targetBranch,
                        InBranchMode,
                        InDryRun,
                        &checkoutDetail);
                }
            }
        }

        if (!checkedOut) {
            std::cerr << "ERROR: failed to recover detached HEAD for repo: " << name << "\n";
            failures += 1;
            continue;
        }

        if (!checkoutDetail.empty()) {
            std::cout << (InDryRun ? "[DRY RUN] " : "") << "Repo: " << name << " | action=" << checkoutDetail << "\n";
        }

        if (isRegistered) {
            const auto binding = FindGitmodulesBindingForPath(root, repoPath);
            if (!binding.has_value()) {
                std::cerr << "WARN: registered repo without resolvable .gitmodules binding: " << name << "\n";
                continue;
            }

            if (!WriteGitmodulesBranch(*binding, targetBranch, InDryRun)) {
                std::cerr << "WARN: failed to write .gitmodules branch for repo: " << name << "\n";
            }
        }
    }

    std::cout << "=== Pre-Commit Repair Complete ===\n";
    return failures > 0 ? 1 : 0;
}

auto RunNativeUpstreamForcePush(
    const std::filesystem::path& InRepo,
    bool InDryRun) -> int {
    const auto repo = std::filesystem::weakly_canonical(InRepo);
    if (GitCapture(repo, {"rev-parse", "--is-inside-work-tree"}).exitCode != 0) {
        std::cerr << "ERROR: Not a git repository: " << repo.generic_string() << "\n";
        return 1;
    }

    if (!HasRemote(repo, "upstream")) {
        std::cerr << "ERROR: Missing upstream remote\n";
        return 1;
    }
    if (!HasRemote(repo, "origin")) {
        std::cerr << "ERROR: Missing origin remote\n";
        return 1;
    }

    const auto branch = CurrentBranch(repo);
    if (branch.empty()) {
        std::cerr << "ERROR: Detached HEAD not supported for upstream-force-push\n";
        return 1;
    }

    auto upstreamDefault = DetectRemoteDefaultBranch(repo, "upstream");
    if (upstreamDefault.empty()) {
        std::cerr << "ERROR: Cannot resolve upstream default branch\n";
        return 1;
    }

    if (InDryRun) {
        std::cout << "[DRY RUN] Would run: git fetch upstream\n";
        std::cout << "[DRY RUN] Would run: git rebase " << upstreamDefault << "\n";
        std::cout << "[DRY RUN] Would run: git push --force-with-lease origin " << branch << "\n";
        return 0;
    }

    const auto fetch = GitPassThrough(repo, {"fetch", "upstream"});
    if (fetch.exitCode != 0) {
        return fetch.exitCode;
    }

    const auto rebase = GitPassThrough(repo, {"rebase", upstreamDefault});
    if (rebase.exitCode != 0) {
        return rebase.exitCode;
    }

    const auto push = GitPassThrough(repo, {"push", "--force-with-lease", "origin", branch});
    return push.exitCode;
}

} // namespace

void RegisterSync(CLI::App& InApp) {
    auto* cmd = InApp.add_subcommand("sync", "Pipeline sync stage (origin-latest by default) plus specialized sync workflows");

    // --- sync pre-commit ---
    auto* pre_commit = cmd->add_subcommand("pre-commit", "Repair detached HEAD before commit workflow");
    pre_commit->allow_extras();
    auto* preCommitRepo = new std::string{"."};
    auto* preCommitRemote = new std::string{"origin"};
    auto* preCommitDryRun = new bool{false};
    auto* preCommitMaxDepth = new int{0};
    auto* preCommitNoCache = new bool{false};
    auto* preCommitRefreshCache = new bool{false};
    auto* preCommitNoRecursive = new bool{false};
    auto* preCommitBranchMode = new std::string{"default"};
    auto* preCommitProfile = new bool{false};

    pre_commit->add_option("--repo", *preCommitRepo, "Target repository root path");
    pre_commit->add_option("--remote", *preCommitRemote, "Preferred remote name");
    pre_commit->add_flag("--dry-run", *preCommitDryRun, "Preview detached-head repair actions");
    pre_commit->add_option("--native-max-depth", *preCommitMaxDepth, "Native discovery max depth (0 = unlimited)");
    pre_commit->add_flag("--native-no-cache", *preCommitNoCache, "Disable native discovery cache");
    pre_commit->add_flag("--native-refresh-cache", *preCommitRefreshCache, "Force native cache refresh");
    pre_commit->add_flag("--no-recursive,-N", *preCommitNoRecursive, "Repair only current repository");
    pre_commit->add_option("--branch-mode", *preCommitBranchMode, "Detached-branch inference mode: default|stable-dev");
    pre_commit->add_flag("--profile", *preCommitProfile, "Print native pre-commit timing/profile summary");

    pre_commit->callback([=]() {
        auto extras = pre_commit->remaining();
        if (!extras.empty()) {
            std::cerr << "Error: unsupported extra arguments in native sync pre-commit mode:";
            for (const auto& extra : extras) {
                std::cerr << ' ' << extra;
            }
            std::cerr << "\n";
            std::exit(2);
        }

        const auto branchMode = ParseBranchMode(*preCommitBranchMode);
        if (!branchMode.has_value()) {
            std::cerr << "ERROR: Unsupported --branch-mode: " << *preCommitBranchMode << " (supported: default, stable-dev)\n";
            std::exit(1);
        }

        const auto start = std::chrono::steady_clock::now();
        const auto repoRoot = std::filesystem::weakly_canonical(std::filesystem::path(*preCommitRepo));
        const auto code = RunNativePreCommitRepair(
            repoRoot,
            *preCommitRemote,
            *preCommitMaxDepth,
            *preCommitDryRun,
            *preCommitNoCache,
            *preCommitRefreshCache,
            !*preCommitNoRecursive,
            *branchMode);
        if (*preCommitProfile) {
            const auto totalMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
            std::cout << "\n=== Sync Profile Summary ===\n";
            std::cout << "mode: native\n";
            std::cout << "workflow: pre-commit\n";
            std::cout << "recursive: " << (!*preCommitNoRecursive ? "true" : "false") << "\n";
            std::cout << "dry_run: " << (*preCommitDryRun ? "true" : "false") << "\n";
            std::cout << "branch_mode: " << *preCommitBranchMode << "\n";
            std::cout << "total_ms: " << totalMs << "\n";
        }
        std::exit(code);
    });

    // --- sync origin-latest ---
    auto* origin_latest = cmd->add_subcommand("origin-latest", "Sync to origin default branch latest");
    origin_latest->allow_extras();
    auto* originLatestShell = new bool{false};
    auto* originLatestRepo = new std::string{"."};
    auto* originLatestRemote = new std::string{"origin"};
    auto* originLatestDryRun = new bool{false};
    auto* originLatestMaxDepth = new int{0};
    auto* originLatestNoCache = new bool{false};
    auto* originLatestRefreshCache = new bool{false};
    auto* originLatestNoRecursive = new bool{false};
    auto* originLatestNoAutoStash = new bool{false};
    auto* originLatestCleanupStaleLocks = new bool{false};
    auto* originLatestProfile = new bool{false};

    origin_latest->add_flag("--shell", *originLatestShell, "Deprecated compatibility flag (shell path removed)");
    origin_latest->add_option("--repo", *originLatestRepo, "Target repository root path");
    origin_latest->add_option("--remote", *originLatestRemote, "Preferred remote name");
    origin_latest->add_flag("--dry-run", *originLatestDryRun, "Preview sync actions without modifying repositories");
    origin_latest->add_option("--native-max-depth", *originLatestMaxDepth, "Native discovery max depth (0 = unlimited)");
    origin_latest->add_flag("--native-no-cache", *originLatestNoCache, "Disable native discovery cache");
    origin_latest->add_flag("--native-refresh-cache", *originLatestRefreshCache, "Force native cache refresh");
    origin_latest->add_flag("--no-recursive,-N", *originLatestNoRecursive, "Sync only current repository");
    origin_latest->add_flag("--no-auto-stash", *originLatestNoAutoStash, "Do not auto-stash local changes before sync");
    origin_latest->add_flag("--cleanup-stale-locks", *originLatestCleanupStaleLocks, "When auto-stash fails on index.lock and no git/kano-git process is active, remove the stale lock and retry once");
    origin_latest->add_flag("--profile", *originLatestProfile, "Print native sync timing/profile summary");

    origin_latest->callback([=]() {
        auto extras = origin_latest->remaining();
        if (*originLatestShell) {
            std::cerr << "Error: --shell is no longer supported; sync origin-latest is fully native now\n";
            std::exit(2);
        }
        if (!extras.empty()) {
            std::cerr << "Error: unsupported extra arguments in native sync origin-latest mode:";
            for (const auto& extra : extras) {
                std::cerr << ' ' << extra;
            }
            std::cerr << "\n";
            std::exit(2);
        }

        const auto start = std::chrono::steady_clock::now();
        const auto repoRoot = std::filesystem::weakly_canonical(std::filesystem::path(*originLatestRepo));
        const auto code = RunNativeOriginLatestSync(
            repoRoot,
            *originLatestRemote,
            *originLatestMaxDepth,
            *originLatestDryRun,
            *originLatestNoCache,
            *originLatestRefreshCache,
            !*originLatestNoRecursive,
            !*originLatestNoAutoStash,
            *originLatestCleanupStaleLocks);
        if (*originLatestProfile) {
            const auto totalMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
            std::cout << "\n=== Sync Profile Summary ===\n";
            std::cout << "mode: native\n";
            std::cout << "workflow: origin-latest\n";
            std::cout << "recursive: " << (!*originLatestNoRecursive ? "true" : "false") << "\n";
            std::cout << "dry_run: " << (*originLatestDryRun ? "true" : "false") << "\n";
            std::cout << "total_ms: " << totalMs << "\n";
        }
        std::exit(code);
    });

    // --- sync upstream-force-push ---
    auto* upstream_fp = cmd->add_subcommand("upstream-force-push", "Sync from upstream, force-push to origin");
    upstream_fp->allow_extras();
    auto* upstreamRepo = new std::string{"."};
    auto* upstreamDryRun = new bool{false};
    auto* upstreamProfile = new bool{false};
    upstream_fp->add_option("--repo", *upstreamRepo, "Target repository path");
    upstream_fp->add_flag("--dry-run", *upstreamDryRun, "Preview force-push sync actions");
    upstream_fp->add_flag("--profile", *upstreamProfile, "Print native sync timing/profile summary");
    upstream_fp->callback([=]() {
        auto extras = upstream_fp->remaining();
        if (!extras.empty()) {
            std::cerr << "Error: unsupported extra arguments in native sync upstream-force-push mode:";
            for (const auto& extra : extras) {
                std::cerr << ' ' << extra;
            }
            std::cerr << "\n";
            std::exit(2);
        }

        const auto start = std::chrono::steady_clock::now();
        const auto repoRoot = std::filesystem::weakly_canonical(std::filesystem::path(*upstreamRepo));
        const auto code = RunNativeUpstreamForcePush(repoRoot, *upstreamDryRun);
        if (*upstreamProfile) {
            const auto totalMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
            std::cout << "\n=== Sync Profile Summary ===\n";
            std::cout << "mode: native\n";
            std::cout << "workflow: upstream-force-push\n";
            std::cout << "dry_run: " << (*upstreamDryRun ? "true" : "false") << "\n";
            std::cout << "total_ms: " << totalMs << "\n";
        }
        std::exit(code);
    });

    // --- sync stable-dev ---
    auto* stable_dev = cmd->add_subcommand(
        "stable-dev",
        "Stable-dev sync: fetch upstream tags, rebase current stable branch onto latest stable tag, and retarget to branch_<latestTag> when needed");
    stable_dev->allow_extras();
    auto* stableDevWorkspace = new bool{false};
    auto* stableDevReportFormat = new std::string{"compact"};
    auto* stableDevRepo = new std::string{"."};
    auto* stableDevDryRun = new bool{false};
    auto* stableDevProfile = new bool{false};
    stable_dev->add_flag(
        "--workspace",
        *stableDevWorkspace,
        "Run stable-dev across src/* submodules with upstream remotes; may leave repos in rebase conflict state until resolved");
    stable_dev->add_option("--format", *stableDevReportFormat, "Workspace report format: compact|table|tsv|json|markdown");
    stable_dev->add_option("--repo", *stableDevRepo, "Single-repo mode target path");
    stable_dev->add_flag(
        "--dry-run",
        *stableDevDryRun,
        "Preview fetch/tag/rebase/branch-retarget actions without modifying repos");
    stable_dev->add_flag("--profile", *stableDevProfile, "Print native sync timing/profile summary");
    stable_dev->callback([=]() {
        const auto start = std::chrono::steady_clock::now();
        auto extras = stable_dev->remaining();
        std::vector<std::string> args(extras.begin(), extras.end());

        if (!extras.empty()) {
            std::cerr << "Error: unsupported extra arguments in native sync stable-dev mode:";
            for (const auto& extra : extras) {
                std::cerr << ' ' << extra;
            }
            std::cerr << "\n";
            std::exit(2);
        }

        const auto format = ParseStableDevReportFormat(*stableDevReportFormat);
        if (!format.has_value()) {
            std::cerr << "ERROR: Unsupported --format: " << *stableDevReportFormat << " (supported: compact, table, tsv, json, markdown)\n";
            std::exit(1);
        }

        std::filesystem::path workspaceRoot;
        if (const char* rootEnv = std::getenv("KANO_GIT_MASTER_ROOT"); rootEnv != nullptr && std::string(rootEnv).size() > 0) {
            workspaceRoot = std::filesystem::weakly_canonical(std::filesystem::path(rootEnv));
        } else {
            workspaceRoot = std::filesystem::current_path();
        }

        if (*stableDevWorkspace) {
            if (!*stableDevDryRun && HasLongFlag(args, "--dry-run")) {
                *stableDevDryRun = true;
            }
            std::vector<std::string> workspaceArgs;
            if (*stableDevDryRun) {
                workspaceArgs.push_back("--dry-run");
            }
            const auto code = RunStableDevWorkspace(workspaceRoot, *format, workspaceArgs);
            if (*stableDevProfile) {
                const auto totalMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
                std::cout << "\n=== Sync Profile Summary ===\n";
                std::cout << "mode: native\n";
                std::cout << "workflow: stable-dev(workspace)\n";
                std::cout << "dry_run: " << (*stableDevDryRun ? "true" : "false") << "\n";
                std::cout << "total_ms: " << totalMs << "\n";
            }
            std::exit(code);
        }

        const auto repoPath = std::filesystem::weakly_canonical(std::filesystem::path(*stableDevRepo));
        const auto repoRel = std::filesystem::relative(repoPath, workspaceRoot).generic_string();
        const auto repoLabel = (repoRel.empty() || repoRel == ".") ? "." : repoRel;

        if (GitCapture(repoPath, {"rev-parse", "--is-inside-work-tree"}).exitCode != 0) {
            std::cerr << "ERROR: Not a git repository: " << repoPath.generic_string() << "\n";
            std::exit(1);
        }
        if (!HasRemote(repoPath, "upstream")) {
            std::cerr << "ERROR: Missing upstream remote for repo: " << repoLabel << "\n";
            std::exit(1);
        }

        StableDevSummaryRow row;
        const auto code = RunNativeStableDevSync(workspaceRoot, repoPath, repoLabel, *stableDevDryRun, row);

        std::cout << "=== upstream-stable-dev wrapper summary ===\n";
        std::cout << "success: " << (code == 0 ? 1 : 0) << "\n";
        std::cout << "skipped: 0\n";
        std::cout << "failed: " << (code == 0 ? 0 : 1) << "\n";
        std::cout << "OVERALL RESULT: " << (code == 0 ? "SUCCESS" : "FAILED") << "\n";
        std::cout << "=== upstream-stable-dev branch report ===\n";
        PrintStableDevSummary({row}, *format);
        if (*stableDevProfile) {
            const auto totalMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
            std::cout << "\n=== Sync Profile Summary ===\n";
            std::cout << "mode: native\n";
            std::cout << "workflow: stable-dev(single-repo)\n";
            std::cout << "dry_run: " << (*stableDevDryRun ? "true" : "false") << "\n";
            std::cout << "total_ms: " << totalMs << "\n";
        }
        std::exit(code == 0 ? 0 : 1);
    });

    // --- sync dev ---
    auto* dev = cmd->add_subcommand("dev", "Dev sync (upstream default branch tip)");
    dev->allow_extras();
    auto* devRepo = new std::string{"."};
    auto* devDryRun = new bool{false};
    auto* devMaxDepth = new int{0};
    auto* devNoCache = new bool{false};
    auto* devRefreshCache = new bool{false};
    auto* devNoRecursive = new bool{false};
    auto* devCleanupStaleLocks = new bool{false};
    auto* devProfile = new bool{false};
    dev->add_option("--repo", *devRepo, "Target repository root path");
    dev->add_flag("--dry-run", *devDryRun, "Preview sync actions without modifying repositories");
    dev->add_option("--native-max-depth", *devMaxDepth, "Native discovery max depth (0 = unlimited)");
    dev->add_flag("--native-no-cache", *devNoCache, "Disable native discovery cache");
    dev->add_flag("--native-refresh-cache", *devRefreshCache, "Force native cache refresh");
    dev->add_flag("--no-recursive,-N", *devNoRecursive, "Sync only current repository");
    dev->add_flag("--cleanup-stale-locks", *devCleanupStaleLocks, "When auto-stash fails on index.lock and no git/kano-git process is active, remove the stale lock and retry once");
    dev->add_flag("--profile", *devProfile, "Print native sync timing/profile summary");
    dev->callback([=]() {
        auto extras = dev->remaining();
        if (!extras.empty()) {
            std::cerr << "Error: unsupported extra arguments in native sync dev mode:";
            for (const auto& extra : extras) {
                std::cerr << ' ' << extra;
            }
            std::cerr << "\n";
            std::exit(2);
        }

        const auto start = std::chrono::steady_clock::now();
        const auto repoRoot = std::filesystem::weakly_canonical(std::filesystem::path(*devRepo));
        const auto code = RunNativeOriginLatestSync(
            repoRoot,
            "upstream",
            *devMaxDepth,
            *devDryRun,
            *devNoCache,
            *devRefreshCache,
            !*devNoRecursive,
            true,
            *devCleanupStaleLocks);
        if (*devProfile) {
            const auto totalMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
            std::cout << "\n=== Sync Profile Summary ===\n";
            std::cout << "mode: native\n";
            std::cout << "workflow: dev\n";
            std::cout << "recursive: " << (!*devNoRecursive ? "true" : "false") << "\n";
            std::cout << "dry_run: " << (*devDryRun ? "true" : "false") << "\n";
            std::cout << "total_ms: " << totalMs << "\n";
        }
        std::exit(code);
    });

    // --- sync launcher-update-check ---
    auto* launcher_update = cmd->add_subcommand("launcher-update-check", "Launcher dev-mode remote update check");
    auto* launcherRepo = new std::string{"."};
    auto* launcherRemote = new std::string{"upstream"};
    auto* launcherAutoSync = new bool{false};
    auto* launcherNonInteractive = new bool{false};
    launcher_update->add_option("--repo", *launcherRepo, "Launcher repository root path");
    launcher_update->add_option("--remote", *launcherRemote, "Preferred remote name (fallback: origin/upstream)");
    launcher_update->add_flag("--auto-sync", *launcherAutoSync, "Auto-run sync without prompt when updates exist");
    launcher_update->add_flag("--non-interactive", *launcherNonInteractive, "Disable prompt and skip auto-sync unless --auto-sync");
    launcher_update->callback([=]() {
        const auto repoRoot = std::filesystem::weakly_canonical(std::filesystem::path(*launcherRepo));
        if (GitCapture(repoRoot, {"rev-parse", "--is-inside-work-tree"}).exitCode != 0) {
            std::exit(0);
        }
        if (!Trim(GitCapture(repoRoot, {"status", "--porcelain"}).stdoutStr).empty()) {
            std::cerr << "[launcher] Skip remote update check: dirty worktree in " << repoRoot.generic_string() << "\n";
            std::exit(0);
        }

        const auto preferred = Trim(*launcherRemote);
        auto remote = preferred;
        if (remote.empty() || !HasRemote(repoRoot, remote)) {
            if (HasRemote(repoRoot, "upstream")) {
                remote = "upstream";
            } else if (HasRemote(repoRoot, "origin")) {
                remote = "origin";
            } else {
                std::exit(0);
            }
        }

        std::cerr << "[launcher] Checking remote updates from " << remote << "...\n";
        (void)GitCapture(repoRoot, {"fetch", remote, "--prune"});

        auto branch = DetectRemoteDefaultBranch(repoRoot, remote);
        if (branch.empty()) {
            branch = CurrentBranch(repoRoot);
        }
        branch = Trim(branch);
        if (branch.empty()) {
            std::exit(0);
        }

        const auto ahead = GitCapture(repoRoot, {"rev-list", "--count", std::format("HEAD..{}/{}", remote, branch)});
        if (ahead.exitCode != 0) {
            std::exit(0);
        }
        int aheadCount = 0;
        try {
            aheadCount = std::stoi(Trim(ahead.stdoutStr));
        } catch (...) {
            aheadCount = 0;
        }
        if (aheadCount <= 0) {
            std::exit(0);
        }

        bool shouldSync = *launcherAutoSync;
        const bool interactiveAllowed = !*launcherNonInteractive && !IsAgentModeEnabled() && IsInteractiveTerminal();
        if (!shouldSync && interactiveAllowed) {
            shouldSync = PromptYesNo(std::format("[launcher] Found {} upstream commit(s) on {}/{}. Run sync now?", aheadCount, remote, branch));
        }
        if (!shouldSync) {
            std::exit(0);
        }

        const auto code = RunNativeOriginLatestSync(
            repoRoot,
            remote,
            12,
            false,
            false,
            false,
            false,
            true,
            false);
        std::exit(code);
    });

    // --- sync (default: auto-detect) ---
    auto* defaultNoRecursive = new bool{false};
    auto* defaultCleanupStaleLocks = new bool{false};
    auto* defaultProfile = new bool{false};
    cmd->add_flag("--no-recursive,-N", *defaultNoRecursive, "Default sync: only current repository");
    cmd->add_flag("--cleanup-stale-locks", *defaultCleanupStaleLocks, "Default sync: remove stale index.lock automatically when no git/kano-git process is active");
    cmd->add_flag("--profile", *defaultProfile, "Default sync: print native timing/profile summary");
    cmd->allow_extras();
    cmd->callback([=]() {
        if (cmd->get_subcommands().empty()) {
            auto extras = cmd->remaining();
            if (!extras.empty()) {
                std::cerr << "Error: default sync in native-only mode does not accept extra args.\n";
                std::cerr << "Hint: use explicit subcommands, e.g. 'kog sync origin-latest'.\n";
                std::exit(2);
            }

            const auto start = std::chrono::steady_clock::now();
            const auto code = RunNativeOriginLatestSync(
                std::filesystem::current_path(),
                "origin",
                12,
                false,
                false,
                false,
                !*defaultNoRecursive,
                true,
                *defaultCleanupStaleLocks);
            if (*defaultProfile) {
                const auto totalMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
                std::cout << "\n=== Sync Profile Summary ===\n";
                std::cout << "mode: native\n";
                std::cout << "workflow: default(origin-latest)\n";
                std::cout << "recursive: " << (!*defaultNoRecursive ? "true" : "false") << "\n";
                std::cout << "dry_run: false\n";
                std::cout << "total_ms: " << totalMs << "\n";
            }
            std::exit(code);
        }
    });
}

auto RunSyncPreCommitNative(const std::filesystem::path& InRepoRoot,
                            const bool InRecursive,
                            const bool InDryRun,
                            const std::string& InBranchMode) -> int {
    const auto branchMode = ParseBranchMode(InBranchMode);
    if (!branchMode.has_value()) {
        std::cerr << "ERROR: Unsupported --branch-mode: " << InBranchMode << " (supported: default, stable-dev)\n";
        return 2;
    }
    return RunNativePreCommitRepair(
        InRepoRoot,
        "origin",
        12,
        InDryRun,
        false,
        false,
        InRecursive,
        *branchMode);
}

auto RunSyncOriginLatestNative(const std::filesystem::path& InRepoRoot,
                               const bool InRecursive,
                               const bool InDryRun,
                               const bool InCleanupStaleLocks) -> int {
    return RunNativeOriginLatestSync(
        InRepoRoot,
        "origin",
        12,
        InDryRun,
        false,
        false,
        InRecursive,
        true,
        InCleanupStaleLocks);
}

} // namespace kano::git::commands
