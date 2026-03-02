// sync command — Repository synchronization workflows
// Delegates to: scripts/commit-tools/sync/smart-sync*.sh

#include "command_registry.hpp"
#include "discovery.hpp"
#include "shell_executor.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <format>
#include <cstdlib>
#include <iostream>
#include <regex>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace kano::git::commands {
namespace {

struct SyncPlan {
    std::filesystem::path path;
    std::string type;
    std::string remote;
    std::string targetBranch;
    std::string branchSource;
};

enum class BranchMode {
    Default,
    StableDev,
};

struct GitmodulesBinding {
    std::filesystem::path root;
    std::string prefix;
};

enum class StableDevReportFormat {
    Compact,
    Table,
    Tsv,
    Json,
    Markdown,
};

struct StableDevSummaryRow {
    std::string repo;
    std::string result;
    std::string currentBranch;
    bool sameCommit{false};
    std::string latestUpstreamCommit;
    std::string latestStableCommit;
    std::string reason;
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

auto GitPassThrough(const std::filesystem::path& InRepo, const std::vector<std::string>& InArgs) -> shell::ExecResult {
    return shell::ExecuteCommand("git", InArgs, shell::ExecMode::PassThrough, InRepo);
}

auto TruncateText(const std::string& InText, std::size_t InMax) -> std::string {
    if (InText.size() <= InMax) {
        return InText;
    }
    if (InMax <= 3) {
        return InText.substr(0, InMax);
    }
    return InText.substr(0, InMax - 3) + "...";
}

auto HasRemote(const std::filesystem::path& InRepo, const std::string& InRemote) -> bool {
    if (InRemote.empty()) {
        return false;
    }
    const auto result = GitCapture(InRepo, {"remote", "get-url", InRemote});
    return result.exitCode == 0;
}

auto ResolveRemote(const std::filesystem::path& InRepo, const std::string& InPreferredRemote) -> std::string {
    if (!InPreferredRemote.empty() && HasRemote(InRepo, InPreferredRemote)) {
        return InPreferredRemote;
    }
    if (HasRemote(InRepo, "origin")) {
        return "origin";
    }
    if (HasRemote(InRepo, "upstream")) {
        return "upstream";
    }
    const auto remotes = GitCapture(InRepo, {"remote"});
    if (remotes.exitCode != 0) {
        return {};
    }
    std::istringstream iss(remotes.stdoutStr);
    std::string line;
    if (std::getline(iss, line)) {
        return Trim(line);
    }
    return {};
}

auto DetectRemoteDefaultBranch(const std::filesystem::path& InRepo, const std::string& InRemote) -> std::string {
    const auto remoteHead = GitCapture(InRepo, {"symbolic-ref", "--quiet", std::format("refs/remotes/{}/HEAD", InRemote)});
    if (remoteHead.exitCode == 0) {
        const auto ref = Trim(remoteHead.stdoutStr);
        const auto marker = std::format("refs/remotes/{}/", InRemote);
        if (ref.starts_with(marker) && ref.size() > marker.size()) {
            return ref.substr(marker.size());
        }
    }

    for (const std::string branch : {"main", "master", "dev", "develop", "trunk"}) {
        const auto exists = GitCapture(InRepo, {"show-ref", "--verify", "--quiet", std::format("refs/remotes/{}/{}", InRemote, branch)});
        if (exists.exitCode == 0) {
            return branch;
        }
    }
    return {};
}

auto CurrentBranch(const std::filesystem::path& InRepo) -> std::string {
    const auto result = GitCapture(InRepo, {"symbolic-ref", "--quiet", "--short", "HEAD"});
    if (result.exitCode != 0) {
        return {};
    }
    return Trim(result.stdoutStr);
}

auto HasRebaseInProgress(const std::filesystem::path& InRepo) -> bool {
    const auto rebaseMergePath = GitCapture(InRepo, {"rev-parse", "--git-path", "rebase-merge"});
    if (rebaseMergePath.exitCode == 0) {
        const auto path = Trim(rebaseMergePath.stdoutStr);
        if (!path.empty() && std::filesystem::exists(path)) {
            return true;
        }
    }

    const auto rebaseApplyPath = GitCapture(InRepo, {"rev-parse", "--git-path", "rebase-apply"});
    if (rebaseApplyPath.exitCode == 0) {
        const auto path = Trim(rebaseApplyPath.stdoutStr);
        if (!path.empty() && std::filesystem::exists(path)) {
            return true;
        }
    }

    return false;
}

auto RecoverRebaseState(const std::filesystem::path& InRepo, const std::string& InRepoName, bool InDryRun) -> bool {
    if (!HasRebaseInProgress(InRepo)) {
        return true;
    }

    if (InDryRun) {
        std::cout << "[DRY RUN] Detected in-progress rebase in " << InRepoName << "; would run: git rebase --abort\n";
        return true;
    }

    std::cout << "WARN: Detected in-progress rebase in " << InRepoName << "; attempting auto-recovery via rebase --abort\n";
    const auto abortRebase = GitPassThrough(InRepo, {"rebase", "--abort"});
    if (abortRebase.exitCode != 0) {
        std::cerr << "ERROR: Failed to recover rebase state for " << InRepoName << "\n";
        return false;
    }

    std::cout << "Recovered rebase state for " << InRepoName << "\n";
    return true;
}

auto LocalBranchExists(const std::filesystem::path& InRepo, const std::string& InBranch) -> bool {
    if (InBranch.empty()) {
        return false;
    }
    return GitCapture(InRepo, {"show-ref", "--verify", "--quiet", std::format("refs/heads/{}", InBranch)}).exitCode == 0;
}

auto RemoteBranchExists(const std::filesystem::path& InRepo, const std::string& InRemote, const std::string& InBranch) -> bool {
    if (InRemote.empty() || InBranch.empty()) {
        return false;
    }
    return GitCapture(InRepo, {"show-ref", "--verify", "--quiet", std::format("refs/remotes/{}/{}", InRemote, InBranch)}).exitCode == 0;
}

auto ParseStableDevReportFormat(const std::string& InValue) -> std::optional<StableDevReportFormat> {
    if (InValue == "compact") {
        return StableDevReportFormat::Compact;
    }
    if (InValue == "table") {
        return StableDevReportFormat::Table;
    }
    if (InValue == "tsv") {
        return StableDevReportFormat::Tsv;
    }
    if (InValue == "json") {
        return StableDevReportFormat::Json;
    }
    if (InValue == "markdown") {
        return StableDevReportFormat::Markdown;
    }
    return std::nullopt;
}

auto ParseBranchMode(const std::string& InValue) -> std::optional<BranchMode> {
    if (InValue == "default") {
        return BranchMode::Default;
    }
    if (InValue == "stable-dev") {
        return BranchMode::StableDev;
    }
    return std::nullopt;
}

auto IsAgentModeEnabled() -> bool {
    const char* value = std::getenv("KANO_AGENT_MODE");
    if (value == nullptr) {
        return false;
    }
    const std::string normalized = Trim(value);
    return normalized == "1" || normalized == "true" || normalized == "TRUE";
}

auto HasLongFlag(const std::vector<std::string>& InArgs, const std::string& InFlag) -> bool {
    for (const auto& arg : InArgs) {
        if (arg == InFlag || arg.starts_with(InFlag + "=")) {
            return true;
        }
    }
    return false;
}

auto JsonEscape(const std::string& InValue) -> std::string {
    std::string out;
    out.reserve(InValue.size() + 16);
    for (const char c : InValue) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out.push_back(c); break;
        }
    }
    return out;
}

auto ResolveLatestStableTag(const std::filesystem::path& InRepo) -> std::string {
    static const std::regex stableTagPattern(R"((release[-_/])?(v)?[0-9]+(\.[0-9]+){1,3}(\+[0-9A-Za-z.-]+)?)", std::regex::icase);
    const auto tags = GitCapture(InRepo, {"tag", "--list", "--sort=-version:refname"});
    if (tags.exitCode != 0) {
        return {};
    }

    std::istringstream iss(tags.stdoutStr);
    std::string line;
    while (std::getline(iss, line)) {
        line = Trim(line);
        if (line.empty()) {
            continue;
        }
        if (std::regex_match(line, stableTagPattern)) {
            return line;
        }
    }
    return {};
}

auto ResolveCommitSha(const std::filesystem::path& InRepo, const std::string& InRev) -> std::string {
    if (InRev.empty()) {
        return {};
    }
    const auto result = GitCapture(InRepo, {"rev-parse", InRev});
    if (result.exitCode != 0) {
        return {};
    }
    return Trim(result.stdoutStr);
}

auto ResolveCommitLine(const std::filesystem::path& InRepo, const std::string& InRev) -> std::string {
    if (InRev.empty()) {
        return "N/A";
    }
    const auto result = GitCapture(InRepo, {"show", "-s", "--format=%h | %cI | %an | %s", InRev});
    if (result.exitCode != 0) {
        return "N/A";
    }
    const auto line = Trim(result.stdoutStr);
    return line.empty() ? "N/A" : line;
}

auto ResolveGitmodulesBranch(const std::filesystem::path& InRoot, const std::string& InRelPath) -> std::string {
    const auto paths = GitCapture(InRoot, {"config", "-f", ".gitmodules", "--get-regexp", "^submodule\\..*\\.path$"});
    if (paths.exitCode != 0) {
        return {};
    }

    std::istringstream iss(paths.stdoutStr);
    std::string line;
    while (std::getline(iss, line)) {
        line = Trim(line);
        if (line.empty()) {
            continue;
        }
        const auto sp = line.find(' ');
        if (sp == std::string::npos || sp + 1 >= line.size()) {
            continue;
        }
        const auto key = line.substr(0, sp);
        const auto value = line.substr(sp + 1);
        if (value != InRelPath || !key.ends_with(".path")) {
            continue;
        }
        const auto prefix = key.substr(0, key.size() - 5);
        const auto branch = GitCapture(InRoot, {"config", "-f", ".gitmodules", "--get", prefix + ".branch"});
        if (branch.exitCode == 0) {
            return Trim(branch.stdoutStr);
        }
        return {};
    }

    return {};
}

auto ResolveStableRef(const std::filesystem::path& InRoot, const std::filesystem::path& InRepo, const std::string& InCurrentBranch, const std::string& InRelPath) -> std::string {
    if (InCurrentBranch.starts_with("branch_")) {
        return InCurrentBranch;
    }

    const auto gmBranch = ResolveGitmodulesBranch(InRoot, InRelPath);
    if (!gmBranch.empty()) {
        if (GitCapture(InRepo, {"show-ref", "--verify", "--quiet", "refs/heads/" + gmBranch}).exitCode == 0) {
            return gmBranch;
        }
        if (GitCapture(InRepo, {"show-ref", "--verify", "--quiet", "refs/remotes/origin/" + gmBranch}).exitCode == 0) {
            return "origin/" + gmBranch;
        }
    }

    return InCurrentBranch;
}

auto CollectSrcSubmodulePaths(const std::filesystem::path& InRoot) -> std::vector<std::string> {
    std::vector<std::string> out;
    const auto paths = GitCapture(InRoot, {"config", "-f", ".gitmodules", "--get-regexp", "^submodule\\..*\\.path$"});
    if (paths.exitCode != 0) {
        return out;
    }

    std::istringstream iss(paths.stdoutStr);
    std::string line;
    while (std::getline(iss, line)) {
        line = Trim(line);
        if (line.empty()) {
            continue;
        }
        const auto sp = line.find(' ');
        if (sp == std::string::npos || sp + 1 >= line.size()) {
            continue;
        }
        const auto rel = line.substr(sp + 1);
        if (rel.starts_with("src/")) {
            out.push_back(rel);
        }
    }
    return out;
}

void PrintStableDevSummary(const std::vector<StableDevSummaryRow>& InRows, StableDevReportFormat InFormat) {
    switch (InFormat) {
        case StableDevReportFormat::Compact: {
            for (const auto& row : InRows) {
                std::cout << "[" << row.repo << "] RESULT=" << row.result << " | branch=" << row.currentBranch << "\n";
                if (!row.reason.empty()) {
                    std::cout << "  reason: " << row.reason << "\n";
                }
                if (row.sameCommit) {
                    std::cout << "  commit: " << row.latestUpstreamCommit << "\n";
                } else {
                    std::cout << "  upstream: " << row.latestUpstreamCommit << "\n";
                    std::cout << "  stable:   " << row.latestStableCommit << "\n";
                }
            }
            break;
        }
        case StableDevReportFormat::Table: {
            const std::string sep = "+------------------------------+---------+------------------------+------------------------------------------------------------+------------------------------------------------------------+------------------------------+";
            std::cout << sep << "\n";
            std::cout << std::format("| {:<28} | {:<7} | {:<22} | {:<58} | {:<58} | {:<28} |\n",
                "Repo", "Result", "Current Branch", "Latest Upstream Commit (sha|time|author|title)", "Latest Stable Commit (sha|time|author|title)", "Reason");
            std::cout << sep << "\n";
            for (const auto& row : InRows) {
                std::cout << std::format("| {:<28} | {:<7} | {:<22} | {:<58} | {:<58} | {:<28} |\n",
                    TruncateText(row.repo, 28),
                    TruncateText(row.result, 7),
                    TruncateText(row.currentBranch, 22),
                    TruncateText(row.latestUpstreamCommit, 58),
                    TruncateText(row.latestStableCommit, 58),
                    TruncateText(row.reason, 28));
            }
            std::cout << sep << "\n";
            break;
        }
        case StableDevReportFormat::Tsv: {
            std::cout << "repo\tresult\tcurrent_branch\tsame_commit\tlatest_upstream_commit\tlatest_stable_commit\treason\n";
            for (const auto& row : InRows) {
                std::cout << row.repo << "\t" << row.result << "\t" << row.currentBranch << "\t"
                          << (row.sameCommit ? "1" : "0") << "\t" << row.latestUpstreamCommit << "\t"
                          << row.latestStableCommit << "\t" << row.reason << "\n";
            }
            break;
        }
        case StableDevReportFormat::Json: {
            std::cout << "[\n";
            for (std::size_t i = 0; i < InRows.size(); ++i) {
                const auto& row = InRows[i];
                std::cout << std::format(
                    "  {{\"repo\":\"{}\",\"result\":\"{}\",\"current_branch\":\"{}\",\"same_commit\":{},\"latest_upstream_commit\":\"{}\",\"latest_stable_commit\":\"{}\",\"reason\":\"{}\"}}{}\n",
                    JsonEscape(row.repo),
                    JsonEscape(row.result),
                    JsonEscape(row.currentBranch),
                    row.sameCommit ? "true" : "false",
                    JsonEscape(row.latestUpstreamCommit),
                    JsonEscape(row.latestStableCommit),
                    JsonEscape(row.reason),
                    (i + 1 < InRows.size()) ? "," : "");
            }
            std::cout << "]\n";
            break;
        }
        case StableDevReportFormat::Markdown: {
            std::cout << "| Repo | Result | Current Branch | Latest Upstream Commit | Latest Stable Commit | Reason |\n";
            std::cout << "| --- | --- | --- | --- | --- | --- |\n";
            for (const auto& row : InRows) {
                auto esc = [](std::string value) {
                    std::size_t pos = 0;
                    while ((pos = value.find('|', pos)) != std::string::npos) {
                        value.replace(pos, 1, "\\|");
                        pos += 2;
                    }
                    return value;
                };
                std::cout << "| " << row.repo << " | " << row.result << " | " << row.currentBranch << " | "
                          << esc(row.latestUpstreamCommit) << " | " << esc(row.latestStableCommit) << " | " << esc(row.reason) << " |\n";
            }
            break;
        }
    }
}

auto RunNativeStableDevSync(
    const std::filesystem::path& InRoot,
    const std::filesystem::path& InRepo,
    const std::string& InRel,
    bool InDryRun,
    StableDevSummaryRow& OutRow) -> int {
    OutRow.repo = InRel;
    OutRow.currentBranch = CurrentBranch(InRepo);

    if (OutRow.currentBranch.empty()) {
        OutRow.result = "FAILED";
        OutRow.sameCommit = false;
        OutRow.latestUpstreamCommit = "N/A";
        OutRow.latestStableCommit = "N/A";
        OutRow.reason = "detached HEAD";
        return 1;
    }

    const auto latestTag = ResolveLatestStableTag(InRepo);
    if (latestTag.empty()) {
        OutRow.result = "FAILED";
        OutRow.sameCommit = false;
        OutRow.latestUpstreamCommit = "N/A";
        OutRow.latestStableCommit = "N/A";
        OutRow.reason = "no stable tag found";
        return 1;
    }

    if (InDryRun) {
        std::cout << "[DRY RUN] " << InRel << ": git fetch upstream --tags --prune\n";
    } else {
        const auto fetch = GitPassThrough(InRepo, {"fetch", "upstream", "--tags", "--prune"});
        if (fetch.exitCode != 0) {
            OutRow.result = "FAILED";
            OutRow.sameCommit = false;
            OutRow.latestUpstreamCommit = "N/A";
            OutRow.latestStableCommit = "N/A";
            OutRow.reason = "fetch upstream --tags failed";
            return fetch.exitCode;
        }
    }

    const auto upstreamSha = ResolveCommitSha(InRepo, std::format("refs/tags/{}^{{commit}}", latestTag));
    const auto stableRef = ResolveStableRef(InRoot, InRepo, OutRow.currentBranch, InRel);
    const auto stableShaBefore = ResolveCommitSha(InRepo, stableRef);

    OutRow.latestUpstreamCommit = ResolveCommitLine(InRepo, upstreamSha);
    OutRow.latestStableCommit = ResolveCommitLine(InRepo, stableShaBefore);

    if (upstreamSha.empty()) {
        OutRow.result = "FAILED";
        OutRow.sameCommit = false;
        OutRow.reason = "cannot resolve stable tag commit";
        return 1;
    }

    if (stableShaBefore.empty()) {
        OutRow.result = "FAILED";
        OutRow.sameCommit = false;
        OutRow.reason = "cannot resolve stable branch ref";
        return 1;
    }

    if (upstreamSha == stableShaBefore) {
        OutRow.result = "SUCCESS";
        OutRow.sameCommit = true;
        OutRow.latestStableCommit = "(same as upstream)";
        OutRow.reason.clear();
        return 0;
    }

    if (InDryRun) {
        std::cout << "[DRY RUN] " << InRel << ": git rebase refs/tags/" << latestTag << "\n";
        OutRow.result = "SUCCESS";
        OutRow.sameCommit = false;
        OutRow.reason = "dry-run";
        return 0;
    }

    const auto rebase = GitPassThrough(InRepo, {"rebase", std::format("refs/tags/{}", latestTag)});
    if (rebase.exitCode != 0) {
        OutRow.result = "FAILED";
        OutRow.sameCommit = false;
        OutRow.reason = "rebase onto latest stable tag failed";
        return rebase.exitCode;
    }

    const auto stableShaAfter = ResolveCommitSha(InRepo, "HEAD");
    OutRow.result = "SUCCESS";
    OutRow.sameCommit = (stableShaAfter == upstreamSha);
    OutRow.latestStableCommit = OutRow.sameCommit
        ? "(same as upstream)"
        : ResolveCommitLine(InRepo, stableShaAfter);
    OutRow.reason.clear();
    return 0;
}

auto RunStableDevWorkspace(
    const std::filesystem::path& InRoot,
    StableDevReportFormat InFormat,
    std::vector<std::string> InForwardArgs) -> int {
    if (HasLongFlag(InForwardArgs, "--repo")) {
        std::cerr << "ERROR: --repo is not allowed with --workspace\n";
        return 1;
    }

    const bool dryRun = HasLongFlag(InForwardArgs, "--dry-run");

    const auto gitmodules = InRoot / ".gitmodules";
    if (!std::filesystem::exists(gitmodules)) {
        std::cerr << "ERROR: .gitmodules not found at project root: " << gitmodules.generic_string() << "\n";
        return 1;
    }

    const auto submodulePaths = CollectSrcSubmodulePaths(InRoot);
    if (submodulePaths.empty()) {
        std::cout << "INFO: No src/* submodules found. Nothing to do.\n";
        return 0;
    }

    int success = 0;
    int skipped = 0;
    int failed = 0;
    std::vector<StableDevSummaryRow> summary;
    summary.reserve(submodulePaths.size());

    for (const auto& rel : submodulePaths) {
        const auto repoPath = InRoot / rel;
        if (GitCapture(repoPath, {"rev-parse", "--is-inside-work-tree"}).exitCode != 0) {
            std::cout << "SKIP: " << rel << " (not initialized git repo)\n";
            skipped += 1;
            summary.push_back({rel, "SKIPPED", "N/A", false, "N/A", "N/A", "not initialized git repo"});
            continue;
        }

        if (!HasRemote(repoPath, "upstream")) {
            std::cout << "SKIP: " << rel << " (no upstream remote)\n";
            skipped += 1;
            summary.push_back({rel, "SKIPPED", CurrentBranch(repoPath), false, "N/A", "N/A", "no upstream remote"});
            continue;
        }

        std::cout << "RUN: " << rel << "\n";
        StableDevSummaryRow row;
        const auto runExitCode = RunNativeStableDevSync(InRoot, repoPath, rel, dryRun, row);

        if (runExitCode == 0) {
            success += 1;
            summary.push_back(row);
        } else {
            failed += 1;
            std::cerr << "FAIL: " << rel << " (" << row.reason << ")\n";
            summary.push_back(row);
        }
    }

    std::cout << "=== upstream-stable-dev wrapper summary ===\n";
    std::cout << "success: " << success << "\n";
    std::cout << "skipped: " << skipped << "\n";
    std::cout << "failed: " << failed << "\n";
    std::cout << "OVERALL RESULT: " << ((failed > 0) ? "FAILED" : "SUCCESS") << "\n";
    std::cout << "=== upstream-stable-dev branch report ===\n";
    PrintStableDevSummary(summary, InFormat);

    return failed > 0 ? 1 : 0;
}

auto DiscoverRegisteredPathsRecursive(const std::filesystem::path& InWorkspaceRoot) -> std::set<std::string> {
    std::set<std::string> out;
    std::vector<std::filesystem::path> queue{InWorkspaceRoot};

    while (!queue.empty()) {
        const auto current = queue.back();
        queue.pop_back();

        const auto gitmodules = current / ".gitmodules";
        if (!std::filesystem::exists(gitmodules)) {
            continue;
        }

        const auto pathsResult = GitCapture(current, {"config", "-f", ".gitmodules", "--get-regexp", "^submodule\\..*\\.path$"});
        if (pathsResult.exitCode != 0) {
            continue;
        }

        std::istringstream iss(pathsResult.stdoutStr);
        std::string line;
        while (std::getline(iss, line)) {
            line = Trim(line);
            if (line.empty()) {
                continue;
            }
            const auto sp = line.find(' ');
            if (sp == std::string::npos || sp + 1 >= line.size()) {
                continue;
            }
            const auto relPath = line.substr(sp + 1);
            const auto full = std::filesystem::weakly_canonical(current / relPath).generic_string();
            if (out.insert(full).second) {
                queue.push_back(full);
            }
        }
    }

    return out;
}

auto GitmodulesBranchForPath(const std::filesystem::path& InWorkspaceRoot, const std::filesystem::path& InRepoPath) -> std::optional<std::string> {
    auto current = InRepoPath.parent_path();
    const auto workspace = std::filesystem::weakly_canonical(InWorkspaceRoot);
    const auto target = std::filesystem::weakly_canonical(InRepoPath);

    while (!current.empty()) {
        const auto candidateGitmodules = current / ".gitmodules";
        if (std::filesystem::exists(candidateGitmodules)) {
            std::error_code ec;
            auto rel = std::filesystem::relative(target, current, ec);
            if (!ec) {
                const auto relPath = rel.generic_string();
                const auto pathResult = GitCapture(current, {"config", "-f", ".gitmodules", "--get-regexp", "^submodule\\..*\\.path$"});
                if (pathResult.exitCode == 0) {
                    std::istringstream iss(pathResult.stdoutStr);
                    std::string line;
                    while (std::getline(iss, line)) {
                        line = Trim(line);
                        if (line.empty()) {
                            continue;
                        }
                        const auto sp = line.find(' ');
                        if (sp == std::string::npos || sp + 1 >= line.size()) {
                            continue;
                        }
                        const auto key = line.substr(0, sp);
                        const auto value = line.substr(sp + 1);
                        if (value != relPath) {
                            continue;
                        }

                        const std::string suffix = ".path";
                        if (!key.ends_with(suffix)) {
                            continue;
                        }
                        const auto prefix = key.substr(0, key.size() - suffix.size());
                        const auto branchResult = GitCapture(current, {"config", "-f", ".gitmodules", "--get", prefix + ".branch"});
                        if (branchResult.exitCode == 0) {
                            const auto branch = Trim(branchResult.stdoutStr);
                            if (!branch.empty()) {
                                return branch;
                            }
                        }
                        return std::nullopt;
                    }
                }
            }
        }

        if (std::filesystem::weakly_canonical(current) == workspace) {
            break;
        }
        const auto next = current.parent_path();
        if (next == current) {
            break;
        }
        current = next;
    }

    return std::nullopt;
}

auto FindGitmodulesBindingForPath(const std::filesystem::path& InWorkspaceRoot, const std::filesystem::path& InRepoPath) -> std::optional<GitmodulesBinding> {
    auto current = InRepoPath.parent_path();
    const auto workspace = std::filesystem::weakly_canonical(InWorkspaceRoot);
    const auto target = std::filesystem::weakly_canonical(InRepoPath);

    while (!current.empty()) {
        const auto candidateGitmodules = current / ".gitmodules";
        if (std::filesystem::exists(candidateGitmodules)) {
            std::error_code ec;
            auto rel = std::filesystem::relative(target, current, ec);
            if (!ec) {
                const auto relPath = rel.generic_string();
                const auto pathResult = GitCapture(current, {"config", "-f", ".gitmodules", "--get-regexp", "^submodule\\..*\\.path$"});
                if (pathResult.exitCode == 0) {
                    std::istringstream iss(pathResult.stdoutStr);
                    std::string line;
                    while (std::getline(iss, line)) {
                        line = Trim(line);
                        if (line.empty()) {
                            continue;
                        }
                        const auto sp = line.find(' ');
                        if (sp == std::string::npos || sp + 1 >= line.size()) {
                            continue;
                        }
                        const auto key = line.substr(0, sp);
                        const auto value = line.substr(sp + 1);
                        if (value != relPath || !key.ends_with(".path")) {
                            continue;
                        }
                        return GitmodulesBinding{
                            .root = std::filesystem::weakly_canonical(current),
                            .prefix = key.substr(0, key.size() - 5),
                        };
                    }
                }
            }
        }

        if (std::filesystem::weakly_canonical(current) == workspace) {
            break;
        }
        const auto next = current.parent_path();
        if (next == current) {
            break;
        }
        current = next;
    }

    return std::nullopt;
}

auto WriteGitmodulesBranch(const GitmodulesBinding& InBinding, const std::string& InBranch, bool InDryRun) -> bool {
    if (InBranch.empty()) {
        return false;
    }

    if (InDryRun) {
        std::cout << "[DRY RUN] Would run: git -C " << InBinding.root.generic_string()
                  << " config -f .gitmodules --replace-all " << InBinding.prefix << ".branch " << InBranch << "\n";
        return true;
    }

    const auto write = GitCapture(InBinding.root, {"config", "-f", ".gitmodules", "--replace-all", InBinding.prefix + ".branch", InBranch});
    return write.exitCode == 0;
}

auto ResolveDetachedTargetBranch(const std::filesystem::path& InRepo, const std::string& InRemote, const BranchMode InMode) -> std::pair<std::string, std::string> {
    if (InMode == BranchMode::StableDev) {
        const auto latestTag = ResolveLatestStableTag(InRepo);
        if (latestTag.empty()) {
            return {{}, "stable-dev mode: no release tag found"};
        }
        return {"branch_" + latestTag, "stable-dev inferred branch from latest tag"};
    }

    const auto remoteDefault = DetectRemoteDefaultBranch(InRepo, InRemote);
    if (remoteDefault.empty()) {
        return {{}, "default mode: cannot resolve remote default branch"};
    }
    return {remoteDefault, "default mode inferred remote default branch"};
}

auto CheckoutRecoveredBranch(
    const std::filesystem::path& InRepo,
    const std::string& InRemote,
    const std::string& InBranch,
    const BranchMode InMode,
    bool InDryRun,
    std::string* OutDetail) -> bool {
    if (InBranch.empty()) {
        return false;
    }

    if (LocalBranchExists(InRepo, InBranch)) {
        if (OutDetail != nullptr) {
            *OutDetail = "checkout existing local branch";
        }
        if (InDryRun) {
            std::cout << "[DRY RUN] Would run: git checkout " << InBranch << "\n";
            return true;
        }
        return GitPassThrough(InRepo, {"checkout", InBranch}).exitCode == 0;
    }

    if (RemoteBranchExists(InRepo, InRemote, InBranch)) {
        if (OutDetail != nullptr) {
            *OutDetail = "create local branch from remote";
        }
        if (InDryRun) {
            std::cout << "[DRY RUN] Would run: git checkout -b " << InBranch << " " << InRemote << "/" << InBranch << "\n";
            return true;
        }
        return GitPassThrough(InRepo, {"checkout", "-b", InBranch, std::format("{}/{}", InRemote, InBranch)}).exitCode == 0;
    }

    if (InMode == BranchMode::StableDev) {
        const auto latestTag = ResolveLatestStableTag(InRepo);
        if (!latestTag.empty()) {
            if (OutDetail != nullptr) {
                *OutDetail = "create stable-dev branch from latest tag";
            }
            if (InDryRun) {
                std::cout << "[DRY RUN] Would run: git checkout -b " << InBranch << " refs/tags/" << latestTag << "\n";
                return true;
            }
            return GitPassThrough(InRepo, {"checkout", "-b", InBranch, std::format("refs/tags/{}", latestTag)}).exitCode == 0;
        }
    }

    return false;
}

auto BuildSyncPlans(
    const std::filesystem::path& InRoot,
    const std::string& InPreferredRemote,
    int InMaxDepth,
    bool InNoCache,
    bool InRefreshCache) -> std::pair<std::vector<SyncPlan>, std::string> {
    workspace::DiscoverOptions options;
    options.rootDir = InRoot;
    options.maxDepth = InMaxDepth;
    options.useCache = !InNoCache;
    options.refreshCache = InRefreshCache;
    options.metadataLevel = "full";

    const auto discovery = workspace::DiscoverRepos(options);
    const auto root = std::filesystem::weakly_canonical(InRoot);
    const auto registeredPaths = DiscoverRegisteredPathsRecursive(root);

    std::vector<SyncPlan> plans;
    plans.reserve(discovery.repos.size());

    for (const auto& repo : discovery.repos) {
        const auto repoPath = std::filesystem::weakly_canonical(repo.path);
        const auto remote = ResolveRemote(repoPath, InPreferredRemote);
        if (remote.empty()) {
            std::cerr << "WARN: Skip repo without remotes: " << repoPath.generic_string() << "\n";
            continue;
        }

        const auto current = CurrentBranch(repoPath);
        const bool isRoot = (repoPath == root);
        const bool isRegistered = registeredPaths.contains(repoPath.generic_string());

        std::string branchSource;
        std::string targetBranch;
        if (isRoot) {
            if (!current.empty()) {
                targetBranch = current;
                branchSource = "root current branch";
            } else {
                targetBranch = DetectRemoteDefaultBranch(repoPath, remote);
                branchSource = "root detached -> remote default";
            }
        } else if (isRegistered) {
            const auto configured = GitmodulesBranchForPath(root, repoPath);
            if (configured.has_value() && !configured->empty()) {
                targetBranch = *configured;
                branchSource = "registered .gitmodules branch";
            } else {
                targetBranch = DetectRemoteDefaultBranch(repoPath, remote);
                branchSource = "registered remote default branch";
            }
        } else {
            if (!current.empty()) {
                targetBranch = current;
                branchSource = "unregistered current branch";
            } else {
                targetBranch = DetectRemoteDefaultBranch(repoPath, remote);
                branchSource = "unregistered detached -> remote default";
            }
        }

        if (targetBranch.empty()) {
            std::cerr << "ERROR: Could not determine target branch for repo: " << repoPath.generic_string() << "\n";
            continue;
        }

        plans.push_back(SyncPlan{
            .path = repoPath,
            .type = isRoot ? "root" : (isRegistered ? "registered" : "unregistered"),
            .remote = remote,
            .targetBranch = targetBranch,
            .branchSource = branchSource,
        });
    }

    std::sort(plans.begin(), plans.end(), [](const SyncPlan& A, const SyncPlan& B) {
        return A.path.generic_string() < B.path.generic_string();
    });

    return {plans, discovery.mode};
}

auto RunNativeOriginLatestSync(
    const std::filesystem::path& InRepoRoot,
    const std::string& InRemote,
    int InMaxDepth,
    bool InDryRun,
    bool InNoCache,
    bool InRefreshCache,
    bool InRecursive) -> int {
    std::vector<SyncPlan> plans;
    std::string mode;
    try {
        auto planResult = BuildSyncPlans(InRepoRoot, InRemote, InMaxDepth, InNoCache, InRefreshCache);
        plans = std::move(planResult.first);
        mode = std::move(planResult.second);
    } catch (const std::exception& ex) {
        if (!InNoCache) {
            std::cerr << "WARN: native discovery failed with cache enabled, retrying without cache: " << ex.what() << "\n";
            try {
                auto planResult = BuildSyncPlans(InRepoRoot, InRemote, InMaxDepth, true, InRefreshCache);
                plans = std::move(planResult.first);
                mode = std::move(planResult.second);
            } catch (const std::exception& exNoCache) {
                std::cerr << "ERROR: native discovery failed without cache: " << exNoCache.what() << "\n";
                return 1;
            }
        } else {
            std::cerr << "ERROR: native discovery failed: " << ex.what() << "\n";
            return 1;
        }
    }

    if (!InRecursive) {
        const auto root = std::filesystem::weakly_canonical(InRepoRoot);
        plans.erase(
            std::remove_if(plans.begin(), plans.end(), [&](const SyncPlan& InPlan) {
                return std::filesystem::weakly_canonical(InPlan.path) != root;
            }),
            plans.end());
    }

    std::cout << (InRecursive
        ? "Syncing workspace repos with recursive branch rules\n"
        : "Syncing current repository only (non-recursive mode)\n");
    std::cout << "Discover mode: " << (mode.empty() ? "unknown" : mode) << "\n";

    int failures = 0;
    for (const auto& plan : plans) {
        const auto rel = std::filesystem::relative(plan.path, InRepoRoot).generic_string();
        const auto name = (rel.empty() || rel == ".") ? "." : rel;

        std::cout << (InDryRun ? "[DRY RUN] " : "") << "Repo: " << name << "\n";
        std::cout << (InDryRun ? "[DRY RUN] " : "") << "Branch source: " << plan.branchSource << "\n";

        const auto fetch = GitCapture(plan.path, {"fetch", plan.remote, "--prune", "--tags"});
        if (!InDryRun && fetch.exitCode != 0) {
            std::cerr << "WARN: fetch failed for " << name << "\n";
        }

        const auto hasLocal = GitCapture(plan.path, {"show-ref", "--verify", "--quiet", std::format("refs/heads/{}", plan.targetBranch)}).exitCode == 0;
        const auto hasRemote = GitCapture(plan.path, {"show-ref", "--verify", "--quiet", std::format("refs/remotes/{}/{}", plan.remote, plan.targetBranch)}).exitCode == 0;

        std::vector<std::string> checkoutArgs;
        if (hasLocal) {
            checkoutArgs = {"checkout", plan.targetBranch};
        } else if (hasRemote) {
            checkoutArgs = {"checkout", "-b", plan.targetBranch, std::format("{}/{}", plan.remote, plan.targetBranch)};
        } else {
            if (plan.type == "unregistered") {
                checkoutArgs = {"checkout", plan.targetBranch};
                std::cout << "WARN: Unregistered repo branch has no remote ref, keeping local branch: " << name << "\n";
            } else {
                std::cerr << "ERROR: Target branch not found for " << name << "\n";
                failures += 1;
                continue;
            }
        }

        if (InDryRun) {
            std::cout << "[DRY RUN] Would run: git";
            for (const auto& arg : checkoutArgs) {
                std::cout << " " << arg;
            }
            std::cout << "\n";
            if (hasRemote) {
                std::cout << "[DRY RUN] Would run: git pull --rebase " << plan.remote << " " << plan.targetBranch << "\n";
            } else {
                std::cout << "[DRY RUN] Skip pull: missing remote branch " << plan.remote << "/" << plan.targetBranch << "\n";
            }
            continue;
        }

        const auto checkout = GitPassThrough(plan.path, checkoutArgs);
        if (checkout.exitCode != 0) {
            std::cerr << "ERROR: checkout failed for " << name << "\n";
            failures += 1;
            continue;
        }

        if (hasRemote) {
            if (!RecoverRebaseState(plan.path, name, InDryRun)) {
                failures += 1;
                continue;
            }

            const auto pull = GitPassThrough(plan.path, {"pull", "--rebase", plan.remote, plan.targetBranch});
            if (pull.exitCode != 0) {
                std::cerr << "ERROR: pull --rebase failed for " << name << "\n";
                failures += 1;
            }
        }
    }

    std::cout << "=== Sync Complete ===\n";
    return failures > 0 ? 1 : 0;
}

auto RunNativePreCommitRepair(
    const std::filesystem::path& InRepoRoot,
    const std::string& InRemote,
    int InMaxDepth,
    bool InDryRun,
    bool InNoCache,
    bool InRefreshCache,
    bool InRecursive,
    BranchMode InBranchMode) -> int {
    workspace::DiscoverOptions options;
    options.rootDir = InRepoRoot;
    options.maxDepth = InMaxDepth;
    options.useCache = !InNoCache;
    options.refreshCache = InRefreshCache;
    options.metadataLevel = "full";

    const auto discovery = workspace::DiscoverRepos(options);
    const auto root = std::filesystem::weakly_canonical(InRepoRoot);
    const auto registeredPaths = DiscoverRegisteredPathsRecursive(root);

    int failures = 0;

    std::cout << (InRecursive
        ? "Pre-commit detached-HEAD repair for workspace repos\n"
        : "Pre-commit detached-HEAD repair for current repository only\n");

    for (const auto& repo : discovery.repos) {
        const auto repoPath = std::filesystem::weakly_canonical(repo.path);
        if (!InRecursive && repoPath != root) {
            continue;
        }

        const auto rel = std::filesystem::relative(repoPath, InRepoRoot).generic_string();
        const auto name = (rel.empty() || rel == ".") ? "." : rel;

        const auto remote = ResolveRemote(repoPath, InRemote);
        if (remote.empty()) {
            std::cerr << "WARN: Skip repo without remotes: " << name << "\n";
            continue;
        }

        const auto current = CurrentBranch(repoPath);
        if (!current.empty()) {
            std::cout << (InDryRun ? "[DRY RUN] " : "") << "Repo: " << name << " already on branch " << current << "\n";
            continue;
        }

        const bool isRegistered = registeredPaths.contains(repoPath.generic_string());
        std::string branchSource;
        std::string targetBranch;

        if (isRegistered) {
            const auto configured = GitmodulesBranchForPath(root, repoPath);
            if (configured.has_value() && !configured->empty()) {
                targetBranch = *configured;
                branchSource = "registered .gitmodules branch";
            }
        }

        if (targetBranch.empty()) {
            auto [inferredBranch, inferredSource] = ResolveDetachedTargetBranch(repoPath, remote, InBranchMode);
            targetBranch = std::move(inferredBranch);
            branchSource = std::move(inferredSource);
        }

        if (targetBranch.empty()) {
            std::cerr << "ERROR: " << name << " detached HEAD with no resolvable target branch\n";
            failures += 1;
            continue;
        }

        std::cout << (InDryRun ? "[DRY RUN] " : "") << "Repo: " << name << " | source=" << branchSource << " | target=" << targetBranch << "\n";

        if (!InDryRun) {
            const auto fetch = GitCapture(repoPath, {"fetch", remote, "--prune", "--tags"});
            if (fetch.exitCode != 0) {
                std::cerr << "WARN: fetch failed before detached recovery: " << name << "\n";
            }
        }

        std::string checkoutDetail;
        const auto checkedOut = CheckoutRecoveredBranch(
            repoPath,
            remote,
            targetBranch,
            InBranchMode,
            InDryRun,
            &checkoutDetail);
        if (!checkedOut) {
            std::cerr << "ERROR: failed to recover detached HEAD for repo: " << name << "\n";
            failures += 1;
            continue;
        }

        if (!checkoutDetail.empty()) {
            std::cout << (InDryRun ? "[DRY RUN] " : "") << "Repo: " << name << " | action=" << checkoutDetail << "\n";
        }

        if (isRegistered) {
            const auto binding = FindGitmodulesBindingForPath(root, repoPath);
            if (!binding.has_value()) {
                std::cerr << "WARN: registered repo without resolvable .gitmodules binding: " << name << "\n";
                continue;
            }

            if (!WriteGitmodulesBranch(*binding, targetBranch, InDryRun)) {
                std::cerr << "WARN: failed to write .gitmodules branch for repo: " << name << "\n";
            }
        }
    }

    std::cout << "=== Pre-Commit Repair Complete ===\n";
    return failures > 0 ? 1 : 0;
}

auto RunNativeUpstreamForcePush(
    const std::filesystem::path& InRepo,
    bool InDryRun) -> int {
    const auto repo = std::filesystem::weakly_canonical(InRepo);
    if (GitCapture(repo, {"rev-parse", "--is-inside-work-tree"}).exitCode != 0) {
        std::cerr << "ERROR: Not a git repository: " << repo.generic_string() << "\n";
        return 1;
    }

    if (!HasRemote(repo, "upstream")) {
        std::cerr << "ERROR: Missing upstream remote\n";
        return 1;
    }
    if (!HasRemote(repo, "origin")) {
        std::cerr << "ERROR: Missing origin remote\n";
        return 1;
    }

    const auto branch = CurrentBranch(repo);
    if (branch.empty()) {
        std::cerr << "ERROR: Detached HEAD not supported for upstream-force-push\n";
        return 1;
    }

    auto upstreamDefault = DetectRemoteDefaultBranch(repo, "upstream");
    if (upstreamDefault.empty()) {
        std::cerr << "ERROR: Cannot resolve upstream default branch\n";
        return 1;
    }

    if (InDryRun) {
        std::cout << "[DRY RUN] Would run: git fetch upstream\n";
        std::cout << "[DRY RUN] Would run: git rebase " << upstreamDefault << "\n";
        std::cout << "[DRY RUN] Would run: git push --force-with-lease origin " << branch << "\n";
        return 0;
    }

    const auto fetch = GitPassThrough(repo, {"fetch", "upstream"});
    if (fetch.exitCode != 0) {
        return fetch.exitCode;
    }

    const auto rebase = GitPassThrough(repo, {"rebase", upstreamDefault});
    if (rebase.exitCode != 0) {
        return rebase.exitCode;
    }

    const auto push = GitPassThrough(repo, {"push", "--force-with-lease", "origin", branch});
    return push.exitCode;
}

} // namespace

void RegisterSync(CLI::App& InApp) {
    auto* cmd = InApp.add_subcommand("sync", "Repository synchronization workflows");

    // --- sync pre-commit ---
    auto* pre_commit = cmd->add_subcommand("pre-commit", "Repair detached HEAD before commit workflow");
    pre_commit->allow_extras();
    auto* preCommitRepo = new std::string{"."};
    auto* preCommitRemote = new std::string{"origin"};
    auto* preCommitDryRun = new bool{false};
    auto* preCommitMaxDepth = new int{6};
    auto* preCommitNoCache = new bool{false};
    auto* preCommitRefreshCache = new bool{false};
    auto* preCommitNoRecursive = new bool{false};
    auto* preCommitBranchMode = new std::string{"default"};
    auto* preCommitProfile = new bool{false};

    pre_commit->add_option("--repo", *preCommitRepo, "Target repository root path");
    pre_commit->add_option("--remote", *preCommitRemote, "Preferred remote name");
    pre_commit->add_flag("--dry-run", *preCommitDryRun, "Preview detached-head repair actions");
    pre_commit->add_option("--native-max-depth", *preCommitMaxDepth, "Native discovery max depth");
    pre_commit->add_flag("--native-no-cache", *preCommitNoCache, "Disable native discovery cache");
    pre_commit->add_flag("--native-refresh-cache", *preCommitRefreshCache, "Force native cache refresh");
    pre_commit->add_flag("--no-recursive,-N", *preCommitNoRecursive, "Repair only current repository");
    pre_commit->add_option("--branch-mode", *preCommitBranchMode, "Detached-branch inference mode: default|stable-dev");
    pre_commit->add_flag("--profile", *preCommitProfile, "Print native pre-commit timing/profile summary");

    pre_commit->callback([=]() {
        auto extras = pre_commit->remaining();
        if (!extras.empty()) {
            std::cerr << "Error: unsupported extra arguments in native sync pre-commit mode:";
            for (const auto& extra : extras) {
                std::cerr << ' ' << extra;
            }
            std::cerr << "\n";
            std::exit(2);
        }

        const auto branchMode = ParseBranchMode(*preCommitBranchMode);
        if (!branchMode.has_value()) {
            std::cerr << "ERROR: Unsupported --branch-mode: " << *preCommitBranchMode << " (supported: default, stable-dev)\n";
            std::exit(1);
        }

        const auto start = std::chrono::steady_clock::now();
        const auto repoRoot = std::filesystem::weakly_canonical(std::filesystem::path(*preCommitRepo));
        const auto code = RunNativePreCommitRepair(
            repoRoot,
            *preCommitRemote,
            *preCommitMaxDepth,
            *preCommitDryRun,
            *preCommitNoCache,
            *preCommitRefreshCache,
            !*preCommitNoRecursive,
            *branchMode);
        if (*preCommitProfile) {
            const auto totalMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
            std::cout << "\n=== Sync Profile Summary ===\n";
            std::cout << "mode: native\n";
            std::cout << "workflow: pre-commit\n";
            std::cout << "recursive: " << (!*preCommitNoRecursive ? "true" : "false") << "\n";
            std::cout << "dry_run: " << (*preCommitDryRun ? "true" : "false") << "\n";
            std::cout << "branch_mode: " << *preCommitBranchMode << "\n";
            std::cout << "total_ms: " << totalMs << "\n";
        }
        std::exit(code);
    });

    // --- sync origin-latest ---
    auto* origin_latest = cmd->add_subcommand("origin-latest", "Sync to origin default branch latest");
    origin_latest->allow_extras();
    auto* originLatestShell = new bool{false};
    auto* originLatestRepo = new std::string{"."};
    auto* originLatestRemote = new std::string{"origin"};
    auto* originLatestDryRun = new bool{false};
    auto* originLatestMaxDepth = new int{6};
    auto* originLatestNoCache = new bool{false};
    auto* originLatestRefreshCache = new bool{false};
    auto* originLatestNoRecursive = new bool{false};
    auto* originLatestProfile = new bool{false};

    origin_latest->add_flag("--shell", *originLatestShell, "Deprecated compatibility flag (shell path removed)");
    origin_latest->add_option("--repo", *originLatestRepo, "Target repository root path");
    origin_latest->add_option("--remote", *originLatestRemote, "Preferred remote name");
    origin_latest->add_flag("--dry-run", *originLatestDryRun, "Preview sync actions without modifying repositories");
    origin_latest->add_option("--native-max-depth", *originLatestMaxDepth, "Native discovery max depth");
    origin_latest->add_flag("--native-no-cache", *originLatestNoCache, "Disable native discovery cache");
    origin_latest->add_flag("--native-refresh-cache", *originLatestRefreshCache, "Force native cache refresh");
    origin_latest->add_flag("--no-recursive,-N", *originLatestNoRecursive, "Sync only current repository");
    origin_latest->add_flag("--profile", *originLatestProfile, "Print native sync timing/profile summary");

    origin_latest->callback([=]() {
        auto extras = origin_latest->remaining();
        if (*originLatestShell) {
            std::cerr << "Error: --shell is no longer supported; sync origin-latest is fully native now\n";
            std::exit(2);
        }
        if (!extras.empty()) {
            std::cerr << "Error: unsupported extra arguments in native sync origin-latest mode:";
            for (const auto& extra : extras) {
                std::cerr << ' ' << extra;
            }
            std::cerr << "\n";
            std::exit(2);
        }

        const auto start = std::chrono::steady_clock::now();
        const auto repoRoot = std::filesystem::weakly_canonical(std::filesystem::path(*originLatestRepo));
        const auto code = RunNativeOriginLatestSync(
            repoRoot,
            *originLatestRemote,
            *originLatestMaxDepth,
            *originLatestDryRun,
            *originLatestNoCache,
            *originLatestRefreshCache,
            !*originLatestNoRecursive);
        if (*originLatestProfile) {
            const auto totalMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
            std::cout << "\n=== Sync Profile Summary ===\n";
            std::cout << "mode: native\n";
            std::cout << "workflow: origin-latest\n";
            std::cout << "recursive: " << (!*originLatestNoRecursive ? "true" : "false") << "\n";
            std::cout << "dry_run: " << (*originLatestDryRun ? "true" : "false") << "\n";
            std::cout << "total_ms: " << totalMs << "\n";
        }
        std::exit(code);
    });

    // --- sync upstream-force-push ---
    auto* upstream_fp = cmd->add_subcommand("upstream-force-push", "Sync from upstream, force-push to origin");
    upstream_fp->allow_extras();
    auto* upstreamRepo = new std::string{"."};
    auto* upstreamDryRun = new bool{false};
    auto* upstreamProfile = new bool{false};
    upstream_fp->add_option("--repo", *upstreamRepo, "Target repository path");
    upstream_fp->add_flag("--dry-run", *upstreamDryRun, "Preview force-push sync actions");
    upstream_fp->add_flag("--profile", *upstreamProfile, "Print native sync timing/profile summary");
    upstream_fp->callback([=]() {
        auto extras = upstream_fp->remaining();
        if (!extras.empty()) {
            std::cerr << "Error: unsupported extra arguments in native sync upstream-force-push mode:";
            for (const auto& extra : extras) {
                std::cerr << ' ' << extra;
            }
            std::cerr << "\n";
            std::exit(2);
        }

        const auto start = std::chrono::steady_clock::now();
        const auto repoRoot = std::filesystem::weakly_canonical(std::filesystem::path(*upstreamRepo));
        const auto code = RunNativeUpstreamForcePush(repoRoot, *upstreamDryRun);
        if (*upstreamProfile) {
            const auto totalMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
            std::cout << "\n=== Sync Profile Summary ===\n";
            std::cout << "mode: native\n";
            std::cout << "workflow: upstream-force-push\n";
            std::cout << "dry_run: " << (*upstreamDryRun ? "true" : "false") << "\n";
            std::cout << "total_ms: " << totalMs << "\n";
        }
        std::exit(code);
    });

    // --- sync stable-dev ---
    auto* stable_dev = cmd->add_subcommand("stable-dev", "Stable-dev sync (tag-based cherry-pick migration)");
    stable_dev->allow_extras();
    auto* stableDevWorkspace = new bool{false};
    auto* stableDevReportFormat = new std::string{"compact"};
    auto* stableDevRepo = new std::string{"."};
    auto* stableDevDryRun = new bool{false};
    auto* stableDevProfile = new bool{false};
    stable_dev->add_flag("--workspace", *stableDevWorkspace, "Run stable-dev across src/* submodules with aggregated summary report");
    stable_dev->add_option("--format", *stableDevReportFormat, "Workspace report format: compact|table|tsv|json|markdown");
    stable_dev->add_option("--repo", *stableDevRepo, "Single-repo mode target path");
    stable_dev->add_flag("--dry-run", *stableDevDryRun, "Preview stable-dev sync actions");
    stable_dev->add_flag("--profile", *stableDevProfile, "Print native sync timing/profile summary");
    stable_dev->callback([=]() {
        const auto start = std::chrono::steady_clock::now();
        auto extras = stable_dev->remaining();
        std::vector<std::string> args(extras.begin(), extras.end());

        if (!extras.empty()) {
            std::cerr << "Error: unsupported extra arguments in native sync stable-dev mode:";
            for (const auto& extra : extras) {
                std::cerr << ' ' << extra;
            }
            std::cerr << "\n";
            std::exit(2);
        }

        const auto format = ParseStableDevReportFormat(*stableDevReportFormat);
        if (!format.has_value()) {
            std::cerr << "ERROR: Unsupported --format: " << *stableDevReportFormat << " (supported: compact, table, tsv, json, markdown)\n";
            std::exit(1);
        }

        std::filesystem::path workspaceRoot;
        if (const char* rootEnv = std::getenv("KANO_GIT_MASTER_ROOT"); rootEnv != nullptr && std::string(rootEnv).size() > 0) {
            workspaceRoot = std::filesystem::weakly_canonical(std::filesystem::path(rootEnv));
        } else {
            workspaceRoot = std::filesystem::current_path();
        }

        if (*stableDevWorkspace) {
            if (!*stableDevDryRun && HasLongFlag(args, "--dry-run")) {
                *stableDevDryRun = true;
            }
            std::vector<std::string> workspaceArgs;
            if (*stableDevDryRun) {
                workspaceArgs.push_back("--dry-run");
            }
            const auto code = RunStableDevWorkspace(workspaceRoot, *format, workspaceArgs);
            if (*stableDevProfile) {
                const auto totalMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
                std::cout << "\n=== Sync Profile Summary ===\n";
                std::cout << "mode: native\n";
                std::cout << "workflow: stable-dev(workspace)\n";
                std::cout << "dry_run: " << (*stableDevDryRun ? "true" : "false") << "\n";
                std::cout << "total_ms: " << totalMs << "\n";
            }
            std::exit(code);
        }

        const auto repoPath = std::filesystem::weakly_canonical(std::filesystem::path(*stableDevRepo));
        const auto repoRel = std::filesystem::relative(repoPath, workspaceRoot).generic_string();
        const auto repoLabel = (repoRel.empty() || repoRel == ".") ? "." : repoRel;

        if (GitCapture(repoPath, {"rev-parse", "--is-inside-work-tree"}).exitCode != 0) {
            std::cerr << "ERROR: Not a git repository: " << repoPath.generic_string() << "\n";
            std::exit(1);
        }
        if (!HasRemote(repoPath, "upstream")) {
            std::cerr << "ERROR: Missing upstream remote for repo: " << repoLabel << "\n";
            std::exit(1);
        }

        StableDevSummaryRow row;
        const auto code = RunNativeStableDevSync(workspaceRoot, repoPath, repoLabel, *stableDevDryRun, row);

        std::cout << "=== upstream-stable-dev wrapper summary ===\n";
        std::cout << "success: " << (code == 0 ? 1 : 0) << "\n";
        std::cout << "skipped: 0\n";
        std::cout << "failed: " << (code == 0 ? 0 : 1) << "\n";
        std::cout << "OVERALL RESULT: " << (code == 0 ? "SUCCESS" : "FAILED") << "\n";
        std::cout << "=== upstream-stable-dev branch report ===\n";
        PrintStableDevSummary({row}, *format);
        if (*stableDevProfile) {
            const auto totalMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
            std::cout << "\n=== Sync Profile Summary ===\n";
            std::cout << "mode: native\n";
            std::cout << "workflow: stable-dev(single-repo)\n";
            std::cout << "dry_run: " << (*stableDevDryRun ? "true" : "false") << "\n";
            std::cout << "total_ms: " << totalMs << "\n";
        }
        std::exit(code == 0 ? 0 : 1);
    });

    // --- sync dev ---
    auto* dev = cmd->add_subcommand("dev", "Dev sync (upstream default branch tip)");
    dev->allow_extras();
    auto* devRepo = new std::string{"."};
    auto* devDryRun = new bool{false};
    auto* devMaxDepth = new int{6};
    auto* devNoCache = new bool{false};
    auto* devRefreshCache = new bool{false};
    auto* devNoRecursive = new bool{false};
    auto* devProfile = new bool{false};
    dev->add_option("--repo", *devRepo, "Target repository root path");
    dev->add_flag("--dry-run", *devDryRun, "Preview sync actions without modifying repositories");
    dev->add_option("--native-max-depth", *devMaxDepth, "Native discovery max depth");
    dev->add_flag("--native-no-cache", *devNoCache, "Disable native discovery cache");
    dev->add_flag("--native-refresh-cache", *devRefreshCache, "Force native cache refresh");
    dev->add_flag("--no-recursive,-N", *devNoRecursive, "Sync only current repository");
    dev->add_flag("--profile", *devProfile, "Print native sync timing/profile summary");
    dev->callback([=]() {
        auto extras = dev->remaining();
        if (!extras.empty()) {
            std::cerr << "Error: unsupported extra arguments in native sync dev mode:";
            for (const auto& extra : extras) {
                std::cerr << ' ' << extra;
            }
            std::cerr << "\n";
            std::exit(2);
        }

        const auto start = std::chrono::steady_clock::now();
        const auto repoRoot = std::filesystem::weakly_canonical(std::filesystem::path(*devRepo));
        const auto code = RunNativeOriginLatestSync(
            repoRoot,
            "upstream",
            *devMaxDepth,
            *devDryRun,
            *devNoCache,
            *devRefreshCache,
            !*devNoRecursive);
        if (*devProfile) {
            const auto totalMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
            std::cout << "\n=== Sync Profile Summary ===\n";
            std::cout << "mode: native\n";
            std::cout << "workflow: dev\n";
            std::cout << "recursive: " << (!*devNoRecursive ? "true" : "false") << "\n";
            std::cout << "dry_run: " << (*devDryRun ? "true" : "false") << "\n";
            std::cout << "total_ms: " << totalMs << "\n";
        }
        std::exit(code);
    });

    // --- sync (default: auto-detect) ---
    auto* defaultNoRecursive = new bool{false};
    auto* defaultProfile = new bool{false};
    cmd->add_flag("--no-recursive,-N", *defaultNoRecursive, "Default sync: only current repository");
    cmd->add_flag("--profile", *defaultProfile, "Default sync: print native timing/profile summary");
    cmd->allow_extras();
    cmd->callback([=]() {
        if (cmd->get_subcommands().empty()) {
            auto extras = cmd->remaining();
            if (!extras.empty()) {
                std::cerr << "Error: default sync in native-only mode does not accept extra args.\n";
                std::cerr << "Hint: use explicit subcommands, e.g. 'kog sync origin-latest'.\n";
                std::exit(2);
            }

            const auto start = std::chrono::steady_clock::now();
            const auto code = RunNativeOriginLatestSync(
                std::filesystem::current_path(),
                "origin",
                6,
                false,
                false,
                false,
                !*defaultNoRecursive);
            if (*defaultProfile) {
                const auto totalMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
                std::cout << "\n=== Sync Profile Summary ===\n";
                std::cout << "mode: native\n";
                std::cout << "workflow: default(origin-latest)\n";
                std::cout << "recursive: " << (!*defaultNoRecursive ? "true" : "false") << "\n";
                std::cout << "dry_run: false\n";
                std::cout << "total_ms: " << totalMs << "\n";
            }
            std::exit(code);
        }
    });
}

} // namespace kano::git::commands
