// tui command — FTXUI dashboard with incremental history pager

#include "command_registry.hpp"
#include "discovery.hpp"
#include "shell_executor.hpp"

#include <ftxui/component/component.hpp>
#include <ftxui/component/component_options.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <iostream>
#include <optional>
#include <map>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace kano::git::commands {
namespace {

constexpr int kHistoryPageSize = 20;

struct RepoView {
    std::filesystem::path path;
    std::string type;
    std::string branch;
    std::string upstream;
    std::string tracking;
    bool repoDirty = false;
    bool worktreeDirty = false;
    std::string dirtyWorktrees;
    std::vector<std::string> dirtyFiles;
    std::string parentRepo;
    int childRepoCount = 0;
    int treeDepth = 0;
};

struct RepoHistoryCache {
    std::map<int, std::vector<std::string>> pages;
    std::optional<int> totalCommits;
    std::unordered_map<std::string, std::string> commitDetails;
    std::unordered_map<std::string, std::string> commitQuickStats;
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
    int detailMode = 0; // 0=summary, 1=files, 2=patch
    int sortMode = 0;   // 0=time-desc, 1=time-asc, 2=match-first
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

auto DirtyFiles(const std::filesystem::path& InRepo) -> std::vector<std::string> {
    const auto out = GitCapture(InRepo, {"status", "--porcelain"});
    if (out.exitCode != 0) {
        return {};
    }
    std::istringstream iss(out.stdoutStr);
    std::string line;
    std::vector<std::string> files;
    while (std::getline(iss, line)) {
        line = Trim(line);
        if (line.size() >= 4) {
            files.push_back(line.substr(3));
        }
    }
    return files;
}

auto IsAncestorPath(const std::filesystem::path& Ancestor, const std::filesystem::path& Child) -> bool {
    auto a = Ancestor.lexically_normal();
    auto c = Child.lexically_normal();
    if (a == c) {
        return false;
    }
    auto ai = a.begin();
    auto ci = c.begin();
    for (; ai != a.end() && ci != c.end(); ++ai, ++ci) {
        if (*ai != *ci) {
            return false;
        }
    }
    return ai == a.end();
}

auto DiscoverRepoViews(const bool InDirtyOnly) -> std::vector<RepoView> {
    workspace::DiscoverOptions options;
    options.rootDir = std::filesystem::current_path();
    options.maxDepth = 3;
    options.useCache = true;
    options.metadataLevel = "full";

    const auto discovery = workspace::DiscoverRepos(options);
    std::vector<RepoView> rows;
    rows.reserve(discovery.repos.size());

    for (const auto& repo : discovery.repos) {
        if (InDirtyOnly && !repo.hasChanges) {
            continue;
        }
        RepoView row;
        row.path = repo.path;
        row.type = repo.type;
        row.repoDirty = repo.hasChanges;
        row.branch = CurrentBranch(repo.path);
        row.upstream = CurrentUpstream(repo.path);
        row.tracking = TrackingSummary(repo.path);
        row.worktreeDirty = HasDirtyWorktrees(repo.path, row.dirtyWorktrees);
        row.dirtyFiles = DirtyFiles(repo.path);
        rows.push_back(std::move(row));
    }

    std::sort(rows.begin(), rows.end(), [](const RepoView& A, const RepoView& B) {
        return A.path.lexically_normal().generic_string() < B.path.lexically_normal().generic_string();
    });

    for (std::size_t i = 0; i < rows.size(); ++i) {
        std::filesystem::path bestParent;
        for (std::size_t j = 0; j < rows.size(); ++j) {
            if (i == j) {
                continue;
            }
            if (IsAncestorPath(rows[j].path, rows[i].path)) {
                if (bestParent.empty() || rows[j].path.generic_string().size() > bestParent.generic_string().size()) {
                    bestParent = rows[j].path;
                }
            }
        }
        rows[i].parentRepo = bestParent.empty() ? "(none)" : bestParent.lexically_normal().generic_string();
    }

    for (auto& row : rows) {
        int depth = 0;
        std::string cursor = row.parentRepo;
        while (cursor != "(none)") {
            depth += 1;
            std::string next = "(none)";
            for (const auto& maybeParent : rows) {
                if (maybeParent.path.lexically_normal().generic_string() == cursor) {
                    next = maybeParent.parentRepo;
                    break;
                }
            }
            cursor = next;
        }
        row.treeDepth = depth;
    }

    for (auto& row : rows) {
        row.childRepoCount = 0;
    }
    for (auto& row : rows) {
        if (row.parentRepo == "(none)") {
            continue;
        }
        for (auto& maybeParent : rows) {
            if (maybeParent.path.lexically_normal().generic_string() == row.parentRepo) {
                maybeParent.childRepoCount += 1;
                break;
            }
        }
    }

    return rows;
}

auto FetchHistoryPage(const std::filesystem::path& InRepo, const int InPageIndex) -> std::vector<std::string> {
    const auto skip = std::to_string(InPageIndex * kHistoryPageSize);
    const auto count = std::to_string(kHistoryPageSize);
    const auto out = GitCapture(InRepo, {"log", "--oneline", "--no-decorate", "--skip", skip, "-n", count});
    if (out.exitCode != 0) {
        return {"(failed to load history page)"};
    }
    std::istringstream iss(out.stdoutStr);
    std::string line;
    std::vector<std::string> lines;
    while (std::getline(iss, line)) {
        line = Trim(line);
        if (!line.empty()) {
            lines.push_back(line);
        }
    }
    if (lines.empty()) {
        lines.push_back("(no commits in this page)");
    }
    return lines;
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
        args = {"show", "--no-color", "--date=iso", "--name-status", "--pretty=fuller", "-n", "1", sha};
    } else if (mode == 2) {
        args = {"show", "--no-color", "--date=iso", "--pretty=fuller", "-n", "1", sha};
    } else {
        args = {"show", "--no-color", "--date=iso", "--stat", "--name-status", "--pretty=fuller", "-n", "1", sha};
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
                if (parent.path.lexically_normal().generic_string() == cursor) {
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

auto RunFtxuiDashboard() -> int {
    using namespace ftxui;

    bool dirtyOnly = false;
    std::vector<RepoView> repos = DiscoverRepoViews(dirtyOnly);
    std::unordered_map<std::string, bool> collapsedRoots;
    std::vector<int> displayedRepoIndices;
    std::vector<std::string> menu;
    int selectedDisplayed = 0;
    std::string footer = "r=refresh d=dirty-only f=fetch Enter=history q=quit";
    HistoryState history{};
    std::unordered_map<std::string, RepoHistoryCache> historyCache;

    auto refresh_menu = [&] {
        displayedRepoIndices = BuildDisplayedRepoIndices(repos, collapsedRoots);
        menu.clear();
        menu.reserve(displayedRepoIndices.size());
        for (const auto idx : displayedRepoIndices) {
            const auto& repo = repos[idx];
            auto path = repo.path.lexically_normal().generic_string();
            if (path.size() > 64) {
                path = "..." + path.substr(path.size() - 61);
            }
            std::string indent(static_cast<std::size_t>(repo.treeDepth * 2), ' ');
            const bool collapsed = collapsedRoots[repo.path.lexically_normal().generic_string()];
            const std::string marker = repo.childRepoCount > 0 ? (collapsed ? "[+] " : "[-] ") : "    ";
            menu.push_back(indent + marker + (repo.repoDirty ? "* " : "  ") + repo.branch + " | " + path);
        }
        if (menu.empty()) {
            menu.push_back("(no repositories found)");
            selectedDisplayed = 0;
        } else if (selectedDisplayed >= static_cast<int>(menu.size())) {
            selectedDisplayed = static_cast<int>(menu.size()) - 1;
        }
    };

    auto refresh_all = [&] {
        repos = DiscoverRepoViews(dirtyOnly);
        refresh_menu();
        historyCache.clear();
    };

    refresh_menu();

    auto ensure_history_loaded = [&](const int RepoIndex, const int PageIndex) {
        if (RepoIndex < 0 || RepoIndex >= static_cast<int>(repos.size()) || PageIndex < 0) {
            return;
        }
        const auto key = repos[RepoIndex].path.lexically_normal().generic_string();
        auto& cache = historyCache[key];
        if (cache.pages.find(PageIndex) == cache.pages.end()) {
            cache.pages[PageIndex] = FetchHistoryPage(repos[RepoIndex].path, PageIndex);
        }
        if (!cache.totalCommits.has_value()) {
            cache.totalCommits = FetchTotalCommitCount(repos[RepoIndex].path);
        }
    };

    auto current_history_lines = [&]() -> std::vector<std::string> {
        if (!history.active || repos.empty() || history.repoIndex < 0 || history.repoIndex >= static_cast<int>(repos.size())) {
            return {};
        }
        ensure_history_loaded(history.repoIndex, history.pageIndex);
        const auto key = repos[history.repoIndex].path.lexically_normal().generic_string();
        auto itCache = historyCache.find(key);
        if (itCache == historyCache.end()) {
            return {};
        }
        auto itPage = itCache->second.pages.find(history.pageIndex);
        if (itPage == itCache->second.pages.end()) {
            return {};
        }
        return itPage->second;
    };

    auto screen = ScreenInteractive::FitComponent();
    auto list = Menu(&menu, &selectedDisplayed);
    auto refresh_button = Button("Refresh", [&] {
        refresh_all();
        footer = "refreshed";
    });
    auto root = Container::Vertical({list, refresh_button});

    auto with_keys = CatchEvent(root, [&](Event event) {
        if (event == Event::Character('q')) {
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
        }

        if (event == Event::Character('r')) {
            refresh_all();
            footer = "refreshed";
            return true;
        }

        if (event == Event::Character('d')) {
            dirtyOnly = !dirtyOnly;
            refresh_all();
            footer = dirtyOnly ? "dirty-only enabled" : "dirty-only disabled";
            return true;
        }

        if (event == Event::Character('f')) {
            const int selected = RepoIndexFromDisplayed(displayedRepoIndices, selectedDisplayed);
            if (repos.empty() || selected < 0 || selected >= static_cast<int>(repos.size())) {
                footer = "fetch skipped: no selected repo";
                return true;
            }
            const auto result = shell::ExecuteCommand("git", {"fetch", "--all", "--prune"}, shell::ExecMode::Capture, repos[selected].path);
            footer = result.exitCode == 0
                ? "fetch ok: " + repos[selected].path.filename().string()
                : "fetch failed(" + std::to_string(result.exitCode) + "): " + repos[selected].path.filename().string();
            refresh_all();
            return true;
        }

        if (event == Event::Return || event == Event::Character('\n')) {
            const int selected = RepoIndexFromDisplayed(displayedRepoIndices, selectedDisplayed);
            if (repos.empty() || selected < 0 || selected >= static_cast<int>(repos.size())) {
                footer = "history skipped: no selected repo";
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
            const auto key = repos[selected].path.lexically_normal().generic_string();
            if (repos[selected].childRepoCount > 0) {
                collapsedRoots[key] = !collapsedRoots[key];
                refresh_menu();
                footer = collapsedRoots[key] ? "tree collapsed" : "tree expanded";
            } else {
                footer = "selected repo has no child repos";
            }
            return true;
        }

        if (history.active) {
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
                const auto key = repos[history.repoIndex].path.lexically_normal().generic_string();
                const auto& page = historyCache[key].pages[history.pageIndex];
                if (page.empty()) {
                    footer = "history detail skipped: page empty";
                    return true;
                }

                const int line = std::clamp(history.selectedLine, 0, static_cast<int>(page.size()) - 1);
                const auto sha = CommitShaFromOneline(page[line]);
                if (sha.empty() || sha[0] == '(') {
                    footer = "history detail skipped: invalid sha";
                    return true;
                }

                auto& cache = historyCache[key];
                const auto detailKey = sha + "|" + std::to_string(history.detailMode);
                auto it = cache.commitDetails.find(detailKey);
                if (it == cache.commitDetails.end()) {
                    cache.commitDetails[detailKey] = FetchCommitDetail(repos[history.repoIndex].path, sha, history.detailMode);
                }
                history.detailActive = true;
                history.detailSha = sha;
                history.detailBody = cache.commitDetails[detailKey];
                footer = "history detail opened: " + sha;
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
                footer = history.sortMode == 0 ? "history sort: time-desc" : (history.sortMode == 1 ? "history sort: time-asc" : "history sort: match-first");
                return true;
            }

            if (event == Event::Character('m')) {
                history.detailMode = (history.detailMode + 1) % 3;
                if (history.detailActive && !history.detailSha.empty()) {
                    const auto key = repos[history.repoIndex].path.lexically_normal().generic_string();
                    auto& cache = historyCache[key];
                    const auto detailKey = history.detailSha + "|" + std::to_string(history.detailMode);
                    auto it = cache.commitDetails.find(detailKey);
                    if (it == cache.commitDetails.end()) {
                        cache.commitDetails[detailKey] = FetchCommitDetail(repos[history.repoIndex].path, history.detailSha, history.detailMode);
                    }
                    history.detailBody = cache.commitDetails[detailKey];
                }
                footer = history.detailMode == 0 ? "detail mode: summary" : (history.detailMode == 1 ? "detail mode: files" : "detail mode: patch");
                return true;
            }

            if (event == Event::ArrowUp || event == Event::Character('k')) {
                ensure_history_loaded(history.repoIndex, history.pageIndex);
                const auto key = repos[history.repoIndex].path.lexically_normal().generic_string();
                const auto& page = historyCache[key].pages[history.pageIndex];
                if (!page.empty()) {
                    history.selectedLine = std::max(0, history.selectedLine - 1);
                    footer = "history line up";
                    return true;
                }
            }
            if (event == Event::ArrowDown || event == Event::Character('j')) {
                ensure_history_loaded(history.repoIndex, history.pageIndex);
                const auto key = repos[history.repoIndex].path.lexically_normal().generic_string();
                const auto& page = historyCache[key].pages[history.pageIndex];
                if (!page.empty()) {
                    history.selectedLine = std::min(static_cast<int>(page.size()) - 1, history.selectedLine + 1);
                    footer = "history line down";
                    return true;
                }
            }

            if (event == Event::ArrowLeft || event == Event::Character('h')) {
                if (!repos.empty()) {
                    history.repoIndex = (history.repoIndex - 1 + static_cast<int>(repos.size())) % static_cast<int>(repos.size());
                    history.pageIndex = 0;
                    history.selectedLine = 0;
                    history.highlightedLine = -1;
                    history.detailActive = false;
                    ensure_history_loaded(history.repoIndex, history.pageIndex);
                    footer = "history repo <-";
                }
                return true;
            }
            if (event == Event::ArrowRight || event == Event::Character('l')) {
                if (!repos.empty()) {
                    history.repoIndex = (history.repoIndex + 1) % static_cast<int>(repos.size());
                    history.pageIndex = 0;
                    history.selectedLine = 0;
                    history.highlightedLine = -1;
                    history.detailActive = false;
                    ensure_history_loaded(history.repoIndex, history.pageIndex);
                    footer = "history repo ->";
                }
                return true;
            }
            if (event == Event::PageUp || event == Event::Character('K')) {
                history.pageIndex += 1;
                history.selectedLine = 0;
                history.highlightedLine = -1;
                history.detailActive = false;
                ensure_history_loaded(history.repoIndex, history.pageIndex);
                footer = "history page older";
                return true;
            }
            if (event == Event::PageDown || event == Event::Character('J')) {
                if (history.pageIndex > 0) {
                    history.pageIndex -= 1;
                    history.selectedLine = 0;
                    history.highlightedLine = -1;
                    history.detailActive = false;
                    ensure_history_loaded(history.repoIndex, history.pageIndex);
                    footer = "history page newer";
                }
                return true;
            }
        }

        return false;
    });

    auto ui = Renderer(with_keys, [&] {
        Element rightPanel;

        if (history.active && !repos.empty()) {
            const auto& repo = repos[history.repoIndex];
            const auto key = repo.path.lexically_normal().generic_string();
            ensure_history_loaded(history.repoIndex, history.pageIndex);
            const auto& cache = historyCache[key];
            const auto it = cache.pages.find(history.pageIndex);
            auto lines = (it != cache.pages.end()) ? it->second : std::vector<std::string>{"(no data)"};

            if (history.sortMode == 1) {
                std::reverse(lines.begin(), lines.end());
            } else if (history.sortMode == 2 && !history.searchQuery.empty()) {
                std::stable_sort(lines.begin(), lines.end(), [&](const std::string& a, const std::string& b) {
                    const bool am = ToLowerAscii(a).find(ToLowerAscii(history.searchQuery)) != std::string::npos;
                    const bool bm = ToLowerAscii(b).find(ToLowerAscii(history.searchQuery)) != std::string::npos;
                    return am > bm;
                });
            }

            std::string totalPages = "?";
            if (cache.totalCommits.has_value()) {
                const int pages = std::max(1, (cache.totalCommits.value() + kHistoryPageSize - 1) / kHistoryPageSize);
                totalPages = std::to_string(pages);
            }

            rightPanel = vbox({
                text("History Pager") | bold,
                separator(),
                text("repo path: " + repo.path.lexically_normal().generic_string()),
                text("parent repo: " + repo.parentRepo),
                text("child repos: " + std::to_string(repo.childRepoCount)),
                text("repo index: " + std::to_string(history.repoIndex + 1) + "/" + std::to_string(repos.size())),
                text("page index: " + std::to_string(history.pageIndex + 1) + "/" + totalPages),
                text("line index: " + std::to_string(std::max(0, history.selectedLine) + 1) + "/" + std::to_string(lines.size())),
                text("search: " + (history.searchQuery.empty() ? "(none)" : "/" + history.searchQuery)),
                text("detail mode: " + std::string(history.detailMode == 0 ? "summary" : (history.detailMode == 1 ? "files" : "patch"))),
                text("sort mode: " + std::string(history.sortMode == 0 ? "time-desc" : (history.sortMode == 1 ? "time-asc" : "match-first"))),
                separator(),
                text("controls: up/down line, PgUp/PgDn page, left/right repo, Enter detail, m(mode), o(sort), /(search), n(next), q close") | dim,
                separator(),
                vbox([&] {
                    Elements rows;
                    for (std::size_t i = 0; i < lines.size(); ++i) {
                        const bool isHit = history.highlightedLine == static_cast<int>(i);
                        const bool isSelected = history.selectedLine == static_cast<int>(i);
                        auto prefix = isSelected ? "> " : "  ";
                        auto row = text(prefix + lines[i]);
                        if (isHit) {
                            row = row | bold;
                        }
                        rows.push_back(row);
                    }
                    return rows;
                }()) | border,
                [&]() {
                    if (lines.empty()) {
                        return text("quick stats: (no commits)") | dim;
                    }
                    const int line = std::clamp(history.selectedLine, 0, static_cast<int>(lines.size()) - 1);
                    const auto sha = CommitShaFromOneline(lines[line]);
                    if (sha.empty() || sha[0] == '(') {
                        return text("quick stats: (invalid commit)") | dim;
                    }

                    auto& mutableCache = historyCache[key];
                    auto it = mutableCache.commitQuickStats.find(sha);
                    if (it == mutableCache.commitQuickStats.end()) {
                        mutableCache.commitQuickStats[sha] = FetchCommitQuickStats(repo.path, sha);
                        it = mutableCache.commitQuickStats.find(sha);
                    }
                    return text("quick stats: " + it->second) | dim;
                }(),
                history.detailActive
                    ? vbox({
                          separator(),
                          text("Commit Detail: " + history.detailSha + " (" + (history.detailMode == 0 ? std::string("summary") : (history.detailMode == 1 ? std::string("files") : std::string("patch"))) + ")") | bold,
                          paragraph(history.detailBody),
                      }) | border
                    : text(""),
            }) | border;
        } else if (!repos.empty() && RepoIndexFromDisplayed(displayedRepoIndices, selectedDisplayed) >= 0) {
            const int selected = RepoIndexFromDisplayed(displayedRepoIndices, selectedDisplayed);
            const auto& row = repos[selected];
            rightPanel = vbox({
                text("Repository Details") | bold,
                separator(),
                text("path: " + row.path.lexically_normal().generic_string()),
                text("parent repo: " + row.parentRepo),
                text("child repos: " + std::to_string(row.childRepoCount)),
                text("type: " + row.type),
                text("branch: " + row.branch),
                text("upstream: " + row.upstream),
                text("tracking: " + row.tracking),
                text(std::string("dirty: ") + (row.repoDirty ? "yes" : "no")),
                text(std::string("worktree dirty: ") + (row.worktreeDirty ? "yes" : "no")),
                text("dirty worktrees: " + (row.dirtyWorktrees.empty() ? "(none)" : row.dirtyWorktrees)),
                separator(),
                text("dirty files:") | bold,
                row.dirtyFiles.empty()
                    ? text("(none)")
                    : vbox([&] {
                          Elements lines;
                          const auto maxItems = std::min<std::size_t>(row.dirtyFiles.size(), 8);
                          for (std::size_t i = 0; i < maxItems; ++i) {
                              lines.push_back(text("- " + row.dirtyFiles[i]));
                          }
                          if (row.dirtyFiles.size() > maxItems) {
                              lines.push_back(text("... and " + std::to_string(row.dirtyFiles.size() - maxItems) + " more"));
                          }
                          return lines;
                      }()),
            }) | border;
        } else {
            rightPanel = vbox({
                text("Repository Details") | bold,
                separator(),
                text("No repositories to display."),
            }) | border;
        }

        return vbox({
                   text("KOG FTXUI Dashboard v2") | bold,
                   separator(),
                   text("Global repo view + incremental history pager."),
                   text("tree toggle key: t (collapse/expand child repos)") | dim,
                   separator(),
                   hbox({
                       vbox({
                           text("Repositories") | bold,
                           separator(),
                           list->Render() | border,
                       }) | flex,
                       separator(),
                       rightPanel | flex,
                   }) | flex,
                   separator(),
                   refresh_button->Render(),
                   separator(),
                   text(footer) | dim,
               }) |
               border;
    });

    screen.Loop(ui);
    return 0;
}

auto PrintDemo() -> void {
    std::cout << "KOG TUI demo mode\n";
    std::cout << "- FTXUI dashboard enabled\n";
    std::cout << "- repo list + details + incremental history pager\n";
    std::cout << "- controls: r(refresh), d(dirty-only), f(fetch), Enter(history), q(quit)\n";
    std::cout << "- tree: t collapse/expand selected repo subtree\n";
    std::cout << "- history mode: left/right repo, PgUp/PgDn page, up/down line, /search, n-next, m-detail-mode, o-sort-mode\n";
    std::cout << "- quick stats shown for selected commit line (cached by sha)\n";
    std::cout << "Run `kano-git tui` to start the FTXUI UI.\n";
}

} // namespace

void RegisterTui(CLI::App& InApp) {
    auto* cmd = InApp.add_subcommand("tui", "Launch interactive KOG terminal dashboard");

    auto* demo = new bool{false};
    cmd->add_flag("--demo", *demo, "Print demo summary and exit (non-interactive)");

    cmd->callback([demo]() {
        if (*demo) {
            PrintDemo();
            return;
        }
        std::exit(RunFtxuiDashboard());
    });
}

} // namespace kano::git::commands
