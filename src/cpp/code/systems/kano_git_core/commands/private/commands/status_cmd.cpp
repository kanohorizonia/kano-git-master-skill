// status command — global cross-repo status view

#include "command_registry.hpp"
#include "discovery.hpp"
#include "shell_executor.hpp"

#include <algorithm>
#include <filesystem>
#include <format>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <set>
#include <string>
#include <vector>

namespace kano::git::commands {
namespace {

struct RepoView {
    std::filesystem::path path;
    std::string group;
    std::string repoName;
    std::string type;
    std::string branch;
    std::string remote;
    std::string tracking;
    bool repoDirty = false;
    bool hasDirtyWorktree = false;
    std::string dirtyWorktrees;
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
    return shell::ExecuteCommand("git", InArgs, shell::ExecMode::Capture, InRepo);
}

auto CurrentBranch(const std::filesystem::path& InRepo) -> std::string {
    const auto out = GitCapture(InRepo, {"symbolic-ref", "--short", "HEAD"});
    if (out.exitCode != 0) {
        return "(detached)";
    }
    const auto value = Trim(out.stdoutStr);
    return value.empty() ? "(detached)" : value;
}

auto CurrentRemote(const std::filesystem::path& InRepo) -> std::string {
    const auto out = GitCapture(InRepo, {"rev-parse", "--abbrev-ref", "--symbolic-full-name", "@{upstream}"});
    if (out.exitCode != 0) {
        return "(none)";
    }
    const auto value = Trim(out.stdoutStr);
    return value.empty() ? "(none)" : value;
}

auto TrackingSummary(const std::filesystem::path& InRepo) -> std::string {
    const auto out = GitCapture(InRepo, {"status", "--porcelain=v1", "-b"});
    if (out.exitCode != 0) {
        return "unknown";
    }

    std::istringstream iss(out.stdoutStr);
    std::string first;
    if (!std::getline(iss, first)) {
        return "unknown";
    }

    // examples: ## main...origin/main [ahead 1]
    //           ## main...origin/main [ahead 2, behind 1]
    //           ## main
    const auto lb = first.find('[');
    const auto rb = first.find(']');
    if (lb != std::string::npos && rb != std::string::npos && rb > lb) {
        return first.substr(lb + 1, rb - lb - 1);
    }

    if (first.find("...") != std::string::npos) {
        return "up-to-date";
    }

    return "no-upstream";
}

auto HasDirtyWorktrees(const std::filesystem::path& InRepo, std::string& OutDirtyList) -> bool {
    const auto out = GitCapture(InRepo, {"worktree", "list", "--porcelain"});
    if (out.exitCode != 0) {
        OutDirtyList.clear();
        return false;
    }

    std::istringstream iss(out.stdoutStr);
    std::string line;
    std::string currentWorktree;
    std::vector<std::string> dirty;

    while (std::getline(iss, line)) {
        line = Trim(line);
        if (line.rfind("worktree ", 0) == 0) {
            currentWorktree = line.substr(9);
            continue;
        }
        if (line == "dirty" && !currentWorktree.empty()) {
            dirty.push_back(currentWorktree);
        }
    }

    if (dirty.empty()) {
        OutDirtyList.clear();
        return false;
    }

    std::ostringstream joined;
    for (std::size_t i = 0; i < dirty.size(); ++i) {
        if (i > 0) {
            joined << ",";
        }
        joined << dirty[i];
    }

    OutDirtyList = joined.str();
    return true;
}

auto RelativeDisplayPath(const std::filesystem::path& InRoot, const std::filesystem::path& InPath) -> std::filesystem::path {
    const auto normalizedRoot = InRoot.lexically_normal();
    const auto normalizedPath = InPath.lexically_normal();
    const auto relative = normalizedPath.lexically_relative(normalizedRoot);
    if (!relative.empty()) {
        return relative;
    }
    return normalizedPath;
}

auto GroupFromRelativePath(const std::filesystem::path& InRelativePath) -> std::string {
    const auto parent = InRelativePath.parent_path().generic_string();
    if (parent.empty() || parent == ".") {
        return ".";
    }
    return parent;
}

auto RepoNameFromPath(const std::filesystem::path& InPath) -> std::string {
    const auto name = InPath.filename().generic_string();
    if (!name.empty()) {
        return name;
    }
    return InPath.lexically_normal().generic_string();
}

auto FormatTable(const std::vector<RepoView>& InRows) -> std::string {
    std::ostringstream oss;
    std::set<std::string> groups;
    std::size_t dirtyCount = 0;
    for (const auto& row : InRows) {
        groups.insert(row.group);
        if (row.repoDirty) {
            dirtyCount += 1;
        }
    }

    oss << "SUMMARY: repos=" << InRows.size() << ", dirty=" << dirtyCount << ", groups=" << groups.size() << "\n";

    if (!InRows.empty()) {
        oss << std::left
            << std::setw(6) << "#"
            << std::setw(24) << "REPO"
            << std::setw(16) << "BRANCH"
            << std::setw(24) << "REMOTE"
            << std::setw(16) << "TRACKING"
            << std::setw(8) << "DIRTY"
            << std::setw(12) << "WT_DIRTY"
            << "TYPE"
            << "\n";
    }

    std::string currentGroup;
    for (std::size_t i = 0; i < InRows.size(); ++i) {
        const auto& row = InRows[i];
        if (currentGroup != row.group) {
            currentGroup = row.group;
            oss << "\nGROUP: " << currentGroup << "\n";
        }

        auto repoName = row.repoName;
        if (repoName.size() > 22) {
            repoName = repoName.substr(0, 19) + "...";
        }
        auto branch = row.branch;
        if (branch.size() > 14) {
            branch = branch.substr(0, 11) + "...";
        }
        auto remote = row.remote;
        if (remote.size() > 22) {
            remote = remote.substr(0, 19) + "...";
        }

        oss << std::left
            << std::setw(6) << std::to_string(i + 1)
            << std::setw(24) << repoName
            << std::setw(16) << branch
            << std::setw(24) << remote
            << std::setw(16) << row.tracking
            << std::setw(8) << (row.repoDirty ? "yes" : "no")
            << std::setw(12) << (row.hasDirtyWorktree ? "yes" : "no")
            << row.type
            << "\n";
    }

    return oss.str();
}

auto FormatJson(const std::vector<RepoView>& InRows) -> std::string {
    std::ostringstream out;
    std::size_t dirtyCount = 0;
    for (const auto& row : InRows) {
        if (row.repoDirty) {
            dirtyCount += 1;
        }
    }

    out << "{\"summary\":{";
    out << std::format("\"repo_count\":{},\"dirty_count\":{}", InRows.size(), dirtyCount);
    out << "},\"repos\":[";
    for (std::size_t i = 0; i < InRows.size(); ++i) {
        if (i > 0) {
            out << ",";
        }
        const auto& row = InRows[i];
        out << "{";
        out << std::format("\"index\":{},", i + 1);
        out << std::format("\"path\":\"{}\",", row.path.lexically_normal().generic_string());
        out << std::format("\"group\":\"{}\",", row.group);
        out << std::format("\"repo_name\":\"{}\",", row.repoName);
        out << std::format("\"type\":\"{}\",", row.type);
        out << std::format("\"branch\":\"{}\",", row.branch);
        out << std::format("\"remote\":\"{}\",", row.remote);
        out << std::format("\"tracking\":\"{}\",", row.tracking);
        out << std::format("\"dirty\":{},", row.repoDirty ? "true" : "false");
        out << std::format("\"worktree_dirty\":{}", row.hasDirtyWorktree ? "true" : "false");
        if (row.hasDirtyWorktree) {
            out << std::format(",\"dirty_worktrees\":\"{}\"", row.dirtyWorktrees);
        }
        out << "}";
    }
    out << "]}";
    return out.str();
}

} // namespace

void RegisterStatus(CLI::App& InApp) {
    auto* cmd = InApp.add_subcommand("status", "Global cross-repo status view (branch/upstream/dirty/worktree)");

    auto* format = new std::string{"table"};
    auto* maxDepth = new int{8};
    auto* exclude = new std::vector<std::string>{};
    auto* noCache = new bool{false};
    auto* noRefreshCache = new bool{false};

    cmd->add_option("--format", *format, "Output format: table|json")->default_str("table");
    cmd->add_option("--max-depth", *maxDepth, "Discovery max depth");
    cmd->add_option("--exclude", *exclude, "Temporary scan-scope exclude override for this invocation only (repeatable; prefer .gitignore/.kogignore for shared policy)");
    cmd->add_flag("--no-cache", *noCache, "Disable discovery cache for this run");
    cmd->add_flag("--no-refresh-cache", *noRefreshCache, "Do not force cache refresh");

    cmd->callback([format, maxDepth, exclude, noCache, noRefreshCache]() {
        if (*format != "table" && *format != "json") {
            std::cerr << "Error: invalid --format value: " << *format << " (expected table|json)\n";
            std::exit(1);
        }

        workspace::DiscoverOptions options;
        options.rootDir = std::filesystem::current_path();
        options.maxDepth = *maxDepth;
        options.excludePatterns = *exclude;
        options.useCache = !*noCache;
        options.refreshCache = !*noRefreshCache;
        options.metadataLevel = "full";

        const auto discovery = workspace::DiscoverRepos(options);
        std::vector<RepoView> rows;
        rows.reserve(discovery.repos.size());

        for (const auto& repo : discovery.repos) {
            RepoView row;
            row.path = repo.path;
            const auto relativePath = RelativeDisplayPath(options.rootDir, repo.path);
            row.group = GroupFromRelativePath(relativePath);
            row.repoName = RepoNameFromPath(repo.path);
            row.type = repo.type;
            row.repoDirty = repo.hasChanges;

            row.branch = CurrentBranch(repo.path);
            row.remote = CurrentRemote(repo.path);
            row.tracking = TrackingSummary(repo.path);
            row.hasDirtyWorktree = HasDirtyWorktrees(repo.path, row.dirtyWorktrees);

            rows.push_back(std::move(row));
        }

        std::sort(rows.begin(), rows.end(), [](const RepoView& A, const RepoView& B) {
            if (A.group != B.group) {
                return A.group < B.group;
            }
            if (A.repoName != B.repoName) {
                return A.repoName < B.repoName;
            }
            return A.path.lexically_normal().generic_string() < B.path.lexically_normal().generic_string();
        });

        if (*format == "json") {
            std::cout << FormatJson(rows) << '\n';
        } else {
            std::cout << FormatTable(rows) << '\n';
        }
    });
}

} // namespace kano::git::commands
