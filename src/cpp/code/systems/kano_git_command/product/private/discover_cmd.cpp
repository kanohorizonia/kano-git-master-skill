// discover command — repository discovery and workspace manifest refresh

#include <CLI/CLI.hpp>
#include "discovery.hpp"
#include "terminal_color.hpp"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

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

auto FitCell(const std::string& InValue, std::size_t InWidth) -> std::string {
    if (InWidth == 0) {
        return {};
    }
    if (InValue.size() >= InWidth) {
        if (InWidth <= 3) {
            return InValue.substr(0, InWidth);
        }
        return InValue.substr(0, InWidth - 3) + "...";
    }
    return InValue + std::string(InWidth - InValue.size(), ' ');
}

auto TruncateCell(const std::string& InValue, std::size_t InWidth) -> std::string {
    if (InWidth == 0) {
        return {};
    }
    if (InValue.size() <= InWidth) {
        return InValue;
    }
    if (InWidth <= 3) {
        return InValue.substr(0, InWidth);
    }
    return InValue.substr(0, InWidth - 3) + "...";
}

auto FormatNativeStatusJson(const std::vector<workspace::RepoRecord>& InRepos) -> std::string;
auto FormatNativeStatusTable(const std::vector<workspace::RepoRecord>& InRepos, const std::filesystem::path& InRoot) -> std::string;

auto FormatHumanDuration(std::chrono::milliseconds InDuration) -> std::string {
    auto remaining = InDuration.count();
    const long long hours = remaining / (60LL * 60LL * 1000LL);
    remaining %= (60LL * 60LL * 1000LL);
    const long long minutes = remaining / (60LL * 1000LL);
    remaining %= (60LL * 1000LL);
    const long long seconds = remaining / 1000LL;
    remaining %= 1000LL;
    const long long milliseconds = remaining;

    std::ostringstream oss;
    bool wrote = false;
    auto append = [&](long long value, const char* suffix) {
        const bool isMilliseconds = std::string(suffix) == "ms";
        if (value == 0 && !isMilliseconds) {
            return;
        }
        if (wrote) {
            oss << ' ';
        }
        oss << value << suffix;
        wrote = true;
    };

    append(hours, "h");
    append(minutes, "m");
    append(seconds, "s");
    append(milliseconds, "ms");
    return oss.str();
}

void RunDiscoverCommand(const std::string& InFormat,
                        const std::string& InRoot,
                        int InMaxDepth,
                        int InUnregisteredDepth,
                        const std::vector<std::string>& InExclude,
                        bool InNoCache,
                        bool InNoRefresh,
                        bool InNoIncremental,
                        int InCacheTtl,
                        int InMaxStale,
                        const std::string& InMetadata,
                        workspace::DiscoverScope InScope,
                        bool InNoUnregisteredScan) {
    if (InFormat != "table" && InFormat != "json") {
        std::cerr << "Error: invalid --format value: " << InFormat << " (expected table|json)\n";
        std::exit(1);
    }

    workspace::DiscoverOptions options;
    options.rootDir = InRoot.empty() ? std::filesystem::current_path() : std::filesystem::path(InRoot);
    options.maxDepth = InScope == workspace::DiscoverScope::Full ? InUnregisteredDepth : InMaxDepth;
    options.excludePatterns = InExclude;
    options.useCache = !InNoCache;
    options.cacheTtlSeconds = InCacheTtl;
    options.refreshCache = !InNoRefresh;
    options.incremental = !InNoIncremental;
    options.maxStaleSeconds = InMaxStale;
    options.metadataLevel = InMetadata;
    options.scope = InNoUnregisteredScan ? workspace::DiscoverScope::RegisteredOnly : InScope;
    options.includeTrustedUnregistered = true;
    options.progressCallback = [](const std::string& InMessage) {
        std::cerr << kano::terminal::Wrap("[discover]", kano::terminal::Color::Dim) << " " << InMessage << "\n";
    };

    std::cerr << kano::terminal::Wrap("[discover]", kano::terminal::Color::Dim) << " " 
              << kano::terminal::Wrap("start", kano::terminal::Color::BoldWhite) << " root=" << options.rootDir.lexically_normal().generic_string()
              << " cache=" << (options.useCache ? kano::terminal::Wrap("on", kano::terminal::Color::BoldGreen) : kano::terminal::Wrap("off", kano::terminal::Color::BoldRed))
              << " refresh=" << (options.refreshCache ? kano::terminal::Wrap("on", kano::terminal::Color::BoldGreen) : kano::terminal::Wrap("off", kano::terminal::Color::BoldRed))
              << " metadata=" << kano::terminal::Wrap(options.metadataLevel, kano::terminal::Color::BoldWhite)
              << " scope=" << (options.scope == workspace::DiscoverScope::Full ? kano::terminal::Wrap("full", kano::terminal::Color::BoldYellow) : kano::terminal::Wrap("registered-only", kano::terminal::Color::BoldBlue))
              << (options.scope == workspace::DiscoverScope::Full ? std::string(" depth=") + std::to_string(options.maxDepth) : std::string(" recursive=registered"))
              << "\n";

    const auto totalStart = std::chrono::steady_clock::now();
    const auto discoverStart = totalStart;
    auto discovery = workspace::DiscoverRepos(options);
    const auto discoverEnd = std::chrono::steady_clock::now();

    auto repos = discovery.repos;
    std::sort(repos.begin(), repos.end(), [](const workspace::RepoRecord& A, const workspace::RepoRecord& B) {
        return A.path.lexically_normal().generic_string() < B.path.lexically_normal().generic_string();
    });

    const auto manifestStart = std::chrono::steady_clock::now();
    const auto manifest = workspace::BuildWorkspaceManifest(options.rootDir, repos);
    std::cerr << kano::terminal::Wrap("[discover]", kano::terminal::Color::Dim) << " writing workspace manifest -> "
              << kano::terminal::Wrap(manifest.manifestFile.lexically_normal().generic_string(), kano::terminal::Color::BoldCyan) << "\n";
    if (!workspace::SaveWorkspaceManifest(manifest)) {
        std::cerr << kano::terminal::Wrap("Error:", kano::terminal::Color::BoldRed) << " failed to write workspace manifest: "
                  << manifest.manifestFile.lexically_normal().generic_string() << "\n";
        std::exit(1);
    }
    const auto manifestEnd = std::chrono::steady_clock::now();
    const auto totalEnd = manifestEnd;

    const auto discoverElapsed = std::chrono::duration_cast<std::chrono::milliseconds>(discoverEnd - discoverStart);
    const auto manifestElapsed = std::chrono::duration_cast<std::chrono::milliseconds>(manifestEnd - manifestStart);
    const auto totalElapsed = std::chrono::duration_cast<std::chrono::milliseconds>(totalEnd - totalStart);
    const auto discoverElapsedText = FormatHumanDuration(discoverElapsed);
    const auto manifestElapsedText = FormatHumanDuration(manifestElapsed);
    const auto totalElapsedText = FormatHumanDuration(totalElapsed);
    std::cerr << kano::terminal::Wrap("[discover]", kano::terminal::Color::Dim) << " " 
              << kano::terminal::Wrap("complete", kano::terminal::Color::BoldGreen) << " mode=" << discovery.mode << " repos=" << repos.size()
              << " elapsed=" << kano::terminal::Wrap(totalElapsedText, kano::terminal::Color::BoldWhite) << "\n";

    std::cout << kano::terminal::Wrap("Discovery mode: ", kano::terminal::Color::BoldWhite) << discovery.mode << "\n";
    std::cout << kano::terminal::Wrap("Discovery scope: ", kano::terminal::Color::BoldWhite) << (options.scope == workspace::DiscoverScope::Full ? "full" : "registered-only") << "\n";
    std::cout << kano::terminal::Wrap("Discovery cache: ", kano::terminal::Color::BoldWhite) << kano::terminal::Wrap(discovery.cacheFile.lexically_normal().generic_string(), kano::terminal::Color::BoldCyan) << "\n";
    std::cout << kano::terminal::Wrap("Workspace manifest: ", kano::terminal::Color::BoldWhite) << kano::terminal::Wrap(manifest.manifestFile.lexically_normal().generic_string(), kano::terminal::Color::BoldCyan) << "\n";
    std::cout << kano::terminal::Wrap("Repos discovered: ", kano::terminal::Color::BoldWhite) << kano::terminal::Wrap(std::to_string(repos.size()), kano::terminal::Color::BoldGreen) << "\n";
    std::cout << kano::terminal::Wrap("Discovery elapsed: ", kano::terminal::Color::BoldWhite) << discoverElapsedText << "\n";
    std::cout << kano::terminal::Wrap("Manifest write elapsed: ", kano::terminal::Color::BoldWhite) << manifestElapsedText << "\n";
    std::cout << kano::terminal::Wrap("Total elapsed: ", kano::terminal::Color::BoldWhite) << kano::terminal::Wrap(totalElapsedText, kano::terminal::Color::BoldGreen) << "\n\n";

    if (InFormat == "json") {
        std::cout << FormatNativeStatusJson(repos) << "\n";
    } else {
        std::cout << FormatNativeStatusTable(repos, options.rootDir) << "\n";
    }
    std::exit(0);
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
        out += "\"registrationRelativeTo\":\"" + EscapeJson(repo.registrationRelativeTo.lexically_normal().generic_string()) + "\",";
        out += "\"kogSync\":\"" + EscapeJson(repo.kogSyncPolicy) + "\",";
        out += "\"kogCommit\":\"" + EscapeJson(repo.kogCommitPolicy) + "\",";
        out += "\"kogPush\":\"" + EscapeJson(repo.kogPushPolicy) + "\",";
        out += "\"kogHygiene\":\"" + EscapeJson(repo.kogHygienePolicy) + "\",";
        out += "\"branch\":\"" + EscapeJson(repo.currentBranch.empty() ? "(detached)" : repo.currentBranch) + "\",";
        out += "\"dirty\":";
        out += repo.hasChanges ? "true" : "false";
        out += "}";
    }
    out += "]}";
    return out;
}

auto FormatNativeStatusTable(const std::vector<workspace::RepoRecord>& InRepos, const std::filesystem::path& InRoot) -> std::string {
    std::ostringstream oss;
    constexpr std::size_t kIndexWidth = 4;
    constexpr std::size_t kRepoWidth = 26;
    constexpr std::size_t kBranchWidth = 20;
    constexpr std::size_t kTypeWidth = 14;
    constexpr std::size_t kDirtyWidth = 8;
    constexpr const char* kColSep = "  ";
    std::map<std::string, std::vector<std::size_t>> groupedRepoIndexes;
    for (std::size_t i = 0; i < InRepos.size(); ++i) {
        const auto relativePath = RelativeDisplayPath(InRoot, InRepos[i].path);
        groupedRepoIndexes[GroupFromRelativePath(relativePath)].push_back(i);
    }

    if (!InRepos.empty()) {
        oss << kano::terminal::Wrap(
            FitCell("#", kIndexWidth) + kColSep +
            FitCell("REPO", kRepoWidth) + kColSep +
            FitCell("BRANCH", kBranchWidth) + kColSep +
            FitCell("TYPE", kTypeWidth) + kColSep +
            FitCell("DIRTY", kDirtyWidth),
            kano::terminal::Color::Dim)
            << "\n";
    }

    std::size_t globalIndex = 0;
    for (const auto& [group, indexes] : groupedRepoIndexes) {
        oss << kano::terminal::Wrap("GROUP: " + group, kano::terminal::Color::BoldYellow) << "\n";
        for (const auto repoIdx : indexes) {
            const auto& repo = InRepos[repoIdx];
            globalIndex += 1;

            auto repoName = RepoNameFromPath(repo.path);

            auto branch = repo.currentBranch.empty() ? "-" : repo.currentBranch;

            const std::string indexRaw = std::to_string(globalIndex);
            const std::string repoRaw = TruncateCell(repoName, kRepoWidth);
            const std::string branchRaw = TruncateCell(branch, kBranchWidth);
            const std::string typeRaw = TruncateCell(repo.type, kTypeWidth);
            const std::string dirtyRaw = TruncateCell(repo.hasChanges ? "yes" : "no", kDirtyWidth);

            const auto savedFlags = oss.flags();
            oss << std::left;
            oss << kano::terminal::Wrap(indexRaw, kano::terminal::Color::Dim);
            if (indexRaw.size() < kIndexWidth) {
                oss << std::string(kIndexWidth - indexRaw.size(), ' ');
            }
            oss << kColSep;
            oss << kano::terminal::Wrap(repoRaw, kano::terminal::Color::BoldCyan);
            if (repoRaw.size() < kRepoWidth) {
                oss << std::string(kRepoWidth - repoRaw.size(), ' ');
            }
            oss << kColSep;
            oss << kano::terminal::Wrap(branchRaw, kano::terminal::Color::BoldGreen);
            if (branchRaw.size() < kBranchWidth) {
                oss << std::string(kBranchWidth - branchRaw.size(), ' ');
            }
            oss << kColSep;
            oss << typeRaw;
            if (typeRaw.size() < kTypeWidth) {
                oss << std::string(kTypeWidth - typeRaw.size(), ' ');
            }
            oss << kColSep;
            oss << kano::terminal::Wrap(dirtyRaw, repo.hasChanges ? kano::terminal::Color::BoldRed : kano::terminal::Color::BoldGreen);
            if (dirtyRaw.size() < kDirtyWidth) {
                oss << std::string(kDirtyWidth - dirtyRaw.size(), ' ');
            }
            oss << "\n";
            oss.flags(savedFlags);
        }
        oss << "\n";
    }

    if (InRepos.empty()) {
        oss << "(no repositories discovered)\n";
    }

    return oss.str();
}

} // namespace

void RegisterDiscover(CLI::App& InApp) {
    auto* cmd = InApp.add_subcommand("discover", "Discover repositories and refresh workspace manifest");
    auto* full = cmd->add_subcommand("full", "Discover repositories with full filesystem scan (includes unregistered repos)");
    auto* discoverFormat = new std::string{"table"};
    auto* discoverRoot = new std::string{"."};
    auto* discoverMaxDepth = new int{8};
    auto* discoverUnregisteredDepth = new int{2};
    auto* discoverExclude = new std::vector<std::string>{};
    auto* discoverNoCache = new bool{false};
    auto* discoverNoRefresh = new bool{false};
    auto* discoverNoIncremental = new bool{false};
    auto* discoverFull = new bool{false};
    auto* discoverNoUnregisteredScan = new bool{false};
    auto* discoverCacheTtl = new int{60};
    auto* discoverMaxStale = new int{900};
    auto* discoverMetadata = new std::string{"full"};

    cmd->add_option("--format", *discoverFormat, "Output format: table|json")->default_str("table");
    cmd->add_option("--repo-root", *discoverRoot, "Repository root/start path");
    cmd->add_flag("--full", *discoverFull, "Include bounded unregistered filesystem probing");
    cmd->add_option("--unregistered-depth", *discoverUnregisteredDepth, "Bounded unregistered scan depth")->default_str("2");
    cmd->add_flag("--no-unregistered-scan", *discoverNoUnregisteredScan, "Disable new unregistered filesystem probing while keeping trusted manifest/cache repos");
    cmd->add_option("--exclude", *discoverExclude, "Temporary scan-scope exclude override for this invocation only (repeatable; prefer .gitignore/.kogignore for shared policy)");
    cmd->add_flag("--no-cache", *discoverNoCache, "Disable discovery cache for this run");
    cmd->add_flag("--no-refresh-cache", *discoverNoRefresh, "Do not force cache refresh");
    cmd->add_flag("--no-incremental", *discoverNoIncremental, "Disable incremental cache validation");
    cmd->add_option("--cache-ttl", *discoverCacheTtl, "Cache TTL seconds");
    cmd->add_option("--max-stale", *discoverMaxStale, "Incremental max stale seconds");
    cmd->add_option("--metadata-level", *discoverMetadata, "Metadata level: full|minimal");
    full->add_option("--format", *discoverFormat, "Output format: table|json")->default_str("table");
    full->add_option("--repo-root", *discoverRoot, "Repository root/start path");
    full->add_option("--max-depth", *discoverMaxDepth, "Full discovery max depth");
    full->add_option("--unregistered-depth", *discoverUnregisteredDepth, "Bounded unregistered scan depth")->default_str("2");
    full->add_flag("--no-unregistered-scan", *discoverNoUnregisteredScan, "Disable new unregistered filesystem probing while keeping trusted manifest/cache repos");
    full->add_option("--exclude", *discoverExclude, "Temporary scan-scope exclude override for this invocation only (repeatable; prefer .gitignore/.kogignore for shared policy)");
    full->add_flag("--no-cache", *discoverNoCache, "Disable discovery cache for this run");
    full->add_flag("--no-refresh-cache", *discoverNoRefresh, "Do not force cache refresh");
    full->add_flag("--no-incremental", *discoverNoIncremental, "Disable incremental cache validation");
    full->add_option("--cache-ttl", *discoverCacheTtl, "Cache TTL seconds");
    full->add_option("--max-stale", *discoverMaxStale, "Incremental max stale seconds");
    full->add_option("--metadata-level", *discoverMetadata, "Metadata level: full|minimal");

    cmd->callback([=]() {
        RunDiscoverCommand(
            *discoverFormat,
            *discoverRoot,
            *discoverMaxDepth,
            *discoverUnregisteredDepth,
            *discoverExclude,
            *discoverNoCache,
            *discoverNoRefresh,
            *discoverNoIncremental,
            *discoverCacheTtl,
            *discoverMaxStale,
            *discoverMetadata,
            *discoverFull ? workspace::DiscoverScope::Full : workspace::DiscoverScope::RegisteredOnly,
            *discoverNoUnregisteredScan);
    });

    full->callback([=]() {
        RunDiscoverCommand(
            *discoverFormat,
            *discoverRoot,
            *discoverMaxDepth,
            *discoverUnregisteredDepth,
            *discoverExclude,
            *discoverNoCache,
            *discoverNoRefresh,
            *discoverNoIncremental,
            *discoverCacheTtl,
            *discoverMaxStale,
            *discoverMetadata,
            workspace::DiscoverScope::Full,
            *discoverNoUnregisteredScan);
    });
}

void RegisterWorkspace(CLI::App& InApp) {
    auto* workspace = InApp.add_subcommand("workspace", "Workspace operations");
    auto* cmd = workspace->add_subcommand("discover", "Discover repositories and refresh workspace manifest");
    auto* full = cmd->add_subcommand("full", "Discover repositories with full filesystem scan (includes unregistered repos)");
    auto* discoverFormat = new std::string{"table"};
    auto* discoverRoot = new std::string{"."};
    auto* discoverMaxDepth = new int{8};
    auto* discoverUnregisteredDepth = new int{2};
    auto* discoverExclude = new std::vector<std::string>{};
    auto* discoverNoCache = new bool{false};
    auto* discoverNoRefresh = new bool{false};
    auto* discoverNoIncremental = new bool{false};
    auto* discoverFull = new bool{false};
    auto* discoverNoUnregisteredScan = new bool{false};
    auto* discoverCacheTtl = new int{60};
    auto* discoverMaxStale = new int{900};
    auto* discoverMetadata = new std::string{"full"};

    cmd->add_option("--format", *discoverFormat, "Output format: table|json")->default_str("table");
    cmd->add_option("--repo-root", *discoverRoot, "Repository root/start path");
    cmd->add_flag("--full", *discoverFull, "Include bounded unregistered filesystem probing");
    cmd->add_option("--unregistered-depth", *discoverUnregisteredDepth, "Bounded unregistered scan depth")->default_str("2");
    cmd->add_flag("--no-unregistered-scan", *discoverNoUnregisteredScan, "Disable new unregistered filesystem probing while keeping trusted manifest/cache repos");
    cmd->add_option("--exclude", *discoverExclude, "Temporary scan-scope exclude override for this invocation only (repeatable; prefer .gitignore/.kogignore for shared policy)");
    cmd->add_flag("--no-cache", *discoverNoCache, "Disable discovery cache for this run");
    cmd->add_flag("--no-refresh-cache", *discoverNoRefresh, "Do not force cache refresh");
    cmd->add_flag("--no-incremental", *discoverNoIncremental, "Disable incremental cache validation");
    cmd->add_option("--cache-ttl", *discoverCacheTtl, "Cache TTL seconds");
    cmd->add_option("--max-stale", *discoverMaxStale, "Incremental max stale seconds");
    cmd->add_option("--metadata-level", *discoverMetadata, "Metadata level: full|minimal");
    full->add_option("--format", *discoverFormat, "Output format: table|json")->default_str("table");
    full->add_option("--repo-root", *discoverRoot, "Repository root/start path");
    full->add_option("--max-depth", *discoverMaxDepth, "Full discovery max depth");
    full->add_option("--unregistered-depth", *discoverUnregisteredDepth, "Bounded unregistered scan depth")->default_str("2");
    full->add_flag("--no-unregistered-scan", *discoverNoUnregisteredScan, "Disable new unregistered filesystem probing while keeping trusted manifest/cache repos");
    full->add_option("--exclude", *discoverExclude, "Temporary scan-scope exclude override for this invocation only (repeatable; prefer .gitignore/.kogignore for shared policy)");
    full->add_flag("--no-cache", *discoverNoCache, "Disable discovery cache for this run");
    full->add_flag("--no-refresh-cache", *discoverNoRefresh, "Do not force cache refresh");
    full->add_flag("--no-incremental", *discoverNoIncremental, "Disable incremental cache validation");
    full->add_option("--cache-ttl", *discoverCacheTtl, "Cache TTL seconds");
    full->add_option("--max-stale", *discoverMaxStale, "Incremental max stale seconds");
    full->add_option("--metadata-level", *discoverMetadata, "Metadata level: full|minimal");

    cmd->callback([=]() {
        RunDiscoverCommand(
            *discoverFormat,
            *discoverRoot,
            *discoverMaxDepth,
            *discoverUnregisteredDepth,
            *discoverExclude,
            *discoverNoCache,
            *discoverNoRefresh,
            *discoverNoIncremental,
            *discoverCacheTtl,
            *discoverMaxStale,
            *discoverMetadata,
            *discoverFull ? workspace::DiscoverScope::Full : workspace::DiscoverScope::RegisteredOnly,
            *discoverNoUnregisteredScan);
    });

    full->callback([=]() {
        RunDiscoverCommand(
            *discoverFormat,
            *discoverRoot,
            *discoverMaxDepth,
            *discoverUnregisteredDepth,
            *discoverExclude,
            *discoverNoCache,
            *discoverNoRefresh,
            *discoverNoIncremental,
            *discoverCacheTtl,
            *discoverMaxStale,
            *discoverMetadata,
            workspace::DiscoverScope::Full,
            *discoverNoUnregisteredScan);
    });
}

} // namespace kano::git::commands
