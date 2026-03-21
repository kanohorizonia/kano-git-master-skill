// submodule command — Git submodule management

#include <CLI/CLI.hpp>
#include "discovery.hpp"
#include "shell_executor.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

#include <chrono>

namespace {

struct SyncUrlEntry {
    std::string name;
    std::string path;
    std::string url;
};

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
        const auto gitArgs = BuildGitSubmoduleArgs("update", *updateRecursive, *updateRemote, *updatePath, "--init");
        std::exit(RunNativeSubmodule(gitArgs, *updateDryRun, "Updating submodules..."));
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
