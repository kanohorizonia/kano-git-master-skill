// status/overview commands — dirty-focused status plus cached workspace overview

#include <CLI/CLI.hpp>
#include "discovery.hpp"
#include "repo_health.hpp"
#include "repo_operation_scheduler.hpp"
#include "shell_executor.hpp"
#include "terminal_color.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <format>
#include <iomanip>
#include <iostream>
#include <map>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
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

struct RecursiveRepoStatus {
    workspace::RepoRecord repo;
    std::string id;
    std::string relativePath;
    std::string absolutePath;
    int depth = 0;
    bool isWorkspaceRoot = false;
    bool isContainerRoot = false;
    bool isSubmodule = false;
    std::vector<std::string> parentRepos;
    std::vector<std::string> childRepos;
    std::string branch;
    std::string head;
    std::string remote;
    std::string upstream;
    int ahead = 0;
    int behind = 0;
    std::string dirtyKind = "CLEAN";
    std::vector<std::string> statusFlags;
    std::vector<std::string> submoduleFacts;
    bool conflicted = false;
    bool pushable = false;
    std::string selectedPushRemote;
    std::string registrationSource;
    std::string registrationRelativeTo;
    bool isPersistedInWorkspaceManifest = false;
    std::string containingRepo;
    std::string containingRelation;
    bool isGitlinkInContainingRepo = false;
    bool isIgnoredByContainingRepo = false;
    bool isExplicitlyAllowed = false;
    std::string managementPolicy;
    bool blocksConverge = false;
    std::string blockReason;
    std::string commandPolicySource;
    std::map<std::string, std::string> commandPolicy;
    std::vector<std::string> diagnostics;
};

struct RecursiveStatusSnapshot {
    std::filesystem::path workspaceRoot;
    std::vector<RecursiveRepoStatus> repos;
};

std::mutex gRecursiveStatusMutex;
std::unordered_map<std::string, RecursiveRepoStatus> gRecursiveStatusResults;

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

auto ToLower(std::string InValue) -> std::string {
    std::transform(InValue.begin(), InValue.end(), InValue.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return InValue;
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

auto EscapeJson(std::string InValue) -> std::string {
    std::string out;
    out.reserve(InValue.size() + 8);
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

auto PathKey(const std::filesystem::path& InPath) -> std::string {
    auto key = InPath.lexically_normal().generic_string();
    while (key.size() > 1 && key.back() == '/') {
        key.pop_back();
    }
#if defined(_WIN32)
    std::transform(key.begin(), key.end(), key.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
#endif
    return key;
}

auto ParseStatusChangedPath(std::string InLine) -> std::string {
    while (!InLine.empty() && (InLine.back() == '\r' || InLine.back() == '\n')) {
        InLine.pop_back();
    }
    if (InLine.size() < 4) {
        return {};
    }

    auto relPath = Trim(InLine.substr(3));
    const auto arrowPos = relPath.find(" -> ");
    if (arrowPos != std::string::npos) {
        relPath = Trim(relPath.substr(arrowPos + 4));
    }
    return relPath;
}

auto IsInternalArtifactPath(const std::string& InPath) -> bool {
    auto lower = InPath;
    std::replace(lower.begin(), lower.end(), '\\', '/');
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });

    auto matchDir = [&](const std::string& token) {
        if (lower == token) return true;
        if (lower.rfind(token + "/", 0) == 0) return true;
        if (lower.find("/" + token + "/") != std::string::npos) return true;
        if (lower.size() >= token.size() + 1 && lower.substr(lower.size() - token.size() - 1) == ("/" + token)) return true;
        return false;
    };

    return matchDir(".kano") || matchDir(".sisyphus");
}

auto FilterVisibleStatusLines(const std::string& InStatusText) -> std::vector<std::string> {
    std::vector<std::string> filtered;
    std::istringstream iss(InStatusText);
    std::string rawLine;
    while (std::getline(iss, rawLine)) {
        while (!rawLine.empty() && (rawLine.back() == '\r' || rawLine.back() == '\n')) {
            rawLine.pop_back();
        }
        if (Trim(rawLine).empty()) {
            continue;
        }
        if (rawLine.find(" ../") != std::string::npos || rawLine.rfind("../", 0) == 0) {
            continue;
        }
        auto normalizedLine = rawLine;
        std::replace(normalizedLine.begin(), normalizedLine.end(), '\\', '/');
        std::transform(normalizedLine.begin(), normalizedLine.end(), normalizedLine.begin(), [](unsigned char ch) {
            return static_cast<char>(std::tolower(ch));
        });
        if (normalizedLine.find(" .kano/") != std::string::npos ||
            normalizedLine.find(" .sisyphus/") != std::string::npos) {
            continue;
        }
        const auto path = ParseStatusChangedPath(rawLine);
        if (!path.empty() && IsInternalArtifactPath(path)) {
            continue;
        }
        filtered.push_back(rawLine);
    }
    return filtered;
}

auto GitCapture(const std::filesystem::path& InRepo, const std::vector<std::string>& InArgs) -> shell::ExecResult {
    return shell::ExecuteCommand("git", InArgs, shell::ExecMode::Capture, InRepo);
}

auto GitCaptureNoOptionalLocks(const std::filesystem::path& InRepo, const std::vector<std::string>& InArgs) -> shell::ExecResult {
    std::vector<std::string> args{"-c", "core.optionalLocks=false"};
    args.insert(args.end(), InArgs.begin(), InArgs.end());
    return GitCapture(InRepo, args);
}

auto IsUnsafeOwnershipGitError(const std::string& InText) -> bool {
    const auto lower = [&]() {
        std::string value = InText;
        std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
            return static_cast<char>(std::tolower(ch));
        });
        return value;
    }();
    return lower.find("detected dubious ownership") != std::string::npos ||
        lower.find("safe.directory") != std::string::npos;
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

auto ShortHead(const std::filesystem::path& InRepo) -> std::string {
    const auto out = GitCaptureNoOptionalLocks(InRepo, {"rev-parse", "--short=12", "HEAD"});
    return out.exitCode == 0 ? Trim(out.stdoutStr) : std::string{};
}

auto FullHead(const std::filesystem::path& InRepo) -> std::string {
    const auto out = GitCaptureNoOptionalLocks(InRepo, {"rev-parse", "HEAD"});
    return out.exitCode == 0 ? Trim(out.stdoutStr) : std::string{};
}

auto CurrentBranchForSnapshot(const std::filesystem::path& InRepo) -> std::string {
    const auto out = GitCaptureNoOptionalLocks(InRepo, {"symbolic-ref", "--quiet", "--short", "HEAD"});
    return out.exitCode == 0 ? Trim(out.stdoutStr) : std::string{"(detached)"};
}

auto UpstreamForSnapshot(const std::filesystem::path& InRepo) -> std::string {
    const auto out = GitCaptureNoOptionalLocks(InRepo, {"rev-parse", "--abbrev-ref", "--symbolic-full-name", "@{upstream}"});
    return out.exitCode == 0 ? Trim(out.stdoutStr) : std::string{};
}

auto RemoteFromUpstream(const std::string& InUpstream) -> std::string {
    const auto slash = InUpstream.find('/');
    if (slash == std::string::npos) {
        return {};
    }
    return InUpstream.substr(0, slash);
}

auto PushRemoteForSnapshot(const std::filesystem::path& InRepo, const std::string& InBranch, const std::string& InRemote) -> std::string {
    if (!InBranch.empty() && InBranch != "(detached)") {
        const auto branchRemote = GitCaptureNoOptionalLocks(InRepo, {"config", "--get", "branch." + InBranch + ".pushRemote"});
        if (branchRemote.exitCode == 0 && !Trim(branchRemote.stdoutStr).empty()) {
            return Trim(branchRemote.stdoutStr);
        }
    }
    const auto pushDefault = GitCaptureNoOptionalLocks(InRepo, {"config", "--get", "remote.pushDefault"});
    if (pushDefault.exitCode == 0 && !Trim(pushDefault.stdoutStr).empty()) {
        return Trim(pushDefault.stdoutStr);
    }
    return InRemote;
}

auto AheadBehindForSnapshot(const std::filesystem::path& InRepo, const std::string& InUpstream) -> std::pair<int, int> {
    if (InUpstream.empty()) {
        return {0, 0};
    }
    const auto out = GitCaptureNoOptionalLocks(InRepo, {"rev-list", "--left-right", "--count", InUpstream + "...HEAD"});
    if (out.exitCode != 0) {
        return {0, 0};
    }
    std::istringstream iss(out.stdoutStr);
    int behind = 0;
    int ahead = 0;
    iss >> behind >> ahead;
    return {ahead, behind};
}

auto ParseStatusFlag(const std::string& InLine) -> std::string {
    if (InLine.size() >= 2) {
        return InLine.substr(0, 2);
    }
    return InLine;
}

auto PushUnique(std::vector<std::string>* IoValues, const std::string& InValue) -> void {
    if (IoValues == nullptr || InValue.empty()) {
        return;
    }
    if (std::find(IoValues->begin(), IoValues->end(), InValue) == IoValues->end()) {
        IoValues->push_back(InValue);
    }
}

auto HasUnpushedCommit(const std::filesystem::path& InRepo, const std::string& InUpstream) -> bool {
    if (InUpstream.empty()) {
        return false;
    }
    const auto out = GitCaptureNoOptionalLocks(InRepo, {"rev-list", "--count", InUpstream + "..HEAD"});
    if (out.exitCode != 0) {
        return false;
    }
    try {
        return std::stoi(Trim(out.stdoutStr)) > 0;
    } catch (...) {
        return false;
    }
}

auto ParentGitlinkHead(const std::filesystem::path& InParent, const std::filesystem::path& InChild) -> std::string {
    const auto rel = InChild.lexically_normal().lexically_relative(InParent.lexically_normal()).generic_string();
    if (rel.empty() || rel.starts_with("..")) {
        return {};
    }
    const auto out = GitCaptureNoOptionalLocks(InParent, {"ls-tree", "HEAD", "--", rel});
    if (out.exitCode != 0) {
        return {};
    }
    std::istringstream iss(out.stdoutStr);
    std::string mode;
    std::string type;
    std::string sha;
    iss >> mode >> type >> sha;
    if (mode == "160000" && type == "commit") {
        return sha;
    }
    return {};
}

auto IsGitlinkPathInHead(const std::filesystem::path& InRepoRoot, const std::string& InPath) -> bool {
    if (InPath.empty()) {
        return false;
    }
    const auto out = GitCaptureNoOptionalLocks(InRepoRoot, {"ls-tree", "HEAD", "--", InPath});
    if (out.exitCode != 0) {
        return false;
    }
    const auto tree = Trim(out.stdoutStr);
    return !tree.empty() && tree.rfind("160000 ", 0) == 0;
}

auto ManagedSubmodulePathsFromGitmodules(const std::filesystem::path& InRepoRoot) -> std::unordered_set<std::string> {
    std::unordered_set<std::string> out;
    const auto cfg = GitCaptureNoOptionalLocks(InRepoRoot, {"config", "-f", ".gitmodules", "--get-regexp", "^submodule\\..*\\.path$"});
    if (cfg.exitCode != 0) {
        return out;
    }
    for (const auto& line : SplitNonEmptyLines(cfg.stdoutStr)) {
        const auto split = line.find(' ');
        if (split == std::string::npos) {
            continue;
        }
        const auto path = Trim(line.substr(split + 1));
        if (!path.empty()) {
            out.insert(path);
        }
    }
    return out;
}

auto IsIgnoredByContainingRepo(const std::filesystem::path& InParent, const std::filesystem::path& InChild) -> bool {
    const auto rel = InChild.lexically_normal().lexically_relative(InParent.lexically_normal()).generic_string();
    if (rel.empty() || rel.starts_with("..")) {
        return false;
    }
    const auto out = GitCaptureNoOptionalLocks(InParent, {"check-ignore", "-q", "--no-index", rel});
    return out.exitCode == 0;
}

auto LoadTrustedManifestPathKeys(const std::filesystem::path& InWorkspaceRoot) -> std::unordered_set<std::string> {
    std::unordered_set<std::string> out;
    std::string ignoredReason;
    const auto manifest = workspace::LoadTrustedWorkspaceManifest(InWorkspaceRoot, &ignoredReason);
    if (!manifest.has_value()) {
        return out;
    }
    for (const auto& repo : manifest->repos) {
        out.insert(PathKey(repo.path));
    }
    return out;
}

auto IsPolicyEnabled(const std::string& InValue) -> bool {
    const auto lowered = ToLower(Trim(InValue));
    if (lowered.empty()) {
        return true;
    }
    return lowered != "false" && lowered != "0" && lowered != "no" && lowered != "off" && lowered != "disabled";
}

auto PolicyEnabled(const std::map<std::string, std::string>& InPolicy, const char* InKey) -> bool {
    const auto it = InPolicy.find(InKey);
    if (it == InPolicy.end()) {
        return true;
    }
    return IsPolicyEnabled(it->second);
}

auto ResolveCommandPolicySource(const workspace::RepoRecord& InRepo,
                                bool InPersistedInManifest,
                                int InDepth,
                                bool InWorkspaceRoot) -> std::string {
    if (InWorkspaceRoot) {
        return "workspace-root";
    }
    if (InRepo.type == "registered") {
        return "gitmodules";
    }
    if (InPersistedInManifest) {
        return "workspace-manifest";
    }
    if (InDepth <= 1) {
        return "shallow-unregistered-probe";
    }
    return "full-scan";
}

auto RelativeId(const std::filesystem::path& InWorkspaceRoot, const std::filesystem::path& InPath) -> std::string {
    const auto root = InWorkspaceRoot.lexically_normal();
    const auto path = InPath.lexically_normal();
    if (PathKey(root) == PathKey(path)) {
        return ".";
    }
    const auto relative = path.lexically_relative(root);
    if (!relative.empty() && !relative.generic_string().starts_with("..")) {
        return relative.generic_string();
    }
    return path.generic_string();
}

auto PathDepth(const std::string& InRelativePath) -> int {
    if (InRelativePath.empty() || InRelativePath == ".") {
        return 0;
    }
    return static_cast<int>(std::count(InRelativePath.begin(), InRelativePath.end(), '/')) + 1;
}

auto JsonArray(const std::vector<std::string>& InValues) -> std::string {
    std::string out = "[";
    for (std::size_t i = 0; i < InValues.size(); ++i) {
        if (i > 0) {
            out += ",";
        }
        out += std::format("\"{}\"", EscapeJson(InValues[i]));
    }
    out += "]";
    return out;
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
        updated.hasChanges = statusOut.exitCode == 0 && !FilterVisibleStatusLines(statusOut.stdoutStr).empty();
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
        const auto typeCell = MaybeColorize(type, kano::terminal::Color::Dim, InColorize);

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
        row.repoDirty = !FilterVisibleStatusLines(statusQuick.stdoutStr).empty();
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
        row.statusLines = FilterVisibleStatusLines(statusOut.stdoutStr);
        row.repoDirty = !row.statusLines.empty();
    }
    
    return row;
}

auto BuildRecursiveRepoStatus(const workspace::RepoRecord& InRepo,
                              const std::filesystem::path& InWorkspaceRoot,
                              const std::unordered_map<std::string, workspace::RepoRecord>& InReposByPath,
                              const std::unordered_set<std::string>& InTrustedManifestPathKeys,
                              bool InSkipFetchHealth) -> RecursiveRepoStatus {
    RecursiveRepoStatus out;
    out.repo = InRepo;
    out.id = RelativeId(InWorkspaceRoot, InRepo.path);
    out.relativePath = out.id;
    out.absolutePath = InRepo.path.lexically_normal().generic_string();
    out.depth = PathDepth(out.relativePath);
    out.isWorkspaceRoot = InRepo.type == "root" || PathKey(InRepo.path) == PathKey(InWorkspaceRoot);
    out.registrationRelativeTo = InRepo.registrationRelativeTo.empty()
        ? std::string{}
        : RelativeId(InWorkspaceRoot, InRepo.registrationRelativeTo);
    out.registrationSource = out.isWorkspaceRoot ? "workspace-root" : (InRepo.type == "registered" ? "gitmodules" : "discover");
    out.isPersistedInWorkspaceManifest = InTrustedManifestPathKeys.contains(PathKey(InRepo.path));
    out.commandPolicySource = ResolveCommandPolicySource(InRepo, out.isPersistedInWorkspaceManifest, out.depth, out.isWorkspaceRoot);
    out.commandPolicy = {
        {"sync", InRepo.kogSyncPolicy},
        {"commit", InRepo.kogCommitPolicy},
        {"push", InRepo.kogPushPolicy},
        {"hygiene", InRepo.kogHygienePolicy},
    };

    for (const auto& dep : InRepo.dependencies) {
        out.parentRepos.push_back(RelativeId(InWorkspaceRoot, dep));
    }
    std::sort(out.parentRepos.begin(), out.parentRepos.end());
    out.parentRepos.erase(std::unique(out.parentRepos.begin(), out.parentRepos.end()), out.parentRepos.end());

    for (const auto& [pathKey, candidate] : InReposByPath) {
        (void)pathKey;
        for (const auto& dep : candidate.dependencies) {
            if (PathKey(dep) == PathKey(InRepo.path)) {
                out.childRepos.push_back(RelativeId(InWorkspaceRoot, candidate.path));
            }
        }
    }
    std::sort(out.childRepos.begin(), out.childRepos.end());
    out.childRepos.erase(std::unique(out.childRepos.begin(), out.childRepos.end()), out.childRepos.end());
    out.isContainerRoot = !out.childRepos.empty();

    if (!InRepo.dependencies.empty()) {
        const auto parentIt = InReposByPath.find(PathKey(InRepo.dependencies.front()));
        if (parentIt != InReposByPath.end()) {
            out.containingRepo = RelativeId(InWorkspaceRoot, parentIt->second.path);
            out.isIgnoredByContainingRepo = IsIgnoredByContainingRepo(parentIt->second.path, InRepo.path);
            const auto parentGitlink = ParentGitlinkHead(parentIt->second.path, InRepo.path);
            out.isGitlinkInContainingRepo = !parentGitlink.empty();
            out.isSubmodule = out.isGitlinkInContainingRepo || InRepo.type == "registered";
            out.containingRelation = out.isGitlinkInContainingRepo ? "gitlink" : "nested";
        }
    }

    out.branch = CurrentBranchForSnapshot(InRepo.path);
    out.head = FullHead(InRepo.path);
    out.upstream = UpstreamForSnapshot(InRepo.path);
    out.remote = RemoteFromUpstream(out.upstream);
    const auto [ahead, behind] = AheadBehindForSnapshot(InRepo.path, out.upstream);
    out.ahead = ahead;
    out.behind = behind;
    out.selectedPushRemote = PushRemoteForSnapshot(InRepo.path, out.branch, out.remote);
    out.pushable = !out.selectedPushRemote.empty() && out.branch != "(detached)";

    const auto statusOut = GitCaptureNoOptionalLocks(InRepo.path, {"status", "--porcelain=v1", "--untracked-files=normal"});
    const bool hasUnsafeOwnership = statusOut.exitCode != 0 && IsUnsafeOwnershipGitError(statusOut.stderrStr);
    std::vector<std::string> visibleStatus;
    if (statusOut.exitCode == 0) {
        visibleStatus = FilterVisibleStatusLines(statusOut.stdoutStr);
    } else {
        out.diagnostics.push_back("git status failed: " + Trim(statusOut.stderrStr));
    }

    bool hasContentDirty = false;
    bool hasGitlinkDirty = false;
    bool hasUntracked = false;
    bool hasIndexDirty = false;
    bool hasConflict = false;
    bool allVisibleUntracked = !visibleStatus.empty();

    auto managedGitlinkPaths = ManagedSubmodulePathsFromGitmodules(InRepo.path);
    for (const auto& childId : out.childRepos) {
        const auto childIt = std::find_if(InReposByPath.begin(), InReposByPath.end(), [&](const auto& entry) {
            return RelativeId(InWorkspaceRoot, entry.second.path) == childId;
        });
        if (childIt == InReposByPath.end()) {
            continue;
        }
        if (childIt->second.type != "registered") {
            continue;
        }
        const auto rel = childIt->second.path.lexically_normal().lexically_relative(InRepo.path.lexically_normal()).generic_string();
        if (!rel.empty() && !rel.starts_with("..")) {
            managedGitlinkPaths.insert(rel);
        }
    }

    std::vector<std::string> unregisteredGitlinkPaths;

    for (const auto& line : visibleStatus) {
        const auto flag = ParseStatusFlag(line);
        PushUnique(&out.statusFlags, flag);
        const auto path = ParseStatusChangedPath(line);
        const bool isGitlinkPath = IsGitlinkPathInHead(InRepo.path, path);
        const bool isManagedGitlinkPath = isGitlinkPath && managedGitlinkPaths.contains(path);
        if (flag == "??") {
            hasUntracked = true;
        } else {
            allVisibleUntracked = false;
        }
        if (!flag.empty() && flag[0] != ' ' && flag[0] != '?') {
            hasIndexDirty = true;
        }
        if (flag.find('U') != std::string::npos || flag == "AA" || flag == "DD") {
            hasConflict = true;
        }
        if (isManagedGitlinkPath) {
            hasGitlinkDirty = true;
            PushUnique(&out.submoduleFacts, "ParentGitlinkDirty");
        } else if (isGitlinkPath) {
            unregisteredGitlinkPaths.push_back(path);
        } else {
            hasContentDirty = true;
        }
    }

    std::sort(unregisteredGitlinkPaths.begin(), unregisteredGitlinkPaths.end());
    unregisteredGitlinkPaths.erase(std::unique(unregisteredGitlinkPaths.begin(), unregisteredGitlinkPaths.end()), unregisteredGitlinkPaths.end());
    for (const auto& path : unregisteredGitlinkPaths) {
        PushUnique(&out.submoduleFacts, "UnregisteredGitlinkSkipped:" + path);
        PushUnique(&out.diagnostics,
                   "UNREGISTERED_GITLINK_SKIPPED: " + path +
                       " no .gitmodules mapping; not registered as managed submodule; skipped parent pointer update");
    }

    if (out.isSubmodule && !out.containingRepo.empty()) {
        const auto parentIt = std::find_if(InReposByPath.begin(), InReposByPath.end(), [&](const auto& entry) {
            return RelativeId(InWorkspaceRoot, entry.second.path) == out.containingRepo;
        });
        if (parentIt != InReposByPath.end()) {
            const auto parentGitlink = ParentGitlinkHead(parentIt->second.path, InRepo.path);
            if (!parentGitlink.empty() && !out.head.empty() && parentGitlink != out.head) {
                PushUnique(&out.submoduleFacts, "SubmoduleHeadMoved");
            }
        }
    }
    if (out.isSubmodule && !visibleStatus.empty()) {
        PushUnique(&out.submoduleFacts, "SubmoduleWorktreeDirty");
    }
    if (out.isSubmodule && HasUnpushedCommit(InRepo.path, out.upstream)) {
        PushUnique(&out.submoduleFacts, "SubmoduleCommitUnpushed");
        PushUnique(&out.statusFlags, "UNPUSHED_SUBMODULE_COMMIT");
    }

    bool childWorktreeDirty = false;
    bool childCommitUnpushed = false;
    for (const auto& childId : out.childRepos) {
        const auto childIt = std::find_if(InReposByPath.begin(), InReposByPath.end(), [&](const auto& entry) {
            return RelativeId(InWorkspaceRoot, entry.second.path) == childId;
        });
        if (childIt == InReposByPath.end()) {
            continue;
        }
        const auto childStatusOut = GitCaptureNoOptionalLocks(childIt->second.path, {"status", "--porcelain=v1", "--untracked-files=normal"});
        if (childStatusOut.exitCode == 0 && !FilterVisibleStatusLines(childStatusOut.stdoutStr).empty()) {
            childWorktreeDirty = true;
        }
        if (HasUnpushedCommit(childIt->second.path, UpstreamForSnapshot(childIt->second.path))) {
            childCommitUnpushed = true;
        }
    }
    if (hasGitlinkDirty && childWorktreeDirty) {
        PushUnique(&out.submoduleFacts, "SubmoduleWorktreeDirty");
        PushUnique(&out.statusFlags, "CHILD_WORKTREE_DIRTY");
    }
    if (hasGitlinkDirty && childCommitUnpushed) {
        PushUnique(&out.submoduleFacts, "SubmoduleCommitUnpushed");
        PushUnique(&out.statusFlags, "CHILD_COMMIT_UNPUSHED");
        PushUnique(&out.statusFlags, "PARENT_POINTER_UNSAFE");
    }

    out.conflicted = hasConflict;
    if (hasUnsafeOwnership) {
        out.dirtyKind = "UNSAFE_OWNERSHIP";
    } else if (hasConflict) {
        out.dirtyKind = "CONFLICTED";
    } else if (hasIndexDirty) {
        out.dirtyKind = "INDEX_DIRTY";
    } else if (hasContentDirty && hasGitlinkDirty) {
        out.dirtyKind = "CONTENT_AND_GITLINK_DIRTY";
    } else if (hasGitlinkDirty) {
        out.dirtyKind = (childWorktreeDirty || childCommitUnpushed) ? "GITLINK_DIRTY_UNSAFE" : "GITLINK_DIRTY_ONLY";
    } else if (!unregisteredGitlinkPaths.empty() && !hasContentDirty) {
        out.dirtyKind = "UNREGISTERED_GITLINK_DIRTY_ONLY_SKIPPED";
    } else if (hasContentDirty) {
        out.dirtyKind = hasUntracked && allVisibleUntracked ? "UNTRACKED_ONLY" : "CONTENT_DIRTY";
    } else if (out.branch == "(detached)") {
        out.dirtyKind = "DETACHED_HEAD";
    } else if (out.upstream.empty()) {
        out.dirtyKind = out.remote.empty() ? "MISSING_REMOTE" : "CLEAN";
    } else if (out.ahead > 0 && out.behind > 0) {
        out.dirtyKind = "DIVERGED";
    } else if (out.ahead > 0) {
        out.dirtyKind = "AHEAD_ONLY";
    } else if (out.behind > 0) {
        out.dirtyKind = "BEHIND_ONLY";
    }

    if (out.dirtyKind != "CLEAN") {
        PushUnique(&out.statusFlags, out.dirtyKind);
    }
    if (out.upstream.empty() && out.remote.empty()) {
        PushUnique(&out.statusFlags, "MISSING_REMOTE");
    }
    if (out.branch == "(detached)") {
        PushUnique(&out.statusFlags, "DETACHED_HEAD");
    }

    if (InRepo.type == "unregistered") {
        out.isExplicitlyAllowed = out.isPersistedInWorkspaceManifest || out.isIgnoredByContainingRepo;
        if (out.isIgnoredByContainingRepo) {
            out.managementPolicy = "ignored";
            out.blocksConverge = false;
        } else if (out.isPersistedInWorkspaceManifest) {
            out.managementPolicy = "manifest-trusted";
            out.blocksConverge = false;
        } else if (out.depth <= 1) {
            out.isExplicitlyAllowed = true;
            out.managementPolicy = "discovered-top-level";
            out.blocksConverge = false;
        } else {
            out.managementPolicy = "discovered-untrusted";
            out.blocksConverge = true;
            out.blockReason = "Discovered unregistered nested Git repository that is not in the trusted workspace manifest. Register it as a submodule/subrepo, ignore it, or move it outside the workspace. If this is intended, run: kog discover --unregistered-depth 1";
        }
    } else {
        out.isExplicitlyAllowed = true;
        out.managementPolicy = InRepo.type == "root" ? "workspace-root" : "managed";
        out.blocksConverge = false;
    }

    if (hasUnsafeOwnership) {
        out.blocksConverge = true;
        out.blockReason = "UNSAFE_OWNERSHIP: run kog doctor --fix-safe-directory before converge mutation";
    }

    if (out.isSubmodule && std::find(out.submoduleFacts.begin(), out.submoduleFacts.end(), "SubmoduleCommitUnpushed") != out.submoduleFacts.end()) {
        PushUnique(&out.statusFlags, "UNPUSHED_SUBMODULE_COMMIT");
    }

    const bool needsSubmoduleHealth = !InSkipFetchHealth && (out.isContainerRoot ||
        !managedGitlinkPaths.empty() ||
        hasGitlinkDirty ||
        !unregisteredGitlinkPaths.empty());

    if (!InSkipFetchHealth || needsSubmoduleHealth) {
        const auto health = workspace::ScanRepoHealth(InRepo.path, workspace::RepoHealthOptions{
            .checkFetchRemotes = !InSkipFetchHealth,
            .checkSubmoduleStatus = needsSubmoduleHealth,
            .checkGitlinkReachability = needsSubmoduleHealth,
            .fetchDryRun = !InSkipFetchHealth,
            .blockOnDetachedHead = true,
            .blockOnNoUpstream = true,
            .blockOnUnpushedCommits = false,
            .blockOnDirtyWorktree = false,
            .blockOnDirtySubmodule = false,
            .strictSubmoduleMappings = false,
            .managedSubmodulePaths = std::move(managedGitlinkPaths),
        });

        for (const auto& flag : health.statusFlags) {
            PushUnique(&out.statusFlags, flag);
        }
        for (const auto& diagnostic : health.diagnostics) {
            PushUnique(&out.diagnostics, diagnostic);
        }
        if (health.hasUnmergedPaths) {
            out.conflicted = true;
            out.dirtyKind = "CONFLICTED";
        } else if (health.detachedHead && out.dirtyKind == "CLEAN") {
            out.dirtyKind = "DETACHED_HEAD";
        }

        for (const auto& blocker : health.blockers) {
            PushUnique(&out.statusFlags, blocker.reasonCode);
            PushUnique(&out.diagnostics, "preflight " + blocker.reasonCode + ": " + blocker.detail);
            if (out.blockReason.empty()) {
                out.blockReason = blocker.reasonCode + ": " + blocker.detail;
            }
            out.blocksConverge = true;
        }
    }
    if (InSkipFetchHealth &&
        !out.isWorkspaceRoot &&
        out.ahead <= 0 &&
        (out.dirtyKind == "DETACHED_HEAD" || out.dirtyKind == "MISSING_REMOTE")) {
        out.blocksConverge = true;
        if (out.blockReason.empty()) {
            out.blockReason = out.dirtyKind + ": clean nested preflight-only branch blocker";
        }
    }

    std::sort(out.statusFlags.begin(), out.statusFlags.end());
    std::sort(out.submoduleFacts.begin(), out.submoduleFacts.end());
    std::sort(out.diagnostics.begin(), out.diagnostics.end());
    return out;
}

auto BuildRecursiveStatusSnapshot(const std::filesystem::path& InWorkspaceRoot,
                                  bool InUseCache,
                                  bool InRefreshCache,
                                  int InUnregisteredDepth,
                                  int InJobs,
                                  bool InSkipFetchHealth) -> RecursiveStatusSnapshot {
    workspace::WorkspaceInventoryOptions inventoryOptions;
    inventoryOptions.rootDir = InWorkspaceRoot;
    inventoryOptions.unregisteredDepth = InUnregisteredDepth;
    inventoryOptions.useCache = InUseCache;
    inventoryOptions.refreshCache = InRefreshCache;
    inventoryOptions.includeTrustedUnregistered = true;
    inventoryOptions.metadataLevel = "minimal";
    inventoryOptions.scope = InUnregisteredDepth > 0 ? workspace::DiscoverScope::Full : workspace::DiscoverScope::RegisteredOnly;

    const auto trustedManifestPathKeys = LoadTrustedManifestPathKeys(InWorkspaceRoot);
    auto repos = workspace::DiscoverWorkspaceInventory(inventoryOptions);
    std::sort(repos.begin(), repos.end(), [](const auto& A, const auto& B) {
        return PathKey(A.path) < PathKey(B.path);
    });

    std::unordered_map<std::string, workspace::RepoRecord> reposByPath;
    reposByPath.reserve(repos.size());
    for (const auto& repo : repos) {
        reposByPath.emplace(PathKey(repo.path), repo);
    }

    auto inputs = workspace::MakeRepoOperationInputs(repos);
    workspace::RepoOperationSchedulerOptions schedulerOptions;
    schedulerOptions.operationName = "recursive-status";
    schedulerOptions.mode = workspace::RepoOperationMode::ReadOnlyParallel;
    schedulerOptions.jobs = std::max(1, InJobs);
    schedulerOptions.resolveGitCommonDirLocks = true;

    {
        std::lock_guard lock(gRecursiveStatusMutex);
        gRecursiveStatusResults.clear();
    }

    const auto aggregate = workspace::RunRepoOperationScheduler(inputs, schedulerOptions, [&](const workspace::RepoOperationInput& InInput) {
        workspace::RepoOperationWorkerResult result;
        const auto it = reposByPath.find(PathKey(InInput.path));
        if (it == reposByPath.end()) {
            result.status = workspace::RepoOperationStatus::Failed;
            result.exitCode = 1;
            result.failureCategory = "repo-missing";
            result.message = "repo missing from discovery inventory";
            return result;
        }
        const auto status = BuildRecursiveRepoStatus(it->second, InWorkspaceRoot, reposByPath, trustedManifestPathKeys, InSkipFetchHealth);
        {
            std::lock_guard lock(gRecursiveStatusMutex);
            gRecursiveStatusResults.insert_or_assign(PathKey(InInput.path), status);
        }
        std::ostringstream payload;
        payload << status.id << "\n";
        payload << status.dirtyKind << "\n";
        for (const auto& diagnostic : status.diagnostics) {
            payload << "diagnostic:" << diagnostic << "\n";
        }
        result.stdoutText = payload.str();
        return result;
    });

    RecursiveStatusSnapshot snapshot;
    snapshot.workspaceRoot = InWorkspaceRoot.lexically_normal();
    snapshot.repos.reserve(aggregate.results.size());
    for (const auto& result : aggregate.results) {
        RecursiveRepoStatus status;
        const auto resultKey = PathKey(result.repoPath);
        {
            std::lock_guard lock(gRecursiveStatusMutex);
            const auto statusIt = gRecursiveStatusResults.find(resultKey);
            if (statusIt == gRecursiveStatusResults.end()) {
                continue;
            }
            status = statusIt->second;
            gRecursiveStatusResults.erase(statusIt);
        }
        if (result.status != workspace::RepoOperationStatus::Succeeded) {
            status.diagnostics.push_back(result.message.empty() ? "scheduler status check failed" : result.message);
        }
        snapshot.repos.push_back(std::move(status));
    }
    std::sort(snapshot.repos.begin(), snapshot.repos.end(), [](const auto& A, const auto& B) {
        return A.id < B.id;
    });
    return snapshot;
}

auto FormatRecursiveStatusJson(const RecursiveStatusSnapshot& InSnapshot) -> std::string {
    std::size_t dirty = 0;
    std::size_t blocked = 0;
    std::map<std::string, std::size_t> byType;
    std::map<std::string, std::size_t> byDirtyKind;
    for (const auto& repo : InSnapshot.repos) {
        byType[repo.repo.type] += 1;
        byDirtyKind[repo.dirtyKind] += 1;
        if (repo.dirtyKind != "CLEAN") {
            dirty += 1;
        }
        if (repo.blocksConverge) {
            blocked += 1;
        }
    }

    std::ostringstream out;
    out << "{";
    out << "\"schemaName\":\"kog.recursiveStatusSnapshot\",";
    out << "\"schemaVersion\":1,";
    out << "\"workspaceRoot\":\"" << EscapeJson(InSnapshot.workspaceRoot.generic_string()) << "\",";
    out << "\"repos\":[";
    for (std::size_t i = 0; i < InSnapshot.repos.size(); ++i) {
        if (i > 0) {
            out << ",";
        }
        const auto& repo = InSnapshot.repos[i];
        out << "{";
        out << "\"id\":\"" << EscapeJson(repo.id) << "\",";
        out << "\"type\":\"" << EscapeJson(repo.repo.type) << "\",";
        out << "\"path\":\"" << EscapeJson(repo.relativePath) << "\",";
        out << "\"absolutePath\":\"" << EscapeJson(repo.absolutePath) << "\",";
        out << "\"depth\":" << repo.depth << ",";
        out << "\"isWorkspaceRoot\":" << (repo.isWorkspaceRoot ? "true" : "false") << ",";
        out << "\"isContainerRoot\":" << (repo.isContainerRoot ? "true" : "false") << ",";
        out << "\"isSubmodule\":" << (repo.isSubmodule ? "true" : "false") << ",";
        out << "\"parentRepos\":" << JsonArray(repo.parentRepos) << ",";
        out << "\"childRepos\":" << JsonArray(repo.childRepos) << ",";
        out << "\"branch\":\"" << EscapeJson(repo.branch) << "\",";
        out << "\"head\":\"" << EscapeJson(repo.head) << "\",";
        out << "\"remote\":\"" << EscapeJson(repo.remote) << "\",";
        out << "\"upstream\":\"" << EscapeJson(repo.upstream) << "\",";
        out << "\"ahead\":" << repo.ahead << ",";
        out << "\"behind\":" << repo.behind << ",";
        out << "\"dirtyKind\":\"" << EscapeJson(repo.dirtyKind) << "\",";
        out << "\"statusFlags\":" << JsonArray(repo.statusFlags) << ",";
        out << "\"submoduleFacts\":" << JsonArray(repo.submoduleFacts) << ",";
        const bool submoduleWorktreeDirty = std::find(repo.submoduleFacts.begin(), repo.submoduleFacts.end(), "SubmoduleWorktreeDirty") != repo.submoduleFacts.end();
        const bool submoduleHeadMoved = std::find(repo.submoduleFacts.begin(), repo.submoduleFacts.end(), "SubmoduleHeadMoved") != repo.submoduleFacts.end();
        const bool submoduleCommitUnpushed = std::find(repo.submoduleFacts.begin(), repo.submoduleFacts.end(), "SubmoduleCommitUnpushed") != repo.submoduleFacts.end();
        const bool parentGitlinkDirty = std::find(repo.submoduleFacts.begin(), repo.submoduleFacts.end(), "ParentGitlinkDirty") != repo.submoduleFacts.end();
        out << "\"submoduleSafety\":{";
        out << "\"submoduleWorktreeDirty\":" << (submoduleWorktreeDirty ? "true" : "false") << ",";
        out << "\"submoduleHeadMoved\":" << (submoduleHeadMoved ? "true" : "false") << ",";
        out << "\"submoduleCommitUnpushed\":" << (submoduleCommitUnpushed ? "true" : "false") << ",";
        out << "\"parentGitlinkDirty\":" << (parentGitlinkDirty ? "true" : "false");
        out << "},";
        out << "\"conflicted\":" << (repo.conflicted ? "true" : "false") << ",";
        out << "\"pushable\":" << (repo.pushable ? "true" : "false") << ",";
        out << "\"selectedPushRemote\":\"" << EscapeJson(repo.selectedPushRemote) << "\",";
        out << "\"registrationSource\":\"" << EscapeJson(repo.registrationSource) << "\",";
        out << "\"registrationRelativeTo\":\"" << EscapeJson(repo.registrationRelativeTo) << "\",";
        out << "\"isPersistedInWorkspaceManifest\":" << (repo.isPersistedInWorkspaceManifest ? "true" : "false") << ",";
        out << "\"containingRepo\":\"" << EscapeJson(repo.containingRepo) << "\",";
        out << "\"containingRelation\":\"" << EscapeJson(repo.containingRelation) << "\",";
        out << "\"isGitlinkInContainingRepo\":" << (repo.isGitlinkInContainingRepo ? "true" : "false") << ",";
        out << "\"isIgnoredByContainingRepo\":" << (repo.isIgnoredByContainingRepo ? "true" : "false") << ",";
        out << "\"isExplicitlyAllowed\":" << (repo.isExplicitlyAllowed ? "true" : "false") << ",";
        out << "\"managementPolicy\":\"" << EscapeJson(repo.managementPolicy) << "\",";
        out << "\"blocksConverge\":" << (repo.blocksConverge ? "true" : "false") << ",";
        out << "\"blockReason\":\"" << EscapeJson(repo.blockReason) << "\",";
        out << "\"commandPolicy\":{";
        out << "\"sync\":" << (PolicyEnabled(repo.commandPolicy, "sync") ? "true" : "false") << ",";
        out << "\"commit\":" << (PolicyEnabled(repo.commandPolicy, "commit") ? "true" : "false") << ",";
        out << "\"push\":" << (PolicyEnabled(repo.commandPolicy, "push") ? "true" : "false") << ",";
        out << "\"hygiene\":" << (PolicyEnabled(repo.commandPolicy, "hygiene") ? "true" : "false") << ",";
        out << "\"source\":\"" << EscapeJson(repo.commandPolicySource) << "\"";
        out << "},";
        out << "\"diagnostics\":" << JsonArray(repo.diagnostics);
        out << "}";
    }
    out << "],";
    out << "\"summary\":{";
    out << "\"repoCount\":" << InSnapshot.repos.size() << ",";
    out << "\"dirtyCount\":" << dirty << ",";
    out << "\"blocksConvergeCount\":" << blocked << ",";
    out << "\"byType\":{";
    std::size_t typeIndex = 0;
    for (const auto& [key, value] : byType) {
        if (typeIndex++ > 0) {
            out << ",";
        }
        out << "\"" << EscapeJson(key) << "\":" << value;
    }
    out << "},\"byDirtyKind\":{";
    std::size_t dirtyIndex = 0;
    for (const auto& [key, value] : byDirtyKind) {
        if (dirtyIndex++ > 0) {
            out << ",";
        }
        out << "\"" << EscapeJson(key) << "\":" << value;
    }
    out << "}}";
    out << "}";
    return out.str();
}

auto FormatRecursiveStatusSummary(const RecursiveStatusSnapshot& InSnapshot) -> std::string {
    std::ostringstream out;
    std::size_t dirty = 0;
    std::size_t blocked = 0;
    std::size_t conflicted = 0;
    for (const auto& repo : InSnapshot.repos) {
        dirty += repo.dirtyKind == "CLEAN" ? 0 : 1;
        blocked += repo.blocksConverge ? 1 : 0;
        conflicted += repo.conflicted ? 1 : 0;
    }
    out << "Recursive status summary\n";
    out << "workspaceRoot=" << InSnapshot.workspaceRoot.generic_string() << "\n";
    out << "repos=" << InSnapshot.repos.size() << " dirty=" << dirty << " conflicted=" << conflicted << " blocksConverge=" << blocked << "\n";
    for (const auto& repo : InSnapshot.repos) {
        if (repo.conflicted) {
            out << "[CONFLICT] ";
        }
        out << repo.id << " type=" << repo.repo.type << " dirtyKind=" << repo.dirtyKind
            << " policy=" << repo.managementPolicy;
        if (repo.conflicted) {
            out << " conflicted=true";
        }
        if (repo.blocksConverge) {
            out << " blocksConverge=true reason=\"" << repo.blockReason << "\"";
        }
        out << "\n";
    }
    return out.str();
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

    for (const auto& repo : InRepos) {
        rows.push_back(MakeRepoView(repo, InRoot));
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
    auto* recursive = new bool{false};
    auto* json = new bool{false};
    auto* summary = new bool{false};
    auto* jobs = new int{1};
    auto* noUnregisteredScan = new bool{false};
    auto* noFetchHealth = new bool{false};
    auto* unregisteredDepth = new int{2};
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
    cmd->add_flag("--recursive", *recursive, "Emit recursive workspace status snapshot using discover inventory");
    cmd->add_flag("--json", *json, "Shortcut for --recursive --format json");
    cmd->add_flag("--summary", *summary, "Shortcut for --recursive --format summary");
    cmd->add_option("--jobs", *jobs, "Parallel read-only status checks for recursive snapshot")->default_str("1");
    cmd->add_option("--unregistered-depth", *unregisteredDepth, "Bounded unregistered discovery depth when refresh is requested")->default_str("2");
    cmd->add_flag("--no-unregistered-scan", *noUnregisteredScan, "Do not perform new unregistered filesystem probing; keep discover trusted manifest/cache repos");
    cmd->add_flag("--no-fetch-health", *noFetchHealth, "Skip remote fetch dry-run health checks in recursive status snapshots");

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
        const auto effectiveFormat = *json ? std::string{"json"} : (*summary ? std::string{"summary"} : *format);
        if (effectiveFormat != "table" && effectiveFormat != "json" && effectiveFormat != "markdown" && effectiveFormat != "summary") {
            std::cerr << "Error: invalid --format value: " << effectiveFormat << " (expected table|json|markdown|summary)\n";
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

        if (*recursive || *json || *summary || effectiveFormat == "summary") {
            const auto snapshot = BuildRecursiveStatusSnapshot(
                root,
                !*noCache,
                *refreshCache && !*noUnregisteredScan,
                *noUnregisteredScan ? 0 : *unregisteredDepth,
                *jobs,
                *noFetchHealth);
            const auto rendered = effectiveFormat == "json"
                ? FormatRecursiveStatusJson(snapshot)
                : FormatRecursiveStatusSummary(snapshot);
            if (!output->empty()) {
                std::ofstream out(*output, std::ios::out | std::ios::binary | std::ios::trunc);
                out << rendered;
            } else {
                std::cout << rendered << '\n';
            }
            return;
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
        const auto statusOut = GitCapture(repoPath, {"status", "--porcelain"});
        record.hasChanges = statusOut.exitCode == 0 && !FilterVisibleStatusLines(statusOut.stdoutStr).empty();

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
