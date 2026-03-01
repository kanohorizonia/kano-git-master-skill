// commit-push command — Orchestrate commit -> sync -> push workflow

#include "command_registry.hpp"
#include "shell_executor.hpp"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

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

auto ParseReposCsv(const std::string& InCsv) -> std::vector<std::string> {
    std::vector<std::string> repos;
    std::istringstream iss(InCsv);
    std::string token;
    while (std::getline(iss, token, ',')) {
        token = Trim(token);
        if (token.empty()) {
            continue;
        }
        repos.push_back(token);
    }
    return repos;
}

auto ResolveLauncherScript() -> std::filesystem::path {
    if (const char* root = std::getenv("KANO_GIT_MASTER_ROOT"); root != nullptr) {
        const std::filesystem::path candidate = std::filesystem::path(root) / "scripts" / "kano-git";
        if (std::filesystem::exists(candidate)) {
            return candidate;
        }
    }

    const std::filesystem::path cwdCandidate = std::filesystem::current_path() / "scripts" / "kano-git";
    if (std::filesystem::exists(cwdCandidate)) {
        return cwdCandidate;
    }

    return {};
}

auto RunKogCommand(const std::vector<std::string>& InArgs) -> shell::ExecResult {
    if (const char* binaryPath = std::getenv("KANO_GIT_BINARY_PATH"); binaryPath != nullptr) {
        const std::filesystem::path binary(binaryPath);
        if (std::filesystem::exists(binary)) {
            return shell::ExecuteCommand(binary.generic_string(), InArgs, shell::ExecMode::PassThrough, std::filesystem::current_path());
        }
    }

    const auto launcher = ResolveLauncherScript();
    if (!launcher.empty()) {
        std::vector<std::string> args;
        args.push_back(launcher.generic_string());
        args.insert(args.end(), InArgs.begin(), InArgs.end());
        return shell::ExecuteCommand("bash", args, shell::ExecMode::PassThrough, std::filesystem::current_path());
    }

    return shell::ExecuteCommand("kano-git", InArgs, shell::ExecMode::PassThrough, std::filesystem::current_path());
}

} // namespace

void RegisterCommitPush(CLI::App& InApp) {
    auto* cmd = InApp.add_subcommand("commit-push", "Run commit, sync, then push in one command");
    cmd->allow_extras();

    auto* repos = new std::string{};
    auto* noRecursive = new bool{false};
    auto* message = new std::string{};
    auto* aiProvider = new std::string{};
    auto* aiModel = new std::string{};
    auto* aiAuto = new bool{false};
    auto* noAiReview = new bool{false};
    auto* stagedOnly = new bool{false};
    auto* dryRun = new bool{false};
    auto* forceWithLease = new bool{false};
    auto* noVerify = new bool{false};
    auto* jobs = new int{0};
    auto* verbose = new bool{false};
    auto* remote = new std::string{};

    cmd->add_option("--repos", *repos, "Target repos (comma-separated)");
    cmd->add_flag("--no-recursive,-N", *noRecursive, "Only operate on current repository (or provided --repos)");
    cmd->add_option("--message,-m", *message, "Commit message (skip AI generation)");
    cmd->add_option("--ai-provider", *aiProvider, "AI provider for commit (copilot, codex, opencode)");
    cmd->add_option("--ai-model", *aiModel, "AI model for commit");
    cmd->add_flag("--ai-auto", *aiAuto, "Enable commit AI auto mode");
    cmd->add_flag("--no-ai-review", *noAiReview, "Skip AI review gate for commit");
    cmd->add_flag("--staged-only", *stagedOnly, "Commit only staged changes");
    cmd->add_flag("--dry-run", *dryRun, "Preview commit/sync/push actions without modifying repositories");
    cmd->add_flag("--force-with-lease", *forceWithLease, "Use force-with-lease for push");
    cmd->add_flag("--no-verify", *noVerify, "Pass --no-verify to push");
    cmd->add_option("--jobs", *jobs, "Push parallel workers");
    cmd->add_flag("--verbose", *verbose, "Verbose push output");
    cmd->add_option("--remote", *remote, "Remote filter for push");

    cmd->callback([=]() {
        const auto extras = cmd->remaining();
        if (!extras.empty()) {
            std::cerr << "Error: unsupported extra arguments in commit-push mode:";
            for (const auto& extra : extras) {
                std::cerr << ' ' << extra;
            }
            std::cerr << "\n";
            std::exit(2);
        }

        std::vector<std::string> commitArgs{"commit"};
        if (!repos->empty()) {
            commitArgs.insert(commitArgs.end(), {"--repos", *repos});
        }
        if (*noRecursive) {
            commitArgs.push_back("--no-recursive");
        }
        if (!message->empty()) {
            commitArgs.insert(commitArgs.end(), {"-m", *message});
        }
        if (!aiProvider->empty()) {
            commitArgs.insert(commitArgs.end(), {"--ai-provider", *aiProvider});
        }
        if (!aiModel->empty()) {
            commitArgs.insert(commitArgs.end(), {"--ai-model", *aiModel});
        }
        if (*aiAuto) {
            commitArgs.push_back("--ai-auto");
        }
        if (*noAiReview) {
            commitArgs.push_back("--no-ai-review");
        }
        if (*stagedOnly) {
            commitArgs.push_back("--staged-only");
        }
        if (*dryRun) {
            commitArgs.push_back("--preflight-only");
        }

        std::cout << "=== commit-push stage: commit ===\n";
        const auto commitResult = RunKogCommand(commitArgs);
        if (commitResult.exitCode != 0) {
            std::exit(commitResult.exitCode);
        }

        std::cout << "=== commit-push stage: sync ===\n";
        const auto repoList = ParseReposCsv(*repos);
        if (!repoList.empty()) {
            for (const auto& repo : repoList) {
                std::vector<std::string> syncArgs{"sync", "origin-latest", "--repo", repo, "--no-recursive"};
                if (*dryRun) {
                    syncArgs.push_back("--dry-run");
                }
                const auto syncResult = RunKogCommand(syncArgs);
                if (syncResult.exitCode != 0) {
                    std::exit(syncResult.exitCode);
                }
            }
        } else {
            std::vector<std::string> syncArgs{"sync", "origin-latest"};
            if (*noRecursive) {
                syncArgs.push_back("--no-recursive");
            }
            if (*dryRun) {
                syncArgs.push_back("--dry-run");
            }
            const auto syncResult = RunKogCommand(syncArgs);
            if (syncResult.exitCode != 0) {
                std::exit(syncResult.exitCode);
            }
        }

        std::vector<std::string> pushArgs{"push", "--skip-sync"};
        if (!repos->empty()) {
            pushArgs.insert(pushArgs.end(), {"--repos", *repos});
        }
        if (*noRecursive) {
            pushArgs.push_back("--no-recursive");
        }
        if (*dryRun) {
            pushArgs.push_back("--dry-run");
        }
        if (*forceWithLease) {
            pushArgs.push_back("--force-with-lease");
        }
        if (*noVerify) {
            pushArgs.push_back("--no-verify");
        }
        if (*jobs > 0) {
            pushArgs.push_back("--jobs");
            pushArgs.push_back(std::to_string(*jobs));
        }
        if (*verbose) {
            pushArgs.push_back("--verbose");
        }
        if (!remote->empty()) {
            pushArgs.push_back("--remote");
            pushArgs.push_back(*remote);
        }

        std::cout << "=== commit-push stage: push ===\n";
        const auto pushResult = RunKogCommand(pushArgs);
        std::exit(pushResult.exitCode);
    });
}

} // namespace kano::git::commands
