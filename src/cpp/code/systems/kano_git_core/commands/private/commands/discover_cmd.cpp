// discover command — repository discovery and workspace manifest refresh

#include "command_registry.hpp"
#include "discovery.hpp"

#include <algorithm>
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

auto FormatNativeStatusTable(const std::vector<workspace::RepoRecord>& InRepos, const std::filesystem::path& InRoot) -> std::string {
    std::ostringstream oss;
    std::map<std::string, std::vector<std::size_t>> groupedRepoIndexes;
    for (std::size_t i = 0; i < InRepos.size(); ++i) {
        const auto relativePath = RelativeDisplayPath(InRoot, InRepos[i].path);
        groupedRepoIndexes[GroupFromRelativePath(relativePath)].push_back(i);
    }

    if (!InRepos.empty()) {
        oss << std::left
            << std::setw(6) << "#"
            << std::setw(26) << "REPO"
            << std::setw(20) << "BRANCH"
            << std::setw(14) << "TYPE"
            << std::setw(8) << "DIRTY"
            << "\n";
    }

    std::size_t globalIndex = 0;
    for (const auto& [group, indexes] : groupedRepoIndexes) {
        oss << "GROUP: " << group << "\n";
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

} // namespace

void RegisterDiscover(CLI::App& InApp) {
    auto* cmd = InApp.add_subcommand("discover", "Discover repositories and refresh workspace manifest");
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

    cmd->add_option("--format", *discoverFormat, "Output format: table|json")->default_str("table");
    cmd->add_option("--repo-root", *discoverRoot, "Repository root/start path");
    cmd->add_option("--max-depth", *discoverMaxDepth, "Discovery max depth");
    cmd->add_option("--exclude", *discoverExclude, "Temporary scan-scope exclude override for this invocation only (repeatable; prefer .gitignore/.kogignore for shared policy)");
    cmd->add_flag("--no-cache", *discoverNoCache, "Disable discovery cache for this run");
    cmd->add_flag("--no-refresh-cache", *discoverNoRefresh, "Do not force cache refresh");
    cmd->add_flag("--no-incremental", *discoverNoIncremental, "Disable incremental cache validation");
    cmd->add_option("--cache-ttl", *discoverCacheTtl, "Cache TTL seconds");
    cmd->add_option("--max-stale", *discoverMaxStale, "Incremental max stale seconds");
    cmd->add_option("--metadata-level", *discoverMetadata, "Metadata level: full|minimal");

    cmd->callback([=]() {
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
        const auto manifest = workspace::BuildWorkspaceManifest(options.rootDir, repos);
        if (!workspace::SaveWorkspaceManifest(manifest)) {
            std::cerr << "Error: failed to write workspace manifest: "
                      << manifest.manifestFile.lexically_normal().generic_string() << "\n";
            std::exit(1);
        }

        std::cout << "Discovery mode: " << discovery.mode << "\n";
        std::cout << "Discovery cache: " << discovery.cacheFile.lexically_normal().generic_string() << "\n";
        std::cout << "Workspace manifest: " << manifest.manifestFile.lexically_normal().generic_string() << "\n";
        std::cout << "Repos discovered: " << repos.size() << "\n\n";

        if (*discoverFormat == "json") {
            std::cout << FormatNativeStatusJson(repos) << "\n";
        } else {
            std::cout << FormatNativeStatusTable(repos, options.rootDir) << "\n";
        }
        std::exit(0);
    });
}

} // namespace kano::git::commands
