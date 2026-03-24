// version command — project version overview

#include <CLI/CLI.hpp>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <regex>
#include <string>
#include <vector>

#include "discovery.hpp"
#include "shell_executor.hpp"
#include "ai_utils.hpp"

namespace kano::git::commands {

namespace {

auto ReadVersionFile(const std::filesystem::path& InRepo) -> std::optional<std::string> {
    const auto versionPath = InRepo / "VERSION";
    if (!std::filesystem::exists(versionPath)) {
        return std::nullopt;
    }
    std::ifstream in(versionPath);
    if (!in) {
        return std::nullopt;
    }
    std::string version;
    std::getline(in, version);
    return Trim(version);
}

auto ReadUnrealProjectVersion(const std::filesystem::path& InRepo) -> std::optional<std::string> {
    const auto iniPath = (InRepo / "Config" / "DefaultGame.ini").lexically_normal();
    if (!std::filesystem::exists(iniPath)) {
        return std::nullopt;
    }
    std::ifstream in(iniPath);
    if (!in) {
        return std::nullopt;
    }
    std::string line;
    static const std::regex re(R"(ProjectVersion\s*=\s*([^\s\r\n]+))");
    while (std::getline(in, line)) {
        std::smatch match;
        if (std::regex_search(line, match, re)) {
            return Trim(match[1].str());
        }
    }
    return std::nullopt;
}

struct ProjectVersionInfo {
    std::string repoPath;
    std::string repoName;
    std::string version;
    std::string versionSource;
    std::string branch;
    std::string revisionCount;
    std::string hash;
    bool isDirty = false;
};

auto AnalyzeRepoVersion(const std::filesystem::path& InRepoPath, const std::filesystem::path& InRootPath) -> ProjectVersionInfo {
    ProjectVersionInfo info;
    info.repoPath = std::filesystem::relative(InRepoPath, InRootPath).lexically_normal().generic_string();
    if (info.repoPath == "" || info.repoPath == ".") {
        info.repoPath = ".";
    }
    info.repoName = InRepoPath.filename().generic_string();

    if (auto v = ReadUnrealProjectVersion(InRepoPath)) {
        info.version = *v;
        info.versionSource = "Config/DefaultGame.ini";
    } else if (auto v = ReadVersionFile(InRepoPath)) {
        info.version = *v;
        info.versionSource = "VERSION";
    } else {
        info.version = "-";
        info.versionSource = "";
    }

    // Git info
    const auto branchResult = shell::ExecuteCommand("git", {"rev-parse", "--abbrev-ref", "HEAD"}, shell::ExecMode::Capture, InRepoPath);
    info.branch = branchResult.exitCode == 0 ? Trim(branchResult.stdoutStr) : "unknown";

    const auto revResult = shell::ExecuteCommand("git", {"rev-list", "--count", "--first-parent", "HEAD"}, shell::ExecMode::Capture, InRepoPath);
    info.revisionCount = revResult.exitCode == 0 ? Trim(revResult.stdoutStr) : "0";

    const auto hashResult = shell::ExecuteCommand("git", {"rev-parse", "--short", "HEAD"}, shell::ExecMode::Capture, InRepoPath);
    info.hash = hashResult.exitCode == 0 ? Trim(hashResult.stdoutStr) : "unknown";

    const auto dirtyResult = shell::ExecuteCommand("git", {"status", "--porcelain"}, shell::ExecMode::Capture, InRepoPath);
    info.isDirty = dirtyResult.exitCode == 0 && !Trim(dirtyResult.stdoutStr).empty();

    return info;
}

} // namespace

void RegisterVersion(CLI::App& InApp) {
    auto* cmd = InApp.add_subcommand("version", "Overview project versions in the workspace");
    
    auto repo = std::make_shared<std::string>(".");
    auto recursive = std::make_shared<bool>(true);
    auto field = std::make_shared<std::string>("");

    cmd->add_option("repo", *repo, "Workspace or project root path")->default_val(".");
    cmd->add_flag("-r,--recursive,!--no-recursive", *recursive, "Recursive repository discovery")->default_val(true);
    cmd->add_option("-f,--field", *field, "Output specific field of the current repo (sha, revision, branch, version, human)");

    cmd->callback([repo, recursive, field]() {
        const auto rootPath = std::filesystem::weakly_canonical(std::filesystem::path(*repo));
        
        std::vector<std::filesystem::path> repoPaths;

        if (*recursive) {
            kano::git::workspace::DiscoverOptions opts;
            opts.rootDir = rootPath;
            opts.maxDepth = 3;
            opts.useCache = true;
            const auto result = kano::git::workspace::DiscoverRepos(opts);

            for (const auto& r : result.repos) {
                repoPaths.push_back(r.path);
            }
        } else {
            repoPaths.push_back(rootPath);
        }

        if (repoPaths.empty()) {
            std::cerr << "Error: No git repositories found.\n";
            std::exit(1);
        }

        if (!field->empty()) {
            auto info = AnalyzeRepoVersion(rootPath, rootPath);
            std::string targetField = *field;
            std::string out;

            if (targetField == "sha" || targetField == "hash") out = info.hash;
            else if (targetField == "revision" || targetField == "rev") out = info.revisionCount;
            else if (targetField == "branch") out = info.branch;
            else if (targetField == "version") out = info.version;
            else if (targetField == "human") out = info.branch + " (rev " + info.revisionCount + ") " + info.hash;
            else {
                std::cerr << "Error: Unknown field: " << targetField << "\n";
                std::exit(1);
            }
            std::cout << (out.empty() ? "-" : out) << "\n";
            return;
        }

        std::vector<ProjectVersionInfo> projects;
        for (const auto& p : repoPaths) {
            projects.push_back(AnalyzeRepoVersion(p, rootPath));
        }

        for (const auto& info : projects) {
            std::cout << "[" << info.repoPath << "] " << info.repoName << "\n";
            std::cout << "    Version:  " << info.version;
            if (!info.versionSource.empty()) {
                std::cout << " (source: " << info.versionSource << ")";
            }
            std::cout << "\n";
            std::cout << "    Revision: " << info.branch << " (rev " << info.revisionCount << ") " << info.hash << "\n";
            std::cout << "    Status:   " << (info.isDirty ? "Dirty" : "Clean") << "\n";
            std::cout << "\n";
        }
    });
}

} // namespace kano::git::commands
