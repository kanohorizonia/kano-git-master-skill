#include "commit_auto_plan_pipeline.hpp"

#include "commit_ai_utils.hpp"
#include "commit_plan_payload.hpp"
#include "plan_utils.hpp"
#include "shell_executor.hpp"
#include "command_runtime_ops.hpp"
#include "runtime_path_layout.hpp"
#include "kog_timing.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <optional>
#include <regex>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace kano::git::commands {

auto RunAmendNativePlanStage(const std::filesystem::path& InWorkspaceRoot,
                             const std::string& InPlanFile,
                             const std::string& InPlanStage,
                             bool InProfile) -> int;

auto DefaultSharedPlanPath(const std::filesystem::path& InWorkspaceRoot) -> std::filesystem::path {
    return runtime_path::Layout::Resolve(InWorkspaceRoot).SharedPlanPath();
}

namespace {

auto ResolveSelfBinaryCommand() -> std::string {
    if (const char* binaryPath = std::getenv("KANO_GIT_BINARY_PATH"); binaryPath != nullptr) {
        const std::filesystem::path p(binaryPath);
        if (std::filesystem::exists(p)) {
            return p.generic_string();
        }
    }
#if defined(_WIN32)
    // Try to find kano-git.exe in the same directory as the current executable
    char selfPath[MAX_PATH];
    if (GetModuleFileNameA(NULL, selfPath, MAX_PATH) > 0) {
        std::filesystem::path self(selfPath);
        std::filesystem::path candidate = self.parent_path() / "kano-git.exe";
        if (std::filesystem::exists(candidate)) {
            return candidate.generic_string();
        }
    }
    return "kano-git.exe";
#else
    return "kano-git";
#endif
}

void EmitCapturedSelfResult(const shell::ExecResult& InResult) {
    if (!InResult.stdoutStr.empty()) {
        std::cout << InResult.stdoutStr;
        if (InResult.stdoutStr.back() != '\n') {
            std::cout << '\n';
        }
    }
    if (!InResult.stderrStr.empty()) {
        std::cerr << InResult.stderrStr;
        if (InResult.stderrStr.back() != '\n') {
            std::cerr << '\n';
        }
    }
}

auto FinalizeNestedSelfResult(const char* InLabel, const shell::ExecResult& InResult) -> int {
    EmitCapturedSelfResult(InResult);
    const auto stderrLower = ToLower(InResult.stderrStr);
    if (stderrLower.find("find_binary: command not found") != std::string::npos) {
        std::cerr << "Error: " << InLabel
                  << " hit nested launcher shell failure (`find_binary` leaked into sh); aborting.\n";
        return InResult.exitCode != 0 ? InResult.exitCode : 127;
    }
    return InResult.exitCode;
}

struct CommitRunbookResult {
    int exitCode = 0;
    std::optional<long long> aiFillMillis;
    bool fallbackUsed = false;  // true if fallback commits were injected after AI fill failed
};

auto ExtractPlanAiFillMillis(const shell::ExecResult& InResult) -> std::optional<long long> {
    std::istringstream iss(InResult.stdoutStr + "\n" + InResult.stderrStr);
    std::string line;
    while (std::getline(iss, line)) {
        const auto trimmed = Trim(line);
        constexpr std::string_view kPrefix = "[plan] ai_fill_ms:";
        if (!trimmed.starts_with(kPrefix)) {
            continue;
        }
        const auto value = Trim(trimmed.substr(kPrefix.size()));
        if (value.empty() || value == "n/a") {
            return std::nullopt;
        }
        try {
            return std::stoll(value);
        } catch (const std::exception&) {
        }
    }
    return std::nullopt;
}

auto ExtractFallbackUsed(const shell::ExecResult& InResult) -> bool {
    const auto combined = InResult.stdoutStr + "\n" + InResult.stderrStr;
    return combined.find("[plan] fallback_used: true") != std::string::npos;
}

} // namespace

auto RunPlanNewViaSelf(const std::filesystem::path& InWorkspaceRoot,
                       const std::filesystem::path& InPlanPath) -> int {
    const auto selfCmd = ResolveSelfBinaryCommand();
    std::vector<std::string> args = {
        "plan", "new",
        "--force",
        "--output", InPlanPath.generic_string(),
    };
    if (IsKogDebugEnabled()) {
        std::cerr << "[DEBUG] RunPlanNewViaSelf: self=" << selfCmd << "\n";
        std::cerr << "[DEBUG] RunPlanNewViaSelf: workspace=" << InWorkspaceRoot.generic_string() << "\n";
        std::cerr << "[DEBUG] RunPlanNewViaSelf: plan_path=" << InPlanPath.generic_string() << "\n";
        for (std::size_t i = 0; i < args.size(); ++i) {
            std::cerr << "[DEBUG] RunPlanNewViaSelf: args[" << i << "]=" << args[i] << "\n";
        }
    }
    const auto result = shell::ExecuteCommand(selfCmd, args, shell::ExecMode::Capture, InWorkspaceRoot);
    if (IsKogDebugEnabled()) {
        std::cerr << "[DEBUG] RunPlanNewViaSelf: exitCode=" << result.exitCode << "\n";
        std::cerr << "[DEBUG] RunPlanNewViaSelf: stdout=" << result.stdoutStr << "\n";
        std::cerr << "[DEBUG] RunPlanNewViaSelf: stderr=" << result.stderrStr << "\n";
    }
    const auto exitCode = FinalizeNestedSelfResult("plan new", result);
    if (exitCode != 0) {
        std::cerr << "Error: plan new failed via native binary (exit=" << exitCode << ").\n";
    }
    return exitCode;
}

auto RunCommitSeedViaSelf(const std::filesystem::path& InWorkspaceRoot,
                          const std::filesystem::path& InPlanPath) -> int {
    std::vector<std::string> args = {
        "plan", "commit-seed",
        "--force",
        "--deterministic",  // Generate actual messages, not placeholders (needed for --combine mode)
        "--plan-file", InPlanPath.generic_string(),
    };
    const auto result = shell::ExecuteCommand(ResolveSelfBinaryCommand(), args, shell::ExecMode::Capture, InWorkspaceRoot);
    const auto exitCode = FinalizeNestedSelfResult("commit-seed", result);
    if (exitCode != 0) {
        std::cerr << "Error: commit-seed failed via native binary (exit=" << exitCode << ").\n";
    }
    return exitCode;
}

namespace {

auto RunIgnorePlanRunbookViaSelf(const std::filesystem::path& InWorkspaceRoot,
                                 const std::filesystem::path& InPlanPath) -> int {
    if (IsKogDebugEnabled()) {
        std::cerr << "[DEBUG] RunIgnorePlanRunbookViaSelf ENTERED" << std::endl;
    }
    std::vector<std::string> args = {
        "plan", "runbook", "ignore",
        "--force",
        "--plan-file", InPlanPath.generic_string(),
    };
    const auto result = shell::ExecuteCommand(ResolveSelfBinaryCommand(), args, shell::ExecMode::Capture, InWorkspaceRoot);
    if (result.exitCode != 0) {
        const auto combinedOutput = result.stdoutStr + "\n" + result.stderrStr;

        static const std::regex driftRegex("state drift", std::regex_constants::icase);
        static const std::regex entriesRegex("ignore plan entries", std::regex_constants::icase);

        if (std::regex_search(combinedOutput, driftRegex) ||
            std::regex_search(combinedOutput, entriesRegex)) {
            EmitCapturedSelfResult(result);
            std::cout << "[native-commit] ignore runbook: no artifact candidates or plan already up-to-date; skipping.\n";
            return 0;
        }
    }
    const auto exitCode = FinalizeNestedSelfResult("ignore runbook", result);
    if (exitCode != 0) {
        std::cerr << "Error: ignore runbook failed via native binary (exit=" << exitCode << ").\n";
    }
    return exitCode;
}

auto RunIgnorePlanApplyViaSelf(const std::filesystem::path& InWorkspaceRoot,
                               const std::filesystem::path& InPlanPath) -> int {
    std::vector<std::string> args = {
        "plan", "apply", "--stage", "ignore",
        "--plan-file", InPlanPath.generic_string(),
    };
    const auto result = shell::ExecuteCommand(ResolveSelfBinaryCommand(), args, shell::ExecMode::Capture, InWorkspaceRoot);
    const auto combinedOutput = result.stdoutStr + "\n" + result.stderrStr;
    static const std::regex entriesRegex("ignore plan entries", std::regex_constants::icase);

    if (result.exitCode != 0 &&
        std::regex_search(combinedOutput, entriesRegex)) {
        EmitCapturedSelfResult(result);
        std::cout << "[native-commit] ignore plan stage is empty; skipping ignore apply.\n";
        return 0;
    }
    const auto exitCode = FinalizeNestedSelfResult("ignore apply", result);
    if (exitCode != 0) {
        std::cerr << "Error: ignore apply failed via native binary (exit=" << exitCode << ").\n";
    }
    return exitCode;
}

auto RunCommitPlanRunbookViaSelf(const std::filesystem::path& InWorkspaceRoot,
                                 const std::filesystem::path& InPlanPath,
                                 const std::string& InProvider,
                                 const std::string& InModel,
                                 const std::string& InFillMode,
                                 bool InAllowEmptyDirty,
                                 bool InYolo) -> CommitRunbookResult {
    std::vector<std::string> args = {
        "plan", "runbook", "commit",
        "--plan-file", InPlanPath.generic_string(),
        "--ai-provider", InProvider.empty() ? "auto" : InProvider,
    };
    if (!InModel.empty()) {
        args.push_back("--ai-model");
        args.push_back(InModel);
    }
    if (!InFillMode.empty()) {
        args.push_back("--ai-fill-mode");
        args.push_back(InFillMode);
    }
    if (InAllowEmptyDirty) {
        args.push_back("--allow-empty-dirty");
    }
    if (InYolo) {
        args.push_back("--yolo");
    }

    const auto binary = ResolveSelfBinaryCommand();
    std::cout << "[plan] invoking internal AI runbook: " << binary;
    for (const auto& a : args) {
        if (a.find(' ') != std::string::npos || a.empty()) std::cout << " \"" << a << "\"";
        else std::cout << " " << a;
    }
    std::cout << std::endl;

    const auto result = shell::ExecuteCommand(binary, args, shell::ExecMode::Capture, InWorkspaceRoot);
    CommitRunbookResult out;
    out.aiFillMillis = ExtractPlanAiFillMillis(result);
    out.fallbackUsed = ExtractFallbackUsed(result);
    const auto exitCode = FinalizeNestedSelfResult("AI commit runbook", result);
    out.exitCode = exitCode;
    if (exitCode != 0) {
        std::cerr << "Error: AI commit runbook failed via native binary (exit=" << exitCode << ").\n";
    }
    return out;
}

} // namespace

auto RunCommitAutoPlanPipeline(const std::filesystem::path& InWorkspaceRoot,
                               const NativeAiConfig& InAi,
                               const std::string& InAiFillMode,
                               const bool InProfile,
                               const bool InAllowEmptyDirty) -> int {
    using clock = std::chrono::steady_clock;
    const auto totalStart = std::chrono::steady_clock::now();
    long long planNewMillis = 0;
    long long ignoreRunbookMillis = 0;
    long long ignoreApplyMillis = 0;
    long long commitRunbookMillis = 0;
    long long preCommitMillis = 0;
    long long commitApplyMillis = 0;
    std::optional<long long> aiFillMillis;

    if (IsKogDebugEnabled()) {
        std::cerr << "[DEBUG] RunCommitAutoPlanPipeline ENTERED" << std::endl;
        std::cerr << "[DEBUG] InWorkspaceRoot=" << InWorkspaceRoot.generic_string() << std::endl;
        std::cerr << "[DEBUG] InAi.enabled=" << InAi.enabled << " provider=" << InAi.provider << " model=" << InAi.model << std::endl;
        std::cerr << "[DEBUG] InAiFillMode=" << InAiFillMode << std::endl;
        std::cerr << "[DEBUG] InProfile=" << InProfile << std::endl;
    }
    std::cerr.flush();

    const auto autoPlanPath = DefaultSharedPlanPath(InWorkspaceRoot);
    std::cout << "[native-commit] auto-plan file: " << autoPlanPath.generic_string() << "\n";

    const auto planNewStart = std::chrono::steady_clock::now();
    const auto planNewCode = RunPlanNewViaSelf(InWorkspaceRoot, autoPlanPath);
    planNewMillis = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - planNewStart).count();
    if (planNewCode != 0) {
        return planNewCode;
    }

    std::cout << "[plan] initializing ignore stages...\n";
    const auto ignoreRunbookStart = std::chrono::steady_clock::now();
    const auto ignoreRunbookCode = RunIgnorePlanRunbookViaSelf(InWorkspaceRoot, autoPlanPath);
    ignoreRunbookMillis = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - ignoreRunbookStart).count();
    if (ignoreRunbookCode != 0) {
        return ignoreRunbookCode;
    }

    std::cout << "[plan] applying ignore rules...\n";
    const auto ignoreApplyStart = std::chrono::steady_clock::now();
    const auto ignoreApplyCode = RunIgnorePlanApplyViaSelf(InWorkspaceRoot, autoPlanPath);
    ignoreApplyMillis = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - ignoreApplyStart).count();
    if (ignoreApplyCode != 0) {
        return ignoreApplyCode;
    }

    if (const auto seedCode = RunCommitSeedViaSelf(InWorkspaceRoot, autoPlanPath); seedCode != 0) {
        return seedCode;
    }

    CommitRunbookResult runbookResult;
    {
        double elapsed = 0.0;
        KOG_SCOPED_TIMING_LOG_WITH_ELAPSED("plan-utils.preparing-commit-plan-via-ai", elapsed);
        runbookResult = RunCommitPlanRunbookViaSelf(InWorkspaceRoot, autoPlanPath, InAi.provider, InAi.model, InAiFillMode, InAllowEmptyDirty, InAi.yolo);
        commitRunbookMillis = static_cast<long long>(elapsed);
        aiFillMillis = runbookResult.aiFillMillis;
        if (runbookResult.exitCode != 0) {
            return runbookResult.exitCode;
        }
    }

    std::string deterministicReason;
    // Skip deterministic check if fallback commits were used (fallback has provider=native model=deterministic metadata)
    if (!runbookResult.fallbackUsed && HumanAutoPlanLooksDeterministic(autoPlanPath, &deterministicReason)) {
        std::cerr << "Error: AI commit runbook produced non-AI deterministic plan metadata; refusing to continue.\n";
        std::cerr << "Hint: verify AI provider/auth and rerun plain `kog commit --ai-auto`.\n";
        std::cerr << "Hint: deterministic metadata: " << deterministicReason << "\n";
        return 2;
    }

    const auto preCommitStart = std::chrono::steady_clock::now();
    const auto preCommitCode = RunSyncPreCommitNative(InWorkspaceRoot, true, false, "default");
    preCommitMillis = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - preCommitStart).count();
    if (preCommitCode != 0) {
        return preCommitCode;
    }

    // Refresh plan workspace hashes after pre-commit repair:
    // RunSyncPreCommitNative may write to .gitmodules (branch bindings), which changes
    // the workspace dirty fingerprint and base HEAD SHA captured in the plan.
    // Updating them here prevents a spurious workspace-state-drift failure in commit apply.
    if (!RefreshPlanWorkspaceHashes(autoPlanPath, InWorkspaceRoot)) {
        std::cerr << "Warning: failed to refresh plan workspace hashes after pre-commit repair; drift check may trigger.\n";
    }

    const auto commitApplyStart = std::chrono::steady_clock::now();
    const auto commitApplyCode = RunCommitNativePlanStage(InWorkspaceRoot, autoPlanPath.generic_string(), "commit", false, true);
    commitApplyMillis = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - commitApplyStart).count();
    if (commitApplyCode != 0) {
        return commitApplyCode;
    }

    if (InProfile) {
        const auto totalMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - totalStart).count();
        std::cout << "\n=== Commit Auto-Plan Profile Summary ===\n";
        std::cout << "mode: plan-first\n";
        std::cout << "plan_new_ms: " << planNewMillis << "\n";
        std::cout << "ignore_runbook_ms: " << ignoreRunbookMillis << "\n";
        std::cout << "ignore_apply_ms: " << ignoreApplyMillis << "\n";
        std::cout << "commit_runbook_ms: " << commitRunbookMillis << "\n";
        if (aiFillMillis.has_value()) {
            std::cout << "ai_fill_ms: " << *aiFillMillis << "\n";
        } else {
            std::cout << "ai_fill_ms: n/a\n";
        }
        std::cout << "pre_commit_ms: " << preCommitMillis << "\n";
        std::cout << "commit_apply_ms: " << commitApplyMillis << "\n";
        std::cout << "total_ms: " << totalMs << "\n";
    }

    return 0;
}

auto RunAmendAutoPlanPipeline(const std::filesystem::path& InWorkspaceRoot,
                               const NativeAiConfig& InAi,
                               const std::string& InAiFillMode,
                               const bool InProfile,
                               const bool InAllowEmptyDirty) -> int {
    using clock = std::chrono::steady_clock;
    const auto totalStart = std::chrono::steady_clock::now();
    long long planNewMillis = 0;
    long long ignoreRunbookMillis = 0;
    long long ignoreApplyMillis = 0;
    long long commitRunbookMillis = 0;
    long long amendApplyMillis = 0;
    std::optional<long long> aiFillMillis;

    if (IsKogDebugEnabled()) {
        std::cerr << "[DEBUG] RunAmendAutoPlanPipeline ENTERED" << std::endl;
        std::cerr << "[DEBUG] InWorkspaceRoot=" << InWorkspaceRoot.generic_string() << std::endl;
        std::cerr << "[DEBUG] InAi.enabled=" << InAi.enabled << " provider=" << InAi.provider << " model=" << InAi.model << std::endl;
        std::cerr << "[DEBUG] InAiFillMode=" << InAiFillMode << std::endl;
        std::cerr << "[DEBUG] InProfile=" << InProfile << std::endl;
    }
    std::cerr.flush();

    const auto autoPlanPath = DefaultSharedPlanPath(InWorkspaceRoot);
    std::cout << "[native-amend] auto-plan file: " << autoPlanPath.generic_string() << "\n";

    const auto planNewStart = std::chrono::steady_clock::now();
    const auto planNewCode = RunPlanNewViaSelf(InWorkspaceRoot, autoPlanPath);
    planNewMillis = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - planNewStart).count();
    if (planNewCode != 0) {
        return planNewCode;
    }

    std::cout << "[plan] initializing ignore stages...\n";
    const auto ignoreRunbookStart = std::chrono::steady_clock::now();
    const auto ignoreRunbookCode = RunIgnorePlanRunbookViaSelf(InWorkspaceRoot, autoPlanPath);
    ignoreRunbookMillis = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - ignoreRunbookStart).count();
    if (ignoreRunbookCode != 0) {
        return ignoreRunbookCode;
    }

    std::cout << "[plan] applying ignore rules...\n";
    const auto ignoreApplyStart = std::chrono::steady_clock::now();
    const auto ignoreApplyCode = RunIgnorePlanApplyViaSelf(InWorkspaceRoot, autoPlanPath);
    ignoreApplyMillis = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - ignoreApplyStart).count();
    if (ignoreApplyCode != 0) {
        return ignoreApplyCode;
    }

    if (const auto seedCode = RunCommitSeedViaSelf(InWorkspaceRoot, autoPlanPath); seedCode != 0) {
        return seedCode;
    }

    CommitRunbookResult runbookResult;
    {
        double elapsed = 0.0;
        KOG_SCOPED_TIMING_LOG_WITH_ELAPSED("plan-utils.preparing-commit-plan-via-ai", elapsed);
        runbookResult = RunCommitPlanRunbookViaSelf(InWorkspaceRoot, autoPlanPath, InAi.provider, InAi.model, InAiFillMode, InAllowEmptyDirty, InAi.yolo);
        commitRunbookMillis = static_cast<long long>(elapsed);
        aiFillMillis = runbookResult.aiFillMillis;
        if (runbookResult.exitCode != 0) {
            return runbookResult.exitCode;
        }
    }

    std::string deterministicReason;
    if (!runbookResult.fallbackUsed && HumanAutoPlanLooksDeterministic(autoPlanPath, &deterministicReason)) {
        std::cerr << "Error: AI commit runbook produced non-AI deterministic plan metadata; refusing to continue.\n";
        std::cerr << "Hint: verify AI provider/auth and rerun plain `kog amend --ai-auto`.\n";
        std::cerr << "Hint: deterministic metadata: " << deterministicReason << "\n";
        return 2;
    }

    // Apply plan via soft-reset + commit (rebuild history) instead of amend
    const auto amendApplyStart = std::chrono::steady_clock::now();
    const auto amendApplyCode = RunAmendNativePlanStage(InWorkspaceRoot, autoPlanPath.generic_string(), "commit", false);
    amendApplyMillis = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - amendApplyStart).count();
    if (amendApplyCode != 0) {
        return amendApplyCode;
    }

    if (InProfile) {
        const auto totalMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - totalStart).count();
        std::cout << "\n=== Amend Auto-Plan Profile Summary ===\n";
        std::cout << "mode: plan-first\n";
        std::cout << "plan_new_ms: " << planNewMillis << "\n";
        std::cout << "ignore_runbook_ms: " << ignoreRunbookMillis << "\n";
        std::cout << "ignore_apply_ms: " << ignoreApplyMillis << "\n";
        std::cout << "commit_runbook_ms: " << commitRunbookMillis << "\n";
        if (aiFillMillis.has_value()) {
            std::cout << "ai_fill_ms: " << *aiFillMillis << "\n";
        } else {
            std::cout << "ai_fill_ms: n/a\n";
        }
        std::cout << "amend_apply_ms: " << amendApplyMillis << "\n";
        std::cout << "total_ms: " << totalMs << "\n";
    }

    return 0;
}

} // namespace kano::git::commands
