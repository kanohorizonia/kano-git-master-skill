// commit-push command — Orchestrate commit -> sync -> push workflow

#include "command_registry.hpp"
#include "shell_executor.hpp"

#include <cctype>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <map>
#include <set>
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

auto RunKogCommand(const std::vector<std::string>& InArgs, shell::ExecMode InMode) -> shell::ExecResult {
    if (const char* binaryPath = std::getenv("KANO_GIT_BINARY_PATH"); binaryPath != nullptr) {
        const std::filesystem::path binary(binaryPath);
        if (std::filesystem::exists(binary)) {
            return shell::ExecuteCommand(binary.generic_string(), InArgs, InMode, std::filesystem::current_path());
        }
    }

    const auto launcher = ResolveLauncherScript();
    if (!launcher.empty()) {
        std::vector<std::string> args;
        args.push_back(launcher.generic_string());
        args.insert(args.end(), InArgs.begin(), InArgs.end());
        return shell::ExecuteCommand("bash", args, InMode, std::filesystem::current_path());
    }

    return shell::ExecuteCommand("kano-git", InArgs, InMode, std::filesystem::current_path());
}

auto RunKogCommand(const std::vector<std::string>& InArgs) -> shell::ExecResult {
    return RunKogCommand(InArgs, shell::ExecMode::PassThrough);
}

auto SplitLines(const std::string& InText) -> std::vector<std::string> {
    std::vector<std::string> out;
    std::istringstream iss(InText);
    std::string line;
    while (std::getline(iss, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        out.push_back(line);
    }
    return out;
}

auto ParsePushFailures(const std::string& InStdout, const std::string& InStderr) -> std::map<std::string, std::vector<std::string>> {
    std::map<std::string, std::vector<std::string>> failures;
    std::map<std::string, std::set<std::string>> dedup;

    auto handleLine = [&](const std::string& line) {
        if (line.empty()) {
            return;
        }

        std::string repo = "(unknown)";
        std::string reason = line;

        const auto lb = line.find('[');
        const auto rb = line.find(']');
        if (lb != std::string::npos && rb != std::string::npos && rb > lb + 1) {
            repo = line.substr(lb + 1, rb - lb - 1);
            if (rb + 1 < line.size()) {
                reason = Trim(line.substr(rb + 1));
            }
        } else {
            const auto forPos = line.find(" for ");
            if (forPos != std::string::npos && forPos + 5 < line.size()) {
                repo = Trim(line.substr(forPos + 5));
            }
        }

        const auto lowered = [] (std::string value) {
            for (auto& ch : value) {
                ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            }
            return value;
        }(reason);

        const bool failedLike =
            lowered.find("failed") != std::string::npos ||
            lowered.find("fatal:") != std::string::npos ||
            lowered.find("error:") != std::string::npos;
        if (!failedLike) {
            return;
        }

        if (lowered.rfind("summary:", 0) == 0) {
            return;
        }

        if (reason.empty()) {
            reason = line;
        }

        if (!dedup[repo].contains(reason)) {
            dedup[repo].insert(reason);
            failures[repo].push_back(reason);
        }
    };

    for (const auto& line : SplitLines(InStdout)) {
        handleLine(line);
    }
    for (const auto& line : SplitLines(InStderr)) {
        handleLine(line);
    }

    return failures;
}

} // namespace

void RegisterCommitPush(CLI::App& InApp) {
    auto* cmd = InApp.add_subcommand("commit-push", "Run pre-commit, commit, sync, post-commit, then push in one command");
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
    auto* profile = new bool{false};
    auto* branchMode = new std::string{"default"};
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
    cmd->add_flag("--profile", *profile, "Print commit-push stage timing summary");
    cmd->add_option("--branch-mode", *branchMode, "Detached branch inference mode for pre-commit: default|stable-dev");
    cmd->add_flag("--force-with-lease", *forceWithLease, "Use force-with-lease for push");
    cmd->add_flag("--no-verify", *noVerify, "Pass --no-verify to push");
    cmd->add_option("--jobs", *jobs, "Push parallel workers");
    cmd->add_flag("--verbose", *verbose, "Verbose push output");
    cmd->add_option("--remote", *remote, "Remote filter for push");

    cmd->callback([=]() {
        const auto totalStart = std::chrono::steady_clock::now();
        long long preCommitMillis = 0;
        long long commitMillis = 0;
        long long syncMillis = 0;
        long long postCommitMillis = 0;
        long long pushMillis = 0;

        const auto extras = cmd->remaining();
        if (!extras.empty()) {
            std::cerr << "Error: unsupported extra arguments in commit-push mode:";
            for (const auto& extra : extras) {
                std::cerr << ' ' << extra;
            }
            std::cerr << "\n";
            std::exit(2);
        }

        std::cout << "=== commit-push stage: pre-commit ===\n";
        const auto preCommitStart = std::chrono::steady_clock::now();
        const auto repoList = ParseReposCsv(*repos);
        if (!repoList.empty()) {
            for (const auto& repo : repoList) {
                std::vector<std::string> preCommitArgs{"sync", "pre-commit", "--repo", repo, "--no-recursive", "--branch-mode", *branchMode};
                if (*dryRun) {
                    preCommitArgs.push_back("--dry-run");
                }
                const auto preCommitResult = RunKogCommand(preCommitArgs);
                if (preCommitResult.exitCode != 0) {
                    std::exit(preCommitResult.exitCode);
                }
            }
        } else {
            std::vector<std::string> preCommitArgs{"sync", "pre-commit", "--branch-mode", *branchMode};
            if (*noRecursive) {
                preCommitArgs.push_back("--no-recursive");
            }
            if (*dryRun) {
                preCommitArgs.push_back("--dry-run");
            }
            const auto preCommitResult = RunKogCommand(preCommitArgs);
            if (preCommitResult.exitCode != 0) {
                std::exit(preCommitResult.exitCode);
            }
        }
        const auto preCommitEnd = std::chrono::steady_clock::now();
        preCommitMillis = std::chrono::duration_cast<std::chrono::milliseconds>(preCommitEnd - preCommitStart).count();

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
        const auto commitStart = std::chrono::steady_clock::now();
        const auto commitResult = RunKogCommand(commitArgs);
        const auto commitEnd = std::chrono::steady_clock::now();
        commitMillis = std::chrono::duration_cast<std::chrono::milliseconds>(commitEnd - commitStart).count();
        if (commitResult.exitCode != 0) {
            std::exit(commitResult.exitCode);
        }

        std::cout << "=== commit-push stage: sync ===\n";
        const auto syncStart = std::chrono::steady_clock::now();
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
        const auto syncEnd = std::chrono::steady_clock::now();
        syncMillis = std::chrono::duration_cast<std::chrono::milliseconds>(syncEnd - syncStart).count();

        std::vector<std::string> postCommitArgs{"commit"};
        if (!repos->empty()) {
            postCommitArgs.insert(postCommitArgs.end(), {"--repos", *repos});
        }
        if (*noRecursive) {
            postCommitArgs.push_back("--no-recursive");
        }
        if (!message->empty()) {
            postCommitArgs.insert(postCommitArgs.end(), {"-m", *message});
        }
        if (!aiProvider->empty()) {
            postCommitArgs.insert(postCommitArgs.end(), {"--ai-provider", *aiProvider});
        }
        if (!aiModel->empty()) {
            postCommitArgs.insert(postCommitArgs.end(), {"--ai-model", *aiModel});
        }
        if (*aiAuto) {
            postCommitArgs.push_back("--ai-auto");
        }
        if (*noAiReview) {
            postCommitArgs.push_back("--no-ai-review");
        }
        if (*stagedOnly) {
            postCommitArgs.push_back("--staged-only");
        }
        if (*dryRun) {
            postCommitArgs.push_back("--preflight-only");
        }

        std::cout << "=== commit-push stage: post-commit ===\n";
        const auto postCommitStart = std::chrono::steady_clock::now();
        const auto postCommitResult = RunKogCommand(postCommitArgs);
        const auto postCommitEnd = std::chrono::steady_clock::now();
        postCommitMillis = std::chrono::duration_cast<std::chrono::milliseconds>(postCommitEnd - postCommitStart).count();
        if (postCommitResult.exitCode != 0) {
            std::exit(postCommitResult.exitCode);
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
        if (*profile) {
            pushArgs.push_back("--profile");
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
        const auto pushStart = std::chrono::steady_clock::now();
        const auto pushResult = RunKogCommand(pushArgs, shell::ExecMode::Capture);

        if (!pushResult.stdoutStr.empty()) {
            std::cout << pushResult.stdoutStr;
            if (pushResult.stdoutStr.back() != '\n') {
                std::cout << "\n";
            }
        }
        if (!pushResult.stderrStr.empty()) {
            std::cerr << pushResult.stderrStr;
            if (pushResult.stderrStr.back() != '\n') {
                std::cerr << "\n";
            }
        }

        if (pushResult.exitCode != 0) {
            const auto failures = ParsePushFailures(pushResult.stdoutStr, pushResult.stderrStr);
            if (!failures.empty()) {
                std::cerr << "\n=== Failed Repos (highlight) ===\n";
                std::size_t idx = 0;
                for (const auto& [repo, reasons] : failures) {
                    idx += 1;
                    std::cerr << "[" << idx << "] " << repo << "\n";
                    for (const auto& reason : reasons) {
                        std::cerr << "  - " << reason << "\n";
                    }
                }
            }
        }

        const auto pushEnd = std::chrono::steady_clock::now();
        pushMillis = std::chrono::duration_cast<std::chrono::milliseconds>(pushEnd - pushStart).count();

        if (*profile) {
            const auto totalEnd = std::chrono::steady_clock::now();
            const auto totalMillis = std::chrono::duration_cast<std::chrono::milliseconds>(totalEnd - totalStart).count();
            std::cout << "\n=== Commit-Push Profile Summary ===\n";
            std::cout << "mode: native\n";
            std::cout << "pre_commit_ms: " << preCommitMillis << "\n";
            std::cout << "commit_ms: " << commitMillis << "\n";
            std::cout << "sync_ms: " << syncMillis << "\n";
            std::cout << "post_commit_ms: " << postCommitMillis << "\n";
            std::cout << "push_ms: " << pushMillis << "\n";
            std::cout << "total_ms: " << totalMillis << "\n";
        }

        std::exit(pushResult.exitCode);
    });
}

} // namespace kano::git::commands
