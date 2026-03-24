#include <CLI/CLI.hpp>
#include "shell_executor.hpp"
#include "ai_utils.hpp"
#include "kog_config.hpp"

#include <cstdlib>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>
#include <filesystem>

namespace kano::git::commands {
void RegisterResolve(CLI::App& InApp) {
    auto* cmd = InApp.add_subcommand("resolve", "AI-powered conflict resolution");
    
    auto* provider = new std::string("auto");
    cmd->add_option("-p,--provider", *provider, "AI provider to use (copilot, codex)")->default_val("auto");

    auto* repo = new std::string(".");
    cmd->add_option("-r,--repo", *repo, "Path to the repository")->default_val(".");

    cmd->callback([=]() {
        std::filesystem::path workspaceRoot = std::filesystem::path(*repo).lexically_normal();
        if (workspaceRoot == ".") {
            const auto root = shell::ExecuteCommand("git", {"rev-parse", "--show-toplevel"}, shell::ExecMode::Capture).stdoutStr;
            workspaceRoot = std::filesystem::path(Trim(root));
        }
        
        if (!AIResolveConflicts(workspaceRoot, *provider)) {
            throw CLI::RuntimeError(1);
        }
    });
}

} // namespace kano::git::commands
