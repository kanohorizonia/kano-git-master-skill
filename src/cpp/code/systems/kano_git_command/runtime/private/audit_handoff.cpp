#include "audit_handoff.hpp"
#include "audit_handoff_private.hpp"

#include "audit_contract.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

namespace kano::git::commands {
namespace {

using Json = nlohmann::json;
using kano::git::audit::CorrelationMode;
using kano::git::audit::OutcomeState;
using kano::git::audit::RedactionStatus;
using kano::git::audit::RepositoryTransition;
using kano::git::audit::WorktreeState;

constexpr std::uint64_t kMaximumJsonSafeInteger = 9'007'199'254'740'991ULL;
constexpr std::uint64_t kMaximumSerializedBytes = 4U << 20U;
constexpr std::size_t kMaximumRows = 4096;
constexpr std::array<std::string_view, 29> kForbiddenKeys{
    "apikey", "argv", "auth", "authorization", "body", "command",
    "credential", "credentials", "env", "environment", "error", "failed",
    "message", "ok", "password", "path", "payload", "privatekey",
    "rawcommand", "secret", "secrets", "stderr", "stdout", "succeeded",
    "success", "token", "tokens", "username", "value"};

// These are metadata values, not evidence bodies. Still, fail closed on
// credential-shaped strings rather than relying only on the receipt schema's
// printable-opaque-ID grammar. Raw evidence IDs and event IDs are hashed before
// output; known stable identifiers are checked before being emitted.
auto ContainsCredentialMarker(std::string_view InValue) -> bool {
    std::string lower;
    lower.reserve(InValue.size());
    for (const char ch : InValue)
        lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    static constexpr std::array<std::string_view, 19> markers{
        "ghp_", "github_pat_", "glpat-", "sk-", "bearer", "akia",
        "xoxb-", "xoxp-", "ya29.", "secret", "password", "api_key",
        "api-key", "apikey", "privatekey", "authorization", "credential",
        "env:", "access_token"};
    return std::any_of(markers.begin(), markers.end(), [&](const auto marker) {
        return lower.find(marker) != std::string::npos;
    });
}

auto IsSafeIdentity(const std::string_view InValue) -> bool {
    return audit::IsStableAuditId(InValue) && !ContainsCredentialMarker(InValue);
}

auto OutcomeName(const OutcomeState InState) -> std::string_view {
    switch (InState) {
    case OutcomeState::Succeeded: return "succeeded";
    case OutcomeState::Failed: return "failed";
    case OutcomeState::Partial: return "partial";
    case OutcomeState::Blocked: return "blocked";
    case OutcomeState::Cancelled: return "cancelled";
    case OutcomeState::TimedOut: return "timed-out";
    case OutcomeState::Unknown: return "unknown";
    }
    return "unknown";
}

auto RedactionName(const RedactionStatus InStatus) -> std::string_view {
    switch (InStatus) {
    case RedactionStatus::NotRequired: return "not-required";
    case RedactionStatus::Redacted: return "redacted";
    case RedactionStatus::Withheld: return "withheld";
    }
    return "withheld";
}

auto HandoffCorrelationModeName(const CorrelationMode InMode) -> std::string_view {
    switch (InMode) {
    case CorrelationMode::Standalone: return "standalone";
    case CorrelationMode::Koa: return "koa";
    }
    return "standalone";
}

auto IsSha256(const std::string_view InValue) -> bool {
    return InValue.size() == 64 &&
        std::all_of(InValue.begin(), InValue.end(), [](const char ch) {
            return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
        });
}

auto IsGitOid(const std::string_view InValue) -> bool {
    return (InValue.size() == 40 || InValue.size() == 64) &&
        std::all_of(InValue.begin(), InValue.end(), [](const char ch) {
            return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
        });
}

auto IsSafeSemanticToken(const std::string_view InValue) -> bool {
    return !InValue.empty() && InValue.size() <= 96 && InValue.front() >= 'a' &&
        InValue.front() <= 'z' &&
        std::all_of(InValue.begin(), InValue.end(), [](const char ch) {
            return (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') ||
                ch == '.' || ch == '_' || ch == '-';
        });
}

auto IsSafeRepositoryId(const std::string_view InValue) -> bool {
    if (InValue.empty() || InValue.size() > 256 || InValue.front() == '/' ||
        InValue.front() == '\\' || InValue.find('\\') != std::string_view::npos ||
        InValue.find("//") != std::string_view::npos || InValue.back() == '/' ||
        ContainsCredentialMarker(InValue)) return false;
    if (InValue.size() >= 2 &&
        std::isalpha(static_cast<unsigned char>(InValue[0])) != 0 &&
        InValue[1] == ':') return false;
    std::size_t begin = 0;
    while (begin <= InValue.size()) {
        const auto slash = InValue.find('/', begin);
        const auto end = slash == std::string_view::npos ? InValue.size() : slash;
        const auto component = InValue.substr(begin, end - begin);
        if (component.empty() || component == "." || component == "..") return false;
        for (const char ch : component) {
            const auto byte = static_cast<unsigned char>(ch);
            if (byte < 0x21U || byte > 0x7eU) return false;
        }
        if (slash == std::string_view::npos) break;
        begin = slash + 1;
    }
    return true;
}

auto IsSafeContentType(const std::string_view InValue) -> bool {
    if (InValue.empty()) return true;
    if (InValue.size() > 128) return false;
    bool slash = false;
    for (const char ch : InValue) {
        if (ch == '/') {
            if (slash) return false;
            slash = true;
            continue;
        }
        if (!((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') ||
              ch == '.' || ch == '+' || ch == '-')) return false;
    }
    return slash && InValue.front() != '/' && InValue.back() != '/';
}

auto SafeCorrelation(const audit::CorrelationRefs& InCorrelation) -> bool {
    const auto valid = [](const std::optional<std::string>& value) {
        return !value || IsSafeIdentity(*value);
    };
    return valid(InCorrelation.productId) && valid(InCorrelation.topicId) &&
        valid(InCorrelation.itemId) && valid(InCorrelation.workOrderId) &&
        valid(InCorrelation.requestId) && valid(InCorrelation.producerId) &&
        valid(InCorrelation.routeId) && valid(InCorrelation.agentId);
}

auto ContainsForbiddenKey(const Json& InValue) -> bool {
    if (InValue.is_object()) {
        for (auto it = InValue.begin(); it != InValue.end(); ++it) {
            std::string normalized;
            normalized.reserve(it.key().size());
            for (const char ch : it.key()) {
                const auto byte = static_cast<unsigned char>(ch);
                if (std::isalnum(byte) != 0)
                    normalized.push_back(static_cast<char>(std::tolower(byte)));
            }
            if (std::find(kForbiddenKeys.begin(), kForbiddenKeys.end(),
                          std::string_view(normalized)) != kForbiddenKeys.end()) return true;
            if (ContainsForbiddenKey(it.value())) return true;
        }
    } else if (InValue.is_array()) {
        for (const auto& item : InValue)
            if (ContainsForbiddenKey(item)) return true;
    }
    return false;
}

auto LimitsValid(const AuditHandoffLimits& InLimits) -> bool {
    return InLimits.maxSerializedBytes >= 256 &&
        InLimits.maxSerializedBytes <= kMaximumSerializedBytes &&
        InLimits.maxEvents > 0 && InLimits.maxEvents <= kMaximumRows &&
        InLimits.maxRepositories > 0 && InLimits.maxRepositories <= kMaximumRows &&
        InLimits.maxEvidenceReferences > 0 &&
        InLimits.maxEvidenceReferences <= kMaximumRows &&
        InLimits.pinnedReadLimits.maxEventStreamBytes > 0 &&
        InLimits.pinnedReadLimits.maxEventStreamBytes <= (64U << 20U) &&
        InLimits.pinnedReadLimits.maxInputBytes > 0 &&
        InLimits.pinnedReadLimits.maxInputBytes <= (4U << 20U) &&
        InLimits.pinnedReadLimits.maxPreviewBytes > 0 &&
        InLimits.pinnedReadLimits.maxPreviewBytes <= (16U << 10U) &&
        InLimits.pinnedReadLimits.maxEventRecords > 0 &&
        InLimits.pinnedReadLimits.maxEventRecords <= kMaximumRows &&
        InLimits.pinnedReadLimits.maxRepositories > 0 &&
        InLimits.pinnedReadLimits.maxRepositories <= kMaximumRows &&
        InLimits.pinnedReadLimits.maxEvidenceReferences > 0 &&
        InLimits.pinnedReadLimits.maxEvidenceReferences <= kMaximumRows &&
        InLimits.pinnedReadLimits.maxDiagnosticBytes > 0 &&
        InLimits.pinnedReadLimits.maxDiagnosticBytes <= 192;
}

auto ProjectionConsistent(const OperationAuditRunProjection& InRun) -> bool {
    const bool listTruncated = InRun.eventsTruncated || InRun.repositoriesTruncated ||
        InRun.evidenceTruncated;
    return InRun.totalEventRecords >= InRun.retainedEventRecords &&
        InRun.totalRepositories >= InRun.retainedRepositories &&
        InRun.totalEvidenceReferences >= InRun.retainedEvidenceReferences &&
        InRun.eventsTruncated == (InRun.totalEventRecords > InRun.retainedEventRecords) &&
        InRun.repositoriesTruncated ==
            (InRun.totalRepositories > InRun.retainedRepositories) &&
        InRun.evidenceTruncated ==
            (InRun.totalEvidenceReferences > InRun.retainedEvidenceReferences) &&
        InRun.retainedEventRecords == InRun.events.size() &&
        InRun.retainedRepositories == InRun.repositories.size() &&
        InRun.retainedEvidenceReferences == InRun.evidence.size() &&
        InRun.events.size() <= kMaximumRows &&
        InRun.repositories.size() <= kMaximumRows &&
        InRun.evidence.size() <= kMaximumRows &&
        InRun.totalEventRecords <= kMaximumJsonSafeInteger &&
        InRun.totalRepositories <= kMaximumJsonSafeInteger &&
        InRun.totalEvidenceReferences <= kMaximumJsonSafeInteger &&
        InRun.redactedEvidenceCount <= InRun.totalEvidenceReferences &&
        InRun.withheldEvidenceCount <= InRun.totalEvidenceReferences &&
        InRun.previewTruncated == listTruncated;
}

auto SafeProjectionValues(const OperationAuditRunProjection& InRun) -> bool {
    if (!IsSafeIdentity(InRun.runId) || !IsSafeIdentity(InRun.planId) ||
        (InRun.parentRunId && !IsSafeIdentity(*InRun.parentRunId)) ||
        !IsSha256(InRun.receiptId) || !IsSha256(InRun.planSha256) ||
        !IsSha256(InRun.frozenInputSha256) || !IsSha256(InRun.eventStreamSha256) ||
        !IsSha256(InRun.repositoryIdentityHeadSha256) ||
        !IsSha256(InRun.catalogRepositoryPreviewIdentityHeadSha256) ||
        !SafeCorrelation(InRun.correlation)) return false;
    for (const auto& repo : InRun.repositories) {
        if (!IsSafeRepositoryId(repo.repositoryId) ||
            (repo.before.headSha && !IsGitOid(*repo.before.headSha)) ||
            (repo.after.headSha && !IsGitOid(*repo.after.headSha))) return false;
    }
    for (const auto& event : InRun.events) {
        if (!IsSafeRepositoryId(event.repositoryId) ||
            !IsSafeSemanticToken(event.phase) || !IsSafeSemanticToken(event.action) ||
            (event.beforeHeadSha && !IsGitOid(*event.beforeHeadSha)) ||
            (event.afterHeadSha && !IsGitOid(*event.afterHeadSha))) return false;
    }
    for (const auto& evidence : InRun.evidence) {
        if (evidence.category != "policy" && evidence.category != "approval" &&
            evidence.category != "artifact") return false;
        if (!IsSafeSemanticToken(evidence.kind) && !evidence.kind.empty()) return false;
        if (!IsSafeContentType(evidence.contentType) || !IsSha256(evidence.sha256)) return false;
        switch (evidence.redactionStatus) {
        case RedactionStatus::NotRequired:
        case RedactionStatus::Redacted:
        case RedactionStatus::Withheld: break;
        default: return false;
        }
    }
    return true;
}

auto OutcomeJson(const audit::Outcome& InOutcome) -> Json {
    Json result = Json::object();
    result["status"] = std::string(OutcomeName(InOutcome.status));
    result["exitCode"] = InOutcome.exitCode ? Json(*InOutcome.exitCode) : Json(nullptr);
    result["retryable"] = InOutcome.retryable;
    // reasonCode is opaque printable text, not a semantic token; omit it to
    // avoid turning a metadata field into a secret/path/command exfiltration.
    return result;
}

auto CorrelationJson(const audit::CorrelationRefs& InCorrelation) -> Json {
    Json result = Json::object();
    result["mode"] = std::string(HandoffCorrelationModeName(InCorrelation.mode));
    result["productId"] = InCorrelation.productId ? Json(*InCorrelation.productId) : Json(nullptr);
    result["topicId"] = InCorrelation.topicId ? Json(*InCorrelation.topicId) : Json(nullptr);
    result["itemId"] = InCorrelation.itemId ? Json(*InCorrelation.itemId) : Json(nullptr);
    result["workOrderId"] = InCorrelation.workOrderId ? Json(*InCorrelation.workOrderId) : Json(nullptr);
    result["requestId"] = InCorrelation.requestId ? Json(*InCorrelation.requestId) : Json(nullptr);
    result["producerId"] = InCorrelation.producerId ? Json(*InCorrelation.producerId) : Json(nullptr);
    result["routeId"] = InCorrelation.routeId ? Json(*InCorrelation.routeId) : Json(nullptr);
    result["agentId"] = InCorrelation.agentId ? Json(*InCorrelation.agentId) : Json(nullptr);
    return result;
}

auto BuildProjectionJson(const OperationAuditRunProjection& InRun,
                         const AuditHandoffLimits& InLimits) -> Json {
    const auto retainedEvents = std::min<std::uint64_t>(InRun.retainedEventRecords,
                                                        InLimits.maxEvents);
    const auto retainedRepos = std::min<std::uint64_t>(InRun.retainedRepositories,
                                                       InLimits.maxRepositories);
    const auto retainedEvidence = std::min<std::uint64_t>(InRun.retainedEvidenceReferences,
                                                          InLimits.maxEvidenceReferences);
    const auto omittedEvents = InRun.totalEventRecords - retainedEvents;
    const auto omittedRepos = InRun.totalRepositories - retainedRepos;
    const auto omittedEvidence = InRun.totalEvidenceReferences - retainedEvidence;
    const auto readerOmittedEvents = InRun.totalEventRecords - InRun.retainedEventRecords;
    const auto readerOmittedRepos = InRun.totalRepositories - InRun.retainedRepositories;
    const auto readerOmittedEvidence = InRun.totalEvidenceReferences - InRun.retainedEvidenceReferences;
    const auto handoffOmittedEvents = InRun.retainedEventRecords - retainedEvents;
    const auto handoffOmittedRepos = InRun.retainedRepositories - retainedRepos;
    const auto handoffOmittedEvidence = InRun.retainedEvidenceReferences - retainedEvidence;

    std::vector<RepositoryTransition> sortedRepos = InRun.repositories;
    std::sort(sortedRepos.begin(), sortedRepos.end(), [](const auto& left, const auto& right) {
        return left.repositoryId < right.repositoryId;
    });
    std::vector<OperationAuditEventPreview> sortedEvents = InRun.events;
    std::sort(sortedEvents.begin(), sortedEvents.end(), [](const auto& left, const auto& right) {
        return std::tie(left.sequence, left.eventId) < std::tie(right.sequence, right.eventId);
    });
    auto sortedEvidence = InRun.evidence;
    std::sort(sortedEvidence.begin(), sortedEvidence.end(), [](const auto& left, const auto& right) {
        return std::tie(left.category, left.id, left.kind, left.sha256) <
               std::tie(right.category, right.id, right.kind, right.sha256);
    });

    Json repos = Json::array();
    for (std::size_t i = 0; i < sortedRepos.size() && i < InLimits.maxRepositories; ++i) {
        const auto& source = sortedRepos[i];
        Json item = Json::object();
        item["repositoryId"] = source.repositoryId;
        item["beforeHeadSha"] = source.before.headSha ? Json(*source.before.headSha) : Json(nullptr);
        item["afterHeadSha"] = source.after.headSha ? Json(*source.after.headSha) : Json(nullptr);
        repos.push_back(std::move(item));
    }

    Json events = Json::array();
    for (std::size_t i = 0; i < sortedEvents.size() && i < InLimits.maxEvents; ++i) {
        const auto& source = sortedEvents[i];
        Json item = Json::object();
        item["eventIdentitySha256"] = audit::Sha256Hex(source.eventId);
        item["sequence"] = source.sequence;
        item["repositoryId"] = source.repositoryId;
        item["beforeHeadSha"] = source.beforeHeadSha ? Json(*source.beforeHeadSha) : Json(nullptr);
        item["afterHeadSha"] = source.afterHeadSha ? Json(*source.afterHeadSha) : Json(nullptr);
        item["phase"] = source.phase;
        item["action"] = source.action;
        item["outcome"] = OutcomeJson(source.outcome);
        events.push_back(std::move(item));
    }

    Json evidence = Json::array();
    for (std::size_t i = 0; i < sortedEvidence.size() && i < InLimits.maxEvidenceReferences; ++i) {
        const auto& source = sortedEvidence[i];
        Json item = Json::object();
        item["category"] = source.category;
        item["referenceIdentitySha256"] = audit::Sha256Hex(source.id);
        item["kind"] = source.kind;
        item["sha256"] = source.sha256;
        item["sizeBytes"] = source.sizeBytes;
        item["contentType"] = source.contentType;
        item["redactionStatus"] = std::string(RedactionName(source.redactionStatus));
        evidence.push_back(std::move(item));
    }

    const bool truncated = InRun.previewTruncated || omittedEvents > 0 || omittedRepos > 0 ||
        omittedEvidence > 0;
    Json doc = Json::object();
    doc["schemaName"] = std::string(kAuditHandoffSchemaName);
    doc["schemaVersion"] = kAuditHandoffSchemaVersion;
    doc["runId"] = InRun.runId;
    doc["parentRunId"] = InRun.parentRunId ? Json(*InRun.parentRunId) : Json(nullptr);
    doc["attempt"] = InRun.attempt;
    doc["planId"] = InRun.planId;
    doc["planSha256"] = InRun.planSha256;
    doc["frozenInputSha256"] = InRun.frozenInputSha256;
    doc["receiptSha256"] = InRun.receiptId;
    doc["eventStreamSha256"] = InRun.eventStreamSha256;
    doc["finishedAtUtc"] = InRun.finishedAtUtc;
    doc["correlation"] = CorrelationJson(InRun.correlation);
    doc["terminalOutcome"] = OutcomeJson(InRun.terminalOutcome);
    doc["repositoryTransitions"] = std::move(repos);
    doc["events"] = std::move(events);
    doc["evidenceReferences"] = std::move(evidence);
    doc["redaction"] = Json{
        {"redactedEvidenceCount", InRun.redactedEvidenceCount},
        {"withheldEvidenceCount", InRun.withheldEvidenceCount},
        {"hasRedactedEvidence", InRun.hasRedactedEvidence},
        {"hasWithheldEvidence", InRun.hasWithheldEvidence},
    };
    doc["truncation"] = Json{
        {"totalEvents", InRun.totalEventRecords},
        {"readerRetainedEvents", InRun.retainedEventRecords},
        {"retainedEvents", retainedEvents},
        {"readerOmittedEvents", readerOmittedEvents},
        {"handoffOmittedEvents", handoffOmittedEvents},
        {"omittedEvents", omittedEvents},
        {"truncatedEvents", omittedEvents},
        {"totalRepositories", InRun.totalRepositories},
        {"readerRetainedRepositories", InRun.retainedRepositories},
        {"retainedRepositories", retainedRepos},
        {"readerOmittedRepositories", readerOmittedRepos},
        {"handoffOmittedRepositories", handoffOmittedRepos},
        {"omittedRepositories", omittedRepos},
        {"truncatedRepositories", omittedRepos},
        {"totalEvidenceReferences", InRun.totalEvidenceReferences},
        {"readerRetainedEvidenceReferences", InRun.retainedEvidenceReferences},
        {"retainedEvidenceReferences", retainedEvidence},
        {"readerOmittedEvidenceReferences", readerOmittedEvidence},
        {"handoffOmittedEvidenceReferences", handoffOmittedEvidence},
        {"omittedEvidenceReferences", omittedEvidence},
        {"truncatedEvidenceReferences", omittedEvidence},
        {"retainedPreviewBytes", InRun.retainedPreviewBytes},
        {"readerTruncated", InRun.previewTruncated},
        {"truncated", truncated},
    };
    doc["generationLimits"] = Json{
        {"maxSerializedBytes", InLimits.maxSerializedBytes},
        {"maxEvents", InLimits.maxEvents},
        {"maxRepositories", InLimits.maxRepositories},
        {"maxEvidenceReferences", InLimits.maxEvidenceReferences},
        {"readerMaxEventStreamBytes", InLimits.pinnedReadLimits.maxEventStreamBytes},
        {"readerMaxInputBytes", InLimits.pinnedReadLimits.maxInputBytes},
        {"readerMaxPreviewBytes", InLimits.pinnedReadLimits.maxPreviewBytes},
        {"readerMaxEventRecords", InLimits.pinnedReadLimits.maxEventRecords},
        {"readerMaxRepositories", InLimits.pinnedReadLimits.maxRepositories},
        {"readerMaxEvidenceReferences", InLimits.pinnedReadLimits.maxEvidenceReferences},
    };
    doc["identityHashes"] = Json{
        {"repositoryIdentityHeadSha256", InRun.repositoryIdentityHeadSha256},
        {"catalogRepositoryPreviewIdentityHeadSha256", InRun.catalogRepositoryPreviewIdentityHeadSha256},
    };
    return doc;
}

} // namespace

auto AuditHandoffResultCodeName(const AuditHandoffResultCode InCode) -> std::string_view {
    switch (InCode) {
    case AuditHandoffResultCode::None: return "none";
    case AuditHandoffResultCode::InvalidConfiguration: return "invalid_configuration";
    case AuditHandoffResultCode::ReadNotVerified: return "read_not_verified";
    case AuditHandoffResultCode::InvalidVerifiedProjection: return "invalid_verified_projection";
    case AuditHandoffResultCode::UnsafeMetadata: return "unsafe_metadata";
    case AuditHandoffResultCode::ByteCapExceeded: return "byte_cap_exceeded";
    }
    return "invalid_configuration";
}

auto SerializeVerifiedAuditHandoff(const OperationAuditRunReadResult& InRead,
                                   const AuditHandoffLimits& InLimits)
    -> AuditHandoffResult {
    AuditHandoffResult result;
    result.readState = InRead.state;
    result.readCode = InRead.code;
    if (!LimitsValid(InLimits)) {
        result.code = AuditHandoffResultCode::InvalidConfiguration;
        return result;
    }
    if (!InRead.verified() || !InRead.run) {
        result.code = AuditHandoffResultCode::ReadNotVerified;
        return result;
    }
    const auto& projection = *InRead.run;
    if (!ProjectionConsistent(projection)) {
        result.code = AuditHandoffResultCode::InvalidVerifiedProjection;
        return result;
    }
    if (!SafeProjectionValues(projection)) {
        result.code = AuditHandoffResultCode::UnsafeMetadata;
        return result;
    }

    const auto document = BuildProjectionJson(projection, InLimits);
    if (ContainsForbiddenKey(document)) {
        result.code = AuditHandoffResultCode::UnsafeMetadata;
        return result;
    }
    const auto serialized = document.dump();
    if (serialized.size() > InLimits.maxSerializedBytes) {
        result.code = AuditHandoffResultCode::ByteCapExceeded;
        return result;
    }

    result.code = AuditHandoffResultCode::None;
    result.serialized = serialized;
    result.sizeBytes = serialized.size();
    result.sha256 = audit::Sha256Hex(serialized);
    result.retainedEvents = std::min<std::uint64_t>(projection.retainedEventRecords,
                                                    InLimits.maxEvents);
    result.omittedEvents = projection.totalEventRecords - result.retainedEvents;
    result.retainedRepositories = std::min<std::uint64_t>(projection.retainedRepositories,
                                                         InLimits.maxRepositories);
    result.omittedRepositories = projection.totalRepositories - result.retainedRepositories;
    result.retainedEvidenceReferences = std::min<std::uint64_t>(projection.retainedEvidenceReferences,
                                                                InLimits.maxEvidenceReferences);
    result.omittedEvidenceReferences = projection.totalEvidenceReferences -
        result.retainedEvidenceReferences;
    result.truncated = projection.previewTruncated || result.omittedEvents > 0 ||
        result.omittedRepositories > 0 || result.omittedEvidenceReferences > 0;
    return result;
}

auto BuildAuditHandoffJson(const OperationAuditVerificationRequest& InRequest,
                           AuditHandoffLimits InLimits) -> AuditHandoffResult {
    if (!LimitsValid(InLimits)) {
        AuditHandoffResult result;
        result.code = AuditHandoffResultCode::InvalidConfiguration;
        return result;
    }
    // Exactly one read; the existing verification path is the only owner of
    // pinned receipt/event/frozen-input validation and namespace stability.
    const auto read = ReadOperationAuditVerification(InRequest,
                                                     InLimits.pinnedReadLimits);
    return SerializeVerifiedAuditHandoff(read, InLimits);
}

} // namespace kano::git::commands
