#include "audit_run_reader.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <limits>
#include <set>
#include <system_error>
#include <tuple>

namespace kano::git::commands {
namespace {

constexpr std::uintmax_t kMarkerByteLimit = 64U << 10U;
constexpr std::uintmax_t kEventsByteLimit = 64U << 20U;
constexpr std::uintmax_t kInputByteLimit = 4U << 20U;

constexpr std::array<std::pair<std::string_view, std::string_view>, 7>
    kSupportedRouteInputPairs = {{
        {"commit.plan", "commit-plan"},
        {"commit-push.plan", "commit-plan"},
        {"plan.apply", "commit-plan"},
        {"converge.repos", "operation-descriptor"},
        {"converge.branches.apply", "operation-descriptor"},
        {"converge.branches.recover", "operation-descriptor"},
        {"converge.branches.retire", "operation-descriptor"},
    }};

auto IsSupportedRouteInputPair(const std::string_view InRoute,
                               const std::string_view InInputKind) -> bool {
    return std::any_of(kSupportedRouteInputPairs.begin(),
                       kSupportedRouteInputPairs.end(),
                       [&](const auto& pair) {
                           return pair.first == InRoute && pair.second == InInputKind;
                       });
}

auto IsValidPolicy(const OperationAuditRunReadPolicy& InPolicy) -> bool {
    return (InPolicy.markerMatch == OperationAuditMarkerMatchPolicy::ExactSpec ||
            InPolicy.markerMatch == OperationAuditMarkerMatchPolicy::IdentityOnly) &&
        (InPolicy.callerCorrelation == OperationAuditCallerCorrelationPolicy::ExactSpec ||
         InPolicy.callerCorrelation == OperationAuditCallerCorrelationPolicy::StandaloneWildcard ||
         InPolicy.callerCorrelation == OperationAuditCallerCorrelationPolicy::LegacyVerify) &&
        (InPolicy.routeBinding == OperationAuditRouteBindingPolicy::ExactRoute ||
         InPolicy.routeBinding == OperationAuditRouteBindingPolicy::CompatibleInputKind);
}

auto DiagnosticFor(const OperationAuditRunReadCode InCode) -> std::string_view {
    switch (InCode) {
    case OperationAuditRunReadCode::None: return {};
    case OperationAuditRunReadCode::InvalidIdentity: return "invalid audit identity";
    case OperationAuditRunReadCode::InvalidConfiguration: return "invalid reader configuration";
    case OperationAuditRunReadCode::AttemptMissing: return "audit attempt is missing";
    case OperationAuditRunReadCode::EvidenceMissing: return "audit evidence is missing";
    case OperationAuditRunReadCode::PublicationPending:
        return "audit receipt publication is still pending";
    case OperationAuditRunReadCode::EvidenceIncomplete:
        return "audit evidence is explicitly incomplete";
    case OperationAuditRunReadCode::MarkerInvalid: return "audit marker is invalid";
    case OperationAuditRunReadCode::UnsupportedSchema: return "audit schema is unsupported";
    case OperationAuditRunReadCode::FrozenOperationUnsupportedSchema:
        return "frozen operation descriptor schema is unsupported";
    case OperationAuditRunReadCode::InputLimit: return "audit input exceeds limit";
    case OperationAuditRunReadCode::MalformedEvidence: return "audit evidence is malformed";
    case OperationAuditRunReadCode::TraceInvalid: return "audit trace is invalid";
    case OperationAuditRunReadCode::ReceiptMismatch: return "audit receipt identity mismatch";
    case OperationAuditRunReadCode::PlanMismatch: return "audit plan identity mismatch";
    case OperationAuditRunReadCode::HashMismatch: return "audit hash mismatch";
    case OperationAuditRunReadCode::BindingMismatch: return "audit binding mismatch";
    case OperationAuditRunReadCode::CorrelationMismatch: return "audit correlation mismatch";
    case OperationAuditRunReadCode::UnstableEvidence:
        return "audit evidence is unreadable or changed while read";
    case OperationAuditRunReadCode::NonRegularEvidence: return "audit evidence is not regular";
    case OperationAuditRunReadCode::PendingMarkerUnreadable: return "audit publication-pending sentinel is unreadable";
    case OperationAuditRunReadCode::PendingMarkerInvalid: return "audit publication-pending sentinel is invalid";
    case OperationAuditRunReadCode::IncompleteMarkerUnreadable: return "audit incomplete marker is unreadable";
    case OperationAuditRunReadCode::IncompleteMarkerInvalid: return "audit incomplete marker is invalid";
    case OperationAuditRunReadCode::PendingMarkerProbeFailed: return "cannot prove absence of audit publication-pending sentinel";
    case OperationAuditRunReadCode::IncompleteMarkerProbeFailed: return "cannot prove absence of audit incomplete marker";
    case OperationAuditRunReadCode::MultipleFrozenBindings: return "multiple main frozen input bindings are present";
    case OperationAuditRunReadCode::FrozenBindingMissing: return "main frozen route/input binding is missing or contradictory";
    case OperationAuditRunReadCode::SourceNotAdmitted: return "current plan bytes are not an admitted source state";
    case OperationAuditRunReadCode::FrozenCommitPlanIdentityMismatch: return "frozen commit-plan identity is contradictory";
    case OperationAuditRunReadCode::FrozenOperationIdentityMismatch: return "frozen operation descriptor identity is contradictory";
    case OperationAuditRunReadCode::FrozenCorrelationMismatch: return "frozen input correlation is contradictory";
    }
    return "audit reader result";
}

auto StateFor(const OperationAuditRunReadCode InCode)
    -> OperationAuditRunReadState {
    switch (InCode) {
    case OperationAuditRunReadCode::None:
        return OperationAuditRunReadState::Ready;
    case OperationAuditRunReadCode::AttemptMissing:
    case OperationAuditRunReadCode::EvidenceMissing:
        return OperationAuditRunReadState::Missing;
    case OperationAuditRunReadCode::PublicationPending:
        return OperationAuditRunReadState::Pending;
    case OperationAuditRunReadCode::PendingMarkerUnreadable:
    case OperationAuditRunReadCode::PendingMarkerInvalid:
    case OperationAuditRunReadCode::PendingMarkerProbeFailed:
        return OperationAuditRunReadState::Corrupt;
    case OperationAuditRunReadCode::EvidenceIncomplete:
        return OperationAuditRunReadState::Incomplete;
    case OperationAuditRunReadCode::IncompleteMarkerUnreadable:
    case OperationAuditRunReadCode::IncompleteMarkerInvalid:
    case OperationAuditRunReadCode::IncompleteMarkerProbeFailed:
        return OperationAuditRunReadState::Corrupt;
    case OperationAuditRunReadCode::UnsupportedSchema:
    case OperationAuditRunReadCode::FrozenOperationUnsupportedSchema:
        return OperationAuditRunReadState::Incompatible;
    case OperationAuditRunReadCode::InputLimit:
        return OperationAuditRunReadState::Truncated;
    case OperationAuditRunReadCode::MarkerInvalid:
    case OperationAuditRunReadCode::MalformedEvidence:
    case OperationAuditRunReadCode::TraceInvalid:
    case OperationAuditRunReadCode::ReceiptMismatch:
    case OperationAuditRunReadCode::PlanMismatch:
    case OperationAuditRunReadCode::HashMismatch:
    case OperationAuditRunReadCode::BindingMismatch:
    case OperationAuditRunReadCode::CorrelationMismatch:
    case OperationAuditRunReadCode::UnstableEvidence:
    case OperationAuditRunReadCode::NonRegularEvidence:
    case OperationAuditRunReadCode::MultipleFrozenBindings:
    case OperationAuditRunReadCode::FrozenBindingMissing:
    case OperationAuditRunReadCode::SourceNotAdmitted:
    case OperationAuditRunReadCode::FrozenCommitPlanIdentityMismatch:
    case OperationAuditRunReadCode::FrozenOperationIdentityMismatch:
    case OperationAuditRunReadCode::FrozenCorrelationMismatch:
        return OperationAuditRunReadState::Corrupt;
    case OperationAuditRunReadCode::InvalidIdentity:
    case OperationAuditRunReadCode::InvalidConfiguration:
        return OperationAuditRunReadState::Invalid;
    }
    return OperationAuditRunReadState::Invalid;
}

auto Result(const OperationAuditRunReadCode InCode,
            const OperationAuditRunReadLimits& InLimits)
    -> OperationAuditRunReadResult {
    const auto diagnostic = DiagnosticFor(InCode);
    const auto retained = std::min(diagnostic.size(), InLimits.maxDiagnosticBytes);
    return {.state = StateFor(InCode),
            .code = InCode,
            .diagnostic = std::string(diagnostic.substr(0, retained)),
            .diagnosticTruncated = retained != diagnostic.size()};
}

auto IsStrictUtcTimestamp(const std::string_view value) -> bool {
// RFC3339 UTC, whole seconds only. Validate calendar fields rather than
    // accepting a merely well-shaped timestamp.
    if (value.size() != 20 || value[4] != '-' || value[7] != '-' ||
        value[10] != 'T' || value[13] != ':' || value[16] != ':' || value[19] != 'Z')
        return false;
    const auto number = [&](const std::size_t offset, const std::size_t count) -> int {
        int out = 0;
        for (std::size_t index = 0; index < count; ++index) {
            const char ch = value[offset + index];
            if (ch < '0' || ch > '9') return -1;
            out = out * 10 + (ch - '0');
        }
        return out;
    };
    const auto year = number(0, 4); const auto month = number(5, 2);
    const auto day = number(8, 2); const auto hour = number(11, 2);
    const auto minute = number(14, 2); const auto second = number(17, 2);
    if (year < 1 || month < 1 || month > 12 || day < 1 || hour > 23 ||
        minute > 59 || second > 59) return false;
    static constexpr std::array<int, 12> days = {31, 28, 31, 30, 31, 30,
                                                  31, 31, 30, 31, 30, 31};
    const bool leap = year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
    return day <= days[month - 1] + (month == 2 && leap ? 1 : 0);
}

auto IsLowerSha256(const std::string_view value) -> bool {
    return value.size() == 64 && std::all_of(value.begin(), value.end(), [](const char ch) {
        return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
    });
}

auto Project(const audit::RunReceipt& InReceipt, const std::vector<audit::AuditEvent>& InEvents,
             const std::string& InFrozenSha, const std::string& InReceiptSha,
             const OperationAuditRunReadLimits& InLimits) -> OperationAuditRunProjection {
    OperationAuditRunProjection out;
    out.receiptId = InReceiptSha; out.runId = InReceipt.runId;
    out.parentRunId = InReceipt.parentRunId; out.attempt = InReceipt.attempt;
    out.planId = InReceipt.planId; out.planSha256 = InReceipt.planSha256;
    out.frozenInputSha256 = InFrozenSha; out.eventStreamSha256 = InReceipt.eventStreamSha256;
    out.correlation = InReceipt.correlation; out.terminalOutcome = InReceipt.terminalOutcome;
    out.totalEventRecords = InEvents.size(); out.totalRepositories = InReceipt.repositories.size();
    out.totalEvidenceReferences = InReceipt.policyRefs.size() + InReceipt.approvalRefs.size() + InReceipt.artifacts.size();
    std::size_t budget = InLimits.maxPreviewBytes;
    const auto payloadBytes = [](const std::initializer_list<std::string_view> strings)
        -> std::optional<std::size_t> {
        std::size_t total = 0;
        for (const auto value : strings) {
            if (value.size() > std::numeric_limits<std::size_t>::max() - total)
                return std::nullopt;
            total += value.size();
        }
        return total;
    };
    auto repositories = InReceipt.repositories;
    std::sort(repositories.begin(), repositories.end(), [](const auto& left, const auto& right) {
        return left.repositoryId < right.repositoryId;
    });
    for (const auto& repository : repositories) {
        if (out.repositories.size() == InLimits.maxRepositories) { out.repositoriesTruncated = true; break; }
        const auto bytes = payloadBytes({repository.repositoryId,
            repository.before.headSha.value_or(""), repository.before.branch.value_or(""),
            repository.before.dirtyFingerprint.value_or(""), repository.before.upstreamHeadSha.value_or(""),
            repository.after.headSha.value_or(""), repository.after.branch.value_or(""),
            repository.after.dirtyFingerprint.value_or(""), repository.after.upstreamHeadSha.value_or("")});
        if (!bytes || *bytes > budget) { out.repositoriesTruncated = true; break; }
        budget -= *bytes;
        out.repositories.push_back(repository);
    }
    struct Evidence {
        std::string category;
        std::string id;
        std::string kind;
        std::string sha;
        std::uint64_t size = 0;
        std::string contentType;
        audit::RedactionStatus redaction = audit::RedactionStatus::NotRequired;
    };
    std::vector<Evidence> evidence;
    for (const auto& item : InReceipt.policyRefs)
        evidence.push_back({"policy", item.id, {}, item.sha256});
    for (const auto& item : InReceipt.approvalRefs)
        evidence.push_back({"approval", item.id, {}, item.sha256});
    for (const auto& item : InReceipt.artifacts) {
        evidence.push_back({"artifact", item.id, item.kind, item.sha256,
                            item.sizeBytes, item.contentType,
                            item.redactionStatus});
        out.hasRedactedEvidence = out.hasRedactedEvidence || item.redactionStatus == audit::RedactionStatus::Redacted;
        out.hasWithheldEvidence = out.hasWithheldEvidence || item.redactionStatus == audit::RedactionStatus::Withheld;
    }
    std::sort(evidence.begin(), evidence.end(), [](const auto& left, const auto& right) {
        return std::tie(left.category, left.id, left.kind, left.sha) < std::tie(right.category, right.id, right.kind, right.sha);
    });
    for (const auto& artifact : evidence) {
        if (out.evidence.size() == InLimits.maxEvidenceReferences) { out.evidenceTruncated = true; break; }
        const auto bytes = payloadBytes({artifact.category, artifact.id, artifact.kind,
                                         artifact.sha, artifact.contentType});
        if (!bytes || *bytes > budget) { out.evidenceTruncated = true; break; }
        OperationAuditEvidencePreview item;
        item.category = artifact.category; item.id = artifact.id; item.kind = artifact.kind;
        item.sha256 = artifact.sha;
        item.sizeBytes = artifact.size; item.contentType = artifact.contentType;
        item.redactionStatus = artifact.redaction;
        budget -= *bytes;
        out.evidence.push_back(std::move(item));
    }
    for (const auto& event : InEvents) {
        if (out.events.size() == InLimits.maxEventRecords) {
            out.eventsTruncated = true;
            break;
        }
        const auto bytes = payloadBytes({event.eventId, event.repository.repositoryId,
            event.repository.before.headSha.value_or(""), event.repository.after.headSha.value_or(""),
            event.phase, event.action, event.outcome.reasonCode.value_or("")});
        if (!bytes || *bytes > budget) {
            out.eventsTruncated = true;
            break;
        }
        OperationAuditEventPreview item;
        item.eventId = event.eventId;
        item.repositoryId = event.repository.repositoryId;
        item.beforeHeadSha = event.repository.before.headSha;
        item.afterHeadSha = event.repository.after.headSha;
        item.sequence = event.sequence;
        item.phase = event.phase;
        item.action = event.action;
        item.outcome = event.outcome;
        budget -= *bytes;
        out.events.push_back(std::move(item));
    }
    out.retainedEventRecords = out.events.size(); out.retainedRepositories = out.repositories.size();
    out.retainedEvidenceReferences = out.evidence.size(); out.retainedPreviewBytes = InLimits.maxPreviewBytes - budget;
    out.previewTruncated = out.previewTruncated || out.eventsTruncated || out.repositoriesTruncated || out.evidenceTruncated;
    return out;
}

auto ReadMarker(const std::filesystem::path& InPath,
                std::string_view InSchemaName,
                std::string_view InRunId,
                const std::uint32_t InAttempt,
                const OperationAuditSpec& InSpec,
                OperationAuditRunReadState InPresentState,
                const OperationAuditRunReadLimits& InLimits,
                const OperationAuditMarkerMatchPolicy InMarkerMatch)
    -> std::optional<OperationAuditRunReadResult> {
    std::error_code ec;
    const auto status = std::filesystem::symlink_status(InPath, ec);
    if (status.type() == std::filesystem::file_type::not_found ||
        ec == std::errc::no_such_file_or_directory) {
        return std::nullopt;
    }
    if (ec) {
        return Result(InPresentState == OperationAuditRunReadState::Pending
                          ? OperationAuditRunReadCode::PendingMarkerProbeFailed
                          : OperationAuditRunReadCode::IncompleteMarkerProbeFailed, InLimits);
    }
    if (std::filesystem::is_symlink(status) ||
        !std::filesystem::is_regular_file(status)) {
        return Result(InPresentState == OperationAuditRunReadState::Pending
                          ? OperationAuditRunReadCode::PendingMarkerUnreadable
                          : OperationAuditRunReadCode::IncompleteMarkerUnreadable, InLimits);
    }
    std::string error;
    const auto bytes = ReadBoundedAuditInput(InPath, kMarkerByteLimit, &error);
    if (!bytes) {
        ec.clear();
        const auto after = std::filesystem::symlink_status(InPath, ec);
        if (after.type() == std::filesystem::file_type::not_found ||
            ec == std::errc::no_such_file_or_directory) {
            return std::nullopt;
        }
        if (ec) {
            return Result(InPresentState == OperationAuditRunReadState::Pending
                              ? OperationAuditRunReadCode::PendingMarkerProbeFailed
                              : OperationAuditRunReadCode::IncompleteMarkerProbeFailed, InLimits);
        }
        return Result(InPresentState == OperationAuditRunReadState::Pending
                          ? OperationAuditRunReadCode::PendingMarkerUnreadable
                          : OperationAuditRunReadCode::IncompleteMarkerUnreadable, InLimits);
    }
    try {
        bool duplicate = false;
        std::vector<std::set<std::string>> objectKeys;
        const auto doc = nlohmann::json::parse(bytes->begin(), bytes->end(),
            [&duplicate, &objectKeys](int, nlohmann::json::parse_event_t event,
                                      nlohmann::json& parsed) {
                if (event == nlohmann::json::parse_event_t::object_start)
                    objectKeys.emplace_back();
                if (event == nlohmann::json::parse_event_t::key &&
                    (objectKeys.empty() ||
                     !objectKeys.back().insert(parsed.get<std::string>()).second)) {
                    duplicate = true;
                }
                if (event == nlohmann::json::parse_event_t::object_end &&
                    !objectKeys.empty()) {
                    objectKeys.pop_back();
                }
                return true;
            });
        const bool pending = InPresentState == OperationAuditRunReadState::Pending;
        const std::set<std::string> expected = pending
            ? std::set<std::string>{"schemaName", "schemaVersion", "runId",
                                    "parentRunId", "attempt", "planId",
                                    "planSha256", "reservedAtUtc"}
            : std::set<std::string>{"schemaName", "schemaVersion", "runId",
                                    "parentRunId", "attempt", "planId",
                                    "planSha256", "reasonCode", "recoverable",
                                    "receiptPublished", "observedEventCount",
                                    "recordedAtUtc"};
        std::set<std::string> actual;
        for (auto it = doc.begin(); it != doc.end(); ++it) actual.insert(it.key());
        if (!doc.is_object() || doc.value("schemaName", "") != InSchemaName ||
            doc.value("schemaVersion", 0) != 1 ||
            doc.value("runId", "") != InRunId ||
            doc.value("attempt", 0U) != InAttempt || duplicate || actual != expected ||
            (!doc.at("parentRunId").is_null() &&
             !doc.at("parentRunId").is_string()) ||
            (doc.at("parentRunId").is_string() &&
             !audit::IsStableAuditId(doc.at("parentRunId").get<std::string>())) ||
            (InMarkerMatch == OperationAuditMarkerMatchPolicy::ExactSpec &&
             (InSpec.correlation.parentRunId.empty() ? !doc.at("parentRunId").is_null()
                : doc.at("parentRunId") != InSpec.correlation.parentRunId)) ||
            !doc.contains("planId") || !doc.at("planId").is_string() ||
            !audit::IsStableAuditId(doc.at("planId").get<std::string>()) ||
            (InMarkerMatch == OperationAuditMarkerMatchPolicy::ExactSpec &&
             doc.value("planId", "") != InSpec.planId) ||
            !doc.contains("planSha256") || !doc.at("planSha256").is_string() ||
            !IsLowerSha256(doc.at("planSha256").get<std::string>()) ||
            (InMarkerMatch == OperationAuditMarkerMatchPolicy::ExactSpec &&
             doc.at("planSha256") != audit::Sha256Hex(InSpec.frozenBytes)) ||
            (pending && (!doc.at("reservedAtUtc").is_string() ||
                         !IsStrictUtcTimestamp(doc.at("reservedAtUtc").get<std::string>()))) ||
            (!pending &&
             (!doc.at("reasonCode").is_string() ||
              !audit::IsStableAuditId(doc.at("reasonCode").get<std::string>()) ||
              !doc.at("recoverable").is_boolean() ||
              !doc.at("receiptPublished").is_boolean() ||
              !doc.at("observedEventCount").is_number_unsigned() ||
              !doc.at("recordedAtUtc").is_string() ||
              !IsStrictUtcTimestamp(doc.at("recordedAtUtc").get<std::string>()) ||
              doc.at("observedEventCount").get<std::uint64_t>() > 1000000U))) {
            return Result(pending ? OperationAuditRunReadCode::PendingMarkerInvalid
                                  : OperationAuditRunReadCode::IncompleteMarkerInvalid, InLimits);
        }
    } catch (...) {
        return Result(InPresentState == OperationAuditRunReadState::Pending
                          ? OperationAuditRunReadCode::PendingMarkerInvalid
                          : OperationAuditRunReadCode::IncompleteMarkerInvalid, InLimits);
    }
    return Result(InPresentState == OperationAuditRunReadState::Pending
                      ? OperationAuditRunReadCode::PublicationPending
                      : OperationAuditRunReadCode::EvidenceIncomplete,
                  InLimits);
}

auto ProbeMarkers(const OperationAuditPaths& InPaths,
                  const OperationAuditSpec& InSpec,
                  const std::string_view InRunId,
                  const std::uint32_t InAttempt,
                  const OperationAuditRunReadLimits& InLimits,
                  const OperationAuditMarkerMatchPolicy InMarkerMatch)
    -> std::optional<OperationAuditRunReadResult> {
    if (const auto incomplete = ReadMarker(
            InPaths.incomplete, "kog.auditIncomplete", InRunId, InAttempt,
            InSpec, OperationAuditRunReadState::Incomplete, InLimits, InMarkerMatch)) {
        return incomplete;
    }
    const auto pending = ReadMarker(
        InPaths.publicationPending, "kog.auditPublicationPending", InRunId,
        InAttempt, InSpec, OperationAuditRunReadState::Pending, InLimits, InMarkerMatch);
    if (!pending) return std::nullopt;
    if (pending->state != OperationAuditRunReadState::Pending) return pending;

    // Incomplete is terminal and wins if it appears while a pending marker is
    // being observed. This closes the two-marker priority race.
    if (const auto incomplete = ReadMarker(
            InPaths.incomplete, "kog.auditIncomplete", InRunId, InAttempt,
            InSpec, OperationAuditRunReadState::Incomplete, InLimits, InMarkerMatch)) {
        return incomplete;
    }
    return pending;
}

enum class EvidenceInputState { Ready, Missing, Limit, NonRegular, Unstable };

struct EvidenceInput {
    EvidenceInputState state = EvidenceInputState::Unstable;
    std::string bytes;
};

auto InspectEvidencePath(const std::filesystem::path& InPath,
                         const std::uintmax_t InLimit)
    -> EvidenceInputState {
    std::error_code ec;
    const auto status = std::filesystem::symlink_status(InPath, ec);
    if (status.type() == std::filesystem::file_type::not_found ||
        ec == std::errc::no_such_file_or_directory) {
        return EvidenceInputState::Missing;
    }
    if (ec) return EvidenceInputState::Unstable;
    if (std::filesystem::is_symlink(status) ||
        !std::filesystem::is_regular_file(status)) {
        return EvidenceInputState::NonRegular;
    }
    const auto size = std::filesystem::file_size(InPath, ec);
    if (ec) return EvidenceInputState::Unstable;
    return size > InLimit ? EvidenceInputState::Limit
                          : EvidenceInputState::Ready;
}

auto ReadEvidence(const std::filesystem::path& InPath,
                  const std::uintmax_t InLimit) -> EvidenceInput {
    const auto before = InspectEvidencePath(InPath, InLimit);
    if (before != EvidenceInputState::Ready) return {.state = before};

    std::string ignored;
    if (auto bytes = ReadBoundedAuditInput(InPath, InLimit, &ignored)) {
        return {.state = EvidenceInputState::Ready,
                .bytes = std::move(*bytes)};
    }
    const auto after = InspectEvidencePath(InPath, InLimit);
    return {.state = after == EvidenceInputState::Ready
                         ? EvidenceInputState::Unstable
                         : after};
}

auto CodeFor(const EvidenceInputState InState) -> OperationAuditRunReadCode {
    switch (InState) {
    case EvidenceInputState::Missing:
        return OperationAuditRunReadCode::EvidenceMissing;
    case EvidenceInputState::Limit:
        return OperationAuditRunReadCode::InputLimit;
    case EvidenceInputState::NonRegular:
        return OperationAuditRunReadCode::NonRegularEvidence;
    case EvidenceInputState::Unstable:
        return OperationAuditRunReadCode::UnstableEvidence;
    case EvidenceInputState::Ready:
        break;
    }
    return OperationAuditRunReadCode::UnstableEvidence;
}

auto HasValidationCode(const audit::ValidationResult& InValidation,
                       const std::string_view InCode) -> bool {
    return std::any_of(InValidation.issues.begin(), InValidation.issues.end(),
                       [&](const auto& issue) { return issue.code == InCode; });
}

auto ValidateFrozenInput(const OperationAuditSpec& InSpec,
                         std::string_view InRunId,
                         const std::uint32_t InAttempt,
                         const audit::RunReceipt& InReceipt,
                         std::string_view InFrozenBytes,
                         const std::string& InFrozenSha256,
                         const OperationAuditCallerCorrelationPolicy InCallerCorrelation,
                         const OperationAuditRouteBindingPolicy InRouteBinding)
    -> std::optional<OperationAuditRunReadCode> {
    const audit::ArtifactReference* frozenInputArtifact = nullptr;
    std::string verifiedRoute;
    for (const auto& [route, inputKind] : kSupportedRouteInputPairs) {
        if (inputKind != InSpec.inputKind) continue;
        const auto kind = std::string("audit-frozen-") + std::string(route) +
            "-" + std::string(inputKind);
        for (const auto& artifact : InReceipt.artifacts) {
            if (artifact.kind != kind) continue;
            if (frozenInputArtifact != nullptr)
                return OperationAuditRunReadCode::MultipleFrozenBindings;
            frozenInputArtifact = &artifact;
            verifiedRoute = route;
        }
    }
    const auto currentSourceSha256 = audit::Sha256Hex(InSpec.sourceBytes);
    if (InReceipt.eventCount == 0) {
        // A before-first-event crash has no event-union artifacts. The caller
        // source remains admitted only when it exactly is the frozen input.
        if (currentSourceSha256 != InFrozenSha256 ||
            InSpec.sourceBytes.size() != InFrozenBytes.size()) {
            return OperationAuditRunReadCode::SourceNotAdmitted;
        }
    } else {
        if (frozenInputArtifact == nullptr ||
            (InRouteBinding == OperationAuditRouteBindingPolicy::ExactRoute &&
             verifiedRoute != InSpec.route) ||
            frozenInputArtifact->sha256 != InFrozenSha256 ||
            frozenInputArtifact->sizeBytes != InFrozenBytes.size()) {
            return OperationAuditRunReadCode::FrozenBindingMissing;
        }
        const auto sourceKind = "audit-source-" + verifiedRoute + "-" + InSpec.inputKind;
        const auto sourceAdmitted = std::any_of(
            InReceipt.artifacts.begin(), InReceipt.artifacts.end(), [&](const auto& artifact) {
                const bool permittedKind = artifact.kind == sourceKind ||
                    (InSpec.inputKind == "commit-plan" &&
                     artifact.kind == "frozen-commit-plan-source-state");
                return permittedKind && artifact.sha256 == currentSourceSha256 &&
                    artifact.sizeBytes == InSpec.sourceBytes.size();
            });
        if (!sourceAdmitted) return OperationAuditRunReadCode::SourceNotAdmitted;
    }

    try {
        const auto frozenDoc = nlohmann::json::parse(InFrozenBytes);
        const nlohmann::json* correlationDoc = nullptr;
        if (InSpec.inputKind == "commit-plan") {
            if (!frozenDoc.is_object() || !frozenDoc.contains("meta") ||
                !frozenDoc.at("meta").is_object() ||
                frozenDoc.at("meta").value("plan_id", std::string{}) != InReceipt.planId ||
                !frozenDoc.at("meta").contains("correlation")) {
                return OperationAuditRunReadCode::FrozenCommitPlanIdentityMismatch;
            }
            correlationDoc = &frozenDoc.at("meta").at("correlation");
        } else {
            if (!frozenDoc.is_object() || !frozenDoc.contains("schema_name") ||
                !frozenDoc.contains("schema_version") ||
                !frozenDoc.at("schema_name").is_string() ||
                !frozenDoc.at("schema_version").is_number_unsigned()) {
                return OperationAuditRunReadCode::FrozenOperationIdentityMismatch;
            }
            if (frozenDoc.at("schema_name").get<std::string>() != "kog.operationAuditInput" ||
                frozenDoc.at("schema_version").get<std::uint32_t>() != 1U) {
                return OperationAuditRunReadCode::FrozenOperationUnsupportedSchema;
            }
            if (InReceipt.eventCount == 0) {
                if (!frozenDoc.contains("route") || !frozenDoc.at("route").is_string())
                    return OperationAuditRunReadCode::FrozenOperationIdentityMismatch;
                verifiedRoute = frozenDoc.at("route").get<std::string>();
                if (!IsSupportedRouteInputPair(verifiedRoute, InSpec.inputKind) ||
                    (InRouteBinding == OperationAuditRouteBindingPolicy::ExactRoute &&
                     verifiedRoute != InSpec.route)) {
                    return OperationAuditRunReadCode::FrozenOperationIdentityMismatch;
                }
            }
            if (frozenDoc.value("route", std::string{}) != verifiedRoute ||
                !frozenDoc.contains("correlation")) {
                return OperationAuditRunReadCode::FrozenOperationIdentityMismatch;
            }
            correlationDoc = &frozenDoc.at("correlation");
        }
        std::string correlationError;
        const auto frozenCorrelation = ParseOperationCorrelationEnvelope(
            correlationDoc->dump(), &correlationError);
        const auto receiptParentRunId = InReceipt.parentRunId.value_or("");
        const auto receiptCorrelationMatches = [&](const auto& correlation) {
            if (correlation.mode == "standalone")
                return InReceipt.correlation.mode == audit::CorrelationMode::Standalone;
            return InReceipt.correlation.mode == audit::CorrelationMode::Koa &&
                InReceipt.correlation.productId == correlation.productId &&
                InReceipt.correlation.topicId ==
                    (correlation.topicId.empty()
                        ? std::nullopt
                        : std::optional<std::string>(correlation.topicId)) &&
                InReceipt.correlation.itemId == correlation.itemId &&
                InReceipt.correlation.workOrderId == correlation.workOrderId &&
                InReceipt.correlation.requestId == correlation.requestId &&
                InReceipt.correlation.producerId == correlation.producerId &&
                InReceipt.correlation.routeId == correlation.routeId &&
                !InReceipt.correlation.agentId.has_value();
        };
        if (!frozenCorrelation ||
            !ValidateOperationCorrelationEnvelope(*frozenCorrelation, true,
                                                  &correlationError) ||
            frozenCorrelation->runId != InRunId ||
            frozenCorrelation->attempt != InAttempt ||
            frozenCorrelation->parentRunId != receiptParentRunId ||
            !receiptCorrelationMatches(*frozenCorrelation) ||
            (InCallerCorrelation == OperationAuditCallerCorrelationPolicy::ExactSpec &&
             InSpec.correlation.mode == "koa" &&
             (frozenCorrelation->productId != InSpec.correlation.productId ||
              frozenCorrelation->topicId != InSpec.correlation.topicId ||
              frozenCorrelation->itemId != InSpec.correlation.itemId ||
              frozenCorrelation->workOrderId != InSpec.correlation.workOrderId ||
              frozenCorrelation->requestId != InSpec.correlation.requestId ||
              frozenCorrelation->producerId != InSpec.correlation.producerId ||
              frozenCorrelation->routeId != InSpec.correlation.routeId))) {
            return OperationAuditRunReadCode::FrozenCorrelationMismatch;
        }
    } catch (...) {
        return OperationAuditRunReadCode::MalformedEvidence;
    }
    return std::nullopt;
}

} // namespace

auto ReadOperationAuditRun(const OperationAuditSpec& InSpec,
                           const std::string_view InRunId,
                           const std::uint32_t InAttempt,
                           OperationAuditRunReadLimits InLimits,
                           const std::optional<std::string_view> InExpectedReceiptId,
                           const OperationAuditRunReadPolicy InPolicy)
    -> OperationAuditRunReadResult {
    InLimits.maxEventStreamBytes = std::min(InLimits.maxEventStreamBytes, kEventsByteLimit);
    InLimits.maxInputBytes = std::min(InLimits.maxInputBytes, kInputByteLimit);
    std::string correlationError;
    if (!audit::IsStableAuditId(InRunId) || InAttempt == 0) {
        return Result(OperationAuditRunReadCode::InvalidIdentity, InLimits);
    }
    const auto expectedFrozenName = InSpec.inputKind == "commit-plan"
        ? std::string_view{"frozen-plan.json"}
        : std::string_view{"frozen-operation.json"};
    if (!IsValidPolicy(InPolicy) || InSpec.workspaceRoot.empty() ||
        !IsSupportedRouteInputPair(InSpec.route, InSpec.inputKind) ||
        InSpec.frozenFileName != expectedFrozenName ||
        !audit::IsStableAuditId(InSpec.planId) ||
        InSpec.inputIdentity.empty() || InSpec.inputIdentity.size() > 4096 ||
        InSpec.inputIdentity.find('\0') != std::string::npos ||
        InSpec.sourceBytes.empty() ||
        InSpec.frozenBytes.empty() || InSpec.sourceBytes.size() > kInputByteLimit ||
        InSpec.frozenBytes.size() > kInputByteLimit ||
        !ValidateOperationCorrelationEnvelope(InSpec.correlation, true, &correlationError) ||
        InSpec.correlation.runId != InRunId || InSpec.correlation.attempt != InAttempt) {
        return Result(OperationAuditRunReadCode::InvalidConfiguration, InLimits);
    }
    if (InExpectedReceiptId &&
        (InExpectedReceiptId->size() != 64 ||
         !std::all_of(InExpectedReceiptId->begin(), InExpectedReceiptId->end(), [](const char ch) {
             return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
         }))) {
        return Result(OperationAuditRunReadCode::InvalidIdentity, InLimits);
    }
    std::string error;
    const auto paths = ResolveOperationAuditPaths(InSpec, InRunId, InAttempt, &error);
    if (!paths)
        return Result(OperationAuditRunReadCode::InvalidIdentity, InLimits);

    std::error_code attemptStatusError;
    const auto attemptStatus = std::filesystem::symlink_status(paths->attemptRoot,
                                                                attemptStatusError);
    if (attemptStatus.type() == std::filesystem::file_type::not_found ||
        attemptStatusError == std::errc::no_such_file_or_directory) {
        return Result(OperationAuditRunReadCode::AttemptMissing, InLimits);
    }
    if (attemptStatusError || std::filesystem::is_symlink(attemptStatus) ||
        !std::filesystem::is_directory(attemptStatus)) {
        return Result(OperationAuditRunReadCode::NonRegularEvidence, InLimits);
    }

    if (const auto marker = ProbeMarkers(*paths, InSpec, InRunId, InAttempt,
                                         InLimits, InPolicy.markerMatch)) {
        return *marker;
    }
    const auto failAfterMarkerProbe = [&](const OperationAuditRunReadCode InCode) {
        if (const auto marker = ProbeMarkers(*paths, InSpec, InRunId, InAttempt,
                                             InLimits, InPolicy.markerMatch)) {
            return *marker;
        }
        return Result(InCode, InLimits);
    };

    auto eventsInput = ReadEvidence(paths->events, InLimits.maxEventStreamBytes);
    if (eventsInput.state != EvidenceInputState::Ready)
        return failAfterMarkerProbe(CodeFor(eventsInput.state));
    auto receiptInput = ReadEvidence(paths->receipt, InLimits.maxInputBytes);
    if (receiptInput.state != EvidenceInputState::Ready)
        return failAfterMarkerProbe(CodeFor(receiptInput.state));
    auto frozenInput = ReadEvidence(paths->frozenInput, InLimits.maxInputBytes);
    if (frozenInput.state != EvidenceInputState::Ready)
        return failAfterMarkerProbe(CodeFor(frozenInput.state));

    // The audit contract permits a crash receipt before the first event.  Its
    // event stream is the empty byte sequence, which the general JSONL parser
    // deliberately rejects for ordinary streams.  Keep that framing rule for
    // non-empty evidence, but let the receipt/trace contract decide this one
    // explicit zero-event representation.
    const bool zeroEventStream = eventsInput.bytes.empty();
    audit::AuditEventsParseResult events;
    if (!zeroEventStream)
        events = audit::ParseAuditEventsJsonl(eventsInput.bytes);
    auto receipt = audit::ParseRunReceiptJson(receiptInput.bytes);
    if (HasValidationCode(events.validation, "unsupported_schema") ||
        HasValidationCode(receipt.validation, "unsupported_schema")) {
        return failAfterMarkerProbe(OperationAuditRunReadCode::UnsupportedSchema);
    }
    if ((!zeroEventStream && !events.ok()) || !receipt.ok() || !receipt.value)
        return failAfterMarkerProbe(OperationAuditRunReadCode::MalformedEvidence);
    const auto trace = audit::ValidateRunTrace(*receipt.value, events.values);
    if (!trace.ok())
        return failAfterMarkerProbe(OperationAuditRunReadCode::TraceInvalid);
    if (zeroEventStream && (!receipt.value->repositories.empty() ||
                            !receipt.value->policyRefs.empty() ||
                            !receipt.value->approvalRefs.empty() ||
                            !receipt.value->artifacts.empty())) {
        return failAfterMarkerProbe(OperationAuditRunReadCode::TraceInvalid);
    }

    if (receipt.value->runId != InRunId || receipt.value->attempt != InAttempt)
        return failAfterMarkerProbe(OperationAuditRunReadCode::ReceiptMismatch);
    if (receipt.value->planId != InSpec.planId)
        return failAfterMarkerProbe(OperationAuditRunReadCode::PlanMismatch);

    const auto& correlation = receipt.value->correlation;
    const bool correlationMatches =
        InPolicy.callerCorrelation == OperationAuditCallerCorrelationPolicy::StandaloneWildcard
        ? true
        : InSpec.correlation.mode == "standalone"
        ? correlation.mode == audit::CorrelationMode::Standalone
        : correlation.mode == audit::CorrelationMode::Koa &&
            correlation.productId == InSpec.correlation.productId &&
            correlation.topicId ==
                (InSpec.correlation.topicId.empty()
                     ? std::nullopt
                     : std::optional<std::string>(InSpec.correlation.topicId)) &&
            correlation.itemId == InSpec.correlation.itemId &&
            correlation.workOrderId == InSpec.correlation.workOrderId &&
            correlation.requestId == InSpec.correlation.requestId &&
            correlation.producerId == InSpec.correlation.producerId &&
            correlation.routeId == InSpec.correlation.routeId &&
            !correlation.agentId.has_value();
    const auto receiptParentRunId = receipt.value->parentRunId.value_or("");
    const bool parentMatches = InPolicy.callerCorrelation ==
            OperationAuditCallerCorrelationPolicy::LegacyVerify
        ? InSpec.correlation.parentRunId.empty() ||
            InSpec.correlation.parentRunId == receiptParentRunId
        : InSpec.correlation.parentRunId == receiptParentRunId;
    if (!correlationMatches ||
        (InPolicy.callerCorrelation != OperationAuditCallerCorrelationPolicy::StandaloneWildcard &&
         (InSpec.correlation.runId != receipt.value->runId ||
          InSpec.correlation.attempt != receipt.value->attempt || !parentMatches))) {
        return failAfterMarkerProbe(OperationAuditRunReadCode::CorrelationMismatch);
    }

    const auto frozenSha256 = audit::Sha256Hex(frozenInput.bytes);
    if (frozenSha256 != receipt.value->planSha256)
        return failAfterMarkerProbe(OperationAuditRunReadCode::HashMismatch);
    if (const auto frozenError = ValidateFrozenInput(InSpec, InRunId, InAttempt,
                                                     *receipt.value,
                                                     frozenInput.bytes,
                                                     frozenSha256, InPolicy.callerCorrelation,
                                                     InPolicy.routeBinding)) {
        return failAfterMarkerProbe(*frozenError);
    }

    if (const auto marker = ProbeMarkers(*paths, InSpec, InRunId, InAttempt,
                                         InLimits, InPolicy.markerMatch)) {
        return *marker;
    }

    const auto receiptSha256 = audit::Sha256Hex(receiptInput.bytes);
    if (InExpectedReceiptId && *InExpectedReceiptId != receiptSha256)
        return failAfterMarkerProbe(OperationAuditRunReadCode::ReceiptMismatch);
    auto projection = Project(*receipt.value, events.values, frozenSha256, receiptSha256, InLimits);
    return {.state = projection.previewTruncated ? OperationAuditRunReadState::Truncated
                                                 : OperationAuditRunReadState::Ready,
            .code = OperationAuditRunReadCode::None,
            .run = std::move(projection)};
}

} // namespace kano::git::commands
