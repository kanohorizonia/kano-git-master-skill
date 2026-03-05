// commit-push command — Orchestrate commit -> sync -> push workflow

#include "command_registry.hpp"
#include "shell_executor.hpp"

#include <cctype>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <regex>
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

auto NormalizeInputPathForCurrentPlatform(std::string InPath) -> std::string {
    auto path = Trim(std::move(InPath));
    if (path.empty()) {
        return path;
    }
#if defined(_WIN32)
    auto toWindowsPath = [](char drive, std::string rest) -> std::string {
        for (auto& ch : rest) {
            if (ch == '/') {
                ch = '\\';
            }
        }
        if (!rest.empty() && (rest.front() == '\\' || rest.front() == '/')) {
            rest.erase(rest.begin());
        }
        std::string out;
        out.reserve(rest.size() + 3);
        out.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(drive))));
        out.append(":\\");
        out.append(rest);
        return out;
    };

    if (path.rfind("/cygdrive/", 0) == 0 && path.size() > 11 && std::isalpha(static_cast<unsigned char>(path[10])) &&
        path[11] == '/') {
        return toWindowsPath(path[10], path.substr(12));
    }
    if (path.rfind("/mnt/", 0) == 0 && path.size() > 6 && std::isalpha(static_cast<unsigned char>(path[5])) &&
        path[6] == '/') {
        return toWindowsPath(path[5], path.substr(7));
    }
    if (path.size() > 3 && path[0] == '/' && std::isalpha(static_cast<unsigned char>(path[1])) && path[2] == '/') {
        return toWindowsPath(path[1], path.substr(3));
    }
#endif
    return path;
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

auto IsAgentModeEnabled() -> bool {
    const char* raw = std::getenv("KANO_AGENT_MODE");
    if (raw == nullptr) {
        return false;
    }
    const auto value = Trim(raw);
    if (value.empty() || value == "0" || value == "false" || value == "FALSE") {
        return false;
    }
    return true;
}

auto CaptureHeadShortSha(const std::filesystem::path& InWorkingDir) -> std::string {
    const auto out = shell::ExecuteCommand("git", {"rev-parse", "--short", "HEAD"}, shell::ExecMode::Capture, InWorkingDir);
    if (out.exitCode != 0) {
        return "nohead";
    }
    auto value = Trim(out.stdoutStr);
    if (value.empty()) {
        return "nohead";
    }
    for (auto& ch : value) {
        if (!std::isalnum(static_cast<unsigned char>(ch))) {
            ch = '_';
        }
    }
    return value;
}

auto CurrentUtcTimestampCompact() -> std::string {
    const auto now = std::chrono::system_clock::now();
    const auto tt = std::chrono::system_clock::to_time_t(now);
    std::tm utc{};
#if defined(_WIN32)
    gmtime_s(&utc, &tt);
#else
    gmtime_r(&tt, &utc);
#endif
    char buffer[32] = {0};
    std::strftime(buffer, sizeof(buffer), "%Y%m%dT%H%M%SZ", &utc);
    return std::string(buffer);
}

auto CurrentUtcTimestampIso8601() -> std::string {
    const auto now = std::chrono::system_clock::now();
    const auto tt = std::chrono::system_clock::to_time_t(now);
    std::tm utc{};
#if defined(_WIN32)
    gmtime_s(&utc, &tt);
#else
    gmtime_r(&tt, &utc);
#endif
    char buffer[32] = {0};
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &utc);
    return std::string(buffer);
}

auto DefaultCommitPlanOutputPath(const std::filesystem::path& InWorkspaceRoot) -> std::filesystem::path {
    const auto headShort = CaptureHeadShortSha(InWorkspaceRoot);
    const auto stamp = CurrentUtcTimestampCompact();
    return (InWorkspaceRoot / ".kano" / "cache" / "git" / "plans" /
            ("plan-" + stamp + "-" + headShort + ".json"))
        .lexically_normal();
}

auto BuildCommitPlanTemplateJson(const std::string& InGeneratedAtUtc) -> std::string {
    std::ostringstream oss;
    oss << R"json({
  "meta": {
    "schema_version": "2",
    "plan_id": "replace-with-unique-id",
    "generated_at_utc": ")json" << InGeneratedAtUtc << R"json(",
    "executed_at_utc": "",
    "base_head_sha": "replace-with-head-sha",
    "dirty_fingerprint": "replace-with-dirty-fingerprint",
    "planner": {
      "provider": "human",
      "ai-model": ""
    },
    "review": {
      "verdict": "pass",
      "reason": "replace-with-review-summary"
    }
  },
  "stages": {
    "commit": [
      {
        "repo": ".",
        "commits": [
          { "message": "feat(scope): replace-with-commit-message" }
        ]
      }
    ],
    "post_sync": []
  }
}
)json";
    return oss.str();
}

auto WriteTextFile(const std::filesystem::path& InPath,
                   const std::string& InText,
                   std::string* OutError) -> bool {
    std::error_code ec;
    std::filesystem::create_directories(InPath.parent_path(), ec);
    if (ec) {
        if (OutError != nullptr) {
            *OutError = "cannot create parent directories";
        }
        return false;
    }

    std::ofstream out(InPath, std::ios::trunc | std::ios::binary);
    if (!out) {
        if (OutError != nullptr) {
            *OutError = "cannot open output file";
        }
        return false;
    }
    out << InText;
    if (!out.good()) {
        if (OutError != nullptr) {
            *OutError = "failed while writing output file";
        }
        return false;
    }
    return true;
}

auto StampCommitPlanExecutedAt(const std::filesystem::path& InPath,
                               std::string* OutError) -> bool {
    std::ifstream in(InPath, std::ios::in | std::ios::binary);
    if (!in) {
        if (OutError != nullptr) {
            *OutError = "cannot open plan file";
        }
        return false;
    }

    std::ostringstream buffer;
    buffer << in.rdbuf();
    auto text = buffer.str();
    if (text.empty()) {
        if (OutError != nullptr) {
            *OutError = "plan file is empty";
        }
        return false;
    }

    const auto executedAt = CurrentUtcTimestampIso8601();
    const std::regex executedPattern(R"("executed_at_utc"\s*:\s*"[^"]*")");
    if (std::regex_search(text, executedPattern)) {
        text = std::regex_replace(
            text,
            executedPattern,
            std::string("\"executed_at_utc\": \"") + executedAt + "\"",
            std::regex_constants::format_first_only);
    } else {
        const std::regex metaPattern(R"("meta"\s*:\s*\{)");
        if (std::regex_search(text, metaPattern)) {
            text = std::regex_replace(
                text,
                metaPattern,
                std::string("\"meta\": {\n    \"executed_at_utc\": \"") + executedAt + "\",",
                std::regex_constants::format_first_only);
        } else {
            if (OutError != nullptr) {
                *OutError = "cannot locate meta object in plan";
            }
            return false;
        }
    }

    return WriteTextFile(InPath, text, OutError);
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

auto RunPipelineSafetyGatesForNonAiCommitPush() -> void {
    const auto ignoreGateResult = RunKogCommand({"plan", "verify", "ignore", "--context", "commit-push"});
    if (ignoreGateResult.exitCode != 0) {
        std::exit(ignoreGateResult.exitCode);
    }

    const auto secretGateProbe = RunKogCommand({"plan", "verify", "secret", "--help"}, shell::ExecMode::Capture);
    if (secretGateProbe.exitCode != 0) {
        std::cerr << "Warning: native binary does not support plan verify secret yet; skipping secret gate.\n";
        return;
    }
    const auto secretGateResult = RunKogCommand({"plan", "verify", "secret", "--context", "commit-push", "--limit", "20"});
    if (secretGateResult.exitCode != 0) {
        std::exit(secretGateResult.exitCode);
    }
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
    auto* cmd = InApp.add_subcommand("commit-push", "Run pre-commit, commit, sync, post-sync, then push in one command");
    cmd->allow_extras();

    auto* repos = new std::string{};
    auto* noRecursive = new bool{false};
    auto* message = new std::string{};
    auto* commitPlanFile = new std::string{};
    auto* writeCommitPlanTemplate = new bool{false};
    auto* commitPlanOut = new std::string{};
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
    cmd->add_option("--plan-file", *commitPlanFile, "Plan JSON file (stage-aware)");
    cmd->add_flag("--write-plan-template", *writeCommitPlanTemplate, "Write plan template JSON and exit");
    cmd->add_option("--plan-out", *commitPlanOut, "Template output path (default: .kano/cache/git/plans/plan-<utc>-<head>.json)");
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
        long long postSyncMillis = 0;
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

        const auto workspaceRoot = std::filesystem::current_path().lexically_normal();
        if (*writeCommitPlanTemplate) {
            const auto outPath = commitPlanOut->empty()
                ? DefaultCommitPlanOutputPath(workspaceRoot)
                : std::filesystem::path(NormalizeInputPathForCurrentPlatform(*commitPlanOut)).lexically_normal();
            std::string error;
            if (!WriteTextFile(outPath, BuildCommitPlanTemplateJson(CurrentUtcTimestampIso8601()), &error)) {
                std::cerr << "Error: failed to write plan template to " << outPath.generic_string();
                if (!error.empty()) {
                    std::cerr << " (" << error << ")";
                }
                std::cerr << "\n";
                std::exit(2);
            }
            std::cout << "Wrote plan template: " << outPath.generic_string() << "\n";
            std::exit(0);
        }
        if (!commitPlanOut->empty()) {
            std::cerr << "Error: --plan-out requires --write-plan-template\n";
            std::exit(2);
        }

        const bool agentMode = IsAgentModeEnabled();
        const auto normalizedCommitPlanFile = NormalizeInputPathForCurrentPlatform(*commitPlanFile);
        const bool hasCommitPlan = !normalizedCommitPlanFile.empty();
        const bool useAiCommitFlow = !hasCommitPlan;
        const bool aiModeRequested = *aiAuto || !aiProvider->empty() || !aiModel->empty();

        if (hasCommitPlan) {
            std::string stampError;
            const auto planPath = std::filesystem::path(normalizedCommitPlanFile).lexically_normal();
            if (!StampCommitPlanExecutedAt(planPath, &stampError)) {
                std::cerr << "Warning: failed to stamp plan executed_at_utc: "
                          << planPath.generic_string();
                if (!stampError.empty()) {
                    std::cerr << " (" << stampError << ")";
                }
                std::cerr << "\n";
            }
        }

        if (agentMode && hasCommitPlan) {
            std::cout << "[commit-push] agent mode + --plan-file detected; using plan-driven flow.\n";
        }

        if (!aiModeRequested) {
            std::cout << "=== commit-push stage: safety-gates ===\n";
            RunPipelineSafetyGatesForNonAiCommitPush();
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
        if (hasCommitPlan) {
            commitArgs.insert(commitArgs.end(), {"--plan-file", normalizedCommitPlanFile, "--plan-stage", "commit"});
        }
        if (useAiCommitFlow && !aiProvider->empty()) {
            commitArgs.insert(commitArgs.end(), {"--ai-provider", *aiProvider});
        }
        if (useAiCommitFlow && !aiModel->empty()) {
            commitArgs.insert(commitArgs.end(), {"--ai-model", *aiModel});
        }
        if (useAiCommitFlow && *aiAuto) {
            commitArgs.push_back("--ai-auto");
        }
        if (useAiCommitFlow && *noAiReview) {
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
        if (hasCommitPlan) {
            postCommitArgs.insert(postCommitArgs.end(), {"--plan-file", normalizedCommitPlanFile, "--plan-stage", "post_sync"});
        }
        if (useAiCommitFlow && !aiProvider->empty()) {
            postCommitArgs.insert(postCommitArgs.end(), {"--ai-provider", *aiProvider});
        }
        if (useAiCommitFlow && !aiModel->empty()) {
            postCommitArgs.insert(postCommitArgs.end(), {"--ai-model", *aiModel});
        }
        if (useAiCommitFlow && *aiAuto) {
            postCommitArgs.push_back("--ai-auto");
        }
        if (useAiCommitFlow && *noAiReview) {
            postCommitArgs.push_back("--no-ai-review");
        }
        if (*stagedOnly) {
            postCommitArgs.push_back("--staged-only");
        }
        if (*dryRun) {
            postCommitArgs.push_back("--preflight-only");
        }

        std::cout << "=== commit-push stage: post-sync ===\n";
        const auto postCommitStart = std::chrono::steady_clock::now();
        const auto postCommitResult = RunKogCommand(postCommitArgs);
        const auto postCommitEnd = std::chrono::steady_clock::now();
        postSyncMillis = std::chrono::duration_cast<std::chrono::milliseconds>(postCommitEnd - postCommitStart).count();
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
            std::cout << "post_sync_ms: " << postSyncMillis << "\n";
            std::cout << "push_ms: " << pushMillis << "\n";
            std::cout << "total_ms: " << totalMillis << "\n";
        }

        std::exit(pushResult.exitCode);
    });
}

} // namespace kano::git::commands
