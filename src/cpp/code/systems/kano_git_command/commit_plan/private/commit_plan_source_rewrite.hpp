#pragma once

#include <filesystem>
#include <string>

namespace kano::git::commands {

class PlanAuditSink;

enum class PlanSourceRewritePhase {
    ClearExecutionStamp,
    WriteCompletionStamp,
};

// Rewrites a plan only when one no-follow handle still names a bounded regular
// file whose exact bytes equal InExpectedBytes.  Validation and mutation use
// that same handle, and failures are recorded as plan.source.revalidate.
// POSIX locks are advisory, so every plan writer must cooperate with flock for
// this conditional guarantee. Observed drift fails closed; this API does not
// claim a linearizable CAS against a hostile writer that ignores flock.
auto RewriteAuditedPlanSourceConditionally(
    PlanAuditSink& InAudit,
    const std::filesystem::path& InWorkspaceRoot,
    const std::filesystem::path& InPlanPath,
    const std::string& InExpectedBytes,
    const std::string& InReplacementBytes,
    PlanSourceRewritePhase InPhase,
    std::string* OutError) -> bool;

auto RestorePlanSourceBytesConditionally(
    const std::filesystem::path& InPlanPath,
    const std::string& InExpectedStampedBytes,
    const std::string& InOriginalAdmittedBytes,
    std::string* OutError) -> bool;

} // namespace kano::git::commands
