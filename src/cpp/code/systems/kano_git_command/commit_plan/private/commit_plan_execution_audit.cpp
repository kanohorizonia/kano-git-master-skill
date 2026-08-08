#include "commit_plan_execution_audit.hpp"

#include "command_runtime_ops.hpp"
#include "commit_ai_utils.hpp"
#include "commit_plan_payload_parser.hpp"

#include <chrono>
#include <ctime>
#include <iostream>
#include <optional>
#include <utility>
#include <vector>

namespace kano::git::commands {
namespace {

auto CurrentUtcIso8601() -> std::string {
    const auto now = std::chrono::system_clock::now();
    const auto time = std::chrono::system_clock::to_time_t(now);
    std::tm utc{};
#if defined(_WIN32)
    gmtime_s(&utc, &time);
#else
    gmtime_r(&time, &utc);
#endif
    char buffer[32]{};
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &utc);
    return buffer;
}

} // namespace

auto ReserveCommitPlanAuditOrReport(
    const std::filesystem::path& InWorkspaceRoot,
    const std::filesystem::path& InPlanPath,
    std::string* OutError) -> std::unique_ptr<PlanAuditSink> {
    auto audit = PlanAuditSink::Reserve(
        InWorkspaceRoot, InPlanPath, OutError, "commit.plan");
    if (!audit) {
        std::cerr << "Error: cannot reserve commit audit evidence";
        if (OutError != nullptr && !OutError->empty())
            std::cerr << " (" << *OutError << ")";
        std::cerr << "\n";
    }
    return audit;
}

auto FinalizeCommitPlanAuditOrReport(
    PlanAuditSink& InAudit,
    const int InExitCode) -> int {
    std::string terminalError;
    if (InAudit.Finalize(InExitCode, &terminalError)) return InExitCode;
    std::cerr << "Error: failed to publish terminal commit audit evidence";
    if (!terminalError.empty()) std::cerr << " (" << terminalError << ")";
    std::cerr << "\n";
    return InExitCode == 0 ? 2 : InExitCode;
}

auto RunAuditedPlanPreCommit(
    PlanAuditSink& InAudit,
    const std::filesystem::path& InWorkspaceRoot,
    const CommitPlanStage InStage,
    const bool InRunPreCommit,
    const bool InRecursive,
    std::string* OutError) -> int {
    if (!InRunPreCommit || !PlanStageNeedsPreCommit(InStage)) return 0;

    auto observedRepos = BuildCommitScopeRecords(
        InWorkspaceRoot, "", !InRecursive, false);
    if (observedRepos.empty()) {
        workspace::RepoRecord fallback;
        fallback.path = InWorkspaceRoot;
        fallback.type = "root";
        observedRepos.push_back(std::move(fallback));
    }
    std::vector<audit::RepositoryState> beforeStates;
    beforeStates.reserve(observedRepos.size());
    for (const auto& repo : observedRepos)
        beforeStates.push_back(InAudit.Capture(repo.path));

    const auto startedAtUtc = CurrentUtcIso8601();
    const auto preCommitCode = RunSyncPreCommitNative(
        InWorkspaceRoot, InRecursive, false, "default");
    for (std::size_t index = 0; index < observedRepos.size(); ++index) {
        if (InAudit.Append("sync.pre-commit", observedRepos[index].path,
                           beforeStates[index], startedAtUtc, preCommitCode,
                           OutError)) {
            continue;
        }
        std::cerr << "Error: failed to persist pre-commit audit event";
        if (OutError != nullptr && !OutError->empty())
            std::cerr << " (" << *OutError << ")";
        std::cerr << "\n";
        return 2;
    }
    return preCommitCode;
}

auto AppendCommitMutationAuditOrReport(
    PlanAuditSink& InAudit,
    const std::filesystem::path& InRepo,
    const audit::RepositoryState& InBefore,
    const std::string& InStartedAtUtc,
    const bool InFailed,
    std::string* OutError) -> bool {
    if (InAudit.Append("commit.apply", InRepo, InBefore, InStartedAtUtc,
                       InFailed ? 1 : 0, OutError)) {
        return true;
    }
    std::cerr << "Error: failed to persist commit mutation audit event";
    if (OutError != nullptr && !OutError->empty())
        std::cerr << " (" << *OutError << ")";
    std::cerr << "\n";
    return false;
}

auto ValidateUnauditedAmendPlanOrReport(
    const std::filesystem::path& InPlanPath) -> bool {
    std::string error;
    const auto planBytes = ReadBoundedAuditInput(InPlanPath, 4U << 20U, &error);
    const auto parsed = planBytes
        ? ParseCommitPlanText(*planBytes, &error)
        : std::nullopt;
    if (!parsed || !ValidateCommitPlanCorrelation(*parsed, &error)) {
        std::cerr << "Error: invalid --plan-file before amend preflight";
        if (!error.empty()) std::cerr << " (" << error << ")";
        std::cerr << "\n";
        return false;
    }
    if (parsed->meta.correlation.present &&
        parsed->meta.correlation.mode == "koa") {
        std::cerr << "Error: KOA-correlated plans are unsupported by the unaudited amend.plan route\n";
        std::cerr << "Hint: use an advertised audited mutation route; amend.plan does not preserve audit provenance.\n";
        return false;
    }
    return true;
}

} // namespace kano::git::commands
