#include <CLI/CLI.hpp>
#include "shell_executor.hpp"
#include "ai_utils.hpp"
#include "kog_config.hpp"
#include "plan_utils.hpp"

#include <iostream>
#include <string>
#include <vector>
#include <filesystem>

namespace kano::git::commands {

namespace {

struct CherryPickPlan {
    std::string repo = ".";
    std::vector<std::string> commits;
    bool aiEnabled = false;
    std::string aiProvider;
};

auto ParseCherryPickPlan(const std::filesystem::path& InPlanPath,
                         std::string* OutError) -> std::optional<CherryPickPlan> {
    const auto payload = ReadFileText(InPlanPath);
    if (!payload.has_value()) {
        if (OutError) *OutError = "cannot read plan file";
        return std::nullopt;
    }

    const auto text = Trim(*payload);
    if (text.empty()) {
        if (OutError) *OutError = "plan file is empty";
        return std::nullopt;
    }

    CherryPickPlan out;
    if (const auto repo = ExtractStringField(text, "repo"); repo.has_value() && !Trim(*repo).empty()) {
        out.repo = Trim(*repo);
    }

    const auto commitsArray = ExtractArrayBodyForKey(text, "commits");
    if (!commitsArray.has_value()) {
        if (OutError) *OutError = "missing commits array";
        return std::nullopt;
    }
    out.commits = ParseJsonStringArrayBody("[" + *commitsArray + "]");
    if (out.commits.empty()) {
        if (OutError) *OutError = "commits array is empty";
        return std::nullopt;
    }

    if (const auto aiObject = ExtractObjectBodyForKey(text, "ai"); aiObject.has_value()) {
        if (const auto enabled = ExtractBoolField(*aiObject, "enabled"); enabled.has_value()) {
            out.aiEnabled = *enabled;
        }
        if (const auto provider = ExtractStringField(*aiObject, "provider"); provider.has_value()) {
            out.aiProvider = Trim(*provider);
        }
    }

    return out;
}

} // namespace

void RegisterCherryPick(CLI::App& InApp) {
    auto* cmd = InApp.add_subcommand("cherry-pick", "Cherry-pick commits with optional AI conflict resolution");
    
    auto* commits = new std::vector<std::string>();
    cmd->add_option("commits", *commits, "Commits to cherry-pick");

    auto* repo = new std::string(".");
    cmd->add_option("-r,--repo", *repo, "Path to the repository")->default_val(".");

    auto* planFile = new std::string{};
    cmd->add_option("--plan-file", *planFile, "Plan JSON file for cherry-pick sequence");

    auto* continueFlag = new bool{false};
    cmd->add_flag("--continue", *continueFlag, "Continue the current cherry-pick operation");

    auto* skipFlag = new bool{false};
    cmd->add_flag("--skip", *skipFlag, "Skip the current commit and continue");

    auto* abortFlag = new bool{false};
    cmd->add_flag("--abort", *abortFlag, "Abort the current cherry-pick operation");

    auto* agentMode = new bool{false};
    cmd->add_flag("--agent", *agentMode, "Enable agent mode (auto AI conflict resolution)");

    auto* aiAuto = new bool{false};
    cmd->add_flag("--ai-auto,--ai", *aiAuto, "Enable AI auto conflict resolution");

    auto* noAiResolve = new bool{false};
    cmd->add_flag("--no-ai-resolve", *noAiResolve, "Disable AI conflict resolution even in agent mode");

    auto* provider = new std::string("auto");
    cmd->add_option("-p,--provider,--ai-provider", *provider, "AI provider to use (copilot, codex)");

    cmd->callback([=]() {
        if (!planFile->empty() && !commits->empty()) {
            std::cerr << "Error: --plan-file cannot be combined with positional commits.\n";
            throw CLI::RuntimeError(2);
        }

        CherryPickPlan plan;
        if (!planFile->empty()) {
            std::string error;
            const auto normalizedPlanPath = NormalizeInputPathForCurrentPlatform(*planFile);
            const auto parsed = ParseCherryPickPlan(std::filesystem::path(normalizedPlanPath), &error);
            if (!parsed.has_value()) {
                std::cerr << "Error: invalid --plan-file: " << normalizedPlanPath;
                if (!error.empty()) {
                    std::cerr << " (" << error << ")";
                }
                std::cerr << "\n";
                throw CLI::RuntimeError(2);
            }
            plan = *parsed;
        }

        const auto effectiveRepo = !planFile->empty() && *repo == "." ? plan.repo : *repo;
        const auto workspaceRoot = std::filesystem::path(effectiveRepo).lexically_normal();

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

        const auto& effectiveCommits = !planFile->empty() ? plan.commits : *commits;
        if (effectiveCommits.empty()) {
            std::cerr << "Error: no commits specified to cherry-pick.\n";
            throw CLI::RuntimeError(1);
        }

        if (!planFile->empty() && (*provider == "auto" || provider->empty()) && !plan.aiProvider.empty()) {
            *provider = plan.aiProvider;
        }

        for (const auto& commit : effectiveCommits) {
            std::cout << ">>> Cherry-picking " << commit << "...\n";
            auto result = shell::ExecuteCommand("git", {"cherry-pick", commit}, shell::ExecMode::PassThrough, workspaceRoot);
            
            if (result.exitCode != 0) {
                const auto status = shell::ExecuteCommand("git", {"status"}, shell::ExecMode::Capture, workspaceRoot).stdoutStr;
                if (status.find("You are currently cherry-picking commit") != std::string::npos && 
                    status.find("Unmerged paths:") != std::string::npos) {
                    
                    std::cout << "Conflict detected during cherry-pick of " << commit << ".\n";
                    
                    bool shouldResolve = (*agentMode || *aiAuto || plan.aiEnabled || IsAgentModeEnabled()) && !(*noAiResolve);
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
