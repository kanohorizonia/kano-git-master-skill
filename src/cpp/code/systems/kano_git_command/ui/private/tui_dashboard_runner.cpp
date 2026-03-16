// tui command — FTXUI dashboard with incremental history pager

#include "tui_dashboard_runner.hpp"
#include "discovery.hpp"
#include "shell_executor.hpp"
#include "tui_state.hpp"
#include "lru_cache.hpp"
#include "metadata_cache.hpp"
#include "autocomplete_engine.hpp"
#include "command_executor.hpp"

#include <ftxui/component/component.hpp>
#include <ftxui/component/component_options.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/terminal.hpp>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <optional>
#include <mutex>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#ifdef KOG_PLATFORM_WINDOWS
#include <windows.h>
#ifdef RGB
#undef RGB
#endif
#else
#include <termios.h>
#include <unistd.h>
#endif

namespace kano::git::commands {
namespace {

struct TerminalModeGuard {
    TerminalModeGuard() {
#ifdef KOG_PLATFORM_WINDOWS
        inputHandle_ = GetStdHandle(STD_INPUT_HANDLE);
        outputHandle_ = GetStdHandle(STD_OUTPUT_HANDLE);
        if (inputHandle_ != nullptr && inputHandle_ != INVALID_HANDLE_VALUE) {
            inputValid_ = GetConsoleMode(inputHandle_, &inputMode_) != 0;
        }
        if (outputHandle_ != nullptr && outputHandle_ != INVALID_HANDLE_VALUE) {
            outputValid_ = GetConsoleMode(outputHandle_, &outputMode_) != 0;
        }
#else
        if (isatty(STDIN_FILENO) == 1) {
            inputValid_ = tcgetattr(STDIN_FILENO, &inputMode_) == 0;
        }
        if (isatty(STDOUT_FILENO) == 1) {
            outputValid_ = tcgetattr(STDOUT_FILENO, &outputMode_) == 0;
        }
#endif
    }

    ~TerminalModeGuard() {
#ifdef KOG_PLATFORM_WINDOWS
        if (inputValid_ && inputHandle_ != nullptr && inputHandle_ != INVALID_HANDLE_VALUE) {
            SetConsoleMode(inputHandle_, inputMode_);
        }
        if (outputValid_ && outputHandle_ != nullptr && outputHandle_ != INVALID_HANDLE_VALUE) {
            SetConsoleMode(outputHandle_, outputMode_);
        }
#else
        if (inputValid_) {
            tcsetattr(STDIN_FILENO, TCSANOW, &inputMode_);
        }
        if (outputValid_) {
            tcsetattr(STDOUT_FILENO, TCSANOW, &outputMode_);
        }
#endif
    }

    TerminalModeGuard(const TerminalModeGuard&) = delete;
    auto operator=(const TerminalModeGuard&) -> TerminalModeGuard& = delete;

private:
#ifdef KOG_PLATFORM_WINDOWS
    HANDLE inputHandle_ = nullptr;
    HANDLE outputHandle_ = nullptr;
    DWORD inputMode_ = 0;
    DWORD outputMode_ = 0;
#else
    termios inputMode_{};
    termios outputMode_{};
#endif
    bool inputValid_ = false;
    bool outputValid_ = false;
};

struct RepoView {
    std::filesystem::path path;
    std::string type;
    std::string branch;
    std::string upstream;
    std::string tracking;
    bool statusFromSnapshot = false;
    bool repoDirty = false;
    bool worktreeDirty = false;
    std::string dirtyWorktrees;
    struct DirtyFileEntry {
        std::string displayPath;
        std::string patchPath;
        int changedLines = -1;
    };
    std::vector<DirtyFileEntry> dirtyFiles;
    std::string parentRepo;
    int childRepoCount = 0;
    int treeDepth = 0;
};

struct RepoHistoryCache {
    struct HistoryEntry {
        bool isDirtyWorkingTree = false;
        std::string sha;
        std::string subject;
        std::string authorName;
        std::string authorEmail;
        int globalIndex = 0;
        int totalCount = 0;
        std::string displayLine;
    };
    struct DetailSection {
        std::string title;
        std::string body;
        std::string patchPath;
        std::string patchPathAlt;  // old path for rename/copy entries
    };
    struct DetailOverlayData {
        std::string title;
        std::vector<DetailSection> sections;
    };
    std::vector<HistoryEntry> allEntries;
    bool fullyLoaded = false;
    std::optional<int> totalCommits;
    LruCache<std::string, std::string> commitDetails{48};
    LruCache<std::string, std::string> commitQuickStats{96};
    LruCache<std::string, DetailOverlayData> detailOverlays{48};
    std::string dirtyToken;  // hash of `git status --porcelain` for dirty overlay cache invalidation
};

struct PreviewData {
    std::vector<std::string> staged;
    std::vector<std::string> unstaged;
    std::string branch;
    std::string upstream;
    std::string tracking;
};

struct HistoryState {
    bool active = false;
    int repoIndex = 0;
    int pageIndex = 0;
    int selectedLine = 0;
    bool searchMode = false;
    std::string searchQuery;
    int highlightedLine = -1;
    bool detailActive = false;
    std::string detailSha;
    std::string detailBody;
    int detailSelectedSection = 0;
    int detailPageIndex = 0;
    int detailMode = 0; // 0=summary, 1=files, 2=patch
    int sortMode = 0;   // 0=time-desc, 1=time-asc, 2=match-first
};

struct PreviewPanelState {
    bool active = false;
    std::string title;
    std::string body;
    bool running = false;
    bool isError = false;
    bool autoCloseAfterRefresh = false;
};

struct ConfirmState {
    bool active = false;
    std::string title;
    std::string description;
    std::filesystem::path repo;
    std::vector<std::string> command;
};

enum class CommandScopeMode {
    Workspace,
    SelectedRepo,
};

struct CherryPickCandidate {
    std::string sha;
    std::string title;
    bool alreadyInTarget = false;
    std::string risk;
};

struct CherryPickPreflightState {
    bool active = false;
    std::string sourceBranch;
    std::string targetBranch;
    std::vector<CherryPickCandidate> commits;
    std::string note;
};

struct CherryPickRunnerState {
    bool active = false;
    std::filesystem::path repo;
    std::vector<CherryPickCandidate> queue;
    int index = 0;
    bool waitingConflictResolution = false;
    std::string lastOutput;
};

struct RebasePreflightState {
    bool active = false;
    std::filesystem::path repo;
    std::string branch;
    std::string upstream;
    std::string tracking;
    std::string mergeBase;
    std::vector<std::string> candidates;
    std::string risk;
    std::string note;
};

struct RebasePlanItem {
    std::string sha;
    std::string title;
    std::string action = "pick"; // pick|squash|fixup|drop
};

struct RebasePlannerState {
    bool active = false;
    std::filesystem::path repo;
    std::string baseRef;
    std::vector<RebasePlanItem> items;
    int selectedIndex = 0;
    std::string preview;
};

struct RebaseRunnerState {
    bool active = false;
    std::filesystem::path repo;
    std::vector<RebasePlanItem> queue;
    int index = 0;
    bool waitingConflictResolution = false;
    std::string lastOutput;
};

struct DiscoverPagerState {
    bool active = false;
    std::vector<std::string> lines;
    int pageIndex = 0;
    int pageSize = 18;
    bool dirtyOnly = false;
    std::string title;
    bool loading = false;
    std::string progress;
};

struct AsyncWorkState {
    bool busy = false;
    bool hasResult = false;
    bool hasError = false;
    bool refreshRepos = false;
    bool refreshSelectedRepo = false;
    bool refreshDiscover = false;
    std::vector<RepoView> repos;
    RepoView refreshedRepo;
    std::string refreshedRepoKey;
    std::vector<std::string> discoverLines;
    std::string discoverTitle;
    std::string label;
    std::string progress;
    std::string completionFooter;
    std::string errorMessage;
    bool showPreview = false;
    std::string previewTitle;
    std::string previewBody;
    bool previewAutoCloseAfterRefresh = false;
};

auto HasDirtyHistoryEntry(const RepoView& InRepo) -> bool;
auto BuildHistoryDisplayLine(const RepoHistoryCache::HistoryEntry& InEntry,
                             const std::string& InAuthorText = std::string()) -> std::string;
auto FetchTotalCommitCount(const std::filesystem::path& InRepo) -> std::optional<int>;
auto FetchAllHistory(const RepoView& InRepo) -> std::vector<RepoHistoryCache::HistoryEntry>;
auto FetchWorkingTreeDetail(const std::filesystem::path& repo, int mode) -> std::string;
auto FetchCommitDetail(const std::filesystem::path& repo, const std::string& sha, int mode) -> std::string;
auto BuildHistoryDetailOverlay(const std::filesystem::path& repo,
                               const RepoView& repoView,
                               const RepoHistoryCache::HistoryEntry& entry,
                               int mode) -> RepoHistoryCache::DetailOverlayData;

auto ResolveKanoGitBinaryCommand() -> std::string {
    if (const char* binaryPath = std::getenv("KANO_GIT_BINARY_PATH"); binaryPath != nullptr) {
        const std::filesystem::path p(binaryPath);
        if (std::filesystem::exists(p)) {
            return p.generic_string();
        }
    }
#if defined(_WIN32)
    return "kano-git.exe";
#else
    return "kano-git";
#endif
}

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

auto DisplayRepoPath(const std::filesystem::path& InRoot, const std::filesystem::path& InPath) -> std::string {
    auto normalizedRoot = InRoot.lexically_normal();
    if (!normalizedRoot.is_absolute()) {
        normalizedRoot = std::filesystem::absolute(normalizedRoot).lexically_normal();
    }
    const auto normalizedPath = InPath.lexically_normal();
    if (normalizedPath == normalizedRoot) {
        return normalizedPath.generic_string();
    }
    return RelativeDisplayPath(normalizedRoot, normalizedPath).generic_string();
}

auto DisplayParentRepo(const std::filesystem::path& InRoot, const std::string& InParentRepo) -> std::string {
    if (InParentRepo == "(none)") {
        return InParentRepo;
    }
    return DisplayRepoPath(InRoot, std::filesystem::path(InParentRepo));
}

// Canonical path string for comparison: lexically_normal + generic_string + strip trailing separator.
// On Windows, (root / ".").lexically_normal() can produce "D:/_work/foo/" (trailing separator for the
// root-looking path), but sub-paths normalize to "D:/_work/foo/bar" (no trailing separator). This breaks
// parent-child path comparisons. Strip trailing '/' to ensure consistent comparison.
auto CanonicalPathString(const std::filesystem::path& InPath) -> std::string {
    auto s = InPath.lexically_normal().generic_string();
    while (s.size() > 1 && s.back() == '/') {
        s.pop_back();
    }
    return s;
}

auto NormalizeRepoPath(const std::filesystem::path& InRoot, const std::filesystem::path& InPath) -> std::filesystem::path {
    auto normalizedRoot = InRoot.lexically_normal();
    if (!normalizedRoot.is_absolute()) {
        normalizedRoot = std::filesystem::absolute(normalizedRoot).lexically_normal();
    }
    if (InPath.is_absolute()) {
        return InPath.lexically_normal();
    }
    return (normalizedRoot / InPath).lexically_normal();
}

auto WithSnapshotTag(const std::string& InValue, const bool InFromSnapshot) -> std::string {
    if (!InFromSnapshot) {
        return InValue;
    }
    if (InValue.find("(cached)") != std::string::npos || InValue == "cached") {
        return InValue;
    }
    return InValue + " (snap)";
}

auto AbbreviateFront(const std::string& InValue, const std::size_t InMaxWidth) -> std::string {
    if (InValue.size() <= InMaxWidth || InMaxWidth <= 3) {
        return InValue;
    }
    const std::size_t tail = InMaxWidth - 3;
    return "..." + InValue.substr(InValue.size() - tail);
}

auto TryParseNonNegativeInt(const std::string& InValue) -> std::optional<int> {
    if (InValue.empty()) {
        return std::nullopt;
    }
    if (!std::all_of(InValue.begin(), InValue.end(), [](const unsigned char ch) { return std::isdigit(ch) != 0; })) {
        return std::nullopt;
    }
    return std::stoi(InValue);
}

auto CompactDetailValue(std::string InValue) -> std::string {
    if (InValue == "(none)") {
        InValue = "-";
    } else if (InValue == "(cached)" || InValue == "cached") {
        InValue = "cache";
    } else if (InValue == "no-upstream") {
        InValue = "no-up";
    } else if (InValue == "in-sync") {
        InValue = "sync";
    }
    return InValue;
}

enum class DetailLabelMode {
    Full,
    Short,
    Bare,
};

auto DetailLine(const DetailLabelMode InMode,
                const std::string& InFullLabel,
                const std::string& InShortLabel,
                const std::string& InValue) -> std::string {
    if (InMode == DetailLabelMode::Bare) {
        return InValue;
    }
    return (InMode == DetailLabelMode::Full ? InFullLabel : InShortLabel) + ": " + InValue;
}

auto ChooseDetailLabelMode(const int InAvailableWidth,
                           const std::vector<std::string>& InFullLines,
                           const std::vector<std::string>& InShortLines) -> DetailLabelMode {
    auto longest = [](const std::vector<std::string>& lines) {
        std::size_t width = 0;
        for (const auto& line : lines) {
            width = std::max(width, line.size());
        }
        return static_cast<int>(width);
    };

    const int safeWidth = std::max(0, InAvailableWidth - 2);
    if (longest(InFullLines) <= safeWidth) {
        return DetailLabelMode::Full;
    }
    if (longest(InShortLines) <= safeWidth) {
        return DetailLabelMode::Short;
    }
    return DetailLabelMode::Bare;
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

auto CurrentUpstream(const std::filesystem::path& InRepo) -> std::string {
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
    const auto lb = first.find('[');
    const auto rb = first.find(']');
    if (lb != std::string::npos && rb != std::string::npos && rb > lb) {
        return first.substr(lb + 1, rb - lb - 1);
    }
    if (first.find("...") != std::string::npos) {
        return "in-sync";
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
            joined << ", ";
        }
        joined << dirty[i];
    }
    OutDirtyList = joined.str();
    return true;
}

auto CollectChangedLineCounts(const std::filesystem::path& InRepo,
                              const std::vector<std::string>& InArgs,
                              std::unordered_map<std::string, int>& OutCounts) -> void {
    const auto out = GitCapture(InRepo, InArgs);
    if (out.exitCode != 0) {
        return;
    }
    std::istringstream iss(out.stdoutStr);
    std::string line;
    while (std::getline(iss, line)) {
        if (line.empty()) {
            continue;
        }
        const auto firstTab = line.find('\t');
        if (firstTab == std::string::npos) {
            continue;
        }
        const auto secondTab = line.find('\t', firstTab + 1);
        if (secondTab == std::string::npos) {
            continue;
        }
        const auto addedText = line.substr(0, firstTab);
        const auto deletedText = line.substr(firstTab + 1, secondTab - firstTab - 1);
        std::string path = line.substr(secondTab + 1);
        if (path.empty()) {
            continue;
        }
        const auto renameArrow = path.rfind("\t");
        if (renameArrow != std::string::npos) {
            path = path.substr(renameArrow + 1);
        }
        if (addedText == "-" || deletedText == "-") {
            OutCounts.emplace(path, -1);
            continue;
        }
        const auto added = TryParseNonNegativeInt(addedText);
        const auto deleted = TryParseNonNegativeInt(deletedText);
        if (!added.has_value() || !deleted.has_value()) {
            continue;
        }
        auto& count = OutCounts[path];
        if (count >= 0) {
            count += *added + *deleted;
        }
    }
}

auto DirtyFiles(const std::filesystem::path& InRepo) -> std::vector<RepoView::DirtyFileEntry> {
    const auto out = GitCapture(InRepo, {"status", "--porcelain"});
    if (out.exitCode != 0) {
        return {};
    }
    std::unordered_map<std::string, int> changedLineCounts;
    CollectChangedLineCounts(InRepo, {"diff", "--numstat"}, changedLineCounts);
    CollectChangedLineCounts(InRepo, {"diff", "--numstat", "--cached"}, changedLineCounts);

    std::istringstream iss(out.stdoutStr);
    std::string line;
    std::vector<RepoView::DirtyFileEntry> files;
    std::unordered_map<std::string, std::size_t> seen;
    while (std::getline(iss, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) {
            line.pop_back();
        }
        if (line.size() < 4) {
            continue;
        }
        const std::string rawPath = Trim(line.substr(3));
        if (rawPath.empty()) {
            continue;
        }
        std::string path = rawPath;
        std::string displayPath = rawPath;
        const char statusX = line.size() > 0 ? line[0] : ' ';
        const char statusY = line.size() > 1 ? line[1] : ' ';
        const bool isRenameOrCopy = statusX == 'R' || statusY == 'R' || statusX == 'C' || statusY == 'C';
        if (isRenameOrCopy) {
            const auto arrowPos = rawPath.find(" -> ");
            if (arrowPos != std::string::npos) {
                const auto oldPath = Trim(rawPath.substr(0, arrowPos));
                const auto newPath = Trim(rawPath.substr(arrowPos + 4));
                if (!newPath.empty()) {
                    path = newPath;
                    displayPath = oldPath.empty() ? newPath : (oldPath + " -> " + newPath);
                }
            }
        }
        auto itSeen = seen.find(path);
        const int changedLines = changedLineCounts.contains(path) ? changedLineCounts[path] : -1;
        if (itSeen == seen.end()) {
            seen[path] = files.size();
            files.push_back({displayPath, path, changedLines});
            continue;
        }
        auto& existing = files[itSeen->second];
        if (existing.changedLines < 0) {
            existing.changedLines = changedLines;
        } else if (changedLines > 0) {
            existing.changedLines = changedLines;
        }
    }

    std::sort(files.begin(), files.end(), [](const RepoView::DirtyFileEntry& A, const RepoView::DirtyFileEntry& B) {
        if (A.changedLines >= 0 && B.changedLines >= 0 && A.changedLines != B.changedLines) {
            return A.changedLines > B.changedLines;
        }
        if ((A.changedLines >= 0) != (B.changedLines >= 0)) {
            return A.changedLines >= 0;
        }
        return A.patchPath < B.patchPath;
    });
    return files;
}

// Compute a lightweight token from `git status --porcelain` output.
// When the working tree state changes, this token changes, causing dirty overlay cache misses.
auto ComputeDirtyToken(const std::filesystem::path& InRepo) -> std::string {
    const auto out = GitCapture(InRepo, {"status", "--porcelain"});
    if (out.exitCode != 0) {
        return "err";
    }
    const auto hash = std::hash<std::string>{}(out.stdoutStr);
    // Encode as hex string for compact, readable cache keys
    std::ostringstream oss;
    oss << std::hex << hash;
    return oss.str();
}

// Build cache key prefix from the stored dirty token (no git call — safe for render path).
auto DirtyCachePrefix(const RepoHistoryCache& cache) -> std::string {
    return "(dirty@" + cache.dirtyToken + ")";
}

// Refresh dirty token for a cache entry. If token changed, purge stale dirty overlay/detail entries.
// Returns the current "(dirty@<token>)" prefix string for cache key construction.
auto RefreshDirtyToken(RepoHistoryCache& cache, const std::filesystem::path& repo) -> std::string {
    const auto newToken = ComputeDirtyToken(repo);
    if (!cache.dirtyToken.empty() && cache.dirtyToken != newToken) {
        // Token changed — purge all stale dirty entries from commitDetails and detailOverlays
        cache.commitDetails.erase_if([](const std::string& k, const std::string&) { return k.starts_with("(dirty@"); });
        cache.detailOverlays.erase_if([](const std::string& k, const RepoHistoryCache::DetailOverlayData&) { return k.starts_with("(dirty@"); });
        cache.fullyLoaded = false;
        cache.allEntries.clear();
    }
    cache.dirtyToken = newToken;
    return "(dirty@" + newToken + ")";
}

auto IsAncestorPath(const std::filesystem::path& Ancestor, const std::filesystem::path& Child) -> bool {
    const auto a = CanonicalPathString(Ancestor);
    const auto c = CanonicalPathString(Child);
    if (a == c || a.empty()) {
        return false;
    }
    // Ancestor is a prefix of Child if Child starts with Ancestor followed by '/'
    if (c.size() <= a.size()) {
        return false;
    }
    return c.compare(0, a.size(), a) == 0 && c[a.size()] == '/';
}

auto ResolveRepoParent(const std::filesystem::path& InWorkspaceRoot,
                       const workspace::RepoRecord& InRepo) -> std::string {
    const auto normalizedRepoPath = NormalizeRepoPath(InWorkspaceRoot, InRepo.path);
    const auto canonicalRepoStr = CanonicalPathString(normalizedRepoPath);
    std::filesystem::path bestParent;
    for (const auto& dep : InRepo.dependencies) {
        const auto normalizedDep = NormalizeRepoPath(InWorkspaceRoot, dep);
        const auto canonicalDepStr = CanonicalPathString(normalizedDep);
        const bool isSelf = (canonicalDepStr == canonicalRepoStr);
        const bool isAnc = IsAncestorPath(normalizedDep, normalizedRepoPath);
        if (isSelf || !isAnc) {
            continue;
        }
        if (bestParent.empty() || canonicalDepStr.size() > CanonicalPathString(bestParent).size()) {
            bestParent = normalizedDep;
        }
    }
    const auto result = bestParent.empty() ? "(none)" : CanonicalPathString(bestParent);
    return result;
}

auto UninitializedRepoListLabel(const std::filesystem::path& /* InWorkspaceRoot */,
                                const RepoView& /* InRepo */) -> std::string {
    return "(uninit)";
}

auto FinalizeRepoTree(std::vector<RepoView> InRows) -> std::vector<RepoView> {
    std::sort(InRows.begin(), InRows.end(), [](const RepoView& A, const RepoView& B) {
        return CanonicalPathString(A.path) < CanonicalPathString(B.path);
    });

    for (std::size_t i = 0; i < InRows.size(); ++i) {
        if (InRows[i].parentRepo.empty()) {
            InRows[i].parentRepo = "(none)";
            continue;
        }
        const auto it = std::find_if(InRows.begin(), InRows.end(), [&](const RepoView& row) {
            return CanonicalPathString(row.path) == InRows[i].parentRepo;
        });
        if (it == InRows.end()) {
            InRows[i].parentRepo = "(none)";
        }
    }

    for (auto& row : InRows) {
        int depth = 0;
        std::string cursor = row.parentRepo;
        while (cursor != "(none)") {
            depth += 1;
            std::string next = "(none)";
            for (const auto& maybeParent : InRows) {
                if (CanonicalPathString(maybeParent.path) == cursor) {
                    next = maybeParent.parentRepo;
                    break;
                }
            }
            cursor = next;
        }
        row.treeDepth = depth;
    }

    for (auto& row : InRows) {
        row.childRepoCount = 0;
    }
    for (const auto& row : InRows) {
        if (row.parentRepo == "(none)") {
            continue;
        }
        for (auto& maybeParent : InRows) {
            if (CanonicalPathString(maybeParent.path) == row.parentRepo) {
                maybeParent.childRepoCount += 1;
                break;
            }
        }
    }

    std::unordered_map<std::string, std::vector<std::size_t>> childIndices;
    std::vector<std::size_t> rootIndices;
    for (std::size_t i = 0; i < InRows.size(); ++i) {
        if (InRows[i].parentRepo == "(none)") {
            rootIndices.push_back(i);
        } else {
            childIndices[InRows[i].parentRepo].push_back(i);
        }
    }

    auto sortIndices = [&](std::vector<std::size_t>& Indices) {
        std::sort(Indices.begin(), Indices.end(), [&](const std::size_t A, const std::size_t B) {
            return CanonicalPathString(InRows[A].path) < CanonicalPathString(InRows[B].path);
        });
    };
    sortIndices(rootIndices);
    for (auto& [_, indices] : childIndices) {
        sortIndices(indices);
    }

    std::vector<RepoView> ordered;
    ordered.reserve(InRows.size());
    std::function<void(std::size_t)> appendSubtree = [&](const std::size_t Index) {
        const auto key = CanonicalPathString(InRows[Index].path);
        ordered.push_back(InRows[Index]);
        auto it = childIndices.find(key);
        if (it == childIndices.end()) {
            return;
        }
        for (const auto childIndex : it->second) {
            appendSubtree(childIndex);
        }
    };
    for (const auto rootIndex : rootIndices) {
        appendSubtree(rootIndex);
    }

    return ordered;
}

auto DiscoverWorkspaceRepoRecordsForTui(const std::filesystem::path& InRoot,
                                        const bool InUseCache = true,
                                        const bool InRefreshCache = false,
                                        const bool InRefreshRepoStatus = true,
                                        const std::function<void(const std::string&)>& InProgressCallback = {}) -> std::vector<workspace::RepoRecord> {
    const auto root = InRoot.lexically_normal();
    auto reportProgress = [&](const std::string& InMessage) {
        if (InProgressCallback) {
            InProgressCallback(InMessage);
        }
    };

    if (InUseCache && !InRefreshCache) {
        reportProgress("loading trusted workspace manifest");
        std::string manifestReason;
        if (const auto manifest = workspace::LoadTrustedWorkspaceManifest(root, &manifestReason); manifest.has_value()) {
            const auto registeredPaths = workspace::DiscoverRegisteredPathsRecursive(root);
            std::vector<std::string> expectedRegisteredKeys;
            expectedRegisteredKeys.reserve(registeredPaths.size() + 1);
            expectedRegisteredKeys.push_back(CanonicalPathString(root));
            for (const auto& registeredPath : registeredPaths) {
                expectedRegisteredKeys.push_back(CanonicalPathString(registeredPath));
            }
            std::sort(expectedRegisteredKeys.begin(), expectedRegisteredKeys.end());
            expectedRegisteredKeys.erase(std::unique(expectedRegisteredKeys.begin(), expectedRegisteredKeys.end()), expectedRegisteredKeys.end());

            std::vector<std::string> manifestRegisteredKeys;
            manifestRegisteredKeys.reserve(manifest->repos.size());
            for (const auto& repo : manifest->repos) {
                if (repo.type == "root" || repo.type == "registered" || repo.type == "registered-uninit") {
                    manifestRegisteredKeys.push_back(CanonicalPathString(NormalizeRepoPath(root, repo.path)));
                }
            }
            std::sort(manifestRegisteredKeys.begin(), manifestRegisteredKeys.end());
            manifestRegisteredKeys.erase(std::unique(manifestRegisteredKeys.begin(), manifestRegisteredKeys.end()), manifestRegisteredKeys.end());

            if (manifestRegisteredKeys != expectedRegisteredKeys) {
                reportProgress("trusted workspace manifest stale - falling back to full discovery");
            } else {
                std::vector<workspace::RepoRecord> repos = manifest->repos;
                if (repos.empty()) {
                    workspace::RepoRecord rootRecord;
                    rootRecord.path = root;
                    rootRecord.type = "root";
                    repos.push_back(std::move(rootRecord));
                }
                if (InRefreshRepoStatus) {
                    reportProgress(std::format("refreshing git status for {} repos", repos.size()));
                    std::size_t repoIndex = 0;
                    for (auto& repo : repos) {
                        repoIndex += 1;
                        if (repo.type == "registered-uninit") {
                            continue;
                        }
                        // Re-validate registered repos: the cached type may be stale if the
                        // repo was not yet initialized when the manifest was last written.
                        // Check that git's --show-toplevel actually points at this path;
                        // if it doesn't (e.g. git walks up to a parent), reclassify.
                        if (repo.type == "registered") {
                            const auto topLevel = GitCapture(repo.path, {"rev-parse", "--show-toplevel"});
                            if (topLevel.exitCode != 0 ||
                                CanonicalPathString(repo.path) != CanonicalPathString(std::filesystem::path(Trim(topLevel.stdoutStr)))) {
                                repo.type = "registered-uninit";
                                repo.currentBranch.clear();
                                repo.hasChanges = false;
                                repo.remotes.clear();
                                continue;
                            }
                        }
                        if (repoIndex == 1 || repoIndex == repos.size() || (repoIndex % 4) == 0) {
                            reportProgress(std::format("git status {}/{}: {}", repoIndex, repos.size(), DisplayRepoPath(root, repo.path)));
                        }
                        const auto status = GitCapture(repo.path, {"status", "--porcelain"});
                        repo.hasChanges = status.exitCode == 0 && !Trim(status.stdoutStr).empty();
                    }
                } else {
                    reportProgress(std::format("using cached workspace manifest for {} repos", repos.size()));
                }
                std::sort(repos.begin(), repos.end(), [](const auto& A, const auto& B) {
                    return CanonicalPathString(A.path) < CanonicalPathString(B.path);
                });
                repos.erase(std::unique(repos.begin(), repos.end(), [](const auto& A, const auto& B) {
                    return CanonicalPathString(A.path) == CanonicalPathString(B.path);
                }), repos.end());
                return repos;
            }
        }
    }

    workspace::DiscoverOptions options;
    options.rootDir = root;
    options.maxDepth = 8;
    options.useCache = InUseCache;
    options.refreshCache = InRefreshCache;
    if (!InUseCache) {
        options.incremental = false;
    }
    options.metadataLevel = "full";
    options.progressCallback = [&](const std::string& InMessage) {
        reportProgress(InMessage);
    };

    const auto discovery = workspace::DiscoverRepos(options);
    std::vector<workspace::RepoRecord> repos = discovery.repos;
    std::sort(repos.begin(), repos.end(), [](const auto& A, const auto& B) {
        return CanonicalPathString(A.path) < CanonicalPathString(B.path);
    });
    repos.erase(std::unique(repos.begin(), repos.end(), [](const auto& A, const auto& B) {
        return CanonicalPathString(A.path) == CanonicalPathString(B.path);
    }), repos.end());

    if (!repos.empty()) {
        const auto manifest = workspace::BuildWorkspaceManifest(root, repos);
        if (!workspace::SaveWorkspaceManifest(manifest)) {
            std::cerr << "[tui] WARN: failed to refresh workspace manifest at "
                      << manifest.manifestFile.lexically_normal().generic_string() << "\n";
        }
    }

    return repos;
}

auto DiscoverRepoViews(const bool InDirtyOnly,
                      const bool InRefreshCache = false,
                      const bool InRefreshRepoStatus = true) -> std::vector<RepoView> {
    const auto workspaceRoot = std::filesystem::current_path().lexically_normal();
    const auto repoRecords = DiscoverWorkspaceRepoRecordsForTui(
        workspaceRoot,
        true,
        InRefreshCache,
        InRefreshRepoStatus);
    std::vector<RepoView> rows;
    rows.reserve(repoRecords.size());

    for (const auto& repo : repoRecords) {
        if (InDirtyOnly && !repo.hasChanges) {
            continue;
        }
        const auto normalizedRepoPath = NormalizeRepoPath(workspaceRoot, repo.path);
        if (repo.type == "registered-uninit") {
            RepoView row;
            row.path = normalizedRepoPath;
            row.type = repo.type;
            row.parentRepo = ResolveRepoParent(workspaceRoot, repo);
            row.statusFromSnapshot = true;
            row.repoDirty = false;
            row.type = "registered-uninit";
            row.branch = "(uninit)";
            row.upstream.clear();
            row.tracking.clear();
            row.worktreeDirty = false;
            rows.push_back(std::move(row));
            continue;
        }
        RepoView row;
        row.path = normalizedRepoPath;
        row.type = repo.type;
        row.parentRepo = ResolveRepoParent(workspaceRoot, repo);
        row.statusFromSnapshot = !InRefreshRepoStatus;
        row.repoDirty = repo.hasChanges;
        if (InRefreshRepoStatus) {
            // Validate that the path is actually an initialized git repo before querying branch.
            // Handles stale manifest entries where type="registered" but directory does not exist.
            if (repo.type == "registered") {
                const auto topLevel = GitCapture(normalizedRepoPath, {"rev-parse", "--show-toplevel"});
                if (topLevel.exitCode != 0 ||
                    CanonicalPathString(normalizedRepoPath) != CanonicalPathString(std::filesystem::path(Trim(topLevel.stdoutStr)))) {
                    row.type = "registered-uninit";
                    row.branch = "(uninit)";
                    row.upstream.clear();
                    row.tracking.clear();
                    row.statusFromSnapshot = true;
                    row.repoDirty = false;
                    rows.push_back(std::move(row));
                    continue;
                }
            }
            row.branch = CurrentBranch(normalizedRepoPath);
            row.upstream = CurrentUpstream(normalizedRepoPath);
            row.tracking = TrackingSummary(normalizedRepoPath);
            row.worktreeDirty = HasDirtyWorktrees(normalizedRepoPath, row.dirtyWorktrees);
            row.dirtyFiles = DirtyFiles(normalizedRepoPath);
        } else {
            row.branch = repo.currentBranch.empty() ? "(cached)" : repo.currentBranch;
            row.upstream = "(cached)";
            row.tracking = "cached";
            row.worktreeDirty = false;
            row.dirtyWorktrees.clear();
            row.dirtyFiles.clear();
        }
        rows.push_back(std::move(row));
    }

    return FinalizeRepoTree(std::move(rows));
}

auto DiscoverRepoViews(const bool InDirtyOnly,
                       const bool InRefreshCache,
                       const bool InRefreshRepoStatus,
                       const std::function<void(const std::string&)>& InProgressCallback) -> std::vector<RepoView> {
    const auto workspaceRoot = std::filesystem::current_path().lexically_normal();
    const auto repoRecords = DiscoverWorkspaceRepoRecordsForTui(
        workspaceRoot,
        true,
        InRefreshCache,
        InRefreshRepoStatus,
        InProgressCallback);
    std::vector<RepoView> rows;
    rows.reserve(repoRecords.size());

    for (const auto& repo : repoRecords) {
        if (InDirtyOnly && !repo.hasChanges) {
            continue;
        }
        const auto normalizedRepoPath = NormalizeRepoPath(workspaceRoot, repo.path);
        if (repo.type == "registered-uninit") {
            RepoView row;
            row.path = normalizedRepoPath;
            row.type = repo.type;
            row.parentRepo = ResolveRepoParent(workspaceRoot, repo);
            row.statusFromSnapshot = true;
            row.repoDirty = false;
            row.type = "registered-uninit";
            row.branch = "(uninit)";
            row.upstream.clear();
            row.tracking.clear();
            row.worktreeDirty = false;
            rows.push_back(std::move(row));
            continue;
        }
        RepoView row;
        row.path = normalizedRepoPath;
        row.type = repo.type;
        row.parentRepo = ResolveRepoParent(workspaceRoot, repo);
        row.statusFromSnapshot = !InRefreshRepoStatus;
        row.repoDirty = repo.hasChanges;
        if (InRefreshRepoStatus) {
            // Validate that the path is actually an initialized git repo before querying branch.
            // Handles stale manifest entries where type="registered" but directory does not exist.
            if (repo.type == "registered") {
                const auto topLevel = GitCapture(normalizedRepoPath, {"rev-parse", "--show-toplevel"});
                if (topLevel.exitCode != 0 ||
                    CanonicalPathString(normalizedRepoPath) != CanonicalPathString(std::filesystem::path(Trim(topLevel.stdoutStr)))) {
                    row.type = "registered-uninit";
                    row.branch = "(uninit)";
                    row.upstream.clear();
                    row.tracking.clear();
                    row.statusFromSnapshot = true;
                    row.repoDirty = false;
                    rows.push_back(std::move(row));
                    continue;
                }
            }
            row.branch = CurrentBranch(normalizedRepoPath);
            row.upstream = CurrentUpstream(normalizedRepoPath);
            row.tracking = TrackingSummary(normalizedRepoPath);
            row.worktreeDirty = HasDirtyWorktrees(normalizedRepoPath, row.dirtyWorktrees);
            row.dirtyFiles = DirtyFiles(normalizedRepoPath);
        } else {
            row.branch = repo.currentBranch.empty() ? "(cached)" : repo.currentBranch;
            row.upstream = "(cached)";
            row.tracking = "cached";
            row.worktreeDirty = false;
            row.dirtyWorktrees.clear();
            row.dirtyFiles.clear();
        }
        rows.push_back(std::move(row));
    }

    return FinalizeRepoTree(std::move(rows));
}

auto BuildLiveRepoView(const std::filesystem::path& InWorkspaceRoot,
                       const RepoView& InCurrentRow) -> RepoView {
    RepoView row = InCurrentRow;
    row.path = InCurrentRow.path.lexically_normal();
    row.parentRepo = InCurrentRow.parentRepo;
    row.statusFromSnapshot = false;
    row.repoDirty = false;
    row.worktreeDirty = false;
    row.dirtyWorktrees.clear();
    row.dirtyFiles.clear();

    if (InCurrentRow.type == "registered-uninit" || InCurrentRow.type == "registered") {
        const auto topLevel = GitCapture(row.path, {"rev-parse", "--show-toplevel"});
        if (topLevel.exitCode != 0 ||
            CanonicalPathString(row.path) != CanonicalPathString(std::filesystem::path(Trim(topLevel.stdoutStr)))) {
            row.type = "registered-uninit";
            row.branch = "(uninit)";
            row.upstream.clear();
            row.tracking.clear();
            return row;
        }
        row.type = "registered";
    }

    const auto status = GitCapture(row.path, {"status", "--porcelain"});
    row.repoDirty = status.exitCode == 0 && !Trim(status.stdoutStr).empty();
    row.branch = CurrentBranch(row.path);
    row.upstream = CurrentUpstream(row.path);
    row.tracking = TrackingSummary(row.path);
    row.worktreeDirty = HasDirtyWorktrees(row.path, row.dirtyWorktrees);
    row.dirtyFiles = DirtyFiles(row.path);
    return row;
}

auto ComputeHistoryPageSize() -> int {
    return std::max(5, ftxui::Terminal::Size().dimy - 14);
}

auto ComputeDetailBodyPageSize() -> int {
    return std::max(5, ftxui::Terminal::Size().dimy - 12);
}

auto ComputeHistoryRowWidthEstimate() -> int {
    constexpr int kHistoryLeftPanelWidth = 22;
    constexpr int kApproxChromeWidth = 12;
    return std::max(24, ftxui::Terminal::Size().dimx - kHistoryLeftPanelWidth - kApproxChromeWidth);
}

auto FetchAllHistory(const RepoView& InRepo) -> std::vector<RepoHistoryCache::HistoryEntry> {
    const auto totalCommits = FetchTotalCommitCount(InRepo.path);
    const bool includeDirtyEntry = HasDirtyHistoryEntry(InRepo);
    const int totalEntries = totalCommits.has_value() ? (totalCommits.value() + (includeDirtyEntry ? 1 : 0)) : 0;

    std::vector<RepoHistoryCache::HistoryEntry> entries;
    if (includeDirtyEntry) {
        RepoHistoryCache::HistoryEntry dirtyEntry;
        dirtyEntry.isDirtyWorkingTree = true;
        dirtyEntry.sha = "(dirty)";
        dirtyEntry.subject = "dirty working tree";
        dirtyEntry.globalIndex = 1;
        dirtyEntry.totalCount = totalEntries;
        dirtyEntry.displayLine = BuildHistoryDisplayLine(dirtyEntry);
        entries.push_back(std::move(dirtyEntry));
    }

    const auto out = GitCapture(InRepo.path, {
        "log", "--no-decorate", "--format=%h%x1f%s%x1f%ae%x1f%an",
    });
    if (out.exitCode != 0) {
        RepoHistoryCache::HistoryEntry failedEntry;
        failedEntry.subject = "(failed to load history)";
        failedEntry.globalIndex = includeDirtyEntry ? 2 : 1;
        failedEntry.totalCount = totalEntries;
        failedEntry.displayLine = failedEntry.subject;
        entries.push_back(std::move(failedEntry));
        return entries;
    }

    std::istringstream iss(out.stdoutStr);
    std::string line;
    int entryOffset = includeDirtyEntry ? 2 : 1;
    while (std::getline(iss, line)) {
        line = Trim(line);
        if (line.empty()) {
            continue;
        }
        std::vector<std::string> parts;
        std::size_t start = 0;
        while (true) {
            const auto sep = line.find('\x1f', start);
            if (sep == std::string::npos) {
                parts.push_back(line.substr(start));
                break;
            }
            parts.push_back(line.substr(start, sep - start));
            start = sep + 1;
        }
        RepoHistoryCache::HistoryEntry entry;
        entry.sha = parts.size() > 0 ? Trim(parts[0]) : std::string();
        entry.subject = parts.size() > 1 ? Trim(parts[1]) : std::string();
        entry.authorEmail = parts.size() > 2 ? Trim(parts[2]) : std::string();
        entry.authorName = parts.size() > 3 ? Trim(parts[3]) : std::string();
        entry.globalIndex = entryOffset;
        entry.totalCount = totalEntries;
        entry.displayLine = BuildHistoryDisplayLine(entry);
        entries.push_back(std::move(entry));
        entryOffset += 1;
    }
    return entries;
}

auto FetchTotalCommitCount(const std::filesystem::path& InRepo) -> std::optional<int> {
    const auto out = GitCapture(InRepo, {"rev-list", "--count", "HEAD"});
    if (out.exitCode != 0) {
        return std::nullopt;
    }
    const auto text = Trim(out.stdoutStr);
    if (text.empty()) {
        return std::nullopt;
    }
    try {
        return std::stoi(text);
    } catch (...) {
        return std::nullopt;
    }
}

auto HasDirtyHistoryEntry(const RepoView& InRepo) -> bool {
    return InRepo.repoDirty || InRepo.worktreeDirty || !InRepo.dirtyFiles.empty();
}

auto BuildHistoryDisplayLine(const RepoHistoryCache::HistoryEntry& InEntry, const std::string& InAuthorText) -> std::string {
    const std::string indexText = InEntry.totalCount > 0
        ? std::to_string(InEntry.globalIndex) + "/" + std::to_string(InEntry.totalCount)
        : std::to_string(InEntry.globalIndex) + "/?";
    std::string line = "[" + indexText + "] ";
    if (InEntry.isDirtyWorkingTree) {
        line += "(dirty) dirty working tree";
    } else {
        line += InEntry.sha + " " + InEntry.subject;
    }
    if (!InAuthorText.empty()) {
        line += " | " + InAuthorText;
    }
    return line;
}

auto CommitShaFromOneline(const std::string& line) -> std::string {
    const auto trimmed = Trim(line);
    const auto sp = trimmed.find(' ');
    if (sp == std::string::npos) {
        return trimmed;
    }
    return trimmed.substr(0, sp);
}

auto FetchCommitDetail(const std::filesystem::path& repo, const std::string& sha, int mode) -> std::string {
    if (sha.empty()) {
        return "(invalid commit sha)";
    }

    std::vector<std::string> args;
    if (mode == 1) {
        args = {"show", "--no-color", "--date=iso", "-M", "-C", "--name-status", "--pretty=fuller", "-n", "1", sha};
    } else if (mode == 2) {
        args = {"show", "--no-color", "--date=iso", "-M", "-C", "--pretty=fuller", "-n", "1", sha};
    } else {
        args = {"show", "--no-color", "--date=iso", "-M", "-C", "--stat", "--name-status", "--pretty=fuller", "-n", "1", sha};
    }

    const auto out = GitCapture(repo, args);
    if (out.exitCode != 0) {
        return "(failed to load commit detail)\n" + out.stderrStr;
    }
    auto body = out.stdoutStr;
    constexpr std::size_t kMaxChars = 20000;
    if (body.size() > kMaxChars) {
        body = body.substr(0, kMaxChars) + "\n... (truncated)";
    }
    return body;
}

auto FetchCommitQuickStats(const std::filesystem::path& repo, const std::string& sha) -> std::string {
    if (sha.empty()) {
        return "(invalid commit sha)";
    }
    const auto out = GitCapture(repo, {
        "show", "--no-color", "--shortstat", "--pretty=format:%h %s", "-n", "1", sha,
    });
    if (out.exitCode != 0) {
        return "(failed to load quick stats)";
    }

    std::istringstream iss(out.stdoutStr);
    std::string title;
    std::getline(iss, title);
    title = Trim(title);

    std::string stat;
    while (std::getline(iss, stat)) {
        stat = Trim(stat);
        if (!stat.empty()) {
            break;
        }
    }

    if (stat.empty()) {
        stat = "0 files changed";
    }

    if (title.empty()) {
        return stat;
    }
    return title + " | " + stat;
}

auto ToLowerAscii(std::string value) -> std::string {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        if (c >= 'A' && c <= 'Z') {
            return static_cast<char>(c - 'A' + 'a');
        }
        return static_cast<char>(c);
    });
    return value;
}

auto FindNextMatch(const std::vector<std::string>& lines,
                   const std::string& query,
                   int startExclusive) -> int {
    if (query.empty() || lines.empty()) {
        return -1;
    }
    const auto needle = ToLowerAscii(query);
    const int size = static_cast<int>(lines.size());
    for (int step = 1; step <= size; ++step) {
        const int idx = (startExclusive + step + size) % size;
        if (ToLowerAscii(lines[idx]).find(needle) != std::string::npos) {
            return idx;
        }
    }
    return -1;
}

auto BuildDisplayedHistoryEntries(std::vector<RepoHistoryCache::HistoryEntry> InEntries,
                                  const HistoryState& InHistory) -> std::vector<RepoHistoryCache::HistoryEntry> {
    if (InHistory.sortMode == 1) {
        std::reverse(InEntries.begin(), InEntries.end());
    } else if (InHistory.sortMode == 2 && !InHistory.searchQuery.empty()) {
        std::stable_sort(InEntries.begin(), InEntries.end(), [&](const auto& a, const auto& b) {
            const auto aAuthor = !a.authorEmail.empty() ? a.authorEmail : a.authorName;
            const auto bAuthor = !b.authorEmail.empty() ? b.authorEmail : b.authorName;
            const bool am = ToLowerAscii(BuildHistoryDisplayLine(a, aAuthor)).find(ToLowerAscii(InHistory.searchQuery)) != std::string::npos;
            const bool bm = ToLowerAscii(BuildHistoryDisplayLine(b, bAuthor)).find(ToLowerAscii(InHistory.searchQuery)) != std::string::npos;
            return am > bm;
        });
    }
    return InEntries;
}

auto SplitLines(const std::string& InText) -> std::vector<std::string> {
    std::vector<std::string> lines;
    std::istringstream iss(InText);
    std::string line;
    while (std::getline(iss, line)) {
        lines.push_back(line);
    }
    if (lines.empty()) {
        lines.push_back("");
    }
    return lines;
}

auto JoinLines(const std::vector<std::string>& InLines) -> std::string {
    std::ostringstream out;
    for (std::size_t i = 0; i < InLines.size(); ++i) {
        if (i > 0) {
            out << "\n";
        }
        out << InLines[i];
    }
    return out.str();
}

struct ParsedFileLine {
    std::string displayTitle;
    std::string patchPath;
    std::string patchPathAlt;  // old path for rename/copy entries
};

auto ParseHistoryFileLine(const std::string& InLine) -> ParsedFileLine {
    const auto trimmed = Trim(InLine);
    if (trimmed.size() <= 2 || std::isspace(static_cast<unsigned char>(trimmed[1])) == 0) {
        return {trimmed, std::string(), std::string()};
    }

    std::string displayTitle = trimmed;
    std::string patchPath;
    std::string patchPathAlt;
    const auto payload = Trim(trimmed.substr(2));
    const auto tabPos = payload.find('\t');
    if ((trimmed[0] == 'R' || trimmed[0] == 'C') && tabPos != std::string::npos) {
        const auto oldPath = Trim(payload.substr(0, tabPos));
        const auto newPath = Trim(payload.substr(tabPos + 1));
        displayTitle = std::string(1, trimmed[0]) + " " + oldPath + " -> " + newPath;
        patchPath = newPath;
        patchPathAlt = oldPath;
    } else {
        patchPath = payload;
    }
    return {displayTitle, patchPath, patchPathAlt};
}

auto FetchCommitFilePatch(const std::filesystem::path& repo,
                          const std::string& sha,
                          const std::string& patchPath,
                          const std::string& patchPathAlt = {}) -> std::string {
    if (sha.empty()) {
        return "(invalid commit sha)";
    }
    if (patchPath.empty()) {
        return FetchCommitDetail(repo, sha, 2);
    }

    std::vector<std::string> args = {
        "show", "--no-color", "--date=iso", "-M", "-C", "--pretty=fuller", "-n", "1", sha, "--", patchPath,
    };
    if (!patchPathAlt.empty() && patchPathAlt != patchPath) {
        args.push_back(patchPathAlt);
    }
    const auto out = GitCapture(repo, args);
    if (out.exitCode != 0) {
        return "(failed to load file patch)\n" + out.stderrStr;
    }
    auto body = out.stdoutStr;

    // Merge commits: git show <sha> -- <path> may produce empty combined diff.
    // Fallback: compare against first parent (sha~1) to show the actual change.
    if (Trim(body).empty() || body.find("diff ") == std::string::npos) {
        std::vector<std::string> fallbackArgs = {
            "diff", "--no-color", sha + "~1", sha, "--", patchPath,
        };
        if (!patchPathAlt.empty() && patchPathAlt != patchPath) {
            fallbackArgs.push_back(patchPathAlt);
        }
        const auto fb = GitCapture(repo, fallbackArgs);
        if (fb.exitCode == 0 && !Trim(fb.stdoutStr).empty()) {
            body = fb.stdoutStr;
        }
    }

    constexpr std::size_t kMaxChars = 20000;
    if (body.size() > kMaxChars) {
        body = body.substr(0, kMaxChars) + "\n... (truncated)";
    }
    return body.empty() ? "(no patch for selected file)" : body;
}

auto FetchWorkingTreeFilePatch(const std::filesystem::path& repo,
                               const std::string& patchPath,
                               const std::string& patchPathAlt = {}) -> std::string {
    if (patchPath.empty()) {
        return FetchWorkingTreeDetail(repo, 2);
    }

    auto buildArgs = [&](std::vector<std::string> base) -> std::vector<std::string> {
        base.push_back("--");
        base.push_back(patchPath);
        if (!patchPathAlt.empty() && patchPathAlt != patchPath) {
            base.push_back(patchPathAlt);
        }
        return base;
    };

    const auto unstaged = GitCapture(repo, buildArgs({"diff", "--no-color"}));
    const auto staged = GitCapture(repo, buildArgs({"diff", "--no-color", "--cached"}));
    const auto fallback = GitCapture(repo, buildArgs({"diff", "--no-color", "HEAD"}));

    if (unstaged.exitCode != 0 && staged.exitCode != 0 && fallback.exitCode != 0) {
        return "(failed to load file patch)\n" + fallback.stderrStr;
    }

    std::string body;
    const auto unstagedBody = Trim(unstaged.stdoutStr);
    const auto stagedBody = Trim(staged.stdoutStr);
    if (!unstagedBody.empty()) {
        body += "# unstaged\n" + unstaged.stdoutStr;
    }
    if (!stagedBody.empty()) {
        if (!body.empty()) {
            body += "\n\n";
        }
        body += "# staged\n" + staged.stdoutStr;
    }
    if (body.empty() && fallback.exitCode == 0) {
        body = fallback.stdoutStr;
    }

    // Fallback for untracked files: git diff returns nothing for files not in the index.
    // git diff --no-index -- /dev/null <path> shows new file content as a diff.
    // Exit code 1 is expected (means differences found).
    if (Trim(body).empty()) {
        const auto noIndex = GitCapture(repo, {"diff", "--no-color", "--no-index", "--", "/dev/null", patchPath});
        if ((noIndex.exitCode == 0 || noIndex.exitCode == 1) && !Trim(noIndex.stdoutStr).empty()) {
            body = "# untracked (new file)\n" + noIndex.stdoutStr;
        }
    }

    constexpr std::size_t kMaxChars = 20000;
    if (body.size() > kMaxChars) {
        body = body.substr(0, kMaxChars) + "\n... (truncated)";
    }
    return Trim(body).empty() ? "(no patch for selected file)" : body;
}

auto BuildHistoryDetailOverlay(const std::filesystem::path& repo,
                               const RepoView& repoView,
                               const RepoHistoryCache::HistoryEntry& entry,
                               const int mode) -> RepoHistoryCache::DetailOverlayData {
    RepoHistoryCache::DetailOverlayData overlay;
    overlay.title = entry.isDirtyWorkingTree ? "dirty working tree" : entry.sha;

    if (mode == 0) {
        RepoHistoryCache::DetailSection section;
        section.title = "summary";
        section.body = entry.isDirtyWorkingTree
            ? FetchWorkingTreeDetail(repo, mode)
            : FetchCommitDetail(repo, entry.sha, mode);
        overlay.sections.push_back(std::move(section));
        return overlay;
    }

    std::vector<RepoHistoryCache::DetailSection> sections;
    if (entry.isDirtyWorkingTree) {
        for (const auto& dirtyFile : repoView.dirtyFiles) {
            RepoHistoryCache::DetailSection section;
            section.title = dirtyFile.displayPath;
            section.patchPath = dirtyFile.patchPath;
            section.body = mode == 2
                ? FetchWorkingTreeFilePatch(repo, dirtyFile.patchPath)
                : (dirtyFile.changedLines >= 0 ? ("changed lines: " + std::to_string(dirtyFile.changedLines)) : "changed file");
            sections.push_back(std::move(section));
        }
        if (sections.empty() && repoView.statusFromSnapshot) {
            RepoHistoryCache::DetailSection section;
            section.title = "(snapshot cache)";
            section.body = "run :refresh to enumerate dirty files";
            sections.push_back(std::move(section));
        }
    } else {
        // Use -z -M -C --name-status for unambiguous NUL-separated parsing of file entries.
        const auto nsOut = GitCapture(repo, {
            "show", "--no-color", "-z", "-M", "-C", "--name-status", "--pretty=format:", "-n", "1", entry.sha,
        });

        if (nsOut.exitCode == 0 && !nsOut.stdoutStr.empty()) {
            // Parse NUL-separated name-status output.
            // Format: <status>\t<path>\0  for A/M/D/T/U
            //         <status>\t<oldPath>\0<newPath>\0  for R/C (rename/copy)
            const auto& raw = nsOut.stdoutStr;
            std::size_t pos = 0;
            while (pos < raw.size()) {
                // Skip leading NUL bytes (the pretty=format: empty output leaves a leading NUL)
                if (raw[pos] == '\0' || raw[pos] == '\n') {
                    ++pos;
                    continue;
                }
                // Read status + tab + path up to NUL
                const auto nulPos = raw.find('\0', pos);
                if (nulPos == std::string::npos) {
                    break;
                }
                const auto entry_str = raw.substr(pos, nulPos - pos);
                pos = nulPos + 1;

                if (entry_str.empty()) {
                    continue;
                }

                const char status = entry_str[0];
                // Find the tab separator between status and path
                const auto tabPos = entry_str.find('\t');
                if (tabPos == std::string::npos) {
                    continue;
                }
                const auto path1 = entry_str.substr(tabPos + 1);

                RepoHistoryCache::DetailSection section;
                if (status == 'R' || status == 'C') {
                    // For rename/copy, path1 is oldPath, next NUL-terminated field is newPath
                    const auto nulPos2 = raw.find('\0', pos);
                    std::string newPath;
                    if (nulPos2 != std::string::npos) {
                        newPath = raw.substr(pos, nulPos2 - pos);
                        pos = nulPos2 + 1;
                    } else {
                        newPath = raw.substr(pos);
                        pos = raw.size();
                    }
                    section.title = std::string(1, status) + " " + path1 + " -> " + newPath;
                    section.patchPath = newPath;
                    section.patchPathAlt = path1;
                } else {
                    section.title = std::string(1, status) + " " + path1;
                    section.patchPath = path1;
                }

                section.body = mode == 2
                    ? FetchCommitFilePatch(repo, entry.sha, section.patchPath, section.patchPathAlt)
                    : section.title;
                sections.push_back(std::move(section));
            }
        }

        // Fallback: if -z parsing produced nothing, try the text-based approach
        if (sections.empty()) {
            const auto filesBody = FetchCommitDetail(repo, entry.sha, 1);
            std::istringstream iss(filesBody);
            std::string line;
            std::vector<std::string> block;
            std::string blockPatchPath;
            std::string blockPatchPathAlt;
            auto flushBlock = [&]() {
                if (block.empty()) {
                    return;
                }
                RepoHistoryCache::DetailSection section;
                section.title = block.front();
                section.patchPath = blockPatchPath;
                section.patchPathAlt = blockPatchPathAlt;
                section.body = mode == 2
                    ? FetchCommitFilePatch(repo, entry.sha, blockPatchPath, blockPatchPathAlt)
                    : JoinLines(block);
                sections.push_back(std::move(section));
                block.clear();
                blockPatchPath.clear();
                blockPatchPathAlt.clear();
            };
            while (std::getline(iss, line)) {
                const auto trimmed = Trim(line);
                const bool isFileLine = trimmed.size() > 2
                    && (trimmed[0] == 'A' || trimmed[0] == 'M' || trimmed[0] == 'D' || trimmed[0] == 'R' || trimmed[0] == 'C' || trimmed[0] == 'T' || trimmed[0] == 'U')
                    && std::isspace(static_cast<unsigned char>(trimmed[1])) != 0;
                if (isFileLine) {
                    flushBlock();
                    const auto parsed = ParseHistoryFileLine(trimmed);
                    block.push_back(parsed.displayTitle);
                    blockPatchPath = parsed.patchPath;
                    blockPatchPathAlt = parsed.patchPathAlt;
                    continue;
                }
                if (!block.empty()) {
                    block.push_back(line);
                }
            }
            flushBlock();
        }
    }

    if (sections.empty()) {
        RepoHistoryCache::DetailSection section;
        section.title = "(no changes)";
        section.body = mode == 2
            ? (entry.isDirtyWorkingTree ? FetchWorkingTreeDetail(repo, 2) : FetchCommitDetail(repo, entry.sha, 2))
            : (entry.isDirtyWorkingTree ? FetchWorkingTreeDetail(repo, 1) : FetchCommitDetail(repo, entry.sha, 1));
        sections.push_back(std::move(section));
    }

    overlay.sections = std::move(sections);
    return overlay;
}

auto ParseStatusFiles(const shell::ExecResult& out) -> std::pair<std::vector<std::string>, std::vector<std::string>> {
    std::vector<std::string> staged;
    std::vector<std::string> unstaged;
    if (out.exitCode != 0) {
        return {staged, unstaged};
    }

    std::istringstream iss(out.stdoutStr);
    std::string line;
    while (std::getline(iss, line)) {
        if (line.size() < 4) {
            continue;
        }
        const char x = line[0];
        const char y = line[1];
        const std::string file = Trim(line.substr(3));
        if (file.empty()) {
            continue;
        }
        if (x != ' ' && x != '?') {
            staged.push_back(file);
        }
        if (y != ' ' || x == '?') {
            unstaged.push_back(file);
        }
    }

    auto dedupe = [](std::vector<std::string>& v) {
        std::sort(v.begin(), v.end());
        v.erase(std::unique(v.begin(), v.end()), v.end());
    };
    dedupe(staged);
    dedupe(unstaged);
    return {staged, unstaged};
}

auto CollectPreviewData(const std::filesystem::path& repo) -> PreviewData {
    PreviewData data;
    data.branch = CurrentBranch(repo);
    data.upstream = CurrentUpstream(repo);
    data.tracking = TrackingSummary(repo);

    const auto status = GitCapture(repo, {"status", "--porcelain"});
    auto parsed = ParseStatusFiles(status);
    data.staged = std::move(parsed.first);
    data.unstaged = std::move(parsed.second);
    return data;
}

auto BuildCommitPreview(const std::filesystem::path& repo) -> std::string {
    const auto data = CollectPreviewData(repo);
    std::ostringstream out;
    out << "Commit Preview\n";
    out << "repo: " << repo.lexically_normal().generic_string() << "\n";
    out << "branch: " << data.branch << "\n\n";

    out << "staged files: " << data.staged.size() << "\n";
    for (std::size_t i = 0; i < std::min<std::size_t>(data.staged.size(), 20); ++i) {
        out << "  + " << data.staged[i] << "\n";
    }
    if (data.staged.size() > 20) {
        out << "  ... and " << (data.staged.size() - 20) << " more\n";
    }

    out << "\nunstaged/untracked files: " << data.unstaged.size() << "\n";
    for (std::size_t i = 0; i < std::min<std::size_t>(data.unstaged.size(), 20); ++i) {
        out << "  ! " << data.unstaged[i] << "\n";
    }
    if (data.unstaged.size() > 20) {
        out << "  ... and " << (data.unstaged.size() - 20) << " more\n";
    }

    out << "\nrisk summary:\n";
    if (data.staged.empty()) {
        out << "  - HIGH: nothing staged (commit would fail)\n";
    }
    if (!data.unstaged.empty()) {
        out << "  - MEDIUM: unstaged changes present; commit may be incomplete\n";
    }
    if (!data.staged.empty() && data.unstaged.empty()) {
        out << "  - LOW: staged set appears clean\n";
    }
    return out.str();
}

auto BuildPushPreview(const std::filesystem::path& repo) -> std::string {
    const auto data = CollectPreviewData(repo);
    std::ostringstream out;
    out << "Push Preview\n";
    out << "repo: " << repo.lexically_normal().generic_string() << "\n";
    out << "branch: " << data.branch << "\n";
    out << "upstream: " << data.upstream << "\n";
    out << "tracking: " << data.tracking << "\n\n";

    out << "risk summary:\n";
    if (data.upstream == "(none)" || data.tracking == "no-upstream") {
        out << "  - HIGH: no upstream tracking branch\n";
    } else if (data.tracking.find("behind") != std::string::npos && data.tracking.find("ahead") == std::string::npos) {
        out << "  - HIGH: behind upstream; push likely rejected\n";
    } else if (data.tracking.find("behind") != std::string::npos && data.tracking.find("ahead") != std::string::npos) {
        out << "  - MEDIUM: diverged with upstream; rebase/merge recommended\n";
    } else if (data.tracking.find("ahead") != std::string::npos) {
        out << "  - LOW: ahead commits available to push\n";
    } else {
        out << "  - INFO: branch appears in sync\n";
    }

    if (!data.unstaged.empty()) {
        out << "  - INFO: working tree has local changes (does not block push)\n";
    }
    return out.str();
}

auto ListBranches(const std::filesystem::path& repo) -> std::vector<std::string> {
    const auto out = GitCapture(repo, {"for-each-ref", "--format=%(refname:short)", "refs/heads"});
    std::vector<std::string> branches;
    if (out.exitCode != 0) {
        return branches;
    }
    std::istringstream iss(out.stdoutStr);
    std::string line;
    while (std::getline(iss, line)) {
        line = Trim(line);
        if (!line.empty()) {
            branches.push_back(line);
        }
    }
    return branches;
}

auto CommitFileSet(const std::filesystem::path& repo, const std::string& sha) -> std::vector<std::string> {
    const auto out = GitCapture(repo, {"show", "--name-only", "--pretty=format:", "-n", "1", sha});
    std::vector<std::string> files;
    if (out.exitCode != 0) {
        return files;
    }
    std::istringstream iss(out.stdoutStr);
    std::string line;
    while (std::getline(iss, line)) {
        line = Trim(line);
        if (!line.empty()) {
            files.push_back(line);
        }
    }
    std::sort(files.begin(), files.end());
    files.erase(std::unique(files.begin(), files.end()), files.end());
    return files;
}

auto HasPatchEquivalentInTarget(const std::filesystem::path& repo,
                                const std::string& targetBranch,
                                const std::string& sha) -> bool {
    const auto out = GitCapture(repo, {"cherry", targetBranch, sha});
    if (out.exitCode != 0) {
        return false;
    }
    std::istringstream iss(out.stdoutStr);
    std::string line;
    while (std::getline(iss, line)) {
        line = Trim(line);
        if (!line.empty() && line[0] == '-') {
            return true;
        }
    }
    return false;
}

auto BuildCherryPickPreflight(const std::filesystem::path& repo,
                              const std::string& sourceBranch,
                              const std::string& targetBranch) -> CherryPickPreflightState {
    CherryPickPreflightState state;
    state.active = true;
    state.sourceBranch = sourceBranch;
    state.targetBranch = targetBranch;

    const auto list = GitCapture(repo, {"log", "--oneline", (targetBranch + ".." + sourceBranch)});
    if (list.exitCode != 0) {
        state.note = "failed to enumerate source commits";
        return state;
    }

    std::istringstream iss(list.stdoutStr);
    std::string line;
    while (std::getline(iss, line)) {
        line = Trim(line);
        if (line.empty()) {
            continue;
        }
        const auto sp = line.find(' ');
        if (sp == std::string::npos) {
            continue;
        }

        CherryPickCandidate c;
        c.sha = line.substr(0, sp);
        c.title = line.substr(sp + 1);
        c.alreadyInTarget = HasPatchEquivalentInTarget(repo, targetBranch, c.sha);

        const auto files = CommitFileSet(repo, c.sha);
        if (files.empty()) {
            c.risk = "unknown";
        } else if (files.size() >= 12) {
            c.risk = "high";
        } else if (files.size() >= 5) {
            c.risk = "medium";
        } else {
            c.risk = "low";
        }

        state.commits.push_back(std::move(c));
    }

    if (state.commits.empty()) {
        state.note = "no candidate commits (source may be fully merged)";
    } else {
        state.note = "candidate commits loaded: " + std::to_string(state.commits.size());
    }
    return state;
}

auto FirstNonEmptyLine(const std::string& text) -> std::string {
    std::istringstream iss(text);
    std::string line;
    while (std::getline(iss, line)) {
        line = Trim(line);
        if (!line.empty()) {
            return line;
        }
    }
    return "";
}

auto CollectRebasePreflight(const std::filesystem::path& repo) -> RebasePreflightState {
    RebasePreflightState state;
    state.active = true;
    state.repo = repo;
    state.branch = CurrentBranch(repo);
    state.upstream = CurrentUpstream(repo);
    state.tracking = TrackingSummary(repo);

    std::string baseRef;
    if (state.upstream != "(none)") {
        baseRef = state.upstream;
    } else {
        const auto mbMain = GitCapture(repo, {"merge-base", "HEAD", "main"});
        if (mbMain.exitCode == 0) {
            baseRef = "main";
        } else {
            const auto mbMaster = GitCapture(repo, {"merge-base", "HEAD", "master"});
            if (mbMaster.exitCode == 0) {
                baseRef = "master";
            }
        }
    }

    if (baseRef.empty()) {
        state.note = "no upstream/main/master base found";
        state.risk = "high";
        return state;
    }

    const auto mergeBaseOut = GitCapture(repo, {"merge-base", "HEAD", baseRef});
    state.mergeBase = FirstNonEmptyLine(mergeBaseOut.stdoutStr);

    const auto logOut = GitCapture(repo, {"log", "--oneline", (baseRef + "..HEAD")});
    if (logOut.exitCode != 0) {
        state.note = "failed to enumerate rebase candidate commits";
        state.risk = "high";
        return state;
    }

    std::istringstream iss(logOut.stdoutStr);
    std::string line;
    while (std::getline(iss, line)) {
        line = Trim(line);
        if (!line.empty()) {
            state.candidates.push_back(line);
        }
    }

    if (state.branch == "main" || state.branch == "master") {
        state.risk = "high";
        state.note = "on protected-like branch; avoid rebase here";
    } else if (state.upstream == "(none)" || state.tracking == "no-upstream") {
        state.risk = "medium";
        state.note = "no upstream tracking branch";
    } else if (state.tracking.find("behind") != std::string::npos && state.tracking.find("ahead") != std::string::npos) {
        state.risk = "high";
        state.note = "diverged with upstream";
    } else if (state.candidates.empty()) {
        state.risk = "low";
        state.note = "no local commits to rebase";
    } else {
        state.risk = "low";
        state.note = "rebase candidates loaded: " + std::to_string(state.candidates.size());
    }

    return state;
}

auto BuildRebasePlanPreview(const RebasePlannerState& planner) -> std::string {
    std::ostringstream out;
    out << "Rebase Plan Preview\n";
    out << "repo: " << planner.repo.lexically_normal().generic_string() << "\n";
    out << "base: " << planner.baseRef << "\n\n";
    out << "# interactive todo style\n";
    for (const auto& item : planner.items) {
        out << item.action << " " << item.sha << " " << item.title << "\n";
    }
    if (planner.items.empty()) {
        out << "(no plan items)\n";
    }
    return out.str();
}

auto BuildRebasePlanner(const std::filesystem::path& repo, const RebasePreflightState& pre) -> RebasePlannerState {
    RebasePlannerState planner;
    planner.active = true;
    planner.repo = repo;
    planner.baseRef = pre.upstream != "(none)" ? pre.upstream : (pre.branch == "main" ? "main" : "master");
    planner.selectedIndex = 0;

    for (const auto& line : pre.candidates) {
        const auto sp = line.find(' ');
        if (sp == std::string::npos) {
            continue;
        }
        RebasePlanItem item;
        item.sha = line.substr(0, sp);
        item.title = line.substr(sp + 1);
        item.action = "pick";
        planner.items.push_back(std::move(item));
    }

    planner.preview = BuildRebasePlanPreview(planner);
    return planner;
}

auto RunRebaseContinue(const std::filesystem::path& repo) -> shell::ExecResult {
    return GitCapture(repo, {"rebase", "--continue"});
}

auto RunRebaseSkip(const std::filesystem::path& repo) -> shell::ExecResult {
    return GitCapture(repo, {"rebase", "--skip"});
}

auto RunRebaseAbort(const std::filesystem::path& repo) -> shell::ExecResult {
    return GitCapture(repo, {"rebase", "--abort"});
}

auto RunRebaseStep(const std::filesystem::path& repo, const RebasePlanItem& item) -> shell::ExecResult {
    if (item.action == "drop") {
        return GitCapture(repo, {"rebase", "--skip"});
    }

    // For planner v1, execute as cherry-pick equivalent sequence for pick/squash/fixup.
    // squash/fixup will be represented as cherry-pick then follow-up continue flow when needed.
    // This keeps runner deterministic while full interactive todo execution is added later.
    return GitCapture(repo, {"cherry-pick", item.sha});
}

auto RunCherryPickOne(const std::filesystem::path& repo, const std::string& sha) -> shell::ExecResult {
    return GitCapture(repo, {"cherry-pick", sha});
}

auto CherryPickContinue(const std::filesystem::path& repo) -> shell::ExecResult {
    return GitCapture(repo, {"cherry-pick", "--continue"});
}

auto CherryPickSkip(const std::filesystem::path& repo) -> shell::ExecResult {
    return GitCapture(repo, {"cherry-pick", "--skip"});
}

auto CherryPickAbort(const std::filesystem::path& repo) -> shell::ExecResult {
    return GitCapture(repo, {"cherry-pick", "--abort"});
}

auto BuildDisplayedRepoIndices(const std::vector<RepoView>& repos,
                              const std::unordered_map<std::string, bool>& collapsedRoots) -> std::vector<int> {
    std::vector<int> indices;
    indices.reserve(repos.size());

    for (std::size_t i = 0; i < repos.size(); ++i) {
        const auto& row = repos[i];
        bool hidden = false;
        std::string cursor = row.parentRepo;
        while (cursor != "(none)") {
            if (auto it = collapsedRoots.find(cursor); it != collapsedRoots.end() && it->second) {
                hidden = true;
                break;
            }
            std::string next = "(none)";
            for (const auto& parent : repos) {
                if (CanonicalPathString(parent.path) == cursor) {
                    next = parent.parentRepo;
                    break;
                }
            }
            cursor = next;
        }
        if (!hidden) {
            indices.push_back(static_cast<int>(i));
        }
    }
    return indices;
}

auto RepoIndexFromDisplayed(const std::vector<int>& displayed, int displayedIndex) -> int {
    if (displayed.empty()) {
        return -1;
    }
    displayedIndex = std::clamp(displayedIndex, 0, static_cast<int>(displayed.size()) - 1);
    return displayed[displayedIndex];
}

auto BuildDiscoverLines(const std::vector<RepoView>& repos, const std::filesystem::path& InWorkspaceRoot) -> std::vector<std::string> {
    std::vector<std::string> lines;
    lines.reserve(repos.size() + 8);

    std::size_t dirtyRepoCount = 0;
    std::size_t worktreeDirtyCount = 0;
    for (const auto& repo : repos) {
        if (repo.repoDirty) {
            dirtyRepoCount += 1;
        }
        if (repo.worktreeDirty) {
            worktreeDirtyCount += 1;
        }
    }

    lines.push_back("discover summary");
    lines.push_back("  total repos: " + std::to_string(repos.size()));
    lines.push_back("  dirty repos: " + std::to_string(dirtyRepoCount));
    lines.push_back("  dirty worktrees: " + std::to_string(worktreeDirtyCount));
    lines.push_back("");
    lines.push_back("# | branch | tracking | dirty | path");

    for (std::size_t i = 0; i < repos.size(); ++i) {
        const auto& r = repos[i];
        std::string path = DisplayRepoPath(InWorkspaceRoot, r.path);
        if (path.size() > 90) {
            path = "..." + path.substr(path.size() - 87);
        }
        std::string tracking = r.tracking;
        if (tracking.size() > 20) {
            tracking = tracking.substr(0, 20) + "...";
        }
        std::string branch = r.branch;
        if (branch.size() > 20) {
            branch = branch.substr(0, 20) + "...";
        }
        const std::string dirty = r.repoDirty ? "yes" : "no";
        lines.push_back(std::to_string(i + 1) + " | " + branch + " | " + tracking + " | " + dirty + " | " + path);
    }

    if (repos.empty()) {
        lines.push_back("(no repositories found)");
    }

    return lines;
}

auto RunFtxuiDashboard(CLI::App& app) -> int {
    using namespace ftxui;

#ifdef KOG_PLATFORM_WINDOWS
    // Ensure the Windows console interprets subprocess output as UTF-8.
    // Without this, non-ASCII characters (bullets, CJK, etc.) captured from
    // child processes via CreateProcessA + pipes render as mojibake.
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
#endif

    bool dirtyOnly = false;
    bool filterMode = false;
    const auto workspaceRoot = std::filesystem::current_path().lexically_normal();
    std::string repoFilter;
    std::vector<RepoView> repos;
    std::unordered_map<std::string, bool> collapsedRoots;
    std::vector<int> displayedRepoIndices;
    std::vector<std::string> menu;
    int selectedDisplayed = 0;
    std::string footer = "startup: cached repo snapshot loaded | r refresh repo | :refresh all | :discover paged output | Enter history | q quit";
    bool footerIsError = false;
    HistoryState history{};
    PreviewPanelState preview{};
    ConfirmState confirm{};
    CommandScopeMode commandScope = CommandScopeMode::Workspace;
    CherryPickPreflightState cherry{};
    CherryPickRunnerState cherryRun{};
    RebasePreflightState rebase{};
    RebasePlannerState rebasePlanner{};
    RebaseRunnerState rebaseRun{};
    DiscoverPagerState discover{};
    std::unordered_map<std::string, RepoHistoryCache> historyCache;
    AsyncWorkState asyncState{};
    std::mutex asyncMu;
    std::thread asyncWorker;

    auto reportStartupProgress = [](const std::string& InMessage) {
        std::cerr << "[tui] " << InMessage << std::endl;
    };

    const Decorator kTitleStyle = [](Element element) { return element | bold | color(Color::RGB(235, 235, 235)); };
    const Decorator kSectionTitleStyle = [](Element element) { return element | bold | color(Color::RGB(255, 215, 120)); };
    const Decorator kInfoStyle = [](Element element) { return element | color(Color::RGB(140, 210, 255)); };
    const Decorator kSecondaryStyle = [](Element element) { return element | color(Color::RGB(210, 210, 210)); };
    const Decorator kMutedStyle = [](Element element) { return element | color(Color::RGB(180, 180, 180)); };
    const Decorator kSuccessStyle = [](Element element) { return element | color(Color::RGB(120, 230, 140)); };
    const Decorator kWarningStyle = [](Element element) { return element | color(Color::RGB(255, 210, 80)); };
    const Decorator kErrorStyle = [](Element element) { return element | color(Color::RGB(255, 110, 110)); };
    const Decorator kRunningStyle = [](Element element) { return element | color(Color::RGB(120, 220, 255)); };
    const Decorator kSelectedStyle = [](Element element) {
        return element | bold | color(Color::White) | bgcolor(Color::RGB(50, 90, 160));
    };
    const Decorator kHighlightStyle = [](Element element) { return element | bold | color(Color::RGB(120, 220, 255)); };
    const Decorator kPlainStyle = [](Element element) { return element; };

    enum class StatusTone {
        None,
        Running,
        Error,
        Warning,
        Success,
        Info,
    };

    auto has_any_token = [](const std::string& text, const std::initializer_list<std::string_view> tokens) {
        for (const auto token : tokens) {
            if (text.find(token) != std::string::npos) {
                return true;
            }
        }
        return false;
    };

    auto classify_line_tone = [&](const std::string& line) -> StatusTone {
        const auto lower = ToLowerAscii(line);
        if (lower.empty()) {
            return StatusTone::None;
        }
        if (has_any_token(lower, {"running", "loading", "starting", "working", "background operation", "in progress"})) {
            return StatusTone::Running;
        }
        if (has_any_token(lower, {"error", "failed", "fatal", "abort"})) {
            return StatusTone::Error;
        }
        if (has_any_token(lower, {"warning", "risk", "dirty", "conflict", "(uninit)"})) {
            return StatusTone::Warning;
        }
        if (has_any_token(lower, {"ready", "loaded", "opened", "startup", "success", "in-sync", "finished", "complete", " ok"})) {
            return StatusTone::Success;
        }
        if (has_any_token(lower, {"repo:", "branch:", "page ", "entry ", "search:", "detail:", "sort:", "command:", "state:", "progress:", "current:", "source:", "target:", "path:", "parent:", "children:", "type:", "upstream:", "tracking:", "worktrees:", "background:", "merge-base:", "base:", "note:", "mode:", "section ", "candidate commits"})) {
            return StatusTone::Info;
        }
        return StatusTone::None;
    };

    auto tone_style = [&](const StatusTone tone) -> Decorator {
        switch (tone) {
            case StatusTone::Running:
                return kRunningStyle;
            case StatusTone::Error:
                return kErrorStyle;
            case StatusTone::Warning:
                return kWarningStyle;
            case StatusTone::Success:
                return kSuccessStyle;
            case StatusTone::Info:
                return kInfoStyle;
            case StatusTone::None:
            default:
                return kPlainStyle;
        }
    };

    const bool useAsciiStatusIcons = [&]() {
        if (const char* value = std::getenv("KOG_TUI_ASCII_STATUS_ICONS"); value != nullptr) {
            const auto normalized = ToLowerAscii(Trim(value));
            return normalized == "1" || normalized == "true" || normalized == "yes" || normalized == "on";
        }
        return false;
    }();

    const bool debugArrowInput = [&]() {
        if (const char* value = std::getenv("KOG_TUI_DEBUG_ARROW_INPUT"); value != nullptr) {
            const auto normalized = ToLowerAscii(Trim(value));
            return normalized == "1" || normalized == "true" || normalized == "yes" || normalized == "on";
        }
        return false;
    }();

    const bool debugEventLog = [&]() {
        if (const char* value = std::getenv("KOG_TUI_DEBUG_EVENT_LOG"); value != nullptr) {
            const auto normalized = ToLowerAscii(Trim(value));
            return normalized == "1" || normalized == "true" || normalized == "yes" || normalized == "on";
        }
        return false;
    }();

    const auto debugEventLogPath = workspaceRoot / ".kano" / "tmp" / "git" / "tui-event-debug.log";

    auto compact_status_icon = [&](const StatusTone tone) -> std::string {
        if (useAsciiStatusIcons) {
            switch (tone) {
                case StatusTone::Running:
                    return "..";
                case StatusTone::Error:
                    return "!!";
                case StatusTone::Warning:
                    return "! ";
                case StatusTone::Success:
                    return "OK";
                case StatusTone::Info:
                    return "o ";
                case StatusTone::None:
                default:
                    return std::string();
            }
        }
        switch (tone) {
            case StatusTone::Running:
                return "◌";
            case StatusTone::Error:
                return "✕";
            case StatusTone::Warning:
                return "▲";
            case StatusTone::Success:
                return "✓";
            case StatusTone::Info:
                return "○";
            case StatusTone::None:
            default:
                return std::string();
        }
    };

    auto classify_line_style = [&](const std::string& line) -> Decorator {
        return tone_style(classify_line_tone(line));
    };

    auto status_line = [&](const std::string& line, const std::optional<StatusTone> forcedTone = std::nullopt) {
        const auto tone = forcedTone.value_or(classify_line_tone(line));
        const auto icon = compact_status_icon(tone);
        if (icon.empty()) {
            return text(line) | tone_style(tone);
        }
        return hbox({
            text(icon + std::string(" ")) | tone_style(tone),
            text(line) | tone_style(tone),
        });
    };

    auto status_text = [&](const std::string& line) {
        return status_line(line);
    };

    auto status_paragraph = [&](const std::string& line) {
        return paragraph(line) | classify_line_style(line);
    };

    auto status_title = [&](const std::string& title, const StatusTone tone = StatusTone::Info) {
        const auto icon = compact_status_icon(tone);
        if (icon.empty()) {
            return text(title) | kSectionTitleStyle;
        }
        return hbox({
            text(icon + std::string(" ")) | tone_style(tone) | bold,
            text(title) | kSectionTitleStyle,
        });
    };

    reportStartupProgress("starting dashboard initialization");
    reportStartupProgress("loading workspace repositories");
    repos = DiscoverRepoViews(dirtyOnly, false, false, [&](const std::string& InMessage) {
        reportStartupProgress(InMessage);
    });
    reportStartupProgress("loaded " + std::to_string(repos.size()) + " repositories");
    
    // Initialize TUI command input system
    kano::git::commands::TuiState tui_state;
    try {
        reportStartupProgress("initializing command input system");
        auto metadata_cache = std::make_shared<kano::git::commands::MetadataCache>(app);
        auto autocomplete_engine = std::make_shared<kano::git::commands::AutocompleteEngine>(metadata_cache);
        auto command_executor = std::make_shared<kano::git::commands::CommandExecutor>(app, std::filesystem::current_path());
        tui_state.autocomplete_engine = autocomplete_engine;
        tui_state.command_executor = command_executor;
        reportStartupProgress("command input system ready");
    } catch (const std::exception& e) {
        std::cerr << "Failed to initialize TUI command input system: " << e.what() << std::endl;
        std::cerr << "Autocomplete will be disabled. TUI will continue with basic functionality." << std::endl;
        // Continue without autocomplete - tui_state.autocomplete_engine remains nullptr
    }

    auto refresh_menu = [&] {
        const auto candidateIndices = BuildDisplayedRepoIndices(repos, collapsedRoots);
        displayedRepoIndices.clear();
        menu.clear();
        menu.reserve(candidateIndices.size());
        displayedRepoIndices.reserve(candidateIndices.size());
        for (const auto idx : candidateIndices) {
            const auto& repo = repos[idx];
            const auto normalized = CanonicalPathString(repo.path);
            const auto displayPath = DisplayRepoPath(workspaceRoot, repo.path);
            const auto filterNeedle = ToLowerAscii(repoFilter);
            if (!repoFilter.empty() && ToLowerAscii(normalized).find(filterNeedle) == std::string::npos && ToLowerAscii(displayPath).find(filterNeedle) == std::string::npos) {
                continue;
            }
            displayedRepoIndices.push_back(idx);
            const auto& path = displayPath;
            std::string indent(static_cast<std::size_t>(repo.treeDepth * 2), ' ');
            const bool collapsed = collapsedRoots[CanonicalPathString(repo.path)];
            const std::string marker = repo.childRepoCount > 0 ? (collapsed ? "[+] " : "[-] ") : "    ";
            if (repo.type == "registered-uninit") {
                menu.push_back(indent + marker + "  " + UninitializedRepoListLabel(workspaceRoot, repo) + " | " + path);
            } else {
                menu.push_back(indent + marker + (repo.repoDirty ? "* " : "  ") + repo.branch + " | " + path);
            }
        }
        if (menu.empty()) {
            menu.push_back("(no repositories found)");
            selectedDisplayed = 0;
        } else if (selectedDisplayed < 0) {
            selectedDisplayed = 0;
        } else if (selectedDisplayed >= static_cast<int>(menu.size())) {
            selectedDisplayed = static_cast<int>(menu.size()) - 1;
        }
    };

    auto refresh_all = [&] {
        repos = DiscoverRepoViews(dirtyOnly, false, true);
        refresh_menu();
        historyCache.clear();
    };

    auto history_page_slice = [&](const RepoHistoryCache& cache, const int PageIndex) -> std::vector<RepoHistoryCache::HistoryEntry> {
        if (cache.allEntries.empty() || PageIndex < 0) {
            return {};
        }
        const int pageSize = ComputeHistoryPageSize();
        const int start = PageIndex * pageSize;
        const int end = std::min(start + pageSize, static_cast<int>(cache.allEntries.size()));
        if (start >= end || start < 0) {
            return {};
        }
        return {cache.allEntries.begin() + start, cache.allEntries.begin() + end};
    };

    refresh_menu();

    auto ensure_history_loaded = [&](const int RepoIndex, const int PageIndex) {
        if (RepoIndex < 0 || RepoIndex >= static_cast<int>(repos.size()) || PageIndex < 0) {
            return;
        }
        const auto key = CanonicalPathString(repos[RepoIndex].path);
        auto& cache = historyCache[key];
        if (!cache.fullyLoaded) {
            cache.allEntries = FetchAllHistory(repos[RepoIndex]);
            cache.totalCommits = FetchTotalCommitCount(repos[RepoIndex].path);
            cache.fullyLoaded = true;
        }
    };

    auto current_history_entries = [&]() -> std::vector<RepoHistoryCache::HistoryEntry> {
        if (!history.active || repos.empty() || history.repoIndex < 0 || history.repoIndex >= static_cast<int>(repos.size())) {
            return {};
        }
        ensure_history_loaded(history.repoIndex, history.pageIndex);
        const auto key = CanonicalPathString(repos[history.repoIndex].path);
        auto itCache = historyCache.find(key);
        if (itCache == historyCache.end()) {
            return {};
        }
        return history_page_slice(itCache->second, history.pageIndex);
    };

    auto current_history_lines = [&]() -> std::vector<std::string> {
        std::vector<std::string> lines;
        for (const auto& entry : BuildDisplayedHistoryEntries(current_history_entries(), history)) {
            lines.push_back(BuildHistoryDisplayLine(entry, !entry.authorEmail.empty() ? entry.authorEmail : entry.authorName));
        }
        return lines;
    };

    auto selected_repo_index = [&]() -> int {
        return RepoIndexFromDisplayed(displayedRepoIndices, selectedDisplayed);
    };

    auto selected_repo_path = [&]() -> std::filesystem::path {
        const int idx = selected_repo_index();
        if (idx >= 0 && idx < static_cast<int>(repos.size())) {
            return repos[idx].path;
        }
        return workspaceRoot;
    };

    auto selected_repo_display = [&]() -> std::string {
        const int idx = selected_repo_index();
        if (idx >= 0 && idx < static_cast<int>(repos.size())) {
            return DisplayRepoPath(workspaceRoot, repos[idx].path);
        }
        return "(none)";
    };

    auto selected_repo_key = [&]() -> std::string {
        const int idx = selected_repo_index();
        if (idx >= 0 && idx < static_cast<int>(repos.size())) {
            return CanonicalPathString(repos[idx].path);
        }
        return {};
    };

    auto command_scope_label = [&]() -> std::string {
        if (commandScope == CommandScopeMode::SelectedRepo) {
            return "selected: " + selected_repo_display();
        }
        return "workspace: " + workspaceRoot.generic_string();
    };

    auto command_scope_repo = [&]() -> std::filesystem::path {
        return commandScope == CommandScopeMode::SelectedRepo ? selected_repo_path() : workspaceRoot;
    };

    auto parse_command_line = [](const std::string& input) -> std::vector<std::string> {
        std::vector<std::string> tokens;
        std::string currentToken;
        bool inQuotes = false;
        bool escapeNext = false;
        for (char ch : input) {
            if (escapeNext) {
                currentToken += ch;
                escapeNext = false;
                continue;
            }
            if (ch == '\\') {
                escapeNext = true;
                continue;
            }
            if (ch == '"' || ch == '\'') {
                inQuotes = !inQuotes;
                continue;
            }
            if (std::isspace(static_cast<unsigned char>(ch)) && !inQuotes) {
                if (!currentToken.empty()) {
                    tokens.push_back(currentToken);
                    currentToken.clear();
                }
                continue;
            }
            currentToken += ch;
        }
        if (!currentToken.empty()) {
            tokens.push_back(currentToken);
        }
        return tokens;
    };

    auto command_supports_target_scope = [](const std::string& command) {
        return command == "commit" || command == "ca" || command == "commit-push" || command == "cp" || command == "cpa" || command == "push" || command == "log" || command == "slog" || command == "amend";
    };

    auto has_option = [](const std::vector<std::string>& args, const std::string& longOpt, const std::string& shortOpt = std::string()) {
        for (const auto& arg : args) {
            if (arg == longOpt || (!shortOpt.empty() && arg == shortOpt)) {
                return true;
            }
            if (arg.rfind(longOpt + "=", 0) == 0) {
                return true;
            }
        }
        return false;
    };

    std::function<bool(const std::filesystem::path&, const std::vector<std::string>&, const std::string&, const std::string&)> begin_async_cli_command;

    auto build_scoped_command_args = [&](const std::string& commandLine) -> std::optional<std::pair<std::filesystem::path, std::vector<std::string>>> {
        auto tokens = parse_command_line(commandLine);
        if (tokens.empty()) {
            return std::nullopt;
        }

        std::vector<std::string> commandArgs = tokens;
        const auto& command = tokens.front();
        if (commandScope == CommandScopeMode::SelectedRepo && command_supports_target_scope(command)) {
            if (!has_option(tokens, "--repos") && !has_option(tokens, "--repo") && !has_option(tokens, "--repo-root") && !has_option(tokens, "--root") && !has_option(tokens, "--source") && !has_option(tokens, "--target")) {
                commandArgs.push_back("--repo-root");
                commandArgs.push_back(workspaceRoot.generic_string());
                commandArgs.push_back(selected_repo_display());
            }
        }
        return std::make_pair(command_scope_repo(), commandArgs);
    };

    auto screen = ScreenInteractive::Fullscreen();
    screen.TrackMouse(false);
    std::function<void()> process_async_state;

    auto request_async_ui_tick = [&]() {
        screen.Post(ftxui::Closure([&]() {
            process_async_state();
            screen.RequestAnimationFrame();
        }));
    };

    auto finish_async_operation = [&]() {
        if (asyncWorker.joinable()) {
            asyncWorker.join();
        }
    };

    auto begin_async_operation = [&](const std::string& InLabel,
                                     const std::function<void()>& InWorkerBody) -> bool {
        {
            std::lock_guard<std::mutex> lock(asyncMu);
            if (asyncState.busy) {
                footer = asyncState.label.empty() ? "background operation already running" : (asyncState.label + " already running");
                return false;
            }
            asyncState = AsyncWorkState{};
            asyncState.busy = true;
            asyncState.label = InLabel;
            asyncState.progress = InLabel + "...";
        }
        if (asyncWorker.joinable()) {
            asyncWorker.join();
        }
        asyncWorker = std::thread([&, InWorkerBody]() {
            InWorkerBody();
            request_async_ui_tick();
        });
        return true;
    };

    begin_async_cli_command = [&](const std::filesystem::path& InRepo,
                                  const std::vector<std::string>& InArgs,
                                  const std::string& InCommandText,
                                  const std::string& InLabel) {
        const auto repoKey = CanonicalPathString(InRepo);
        const auto repoDisplay = DisplayRepoPath(workspaceRoot, InRepo);
        preview.active = true;
        preview.running = true;
        preview.isError = false;
        preview.autoCloseAfterRefresh = false;
        preview.title = InLabel;
        preview.body = "state: running\nrepo: " + InRepo.lexically_normal().generic_string() + "\nscope: " + command_scope_label() + "\ncommand: " + InCommandText + "\n\n(waiting for command output...)";
        if (!begin_async_operation(InLabel, [&, InRepo, InArgs, InCommandText, InLabel, repoKey, repoDisplay]() {
                const auto result = shell::ExecuteCommand(ResolveKanoGitBinaryCommand(), InArgs, shell::ExecMode::Capture, InRepo);
                std::string body = "repo: " + InRepo.lexically_normal().generic_string() + "\n"
                    + "scope: " + command_scope_label() + "\n"
                    + "command: " + InCommandText + "\n"
                    + "exit: " + std::to_string(result.exitCode) + "\n\n";
                body += !result.stdoutStr.empty() ? result.stdoutStr : result.stderrStr;
                std::optional<RepoView> refreshedRow;
                if (result.exitCode == 0) {
                    auto it = std::find_if(repos.begin(), repos.end(), [&](const RepoView& row) {
                        return CanonicalPathString(row.path) == repoKey;
                    });
                    if (it != repos.end()) {
                        refreshedRow = BuildLiveRepoView(workspaceRoot, *it);
                    }
                }
                std::lock_guard<std::mutex> lock(asyncMu);
                asyncState.showPreview = true;
                asyncState.previewTitle = InLabel + " result";
                asyncState.previewBody = std::move(body);
                asyncState.hasResult = true;
                asyncState.completionFooter = result.exitCode == 0 ? "command finished" : "command failed";
                if (refreshedRow.has_value()) {
                    asyncState.refreshSelectedRepo = true;
                    asyncState.refreshedRepo = std::move(*refreshedRow);
                    asyncState.refreshedRepoKey = repoKey;
                    asyncState.completionFooter = "repo refreshed: " + repoDisplay;
                }
                if (result.exitCode != 0) {
                    asyncState.hasError = true;
                    asyncState.errorMessage = "command failed";
                }
            })) {
            preview.active = false;
            preview.running = false;
            return false;
        }
        footer = InLabel + " started in background";
        footerIsError = false;
        return true;
    };

    auto begin_async_refresh = [&](const bool InRefreshDiscoverPanel) {
        const bool discoverWasActive = discover.active;
        const bool discoverDirtyOnly = discover.dirtyOnly;
        const std::string discoverTitle = discover.title.empty()
            ? (discoverDirtyOnly ? "discover (dirty-only)" : "discover")
            : discover.title;
        if (!begin_async_operation("refresh", [&, discoverWasActive, discoverDirtyOnly, discoverTitle, InRefreshDiscoverPanel]() {
                auto progressCallback = [&](const std::string& InMessage) {
                    {
                        std::lock_guard<std::mutex> lock(asyncMu);
                        asyncState.progress = InMessage;
                    }
                    request_async_ui_tick();
                };
                try {
                    const auto refreshedRepos = DiscoverRepoViews(dirtyOnly, false, true, progressCallback);
                    std::vector<std::string> discoverLines;
                    if (InRefreshDiscoverPanel && discoverWasActive) {
                        progressCallback("discover: rebuilding paged output");
                        const auto discoveredRepos = DiscoverRepoViews(discoverDirtyOnly, false, true, progressCallback);
                        discoverLines = BuildDiscoverLines(discoveredRepos, workspaceRoot);
                    }
                    std::lock_guard<std::mutex> lock(asyncMu);
                    asyncState.repos = std::move(refreshedRepos);
                    asyncState.discoverLines = std::move(discoverLines);
                    asyncState.discoverTitle = discoverTitle;
                    asyncState.refreshRepos = true;
                    asyncState.refreshDiscover = InRefreshDiscoverPanel && discoverWasActive;
                    asyncState.hasResult = true;
                    asyncState.completionFooter = "live status refreshed";
                } catch (const std::exception& e) {
                    std::lock_guard<std::mutex> lock(asyncMu);
                    asyncState.hasError = true;
                    asyncState.errorMessage = std::string("refresh failed: ") + e.what();
                }
            })) {
            return;
        }
        footer = "refresh started in background";
        footerIsError = false;
    };

    auto begin_async_selected_repo_refresh = [&]() {
        const int selected = RepoIndexFromDisplayed(displayedRepoIndices, selectedDisplayed);
        if (selected < 0 || selected >= static_cast<int>(repos.size())) {
            footer = "repo refresh skipped: no selected repo";
            footerIsError = true;
            return;
        }

        const auto repoKey = CanonicalPathString(repos[selected].path);
        const auto repoDisplay = DisplayRepoPath(workspaceRoot, repos[selected].path);
        const auto currentRow = repos[selected];

        if (!begin_async_operation("repo refresh", [&, repoKey, repoDisplay, currentRow]() {
                auto progressCallback = [&](const std::string& InMessage) {
                    {
                        std::lock_guard<std::mutex> lock(asyncMu);
                        asyncState.progress = InMessage;
                    }
                    request_async_ui_tick();
                };

                try {
                    progressCallback("refreshing repo: " + repoDisplay);
                    auto refreshedRow = BuildLiveRepoView(workspaceRoot, currentRow);
                    std::lock_guard<std::mutex> lock(asyncMu);
                    asyncState.refreshSelectedRepo = true;
                    asyncState.refreshedRepo = std::move(refreshedRow);
                    asyncState.refreshedRepoKey = repoKey;
                    asyncState.hasResult = true;
                    asyncState.completionFooter = "repo refreshed: " + repoDisplay;
                } catch (const std::exception& e) {
                    std::lock_guard<std::mutex> lock(asyncMu);
                    asyncState.hasError = true;
                    asyncState.errorMessage = std::string("repo refresh failed: ") + e.what();
                }
            })) {
            footer = "repo refresh already running";
            footerIsError = false;
            return;
        }

        footer = "repo refresh started: " + repoDisplay;
        footerIsError = false;
    };

    auto begin_async_discover = [&](const bool InDirtyOnly) {
        if (!begin_async_operation("discover", [&, InDirtyOnly]() {
                auto progressCallback = [&](const std::string& InMessage) {
                    {
                        std::lock_guard<std::mutex> lock(asyncMu);
                        asyncState.progress = InMessage;
                    }
                    request_async_ui_tick();
                };
                try {
                    const auto discoveredRepos = DiscoverRepoViews(InDirtyOnly, true, true, progressCallback);
                    std::lock_guard<std::mutex> lock(asyncMu);
                    asyncState.discoverLines = BuildDiscoverLines(discoveredRepos, workspaceRoot);
                    asyncState.discoverTitle = InDirtyOnly ? "discover (dirty-only)" : "discover";
                    asyncState.refreshDiscover = true;
                    asyncState.hasResult = true;
                    asyncState.completionFooter = "discover loaded: PgUp/PgDn page, [ prev, ] next, Esc/q close";
                } catch (const std::exception& e) {
                    std::lock_guard<std::mutex> lock(asyncMu);
                    asyncState.hasError = true;
                    asyncState.errorMessage = std::string("discover failed: ") + e.what();
                }
            })) {
            return;
        }
        discover.active = true;
        discover.loading = true;
        discover.dirtyOnly = InDirtyOnly;
        discover.pageIndex = 0;
        discover.title = InDirtyOnly ? "discover (dirty-only)" : "discover";
        discover.lines = {"(discover running in background...)"};
        discover.progress = "starting discover...";
        footer = "discover started in background";
        footerIsError = false;
    };

    process_async_state = [&]() {
        bool shouldRefreshMenu = false;
        bool shouldFinishWorker = false;
        std::string nextFooter;
        std::string selectedRepoKeyBeforeApply;
        {
            std::lock_guard<std::mutex> lock(asyncMu);
            if (asyncState.busy) {
                if (!asyncState.hasResult && !asyncState.hasError && asyncState.showPreview) {
                    preview.body = asyncState.previewBody;
                    asyncState.showPreview = false;
                }
                if (asyncState.hasResult) {
                    selectedRepoKeyBeforeApply = selected_repo_key();
                    if (asyncState.refreshRepos) {
                        repos = std::move(asyncState.repos);
                        historyCache.clear();
                        shouldRefreshMenu = true;
                    }
                    if (asyncState.refreshSelectedRepo) {
                        const auto repoKey = asyncState.refreshedRepoKey;
                        auto it = std::find_if(repos.begin(), repos.end(), [&](const RepoView& row) {
                            return CanonicalPathString(row.path) == repoKey;
                        });
                        if (it != repos.end()) {
                            *it = asyncState.refreshedRepo;
                            repos = FinalizeRepoTree(std::move(repos));
                            historyCache.erase(repoKey);
                            shouldRefreshMenu = true;
                        }
                    }
                    if (asyncState.refreshDiscover) {
                        discover.active = true;
                        discover.loading = false;
                        discover.title = asyncState.discoverTitle;
                        discover.lines = std::move(asyncState.discoverLines);
                        discover.progress.clear();
                        const int totalPages = std::max(1, static_cast<int>((discover.lines.size() + static_cast<std::size_t>(discover.pageSize) - 1) / static_cast<std::size_t>(discover.pageSize)));
                        discover.pageIndex = std::clamp(discover.pageIndex, 0, totalPages - 1);
                    }
                    if (asyncState.showPreview) {
                        preview.active = true;
                        preview.running = false;
                        preview.isError = asyncState.hasError;
                        preview.autoCloseAfterRefresh = asyncState.previewAutoCloseAfterRefresh;
                        preview.title = asyncState.previewTitle;
                        preview.body = asyncState.previewBody;
                    }
                    nextFooter = asyncState.completionFooter.empty() ? "background operation complete" : asyncState.completionFooter;
                    asyncState = AsyncWorkState{};
                    shouldFinishWorker = true;
                } else if (asyncState.hasError) {
                    discover.loading = false;
                    nextFooter = asyncState.errorMessage;
                    asyncState = AsyncWorkState{};
                    shouldFinishWorker = true;
                } else {
                    if (discover.loading) {
                        discover.progress = asyncState.progress;
                    } else if (!asyncState.progress.empty()) {
                        footer = asyncState.progress;
                    }
                    if (footer.empty()) {
                        footer = "r refresh repo | :refresh live status | :discover rescan repos | Enter history | q quit";
                    }
                }
            }
        }
        if (shouldRefreshMenu) {
            refresh_menu();
            const auto keyToRestore = selectedRepoKeyBeforeApply;
            bool restored = false;
            if (!keyToRestore.empty()) {
                for (std::size_t displayedIndex = 0; displayedIndex < displayedRepoIndices.size(); ++displayedIndex) {
                    const int repoIndex = displayedRepoIndices[displayedIndex];
                    if (repoIndex >= 0 && repoIndex < static_cast<int>(repos.size()) &&
                        CanonicalPathString(repos[repoIndex].path) == keyToRestore) {
                        selectedDisplayed = static_cast<int>(displayedIndex);
                        restored = true;
                        break;
                    }
                }
            }
            if (!restored) {
                if (menu.empty()) {
                    selectedDisplayed = 0;
                } else {
                    selectedDisplayed = std::clamp(selectedDisplayed, 0, static_cast<int>(menu.size()) - 1);
                }
            }
            tui_state.mode = kano::git::commands::TuiMode::Normal;
            tui_state.command_state = kano::git::commands::CommandModeState{};
            tui_state.palette_state = kano::git::commands::CommandPaletteState{};
            tui_state.help_state = kano::git::commands::HelpPanelState{};
            tui_state.confirm_state = kano::git::commands::ConfirmState{};
            tui_state.footer_message.clear();
            tui_state.footer_is_error = false;
        }
        if (!nextFooter.empty()) {
            footer = nextFooter;
            footerIsError = nextFooter.find("failed:") != std::string::npos;
        }
        if (shouldFinishWorker) {
            finish_async_operation();
            if (preview.active && !preview.running && preview.autoCloseAfterRefresh) {
                if (preview.autoCloseAfterRefresh) {
                    preview.active = false;
                    preview.autoCloseAfterRefresh = false;
                    footer = "r refresh repo | :refresh live status | :discover rescan repos | Enter history | q quit";
                    footerIsError = false;
                }
            }
        }
    };

    MenuOption repoMenuOption;
    repoMenuOption.entries_option.transform = [&](EntryState state) {
        StatusTone tone = StatusTone::Info;
        if (state.label == "(no repositories found)") {
            tone = StatusTone::None;
        } else if (state.label.find("(uninit)") != std::string::npos) {
            tone = StatusTone::Warning;
        } else if (state.label.find("* ") != std::string::npos) {
            tone = StatusTone::Warning;
        }
        auto row = compact_status_icon(tone).empty()
            ? text(state.label) | kSecondaryStyle
            : hbox({
                  text(compact_status_icon(tone) + std::string(" ")) | tone_style(tone),
                  text(state.label) | kSecondaryStyle,
              });
        if (state.label.find("(uninit)") != std::string::npos) {
            row = row | kWarningStyle;
        } else if (state.label.find("* ") != std::string::npos) {
            row = row | kInfoStyle;
        }
        if (state.active) {
            row = row | kSelectedStyle;
        }
        return row;
    };
    auto list = Menu(&menu, &selectedDisplayed, repoMenuOption);
    auto root = Container::Vertical({list});

    auto map_event_to_tui = [&](const Event& event) -> std::optional<std::pair<std::string, char>> {
        if (event == Event::Escape) return std::make_pair(std::string("escape"), '\0');
        if (event == Event::Return || event == Event::Character('\n')) return std::make_pair(std::string("enter"), '\0');
        if (event == Event::Backspace) return std::make_pair(std::string("backspace"), '\0');
        if (event == Event::Delete) return std::make_pair(std::string("delete"), '\0');
        if (event == Event::ArrowLeft) return std::make_pair(std::string("left"), '\0');
        if (event == Event::ArrowRight) return std::make_pair(std::string("right"), '\0');
        if (event == Event::ArrowUp) return std::make_pair(std::string("up"), '\0');
        if (event == Event::ArrowDown) return std::make_pair(std::string("down"), '\0');
        if (event == Event::Home) return std::make_pair(std::string("home"), '\0');
        if (event == Event::End) return std::make_pair(std::string("end"), '\0');
        if (event == Event::Tab) return std::make_pair(std::string("tab"), '\0');
        if (event.is_character()) {
            const auto text = event.character();
            if (text == "\x10") return std::make_pair(std::string("ctrl_p"), '\0');
            if (text == "\x15") return std::make_pair(std::string("ctrl_u"), '\0');
            if (!text.empty()) return std::make_pair(std::string("character"), text[0]);
        }
        return std::nullopt;
    };

    auto is_arrow_up = [&](const Event& event) {
        const auto& input = event.input();
        return event == Event::ArrowUp || input == "\x1B[A" || input == "\x1B[1A";
    };
    auto is_arrow_down = [&](const Event& event) {
        const auto& input = event.input();
        return event == Event::ArrowDown || input == "\x1B[B" || input == "\x1B[1B";
    };
    auto is_arrow_left = [&](const Event& event) {
        const auto& input = event.input();
        return event == Event::ArrowLeft || input == "\x1B[D" || input == "\x1B[1D";
    };
    auto is_arrow_right = [&](const Event& event) {
        const auto& input = event.input();
        return event == Event::ArrowRight || input == "\x1B[C" || input == "\x1B[1C";
    };

    auto append_debug_event_log = [&](const Event& event, const char* stage) {
        if (!debugEventLog) {
            return;
        }
        std::error_code ec;
        std::filesystem::create_directories(debugEventLogPath.parent_path(), ec);
        std::ofstream out(debugEventLogPath, std::ios::app | std::ios::binary);
        if (!out) {
            return;
        }
        out << "stage=" << stage
            << " mode=" << static_cast<int>(tui_state.GetMode())
            << " history=" << history.active
            << " discover=" << discover.active
            << " preview=" << preview.active
            << " confirm=" << confirm.active
            << " selectedDisplayed=" << selectedDisplayed
            << " menuSize=" << menu.size()
            << " displayedSize=" << displayedRepoIndices.size()
            << " input=";
        for (unsigned char ch : event.input()) {
            out << std::format("{:02X}", static_cast<unsigned int>(ch)) << ' ';
        }
        if (event.is_character()) {
            out << " char=" << event.character();
        }
        out << '\n';
    };

    auto with_keys = CatchEvent(root, [&](Event event) {
        append_debug_event_log(event, "catch-start");
        if (tui_state.GetMode() == kano::git::commands::TuiMode::Command &&
            (event == Event::Return || event == Event::Character('\n'))) {
            std::string commandLine = Trim(tui_state.command_state.GetBuffer());
            if (!commandLine.empty() && commandLine[0] == ':') {
                commandLine = Trim(commandLine.substr(1));
            }
            if (!commandLine.empty()) {
                auto lower = ToLowerAscii(commandLine);
                bool handledCommand = true;
                if (lower == "discover") {
                    begin_async_discover(false);
                } else if (lower == "discover dirty" || lower == "discover --dirty") {
                    begin_async_discover(true);
                } else if (lower == "refresh") {
                    begin_async_refresh(discover.active);
                } else {
                    handledCommand = false;
                }

                if (handledCommand) {
                    tui_state.mode = kano::git::commands::TuiMode::Normal;
                    tui_state.command_state = kano::git::commands::CommandModeState{};
                    tui_state.footer_message.clear();
                    tui_state.footer_is_error = false;
                    return true;
                }

                if (const auto scoped = build_scoped_command_args(commandLine); scoped.has_value()) {
                    std::string fullCommand = ResolveKanoGitBinaryCommand();
                    for (const auto& part : scoped->second) {
                        fullCommand += " " + part;
                    }
                    if (!begin_async_cli_command(scoped->first, scoped->second, fullCommand, "command runner")) {
                        footer = "command runner busy";
                        footerIsError = true;
                        return true;
                    }
                    tui_state.mode = kano::git::commands::TuiMode::Normal;
                    tui_state.command_state = kano::git::commands::CommandModeState{};
                    tui_state.footer_message.clear();
                    tui_state.footer_is_error = false;
                    return true;
                }
            }
        }

        if (tui_state.GetMode() == kano::git::commands::TuiMode::Command &&
            tui_state.command_state.GetBuffer().empty() &&
            (event == Event::Character('g') || event == Event::Character('G'))) {
            commandScope = commandScope == CommandScopeMode::Workspace ? CommandScopeMode::SelectedRepo : CommandScopeMode::Workspace;
            footer = "command scope: " + command_scope_label();
            footerIsError = false;
            return true;
        }

        if (event == Event::Custom) {
            return true;
        }

        if (auto mapped = map_event_to_tui(event); mapped.has_value()) {
            const auto prevMode = tui_state.GetMode();
            const bool handled = tui_state.HandleEvent(mapped->first, mapped->second);
            if (handled) {
                if (!tui_state.footer_message.empty()) {
                    footer = tui_state.footer_message;
                } else if (prevMode != tui_state.GetMode()) {
                    if (tui_state.GetMode() == kano::git::commands::TuiMode::Command) {
                        footer = "command mode: g toggle scope (empty input), Tab complete, Enter execute, Esc cancel";
                    } else if (tui_state.GetMode() == kano::git::commands::TuiMode::CommandPalette) {
                        footer = "command palette: type to filter, Enter select, Esc close";
                    } else if (tui_state.GetMode() == kano::git::commands::TuiMode::Help) {
                        footer = "help panel: Esc/q to close";
                    }
                }
                return true;
            }

            if (prevMode != kano::git::commands::TuiMode::Normal) {
                return true;
            }
        }

        auto close_overlay_or_exit = [&]() -> bool {
            {
                std::lock_guard<std::mutex> lock(asyncMu);
                if (asyncState.busy) {
                    footer = asyncState.label + " still running; wait for completion";
                    footerIsError = false;
                    return true;
                }
            }
            if (discover.active) {
                discover.active = false;
                discover.loading = false;
                footer = "discover panel closed";
                return true;
            }
            if (rebaseRun.active) {
                rebaseRun.active = false;
                rebaseRun.waitingConflictResolution = false;
                footer = "rebase runner closed";
                return true;
            }
            if (rebasePlanner.active) {
                rebasePlanner.active = false;
                footer = "rebase planner closed";
                return true;
            }
            if (rebase.active) {
                rebase.active = false;
                footer = "rebase preflight closed";
                return true;
            }
            if (cherryRun.active) {
                cherryRun.active = false;
                cherryRun.waitingConflictResolution = false;
                footer = "cherry-pick runner closed";
                return true;
            }
            if (cherry.active) {
                cherry.active = false;
                footer = "cherry-pick preflight closed";
                return true;
            }
            if (confirm.active) {
                confirm.active = false;
                footer = "confirm cancelled";
                return true;
            }
            if (preview.active) {
                preview.active = false;
                footer = "preview closed";
                return true;
            }
            if (filterMode) {
                filterMode = false;
                footer = "filter mode closed";
                return true;
            }
            if (history.active) {
                if (history.detailActive) {
                    history.detailActive = false;
                    footer = "history detail closed";
                    return true;
                }
                history.active = false;
                history.searchMode = false;
                footer = "history closed";
                return true;
            }
            screen.ExitLoopClosure()();
            return true;
        };

        if (event == Event::Character('q')) {
            return close_overlay_or_exit();
        }

        if (event == Event::Escape) {
            return close_overlay_or_exit();
        }

        if (discover.active && (event == Event::Character(']') || event == Event::PageUp)) {
            const int totalPages = std::max(1, static_cast<int>((discover.lines.size() + static_cast<std::size_t>(discover.pageSize) - 1) / static_cast<std::size_t>(discover.pageSize)));
            discover.pageIndex = std::min(totalPages - 1, discover.pageIndex + 1);
            footer = "discover page ->";
            return true;
        }

        if (discover.active && (event == Event::Character('[') || event == Event::PageDown)) {
            discover.pageIndex = std::max(0, discover.pageIndex - 1);
            footer = "discover page <-";
            return true;
        }

        if (filterMode) {
            if (event == Event::Escape) {
                filterMode = false;
                footer = "filter mode cancelled";
                return true;
            }
            if (event == Event::Backspace) {
                if (!repoFilter.empty()) {
                    repoFilter.pop_back();
                }
                refresh_menu();
                footer = repoFilter.empty() ? "filter cleared" : "filter: " + repoFilter;
                return true;
            }
            if (event == Event::Return || event == Event::Character('\n')) {
                filterMode = false;
                footer = repoFilter.empty() ? "filter cleared" : "filter applied";
                return true;
            }
            if (event.is_character()) {
                repoFilter += event.character();
                refresh_menu();
                footer = "filter: " + repoFilter;
                return true;
            }
            return false;
        }

        

        if (rebasePlanner.active && (event == Event::ArrowUp || event == Event::Character('k'))) {
            if (!rebasePlanner.items.empty()) {
                rebasePlanner.selectedIndex = std::max(0, rebasePlanner.selectedIndex - 1);
                footer = "rebase planner line up";
            }
            return true;
        }

        if (rebasePlanner.active && (event == Event::ArrowDown || event == Event::Character('j'))) {
            if (!rebasePlanner.items.empty()) {
                rebasePlanner.selectedIndex = std::min(static_cast<int>(rebasePlanner.items.size()) - 1, rebasePlanner.selectedIndex + 1);
                footer = "rebase planner line down";
            }
            return true;
        }

        if (rebasePlanner.active && event.is_character()) {
            if (rebasePlanner.items.empty()) {
                return true;
            }
            auto& item = rebasePlanner.items[rebasePlanner.selectedIndex];
            const auto ch = event.character();
            if (ch == "p") {
                item.action = "pick";
            } else if (ch == "s") {
                item.action = "squash";
            } else if (ch == "f") {
                item.action = "fixup";
            } else if (ch == "d") {
                item.action = "drop";
            } else {
                return false;
            }
            rebasePlanner.preview = BuildRebasePlanPreview(rebasePlanner);
            footer = "rebase action set: " + item.action;
            return true;
        }

        if (rebaseRun.active && rebaseRun.waitingConflictResolution && (event == Event::Character('C'))) {
            const auto result = RunRebaseContinue(rebaseRun.repo);
            rebaseRun.lastOutput = !result.stdoutStr.empty() ? result.stdoutStr : result.stderrStr;
            if (result.exitCode == 0) {
                rebaseRun.waitingConflictResolution = false;
                rebaseRun.index += 1;
                footer = "rebase continue ok";
            } else {
                footer = "rebase continue failed";
            }
            return true;
        }

        if (rebaseRun.active && rebaseRun.waitingConflictResolution && (event == Event::Character('S'))) {
            const auto result = RunRebaseSkip(rebaseRun.repo);
            rebaseRun.lastOutput = !result.stdoutStr.empty() ? result.stdoutStr : result.stderrStr;
            if (result.exitCode == 0) {
                rebaseRun.waitingConflictResolution = false;
                rebaseRun.index += 1;
                footer = "rebase skip ok";
            } else {
                footer = "rebase skip failed";
            }
            return true;
        }

        if (rebaseRun.active && (event == Event::Character('A'))) {
            const auto result = RunRebaseAbort(rebaseRun.repo);
            rebaseRun.lastOutput = !result.stdoutStr.empty() ? result.stdoutStr : result.stderrStr;
            rebaseRun.waitingConflictResolution = false;
            rebaseRun.active = false;
            footer = result.exitCode == 0 ? "rebase aborted" : "rebase abort failed";
            return true;
        }

        if (rebaseRun.active && !rebaseRun.waitingConflictResolution && (event == Event::Character('N'))) {
            if (rebaseRun.index >= static_cast<int>(rebaseRun.queue.size())) {
                rebaseRun.active = false;
                footer = "rebase runner complete";
                return true;
            }
            const auto result = RunRebaseStep(rebaseRun.repo, rebaseRun.queue[rebaseRun.index]);
            rebaseRun.lastOutput = !result.stdoutStr.empty() ? result.stdoutStr : result.stderrStr;
            if (result.exitCode == 0) {
                rebaseRun.index += 1;
                footer = "rebase step ok";
            } else {
                rebaseRun.waitingConflictResolution = true;
                footer = "rebase stopped: C=continue S=skip A=abort";
            }
            return true;
        }

        if (cherryRun.active && cherryRun.waitingConflictResolution && (event == Event::Character('c') || event == Event::Character('C'))) {
            const auto result = CherryPickContinue(cherryRun.repo);
            cherryRun.lastOutput = !result.stdoutStr.empty() ? result.stdoutStr : result.stderrStr;
            if (result.exitCode == 0) {
                cherryRun.waitingConflictResolution = false;
                cherryRun.index += 1;
                footer = "cherry-pick continue ok";
            } else {
                footer = "continue failed";
            }
            return true;
        }

        if (cherryRun.active && cherryRun.waitingConflictResolution && (event == Event::Character('s') || event == Event::Character('S'))) {
            const auto result = CherryPickSkip(cherryRun.repo);
            cherryRun.lastOutput = !result.stdoutStr.empty() ? result.stdoutStr : result.stderrStr;
            if (result.exitCode == 0) {
                cherryRun.waitingConflictResolution = false;
                cherryRun.index += 1;
                footer = "cherry-pick skip ok";
            } else {
                footer = "skip failed";
            }
            return true;
        }

        if (cherryRun.active && (event == Event::Character('a') || event == Event::Character('A'))) {
            const auto result = CherryPickAbort(cherryRun.repo);
            cherryRun.lastOutput = !result.stdoutStr.empty() ? result.stdoutStr : result.stderrStr;
            cherryRun.waitingConflictResolution = false;
            cherryRun.active = false;
            footer = result.exitCode == 0 ? "cherry-pick aborted" : "abort failed";
            return true;
        }

        if (cherryRun.active && !cherryRun.waitingConflictResolution && (event == Event::Character('n') || event == Event::Character('N'))) {
            if (cherryRun.index >= static_cast<int>(cherryRun.queue.size())) {
                cherryRun.active = false;
                footer = "cherry-pick runner complete";
                return true;
            }
            const auto result = RunCherryPickOne(cherryRun.repo, cherryRun.queue[cherryRun.index].sha);
            cherryRun.lastOutput = !result.stdoutStr.empty() ? result.stdoutStr : result.stderrStr;
            if (result.exitCode == 0) {
                cherryRun.index += 1;
                footer = "cherry-pick step ok";
            } else {
                cherryRun.waitingConflictResolution = true;
                footer = "cherry-pick conflict/stop: c=continue s=skip a=abort";
            }
            return true;
        }

        if (confirm.active && (event == Event::Character('y') || event == Event::Character('Y'))) {
            std::string fullCommand = ResolveKanoGitBinaryCommand();
            for (const auto& part : confirm.command) {
                fullCommand += " " + part;
            }
            confirm.active = false;
            if (!begin_async_cli_command(confirm.repo, confirm.command, fullCommand, confirm.title)) {
                footer = "command runner busy";
                footerIsError = true;
            }
            return true;
        }

        if (confirm.active && (event == Event::Character('n') || event == Event::Character('N') || event == Event::Escape)) {
            confirm.active = false;
            footer = "confirm declined";
            return true;
        }

        if (!history.active && !discover.active && !preview.active && !confirm.active &&
            tui_state.GetMode() == kano::git::commands::TuiMode::Normal) {
            if (debugArrowInput && !event.input().empty() && event.input().front() == '\x1B') {
                std::ostringstream oss;
                oss << "arrow-debug input=";
                for (unsigned char ch : event.input()) {
                    oss << std::format("{:02X}", static_cast<unsigned int>(ch));
                    oss << ' ';
                }
                footer = Trim(oss.str());
            }
            if (is_arrow_up(event) || event == Event::Character('k') || event == Event::Character('w')) {
                if (!menu.empty()) {
                    selectedDisplayed = std::max(0, selectedDisplayed - 1);
                }
                return true;
            }
            if (is_arrow_down(event) || event == Event::Character('j') || event == Event::Character('s')) {
                if (!menu.empty()) {
                    selectedDisplayed = std::min(static_cast<int>(menu.size()) - 1, selectedDisplayed + 1);
                }
                return true;
            }
            if (is_arrow_left(event) || event == Event::Character('a')) {
                const int selected = RepoIndexFromDisplayed(displayedRepoIndices, selectedDisplayed);
                if (selected >= 0 && selected < static_cast<int>(repos.size())) {
                    const auto key = CanonicalPathString(repos[selected].path);
                    if (repos[selected].childRepoCount > 0 && !collapsedRoots[key]) {
                        collapsedRoots[key] = true;
                        refresh_menu();
                        footer = "tree collapsed";
                    }
                }
                return true;
            }
            if (is_arrow_right(event) || event == Event::Character('d')) {
                const int selected = RepoIndexFromDisplayed(displayedRepoIndices, selectedDisplayed);
                if (selected >= 0 && selected < static_cast<int>(repos.size())) {
                    const auto key = CanonicalPathString(repos[selected].path);
                    if (repos[selected].childRepoCount > 0 && collapsedRoots[key]) {
                        collapsedRoots[key] = false;
                        refresh_menu();
                        footer = "tree expanded";
                    }
                }
                return true;
            }
        }

        if (!history.active && (event == Event::Return || event == Event::Character('\n'))) {
            const int selected = RepoIndexFromDisplayed(displayedRepoIndices, selectedDisplayed);
            if (repos.empty() || selected < 0 || selected >= static_cast<int>(repos.size())) {
                footer = "history skipped: no selected repo";
                return true;
            }
            if (repos[selected].type == "registered-uninit") {
                footer = "cannot view history for uninitialized submodule";
                return true;
            }
            history.active = true;
            history.repoIndex = selected;
            history.pageIndex = 0;
            history.selectedLine = 0;
            history.searchMode = false;
            history.searchQuery.clear();
            history.highlightedLine = -1;
            history.detailActive = false;
            history.detailSha.clear();
            history.detailBody.clear();
            history.detailMode = 0;
            ensure_history_loaded(history.repoIndex, history.pageIndex);
            footer = "history opened";
            return true;
        }

        if (event == Event::Character('t')) {
            const int selected = RepoIndexFromDisplayed(displayedRepoIndices, selectedDisplayed);
            if (selected < 0 || selected >= static_cast<int>(repos.size())) {
                footer = "tree toggle skipped: no selected repo";
                return true;
            }
            const auto key = CanonicalPathString(repos[selected].path);
            if (repos[selected].childRepoCount > 0) {
                collapsedRoots[key] = !collapsedRoots[key];
                refresh_menu();
                footer = collapsedRoots[key] ? "tree collapsed" : "tree expanded";
            } else {
                footer = "selected repo has no child repos";
            }
            return true;
        }

        if (event == Event::Character('r') && !history.active && !discover.active && !preview.active) {
            begin_async_selected_repo_refresh();
            return true;
        }

        // --- 'I' key: initialize uninitialized submodule ---
        if (event == Event::Character('I') && !history.active && !discover.active && !preview.active) {
            const int selected = RepoIndexFromDisplayed(displayedRepoIndices, selectedDisplayed);
            if (selected < 0 || static_cast<std::size_t>(selected) >= repos.size()) {
                footer = "submodule init skipped: no selected repo";
                return true;
            }
            const auto& row = repos[selected];
            if (row.type != "registered-uninit") {
                footer = "submodule init: only available for uninitialized repos";
                return true;
            }
            if (row.parentRepo.empty()) {
                footer = "submodule init: cannot determine parent repo";
                return true;
            }
            const auto parentPath = std::filesystem::path(row.parentRepo);
            const auto relPath = row.path.lexically_relative(parentPath).generic_string();
            const std::string commandText = "git -C \"" + parentPath.generic_string() + "\" submodule update --init --progress -- \"" + relPath + "\"";
            const std::string label = "submodule init";

            preview.active = true;
            preview.running = true;
            preview.isError = false;
            preview.autoCloseAfterRefresh = false;
            preview.title = label;
            preview.body = "state: running\nrepo: " + row.path.lexically_normal().generic_string()
                + "\nparent: " + parentPath.generic_string()
                + "\ncommand: " + commandText
                + "\n\n(waiting for command output...)";

            if (!begin_async_operation(label, [&, parentPath, relPath, commandText, label]() {
                    TerminalModeGuard terminalModeGuard;

                    // --- Phase 1: Submodule init with streaming progress ---
                    std::string streamOutput;
                    std::mutex streamMu;

                    auto progressCb = [&](std::string_view chunk, bool isStderr) {
                        (void)isStderr;
                        std::string currentOutput;
                        {
                            std::lock_guard<std::mutex> slock(streamMu);
                            streamOutput.append(chunk.data(), chunk.size());
                            currentOutput = streamOutput;
                        }
                        std::string liveBody = "repo: " + std::filesystem::path(relPath).lexically_normal().generic_string() + "\n"
                            + "parent: " + parentPath.generic_string() + "\n"
                            + "command: " + commandText + "\n"
                            + "state: running\n\n"
                            + currentOutput;
                        bool shouldPost = true;
                        {
                            std::lock_guard<std::mutex> lock(asyncMu);
                            if (asyncState.hasResult || asyncState.hasError) {
                                shouldPost = false;
                            } else {
                                asyncState.previewBody = std::move(liveBody);
                                asyncState.showPreview = true;
                            }
                        }
                        if (shouldPost) {
                            request_async_ui_tick();
                        }
                    };

                    const auto result = shell::ExecuteCommand("git",
                        {"-C", parentPath.string(), "submodule", "update", "--init", "--progress", "--", relPath},
                        shell::ExecMode::Capture, parentPath, progressCb);

                    // Build final body with both stdout and stderr
                    std::string body = "repo: " + std::filesystem::path(relPath).lexically_normal().generic_string() + "\n"
                        + "parent: " + parentPath.generic_string() + "\n"
                        + "command: " + commandText + "\n"
                        + "exit: " + std::to_string(result.exitCode) + "\n";

                    if (!result.stdoutStr.empty()) {
                        body += "\n--- stdout ---\n" + result.stdoutStr;
                    }
                    if (!result.stderrStr.empty()) {
                        body += "\n--- stderr ---\n" + result.stderrStr;
                    }

                    // Helper: post live progress to preview panel during Phase 2
                    auto postProgress = [&](const std::string& InBody) {
                        bool shouldPost = true;
                        {
                            std::lock_guard<std::mutex> lock(asyncMu);
                            if (asyncState.hasResult || asyncState.hasError) {
                                shouldPost = false;
                            } else {
                                asyncState.previewBody = InBody;
                                asyncState.showPreview = true;
                            }
                        }
                        if (shouldPost) {
                            request_async_ui_tick();
                        }
                    };

                    // --- Phase 2: Post-init branch checkout (only on success) ---
                    std::string branchLog;
                    if (result.exitCode == 0) {
                        const auto repoPath = parentPath / relPath;

                        postProgress(body + "\n--- branch ---\nresolving default branch...");

                        // Step 2a: Find submodule prefix in .gitmodules
                        std::string submodulePrefix;
                        {
                            const auto pathResult = GitCapture(parentPath, {"config", "-f", ".gitmodules", "--get-regexp", "^submodule\\..*\\.path$"});
                            if (pathResult.exitCode == 0) {
                                std::istringstream iss(pathResult.stdoutStr);
                                std::string line;
                                while (std::getline(iss, line)) {
                                    // Trim
                                    while (!line.empty() && (line.back() == '\r' || line.back() == '\n' || line.back() == ' ')) line.pop_back();
                                    while (!line.empty() && (line.front() == ' ')) line.erase(line.begin());
                                    if (line.empty()) continue;
                                    const auto sp = line.find(' ');
                                    if (sp == std::string::npos || sp + 1 >= line.size()) continue;
                                    const auto key = line.substr(0, sp);
                                    const auto value = line.substr(sp + 1);
                                    if (value == relPath && key.ends_with(".path")) {
                                        submodulePrefix = key.substr(0, key.size() - 5);  // strip ".path"
                                        break;
                                    }
                                }
                            }
                        }

                        // Step 2b: Resolve .gitmodules branch
                        std::string targetBranch;
                        bool branchFromGitmodules = false;
                        if (!submodulePrefix.empty()) {
                            const auto branchResult = GitCapture(parentPath, {"config", "-f", ".gitmodules", "--get", submodulePrefix + ".branch"});
                            if (branchResult.exitCode == 0) {
                                targetBranch = branchResult.stdoutStr;
                                while (!targetBranch.empty() && (targetBranch.back() == '\r' || targetBranch.back() == '\n' || targetBranch.back() == ' ')) targetBranch.pop_back();
                                while (!targetBranch.empty() && (targetBranch.front() == ' ')) targetBranch.erase(targetBranch.begin());
                                if (!targetBranch.empty()) {
                                    branchFromGitmodules = true;
                                    branchLog += "default branch (from .gitmodules): " + targetBranch + "\n";
                                }
                            }
                        }

                        // Step 2c: If no .gitmodules branch, detect remote default
                        if (targetBranch.empty()) {
                            const auto remoteHead = GitCapture(repoPath, {"symbolic-ref", "--quiet", "refs/remotes/origin/HEAD"});
                            if (remoteHead.exitCode == 0) {
                                auto ref = remoteHead.stdoutStr;
                                while (!ref.empty() && (ref.back() == '\r' || ref.back() == '\n' || ref.back() == ' ')) ref.pop_back();
                                const std::string marker = "refs/remotes/origin/";
                                if (ref.starts_with(marker) && ref.size() > marker.size()) {
                                    targetBranch = ref.substr(marker.size());
                                }
                            }
                            if (targetBranch.empty()) {
                                for (const std::string& probe : {"main", "master", "dev", "develop", "trunk"}) {
                                    const auto exists = GitCapture(repoPath, {"show-ref", "--verify", "--quiet", "refs/remotes/origin/" + probe});
                                    if (exists.exitCode == 0) {
                                        targetBranch = probe;
                                        break;
                                    }
                                }
                            }
                            if (!targetBranch.empty()) {
                                branchLog += "default branch (detected from remote): " + targetBranch + "\n";
                            }
                        }

                        // Step 2d: Checkout the branch
                        if (!targetBranch.empty()) {
                            postProgress(body + "\n--- branch ---\n" + branchLog + "checking out " + targetBranch + "...");

                            // Step 2d-i: Stash any residual files before checkout to avoid conflicts
                            bool stashCreated = false;
                            {
                                const auto statusCheck = GitCapture(repoPath, {"status", "--porcelain"});
                                if (statusCheck.exitCode == 0 && !Trim(statusCheck.stdoutStr).empty()) {
                                    const auto stash = GitCapture(repoPath, {"stash", "push", "--include-untracked", "-m", "kano-init-stash"});
                                    stashCreated = stash.exitCode == 0;
                                    branchLog += "stash: " + std::string(stashCreated ? "ok" : "failed") + "\n";
                                    if (!stash.stderrStr.empty()) {
                                        branchLog += stash.stderrStr + "\n";
                                    }
                                } else {
                                    branchLog += "stash: skipped (working tree clean)\n";
                                }
                            }

                            const auto localExists = GitCapture(repoPath, {"show-ref", "--verify", "--quiet", "refs/heads/" + targetBranch});
                            int checkoutExit = -1;
                            if (localExists.exitCode == 0) {
                                const auto co = GitCapture(repoPath, {"checkout", targetBranch});
                                checkoutExit = co.exitCode;
                                branchLog += "checkout " + targetBranch + ": " + (co.exitCode == 0 ? "ok" : "failed") + "\n";
                                if (co.exitCode != 0 && !co.stderrStr.empty()) {
                                    branchLog += co.stderrStr + "\n";
                                }
                            } else {
                                const auto co = GitCapture(repoPath, {"checkout", "-b", targetBranch, "origin/" + targetBranch});
                                checkoutExit = co.exitCode;
                                branchLog += "checkout -b " + targetBranch + " origin/" + targetBranch + ": " + (co.exitCode == 0 ? "ok" : "failed") + "\n";
                                if (co.exitCode != 0 && !co.stderrStr.empty()) {
                                    branchLog += co.stderrStr + "\n";
                                }
                            }

                            // Step 2d-ii: Stash pop after checkout
                            if (stashCreated) {
                                postProgress(body + "\n--- branch ---\n" + branchLog + "restoring stash...");
                                const auto pop = GitCapture(repoPath, {"stash", "pop"});
                                if (pop.exitCode == 0) {
                                    branchLog += "stash pop: ok\n";
                                } else {
                                    branchLog += "stash pop: conflict - working tree left in conflict state\n";
                                    if (!pop.stdoutStr.empty()) {
                                        branchLog += pop.stdoutStr + "\n";
                                    }
                                    if (!pop.stderrStr.empty()) {
                                        branchLog += pop.stderrStr + "\n";
                                    }
                                    branchLog += "WARNING: resolve conflicts manually, then run: git stash drop\n";
                                }
                            }

                            // Step 2e: Write back to .gitmodules if we detected the branch (not from .gitmodules)
                            if (checkoutExit == 0 && !branchFromGitmodules && !submodulePrefix.empty()) {
                                postProgress(body + "\n--- branch ---\n" + branchLog + "writing .gitmodules...");
                                const auto write = GitCapture(parentPath, {"config", "-f", ".gitmodules", "--replace-all", submodulePrefix + ".branch", targetBranch});
                                branchLog += "write .gitmodules branch=" + targetBranch + ": " + (write.exitCode == 0 ? "ok" : "failed") + "\n";
                            }
                        } else {
                            branchLog += "default branch: not detected (submodule remains in detached HEAD)\n";
                        }
                    }

                    if (!branchLog.empty()) {
                        body += "\n--- branch ---\n" + branchLog;
                    }

                    std::vector<RepoView> refreshedRepos;
                    try {
                        // Refresh workspace manifest and immediately rebuild live repo views
                        // in this same worker, so completion stays single-phase and doesn't
                        // leave preview/modal state waiting on a second refresh async cycle.
                        postProgress(body + "\nrefreshing workspace manifest...");
                        workspace::RefreshWorkspaceManifestAfterRegisteredChange(workspaceRoot);

                        postProgress(body + "\nreloading repo status...");
                        refreshedRepos = DiscoverRepoViews(dirtyOnly, discover.active, true);
                    } catch (const std::exception& e) {
                        body += std::string("\n--- refresh ---\nfailed: ") + e.what() + "\n";
                        {
                            std::lock_guard<std::mutex> lock(asyncMu);
                            asyncState.showPreview = true;
                            asyncState.previewTitle = label + " failed";
                            asyncState.previewBody = std::move(body);
                            asyncState.previewAutoCloseAfterRefresh = false;
                            asyncState.hasResult = true;
                            asyncState.hasError = true;
                            asyncState.errorMessage = std::string("submodule init refresh failed: ") + e.what();
                            asyncState.completionFooter = "command failed";
                        }
                        return;
                    }

                    {
                        std::lock_guard<std::mutex> lock(asyncMu);
                        asyncState.showPreview = true;
                        asyncState.previewTitle = label + (result.exitCode == 0 ? " complete" : " failed");
                        asyncState.previewBody = std::move(body);
                        asyncState.previewAutoCloseAfterRefresh = result.exitCode == 0;
                        asyncState.repos = refreshedRepos;
                        asyncState.refreshRepos = true;
                        asyncState.hasResult = true;
                        asyncState.completionFooter = result.exitCode == 0 ? "command finished" : "command failed";
                        if (result.exitCode != 0) {
                            asyncState.hasError = true;
                            asyncState.errorMessage = "submodule init failed";
                        }
                    }
                })) {
                preview.active = false;
                preview.running = false;
            } else {
                footer = label + " started in background";
                footerIsError = false;
            }
            return true;
        }

        if (history.active) {
            auto close_history_detail = [&]() {
                history.detailActive = false;
                history.detailSha.clear();
                history.detailBody.clear();
                history.detailSelectedSection = 0;
                history.detailPageIndex = 0;
            };

            auto switch_history_repo = [&](const int InDelta) {
                if (displayedRepoIndices.empty()) {
                    return;
                }
                // Find current position within displayed repos
                auto currentIt = std::find(displayedRepoIndices.begin(), displayedRepoIndices.end(), history.repoIndex);
                int posInDisplayed = 0;
                if (currentIt != displayedRepoIndices.end()) {
                    posInDisplayed = static_cast<int>(std::distance(displayedRepoIndices.begin(), currentIt));
                }
                // Cycle within displayed repos only
                const int n = static_cast<int>(displayedRepoIndices.size());
                posInDisplayed = (posInDisplayed + InDelta + n) % n;
                history.repoIndex = displayedRepoIndices[posInDisplayed];
                selectedDisplayed = posInDisplayed;
                history.pageIndex = 0;
                history.selectedLine = 0;
                history.highlightedLine = -1;
                close_history_detail();
                ensure_history_loaded(history.repoIndex, history.pageIndex);
            };

            auto move_history_page = [&](const int InDelta, const bool InSelectEdge) -> bool {
                const auto repoKey = CanonicalPathString(repos[history.repoIndex].path);
                const auto& currentCache = historyCache[repoKey];
                int targetPage = std::max(0, history.pageIndex + InDelta);
                const int pageSize = ComputeHistoryPageSize();
                const int totalEntries = static_cast<int>(currentCache.allEntries.size());
                const int maxPageIndex = std::max(0, (totalEntries + pageSize - 1) / pageSize - 1);
                if (InDelta > 0) {
                    targetPage = std::min(targetPage, maxPageIndex);
                }
                if (targetPage == history.pageIndex && InDelta < 0) {
                    return false;
                }
                if (targetPage == history.pageIndex && InDelta > 0) {
                    return false;
                }

                ensure_history_loaded(history.repoIndex, targetPage);
                const auto targetLines = history_page_slice(historyCache[repoKey], targetPage);
                if (targetLines.empty() && targetPage != 0) {
                    return false;
                }

                history.pageIndex = targetPage;
                history.highlightedLine = -1;
                close_history_detail();
                if (targetLines.empty()) {
                    history.selectedLine = 0;
                } else if (InSelectEdge && InDelta > 0) {
                    history.selectedLine = 0;
                } else if (InSelectEdge && InDelta < 0) {
                    history.selectedLine = static_cast<int>(targetLines.size()) - 1;
                } else {
                    history.selectedLine = std::clamp(history.selectedLine, 0, static_cast<int>(targetLines.size()) - 1);
                }
                return true;
            };

            if (history.detailActive) {
                const auto key = CanonicalPathString(repos[history.repoIndex].path);
                const bool isDirtyWorkingTree = history.detailSha == "dirty working tree";
                const auto detailKey = (isDirtyWorkingTree ? DirtyCachePrefix(historyCache[key]) : history.detailSha) + "|" + std::to_string(history.detailMode);
                auto* overlay = historyCache[key].detailOverlays.get(detailKey);
                if (overlay != nullptr) {
                    const int sectionCount = static_cast<int>(overlay->sections.size());
                    if (sectionCount > 0) {
                        history.detailSelectedSection = std::clamp(history.detailSelectedSection, 0, sectionCount - 1);
                        const auto sectionLines = SplitLines(overlay->sections[history.detailSelectedSection].body);
                        const int pageSize = ComputeDetailBodyPageSize();
                        const int pageCount = std::max(1, static_cast<int>((sectionLines.size() + pageSize - 1) / pageSize));
                        history.detailPageIndex = std::clamp(history.detailPageIndex, 0, pageCount - 1);

                        if (event == Event::Character('m')) {
                            // Cycle: summary(0) <-> patch(2), skip files mode as it's not very informative
                            history.detailMode = (history.detailMode == 0) ? 2 : 0;
                            auto& cache = historyCache[key];
                            const auto dirtyPrefix = isDirtyWorkingTree ? RefreshDirtyToken(cache, repos[history.repoIndex].path) : std::string();
                            const auto nextDetailKey = (isDirtyWorkingTree ? dirtyPrefix : history.detailSha) + "|" + std::to_string(history.detailMode);
                            auto* detail = cache.commitDetails.get(nextDetailKey);
                            if (detail == nullptr) {
                                cache.commitDetails.put(nextDetailKey, isDirtyWorkingTree
                                    ? FetchWorkingTreeDetail(repos[history.repoIndex].path, history.detailMode)
                                    : FetchCommitDetail(repos[history.repoIndex].path, history.detailSha, history.detailMode));
                            }
                            if (!cache.detailOverlays.contains(nextDetailKey)) {
                                RepoHistoryCache::HistoryEntry entry;
                                entry.isDirtyWorkingTree = isDirtyWorkingTree;
                                entry.sha = history.detailSha;
                                cache.detailOverlays.put(nextDetailKey, BuildHistoryDetailOverlay(repos[history.repoIndex].path, repos[history.repoIndex], entry, history.detailMode));
                            }
                            detail = cache.commitDetails.get(nextDetailKey);
                            history.detailBody = detail == nullptr ? std::string() : *detail;
                            history.detailSelectedSection = 0;
                            history.detailPageIndex = 0;
                            footer = history.detailMode == 0 ? "detail mode: summary" : "detail mode: patch";
                            return true;
                        }
                        if (event == Event::ArrowUp || event == Event::Character('k') || event == Event::Character('w')) {
                            if (history.detailSelectedSection > 0) {
                                history.detailSelectedSection -= 1;
                                history.detailPageIndex = 0;
                                footer = "detail change up";
                            } else {
                                footer = "detail at first change";
                            }
                            return true;
                        }
                        if (event == Event::ArrowDown || event == Event::Character('j') || event == Event::Character('s')) {
                            if (history.detailSelectedSection < sectionCount - 1) {
                                history.detailSelectedSection += 1;
                                history.detailPageIndex = 0;
                                footer = "detail change down";
                            } else {
                                footer = "detail at last change";
                            }
                            return true;
                        }
                        if (event == Event::ArrowLeft || event == Event::PageDown || event == Event::Character('h') || event == Event::Character('a') || event == Event::Character('K')) {
                            if (history.detailPageIndex > 0) {
                                history.detailPageIndex -= 1;
                                footer = "detail page <-";
                            } else {
                                footer = "detail at first page";
                            }
                            return true;
                        }
                        if (event == Event::ArrowRight || event == Event::PageUp || event == Event::Character('l') || event == Event::Character('d') || event == Event::Character('J')) {
                            if (history.detailPageIndex < pageCount - 1) {
                                history.detailPageIndex += 1;
                                footer = "detail page ->";
                            } else {
                                footer = "detail at last page";
                            }
                            return true;
                        }
                    }
                }
                return true;
            }

            if (history.searchMode) {
                if (event == Event::Escape) {
                    history.searchMode = false;
                    footer = "history search cancelled";
                    return true;
                }
                if (event == Event::Backspace) {
                    if (!history.searchQuery.empty()) {
                        history.searchQuery.pop_back();
                    }
                    const auto lines = current_history_lines();
                    history.highlightedLine = FindNextMatch(lines, history.searchQuery, -1);
                    footer = history.searchQuery.empty() ? "search cleared" : "search: /" + history.searchQuery;
                    return true;
                }
                if (event == Event::Return || event == Event::Character('\n')) {
                    history.searchMode = false;
                    footer = history.highlightedLine >= 0 ? "search applied" : "search no match";
                    return true;
                }
                if (event.is_character()) {
                    history.searchQuery += event.character();
                    const auto lines = current_history_lines();
                    history.highlightedLine = FindNextMatch(lines, history.searchQuery, -1);
                    footer = "search: /" + history.searchQuery;
                    return true;
                }
                return false;
            }

            if (event == Event::Character('/')) {
                history.searchMode = true;
                history.searchQuery.clear();
                history.highlightedLine = -1;
                footer = "search mode: type query then Enter";
                return true;
            }

            if (event == Event::Return || event == Event::Character('\n')) {
                ensure_history_loaded(history.repoIndex, history.pageIndex);
                const auto key = CanonicalPathString(repos[history.repoIndex].path);
                auto displayedPage = BuildDisplayedHistoryEntries(history_page_slice(historyCache[key], history.pageIndex), history);
                if (displayedPage.empty()) {
                    footer = "history detail skipped: page empty";
                    return true;
                }

                const int line = std::clamp(history.selectedLine, 0, static_cast<int>(displayedPage.size()) - 1);
                const auto& selectedEntry = displayedPage[line];

                auto& cache = historyCache[key];
                const auto dirtyPrefix = selectedEntry.isDirtyWorkingTree ? RefreshDirtyToken(cache, repos[history.repoIndex].path) : std::string();
                const auto detailKey = (selectedEntry.isDirtyWorkingTree ? dirtyPrefix : selectedEntry.sha) + "|" + std::to_string(history.detailMode);
                auto* detail = cache.commitDetails.get(detailKey);
                if (detail == nullptr) {
                    cache.commitDetails.put(detailKey, selectedEntry.isDirtyWorkingTree
                        ? FetchWorkingTreeDetail(repos[history.repoIndex].path, history.detailMode)
                        : FetchCommitDetail(repos[history.repoIndex].path, selectedEntry.sha, history.detailMode));
                }
                auto* overlay = cache.detailOverlays.get(detailKey);
                if (overlay == nullptr) {
                    cache.detailOverlays.put(detailKey, BuildHistoryDetailOverlay(repos[history.repoIndex].path, repos[history.repoIndex], selectedEntry, history.detailMode));
                    overlay = cache.detailOverlays.get(detailKey);
                }
                history.detailActive = true;
                history.detailSha = selectedEntry.isDirtyWorkingTree ? std::string("dirty working tree") : selectedEntry.sha;
                detail = cache.commitDetails.get(detailKey);
                history.detailBody = detail == nullptr ? std::string() : *detail;
                history.detailSelectedSection = 0;
                history.detailPageIndex = 0;
                footer = "history detail opened: " + history.detailSha;
                return true;
            }

            if (event == Event::Character('n')) {
                if (history.searchQuery.empty()) {
                    footer = "search query is empty";
                    return true;
                }
                const auto lines = current_history_lines();
                history.highlightedLine = FindNextMatch(lines, history.searchQuery, history.highlightedLine);
                if (history.highlightedLine >= 0) {
                    history.selectedLine = history.highlightedLine;
                }
                footer = history.highlightedLine >= 0 ? "search next match" : "search no match";
                return true;
            }

            if (event == Event::Character('o')) {
                history.sortMode = (history.sortMode + 1) % 3;
                history.selectedLine = 0;
                history.highlightedLine = -1;
                footer = history.sortMode == 0 ? "history sort: time-desc" : (history.sortMode == 1 ? "history sort: time-asc" : "history sort: match-first");
                return true;
            }

            if (event == Event::Character('m')) {
                // Cycle: summary(0) <-> patch(2), skip files mode as it's not very informative
                history.detailMode = (history.detailMode == 0) ? 2 : 0;
                footer = history.detailMode == 0 ? "detail mode: summary" : "detail mode: patch";
                return true;
            }

            if (event == Event::ArrowUp || event == Event::Character('k') || event == Event::Character('w')) {
                ensure_history_loaded(history.repoIndex, history.pageIndex);
                const auto key = CanonicalPathString(repos[history.repoIndex].path);
                const auto page = history_page_slice(historyCache[key], history.pageIndex);
                if (!page.empty()) {
                    if (history.selectedLine > 0) {
                        history.selectedLine -= 1;
                        footer = "history line up";
                    } else if (move_history_page(-1, true)) {
                        footer = "history page newer";
                    } else {
                        footer = "history at newest commit";
                    }
                    return true;
                }
            }
            if (event == Event::ArrowDown || event == Event::Character('j') || event == Event::Character('s')) {
                ensure_history_loaded(history.repoIndex, history.pageIndex);
                const auto key = CanonicalPathString(repos[history.repoIndex].path);
                const auto page = history_page_slice(historyCache[key], history.pageIndex);
                if (!page.empty()) {
                    if (history.selectedLine < static_cast<int>(page.size()) - 1) {
                        history.selectedLine += 1;
                        footer = "history line down";
                    } else if (move_history_page(1, true)) {
                        footer = "history page older";
                    } else {
                        footer = "history at oldest commit";
                    }
                    return true;
                }
            }

            if (event == Event::Character('[')) {
                switch_history_repo(-1);
                footer = "history repo <-";
                return true;
            }
            if (event == Event::Character(']')) {
                switch_history_repo(1);
                footer = "history repo ->";
                return true;
            }

            if (event == Event::ArrowLeft || event == Event::PageDown || event == Event::Character('h') || event == Event::Character('a') || event == Event::Character('K')) {
                if (move_history_page(-1, false)) {
                    footer = "history page newer";
                } else {
                    footer = "history at newest page";
                }
                return true;
            }
            if (event == Event::ArrowRight || event == Event::PageUp || event == Event::Character('l') || event == Event::Character('d') || event == Event::Character('J')) {
                if (move_history_page(1, false)) {
                    footer = "history page older";
                } else {
                    footer = "history at oldest page";
                }
                return true;
            }
        }

        return false;
    });

    auto ui = Renderer(with_keys, [&] {
        Element rightPanel;

        if (discover.active) {
            const int totalPages = std::max(1, static_cast<int>((discover.lines.size() + static_cast<std::size_t>(discover.pageSize) - 1) / static_cast<std::size_t>(discover.pageSize)));
            const int clampedPage = std::clamp(discover.pageIndex, 0, totalPages - 1);
            const int start = clampedPage * discover.pageSize;
            const int end = std::min(start + discover.pageSize, static_cast<int>(discover.lines.size()));

            Elements pageRows;
            for (int i = start; i < end; ++i) {
                pageRows.push_back(text(discover.lines[static_cast<std::size_t>(i)]));
            }
            if (pageRows.empty()) {
                pageRows.push_back(text("(no lines in this page)") | kMutedStyle);
            }

            rightPanel = vbox({
                status_title("Discover Output (paged)", discover.loading ? StatusTone::Running : StatusTone::Info),
                separator(),
                status_text("command: :" + discover.title),
                status_text(discover.loading
                         ? ("state: running | " + (discover.progress.empty() ? std::string("starting...") : discover.progress))
                         : "state: ready"),
                status_text("page: " + std::to_string(clampedPage + 1) + "/" + std::to_string(totalPages) +
                     "  lines: " + std::to_string(start + 1) + "-" + std::to_string(std::max(start + 1, end)) +
                     "/" + std::to_string(discover.lines.size())),
                text("controls: [ or PgDown prev page | ] or PgUp next page | Esc/q close") | kSecondaryStyle,
                separator(),
                vbox(std::move(pageRows)) | border,
            }) | border;
        } else if (tui_state.GetMode() == kano::git::commands::TuiMode::Help) {
            rightPanel = vbox({
                status_title("Help"),
                separator(),
                text("Command Mode") | kInfoStyle,
                text(": enter command mode, Esc cancel, Enter execute") | kSecondaryStyle,
                text("Try :refresh, :discover, or :discover dirty") | kSecondaryStyle,
                text("Tab/Up/Down navigate candidates, Enter accepts selected candidate") | kSecondaryStyle,
                separator(),
                text("Shortcuts") | kInfoStyle,
                text("r refresh | d dirty-only | f fetch | c/C commit preview/execute") | kSecondaryStyle,
                text("p/P push preview/execute | Enter history | q quit") | kSecondaryStyle,
                separator(),
                text("Press Esc or q to close") | kSecondaryStyle,
            }) | border;
        } else if (tui_state.GetMode() == kano::git::commands::TuiMode::CommandPalette) {
            Elements rows;
            if (tui_state.palette_state.filtered_commands.empty()) {
                rows.push_back(text("(no matching commands)") | kMutedStyle);
            } else {
                for (std::size_t i = 0; i < tui_state.palette_state.filtered_commands.size(); ++i) {
                    const auto& item = tui_state.palette_state.filtered_commands[i];
                    const bool selected = static_cast<int>(i) == tui_state.palette_state.selected_index;
                    auto row = hbox({
                        text(selected ? "> " : "  "),
                        text("[" + item.category + "] ") | kInfoStyle,
                        text(item.name) | bold,
                        text(" - " + item.description) | kSecondaryStyle,
                    });
                    if (selected) {
                        row = row | kSelectedStyle;
                    }
                    rows.push_back(row);
                }
            }

            rightPanel = vbox({
                status_title("Command Palette"),
                separator(),
                status_text("search: " + tui_state.palette_state.search_query),
                separator(),
                vbox(std::move(rows)) | border,
                separator(),
                text("Enter select | Esc close") | kMutedStyle,
            }) | border;
        } else if (tui_state.GetMode() == kano::git::commands::TuiMode::Confirm && tui_state.confirm_state.active) {
            rightPanel = vbox({
                status_title("Confirm Command", StatusTone::Warning),
                separator(),
                paragraph(tui_state.confirm_state.message),
                separator(),
                text("Press y to confirm, n/Esc to cancel") | kWarningStyle,
            }) | border;
        } else if (rebaseRun.active) {
            std::string progress = "progress: " + std::to_string(std::min(rebaseRun.index, static_cast<int>(rebaseRun.queue.size()))) + "/" + std::to_string(rebaseRun.queue.size());
            std::string current = "(none)";
            if (rebaseRun.index >= 0 && rebaseRun.index < static_cast<int>(rebaseRun.queue.size())) {
                const auto& item = rebaseRun.queue[rebaseRun.index];
                current = item.action + " " + item.sha + " " + item.title;
            }
            rightPanel = vbox({
                status_title("Rebase Runner", rebaseRun.waitingConflictResolution ? StatusTone::Warning : StatusTone::Running),
                separator(),
                status_text("repo: " + rebaseRun.repo.lexically_normal().generic_string()),
                status_text(progress),
                status_text("current: " + current),
                status_text(rebaseRun.waitingConflictResolution
                         ? "state: waiting resolution (C=continue, S=skip, A=abort)"
                         : "state: ready (N=next, A=abort, q=close panel)"),
                separator(),
                paragraph(rebaseRun.lastOutput.empty() ? "(no output yet)" : rebaseRun.lastOutput),
            }) | border;
        } else if (rebasePlanner.active) {
            rightPanel = vbox({
                status_title("Rebase Planner"),
                separator(),
                status_text("repo: " + rebasePlanner.repo.lexically_normal().generic_string()),
                status_text("base: " + rebasePlanner.baseRef),
                text("controls: up/down select, p=pick s=squash f=fixup d=drop, q close") | kMutedStyle,
                separator(),
                vbox([&] {
                    Elements rows;
                    for (std::size_t i = 0; i < rebasePlanner.items.size(); ++i) {
                        const auto& it = rebasePlanner.items[i];
                        const bool sel = static_cast<int>(i) == rebasePlanner.selectedIndex;
                        auto row = text(std::string(sel ? "> " : "  ") + it.action + " " + it.sha + " " + it.title);
                        if (sel) {
                            row = row | kHighlightStyle;
                        }
                        rows.push_back(row);
                    }
                    if (rebasePlanner.items.empty()) {
                        rows.push_back(text("(no planner items)"));
                    }
                    return rows;
                }()) | border,
                separator(),
                text("Plan preview") | kInfoStyle,
                paragraph(rebasePlanner.preview),
            }) | border;
        } else if (rebase.active) {
            rightPanel = vbox({
                status_title("Rebase Preflight", StatusTone::Warning),
                separator(),
                status_text("repo: " + rebase.repo.lexically_normal().generic_string()),
                status_text("branch: " + rebase.branch),
                status_text("upstream: " + rebase.upstream),
                status_text("tracking: " + rebase.tracking),
                status_text("merge-base: " + (rebase.mergeBase.empty() ? "(unknown)" : rebase.mergeBase)),
                status_text("risk: " + rebase.risk),
                status_text("note: " + rebase.note),
                separator(),
                text("candidate commits") | kInfoStyle,
                vbox([&] {
                    Elements rows;
                    const auto maxItems = std::min<std::size_t>(rebase.candidates.size(), 24);
                    for (std::size_t i = 0; i < maxItems; ++i) {
                        rows.push_back(text(rebase.candidates[i]));
                    }
                    if (rebase.candidates.size() > maxItems) {
                        rows.push_back(text("... and " + std::to_string(rebase.candidates.size() - maxItems) + " more"));
                    }
                    if (rebase.candidates.empty()) {
                        rows.push_back(text("(none)"));
                    }
                    return rows;
                }()) | border,
                separator(),
                text("Press q to close rebase preflight") | kMutedStyle,
            }) | border;
        } else if (cherryRun.active) {
            std::string progress = "progress: " + std::to_string(std::min(cherryRun.index, static_cast<int>(cherryRun.queue.size()))) + "/" + std::to_string(cherryRun.queue.size());
            std::string current = "(none)";
            if (cherryRun.index >= 0 && cherryRun.index < static_cast<int>(cherryRun.queue.size())) {
                current = cherryRun.queue[cherryRun.index].sha + " " + cherryRun.queue[cherryRun.index].title;
            }
            rightPanel = vbox({
                status_title("Cherry-pick Runner", cherryRun.waitingConflictResolution ? StatusTone::Warning : StatusTone::Running),
                separator(),
                status_text("repo: " + cherryRun.repo.lexically_normal().generic_string()),
                status_text(progress),
                status_text("current: " + current),
                status_text(cherryRun.waitingConflictResolution
                         ? "state: waiting conflict resolution (c=continue, s=skip, a=abort)"
                         : "state: ready (n=next, a=abort, q=close panel)"),
                separator(),
                paragraph(cherryRun.lastOutput.empty() ? "(no output yet)" : cherryRun.lastOutput),
            }) | border;
        } else if (cherry.active) {
            rightPanel = vbox({
                status_title("Cherry-pick Preflight", StatusTone::Warning),
                separator(),
                status_text("source: " + cherry.sourceBranch),
                status_text("target: " + cherry.targetBranch),
                status_text(cherry.note),
                separator(),
                text("candidate commits") | kInfoStyle,
                vbox([&] {
                    Elements rows;
                    const auto maxItems = std::min<std::size_t>(cherry.commits.size(), 24);
                    for (std::size_t i = 0; i < maxItems; ++i) {
                        const auto& c = cherry.commits[i];
                        const auto dup = c.alreadyInTarget ? "dup" : "new";
                        rows.push_back(text(c.sha + " | " + dup + " | risk=" + c.risk + " | " + c.title));
                    }
                    if (cherry.commits.size() > maxItems) {
                        rows.push_back(text("... and " + std::to_string(cherry.commits.size() - maxItems) + " more"));
                    }
                    return rows;
                }()) | border,
                separator(),
                text("Press q to close preflight panel") | kMutedStyle,
            }) | border;
        } else if (confirm.active) {
            rightPanel = vbox({
                status_title(confirm.title, StatusTone::Warning),
                separator(),
                text(confirm.description),
                status_text("repo: " + confirm.repo.lexically_normal().generic_string()),
                status_text("command: git" + [&] {
                    std::string cmd;
                    for (const auto& part : confirm.command) {
                        cmd += " " + part;
                    }
                    return cmd;
                }()),
                separator(),
                text("Press y to execute, n/Esc/q to cancel") | kWarningStyle,
            }) | border;
        } else if (preview.active) {
            rightPanel = vbox({
                status_title(preview.title + (preview.running ? " (running)" : ""), preview.running ? StatusTone::Running : (preview.isError ? StatusTone::Error : StatusTone::Success)),
                separator(),
                text(preview.running ? "Running... q will close panel only" : "Press q to close preview") | (preview.running ? kInfoStyle : kMutedStyle),
                separator(),
                [&] {
                    const auto lines = SplitLines(preview.body);
                    Elements elems;
                    elems.reserve(lines.size());
                    for (const auto& line : lines) {
                        elems.push_back(line.empty() ? text("") : paragraph(line));
                    }
                    return vbox(std::move(elems));
                }(),
            }) | border;
        } else if (history.active && !repos.empty()) {
            const auto& repo = repos[history.repoIndex];
            const auto key = CanonicalPathString(repo.path);
            ensure_history_loaded(history.repoIndex, history.pageIndex);
            const auto& cache = historyCache[key];

            if (history.detailActive) {
                const bool isDirtyWorkingTree = history.detailSha == "dirty working tree";
                const auto detailKey = (isDirtyWorkingTree ? DirtyCachePrefix(historyCache[key]) : history.detailSha) + "|" + std::to_string(history.detailMode);
                auto* overlay = historyCache[key].detailOverlays.get(detailKey);

                if (overlay == nullptr) {
                    rightPanel = vbox({
                        status_title("History Detail"),
                        separator(),
                        text("(detail data unavailable)") | kMutedStyle,
                        separator(),
                        text("Press Esc or q to close") | kMutedStyle,
                    }) | border;
                } else {
                    const int sectionCount = static_cast<int>(overlay->sections.size());
                    const int selectedSection = sectionCount == 0 ? 0 : std::clamp(history.detailSelectedSection, 0, sectionCount - 1);
                    const auto sectionLines = sectionCount == 0
                        ? std::vector<std::string>{"(no detail sections)"}
                        : SplitLines(overlay->sections[selectedSection].body);
                    const int pageSize = ComputeDetailBodyPageSize();
                    const int pageCount = std::max(1, static_cast<int>((sectionLines.size() + pageSize - 1) / pageSize));
                    const int pageIndex = std::clamp(history.detailPageIndex, 0, pageCount - 1);
                    const int start = pageIndex * pageSize;
                    const int end = std::min(start + pageSize, static_cast<int>(sectionLines.size()));

                    Elements sectionRows;
                    for (int i = 0; i < sectionCount; ++i) {
                        const bool isSelected = i == selectedSection;
                        auto row = text(std::string(isSelected ? "> " : "  ") + overlay->sections[i].title);
                        if (isSelected) {
                            row = row | kSelectedStyle;
                        }
                        sectionRows.push_back(row);
                    }
                    if (sectionRows.empty()) {
                        sectionRows.push_back(text("(no changes)") | kMutedStyle);
                    }

                    Elements bodyRows;
                    for (int i = start; i < end; ++i) {
                        const auto& lineText = sectionLines[static_cast<std::size_t>(i)];
                        auto row = status_paragraph(lineText);
                        if (history.detailMode == 2 && !lineText.empty()) {
                            if (lineText[0] == '+') {
                                row = row | kSuccessStyle;
                            } else if (lineText[0] == '-') {
                                row = row | kErrorStyle;
                            } else if (lineText.rfind("@@", 0) == 0 || lineText.rfind("diff ", 0) == 0 || lineText.rfind("index ", 0) == 0) {
                                row = row | kInfoStyle;
                            }
                        }
                        bodyRows.push_back(row);
                    }
                    if (bodyRows.empty()) {
                        bodyRows.push_back(paragraph("(empty page)") | kMutedStyle);
                    }

                    const auto modeLabel = history.detailMode == 0 ? "summary" : "patch";
                    const auto sectionLabel = sectionCount == 0
                        ? std::string("0/0")
                        : std::to_string(selectedSection + 1) + "/" + std::to_string(sectionCount);

                    rightPanel = vbox({
                        hbox({
                            status_text("commit: " + history.detailSha),
                            filler(),
                            status_text("mode: " + std::string(modeLabel)),
                            filler(),
                            status_text("section " + sectionLabel),
                            filler(),
                            status_text("page " + std::to_string(pageIndex + 1) + "/" + std::to_string(pageCount)),
                        }) | kTitleStyle,
                        separator(),
                        hbox({
                            vbox({
                                text("changes") | kInfoStyle,
                                separator(),
                                vbox(std::move(sectionRows)) | yframe | flex,
                            }) | size(WIDTH, EQUAL, 32) | border,
                            separator(),
                            vbox({
                                text(overlay->title) | kSectionTitleStyle,
                                text(sectionCount == 0 ? std::string("(no selected change)") : overlay->sections[selectedSection].title) | kMutedStyle,
                                separator(),
                                vbox(std::move(bodyRows)) | yframe | flex,
                            }) | flex,
                        }) | flex,
                        separator(),
                        text("detail controls: up/down change, left/right page, m mode, Esc/q close") | kSecondaryStyle,
                    }) | border;
                }
            } else {
                auto entries = BuildDisplayedHistoryEntries(
                    history_page_slice(cache, history.pageIndex),
                    history);

            std::string totalPages = "?";
            const int totalEntries = static_cast<int>(cache.allEntries.size());
            const int pageSize = ComputeHistoryPageSize();
            const int pages = std::max(1, (totalEntries + pageSize - 1) / pageSize);
            totalPages = std::to_string(pages);

            const int clampedSelectedLine = entries.empty() ? 0 : std::clamp(history.selectedLine, 0, static_cast<int>(entries.size()) - 1);
            const auto* selectedEntry = entries.empty() ? nullptr : &entries[clampedSelectedLine];

            auto historyList = vbox([&] {
                 Elements rows;
                 const int historyRowWidth = ComputeHistoryRowWidthEstimate();
                 constexpr int kMinSubjectWidth = 15;
                 const std::string kRowPrefix = "│ ";
                 const std::string kAuthorSeparator = " | ";

                 auto build_index_text = [&](const RepoHistoryCache::HistoryEntry& entry) {
                     return entry.totalCount > 0
                         ? std::to_string(entry.globalIndex) + "/" + std::to_string(entry.totalCount)
                         : std::to_string(entry.globalIndex) + "/?";
                 };

                 auto base_prefix_width = static_cast<int>(std::string("  ").size() + kRowPrefix.size());
                 int minAuthorAllowance = historyRowWidth;
                 std::size_t maxEmailLen = 0;
                 std::size_t maxNameLen = 0;

                 for (const auto& entry : entries) {
                     const auto indexText = build_index_text(entry);
                     const auto indexPart = "[" + indexText + "] ";
                     const auto shaPart = entry.isDirtyWorkingTree ? "(dirty)" : entry.sha;
                     const int baseLen = static_cast<int>(indexPart.size() + shaPart.size() + 1);
                     const int allowance = historyRowWidth - base_prefix_width - baseLen - kMinSubjectWidth - static_cast<int>(kAuthorSeparator.size());
                     minAuthorAllowance = std::min(minAuthorAllowance, allowance);
                     if (!entry.authorEmail.empty()) {
                         maxEmailLen = std::max(maxEmailLen, entry.authorEmail.size());
                     }
                     if (!entry.authorName.empty()) {
                         maxNameLen = std::max(maxNameLen, entry.authorName.size());
                     }
                 }

                 enum class AuthorMode {
                     None,
                     Email,
                     Name,
                 };

                 AuthorMode authorMode = AuthorMode::None;
                 int authorColumnWidth = 0;
                 if (!entries.empty() && minAuthorAllowance > 0) {
                     const int allowed = minAuthorAllowance;
                     if (maxEmailLen > 0 && static_cast<int>(maxEmailLen) <= allowed) {
                         authorMode = AuthorMode::Email;
                         authorColumnWidth = static_cast<int>(maxEmailLen);
                     } else if (maxNameLen > 0 && static_cast<int>(maxNameLen) <= allowed) {
                         authorMode = AuthorMode::Name;
                         authorColumnWidth = static_cast<int>(maxNameLen);
                     } else if (allowed > 0) {
                         if (maxEmailLen > 0) {
                             authorMode = AuthorMode::Email;
                             authorColumnWidth = allowed;
                         } else if (maxNameLen > 0) {
                             authorMode = AuthorMode::Name;
                             authorColumnWidth = allowed;
                         }
                     }
                 }

                 auto truncate_with_ellipsis = [&](const std::string& value, int width) {
                     if (width <= 0) {
                         return std::string();
                     }
                     if (static_cast<int>(value.size()) <= width) {
                         return value;
                     }
                     if (width <= 3) {
                         return value.substr(0, static_cast<std::size_t>(width));
                     }
                     return value.substr(0, static_cast<std::size_t>(width - 3)) + "...";
                 };

                 auto pad_right = [&](const std::string& value, int width) {
                     if (width <= 0) {
                         return std::string();
                     }
                     if (static_cast<int>(value.size()) >= width) {
                         return value.substr(0, static_cast<std::size_t>(width));
                     }
                     return value + std::string(static_cast<std::size_t>(width - static_cast<int>(value.size())), ' ');
                 };

                 auto pad_left = [&](const std::string& value, int width) {
                     if (width <= 0) {
                         return std::string();
                     }
                     if (static_cast<int>(value.size()) >= width) {
                         return value.substr(0, static_cast<std::size_t>(width));
                     }
                     return std::string(static_cast<std::size_t>(width - static_cast<int>(value.size())), ' ') + value;
                 };

                 for (std::size_t i = 0; i < entries.size(); ++i) {
                       const bool isHit = history.highlightedLine == static_cast<int>(i);
                      const bool isSelected = clampedSelectedLine == static_cast<int>(i);
                       auto prefix = std::string(isSelected ? "> " : "  ");

                       std::string chosenLine;
                       if (authorMode == AuthorMode::None || authorColumnWidth <= 0) {
                           chosenLine = BuildHistoryDisplayLine(entries[i]);
                       } else {
                           const auto indexText = build_index_text(entries[i]);
                           const auto indexPart = "[" + indexText + "] ";
                           const auto shaPart = entries[i].isDirtyWorkingTree ? "(dirty)" : entries[i].sha;
                           const auto subjectPart = entries[i].isDirtyWorkingTree ? "dirty working tree" : entries[i].subject;
                           const int baseLen = static_cast<int>(indexPart.size() + shaPart.size() + 1);
                           const int subjectWidth = historyRowWidth - base_prefix_width - baseLen - authorColumnWidth - static_cast<int>(kAuthorSeparator.size());
                           const auto subjectTrimmed = truncate_with_ellipsis(subjectPart, subjectWidth);
                           const auto subjectPadded = pad_right(subjectTrimmed, subjectWidth);

                           std::string authorText = (authorMode == AuthorMode::Email) ? entries[i].authorEmail : entries[i].authorName;
                           if (static_cast<int>(authorText.size()) > authorColumnWidth) {
                               authorText = authorText.substr(0, static_cast<std::size_t>(authorColumnWidth));
                           }
                           const auto authorPadded = pad_left(authorText, authorColumnWidth);
                           chosenLine = indexPart + shaPart + " " + subjectPadded + kAuthorSeparator + authorPadded;
                       }

                       auto row = text(prefix + kRowPrefix + chosenLine);
                        if (isSelected) {
                            row = row | kSelectedStyle;
                        }
                        if (isHit) {
                            row = row | kHighlightStyle;
                        }
                       rows.push_back(row);
                  }
                  if (entries.empty()) {
                      rows.push_back(text("  │ (no history entries)") | kMutedStyle);
                  }
                  return rows;
              }()) | yframe;

            auto quickStats = [&]() {
                if (selectedEntry == nullptr) {
                    return text("quick stats: (no history entries)") | kSecondaryStyle;
                }
                if (selectedEntry->isDirtyWorkingTree) {
                    return text("quick stats: dirty working tree | " + std::to_string(repo.dirtyFiles.size()) + " files") | kWarningStyle;
                }

                auto& mutableCache = historyCache[key];
                auto* stat = mutableCache.commitQuickStats.get(selectedEntry->sha);
                if (stat == nullptr) {
                    mutableCache.commitQuickStats.put(selectedEntry->sha, FetchCommitQuickStats(repo.path, selectedEntry->sha));
                    stat = mutableCache.commitQuickStats.get(selectedEntry->sha);
                }
                return text("quick stats: " + (stat == nullptr ? std::string("(unavailable)") : *stat)) | kSecondaryStyle;
            }();

                rightPanel = vbox({
                    status_title("History Pager"),
                    separator(),
                    hbox({
                        status_text("repo: " + DisplayRepoPath(workspaceRoot, repo.path)),
                        filler(),
                        status_text("branch: " + repo.branch),
                        filler(),
                        status_text("repo " + [&]() {
                            auto it = std::find(displayedRepoIndices.begin(), displayedRepoIndices.end(), history.repoIndex);
                            const int pos = (it != displayedRepoIndices.end()) ? static_cast<int>(std::distance(displayedRepoIndices.begin(), it)) + 1 : 0;
                            return std::to_string(pos) + "/" + std::to_string(displayedRepoIndices.size());
                        }()),
                    }),
                    hbox({
                        status_text("parent: " + DisplayParentRepo(workspaceRoot, repo.parentRepo)),
                        filler(),
                        status_text("children: " + std::to_string(repo.childRepoCount)),
                        filler(),
                        status_text("page " + std::to_string(history.pageIndex + 1) + "/" + totalPages),
                        filler(),
                        status_text("entry " + (selectedEntry == nullptr ? std::string("0/?") : std::to_string(selectedEntry->globalIndex) + "/" + (selectedEntry->totalCount > 0 ? std::to_string(selectedEntry->totalCount) : std::string("?")))),
                        filler(),
                        status_text("search: " + (history.searchQuery.empty() ? "(none)" : "/" + history.searchQuery)),
                        filler(),
                        status_text("detail: " + std::string(history.detailMode == 0 ? "summary" : "patch")),
                        filler(),
                        status_text("sort: " + std::string(history.sortMode == 0 ? "time-desc" : (history.sortMode == 1 ? "time-asc" : "match-first"))),
                    }),
                    separator(),
                    historyList | border | flex,
                    quickStats,
                    text(""),
                    separator(),
                    text("controls: [ prev-repo, ] next-repo, left/right page, up/down line->page, Enter detail, m(mode), o(sort), /(search), n(next), q close") | kSecondaryStyle,
                }) | border;
            }
        } else if (!repos.empty() && RepoIndexFromDisplayed(displayedRepoIndices, selectedDisplayed) >= 0) {
            const int selected = RepoIndexFromDisplayed(displayedRepoIndices, selectedDisplayed);
            const auto& row = repos[selected];
            if (row.type == "registered-uninit") {
                const auto pathText = DisplayRepoPath(workspaceRoot, row.path);
                const auto parentText = CompactDetailValue(DisplayParentRepo(workspaceRoot, row.parentRepo));
                rightPanel = vbox({
                    status_title("Repository Details", StatusTone::Warning),
                    separator(),
                    status_paragraph("path: " + pathText),
                    status_paragraph("parent: " + parentText),
                    status_paragraph("type: registered-uninit"),
                    separator(),
                    text("not initialized") | kWarningStyle,
                    paragraph("This registered source path exists in the workspace, but it is not an initialized git repository yet.") | kMutedStyle,
                    separator(),
                    text("suggested actions") | kInfoStyle,
                    status_paragraph("hotkey: press I to initialize submodule"),
                    status_paragraph("manual: git -C \"" + row.parentRepo + "\" submodule update --init -- \"" + (row.parentRepo.empty() ? row.path.lexically_normal().generic_string() : std::filesystem::path(row.path).lexically_relative(std::filesystem::path(row.parentRepo)).generic_string()) + "\""),
                    separator(),
                    paragraph("history, branch, upstream, tracking, and dirty-file details are unavailable until this source is initialized.") | kMutedStyle,
                }) | border;
            } else {
                const auto cacheMark = row.statusFromSnapshot ? "*" : "";
                const auto pathText = DisplayRepoPath(workspaceRoot, row.path);
                const auto parentText = CompactDetailValue(DisplayParentRepo(workspaceRoot, row.parentRepo));
                const auto branchText = CompactDetailValue(row.branch) + cacheMark;
                const auto upstreamText = CompactDetailValue(row.upstream) + cacheMark;
                const auto trackingText = CompactDetailValue(row.tracking) + cacheMark;
                const auto dirtyText = std::string(row.repoDirty ? "y" : "n") + cacheMark;
                const auto worktreeDirtyText = std::string(row.worktreeDirty ? "y" : "n") + cacheMark;
                const auto dirtyWorktreesText = CompactDetailValue(row.dirtyWorktrees.empty() ? "-" : row.dirtyWorktrees);
                int detailRepoListWidth = 0;
                for (const auto& entry : menu) {
                    detailRepoListWidth = std::max(detailRepoListWidth, static_cast<int>(entry.size()));
                }
                detailRepoListWidth = std::clamp(detailRepoListWidth + 4, 22, 60);
                const auto terminalWidth = ftxui::Terminal::Size().dimx;
                const int estimatedRightPanelWidth = std::max(24, terminalWidth - detailRepoListWidth - 7);
                const auto fullDetailLines = std::vector<std::string>{
                    "path: " + pathText,
                    "parent: " + parentText,
                    "type: " + row.type + " | children: " + std::to_string(row.childRepoCount),
                    "branch: " + branchText + " | dirty: " + dirtyText,
                    "upstream: " + upstreamText,
                    "tracking: " + trackingText + " | worktree dirty: " + worktreeDirtyText,
                    "worktrees: " + dirtyWorktreesText,
                };
                const auto shortDetailLines = std::vector<std::string>{
                    "p: " + pathText,
                    "par: " + parentText,
                    "t: " + row.type + " | k: " + std::to_string(row.childRepoCount),
                    "br: " + branchText + " | d: " + dirtyText,
                    "up: " + upstreamText,
                    "tr: " + trackingText + " | wt: " + worktreeDirtyText,
                    "wts: " + dirtyWorktreesText,
                };
                const auto detailLabelMode = ChooseDetailLabelMode(estimatedRightPanelWidth, fullDetailLines, shortDetailLines);
                const auto dirtyFileCount = row.dirtyFiles.size();
                const auto dirtyPreviewLimit = std::min<std::size_t>(dirtyFileCount, 8);
                const auto dirtyFilesTitle = std::string("dirty files: ") + std::to_string(dirtyFileCount)
                    + (row.statusFromSnapshot ? " (snapshot cache; run :refresh for live list)" : " (top " + std::to_string(dirtyPreviewLimit) + ")");
                rightPanel = vbox({
                    status_title("Repository Details"),
                    separator(),
                    paragraph("*: snapshot cache") | kMutedStyle,
                    status_paragraph(DetailLine(detailLabelMode, "path", "p", pathText)),
                    status_paragraph(DetailLine(detailLabelMode, "parent", "par", parentText)),
                    status_paragraph(DetailLine(detailLabelMode, "type", "t", row.type)
                        + (detailLabelMode == DetailLabelMode::Bare ? " | " + std::to_string(row.childRepoCount) + " children" : " | " + DetailLine(detailLabelMode, "children", "k", std::to_string(row.childRepoCount)))),
                    status_paragraph(DetailLine(detailLabelMode, "branch", "br", branchText)
                        + (detailLabelMode == DetailLabelMode::Bare ? " | dirty " + dirtyText : " | " + DetailLine(detailLabelMode, "dirty", "d", dirtyText))),
                    status_paragraph(DetailLine(detailLabelMode, "upstream", "up", upstreamText)),
                    status_paragraph(DetailLine(detailLabelMode, "tracking", "tr", trackingText)
                        + (detailLabelMode == DetailLabelMode::Bare ? " | wt " + worktreeDirtyText : " | " + DetailLine(detailLabelMode, "worktree dirty", "wt", worktreeDirtyText))),
                    status_paragraph(DetailLine(detailLabelMode, "worktrees", "wts", dirtyWorktreesText)),
                    separator(),
                    text(dirtyFilesTitle) | kInfoStyle,
                    row.statusFromSnapshot
                        ? paragraph("(unavailable in snapshot cache)") | kMutedStyle
                        : row.dirtyFiles.empty()
                        ? paragraph("(none)") | kMutedStyle
                        : vbox([&] {
                              Elements lines;
                              for (std::size_t i = 0; i < dirtyPreviewLimit; ++i) {
                                  const auto& dirtyFile = row.dirtyFiles[i];
                                  const auto changeSuffix = dirtyFile.changedLines >= 0
                                      ? " (" + std::to_string(dirtyFile.changedLines) + " lines)"
                                      : "";
                                  lines.push_back(paragraph("- " + dirtyFile.displayPath + changeSuffix) | kWarningStyle);
                              }
                              if (row.dirtyFiles.size() > dirtyPreviewLimit) {
                                  lines.push_back(paragraph("...") | kMutedStyle);
                              }
                              return lines;
                          }()),
                }) | border;
            }
        } else {
            rightPanel = vbox({
                status_title("Repository Details"),
                separator(),
                text("No repositories to display.") | kMutedStyle,
            }) | border;
        }

        Elements commandRows;
        if (tui_state.GetMode() == kano::git::commands::TuiMode::Command) {
            if (tui_state.command_state.HasCandidates()) {
                Elements candRows;
                for (std::size_t i = 0; i < tui_state.command_state.candidates.items.size(); ++i) {
                    const auto& c = tui_state.command_state.candidates.items[i];
                    const bool selected = static_cast<int>(i) == tui_state.command_state.candidates.selected_index;
                    auto row = hbox({
                        text(selected ? "> " : "  "),
                        text(c.text) | bold | color(Color::White),
                        text(" - " + c.description) | kSecondaryStyle,
                    });
                    if (selected) {
                        row = row | kSelectedStyle;
                    }
                    candRows.push_back(row);
                }
                commandRows.push_back(vbox({text("Candidates") | kInfoStyle, vbox(std::move(candRows))}) | border);
                commandRows.push_back(separator());
            }

            std::string inputLine = tui_state.command_state.GetBuffer();
            const auto cursorPos = std::min(tui_state.command_state.GetCursorPos(), inputLine.size());
            inputLine.insert(cursorPos, "█");
            commandRows.push_back(status_text("scope: " + command_scope_label()));
            commandRows.push_back(text("command controls: g toggle scope (empty input), Tab complete, Enter execute, Esc cancel") | kSecondaryStyle);
            commandRows.push_back(separator());
            commandRows.push_back((text(":" + inputLine) | color(Color::White)) | border);
            commandRows.push_back(separator());
        }

        const auto footerText = tui_state.footer_message.empty() ? footer : tui_state.footer_message;
        const auto footerTone = (tui_state.footer_is_error || footerIsError) ? StatusTone::Error : classify_line_tone(footerText);
        auto footerElement = status_line(footerText, footerTone);

        auto repoListPanel = vbox({
            status_title(history.active ? "Repos" : "Repositories"),
            separator(),
            list->Render() | border,
        });

        // Compute a stable width for the left panel based on the longest
        // menu entry, capped so it never dominates the screen.  This prevents
        // the two-flex layout from shifting when content on either side changes.
        const int kMinRepoListWidth = 22;
        const int kMaxRepoListWidth = 60;
        int longestEntry = 0;
        for (const auto& entry : menu) {
            longestEntry = std::max(longestEntry, static_cast<int>(entry.size()));
        }
        // +4 accounts for border (2) + small padding (2)
        const int repoListWidth = std::clamp(longestEntry + 4, kMinRepoListWidth, kMaxRepoListWidth);

        auto mainPanel = history.active
                             ? hbox({
                                    repoListPanel | size(WIDTH, EQUAL, kMinRepoListWidth),
                                    separator(),
                                    rightPanel | flex,
                                })
                             : hbox({
                                   repoListPanel | size(WIDTH, EQUAL, repoListWidth),
                                   separator(),
                                   rightPanel | flex,
                               });

        Elements rootRows;
        rootRows.push_back(text("KOG FTXUI Dashboard v2") | kTitleStyle);
        rootRows.push_back(separator());
        if (!history.active) {
            rootRows.push_back(paragraph("Command-input-first workflow + global repo view + incremental history pager.") | kInfoStyle);
            rootRows.push_back(paragraph("command examples: :refresh | :discover | :discover dirty") | kSecondaryStyle);
            rootRows.push_back(status_paragraph("repo filter: " + (repoFilter.empty() ? "(none)" : repoFilter)));
        }
        rootRows.push_back([&]() {
                        std::lock_guard<std::mutex> lock(asyncMu);
                        if (!asyncState.busy) {
                        return paragraph("") | kSecondaryStyle;
                        }
                        std::string backgroundText = "background: " + asyncState.label + " | " + asyncState.progress;
                        if (asyncState.label == "refresh" && asyncState.progress.rfind("discover:", 0) == 0) {
                            backgroundText = "background: refresh fallback -> " + asyncState.progress;
                        }
                        return status_paragraph(backgroundText);
                    }());
        if (!history.active) {
            rootRows.push_back(paragraph("tree toggle key: t (collapse/expand child repos)") | kSecondaryStyle);
        }
        rootRows.push_back(separator());
        rootRows.push_back(mainPanel | flex);
        rootRows.push_back(separator());
        rootRows.push_back(vbox(std::move(commandRows)));
        rootRows.push_back(footerElement);

        auto rootElement = vbox(std::move(rootRows)) | border;

        return rootElement;
    });

    begin_async_refresh(false);

    screen.Loop(ui);
    finish_async_operation();
    std::cout << "\x1b[?1000l\x1b[?1002l\x1b[?1003l\x1b[?1005l\x1b[?1006l\x1b[?1015l" << std::flush;
    return 0;
}

auto FetchWorkingTreeDetail(const std::filesystem::path& repo, int mode) -> std::string {
    std::vector<std::string> args;
    if (mode == 1) {
        args = {"status", "--short", "--branch"};
    } else if (mode == 2) {
        args = {"diff", "--no-color", "HEAD"};
    } else {
        args = {"diff", "--no-color", "--stat", "HEAD"};
    }

    auto out = GitCapture(repo, args);
    if (out.exitCode != 0 && (mode == 0 || mode == 2)) {
        args = mode == 2
            ? std::vector<std::string>{"diff", "--no-color"}
            : std::vector<std::string>{"diff", "--no-color", "--stat"};
        out = GitCapture(repo, args);
    }
    if (out.exitCode != 0) {
        return "(failed to load working tree detail)\n" + out.stderrStr;
    }
    auto body = out.stdoutStr;
    if (Trim(body).empty()) {
        body = "working tree is clean";
    }
    constexpr std::size_t kMaxChars = 20000;
    if (body.size() > kMaxChars) {
        body = body.substr(0, kMaxChars) + "\n... (truncated)";
    }
    return body;
}

auto PrintDemo() -> void {
    std::cout << "KOG TUI demo mode\n";
    std::cout << "- FTXUI dashboard enabled\n";
    std::cout << "- repo list + details + incremental history pager\n";
    std::cout << "- controls: :(command mode), t(tree toggle), Enter(history), q(quit)\n";
    std::cout << "- cherry runner controls: n(next), c(continue), s(skip), a(abort), q(close panel)\n";
    std::cout << "- rebase runner controls: N(next), C(continue), S(skip), A(abort), q(close panel)\n";
    std::cout << "- command-first flow: : to enter command mode, then run :refresh, :discover, or :discover dirty\n";
    std::cout << "- discover panel controls: ]/PgUp next page, [/PgDown prev page, Esc/q close\n";
    std::cout << "- tree: t collapse/expand selected repo subtree\n";
    std::cout << "- history mode: left/right repo, PgUp/PgDn page, up/down line, /search, n-next, m-detail-mode, o-sort-mode\n";
    std::cout << "- quick stats shown for selected commit line (cached by sha)\n";
    std::cout << "Run `kano-git tui` to start the FTXUI UI.\n";
}

} // namespace

auto RunTuiDashboard(CLI::App& InApp) -> int {
    return RunFtxuiDashboard(InApp);
}

auto PrintTuiDemoSummary() -> void {
    PrintDemo();
}

} // namespace kano::git::commands
