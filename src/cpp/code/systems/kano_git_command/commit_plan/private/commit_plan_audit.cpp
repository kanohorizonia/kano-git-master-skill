#include "commit_plan_audit.hpp"

#include "commit_plan_payload_parser.hpp"
#include "plan_utils.hpp"

#include <nlohmann/json.hpp>

namespace kano::git::commands {
namespace {

auto ToOperationCorrelation(const CommitPlanPayload::Meta::Correlation& In)
    -> OperationCorrelationEnvelope {
    OperationCorrelationEnvelope out;
    out.mode = In.mode;
    out.productId = In.productId;
    out.topicId = In.topicId;
    out.itemId = In.itemId;
    out.workOrderId = In.workOrderId;
    out.requestId = In.requestId;
    out.runId = In.runId;
    out.parentRunId = In.parentRunId;
    out.producerId = In.producerId;
    out.routeId = In.routeId;
    out.attempt = In.attempt;
    return out;
}

auto ResolvedFrozenPlan(const std::string& InSourceBytes,
                        const CommitPlanPayload& InParsed,
                        const OperationCorrelationEnvelope& InCorrelation,
                        const std::string& InPlanId,
                        std::string* OutError) -> std::optional<std::string> {
    try {
        if (InParsed.meta.correlation.runId == InCorrelation.runId &&
            InParsed.meta.planId == InPlanId) {
            return InSourceBytes;
        }
        auto doc = nlohmann::json::parse(InSourceBytes);
        doc["meta"]["plan_id"] = InPlanId;
        doc["meta"]["correlation"] = nlohmann::json::parse(
            SerializeOperationCorrelationEnvelope(InCorrelation));
        const auto frozen = doc.dump(2) + '\n';
        const auto reparsed = ParseCommitPlanText(frozen, OutError);
        if (!reparsed || !ValidateCommitPlanCorrelation(*reparsed, OutError))
            return std::nullopt;
        return frozen;
    } catch (const std::exception& ex) {
        if (OutError) *OutError = ex.what();
        return std::nullopt;
    }
}

auto IsUnboundStandaloneCorrelation(const OperationCorrelationEnvelope& In) -> bool {
    return In.mode == "standalone" && In.productId.empty() && In.topicId.empty() &&
        In.itemId.empty() && In.workOrderId.empty() && In.requestId.empty() &&
        In.runId.empty() && In.parentRunId.empty() && In.producerId.empty() &&
        In.routeId.empty() && In.attempt == 1;
}

} // namespace

auto PlanAuditSink::Reserve(const std::filesystem::path& InWorkspaceRoot,
                            const std::filesystem::path& InPlanPath,
                            std::string* OutError,
                            std::string InRoute) -> std::unique_ptr<PlanAuditSink> {
    const auto sourceBytes = ReadBoundedAuditInput(InPlanPath, 4U << 20U, OutError);
    if (!sourceBytes) return nullptr;
    const auto parsed = ParseCommitPlanText(*sourceBytes, OutError);
    if (!parsed || !ValidateCommitPlanCorrelation(*parsed, OutError)) return nullptr;
    if (!audit::IsStableAuditId(parsed->meta.planId)) {
        if (OutError) *OutError = "plan id violates the stable-ID grammar";
        return nullptr;
    }

    auto sink = std::unique_ptr<PlanAuditSink>(new PlanAuditSink());
    auto correlation = ToOperationCorrelation(parsed->meta.correlation);
    sink->mSourceBytes = *sourceBytes;
    sink->mSourceSha256 = audit::Sha256Hex(*sourceBytes);
    sink->mPlanId = parsed->meta.planId;
    if (auto* active = OperationAuditContext::Current()) {
        if (!IsUnboundStandaloneCorrelation(correlation) &&
            correlation != active->Correlation()) {
            if (OutError) {
                *OutError = "nested commit plan correlation contradicts the active audit owner";
            }
            return nullptr;
        }
        correlation = active->Correlation();
        const auto frozen = ResolvedFrozenPlan(
            *sourceBytes, *parsed, correlation, parsed->meta.planId, OutError);
        if (!frozen) return nullptr;
        if (active->InputKind() == "commit-plan" &&
            audit::Sha256Hex(*sourceBytes) == active->SourceSha256()) {
            sink->mFrozenPlanPath = active->FrozenInputPath();
        } else {
            const auto supplemental = active->FreezeSupplementalBytes(
                *frozen, "commit-plan", parsed->meta.planId, OutError);
            if (!supplemental) return nullptr;
            sink->mFrozenPlanPath = *supplemental;
        }
        sink->mContext = active;
        return sink;
    }

    correlation = ResolveStandaloneCorrelation(
        correlation, parsed->meta.planId, audit::Sha256Hex(*sourceBytes));
    std::string correlationError;
    if (!ValidateOperationCorrelationEnvelope(correlation, true, &correlationError)) {
        if (OutError) *OutError = correlationError;
        return nullptr;
    }
    const auto frozen = ResolvedFrozenPlan(
        *sourceBytes, *parsed, correlation, parsed->meta.planId, OutError);
    if (!frozen) return nullptr;

    std::error_code ec;
    const auto canonicalPlan = std::filesystem::weakly_canonical(InPlanPath, ec);
    if (ec || canonicalPlan.empty()) {
        if (OutError) *OutError = "cannot resolve canonical plan identity";
        return nullptr;
    }
    OperationAuditSpec spec;
    spec.workspaceRoot = InWorkspaceRoot.lexically_normal();
    spec.sourcePath = InPlanPath;
    spec.inputIdentity = canonicalPlan.generic_string();
    spec.inputKind = "commit-plan";
    spec.route = std::move(InRoute);
    spec.planId = parsed->meta.planId;
    spec.sourceBytes = *sourceBytes;
    spec.frozenBytes = *frozen;
    spec.frozenFileName = "frozen-plan.json";
    spec.correlation = correlation;
    sink->mOwnedContext = OperationAuditContext::Reserve(std::move(spec), OutError);
    if (!sink->mOwnedContext) return nullptr;
    sink->mContext = sink->mOwnedContext.get();
    sink->mFrozenPlanPath = sink->mContext->FrozenInputPath();
    return sink;
}

PlanAuditSink::~PlanAuditSink() = default;

auto PlanAuditSink::Capture(const std::filesystem::path& InRepo) const
    -> audit::RepositoryState {
    return mContext->Capture(InRepo);
}

auto PlanAuditSink::Append(std::string InAction,
                           const std::filesystem::path& InRepo,
                           const audit::RepositoryState& InBefore,
                           std::string InStartedAtUtc,
                           const int InExitCode,
                           std::string* OutError) -> bool {
    return mContext->Append(std::move(InAction), InRepo, InBefore,
                            std::move(InStartedAtUtc), InExitCode, OutError);
}

auto PlanAuditSink::Finalize(const int InExitCode, std::string* OutError) -> bool {
    if (!mOwnedContext) return true;
    return mContext->Finalize(InExitCode, OutError);
}

auto PlanAuditSink::BindSourceStateBytes(
    const std::string_view InSourceBytes,
    std::string* OutError) -> bool {
    return mContext->FreezeSupplementalBytes(
               InSourceBytes, "commit-plan-source-state",
               mPlanId + "-source", OutError)
        .has_value();
}

auto PlanAuditSink::RevalidateAdmittedSource(
    const std::filesystem::path& InPlanPath,
    std::string* OutError,
    std::string* OutSourceBytes) const -> bool {
    const auto current = ReadBoundedAuditInput(InPlanPath, 4U << 20U, OutError);
    if (!current) return false;
    const auto currentSha256 = audit::Sha256Hex(*current);
    if (currentSha256 != mSourceSha256 || *current != mSourceBytes) {
        if (OutError)
            *OutError = "plan source bytes changed after audit reservation";
        return false;
    }
    if (OutSourceBytes) *OutSourceBytes = *current;
    return true;
}

auto PlanAuditSink::FrozenPlanPath() const -> const std::filesystem::path& {
    return mFrozenPlanPath;
}

auto PlanAuditSink::PlanSha256() const -> const std::string& {
    return mContext->PlanSha256();
}

auto PlanAuditSink::RunId() const -> const std::string& {
    return mContext->RunId();
}

auto PlanAuditSink::PlanId() const -> const std::string& {
    return mPlanId;
}

} // namespace kano::git::commands
