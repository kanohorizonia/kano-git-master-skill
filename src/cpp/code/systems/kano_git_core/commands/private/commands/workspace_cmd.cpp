// workspace command — Multi-repository workspace operations (native C++)

#include "command_registry.hpp"
#include "shell_executor.hpp"
#include "native_workspace.hpp"
#include "discovery.hpp"

#include <algorithm>
#include <chrono>
#include <format>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <future>
#include <iomanip>
#include <iostream>
#include <map>
#include <thread>
#include <sstream>

namespace kano::git::commands {
namespace {

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

auto UtcNowIso() -> std::string {
    const std::time_t raw = std::time(nullptr);
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &raw);
#else
    gmtime_r(&raw, &tm);
#endif
    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
    return oss.str();
}

auto FormatNativeStatusJson(const std::vector<workspace::RepoRecord>& InRepos) -> std::string {
    std::string out;
    out += "{";
    out += "\"generated_at\":\"" + EscapeJson(UtcNowIso()) + "\",";
    out += "\"repos\":[";
    for (std::size_t i = 0; i < InRepos.size(); ++i) {
        if (i > 0) {
            out += ",";
        }
        const auto& repo = InRepos[i];
        out += "{";
        out += "\"path\":\"" + EscapeJson(repo.path.lexically_normal().generic_string()) + "\",";
        out += "\"type\":\"" + EscapeJson(repo.type) + "\",";
        out += "\"branch\":\"" + EscapeJson(repo.currentBranch.empty() ? "(detached)" : repo.currentBranch) + "\",";
        out += "\"dirty\":";
        out += repo.hasChanges ? "true" : "false";
        out += "}";
    }
    out += "]}";
    return out;
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

auto FormatNativeStatusTable(const std::vector<workspace::RepoRecord>& InRepos, const std::filesystem::path& InRoot) -> std::string {
    std::ostringstream oss;
    std::map<std::string, std::vector<std::size_t>> groupedRepoIndexes;
    for (std::size_t i = 0; i < InRepos.size(); ++i) {
        const auto relativePath = RelativeDisplayPath(InRoot, InRepos[i].path);
        groupedRepoIndexes[GroupFromRelativePath(relativePath)].push_back(i);
    }

    std::size_t globalIndex = 0;
    for (const auto& [group, indexes] : groupedRepoIndexes) {
        oss << "GROUP: " << group << "\n";
        oss << std::left
            << std::setw(6) << "#"
            << std::setw(26) << "REPO"
            << std::setw(20) << "BRANCH"
            << std::setw(14) << "TYPE"
            << std::setw(8) << "DIRTY"
            << "\n";

        for (const auto repoIdx : indexes) {
            const auto& repo = InRepos[repoIdx];
            globalIndex += 1;

            auto repoName = RepoNameFromPath(repo.path);
            if (repoName.size() > 24) {
                repoName = repoName.substr(0, 21) + "...";
            }

            auto branch = repo.currentBranch.empty() ? "(detached)" : repo.currentBranch;
            if (branch.size() > 18) {
                branch = branch.substr(0, 15) + "...";
            }

            oss << std::left
                << std::setw(6) << std::to_string(globalIndex)
                << std::setw(26) << repoName
                << std::setw(20) << branch
                << std::setw(14) << repo.type
                << std::setw(8) << (repo.hasChanges ? "yes" : "no")
                << "\n";
        }
        oss << "\n";
    }

    if (InRepos.empty()) {
        oss << "(no repositories discovered)\n";
    }

    return oss.str();
}

auto FormatNativeStatusMarkdown(const std::vector<workspace::RepoRecord>& InRepos) -> std::string {
    std::size_t dirty = 0;
    for (const auto& repo : InRepos) {
        if (repo.hasChanges) {
            dirty += 1;
        }
    }

    std::ostringstream oss;
    oss << "# Workspace Status\n\n";
    oss << "Generated: " << UtcNowIso() << "\n\n";
    oss << "- Total repos: " << InRepos.size() << "\n";
    oss << "- Dirty repos: " << dirty << "\n";
    oss << "- Clean repos: " << (InRepos.size() - dirty) << "\n\n";
    oss << "| Path | Branch | Type | Dirty |\n";
    oss << "| --- | --- | --- | --- |\n";
    for (const auto& repo : InRepos) {
        const auto branch = repo.currentBranch.empty() ? "(detached)" : repo.currentBranch;
        oss << "| "
            << repo.path.lexically_normal().generic_string() << " | "
            << branch << " | "
            << repo.type << " | "
            << (repo.hasChanges ? "yes" : "no")
            << " |\n";
    }
    return oss.str();
}

struct RepoExecResult {
    std::string path;
    std::string type;
    int exitCode = 0;
    std::string stdoutStr;
    std::string stderrStr;
};

struct RepoUpdateResult {
    std::string path;
    std::string type;
    int exitCode = 0;
    bool skipped = false;
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

auto GitCaptureRoot(const std::vector<std::string>& InArgs) -> shell::ExecResult {
    return shell::ExecuteCommand("git", InArgs, shell::ExecMode::Capture);
}

auto DefaultBranchFromRemote(const std::filesystem::path& InRepo, const std::string& InRemote) -> std::string {
    const auto ref = GitCapture(InRepo, {"symbolic-ref", "--quiet", "--short", std::format("refs/remotes/{}/HEAD", InRemote)});
    if (ref.exitCode == 0) {
        const auto value = Trim(ref.stdoutStr);
        const auto slash = value.find('/');
        if (slash != std::string::npos && slash + 1 < value.size()) {
            return value.substr(slash + 1);
        }
    }

    const auto remoteShow = GitCapture(InRepo, {"remote", "show", InRemote});
    if (remoteShow.exitCode != 0) {
        return {};
    }
    std::istringstream iss(remoteShow.stdoutStr);
    std::string line;
    while (std::getline(iss, line)) {
        const std::string marker = "HEAD branch:";
        const auto pos = line.find(marker);
        if (pos == std::string::npos) {
            continue;
        }
        const auto branch = Trim(line.substr(pos + marker.size()));
        if (!branch.empty() && branch != "(unknown)") {
            return branch;
        }
    }
    return {};
}

auto UpdateRepoNative(const workspace::RepoRecord& InRepo, const std::string& InRemote, bool InDryRun) -> RepoUpdateResult {
    RepoUpdateResult out;
    out.path = InRepo.path.lexically_normal().generic_string();
    out.type = InRepo.type;

    const auto repoCheck = GitCapture(InRepo.path, {"rev-parse", "--git-dir"});
    if (repoCheck.exitCode != 0) {
        out.exitCode = 1;
        out.message = "Not a git repository";
        return out;
    }

    const auto remoteCheck = GitCapture(InRepo.path, {"remote", "get-url", InRemote});
    if (remoteCheck.exitCode != 0) {
        out.exitCode = 0;
        out.skipped = true;
        out.message = std::format("Skip: remote '{}' not found", InRemote);
        return out;
    }

    const auto currentBranchResult = GitCapture(InRepo.path, {"symbolic-ref", "--quiet", "--short", "HEAD"});
    const auto currentBranch = Trim(currentBranchResult.stdoutStr);
    if (currentBranchResult.exitCode != 0 || currentBranch.empty()) {
        out.exitCode = 0;
        out.skipped = true;
        out.message = "Skip: detached HEAD";
        return out;
    }

    std::string targetBranch = currentBranch;
    const auto remoteBranchCheck = GitCapture(InRepo.path, {"show-ref", "--verify", "--quiet", std::format("refs/remotes/{}/{}", InRemote, currentBranch)});
    if (remoteBranchCheck.exitCode != 0) {
        targetBranch = DefaultBranchFromRemote(InRepo.path, InRemote);
        if (targetBranch.empty()) {
            out.exitCode = 1;
            out.message = "Could not detect remote default branch";
            return out;
        }
    }

    if (InDryRun) {
        out.exitCode = 0;
        out.message = std::format("[DRY-RUN] Would fetch '{}' and rebase onto '{}/{}'", InRemote, InRemote, targetBranch);
        return out;
    }

    bool stashCreated = false;
    const auto changes = GitCapture(InRepo.path, {"status", "--porcelain"});
    if (changes.exitCode == 0 && !Trim(changes.stdoutStr).empty()) {
        const auto stashPush = GitCapture(InRepo.path, {"stash", "push", "-m", "auto-stash-workspace-update"});
        if (stashPush.exitCode == 0 && stashPush.stdoutStr.find("No local changes to save") == std::string::npos) {
            stashCreated = true;
        }
    }

    const auto fetch = GitCapture(InRepo.path, {"fetch", InRemote});
    if (fetch.exitCode != 0) {
        out.exitCode = 1;
        out.message = std::format("Fetch failed for remote '{}'", InRemote);
        return out;
    }

    const auto rebase = GitCapture(InRepo.path, {"rebase", std::format("{}/{}", InRemote, targetBranch)});
    if (rebase.exitCode != 0) {
        out.exitCode = 1;
        out.message = std::format("Rebase failed on '{}/{}'", InRemote, targetBranch);
        return out;
    }

    if (stashCreated) {
        const auto stashPop = GitCapture(InRepo.path, {"stash", "pop"});
        if (stashPop.exitCode != 0) {
            out.exitCode = 1;
            out.message = "Updated but failed to restore stash";
            return out;
        }
    }

    out.exitCode = 0;
    out.message = std::format("Updated via {}/{}", InRemote, targetBranch);
    return out;
}

auto PrintRepoUpdateResult(const RepoUpdateResult& InResult) -> void {
    std::cout << "\n==> [" << InResult.path << "] (" << InResult.type << ")\n";
    if (InResult.skipped) {
        std::cout << InResult.message << "\n";
        return;
    }
    if (InResult.exitCode == 0) {
        std::cout << InResult.message << "\n";
    } else {
        std::cerr << InResult.message << "\n";
    }
}

auto ExecuteRepoCommand(const workspace::RepoRecord& InRepo, const std::string& InCommand) -> RepoExecResult {
    RepoExecResult out;
    out.path = InRepo.path.lexically_normal().generic_string();
    out.type = InRepo.type;

#if defined(_WIN32)
    auto result = shell::ExecuteCommand(
        "cmd.exe",
        {"/d", "/s", "/c", InCommand},
        shell::ExecMode::Capture,
        InRepo.path);
#else
    auto result = shell::ExecuteCommand(
        "bash",
        {"-lc", InCommand},
        shell::ExecMode::Capture,
        InRepo.path);
#endif

    out.exitCode = result.exitCode;
    out.stdoutStr = std::move(result.stdoutStr);
    out.stderrStr = std::move(result.stderrStr);
    return out;
}

auto PrintRepoExecResult(const RepoExecResult& InResult) -> void {
    std::cout << "\n==> [" << InResult.path << "] (" << InResult.type << ")\n";
    if (!InResult.stdoutStr.empty()) {
        std::cout << InResult.stdoutStr;
        if (InResult.stdoutStr.back() != '\n') {
            std::cout << '\n';
        }
    }
    if (!InResult.stderrStr.empty()) {
        std::cerr << InResult.stderrStr;
        if (InResult.stderrStr.back() != '\n') {
            std::cerr << '\n';
        }
    }
}

} // namespace

void RegisterWorkspace(CLI::App& InApp) {
    auto* cmd = InApp.add_subcommand("workspace", "Multi-repository workspace operations");

    auto* status = cmd->add_subcommand("status", "Status report of all repos");
    status->allow_extras();
    auto* statusNative = new bool{false};
    auto* statusNativeMaxDepth = new int{3};
    auto* statusNativeExclude = new std::vector<std::string>{};
    auto* statusNativeNoCache = new bool{false};
    auto* statusNativeRefreshCache = new bool{false};
    auto* statusNativeNoIncremental = new bool{false};
    auto* statusNativeCacheTtl = new int{60};
    auto* statusNativeMaxStale = new int{900};
    auto* statusNativeMetadata = new std::string{"full"};
    auto* statusShell = new bool{false};
    auto* statusManifest = new std::string{};
    auto* statusRepoRoot = new std::string{"."};
    auto* statusIncludeTypes = new std::string{};
    auto* statusNoSubmodules = new bool{false};
    auto* statusNoRecursive = new bool{false};
    auto* statusExclude = new std::vector<std::string>{};
    auto* statusMaxDepth = new int{3};
    auto* statusCheckRemote = new bool{false};
    auto* statusDetail = new bool{false};
    auto* statusDetailCommits = new int{3};
    auto* statusDetailLog = new std::string{"oneline"};
    auto* statusFormat = new std::string{"table"};
    auto* statusOutput = new std::string{};

    status->add_flag("--native", *statusNative, "Use native C++ workspace status implementation (default)");
    status->add_flag("--shell", *statusShell, "Deprecated compatibility flag (shell path removed)");
    status->add_option("--native-max-depth", *statusNativeMaxDepth, "Native discovery max depth");
    status->add_option("--native-exclude", *statusNativeExclude, "Native discovery exclude pattern (repeatable)");
    status->add_flag("--native-no-cache", *statusNativeNoCache, "Disable native discovery cache");
    status->add_flag("--native-refresh-cache", *statusNativeRefreshCache, "Force native cache refresh");
    status->add_flag("--native-no-incremental", *statusNativeNoIncremental, "Disable native incremental cache validation");
    status->add_option("--native-cache-ttl", *statusNativeCacheTtl, "Native cache TTL seconds");
    status->add_option("--native-max-stale", *statusNativeMaxStale, "Native incremental max stale seconds");
    status->add_option("--native-metadata-level", *statusNativeMetadata, "Native metadata level: full|minimal");

    status->add_option("--manifest", *statusManifest, "Manifest file path");
    status->add_option("--repo-root", *statusRepoRoot, "Repository root/start path");
    status->add_option("--include-types", *statusIncludeTypes, "Include repo types");
    status->add_flag("--no-submodules", *statusNoSubmodules, "Exclude registered submodules");
    status->add_flag("--no-recursive", *statusNoRecursive, "Disable recursive discovery");
    status->add_option("--exclude", *statusExclude, "Exclude path pattern (repeatable)");
    status->add_option("--max-depth", *statusMaxDepth, "Discovery max depth");
    status->add_flag("--check-remote", *statusCheckRemote, "Check remote status");
    status->add_flag("--detail", *statusDetail, "Show detail commits");
    status->add_option("--detail-commits", *statusDetailCommits, "Number of detail commits");
    status->add_option("--detail-log", *statusDetailLog, "Detail log mode: oneline|full");
    status->add_option("--format", *statusFormat, "Output format: table|json|markdown");
    status->add_option("--output", *statusOutput, "Write output to file");

    status->callback([=]() {
        if (*statusShell) {
            std::cerr << "Error: --shell is no longer supported; workspace status is fully native now\n";
            std::exit(2);
        }
        auto extras = status->remaining();
        if (!extras.empty()) {
            std::cerr << "Error: unsupported extra arguments in native workspace status mode:";
            for (const auto& extra : extras) {
                std::cerr << ' ' << extra;
            }
            std::cerr << "\n";
            std::exit(2);
        }

        if (*statusFormat != "table" && *statusFormat != "json" && *statusFormat != "markdown") {
            std::cerr << "Error: invalid --format value: " << *statusFormat << " (expected table|json|markdown)\n";
            std::exit(1);
        }

        workspace::DiscoverOptions options;
        options.rootDir = statusRepoRoot->empty() ? std::filesystem::current_path() : std::filesystem::path(*statusRepoRoot);
        options.maxDepth = *statusNativeMaxDepth > 0 ? *statusNativeMaxDepth : *statusMaxDepth;
        options.excludePatterns = *statusNativeExclude;
        if (options.excludePatterns.empty()) {
            options.excludePatterns = *statusExclude;
        }
        options.useCache = !*statusNativeNoCache;
        options.cacheTtlSeconds = *statusNativeCacheTtl;
        options.refreshCache = *statusNativeRefreshCache;
        options.incremental = !*statusNativeNoIncremental;
        options.maxStaleSeconds = *statusNativeMaxStale;
        options.metadataLevel = *statusNativeMetadata;

        auto discovery = workspace::DiscoverRepos(options);
        auto repos = discovery.repos;
        std::sort(repos.begin(), repos.end(), [](const workspace::RepoRecord& A, const workspace::RepoRecord& B) {
            return A.path.lexically_normal().generic_string() < B.path.lexically_normal().generic_string();
        });

        std::string output;
        if (*statusFormat == "json") {
            output = FormatNativeStatusJson(repos);
        } else if (*statusFormat == "markdown") {
            output = FormatNativeStatusMarkdown(repos);
        } else {
            output = FormatNativeStatusTable(repos, options.rootDir);
        }

        if (!statusOutput->empty()) {
            std::ofstream out(*statusOutput, std::ios::out | std::ios::binary | std::ios::trunc);
            out << output;
        } else {
            std::cout << output << "\n";
        }
        std::exit(0);
    });

    auto* discover = cmd->add_subcommand("discover", "Discover repositories and refresh discovery cache");
    auto* discoverFormat = new std::string{"table"};
    auto* discoverRoot = new std::string{"."};
    auto* discoverMaxDepth = new int{8};
    auto* discoverExclude = new std::vector<std::string>{};
    auto* discoverNoCache = new bool{false};
    auto* discoverNoRefresh = new bool{false};
    auto* discoverNoIncremental = new bool{false};
    auto* discoverCacheTtl = new int{60};
    auto* discoverMaxStale = new int{900};
    auto* discoverMetadata = new std::string{"full"};

    discover->add_option("--format", *discoverFormat, "Output format: table|json")->default_str("table");
    discover->add_option("--repo-root", *discoverRoot, "Repository root/start path");
    discover->add_option("--max-depth", *discoverMaxDepth, "Discovery max depth");
    discover->add_option("--exclude", *discoverExclude, "Exclude path pattern (repeatable)");
    discover->add_flag("--no-cache", *discoverNoCache, "Disable discovery cache for this run");
    discover->add_flag("--no-refresh-cache", *discoverNoRefresh, "Do not force cache refresh");
    discover->add_flag("--no-incremental", *discoverNoIncremental, "Disable incremental cache validation");
    discover->add_option("--cache-ttl", *discoverCacheTtl, "Cache TTL seconds");
    discover->add_option("--max-stale", *discoverMaxStale, "Incremental max stale seconds");
    discover->add_option("--metadata-level", *discoverMetadata, "Metadata level: full|minimal");

    discover->callback([=]() {
        if (*discoverFormat != "table" && *discoverFormat != "json") {
            std::cerr << "Error: invalid --format value: " << *discoverFormat << " (expected table|json)\n";
            std::exit(1);
        }

        workspace::DiscoverOptions options;
        options.rootDir = discoverRoot->empty() ? std::filesystem::current_path() : std::filesystem::path(*discoverRoot);
        options.maxDepth = *discoverMaxDepth;
        options.excludePatterns = *discoverExclude;
        options.useCache = !*discoverNoCache;
        options.cacheTtlSeconds = *discoverCacheTtl;
        options.refreshCache = !*discoverNoRefresh;
        options.incremental = !*discoverNoIncremental;
        options.maxStaleSeconds = *discoverMaxStale;
        options.metadataLevel = *discoverMetadata;

        auto discovery = workspace::DiscoverRepos(options);
        auto repos = discovery.repos;
        std::sort(repos.begin(), repos.end(), [](const workspace::RepoRecord& A, const workspace::RepoRecord& B) {
            return A.path.lexically_normal().generic_string() < B.path.lexically_normal().generic_string();
        });

        std::cout << "Discovery mode: " << discovery.mode << "\n";
        std::cout << "Discovery cache: " << discovery.cacheFile.lexically_normal().generic_string() << "\n";
        std::cout << "Repos discovered: " << repos.size() << "\n\n";

        if (*discoverFormat == "json") {
            std::cout << FormatNativeStatusJson(repos) << "\n";
        } else {
            std::cout << FormatNativeStatusTable(repos, options.rootDir) << "\n";
        }
        std::exit(0);
    });

    auto* update = cmd->add_subcommand("update", "Update all workspace repos");
    update->allow_extras();
    auto* updateShell = new bool{false};
    auto* nativeUpdate = new bool{false};
    auto* nativePlan = new bool{false};
    auto* nativePlanOnly = new bool{false};
    auto* nativeMaxDepth = new int{3};
    auto* nativeExclude = new std::vector<std::string>{};
    auto* nativeNoCache = new bool{false};
    auto* nativeRefreshCache = new bool{false};
    auto* nativeNoIncremental = new bool{false};
    auto* nativeCacheTtl = new int{60};
    auto* nativeMaxStale = new int{900};
    auto* updateManifest = new std::string{};
    auto* updateIncludeTypes = new std::string{"root,registered,unregistered"};
    auto* updateExclude = new std::vector<std::string>{};
    auto* updateRemote = new std::string{"origin"};
    auto* updateMaxDepth = new int{3};
    auto* updateParallel = new int{1};
    auto* updateContinueOnError = new bool{false};
    auto* updateDryRun = new bool{false};

    update->add_flag("--native", *nativeUpdate, "Use native C++ wave executor for update operations (default)");
    update->add_flag("--shell", *updateShell, "Deprecated compatibility flag (shell path removed)");
    update->add_flag("--native-plan", *nativePlan, "Use native C++ discovery + scheduler plan");
    update->add_flag("--native-plan-only", *nativePlanOnly, "Emit native wave plan JSON only (no shell update execution)");
    update->add_option("--native-max-depth", *nativeMaxDepth, "Native discovery max depth");
    update->add_option("--native-exclude", *nativeExclude, "Native discovery exclude pattern (repeatable)");
    update->add_flag("--native-no-cache", *nativeNoCache, "Disable native discovery cache");
    update->add_flag("--native-refresh-cache", *nativeRefreshCache, "Force native cache refresh");
    update->add_flag("--native-no-incremental", *nativeNoIncremental, "Disable native incremental cache validation");
    update->add_option("--native-cache-ttl", *nativeCacheTtl, "Native cache TTL seconds");
    update->add_option("--native-max-stale", *nativeMaxStale, "Native incremental max stale seconds");
    update->add_option("--manifest", *updateManifest, "Use manifest file (default: auto-discover)");
    update->add_option("--include-types", *updateIncludeTypes, "Comma-separated: root,registered,unregistered");
    update->add_option("--exclude", *updateExclude, "Exclude path pattern (repeatable)");
    update->add_option("--remote", *updateRemote, "Remote name (default: origin)");
    update->add_option("--max-depth", *updateMaxDepth, "Discovery max depth (default: 3)");
    update->add_option("--parallel", *updateParallel, "Parallel updates (default: 1)");
    update->add_flag("--continue-on-error", *updateContinueOnError, "Continue if a repo fails");
    update->add_flag("--dry-run", *updateDryRun, "Preview mode");

    update->callback([=]() {
        if (*nativePlanOnly) {
            *nativePlan = true;
        }

        if (*updateShell) {
            std::cerr << "Error: --shell is no longer supported; workspace update is fully native now\n";
            std::exit(2);
        }
        if (!*nativeUpdate && !*nativePlan) {
            *nativeUpdate = true;
        }
        auto extras = update->remaining();
        if (!extras.empty()) {
            std::cerr << "Error: unsupported extra arguments in native workspace update mode:";
            for (const auto& extra : extras) {
                std::cerr << ' ' << extra;
            }
            std::cerr << "\n";
            std::exit(2);
        }
        if (*nativePlan && !*nativePlanOnly) {
            *nativeUpdate = true;
            *nativePlan = false;
        }

        if (*nativeUpdate || *nativePlan) {
            if (*updateParallel <= 0) {
                std::cerr << "Error: --parallel must be >= 1\n";
                std::exit(1);
            }

            workspace::DiscoverOptions options;
            options.rootDir = std::filesystem::current_path();
            options.maxDepth = *nativeMaxDepth;
            options.excludePatterns = *nativeExclude;
            if (options.excludePatterns.empty()) {
                options.excludePatterns = *updateExclude;
            }
            options.useCache = !*nativeNoCache;
            options.cacheTtlSeconds = *nativeCacheTtl;
            options.refreshCache = *nativeRefreshCache;
            options.incremental = !*nativeNoIncremental;
            options.maxStaleSeconds = *nativeMaxStale;
            options.metadataLevel = "full";

            const auto native = workspace::BuildNativeWorkspaceOutput(options, std::filesystem::current_path());

            if (*nativePlanOnly) {
                std::cout << native.updatePlanJson << "\n";
                std::exit(native.hasCycle ? 2 : 0);
            }

            if (native.hasCycle) {
                std::cerr << "Error: workspace dependency graph has cycle(s), aborting native plan execution.\n";
                std::cerr << native.wavesJson << "\n";
                std::exit(2);
            }

            if (*nativeUpdate) {
                std::vector<std::string> includeTypes;
                {
                    std::stringstream ss(*updateIncludeTypes);
                    std::string item;
                    while (std::getline(ss, item, ',')) {
                        if (item.empty()) {
                            continue;
                        }
                        if (item == "submodule") {
                            item = "registered";
                        } else if (item == "standalone") {
                            item = "unregistered";
                        }
                        includeTypes.push_back(item);
                    }
                }
                if (includeTypes.empty()) {
                    includeTypes = {"root", "registered", "unregistered"};
                }

                const auto typeAllowed = [&includeTypes](const std::string& InType) {
                    return std::find(includeTypes.begin(), includeTypes.end(), InType) != includeTypes.end();
                };

                int successCount = 0;
                int failureCount = 0;
                int skippedCount = 0;

                for (const auto& wave : native.waves) {
                    std::vector<workspace::RepoRecord> repos;
                    repos.reserve(wave.size());
                    for (const auto idx : wave) {
                        if (idx >= native.discovery.repos.size()) {
                            continue;
                        }
                        const auto& repo = native.discovery.repos[idx];
                        if (typeAllowed(repo.type)) {
                            repos.push_back(repo);
                        }
                    }

                    if (repos.empty()) {
                        continue;
                    }

                    std::vector<RepoUpdateResult> results;
                    if (*updateParallel <= 1 || repos.size() == 1) {
                        for (const auto& repo : repos) {
                            results.push_back(UpdateRepoNative(repo, *updateRemote, *updateDryRun));
                        }
                    } else {
                        std::vector<std::future<RepoUpdateResult>> futures;
                        futures.reserve(repos.size());
                        for (const auto& repo : repos) {
                            futures.push_back(std::async(std::launch::async, [repo, updateRemote, updateDryRun]() {
                                return UpdateRepoNative(repo, *updateRemote, *updateDryRun);
                            }));
                        }
                        for (auto& fut : futures) {
                            results.push_back(fut.get());
                        }
                    }

                    for (const auto& result : results) {
                        PrintRepoUpdateResult(result);
                        if (result.exitCode == 0) {
                            successCount += 1;
                            if (result.skipped) {
                                skippedCount += 1;
                            }
                        } else {
                            failureCount += 1;
                            if (!*updateContinueOnError) {
                                std::cerr << "Error: update failed, stopping (use --continue-on-error to continue)\n";
                                std::cout << "\nSummary: " << (successCount + failureCount) << " repos, "
                                          << successCount << " succeeded, " << failureCount << " failed"
                                          << " (" << skippedCount << " skipped)\n";
                                std::exit(1);
                            }
                        }
                    }
                }

                std::cout << "\nSummary: " << (successCount + failureCount) << " repos, "
                          << successCount << " succeeded, " << failureCount << " failed"
                          << " (" << skippedCount << " skipped)\n";
                std::exit(failureCount > 0 ? 1 : 0);
            }

            std::cerr << "Error: native-plan adapter execution via shell is disabled in strict native-only mode\n";
            std::exit(2);
        }
        std::cerr << "Error: workspace update must run in native mode\n";
        std::exit(2);
    });

    auto* foreach = cmd->add_subcommand("foreach", "Run command on each repo");
    foreach->allow_extras();
    foreach->positionals_at_end(false);
    auto* foreachNative = new bool{false};
    auto* foreachShell = new bool{false};
    auto* foreachNativePlan = new bool{false};
    auto* foreachNativePlanOnly = new bool{false};
    auto* foreachCommand = new std::string{};
    auto* foreachPositionalCommand = new std::string{};
    auto* foreachContinueOnError = new bool{false};
    auto* foreachParallel = new int{1};
    auto* foreachNativeMaxDepth = new int{3};
    auto* foreachNativeExclude = new std::vector<std::string>{};
    auto* foreachNativeNoCache = new bool{false};
    auto* foreachNativeRefreshCache = new bool{false};
    auto* foreachNativeNoIncremental = new bool{false};
    auto* foreachNativeCacheTtl = new int{60};
    auto* foreachNativeMaxStale = new int{900};
    auto* foreachNativeMetadata = new std::string{"full"};
    auto* foreachManifest = new std::string{};
    auto* foreachIncludeTypes = new std::string{"root,registered,unregistered"};
    auto* foreachExclude = new std::vector<std::string>{};
    auto* foreachMaxDepth = new int{3};
    auto* foreachDryRun = new bool{false};

    foreach->add_flag("--native", *foreachNative, "Use native C++ wave executor (default)");
    foreach->add_flag("--shell", *foreachShell, "Deprecated compatibility flag (shell path removed)");
    foreach->add_flag("--native-plan", *foreachNativePlan, "Use native C++ planner + shell foreach adapter execution");
    foreach->add_flag("--native-plan-only", *foreachNativePlanOnly, "Emit native foreach plan JSON only");
    foreach->add_flag("--continue-on-error", *foreachContinueOnError, "Continue if command fails in a repo");
    foreach->add_option("--parallel", *foreachParallel, "Parallel execution per wave (default 1)");
    foreach->add_option("--native-max-depth", *foreachNativeMaxDepth, "Native discovery max depth");
    foreach->add_option("--native-exclude", *foreachNativeExclude, "Native discovery exclude pattern (repeatable)");
    foreach->add_flag("--native-no-cache", *foreachNativeNoCache, "Disable native discovery cache");
    foreach->add_flag("--native-refresh-cache", *foreachNativeRefreshCache, "Force native cache refresh");
    foreach->add_flag("--native-no-incremental", *foreachNativeNoIncremental, "Disable native incremental cache validation");
    foreach->add_option("--native-cache-ttl", *foreachNativeCacheTtl, "Native cache TTL seconds");
    foreach->add_option("--native-max-stale", *foreachNativeMaxStale, "Native incremental max stale seconds");
    foreach->add_option("--native-metadata-level", *foreachNativeMetadata, "Native metadata level: full|minimal");
    foreach->add_option("--manifest", *foreachManifest, "Manifest file path");
    foreach->add_option("--include-types", *foreachIncludeTypes, "Comma-separated repo types");
    foreach->add_option("--exclude", *foreachExclude, "Exclude path pattern (repeatable)");
    foreach->add_option("--max-depth", *foreachMaxDepth, "Discovery max depth");
    foreach->add_flag("--dry-run", *foreachDryRun, "Preview mode");
    foreach->add_option("--command", *foreachCommand, "Explicit command string for native foreach");
    foreach->add_option("cmd", *foreachPositionalCommand, "Positional command string");
    foreach->callback([=]() {
        if (*foreachNativePlanOnly) {
            *foreachNativePlan = true;
        }
        if (*foreachShell) {
            std::cerr << "Error: --shell is no longer supported; workspace foreach is fully native now\n";
            std::exit(2);
        }
        if (!*foreachNative && !*foreachNativePlan) {
            *foreachNative = true;
        }
        if (foreachCommand->empty() && !foreachPositionalCommand->empty()) {
            *foreachCommand = *foreachPositionalCommand;
        }
        auto extras = foreach->remaining();
        if (foreachCommand->empty() && !extras.empty()) {
            *foreachCommand = extras.front();
            extras.erase(extras.begin());
        }
        if (!extras.empty()) {
            std::cerr << "Error: unsupported extra arguments in native workspace foreach mode:";
            for (const auto& extra : extras) {
                std::cerr << ' ' << extra;
            }
            std::cerr << "\n";
            std::exit(2);
        }
        if (*foreachNativePlan && !*foreachNativePlanOnly) {
            *foreachNative = true;
            *foreachNativePlan = false;
        }

        if (*foreachNative || *foreachNativePlan) {
            if (foreachCommand->empty()) {
                std::cerr << "Error: command is required for native/native-plan foreach\n";
                std::exit(1);
            }
            if (*foreachParallel <= 0) {
                std::cerr << "Error: --parallel must be >= 1\n";
                std::exit(1);
            }

            workspace::DiscoverOptions options;
            options.rootDir = std::filesystem::current_path();
            options.maxDepth = *foreachNativeMaxDepth;
            options.excludePatterns = *foreachNativeExclude;
            if (options.excludePatterns.empty()) {
                options.excludePatterns = *foreachExclude;
            }
            options.useCache = !*foreachNativeNoCache;
            options.cacheTtlSeconds = *foreachNativeCacheTtl;
            options.refreshCache = *foreachNativeRefreshCache;
            options.incremental = !*foreachNativeNoIncremental;
            options.maxStaleSeconds = *foreachNativeMaxStale;
            options.metadataLevel = *foreachNativeMetadata;

            const auto native = workspace::BuildNativeWorkspaceOutput(options, std::filesystem::current_path());
            if (native.hasCycle) {
                std::cerr << "Error: workspace dependency graph has cycle(s); cannot execute native foreach\n";
                std::cerr << native.wavesJson << "\n";
                std::exit(2);
            }

            std::vector<std::string> includeTypes;
            {
                std::stringstream ss(*foreachIncludeTypes);
                std::string item;
                while (std::getline(ss, item, ',')) {
                    if (!item.empty()) {
                        if (item == "submodule") {
                            item = "registered";
                        } else if (item == "standalone") {
                            item = "unregistered";
                        }
                        includeTypes.push_back(item);
                    }
                }
            }
            if (includeTypes.empty()) {
                includeTypes = {"root", "registered", "unregistered"};
            }

            const auto typeAllowed = [&includeTypes](const std::string& InType) {
                return std::find(includeTypes.begin(), includeTypes.end(), InType) != includeTypes.end();
            };

            const auto foreachOperations = workspace::BuildPlanOperations(
                native.discovery.repos,
                native.waves,
                "foreach",
                *foreachCommand,
                includeTypes);

            const auto foreachPlanJson = workspace::BuildPlanJson(
                "native-foreach",
                foreachOperations,
                native.wavesJson,
                "workspace/foreach-repo.sh",
                {},
                *foreachCommand);

            if (*foreachNativePlanOnly) {
                std::cout << foreachPlanJson << "\n";
                std::exit(0);
            }

            if (*foreachNativePlan) {
                std::cerr << "Error: native-plan adapter execution via shell is disabled in strict native-only mode\n";
                std::exit(2);
            }

            int successCount = 0;
            int failureCount = 0;

            for (const auto& wave : native.waves) {
                std::vector<workspace::RepoRecord> repos;
                repos.reserve(wave.size());
                for (const auto idx : wave) {
                    if (idx >= native.discovery.repos.size()) {
                        continue;
                    }
                    const auto& repo = native.discovery.repos[idx];
                    if (typeAllowed(repo.type)) {
                        repos.push_back(repo);
                    }
                }

                if (repos.empty()) {
                    continue;
                }

                std::vector<RepoExecResult> results;
                if (*foreachParallel <= 1 || repos.size() == 1) {
                    for (const auto& repo : repos) {
                        results.push_back(ExecuteRepoCommand(repo, *foreachCommand));
                    }
                } else {
                    std::vector<std::future<RepoExecResult>> futures;
                    futures.reserve(repos.size());
                    for (const auto& repo : repos) {
                        futures.push_back(std::async(std::launch::async, [repo, foreachCommand]() {
                            return ExecuteRepoCommand(repo, *foreachCommand);
                        }));
                    }
                    for (auto& fut : futures) {
                        results.push_back(fut.get());
                    }
                }

                for (const auto& result : results) {
                    PrintRepoExecResult(result);
                    if (result.exitCode == 0) {
                        successCount += 1;
                    } else {
                        failureCount += 1;
                        if (!*foreachContinueOnError) {
                            std::cerr << "Error: command failed, stopping (use --continue-on-error to continue)\n";
                            std::cout << "\nSummary: " << (successCount + failureCount) << " repos, "
                                      << successCount << " succeeded, " << failureCount << " failed\n";
                            std::exit(1);
                        }
                    }
                }
            }

            std::cout << "\nSummary: " << (successCount + failureCount) << " repos, "
                      << successCount << " succeeded, " << failureCount << " failed\n";
            std::exit(failureCount > 0 ? 1 : 0);
        }

        std::cerr << "Error: workspace foreach must run in native mode\n";
        std::exit(2);
    });

    auto* update_repo = cmd->add_subcommand("update-repo", "Update a single repo + registered subrepos");
    auto* updateRepoPath = new std::string{"."};
    auto* updateRepoRemote = new std::string{"origin"};
    auto* updateRepoDryRun = new bool{false};
    update_repo->add_option("--repo", *updateRepoPath, "Repository path to update");
    update_repo->add_option("--remote", *updateRepoRemote, "Remote name to sync from");
    update_repo->add_flag("--dry-run", *updateRepoDryRun, "Preview single-repo update");
    update_repo->allow_extras();
    update_repo->callback([=]() {
        auto extras = update_repo->remaining();
        if (!extras.empty()) {
            std::cerr << "Error: unsupported extra arguments in native workspace update-repo mode:";
            for (const auto& extra : extras) {
                std::cerr << ' ' << extra;
            }
            std::cerr << "\n";
            std::exit(2);
        }

        workspace::RepoRecord repo;
        repo.path = std::filesystem::weakly_canonical(std::filesystem::path(*updateRepoPath));
        repo.type = "direct";
        const auto result = UpdateRepoNative(repo, *updateRepoRemote, *updateRepoDryRun);
        PrintRepoUpdateResult(result);
        std::exit(result.exitCode == 0 ? 0 : 1);
    });
}

} // namespace kano::git::commands
