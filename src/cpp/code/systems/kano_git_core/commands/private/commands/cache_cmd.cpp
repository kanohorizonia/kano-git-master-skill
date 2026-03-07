// cache command — show and clear kano-git cache locations

#include "command_registry.hpp"
#include "discovery.hpp"
#include "shell_executor.hpp"

#include <cstdlib>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <set>
#include <string>
#include <vector>
#include <chrono>

#if defined(_WIN32)
#include <io.h>
#include <process.h>
#else
#include <cerrno>
#include <csignal>
#include <unistd.h>
#endif

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

auto HomeDirectory() -> std::filesystem::path {
    if (const char* home = std::getenv("HOME"); home != nullptr && std::string(home).size() > 0) {
        return std::filesystem::path(home);
    }
    if (const char* userProfile = std::getenv("USERPROFILE"); userProfile != nullptr && std::string(userProfile).size() > 0) {
        return std::filesystem::path(userProfile);
    }
    return {};
}

auto InRepo(const std::filesystem::path& InDir) -> bool {
    const auto result = shell::ExecuteCommand("git", {"-C", InDir.generic_string(), "rev-parse", "--is-inside-work-tree"}, shell::ExecMode::Capture);
    return result.exitCode == 0 && Trim(result.stdoutStr) == "true";
}

struct ConfigLookup {
    std::string scope;
    std::string value;
};

struct EffectiveLookup {
    std::string value;
    std::string origin;
};

auto ReadGitConfigScoped(const std::filesystem::path& InDir,
                         const std::string& InScope,
                         const std::string& InKey) -> ConfigLookup {
    std::vector<std::string> args = {"-C", InDir.generic_string(), "config", InScope, "--path", "--get", InKey};
    const auto out = shell::ExecuteCommand("git", args, shell::ExecMode::Capture);
    ConfigLookup result;
    result.scope = InScope;
    if (out.exitCode == 0) {
        result.value = Trim(out.stdoutStr);
    }
    return result;
}

auto ReadGitConfigEffective(const std::filesystem::path& InDir,
                            const std::string& InKey) -> EffectiveLookup {
    std::vector<std::string> args = {"-C", InDir.generic_string(), "config", "--show-origin", "--path", "--get", InKey};
    const auto out = shell::ExecuteCommand("git", args, shell::ExecMode::Capture);
    EffectiveLookup result;
    if (out.exitCode != 0) {
        return result;
    }

    auto line = Trim(out.stdoutStr);
    const auto tabPos = line.find('\t');
    if (tabPos == std::string::npos) {
        result.value = line;
        return result;
    }

    auto origin = line.substr(0, tabPos);
    if (origin.rfind("file:", 0) == 0) {
        origin = origin.substr(5);
    }
    result.origin = origin;
    result.value = Trim(line.substr(tabPos + 1));
    return result;
}

auto ResolveDirValue(const std::filesystem::path& InBase, const std::string& InValue) -> std::filesystem::path {
    const auto value = Trim(InValue);
    if (value.empty()) {
        return {};
    }
    std::filesystem::path path(value);
    if (path.is_absolute()) {
        return path.lexically_normal();
    }
    return (InBase / path).lexically_normal();
}

auto DefaultGlobalCacheRoot() -> std::filesystem::path {
    const auto home = HomeDirectory();
    if (home.empty()) {
        return {};
    }
    return (home / ".kano" / "cache" / "git").lexically_normal();
}

auto DefaultLocalCacheRoot(const std::filesystem::path& InDir) -> std::filesystem::path {
    return (InDir / ".kano" / "cache" / "git").lexically_normal();
}

struct CacheSetting {
    std::string key;
    ConfigLookup system;
    ConfigLookup global;
    ConfigLookup local;
    EffectiveLookup effective;
    std::filesystem::path defaultPath;
    std::filesystem::path effectivePath;
};

struct GitLockInfo {
    std::filesystem::path repoPath;
    std::filesystem::path lockPath;
    bool exists = false;
    bool activeGitProcessDetected = false;
    long long ageSeconds = -1;
    bool staleCandidate = false;
};

auto BuildCacheSetting(const std::filesystem::path& InDir,
                       const std::string& InKey,
                       const std::filesystem::path& InDefault,
                       bool InHasLocalScope) -> CacheSetting {
    CacheSetting setting;
    setting.key = InKey;
    setting.defaultPath = InDefault;
    setting.system = ReadGitConfigScoped(InDir, "--system", InKey);
    setting.global = ReadGitConfigScoped(InDir, "--global", InKey);
    if (InHasLocalScope) {
        setting.local = ReadGitConfigScoped(InDir, "--local", InKey);
    } else {
        setting.local.scope = "--local";
    }
    setting.effective = ReadGitConfigEffective(InDir, InKey);

    if (!setting.effective.value.empty()) {
        setting.effectivePath = ResolveDirValue(InDir, setting.effective.value);
    } else {
        setting.effectivePath = setting.defaultPath;
    }

    return setting;
}

void PrintSetting(const std::string& InLabel, const CacheSetting& InSetting) {
    std::cout << "\n[" << InLabel << "]\n";
    std::cout << std::left << std::setw(12) << "system" << (InSetting.system.value.empty() ? "<unset>" : InSetting.system.value) << "\n";
    std::cout << std::left << std::setw(12) << "global" << (InSetting.global.value.empty() ? "<unset>" : InSetting.global.value) << "\n";
    std::cout << std::left << std::setw(12) << "local" << (InSetting.local.value.empty() ? "<unset>" : InSetting.local.value) << "\n";

    const auto effectiveSource = InSetting.effective.origin.empty() ? "<default>" : InSetting.effective.origin;
    std::cout << std::left << std::setw(12) << "effective" << InSetting.effectivePath.generic_string() << "\n";
    std::cout << std::left << std::setw(12) << "source" << effectiveSource << "\n";
}

void RemoveIfExists(const std::filesystem::path& InPath, std::vector<std::filesystem::path>& IoRemoved) {
    std::error_code ec;
    if (!std::filesystem::exists(InPath, ec)) {
        return;
    }
    std::filesystem::remove_all(InPath, ec);
    if (!ec) {
        IoRemoved.push_back(InPath.lexically_normal());
    }
}

auto CurrentProcessId() -> long long {
#if defined(_WIN32)
    return static_cast<long long>(_getpid());
#else
    return static_cast<long long>(getpid());
#endif
}

auto DetectActiveGitProcesses() -> bool {
#if defined(_WIN32)
    const auto result = shell::ExecuteCommand(
        "powershell",
        {"-NoLogo", "-NoProfile", "-Command",
         std::format("$self={}; $p = Get-Process git -ErrorAction SilentlyContinue | Where-Object {{ $_.Id -ne $self }}; if ($null -eq $p) {{ exit 1 }} else {{ exit 0 }}",
                     CurrentProcessId())},
        shell::ExecMode::Capture,
        std::filesystem::current_path());
    return result.exitCode == 0;
#else
    const auto result = shell::ExecuteCommand(
        "ps",
        {"-axo", "pid=,comm="},
        shell::ExecMode::Capture,
        std::filesystem::current_path());
    if (result.exitCode != 0) {
        return false;
    }
    std::istringstream iss(result.stdoutStr);
    std::string line;
    while (std::getline(iss, line)) {
        line = Trim(line);
        if (line.empty()) {
            continue;
        }
        std::istringstream ls(line);
        long long pid = -1;
        std::string command;
        ls >> pid >> command;
        if (pid <= 0 || pid == CurrentProcessId()) {
            continue;
        }
        if (command == "git" || command == "kano-git") {
            return true;
        }
    }
    return false;
#endif
}

auto FileAgeSeconds(const std::filesystem::path& InPath) -> long long {
    std::error_code ec;
    const auto writeTime = std::filesystem::last_write_time(InPath, ec);
    if (ec) {
        return -1;
    }
    return std::chrono::duration_cast<std::chrono::seconds>(decltype(writeTime)::clock::now() - writeTime).count();
}

auto ResolveGitLockPath(const std::filesystem::path& InRepo) -> std::filesystem::path {
    const auto result = shell::ExecuteCommand("git",
                                              {"-C", InRepo.generic_string(), "rev-parse", "--git-path", "index.lock"},
                                              shell::ExecMode::Capture,
                                              std::filesystem::current_path());
    if (result.exitCode != 0) {
        return {};
    }
    auto path = std::filesystem::path(Trim(result.stdoutStr));
    if (path.empty()) {
        return {};
    }
    if (path.is_relative()) {
        path = std::filesystem::absolute((InRepo / path).lexically_normal());
    }
    return path.lexically_normal();
}

auto DiscoverWorkspaceReposForCache(const std::filesystem::path& InRoot, int InMaxDepth) -> std::vector<workspace::RepoRecord> {
    std::string manifestReason;
    if (const auto manifest = workspace::LoadTrustedWorkspaceManifest(InRoot, &manifestReason); manifest.has_value()) {
        return manifest->repos;
    }

    workspace::DiscoverOptions options;
    options.rootDir = InRoot;
    options.maxDepth = InMaxDepth;
    options.useCache = true;
    options.cacheTtlSeconds = 900;
    options.incremental = true;
    options.maxStaleSeconds = 86400;
    options.metadataLevel = "minimal";
    return workspace::DiscoverRepos(options).repos;
}

auto InspectWorkspaceGitLocks(const std::filesystem::path& InRoot, int InMaxDepth = 8) -> std::vector<GitLockInfo> {
    std::vector<GitLockInfo> out;
    const auto repos = DiscoverWorkspaceReposForCache(InRoot, InMaxDepth);
    const bool activeGit = DetectActiveGitProcesses();
    for (const auto& repo : repos) {
        GitLockInfo info;
        info.repoPath = repo.path;
        info.lockPath = ResolveGitLockPath(repo.path);
        if (info.lockPath.empty()) {
            continue;
        }
        std::error_code ec;
        info.exists = std::filesystem::exists(info.lockPath, ec) && !ec;
        if (!info.exists) {
            continue;
        }
        info.activeGitProcessDetected = activeGit;
        info.ageSeconds = FileAgeSeconds(info.lockPath);
        info.staleCandidate = !info.activeGitProcessDetected && info.ageSeconds >= 30;
        out.push_back(std::move(info));
    }
    return out;
}

} // namespace

void RegisterCache(CLI::App& InApp) {
    auto* cmd = InApp.add_subcommand("cache", "Show/clear kano-git cache directories");
    cmd->require_subcommand(1);

    auto* show = cmd->add_subcommand("show", "Show cache config scopes and effective paths");
    show->callback([]() {
        const auto cwd = std::filesystem::current_path();
        const bool hasLocalScope = InRepo(cwd);

        const auto globalSetting = BuildCacheSetting(
            cwd,
            "kano.cache.global-dir",
            DefaultGlobalCacheRoot(),
            hasLocalScope);

        const auto localSetting = BuildCacheSetting(
            cwd,
            "kano.cache.local-dir",
            DefaultLocalCacheRoot(cwd),
            hasLocalScope);

        std::cout << "=== kano-git cache config ===\n";
        std::cout << "cwd: " << cwd.lexically_normal().generic_string() << "\n";
        std::cout << "repo-local scope: " << (hasLocalScope ? "available" : "not in a git repo") << "\n";

        PrintSetting("global-cache", globalSetting);
        PrintSetting("local-cache", localSetting);

        std::cout << "\nPaths used by kano-git:\n";
        std::cout << "- AI model memory: " << (globalSetting.effectivePath / "ai").generic_string() << "\n";
        std::cout << "- Discover repos cache: " << (localSetting.effectivePath / "discover-repos").generic_string() << "\n";
    });

    auto* clear = cmd->add_subcommand("clear", "Clear kano-git related caches (global + local + legacy)");
    clear->callback([]() {
        const auto cwd = std::filesystem::current_path();
        const bool hasLocalScope = InRepo(cwd);

        const auto globalSetting = BuildCacheSetting(
            cwd,
            "kano.cache.global-dir",
            DefaultGlobalCacheRoot(),
            hasLocalScope);

        const auto localSetting = BuildCacheSetting(
            cwd,
            "kano.cache.local-dir",
            DefaultLocalCacheRoot(cwd),
            hasLocalScope);

        std::set<std::string> planned;
        std::vector<std::filesystem::path> removed;

        const auto addPath = [&](const std::filesystem::path& p) {
            if (!p.empty()) {
                planned.insert(p.lexically_normal().generic_string());
            }
        };

        addPath(globalSetting.effectivePath);
        addPath(localSetting.effectivePath);

        if (!HomeDirectory().empty()) {
            addPath((HomeDirectory() / ".cache" / "kano-git-master-skill").lexically_normal());
        }
        addPath((cwd / ".git" / ".kano-cache").lexically_normal());
        addPath((cwd / ".cache" / "kano-git-master-skill").lexically_normal());

        for (const auto& item : planned) {
            RemoveIfExists(std::filesystem::path(item), removed);
        }

        std::cout << "Cleared " << removed.size() << " cache path(s).\n";
        for (const auto& item : removed) {
            std::cout << "- " << item.generic_string() << "\n";
        }
    });

    auto* locks = cmd->add_subcommand("locks", "Inspect or cleanup stale cache/git locks");
    auto* locksCleanup = new bool{false};
    auto* includeGitLocks = new bool{false};
    auto* minAgeSeconds = new int{30};
    locks->add_flag("--cleanup", *locksCleanup, "Remove stale cache locks (and optional git index.lock files)");
    locks->add_flag("--include-git-locks", *includeGitLocks, "Also inspect/cleanup stale git index.lock files under discovered workspace repos");
    locks->add_option("--min-age-seconds", *minAgeSeconds, "Minimum lock age to treat as stale")->default_val(30);
    locks->callback([=]() {
        const auto cwd = std::filesystem::current_path();
        const bool hasLocalScope = InRepo(cwd);
        const auto globalSetting = BuildCacheSetting(
            cwd,
            "kano.cache.global-dir",
            DefaultGlobalCacheRoot(),
            hasLocalScope);
        const auto localSetting = BuildCacheSetting(
            cwd,
            "kano.cache.local-dir",
            DefaultLocalCacheRoot(cwd),
            hasLocalScope);

        std::vector<workspace::CacheLockInfo> cacheLocks;
        for (const auto& root : {globalSetting.effectivePath, localSetting.effectivePath}) {
            if (root.empty()) {
                continue;
            }
            auto locksForRoot = *locksCleanup
                                    ? workspace::CleanupStaleCacheLocks(root, *minAgeSeconds)
                                    : workspace::InspectCacheLocks(root);
            cacheLocks.insert(cacheLocks.end(), locksForRoot.begin(), locksForRoot.end());
        }

        std::cout << "Cache locks: " << cacheLocks.size() << "\n";
        for (const auto& lock : cacheLocks) {
            std::cout << "- lock=" << lock.lockPath.generic_string()
                      << " target=" << lock.targetPath.generic_string()
                      << " pid=" << lock.ownerPid
                      << " age=" << lock.ageSeconds << "s"
                      << " active=" << (lock.activeProcessDetected ? "yes" : "no")
                      << " stale=" << (lock.staleCandidate ? "yes" : "no");
            if (*locksCleanup && lock.staleCandidate) {
                std::cout << " removed=" << (lock.exists ? "no" : "yes");
            }
            std::cout << "\n";
        }

        if (!*includeGitLocks) {
            return;
        }

        const auto gitLocks = InspectWorkspaceGitLocks(cwd);
        std::cout << "Git index locks: " << gitLocks.size() << "\n";
        for (const auto& lock : gitLocks) {
            bool removed = false;
            if (locksCleanup && lock.staleCandidate) {
                std::error_code ec;
                removed = std::filesystem::remove(lock.lockPath, ec) && !ec;
            }
            std::cout << "- repo=" << lock.repoPath.generic_string()
                      << " lock=" << lock.lockPath.generic_string()
                      << " age=" << lock.ageSeconds << "s"
                      << " active=" << (lock.activeGitProcessDetected ? "yes" : "no")
                      << " stale=" << (lock.staleCandidate ? "yes" : "no");
            if (*locksCleanup && lock.staleCandidate) {
                std::cout << " removed=" << (removed ? "yes" : "no");
            }
            std::cout << "\n";
        }
    });

}

} // namespace kano::git::commands
