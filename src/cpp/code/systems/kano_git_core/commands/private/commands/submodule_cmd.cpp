// submodule command — Git submodule management

#include "command_registry.hpp"
#include "shell_executor.hpp"

#include <filesystem>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace {

struct SyncUrlEntry {
    std::string name;
    std::string path;
    std::string url;
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
    add->callback([=]() {
        auto extras = add->remaining();
        std::vector<std::string> args = {"add"};
        args.insert(args.end(), extras.begin(), extras.end());
        auto result = shell::ExecuteScript("submodules/kog-submodule.sh", args);
        std::exit(result.exitCode);
    });

    auto* sync = cmd->add_subcommand("sync", "Sync submodule URLs");
    sync->allow_extras();
    auto* syncNative = new bool{false};
    auto* syncShell = new bool{false};
    auto* syncRecursive = new bool{false};
    auto* syncDryRun = new bool{false};
    auto* syncPath = new std::string{};
    sync->add_flag("--native", *syncNative, "Use native git submodule sync implementation (default)");
    sync->add_flag("--shell", *syncShell, "Use shell fallback implementation");
    sync->add_flag("--recursive", *syncRecursive, "Sync recursively");
    sync->add_flag("--dry-run", *syncDryRun, "Preview mode");
    sync->add_option("path", *syncPath, "Optional submodule path");
    sync->callback([=]() {
        if (*syncShell && *syncNative) {
            std::cerr << "Error: --shell cannot be combined with --native\n";
            std::exit(1);
        }
        if (!*syncShell) {
            const auto gitArgs = BuildGitSubmoduleArgs("sync", *syncRecursive, false, *syncPath, {});
            std::exit(RunNativeSubmodule(gitArgs, *syncDryRun, "Syncing submodule URLs..."));
        }

        auto extras = sync->remaining();
        std::vector<std::string> args = {"sync"};
        if (*syncRecursive) {
            args.push_back("--recursive");
        }
        if (*syncDryRun) {
            args.push_back("--dry-run");
        }
        if (!syncPath->empty()) {
            args.push_back(*syncPath);
        }
        args.insert(args.end(), extras.begin(), extras.end());
        auto result = shell::ExecuteScript("submodules/kog-submodule.sh", args);
        std::exit(result.exitCode);
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
    update->add_flag("--shell", *updateShell, "Use shell fallback implementation");
    update->add_flag("--recursive", *updateRecursive, "Update recursively");
    update->add_flag("--remote", *updateRemote, "Update to latest remote tracked commit");
    update->add_flag("--dry-run", *updateDryRun, "Preview mode");
    update->add_option("path", *updatePath, "Optional submodule path");
    update->callback([=]() {
        if (*updateShell && *updateNative) {
            std::cerr << "Error: --shell cannot be combined with --native\n";
            std::exit(1);
        }
        if (!*updateShell) {
            const auto gitArgs = BuildGitSubmoduleArgs("update", *updateRecursive, *updateRemote, *updatePath, "--init");
            std::exit(RunNativeSubmodule(gitArgs, *updateDryRun, "Updating submodules..."));
        }

        auto extras = update->remaining();
        std::vector<std::string> args;
        if (*updateRecursive) {
            args.push_back("--recursive");
        }
        if (*updateRemote) {
            args.push_back("--remote");
        }
        if (*updateDryRun) {
            args.push_back("--dry-run");
        }
        if (!updatePath->empty()) {
            args.push_back(*updatePath);
        }
        args.insert(args.end(), extras.begin(), extras.end());
        auto result = shell::ExecuteScript("submodules/update-submodules.sh", args);
        std::exit(result.exitCode);
    });

    auto* remove = cmd->add_subcommand("remove", "Remove a submodule");
    remove->allow_extras();
    auto* removeNative = new bool{false};
    auto* removeShell = new bool{false};
    auto* removeDryRun = new bool{false};
    auto* removePath = new std::string{};
    remove->add_flag("--native", *removeNative, "Use native submodule remove implementation (default)");
    remove->add_flag("--shell", *removeShell, "Use shell fallback implementation");
    remove->add_flag("--dry-run", *removeDryRun, "Preview mode");
    remove->add_option("path", *removePath, "Submodule path to remove");
    remove->callback([=]() {
        if (*removeShell && *removeNative) {
            std::cerr << "Error: --shell cannot be combined with --native\n";
            std::exit(1);
        }

        auto extras = remove->remaining();
        if (!*removeShell) {
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
        }

        std::vector<std::string> args;
        if (!removePath->empty()) {
            args.push_back(*removePath);
        }
        if (*removeDryRun) {
            args.push_back("--dry-run");
        }
        args.insert(args.end(), extras.begin(), extras.end());
        auto result = shell::ExecuteScript("submodules/remove-submodule.sh", args);
        std::exit(result.exitCode);
    });

    auto* foreach = cmd->add_subcommand("foreach", "Run command on each submodule");
    foreach->allow_extras();
    auto* foreachNative = new bool{false};
    auto* foreachShell = new bool{false};
    auto* foreachRecursive = new bool{false};
    auto* foreachDryRun = new bool{false};
    auto* foreachCommand = new std::string{};
    foreach->add_flag("--native", *foreachNative, "Use native git submodule foreach implementation (default)");
    foreach->add_flag("--shell", *foreachShell, "Use shell fallback implementation");
    foreach->add_flag("--recursive", *foreachRecursive, "Execute recursively");
    foreach->add_flag("--dry-run", *foreachDryRun, "Preview mode");
    foreach->add_option("--command", *foreachCommand, "Command to execute in each submodule");
    foreach->add_option("cmd", *foreachCommand, "Positional command to execute in each submodule");
    foreach->callback([=]() {
        if (*foreachShell && *foreachNative) {
            std::cerr << "Error: --shell cannot be combined with --native\n";
            std::exit(1);
        }
        if (!*foreachShell) {
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
        }

        auto extras = foreach->remaining();
        std::vector<std::string> args;
        if (*foreachRecursive) {
            args.push_back("--recursive");
        }
        if (*foreachDryRun) {
            args.push_back("--dry-run");
        }
        if (!foreachCommand->empty()) {
            args.push_back(*foreachCommand);
        }
        args.insert(args.end(), extras.begin(), extras.end());
        auto result = shell::ExecuteScript("submodules/foreach-submodule.sh", args);
        std::exit(result.exitCode);
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
    sync_urls->add_flag("--shell", *syncUrlsShell, "Use shell fallback implementation");
    sync_urls->add_flag("--dry-run", *syncUrlsDryRun, "Preview mode");
    sync_urls->add_flag("--init-missing", *syncUrlsInitMissing, "Initialize missing submodules before syncing");
    sync_urls->add_flag("--no-recursive", *syncUrlsNoRecursive, "Disable recursive submodule sync");
    sync_urls->add_flag("--no-origin", *syncUrlsNoOrigin, "Skip updating submodule origin remotes");
    sync_urls->callback([=]() {
        if (*syncUrlsShell && *syncUrlsNative) {
            std::cerr << "Error: --shell cannot be combined with --native\n";
            std::exit(1);
        }

        if (!*syncUrlsShell) {
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
        }

        auto extras = sync_urls->remaining();
        std::vector<std::string> args;
        if (*syncUrlsDryRun) {
            args.push_back("--dry-run");
        }
        if (*syncUrlsInitMissing) {
            args.push_back("--init-missing");
        }
        if (*syncUrlsNoRecursive) {
            args.push_back("--no-recursive");
        }
        if (*syncUrlsNoOrigin) {
            args.push_back("--no-origin");
        }
        args.insert(args.end(), extras.begin(), extras.end());
        auto result = shell::ExecuteScript("submodules/sync-urls.sh", args);
        std::exit(result.exitCode);
    });
}

} // namespace kano::git::commands
