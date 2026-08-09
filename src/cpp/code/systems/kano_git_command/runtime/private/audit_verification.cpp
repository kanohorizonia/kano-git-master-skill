#include "audit_verification.hpp"

#include "audit_verification_internal.hpp"

#include <algorithm>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <utility>

namespace kano::git::commands {
namespace {

auto InvalidVerificationRead(std::string InDiagnostic,
                             const std::size_t InMaximumBytes)
    -> OperationAuditRunReadResult {
    bool truncated = false;
    if (InDiagnostic.size() > InMaximumBytes) {
        InDiagnostic.resize(InMaximumBytes);
        truncated = true;
    }
    return {
        .state = OperationAuditRunReadState::Invalid,
        .code = OperationAuditRunReadCode::InvalidConfiguration,
        .diagnostic = std::move(InDiagnostic),
        .diagnosticTruncated = truncated,
    };
}

} // namespace

auto MakeOperationAuditVerificationSpec(
    const OperationAuditVerificationRequest& InRequest,
    std::string* OutError) -> std::optional<OperationAuditSpec> {
    if (InRequest.workspaceRoot.empty() || InRequest.planFile.empty()) {
        if (OutError != nullptr) {
            *OutError = "audit verification identity is incomplete";
        }
        return std::nullopt;
    }

    std::error_code workspaceError;
    const auto workspace = std::filesystem::weakly_canonical(
        InRequest.workspaceRoot,
        workspaceError);
    if (workspaceError || workspace.empty()) {
        if (OutError != nullptr) {
            *OutError = "cannot resolve workspace identity";
        }
        return std::nullopt;
    }

    const auto requestedPlan = InRequest.planFile.is_absolute()
        ? InRequest.planFile
        : workspace / InRequest.planFile;
    std::error_code planError;
    const auto canonicalPlan = std::filesystem::weakly_canonical(
        requestedPlan,
        planError);
    if (planError || canonicalPlan.empty()) {
        if (OutError != nullptr) {
            *OutError = "cannot resolve plan identity";
        }
        return std::nullopt;
    }

    const auto sourceBytes = ReadBoundedAuditInput(
        canonicalPlan,
        4U << 20U,
        OutError);
    if (!sourceBytes.has_value()) {
        return std::nullopt;
    }

    std::string planId;
    try {
        const auto document = nlohmann::json::parse(*sourceBytes);
        if (!document.is_object() || !document.contains("meta") ||
            !document.at("meta").is_object() ||
            !document.at("meta").contains("plan_id") ||
            !document.at("meta").at("plan_id").is_string()) {
            throw std::runtime_error(
                "current admitted plan identity is missing");
        }
        planId = document.at("meta").at("plan_id").get<std::string>();
        if (!audit::IsStableAuditId(planId)) {
            throw std::runtime_error(
                "current admitted plan id is invalid");
        }
    } catch (const std::exception& exception) {
        if (OutError != nullptr) {
            *OutError = exception.what();
        }
        return std::nullopt;
    }

    OperationAuditSpec spec;
    spec.workspaceRoot = workspace.lexically_normal();
    spec.sourcePath = canonicalPlan;
    spec.inputIdentity = canonicalPlan.generic_string();
    spec.inputKind = "commit-plan";
    spec.route = "commit-push.plan";
    spec.planId = std::move(planId);
    spec.sourceBytes = *sourceBytes;
    spec.frozenBytes = *sourceBytes;
    spec.frozenFileName = "frozen-plan.json";
    spec.correlation.mode = "standalone";
    spec.correlation.runId = InRequest.runId;
    spec.correlation.attempt = InRequest.attempt;
    return spec;
}

auto ReadOperationAuditVerification(
    const OperationAuditVerificationRequest& InRequest,
    const OperationAuditRunReadLimits InLimits)
    -> OperationAuditRunReadResult {
    if (!audit::IsStableAuditId(InRequest.runId) ||
        InRequest.attempt == 0) {
        return InvalidVerificationRead(
            "audit verification run identity is invalid",
            InLimits.maxDiagnosticBytes);
    }

    std::string error;
    const auto spec = MakeOperationAuditVerificationSpec(InRequest, &error);
    if (!spec.has_value()) {
        return InvalidVerificationRead(
            error.empty()
                ? std::string("audit verification request is invalid")
                : std::move(error),
            InLimits.maxDiagnosticBytes);
    }

    OperationAuditRunReadPolicy policy;
    policy.markerMatch = OperationAuditMarkerMatchPolicy::IdentityOnly;
    policy.callerCorrelation =
        OperationAuditCallerCorrelationPolicy::StandaloneWildcard;
    policy.routeBinding =
        OperationAuditRouteBindingPolicy::CompatibleInputKind;
    return ReadOperationAuditRun(
        *spec,
        InRequest.runId,
        InRequest.attempt,
        InLimits,
        std::nullopt,
        policy);
}

} // namespace kano::git::commands
