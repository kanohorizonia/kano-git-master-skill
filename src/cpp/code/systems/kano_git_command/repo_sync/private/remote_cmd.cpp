// remote command — show git remotes and URLs

#include <CLI/CLI.hpp>
#include "discovery.hpp"
#include "shell_executor.hpp"

#include <algorithm>
#include <filesystem>
#include <format>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace kano::git::commands {
namespace {

struct RemoteRow {
    std::string name;
    std::string fetchUrl;
    std::string pushUrl;
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

auto SplitLines(const std::string& InText) -> std::vector<std::string> {
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

auto GitCapture(const std::filesystem::path& InRepo, const std::vector<std::string>& InArgs) -> shell::ExecResult {
    return shell::ExecuteCommand("git", InArgs, shell::ExecMode::Capture, InRepo);
}

auto CurrentBranch(const std::filesystem::path& InRepo) -> std::string {
    const auto out = GitCapture(InRepo, {"symbolic-ref", "--short", "HEAD"});
    if (out.exitCode != 0) {
        return "(detached)";
    }
    const auto value = Trim(out.stdoutStr);
    return value.empty() ? "(detached)" : value;
}

auto CurrentRemoteRef(const std::filesystem::path& InRepo) -> std::string {
    const auto out = GitCapture(InRepo, {"rev-parse", "--abbrev-ref", "--symbolic-full-name", "@{upstream}"});
    if (out.exitCode != 0) {
        return "(none)";
    }
    const auto value = Trim(out.stdoutStr);
    return value.empty() ? "(none)" : value;
}

auto DiscoverRemotes(const std::filesystem::path& InRepo) -> std::vector<RemoteRow> {
    std::vector<RemoteRow> rows;

    const auto remotesOut = GitCapture(InRepo, {"remote"});
    if (remotesOut.exitCode != 0) {
        return rows;
    }

    for (const auto& name : SplitLines(remotesOut.stdoutStr)) {
        const auto fetch = GitCapture(InRepo, {"remote", "get-url", name});
        const auto push = GitCapture(InRepo, {"remote", "get-url", "--push", name});

        RemoteRow row;
        row.name = name;
        row.fetchUrl = fetch.exitCode == 0 ? Trim(fetch.stdoutStr) : "(none)";
        row.pushUrl = push.exitCode == 0 ? Trim(push.stdoutStr) : row.fetchUrl;
        rows.push_back(std::move(row));
    }

    std::sort(rows.begin(), rows.end(), [](const RemoteRow& A, const RemoteRow& B) {
        return A.name < B.name;
    });
    return rows;
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

auto FormatTable(const std::filesystem::path& InRepo,
                 const std::string& InBranch,
                 const std::string& InRemoteRef,
                 const std::vector<RemoteRow>& InRows,
                 std::size_t InRepoIndex,
                 std::size_t InRepoCount,
                 const std::string& InGroup,
                 const std::string& InRepoName) -> std::string {
    std::ostringstream oss;
    oss << "[" << InRepoIndex << "/" << InRepoCount << "] REPO: " << InRepoName << "\n";
    oss << "GROUP : " << InGroup << "\n";
    oss << "BRANCH: " << InBranch << "\n";
    oss << "REMOTE: " << InRemoteRef << "\n\n";

    oss << "Remotes: " << InRows.size() << "\n";

    for (std::size_t i = 0; i < InRows.size(); ++i) {
        const auto& row = InRows[i];
        oss << "  [" << (i + 1) << "] " << row.name << "\n";
        oss << "      fetch: " << row.fetchUrl << "\n";
        oss << "      push : " << row.pushUrl << "\n";
    }

    if (InRows.empty()) {
        oss << "  (no remotes)\n";
    }

    return oss.str();
}

auto FormatWorkspaceTable(const std::vector<std::filesystem::path>& InRepos,
                         const std::vector<std::string>& InBranches,
                         const std::vector<std::string>& InRemoteRefs,
                         const std::vector<std::vector<RemoteRow>>& InRows,
                         const std::filesystem::path& InRoot) -> std::string {
    std::ostringstream out;
    std::map<std::string, std::vector<std::size_t>> groupedRepoIndexes;
    for (std::size_t i = 0; i < InRepos.size(); ++i) {
        const auto relativePath = RelativeDisplayPath(InRoot, InRepos[i]);
        groupedRepoIndexes[GroupFromRelativePath(relativePath)].push_back(i);
    }

    out << "SUMMARY: repos=" << InRepos.size() << ", groups=" << groupedRepoIndexes.size() << "\n";
    std::size_t globalIndex = 0;
    for (const auto& [group, indexes] : groupedRepoIndexes) {
        out << "\nGROUP: " << group << " (repos=" << indexes.size() << ")\n\n";
        for (const auto repoIdx : indexes) {
            globalIndex += 1;
            if (globalIndex > 1) {
                out << "\n";
            }
            out << FormatTable(
                InRepos[repoIdx],
                InBranches[repoIdx],
                InRemoteRefs[repoIdx],
                InRows[repoIdx],
                globalIndex,
                InRepos.size(),
                group,
                RepoNameFromPath(InRepos[repoIdx]));
        }
    }
    if (InRepos.empty()) {
        out << "(no repositories discovered)\n";
    }
    return out.str();
}

auto FormatJson(const std::filesystem::path& InRepo, const std::string& InBranch, const std::string& InRemoteRef, const std::vector<RemoteRow>& InRows) -> std::string {
    std::ostringstream out;
    out << "{";
    out << std::format("\"repo\":\"{}\",", InRepo.lexically_normal().generic_string());
    out << std::format("\"repo_name\":\"{}\",", RepoNameFromPath(InRepo));
    out << std::format("\"branch\":\"{}\",", InBranch);
    out << std::format("\"remote\":\"{}\",", InRemoteRef);
    out << "\"remotes\":[";
    for (std::size_t i = 0; i < InRows.size(); ++i) {
        if (i > 0) {
            out << ",";
        }
        const auto& row = InRows[i];
        out << "{";
        out << std::format("\"index\":{},", i + 1);
        out << std::format("\"name\":\"{}\",", row.name);
        out << std::format("\"fetch_url\":\"{}\",", row.fetchUrl);
        out << std::format("\"push_url\":\"{}\"", row.pushUrl);
        out << "}";
    }
    out << "]}";
    return out.str();
}

auto FormatWorkspaceJson(const std::vector<std::filesystem::path>& InRepos,
                        const std::vector<std::string>& InBranches,
                        const std::vector<std::string>& InRemoteRefs,
                        const std::vector<std::vector<RemoteRow>>& InRows,
                        const std::filesystem::path& InRoot) -> std::string {
    std::ostringstream out;
    std::map<std::string, std::size_t> groupedRepoCounts;
    for (const auto& repoPath : InRepos) {
        const auto relativePath = RelativeDisplayPath(InRoot, repoPath);
        groupedRepoCounts[GroupFromRelativePath(relativePath)] += 1;
    }

    out << "{\"summary\":{";
    out << std::format("\"repo_count\":{},\"group_count\":{}", InRepos.size(), groupedRepoCounts.size());
    out << "},\"repos\":[";
    for (std::size_t i = 0; i < InRepos.size(); ++i) {
        if (i > 0) {
            out << ",";
        }
        const auto relativePath = RelativeDisplayPath(InRoot, InRepos[i]);
        out << "{";
        out << std::format("\"index\":{},", i + 1);
        out << std::format("\"group\":\"{}\",", GroupFromRelativePath(relativePath));
        const auto repoJson = FormatJson(InRepos[i], InBranches[i], InRemoteRefs[i], InRows[i]);
        out << repoJson.substr(1, repoJson.size() - 2);
        out << "}";
    }
    out << "]}";
    return out.str();
}

} // namespace

void RegisterRemote(CLI::App& InApp) {
    auto* cmd = InApp.add_subcommand("remote", "Show remote references and URLs");

    auto* format = new std::string{"table"};
    auto* repo = new std::string{};
    auto* maxDepth = new int{8};
    auto* noCache = new bool{false};
    auto* noRefreshCache = new bool{false};

    cmd->add_option("--format", *format, "Output format: table|json")->default_str("table");
    cmd->add_option("--repo", *repo, "Target repository path (optional; default: all discovered repos)");
    cmd->add_option("--max-depth", *maxDepth, "Discovery max depth for workspace mode");
    cmd->add_flag("--no-cache", *noCache, "Disable discovery cache in workspace mode");
    cmd->add_flag("--no-refresh-cache", *noRefreshCache, "Do not force cache refresh in workspace mode");

    cmd->callback([format, repo, maxDepth, noCache, noRefreshCache]() {
        if (*format != "table" && *format != "json") {
            std::cerr << "Error: invalid --format value: " << *format << " (expected table|json)\n";
            std::exit(1);
        }

        if (!repo->empty()) {
            const auto repoPath = std::filesystem::path(*repo).lexically_normal();
            const auto check = GitCapture(repoPath, {"rev-parse", "--git-dir"});
            if (check.exitCode != 0) {
                std::cerr << "Error: not a git repository: " << repoPath.generic_string() << "\n";
                std::exit(1);
            }

            const auto branch = CurrentBranch(repoPath);
            const auto remoteRef = CurrentRemoteRef(repoPath);
            const auto rows = DiscoverRemotes(repoPath);

            if (*format == "json") {
                std::cout << FormatJson(repoPath, branch, remoteRef, rows) << '\n';
            } else {
                std::vector<std::filesystem::path> repoPaths{repoPath};
                std::vector<std::string> branches{branch};
                std::vector<std::string> remoteRefs{remoteRef};
                std::vector<std::vector<RemoteRow>> allRows{rows};
                std::cout << FormatWorkspaceTable(repoPaths, branches, remoteRefs, allRows, std::filesystem::current_path());
            }
            return;
        }

        workspace::DiscoverOptions options;
        options.rootDir = std::filesystem::current_path();
        options.maxDepth = *maxDepth;
        options.useCache = !*noCache;
        options.refreshCache = !*noRefreshCache;
        options.metadataLevel = "minimal";

        const auto discovery = workspace::DiscoverRepos(options);
        std::vector<std::filesystem::path> repoPaths;
        std::vector<std::string> branches;
        std::vector<std::string> remoteRefs;
        std::vector<std::vector<RemoteRow>> allRows;

        for (const auto& repoRecord : discovery.repos) {
            const auto check = GitCapture(repoRecord.path, {"rev-parse", "--git-dir"});
            if (check.exitCode != 0) {
                continue;
            }
            repoPaths.push_back(repoRecord.path);
            branches.push_back(CurrentBranch(repoRecord.path));
            remoteRefs.push_back(CurrentRemoteRef(repoRecord.path));
            allRows.push_back(DiscoverRemotes(repoRecord.path));
        }

        if (*format == "json") {
            std::cout << FormatWorkspaceJson(repoPaths, branches, remoteRefs, allRows, options.rootDir) << '\n';
        } else {
            std::cout << FormatWorkspaceTable(repoPaths, branches, remoteRefs, allRows, options.rootDir);
        }
    });
}

} // namespace kano::git::commands
