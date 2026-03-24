#include <CLI/CLI.hpp>
#include "shell_executor.hpp"
#include "ai_utils.hpp"
#include "kog_config.hpp"

#include <iostream>
#include <string>
#include <vector>
#include <filesystem>

namespace kano::git::commands {
void RegisterCherryPick(CLI::App& InApp) {
    auto* cmd = InApp.add_subcommand("cherry-pick", "Cherry-pick commits with optional AI conflict resolution");
    
    auto* commits = new std::vector<std::string>();
    cmd->add_option("commits", *commits, "Commits to cherry-pick");

    auto* repo = new std::string(".");
    cmd->add_option("-r,--repo", *repo, "Path to the repository")->default_val(".");

    auto* continueFlag = new bool{false};
    cmd->add_flag("--continue", *continueFlag, "Continue the current cherry-pick operation");

    auto* skipFlag = new bool{false};
    cmd->add_flag("--skip", *skipFlag, "Skip the current commit and continue");

    auto* abortFlag = new bool{false};
    cmd->add_flag("--abort", *abortFlag, "Abort the current cherry-pick operation");

    auto* agentMode = new bool{false};
    cmd->add_flag("--agent", *agentMode, "Enable agent mode (auto AI conflict resolution)");

    auto* noAiResolve = new bool{false};
    cmd->add_flag("--no-ai-resolve", *noAiResolve, "Disable AI conflict resolution even in agent mode");

    auto* provider = new std::string("auto");
    cmd->add_option("-p,--provider", *provider, "AI provider to use (copilot, codex)");

    cmd->callback([=]() {
        const auto root = shell::ExecuteCommand("git", {"rev-parse", "--show-toplevel"}, shell::ExecMode::Capture).stdoutStr;
        const auto workspaceRoot = std::filesystem::path(Trim(root));

        if (*abortFlag) {
            shell::ExecuteCommand("git", {"cherry-pick", "--abort"}, shell::ExecMode::PassThrough, workspaceRoot);
            return;
        }

        if (*skipFlag) {
            shell::ExecuteCommand("git", {"cherry-pick", "--skip"}, shell::ExecMode::PassThrough, workspaceRoot);
            return;
        }

        if (*continueFlag) {
            shell::ExecuteCommand("git", {"cherry-pick", "--continue"}, shell::ExecMode::PassThrough, workspaceRoot);
            return;
        }

        if (commits->empty()) {
            std::cerr << "Error: no commits specified to cherry-pick.\n";
            throw CLI::RuntimeError(1);
        }

        for (const auto& commit : *commits) {
            std::cout << ">>> Cherry-picking " << commit << "...\n";
            auto result = shell::ExecuteCommand("git", {"cherry-pick", commit}, shell::ExecMode::PassThrough, workspaceRoot);
            
            if (result.exitCode != 0) {
                const auto status = shell::ExecuteCommand("git", {"status"}, shell::ExecMode::Capture, workspaceRoot).stdoutStr;
                if (status.find("You are currently cherry-picking commit") != std::string::npos && 
                    status.find("Unmerged paths:") != std::string::npos) {
                    
                    std::cout << "Conflict detected during cherry-pick of " << commit << ".\n";
                    
                    bool shouldResolve = (*agentMode || IsAgentModeEnabled()) && !(*noAiResolve);
                    if (shouldResolve) {
                        std::cout << "Attempting AI conflict resolution...\n";
                        if (AIResolveConflicts(workspaceRoot, *provider)) {
                            std::cout << "Conflicts resolved. Continuing...\n";
                            shell::ExecuteCommand("git", {"cherry-pick", "--continue", "--no-edit"}, shell::ExecMode::PassThrough, workspaceRoot);
                        } else {
                            std::cerr << "AI resolution failed. Please resolve conflicts manually and run 'kog cherry-pick --continue'.\n";
                            throw CLI::RuntimeError(1);
                        }
                    } else {
                        std::cout << "Manual resolution required. Resolve conflicts and run 'kog cherry-pick --continue'.\n";
                        throw CLI::RuntimeError(1);
                    }
                } else {
                    std::cerr << "Error: cherry-pick failed for " << commit << ".\n";
                    throw CLI::RuntimeError(result.exitCode);
                }
            }
        }
        
        std::cout << "Cherry-pick sequence completed successfully.\n";
    });
}

} // namespace kano::git::commands
