// dirty command — show repositories with dirty content/worktrees

#include <CLI/CLI.hpp>
#include "discovery.hpp"
#include "shell_executor.hpp"

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace kano::git::commands {
namespace {

struct DirtyEntry {
    std::filesystem::path path;
    std::string relativePath;
    std::string group;
    std::string repoName;
    bool repoDirty = false;
    bool worktreeDirty = false;
    std::string dirtyWorktrees;
    std::vector<std::string> statusLines;
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

auto SplitNonEmptyLines(const std::string& InText) -> std::vector<std::string> {
    std::vector<std::string> out;
    std::istringstream iss(InText);
    std::string line;
    while (std::getline(iss, line)) {
        line = Trim(line);
        if (!line.empty()) {
            out.push_back(line);
        }
    }
    return out;
}

auto RelativeDisplayPath(const std::filesystem::path& InRoot, const std::filesystem::path& InPath) -> std::filesystem::path {
    auto normalizedRoot = InRoot.lexically_normal();
    if (!normalizedRoot.is_absolute()) {
        normalizedRoot = std::filesystem::absolute(normalizedRoot).lexically_normal();
    }
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
    const auto normalized = InPath.lexically_normal();
    auto name = normalized.filename().generic_string();
    if (name.empty()) {
        name = normalized.parent_path().filename().generic_string();
    }
    if (!name.empty()) {
        return name;
    }
    return normalized.generic_string();
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

auto PrintTable(const std::vector<DirtyEntry>& InEntries) -> void {
    std::map<std::string, std::vector<const DirtyEntry*>> grouped;
    for (const auto& entry : InEntries) {
        grouped[entry.group].push_back(&entry);
    }

    std::cout << "SUMMARY: dirty_repos=" << InEntries.size() << ", groups=" << grouped.size() << "\n\n";

    std::size_t index = 0;
    for (const auto& [group, rows] : grouped) {
        std::cout << "GROUP: " << group << "\n";
        for (const auto* row : rows) {
            index += 1;
            std::cout << "[" << index << "] " << row->repoName
                      << "  (repo_dirty=" << (row->repoDirty ? "yes" : "no")
                      << ", wt_dirty=" << (row->worktreeDirty ? "yes" : "no") << ")\n";
            std::cout << "  path: " << row->relativePath << "\n";

            if (!row->statusLines.empty()) {
                std::cout << "  status:\n";
                for (const auto& line : row->statusLines) {
                    std::cout << "    " << line << "\n";
                }
            }

            if (row->worktreeDirty) {
                std::cout << "  dirty_worktrees: " << row->dirtyWorktrees << "\n";
            }
        }
        std::cout << "\n";
    }

    if (InEntries.empty()) {
        std::cout << "No dirty repositories found.\n";
    }
}

} // namespace

void RegisterDirty(CLI::App& InApp) {
    auto configure = [](CLI::App* InCmd,
                        std::string* InFormat,
                        std::string* InRoot,
                        int* InMaxDepth,
                        bool* InNoCache,
                        bool* InNoRefreshCache) {
        InCmd->add_option("--format", *InFormat, "Output format: table|json")->default_str("table");
        InCmd->add_option("--repo-root", *InRoot, "Workspace root/start path");
        InCmd->add_option("--max-depth", *InMaxDepth, "Discovery max depth");
        InCmd->add_flag("--no-cache", *InNoCache, "Disable discovery cache for this run");
        InCmd->add_flag("--no-refresh-cache", *InNoRefreshCache, "Do not force cache refresh");
    };

    auto* cmd = InApp.add_subcommand("dirty", "List dirty repositories and print their changed content");
    auto* cmdAlias = InApp.add_subcommand("dwt", "Alias of dirty");

    auto* format = new std::string{"table"};
    auto* root = new std::string{"."};
    auto* maxDepth = new int{8};
    auto* noCache = new bool{false};
    auto* noRefreshCache = new bool{false};

    configure(cmd, format, root, maxDepth, noCache, noRefreshCache);
    configure(cmdAlias, format, root, maxDepth, noCache, noRefreshCache);

    auto run = [=]() {
        if (*format != "table" && *format != "json") {
            std::cerr << "Error: invalid --format value: " << *format << " (expected table|json)\n";
            std::exit(1);
        }

        workspace::DiscoverOptions options;
        options.rootDir = std::filesystem::path(*root);
        options.maxDepth = *maxDepth;
        options.useCache = !*noCache;
        options.refreshCache = !*noRefreshCache;
        options.metadataLevel = "minimal";

        const auto discovery = workspace::DiscoverRepos(options);
        std::vector<DirtyEntry> entries;

        for (const auto& repo : discovery.repos) {
            const auto repoPath = repo.path.lexically_normal();

            const auto statusOut = GitCapture(repoPath, {"status", "--short"});
            const auto statusLines = (statusOut.exitCode == 0)
                ? SplitNonEmptyLines(statusOut.stdoutStr)
                : std::vector<std::string>{};

            std::string dirtyWorktrees;
            const bool wtDirty = HasDirtyWorktrees(repoPath, dirtyWorktrees);
            const bool repoDirty = !statusLines.empty();

            if (!repoDirty && !wtDirty) {
                continue;
            }

            DirtyEntry entry;
            entry.path = repoPath;
            const auto relative = RelativeDisplayPath(options.rootDir, repoPath);
            entry.relativePath = relative.generic_string();
            entry.group = GroupFromRelativePath(relative);
            entry.repoName = RepoNameFromPath(repoPath);
            entry.repoDirty = repoDirty;
            entry.worktreeDirty = wtDirty;
            entry.dirtyWorktrees = dirtyWorktrees;
            entry.statusLines = statusLines;
            entries.push_back(std::move(entry));
        }

        std::sort(entries.begin(), entries.end(), [](const DirtyEntry& A, const DirtyEntry& B) {
            if (A.group != B.group) {
                return A.group < B.group;
            }
            if (A.repoName != B.repoName) {
                return A.repoName < B.repoName;
            }
            return A.relativePath < B.relativePath;
        });

        if (*format == "json") {
            std::ostringstream out;
            out << "{\"summary\":{";
            out << "\"dirty_repo_count\":" << entries.size();
            out << "},\"repos\":[";
            for (std::size_t i = 0; i < entries.size(); ++i) {
                if (i > 0) {
                    out << ",";
                }
                const auto& row = entries[i];
                out << "{";
                out << "\"index\":" << (i + 1) << ",";
                out << "\"path\":\"" << row.path.generic_string() << "\",";
                out << "\"relative_path\":\"" << row.relativePath << "\",";
                out << "\"group\":\"" << row.group << "\",";
                out << "\"repo_name\":\"" << row.repoName << "\",";
                out << "\"repo_dirty\":" << (row.repoDirty ? "true" : "false") << ",";
                out << "\"worktree_dirty\":" << (row.worktreeDirty ? "true" : "false") << ",";
                out << "\"dirty_worktrees\":\"" << row.dirtyWorktrees << "\",";
                out << "\"status\":[";
                for (std::size_t j = 0; j < row.statusLines.size(); ++j) {
                    if (j > 0) {
                        out << ",";
                    }
                    out << "\"" << row.statusLines[j] << "\"";
                }
                out << "]}";
            }
            out << "]}";
            std::cout << out.str() << "\n";
        } else {
            PrintTable(entries);
        }
    };

    cmd->callback(run);
    cmdAlias->callback(run);
}

} // namespace kano::git::commands
