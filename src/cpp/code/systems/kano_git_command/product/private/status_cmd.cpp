// status command — global cross-repo status view

#include <CLI/CLI.hpp>
#include "discovery.hpp"
#include "shell_executor.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <format>
#include <future>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
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

auto ResolveRepoFromSpec(const std::filesystem::path& InRoot,
                         const std::string& InSpec,
                         int InMaxDepth,
                         bool InUseCache) -> std::filesystem::path {
    if (InSpec.empty()) {
        return std::filesystem::current_path().lexically_normal();
    }

    const std::filesystem::path asPath(InSpec);
    const auto candidate = (asPath.is_absolute() ? asPath : (InRoot / asPath)).lexically_normal();
    if (std::filesystem::exists(candidate) && GitCapture(candidate, {"rev-parse", "--git-dir"}).exitCode == 0) {
        return candidate;
    }

    workspace::DiscoverOptions options;
    options.rootDir = InRoot;
    options.maxDepth = InMaxDepth;
    options.useCache = InUseCache;
    options.metadataLevel = "minimal";

    const auto discovery = workspace::DiscoverRepos(options);
    std::vector<std::filesystem::path> exactMatches;
    std::vector<std::filesystem::path> fuzzyMatches;

    for (const auto& repo : discovery.repos) {
        const auto repoPath = repo.path.lexically_normal();
        const auto repoName = RepoNameFromPath(repoPath);
        const auto repoKey = repoPath.generic_string();
        const auto relativeKey = RelativeDisplayPath(InRoot, repoPath).generic_string();

        if (repoName == InSpec || repoKey == InSpec || relativeKey == InSpec) {
            exactMatches.push_back(repoPath);
            continue;
        }
        if (repoKey.find(InSpec) != std::string::npos || relativeKey.find(InSpec) != std::string::npos) {
            fuzzyMatches.push_back(repoPath);
        }
    }

    auto matches = exactMatches.empty() ? fuzzyMatches : exactMatches;
    std::sort(matches.begin(), matches.end(), [](const auto& A, const auto& B) {
        return A.generic_string() < B.generic_string();
    });
    matches.erase(std::unique(matches.begin(), matches.end(), [](const auto& A, const auto& B) {
        return A.generic_string() == B.generic_string();
    }), matches.end());

    if (matches.empty()) {
        throw std::runtime_error("repo not found: " + InSpec);
    }
    if (matches.size() > 1) {
        std::ostringstream oss;
        oss << "repo spec is ambiguous: " << InSpec << "\nMatches:\n";
        for (const auto& match : matches) {
            oss << "  - " << match.generic_string() << "\n";
        }
        throw std::runtime_error(oss.str());
    }

    return matches.front();
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

auto FormatMarkdown(const std::vector<RepoView>& InRows) -> std::string {
    std::ostringstream oss;
    std::size_t dirtyCount = 0;
    for (const auto& row : InRows) {
        if (row.repoDirty) {
            dirtyCount += 1;
        }
    }

    oss << "# Status\n\n";
    oss << "- Repos: " << InRows.size() << "\n";
    oss << "- Dirty: " << dirtyCount << "\n\n";
    oss << "| Path | Branch | Remote | Tracking | Dirty | Worktree Dirty | Type |\n";
    oss << "| --- | --- | --- | --- | --- | --- | --- |\n";
    for (const auto& row : InRows) {
        oss << "| "
            << row.path.lexically_normal().generic_string() << " | "
            << row.branch << " | "
            << row.remote << " | "
            << row.tracking << " | "
            << (row.repoDirty ? "yes" : "no") << " | "
            << (row.hasDirtyWorktree ? "yes" : "no") << " | "
            << row.type << " |\n";
    }
    return oss.str();
}

auto MakeRepoView(const workspace::RepoRecord& InRepo, const std::filesystem::path& InRoot) -> RepoView {
    RepoView row;
    row.path = InRepo.path;
    const auto relativePath = RelativeDisplayPath(InRoot, InRepo.path);
    row.group = GroupFromRelativePath(relativePath);
    row.repoName = RepoNameFromPath(InRepo.path);
    row.type = InRepo.type;
    row.repoDirty = InRepo.hasChanges;
    row.branch = CurrentBranch(InRepo.path);
    row.remote = CurrentRemote(InRepo.path);
    row.tracking = TrackingSummary(InRepo.path);
    row.hasDirtyWorktree = HasDirtyWorktrees(InRepo.path, row.dirtyWorktrees);
    return row;
}

auto BuildRepoViews(const std::vector<workspace::RepoRecord>& InRepos, const std::filesystem::path& InRoot) -> std::vector<RepoView> {
    std::vector<RepoView> rows;
    rows.reserve(InRepos.size());

    if (InRepos.size() <= 1) {
        for (const auto& repo : InRepos) {
            rows.push_back(MakeRepoView(repo, InRoot));
        }
    } else {
        std::vector<std::future<RepoView>> futures;
        futures.reserve(InRepos.size());
        for (const auto& repo : InRepos) {
            futures.push_back(std::async(std::launch::async, [&repo, &InRoot]() {
                return MakeRepoView(repo, InRoot);
            }));
        }
        for (auto& future : futures) {
            rows.push_back(future.get());
        }
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
    return rows;
}

auto SelfBinaryPath() -> std::string {
    if (const char* path = std::getenv("KANO_GIT_BINARY_PATH"); path != nullptr && *path != '\0') {
        return std::string(path);
    }
    return "kano-git";
}

auto RunSelfScopedCommand(const std::string& InCommand,
                          const std::filesystem::path& InResolvedRepo,
                          const std::vector<std::string>& InExtraArgs) -> int {
    std::vector<std::string> args;
    if (InCommand == "push" || InCommand == "commit" || InCommand == "commit-push") {
        args = {InCommand, "--repos", InResolvedRepo.generic_string(), "--no-recursive"};
    } else if (InCommand == "log" || InCommand == "slog") {
        args = {InCommand, "--repo", InResolvedRepo.generic_string(), "--no-recursive"};
    } else if (InCommand == "update") {
        args = {InCommand, "--repo", InResolvedRepo.generic_string()};
    } else {
        std::cerr << "Error: unsupported repo-scoped command: " << InCommand << "\n";
        return 2;
    }
    args.insert(args.end(), InExtraArgs.begin(), InExtraArgs.end());
    const auto result = shell::ExecuteCommand(SelfBinaryPath(), args, shell::ExecMode::PassThrough, std::filesystem::current_path());
    return result.exitCode;
}

} // namespace

void RegisterStatus(CLI::App& InApp) {
    auto* cmd = InApp.add_subcommand("status", "Global cross-repo status view (branch/upstream/dirty/worktree)");

    auto* format = new std::string{"table"};
    auto* maxDepth = new int{8};
    auto* exclude = new std::vector<std::string>{};
    auto* noCache = new bool{false};
    auto* noRefreshCache = new bool{false};
    auto* repoRoot = new std::string{"."};
    auto* output = new std::string{};
    auto* target = new std::string{};

    cmd->add_option("--format", *format, "Output format: table|json|markdown")->default_str("table");
    cmd->add_option("--max-depth", *maxDepth, "Discovery max depth");
    cmd->add_option("--exclude", *exclude, "Temporary scan-scope exclude override for this invocation only (repeatable; prefer .gitignore/.kogignore for shared policy)");
    cmd->add_flag("--no-cache", *noCache, "Disable discovery cache for this run");
    cmd->add_flag("--no-refresh-cache", *noRefreshCache, "Do not force cache refresh");
    cmd->add_option("--repo-root", *repoRoot, "Repository root/start path");
    cmd->add_option("--output", *output, "Write output to file");
    cmd->add_option("target", *target, "Optional repo target (repo name or relative path)")->required(false);

    cmd->callback([format, maxDepth, exclude, noCache, noRefreshCache, repoRoot, output, target]() {
        if (*format != "table" && *format != "json" && *format != "markdown") {
            std::cerr << "Error: invalid --format value: " << *format << " (expected table|json|markdown)\n";
            std::exit(1);
        }

        auto root = repoRoot->empty() ? std::filesystem::current_path() : std::filesystem::path(*repoRoot);
        root = std::filesystem::absolute(root).lexically_normal();
        if (!target->empty()) {
            try {
                root = ResolveRepoFromSpec(root, *target, *maxDepth, !*noCache);
            } catch (const std::exception& ex) {
                std::cerr << "Error: " << ex.what() << "\n";
                std::exit(1);
            }
        }

        workspace::DiscoverOptions options;
        options.rootDir = root;
        options.maxDepth = *maxDepth;
        options.excludePatterns = *exclude;
        options.useCache = !*noCache;
        options.refreshCache = !*noRefreshCache;
        options.metadataLevel = "full";

        const auto discovery = workspace::DiscoverRepos(options);
        const auto rows = BuildRepoViews(discovery.repos, options.rootDir);

        std::string rendered;
        if (*format == "json") {
            rendered = FormatJson(rows);
        } else if (*format == "markdown") {
            rendered = FormatMarkdown(rows);
        } else {
            rendered = FormatTable(rows);
        }

        if (!output->empty()) {
            std::ofstream out(*output, std::ios::out | std::ios::binary | std::ios::trunc);
            out << rendered;
        } else {
            std::cout << rendered << '\n';
        }
    });
}

void RegisterRepo(CLI::App& InApp) {
    auto* cmd = InApp.add_subcommand("repo", "Single-repo scoped command variants");
    auto* status = cmd->add_subcommand("status", "Status for a single repo without recursive expansion");

    auto* format = new std::string{"table"};
    auto* repoRoot = new std::string{"."};
    auto* output = new std::string{};
    auto* target = new std::string{"."};

    status->add_option("target", *target, "Target repo (repo name or relative path)")->required();
    status->add_option("--format", *format, "Output format: table|json|markdown")->default_str("table");
    status->add_option("--repo-root", *repoRoot, "Workspace root/start path used for repo-name lookup");
    status->add_option("--output", *output, "Write output to file");

    status->callback([format, repoRoot, output, target]() {
        if (*format != "table" && *format != "json" && *format != "markdown") {
            std::cerr << "Error: invalid --format value: " << *format << " (expected table|json|markdown)\n";
            std::exit(1);
        }

        auto root = repoRoot->empty() ? std::filesystem::current_path() : std::filesystem::path(*repoRoot);
        root = std::filesystem::absolute(root).lexically_normal();

        std::filesystem::path repoPath;
        try {
            repoPath = ResolveRepoFromSpec(root, *target, 8, true);
        } catch (const std::exception& ex) {
            std::cerr << "Error: " << ex.what() << "\n";
            std::exit(1);
        }

        workspace::RepoRecord record;
        record.path = repoPath;
        record.type = (repoPath == root) ? "root" : "direct";
        record.hasChanges = GitCapture(repoPath, {"status", "--porcelain"}).exitCode == 0 &&
                            !Trim(GitCapture(repoPath, {"status", "--porcelain"}).stdoutStr).empty();

        const auto rows = BuildRepoViews({record}, repoPath);

        std::string rendered;
        if (*format == "json") {
            rendered = FormatJson(rows);
        } else if (*format == "markdown") {
            rendered = FormatMarkdown(rows);
        } else {
            rendered = FormatTable(rows);
        }

        if (!output->empty()) {
            std::ofstream out(*output, std::ios::out | std::ios::binary | std::ios::trunc);
            out << rendered;
        } else {
            std::cout << rendered << '\n';
        }
        std::exit(0);
    });

    auto registerRepoLogLike = [&](const std::string& InName, bool InShort) {
        auto* sub = cmd->add_subcommand(InName, InShort ? "Short log for a single repo" : "Log for a single repo");
        auto* targetArg = new std::string{"."};
        auto* count = new int{3};
        auto* repoRoot = new std::string{"."};
        sub->add_option("target", *targetArg, "Target repo (repo name or relative path)")->required();
        sub->add_option("--count,-n", *count, "Number of commits to show");
        sub->add_option("--repo-root", *repoRoot, "Workspace root/start path used for repo-name lookup");
        sub->callback([targetArg, count, repoRoot, InShort]() {
            auto root = repoRoot->empty() ? std::filesystem::current_path() : std::filesystem::path(*repoRoot);
            root = std::filesystem::absolute(root).lexically_normal();
            try {
                const auto repoPath = ResolveRepoFromSpec(root, *targetArg, 8, true);
                const auto code = RunSelfScopedCommand(
                    InShort ? "slog" : "log",
                    repoPath,
                    {"--count", std::to_string(*count)});
                std::exit(code);
            } catch (const std::exception& ex) {
                std::cerr << "Error: " << ex.what() << "\n";
                std::exit(1);
            }
        });
    };

    registerRepoLogLike("log", false);
    registerRepoLogLike("slog", true);

    auto registerRepoPassThrough = [&](const std::string& InName) {
        auto* sub = cmd->add_subcommand(InName, std::format("Run {} against a single repo", InName));
        sub->allow_extras();
        sub->prefix_command();
        auto* targetArg = new std::string{"."};
        sub->add_option("target", *targetArg, "Target repo (repo name or relative path)")->required();
        sub->callback([sub, targetArg, InName]() {
            const auto extrasRaw = sub->remaining();
            std::vector<std::string> extras(extrasRaw.begin(), extrasRaw.end());
            try {
                const auto repoPath = ResolveRepoFromSpec(std::filesystem::current_path(), *targetArg, 8, true);
                std::exit(RunSelfScopedCommand(InName, repoPath, extras));
            } catch (const std::exception& ex) {
                std::cerr << "Error: " << ex.what() << "\n";
                std::exit(1);
            }
        });
    };

    registerRepoPassThrough("push");
    registerRepoPassThrough("commit");
    registerRepoPassThrough("commit-push");
    registerRepoPassThrough("update");
}

} // namespace kano::git::commands
