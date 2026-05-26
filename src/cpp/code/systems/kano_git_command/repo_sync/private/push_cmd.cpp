// push command — Multi-remote push workflow
// Delegates to: scripts/commit-tools/smart-push.sh

#include <CLI/CLI.hpp>
#include "shell_executor.hpp"
#include "discovery.hpp"
#include "repo_operation_scheduler.hpp"
#include "terminal_color.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <chrono>
#include <unordered_map>
#include <unordered_set>
#include <mutex>
#include <thread>
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

auto ToLower(std::string InValue) -> std::string {
    std::transform(InValue.begin(), InValue.end(), InValue.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return InValue;
}

auto LooksLikeLfsPushFailure(const shell::ExecResult& InResult) -> bool {
    const auto merged = ToLower(InResult.stdoutStr + "\n" + InResult.stderrStr);
    if (merged.find("git-lfs-authenticate") != std::string::npos) {
        return true;
    }
    if (merged.find("batch request") != std::string::npos && merged.find("lfs") != std::string::npos) {
        return true;
    }
    if (merged.find("uploading lfs objects") != std::string::npos &&
        merged.find("failed to push some refs") != std::string::npos) {
        return true;
    }
    return false;
}

auto PrintCapturedOutputIfAny(const shell::ExecResult& InResult) -> void {
    if (!InResult.stdoutStr.empty()) {
        std::cout << InResult.stdoutStr;
    }
    if (!InResult.stderrStr.empty()) {
        std::cerr << InResult.stderrStr;
    }
}

auto GitCapture(const std::filesystem::path& InRepo, const std::vector<std::string>& InArgs) -> shell::ExecResult {
    return shell::ExecuteCommand("git", InArgs, shell::ExecMode::Capture, InRepo);
}

auto IsGitRepo(const std::filesystem::path& InRepo) -> bool {
    const auto inside = GitCapture(InRepo, {"rev-parse", "--is-inside-work-tree"});
    if (inside.exitCode != 0 || Trim(inside.stdoutStr) != "true") {
        return false;
    }
    const auto topLevel = GitCapture(InRepo, {"rev-parse", "--show-toplevel"});
    if (topLevel.exitCode != 0) {
        return false;
    }
    std::error_code ec;
    const auto repoAbs = std::filesystem::weakly_canonical(InRepo, ec);
    const auto topAbs = std::filesystem::weakly_canonical(std::filesystem::path(Trim(topLevel.stdoutStr)), ec);
    return repoAbs == topAbs;
}

auto GitPassThrough(const std::filesystem::path& InRepo, const std::vector<std::string>& InArgs) -> shell::ExecResult {
    return shell::ExecuteCommand("git", InArgs, shell::ExecMode::PassThrough, InRepo);
}

auto HasRemote(const std::filesystem::path& InRepo, const std::string& InRemote) -> bool {
    const auto out = GitCapture(InRepo, {"remote", "get-url", InRemote});
    return out.exitCode == 0;
}

auto GetRemoteUrl(const std::filesystem::path& InRepo, const std::string& InRemote) -> std::optional<std::string> {
    const auto out = GitCapture(InRepo, {"remote", "get-url", InRemote});
    if (out.exitCode != 0) {
        return std::nullopt;
    }
    const auto url = Trim(out.stdoutStr);
    if (url.empty()) {
        return std::nullopt;
    }
    return url;
}

auto ParseNonNegativeInt(const std::string& InValue) -> int {
    const auto trimmed = Trim(InValue);
    if (trimmed.empty()) {
        return 0;
    }
    try {
        return std::max(0, std::stoi(trimmed));
    } catch (const std::exception&) {
        return 0;
    }
}

auto HasCommitsToPush(const std::filesystem::path& InRepo,
                      const std::string& InRemote,
                      const std::string& InBranch) -> bool {
    const auto remoteRef = std::format("refs/remotes/{}/{}", InRemote, InBranch);
    const auto localRef = std::format("refs/heads/{}", InBranch);

    const auto hasRemoteRef = GitCapture(InRepo, {"show-ref", "--verify", "--quiet", remoteRef}).exitCode == 0;
    if (!hasRemoteRef) {
        return true;
    }

    const auto ahead = GitCapture(InRepo, {"rev-list", "--count", std::format("{}..{}", remoteRef, localRef)});
    if (ahead.exitCode != 0) {
        return true;
    }

    return ParseNonNegativeInt(ahead.stdoutStr) > 0;
}

auto ParseReposCsv(const std::string& InCsv) -> std::vector<std::filesystem::path> {
    std::vector<std::filesystem::path> out;
    std::istringstream iss(InCsv);
    std::string token;
    while (std::getline(iss, token, ',')) {
        const auto trimmed = Trim(token);
        if (trimmed.empty()) {
            continue;
        }
        out.emplace_back(trimmed);
    }
    return out;
}

auto RepoKey(const std::filesystem::path& InPath) -> std::string {
    std::error_code ec;
    const auto canonical = std::filesystem::weakly_canonical(InPath, ec);
    const auto normalized = (ec ? InPath : canonical).lexically_normal().generic_string();
#if defined(_WIN32)
    return ToLower(normalized);
#else
    return normalized;
#endif
}

auto GitCommonDirKey(const std::filesystem::path& InRepo) -> std::string {
    const auto out = GitCapture(InRepo, {"rev-parse", "--git-common-dir"});
    if (out.exitCode != 0) {
        return {};
    }
    auto raw = Trim(out.stdoutStr);
    if (raw.empty()) {
        return {};
    }
    std::filesystem::path commonDir(raw);
    if (commonDir.is_relative()) {
        commonDir = (InRepo / commonDir).lexically_normal();
    }
    return RepoKey(commonDir);
}

auto IsFalsePolicy(std::string InValue) -> bool {
    InValue = ToLower(Trim(std::move(InValue)));
    return InValue == "false" || InValue == "0" || InValue == "no" || InValue == "off" || InValue == "disabled";
}

auto RepoTypeRank(const std::string& InType) -> int {
    if (InType == "root") {
        return 0;
    }
    if (InType == "registered") {
        return 1;
    }
    if (InType == "unregistered") {
        return 2;
    }
    return 3;
}

auto NormalizeRepoRecord(workspace::RepoRecord InRecord) -> workspace::RepoRecord {
    std::error_code ec;
    auto path = std::filesystem::weakly_canonical(InRecord.path, ec);
    if (ec) {
        path = std::filesystem::absolute(InRecord.path, ec);
    }
    if (!ec) {
        InRecord.path = path.lexically_normal();
    } else {
        InRecord.path = InRecord.path.lexically_normal();
    }

    if (!InRecord.registrationRelativeTo.empty() && InRecord.registrationRelativeTo != ".") {
        ec.clear();
        auto parent = std::filesystem::weakly_canonical(InRecord.registrationRelativeTo, ec);
        if (ec) {
            parent = std::filesystem::absolute(InRecord.registrationRelativeTo, ec);
        }
        InRecord.registrationRelativeTo = ec ? InRecord.registrationRelativeTo.lexically_normal() : parent.lexically_normal();
    }

    return InRecord;
}

auto DedupePushRepoRecords(std::vector<workspace::RepoRecord> InRepos) -> std::vector<workspace::RepoRecord> {
    for (auto& repo : InRepos) {
        repo = NormalizeRepoRecord(std::move(repo));
    }
    std::sort(InRepos.begin(), InRepos.end(), [](const auto& A, const auto& B) {
        const auto rankA = RepoTypeRank(A.type);
        const auto rankB = RepoTypeRank(B.type);
        if (rankA != rankB) {
            return rankA < rankB;
        }
        return RepoKey(A.path) < RepoKey(B.path);
    });

    std::unordered_set<std::string> seenWorktrees;
    std::unordered_set<std::string> seenCommonDirs;
    std::vector<workspace::RepoRecord> out;
    out.reserve(InRepos.size());
    for (auto& repo : InRepos) {
        if (!IsGitRepo(repo.path)) {
            continue;
        }
        const auto worktreeKey = RepoKey(repo.path);
        const auto commonDirKey = GitCommonDirKey(repo.path);
        if (seenWorktrees.contains(worktreeKey)) {
            continue;
        }
        if (!commonDirKey.empty() && seenCommonDirs.contains(commonDirKey)) {
            continue;
        }
        seenWorktrees.insert(worktreeKey);
        if (!commonDirKey.empty()) {
            seenCommonDirs.insert(commonDirKey);
        }
        out.push_back(std::move(repo));
    }

    std::sort(out.begin(), out.end(), [](const auto& A, const auto& B) {
        return RepoKey(A.path) < RepoKey(B.path);
    });
    return out;
}

auto DiscoverWorkspaceRepos(const std::filesystem::path& InRoot) -> std::vector<workspace::RepoRecord> {
    workspace::WorkspaceInventoryOptions options;
    options.rootDir = InRoot;
    options.unregisteredDepth = 1;
    options.useCache = true;
    options.refreshCache = false;
    options.metadataLevel = "minimal";
    options.scope = workspace::DiscoverScope::RegisteredOnly;
    options.includeTrustedUnregistered = true;
    auto repos = workspace::DiscoverWorkspaceInventory(options);

    std::error_code ec;
    auto rootPath = std::filesystem::weakly_canonical(InRoot, ec);
    if (ec) {
        ec.clear();
        rootPath = std::filesystem::absolute(InRoot, ec);
    }
    if (!ec) {
        rootPath = rootPath.lexically_normal();
    } else {
        rootPath = InRoot.lexically_normal();
    }

    const auto rootKey = RepoKey(rootPath);
    const bool hasRoot = std::any_of(repos.begin(), repos.end(), [&](const auto& repo) {
        return RepoKey(repo.path) == rootKey;
    });
    if (!hasRoot && IsGitRepo(rootPath)) {
        workspace::RepoRecord rootRecord;
        rootRecord.path = rootPath;
        rootRecord.type = "root";
        rootRecord.registrationRelativeTo = std::filesystem::path{"."};
        repos.push_back(std::move(rootRecord));
    }

    return DedupePushRepoRecords(std::move(repos));
}

auto BuildExplicitRepoRecords(const std::filesystem::path& InWorkspaceRoot,
                              const std::vector<std::filesystem::path>& InRepos) -> std::vector<workspace::RepoRecord> {
    auto inventory = DiscoverWorkspaceRepos(InWorkspaceRoot);
    std::unordered_map<std::string, workspace::RepoRecord> byPath;
    byPath.reserve(inventory.size());
    for (auto& repo : inventory) {
        byPath.emplace(RepoKey(repo.path), std::move(repo));
    }

    std::vector<workspace::RepoRecord> out;
    out.reserve(InRepos.size());
    for (const auto& path : InRepos) {
        const auto key = RepoKey(path);
        if (const auto it = byPath.find(key); it != byPath.end()) {
            out.push_back(it->second);
            continue;
        }
        workspace::RepoRecord record;
        record.path = path;
        record.type = RepoKey(path) == RepoKey(InWorkspaceRoot) ? "root" : "unregistered";
        record.registrationRelativeTo = record.type == "root" ? std::filesystem::path{"."} : InWorkspaceRoot;
        out.push_back(std::move(record));
    }
    return DedupePushRepoRecords(std::move(out));
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
                         const std::filesystem::path& InSpec,
                         const int InMaxDepth,
                         const bool InUseCache) -> std::filesystem::path {
    if (InSpec.empty() || InSpec == ".") {
        return InRoot.lexically_normal();
    }

    const auto specText = InSpec.generic_string();
    const auto candidate = (InSpec.is_absolute() ? InSpec : (InRoot / InSpec)).lexically_normal();
    if (std::filesystem::exists(candidate) && IsGitRepo(candidate)) {
        return candidate;
    }

    std::string manifestReason;
    if (const auto manifest = workspace::LoadTrustedWorkspaceManifest(InRoot, &manifestReason); manifest.has_value()) {
        std::vector<std::filesystem::path> exactMatches;
        std::vector<std::filesystem::path> fuzzyMatches;
        for (const auto& repo : manifest->repos) {
            const auto repoPath = repo.path.lexically_normal();
            const auto repoName = RepoNameFromPath(repoPath);
            const auto repoKey = repoPath.generic_string();
            const auto relativeKey = RelativeDisplayPath(InRoot, repoPath).generic_string();
            if (repoName == specText || repoKey == specText || relativeKey == specText) {
                exactMatches.push_back(repoPath);
                continue;
            }
            if (repoKey.find(specText) != std::string::npos || relativeKey.find(specText) != std::string::npos) {
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
        if (matches.size() == 1) {
            return matches.front();
        }
        if (matches.size() > 1) {
            std::ostringstream oss;
            oss << "repo spec is ambiguous: " << specText << "\nMatches:\n";
            for (const auto& match : matches) {
                oss << "  - " << match.generic_string() << "\n";
            }
            throw std::runtime_error(oss.str());
        }
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

        if (repoName == specText || repoKey == specText || relativeKey == specText) {
            exactMatches.push_back(repoPath);
            continue;
        }
        if (repoKey.find(specText) != std::string::npos || relativeKey.find(specText) != std::string::npos) {
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
        throw std::runtime_error("repo not found: " + specText);
    }
    if (matches.size() > 1) {
        std::ostringstream oss;
        oss << "repo spec is ambiguous: " << specText << "\nMatches:\n";
        for (const auto& match : matches) {
            oss << "  - " << match.generic_string() << "\n";
        }
        throw std::runtime_error(oss.str());
    }

    return matches.front();
}

auto ResolveReposCsv(const std::filesystem::path& InRoot, const std::string& InCsv) -> std::vector<std::filesystem::path> {
    std::vector<std::filesystem::path> out;
    for (const auto& repo : ParseReposCsv(InCsv)) {
        out.push_back(ResolveRepoFromSpec(InRoot, repo, 12, true));
    }
    return out;
}

auto ResolveGitmodulesPushPolicy(const std::filesystem::path& InRepo) -> std::string {
    auto repo = std::filesystem::weakly_canonical(InRepo).lexically_normal();
    auto current = repo.parent_path();

    while (!current.empty()) {
        const auto gitmodulesPath = current / ".gitmodules";
        if (std::filesystem::exists(gitmodulesPath)) {
            const auto relative = repo.lexically_relative(current);
            const auto rel = relative.generic_string();
            if (!rel.empty() && rel != "." && !rel.starts_with("..")) {
                const auto key = std::format("submodule.{}.kog-push-policy", rel);
                const auto configured = GitCapture(current, {"config", "-f", ".gitmodules", "--get", key});
                if (configured.exitCode == 0) {
                    const auto policy = ToLower(Trim(configured.stdoutStr));
                    if (!policy.empty()) {
                        return policy;
                    }
                }
            }
        }

        const auto parent = current.parent_path();
        if (parent == current) {
            break;
        }
        current = parent;
    }

    return {};
}

auto TryResolveLocalRemotePath(const std::filesystem::path& InRepo, const std::string& InRemoteUrl)
    -> std::optional<std::filesystem::path> {
    auto url = Trim(InRemoteUrl);
    if (url.empty()) {
        return std::nullopt;
    }

    if (url.rfind("file://", 0) == 0) {
        url = url.substr(7);
#if defined(_WIN32)
        if (url.size() >= 3 && url[0] == '/' && std::isalpha(static_cast<unsigned char>(url[1])) && url[2] == ':') {
            url.erase(url.begin());
        }
#endif
    } else {
        const bool hasScheme = url.find("://") != std::string::npos;
        const bool scpLikeRemote = url.find('@') != std::string::npos && url.find(':') != std::string::npos;
        if (hasScheme || scpLikeRemote) {
            return std::nullopt;
        }
    }

    auto remotePath = std::filesystem::path(url);
    if (remotePath.is_relative()) {
        remotePath = (InRepo / remotePath).lexically_normal();
    }

    std::error_code ec;
    remotePath = std::filesystem::weakly_canonical(remotePath, ec);
    if (ec || !std::filesystem::exists(remotePath)) {
        return std::nullopt;
    }
    return remotePath.lexically_normal();
}

auto ShouldSkipLocalCheckedOutRemotePush(const std::filesystem::path& InRepo,
                                         const std::string& InRemote,
                                         const std::string& InBranch,
                                         std::string* OutReason) -> bool {
    const auto remoteUrl = GetRemoteUrl(InRepo, InRemote);
    if (!remoteUrl.has_value()) {
        return false;
    }

    const auto remotePath = TryResolveLocalRemotePath(InRepo, *remoteUrl);
    if (!remotePath.has_value() || !IsGitRepo(*remotePath)) {
        return false;
    }

    const auto bareResult = GitCapture(*remotePath, {"rev-parse", "--is-bare-repository"});
    if (bareResult.exitCode != 0 || ToLower(Trim(bareResult.stdoutStr)) == "true") {
        return false;
    }

    const auto headBranchResult = GitCapture(*remotePath, {"symbolic-ref", "--quiet", "--short", "HEAD"});
    if (headBranchResult.exitCode != 0) {
        return false;
    }

    const auto checkedOutBranch = Trim(headBranchResult.stdoutStr);
    if (checkedOutBranch != InBranch) {
        return false;
    }

    if (OutReason != nullptr) {
        *OutReason = std::format(
            "local non-bare remote has checked-out branch '{}' at {}",
            checkedOutBranch,
            remotePath->generic_string());
    }
    return true;
}

auto IsParentPath(const std::filesystem::path& InParent, const std::filesystem::path& InChild) -> bool {
    const auto parent = InParent.lexically_normal().generic_string();
    const auto child = InChild.lexically_normal().generic_string();
    if (parent.empty() || child.empty() || parent == child) {
        return false;
    }
    const std::string prefix = parent + "/";
    return child.rfind(prefix, 0) == 0;
}

auto AddUniqueDependency(std::vector<std::filesystem::path>* OutDependencies, const std::filesystem::path& InDependency) -> void {
    if (OutDependencies == nullptr || InDependency.empty()) {
        return;
    }
    const auto dependencyKey = RepoKey(InDependency);
    const bool exists = std::any_of(OutDependencies->begin(), OutDependencies->end(), [&](const auto& candidate) {
        return RepoKey(candidate) == dependencyKey;
    });
    if (!exists) {
        OutDependencies->push_back(InDependency);
    }
}

auto BuildPushSchedulerInputs(const std::vector<workspace::RepoRecord>& InRepos) -> std::vector<workspace::RepoOperationInput> {
    std::unordered_map<std::string, std::size_t> byPath;
    byPath.reserve(InRepos.size());
    for (std::size_t idx = 0; idx < InRepos.size(); ++idx) {
        byPath.emplace(RepoKey(InRepos[idx].path), idx);
    }

    std::vector<workspace::RepoOperationInput> inputs;
    inputs.reserve(InRepos.size());
    for (const auto& repo : InRepos) {
        workspace::RepoOperationInput input;
        input.id = RepoKey(repo.path);
        input.path = repo.path;
        input.type = repo.type;
        inputs.push_back(std::move(input));
    }

    // Primary graph edge source: discover-owned .gitmodules/gitlink registration metadata.
    // The scheduler interprets dependencies as prerequisites, so parent repos depend on their children.
    std::unordered_set<std::string> childrenWithGraphParent;
    for (const auto& child : InRepos) {
        if (child.registrationRelativeTo.empty() || child.registrationRelativeTo == ".") {
            continue;
        }
        const auto parentIt = byPath.find(RepoKey(child.registrationRelativeTo));
        const auto childIt = byPath.find(RepoKey(child.path));
        if (parentIt == byPath.end() || childIt == byPath.end() || parentIt->second == childIt->second) {
            continue;
        }
        AddUniqueDependency(&inputs[parentIt->second].dependencies, child.path);
        childrenWithGraphParent.insert(RepoKey(child.path));
    }

    // Fallback ordering only: nested workspace repos without explicit graph metadata still run bottom-up.
    for (std::size_t parentIdx = 0; parentIdx < InRepos.size(); ++parentIdx) {
        for (std::size_t childIdx = 0; childIdx < InRepos.size(); ++childIdx) {
            if (parentIdx == childIdx) {
                continue;
            }
            if (!IsParentPath(InRepos[parentIdx].path, InRepos[childIdx].path)) {
                continue;
            }
            if (childrenWithGraphParent.contains(RepoKey(InRepos[childIdx].path))) {
                continue;
            }
            AddUniqueDependency(&inputs[parentIdx].dependencies, InRepos[childIdx].path);
        }
    }

    return inputs;
}

auto DetectDefaultPushJobs() -> int {
    const unsigned int cores = std::thread::hardware_concurrency();
    if (cores == 0) {
        return 1;
    }
    return static_cast<int>(cores);
}

struct PushSummaryEntry {
    std::filesystem::path repo;
    std::string outcome;
    std::string remote;
    std::string branch;
    std::string reason;
};

auto PrintCapturedOutputToStreams(const shell::ExecResult& InResult, std::ostream& OutStdout, std::ostream& OutStderr) -> void {
    if (!InResult.stdoutStr.empty()) {
        OutStdout << InResult.stdoutStr;
    }
    if (!InResult.stderrStr.empty()) {
        OutStderr << InResult.stderrStr;
    }
}

auto GitConfigValue(const std::filesystem::path& InRepo, const std::string& InKey) -> std::optional<std::string> {
    const auto out = GitCapture(InRepo, {"config", "--get", InKey});
    if (out.exitCode != 0) {
        return std::nullopt;
    }
    const auto value = Trim(out.stdoutStr);
    if (value.empty()) {
        return std::nullopt;
    }
    return value;
}

auto AddExistingRemote(std::vector<std::string>* OutRemotes,
                       const std::filesystem::path& InRepo,
                       const std::string& InRemote) -> void {
    if (OutRemotes == nullptr || InRemote.empty() || !HasRemote(InRepo, InRemote)) {
        return;
    }
    if (std::find(OutRemotes->begin(), OutRemotes->end(), InRemote) == OutRemotes->end()) {
        OutRemotes->push_back(InRemote);
    }
}

auto ResolvePushRemotePriority(const std::filesystem::path& InRepo,
                               const std::string& InBranch,
                               const std::string& InRemoteFilter) -> std::optional<std::string> {
    if (!InRemoteFilter.empty()) {
        return HasRemote(InRepo, InRemoteFilter) ? std::optional<std::string>{InRemoteFilter} : std::nullopt;
    }

    std::vector<std::string> candidates;
    if (!InBranch.empty()) {
        if (const auto branchPushRemote = GitConfigValue(InRepo, "branch." + InBranch + ".pushRemote"); branchPushRemote.has_value()) {
            AddExistingRemote(&candidates, InRepo, *branchPushRemote);
        }
    }
    if (const auto pushDefault = GitConfigValue(InRepo, "remote.pushDefault"); pushDefault.has_value()) {
        AddExistingRemote(&candidates, InRepo, *pushDefault);
    }

    for (const auto& remote : {"origin-ssh", "origin", "origin-http", "upstream-ssh", "upstream", "upstream-http"}) {
        AddExistingRemote(&candidates, InRepo, remote);
    }

    if (candidates.empty()) {
        return std::nullopt;
    }
    return candidates.front();
}

auto HasAnyCommit(const std::filesystem::path& InRepo) -> bool {
    return GitCapture(InRepo, {"rev-parse", "--verify", "HEAD"}).exitCode == 0;
}

auto ParentGitlinkHeadForPush(const std::filesystem::path& InParent, const std::filesystem::path& InChild) -> std::string {
    const auto rel = InChild.lexically_normal().lexically_relative(InParent.lexically_normal()).generic_string();
    if (rel.empty() || rel.starts_with("..")) {
        return {};
    }
    const auto out = GitCapture(InParent, {"ls-tree", "HEAD", "--", rel});
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

auto CommitIsKnownOnRemoteTrackingRef(const std::filesystem::path& InRepo, const std::string& InCommit) -> bool {
    if (InCommit.empty()) {
        return true;
    }
    const auto contains = GitCapture(InRepo, {"branch", "-r", "--contains", InCommit});
    return contains.exitCode == 0 && !Trim(contains.stdoutStr).empty();
}

auto ParsePorcelainPath(std::string InLine) -> std::string {
    while (!InLine.empty() && (InLine.back() == '\r' || InLine.back() == '\n')) {
        InLine.pop_back();
    }
    if (InLine.size() < 4) {
        return {};
    }
    auto path = Trim(InLine.substr(3));
    const auto renamePos = path.find(" -> ");
    if (renamePos != std::string::npos) {
        path = Trim(path.substr(renamePos + 4));
    }
    return path;
}

auto HasContentChangesOutsideDependencies(const std::filesystem::path& InRepo,
                                          const std::vector<std::filesystem::path>& InDependencies) -> bool {
    const auto status = GitCapture(InRepo, {"status", "--porcelain", "--untracked-files=all"});
    if (status.exitCode != 0) {
        return true;
    }

    std::vector<std::string> dependencyRelPaths;
    dependencyRelPaths.reserve(InDependencies.size());
    for (const auto& dependency : InDependencies) {
        const auto rel = dependency.lexically_normal().lexically_relative(InRepo.lexically_normal()).generic_string();
        if (!rel.empty() && !rel.starts_with("..")) {
            dependencyRelPaths.push_back(rel);
        }
    }

    std::istringstream iss(status.stdoutStr);
    std::string line;
    while (std::getline(iss, line)) {
        const auto changedPath = ParsePorcelainPath(line);
        if (changedPath.empty()) {
            continue;
        }
        auto normalizedChangedPath = changedPath;
        std::replace(normalizedChangedPath.begin(), normalizedChangedPath.end(), '\\', '/');
        while (normalizedChangedPath.size() > 1 && normalizedChangedPath.back() == '/') {
            normalizedChangedPath.pop_back();
        }
        if (normalizedChangedPath == ".gitmodules" ||
            normalizedChangedPath == ".kano" || normalizedChangedPath.rfind(".kano/", 0) == 0 ||
            normalizedChangedPath == ".sisyphus" || normalizedChangedPath.rfind(".sisyphus/", 0) == 0) {
            continue;
        }
        const auto nestedCandidate = (InRepo / std::filesystem::path(normalizedChangedPath)).lexically_normal();
        if (std::filesystem::exists(nestedCandidate) && IsGitRepo(nestedCandidate)) {
            continue;
        }
        const bool dependencyOnly = std::any_of(dependencyRelPaths.begin(), dependencyRelPaths.end(), [&](const auto& depRel) {
            return normalizedChangedPath == depRel ||
                normalizedChangedPath.rfind(depRel + "/", 0) == 0 ||
                depRel.rfind(normalizedChangedPath + "/", 0) == 0;
        });
        if (!dependencyOnly) {
            return true;
        }
    }
    return false;
}

auto HasNestedRepoChildren(const std::filesystem::path& InRepo,
                           const std::unordered_map<std::string, workspace::RepoRecord>& InReposByPath) -> bool {
    for (const auto& [key, record] : InReposByPath) {
        (void)key;
        if (RepoKey(record.path) == RepoKey(InRepo)) {
            continue;
        }
        if (IsParentPath(InRepo, record.path)) {
            return true;
        }
    }
    return false;
}

auto BuildContainerChildPaths(const std::filesystem::path& InRepo,
                              const std::vector<std::filesystem::path>& InDependencies,
                              const std::unordered_map<std::string, workspace::RepoRecord>& InReposByPath) -> std::vector<std::filesystem::path> {
    std::vector<std::filesystem::path> out = InDependencies;
    for (const auto& [key, record] : InReposByPath) {
        (void)key;
        if (RepoKey(record.path) == RepoKey(InRepo) || !IsParentPath(InRepo, record.path)) {
            continue;
        }
        AddUniqueDependency(&out, record.path);
    }
    return out;
}

auto ClassifyPushFailure(const shell::ExecResult& InResult) -> std::string {
    const auto merged = ToLower(InResult.stdoutStr + "\n" + InResult.stderrStr);
    if (merged.find("authentication failed") != std::string::npos ||
        merged.find("permission denied") != std::string::npos ||
        merged.find("publickey") != std::string::npos ||
        merged.find("access denied") != std::string::npos ||
        merged.find("unauthorized") != std::string::npos ||
        merged.find("could not read username") != std::string::npos) {
        return "FAILED_AUTH";
    }
    if (merged.find("could not resolve host") != std::string::npos ||
        merged.find("failed to connect") != std::string::npos ||
        merged.find("connection timed out") != std::string::npos ||
        merged.find("network is unreachable") != std::string::npos ||
        merged.find("couldn't connect") != std::string::npos) {
        return "FAILED_CONNECTION";
    }
    if (merged.find("does not appear to be a git repository") != std::string::npos ||
        merged.find("repository not found") != std::string::npos ||
        merged.find("no such remote") != std::string::npos) {
        return "FAILED_MISSING_REMOTE";
    }
    return "FAILED_PUSH";
}

auto RunNativePush(
    const std::vector<workspace::RepoRecord>& InRepos,
    const std::filesystem::path& InWorkspaceRoot,
    const bool InRecursive,
    const bool InSkipSync,
    const bool InFetchOnly,
    const bool InDryRun,
    const bool InForceWithLease,
    const bool InNoVerify,
    const bool InStashLocalChanges,
    const bool InFailOnDirtySync,
    const int InJobs,
    const bool InProfile,
    const bool InVerbose,
    const std::string& InRemoteFilter,
    workspace::RepoOperationAggregate* OutAggregate = nullptr) -> int {
    const auto totalStart = std::chrono::steady_clock::now();
    long long syncMillis = 0;
    long long pushMillis = 0;
    int maxParallelObserved = 1;
    std::mutex statsMutex;
    std::vector<PushSummaryEntry> pushStats;
    std::unordered_map<std::string, PushSummaryEntry> pushSummaryByRepo;
    int failures = 0;
    int successes = 0;
    std::mutex profileMutex;
    int activeRepoRuns = 0;

    struct RepoRunResult {
        workspace::RepoOperationStatus status{workspace::RepoOperationStatus::Succeeded};
        int exitCode{0};
        std::string stdoutText;
        std::string stderrText;
        std::string failureCategory;
        std::string message;
        std::string skipReason;
        PushSummaryEntry summary;
        std::filesystem::path repo;
    };

    std::unordered_map<std::string, std::size_t> repoOrderByPath;
    std::unordered_map<std::string, workspace::RepoRecord> repoRecordsByPath;
    repoOrderByPath.reserve(InRepos.size());
    repoRecordsByPath.reserve(InRepos.size());
    for (std::size_t i = 0; i < InRepos.size(); ++i) {
        const auto repoPath = std::filesystem::weakly_canonical(InRepos[i].path).lexically_normal().generic_string();
        repoOrderByPath[repoPath] = i + 1;
        repoRecordsByPath[RepoKey(InRepos[i].path)] = InRepos[i];
    }

    auto runOneRepo = [&](const workspace::RepoOperationInput& operation) -> RepoRunResult {
        const auto& repoPathRaw = operation.path;
        const auto repoPath = std::filesystem::weakly_canonical(repoPathRaw);
        const auto repoLabel = repoPath.lexically_normal().generic_string();
        std::ostringstream out;
        std::ostringstream err;
        const auto normalizeCapturedText = [](std::string text) {
            std::string normalized;
            normalized.reserve(text.size() + 8);
            for (std::size_t i = 0; i < text.size(); ++i) {
                const char ch = text[i];
                if (ch == '\r') {
                    if ((i + 1) < text.size() && text[i + 1] == '\n') {
                        continue;
                    }
                    normalized.push_back('\n');
                    continue;
                }
                normalized.push_back(ch);
            }
            while (!normalized.empty() && normalized.back() == '\n') {
                normalized.pop_back();
            }
            if (!normalized.empty()) {
                normalized.push_back('\n');
            }
            return normalized;
        };
        shell::ScopedCommandLogCapture commandLogCapture(shell::CommandLogCallbacks{
            .onStdout = [&](const std::string& line) {
                out << line;
            },
            .onStderr = [&](const std::string& line) {
                err << line;
            },
        });
        shell::ScopedConsoleWriteSuppression suppressShellConsoleWrites;
        std::size_t repoIndex = 0;
        if (const auto it = repoOrderByPath.find(repoLabel); it != repoOrderByPath.end()) {
            repoIndex = it->second;
        }

        RepoRunResult run;
        run.repo = repoPath;
        run.summary.repo = repoPath;

        auto finishSuccess = [&](const std::string& outcome, const std::string& remote, const std::string& branch, const std::string& reason) {
            run.status = workspace::RepoOperationStatus::Succeeded;
            run.exitCode = 0;
            run.summary = PushSummaryEntry{repoPath, outcome, remote, branch, reason};
            run.stdoutText = normalizeCapturedText(out.str());
            run.stderrText = normalizeCapturedText(err.str());
            return run;
        };

        auto finishSkipped = [&](const std::string& outcome, const std::string& remote, const std::string& branch, const std::string& reason) {
            run.status = workspace::RepoOperationStatus::Skipped;
            run.exitCode = 0;
            run.skipReason = reason;
            run.message = reason;
            run.summary = PushSummaryEntry{repoPath, outcome, remote, branch, reason};
            run.stdoutText = normalizeCapturedText(out.str());
            run.stderrText = normalizeCapturedText(err.str());
            return run;
        };

        auto finishFailed = [&](const std::string& category, const std::string& remote, const std::string& branch, const std::string& reason) {
            run.status = category == "BLOCKED_BY_CHILD_FAILURE"
                ? workspace::RepoOperationStatus::Blocked
                : workspace::RepoOperationStatus::Failed;
            run.exitCode = 1;
            run.failureCategory = category;
            run.message = reason;
            run.summary = PushSummaryEntry{repoPath, category, remote, branch, reason};
            run.stdoutText = normalizeCapturedText(out.str());
            run.stderrText = normalizeCapturedText(err.str());
            return run;
        };

        if (repoIndex > 0) {
            out << "[" << kano::terminal::Wrap(std::to_string(repoIndex) + "/" + std::to_string(InRepos.size()), kano::terminal::Color::Dim)
                << "] Processing " << kano::terminal::Wrap(repoLabel, kano::terminal::Color::BoldCyan) << "\n";
        } else {
            out << "[" << kano::terminal::Wrap("?/?", kano::terminal::Color::Dim)
                << "] Processing " << kano::terminal::Wrap(repoLabel, kano::terminal::Color::BoldCyan) << "\n";
        }

        const auto gitDir = GitCapture(repoPath, {"rev-parse", "--git-dir"});
        if (gitDir.exitCode != 0) {
            err << "[" << repoLabel << "] FAILED_PUSH: not a git repository\n";
            return finishFailed("FAILED_PUSH", {}, {}, "not a git repository");
        }

        if (InFetchOnly) {
            if (InDryRun) {
                out << "[DRY RUN] [" << repoLabel << "] Would run: git fetch --all --prune --tags\n";
            } else {
                const auto fetch = GitCapture(repoPath, {"fetch", "--all", "--prune", "--tags"});
                if (fetch.exitCode != 0) {
                    PrintCapturedOutputToStreams(fetch, out, err);
                    err << "[" << repoLabel << "] FAILED_CONNECTION: fetch failed\n";
                    return finishFailed("FAILED_CONNECTION", {}, {}, "fetch failed");
                }
            }
            out << "[" << kano::terminal::Wrap(repoLabel, kano::terminal::Color::BoldCyan) << "] Fetch-only mode: skipping rebase and push\n";
            return finishSkipped("SKIPPED_FETCH_ONLY", {}, {}, "fetch-only mode");
        }

        const auto repoRecordIt = repoRecordsByPath.find(RepoKey(repoPath));
        const bool pushDisabledByCommandPolicy = repoRecordIt != repoRecordsByPath.end() && IsFalsePolicy(repoRecordIt->second.kogPushPolicy);
        if (pushDisabledByCommandPolicy) {
            const std::string reason = "commandPolicy.push=false / kog-push=false";
            out << "[" << repoLabel << "] SKIPPED_BY_POLICY: " << reason << "\n";
            return finishSkipped("SKIPPED_BY_POLICY", {}, {}, reason);
        }

        const auto pushPolicy = ResolveGitmodulesPushPolicy(repoPath);
        if (pushPolicy == "skip") {
            const std::string reason = ".gitmodules policy kog-push-policy=skip";
            out << "[" << repoLabel << "] SKIPPED_BY_POLICY: " << reason << "\n";
            return finishSkipped("SKIPPED_BY_POLICY", {}, {}, reason);
        }
        if (!pushPolicy.empty() && pushPolicy != "allow") {
            err << "[" << repoLabel << "] Warning: unknown .gitmodules kog-push-policy='" << pushPolicy
                << "'; expected skip|allow, treating as allow\n";
        }

        if (!HasAnyCommit(repoPath)) {
            err << "[" << repoLabel << "] FAILED_PUSH: repository has no commits to push\n";
            return finishFailed("FAILED_PUSH", {}, {}, "repository has no commits to push");
        }

        const auto branchOut = GitCapture(repoPath, {"symbolic-ref", "--quiet", "--short", "HEAD"});
        const auto branch = Trim(branchOut.stdoutStr);
        if (branchOut.exitCode != 0 || branch.empty()) {
            err << "[" << repoLabel << "] FAILED_PUSH: detached HEAD is not supported by native push flow\n";
            return finishFailed("FAILED_PUSH", {}, {}, "detached HEAD is not supported by native push flow");
        }

        const bool hasUpstream = (GitCapture(repoPath, {"rev-parse", "--abbrev-ref", "@{upstream}"}).exitCode == 0);
        const bool hasLocalChanges = !Trim(GitCapture(repoPath, {"status", "--porcelain"}).stdoutStr).empty();

        if (!InSkipSync && hasUpstream) {
            const auto syncStart = std::chrono::steady_clock::now();
            bool hadStash = false;
            std::string stashName;
            if (hasLocalChanges) {
                if (InFailOnDirtySync) {
                    err << "[" << repoLabel << "] FAILED_PUSH: sync failed because local changes are present (--fail-on-dirty-sync)\n";
                    return finishFailed("FAILED_PUSH", {}, branch, "sync failed because local changes are present");
                }

                if (InStashLocalChanges) {
                    stashName = "kano-native-push-autostash";
                    if (InDryRun) {
                        out << "[DRY RUN] [" << repoLabel << "] Would run: git stash push -u -m " << stashName << "\n";
                        hadStash = true;
                    } else {
                        const auto stash = GitCapture(repoPath, {"stash", "push", "-u", "-m", stashName});
                        if (stash.exitCode != 0) {
                            PrintCapturedOutputToStreams(stash, out, err);
                            err << "[" << repoLabel << "] FAILED_PUSH: failed to create auto-stash before sync\n";
                            return finishFailed("FAILED_PUSH", {}, branch, "failed to create auto-stash before sync");
                        }
                        const auto stashOut = Trim(stash.stdoutStr);
                        hadStash = stashOut.find("No local changes to save") == std::string::npos;
                        if (hadStash) {
                            out << "[" << repoLabel << "] Auto-stashed local changes for sync\n";
                        }
                    }
                } else {
                    out << "[" << kano::terminal::Wrap(repoLabel, kano::terminal::Color::BoldCyan) << "] Sync skipped: local changes present; proceeding to push\n";
                }
            } else {
                const auto upstreamRef = Trim(GitCapture(repoPath, {"rev-parse", "--abbrev-ref", "@{upstream}"}).stdoutStr);
                bool shouldPullRebase = true;
                int aheadCount = -1;
                int behindCount = -1;
                if (!upstreamRef.empty()) {
                    const auto aheadBehind = GitCapture(repoPath, {"rev-list", "--left-right", "--count", std::format("HEAD...{}", upstreamRef)});
                    if (aheadBehind.exitCode == 0) {
                        std::istringstream iss(aheadBehind.stdoutStr);
                        if (iss >> aheadCount >> behindCount) {
                            shouldPullRebase = behindCount > 0;
                        }
                    }
                }

                if (InDryRun) {
                    if (shouldPullRebase) {
                        out << "[DRY RUN] [" << repoLabel << "] Would run: git pull --rebase\n";
                    } else {
                        out << "[DRY RUN] [" << repoLabel << "] Skip sync pull: local branch is not behind upstream";
                        if (aheadCount >= 0 && behindCount >= 0) {
                            out << " (ahead=" << aheadCount << ", behind=" << behindCount << ")";
                        }
                        out << "\n";
                    }
                } else if (shouldPullRebase) {
                    const auto pull = GitCapture(repoPath, {"pull", "--rebase"});
                    if (pull.exitCode != 0) {
                        PrintCapturedOutputToStreams(pull, out, err);
                        err << "[" << repoLabel << "] FAILED_PUSH: sync failed before push\n";
                        return finishFailed("FAILED_PUSH", {}, branch, "sync failed before push");
                    }
                } else {
                    out << "[" << kano::terminal::Wrap(repoLabel, kano::terminal::Color::BoldCyan) << "] Sync skipped: local branch is not behind upstream";
                    if (aheadCount >= 0 && behindCount >= 0) {
                        out << " (ahead=" << aheadCount << ", behind=" << behindCount << ")";
                    }
                    out << "\n";
                }
            }

            if (InStashLocalChanges && hasLocalChanges && hadStash) {
                if (InDryRun) {
                    out << "[DRY RUN] [" << repoLabel << "] Would run: git stash pop\n";
                } else {
                    const auto pop = GitCapture(repoPath, {"stash", "pop"});
                    if (pop.exitCode != 0) {
                        PrintCapturedOutputToStreams(pop, out, err);
                        err << "[" << repoLabel << "] FAILED_PUSH: failed to restore auto-stash after sync\n";
                        return finishFailed("FAILED_PUSH", {}, branch, "failed to restore auto-stash after sync");
                    }
                }
            }
            const auto syncEnd = std::chrono::steady_clock::now();
            {
                std::lock_guard<std::mutex> lock(profileMutex);
                syncMillis += std::chrono::duration_cast<std::chrono::milliseconds>(syncEnd - syncStart).count();
            }
        }

        for (const auto& dependency : operation.dependencies) {
            const auto childIt = repoRecordsByPath.find(RepoKey(dependency));
            if (childIt == repoRecordsByPath.end() || !IsFalsePolicy(childIt->second.kogPushPolicy)) {
                continue;
            }
            const auto gitlinkHead = ParentGitlinkHeadForPush(repoPath, childIt->second.path);
            if (!gitlinkHead.empty() && !CommitIsKnownOnRemoteTrackingRef(childIt->second.path, gitlinkHead)) {
                const auto reason = std::format(
                    "push-disabled child {} points at commit {} that is not available on a remote-tracking ref",
                    childIt->second.path.lexically_normal().generic_string(),
                    gitlinkHead);
                err << "[" << repoLabel << "] BLOCKED_BY_CHILD_FAILURE: " << reason << "\n";
                return finishFailed("BLOCKED_BY_CHILD_FAILURE", {}, branch, reason);
            }
        }

        const auto selectedRemote = ResolvePushRemotePriority(repoPath, branch, InRemoteFilter);
        if (!selectedRemote.has_value()) {
            const auto workspaceRoot = std::filesystem::weakly_canonical(InWorkspaceRoot).lexically_normal();
            const auto containerChildPaths = BuildContainerChildPaths(repoPath, operation.dependencies, repoRecordsByPath);
            const bool rootHasChildren = !containerChildPaths.empty() || HasNestedRepoChildren(repoPath, repoRecordsByPath);
            if (InRecursive && repoPath == workspaceRoot && rootHasChildren && !HasContentChangesOutsideDependencies(repoPath, containerChildPaths)) {
                const std::string reason = "no pushable remote on clean workspace root container repo";
                out << "[" << repoLabel << "] Push skipped: no pushable remote on workspace root container repo\n";
                out << "[" << repoLabel << "] SKIPPED_NO_REMOTE: Push skipped: " << reason << "\n";
                return finishSkipped("SKIPPED_NO_REMOTE", {}, branch, reason);
            }
            const std::string reason = InRemoteFilter.empty()
                ? "no usable push remote found; configure origin-ssh, origin, origin-http, upstream-ssh, upstream, upstream-http, branch.<name>.pushRemote, or remote.pushDefault"
                : "requested push remote '" + InRemoteFilter + "' is not configured";
            err << "[" << repoLabel << "] FAILED_MISSING_REMOTE: " << reason << "\n";
            return finishFailed("FAILED_MISSING_REMOTE", {}, branch, reason);
        }

        const auto remote = *selectedRemote;

        std::vector<std::string> pushArgs;
        if (InForceWithLease) {
            pushArgs.push_back("--force-with-lease");
        }
        if (InNoVerify) {
            pushArgs.push_back("--no-verify");
        }

        const auto pushStart = std::chrono::steady_clock::now();
        const bool hasSomethingToPush = HasCommitsToPush(repoPath, remote, branch);
        if (!hasSomethingToPush) {
            const auto reason = "remote branch already contains local branch";
            out << "[" << repoLabel << "] SKIPPED_UP_TO_DATE (" << remote << ", " << branch << "): " << reason << "\n";
            return finishSkipped("SKIPPED_UP_TO_DATE", remote, branch, reason);
        }

        std::string skipReason;
        if (ShouldSkipLocalCheckedOutRemotePush(repoPath, remote, branch, &skipReason)) {
            out << "[" << repoLabel << "] SKIPPED_BY_POLICY (" << remote << ", " << branch << "): " << skipReason << "\n";
            return finishSkipped("SKIPPED_BY_POLICY", remote, branch, skipReason);
        }

        std::vector<std::string> args = {"push"};
        args.insert(args.end(), pushArgs.begin(), pushArgs.end());
        args.push_back(remote);
        args.push_back(branch);

        if (InDryRun) {
            out << "[DRY RUN] [" << repoLabel << "] Would run: git";
            for (const auto& arg : args) {
                out << " " << arg;
            }
            out << "\n";
            return finishSuccess("PUSHED_DRY_RUN", remote, branch, "dry-run planned push");
        }

        auto result = GitCapture(repoPath, args);
        if (InVerbose) {
            PrintCapturedOutputToStreams(result, out, err);
        }
        if (result.exitCode == 0) {
            out << "[" << kano::terminal::Wrap(repoLabel, kano::terminal::Color::BoldCyan) << "] "
                << kano::terminal::Wrap("Pushed", kano::terminal::Color::BoldGreen) << " (" << remote << ", " << branch << ")\n";
            const auto pushEnd = std::chrono::steady_clock::now();
            {
                std::lock_guard<std::mutex> lock(profileMutex);
                pushMillis += std::chrono::duration_cast<std::chrono::milliseconds>(pushEnd - pushStart).count();
            }
            return finishSuccess("PUSHED", remote, branch, "pushed successfully");
        }

                bool recoveredByLfsRetry = false;
                if (!InDryRun && LooksLikeLfsPushFailure(result)) {
                    err << "[" << kano::terminal::Wrap(repoLabel, kano::terminal::Color::BoldCyan) << "] "
                        << kano::terminal::Wrap("Push failed", kano::terminal::Color::BoldRed) << " (" << remote
                        << ") due to LFS transport/auth issue; attempting git lfs push retry\n";

                    const auto lfsPush = GitCapture(repoPath, {"lfs", "push", remote, branch});
                    if (InVerbose || lfsPush.exitCode != 0) {
                        PrintCapturedOutputToStreams(lfsPush, out, err);
                    }

                    if (lfsPush.exitCode == 0) {
                        auto retryResult = GitCapture(repoPath, args);
                        if (InVerbose || retryResult.exitCode != 0) {
                            PrintCapturedOutputToStreams(retryResult, out, err);
                        }
                        if (retryResult.exitCode == 0) {
                            out << "[" << kano::terminal::Wrap(repoLabel, kano::terminal::Color::BoldCyan) << "] "
                                << kano::terminal::Wrap("Pushed", kano::terminal::Color::BoldGreen) << " (" << remote
                                << ", " << branch << ") after LFS retry\n";
                            recoveredByLfsRetry = true;
                        }
                    }
                }

                if (recoveredByLfsRetry) {
                    const auto pushEnd = std::chrono::steady_clock::now();
                    {
                        std::lock_guard<std::mutex> lock(profileMutex);
                        pushMillis += std::chrono::duration_cast<std::chrono::milliseconds>(pushEnd - pushStart).count();
                    }
                    return finishSuccess("PUSHED", remote, branch, "pushed successfully after LFS retry");
                }

                if (!InVerbose) {
                    PrintCapturedOutputToStreams(result, out, err);
                }
                const auto category = ClassifyPushFailure(result);
                err << "[" << kano::terminal::Wrap(repoLabel, kano::terminal::Color::BoldCyan) << "] "
                    << category << ": " << kano::terminal::Wrap("Push failed", kano::terminal::Color::BoldRed)
                    << " (" << remote << ", " << branch << ")\n";
                return finishFailed(category, remote, branch, "git push failed");
    };

    auto schedulerInputs = BuildPushSchedulerInputs(InRepos);

    workspace::RepoOperationSchedulerOptions schedulerOptions;
    schedulerOptions.operationName = "push";
    schedulerOptions.mode = workspace::RepoOperationMode::MutatingDependencyWaves;
    schedulerOptions.jobs = InDryRun ? 1 : (InJobs < 1 ? 1 : InJobs);

    const auto aggregate = workspace::RunRepoOperationScheduler(
        schedulerInputs,
        schedulerOptions,
        [&](const workspace::RepoOperationInput& operation) {
            {
                std::lock_guard<std::mutex> lock(profileMutex);
                activeRepoRuns += 1;
                maxParallelObserved = std::max(maxParallelObserved, activeRepoRuns);
            }

            const auto result = runOneRepo(operation);

            {
                std::lock_guard<std::mutex> lock(profileMutex);
                activeRepoRuns -= 1;
            }

            workspace::RepoOperationWorkerResult out;
            out.status = result.status;
            out.exitCode = result.exitCode;
            out.stdoutText = result.stdoutText;
            out.stderrText = result.stderrText;
            out.failureCategory = result.failureCategory;
            out.message = result.message;
            out.skipReason = result.skipReason;
            {
                std::lock_guard<std::mutex> lock(statsMutex);
                pushSummaryByRepo[RepoKey(result.repo)] = result.summary;
            }
            return out;
        });

    if (OutAggregate != nullptr) {
        *OutAggregate = aggregate;
    }

    successes = static_cast<int>(aggregate.succeeded);
    failures = static_cast<int>(aggregate.failed + aggregate.blocked + aggregate.pending);

    for (const auto& result : aggregate.results) {
        if (!result.stdoutText.empty()) {
            std::cout << result.stdoutText;
        }
        if (!result.stderrText.empty()) {
            std::cout << result.stderrText;
        }
    }

    for (const auto& result : aggregate.results) {
        if (result.status == workspace::RepoOperationStatus::Blocked) {
            const auto reason = result.blockReason.empty()
                ? std::string{"one or more nested repositories failed in earlier wave"}
                : result.blockReason;
            std::cout << "[" << result.repoPath.generic_string() << "] BLOCKED_BY_CHILD_FAILURE: " << reason << "\n";
            std::cout << "[" << result.repoPath.generic_string() << "] Push blocked: one or more nested repositories failed in earlier wave\n";
        } else if (result.status == workspace::RepoOperationStatus::Pending) {
            std::cout << "[" << result.repoPath.generic_string() << "] Push pending: scheduler did not execute repository\n";
        }
    }

    pushStats.clear();
    pushStats.reserve(aggregate.results.size());
    for (const auto& result : aggregate.results) {
        const auto key = RepoKey(result.repoPath);
        if (const auto it = pushSummaryByRepo.find(key); it != pushSummaryByRepo.end()) {
            pushStats.push_back(it->second);
            continue;
        }
        if (result.status == workspace::RepoOperationStatus::Blocked) {
            pushStats.push_back(PushSummaryEntry{
                result.repoPath,
                "BLOCKED_BY_CHILD_FAILURE",
                {},
                {},
                result.blockReason.empty() ? std::string{"dependency failed in an earlier phase"} : result.blockReason});
        } else if (result.status == workspace::RepoOperationStatus::Pending) {
            pushStats.push_back(PushSummaryEntry{result.repoPath, "PENDING", {}, {}, "scheduler did not execute repository"});
        }
    }

    if (!pushStats.empty()) {
        const auto root = std::filesystem::current_path().lexically_normal();
        std::vector<std::string> groupOrder;
        std::map<std::string, std::vector<PushSummaryEntry>> grouped;
        std::map<std::string, int> outcomeCounts;

        for (const auto& stat : pushStats) {
            const std::filesystem::path repoPath(stat.repo);
            const auto relative = RelativeDisplayPath(root, repoPath);
            const auto group = GroupFromRelativePath(relative);
            if (!grouped.contains(group)) {
                groupOrder.push_back(group);
            }
            grouped[group].push_back(stat);
            outcomeCounts[stat.outcome] += 1;
        }

        std::cout << "\n=== " << kano::terminal::Wrap("Push Summary", kano::terminal::Color::BoldWhite) << " ===\n";
        std::cout << kano::terminal::Wrap(
            std::format(
                "SUMMARY: repos={}, pushed={}, skipped_no_remote={}, skipped_by_policy={}, skipped_up_to_date={}, blocked={}, failed={}",
                pushStats.size(),
                outcomeCounts["PUSHED"] + outcomeCounts["PUSHED_DRY_RUN"],
                outcomeCounts["SKIPPED_NO_REMOTE"],
                outcomeCounts["SKIPPED_BY_POLICY"],
                outcomeCounts["SKIPPED_UP_TO_DATE"],
                outcomeCounts["BLOCKED_BY_CHILD_FAILURE"],
                outcomeCounts["FAILED_PUSH"] + outcomeCounts["FAILED_CONNECTION"] + outcomeCounts["FAILED_AUTH"] + outcomeCounts["FAILED_MISSING_REMOTE"]),
            kano::terminal::Color::BoldWhite) << "\n\n";

        std::size_t index = 0;
        for (const auto& group : groupOrder) {
            const auto rowsIt = grouped.find(group);
            if (rowsIt == grouped.end()) {
                continue;
            }
            std::cout << kano::terminal::Wrap("GROUP: " + group, kano::terminal::Color::BoldYellow) << "\n";
            for (const auto& row : rowsIt->second) {
                index += 1;
                std::cout << kano::terminal::Wrap(std::format("[{}]", index), kano::terminal::Color::Dim) << " "
                          << kano::terminal::Wrap(RepoNameFromPath(row.repo), kano::terminal::Color::BoldCyan)
                          << "  outcome=" << row.outcome;
                if (!row.remote.empty()) {
                    std::cout << "  remote=" << kano::terminal::Wrap(row.remote, kano::terminal::Color::Green);
                }
                if (!row.branch.empty()) {
                    std::cout << "  branch=" << kano::terminal::Wrap(row.branch, kano::terminal::Color::BoldWhite);
                }
                if (!row.reason.empty()) {
                    std::cout << "  reason=" << row.reason;
                }
                std::cout << "\n";
            }
            std::cout << "\n";
        }
    }

    std::cout << "\nSummary: " << kano::terminal::Wrap(std::to_string(successes) + " pushed", kano::terminal::Color::BoldGreen)
              << ", " << kano::terminal::Wrap(std::to_string(static_cast<int>(aggregate.skipped)) + " skipped", kano::terminal::Color::BoldYellow)
              << ", " << kano::terminal::Wrap(std::to_string(static_cast<int>(aggregate.blocked)) + " blocked", kano::terminal::Color::BoldRed)
              << ", " << kano::terminal::Wrap(std::to_string(static_cast<int>(aggregate.failed + aggregate.pending)) + " failed", kano::terminal::Color::BoldRed) << "\n";
    if (InProfile) {
        const auto totalEnd = std::chrono::steady_clock::now();
        const auto totalMillis = std::chrono::duration_cast<std::chrono::milliseconds>(totalEnd - totalStart).count();
        std::cout << "\n=== Profile Summary ===\n";
        std::cout << "mode: native\n";
        std::cout << "repo_count: " << InRepos.size() << "\n";
        std::cout << "jobs_requested: " << schedulerOptions.jobs << "\n";
        std::cout << "max_parallel_observed: " << maxParallelObserved << "\n";
        std::cout << "sync_ms: " << syncMillis << "\n";
        std::cout << "push_ms: " << pushMillis << "\n";
        std::cout << "total_ms: " << totalMillis << "\n";
    }
    return failures == 0 ? 0 : 1;
}

} // namespace

auto RunPushNativeSimpleDetailed(const std::filesystem::path& InWorkspaceRoot,
                                 bool InRecursive,
                                 bool InDryRun,
                                 bool InProfile,
                                 bool InForceWithLease,
                                 bool InNoVerify,
                                 int InJobs,
                                 bool InVerbose,
                                 const std::string& InRemote) -> std::pair<int, workspace::RepoOperationAggregate>;

auto RunPushNativeSimple(const std::filesystem::path& InWorkspaceRoot,
                          const bool InRecursive,
                          const bool InDryRun,
                         const bool InProfile,
                         const bool InForceWithLease,
                         const bool InNoVerify,
                          const int InJobs,
                          const bool InVerbose,
                          const std::string& InRemote) -> int {
    const auto detailed = RunPushNativeSimpleDetailed(
        InWorkspaceRoot,
        InRecursive,
        InDryRun,
        InProfile,
        InForceWithLease,
        InNoVerify,
        InJobs,
        InVerbose,
        InRemote);
    return detailed.first;
}

auto RunPushNativeSimpleDetailed(const std::filesystem::path& InWorkspaceRoot,
                                 const bool InRecursive,
                                 const bool InDryRun,
                                 const bool InProfile,
                                 const bool InForceWithLease,
                                 const bool InNoVerify,
                                 const int InJobs,
                                 const bool InVerbose,
                                 const std::string& InRemote) -> std::pair<int, workspace::RepoOperationAggregate> {
    std::vector<workspace::RepoRecord> repos;
    if (InRecursive) {
        repos = DiscoverWorkspaceRepos(InWorkspaceRoot);
        if (repos.empty()) {
            repos = BuildExplicitRepoRecords(InWorkspaceRoot, {InWorkspaceRoot});
        }
    } else {
        repos = BuildExplicitRepoRecords(InWorkspaceRoot, {InWorkspaceRoot});
    }

    workspace::RepoOperationAggregate aggregate;
    const auto code = RunNativePush(
        repos,
        InWorkspaceRoot,
        InRecursive,
        true,
        false,
        InDryRun,
        InForceWithLease,
        InNoVerify,
        true,
        false,
        InJobs,
        InProfile,
        InVerbose,
        InRemote,
        &aggregate);
    return {code, std::move(aggregate)};
}

void RegisterPush(CLI::App& InApp) {
    auto* cmd = InApp.add_subcommand("push", "Pipeline push stage (push only by default; no implicit sync)");
    cmd->allow_extras();

    auto* shellMode = new bool{false};
    auto* recursive = new bool{false};
    auto* noRecursive = new bool{false};
    auto* bottomUp = new bool{false};
    auto* repos = new std::string{};
    auto* repoRoot = new std::string{};
    auto* target = new std::string{};
    auto* withSync = new bool{false};
    auto* skipSync = new bool{false};
    auto* fetchOnly = new bool{false};
    auto* dryRun = new bool{false};
    auto* forceWithLease = new bool{false};
    auto* noVerify = new bool{false};
    auto* noSmartSync = new bool{false};
    auto* stashLocalChanges = new bool{false};
    auto* failOnDirtySync = new bool{false};
    auto* jobs = new int{DetectDefaultPushJobs()};
    auto* profile = new bool{false};
    auto* verbose = new bool{false};
    auto* remote = new std::string{};

    cmd->add_flag("--shell", *shellMode, "Deprecated compatibility flag (shell path removed)");
    cmd->add_flag("--recursive", *recursive, "Push workspace repositories recursively (default unless --no-recursive is set)");
    cmd->add_flag("--no-recursive,-N", *noRecursive, "Only push current repository (disable workspace recursive discovery)");
    cmd->add_flag("--current-only", *noRecursive, "Alias of --no-recursive (backward compatible)");
    cmd->add_flag("--bottom-up", *bottomUp, "Plan recursive push bottom-up so child repositories run before parents");
    cmd->add_option("--repos", *repos, "Repo filter (comma-separated paths, native mode)");
    cmd->add_option("--repo-root", *repoRoot, "Workspace root/start path used for repo-name lookup");
    cmd->add_option("target", *target, "Optional repo target root (repo name or relative path)")->required(false);
    cmd->add_flag("--with-sync", *withSync, "Run legacy sync-before-push behavior");
    cmd->add_flag("--skip-sync", *skipSync, "Deprecated compatibility flag; push already skips sync by default");
    cmd->add_flag("--fetch-only", *fetchOnly, "Run fetch only (legacy sync path; implies --with-sync)");
    cmd->add_flag("--dry-run", *dryRun, "Preview push operations");
    cmd->add_flag("--force-with-lease", *forceWithLease, "Use force-with-lease for push");
    cmd->add_flag("--no-verify", *noVerify, "Pass --no-verify to git push");
    cmd->add_flag("--no-smart-sync", *noSmartSync, "Compatibility flag (native uses simple pull --rebase)");
    cmd->add_flag("--stash-local-changes", *stashLocalChanges, "Auto-stash local changes during legacy sync-before-push");
    cmd->add_flag("--fail-on-dirty-sync", *failOnDirtySync, "Fail legacy sync-before-push when local changes exist");
    cmd->add_option("--jobs", *jobs, "Number of parallel repo workers for native push (default: CPU cores)");
    cmd->add_flag("--profile", *profile, "Print native push timing/profile summary");
    cmd->add_flag("--verbose", *verbose, "Show detailed native push output including partial failures");
    cmd->add_option("--remote", *remote, "Native remote filter (default fan-out origin-ssh/http/origin)");

    cmd->callback([=]() {
        auto extras = cmd->remaining();

        if (*shellMode) {
            std::cerr << "Error: --shell is no longer supported; push workflow is fully native now\n";
            std::exit(2);
        }

        if (*recursive && *noRecursive) {
            std::cerr << "Error: --recursive cannot be combined with --no-recursive/--current-only\n";
            std::exit(2);
        }

        if (*bottomUp && *noRecursive) {
            std::cerr << "Error: --bottom-up requires recursive push planning; remove --no-recursive/--current-only\n";
            std::exit(2);
        }

        if (!extras.empty()) {
            std::cerr << "Error: unsupported extra arguments in native push mode:";
            for (const auto& extra : extras) {
                std::cerr << ' ' << extra;
            }
            std::cerr << "\n";
            std::exit(2);
        }

        const auto invocationRoot = repoRoot->empty() ? std::filesystem::current_path() : std::filesystem::path(*repoRoot);
        const auto workspaceRoot = target->empty()
            ? invocationRoot.lexically_normal()
            : ResolveRepoFromSpec(invocationRoot.lexically_normal(), std::filesystem::path(*target), 12, true);

        if (!repos->empty() && !target->empty()) {
            std::cerr << "Error: positional target cannot be combined with --repos\n";
            std::exit(2);
        }

        std::vector<workspace::RepoRecord> nativeRepos;
        if (!repos->empty()) {
            nativeRepos = BuildExplicitRepoRecords(workspaceRoot, ResolveReposCsv(workspaceRoot, *repos));
        }

        if (nativeRepos.empty() && !*noRecursive) {
            nativeRepos = DiscoverWorkspaceRepos(workspaceRoot);
        }

        if (nativeRepos.empty()) {
            nativeRepos = BuildExplicitRepoRecords(workspaceRoot, {workspaceRoot});
        }

        if (*stashLocalChanges && *failOnDirtySync) {
            std::cerr << "Error: --stash-local-changes and --fail-on-dirty-sync cannot be used together\n";
            std::exit(1);
        }

        const bool legacySyncRequested = *withSync || *fetchOnly || *stashLocalChanges || *failOnDirtySync;
        const bool effectiveSkipSync = !legacySyncRequested || *skipSync;

        if (*withSync && *skipSync) {
            std::cerr << "Error: --with-sync cannot be combined with --skip-sync\n";
            std::exit(1);
        }

        if (*fetchOnly && effectiveSkipSync) {
            std::cerr << "Error: --fetch-only requires sync path; use --with-sync and remove --skip-sync\n";
            std::exit(1);
        }

        if ((*stashLocalChanges || *failOnDirtySync) && effectiveSkipSync) {
            std::cerr << "Error: sync-only flags require --with-sync\n";
            std::exit(1);
        }

        if (*jobs < 1) {
            std::cerr << "Error: --jobs must be a positive integer\n";
            std::exit(1);
        }

        const auto code = RunNativePush(
            nativeRepos,
            workspaceRoot,
            !*noRecursive,
            effectiveSkipSync,
            *fetchOnly,
            *dryRun,
            *forceWithLease,
            *noVerify,
            *stashLocalChanges,
            *failOnDirtySync,
            *jobs,
            *profile,
            *verbose,
            *remote);
        std::exit(code);
    });
}

} // namespace kano::git::commands
