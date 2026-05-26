// log-related commands: slog / uplog

#include <CLI/CLI.hpp>
#include "discovery.hpp"
#include "kog_config.hpp"
#include "shell_executor.hpp"
#include "terminal_color.hpp"

#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <format>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

namespace kano::git::commands {
namespace {

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
    std::vector<std::string> lines;
    std::istringstream iss(InText);
    std::string line;
    while (std::getline(iss, line)) {
        auto trimmed = Trim(line);
        if (trimmed.empty()) {
            continue;
        }
        lines.push_back(std::move(trimmed));
    }
    return lines;
}

auto ParsePositiveInt(const std::string& InValue, int InDefault) -> int {
    const auto value = Trim(InValue);
    if (value.empty()) {
        return InDefault;
    }
    int parsed = 0;
    const auto* begin = value.data();
    const auto* end = value.data() + value.size();
    const auto result = std::from_chars(begin, end, parsed);
    if (result.ec != std::errc{} || result.ptr != end || parsed <= 0) {
        return InDefault;
    }
    return parsed;
}

auto ParseNonNegativeInt(const std::string& InValue, int InDefault) -> int {
    const auto value = Trim(InValue);
    if (value.empty()) {
        return InDefault;
    }
    int parsed = 0;
    const auto* begin = value.data();
    const auto* end = value.data() + value.size();
    const auto result = std::from_chars(begin, end, parsed);
    if (result.ec != std::errc{} || result.ptr != end || parsed < 0) {
        return InDefault;
    }
    return parsed;
}

auto GitCapture(const std::filesystem::path& InRepo, const std::vector<std::string>& InArgs) -> shell::ExecResult {
    return shell::ExecuteCommand("git", InArgs, shell::ExecMode::Capture, InRepo);
}

auto IsGitRepo(const std::filesystem::path& InRepo) -> bool {
    return GitCapture(InRepo, {"rev-parse", "--git-dir"}).exitCode == 0;
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

auto ResolveRepoFromSpec(const std::filesystem::path& InRoot,
                         const std::string& InSpec,
                         int InMaxDepth,
                         bool InUseCache) -> std::filesystem::path {
    if (InSpec.empty()) {
        return std::filesystem::current_path().lexically_normal();
    }

    const std::filesystem::path asPath(InSpec);
    const auto candidate = (asPath.is_absolute() ? asPath : (InRoot / asPath)).lexically_normal();
    if (std::filesystem::exists(candidate) && IsGitRepo(candidate)) {
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

    std::vector<std::filesystem::path> matches = exactMatches.empty() ? fuzzyMatches : exactMatches;

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
        oss << "repo spec is ambiguous: " << InSpec << "\n";
        oss << "Matches:\n";
        for (const auto& match : matches) {
            oss << "  - " << match.generic_string() << "\n";
        }
        throw std::runtime_error(oss.str());
    }

    return matches.front();
}

struct RepoBranchRefs {
    std::string localBranch = "HEAD(detached)";
    std::string localCommit = "unknown";
    std::string upstreamBranch = "(no upstream)";
    std::string upstreamCommit = "-";
};

enum class RepoSyncState { Synced, Ahead, Behind, Diverged, Detached, NoUpstream };

struct RepoLogEntry {
    std::filesystem::path repoPath;
    std::string repoName;
    std::string group;
    std::string localBranch;
    std::string upstreamBranch;
    int aheadCount = 0;
    int behindCount = 0;
    RepoSyncState syncState = RepoSyncState::NoUpstream;
    std::vector<std::string> remoteLogLines;
    int remoteOmittedCount = 0;
    std::vector<std::string> logLines; // formatted with markers
    bool failed = false;
    bool dirty = false;  // has uncommitted changes
    bool conflicted = false;
};

struct RepoWorktreeState {
    bool dirty = false;
    bool conflicted = false;
};

auto ResolveSkillRoot(const std::filesystem::path& InWorkspaceRoot) -> std::filesystem::path {
    if (const char* envValue = std::getenv("KANO_GIT_SKILL_ROOT"); envValue != nullptr && *envValue != '\0') {
        return std::filesystem::path(envValue).lexically_normal();
    }

    auto current = InWorkspaceRoot.lexically_normal();
    for (int i = 0; i < 8 && !current.empty(); ++i) {
        const auto marker = (current / "SKILL.md").lexically_normal();
        std::error_code ec;
        if (std::filesystem::exists(marker, ec) && !ec && std::filesystem::is_regular_file(marker, ec)) {
            return current;
        }
        current = current.parent_path();
    }
    return {};
}

auto ResolveLogRemotePreviewCount(const std::filesystem::path& InWorkspaceRoot,
                                  int InFallback = 3) -> int {
    const auto value = kog_config::ReadEffectiveValue(
        InWorkspaceRoot,
        ResolveSkillRoot(InWorkspaceRoot),
        "log.remote_preview_count");
    return std::max(0, ParseNonNegativeInt(value, InFallback));
}

auto ExtractShaPrefix(const std::string& InLine) -> std::string {
    const auto trimmed = Trim(InLine);
    if (trimmed.empty()) {
        return {};
    }
    const auto firstSpace = trimmed.find(' ');
    if (firstSpace == std::string::npos) {
        return trimmed;
    }
    return trimmed.substr(0, firstSpace);
}

auto FirstParentRevisionCount(const std::filesystem::path& InRepo, const std::string& InRev) -> int {
    if (InRev.empty()) {
        return 0;
    }
    const auto out = GitCapture(InRepo, {"rev-list", "--count", "--first-parent", InRev});
    if (out.exitCode != 0) {
        return 0;
    }
    return ParsePositiveInt(out.stdoutStr, 0);
}

auto FormatLogLineWithMarkers(const std::filesystem::path& InRepo,
                              const RepoBranchRefs& InRefs,
                              const std::string& InLine) -> std::string {
    const auto trimmed = Trim(InLine);
    if (trimmed.empty()) {
        return {};
    }

    const auto sha = ExtractShaPrefix(trimmed);
    const auto revision = FirstParentRevisionCount(InRepo, sha);

    const bool isLocal = (!InRefs.localCommit.empty() && InRefs.localCommit != "unknown" && sha == InRefs.localCommit);
    const bool isRemote = (!InRefs.upstreamCommit.empty() && InRefs.upstreamCommit != "-" && sha == InRefs.upstreamCommit);

    std::ostringstream oss;
    if (isLocal && isRemote) {
        oss << kano::terminal::Wrap("SYNCED", kano::terminal::Color::BoldCyan) << " ";
    } else if (isLocal) {
        oss << kano::terminal::Wrap("LOCAL", kano::terminal::Color::BoldGreen) << " ";
    } else if (isRemote) {
        oss << kano::terminal::Wrap("REMOTE", kano::terminal::Color::BoldYellow) << " ";
    }

    if (revision > 0) {
        oss << kano::terminal::Wrap("[" + std::to_string(revision) + "] ", kano::terminal::Color::Dim);
    } else {
        oss << kano::terminal::Wrap("[?] ", kano::terminal::Color::Dim);
    }
    oss << trimmed;
    return oss.str();
}

auto FormatLogLineWithExplicitMarker(const std::filesystem::path& InRepo,
                                     const std::string& InMarker,
                                     const char* InColor,
                                     const std::string& InLine) -> std::string {
    const auto trimmed = Trim(InLine);
    if (trimmed.empty()) {
        return {};
    }

    const auto sha = ExtractShaPrefix(trimmed);
    const auto revision = FirstParentRevisionCount(InRepo, sha);

    std::ostringstream oss;
    oss << kano::terminal::Wrap(InMarker, InColor) << " ";
    if (revision > 0) {
        oss << kano::terminal::Wrap("[" + std::to_string(revision) + "] ", kano::terminal::Color::Dim);
    } else {
        oss << kano::terminal::Wrap("[?] ", kano::terminal::Color::Dim);
    }
    oss << trimmed;
    return oss.str();
}

auto ResolveRepoBranchRefs(const std::filesystem::path& InRepo) -> RepoBranchRefs {
    RepoBranchRefs refs;

    if (const auto branchOut = GitCapture(InRepo, {"symbolic-ref", "--short", "-q", "HEAD"}); branchOut.exitCode == 0) {
        const auto parsed = Trim(branchOut.stdoutStr);
        if (!parsed.empty()) {
            refs.localBranch = parsed;
        }
    }

    if (const auto headOut = GitCapture(InRepo, {"rev-parse", "--short", "HEAD"}); headOut.exitCode == 0) {
        const auto parsed = Trim(headOut.stdoutStr);
        if (!parsed.empty()) {
            refs.localCommit = parsed;
        }
    }

    if (const auto upstreamOut = GitCapture(InRepo, {"rev-parse", "--abbrev-ref", "--symbolic-full-name", "@{upstream}"});
        upstreamOut.exitCode == 0) {
        const auto parsed = Trim(upstreamOut.stdoutStr);
        if (!parsed.empty()) {
            refs.upstreamBranch = parsed;
            if (const auto upstreamCommitOut = GitCapture(InRepo, {"rev-parse", "--short", parsed}); upstreamCommitOut.exitCode == 0) {
                const auto parsedCommit = Trim(upstreamCommitOut.stdoutStr);
                if (!parsedCommit.empty()) {
                    refs.upstreamCommit = parsedCommit;
                }
            }
        }
    }

    return refs;
}

auto CollectRepoWorktreeState(const std::filesystem::path& InRepo) -> RepoWorktreeState {
    RepoWorktreeState state;
    const auto statusOut = GitCapture(InRepo, {"status", "--porcelain=v1"});
    if (statusOut.exitCode != 0) {
        return state;
    }

    for (const auto& line : SplitNonEmptyLines(statusOut.stdoutStr)) {
        if (line.size() < 2) {
            continue;
        }
        state.dirty = true;
        const char indexState = line[0];
        const char worktreeState = line[1];
        const bool unmerged = indexState == 'U' || worktreeState == 'U' ||
                              (indexState == 'A' && worktreeState == 'A') ||
                              (indexState == 'D' && worktreeState == 'D');
        if (unmerged) {
            state.conflicted = true;
        }
    }

    return state;
}

auto CollectRepoLogEntry(const std::filesystem::path& InRoot,
                         const std::filesystem::path& InRepo,
                         int InCount,
                         bool InSlogFormat,
                         int InRemotePreviewCount) -> RepoLogEntry {
    RepoLogEntry entry;
    entry.repoPath = InRepo;
    entry.repoName = RepoNameFromPath(InRepo);
    const auto rel = RelativeDisplayPath(InRoot, InRepo);
    entry.group = GroupFromRelativePath(rel);

    if (!IsGitRepo(InRepo)) {
        entry.failed = true;
        return entry;
    }

    const auto refs = ResolveRepoBranchRefs(InRepo);
    entry.localBranch   = refs.localBranch;
    entry.upstreamBranch = refs.upstreamBranch;
    const auto worktreeState = CollectRepoWorktreeState(InRepo);
    entry.dirty = worktreeState.dirty;
    entry.conflicted = worktreeState.conflicted;

    // Determine sync state
    const bool isDetached = (refs.localBranch == "HEAD(detached)");
    const bool hasUpstream = (refs.upstreamBranch != "(no upstream)");

    if (isDetached) {
        entry.syncState = RepoSyncState::Detached;
    } else if (!hasUpstream) {
        entry.syncState = RepoSyncState::NoUpstream;
    } else {
        const auto aheadOut  = GitCapture(InRepo, {"rev-list", "--count", "@{upstream}..HEAD"});
        const auto behindOut = GitCapture(InRepo, {"rev-list", "--count", "HEAD..@{upstream}"});
        entry.aheadCount  = (aheadOut.exitCode  == 0) ? ParsePositiveInt(aheadOut.stdoutStr,  0) : 0;
        entry.behindCount = (behindOut.exitCode == 0) ? ParsePositiveInt(behindOut.stdoutStr, 0) : 0;

        if (entry.aheadCount == 0 && entry.behindCount == 0) {
            entry.syncState = RepoSyncState::Synced;
        } else if (entry.aheadCount > 0 && entry.behindCount == 0) {
            entry.syncState = RepoSyncState::Ahead;
        } else if (entry.aheadCount == 0 && entry.behindCount > 0) {
            entry.syncState = RepoSyncState::Behind;
        } else {
            entry.syncState = RepoSyncState::Diverged;
        }
    }

    const auto fmt = InSlogFormat ? "--pretty=format:%h %an %s" : "--pretty=format:%h %an %s";

    if (hasUpstream && entry.behindCount > 0 && InRemotePreviewCount > 0) {
        const int remoteCount = std::min(InRemotePreviewCount, entry.behindCount);
        const auto remoteOut = GitCapture(InRepo, {"log", std::format("-{}", remoteCount), fmt, "HEAD..@{upstream}"});
        if (remoteOut.exitCode == 0) {
            for (const auto& line : SplitNonEmptyLines(remoteOut.stdoutStr)) {
                const auto formatted = FormatLogLineWithExplicitMarker(InRepo, "REMOTE", kano::terminal::Color::BoldYellow, line);
                if (!formatted.empty()) {
                    entry.remoteLogLines.push_back(formatted);
                }
            }
        }
        if (entry.behindCount > static_cast<int>(entry.remoteLogLines.size())) {
            entry.remoteOmittedCount = entry.behindCount - static_cast<int>(entry.remoteLogLines.size());
        }
    }

    if (entry.syncState == RepoSyncState::Behind && entry.aheadCount == 0) {
        const auto localHeadOut = GitCapture(InRepo, {"log", "-1", fmt, "HEAD"});
        if (localHeadOut.exitCode == 0) {
            for (const auto& line : SplitNonEmptyLines(localHeadOut.stdoutStr)) {
                const auto formatted = FormatLogLineWithExplicitMarker(InRepo, "LOCAL", kano::terminal::Color::BoldGreen, line);
                if (!formatted.empty()) {
                    entry.logLines.push_back(formatted);
                }
            }
        }
        return entry;
    }

    if (entry.syncState == RepoSyncState::Diverged && hasUpstream) {
        const auto localDivergedOut = GitCapture(InRepo, {"log", std::format("-{}", InCount), fmt, "@{upstream}..HEAD"});
        if (localDivergedOut.exitCode == 0) {
            for (const auto& line : SplitNonEmptyLines(localDivergedOut.stdoutStr)) {
                const auto formatted = FormatLogLineWithExplicitMarker(InRepo, "LOCAL", kano::terminal::Color::BoldGreen, line);
                if (!formatted.empty()) {
                    entry.logLines.push_back(formatted);
                }
            }
            return entry;
        }
    }

    const auto logOut = GitCapture(InRepo, {"log", std::format("-{}", InCount), fmt});
    if (logOut.exitCode == 0) {
        for (const auto& line : SplitNonEmptyLines(logOut.stdoutStr)) {
            const auto formatted = FormatLogLineWithMarkers(InRepo, refs, line);
            if (!formatted.empty()) {
                entry.logLines.push_back(formatted);
            }
        }
    }

    return entry;
}

auto PrintGroupedLog(const std::vector<RepoLogEntry>& InEntries, int InCount, bool InSlogFormat) -> void {
    const std::string header = InSlogFormat
        ? "SLOG(last " + std::to_string(InCount) + ")"
        : "LOG(last "  + std::to_string(InCount) + ")";

    std::string currentGroup;
    for (const auto& entry : InEntries) {
        if (entry.group != currentGroup) {
            currentGroup = entry.group;
            std::cout << "\n" << kano::terminal::Wrap("GROUP:", kano::terminal::Color::BoldWhite)
                      << " " << currentGroup << "\n";
        }

        // Repo header line: name + branch + sync badge
        std::ostringstream repoHeader;
        repoHeader << kano::terminal::Wrap(entry.repoName, kano::terminal::Color::BoldCyan);
        repoHeader << " [" << entry.localBranch << "]";
        if (entry.conflicted) {
            repoHeader << " " << kano::terminal::Wrap("conflict", kano::terminal::Color::BoldRed);
        }
        switch (entry.syncState) {
            case RepoSyncState::Synced:
                repoHeader << " " << kano::terminal::Wrap("synced", kano::terminal::Color::BoldGreen);
                break;
            case RepoSyncState::Ahead:
                repoHeader << " " << kano::terminal::Wrap(
                    "ahead " + std::to_string(entry.aheadCount), kano::terminal::Color::BoldYellow);
                break;
            case RepoSyncState::Behind:
                repoHeader << " " << kano::terminal::Wrap(
                    "behind " + std::to_string(entry.behindCount), kano::terminal::Color::BoldRed);
                break;
            case RepoSyncState::Diverged:
                repoHeader << " " << kano::terminal::Wrap(
                    "diverged +" + std::to_string(entry.aheadCount) +
                    "/-" + std::to_string(entry.behindCount), kano::terminal::Color::BoldRed);
                break;
            case RepoSyncState::Detached:
                repoHeader << " " << kano::terminal::Wrap("detached", kano::terminal::Color::Dim);
                break;
            case RepoSyncState::NoUpstream:
                repoHeader << " " << kano::terminal::Wrap("no-upstream", kano::terminal::Color::Dim);
                break;
        }
        std::cout << repoHeader.str() << "\n";

        if (entry.failed) {
            std::cout << kano::terminal::Wrap("  (not a git repo)", kano::terminal::Color::Dim) << "\n";
            continue;
        }
        if (entry.remoteLogLines.empty() && entry.logLines.empty()) {
            std::cout << kano::terminal::Wrap("  (no commits)", kano::terminal::Color::Dim) << "\n";
            continue;
        }

        for (const auto& line : entry.remoteLogLines) {
            std::cout << "  " << line << "\n";
        }
        if (entry.remoteOmittedCount > 0) {
            std::cout << kano::terminal::Wrap(
                "  ... " + std::to_string(entry.remoteOmittedCount) + " remote commits omitted",
                kano::terminal::Color::Dim) << "\n";
        }
        for (const auto& line : entry.logLines) {
            std::cout << "  " << line << "\n";
        }
    }
}

auto PrintLogSummary(const std::vector<RepoLogEntry>& InEntries) -> void {
    int synced = 0, ahead = 0, behind = 0, diverged = 0, detached = 0, noUpstream = 0, conflicted = 0;
    for (const auto& e : InEntries) {
        conflicted += e.conflicted ? 1 : 0;
        switch (e.syncState) {
            case RepoSyncState::Synced:     synced++;     break;
            case RepoSyncState::Ahead:      ahead++;      break;
            case RepoSyncState::Behind:     behind++;     break;
            case RepoSyncState::Diverged:   diverged++;   break;
            case RepoSyncState::Detached:   detached++;   break;
            case RepoSyncState::NoUpstream: noUpstream++; break;
        }
    }

    std::cout << "\n" << kano::terminal::Wrap("SUMMARY", kano::terminal::Color::BoldCyan)
              << "  repos=" << InEntries.size();

    if (synced > 0) {
        std::cout << "  " << kano::terminal::Wrap("synced=" + std::to_string(synced), kano::terminal::Color::BoldGreen);
    }
    if (ahead > 0) {
        std::cout << "  " << kano::terminal::Wrap("ahead=" + std::to_string(ahead), kano::terminal::Color::BoldYellow);
    }
    if (behind > 0) {
        std::cout << "  " << kano::terminal::Wrap("behind=" + std::to_string(behind), kano::terminal::Color::BoldRed);
    }
    if (diverged > 0) {
        std::cout << "  " << kano::terminal::Wrap("diverged=" + std::to_string(diverged), kano::terminal::Color::BoldRed);
    }
    if (conflicted > 0) {
        std::cout << "  " << kano::terminal::Wrap("conflicted=" + std::to_string(conflicted), kano::terminal::Color::BoldRed);
    }
    if (detached > 0) {
        std::cout << "  " << kano::terminal::Wrap("detached=" + std::to_string(detached), kano::terminal::Color::Dim);
    }
    if (noUpstream > 0) {
        std::cout << "  " << kano::terminal::Wrap("no-upstream=" + std::to_string(noUpstream), kano::terminal::Color::Dim);
    }
    std::cout << "\n";
}

auto PrintSlog(const std::filesystem::path& InRepo, int InCount) -> int {
    if (!IsGitRepo(InRepo)) {
        std::cerr << "Error: not a git repository: " << InRepo.generic_string() << "\n";
        return 1;
    }

    const auto logOut = GitCapture(InRepo, {"log", std::format("-{}", InCount), "--pretty=format:%h %an %s"});
    if (logOut.exitCode != 0) {
        std::cerr << "Error: failed to read log for repo: " << InRepo.generic_string() << "\n";
        return 1;
    }

    const auto refs = ResolveRepoBranchRefs(InRepo);

    std::cout << kano::terminal::Wrap("REPO:", kano::terminal::Color::BoldCyan) << " "
              << InRepo.lexically_normal().generic_string() << "\n";
    std::cout << kano::terminal::Wrap("SLOG(last " + std::to_string(InCount) + ")", kano::terminal::Color::BoldWhite) << "\n";

    const auto lines = SplitNonEmptyLines(logOut.stdoutStr);
    if (lines.empty()) {
        std::cout << kano::terminal::Wrap("[?]", kano::terminal::Color::BoldYellow) << " (no commits)\n";
        return 0;
    }

    for (const auto& line : lines) {
        const auto formatted = FormatLogLineWithMarkers(InRepo, refs, line);
        if (!formatted.empty()) {
            std::cout << formatted << "\n";
        }
    }
    return 0;
}

auto CollectSlogTargets(const std::filesystem::path& InRoot,
                        int InMaxDepth,
                        bool InUseCache,
                        bool InNoRecursive) -> std::vector<std::filesystem::path> {
    std::vector<std::filesystem::path> targets;
    if (InNoRecursive) {
        const auto cwd = std::filesystem::current_path().lexically_normal();
        if (IsGitRepo(cwd)) {
            targets.push_back(cwd);
        }
        return targets;
    }

    workspace::DiscoverOptions options;
    options.rootDir = InRoot;
    options.maxDepth = InMaxDepth;
    options.useCache = InUseCache;
    options.metadataLevel = "minimal";

    const auto discovery = workspace::DiscoverRepos(options);
    for (const auto& repo : discovery.repos) {
        const auto path = repo.path.lexically_normal();
        if (!IsGitRepo(path)) {
            continue;
        }
        targets.push_back(path);
    }

    std::sort(targets.begin(), targets.end(), [](const auto& A, const auto& B) {
        return A.generic_string() < B.generic_string();
    });
    targets.erase(std::unique(targets.begin(), targets.end(), [](const auto& A, const auto& B) {
        return A.generic_string() == B.generic_string();
    }), targets.end());

    return targets;
}

auto PrintFullLog(const std::filesystem::path& InRepo, int InCount) -> int {
    if (!IsGitRepo(InRepo)) {
        std::cerr << "Error: not a git repository: " << InRepo.generic_string() << "\n";
        return 1;
    }

    const auto logOut = GitCapture(InRepo, {"log", std::format("-{}", InCount), "--decorate=short", "--date=iso-strict", "--pretty=format:%h %an %s"});
    if (logOut.exitCode != 0) {
        std::cerr << "Error: failed to read log for repo: " << InRepo.generic_string() << "\n";
        return 1;
    }

    const auto refs = ResolveRepoBranchRefs(InRepo);
    std::cout << kano::terminal::Wrap("REPO:", kano::terminal::Color::BoldCyan) << " "
              << InRepo.lexically_normal().generic_string() << "\n";
    std::cout << kano::terminal::Wrap("LOG(last " + std::to_string(InCount) + ")", kano::terminal::Color::BoldWhite) << "\n";

    const auto lines = SplitNonEmptyLines(logOut.stdoutStr);
    if (lines.empty()) {
        std::cout << kano::terminal::Wrap("[?]", kano::terminal::Color::BoldYellow) << " (no commits)\n";
        return 0;
    }

    for (const auto& line : lines) {
        const auto formatted = FormatLogLineWithMarkers(InRepo, refs, line);
        if (!formatted.empty()) {
            std::cout << formatted << "\n";
        }
    }
    return 0;
}

struct UplogEntry {
    std::filesystem::path repoPath;
    std::string group;
    std::string repoName;
    std::string upstream;
    int aheadCount = 0;
    std::string shortLog;
};

auto CollectUplog(const std::filesystem::path& InRoot,
                 int InMaxDepth,
                 bool InUseCache,
                 bool InRefreshCache,
                 int InCount) -> std::vector<UplogEntry> {
    workspace::DiscoverOptions options;
    options.rootDir = InRoot;
    options.maxDepth = InMaxDepth;
    options.useCache = InUseCache;
    options.refreshCache = InRefreshCache;
    options.metadataLevel = "minimal";

    const auto discovery = workspace::DiscoverRepos(options);
    std::vector<UplogEntry> out;

    for (const auto& repo : discovery.repos) {
        const auto repoPath = repo.path.lexically_normal();
        if (!IsGitRepo(repoPath)) {
            continue;
        }

        const auto upstreamOut = GitCapture(repoPath, {"rev-parse", "--abbrev-ref", "--symbolic-full-name", "@{upstream}"});
        if (upstreamOut.exitCode != 0) {
            continue;
        }
        const auto upstream = Trim(upstreamOut.stdoutStr);
        if (upstream.empty()) {
            continue;
        }

        const auto aheadOut = GitCapture(repoPath, {"rev-list", "--count", "@{upstream}..HEAD"});
        if (aheadOut.exitCode != 0) {
            continue;
        }
        const auto aheadCount = ParsePositiveInt(aheadOut.stdoutStr, 0);
        if (aheadCount <= 0) {
            continue;
        }

        const auto logOut = GitCapture(repoPath, {"log", std::format("-{}", InCount), "--oneline", "@{upstream}..HEAD"});
        if (logOut.exitCode != 0) {
            continue;
        }

        UplogEntry entry;
        entry.repoPath = repoPath;
        const auto relative = RelativeDisplayPath(InRoot, repoPath);
        entry.group = GroupFromRelativePath(relative);
        entry.repoName = RepoNameFromPath(repoPath);
        entry.upstream = upstream;
        entry.aheadCount = aheadCount;
        entry.shortLog = Trim(logOut.stdoutStr);
        out.push_back(std::move(entry));
    }

    std::sort(out.begin(), out.end(), [](const UplogEntry& A, const UplogEntry& B) {
        if (A.group != B.group) {
            return A.group < B.group;
        }
        if (A.repoName != B.repoName) {
            return A.repoName < B.repoName;
        }
        return A.repoPath.generic_string() < B.repoPath.generic_string();
    });

    return out;
}

auto PrintUplog(const std::vector<UplogEntry>& InEntries) -> void {
    int totalAhead = 0;
    std::map<std::string, std::vector<const UplogEntry*>> grouped;
    for (const auto& entry : InEntries) {
        grouped[entry.group].push_back(&entry);
        totalAhead += entry.aheadCount;
    }

    std::cout << kano::terminal::Wrap("SUMMARY:", kano::terminal::Color::BoldCyan)
              << " repos_with_unpushed=" << InEntries.size() << ", commits=" << totalAhead << ", groups=" << grouped.size() << "\n\n";

    std::size_t globalIndex = 0;
    for (const auto& [group, rows] : grouped) {
        std::cout << kano::terminal::Wrap("GROUP:", kano::terminal::Color::BoldWhite) << " " << group << "\n";
        for (const auto* row : rows) {
            globalIndex += 1;
            std::cout << "[" << globalIndex << "] "
                      << kano::terminal::Wrap(row->repoName, kano::terminal::Color::BoldCyan)
                      << "  (ahead " << row->aheadCount << ", upstream " << row->upstream << ")\n";
            if (row->shortLog.empty()) {
                std::cout << kano::terminal::Wrap("  (no commits)", kano::terminal::Color::Dim) << "\n";
            } else {
                std::istringstream iss(row->shortLog);
                std::string line;
                while (std::getline(iss, line)) {
                    if (line.empty()) {
                        continue;
                    }
                    std::cout << "  " << line << "\n";
                }
            }
        }
        std::cout << "\n";
    }

    if (InEntries.empty()) {
        std::cout << kano::terminal::Wrap("No unpushed commits found.", kano::terminal::Color::Dim) << "\n";
    }
}

} // namespace

void RegisterSlog(CLI::App& InApp) {
    auto* cmd = InApp.add_subcommand("slog", "Show short logs recursively by default (sha author subject + local/upstream refs)");
    auto* count = new int{3};
    auto* repo = new std::string{};
    auto* countPos = new std::string{};
    auto* repoPos = new std::string{};
    auto* root = new std::string{"."};
    auto* maxDepth = new int{8};
    auto* noCache = new bool{false};
    auto* noRecursive = new bool{false};
    auto* remoteCount = new int{-1};
    auto* noRemotePreview = new bool{false};

    cmd->add_option("--count,-n", *count, "Number of commits to show");
    cmd->add_option("--repo", *repo, "Repo path or repo name");
    cmd->add_option("count_pos", *countPos, "Positional count (e.g. slog 20)");
    cmd->add_option("repo_spec", *repoPos, "Positional repo path/name (e.g. slog 20 kano)");
    cmd->add_option("--repo-root", *root, "Workspace root used for repo-name lookup");
    cmd->add_option("--max-depth", *maxDepth, "Discovery max depth for repo-name lookup");
    cmd->add_flag("--no-cache", *noCache, "Disable discovery cache for repo-name lookup");
    cmd->add_flag("--no-recursive,-N", *noRecursive, "Disable recursive discovery and show current repo only (when --repo is not provided)");
    cmd->add_option("--remote-count", *remoteCount, "Remote-only preview lines when behind/diverged (default: layered config or 3)");
    cmd->add_flag("--no-remote-preview", *noRemotePreview, "Disable remote-only preview lines (equivalent to --remote-count 0)");

    cmd->callback([=]() {
        if (!countPos->empty()) {
            const auto parsed = ParsePositiveInt(*countPos, -1);
            if (parsed > 0) {
                *count = parsed;
            } else if (repoPos->empty() && repo->empty()) {
                *repoPos = *countPos;
            }
        }
        if (*count <= 0) {
            std::cerr << "Error: --count must be a positive integer\n";
            std::exit(1);
        }

        const auto rootPath = std::filesystem::path(*root).lexically_normal();
        int effectiveRemoteCount = *remoteCount >= 0 ? *remoteCount : ResolveLogRemotePreviewCount(rootPath, 3);
        if (*noRemotePreview) {
            effectiveRemoteCount = 0;
        }
        if (effectiveRemoteCount < 0) {
            std::cerr << "Error: --remote-count must be >= 0\n";
            std::exit(1);
        }

        std::string repoSpec = *repo;
        if (repoSpec.empty() && !repoPos->empty()) {
            repoSpec = *repoPos;
        }

        try {
            if (!repoSpec.empty()) {
                const auto repoPath = ResolveRepoFromSpec(rootPath, repoSpec, *maxDepth, !*noCache);
                std::vector<RepoLogEntry> entries;
                entries.push_back(CollectRepoLogEntry(rootPath, repoPath, *count, true, effectiveRemoteCount));
                PrintGroupedLog(entries, *count, true);
                PrintLogSummary(entries);
                std::exit(entries.front().failed ? 1 : 0);
            }

            const auto targets = CollectSlogTargets(rootPath, *maxDepth, !*noCache, *noRecursive);
            if (targets.empty()) {
                std::cerr << "Error: no git repositories found for slog target scope\n";
                std::exit(1);
            }

            std::vector<RepoLogEntry> entries;
            entries.reserve(targets.size());
            int failed = 0;
            for (const auto& target : targets) {
                auto entry = CollectRepoLogEntry(rootPath, target, *count, true, effectiveRemoteCount);
                if (entry.failed) failed++;
                entries.push_back(std::move(entry));
            }
            PrintGroupedLog(entries, *count, true);
            PrintLogSummary(entries);
            std::exit(failed == 0 ? 0 : 1);
        } catch (const std::exception& ex) {
            std::cerr << "Error: " << ex.what() << "\n";
            std::exit(1);
        }
    });
}

void RegisterLog(CLI::App& InApp) {
    auto* cmd = InApp.add_subcommand("log", "Show one-line commit logs recursively by default (LOCAL/REMOTE markers + first-parent revision)");
    auto* count = new int{3};
    auto* repo = new std::string{};
    auto* countPos = new std::string{};
    auto* repoPos = new std::string{};
    auto* root = new std::string{"."};
    auto* maxDepth = new int{8};
    auto* noCache = new bool{false};
    auto* noRecursive = new bool{false};
    auto* remoteCount = new int{-1};
    auto* noRemotePreview = new bool{false};

    cmd->add_option("--count,-n", *count, "Number of commits to show");
    cmd->add_option("--repo", *repo, "Repo path or repo name");
    cmd->add_option("count_pos", *countPos, "Positional count (e.g. log 20)");
    cmd->add_option("repo_spec", *repoPos, "Positional repo path/name (e.g. log 20 kano)");
    cmd->add_option("--repo-root", *root, "Workspace root used for repo-name lookup");
    cmd->add_option("--max-depth", *maxDepth, "Discovery max depth for repo-name lookup");
    cmd->add_flag("--no-cache", *noCache, "Disable discovery cache for repo-name lookup");
    cmd->add_flag("--no-recursive,-N", *noRecursive, "Disable recursive discovery and show current repo only (when --repo is not provided)");
    cmd->add_option("--remote-count", *remoteCount, "Remote-only preview lines when behind/diverged (default: layered config or 3)");
    cmd->add_flag("--no-remote-preview", *noRemotePreview, "Disable remote-only preview lines (equivalent to --remote-count 0)");

    cmd->callback([=]() {
        if (!countPos->empty()) {
            const auto parsed = ParsePositiveInt(*countPos, -1);
            if (parsed > 0) {
                *count = parsed;
            } else if (repoPos->empty() && repo->empty()) {
                *repoPos = *countPos;
            }
        }
        if (*count <= 0) {
            std::cerr << "Error: --count must be a positive integer\n";
            std::exit(1);
        }

        const auto rootPath = std::filesystem::path(*root).lexically_normal();
        int effectiveRemoteCount = *remoteCount >= 0 ? *remoteCount : ResolveLogRemotePreviewCount(rootPath, 3);
        if (*noRemotePreview) {
            effectiveRemoteCount = 0;
        }
        if (effectiveRemoteCount < 0) {
            std::cerr << "Error: --remote-count must be >= 0\n";
            std::exit(1);
        }

        std::string repoSpec = *repo;
        if (repoSpec.empty() && !repoPos->empty()) {
            repoSpec = *repoPos;
        }

        try {
            if (!repoSpec.empty()) {
                const auto repoPath = ResolveRepoFromSpec(rootPath, repoSpec, *maxDepth, !*noCache);
                std::vector<RepoLogEntry> entries;
                entries.push_back(CollectRepoLogEntry(rootPath, repoPath, *count, false, effectiveRemoteCount));
                PrintGroupedLog(entries, *count, false);
                PrintLogSummary(entries);
                std::exit(entries.front().failed ? 1 : 0);
            }

            const auto targets = CollectSlogTargets(rootPath, *maxDepth, !*noCache, *noRecursive);
            if (targets.empty()) {
                std::cerr << "Error: no git repositories found for log target scope\n";
                std::exit(1);
            }

            std::vector<RepoLogEntry> entries;
            entries.reserve(targets.size());
            int failed = 0;
            for (const auto& target : targets) {
                auto entry = CollectRepoLogEntry(rootPath, target, *count, false, effectiveRemoteCount);
                if (entry.failed) failed++;
                entries.push_back(std::move(entry));
            }
            PrintGroupedLog(entries, *count, false);
            PrintLogSummary(entries);
            std::exit(failed == 0 ? 0 : 1);
        } catch (const std::exception& ex) {
            std::cerr << "Error: " << ex.what() << "\n";
            std::exit(1);
        }
    });
}

void RegisterUplog(CLI::App& InApp) {
    auto* cmd = InApp.add_subcommand("uplog", "Show unpushed local commit short logs across repositories");

    auto* count = new int{10};
    auto* root = new std::string{"."};
    auto* maxDepth = new int{8};
    auto* noCache = new bool{false};
    auto* noRefreshCache = new bool{false};

    auto configure = [&](CLI::App* InCmd) {
        InCmd->add_option("--count,-n", *count, "Max commits per repo");
        InCmd->add_option("--repo-root", *root, "Workspace root path");
        InCmd->add_option("--max-depth", *maxDepth, "Discovery max depth");
        InCmd->add_flag("--no-cache", *noCache, "Disable discovery cache");
        InCmd->add_flag("--no-refresh-cache", *noRefreshCache, "Do not force cache refresh");
    };

    configure(cmd);

    auto run = [=]() {
        if (*count <= 0) {
            std::cerr << "Error: --count must be a positive integer\n";
            std::exit(1);
        }
        const auto entries = CollectUplog(std::filesystem::path(*root), *maxDepth, !*noCache, !*noRefreshCache, *count);
        PrintUplog(entries);
    };

    cmd->callback(run);
}

} // namespace kano::git::commands
