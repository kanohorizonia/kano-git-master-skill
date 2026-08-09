#pragma once

#include "operation_audit.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace kano::git::commands {

enum class OperationAuditRunReadState { Ready, Missing, Pending, Incomplete, Corrupt,
                                        Incompatible, Truncated, Invalid };
// These three policies deliberately govern independent comparison boundaries.
// Receipt-to-frozen-input consistency is always exact, regardless of policy.
enum class OperationAuditMarkerMatchPolicy { ExactSpec, IdentityOnly };
enum class OperationAuditCallerCorrelationPolicy {
    ExactSpec,
    StandaloneWildcard,
    // VerifyOperationAuditJson only: a supplied KOA parent ID is exact, but
    // an omitted parent ID remains a legacy lookup wildcard.
    LegacyVerify,
};
enum class OperationAuditRouteBindingPolicy { ExactRoute, CompatibleInputKind };
struct OperationAuditRunReadPolicy {
    OperationAuditMarkerMatchPolicy markerMatch = OperationAuditMarkerMatchPolicy::ExactSpec;
    OperationAuditCallerCorrelationPolicy callerCorrelation =
        OperationAuditCallerCorrelationPolicy::ExactSpec;
    OperationAuditRouteBindingPolicy routeBinding =
        OperationAuditRouteBindingPolicy::ExactRoute;
};
enum class OperationAuditRunReadCode {
    None, InvalidIdentity, InvalidConfiguration, AttemptMissing, EvidenceMissing,
    PublicationPending, EvidenceIncomplete,
    MarkerInvalid, UnsupportedSchema, FrozenOperationUnsupportedSchema,
    InputLimit, MalformedEvidence, TraceInvalid,
    ReceiptMismatch, PlanMismatch, HashMismatch, BindingMismatch, CorrelationMismatch,
    UnstableEvidence, NonRegularEvidence,
    PendingMarkerUnreadable, PendingMarkerInvalid, IncompleteMarkerUnreadable,
    IncompleteMarkerInvalid, PendingMarkerProbeFailed, IncompleteMarkerProbeFailed,
    MultipleFrozenBindings, FrozenBindingMissing,
    SourceNotAdmitted, FrozenCommitPlanIdentityMismatch,
    FrozenOperationIdentityMismatch, FrozenCorrelationMismatch,
};

struct OperationAuditRunReadLimits {
    std::uintmax_t maxEventStreamBytes = 64U << 20U;
    std::uintmax_t maxInputBytes = 4U << 20U;
    // Caps the exact sum of UTF-8 string payload bytes retained in preview
    // records. Scalars and object overhead are bounded independently by the
    // record caps; records are retained whole, never partially.
    std::size_t maxPreviewBytes = 16U << 10U;
    std::size_t maxEventRecords = 64;
    std::size_t maxRepositories = 64;
    std::size_t maxEvidenceReferences = 64;
    std::size_t maxDiagnosticBytes = 192;
};

struct OperationAuditEventPreview {
    std::string eventId;
    std::string repositoryId;
    std::optional<std::string> beforeHeadSha;
    std::optional<std::string> afterHeadSha;
    std::uint64_t sequence = 0;
    std::string phase;
    std::string action;
    audit::Outcome outcome;
};
struct OperationAuditEvidencePreview {
    std::string category;
    std::string id;
    std::string kind;
    std::string sha256;
    std::uint64_t sizeBytes = 0;
    std::string contentType;
    audit::RedactionStatus redactionStatus = audit::RedactionStatus::NotRequired;
};
struct OperationAuditRunProjection {
    std::string receiptId;
    std::string runId;
    std::optional<std::string> parentRunId;
    std::uint32_t attempt = 0;
    std::string planId;
    std::string planSha256;
    std::string frozenInputSha256;
    std::string eventStreamSha256;
    audit::CorrelationRefs correlation;
    audit::Outcome terminalOutcome;
    std::uint64_t totalEventRecords = 0;
    std::uint64_t retainedEventRecords = 0;
    std::uint64_t totalRepositories = 0;
    std::uint64_t retainedRepositories = 0;
    std::uint64_t totalEvidenceReferences = 0;
    std::uint64_t retainedEvidenceReferences = 0;
    // Uses the same retained UTF-8 string-payload metric as maxPreviewBytes.
    std::size_t retainedPreviewBytes = 0;
    bool previewTruncated = false;
    bool eventsTruncated = false;
    bool repositoriesTruncated = false;
    bool evidenceTruncated = false;
    bool hasRedactedEvidence = false;
    bool hasWithheldEvidence = false;
    std::vector<OperationAuditEventPreview> events;
    std::vector<audit::RepositoryTransition> repositories;
    std::vector<OperationAuditEvidencePreview> evidence;
};
struct OperationAuditRunReadResult {
    OperationAuditRunReadState state = OperationAuditRunReadState::Invalid;
    OperationAuditRunReadCode code = OperationAuditRunReadCode::InvalidConfiguration;
    std::optional<OperationAuditRunProjection> run;
    std::string diagnostic;
    bool diagnosticTruncated = false;
    [[nodiscard]] auto verified() const noexcept -> bool {
        return (state == OperationAuditRunReadState::Ready ||
                state == OperationAuditRunReadState::Truncated) && run.has_value();
    }
};
// The audit root and its ancestor directories are writer-owned and stable for
// the duration of one read. Child evidence files are opened no-follow and
// checked as bounded regular files; cross-platform namespace pinning is a
// separate hardening boundary.
[[nodiscard]] auto ReadOperationAuditRun(const OperationAuditSpec& InSpec,
                                         std::string_view InRunId,
                                         std::uint32_t InAttempt,
                                         OperationAuditRunReadLimits InLimits = {},
                                         std::optional<std::string_view> InExpectedReceiptId = std::nullopt,
                                         OperationAuditRunReadPolicy InPolicy = {})
    -> OperationAuditRunReadResult;
} // namespace kano::git::commands
