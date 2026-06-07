// submodule command — Git submodule management

#include <CLI/CLI.hpp>
#include "discovery.hpp"
#include "shell_executor.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <map>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

#include <chrono>
#include <thread>

namespace {

struct SyncUrlEntry {
    std::string name;
    std::string path;
    std::string url;
};

struct SubmoduleUpdateTask {
    std::string displayPath;
    std::filesystem::path repoRoot;
    std::string localPath;
};

enum class SubmoduleUpdateOutcomeKind {
    UpdatedCleanly,
    UpdatedWithWarnings,
    RepairedAndUpdated,
    Failed,
    Blocked,
};

struct SubmoduleWarning {
    std::string code;
    std::string message;
};

struct SubmoduleIssue {
    std::string code;
    std::string message;
};

struct SubmoduleUpdateOutcome {
    SubmoduleUpdateTask task;
    SubmoduleUpdateOutcomeKind kind = SubmoduleUpdateOutcomeKind::Failed;
    std::vector<SubmoduleWarning> warnings;
    std::vector<SubmoduleIssue> issues;
    bool attemptedRepair = false;
};

struct SubmoduleRepairSafety {
    bool safe = false;
    std::string reason;
    std::filesystem::path modulePath;
};

auto DidSubmoduleUpdateSucceed(const std::filesystem::path& InRepoRoot, const std::string& InLocalPath) -> bool;
auto CollectRegisteredSubmodulePaths(const std::filesystem::path& InRepoRoot) -> std::vector<std::string>;
auto CollectNestedSubmoduleTasks(
    const std::filesystem::path& InWorkspaceRoot,
    const std::string& InParentDisplayPath) -> std::vector<SubmoduleUpdateTask>;

struct ParsedSubmoduleAddArgs {
    std::string url;
    std::string path;
    std::string branch;
};

enum class RemoteHeadProbe {
    HasHead,
    MissingHead,
    ProbeError,
};

struct ScopedTempDir {
    std::filesystem::path path;

    ScopedTempDir() = default;
    ScopedTempDir(const ScopedTempDir&) = delete;
    auto operator=(const ScopedTempDir&) -> ScopedTempDir& = delete;

    ScopedTempDir(ScopedTempDir&& InOther) noexcept
        : path(std::move(InOther.path)) {
        InOther.path.clear();
    }

    auto operator=(ScopedTempDir&& InOther) noexcept -> ScopedTempDir& {
        if (this == &InOther) {
            return *this;
        }
        std::error_code ec;
        if (!path.empty()) {
            std::filesystem::remove_all(path, ec);
        }
        path = std::move(InOther.path);
        InOther.path.clear();
        return *this;
    }

    ~ScopedTempDir() {
        if (path.empty()) {
            return;
        }
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
};

auto Trim(const std::string& InValue) -> std::string {
    const auto start = InValue.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) {
        return {};
    }
    const auto end = InValue.find_last_not_of(" \t\r\n");
    return InValue.substr(start, end - start + 1);
}

auto ToLower(std::string InValue) -> std::string {
    std::transform(InValue.begin(), InValue.end(), InValue.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return InValue;
}

auto ContainsCaseInsensitive(const std::string& InText, const std::string& InNeedle) -> bool {
    if (InNeedle.empty()) {
        return true;
    }
    return ToLower(InText).find(ToLower(InNeedle)) != std::string::npos;
}

auto SplitLines(const std::string& InText) -> std::vector<std::string> {
    std::vector<std::string> out;
    std::istringstream iss(InText);
    std::string line;
    while (std::getline(iss, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        out.push_back(line);
    }
    return out;
}

auto RunGitCapture(
    const std::vector<std::string>& InGitArgs,
    std::optional<std::filesystem::path> InWorkingDir = std::nullopt) -> kano::git::shell::ExecResult {
    return kano::git::shell::ExecuteCommand("git", InGitArgs, kano::git::shell::ExecMode::Capture, InWorkingDir);
}

auto RunGitPassThrough(
    const std::vector<std::string>& InGitArgs,
    std::optional<std::filesystem::path> InWorkingDir = std::nullopt) -> kano::git::shell::ExecResult {
    return kano::git::shell::ExecuteCommand("git", InGitArgs, kano::git::shell::ExecMode::PassThrough, InWorkingDir);
}

void PrintDryRunGit(const std::vector<std::string>& InGitArgs) {
    std::cout << "[DRY-RUN] Would execute: git";
    for (const auto& part : InGitArgs) {
        std::cout << " " << part;
    }
    std::cout << "\n";
}

auto ExtractSubmoduleNameFromUrlKey(const std::string& InKey) -> std::string {
    constexpr std::string_view prefix = "submodule.";
    constexpr std::string_view suffix = ".url";
    if (!InKey.starts_with(prefix) || !InKey.ends_with(suffix) || InKey.size() <= prefix.size() + suffix.size()) {
        return {};
    }
    return InKey.substr(prefix.size(), InKey.size() - prefix.size() - suffix.size());
}

auto IsSubmoduleAddOptionWithValue(const std::string& InToken) -> bool {
    return InToken == "-b" ||
           InToken == "--branch" ||
           InToken == "--name" ||
           InToken == "--reference" ||
           InToken == "--depth" ||
           InToken == "--filter" ||
           InToken == "--ref-format" ||
           InToken == "--jobs";
}

auto IsSubmoduleAddOptionWithoutValue(const std::string& InToken) -> bool {
    return InToken == "-f" ||
           InToken == "--force" ||
           InToken == "--progress" ||
           InToken == "--quiet" ||
           InToken == "--dissociate";
}

auto ParseInlineOptionValue(const std::string& InToken, const std::string& InPrefix) -> std::optional<std::string> {
    if (!InToken.starts_with(InPrefix)) {
        return std::nullopt;
    }
    return InToken.substr(InPrefix.size());
}

auto TryParseSubmoduleAddArgs(const std::vector<std::string>& InArgs) -> std::optional<ParsedSubmoduleAddArgs> {
    ParsedSubmoduleAddArgs parsed;
    std::vector<std::string> positional;
    bool positionalOnly = false;

    for (std::size_t index = 0; index < InArgs.size(); ++index) {
        const auto& token = InArgs[index];
        if (positionalOnly) {
            positional.push_back(token);
            continue;
        }
        if (token == "--") {
            positionalOnly = true;
            continue;
        }
        if (token.empty()) {
            continue;
        }
        if (!token.starts_with("-")) {
            positional.push_back(token);
            continue;
        }
        if (const auto branchValue = ParseInlineOptionValue(token, "--branch="); branchValue.has_value()) {
            parsed.branch = *branchValue;
            continue;
        }
        if (const auto ignoredValue = ParseInlineOptionValue(token, "--name="); ignoredValue.has_value()) {
            continue;
        }
        if (const auto ignoredValue = ParseInlineOptionValue(token, "--reference="); ignoredValue.has_value()) {
            continue;
        }
        if (const auto ignoredValue = ParseInlineOptionValue(token, "--depth="); ignoredValue.has_value()) {
            continue;
        }
        if (const auto ignoredValue = ParseInlineOptionValue(token, "--filter="); ignoredValue.has_value()) {
            continue;
        }
        if (const auto ignoredValue = ParseInlineOptionValue(token, "--ref-format="); ignoredValue.has_value()) {
            continue;
        }
        if (const auto ignoredValue = ParseInlineOptionValue(token, "--jobs="); ignoredValue.has_value()) {
            continue;
        }
        if (IsSubmoduleAddOptionWithoutValue(token)) {
            continue;
        }
        if (IsSubmoduleAddOptionWithValue(token)) {
            if (index + 1 >= InArgs.size()) {
                return std::nullopt;
            }
            const auto& optionValue = InArgs[++index];
            if (token == "-b" || token == "--branch") {
                parsed.branch = optionValue;
            }
            continue;
        }
        return std::nullopt;
    }

    if (positional.empty()) {
        return std::nullopt;
    }

    parsed.url = positional.front();
    if (positional.size() >= 2) {
        parsed.path = positional[1];
    }
    return parsed;
}

auto ProbeRemoteHeadState(const std::string& InUrl) -> RemoteHeadProbe {
    const auto result = RunGitCapture({"ls-remote", "--exit-code", InUrl, "HEAD"});
    if (result.exitCode == 0) {
        return RemoteHeadProbe::HasHead;
    }
    if (result.exitCode == 2) {
        return RemoteHeadProbe::MissingHead;
    }
    return RemoteHeadProbe::ProbeError;
}

auto ResolveDefaultBootstrapBranch(const std::optional<ParsedSubmoduleAddArgs>& InParsed) -> std::string {
    if (InParsed.has_value() && !InParsed->branch.empty()) {
        return InParsed->branch;
    }
    const auto configResult = RunGitCapture({"config", "--get", "init.defaultBranch"});
    const auto configured = configResult.exitCode == 0 ? Trim(configResult.stdoutStr) : std::string{};
    if (!configured.empty()) {
        return configured;
    }
    return "main";
}

auto DeriveRepoDisplayName(const std::string& InUrl) -> std::string {
    auto normalized = Trim(InUrl);
    while (!normalized.empty() && (normalized.back() == '/' || normalized.back() == '\\')) {
        normalized.pop_back();
    }
    if (normalized.ends_with(".git")) {
        normalized.resize(normalized.size() - 4);
    }
    const auto slashPos = normalized.find_last_of("/\\");
    const auto colonPos = normalized.find_last_of(':');
    const auto splitPos = std::max(slashPos, colonPos);
    if (splitPos == std::string::npos || splitPos + 1 >= normalized.size()) {
        return normalized.empty() ? std::string{"submodule"} : normalized;
    }
    return normalized.substr(splitPos + 1);
}

auto MakeBootstrapTempDir() -> std::optional<ScopedTempDir> {
    const auto uniqueId = std::to_string(std::chrono::high_resolution_clock::now().time_since_epoch().count());
    auto tempPath = std::filesystem::temp_directory_path() / ("kog-submodule-bootstrap-" + uniqueId);
    std::error_code ec;
    std::filesystem::create_directories(tempPath, ec);
    if (ec) {
        return std::nullopt;
    }
    ScopedTempDir scoped;
    scoped.path = tempPath.lexically_normal();
    return scoped;
}

auto TrySetLocalBareRemoteHead(const std::string& InUrl, const std::string& InBranch) -> void {
    if (InUrl.empty() || InBranch.empty()) {
        return;
    }
    const std::filesystem::path remotePath = std::filesystem::path(InUrl).lexically_normal();
    if (!std::filesystem::exists(remotePath)) {
        return;
    }
    const auto headFile = (remotePath / "HEAD").lexically_normal();
    const auto objectsDir = (remotePath / "objects").lexically_normal();
    if (!std::filesystem::exists(headFile) || !std::filesystem::exists(objectsDir)) {
        return;
    }
    (void)RunGitCapture({"--git-dir", remotePath.string(), "symbolic-ref", "HEAD", "refs/heads/" + InBranch});
}

auto BootstrapEmptyRemoteForSubmodule(const ParsedSubmoduleAddArgs& InParsed, const std::string& InBranch) -> int {
    const auto tempDir = MakeBootstrapTempDir();
    if (!tempDir.has_value()) {
        std::cerr << "Error: failed to create temporary bootstrap directory\n";
        return 1;
    }

    const auto repoName = DeriveRepoDisplayName(InParsed.url);
    const auto readmePath = (tempDir->path / "README.md").lexically_normal();
    {
        std::ofstream out(readmePath, std::ios::out | std::ios::binary | std::ios::trunc);
        if (!out.good()) {
            std::cerr << "Error: failed to write bootstrap README.md\n";
            return 1;
        }
        out << "# " << (repoName.empty() ? std::string{"submodule"} : repoName) << "\n";
    }

    const auto initResult = RunGitPassThrough({"-C", tempDir->path.string(), "-c", "init.defaultBranch=" + InBranch, "init"});
    if (initResult.exitCode != 0) {
        return initResult.exitCode;
    }
    const auto addResult = RunGitPassThrough({"-C", tempDir->path.string(), "add", "README.md"});
    if (addResult.exitCode != 0) {
        return addResult.exitCode;
    }
    const auto commitResult = RunGitPassThrough(
        {
            "-C", tempDir->path.string(),
            "-c", "user.name=kog bootstrap",
            "-c", "user.email=kog-bootstrap@example.invalid",
            "commit", "-m", "Initial commit"
        }
    );
    if (commitResult.exitCode != 0) {
        return commitResult.exitCode;
    }
    const auto remoteResult = RunGitPassThrough({"-C", tempDir->path.string(), "remote", "add", "origin", InParsed.url});
    if (remoteResult.exitCode != 0) {
        return remoteResult.exitCode;
    }
    const auto pushResult = RunGitPassThrough({"-C", tempDir->path.string(), "push", "-u", "origin", InBranch});
    if (pushResult.exitCode != 0) {
        return pushResult.exitCode;
    }

    TrySetLocalBareRemoteHead(InParsed.url, InBranch);
    return 0;
}

auto RunNativeAddSubmodule(const std::vector<std::string>& InArgs) -> int {
    if (InArgs.empty()) {
        std::cerr << "Error: submodule add requires arguments (for example: <url> [path])\n";
        return 1;
    }

    const auto parsed = TryParseSubmoduleAddArgs(InArgs);
    if (parsed.has_value()) {
        const auto probe = ProbeRemoteHeadState(parsed->url);
        if (probe == RemoteHeadProbe::MissingHead) {
            const auto bootstrapBranch = ResolveDefaultBootstrapBranch(parsed);
            std::cout << "Remote appears empty; bootstrapping initial commit on branch '" << bootstrapBranch << "'...\n";
            const auto bootstrapExitCode = BootstrapEmptyRemoteForSubmodule(*parsed, bootstrapBranch);
            if (bootstrapExitCode != 0) {
                if (ProbeRemoteHeadState(parsed->url) != RemoteHeadProbe::HasHead) {
                    return bootstrapExitCode;
                }
                std::cout << "Remote became initialized during bootstrap attempt; retrying submodule add...\n";
            }
        }
    }

    std::vector<std::string> gitArgs = {"submodule", "add"};
    gitArgs.insert(gitArgs.end(), InArgs.begin(), InArgs.end());
    const auto result = kano::git::shell::ExecuteCommand("git", gitArgs, kano::git::shell::ExecMode::PassThrough);
    if (result.exitCode == 0) {
        const auto rootResult = RunGitCapture({"rev-parse", "--show-toplevel"});
        if (rootResult.exitCode == 0) {
            const auto root = Trim(rootResult.stdoutStr);
            if (!kano::git::workspace::RefreshWorkspaceManifestAfterRegisteredChange(std::filesystem::path(root))) {
                std::cerr << "[WARN] submodule add succeeded, but failed to refresh workspace manifest\n";
            }
        }
    }
    return result.exitCode;
}

auto ReadGitConfigValue(
    const std::string& InRoot,
    const std::string& InConfigPath,
    const std::string& InKey) -> std::string {
    auto valueResult = RunGitCapture({"-C", InRoot, "config", "-f", InConfigPath, "--get", InKey});
    if (valueResult.exitCode != 0) {
        return {};
    }
    return Trim(valueResult.stdoutStr);
}

auto CollectSyncUrlEntries(const std::string& InRoot, const std::string& InGitmodulesPath) -> std::vector<SyncUrlEntry> {
    std::vector<SyncUrlEntry> entries;
    auto keyResult = RunGitCapture(
        {"-C", InRoot, "config", "-f", InGitmodulesPath, "--name-only", "--get-regexp", "^submodule\\..*\\.url$"}
    );
    if (keyResult.exitCode != 0) {
        return entries;
    }

    std::istringstream keyStream(keyResult.stdoutStr);
    std::string key;
    while (std::getline(keyStream, key)) {
        const auto trimmedKey = Trim(key);
        if (trimmedKey.empty()) {
            continue;
        }
        const auto submoduleName = ExtractSubmoduleNameFromUrlKey(trimmedKey);
        if (submoduleName.empty()) {
            continue;
        }

        const auto path = ReadGitConfigValue(
            InRoot,
            InGitmodulesPath,
            "submodule." + submoduleName + ".path"
        );
        const auto url = ReadGitConfigValue(
            InRoot,
            InGitmodulesPath,
            "submodule." + submoduleName + ".url"
        );

        entries.push_back(SyncUrlEntry{
            .name = submoduleName,
            .path = path,
            .url = url,
        });
    }

    return entries;
}

auto DetectDefaultSubmoduleUpdateJobs() -> int {
    const unsigned int cores = std::thread::hardware_concurrency();
    if (cores == 0) {
        return 1;
    }
    return static_cast<int>(cores);
}

auto PathKey(const std::filesystem::path& InPath) -> std::string {
    std::error_code ec;
    auto canonical = std::filesystem::weakly_canonical(InPath, ec);
    if (ec) {
        canonical = std::filesystem::absolute(InPath, ec);
    }
    if (ec) {
        canonical = InPath;
    }
    auto out = canonical.lexically_normal().generic_string();
#if defined(_WIN32)
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
#endif
    return out;
}

auto IsInsideWorkTreeAtPath(const std::filesystem::path& InPath) -> bool {
    if (!std::filesystem::exists(InPath)) {
        return false;
    }
    const auto inside = RunGitCapture({"-C", InPath.string(), "rev-parse", "--is-inside-work-tree"});
    if (inside.exitCode != 0 || Trim(inside.stdoutStr) != "true") {
        return false;
    }
    const auto top = RunGitCapture({"-C", InPath.string(), "rev-parse", "--show-toplevel"});
    if (top.exitCode != 0) {
        return false;
    }
    return PathKey(Trim(top.stdoutStr)) == PathKey(InPath);
}

auto GetExpectedGitlinkHead(const std::filesystem::path& InRepoRoot, const std::string& InLocalPath) -> std::optional<std::string> {
    const auto tree = RunGitCapture({"-C", InRepoRoot.string(), "ls-tree", "HEAD", "--", InLocalPath});
    if (tree.exitCode != 0) {
        return std::nullopt;
    }

    std::istringstream iss(tree.stdoutStr);
    std::string mode;
    std::string type;
    std::string sha;
    iss >> mode >> type >> sha;
    if (mode != "160000" || type != "commit" || sha.size() != 40) {
        return std::nullopt;
    }
    return sha;
}

auto ResolveModulePathForSubmodule(const std::filesystem::path& InRepoRoot, const std::string& InLocalPath) -> std::filesystem::path {
    const auto out = RunGitCapture({"-C", InRepoRoot.string(), "rev-parse", "--git-path", "modules/" + InLocalPath});
    if (out.exitCode != 0) {
        return {};
    }
    const auto path = Trim(out.stdoutStr);
    if (path.empty()) {
        return {};
    }
    return std::filesystem::path(path).lexically_normal();
}

auto TryReadGitdirReference(const std::filesystem::path& InSubmodulePath) -> std::optional<std::filesystem::path> {
    const auto gitPath = (InSubmodulePath / ".git").lexically_normal();
    if (!std::filesystem::exists(gitPath) || std::filesystem::is_directory(gitPath)) {
        return std::nullopt;
    }

    std::ifstream in(gitPath, std::ios::binary);
    if (!in.good()) {
        return std::nullopt;
    }
    std::string line;
    std::getline(in, line);
    line = Trim(line);
    constexpr std::string_view prefix = "gitdir:";
    if (!line.starts_with(prefix)) {
        return std::nullopt;
    }
    auto raw = Trim(line.substr(prefix.size()));
    if (raw.empty()) {
        return std::nullopt;
    }

    auto resolved = std::filesystem::path(raw);
    if (resolved.is_relative()) {
        resolved = (InSubmodulePath / resolved).lexically_normal();
    }
    return resolved;
}

auto HasInvalidSubmoduleGitdirReference(const std::filesystem::path& InSubmodulePath) -> bool {
    const auto ref = TryReadGitdirReference(InSubmodulePath);
    if (!ref.has_value()) {
        return false;
    }
    return !std::filesystem::exists(*ref);
}

auto DirectoryHasUserContentBeyondDotGit(const std::filesystem::path& InPath) -> bool {
    if (!std::filesystem::exists(InPath) || !std::filesystem::is_directory(InPath)) {
        return false;
    }
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(InPath, ec)) {
        if (ec) {
            return true;
        }
        if (entry.path().filename() != ".git") {
            return true;
        }
    }
    return false;
}

auto IsDirectoryEmpty(const std::filesystem::path& InPath) -> bool {
    if (!std::filesystem::exists(InPath)) {
        return true;
    }
    std::error_code ec;
    return std::filesystem::is_directory(InPath) && std::filesystem::is_empty(InPath, ec) && !ec;
}

auto ModulePathBelongsToSubmodule(
    const std::filesystem::path& InModulePath,
    const std::filesystem::path& InSubmodulePath) -> bool {
    if (!std::filesystem::exists(InModulePath)) {
        return true;
    }

    const auto worktreeOut = RunGitCapture({"--git-dir", InModulePath.string(), "config", "--get", "core.worktree"});
    if (worktreeOut.exitCode != 0) {
        return false;
    }
    auto worktree = Trim(worktreeOut.stdoutStr);
    if (worktree.empty()) {
        return false;
    }

    auto resolved = std::filesystem::path(worktree);
    if (resolved.is_relative()) {
        resolved = (InModulePath / resolved).lexically_normal();
    }
    return PathKey(resolved) == PathKey(InSubmodulePath);
}

auto HasRetryableCloneFailureSignal(const std::string& InText) -> bool {
    static const std::vector<std::string> needles = {
        "initial ref transaction called with existing refs",
        "submodule considered for cloning, doesn't need cloning any more",
        "retry scheduled",
        "failed to clone",
        "clone of",
        "already exists and is not empty",
    };
    for (const auto& needle : needles) {
        if (ContainsCaseInsensitive(InText, needle)) {
            return true;
        }
    }
    return false;
}

auto ParseLfsPointerMismatchCount(const std::string& InText) -> int {
    const auto lines = SplitLines(InText);
    int count = 0;
    bool collecting = false;

    for (const auto& rawLine : lines) {
        const auto line = Trim(rawLine);
        if (!collecting) {
            if (ContainsCaseInsensitive(rawLine, "Encountered") &&
                ContainsCaseInsensitive(rawLine, "should have been pointers, but weren't")) {
                collecting = true;
            }
            continue;
        }

        if (line.empty()) {
            if (count > 0) {
                break;
            }
            continue;
        }

        if (rawLine.size() > 1 && std::isspace(static_cast<unsigned char>(rawLine.front()))) {
            count += 1;
            continue;
        }

        break;
    }

    if (collecting && count == 0) {
        return 1;
    }
    return count;
}

auto EvaluateSubmoduleRepairSafety(
    const SubmoduleUpdateTask& InTask,
    const std::string& InOutput) -> SubmoduleRepairSafety {
    SubmoduleRepairSafety out;
    const auto repoRoot = InTask.repoRoot.lexically_normal();
    const auto submodulePath = (repoRoot / std::filesystem::path(InTask.localPath)).lexically_normal();

    const auto declared = CollectRegisteredSubmodulePaths(repoRoot);
    const auto normalizedLocalPath = std::filesystem::path(InTask.localPath).lexically_normal().generic_string();
    const auto isDeclared = std::find(declared.begin(), declared.end(), normalizedLocalPath) != declared.end();
    if (!isDeclared) {
        out.reason = "path is not declared in .gitmodules";
        return out;
    }

    if (!GetExpectedGitlinkHead(repoRoot, InTask.localPath).has_value()) {
        out.reason = "path is not a gitlink in parent HEAD";
        return out;
    }

    out.modulePath = ResolveModulePathForSubmodule(repoRoot, InTask.localPath);
    if (out.modulePath.empty()) {
        out.reason = "unable to resolve .git/modules path for submodule";
        return out;
    }
    if (!ModulePathBelongsToSubmodule(out.modulePath, submodulePath)) {
        out.reason = ".git/modules state does not belong to this submodule";
        return out;
    }

    if (IsInsideWorkTreeAtPath(submodulePath)) {
        const auto status = RunGitCapture({"-C", submodulePath.string(), "status", "--porcelain"});
        if (status.exitCode != 0) {
            out.reason = "failed to evaluate submodule dirtiness";
            return out;
        }
        if (!Trim(status.stdoutStr).empty()) {
            out.reason = "submodule has local dirty user changes";
            return out;
        }
        out.safe = true;
        return out;
    }

    if (!std::filesystem::exists(submodulePath) || IsDirectoryEmpty(submodulePath)) {
        out.safe = true;
        return out;
    }

    const bool invalidGitdirRef = HasInvalidSubmoduleGitdirReference(submodulePath);
    const bool retryableSignal = HasRetryableCloneFailureSignal(InOutput);
    const bool hasOnlyDotGit = !DirectoryHasUserContentBeyondDotGit(submodulePath);

    if ((invalidGitdirRef && hasOnlyDotGit) || (retryableSignal && hasOnlyDotGit)) {
        out.safe = true;
        return out;
    }

    out.reason = "submodule path is non-empty and cannot be proven as safe failed clone state";
    return out;
}

auto RemovePathIfExists(const std::filesystem::path& InPath) -> bool {
    if (!std::filesystem::exists(InPath)) {
        return true;
    }
    std::error_code ec;
    std::filesystem::remove_all(InPath, ec);
    return !ec;
}

auto AttemptSafeSubmoduleRepair(
    const SubmoduleUpdateTask& InTask,
    const SubmoduleRepairSafety& InSafety,
    bool InRemote) -> kano::git::shell::ExecResult {
    const auto repoRoot = InTask.repoRoot.lexically_normal();
    const auto submodulePath = (repoRoot / std::filesystem::path(InTask.localPath)).lexically_normal();

    const auto deinit = RunGitCapture({"-C", repoRoot.string(), "submodule", "deinit", "-f", "--", InTask.localPath});
    (void)deinit;

    if (!RemovePathIfExists(submodulePath)) {
        return kano::git::shell::ExecResult{.exitCode = 1, .stderrStr = "failed to remove submodule worktree path"};
    }
    if (!RemovePathIfExists(InSafety.modulePath)) {
        return kano::git::shell::ExecResult{.exitCode = 1, .stderrStr = "failed to remove .git/modules entry"};
    }

    std::vector<std::string> retryArgs = {
        "-C", repoRoot.string(), "submodule", "update", "--init"
    };
    if (InRemote) {
        retryArgs.push_back("--remote");
    }
    retryArgs.push_back("--");
    retryArgs.push_back(InTask.localPath);
    return RunGitCapture(retryArgs);
}

auto BuildLfsPointerMismatchWarning(int InCount) -> SubmoduleWarning {
    std::ostringstream oss;
    if (InCount <= 1) {
        oss << "1 file should have been an LFS pointer but was a full file";
    } else {
        oss << InCount << " files should have been LFS pointers but were full files";
    }
    return SubmoduleWarning{.code = "LFS_POINTER_MISMATCH", .message = oss.str()};
}

auto ValidateUpdatedSubmodule(
    const SubmoduleUpdateTask& InTask,
    const std::string& InCombinedOutput,
    std::vector<SubmoduleWarning>* IoWarnings,
    std::vector<SubmoduleIssue>* IoIssues) -> bool {
    const auto repoRoot = InTask.repoRoot.lexically_normal();
    const auto submodulePath = (repoRoot / std::filesystem::path(InTask.localPath)).lexically_normal();

    if (!std::filesystem::exists(submodulePath)) {
        IoIssues->push_back(SubmoduleIssue{.code = "SUBMODULE_NOT_INITIALIZED", .message = "submodule path does not exist after update"});
        return false;
    }

    if (!IsInsideWorkTreeAtPath(submodulePath)) {
        IoIssues->push_back(SubmoduleIssue{.code = "SUBMODULE_GITDIR_BROKEN", .message = "submodule is not a valid git worktree"});
        return false;
    }

    const auto expectedHead = GetExpectedGitlinkHead(repoRoot, InTask.localPath);
    if (!expectedHead.has_value()) {
        IoIssues->push_back(SubmoduleIssue{.code = "SUBMODULE_NOT_INITIALIZED", .message = "parent gitlink commit is missing for submodule path"});
        return false;
    }

    const auto childHead = RunGitCapture({"-C", submodulePath.string(), "rev-parse", "HEAD"});
    if (childHead.exitCode != 0) {
        IoIssues->push_back(SubmoduleIssue{.code = "SUBMODULE_GITDIR_BROKEN", .message = "failed to resolve submodule HEAD"});
        return false;
    }
    if (Trim(childHead.stdoutStr) != *expectedHead) {
        IoIssues->push_back(SubmoduleIssue{.code = "SUBMODULE_HEAD_MISMATCH", .message = "submodule HEAD does not match parent gitlink commit"});
        return false;
    }

    if (!DidSubmoduleUpdateSucceed(repoRoot, InTask.localPath)) {
        IoIssues->push_back(SubmoduleIssue{.code = "SUBMODULE_NOT_INITIALIZED", .message = "submodule status indicates unresolved initialization"});
        return false;
    }

    int mismatchCount = ParseLfsPointerMismatchCount(InCombinedOutput);

    const auto lfsVersion = RunGitCapture({"-C", submodulePath.string(), "lfs", "version"});
    if (lfsVersion.exitCode == 0) {
        const auto lfsTracked = RunGitCapture({"-C", submodulePath.string(), "lfs", "ls-files", "-n"});
        if (lfsTracked.exitCode == 0 && !Trim(lfsTracked.stdoutStr).empty()) {
            const auto fsck = RunGitCapture({"-C", submodulePath.string(), "lfs", "fsck"});
            mismatchCount = std::max(mismatchCount, ParseLfsPointerMismatchCount(fsck.stdoutStr + "\n" + fsck.stderrStr));
        }
    }

    if (mismatchCount > 0) {
        IoWarnings->push_back(BuildLfsPointerMismatchWarning(mismatchCount));
    }

    return true;
}

auto ExecuteSubmoduleUpdateTask(
    const SubmoduleUpdateTask& InTask,
    bool InRemote) -> SubmoduleUpdateOutcome {
    SubmoduleUpdateOutcome outcome;
    outcome.task = InTask;

    std::vector<std::string> args = {
        "-C", InTask.repoRoot.string(), "submodule", "update", "--init"
    };
    if (InRemote) {
        args.push_back("--remote");
    }
    args.push_back("--");
    args.push_back(InTask.localPath);

    auto firstRun = RunGitCapture(args);
    std::string combinedOutput = firstRun.stdoutStr + "\n" + firstRun.stderrStr;

    if (const char* testStderr = std::getenv("KOG_TEST_SUBMODULE_UPDATE_STDERR"); testStderr != nullptr) {
        combinedOutput += "\n" + std::string(testStderr);
    }

    bool shouldAttemptRepair = firstRun.exitCode != 0 ||
                               HasRetryableCloneFailureSignal(combinedOutput) ||
                               HasInvalidSubmoduleGitdirReference((InTask.repoRoot / std::filesystem::path(InTask.localPath)).lexically_normal());

    if (shouldAttemptRepair) {
        const auto safety = EvaluateSubmoduleRepairSafety(InTask, combinedOutput);
        if (!safety.safe) {
            outcome.kind = SubmoduleUpdateOutcomeKind::Blocked;
            outcome.issues.push_back(SubmoduleIssue{.code = "BLOCKED_SUBMODULE_REPAIR_UNSAFE", .message = safety.reason});
            return outcome;
        }

        outcome.attemptedRepair = true;
        const auto repair = AttemptSafeSubmoduleRepair(InTask, safety, InRemote);
        combinedOutput += "\n" + repair.stdoutStr + "\n" + repair.stderrStr;
        if (repair.exitCode != 0) {
            outcome.kind = SubmoduleUpdateOutcomeKind::Failed;
            outcome.issues.push_back(SubmoduleIssue{.code = "SUBMODULE_REPAIR_FAILED", .message = "safe cleanup/retry failed"});
            return outcome;
        }
    }

    if (!ValidateUpdatedSubmodule(InTask, combinedOutput, &outcome.warnings, &outcome.issues)) {
        outcome.kind = SubmoduleUpdateOutcomeKind::Failed;
        return outcome;
    }

    if (outcome.attemptedRepair) {
        outcome.kind = SubmoduleUpdateOutcomeKind::RepairedAndUpdated;
    } else if (!outcome.warnings.empty()) {
        outcome.kind = SubmoduleUpdateOutcomeKind::UpdatedWithWarnings;
    } else {
        outcome.kind = SubmoduleUpdateOutcomeKind::UpdatedCleanly;
    }

    return outcome;
}

auto DidSubmoduleUpdateSucceed(const std::filesystem::path& InRepoRoot, const std::string& InLocalPath) -> bool {
    const auto result = RunGitCapture({"-C", InRepoRoot.string(), "submodule", "status", "--", InLocalPath});
    if (result.exitCode != 0) {
        return false;
    }

    const auto line = Trim(result.stdoutStr);
    if (line.empty()) {
        return false;
    }

    return line.front() != '-';
}

auto CollectRegisteredSubmodulePaths(const std::filesystem::path& InRepoRoot) -> std::vector<std::string> {
    const auto gitmodulesPath = (InRepoRoot / ".gitmodules").lexically_normal();
    if (!std::filesystem::exists(gitmodulesPath)) {
        return {};
    }

    std::vector<std::string> paths;
    const auto entries = CollectSyncUrlEntries(InRepoRoot.string(), gitmodulesPath.string());
    paths.reserve(entries.size());
    for (const auto& entry : entries) {
        const auto path = Trim(entry.path);
        if (!path.empty()) {
            paths.push_back(std::filesystem::path(path).lexically_normal().generic_string());
        }
    }
    return paths;
}

auto CollectNestedSubmoduleTasks(
    const std::filesystem::path& InWorkspaceRoot,
    const std::string& InParentDisplayPath) -> std::vector<SubmoduleUpdateTask> {
    const auto parentRepoRoot = (InWorkspaceRoot / std::filesystem::path(InParentDisplayPath)).lexically_normal();
    std::vector<SubmoduleUpdateTask> out;
    for (const auto& nestedPath : CollectRegisteredSubmodulePaths(parentRepoRoot)) {
        const auto combined = (std::filesystem::path(InParentDisplayPath) / std::filesystem::path(nestedPath)).lexically_normal();
        out.push_back(SubmoduleUpdateTask{
            .displayPath = combined.generic_string(),
            .repoRoot = parentRepoRoot,
            .localPath = nestedPath,
        });
    }
    return out;
}

auto RunNativeSyncUrls(
    bool InDryRun,
    bool InInitMissing,
    bool InNoRecursive,
    bool InNoOrigin) -> int {
    const auto rootResult = RunGitCapture({"rev-parse", "--show-toplevel"});
    if (rootResult.exitCode != 0) {
        std::cerr << "ERROR: not inside a git repository.\n";
        return 1;
    }

    const auto root = Trim(rootResult.stdoutStr);
    const auto gitmodulesPathObj = std::filesystem::path(root) / ".gitmodules";
    if (!std::filesystem::exists(gitmodulesPathObj)) {
        std::cout << "Skip: no .gitmodules found at " << root << "\n";
        return 0;
    }
    const auto gitmodulesPath = gitmodulesPathObj.string();

    std::cout << "Root: " << root << "\n";

    if (InInitMissing) {
        std::cout << "Initializing submodules (if needed)...\n";
        const std::vector<std::string> initArgs = {"-C", root, "submodule", "update", "--init", "--recursive"};
        if (InDryRun) {
            PrintDryRunGit(initArgs);
        } else {
            const auto initResult = kano::git::shell::ExecuteCommand("git", initArgs, kano::git::shell::ExecMode::PassThrough);
            if (initResult.exitCode != 0) {
                return initResult.exitCode;
            }
        }
    }

    std::cout << "Syncing submodule URLs into superproject config...\n";
    std::vector<std::string> syncArgs = {"-C", root, "submodule", "sync"};
    if (!InNoRecursive) {
        syncArgs.push_back("--recursive");
    }

    if (InDryRun) {
        PrintDryRunGit(syncArgs);
    } else {
        const auto syncResult = kano::git::shell::ExecuteCommand("git", syncArgs, kano::git::shell::ExecMode::PassThrough);
        if (syncResult.exitCode != 0) {
            return syncResult.exitCode;
        }
    }

    if (InNoOrigin) {
        std::cout << "Done (skipped updating submodule origin remotes).\n";
        return 0;
    }

    std::cout << "Updating submodule repo origin URLs to match .gitmodules...\n";
    const auto entries = CollectSyncUrlEntries(root, gitmodulesPath);
    for (const auto& entry : entries) {
        if (entry.path.empty() || entry.url.empty()) {
            std::cout << "Skip: invalid .gitmodules entry for submodule." << entry.name << " (missing path/url).\n";
            continue;
        }

        const auto repoPath = (std::filesystem::path(root) / entry.path).string();
        const auto isRepoResult = RunGitCapture({"-C", repoPath, "rev-parse", "--is-inside-work-tree"});
        if (isRepoResult.exitCode != 0) {
            std::cout << "Skip: submodule not initialized: " << entry.path << "\n";
            continue;
        }

        const auto currentUrlResult = RunGitCapture({"-C", repoPath, "remote", "get-url", "origin"});
        const auto currentUrl = currentUrlResult.exitCode == 0 ? Trim(currentUrlResult.stdoutStr) : std::string{};

        if (currentUrl.empty()) {
            std::cout << "[" << entry.path << "] add origin -> " << entry.url << "\n";
            const std::vector<std::string> addArgs = {"-C", repoPath, "remote", "add", "origin", entry.url};
            if (InDryRun) {
                PrintDryRunGit(addArgs);
            } else {
                const auto addResult = kano::git::shell::ExecuteCommand("git", addArgs, kano::git::shell::ExecMode::PassThrough);
                if (addResult.exitCode != 0) {
                    return addResult.exitCode;
                }
            }
            continue;
        }

        if (currentUrl == entry.url) {
            std::cout << "[" << entry.path << "] origin already up-to-date\n";
            continue;
        }

        std::cout << "[" << entry.path << "] set origin: " << currentUrl << " -> " << entry.url << "\n";
        const std::vector<std::string> setArgs = {"-C", repoPath, "remote", "set-url", "origin", entry.url};
        if (InDryRun) {
            PrintDryRunGit(setArgs);
        } else {
            const auto setResult = kano::git::shell::ExecuteCommand("git", setArgs, kano::git::shell::ExecMode::PassThrough);
            if (setResult.exitCode != 0) {
                return setResult.exitCode;
            }
        }
    }

    std::cout << "Done.\n";
    return 0;
}

auto RunNativeRemoveSubmodule(const std::string& InPath, bool InDryRun) -> int {
    if (InPath.empty()) {
        std::cerr << "Error: Submodule path is required\n";
        return 1;
    }

    const auto repoCheck = RunGitCapture({"rev-parse", "--git-dir"});
    if (repoCheck.exitCode != 0) {
        std::cerr << "Error: Not in a git repository\n";
        return 1;
    }

    const auto rootResult = RunGitCapture({"rev-parse", "--show-toplevel"});
    if (rootResult.exitCode != 0) {
        std::cerr << "Error: Not in a git repository\n";
        return 1;
    }
    const auto root = Trim(rootResult.stdoutStr);

    const auto gitmodulesPathObj = std::filesystem::path(root) / ".gitmodules";
    if (!std::filesystem::exists(gitmodulesPathObj)) {
        std::cerr << "Error: No .gitmodules file found\n";
        return 1;
    }
    const auto gitmodulesPath = gitmodulesPathObj.string();

    auto existsResult = RunGitCapture(
        {"-C", root, "config", "--file", gitmodulesPath, "--get", "submodule." + InPath + ".path"}
    );
    if (existsResult.exitCode != 0) {
        std::cerr << "Error: Submodule '" << InPath << "' not found in .gitmodules\n";
        return 1;
    }

    auto urlResult = RunGitCapture(
        {"-C", root, "config", "--file", gitmodulesPath, "--get", "submodule." + InPath + ".url"}
    );
    const auto submoduleUrl = urlResult.exitCode == 0 ? Trim(urlResult.stdoutStr) : std::string{"unknown"};

    std::cout << "Removing submodule: " << InPath << "\n";
    std::cout << "  URL: " << (submoduleUrl.empty() ? "unknown" : submoduleUrl) << "\n\n";

    if (InDryRun) {
        std::cout << "[DRY RUN] Would perform the following steps:\n";
        std::cout << "  1. git submodule deinit -f " << InPath << "\n";
        std::cout << "  2. git rm -f " << InPath << "\n";
        std::cout << "  3. rm -rf .git/modules/" << InPath << "\n\n";
        std::cout << "[DRY RUN] You would then need to commit the changes:\n";
        std::cout << "  git commit -m \"Remove submodule " << InPath << "\"\n";
        return 0;
    }

    std::cout << "Step 1: Deinitializing submodule...\n";
    auto deinitResult = kano::git::shell::ExecuteCommand(
        "git",
        {"-C", root, "submodule", "deinit", "-f", InPath},
        kano::git::shell::ExecMode::PassThrough
    );
    if (deinitResult.exitCode != 0) {
        return deinitResult.exitCode;
    }

    std::cout << "Step 2: Removing from git index and working tree...\n";
    auto rmResult = kano::git::shell::ExecuteCommand(
        "git",
        {"-C", root, "rm", "-f", InPath},
        kano::git::shell::ExecMode::PassThrough
    );
    if (rmResult.exitCode != 0) {
        return rmResult.exitCode;
    }

    const auto modulePathResult = RunGitCapture({"-C", root, "rev-parse", "--git-path", "modules/" + InPath});
    const auto modulePath = modulePathResult.exitCode == 0 ? Trim(modulePathResult.stdoutStr) : std::string{};
    if (!modulePath.empty() && std::filesystem::exists(modulePath)) {
        std::cout << "Step 3: Removing " << modulePath << "...\n";
        std::error_code removeEc;
        std::filesystem::remove_all(modulePath, removeEc);
        if (removeEc) {
            std::cerr << "Error: Failed to remove '" << modulePath << "': " << removeEc.message() << "\n";
            return 1;
        }
    }

    std::cout << "\n✓ Submodule removed successfully\n\n";
    std::cout << "Next steps:\n";
    std::cout << "  1. Review the changes: git status\n";
    std::cout << "  2. Commit the changes: git commit -m \"Remove submodule " << InPath << "\"\n";
    if (!kano::git::workspace::RefreshWorkspaceManifestAfterRegisteredChange(std::filesystem::path(root))) {
        std::cerr << "[WARN] submodule remove succeeded, but failed to refresh workspace manifest\n";
    }
    return 0;
}

auto BuildGitSubmoduleArgs(
    std::string InSubcommand,
    bool InRecursive,
    bool InRemote,
    const std::string& InPath,
    const std::string& InCommand) -> std::vector<std::string> {
    std::vector<std::string> args;
    args.push_back("submodule");
    args.push_back(std::move(InSubcommand));

    if (InRecursive) {
        args.push_back("--recursive");
    }
    if (InRemote) {
        args.push_back("--remote");
    }
    if (!InCommand.empty()) {
        args.push_back(InCommand);
    }
    if (!InPath.empty()) {
        args.push_back("--");
        args.push_back(InPath);
    }
    return args;
}

auto RunNativeSubmodule(
    const std::vector<std::string>& InGitArgs,
    bool InDryRun,
    const std::string& InDisplay) -> int {
    if (InDryRun) {
        std::cout << "[DRY-RUN] Would execute: git";
        for (const auto& part : InGitArgs) {
            std::cout << " " << part;
        }
        std::cout << "\n";
        return 0;
    }

    std::cout << InDisplay << "\n";
    auto result = kano::git::shell::ExecuteCommand("git", InGitArgs, kano::git::shell::ExecMode::PassThrough);
    return result.exitCode;
}

auto RunSubmoduleUpdateContinueOnError(
    bool InRecursive,
    bool InRemote,
    const std::string& InPath,
    bool InDryRun) -> int {
    const auto rootResult = RunGitCapture({"rev-parse", "--show-toplevel"});
    if (rootResult.exitCode != 0) {
        std::cerr << "ERROR: not inside a git repository.\n";
        return 1;
    }
    const auto root = Trim(rootResult.stdoutStr);
    const auto rootPath = std::filesystem::path(root).lexically_normal();

    std::cout << "Updating submodules...\n";

    std::vector<SubmoduleUpdateTask> currentWave;
    if (!InPath.empty()) {
        const auto normalized = std::filesystem::path(InPath).lexically_normal().generic_string();
        currentWave.push_back(SubmoduleUpdateTask{
            .displayPath = normalized,
            .repoRoot = rootPath,
            .localPath = normalized,
        });
    } else {
        for (const auto& path : CollectRegisteredSubmodulePaths(rootPath)) {
            currentWave.push_back(SubmoduleUpdateTask{
                .displayPath = path,
                .repoRoot = rootPath,
                .localPath = path,
            });
        }
        if (currentWave.empty()) {
            // No submodules registered yet — fall back to a single update call
            const auto gitArgs = BuildGitSubmoduleArgs("update", InRecursive, InRemote, {}, "--init");
            return RunNativeSubmodule(gitArgs, InDryRun, "Updating submodules...");
        }
    }

    std::vector<SubmoduleUpdateOutcome> outcomes;
    const int jobs = DetectDefaultSubmoduleUpdateJobs();
    std::mutex outputMutex;

    while (!currentWave.empty()) {
        if (InDryRun) {
            for (const auto& task : currentWave) {
                std::vector<std::string> args = {
                    "-C", task.repoRoot.string(), "submodule", "update", "--init"
                };
                if (InRemote) {
                    args.push_back("--remote");
                }
                args.push_back("--");
                args.push_back(task.localPath);
                PrintDryRunGit(args);
            }
            break;
        }

        std::vector<SubmoduleUpdateTask> nextWave;
        struct TaskRunResult {
            SubmoduleUpdateOutcome outcome;
        };

        std::map<std::string, std::vector<SubmoduleUpdateTask>> grouped;
        for (const auto& task : currentWave) {
            grouped[task.repoRoot.lexically_normal().generic_string()].push_back(task);
        }

        auto runTaskGroup = [&](std::vector<SubmoduleUpdateTask> tasks) -> std::vector<TaskRunResult> {
            std::vector<TaskRunResult> results;
            results.reserve(tasks.size());
            for (const auto& task : tasks) {
                results.push_back(TaskRunResult{
                    .outcome = ExecuteSubmoduleUpdateTask(task, InRemote),
                });
            }
            return results;
        };

        std::vector<std::future<std::vector<TaskRunResult>>> active;
        active.reserve(static_cast<std::size_t>(std::max(1, jobs)));

        auto handleResult = [&](TaskRunResult&& result) {
            std::lock_guard<std::mutex> lock(outputMutex);
            const auto& task = result.outcome.task;
            const auto taskDisplayPath = task.displayPath;
            const auto taskKind = result.outcome.kind;
            switch (result.outcome.kind) {
            case SubmoduleUpdateOutcomeKind::UpdatedCleanly:
                std::cout << "[" << task.displayPath << "] updated\n";
                break;
            case SubmoduleUpdateOutcomeKind::UpdatedWithWarnings:
                std::cout << "[" << task.displayPath << "] updated (warnings)\n";
                break;
            case SubmoduleUpdateOutcomeKind::RepairedAndUpdated:
                std::cout << "[" << task.displayPath << "] repaired and updated\n";
                break;
            case SubmoduleUpdateOutcomeKind::Blocked:
                std::cerr << "[" << task.displayPath << "] BLOCKED\n";
                break;
            case SubmoduleUpdateOutcomeKind::Failed:
            default:
                std::cerr << "[" << task.displayPath << "] FAILED\n";
                break;
            }
            outcomes.push_back(std::move(result.outcome));

            if (!InRecursive) {
                return;
            }

            if (taskKind == SubmoduleUpdateOutcomeKind::UpdatedCleanly ||
                taskKind == SubmoduleUpdateOutcomeKind::UpdatedWithWarnings ||
                taskKind == SubmoduleUpdateOutcomeKind::RepairedAndUpdated) {
                for (auto& nestedTask : CollectNestedSubmoduleTasks(rootPath, taskDisplayPath)) {
                    nextWave.push_back(std::move(nestedTask));
                }
            }
        };

        auto waitOne = [&]() {
            if (active.empty()) {
                return;
            }
            auto results = active.front().get();
            for (auto& result : results) {
                handleResult(std::move(result));
            }
            active.erase(active.begin());
        };

        for (auto& [repoRootString, tasks] : grouped) {
            while (static_cast<int>(active.size()) >= std::max(1, jobs)) {
                waitOne();
            }
            active.push_back(std::async(std::launch::async, [&, tasks]() mutable {
                return runTaskGroup(std::move(tasks));
            }));
        }

        while (!active.empty()) {
            waitOne();
        }

        currentWave.clear();
        if (!InRecursive) {
            break;
        }

        std::sort(nextWave.begin(), nextWave.end(), [](const auto& left, const auto& right) {
            return left.displayPath < right.displayPath;
        });
        nextWave.erase(
            std::unique(nextWave.begin(), nextWave.end(), [](const auto& left, const auto& right) {
                return left.displayPath == right.displayPath;
            }),
            nextWave.end());
        currentWave = std::move(nextWave);
    }

    int cleanCount = 0;
    int warningCount = 0;
    int repairedCount = 0;
    int failedCount = 0;
    int blockedCount = 0;
    std::vector<std::pair<std::string, SubmoduleWarning>> warningDetails;
    std::vector<std::pair<std::string, SubmoduleIssue>> issueDetails;

    for (const auto& outcome : outcomes) {
        switch (outcome.kind) {
        case SubmoduleUpdateOutcomeKind::UpdatedCleanly:
            cleanCount += 1;
            break;
        case SubmoduleUpdateOutcomeKind::UpdatedWithWarnings:
            warningCount += 1;
            break;
        case SubmoduleUpdateOutcomeKind::RepairedAndUpdated:
            repairedCount += 1;
            break;
        case SubmoduleUpdateOutcomeKind::Failed:
            failedCount += 1;
            break;
        case SubmoduleUpdateOutcomeKind::Blocked:
            blockedCount += 1;
            break;
        }

        for (const auto& warning : outcome.warnings) {
            warningDetails.emplace_back(outcome.task.displayPath, warning);
        }
        for (const auto& issue : outcome.issues) {
            issueDetails.emplace_back(outcome.task.displayPath, issue);
        }
    }

    std::cout << "\n=== Submodule Update Complete ===\n";
    std::cout << "Updated cleanly: " << cleanCount << "\n";
    std::cout << "Updated with warnings: " << warningCount << "\n";
    std::cout << "Repaired and updated: " << repairedCount << "\n";
    std::cout << "Failed: " << failedCount << "\n";
    std::cout << "Blocked: " << blockedCount << "\n";

    if (!warningDetails.empty()) {
        std::cout << "\n=== WARNINGS ===\n";
        for (const auto& [path, warning] : warningDetails) {
            std::cout << "[" << path << "] " << warning.code << ": " << warning.message << "\n";
        }
    }

    if (!issueDetails.empty()) {
        std::cerr << "\n=== ISSUES ===\n";
        for (const auto& [path, issue] : issueDetails) {
            std::cerr << "[" << path << "] " << issue.code << ": " << issue.message << "\n";
        }
    }

    if (failedCount > 0 || blockedCount > 0) {
        return 1;
    }

    if (outcomes.empty()) {
        std::cout << "No submodules required updates.\n";
    }

    if (warningCount > 0 || repairedCount > 0) {
        return 0;
    }

    if (cleanCount == 0 && !InPath.empty()) {
        std::cerr << "No matching submodule tasks were executed for path: " << InPath << "\n";
        }
    return 0;
}

} // namespace

namespace kano::git::commands {

void RegisterSubmodule(CLI::App& InApp) {
    auto* cmd = InApp.add_subcommand("submodule", "Submodule management");

    auto* add = cmd->add_subcommand("add", "Add a submodule");
    add->allow_extras();
    auto* addUrl = new std::string{};
    auto* addPath = new std::string{};
    auto* addBranch = new std::string{};
    auto* addForce = new bool{false};
    add->add_option("url", *addUrl, "Submodule repository URL");
    add->add_option("path", *addPath, "Optional submodule path");
    add->add_option("-b,--branch", *addBranch, "Branch to track while adding the submodule");
    add->add_flag("-f,--force", *addForce, "Allow adding an existing local path as a submodule");
    add->callback([=]() {
        std::vector<std::string> args;
        if (*addForce) {
            args.push_back("--force");
        }
        if (!addBranch->empty()) {
            args.push_back("--branch");
            args.push_back(*addBranch);
        }
        if (addUrl->empty()) {
            const auto extras = add->remaining();
            args.insert(args.end(), extras.begin(), extras.end());
        } else {
            args.push_back(*addUrl);
            if (!addPath->empty()) {
                args.push_back(*addPath);
            }
            const auto extras = add->remaining();
            args.insert(args.end(), extras.begin(), extras.end());
        }
        std::exit(RunNativeAddSubmodule(args));
    });

    auto* sync = cmd->add_subcommand("sync", "Sync submodule URLs");
    sync->allow_extras();
    auto* syncNative = new bool{false};
    auto* syncShell = new bool{false};
    auto* syncRecursive = new bool{false};
    auto* syncDryRun = new bool{false};
    auto* syncPath = new std::string{};
    sync->add_flag("--native", *syncNative, "Use native git submodule sync implementation (default)");
    sync->add_flag("--shell", *syncShell, "Deprecated compatibility flag (shell path removed)");
    sync->add_flag("--recursive", *syncRecursive, "Sync recursively");
    sync->add_flag("--dry-run", *syncDryRun, "Preview mode");
    sync->add_option("path", *syncPath, "Optional submodule path");
    sync->callback([=]() {
        if (*syncShell && *syncNative) {
            std::cerr << "Error: --shell cannot be combined with --native\n";
            std::exit(1);
        }
        if (*syncShell) {
            std::cerr << "Error: --shell is no longer supported; submodule sync is fully native now\n";
            std::exit(2);
        }
        const auto gitArgs = BuildGitSubmoduleArgs("sync", *syncRecursive, false, *syncPath, {});
        std::exit(RunNativeSubmodule(gitArgs, *syncDryRun, "Syncing submodule URLs..."));
    });

    auto* update = cmd->add_subcommand("update", "Update submodules");
    update->allow_extras();
    auto* updateNative = new bool{false};
    auto* updateShell = new bool{false};
    auto* updateRecursive = new bool{false};
    auto* updateRemote = new bool{false};
    auto* updateDryRun = new bool{false};
    auto* updatePath = new std::string{};
    update->add_flag("--native", *updateNative, "Use native git submodule update implementation (default)");
    update->add_flag("--shell", *updateShell, "Deprecated compatibility flag (shell path removed)");
    update->add_flag("--recursive", *updateRecursive, "Update recursively");
    update->add_flag("--remote", *updateRemote, "Update to latest remote tracked commit");
    update->add_flag("--dry-run", *updateDryRun, "Preview mode");
    update->add_option("path", *updatePath, "Optional submodule path");
    update->callback([=]() {
        if (*updateShell && *updateNative) {
            std::cerr << "Error: --shell cannot be combined with --native\n";
            std::exit(1);
        }
        if (*updateShell) {
            std::cerr << "Error: --shell is no longer supported; submodule update is fully native now\n";
            std::exit(2);
        }
        std::exit(RunSubmoduleUpdateContinueOnError(*updateRecursive, *updateRemote, *updatePath, *updateDryRun));
    });

    auto* remove = cmd->add_subcommand("remove", "Remove a submodule");
    remove->allow_extras();
    auto* removeNative = new bool{false};
    auto* removeShell = new bool{false};
    auto* removeDryRun = new bool{false};
    auto* removePath = new std::string{};
    remove->add_flag("--native", *removeNative, "Use native submodule remove implementation (default)");
    remove->add_flag("--shell", *removeShell, "Deprecated compatibility flag (shell path removed)");
    remove->add_flag("--dry-run", *removeDryRun, "Preview mode");
    remove->add_option("path", *removePath, "Submodule path to remove");
    remove->callback([=]() {
        if (*removeShell && *removeNative) {
            std::cerr << "Error: --shell cannot be combined with --native\n";
            std::exit(1);
        }
        if (*removeShell) {
            std::cerr << "Error: --shell is no longer supported; submodule remove is fully native now\n";
            std::exit(2);
        }

        auto extras = remove->remaining();
        std::string targetPath = *removePath;
        if (targetPath.empty() && !extras.empty()) {
            targetPath = extras.front();
            extras.erase(extras.begin());
        }
        if (!extras.empty()) {
            std::cerr << "Error: Too many arguments\n";
            std::exit(1);
        }

        std::exit(RunNativeRemoveSubmodule(targetPath, *removeDryRun));
    });

    auto* foreach = cmd->add_subcommand("foreach", "Run command on each submodule");
    foreach->allow_extras();
    auto* foreachNative = new bool{false};
    auto* foreachShell = new bool{false};
    auto* foreachRecursive = new bool{false};
    auto* foreachDryRun = new bool{false};
    auto* foreachCommand = new std::string{};
    foreach->add_flag("--native", *foreachNative, "Use native git submodule foreach implementation (default)");
    foreach->add_flag("--shell", *foreachShell, "Deprecated compatibility flag (shell path removed)");
    foreach->add_flag("--recursive", *foreachRecursive, "Execute recursively");
    foreach->add_flag("--dry-run", *foreachDryRun, "Preview mode");
    foreach->add_option("--command", *foreachCommand, "Command to execute in each submodule");
    foreach->add_option("cmd", *foreachCommand, "Positional command to execute in each submodule");
    foreach->callback([=]() {
        if (*foreachShell && *foreachNative) {
            std::cerr << "Error: --shell cannot be combined with --native\n";
            std::exit(1);
        }
        if (*foreachShell) {
            std::cerr << "Error: --shell is no longer supported; submodule foreach is fully native now\n";
            std::exit(2);
        }
        auto extras = foreach->remaining();
        std::string command = *foreachCommand;
        if (command.empty() && !extras.empty()) {
            command = extras.front();
            extras.erase(extras.begin());
        }
        if (!extras.empty()) {
            if (!command.empty()) {
                command += " ";
            }
            for (std::size_t i = 0; i < extras.size(); ++i) {
                if (i > 0) {
                    command += " ";
                }
                command += extras[i];
            }
        }
        if (command.empty()) {
            std::cerr << "Error: command is required\n";
            std::exit(1);
        }

        const auto gitArgs = BuildGitSubmoduleArgs("foreach", *foreachRecursive, false, {}, command);
        std::exit(RunNativeSubmodule(gitArgs, *foreachDryRun, "Executing command in submodules..."));
    });

    auto* sync_urls = cmd->add_subcommand("sync-urls", "Sync submodule URLs from .gitmodules");
    sync_urls->allow_extras();
    auto* syncUrlsNative = new bool{false};
    auto* syncUrlsShell = new bool{false};
    auto* syncUrlsDryRun = new bool{false};
    auto* syncUrlsInitMissing = new bool{false};
    auto* syncUrlsNoRecursive = new bool{false};
    auto* syncUrlsNoOrigin = new bool{false};
    sync_urls->add_flag("--native", *syncUrlsNative, "Use native sync-urls implementation (default)");
    sync_urls->add_flag("--shell", *syncUrlsShell, "Deprecated compatibility flag (shell path removed)");
    sync_urls->add_flag("--dry-run", *syncUrlsDryRun, "Preview mode");
    sync_urls->add_flag("--init-missing", *syncUrlsInitMissing, "Initialize missing submodules before syncing");
    sync_urls->add_flag("--no-recursive", *syncUrlsNoRecursive, "Disable recursive submodule sync");
    sync_urls->add_flag("--no-origin", *syncUrlsNoOrigin, "Skip updating submodule origin remotes");
    sync_urls->callback([=]() {
        if (*syncUrlsShell && *syncUrlsNative) {
            std::cerr << "Error: --shell cannot be combined with --native\n";
            std::exit(1);
        }
        if (*syncUrlsShell) {
            std::cerr << "Error: --shell is no longer supported; submodule sync-urls is fully native now\n";
            std::exit(2);
        }

        const auto extras = sync_urls->remaining();
        if (!extras.empty()) {
            std::cerr << "Error: unknown arguments for native sync-urls:";
            for (const auto& extra : extras) {
                std::cerr << " " << extra;
            }
            std::cerr << "\n";
            std::exit(1);
        }

        std::exit(RunNativeSyncUrls(
            *syncUrlsDryRun,
            *syncUrlsInitMissing,
            *syncUrlsNoRecursive,
            *syncUrlsNoOrigin
        ));
    });
}

} // namespace kano::git::commands
