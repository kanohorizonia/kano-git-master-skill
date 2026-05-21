// fetch command — recursively fetch workspace repos in parallel

#include <CLI/CLI.hpp>
#include "discovery.hpp"
#include "shell_executor.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace kano::git::commands {
namespace {

enum class FetchStatus {
    Unknown,
    Ok,
    Skip,
    Fail,
};

struct FetchOptions {
    std::filesystem::path repoRoot;
    int maxDepth = 8;
    bool useCache = true;
    bool refreshCache = false;
    bool noRecursive = false;
    std::string jobs = "auto";
    std::string remote;
    bool useAll = true;
    bool useTags = true;
    bool usePrune = true;
    bool dryRun = false;
    bool continueOnError = true;
};

struct FetchRepoTarget {
    std::filesystem::path repoPath;
    std::filesystem::path displayPath;
    std::string group;
    std::string repoName;
};

struct FetchRepoResult {
    FetchStatus status = FetchStatus::Unknown;
    int exitCode = 0;
    std::filesystem::path displayPath;
    std::string message;
    std::string commandPreview;
    std::string stdoutText;
    std::string stderrText;
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
        const auto trimmed = Trim(line);
        if (!trimmed.empty()) {
            out.push_back(trimmed);
        }
    }
    return out;
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

auto BuildFetchArgs(const FetchOptions& InOptions) -> std::vector<std::string> {
    std::vector<std::string> args;
    args.push_back("fetch");

    if (!InOptions.remote.empty()) {
        args.push_back(InOptions.remote);
    } else if (InOptions.useAll) {
        args.push_back("--all");
    }

    if (InOptions.usePrune) {
        args.push_back("--prune");
    }
    if (InOptions.useTags) {
        args.push_back("--tags");
    } else {
        args.push_back("--no-tags");
    }

    return args;
}

auto JoinCommandPreview(const std::vector<std::string>& InArgs) -> std::string {
    std::ostringstream oss;
    oss << "git";
    for (const auto& arg : InArgs) {
        oss << " " << arg;
    }
    return oss.str();
}

auto ResolveJobCount(const std::string& InJobs, const std::size_t InRepoCount) -> int {
    if (InRepoCount == 0) {
        return 1;
    }

    if (InJobs == "auto") {
        const auto hw = std::thread::hardware_concurrency();
        const auto fallback = static_cast<unsigned int>(4);
        int resolved = static_cast<int>(hw == 0 ? fallback : hw);
        resolved = std::max(1, resolved);
        resolved = std::min(resolved, static_cast<int>(InRepoCount));
        return resolved;
    }

    try {
        const int parsed = std::stoi(InJobs);
        if (parsed < 1) {
            return -1;
        }
        return std::min(parsed, static_cast<int>(InRepoCount));
    } catch (...) {
        return -1;
    }
}

auto CollectFetchTargets(const FetchOptions& InOptions) -> std::vector<FetchRepoTarget> {
    std::vector<FetchRepoTarget> targets;
    const auto root = InOptions.repoRoot.lexically_normal();

    if (InOptions.noRecursive) {
        if (!IsGitRepo(root)) {
            return targets;
        }
        FetchRepoTarget target;
        target.repoPath = root;
        target.displayPath = RelativeDisplayPath(root, root);
        target.group = GroupFromRelativePath(target.displayPath);
        target.repoName = RepoNameFromPath(root);
        targets.push_back(std::move(target));
        return targets;
    }

    workspace::DiscoverOptions discoverOptions;
    discoverOptions.rootDir = root;
    discoverOptions.maxDepth = InOptions.maxDepth;
    discoverOptions.useCache = InOptions.useCache;
    discoverOptions.refreshCache = InOptions.refreshCache;
    discoverOptions.metadataLevel = "minimal";
    discoverOptions.scope = workspace::DiscoverScope::Full;

    const auto discovered = workspace::DiscoverRepos(discoverOptions);
    targets.reserve(discovered.repos.size());
    for (const auto& repo : discovered.repos) {
        if (!IsGitRepo(repo.path)) {
            continue;
        }

        FetchRepoTarget target;
        target.repoPath = repo.path.lexically_normal();
        target.displayPath = RelativeDisplayPath(root, target.repoPath);
        target.group = GroupFromRelativePath(target.displayPath);
        target.repoName = RepoNameFromPath(target.repoPath);
        targets.push_back(std::move(target));
    }

    std::sort(targets.begin(), targets.end(), [](const FetchRepoTarget& InLhs, const FetchRepoTarget& InRhs) {
        if (InLhs.group != InRhs.group) {
            return InLhs.group < InRhs.group;
        }
        if (InLhs.repoName != InRhs.repoName) {
            return InLhs.repoName < InRhs.repoName;
        }
        return InLhs.displayPath.generic_string() < InRhs.displayPath.generic_string();
    });

    targets.erase(std::unique(targets.begin(), targets.end(), [](const FetchRepoTarget& InLhs, const FetchRepoTarget& InRhs) {
        return InLhs.repoPath.lexically_normal().generic_string() == InRhs.repoPath.lexically_normal().generic_string();
    }), targets.end());

    return targets;
}

auto RunFetchForRepo(const FetchRepoTarget& InTarget, const FetchOptions& InOptions) -> FetchRepoResult {
    FetchRepoResult out;
    out.displayPath = InTarget.displayPath;

    if (!IsGitRepo(InTarget.repoPath)) {
        out.status = FetchStatus::Fail;
        out.exitCode = 1;
        out.message = "not a git repository";
        return out;
    }

    const auto remotesOut = GitCapture(InTarget.repoPath, {"remote"});
    if (remotesOut.exitCode != 0) {
        out.status = FetchStatus::Fail;
        out.exitCode = remotesOut.exitCode;
        out.message = "failed to query remotes";
        out.stderrText = remotesOut.stderrStr;
        return out;
    }

    const auto remotes = SplitNonEmptyLines(remotesOut.stdoutStr);
    if (remotes.empty()) {
        out.status = FetchStatus::Skip;
        out.message = "no-remotes";
        return out;
    }

    if (!InOptions.remote.empty()) {
        const bool hasRequestedRemote = std::find(remotes.begin(), remotes.end(), InOptions.remote) != remotes.end();
        if (!hasRequestedRemote) {
            out.status = FetchStatus::Fail;
            out.exitCode = 2;
            out.message = "requested remote not found: " + InOptions.remote;
            return out;
        }
    }

    const auto fetchArgs = BuildFetchArgs(InOptions);
    out.commandPreview = JoinCommandPreview(fetchArgs);

    if (InOptions.dryRun) {
        out.status = FetchStatus::Ok;
        out.message = std::string{"[DRY RUN] "} + out.commandPreview;
        return out;
    }

    const auto run = GitCapture(InTarget.repoPath, fetchArgs);
    out.exitCode = run.exitCode;
    out.stdoutText = run.stdoutStr;
    out.stderrText = run.stderrStr;
    if (run.exitCode == 0) {
        out.status = FetchStatus::Ok;
        out.message = InOptions.remote.empty() ? "fetched all remotes" : ("fetched remote '" + InOptions.remote + "'");
    } else {
        out.status = FetchStatus::Fail;
        std::ostringstream oss;
        oss << "git fetch failed: exit=" << run.exitCode;
        out.message = oss.str();
    }
    return out;
}

auto RunFetchParallel(const std::vector<FetchRepoTarget>& InTargets,
                     const FetchOptions& InOptions,
                     const int InJobs) -> std::vector<FetchRepoResult> {
    std::vector<FetchRepoResult> results(InTargets.size());
    std::atomic<std::size_t> nextIndex{0};
    std::atomic<bool> stopRequested{false};

    const auto worker = [&]() {
        while (true) {
            if (stopRequested.load() && !InOptions.continueOnError) {
                return;
            }
            const auto index = nextIndex.fetch_add(1);
            if (index >= InTargets.size()) {
                return;
            }

            results[index] = RunFetchForRepo(InTargets[index], InOptions);
            if (results[index].status == FetchStatus::Fail && !InOptions.continueOnError) {
                stopRequested.store(true);
            }
        }
    };

    std::vector<std::thread> threads;
    threads.reserve(static_cast<std::size_t>(InJobs));
    for (int i = 0; i < InJobs; ++i) {
        threads.emplace_back(worker);
    }
    for (auto& thread : threads) {
        if (thread.joinable()) {
            thread.join();
        }
    }

    if (stopRequested.load() && !InOptions.continueOnError) {
        for (auto& result : results) {
            if (result.status == FetchStatus::Unknown) {
                result.status = FetchStatus::Skip;
                result.message = "fail-fast: not executed";
            }
        }
    }

    return results;
}

void PrintResultDetail(const FetchRepoResult& InResult) {
    if (InResult.status != FetchStatus::Fail) {
        return;
    }

    const auto stdoutLines = SplitNonEmptyLines(InResult.stdoutText);
    const auto stderrLines = SplitNonEmptyLines(InResult.stderrText);
    for (const auto& line : stdoutLines) {
        std::cout << "       stdout: " << line << "\n";
    }
    for (const auto& line : stderrLines) {
        std::cout << "       stderr: " << line << "\n";
    }
}

void PrintFetchSummary(const std::vector<FetchRepoResult>& InResults,
                       const std::chrono::steady_clock::time_point InStartedAt) {
    int ok = 0;
    int failed = 0;
    int skipped = 0;
    for (const auto& result : InResults) {
        switch (result.status) {
            case FetchStatus::Ok:
                ok += 1;
                break;
            case FetchStatus::Skip:
                skipped += 1;
                break;
            case FetchStatus::Fail:
                failed += 1;
                break;
            case FetchStatus::Unknown:
                skipped += 1;
                break;
        }
    }

    const auto elapsedSeconds = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - InStartedAt).count() / 1000.0;
    std::cout << "\nSUMMARY repos=" << InResults.size()
              << " ok=" << ok
              << " failed=" << failed
              << " skipped=" << skipped
              << " elapsed=" << std::fixed << std::setprecision(2) << elapsedSeconds << "s\n";
}

} // namespace

void RegisterFetch(CLI::App& InApp) {
    auto* cmd = InApp.add_subcommand("fetch", "Recursively fetch discovered repositories in parallel");

    auto* repoRoot = new std::string{"."};
    auto* maxDepth = new int{8};
    auto* noCache = new bool{false};
    auto* refreshCache = new bool{false};
    auto* noRecursive = new bool{false};
    auto* jobs = new std::string{"auto"};
    auto* remote = new std::string{};
    auto* all = new bool{false};
    auto* noTags = new bool{false};
    auto* noPrune = new bool{false};
    auto* dryRun = new bool{false};
    auto* continueOnError = new bool{true};
    auto* failFast = new bool{false};

    cmd->add_option("--repo-root", *repoRoot, "Workspace root path used for recursive discovery (default: .)");
    cmd->add_option("--max-depth", *maxDepth, "Discovery max depth (default: 8)");
    cmd->add_flag("--no-cache", *noCache, "Disable discovery cache");
    cmd->add_flag("--refresh-cache", *refreshCache, "Force refresh of discovery cache");
    cmd->add_flag("--no-recursive", *noRecursive, "Fetch only the repository at --repo-root");
    cmd->add_option("--jobs,-j", *jobs, "Parallel jobs: auto or integer >= 1 (default: auto)");
    cmd->add_option("--remote", *remote, "Fetch from one remote name instead of --all");
    cmd->add_flag("--all", *all, "Explicitly fetch all remotes when --remote is not set");
    cmd->add_flag("--no-tags", *noTags, "Do not fetch tags");
    cmd->add_flag("--no-prune", *noPrune, "Do not prune stale refs");
    cmd->add_flag("--dry-run", *dryRun, "Print fetch commands without executing");
    cmd->add_flag("--continue-on-error", *continueOnError, "Continue processing other repos when one fails (default)");
    cmd->add_flag("--fail-fast", *failFast, "Stop processing after the first fetch failure");

    cmd->callback([=]() {
        if (*maxDepth < 1) {
            std::cerr << "Error: --max-depth must be >= 1\n";
            std::exit(2);
        }

        FetchOptions options;
        options.repoRoot = std::filesystem::path(*repoRoot).lexically_normal();
        options.maxDepth = *maxDepth;
        options.useCache = !*noCache;
        options.refreshCache = *refreshCache;
        options.noRecursive = *noRecursive;
        options.jobs = Trim(*jobs);
        options.remote = Trim(*remote);
        options.useAll = options.remote.empty() ? true : *all;
        options.useTags = !*noTags;
        options.usePrune = !*noPrune;
        options.dryRun = *dryRun;
        options.continueOnError = *continueOnError;
        if (*failFast) {
            options.continueOnError = false;
        }

        if (options.jobs.empty()) {
            options.jobs = "auto";
        }

        const auto targets = CollectFetchTargets(options);
        if (targets.empty()) {
            std::cerr << "Error: no git repositories discovered under " << options.repoRoot.generic_string() << "\n";
            std::exit(1);
        }

        const int resolvedJobs = ResolveJobCount(options.jobs, targets.size());
        if (resolvedJobs < 1) {
            std::cerr << "Error: --jobs must be 'auto' or an integer >= 1\n";
            std::exit(2);
        }

        const auto modeLabel = options.remote.empty() ? "all" : ("remote:" + options.remote);
        std::cout << "FETCH workspace=" << options.repoRoot.generic_string()
                  << " repos=" << targets.size()
                  << " jobs=" << resolvedJobs
                  << " mode=" << modeLabel
                  << " prune=" << (options.usePrune ? "true" : "false")
                  << " tags=" << (options.useTags ? "true" : "false")
                  << (options.dryRun ? " dry-run=true" : "")
                  << "\n\n";

        const auto startedAt = std::chrono::steady_clock::now();
        const auto results = RunFetchParallel(targets, options, resolvedJobs);

        for (std::size_t i = 0; i < results.size(); ++i) {
            const auto& result = results[i];
            std::string statusText = "SKIP";
            if (result.status == FetchStatus::Ok) {
                statusText = options.dryRun ? "DRY" : "OK";
            } else if (result.status == FetchStatus::Fail) {
                statusText = "FAIL";
            }

            auto display = result.displayPath.generic_string();
            if (display.empty()) {
                display = ".";
            }

            std::cout << "[" << (i + 1) << "/" << results.size() << "] "
                      << std::left << std::setw(5) << statusText << " "
                      << std::setw(28) << display << " "
                      << result.message << "\n";
            PrintResultDetail(result);
        }

        PrintFetchSummary(results, startedAt);

        const bool hasFailure = std::any_of(results.begin(), results.end(), [](const FetchRepoResult& InResult) {
            return InResult.status == FetchStatus::Fail;
        });
        std::exit(hasFailure ? 1 : 0);
    });
}

} // namespace kano::git::commands
