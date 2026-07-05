// sync command — Repository synchronization workflows
// Uses native Git synchronization and kog-managed workflows

#include <CLI/CLI.hpp>
#include "discovery.hpp"
#include "repo_health.hpp"
#include "shell_executor.hpp"
#include "../public/sync_output_sanitizer.hpp"
#include "terminal_color.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <format>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <mutex>
#include <regex>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <fstream>

#include "repo_operation_scheduler.hpp"

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
    std::string remoteSelectionSource;
    std::string remoteSelectionError;
    std::string remoteSelectionDetail;
    std::string targetBranch;
    std::string branchSource;
    std::string kogSyncPolicy;
    std::filesystem::path registrationRelativeTo;
    std::vector<std::filesystem::path> dependencies;
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
    return (InSelfRepoRoot / "src/cpp/scripts/self/build.sh").lexically_normal();
}

auto RunSelfCppBuild(const std::filesystem::path& InSelfRepoRoot) -> int {
    const auto buildScript = ResolveSelfBuildScript(InSelfRepoRoot);
    if (!std::filesystem::exists(buildScript)) {
        std::cerr << "ERROR: self C++ build script not found: " << buildScript.generic_string() << "\n";
        return 1;
    }

    std::error_code relEc;
    const auto relativeScript = std::filesystem::relative(buildScript, InSelfRepoRoot, relEc);
    const auto scriptArg = relEc ? buildScript.generic_string() : relativeScript.generic_string();

    std::cout << "[sync] self repo C++ changes detected; running build: " << buildScript.generic_string() << "\n";
    const auto run = shell::ExecuteCommand(
        "bash",
        {scriptArg},
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

auto IsFalsePolicy(std::string InValue) -> bool {
    InValue = ToLower(Trim(std::move(InValue)));
    return InValue == "false" || InValue == "0" || InValue == "no" || InValue == "off" || InValue == "disabled";
}

auto RepoPathKey(const std::filesystem::path& InPath) -> std::string {
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

auto AppendSyncRepoContext(std::ostream& InOut,
                           const std::string& InName,
                           const SyncPlan& InPlan,
                           bool InDryRun) -> void {
    const std::string dryRunPrefix = InDryRun ? "[DRY RUN] " : "";
    InOut << dryRunPrefix << "Repo: " << InName << "\n";
    InOut << "Taxonomy: " << InPlan.type << "\n";
    InOut << "Branch source: " << InPlan.branchSource << "\n";
    InOut << "Selected remote: " << (InPlan.remote.empty() ? "(none)" : InPlan.remote) << "\n";
    InOut << "Remote selection source: " << (InPlan.remoteSelectionSource.empty() ? "unknown" : InPlan.remoteSelectionSource) << "\n";
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

struct RemoteSelectionResult {
    std::string remote;
    std::string source;
    std::string error;
    std::string detail;
    std::vector<std::string> candidates;
    std::vector<std::string> ignoredRemotes;
};

struct AuthTarget {
    std::filesystem::path repoPath;
    std::string repoLabel;
    std::string remoteName;
    std::string remoteUrl;
    std::string source;
    std::string detail;
    bool explicitUrl{false};
};

struct AuthProbeResult {
    bool success{false};
    bool skipped{false};
    std::string category;
    std::string summary;
    std::string stdoutText;
    std::string stderrText;
};

enum class AuthProbeScope {
    CommandTest,
    SyncPreflight,
};

struct AuthTargetSelectionOptions {
    std::filesystem::path repoRoot;
    std::string explicitRemote;
    std::string explicitUrl;
    bool selectedRemotes{false};
    bool allLocalRemotes{false};
    bool recursive{true};
    bool noCache{false};
    bool refreshCache{false};
};

auto SelectSyncRemote(const std::filesystem::path& InWorkspaceRoot,
                      const std::filesystem::path& InRepo,
                      const std::string& InPreferredRemote,
                      bool InIsRegistered,
                      bool InIsRoot,
                      bool InIsDetached) -> RemoteSelectionResult;

auto CurrentBranch(const std::filesystem::path& InRepo) -> std::string;

auto BuildSyncPlans(
    const std::filesystem::path& InRoot,
    const std::string& InPreferredRemote,
    int InMaxDepth,
    bool InNoCache,
    bool InRefreshCache) -> std::pair<std::vector<SyncPlan>, std::string>;

auto ClassifySyncFailure(const shell::ExecResult& InResult, const std::string& InFallback) -> std::string;

auto ListRemotes(const std::filesystem::path& InRepo) -> std::vector<std::string> {
    std::vector<std::string> out;
    const auto remotes = GitCapture(InRepo, {"remote"});
    if (remotes.exitCode != 0) {
        return out;
    }
    std::istringstream iss(remotes.stdoutStr);
    std::string line;
    while (std::getline(iss, line)) {
        line = Trim(line);
        if (!line.empty()) {
            out.push_back(line);
        }
    }
    return out;
}

auto RemoteUrl(const std::filesystem::path& InRepo, const std::string& InRemote) -> std::string {
    const auto out = GitCapture(InRepo, {"remote", "get-url", InRemote});
    if (out.exitCode != 0) {
        return {};
    }
    return Trim(out.stdoutStr);
}

auto NormalizeRemoteUrlForMatch(std::string InUrl) -> std::string {
    InUrl = Trim(std::move(InUrl));
    if (InUrl.empty()) {
        return InUrl;
    }
    if (InUrl.ends_with(".git")) {
        InUrl = InUrl.substr(0, InUrl.size() - 4);
    }
    const auto schemePos = InUrl.find("://");
    if (schemePos != std::string::npos) {
        const auto authorityStart = schemePos + 3;
        const auto pathPos = InUrl.find('/', authorityStart);
        const auto authorityEnd = (pathPos == std::string::npos) ? InUrl.size() : pathPos;
        auto authority = InUrl.substr(authorityStart, authorityEnd - authorityStart);
        const auto atPos = authority.find('@');
        if (atPos != std::string::npos) {
            authority = authority.substr(atPos + 1);
        }
        std::transform(authority.begin(), authority.end(), authority.begin(), [](unsigned char ch) {
            return static_cast<char>(std::tolower(ch));
        });
        InUrl = InUrl.substr(0, authorityStart) + authority + InUrl.substr(authorityEnd);
    }
    return InUrl;
}

class ScopedEnvOverride {
  public:
    explicit ScopedEnvOverride(const std::vector<std::pair<std::string, std::string>>& InEntries) {
        previousValues_.reserve(InEntries.size());
        for (const auto& [key, value] : InEntries) {
            if (key.empty()) {
                continue;
            }
            if (const auto* previous = std::getenv(key.c_str()); previous != nullptr) {
                previousValues_.push_back({key, std::string(previous)});
            } else {
                previousValues_.push_back({key, std::nullopt});
            }
#if defined(_WIN32)
            _putenv_s(key.c_str(), value.c_str());
#else
            setenv(key.c_str(), value.c_str(), 1);
#endif
        }
    }

    ~ScopedEnvOverride() {
        for (auto it = previousValues_.rbegin(); it != previousValues_.rend(); ++it) {
            const auto& [key, previous] = *it;
#if defined(_WIN32)
            if (previous.has_value()) {
                _putenv_s(key.c_str(), previous->c_str());
            } else {
                _putenv_s(key.c_str(), "");
            }
#else
            if (previous.has_value()) {
                setenv(key.c_str(), previous->c_str(), 1);
            } else {
                unsetenv(key.c_str());
            }
#endif
        }
    }

    ScopedEnvOverride(const ScopedEnvOverride&) = delete;
    auto operator=(const ScopedEnvOverride&) -> ScopedEnvOverride& = delete;

  private:
    std::vector<std::pair<std::string, std::optional<std::string>>> previousValues_;
};

auto MakeNonInteractiveGitEnvOverrides() -> const std::vector<std::pair<std::string, std::string>>& {
    static const std::vector<std::pair<std::string, std::string>> kOverrides = {
        {"KOG_GIT_INTERACTIVE", "0"},
        {"GIT_TERMINAL_PROMPT", "0"},
        {"GCM_INTERACTIVE", "never"},
        {"GIT_ASKPASS", "true"},
        {"SSH_ASKPASS", "true"},
    };
    return kOverrides;
}

auto RedactUrlCredentials(std::string InText) -> std::string {
    static const std::regex kCredentialRegex(R"(((?:[a-z][a-z0-9+.-]*://))([^/\s@]+)@)", std::regex::icase);
    return std::regex_replace(InText, kCredentialRegex, "$1<redacted>@");
}

auto SanitizeAuthText(std::string InText) -> std::string {
    InText = NormalizeSyncCapturedText(InText, SyncOutputSanitizeMode::Human);
    return RedactUrlCredentials(std::move(InText));
}

auto IsTruthyValue(std::string InValue) -> bool {
    InValue = ToLower(Trim(std::move(InValue)));
    return InValue == "1" || InValue == "true" || InValue == "yes" || InValue == "on";
}

auto AuthProtocolForUrl(const std::string& InUrl) -> std::string {
    const auto url = ToLower(Trim(InUrl));
    if (url.empty()) {
        return "unknown";
    }
    if (url.rfind("https://", 0) == 0) {
        return "https";
    }
    if (url.rfind("http://", 0) == 0) {
        return "http";
    }
    if (url.rfind("ssh://", 0) == 0) {
        return "ssh";
    }
    if (url.rfind("file://", 0) == 0) {
        return "file";
    }
#if defined(_WIN32)
    if (url.size() > 2 && std::isalpha(static_cast<unsigned char>(url[0])) && url[1] == ':' && (url[2] == '/' || url[2] == '\\')) {
        return "file";
    }
#endif
    if (url.rfind("./", 0) == 0 || url.rfind("../", 0) == 0 || url.rfind('/', 0) == 0 || url.rfind('\\', 0) == 0) {
        return "file";
    }
    const auto atPos = url.find('@');
    const auto colonPos = url.find(':');
    const auto slashPos = url.find('/');
    if (atPos != std::string::npos && colonPos != std::string::npos && (slashPos == std::string::npos || colonPos < slashPos)) {
        return "ssh";
    }
    return "other";
}

auto ShouldSkipCommandAuthProbe(const std::string& InUrl) -> bool {
    return AuthProtocolForUrl(InUrl) == "file";
}

auto ShouldRunSyncAuthPreflight(const std::string& InUrl) -> bool {
    const auto protocol = AuthProtocolForUrl(InUrl);
    return protocol == "http" || protocol == "https";
}

auto LastNonEmptyLine(const std::string& InText) -> std::string {
    std::istringstream iss(InText);
    std::string line;
    std::string last;
    while (std::getline(iss, line)) {
        line = Trim(line);
        if (!line.empty()) {
            last = line;
        }
    }
    return last;
}

auto MakeRepoDisplayName(const std::filesystem::path& InRoot, const std::filesystem::path& InRepo) -> std::string {
    std::error_code ec;
    const auto rel = std::filesystem::relative(InRepo, InRoot, ec);
    if (!ec) {
        const auto text = rel.generic_string();
        if (text.empty() || text == ".") {
            return ".";
        }
        return text;
    }
    return InRepo.generic_string();
}

auto MakeAuthTarget(const std::filesystem::path& InWorkspaceRoot,
                    const std::filesystem::path& InRepo,
                    const std::string& InRemote,
                    const std::string& InSource,
                    const std::string& InDetail,
                    bool InExplicitUrl = false,
                    const std::string& InExplicitUrlValue = {}) -> AuthTarget {
    AuthTarget target;
    target.repoPath = InRepo;
    target.repoLabel = MakeRepoDisplayName(InWorkspaceRoot, InRepo);
    target.remoteName = InRemote;
    target.remoteUrl = InExplicitUrl ? InExplicitUrlValue : RemoteUrl(InRepo, InRemote);
    target.source = InSource;
    target.detail = InDetail;
    target.explicitUrl = InExplicitUrl;
    return target;
}

auto RunGitCaptureNonInteractive(const std::filesystem::path& InRepo, const std::vector<std::string>& InArgs) -> shell::ExecResult {
    shell::ScopedConsoleWriteSuppression suppressConsoleWrites;
    ScopedEnvOverride envOverride(MakeNonInteractiveGitEnvOverrides());
    return GitCapture(InRepo, InArgs);
}

auto ClassifyAuthProbeFailure(const shell::ExecResult& InResult) -> std::string {
    const auto merged = ToLower(InResult.stdoutStr + "\n" + InResult.stderrStr);
    if (merged.find("terminal prompts disabled") != std::string::npos ||
        merged.find("could not read password") != std::string::npos ||
        merged.find("credential manager") != std::string::npos ||
        merged.find("interaction required") != std::string::npos ||
        merged.find("user interaction") != std::string::npos) {
        return "FAILED_AUTH";
    }
    return ClassifySyncFailure(InResult, "FAILED_AUTH");
}

auto RunAuthProbe(const AuthTarget& InTarget, AuthProbeScope InScope) -> AuthProbeResult {
    AuthProbeResult result;
    const auto protocol = AuthProtocolForUrl(InTarget.remoteUrl);
    const auto redactedUrl = RedactUrlCredentials(Trim(InTarget.remoteUrl));
    const auto remoteDisplay = InTarget.explicitUrl
        ? std::string("<url>")
        : (InTarget.remoteName.empty() ? std::string("(none)") : InTarget.remoteName);

    if (InTarget.remoteName.empty() && InTarget.remoteUrl.empty()) {
        result.category = "FAILED_MISSING_REMOTE";
        result.summary = InTarget.detail.empty() ? "no usable auth target found" : InTarget.detail;
        result.stderrText = std::format(
            "[{}] AUTH_TEST_FAILED: {} remote={} protocol={}\n",
            InTarget.repoLabel,
            result.category,
            remoteDisplay,
            protocol);
        if (!result.summary.empty()) {
            result.stderrText += std::format("[{}] detail: {}\n", InTarget.repoLabel, result.summary);
        }
        return result;
    }

    if (InScope == AuthProbeScope::CommandTest && ShouldSkipCommandAuthProbe(InTarget.remoteUrl)) {
        result.success = true;
        result.skipped = true;
        result.category = "SKIPPED_LOCAL_REMOTE";
        result.summary = "local/file remotes do not use Git Credential Manager";
        result.stdoutText = std::format(
            "[{}] AUTH_TEST_SKIPPED: remote={} protocol={} url={}\n",
            InTarget.repoLabel,
            remoteDisplay,
            protocol,
            redactedUrl.empty() ? std::string("(none)") : redactedUrl);
        return result;
    }

    if (InScope == AuthProbeScope::SyncPreflight && !ShouldRunSyncAuthPreflight(InTarget.remoteUrl)) {
        result.success = true;
        result.skipped = true;
        result.category = "SKIPPED_NON_HTTP";
        return result;
    }

    std::vector<std::string> args{"ls-remote", "--exit-code"};
    if (InTarget.explicitUrl) {
        args.push_back(InTarget.remoteUrl);
    } else {
        args.push_back(InTarget.remoteName);
    }
    args.push_back("HEAD");

    const auto exec = RunGitCaptureNonInteractive(InTarget.repoPath, args);
    const auto sanitizedStdout = SanitizeAuthText(exec.stdoutStr);
    const auto sanitizedStderr = SanitizeAuthText(exec.stderrStr);

    if (exec.exitCode == 0) {
        result.success = true;
        result.category = "PASS";
        result.summary = "non-interactive ls-remote HEAD succeeded";
        result.stdoutText = std::format(
            "[{}] AUTH_TEST_OK: remote={} protocol={} url={}\n",
            InTarget.repoLabel,
            remoteDisplay,
            protocol,
            redactedUrl.empty() ? std::string("(none)") : redactedUrl);
        return result;
    }

    result.category = ClassifyAuthProbeFailure(exec);
    result.summary = LastNonEmptyLine(!sanitizedStderr.empty() ? sanitizedStderr : sanitizedStdout);
    if (result.summary.empty()) {
        result.summary = "non-interactive ls-remote HEAD failed";
    }
    result.stderrText = std::format(
        "[{}] AUTH_TEST_FAILED: {} remote={} protocol={} url={}\n",
        InTarget.repoLabel,
        result.category,
        remoteDisplay,
        protocol,
        redactedUrl.empty() ? std::string("(none)") : redactedUrl);
    result.stderrText += std::format("[{}] detail: {}\n", InTarget.repoLabel, result.summary);
    return result;
}

auto CollectSelectedAuthTargets(const AuthTargetSelectionOptions& InOptions) -> std::vector<AuthTarget> {
    auto planResult = BuildSyncPlans(
        InOptions.repoRoot,
        "origin",
        0,
        InOptions.noCache,
        InOptions.refreshCache);
    auto plans = std::move(planResult.first);
    if (!InOptions.recursive) {
        const auto root = std::filesystem::weakly_canonical(InOptions.repoRoot);
        plans.erase(
            std::remove_if(plans.begin(), plans.end(), [&](const SyncPlan& InPlan) {
                return std::filesystem::weakly_canonical(InPlan.path) != root;
            }),
            plans.end());
    }

    std::vector<AuthTarget> targets;
    targets.reserve(plans.size());
    for (const auto& plan : plans) {
        if (IsFalsePolicy(plan.kogSyncPolicy)) {
            continue;
        }
        if (plan.remote.empty()) {
            targets.push_back(MakeAuthTarget(
                InOptions.repoRoot,
                plan.path,
                {},
                plan.remoteSelectionSource,
                plan.remoteSelectionError.empty() ? plan.remoteSelectionDetail : plan.remoteSelectionError,
                false));
            continue;
        }
        targets.push_back(MakeAuthTarget(
            InOptions.repoRoot,
            plan.path,
            plan.remote,
            plan.remoteSelectionSource,
            plan.remoteSelectionDetail,
            false));
    }
    return targets;
}

auto CollectAuthTargets(const AuthTargetSelectionOptions& InOptions) -> std::vector<AuthTarget> {
    const auto repoRoot = std::filesystem::weakly_canonical(InOptions.repoRoot);
    if (!InOptions.explicitUrl.empty()) {
        return {MakeAuthTarget(repoRoot, repoRoot, {}, "explicit-url", {}, true, InOptions.explicitUrl)};
    }

    if (!InOptions.explicitRemote.empty()) {
        if (!HasRemote(repoRoot, InOptions.explicitRemote)) {
            return {MakeAuthTarget(repoRoot, repoRoot, {}, "explicit-remote", std::format("remote '{}' is not configured", InOptions.explicitRemote), false)};
        }
        return {MakeAuthTarget(repoRoot, repoRoot, InOptions.explicitRemote, "explicit-remote", {}, false)};
    }

    if (InOptions.allLocalRemotes) {
        const auto remotes = ListRemotes(repoRoot);
        std::vector<AuthTarget> targets;
        if (remotes.empty()) {
            targets.push_back(MakeAuthTarget(repoRoot, repoRoot, {}, "all-local-remotes", "no remotes configured", false));
            return targets;
        }
        targets.reserve(remotes.size());
        for (const auto& remote : remotes) {
            targets.push_back(MakeAuthTarget(repoRoot, repoRoot, remote, "all-local-remotes", {}, false));
        }
        return targets;
    }

    if (InOptions.selectedRemotes) {
        return CollectSelectedAuthTargets(InOptions);
    }

    const auto selected = SelectSyncRemote(repoRoot, repoRoot, {}, false, true, CurrentBranch(repoRoot).empty());
    if (selected.remote.empty()) {
        return {MakeAuthTarget(repoRoot, repoRoot, {}, selected.source, selected.error.empty() ? selected.detail : selected.error, false)};
    }
    return {MakeAuthTarget(repoRoot, repoRoot, selected.remote, selected.source, selected.detail, false)};
}

auto GitConfigGetAllWithOrigin(const std::filesystem::path& InRepo, const std::string& InKey) -> std::vector<std::string> {
    std::vector<std::string> values;
    const auto result = GitCapture(InRepo, {"config", "--show-origin", "--get-all", InKey});
    if (result.exitCode != 0) {
        return values;
    }
    std::istringstream iss(result.stdoutStr);
    std::string line;
    while (std::getline(iss, line)) {
        line = Trim(line);
        if (!line.empty()) {
            values.push_back(line);
        }
    }
    return values;
}

auto GitConfigGetValue(const std::filesystem::path& InRepo, const std::string& InKey) -> std::optional<std::string> {
    const auto result = GitCapture(InRepo, {"config", "--get", InKey});
    if (result.exitCode != 0) {
        return std::nullopt;
    }
    const auto value = Trim(result.stdoutStr);
    if (value.empty()) {
        return std::nullopt;
    }
    return value;
}

auto ProbeGitCredentialManagerVersion(const std::filesystem::path& InRepo) -> shell::ExecResult {
    const auto gitSubcommand = shell::ExecuteCommand("git", {"credential-manager", "--version"}, shell::ExecMode::Capture, InRepo);
    if (gitSubcommand.exitCode == 0) {
        return gitSubcommand;
    }
    return shell::ExecuteCommand("git-credential-manager", {"--version"}, shell::ExecMode::Capture, InRepo);
}

struct CredentialHelperEntry {
    std::string raw;
    std::string origin;
    std::string value;
};

auto ParseCredentialHelperEntry(const std::string& InLine) -> CredentialHelperEntry {
    CredentialHelperEntry entry{.raw = InLine};
    const auto tabPos = InLine.find('\t');
    if (tabPos != std::string::npos) {
        entry.origin = Trim(InLine.substr(0, tabPos));
        entry.value = Trim(InLine.substr(tabPos + 1));
        return entry;
    }

    const auto spacePos = InLine.find_last_of(" \r\n");
    if (spacePos != std::string::npos && spacePos + 1 < InLine.size()) {
        entry.origin = Trim(InLine.substr(0, spacePos));
        entry.value = Trim(InLine.substr(spacePos + 1));
    } else {
        entry.value = Trim(InLine);
    }
    return entry;
}

auto ParseCredentialHelperEntries(const std::vector<std::string>& InHelpers) -> std::vector<CredentialHelperEntry> {
    std::vector<CredentialHelperEntry> entries;
    entries.reserve(InHelpers.size());
    for (const auto& line : InHelpers) {
        entries.push_back(ParseCredentialHelperEntry(line));
    }
    return entries;
}

auto IsStaleCredentialManagerCoreHelper(const CredentialHelperEntry& InEntry) -> bool {
    const auto lowered = ToLower(InEntry.value);
    return lowered == "manager-core" ||
           lowered.find("git-credential-manager-core") != std::string::npos ||
           lowered.find("credential-manager-core") != std::string::npos;
}

auto IsGitCredentialManagerHelper(const CredentialHelperEntry& InEntry) -> bool {
    const auto lowered = ToLower(InEntry.value);
    if (IsStaleCredentialManagerCoreHelper(InEntry)) {
        return false;
    }
    return lowered == "manager" ||
           lowered.find("git-credential-manager") != std::string::npos ||
           lowered.find("credential-manager") != std::string::npos;
}

auto HasGitCredentialManagerHelper(const std::vector<CredentialHelperEntry>& InHelpers) -> bool {
    return std::any_of(InHelpers.begin(), InHelpers.end(), [](const CredentialHelperEntry& entry) {
        return IsGitCredentialManagerHelper(entry);
    });
}

auto HasStaleCredentialManagerCoreHelper(const std::vector<CredentialHelperEntry>& InHelpers) -> bool {
    return std::any_of(InHelpers.begin(), InHelpers.end(), [](const CredentialHelperEntry& entry) {
        return IsStaleCredentialManagerCoreHelper(entry);
    });
}

auto HasAzureDevOpsTarget(const std::vector<AuthTarget>& InTargets) -> bool {
    return std::any_of(InTargets.begin(), InTargets.end(), [](const AuthTarget& target) {
        const auto lowered = ToLower(target.remoteUrl);
        return lowered.find("dev.azure.com") != std::string::npos || lowered.find("visualstudio.com") != std::string::npos;
    });
}

auto GitmodulesUrlForPath(const std::filesystem::path& InWorkspaceRoot, const std::filesystem::path& InRepoPath) -> std::optional<std::string> {
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
                        const auto prefix = key.substr(0, key.size() - 5);
                        const auto urlResult = GitCapture(current, {"config", "-f", ".gitmodules", "--get", prefix + ".url"});
                        if (urlResult.exitCode == 0) {
                            const auto url = Trim(urlResult.stdoutStr);
                            if (!url.empty()) {
                                return url;
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

auto UpstreamRemote(const std::filesystem::path& InRepo) -> std::string {
    const auto upstreamOut = GitCapture(InRepo, {"rev-parse", "--abbrev-ref", "--symbolic-full-name", "@{upstream}"});
    if (upstreamOut.exitCode != 0) {
        return {};
    }
    const auto upstream = Trim(upstreamOut.stdoutStr);
    const auto slash = upstream.find('/');
    if (slash == std::string::npos) {
        return {};
    }
    return upstream.substr(0, slash);
}

auto SelectSyncRemote(const std::filesystem::path& InWorkspaceRoot,
                      const std::filesystem::path& InRepo,
                      const std::string& InPreferredRemote,
                      bool InIsRegistered,
                      bool InIsRoot,
                      bool InIsDetached) -> RemoteSelectionResult {
    RemoteSelectionResult out;
    const auto remotes = ListRemotes(InRepo);
    out.candidates = remotes;
    if (remotes.empty()) {
        out.error = "REMOTE_NOT_FOUND";
        out.detail = "no remotes configured";
        return out;
    }

    if (!InPreferredRemote.empty() && std::find(remotes.begin(), remotes.end(), InPreferredRemote) != remotes.end()) {
        out.remote = InPreferredRemote;
        out.source = "explicit policy";
    }

    if (out.remote.empty() && InIsRegistered && !InIsRoot) {
        if (const auto gmUrl = GitmodulesUrlForPath(InWorkspaceRoot, InRepo); gmUrl.has_value()) {
            const auto normalizedRegistered = NormalizeRemoteUrlForMatch(*gmUrl);
            for (const auto& remote : remotes) {
                const auto normalizedRemoteUrl = NormalizeRemoteUrlForMatch(RemoteUrl(InRepo, remote));
                if (!normalizedRemoteUrl.empty() && normalizedRemoteUrl == normalizedRegistered) {
                    out.remote = remote;
                    out.source = "registered .gitmodules URL match";
                    break;
                }
            }
        }
    }

    if (out.remote.empty() && std::find(remotes.begin(), remotes.end(), "origin") != remotes.end()) {
        out.remote = "origin";
        out.source = "origin-priority";
    }

    if (out.remote.empty() && remotes.size() == 1) {
        out.remote = remotes.front();
        out.source = "sole-remote";
    }

    if (out.remote.empty() && !InIsDetached) {
        const auto upstreamRemote = UpstreamRemote(InRepo);
        if (!upstreamRemote.empty() && std::find(remotes.begin(), remotes.end(), upstreamRemote) != remotes.end()) {
            out.remote = upstreamRemote;
            out.source = "upstream-remote";
        }
    }

    if (out.remote.empty()) {
        out.error = "REMOTE_SELECTION_AMBIGUOUS";
        std::ostringstream detail;
        detail << "candidate remotes=";
        for (std::size_t i = 0; i < remotes.size(); ++i) {
            if (i > 0) {
                detail << ",";
            }
            detail << remotes[i];
        }
        detail << "; configure explicit sync remote policy or add origin";
        out.detail = detail.str();
        return out;
    }

    for (const auto& remote : remotes) {
        if (remote != out.remote) {
            out.ignoredRemotes.push_back(remote);
        }
    }
    return out;
}

auto DetectRemoteDefaultBranch(const std::filesystem::path& InRepo, const std::string& InRemote) -> std::string {
    const auto remoteHeadShort = GitCapture(InRepo, {"symbolic-ref", "--quiet", "--short", std::format("refs/remotes/{}/HEAD", InRemote)});
    if (remoteHeadShort.exitCode == 0) {
        const auto ref = Trim(remoteHeadShort.stdoutStr);
        const auto marker = InRemote + "/";
        if (ref.starts_with(marker) && ref.size() > marker.size()) {
            return ref.substr(marker.size());
        }
    }

    (void)GitCapture(InRepo, {"remote", "set-head", InRemote, "--auto"});
    const auto remoteHead = GitCapture(InRepo, {"symbolic-ref", "--quiet", "--short", std::format("refs/remotes/{}/HEAD", InRemote)});
    if (remoteHead.exitCode == 0) {
        const auto ref = Trim(remoteHead.stdoutStr);
        const auto marker = InRemote + "/";
        if (ref.starts_with(marker) && ref.size() > marker.size()) {
            return ref.substr(marker.size());
        }
    }

    const auto lsRemote = GitCapture(InRepo, {"ls-remote", "--symref", InRemote, "HEAD"});
    if (lsRemote.exitCode == 0) {
        std::istringstream iss(lsRemote.stdoutStr);
        std::string line;
        while (std::getline(iss, line)) {
            line = Trim(line);
            if (!line.starts_with("ref:")) {
                continue;
            }
            const auto headPos = line.find("\tHEAD");
            if (headPos == std::string::npos) {
                continue;
            }
            const auto refName = Trim(line.substr(4, headPos - 4));
            const std::string prefix = "refs/heads/";
            if (refName.starts_with(prefix) && refName.size() > prefix.size()) {
                return refName.substr(prefix.size());
            }
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

auto IsParentPath(const std::filesystem::path& InParent, const std::filesystem::path& InChild) -> bool {
    const auto parent = InParent.lexically_normal().generic_string();
    const auto child = InChild.lexically_normal().generic_string();
    if (parent.empty() || child.empty() || parent == child) {
        return false;
    }
    return child.rfind(parent + "/", 0) == 0;
}

auto AddUniqueDependency(std::vector<std::filesystem::path>* OutDependencies, const std::filesystem::path& InDependency) -> void {
    if (OutDependencies == nullptr || InDependency.empty()) {
        return;
    }
    const auto dependencyKey = RepoPathKey(InDependency);
    const bool exists = std::any_of(OutDependencies->begin(), OutDependencies->end(), [&](const auto& candidate) {
        return RepoPathKey(candidate) == dependencyKey;
    });
    if (!exists) {
        OutDependencies->push_back(InDependency);
    }
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

auto DiscoverRegisteredPathsRecursive(const std::filesystem::path& InWorkspaceRoot, const std::vector<std::filesystem::path>& InExtraSeeds = {}) -> std::set<std::string> {
    std::set<std::string> out;
    std::vector<std::filesystem::path> queue{InWorkspaceRoot};
    for (const auto& seed : InExtraSeeds) {
        queue.push_back(seed);
    }

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
        bool canFastForwardToHead = GitCapture(InRepo, {"merge-base", "--is-ancestor", InBranch, "HEAD"}).exitCode == 0;
        
        if (canFastForwardToHead) {
            const auto headSha = Trim(GitCapture(InRepo, {"rev-parse", "HEAD"}).stdoutStr);
            const auto branchSha = Trim(GitCapture(InRepo, {"rev-parse", InBranch}).stdoutStr);
            if (!headSha.empty() && headSha != branchSha) {
                if (OutDetail != nullptr) {
                    *OutDetail = "fast-forward local branch to detached HEAD and checkout";
                }
                if (InDryRun) {
                    std::cout << "[DRY RUN] Would run: git branch -f " << InBranch << " HEAD\n";
                    std::cout << "[DRY RUN] Would run: git checkout -q " << InBranch << "\n";
                    return true;
                }
                GitPassThrough(InRepo, {"branch", "-f", InBranch, "HEAD"});
                const auto checkout = GitPassThrough(InRepo, {"checkout", "-q", InBranch});
                if (checkout.exitCode == 0) {
                    return true;
                }
            }
        }

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

auto HasCherryPickInProgress(const std::filesystem::path& InRepo) -> bool {
    return GitCapture(InRepo, {"rev-parse", "-q", "--verify", "CHERRY_PICK_HEAD"}).exitCode == 0;
}

auto MakeDetachedSafetyBranchName(const std::filesystem::path& InRepo) -> std::string {
    auto shortHead = Trim(GitCapture(InRepo, {"rev-parse", "--short", "HEAD"}).stdoutStr);
    if (shortHead.empty()) {
        shortHead = "head";
    }

    const std::string base = std::format("kano/sync-detached-backup-{}", shortHead);
    std::string candidate = base;
    int suffix = 0;
    while (GitCapture(InRepo, {"show-ref", "--verify", "--quiet", std::format("refs/heads/{}", candidate)}).exitCode == 0) {
        suffix += 1;
        candidate = std::format("{}-{}", base, suffix);
    }
    return candidate;
}

auto CommitsToReplayFromDetached(const std::filesystem::path& InRepo, const std::string& InDetachedRef, const std::string& InTargetRef) -> std::vector<std::string> {
    std::vector<std::string> commits;
    const auto mergeBaseOut = GitCapture(InRepo, {"merge-base", InDetachedRef, InTargetRef});
    if (mergeBaseOut.exitCode != 0) {
        return commits;
    }

    const auto mergeBase = Trim(mergeBaseOut.stdoutStr);
    if (mergeBase.empty()) {
        return commits;
    }

    const auto revListOut = GitCapture(InRepo, {"rev-list", "--reverse", std::format("{}..{}", mergeBase, InDetachedRef)});
    if (revListOut.exitCode != 0) {
        return commits;
    }

    std::istringstream iss(revListOut.stdoutStr);
    std::string line;
    while (std::getline(iss, line)) {
        line = Trim(line);
        if (!line.empty()) {
            commits.push_back(line);
        }
    }
    return commits;
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

auto GitmodulesBranchForPathFromBlobRef(
    const std::filesystem::path& InParentRepoPath,
    const std::string& InBlobRef,
    const std::filesystem::path& InChildPath) -> std::optional<std::string> {
    std::error_code ec;
    const auto target = std::filesystem::weakly_canonical(InChildPath);
    const auto parent = std::filesystem::weakly_canonical(InParentRepoPath);
    const auto rel = std::filesystem::relative(target, parent, ec);
    if (ec) {
        return std::nullopt;
    }
    const auto relPath = rel.generic_string();
    const auto blobOpt = std::format("--blob={}", InBlobRef);
    const auto pathResult = GitCapture(InParentRepoPath, {"config", blobOpt, "--get-regexp", "^submodule\\..*\\.path$"});
    if (pathResult.exitCode != 0) {
        return std::nullopt;
    }
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
        const auto branchResult = GitCapture(InParentRepoPath, {"config", blobOpt, "--get", prefix + ".branch"});
        if (branchResult.exitCode == 0) {
            const auto branch = Trim(branchResult.stdoutStr);
            if (!branch.empty()) {
                return branch;
            }
        }
        return std::nullopt;
    }
    return std::nullopt;
}

auto BuildSyncPlans(
    const std::filesystem::path& InRoot,
    const std::string& InPreferredRemote,
    int InMaxDepth,
    bool InNoCache,
    bool InRefreshCache) -> std::pair<std::vector<SyncPlan>, std::string> {
    const auto root = std::filesystem::weakly_canonical(InRoot);

    workspace::DiscoverOptions discoverOptions;
    discoverOptions.rootDir = root;
    discoverOptions.maxDepth = InMaxDepth;
    discoverOptions.useCache = !InNoCache;
    discoverOptions.refreshCache = InRefreshCache;
    discoverOptions.incremental = !InRefreshCache;
    discoverOptions.metadataLevel = "minimal";
    // Recursive sync should cover the full workspace tree so nested repos are not skipped.
    discoverOptions.scope = workspace::DiscoverScope::Full;
    discoverOptions.includeTrustedUnregistered = true;
    auto discovery = workspace::DiscoverRepos(discoverOptions);
    auto discoveredRepos = std::move(discovery.repos);
    auto discoverMode = std::move(discovery.mode);
    if (discoverMode.empty()) {
        discoverMode = "full-scan";
    }

    std::vector<SyncPlan> plans;
    plans.reserve(discoveredRepos.size());

    for (const auto& discoveredRepo : discoveredRepos) {
        const auto repoPath = std::filesystem::weakly_canonical(discoveredRepo.path);
        if (IsFalsePolicy(discoveredRepo.kogSyncPolicy)) {
            plans.push_back(SyncPlan{
                .path = repoPath,
                .type = discoveredRepo.type,
                .remote = {},
                .remoteSelectionSource = "policy-disabled",
                .remoteSelectionError = {},
                .remoteSelectionDetail = {},
                .targetBranch = {},
                .branchSource = "commandPolicy.sync=false / kog-sync=false",
                .kogSyncPolicy = discoveredRepo.kogSyncPolicy,
                .registrationRelativeTo = discoveredRepo.registrationRelativeTo,
                .dependencies = discoveredRepo.dependencies,
            });
            continue;
        }
        const auto current = CurrentBranch(repoPath);
        const bool isRoot = (repoPath == root);
        const bool isRegistered = discoveredRepo.type == "registered";
        const auto remoteSelection = SelectSyncRemote(root, repoPath, InPreferredRemote, isRegistered, isRoot, current.empty());
        const auto remote = remoteSelection.remote;
        if (remote.empty()) {
            plans.push_back(SyncPlan{
                .path = repoPath,
                .type = discoveredRepo.type,
                .remote = {},
                .remoteSelectionSource = remoteSelection.source,
                .remoteSelectionError = remoteSelection.error.empty() ? "missing remote" : remoteSelection.error,
                .remoteSelectionDetail = remoteSelection.detail,
                .targetBranch = {},
                .branchSource = "missing remote",
                .kogSyncPolicy = discoveredRepo.kogSyncPolicy,
                .registrationRelativeTo = discoveredRepo.registrationRelativeTo,
                .dependencies = discoveredRepo.dependencies,
            });
            continue;
        }

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
            plans.push_back(SyncPlan{
                .path = repoPath,
                .type = discoveredRepo.type,
                .remote = remote,
                .remoteSelectionSource = remoteSelection.source,
                .remoteSelectionError = {},
                .remoteSelectionDetail = {},
                .targetBranch = {},
                .branchSource = "unresolved target branch",
                .kogSyncPolicy = discoveredRepo.kogSyncPolicy,
                .registrationRelativeTo = discoveredRepo.registrationRelativeTo,
                .dependencies = discoveredRepo.dependencies,
            });
            continue;
        }

        plans.push_back(SyncPlan{
            .path = repoPath,
            .type = discoveredRepo.type,
            .remote = remote,
            .remoteSelectionSource = remoteSelection.source,
            .remoteSelectionError = {},
            .remoteSelectionDetail = {},
            .targetBranch = targetBranch,
            .branchSource = branchSource,
            .kogSyncPolicy = discoveredRepo.kogSyncPolicy,
            .registrationRelativeTo = discoveredRepo.registrationRelativeTo,
            .dependencies = discoveredRepo.dependencies,
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

    // Refresh registered child branches from parent's remote tracking ref.
    // This allows gitmodules branch updates to be seen even when children run before parent.
    for (auto& plan : plans) {
        if (plan.type != "registered" || plan.targetBranch.empty()) {
            continue;
        }
        const auto parentPath = plan.registrationRelativeTo.empty() || plan.registrationRelativeTo == "."
            ? root
            : std::filesystem::weakly_canonical(plan.registrationRelativeTo);
        std::string parentBranch;
        for (const auto& p : plans) {
            if (std::filesystem::weakly_canonical(p.path) == parentPath) {
                parentBranch = p.targetBranch;
                break;
            }
        }
        if (parentBranch.empty()) {
            parentBranch = CurrentBranch(parentPath);
        }
        if (parentBranch.empty()) {
            continue;
        }
        const auto blobRef = std::format("refs/remotes/{}/{}:.gitmodules", InPreferredRemote, parentBranch);
        const auto remoteRefBranch = GitmodulesBranchForPathFromBlobRef(parentPath, blobRef, plan.path);
        if (remoteRefBranch.has_value() && !remoteRefBranch->empty() && *remoteRefBranch != plan.targetBranch) {
            plan.targetBranch = *remoteRefBranch;
            plan.branchSource = "registered .gitmodules branch (refreshed)";
        }
    }

    return {plans, discoverMode};
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

auto ClassifySyncFailure(const shell::ExecResult& InResult, const std::string& InFallback = "FAILED_SYNC") -> std::string {
    const auto merged = ToLower(InResult.stdoutStr + "\n" + InResult.stderrStr);
    if (merged.find("[kog-timeout]") != std::string::npos ||
        merged.find("process timeout") != std::string::npos ||
        merged.find("process timed out") != std::string::npos ||
        merged.find("timeout exceeded") != std::string::npos) {
        return "SYNC_TIMEOUT";
    }
    if (merged.find("authentication failed") != std::string::npos ||
        merged.find("permission denied") != std::string::npos ||
        merged.find("publickey") != std::string::npos ||
        merged.find("access denied") != std::string::npos ||
        merged.find("unauthorized") != std::string::npos ||
        merged.find("could not read username") != std::string::npos) {
        return "FAILED_AUTH";
    }
    if (merged.find("could not resolve host") != std::string::npos ||
        merged.find("failed to connect") != std::string::npos ||
        merged.find("connection timed out") != std::string::npos ||
        merged.find("network is unreachable") != std::string::npos ||
        merged.find("couldn't connect") != std::string::npos) {
        return "FAILED_CONNECTION";
    }
    if (merged.find("does not appear to be a git repository") != std::string::npos ||
        merged.find("repository not found") != std::string::npos ||
        merged.find("no such remote") != std::string::npos) {
        return "FAILED_MISSING_REMOTE";
    }
    return InFallback;
}

struct SyncSummaryEntry {
    std::filesystem::path repo;
    std::string outcome;
    std::string type;
    std::string remote;
    std::string branch;
    std::string reason;
};

auto MakeSyncSchedulerInputs(const std::vector<SyncPlan>& InPlans) -> std::vector<workspace::RepoOperationInput> {
    std::unordered_map<std::string, std::size_t> byPath;
    byPath.reserve(InPlans.size());
    for (std::size_t idx = 0; idx < InPlans.size(); ++idx) {
        byPath.emplace(RepoPathKey(InPlans[idx].path), idx);
    }

    std::vector<workspace::RepoOperationInput> inputs;
    inputs.reserve(InPlans.size());
    for (const auto& plan : InPlans) {
        inputs.push_back(workspace::RepoOperationInput{
            .id = RepoPathKey(plan.path),
            .path = plan.path,
            .type = plan.type,
            .dependencies = {},
        });
    }

    std::unordered_set<std::string> childrenWithGraphParent;
    for (const auto& child : InPlans) {
        if (child.registrationRelativeTo.empty() || child.registrationRelativeTo == ".") {
            continue;
        }
        const auto parentIt = byPath.find(RepoPathKey(child.registrationRelativeTo));
        const auto childIt = byPath.find(RepoPathKey(child.path));
        if (parentIt == byPath.end() || childIt == byPath.end() || parentIt->second == childIt->second) {
            continue;
        }
        AddUniqueDependency(&inputs[parentIt->second].dependencies, child.path);
        childrenWithGraphParent.insert(RepoPathKey(child.path));
    }

    for (std::size_t parentIdx = 0; parentIdx < InPlans.size(); ++parentIdx) {
        for (std::size_t childIdx = 0; childIdx < InPlans.size(); ++childIdx) {
            if (parentIdx == childIdx) {
                continue;
            }
            if (!IsParentPath(InPlans[parentIdx].path, InPlans[childIdx].path)) {
                continue;
            }
            if (childrenWithGraphParent.contains(RepoPathKey(InPlans[childIdx].path))) {
                continue;
            }
            AddUniqueDependency(&inputs[parentIdx].dependencies, InPlans[childIdx].path);
        }
    }

    return inputs;
}

auto DetectDefaultSyncJobs() -> int {
    const unsigned int cores = std::thread::hardware_concurrency();
    if (cores == 0) {
        return 1;
    }
    return static_cast<int>(cores);
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
    bool InCleanupStaleLocks,
    int InJobs,
    workspace::RepoOperationAggregate* OutAggregate = nullptr,
    bool InAuthPreflight = true,
    bool InCheckGitlinkReachability = true,
    std::optional<unsigned int> InGitCaptureTimeoutMs = std::nullopt) -> int {
    std::vector<SyncPlan> plans;
    std::string mode;
    try {
        // Pre-fetch root to update remote tracking refs so BuildSyncPlans can read refreshed
        // .gitmodules branch config from the remote even when children run before parent.
        if (InRecursive && !InDryRun) {
            (void)GitCapture(InRepoRoot, {"fetch", InRemote, "--prune", "--quiet"});
        }
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
    std::unordered_map<std::string, SyncPlan> plansByPath;
    plansByPath.reserve(plans.size());
    for (const auto& plan : plans) {
        plansByPath.emplace(RepoPathKey(plan.path), plan);
    }

    std::unordered_map<std::string, AuthProbeResult> authPreflightByPath;
    if (InAuthPreflight) {
        authPreflightByPath.reserve(plans.size());
        for (const auto& plan : plans) {
            if (IsFalsePolicy(plan.kogSyncPolicy) || plan.remote.empty()) {
                continue;
            }
            const auto preflight = RunAuthProbe(
                MakeAuthTarget(root, plan.path, plan.remote, plan.remoteSelectionSource, plan.remoteSelectionDetail, false),
                AuthProbeScope::SyncPreflight);
            if (!preflight.skipped || !preflight.success) {
                authPreflightByPath.emplace(RepoPathKey(plan.path), preflight);
            }
        }
    }

    bool shouldBuildSelfCpp = false;
    std::mutex selfBuildMutex;

    auto runOnePlan = [&](const workspace::RepoOperationInput& operation) -> workspace::RepoOperationWorkerResult {
        workspace::RepoOperationWorkerResult result;
        const auto planIt = plansByPath.find(RepoPathKey(operation.path));
        if (planIt == plansByPath.end()) {
            result.status = workspace::RepoOperationStatus::Failed;
            result.exitCode = 1;
            result.failureCategory = "FAILED_SYNC";
            result.message = "repo missing from sync plan";
            return result;
        }

        const auto& plan = planIt->second;
        const auto rel = std::filesystem::relative(plan.path, InRepoRoot).generic_string();
        const auto name = (rel.empty() || rel == ".") ? "." : rel;
        std::ostringstream out;
        std::ostringstream err;
        std::unique_ptr<ScopedEnvOverride> syncTimeoutOverride;
        if (InGitCaptureTimeoutMs.has_value() && *InGitCaptureTimeoutMs > 0) {
            const auto timeoutText = std::to_string(*InGitCaptureTimeoutMs);
            syncTimeoutOverride = std::make_unique<ScopedEnvOverride>(
                std::vector<std::pair<std::string, std::string>>{
                    {"KOG_SHELL_TIMEOUT_MS", timeoutText},
                    {"KOG_SHELL_CAPTURE_TIMEOUT_MS", timeoutText},
                });
        }

        auto finishSuccess = [&](const std::string& outcome, const std::string& reason) {
            result.status = workspace::RepoOperationStatus::Succeeded;
            result.exitCode = 0;
            result.failureCategory.clear();
            result.message = reason;
            result.stdoutText = NormalizeSyncCapturedText(out.str(), SyncOutputSanitizeMode::Human);
            result.stderrText = NormalizeSyncCapturedText(err.str(), SyncOutputSanitizeMode::Human);
            return result;
        };

        auto finishSkipped = [&](const std::string& reason) {
            result.status = workspace::RepoOperationStatus::Skipped;
            result.exitCode = 0;
            result.skipReason = reason;
            result.message = reason;
            result.stdoutText = NormalizeSyncCapturedText(out.str(), SyncOutputSanitizeMode::Human);
            result.stderrText = NormalizeSyncCapturedText(err.str(), SyncOutputSanitizeMode::Human);
            return result;
        };

        auto finishFailed = [&](const std::string& category, const std::string& reason) {
            result.status = workspace::RepoOperationStatus::Failed;
            result.exitCode = 1;
            result.failureCategory = category;
            result.message = reason;
            result.stdoutText = NormalizeSyncCapturedText(out.str(), SyncOutputSanitizeMode::Human);
            result.stderrText = NormalizeSyncCapturedText(err.str(), SyncOutputSanitizeMode::Human);
            return result;
        };

        auto finishBlocked = [&](const std::string& category, const std::string& reason) {
            result.status = workspace::RepoOperationStatus::Blocked;
            result.exitCode = 1;
            result.failureCategory = category;
            result.message = reason;
            result.stdoutText = NormalizeSyncCapturedText(out.str(), SyncOutputSanitizeMode::Human);
            result.stderrText = NormalizeSyncCapturedText(err.str(), SyncOutputSanitizeMode::Human);
            return result;
        };

        shell::ScopedCommandLogCapture commandLogCapture(shell::CommandLogCallbacks{
            .onStdout = [&](const std::string& line) {
                out << line;
            },
            .onStderr = [&](const std::string& line) {
                err << line;
            },
        });
        shell::ScopedConsoleWriteSuppression suppressShellConsoleWrites;

        AppendSyncRepoContext(out, name, plan, InDryRun);

        if (IsFalsePolicy(plan.kogSyncPolicy)) {
            const std::string reason = "commandPolicy.sync=false / kog-sync=false";
            out << "[" << name << "] SKIPPED_BY_POLICY: " << reason << "\n";
            return finishSkipped(reason);
        }

        if (plan.remote.empty()) {
            const std::string reason = plan.remoteSelectionError.empty() ? "no usable sync remote found" : plan.remoteSelectionError;
            err << "[" << name << "] BLOCKED_PRECHECK: " << reason;
            if (!plan.remoteSelectionDetail.empty()) {
                err << " (" << plan.remoteSelectionDetail << ")";
            }
            err << "\n";
            return finishBlocked("BLOCKED_PRECHECK", reason);
        }

        if (const auto authIt = authPreflightByPath.find(RepoPathKey(plan.path)); authIt != authPreflightByPath.end()) {
            out << authIt->second.stdoutText;
            err << authIt->second.stderrText;
            if (!authIt->second.success) {
                return finishBlocked(
                    authIt->second.category.empty() ? "FAILED_AUTH" : authIt->second.category,
                    authIt->second.summary.empty() ? "auth preflight failed" : authIt->second.summary);
            }
        }

        if (plan.targetBranch.empty()) {
            const auto currentBranch = CurrentBranch(plan.path);
            if (currentBranch.empty()) {
                err << "[" << name << "] STABLE_BRANCH_NOT_FOUND: detached repo has no resolvable stable branch\n";
                return finishBlocked("BLOCKED_PRECHECK", "STABLE_BRANCH_NOT_FOUND");
            }
            const std::string reason = "target branch could not be resolved";
            err << "[" << name << "] FAILED_MISSING_REMOTE: " << reason << "\n";
            return finishFailed("FAILED_MISSING_REMOTE", reason);
        }

        std::string targetBranch = plan.targetBranch;
        std::string branchSource = plan.branchSource;
        const bool isSelfRepo = selfRepoRoot.has_value() && std::filesystem::weakly_canonical(plan.path) == *selfRepoRoot;
        const auto headBeforeSync = isSelfRepo ? CurrentHeadCommit(plan.path) : std::string{};
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

        const auto health = workspace::ScanRepoHealth(plan.path, workspace::RepoHealthOptions{
            .checkFetchRemotes = true,
            .checkSubmoduleStatus = true,
            .checkGitlinkReachability = InCheckGitlinkReachability,
            .fetchDryRun = true,
            .fetchRemoteOnly = plan.remote,
            .blockOnDetachedHead = false,
            .blockOnNoUpstream = false,
            .blockOnUnpushedCommits = false,
            .blockOnDirtyWorktree = !InAutoStashLocalChanges,
            .blockOnDirtySubmodule = false,
        });
        if (health.detachedHead) {
            out << "[" << name << "] DETACHED_HEAD: switching to stable branch " << targetBranch << "\n";
            if (health.hasDirtyWorktree) {
                err << "[" << name << "] DETACHED_HEAD_DIRTY_WORKTREE: detached HEAD has local working tree changes\n";
                return finishBlocked("BLOCKED_PRECHECK", "DETACHED_HEAD_DIRTY_WORKTREE");
            }
            const auto hasRemoteStable = GitCapture(plan.path, {"show-ref", "--verify", "--quiet", std::format("refs/remotes/{}/{}", plan.remote, targetBranch)}).exitCode == 0;
            if (!hasRemoteStable) {
                err << "[" << name << "] STABLE_BRANCH_NOT_FOUND: missing remote branch " << plan.remote << "/" << targetBranch << "\n";
                return finishBlocked("BLOCKED_PRECHECK", "STABLE_BRANCH_NOT_FOUND");
            }
            bool detachedHasLocalOnlyCommits = false;
            std::string detachedSafetyBranch;
            const auto localOnlyCountOut = GitCapture(plan.path, {"rev-list", "--count", std::format("{}/{}..HEAD", plan.remote, targetBranch)});
            if (localOnlyCountOut.exitCode == 0) {
                const auto localOnlyCount = Trim(localOnlyCountOut.stdoutStr);
                if (localOnlyCount != "0") {
                    detachedHasLocalOnlyCommits = true;
                    detachedSafetyBranch = MakeDetachedSafetyBranchName(plan.path);
                    out << "[" << name << "] DETACHED_HEAD_UNSAFE_LOCAL_COMMITS: preserving detached commits in backup branch "
                        << detachedSafetyBranch << " and replaying onto " << targetBranch << "\n";
                    if (InDryRun) {
                        out << "[DRY RUN] [" << name << "] Would run: git branch " << detachedSafetyBranch << " HEAD\n";
                    } else {
                        const auto backup = GitCapture(plan.path, {"branch", detachedSafetyBranch, "HEAD"});
                        if (backup.exitCode != 0) {
                            out << backup.stdoutStr;
                            err << backup.stderrStr;
                            err << "[" << name << "] DETACHED_HEAD_BACKUP_FAILED: unable to create backup branch for detached commits\n";
                            return finishBlocked("BLOCKED_PRECHECK", "DETACHED_HEAD_BACKUP_FAILED");
                        }
                    }
                }
            }
            std::string repairDetail;
            if (!CheckoutRecoveredBranch(plan.path, plan.remote, targetBranch, BranchMode::Default, InDryRun, &repairDetail)) {
                err << "[" << name << "] BRANCH_REPAIR_FAILED: unable to attach detached HEAD to stable branch " << targetBranch << "\n";
                return finishBlocked("BLOCKED_PRECHECK", "BRANCH_REPAIR_FAILED");
            }
            out << "[" << name << "] DETACHED_HEAD: repaired to stable branch " << targetBranch << " (" << repairDetail << ")\n";
            if (detachedHasLocalOnlyCommits) {
                const auto commitsToReplay = CommitsToReplayFromDetached(plan.path, detachedSafetyBranch, "HEAD");
                if (commitsToReplay.empty()) {
                    out << "[" << name << "] DETACHED_HEAD_REPLAY: no detached commits needed replay after branch repair\n";
                } else if (InDryRun) {
                    out << "[DRY RUN] [" << name << "] Would run: git cherry-pick <" << commitsToReplay.size()
                        << " commit(s)> from " << detachedSafetyBranch << " onto " << targetBranch << "\n";
                } else {
                    std::vector<std::string> cherryPickArgs{"cherry-pick"};
                    cherryPickArgs.insert(cherryPickArgs.end(), commitsToReplay.begin(), commitsToReplay.end());
                    const auto replay = GitCapture(plan.path, cherryPickArgs);
                    if (replay.exitCode != 0) {
                        out << replay.stdoutStr;
                        err << replay.stderrStr;
                        if (HasCherryPickInProgress(plan.path)) {
                            err << "[" << name << "] DETACHED_HEAD_REPLAY_CONFLICT: cherry-pick conflict detected; aborting replay\n";
                            const auto abortCherryPick = GitCapture(plan.path, {"cherry-pick", "--abort"});
                            out << abortCherryPick.stdoutStr;
                            err << abortCherryPick.stderrStr;
                            if (abortCherryPick.exitCode != 0) {
                                err << "WARN: failed to abort cherry-pick replay for " << name << "\n";
                            }
                            return finishBlocked("BLOCKED_PRECHECK", "DETACHED_HEAD_REPLAY_CONFLICT");
                        }
                        err << "[" << name << "] DETACHED_HEAD_REPLAY_FAILED: failed to replay detached commits\n";
                        return finishBlocked("BLOCKED_PRECHECK", "DETACHED_HEAD_REPLAY_FAILED");
                    }
                    out << "[" << name << "] DETACHED_HEAD_REPLAY: replayed " << commitsToReplay.size()
                        << " commit(s) from " << detachedSafetyBranch << " onto " << targetBranch << "\n";
                }
            }
        }

        std::vector<workspace::RepoBlocker> effectiveBlockers;
        effectiveBlockers.reserve(health.blockers.size());
        for (const auto& blocker : health.blockers) {
            if (blocker.kind == workspace::RepoBlockerKind::BranchDiverged) {
                out << "[" << name << "] INFO: " << blocker.detail << "; continuing with fetch/rebase sync flow\n";
                continue;
            }
            effectiveBlockers.push_back(blocker);
        }

        if (!effectiveBlockers.empty()) {
            for (const auto& blocker : effectiveBlockers) {
                err << "[" << name << "] " << blocker.reasonCode << ": " << blocker.detail << "\n";
            }
            return finishBlocked("BLOCKED_PRECHECK", "repo health preflight detected blocking conditions");
        }

        const auto upstreamOut = GitCapture(plan.path, {"rev-parse", "--abbrev-ref", "--symbolic-full-name", "@{upstream}"});
        if (upstreamOut.exitCode != 0) {
            const auto hasRemoteStable = GitCapture(plan.path, {"show-ref", "--verify", "--quiet", std::format("refs/remotes/{}/{}", plan.remote, targetBranch)}).exitCode == 0;
            const auto currentBranch = CurrentBranch(plan.path);
            if (!currentBranch.empty() && currentBranch == targetBranch && hasRemoteStable) {
                if (InDryRun) {
                    out << "[DRY RUN] [" << name << "] NO_UPSTREAM: would set upstream to " << plan.remote << "/" << targetBranch << "\n";
                } else {
                    const auto setUpstream = GitCapture(plan.path, {"branch", "--set-upstream-to", std::format("{}/{}", plan.remote, targetBranch), targetBranch});
                    if (setUpstream.exitCode != 0) {
                        err << "[" << name << "] UPSTREAM_REPAIR_FAILED: failed to set upstream " << plan.remote << "/" << targetBranch << "\n";
                        return finishBlocked("BLOCKED_PRECHECK", "UPSTREAM_REPAIR_FAILED");
                    }
                    out << "[" << name << "] NO_UPSTREAM: setting upstream to " << plan.remote << "/" << targetBranch << "\n";
                }
            } else if (!health.detachedHead) {
                err << "[" << name << "] NO_UPSTREAM: branch has no upstream tracking ref\n";
                return finishBlocked("BLOCKED_PRECHECK", "NO_UPSTREAM");
            }
        }

        if (hasLocalChanges) {
            if (InAutoStashLocalChanges) {
                if (InDryRun) {
                    out << "[DRY RUN] Would run: git stash push -u -m kano-native-sync-autostash";
                    if (!reservedPaths.empty()) {
                        out << " -- .";
                        for (const auto& path : reservedPaths) {
                            out << " :(exclude)" << path;
                        }
                    }
                    out << "\n";
                    stashCreated = true;
                } else {
                    if (!reservedPaths.empty()) {
                        err << "[kog sync] warning: skipped Windows reserved path(s) in " << plan.path.generic_string() << "\n";
                    }
                    auto stash = GitCapture(plan.path, stashArgs);
                    std::optional<IndexLockDiagnosis> indexLockDiagnosis;
                    if (stash.exitCode != 0 && IsIndexLockFailure(stash)) {
                        const auto diagnosis = DiagnoseIndexLock(plan.path);
                        indexLockDiagnosis = diagnosis;
                        if (InCleanupStaleLocks) {
                            if (TryCleanupStaleIndexLock(name, diagnosis)) {
                                stash = GitCapture(plan.path, stashArgs);
                            }
                        }
                    }
                    if (stash.exitCode != 0 && !indexLockDiagnosis.has_value()) {
                        const auto diagnosis = DiagnoseIndexLock(plan.path);
                        if (diagnosis.lockExists) {
                            indexLockDiagnosis = diagnosis;
                            if (InCleanupStaleLocks && !diagnosis.activeGitProcessDetected) {
                                if (TryCleanupStaleIndexLock(name, diagnosis)) {
                                    stash = GitCapture(plan.path, stashArgs);
                                }
                            }
                        }
                    }
                    if (stash.exitCode != 0) {
                        err << "[" << name << "] FAILED_SYNC: failed to auto-stash local changes\n";
                        if (indexLockDiagnosis.has_value()) {
                            PrintIndexLockDiagnosis(name, *indexLockDiagnosis);
                            return finishFailed("FAILED_SYNC", DescribeIndexLockFailure(*indexLockDiagnosis, InCleanupStaleLocks));
                        }
                        return finishFailed("FAILED_SYNC", "auto-stash failed");
                    }

                    const auto stashOut = Trim(stash.stdoutStr + "\n" + stash.stderrStr);
                    stashCreated = stashOut.find("No local changes to save") == std::string::npos;
                    if (stashCreated) {
                        out << "Auto-stashed local changes for " << name << "\n";
                    }
                }
            } else {
                err << "[" << name << "] FAILED_SYNC: local changes detected (auto-stash disabled)\n";
                return finishFailed("FAILED_SYNC", "local changes present and auto-stash disabled");
            }
        }

        if (InDryRun) {
            out << "[DRY RUN] Would run: git fetch " << plan.remote << " --prune --tags\n";
        } else {
            const auto fetch = GitCapture(plan.path, {"fetch", plan.remote, "--prune", "--tags", "--quiet"});
            if (fetch.exitCode != 0) {
                out << fetch.stdoutStr;
                err << fetch.stderrStr;
                const auto category = ClassifySyncFailure(fetch, "FAILED_CONNECTION");
                err << "[" << name << "] " << category << ": fetch failed\n";
                return finishFailed(category, "fetch failed");
            }
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
                out << "WARN: Unregistered repo branch has no remote ref, keeping local branch: " << name << "\n";
            } else {
                std::string tagRef;
                if (TryResolveTagRefForBranch(plan.path, targetBranch, &tagRef)) {
                    checkoutArgs = {"checkout", "-q", "-B", targetBranch, tagRef};
                    out << "INFO: Target branch missing for " << name << "; creating from tag " << tagRef << "\n";
                } else {
                    err << "[" << name << "] FAILED_MISSING_REMOTE: target branch and matching tag not found\n";
                    err << "Target branch not found for " << name << "\n";
                    if (stashCreated && !InDryRun) {
                        const auto pop = GitCapture(plan.path, {"stash", "pop"});
                        out << pop.stdoutStr;
                        err << pop.stderrStr;
                    }
                    return finishFailed("FAILED_MISSING_REMOTE", "target branch and matching tag not found");
                }
            }
        }

        if (InDryRun) {
            out << "[DRY RUN] Would run: git";
            for (const auto& arg : checkoutArgs) {
                out << " " << arg;
            }
            out << "\n";
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
                            out << "[DRY RUN] Skip rebase: local branch is not behind " << rebaseTarget << " (ahead=" << aheadCount << ", behind=" << behindCount << ")\n";
                        }
                    }
                }
                if (shouldRebase) {
                    out << "[DRY RUN] Would run: git rebase " << rebaseTarget << "\n";
                }
            } else {
                out << "[DRY RUN] Skip pull: missing remote branch " << plan.remote << "/" << plan.targetBranch << "\n";
            }
            if (stashCreated) {
                out << "[DRY RUN] Would run: git stash pop\n";
            }
            return finishSuccess("SYNCED_DRY_RUN", "dry-run planned sync");
        }

        const auto checkout = GitCapture(plan.path, checkoutArgs);
        if (checkout.exitCode != 0) {
            out << checkout.stdoutStr;
            err << checkout.stderrStr;
            err << "[" << name << "] FAILED_SYNC: checkout failed\n";
            if (stashCreated) {
                const auto pop = GitCapture(plan.path, {"stash", "pop"});
                out << pop.stdoutStr;
                err << pop.stderrStr;
            }
            return finishFailed("FAILED_SYNC", "checkout failed");
        }

        if (hasRemote) {
            if (HasRebaseInProgress(plan.path)) {
                const auto abort = GitCapture(plan.path, {"rebase", "--abort"});
                out << abort.stdoutStr;
                err << abort.stderrStr;
                if (abort.exitCode != 0) {
                    err << "[" << name << "] SYNC_CONFLICT: rebase state recovery failed\n";
                    return finishFailed("SYNC_CONFLICT", "rebase state recovery failed");
                }
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
                        out << "Skip rebase for " << name << ": local branch is not behind " << rebaseTarget
                            << " (ahead=" << aheadCount << ", behind=" << behindCount << ")\n";
                    }
                }
            }

            if (shouldRebase) {
                const auto rebase = GitCapture(plan.path, {"rebase", "--quiet", rebaseTarget});
                if (rebase.exitCode != 0) {
                    out << rebase.stdoutStr;
                    err << rebase.stderrStr;
                    if (HasRebaseInProgress(plan.path)) {
                        err << "[" << name << "] SYNC_CONFLICT: rebase conflict detected; aborting rebase for manual review\n";
                        const auto abortRebase = GitCapture(plan.path, {"rebase", "--abort"});
                        out << abortRebase.stdoutStr;
                        err << abortRebase.stderrStr;
                        if (abortRebase.exitCode != 0) {
                            err << "WARN: failed to abort rebase after conflict for " << name << "\n";
                        }
                        if (stashCreated) {
                            const auto pop = GitCapture(plan.path, {"stash", "pop"});
                            out << pop.stdoutStr;
                            err << pop.stderrStr;
                        }
                        return finishFailed("SYNC_CONFLICT", "rebase conflict");
                    }
                    const auto category = ClassifySyncFailure(rebase, "FAILED_SYNC");
                    err << "[" << name << "] " << category << ": rebase failed\n";
                    if (stashCreated) {
                        const auto pop = GitCapture(plan.path, {"stash", "pop"});
                        out << pop.stdoutStr;
                        err << pop.stderrStr;
                    }
                    return finishFailed(category, "rebase failed");
                }
                performedRebase = true;
            }
        }

        if (stashCreated) {
            const auto pop = GitCapture(plan.path, {"stash", "pop"});
            out << pop.stdoutStr;
            err << pop.stderrStr;
            if (pop.exitCode != 0) {
                err << "[" << name << "] FAILED_SYNC: stash pop failed after sync\n";
                return finishFailed("FAILED_SYNC", "stash pop failed after sync");
            }
            out << "Restored auto-stash for " << name << "\n";
        }
        if (stashCreated && !performedRebase && !workingTreeSnapshots.empty()) {
            (void)RestoreWorkingTreeFileSnapshots(plan.path, workingTreeSnapshots, name);
        }

        if (isSelfRepo) {
            const auto headAfterSync = CurrentHeadCommit(plan.path);
            if (HeadRangeTouchesSelfCpp(plan.path, headBeforeSync, headAfterSync)) {
                std::lock_guard lock(selfBuildMutex);
                shouldBuildSelfCpp = true;
            }
        }

        out << "[" << name << "] SYNCED (" << plan.remote << ", " << targetBranch << ")\n";
        return finishSuccess("SYNCED", "synced successfully");
    };

    auto schedulerInputs = MakeSyncSchedulerInputs(plans);
    workspace::RepoOperationSchedulerOptions schedulerOptions;
    schedulerOptions.operationName = "sync";
    schedulerOptions.mode = workspace::RepoOperationMode::MutatingDependencyWaves;
    schedulerOptions.jobs = InGitCaptureTimeoutMs.has_value() ? 1 : (InJobs < 1 ? 1 : InJobs);
    schedulerOptions.resolveGitCommonDirLocks = true;

    const auto aggregate = workspace::RunRepoOperationScheduler(
        schedulerInputs,
        schedulerOptions,
        runOnePlan);

    if (OutAggregate != nullptr) {
        *OutAggregate = aggregate;
    }

    for (const auto& result : aggregate.results) {
        if (!result.stdoutText.empty()) {
            std::cout << result.stdoutText;
        }
        if (!result.stderrText.empty()) {
            std::cerr << result.stderrText;
        }
    }

    for (const auto& result : aggregate.results) {
        if (result.status == workspace::RepoOperationStatus::Blocked) {
            std::cout << "[" << result.repoPath.generic_string() << "] BLOCKED_BY_CHILD_FAILURE: "
                      << (result.blockReason.empty() ? "dependency failed in an earlier phase" : result.blockReason) << "\n";
        } else if (result.status == workspace::RepoOperationStatus::Pending) {
            std::cout << "[" << result.repoPath.generic_string() << "] FAILED_SYNC: scheduler did not execute repository\n";
        }
    }

    std::size_t selfBuildFailure = 0;
    std::vector<std::pair<std::string, std::string>> failedDetails;
    std::vector<std::pair<std::string, std::string>> blockedDetails;
    std::vector<std::pair<std::string, std::string>> skippedDetails;
    for (const auto& result : aggregate.results) {
        const auto relative = std::filesystem::relative(result.repoPath, InRepoRoot).generic_string().empty()
            ? std::string{"."}
            : std::filesystem::relative(result.repoPath, InRepoRoot).generic_string();
        if (result.status == workspace::RepoOperationStatus::Failed || result.status == workspace::RepoOperationStatus::Pending) {
            failedDetails.emplace_back(relative,
                result.failureCategory.empty() ? result.message : result.failureCategory + (result.message.empty() ? std::string{} : ": " + result.message));
        } else if (result.status == workspace::RepoOperationStatus::Blocked) {
            blockedDetails.emplace_back(relative,
                result.failureCategory.empty() ? result.message : result.failureCategory + (result.message.empty() ? std::string{} : ": " + result.message));
        } else if (result.status == workspace::RepoOperationStatus::Skipped) {
            skippedDetails.emplace_back(relative,
                result.skipReason.empty() ? result.message : result.skipReason);
        }
    }

    if (!aggregate.hasFailure && shouldBuildSelfCpp && selfRepoRoot.has_value() && !InDryRun) {
        const auto buildCode = RunSelfCppBuild(*selfRepoRoot);
        if (buildCode != 0) {
            selfBuildFailure = 1;
            failedDetails.emplace_back(
                std::filesystem::relative(*selfRepoRoot, InRepoRoot).generic_string().empty()
                    ? std::string{"."}
                    : std::filesystem::relative(*selfRepoRoot, InRepoRoot).generic_string(),
                "self C++ build failed after sync");
        }
    }

    std::cout << "=== " << kano::terminal::Wrap("Sync Complete", kano::terminal::Color::BoldWhite) << " ===\n";
    std::cout << "SUMMARY: repos=" << aggregate.results.size()
              << ", synced=" << aggregate.succeeded
              << ", skipped=" << aggregate.skipped
              << ", blocked=" << aggregate.blocked
              << ", failed=" << (aggregate.failed + aggregate.pending + selfBuildFailure)
              << "\n";
    std::cout << "Succeeded: " << kano::terminal::Wrap(std::to_string(aggregate.succeeded), kano::terminal::Color::BoldGreen) << "\n";
    std::cout << "Skipped: " << kano::terminal::Wrap(std::to_string(aggregate.skipped), kano::terminal::Color::BoldYellow) << "\n";
    std::cout << "Blocked: " << kano::terminal::Wrap(std::to_string(aggregate.blocked), kano::terminal::Color::BoldRed) << "\n";
    std::cout << "Failed: " << kano::terminal::Wrap(std::to_string(aggregate.failed + aggregate.pending + selfBuildFailure), kano::terminal::Color::BoldRed) << "\n";
    if (!failedDetails.empty()) {
        std::cout << "\n=== " << kano::terminal::Wrap("FAILED REPOS", kano::terminal::Color::BoldRed) << " ===\n";
        for (const auto& [repo, reason] : failedDetails) {
            std::cout << kano::terminal::Wrap("[ERROR]", kano::terminal::Color::BoldRed) << " " 
                      << kano::terminal::Wrap(repo, kano::terminal::Color::BoldCyan) << " | " << reason << "\n";
        }
    }
    if (!blockedDetails.empty()) {
        std::cout << "\n=== " << kano::terminal::Wrap("BLOCKED REPOS", kano::terminal::Color::BoldRed) << " ===\n";
        for (const auto& [repo, reason] : blockedDetails) {
            std::cout << kano::terminal::Wrap("[BLOCKED]", kano::terminal::Color::BoldRed) << " "
                      << kano::terminal::Wrap(repo, kano::terminal::Color::BoldCyan) << " | " << reason << "\n";
        }
    }
    if (!skippedDetails.empty()) {
        std::cout << "\n=== " << kano::terminal::Wrap("SKIPPED REPOS", kano::terminal::Color::BoldYellow) << " ===\n";
        for (const auto& [repo, reason] : skippedDetails) {
            std::cout << kano::terminal::Wrap("[SKIPPED]", kano::terminal::Color::BoldYellow) << " "
                      << kano::terminal::Wrap(repo, kano::terminal::Color::BoldCyan) << " | " << reason << "\n";
        }
    }
    return (aggregate.hasFailure || selfBuildFailure > 0) ? 1 : 0;
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

void RegisterAuth(CLI::App& InApp) {
    auto* cmd = InApp.add_subcommand("auth", "Credential manager diagnostics and non-interactive auth probes");

    auto* doctor = cmd->add_subcommand("doctor", "Inspect Git Credential Manager and selected remote auth configuration");
    auto* doctorRepo = new std::string{"."};
    auto* doctorRemote = new std::string{};
    auto* doctorUrl = new std::string{};
    auto* doctorSelectedRemotes = new bool{false};
    auto* doctorAllLocalRemotes = new bool{false};
    auto* doctorNoRecursive = new bool{false};
    auto* doctorNoCache = new bool{false};
    auto* doctorRefreshCache = new bool{false};
    auto* doctorFix = new bool{false};
    doctor->add_option("--repo", *doctorRepo, "Repository root used for config inspection and target discovery");
    doctor->add_option("--remote", *doctorRemote, "Inspect a single configured remote in the current repository");
    doctor->add_option("--url", *doctorUrl, "Inspect an explicit remote URL without storing credentials");
    doctor->add_flag("--selected-remotes", *doctorSelectedRemotes, "Inspect the sync-selected remote for each discovered repo");
    doctor->add_flag("--all-local-remotes", *doctorAllLocalRemotes, "Inspect every configured remote in the current repository");
    doctor->add_flag("--no-recursive,-N", *doctorNoRecursive, "When used with --selected-remotes, inspect only the current repository");
    doctor->add_flag("--native-no-cache", *doctorNoCache, "Disable native discovery cache for --selected-remotes");
    doctor->add_flag("--native-refresh-cache", *doctorRefreshCache, "Force native discovery cache refresh for --selected-remotes");
    doctor->add_flag("--fix", *doctorFix, "Remove stale credential.helper=manager-core entries and configure modern Git Credential Manager if needed");
    doctor->callback([=]() {
        const int selectorCount = (!doctorRemote->empty() ? 1 : 0) + (!doctorUrl->empty() ? 1 : 0) + (*doctorSelectedRemotes ? 1 : 0) + (*doctorAllLocalRemotes ? 1 : 0);
        if (selectorCount > 1) {
            std::cerr << "Error: choose at most one of --remote, --url, --selected-remotes, or --all-local-remotes\n";
            std::exit(2);
        }

        const auto repoRoot = std::filesystem::weakly_canonical(std::filesystem::path(*doctorRepo));
        const auto gitVersion = shell::ExecuteCommand("git", {"--version"}, shell::ExecMode::Capture, repoRoot);
        if (gitVersion.exitCode != 0) {
            std::cerr << kano::terminal::FailTag() << " git is not available in PATH\n";
            std::exit(1);
        }

        std::vector<AuthTarget> targets;
        try {
            targets = CollectAuthTargets(AuthTargetSelectionOptions{
                .repoRoot = repoRoot,
                .explicitRemote = *doctorRemote,
                .explicitUrl = *doctorUrl,
                .selectedRemotes = *doctorSelectedRemotes,
                .allLocalRemotes = *doctorAllLocalRemotes,
                .recursive = !*doctorNoRecursive,
                .noCache = *doctorNoCache,
                .refreshCache = *doctorRefreshCache,
            });
        } catch (const std::exception& ex) {
            std::cerr << "ERROR: failed to collect auth targets: " << ex.what() << "\n";
            std::exit(1);
        }

        auto helperLines = GitConfigGetAllWithOrigin(repoRoot, "credential.helper");
        auto helperEntries = ParseCredentialHelperEntries(helperLines);
        const auto credentialInteractive = GitConfigGetValue(repoRoot, "credential.interactive");
        const auto useHttpPath = GitConfigGetValue(repoRoot, "credential.useHttpPath");
        const auto gcmVersion = ProbeGitCredentialManagerVersion(repoRoot);
        bool hasGcmHelper = HasGitCredentialManagerHelper(helperEntries);
        bool hasStaleManagerCoreHelper = HasStaleCredentialManagerCoreHelper(helperEntries);
        const bool hasHttpsTarget = std::any_of(targets.begin(), targets.end(), [](const AuthTarget& target) {
            const auto protocol = AuthProtocolForUrl(target.remoteUrl);
            return protocol == "http" || protocol == "https";
        });

        int failures = 0;
        int warnings = 0;

        std::cout << kano::terminal::PreflightHeader("Auth Doctor") << "\n";
        std::cout << kano::terminal::PassTag() << " " << Trim(gitVersion.stdoutStr) << "\n";

        if (*doctorFix) {
            if (hasStaleManagerCoreHelper) {
                std::cout << kano::terminal::InfoTag() << " removing stale credential.helper=manager-core entries from local/global config\n";
                const auto unsetLocal = GitCapture(repoRoot, {"config", "--local", "--unset-all", "credential.helper", "manager-core"});
                const auto unsetGlobal = GitCapture(repoRoot, {"config", "--global", "--unset-all", "credential.helper", "manager-core"});
                if (unsetLocal.exitCode != 0 && unsetGlobal.exitCode != 0) {
                    std::cout << kano::terminal::InfoTag() << " no local/global manager-core value was removed; stale value may come from a custom config file\n";
                }
            }

            helperLines = GitConfigGetAllWithOrigin(repoRoot, "credential.helper");
            helperEntries = ParseCredentialHelperEntries(helperLines);
            hasGcmHelper = HasGitCredentialManagerHelper(helperEntries);
            hasStaleManagerCoreHelper = HasStaleCredentialManagerCoreHelper(helperEntries);

            if (hasHttpsTarget && !hasGcmHelper && gcmVersion.exitCode == 0) {
                const auto addManager = GitCapture(repoRoot, {"config", "--global", "--add", "credential.helper", "manager"});
                if (addManager.exitCode == 0) {
                    std::cout << kano::terminal::PassTag() << " configured credential.helper=manager in global Git config\n";
                    helperLines = GitConfigGetAllWithOrigin(repoRoot, "credential.helper");
                    helperEntries = ParseCredentialHelperEntries(helperLines);
                    hasGcmHelper = HasGitCredentialManagerHelper(helperEntries);
                    hasStaleManagerCoreHelper = HasStaleCredentialManagerCoreHelper(helperEntries);
                } else {
                    std::cerr << kano::terminal::FailTag() << " failed to configure credential.helper=manager in global Git config\n";
                    failures += 1;
                }
            }
        }

        if (helperLines.empty()) {
            if (hasHttpsTarget) {
                std::cerr << kano::terminal::FailTag() << " no credential.helper is configured for HTTPS remotes\n";
                failures += 1;
            } else {
                std::cout << kano::terminal::InfoTag() << " no credential.helper configured\n";
            }
        } else {
            std::cout << (hasGcmHelper ? kano::terminal::PassTag() : kano::terminal::WarnTag())
                      << " credential.helper configuration:\n";
            if (!hasGcmHelper && hasHttpsTarget) {
                warnings += 1;
            }
            for (const auto& line : helperLines) {
                std::cout << "  - " << line << "\n";
            }
        }

        if (hasStaleManagerCoreHelper) {
            const auto tag = hasHttpsTarget ? kano::terminal::FailTag() : kano::terminal::WarnTag();
            std::cerr << tag << " stale credential.helper=manager-core is configured; current Git for Windows expects credential.helper=manager\n";
            std::cerr << kano::terminal::InfoTag() << " repair with: kog auth doctor --fix\n";
            for (const auto& entry : helperEntries) {
                if (IsStaleCredentialManagerCoreHelper(entry)) {
                    std::cerr << "  - " << entry.raw << "\n";
                }
            }
            if (hasHttpsTarget) {
                failures += 1;
            } else {
                warnings += 1;
            }
        }

        if (gcmVersion.exitCode == 0) {
            std::cout << kano::terminal::PassTag() << " Git Credential Manager detected: " << Trim(gcmVersion.stdoutStr + gcmVersion.stderrStr) << "\n";
        } else if (hasHttpsTarget) {
            std::cerr << kano::terminal::WarnTag() << " Git Credential Manager executable was not detected via 'git credential-manager --version'\n";
            warnings += 1;
        } else {
            std::cout << kano::terminal::InfoTag() << " Git Credential Manager executable not detected\n";
        }

        if (credentialInteractive.has_value()) {
            std::cout << kano::terminal::InfoTag() << " credential.interactive=" << *credentialInteractive << "\n";
        } else {
            std::cout << kano::terminal::InfoTag() << " credential.interactive is unset\n";
        }

        if (useHttpPath.has_value()) {
            std::cout << kano::terminal::InfoTag() << " credential.useHttpPath=" << *useHttpPath << "\n";
        } else {
            std::cout << kano::terminal::InfoTag() << " credential.useHttpPath is unset\n";
        }

        if (HasAzureDevOpsTarget(targets) && (!useHttpPath.has_value() || !IsTruthyValue(*useHttpPath))) {
            std::cerr << kano::terminal::WarnTag() << " Azure DevOps remotes usually need credential.useHttpPath=true for reliable account matching\n";
            warnings += 1;
        }

        for (const auto& target : targets) {
            const auto protocol = AuthProtocolForUrl(target.remoteUrl);
            const auto redactedUrl = RedactUrlCredentials(Trim(target.remoteUrl));
            const auto remoteDisplay = target.explicitUrl ? std::string("<url>") : (target.remoteName.empty() ? std::string("(none)") : target.remoteName);
            if (target.remoteName.empty() && target.remoteUrl.empty()) {
                std::cerr << kano::terminal::FailTag() << " [" << target.repoLabel << "] no usable auth target: "
                          << (target.detail.empty() ? "missing remote" : target.detail) << "\n";
                failures += 1;
                continue;
            }
            std::cout << kano::terminal::InfoTag() << " [" << target.repoLabel << "] remote=" << remoteDisplay
                      << " protocol=" << protocol;
            if (!redactedUrl.empty()) {
                std::cout << " url=" << redactedUrl;
            }
            if (!target.source.empty()) {
                std::cout << " source=" << target.source;
            }
            std::cout << "\n";

            if ((protocol == "http" || protocol == "https") && !hasGcmHelper) {
                std::cerr << kano::terminal::FailTag() << " [" << target.repoLabel << "] HTTPS remote is selected but no Git Credential Manager helper is configured\n";
                failures += 1;
            }
        }

        std::cout << kano::terminal::PreflightHeader("Auth Doctor Complete") << "\n";
        std::cout << "failures=" << failures << " warnings=" << warnings << " targets=" << targets.size() << "\n";
        std::exit(failures == 0 ? 0 : 1);
    });

    auto* test = cmd->add_subcommand("test", "Run a non-interactive git ls-remote auth probe");
    auto* testRepo = new std::string{"."};
    auto* testRemote = new std::string{};
    auto* testUrl = new std::string{};
    auto* testSelectedRemotes = new bool{false};
    auto* testAllLocalRemotes = new bool{false};
    auto* testNoRecursive = new bool{false};
    auto* testNoCache = new bool{false};
    auto* testRefreshCache = new bool{false};
    test->add_option("--repo", *testRepo, "Repository root used for target discovery and ls-remote working directory");
    test->add_option("--remote", *testRemote, "Probe one configured remote in the current repository");
    test->add_option("--url", *testUrl, "Probe an explicit remote URL without storing credentials");
    test->add_flag("--selected-remotes", *testSelectedRemotes, "Probe the sync-selected remote for each discovered repo");
    test->add_flag("--all-local-remotes", *testAllLocalRemotes, "Probe every configured remote in the current repository");
    test->add_flag("--no-recursive,-N", *testNoRecursive, "When used with --selected-remotes, probe only the current repository");
    test->add_flag("--native-no-cache", *testNoCache, "Disable native discovery cache for --selected-remotes");
    test->add_flag("--native-refresh-cache", *testRefreshCache, "Force native discovery cache refresh for --selected-remotes");
    test->callback([=]() {
        const int selectorCount = (!testRemote->empty() ? 1 : 0) + (!testUrl->empty() ? 1 : 0) + (*testSelectedRemotes ? 1 : 0) + (*testAllLocalRemotes ? 1 : 0);
        if (selectorCount > 1) {
            std::cerr << "Error: choose at most one of --remote, --url, --selected-remotes, or --all-local-remotes\n";
            std::exit(2);
        }

        std::vector<AuthTarget> targets;
        try {
            targets = CollectAuthTargets(AuthTargetSelectionOptions{
                .repoRoot = std::filesystem::weakly_canonical(std::filesystem::path(*testRepo)),
                .explicitRemote = *testRemote,
                .explicitUrl = *testUrl,
                .selectedRemotes = *testSelectedRemotes,
                .allLocalRemotes = *testAllLocalRemotes,
                .recursive = !*testNoRecursive,
                .noCache = *testNoCache,
                .refreshCache = *testRefreshCache,
            });
        } catch (const std::exception& ex) {
            std::cerr << "ERROR: failed to collect auth targets: " << ex.what() << "\n";
            std::exit(1);
        }

        int passed = 0;
        int skipped = 0;
        int failed = 0;
        std::cout << kano::terminal::PreflightHeader("Auth Test") << "\n";
        for (const auto& target : targets) {
            const auto probe = RunAuthProbe(target, AuthProbeScope::CommandTest);
            std::cout << probe.stdoutText;
            std::cerr << probe.stderrText;
            if (probe.skipped) {
                skipped += 1;
            } else if (probe.success) {
                passed += 1;
            } else {
                failed += 1;
            }
        }
        std::cout << kano::terminal::PreflightHeader("Auth Test Complete") << "\n";
        std::cout << "passed=" << passed << " skipped=" << skipped << " failed=" << failed << "\n";
        std::exit(failed == 0 ? 0 : 1);
    });
}

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
    auto* originLatestNoAuthPreflight = new bool{false};
    auto* originLatestCleanupStaleLocks = new bool{false};
    auto* originLatestJobs = new int{DetectDefaultSyncJobs()};
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
    origin_latest->add_flag("--no-auth-preflight", *originLatestNoAuthPreflight, "Skip Git Credential Manager-focused non-interactive auth preflight before sync");
    origin_latest->add_flag("--cleanup-stale-locks", *originLatestCleanupStaleLocks, "When auto-stash fails on index.lock and no git/kano-git process is active, remove the stale lock and retry once");
    origin_latest->add_option("--jobs", *originLatestJobs, "Number of parallel repo workers for recursive sync (default: CPU cores)");
    origin_latest->add_flag("--profile", *originLatestProfile, "Print native sync timing/profile summary");

    origin_latest->callback([=]() {
        auto extras = origin_latest->remaining();
        if (*originLatestShell) {
            std::cerr << "Error: --shell is no longer supported; sync origin-latest is fully native now\n";
            std::exit(2);
        }
        if (*originLatestJobs < 1) {
            std::cerr << "Error: --jobs must be a positive integer\n";
            std::exit(1);
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
            *originLatestCleanupStaleLocks,
            *originLatestJobs,
            nullptr,
            !*originLatestNoAuthPreflight);
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
    auto* devNoAuthPreflight = new bool{false};
    auto* devCleanupStaleLocks = new bool{false};
    auto* devJobs = new int{DetectDefaultSyncJobs()};
    auto* devProfile = new bool{false};
    dev->add_option("--repo", *devRepo, "Target repository root path");
    dev->add_flag("--dry-run", *devDryRun, "Preview sync actions without modifying repositories");
    dev->add_option("--native-max-depth", *devMaxDepth, "Native discovery max depth (0 = unlimited)");
    dev->add_flag("--native-no-cache", *devNoCache, "Disable native discovery cache");
    dev->add_flag("--native-refresh-cache", *devRefreshCache, "Force native cache refresh");
    dev->add_flag("--no-recursive,-N", *devNoRecursive, "Sync only current repository");
    dev->add_flag("--no-auth-preflight", *devNoAuthPreflight, "Skip Git Credential Manager-focused non-interactive auth preflight before sync");
    dev->add_flag("--cleanup-stale-locks", *devCleanupStaleLocks, "When auto-stash fails on index.lock and no git/kano-git process is active, remove the stale lock and retry once");
    dev->add_option("--jobs", *devJobs, "Number of parallel repo workers for recursive sync (default: CPU cores)");
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
        if (*devJobs < 1) {
            std::cerr << "Error: --jobs must be a positive integer\n";
            std::exit(1);
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
            *devCleanupStaleLocks,
            *devJobs,
            nullptr,
            !*devNoAuthPreflight);
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
            false,
            1);
        std::exit(code);
    });

    // --- sync (default: auto-detect) ---
    auto* defaultNoRecursive = new bool{false};
    auto* defaultNoAuthPreflight = new bool{false};
    auto* defaultCleanupStaleLocks = new bool{false};
    auto* defaultJobs = new int{DetectDefaultSyncJobs()};
    auto* defaultProfile = new bool{false};
    cmd->add_flag("--no-recursive,-N", *defaultNoRecursive, "Default sync: only current repository");
    cmd->add_flag("--no-auth-preflight", *defaultNoAuthPreflight, "Default sync: skip Git Credential Manager-focused non-interactive auth preflight");
    cmd->add_flag("--cleanup-stale-locks", *defaultCleanupStaleLocks, "Default sync: remove stale index.lock automatically when no git/kano-git process is active");
    cmd->add_option("--jobs", *defaultJobs, "Default sync: number of parallel repo workers for recursive sync (default: CPU cores)");
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
            if (*defaultJobs < 1) {
                std::cerr << "Error: --jobs must be a positive integer\n";
                std::exit(1);
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
                *defaultCleanupStaleLocks,
                *defaultJobs,
                nullptr,
                !*defaultNoAuthPreflight);
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

auto RunSyncOriginLatestNativeDetailed(const std::filesystem::path& InRepoRoot,
                                       bool InRecursive,
                                       bool InDryRun,
                                       bool InCleanupStaleLocks,
                                       bool InCheckGitlinkReachability,
                                       std::optional<unsigned int> InGitCaptureTimeoutMs) -> std::pair<int, workspace::RepoOperationAggregate>;

auto RunSyncOriginLatestNative(const std::filesystem::path& InRepoRoot,
                               const bool InRecursive,
                               const bool InDryRun,
                               const bool InCleanupStaleLocks,
                               const bool InCheckGitlinkReachability,
                               std::optional<unsigned int> InGitCaptureTimeoutMs) -> int {
    const auto detailed = RunSyncOriginLatestNativeDetailed(
        InRepoRoot,
        InRecursive,
        InDryRun,
        InCleanupStaleLocks,
        InCheckGitlinkReachability,
        InGitCaptureTimeoutMs);
    return detailed.first;
}

auto RunSyncOriginLatestNativeDetailed(const std::filesystem::path& InRepoRoot,
                                       const bool InRecursive,
                                       const bool InDryRun,
                                       const bool InCleanupStaleLocks,
                                       const bool InCheckGitlinkReachability,
                                       std::optional<unsigned int> InGitCaptureTimeoutMs) -> std::pair<int, workspace::RepoOperationAggregate> {
    workspace::RepoOperationAggregate aggregate;
    const auto code = RunNativeOriginLatestSync(
        InRepoRoot,
        "origin",
        12,
        InDryRun,
        false,
        false,
        InRecursive,
        true,
        InCleanupStaleLocks,
        1,
        &aggregate,
        true,
        InCheckGitlinkReachability,
        InGitCaptureTimeoutMs);
    return {code, std::move(aggregate)};
}

} // namespace kano::git::commands
