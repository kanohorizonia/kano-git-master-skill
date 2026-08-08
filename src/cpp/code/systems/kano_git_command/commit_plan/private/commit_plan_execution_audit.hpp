#pragma once

#include "commit_plan_audit.hpp"
#include "commit_plan_payload.hpp"

#include <filesystem>
#include <memory>
#include <string>

namespace kano::git::commands {

auto ReserveCommitPlanAuditOrReport(
    const std::filesystem::path& InWorkspaceRoot,
    const std::filesystem::path& InPlanPath,
    std::string* OutError) -> std::unique_ptr<PlanAuditSink>;

auto FinalizeCommitPlanAuditOrReport(
    PlanAuditSink& InAudit,
    int InExitCode) -> int;

auto RunAuditedPlanPreCommit(
    PlanAuditSink& InAudit,
    const std::filesystem::path& InWorkspaceRoot,
    CommitPlanStage InStage,
    bool InRunPreCommit,
    bool InRecursive,
    std::string* OutError) -> int;

auto AppendCommitMutationAuditOrReport(
    PlanAuditSink& InAudit,
    const std::filesystem::path& InRepo,
    const audit::RepositoryState& InBefore,
    const std::string& InStartedAtUtc,
    bool InFailed,
    std::string* OutError) -> bool;

auto ValidateUnauditedAmendPlanOrReport(
    const std::filesystem::path& InPlanPath) -> bool;

} // namespace kano::git::commands
