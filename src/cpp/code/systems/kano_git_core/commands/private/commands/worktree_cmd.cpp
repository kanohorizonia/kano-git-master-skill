// worktree command — Git worktree management
// Delegates to: scripts/worktree/*.sh

#include "command_registry.hpp"
#include "shell_executor.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <format>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {

struct WorktreeRecord {
    std::string path;
    std::string branch;
    std::string commit;
    bool orphan = false;
    std::string status;
    std::string lastCommit;
};

auto CollectWorktrees() -> std::vector<WorktreeRecord>;
void PrintWorktreesTable(const std::vector<WorktreeRecord>& InRecords, bool InDetailed);

auto Trim(std::string InValue) -> std::string {
    const auto notSpace = [](unsigned char ch) { return !std::isspace(ch); };
    InValue.erase(InValue.begin(), std::find_if(InValue.begin(), InValue.end(), notSpace));
    InValue.erase(std::find_if(InValue.rbegin(), InValue.rend(), notSpace).base(), InValue.end());
    return InValue;
}

auto JsonEscape(const std::string& InValue) -> std::string {
    std::string out;
    out.reserve(InValue.size() + 16);
    for (const char ch : InValue) {
        switch (ch) {
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out.push_back(ch); break;
        }
    }
    return out;
}

auto EnsureGitRepository() -> bool {
    const auto result = kano::git::shell::ExecuteCommand(
        "git",
        {"rev-parse", "--git-dir"},
        kano::git::shell::ExecMode::Capture
    );
    if (result.exitCode == 0) {
        return true;
    }
    std::cerr << "Error: Not in a git repository\n";
    return false;
}

auto GetWorktreeStatus(const std::filesystem::path& InPath) -> std::string {
    const auto result = kano::git::shell::ExecuteCommand(
        "git",
        {"status", "--porcelain"},
        kano::git::shell::ExecMode::Capture,
        InPath
    );
    const auto status = Trim(result.stdoutStr);
    if (status.empty()) {
        return "Clean";
    }

    std::istringstream iss(status);
    std::string line;
    while (std::getline(iss, line)) {
        if (line.rfind("??", 0) == 0) {
            return "Untracked";
        }
    }
    return "Modified";
}

auto GetLastCommit(const std::filesystem::path& InPath) -> std::string {
    const auto result = kano::git::shell::ExecuteCommand(
        "git",
        {"log", "-1", "--format=%h %s"},
        kano::git::shell::ExecMode::Capture,
        InPath
    );
    if (result.exitCode != 0) {
        return "N/A";
    }
    const auto value = Trim(result.stdoutStr);
    return value.empty() ? "N/A" : value;
}

auto IsOrphanBranch(const std::string& InBranch) -> bool {
    if (InBranch.empty() || InBranch == "detached") {
        return false;
    }
    const auto result = kano::git::shell::ExecuteCommand(
        "git",
        {"rev-list", "--parents", InBranch},
        kano::git::shell::ExecMode::Capture
    );
    if (result.exitCode != 0 || result.stdoutStr.empty()) {
        return false;
    }

    std::istringstream iss(result.stdoutStr);
    std::string line;
    std::string lastLine;
    while (std::getline(iss, line)) {
        const auto trimmed = Trim(line);
        if (!trimmed.empty()) {
            lastLine = trimmed;
        }
    }
    if (lastLine.empty()) {
        return false;
    }

    std::istringstream words(lastLine);
    int count = 0;
    std::string word;
    while (words >> word) {
        count += 1;
    }
    return count == 1;
}

auto WorktreeExistsForBranch(const std::string& InBranch) -> bool {
    const auto records = CollectWorktrees();
    return std::any_of(records.begin(), records.end(), [&](const WorktreeRecord& record) {
        return record.branch == InBranch;
    });
}

auto GetWorktreePathByBranch(const std::string& InBranch) -> std::string {
    const auto records = CollectWorktrees();
    for (const auto& record : records) {
        if (record.branch == InBranch) {
            return record.path;
        }
    }
    return {};
}

auto WorktreeHasChanges(const std::string& InPath) -> bool {
    const auto result = kano::git::shell::ExecuteCommand(
        "git",
        {"status", "--porcelain"},
        kano::git::shell::ExecMode::Capture,
        std::filesystem::path(InPath)
    );
    return result.exitCode == 0 && !Trim(result.stdoutStr).empty();
}

auto RunNativeWorktreeRemove(
    const std::string& InBranch,
    bool InForce,
    bool InDeleteBranch,
    bool InDryRun) -> int {
    if (InBranch.empty()) {
        std::cerr << "Error: Branch name is required\n";
        return 1;
    }

    if (!EnsureGitRepository()) {
        return 1;
    }

    if (!WorktreeExistsForBranch(InBranch)) {
        std::cerr << "Error: Worktree does not exist for branch: " << InBranch << "\n";
        return 1;
    }

    const auto worktreePath = GetWorktreePathByBranch(InBranch);
    if (worktreePath.empty()) {
        std::cerr << "Error: Failed to resolve worktree path for branch: " << InBranch << "\n";
        return 1;
    }

    std::cout << "[INFO] Removing worktree for branch: " << InBranch << "\n";
    std::cout << "[INFO] Worktree path: " << worktreePath << "\n";

    if (!InForce && WorktreeHasChanges(worktreePath)) {
        std::cerr << "[ERROR] Worktree has uncommitted changes. Use --force to remove anyway.\n";
        std::cout << "[INFO] Changed files:\n";
        auto statusResult = kano::git::shell::ExecuteCommand(
            "git",
            {"status", "--short"},
            kano::git::shell::ExecMode::PassThrough,
            std::filesystem::path(worktreePath)
        );
        return statusResult.exitCode == 0 ? 1 : statusResult.exitCode;
    }

    if (InDryRun) {
        std::cout << "+ git worktree remove";
        if (InForce) {
            std::cout << " --force";
        }
        std::cout << " \"" << worktreePath << "\"\n";
        if (InDeleteBranch) {
            std::cout << "+ git branch -D \"" << InBranch << "\"\n";
        }
        std::cout << "[INFO] Done!\n";
        return 0;
    }

    std::cout << "[INFO] Removing worktree...\n";
    std::vector<std::string> removeArgs = {"worktree", "remove"};
    if (InForce) {
        removeArgs.push_back("--force");
    }
    removeArgs.push_back(worktreePath);

    auto removeResult = kano::git::shell::ExecuteCommand("git", removeArgs, kano::git::shell::ExecMode::PassThrough);
    if (removeResult.exitCode != 0) {
        return removeResult.exitCode;
    }

    std::cout << "[INFO] Worktree removed successfully!\n";
    if (InDeleteBranch) {
        std::cout << "[INFO] Deleting branch: " << InBranch << "\n";
        auto branchResult = kano::git::shell::ExecuteCommand(
            "git",
            {"branch", "-D", InBranch},
            kano::git::shell::ExecMode::PassThrough
        );
        if (branchResult.exitCode != 0) {
            return branchResult.exitCode;
        }
        std::cout << "[INFO] Branch deleted successfully!\n";
    }

    std::cout << "[INFO] Done!\n";
    return 0;
}

auto BranchExists(const std::string& InBranch) -> bool {
    if (InBranch.empty()) {
        return false;
    }
    const auto result = kano::git::shell::ExecuteCommand(
        "git",
        {"show-ref", "--verify", "--quiet", "refs/heads/" + InBranch},
        kano::git::shell::ExecMode::Capture
    );
    return result.exitCode == 0;
}

auto RepoTopLevelName() -> std::string {
    const auto result = kano::git::shell::ExecuteCommand(
        "git",
        {"rev-parse", "--show-toplevel"},
        kano::git::shell::ExecMode::Capture
    );
    if (result.exitCode != 0) {
        return {};
    }

    const auto rootPath = Trim(result.stdoutStr);
    if (rootPath.empty()) {
        return {};
    }
    return std::filesystem::path(rootPath).filename().string();
}

auto MakeDefaultWorktreePath(const std::string& InBranch) -> std::string {
    const auto repoName = RepoTopLevelName();
    if (repoName.empty()) {
        return {};
    }

    std::string safeBranch = InBranch;
    for (auto& ch : safeBranch) {
        if (ch == '/') {
            ch = '-';
        }
    }
    return std::string{"../"} + repoName + "-" + safeBranch;
}

auto WorktreePathExists(const std::string& InPath) -> bool {
    if (InPath.empty()) {
        return false;
    }
    return std::filesystem::exists(std::filesystem::path(InPath));
}

auto RunNativeWorktreeCreate(
    const std::string& InBranch,
    const std::string& InPath,
    bool InNewBranch,
    bool InDryRun,
    bool InOpen,
    const std::string& InIde) -> int {
    if (InBranch.empty()) {
        std::cerr << "Error: Branch name is required\n";
        return 1;
    }
    if (!EnsureGitRepository()) {
        return 1;
    }

    const auto worktreePath = InPath.empty() ? MakeDefaultWorktreePath(InBranch) : InPath;
    if (worktreePath.empty()) {
        std::cerr << "Error: Failed to resolve worktree path\n";
        return 1;
    }

    std::cout << "[INFO] Creating worktree for branch: " << InBranch << "\n";
    std::cout << "[INFO] Worktree path: " << worktreePath << "\n";

    if (WorktreeExistsForBranch(InBranch)) {
        const auto existingPath = GetWorktreePathByBranch(InBranch);
        std::cerr << "[ERROR] Worktree already exists for branch '" << InBranch << "' at: " << existingPath << "\n";
        return 1;
    }

    if (WorktreePathExists(worktreePath)) {
        std::cerr << "[ERROR] Path already exists: " << worktreePath << "\n";
        return 1;
    }

    if (!InNewBranch && !BranchExists(InBranch)) {
        std::cerr << "[ERROR] Branch '" << InBranch << "' does not exist. Use --new-branch to create it.\n";
        return 1;
    }

    std::vector<std::string> addArgs = {"worktree", "add"};
    if (InNewBranch) {
        addArgs.push_back("-b");
        addArgs.push_back(InBranch);
        addArgs.push_back(worktreePath);
    } else {
        addArgs.push_back(worktreePath);
        addArgs.push_back(InBranch);
    }

    if (InDryRun) {
        std::cout << "+ git";
        for (const auto& arg : addArgs) {
            std::cout << " \"" << arg << "\"";
        }
        std::cout << "\n";
        std::cout << "[INFO] Done!\n";
        return 0;
    }

    if (InNewBranch) {
        std::cout << "[INFO] Creating new branch and worktree...\n";
    } else {
        std::cout << "[INFO] Creating worktree for existing branch...\n";
    }

    const auto addResult = kano::git::shell::ExecuteCommand("git", addArgs, kano::git::shell::ExecMode::PassThrough);
    if (addResult.exitCode != 0) {
        return addResult.exitCode;
    }

    std::cout << "[INFO] Worktree created successfully!\n";
    std::cout << "[INFO] Path: " << worktreePath << "\n";
    std::cout << "[INFO] Branch: " << InBranch << "\n";

    if (InOpen) {
        std::vector<std::string> openArgs = {InBranch};
        if (!InIde.empty() && InIde != "auto") {
            openArgs.push_back("--ide");
            openArgs.push_back(InIde);
        }
        const auto openResult = kano::git::shell::ExecuteScript("worktree/open-worktree.sh", openArgs);
        if (openResult.exitCode != 0) {
            return openResult.exitCode;
        }
    }

    std::cout << "[INFO] Done!\n";
    return 0;
}

auto SplitCsv(const std::string& InCsv) -> std::vector<std::string> {
    std::vector<std::string> out;
    std::istringstream iss(InCsv);
    std::string item;
    while (std::getline(iss, item, ',')) {
        auto trimmed = Trim(item);
        if (!trimmed.empty()) {
            out.push_back(trimmed);
        }
    }
    return out;
}

auto ContainsBranchFilter(const std::vector<std::string>& InFilters, const std::string& InBranch) -> bool {
    if (InFilters.empty()) {
        return true;
    }
    return std::find(InFilters.begin(), InFilters.end(), InBranch) != InFilters.end();
}

auto RunNativeWorktreeSync(
    bool InShowStatus,
    const std::string& InFilterWorktrees,
    bool InDryRun) -> int {
    if (!EnsureGitRepository()) {
        return 1;
    }

    const auto filters = SplitCsv(InFilterWorktrees);
    const auto records = CollectWorktrees();

    std::cout << "[INFO] Syncing worktrees...\n";
    int syncedCount = 0;
    int failedCount = 0;

    for (const auto& record : records) {
        if (record.branch.empty() || record.branch == "detached") {
            continue;
        }
        if (!ContainsBranchFilter(filters, record.branch)) {
            continue;
        }

        std::cout << "[INFO] Syncing: " << record.path << " (" << record.branch << ")\n";
        if (InDryRun) {
            std::cout << "+ cd \"" << record.path << "\"\n";
            std::cout << "+ git fetch --all --prune\n";
            std::cout << "+ git pull --rebase\n";
            continue;
        }

        const auto fetchResult = kano::git::shell::ExecuteCommand(
            "git",
            {"fetch", "--all", "--prune"},
            kano::git::shell::ExecMode::PassThrough,
            std::filesystem::path(record.path)
        );
        if (fetchResult.exitCode != 0) {
            std::cerr << "[ERROR]   Failed to fetch\n";
            failedCount += 1;
            continue;
        }
        std::cout << "[INFO]   Fetched successfully\n";

        const auto pullResult = kano::git::shell::ExecuteCommand(
            "git",
            {"pull", "--rebase"},
            kano::git::shell::ExecMode::PassThrough,
            std::filesystem::path(record.path)
        );
        if (pullResult.exitCode != 0) {
            std::cerr << "[ERROR]   Failed to pull (may have conflicts)\n";
            failedCount += 1;
            continue;
        }
        std::cout << "[INFO]   Pulled successfully\n";
        syncedCount += 1;
    }

    std::cout << "[INFO] Sync complete!\n";
    if (!InDryRun) {
        std::cout << "[INFO] Synced: " << syncedCount << ", Failed: " << failedCount << "\n";
    }

    if (InShowStatus && !InDryRun) {
        std::cout << "[INFO] \n";
        std::cout << "[INFO] Worktree Status:\n";
        PrintWorktreesTable(CollectWorktrees(), false);
    }

    return failedCount > 0 ? 1 : 0;
}

auto CollectWorktrees() -> std::vector<WorktreeRecord> {
    const auto result = kano::git::shell::ExecuteCommand(
        "git",
        {"worktree", "list", "--porcelain"},
        kano::git::shell::ExecMode::Capture
    );
    if (result.exitCode != 0) {
        return {};
    }

    std::vector<WorktreeRecord> records;
    WorktreeRecord current;
    bool haveCurrent = false;

    std::istringstream iss(result.stdoutStr);
    std::string line;
    while (std::getline(iss, line)) {
        if (line.empty()) {
            if (haveCurrent && !current.path.empty()) {
                const auto pathObj = std::filesystem::path(current.path);
                current.orphan = IsOrphanBranch(current.branch);
                current.status = GetWorktreeStatus(pathObj);
                current.lastCommit = GetLastCommit(pathObj);
                records.push_back(current);
            }
            current = WorktreeRecord{};
            haveCurrent = false;
            continue;
        }

        haveCurrent = true;
        if (line.rfind("worktree ", 0) == 0) {
            current.path = line.substr(9);
            continue;
        }
        if (line.rfind("branch refs/heads/", 0) == 0) {
            current.branch = line.substr(18);
            continue;
        }
        if (line.rfind("HEAD ", 0) == 0) {
            current.commit = line.substr(5);
            continue;
        }
    }

    if (haveCurrent && !current.path.empty()) {
        const auto pathObj = std::filesystem::path(current.path);
        current.orphan = IsOrphanBranch(current.branch);
        current.status = GetWorktreeStatus(pathObj);
        current.lastCommit = GetLastCommit(pathObj);
        records.push_back(current);
    }

    return records;
}

void PrintWorktreesJson(const std::vector<WorktreeRecord>& InRecords) {
    std::cout << "[\n";
    for (std::size_t i = 0; i < InRecords.size(); ++i) {
        const auto& record = InRecords[i];
        std::cout << "  {\n";
        std::cout << std::format("    \"path\": \"{}\",\n", JsonEscape(record.path));
        std::cout << std::format("    \"branch\": \"{}\",\n", JsonEscape(record.branch.empty() ? "detached" : record.branch));
        std::cout << std::format("    \"commit\": \"{}\",\n", JsonEscape(record.commit));
        std::cout << std::format("    \"orphan\": {},\n", record.orphan ? "true" : "false");
        std::cout << std::format("    \"status\": \"{}\",\n", JsonEscape(record.status));
        std::cout << std::format("    \"last_commit\": \"{}\"\n", JsonEscape(record.lastCommit));
        std::cout << "  }";
        if (i + 1 < InRecords.size()) {
            std::cout << ",";
        }
        std::cout << "\n";
    }
    std::cout << "]\n";
}

void PrintWorktreesTable(const std::vector<WorktreeRecord>& InRecords, bool InDetailed) {
    if (InDetailed) {
        std::cout << std::format("{:<40} {:<20} {:<8} {:<12} {}\n", "Worktree", "Branch", "Orphan", "Status", "Last Commit");
        std::cout << std::format("{:<40} {:<20} {:<8} {:<12} {}\n", "========", "======", "======", "======", "===========");
        for (const auto& record : InRecords) {
            std::cout << std::format(
                "{:<40} {:<20} {:<8} {:<12} {}\n",
                record.path,
                record.branch.empty() ? "detached" : record.branch,
                record.orphan ? "Yes" : "No",
                record.status,
                record.lastCommit
            );
        }
        return;
    }

    std::cout << std::format("{:<40} {:<20} {:<8} {:<12}\n", "Worktree", "Branch", "Orphan", "Status");
    std::cout << std::format("{:<40} {:<20} {:<8} {:<12}\n", "========", "======", "======", "======");
    for (const auto& record : InRecords) {
        std::cout << std::format(
            "{:<40} {:<20} {:<8} {:<12}\n",
            record.path,
            record.branch.empty() ? "detached" : record.branch,
            record.orphan ? "Yes" : "No",
            record.status
        );
    }
}

} // namespace

namespace kano::git::commands {

void RegisterWorktree(CLI::App& InApp) {
    auto* cmd = InApp.add_subcommand("worktree", "Git worktree management");

    auto* create = cmd->add_subcommand("create", "Create a new worktree");
    create->allow_extras();
    auto* createNative = new bool{false};
    auto* createShell = new bool{false};
    auto* createPath = new std::string{};
    auto* createNewBranch = new bool{false};
    auto* createOpen = new bool{false};
    auto* createIde = new std::string{"auto"};
    auto* createDryRun = new bool{false};
    auto* createBranch = new std::string{};
    create->add_flag("--native", *createNative, "Use native C++ worktree create implementation (default)");
    create->add_flag("--shell", *createShell, "Use shell fallback implementation");
    create->add_option("--path", *createPath, "Custom worktree path");
    create->add_flag("--new-branch", *createNewBranch, "Create new branch");
    create->add_flag("--open", *createOpen, "Open in IDE after creation");
    create->add_option("--ide", *createIde, "IDE to use: auto, code, idea, vim, terminal");
    create->add_flag("--dry-run", *createDryRun, "Preview mode");
    create->add_option("branch", *createBranch, "Branch name");
    create->callback([=]() {
        if (*createShell && *createNative) {
            std::cerr << "Error: --shell cannot be combined with --native\n";
            std::exit(1);
        }

        auto extras = create->remaining();
        std::string branch = *createBranch;
        if (branch.empty() && !extras.empty()) {
            branch = extras.front();
            extras.erase(extras.begin());
        }

        if (!*createShell) {
            if (!extras.empty()) {
                std::cerr << "Error: Unexpected argument: " << extras.front() << "\n";
                std::exit(1);
            }

            std::exit(RunNativeWorktreeCreate(
                branch,
                *createPath,
                *createNewBranch,
                *createDryRun,
                *createOpen,
                *createIde
            ));
        }

        std::vector<std::string> args;
        if (!branch.empty()) {
            args.push_back(branch);
        }
        if (!createPath->empty()) {
            args.push_back("--path");
            args.push_back(*createPath);
        }
        if (*createNewBranch) {
            args.push_back("--new-branch");
        }
        if (*createOpen) {
            args.push_back("--open");
        }
        if (!createIde->empty()) {
            args.push_back("--ide");
            args.push_back(*createIde);
        }
        if (*createDryRun) {
            args.push_back("--dry-run");
        }
        args.insert(args.end(), extras.begin(), extras.end());
        auto result = shell::ExecuteScript("worktree/create-worktree.sh", args);
        std::exit(result.exitCode);
    });

    auto* list = cmd->add_subcommand("list", "List all worktrees");
    list->allow_extras();
    auto* listNative = new bool{false};
    auto* listShell = new bool{false};
    auto* listFormat = new std::string{"table"};
    auto* listDetailed = new bool{false};
    list->add_flag("--native", *listNative, "Use native C++ worktree list implementation (default)");
    list->add_flag("--shell", *listShell, "Use shell fallback implementation");
    list->add_option("--format", *listFormat, "Output format: table|json");
    list->add_flag("--detailed", *listDetailed, "Show detailed information");
    list->callback([=]() {
        if (!*listShell) {
            if (*listFormat != "table" && *listFormat != "json") {
                std::cerr << "Error: Unknown format: " << *listFormat << "\n";
                std::exit(1);
            }
            if (!EnsureGitRepository()) {
                std::exit(1);
            }

            const auto records = CollectWorktrees();
            if (*listFormat == "json") {
                PrintWorktreesJson(records);
            } else {
                PrintWorktreesTable(records, *listDetailed);
            }
            std::exit(0);
        }

        auto extras = list->remaining();
        std::vector<std::string> args(extras.begin(), extras.end());
        auto result = shell::ExecuteScript("worktree/list-worktrees.sh", args);
        std::exit(result.exitCode);
    });

    auto* remove = cmd->add_subcommand("remove", "Remove a worktree");
    remove->allow_extras();
    auto* removeNative = new bool{false};
    auto* removeShell = new bool{false};
    auto* removeForce = new bool{false};
    auto* removeDeleteBranch = new bool{false};
    auto* removeDryRun = new bool{false};
    auto* removeBranch = new std::string{};
    remove->add_flag("--native", *removeNative, "Use native C++ worktree remove implementation (default)");
    remove->add_flag("--shell", *removeShell, "Use shell fallback implementation");
    remove->add_flag("--force", *removeForce, "Force removal even with uncommitted changes");
    remove->add_flag("--delete-branch", *removeDeleteBranch, "Also delete the branch");
    remove->add_flag("--dry-run", *removeDryRun, "Preview mode");
    remove->add_option("branch", *removeBranch, "Branch name");
    remove->callback([=]() {
        if (*removeShell && *removeNative) {
            std::cerr << "Error: --shell cannot be combined with --native\n";
            std::exit(1);
        }

        auto extras = remove->remaining();
        std::string branch = *removeBranch;
        if (branch.empty() && !extras.empty()) {
            branch = extras.front();
            extras.erase(extras.begin());
        }

        if (!*removeShell) {
            if (!extras.empty()) {
                std::cerr << "Error: Unexpected argument: " << extras.front() << "\n";
                std::exit(1);
            }

            std::exit(RunNativeWorktreeRemove(
                branch,
                *removeForce,
                *removeDeleteBranch,
                *removeDryRun
            ));
        }

        std::vector<std::string> args;
        if (!branch.empty()) {
            args.push_back(branch);
        }
        if (*removeForce) {
            args.push_back("--force");
        }
        if (*removeDeleteBranch) {
            args.push_back("--delete-branch");
        }
        if (*removeDryRun) {
            args.push_back("--dry-run");
        }
        args.insert(args.end(), extras.begin(), extras.end());
        auto result = shell::ExecuteScript("worktree/remove-worktree.sh", args);
        std::exit(result.exitCode);
    });

    auto* sync_wt = cmd->add_subcommand("sync", "Sync all worktrees");
    sync_wt->allow_extras();
    auto* syncNative = new bool{false};
    auto* syncShell = new bool{false};
    auto* syncShowStatus = new bool{false};
    auto* syncFilterWorktrees = new std::string{};
    auto* syncDryRun = new bool{false};
    sync_wt->add_flag("--native", *syncNative, "Use native C++ worktree sync implementation (default)");
    sync_wt->add_flag("--shell", *syncShell, "Use shell fallback implementation");
    sync_wt->add_flag("--status", *syncShowStatus, "Show status after sync");
    sync_wt->add_option("--worktrees", *syncFilterWorktrees, "Comma-separated list of branches to sync");
    sync_wt->add_flag("--dry-run", *syncDryRun, "Preview mode");
    sync_wt->callback([=]() {
        if (*syncShell && *syncNative) {
            std::cerr << "Error: --shell cannot be combined with --native\n";
            std::exit(1);
        }

        auto extras = sync_wt->remaining();
        if (!*syncShell) {
            if (!extras.empty()) {
                std::cerr << "Error: Unexpected argument: " << extras.front() << "\n";
                std::exit(1);
            }
            std::exit(RunNativeWorktreeSync(
                *syncShowStatus,
                *syncFilterWorktrees,
                *syncDryRun
            ));
        }

        std::vector<std::string> args;
        if (*syncShowStatus) {
            args.push_back("--status");
        }
        if (!syncFilterWorktrees->empty()) {
            args.push_back("--worktrees");
            args.push_back(*syncFilterWorktrees);
        }
        if (*syncDryRun) {
            args.push_back("--dry-run");
        }
        args.insert(args.end(), extras.begin(), extras.end());
        auto result = shell::ExecuteScript("worktree/sync-worktrees.sh", args);
        std::exit(result.exitCode);
    });

    auto* open = cmd->add_subcommand("open", "Open worktree in IDE");
    open->allow_extras();
    open->callback([=]() {
        auto extras = open->remaining();
        std::vector<std::string> args(extras.begin(), extras.end());
        auto result = shell::ExecuteScript("worktree/open-worktree.sh", args);
        std::exit(result.exitCode);
    });

    auto* orphan = cmd->add_subcommand("create-orphan", "Create orphan branch worktree");
    orphan->allow_extras();
    orphan->callback([=]() {
        auto extras = orphan->remaining();
        std::vector<std::string> args(extras.begin(), extras.end());
        auto result = shell::ExecuteScript("worktree/create-orphan-worktree.sh", args);
        std::exit(result.exitCode);
    });
}

} // namespace kano::git::commands
