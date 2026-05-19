// status/overview commands — dirty-focused status plus cached workspace overview

#include <CLI/CLI.hpp>
#include "discovery.hpp"
#include "shell_executor.hpp"
#include "terminal_color.hpp"

#include <algorithm>
#include <array>
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
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#else
#include <sys/ioctl.h>
#include <unistd.h>
#endif

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
    std::string revision;
    bool repoDirty = false;
    bool hasDirtyWorktree = false;
    std::string dirtyWorktrees;
    std::vector<std::string> statusLines;
};

struct TableLayout {
    int indexWidth = 6;
    int repoWidth = 24;
    int branchWidth = 12;
    int remoteWidth = 16;
    int trackingWidth = 14;
    int revisionWidth = 6;
    int dirtyWidth = 7;
    int worktreeDirtyWidth = 10;
    int typeWidth = 8;
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

auto GitCapture(const std::filesystem::path& InRepo, const std::vector<std::string>& InArgs) -> shell::ExecResult {
    return shell::ExecuteCommand("git", InArgs, shell::ExecMode::Capture, InRepo);
}

auto ParsePositiveIntEnv(const char* InName) -> int {
    if (InName == nullptr) {
        return 0;
    }
    const char* raw = std::getenv(InName);
    if (raw == nullptr || *raw == '\0') {
        return 0;
    }
    try {
        return std::max(0, std::stoi(Trim(raw)));
    } catch (const std::exception&) {
        return 0;
    }
}

auto DetectTerminalWidth() -> int {
    if (const int columns = ParsePositiveIntEnv("COLUMNS"); columns > 0) {
        return columns;
    }
#if defined(_WIN32)
    CONSOLE_SCREEN_BUFFER_INFO info{};
    const HANDLE stdoutHandle = GetStdHandle(STD_OUTPUT_HANDLE);
    if (stdoutHandle != INVALID_HANDLE_VALUE && stdoutHandle != nullptr && GetConsoleScreenBufferInfo(stdoutHandle, &info)) {
        const auto width = static_cast<int>(info.srWindow.Right - info.srWindow.Left + 1);
        if (width > 0) {
            return width;
        }
    }
#else
    winsize size{};
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) == 0 && size.ws_col > 0) {
        return static_cast<int>(size.ws_col);
    }
#endif
    return 120;
}

auto DisplayWidthForContent(const std::string& InText) -> int {
    return static_cast<int>(InText.size()) + 2;
}

auto ComputeTypeWidth(const std::vector<RepoView>& InRows) -> int {
    int width = DisplayWidthForContent("TYPE");
    for (const auto& row : InRows) {
        width = std::max(width, DisplayWidthForContent(row.type));
    }
    return std::clamp(width, 6, 24);
}

auto TruncateWithEllipsis(const std::string& InValue, int InWidth) -> std::string {
    if (InWidth <= 0 || static_cast<int>(InValue.size()) <= InWidth) {
        return InValue;
    }
    if (InWidth <= 3) {
        return InValue.substr(0, static_cast<std::size_t>(InWidth));
    }
    return InValue.substr(0, static_cast<std::size_t>(InWidth - 3)) + "...";
}

auto PadRight(const std::string& InValue, int InWidth) -> std::string {
    if (InWidth <= 0) {
        return InValue;
    }
    if (static_cast<int>(InValue.size()) >= InWidth) {
        return InValue;
    }
    return InValue + std::string(static_cast<std::size_t>(InWidth - static_cast<int>(InValue.size())), ' ');
}

auto ComputeTableLayout(const std::vector<RepoView>& InRows) -> TableLayout {
    TableLayout layout;
    layout.typeWidth = ComputeTypeWidth(InRows);

    const int terminalWidth = DetectTerminalWidth();
    const int fixedWidth = layout.indexWidth + layout.revisionWidth + layout.dirtyWidth + layout.worktreeDirtyWidth + layout.typeWidth;

    struct DynamicColumn {
        int minimum;
        int desired;
        int width;
    };

    std::array<DynamicColumn, 4> columns{{
        DynamicColumn{14, DisplayWidthForContent("REPO"), 14},
        DynamicColumn{8, DisplayWidthForContent("BRANCH"), 8},
        DynamicColumn{8, DisplayWidthForContent("REMOTE"), 8},
        DynamicColumn{10, DisplayWidthForContent("TRACKING"), 10},
    }};

    for (const auto& row : InRows) {
        columns[0].desired = std::max(columns[0].desired, DisplayWidthForContent(row.repoName));
        columns[1].desired = std::max(columns[1].desired, DisplayWidthForContent(row.branch));
        columns[2].desired = std::max(columns[2].desired, DisplayWidthForContent(row.remote));
        columns[3].desired = std::max(columns[3].desired, DisplayWidthForContent(row.tracking));
        layout.revisionWidth = std::max(layout.revisionWidth, DisplayWidthForContent(row.revision));
    }

    columns[0].desired = std::clamp(columns[0].desired, columns[0].minimum, 80);
    columns[1].desired = std::clamp(columns[1].desired, columns[1].minimum, 24);
    columns[2].desired = std::clamp(columns[2].desired, columns[2].minimum, 32);
    columns[3].desired = std::clamp(columns[3].desired, columns[3].minimum, 20);

    const int totalMinimumWidth = fixedWidth + columns[0].minimum + columns[1].minimum + columns[2].minimum + columns[3].minimum;
    int remaining = std::max(0, terminalWidth - totalMinimumWidth);

    while (remaining > 0) {
        bool grew = false;
        for (const std::size_t index : {std::size_t{0}, std::size_t{2}, std::size_t{1}, std::size_t{3}}) {
            if (remaining == 0) {
                break;
            }
            if (columns[index].width >= columns[index].desired) {
                continue;
            }
            columns[index].width += 1;
            remaining -= 1;
            grew = true;
        }
        if (!grew) {
            break;
        }
    }

    layout.repoWidth = columns[0].width;
    layout.branchWidth = columns[1].width;
    layout.remoteWidth = columns[2].width;
    layout.trackingWidth = columns[3].width;
    return layout;
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

auto IsAttentionDirty(const RepoView& InRow) -> bool {
    return InRow.repoDirty || InRow.hasDirtyWorktree;
}

auto FilterDirtyRows(const std::vector<RepoView>& InRows) -> std::vector<RepoView> {
    std::vector<RepoView> filtered;
    filtered.reserve(InRows.size());
    for (const auto& row : InRows) {
        if (IsAttentionDirty(row)) {
            filtered.push_back(row);
        }
    }
    return filtered;
}

auto LoadCachedWorkspaceReposOrThrow(const std::filesystem::path& InRoot) -> std::vector<workspace::RepoRecord> {
    std::string reason;
    const auto manifest = workspace::LoadTrustedWorkspaceManifest(InRoot, &reason);
    if (!manifest.has_value()) {
        std::ostringstream oss;
        oss << "cached workspace overview unavailable";
        if (!reason.empty()) {
            oss << ": " << reason;
        }
        oss << ". Run 'kog discover' first to refresh the workspace manifest.";
        throw std::runtime_error(oss.str());
    }
    return manifest->repos;
}

auto RefreshCachedRepoRecords(const std::vector<workspace::RepoRecord>& InRepos) -> std::vector<workspace::RepoRecord> {
    std::vector<workspace::RepoRecord> refreshed;
    refreshed.reserve(InRepos.size());
    for (const auto& repo : InRepos) {
        workspace::RepoRecord updated = repo;
        updated.currentBranch = CurrentBranch(repo.path);
        updated.remotes = CurrentRemote(repo.path);
        const auto statusOut = GitCapture(repo.path, {"status", "--porcelain"});
        updated.hasChanges = statusOut.exitCode == 0 && !Trim(statusOut.stdoutStr).empty();
        refreshed.push_back(std::move(updated));
    }
    return refreshed;
}

auto NormalizeCachedRootRepoNames(std::vector<workspace::RepoRecord>* IoRepos) -> void {
    if (IoRepos == nullptr) {
        return;
    }
    for (auto& repo : *IoRepos) {
        if (repo.type == "root") {
            repo.path = repo.path.lexically_normal();
        }
    }
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
    options.scope = workspace::DiscoverScope::RegisteredOnly;

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

auto MaybeColorize(const std::string& InText, const char* InColor, bool InEnabled) -> std::string {
    if (!InEnabled) {
        return InText;
    }
    return kano::terminal::Wrap(InText, InColor);
}

auto FormatTable(const std::vector<RepoView>& InRows, bool InColorize = true) -> std::string {
    std::ostringstream oss;
    const auto layout = ComputeTableLayout(InRows);
    std::set<std::string> groups;
    std::size_t dirtyCount = 0;
    for (const auto& row : InRows) {
        groups.insert(row.group);
        if (row.repoDirty) {
            dirtyCount += 1;
        }
    }

    oss << MaybeColorize("SUMMARY:", kano::terminal::Color::BoldCyan, InColorize)
        << " repos=" << InRows.size() << ", dirty=" << dirtyCount << ", groups=" << groups.size() << "\n";

    if (!InRows.empty()) {
        oss << MaybeColorize(PadRight("#", layout.indexWidth), kano::terminal::Color::BoldWhite, InColorize)
            << MaybeColorize(PadRight("REPO", layout.repoWidth), kano::terminal::Color::BoldWhite, InColorize)
            << MaybeColorize(PadRight("BRANCH", layout.branchWidth), kano::terminal::Color::BoldWhite, InColorize)
            << MaybeColorize(PadRight("REMOTE", layout.remoteWidth), kano::terminal::Color::BoldWhite, InColorize)
            << MaybeColorize(PadRight("TRACKING", layout.trackingWidth), kano::terminal::Color::BoldWhite, InColorize)
            << MaybeColorize(PadRight("REV", layout.revisionWidth), kano::terminal::Color::BoldWhite, InColorize)
            << MaybeColorize(PadRight("DIRTY", layout.dirtyWidth), kano::terminal::Color::BoldWhite, InColorize)
            << MaybeColorize(PadRight("WT_DIRTY", layout.worktreeDirtyWidth), kano::terminal::Color::BoldWhite, InColorize)
            << MaybeColorize("TYPE", kano::terminal::Color::BoldWhite, InColorize) << "\n";
    }

    auto formatDirty = [](bool InDirty, int InWidth) {
        std::string padded = PadRight(InDirty ? "yes" : "no", InWidth);
        return kano::terminal::Wrap(padded, InDirty ? kano::terminal::Color::BoldRed : kano::terminal::Color::Green);
    };

    std::string currentGroup;
    for (std::size_t i = 0; i < InRows.size(); ++i) {
        const auto& row = InRows[i];
        if (currentGroup != row.group) {
            currentGroup = row.group;
            oss << "\n" << MaybeColorize("GROUP:", kano::terminal::Color::BoldWhite, InColorize) << " " << currentGroup << "\n";
        }

        const auto repoName = TruncateWithEllipsis(row.repoName, std::max(1, layout.repoWidth - 1));
        const auto branch = TruncateWithEllipsis(row.branch, std::max(1, layout.branchWidth - 1));
        const auto remote = TruncateWithEllipsis(row.remote, std::max(1, layout.remoteWidth - 1));
        const auto tracking = TruncateWithEllipsis(row.tracking, std::max(1, layout.trackingWidth - 1));
        const auto type = TruncateWithEllipsis(row.type, std::max(1, layout.typeWidth - 1));
        const auto dirtyCell = MaybeColorize(PadRight(row.repoDirty ? "yes" : "no", layout.dirtyWidth),
                                             row.repoDirty ? kano::terminal::Color::BoldRed : kano::terminal::Color::Green,
                                             InColorize);
        const auto worktreeDirtyCell = MaybeColorize(PadRight(row.hasDirtyWorktree ? "yes" : "no", layout.worktreeDirtyWidth),
                                                     row.hasDirtyWorktree ? kano::terminal::Color::BoldRed : kano::terminal::Color::Green,
                                                     InColorize);
        const auto typeCell = MaybeColorize(type,
                                            row.type == "registered-uninit" ? kano::terminal::Color::BoldYellow : kano::terminal::Color::Dim,
                                            InColorize);

        const auto revision = TruncateWithEllipsis(row.revision, std::max(1, layout.revisionWidth - 1));
        oss << kano::terminal::Wrap(PadRight(std::to_string(i + 1), layout.indexWidth), kano::terminal::Color::Dim)
            << kano::terminal::Wrap(PadRight(repoName, layout.repoWidth), kano::terminal::Color::BoldCyan)
            << kano::terminal::Wrap(PadRight(branch, layout.branchWidth), kano::terminal::Color::Green)
            << PadRight(remote, layout.remoteWidth)
            << PadRight(tracking, layout.trackingWidth)
            << kano::terminal::Wrap(PadRight(revision, layout.revisionWidth), kano::terminal::Color::Dim)
            << dirtyCell
            << worktreeDirtyCell
            << typeCell << "\n";
            
        if (!row.statusLines.empty()) {
            for (const auto& line : row.statusLines) {
                oss << "    " << kano::terminal::Wrap(line, kano::terminal::Color::Yellow) << "\n";
            }
        }
        if (row.hasDirtyWorktree) {
            oss << "    " << kano::terminal::Wrap("dirty worktrees: " + row.dirtyWorktrees, kano::terminal::Color::BoldRed) << "\n";
        }
        if (!row.statusLines.empty() || row.hasDirtyWorktree) {
            oss << "\n";
        }
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
        out << std::format("\"revision\":\"{}\",", row.revision);
        out << std::format("\"dirty\":{},", row.repoDirty ? "true" : "false");
        out << std::format("\"worktree_dirty\":{}", row.hasDirtyWorktree ? "true" : "false");
        if (row.hasDirtyWorktree) {
            out << std::format(",\"dirty_worktrees\":\"{}\"", row.dirtyWorktrees);
        }
        out << ",\"status_lines\":[";
        for (std::size_t j = 0; j < row.statusLines.size(); ++j) {
            if (j > 0) {
                out << ",";
            }
            std::string escaped = row.statusLines[j];
            std::string escapedOut;
            for (char c : escaped) {
                if (c == '"') escapedOut += "\\\"";
                else if (c == '\\') escapedOut += "\\\\";
                else escapedOut += c;
            }
            out << "\"" << escapedOut << "\"";
        }
        out << "]}";
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
    oss << "| Path | Branch | Remote | Tracking | Rev | Dirty | Worktree Dirty | Type |\n";
    oss << "| --- | --- | --- | --- | --- | --- | --- | --- |\n";
    for (const auto& row : InRows) {
        oss << "| "
            << row.path.lexically_normal().generic_string() << " | "
            << row.branch << " | "
            << row.remote << " | "
            << row.tracking << " | "
            << row.revision << " | "
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

    // Fast check: perform exactly 1 uncolored git status to see if it's strictly dirty.
    // We skip the 4 expensive branch tracking operations if it's clean and just use the cache.
    const auto statusQuick = GitCapture(InRepo.path, {"status", "--porcelain"});
    // Filter out external paths (starting with "../") which represent submodule changes
    // outside this repo's root - these should not mark the repo as dirty
    if (statusQuick.exitCode == 0 && !Trim(statusQuick.stdoutStr).empty()) {
        const auto allLines = SplitNonEmptyLines(statusQuick.stdoutStr);
        bool hasInternalDirty = false;
        for (const auto& line : allLines) {
            // Skip lines with "../" paths - these are external submodules or paths
            if (line.find(" ../") != std::string::npos || line.rfind("../", 0) == 0) {
                continue;
            }
            hasInternalDirty = true;
            break;
        }
        row.repoDirty = hasInternalDirty;
    } else {
        row.repoDirty = false;
    }

    {
        const auto revOut = GitCapture(InRepo.path, {"rev-list", "--count", "--first-parent", "HEAD"});
        row.revision = revOut.exitCode == 0 ? Trim(revOut.stdoutStr) : "-";
    }

    if (!row.repoDirty) {
        row.branch = InRepo.currentBranch.empty() ? "(detached)" : InRepo.currentBranch;
        row.remote = InRepo.remotes.empty() ? "-" : InRepo.remotes;
        row.tracking = "-";
        row.hasDirtyWorktree = false;
        return row;
    }

    row.branch = CurrentBranch(InRepo.path);
    row.remote = CurrentRemote(InRepo.path);
    row.tracking = TrackingSummary(InRepo.path);
    row.hasDirtyWorktree = HasDirtyWorktrees(InRepo.path, row.dirtyWorktrees);
    {
        const auto revOut = GitCapture(InRepo.path, {"rev-list", "--count", "--first-parent", "HEAD"});
        row.revision = revOut.exitCode == 0 ? Trim(revOut.stdoutStr) : "-";
    }
    
    const auto statusOut = GitCapture(InRepo.path, {"status", "--porcelain"});
    if (statusOut.exitCode == 0) {
        // Filter to only include internal paths (not external "../" submodule paths)
        const auto allLines = SplitNonEmptyLines(statusOut.stdoutStr);
        for (const auto& line : allLines) {
            // Skip lines with "../" paths - these are external submodules or paths outside this repo
            if (line.find(" ../") != std::string::npos || line.rfind("../", 0) == 0) {
                continue;
            }
            row.statusLines.push_back(line);
        }
    }
    
    return row;
}

auto MakeCachedRepoView(const workspace::RepoRecord& InRepo, const std::filesystem::path& InRoot) -> RepoView {
    RepoView row;
    row.path = InRepo.path;
    const auto relativePath = RelativeDisplayPath(InRoot, InRepo.path);
    row.group = GroupFromRelativePath(relativePath);
    row.repoName = RepoNameFromPath(InRepo.path);
    row.type = InRepo.type;
    row.repoDirty = InRepo.hasChanges;
    row.branch = InRepo.currentBranch.empty() ? "(detached)" : InRepo.currentBranch;
    row.remote = "-";
    row.tracking = "-";
    row.hasDirtyWorktree = false;
    {
        const auto revOut = GitCapture(InRepo.path, {"rev-list", "--count", "--first-parent", "HEAD"});
        row.revision = revOut.exitCode == 0 ? Trim(revOut.stdoutStr) : "-";
    }
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
            futures.push_back(std::async([&repo, &InRoot]() {
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

auto BuildCachedRepoViews(const std::vector<workspace::RepoRecord>& InRepos, const std::filesystem::path& InRoot) -> std::vector<RepoView> {
    std::vector<RepoView> rows;
    rows.reserve(InRepos.size());
    for (const auto& repo : InRepos) {
        rows.push_back(MakeCachedRepoView(repo, InRoot));
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
    auto* cmd = InApp.add_subcommand("status", "Show dirty repositories across the workspace (git-status style)");
    auto* overview = InApp.add_subcommand("overview", "Show cached workspace repo overview without re-running discover");

    auto* format = new std::string{"table"};
    auto* maxDepth = new int{8};
    auto* exclude = new std::vector<std::string>{};
    auto* noCache = new bool{false};
    auto* refreshCache = new bool{false};
    auto* all = new bool{false};
    auto* repoRoot = new std::string{"."};
    auto* output = new std::string{};
    auto* target = new std::string{};

    auto configureOutput = [&](CLI::App* InCmd) {
        InCmd->add_option("--format", *format, "Output format: table|json|markdown")->default_str("table");
        InCmd->add_option("--repo-root", *repoRoot, "Repository root/start path");
        InCmd->add_option("--output", *output, "Write output to file");
        InCmd->add_option("target", *target, "Optional repo target (repo name or relative path)")->required(false);
    };

    configureOutput(cmd);
    cmd->add_option("--max-depth", *maxDepth, "Discovery max depth");
    cmd->add_option("--exclude", *exclude, "Temporary scan-scope exclude override for this invocation only (repeatable; prefer .gitignore/.kogignore for shared policy)");
    cmd->add_flag("--no-cache", *noCache, "Disable discovery cache for this run");
    cmd->add_flag("--refresh-cache", *refreshCache, "Force cache refresh");
    cmd->add_flag("--all", *all, "Show all discovered repos instead of only dirty ones");

    configureOutput(overview);

    auto renderRows = [=](const std::vector<RepoView>& InRows) {
        if (*format != "table" && *format != "json" && *format != "markdown") {
            std::cerr << "Error: invalid --format value: " << *format << " (expected table|json|markdown)\n";
            std::exit(1);
        }

        std::string rendered;
        if (*format == "json") {
            rendered = FormatJson(InRows);
        } else if (*format == "markdown") {
            rendered = FormatMarkdown(InRows);
        } else {
            rendered = FormatTable(InRows, output->empty());
        }

        if (!output->empty()) {
            std::ofstream out(*output, std::ios::out | std::ios::binary | std::ios::trunc);
            out << rendered;
        } else {
            std::cout << rendered << '\n';
        }
    };

    cmd->callback([=]() {
        auto t_start = std::chrono::steady_clock::now();
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
        
        auto t_resolve = std::chrono::steady_clock::now();

        workspace::DiscoverOptions options;
        options.rootDir = root;
        options.maxDepth = *maxDepth;
        options.excludePatterns = *exclude;
        options.useCache = !*noCache;
        options.refreshCache = *refreshCache;
        options.cacheTtlSeconds = (std::numeric_limits<int>::max)();
        options.maxStaleSeconds = (std::numeric_limits<int>::max)();
        options.metadataLevel = "minimal";
        options.scope = workspace::DiscoverScope::RegisteredOnly;

        const auto discovery = workspace::DiscoverRepos(options);
        auto t_discover = std::chrono::steady_clock::now();

        auto rows = BuildRepoViews(discovery.repos, options.rootDir);
        auto t_build = std::chrono::steady_clock::now();

        if (!*all) {
            rows = FilterDirtyRows(rows);
        }
        renderRows(rows);
        auto t_render = std::chrono::steady_clock::now();

        auto ms = [](auto t1, auto t2) { return std::chrono::duration_cast<std::chrono::milliseconds>(t2 - t1).count(); };
        std::cerr << "[DEBUG] Resolve: " << ms(t_start, t_resolve) << "ms\n";
        std::cerr << "[DEBUG] Discover: " << ms(t_resolve, t_discover) << "ms\n";
        std::cerr << "[DEBUG] Build: " << ms(t_discover, t_build) << "ms\n";
        std::cerr << "[DEBUG] Render: " << ms(t_build, t_render) << "ms\n";
    });

    overview->callback([=]() {
        if (*format != "table" && *format != "json" && *format != "markdown") {
            std::cerr << "Error: invalid --format value: " << *format << " (expected table|json|markdown)\n";
            std::exit(1);
        }

        auto root = repoRoot->empty() ? std::filesystem::current_path() : std::filesystem::path(*repoRoot);
        root = std::filesystem::absolute(root).lexically_normal();
        if (!target->empty()) {
            try {
                root = ResolveRepoFromSpec(root, *target, *maxDepth, true);
            } catch (const std::exception& ex) {
                std::cerr << "Error: " << ex.what() << "\n";
                std::exit(1);
            }
        }

        try {
            auto repos = LoadCachedWorkspaceReposOrThrow(root);
            NormalizeCachedRootRepoNames(&repos);
            const auto rows = BuildCachedRepoViews(repos, root);
            renderRows(rows);
        } catch (const std::exception& ex) {
            std::cerr << "Error: " << ex.what() << "\n";
            std::exit(1);
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
            rendered = FormatTable(rows, output->empty());
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
