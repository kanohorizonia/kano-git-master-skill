// clone command — Smart clone with upstream support
// Delegates to: scripts/core/smart-clone.sh

#include <CLI/CLI.hpp>
#include "discovery.hpp"
#include "shell_executor.hpp"

#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace {

auto DeriveRepoDirName(const std::string& InRepoUrl) -> std::string {
    auto value = InRepoUrl;
    while (!value.empty() && value.back() == '/') {
        value.pop_back();
    }
    const auto slashPos = value.find_last_of('/');
    const auto colonPos = value.find_last_of(':');
    const auto sepPos = std::max(slashPos, colonPos);
    auto name = (sepPos == std::string::npos) ? value : value.substr(sepPos + 1);
    if (name.ends_with(".git")) {
        name = name.substr(0, name.size() - 4);
    }
    return name;
}

} // namespace

namespace kano::git::commands {

void RegisterClone(CLI::App& InApp) {
    auto* cmd = InApp.add_subcommand("clone", "Smart clone with upstream remote support");

    struct CloneOptions {
        std::string repoUrl;
        std::string upstreamUrl;
        std::string targetDir;
        bool noInit = false;
        bool noCheckout = false;
        bool dryRun = false;
    };

    auto options = std::make_shared<CloneOptions>();

    cmd->add_option("repo-url", options->repoUrl, "Repository URL to clone")->required();
    cmd->add_option("upstream-url", options->upstreamUrl, "Optional upstream repository URL");
    cmd->add_option("--dir", options->targetDir, "Target directory");
    cmd->add_flag("--no-init", options->noInit, "Skip initializing if remote is empty");
    cmd->add_flag("--no-checkout", options->noCheckout, "Skip checkout to default branch");
    cmd->add_flag("--dry-run", options->dryRun, "Show what would be done");

    cmd->callback([cmd, options]() {
        auto extras = cmd->remaining();
        if (!extras.empty()) {
            std::cerr << "Error: unsupported extra arguments in native-only mode for clone.";
            for (const auto& extra : extras) {
                std::cerr << ' ' << extra;
            }
            std::cerr << "\n";
            throw CLI::RuntimeError(2);
        }

        if (options->noInit) {
            std::cout << "[WARN] --no-init is a compatibility flag and is currently a no-op in native clone mode\n";
        }

        std::vector<std::string> cloneArgs = {"clone"};
        if (options->noCheckout) {
            cloneArgs.push_back("--no-checkout");
        }
        cloneArgs.push_back(options->repoUrl);
        if (!options->targetDir.empty()) {
            cloneArgs.push_back(options->targetDir);
        }

        if (options->dryRun) {
            std::cout << "[DRY-RUN] Would execute: git";
            for (const auto& arg : cloneArgs) {
                std::cout << " " << arg;
            }
            std::cout << "\n";
            if (!options->upstreamUrl.empty()) {
                std::cout << "[DRY-RUN] Would configure upstream remote: " << options->upstreamUrl << "\n";
            }
            std::exit(0);
        }

        const auto cloneResult = shell::ExecuteCommand("git", cloneArgs, shell::ExecMode::PassThrough);
        if (cloneResult.exitCode != 0) {
            throw CLI::RuntimeError(cloneResult.exitCode);
        }

        if (!options->upstreamUrl.empty()) {
            const auto repoDir = options->targetDir.empty() ? DeriveRepoDirName(options->repoUrl) : options->targetDir;
            const auto repoPath = std::filesystem::current_path() / std::filesystem::path(repoDir);
            const auto addUpstream = shell::ExecuteCommand(
                "git",
                {"remote", "add", "upstream", options->upstreamUrl},
                shell::ExecMode::PassThrough,
                repoPath
            );
            if (addUpstream.exitCode != 0) {
                throw CLI::RuntimeError(addUpstream.exitCode);
            }
            std::cout << "[INFO] Added upstream remote in " << repoPath.generic_string() << "\n";
        }

        const auto repoDir = options->targetDir.empty() ? DeriveRepoDirName(options->repoUrl) : options->targetDir;
        const auto repoPath = (std::filesystem::current_path() / std::filesystem::path(repoDir)).lexically_normal();
        const auto workspaceRoot = std::filesystem::current_path().lexically_normal();
        const auto relative = repoPath.lexically_relative(workspaceRoot);
        if (!relative.empty() && !relative.generic_string().starts_with("..")) {
            if (!workspace::UpsertUnregisteredRepoIntoWorkspaceManifest(workspaceRoot, repoPath)) {
                std::cerr << "[WARN] cloned repo succeeded, but failed to update workspace manifest\n";
            }
        }

        std::exit(0);
    });
}

} // namespace kano::git::commands
