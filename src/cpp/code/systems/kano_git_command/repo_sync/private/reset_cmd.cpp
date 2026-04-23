// reset command — hard reset workspace repos to local/remote/stable concepts

#include <CLI/CLI.hpp>
#include "discovery.hpp"
#include "native_workspace.hpp"
#include "shell_executor.hpp"
#include "terminal_color.hpp"


#include <algorithm>
#include <filesystem>
#include <format>
#include <future>
#include <iostream>
#include <optional>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

namespace kano::git::commands {
namespace {

enum class ResetMode {
    Local,
    Remote,
    StableLocal,
    StableRemote,
};

struct ResetPlan {
    std::filesystem::path path;
    std::string type;
    std::string relPath;
    std::string displayName;
    std::string remote;
};

struct ResetResolution {
    std::string checkoutBranch;
    std::string resetRef;
    std::string branchSource;
    bool shouldUpdateGitmodules = false;
};

struct ResetResult {
    std::string repo;
    int exitCode = 0;
    bool skipped = false;
    std::string branchSource;
    std::string resetRef;
    std::string message;
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

auto IsGitRepo(const std::filesystem::path& InRepo) -> bool {
    return GitCapture(InRepo, {"rev-parse", "--git-dir"}).exitCode == 0;
}

auto IsWorkingTreeDirty(const std::filesystem::path& InRepo) -> bool {
    const auto status = GitCapture(InRepo, {"status", "--porcelain"});
    return status.exitCode == 0 && !Trim(status.stdoutStr).empty();
}

auto CurrentBranch(const std::filesystem::path& InRepo) -> std::string {
    const auto result = GitCapture(InRepo, {"symbolic-ref", "--quiet", "--short", "HEAD"});
    if (result.exitCode != 0) {
        return {};
    }
    return Trim(result.stdoutStr);
}

auto HasRemote(const std::filesystem::path& InRepo, const std::string& InRemote) -> bool {
    return !InRemote.empty() && GitCapture(InRepo, {"remote", "get-url", InRemote}).exitCode == 0;
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
        if (GitCapture(InRepo, {"show-ref", "--verify", "--quiet", std::format("refs/remotes/{}/{}", InRemote, branch)}).exitCode == 0) {
            return branch;
        }
    }
    return {};
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
        if (!line.empty() && std::regex_match(line, stableTagPattern)) {
            return line;
        }
    }
    return {};
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

auto ResolveGitmodulesSection(const std::filesystem::path& InRoot, const std::string& InRelPath) -> std::string {
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
        if (value == InRelPath && key.ends_with(".path")) {
            return key.substr(0, key.size() - 5);
        }
    }
    return {};
}

auto UpdateGitmodulesBranch(const std::filesystem::path& InRoot,
                            const std::string& InRelPath,
                            const std::string& InBranch) -> bool {
    if (InRelPath.empty() || InRelPath == "." || InBranch.empty()) {
        return true;
    }

    const auto section = ResolveGitmodulesSection(InRoot, InRelPath);
    if (section.empty()) {
        return true;
    }

    return GitCapture(InRoot, {"config", "-f", ".gitmodules", section + ".branch", InBranch}).exitCode == 0;
}

auto RelativePathDepth(const std::filesystem::path& InRoot, const std::filesystem::path& InPath) -> std::size_t {
    std::error_code ec;
    const auto rel = std::filesystem::relative(InPath, InRoot, ec);
    if (ec) {
        return static_cast<std::size_t>(std::distance(InPath.begin(), InPath.end()));
    }
    return static_cast<std::size_t>(std::distance(rel.begin(), rel.end()));
}

auto ResolveRemote(const std::filesystem::path& InRepo, const std::string& InPreferredRemote) -> std::string {
    if (HasRemote(InRepo, InPreferredRemote)) {
        return InPreferredRemote;
    }
    for (const auto& candidate : {"origin", "upstream"}) {
        if (candidate != InPreferredRemote && HasRemote(InRepo, candidate)) {
            return candidate;
        }
    }
    return {};
}

auto ResolveRepoFromSpec(const std::filesystem::path& InRoot,
                         const std::filesystem::path& InSpec,
                         const int InMaxDepth,
                         const bool InUseCache) -> std::filesystem::path {
    if (InSpec.empty() || InSpec == ".") {
        return InRoot.lexically_normal();
    }

    const auto candidate = (InSpec.is_absolute() ? InSpec : (InRoot / InSpec)).lexically_normal();
    if (std::filesystem::exists(candidate) && IsGitRepo(candidate)) {
        return candidate;
    }

    workspace::DiscoverOptions options;
    options.rootDir = InRoot;
    options.maxDepth = InMaxDepth;
    options.useCache = InUseCache;
    options.metadataLevel = "full";

    const auto discovered = workspace::DiscoverRepos(options);
    const auto specText = InSpec.generic_string();
    std::vector<std::filesystem::path> matches;
    for (const auto& repo : discovered.repos) {
        const auto normalized = repo.path.lexically_normal();
        const auto rel = normalized.lexically_relative(InRoot).generic_string();
        const auto name = normalized.filename().generic_string();
        if (normalized.generic_string() == specText || rel == specText || name == specText) {
            matches.push_back(normalized);
        }
    }
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

auto BuildResetPlans(const std::filesystem::path& InRoot,
                     const std::string& InPreferredRemote,
                     int InMaxDepth,
                     bool InNoCache,
                     bool InRefreshCache) -> std::vector<ResetPlan> {
    workspace::DiscoverOptions options;
    options.rootDir = InRoot;
    options.maxDepth = InMaxDepth;
    options.useCache = !InNoCache;
    options.refreshCache = InRefreshCache;
    options.metadataLevel = "full";

    const auto discovery = workspace::DiscoverRepos(options);
    const auto root = std::filesystem::weakly_canonical(InRoot);
    std::vector<ResetPlan> plans;
    plans.reserve(discovery.repos.size());

    for (const auto& repo : discovery.repos) {
        const auto repoPath = std::filesystem::weakly_canonical(repo.path);
        std::error_code ec;
        auto rel = std::filesystem::relative(repoPath, root, ec).generic_string();
        if (ec || rel.empty()) {
            rel = ".";
        }
        plans.push_back(ResetPlan{
            .path = repoPath,
            .type = repo.type,
            .relPath = rel,
            .displayName = rel,
            .remote = ResolveRemote(repoPath, InPreferredRemote),
        });
    }

    std::sort(plans.begin(), plans.end(), [&](const ResetPlan& A, const ResetPlan& B) {
        const auto depthA = RelativePathDepth(root, A.path);
        const auto depthB = RelativePathDepth(root, B.path);
        if (depthA != depthB) {
            return depthA < depthB;
        }
        return A.path.generic_string() < B.path.generic_string();
    });
    return plans;
}

auto LocalRefExists(const std::filesystem::path& InRepo, const std::string& InBranch) -> bool {
    return !InBranch.empty() && GitCapture(InRepo, {"show-ref", "--verify", "--quiet", std::format("refs/heads/{}", InBranch)}).exitCode == 0;
}

auto RemoteRefExists(const std::filesystem::path& InRepo, const std::string& InRemote, const std::string& InBranch) -> bool {
    return !InRemote.empty() && !InBranch.empty() &&
        GitCapture(InRepo, {"show-ref", "--verify", "--quiet", std::format("refs/remotes/{}/{}", InRemote, InBranch)}).exitCode == 0;
}

auto TagRefExists(const std::filesystem::path& InRepo, const std::string& InTag) -> bool {
    return !InTag.empty() && GitCapture(InRepo, {"show-ref", "--verify", "--quiet", std::format("refs/tags/{}", InTag)}).exitCode == 0;
}

auto IsStableBranchName(const std::string& InBranch) -> bool {
    static const std::regex stableBranchPattern(R"(branch_((v)?[0-9]+(\.[0-9]+){1,3}))", std::regex::icase);
    return !InBranch.empty() && std::regex_match(InBranch, stableBranchPattern);
}

auto ResolveLatestStableBranchRef(const std::filesystem::path& InRepo,
                                  const std::string& InRefPrefix,
                                  const std::string& InShortPrefix) -> std::string {
    const auto refs = GitCapture(InRepo, {"for-each-ref", "--sort=-version:refname", "--format=%(refname:short)", InRefPrefix});
    if (refs.exitCode != 0) {
        return {};
    }

    std::istringstream iss(refs.stdoutStr);
    std::string line;
    while (std::getline(iss, line)) {
        line = Trim(line);
        if (line.empty()) {
            continue;
        }
        if (!InShortPrefix.empty() && line.starts_with(InShortPrefix)) {
            line = line.substr(InShortPrefix.size());
        }
        if (IsStableBranchName(line)) {
            return line;
        }
    }
    return {};
}

auto ResolveLocalTarget(const std::filesystem::path& InRoot, const ResetPlan& InPlan) -> std::optional<ResetResolution> {
    const auto current = CurrentBranch(InPlan.path);
    if (InPlan.type == "registered") {
        const auto gmBranch = ResolveGitmodulesBranch(InRoot, InPlan.relPath);
        if (!gmBranch.empty() && LocalRefExists(InPlan.path, gmBranch)) {
            return ResetResolution{
                .checkoutBranch = gmBranch,
                .resetRef = std::format("refs/heads/{}", gmBranch),
                .branchSource = "registered .gitmodules branch",
            };
        }
    }
    if (!current.empty() && LocalRefExists(InPlan.path, current)) {
        return ResetResolution{
            .checkoutBranch = current,
            .resetRef = std::format("refs/heads/{}", current),
            .branchSource = current == CurrentBranch(InRoot) ? "current branch" : "repo current branch",
        };
    }
    return std::nullopt;
}

auto ResolveRemoteTarget(const std::filesystem::path& InRoot, const ResetPlan& InPlan) -> std::optional<ResetResolution> {
    if (InPlan.remote.empty()) {
        return std::nullopt;
    }
    const auto current = CurrentBranch(InPlan.path);
    std::string targetBranch;
    std::string branchSource;
    if (InPlan.relPath == ".") {
        if (!current.empty()) {
            targetBranch = current;
            branchSource = "root current branch";
        } else {
            targetBranch = DetectRemoteDefaultBranch(InPlan.path, InPlan.remote);
            branchSource = "root detached -> remote default";
        }
    } else if (InPlan.type == "registered") {
        const auto gmBranch = ResolveGitmodulesBranch(InRoot, InPlan.relPath);
        if (!gmBranch.empty()) {
            targetBranch = gmBranch;
            branchSource = "registered .gitmodules branch";
        } else {
            targetBranch = DetectRemoteDefaultBranch(InPlan.path, InPlan.remote);
            branchSource = "registered remote default branch";
        }
    } else {
        if (!current.empty()) {
            targetBranch = current;
            branchSource = "unregistered current branch";
        } else {
            targetBranch = DetectRemoteDefaultBranch(InPlan.path, InPlan.remote);
            branchSource = "unregistered detached -> remote default";
        }
    }
    if (targetBranch.empty() || !RemoteRefExists(InPlan.path, InPlan.remote, targetBranch)) {
        return std::nullopt;
    }
    return ResetResolution{
        .checkoutBranch = targetBranch,
        .resetRef = std::format("refs/remotes/{}/{}", InPlan.remote, targetBranch),
        .branchSource = branchSource,
    };
}

auto ResolveStableTarget(const std::filesystem::path& InRoot,
                         const ResetPlan& InPlan,
                         const ResetMode InMode) -> std::optional<ResetResolution> {
    const auto current = CurrentBranch(InPlan.path);
    if (InMode == ResetMode::StableLocal && IsStableBranchName(current) && LocalRefExists(InPlan.path, current)) {
        return ResetResolution{
            .checkoutBranch = current,
            .resetRef = std::format("refs/heads/{}", current),
            .branchSource = "current stable branch",
        };
    }

    if (InMode == ResetMode::StableRemote &&
        !InPlan.remote.empty() &&
        IsStableBranchName(current) &&
        RemoteRefExists(InPlan.path, InPlan.remote, current)) {
        return ResetResolution{
            .checkoutBranch = current,
            .resetRef = std::format("refs/remotes/{}/{}", InPlan.remote, current),
            .branchSource = "current stable branch",
            .shouldUpdateGitmodules = InPlan.type == "registered" && InPlan.relPath != ".",
        };
    }

    if (InPlan.type == "registered") {
        const auto gmBranch = ResolveGitmodulesBranch(InRoot, InPlan.relPath);
        if (InMode == ResetMode::StableLocal &&
            IsStableBranchName(gmBranch) &&
            LocalRefExists(InPlan.path, gmBranch)) {
            return ResetResolution{
                .checkoutBranch = gmBranch,
                .resetRef = std::format("refs/heads/{}", gmBranch),
                .branchSource = "registered .gitmodules stable branch",
            };
        }
        if (InMode == ResetMode::StableRemote &&
            !InPlan.remote.empty() &&
            IsStableBranchName(gmBranch) &&
            RemoteRefExists(InPlan.path, InPlan.remote, gmBranch)) {
            return ResetResolution{
                .checkoutBranch = gmBranch,
                .resetRef = std::format("refs/remotes/{}/{}", InPlan.remote, gmBranch),
                .branchSource = "registered .gitmodules stable branch",
                .shouldUpdateGitmodules = InPlan.relPath != ".",
            };
        }
    }

    if (InMode == ResetMode::StableLocal) {
        const auto stableBranch = ResolveLatestStableBranchRef(InPlan.path, "refs/heads", "");
        if (!stableBranch.empty() && LocalRefExists(InPlan.path, stableBranch)) {
            return ResetResolution{
                .checkoutBranch = stableBranch,
                .resetRef = std::format("refs/heads/{}", stableBranch),
                .branchSource = "latest local stable branch",
            };
        }
    }

    if (InMode == ResetMode::StableRemote && !InPlan.remote.empty()) {
        const auto stableBranch = ResolveLatestStableBranchRef(
            InPlan.path,
            std::format("refs/remotes/{}", InPlan.remote),
            InPlan.remote + "/");
        if (!stableBranch.empty() && RemoteRefExists(InPlan.path, InPlan.remote, stableBranch)) {
            return ResetResolution{
                .checkoutBranch = stableBranch,
                .resetRef = std::format("refs/remotes/{}/{}", InPlan.remote, stableBranch),
                .branchSource = "latest remote stable branch",
                .shouldUpdateGitmodules = InPlan.type == "registered" && InPlan.relPath != ".",
            };
        }
    }

    const auto latestTag = ResolveLatestStableTag(InPlan.path);
    if (latestTag.empty()) {
        return std::nullopt;
    }
    const auto stableBranch = std::string{"branch_"} + latestTag;
    if (InMode == ResetMode::StableLocal && LocalRefExists(InPlan.path, stableBranch)) {
        return ResetResolution{
            .checkoutBranch = stableBranch,
            .resetRef = std::format("refs/heads/{}", stableBranch),
            .branchSource = "local stable branch",
        };
    }
    if (InMode == ResetMode::StableRemote && !InPlan.remote.empty() && RemoteRefExists(InPlan.path, InPlan.remote, stableBranch)) {
        return ResetResolution{
            .checkoutBranch = stableBranch,
            .resetRef = std::format("refs/remotes/{}/{}", InPlan.remote, stableBranch),
            .branchSource = "remote stable branch",
            .shouldUpdateGitmodules = InPlan.type == "registered" && InPlan.relPath != ".",
        };
    }
    if (TagRefExists(InPlan.path, latestTag)) {
        return ResetResolution{
            .checkoutBranch = stableBranch,
            .resetRef = std::format("refs/tags/{}", latestTag),
            .branchSource = InMode == ResetMode::StableRemote ? "fetched stable tag" : "local stable tag",
        };
    }
    return std::nullopt;
}

auto RunResetPlan(const std::filesystem::path& InRoot,
                  const ResetPlan& InPlan,
                  ResetMode InMode,
                  bool InDryRun,
                  bool InFetchFirst) -> ResetResult {
    ResetResult out;
    out.repo = InPlan.displayName;

    if (InFetchFirst && !InPlan.remote.empty()) {
        std::vector<std::string> fetchArgs = {"fetch", InPlan.remote, "--prune"};
        if (InMode == ResetMode::StableLocal || InMode == ResetMode::StableRemote) {
            fetchArgs.push_back("--tags");
        }
        const auto fetch = GitCapture(InPlan.path, fetchArgs);
        if (fetch.exitCode != 0) {
            out.exitCode = 1;
            out.message = std::format("fetch failed for remote '{}'", InPlan.remote);
            return out;
        }
    }

    std::optional<ResetResolution> resolution;
    if (InMode == ResetMode::Local) {
        resolution = ResolveLocalTarget(InRoot, InPlan);
    } else if (InMode == ResetMode::Remote) {
        resolution = ResolveRemoteTarget(InRoot, InPlan);
    } else if (InMode == ResetMode::StableLocal) {
        resolution = ResolveStableTarget(InRoot, InPlan, InMode);
        if (!resolution.has_value()) {
            resolution = ResolveLocalTarget(InRoot, InPlan);
        }
    } else if (InMode == ResetMode::StableRemote) {
        resolution = ResolveStableTarget(InRoot, InPlan, InMode);
    }

    if (!resolution.has_value()) {
        out.exitCode = 1;
        out.message = "could not resolve target reset ref";
        return out;
    }

    out.branchSource = resolution->branchSource;
    out.resetRef = resolution->resetRef;

    if (InDryRun) {
        out.message = std::format("[DRY-RUN] Would checkout '{}' then reset --hard '{}' and clean -fd",
                                  resolution->checkoutBranch,
                                  resolution->resetRef);
        if (resolution->shouldUpdateGitmodules) {
            out.message += std::format("; would update .gitmodules branch to '{}'", resolution->checkoutBranch);
        }
        if (IsWorkingTreeDirty(InPlan.path)) {
            out.message += "; would stash uncommitted changes";
        }
        return out;
    }

    if (IsWorkingTreeDirty(InPlan.path)) {
        const auto stash = GitCapture(InPlan.path, {"stash", "push", "--include-untracked", "-m", "kog auto stash before reset"});
        if (stash.exitCode != 0) {
            out.message = "warning: stash failed";
        }
    }

    const auto checkout = GitCapture(InPlan.path, {"checkout", "-q", "-f", "-B", resolution->checkoutBranch, resolution->resetRef});
    if (checkout.exitCode != 0) {
        out.exitCode = 1;
        out.message = out.message.empty() ? std::format("checkout -f -B failed for '{}'", resolution->checkoutBranch)
                                          : std::format("checkout -f -B failed for '{}' ({})", resolution->checkoutBranch, out.message);
        return out;
    }

    const auto reset = GitCapture(InPlan.path, {"reset", "--hard", resolution->resetRef});
    if (reset.exitCode != 0) {
        out.exitCode = 1;
        out.message = std::format("reset --hard failed for '{}'", resolution->resetRef);
        return out;
    }

    const auto clean = GitCapture(InPlan.path, {"clean", "-fd"});
    if (clean.exitCode != 0) {
        out.exitCode = 1;
        out.message = "git clean -fd failed";
        return out;
    }

    if (resolution->shouldUpdateGitmodules &&
        !UpdateGitmodulesBranch(InRoot, InPlan.relPath, resolution->checkoutBranch)) {
        out.exitCode = 1;
        out.message = std::format("updated repo but failed to write .gitmodules branch '{}'", resolution->checkoutBranch);
        return out;
    }

    out.message = out.message.empty() ? "ok" : "ok (" + out.message + ")";
    return out;
}

void PrintResetResult(const ResetResult& InResult) {
    std::string repoName = InResult.repo.empty() ? "." : InResult.repo;
    std::cout << "[" << kano::terminal::Wrap(repoName, kano::terminal::Color::BoldCyan) << "] ";
    
    if (!InResult.branchSource.empty()) {
        std::cout << InResult.branchSource << " -> ";
    }
    if (!InResult.resetRef.empty()) {
        std::cout << InResult.resetRef << " ";
    }
    
    if (InResult.exitCode == 0) {
        std::cout << kano::terminal::Wrap("OK", kano::terminal::Color::BoldGreen);
    } else {
        std::cout << kano::terminal::Wrap("FAIL", kano::terminal::Color::BoldRed);
    }
    
    if (!InResult.message.empty()) {
        std::cout << " | ";
        if (InResult.exitCode == 0) {
            std::cout << kano::terminal::Wrap(InResult.message, kano::terminal::Color::Green);
        } else {
            std::cout << kano::terminal::Wrap(InResult.message, kano::terminal::Color::Red);
        }
    }
    std::cout << "\n";
}

void RegisterResetMode(CLI::App* InCmd,
                       ResetMode InMode,
                       const std::string& InPreferredRemote,
                       const bool InFetchByDefault) {
    auto* repo = new std::string{"."};
    auto* maxDepth = new int{12};
    auto* noCache = new bool{false};
    auto* refreshCache = new bool{false};
    auto* noRecursive = new bool{false};
    auto* dryRun = new bool{false};
    auto* continueOnError = new bool{false};
    auto* fetch = new bool{InFetchByDefault};
    auto* remote = new std::string{InPreferredRemote};

    InCmd->add_option("--repo", *repo, "Target repository root path");
    InCmd->add_option("--native-max-depth", *maxDepth, "Native discovery max depth (0 = unlimited)");
    InCmd->add_flag("--native-no-cache", *noCache, "Disable native discovery cache");
    InCmd->add_flag("--native-refresh-cache", *refreshCache, "Force native cache refresh");
    InCmd->add_flag("--no-recursive,-N", *noRecursive, "Reset only current repository");
    InCmd->add_flag("--dry-run", *dryRun, "Preview reset actions without modifying repositories");
    InCmd->add_flag("--continue-on-error", *continueOnError, "Continue if a repo fails");
    InCmd->add_flag("--fetch", *fetch, "Refresh refs before resolving reset targets");
    if (InMode != ResetMode::Local) {
        InCmd->add_option("--remote", *remote, "Preferred remote name");
    }

    InCmd->callback([=]() {
        const auto root = std::filesystem::weakly_canonical(std::filesystem::path(*repo));
        std::vector<ResetPlan> plans;
        try {
            plans = BuildResetPlans(root, *remote, *maxDepth, *noCache, *refreshCache);
        } catch (const std::exception& ex) {
            std::cerr << "ERROR: reset discovery failed: " << ex.what() << "\n";
            std::exit(1);
        }

        if (*noRecursive) {
            const auto resolvedRoot = ResolveRepoFromSpec(root, std::filesystem::path("."), *maxDepth, !*noCache);
            plans.erase(
                std::remove_if(plans.begin(), plans.end(), [&](const ResetPlan& InPlan) {
                    return std::filesystem::weakly_canonical(InPlan.path) != resolvedRoot;
                }),
                plans.end());
        }

        if (plans.empty()) {
            std::cout << "No repositories found.\n";
            std::exit(0);
        }

        const auto workspaceRoot = std::filesystem::weakly_canonical(root);
        int successCount = 0;
        int failureCount = 0;
        for (const auto& plan : plans) {
            const auto result = RunResetPlan(workspaceRoot, plan, InMode, *dryRun, *fetch);
            PrintResetResult(result);
            if (result.exitCode == 0) {
                successCount += 1;
            } else {
                failureCount += 1;
                if (!*continueOnError) {
                    std::cout << "\nSummary: " << (successCount + failureCount) << " repos, "
                              << successCount << " succeeded, " << failureCount << " failed\n";
                    std::exit(1);
                }
            }
        }

        std::cout << "\nSummary: " << (successCount + failureCount) << " repos, "
                  << successCount << " succeeded, " << failureCount << " failed\n";
        std::exit(failureCount > 0 ? 1 : 0);
    });
}

} // namespace

void RegisterReset(CLI::App& InApp) {
    auto* cmd = InApp.add_subcommand("reset", "Hard reset workspace repos to local/remote/stable branch concepts");

    auto* local = cmd->add_subcommand("local", "Hard reset to local branch concepts recursively");
    RegisterResetMode(local, ResetMode::Local, "", false);

    auto* remote = cmd->add_subcommand("remote", "Hard reset to remote-tracking branch concepts recursively");
    RegisterResetMode(remote, ResetMode::Remote, "origin", true);

    auto* stableLocal = cmd->add_subcommand("stable-local", "Hard reset to locally available stable branch/tag concepts recursively");
    RegisterResetMode(stableLocal, ResetMode::StableLocal, "", false);

    auto* stableRemote = cmd->add_subcommand("stable-remote", "Hard reset to remote stable branch/tag concepts recursively");
    RegisterResetMode(stableRemote, ResetMode::StableRemote, "origin", true);
}

} // namespace kano::git::commands
