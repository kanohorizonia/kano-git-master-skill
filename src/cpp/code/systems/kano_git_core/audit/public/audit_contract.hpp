#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace kano::git::audit {

inline constexpr std::string_view kAuditEventSchemaName = "kog.auditEvent";
inline constexpr std::string_view kRunReceiptSchemaName = "kog.runReceipt";
inline constexpr std::uint32_t kAuditSchemaVersionV1 = 1;

enum class OutcomeState {
    Succeeded,
    Failed,
    Partial,
    Blocked,
    Cancelled,
    TimedOut,
    Unknown,
};

enum class RedactionStatus {
    NotRequired,
    Redacted,
    Withheld,
};

enum class WorktreeState {
    Clean,
    Dirty,
    Unknown,
};

enum class CorrelationMode {
    Standalone,
    Koa,
};

struct CorrelationRefs {
    CorrelationMode mode = CorrelationMode::Standalone;
    std::optional<std::string> productId;
    std::optional<std::string> topicId;
    std::optional<std::string> itemId;
    std::optional<std::string> workOrderId;
    std::optional<std::string> requestId;
    std::optional<std::string> producerId;
    std::optional<std::string> routeId;
    std::optional<std::string> agentId;

    auto operator==(const CorrelationRefs&) const -> bool = default;
};

struct RepositoryState {
    std::optional<std::string> headSha;
    std::optional<std::string> branch;
    WorktreeState worktreeState = WorktreeState::Unknown;
    std::optional<std::string> dirtyFingerprint;
    std::optional<std::string> upstreamHeadSha;
    std::optional<std::uint64_t> ahead;
    std::optional<std::uint64_t> behind;

    auto operator==(const RepositoryState&) const -> bool = default;
};

struct RepositoryTransition {
    std::string repositoryId;
    RepositoryState before;
    RepositoryState after;

    auto operator==(const RepositoryTransition&) const -> bool = default;
};

struct AuditReference {
    std::string id;
    std::string sha256;

    auto operator==(const AuditReference&) const -> bool = default;
};

struct ArtifactReference {
    std::string id;
    std::string kind;
    std::string sha256;
    std::uint64_t sizeBytes = 0;
    std::string contentType;
    RedactionStatus redactionStatus = RedactionStatus::Withheld;

    auto operator==(const ArtifactReference&) const -> bool = default;
};

struct Outcome {
    OutcomeState status = OutcomeState::Unknown;
    std::optional<int> exitCode;
    std::optional<std::string> reasonCode;
    bool retryable = false;

    auto operator==(const Outcome&) const -> bool = default;
};

struct AuditEvent {
    std::string schemaName = std::string(kAuditEventSchemaName);
    std::uint32_t schemaVersion = kAuditSchemaVersionV1;
    std::string eventId;
    std::string runId;
    std::optional<std::string> parentRunId;
    std::uint32_t attempt = 1;
    std::string planId;
    std::string planSha256;
    std::uint64_t sequence = 0;
    std::string startedAtUtc;
    std::string finishedAtUtc;
    RepositoryTransition repository;
    std::string phase;
    std::string action;
    Outcome outcome;
    CorrelationRefs correlation;
    std::vector<AuditReference> policyRefs;
    std::vector<AuditReference> approvalRefs;
    std::vector<ArtifactReference> artifacts;

    auto operator==(const AuditEvent&) const -> bool = default;
};

struct RunReceipt {
    std::string schemaName = std::string(kRunReceiptSchemaName);
    std::uint32_t schemaVersion = kAuditSchemaVersionV1;
    std::string runId;
    std::optional<std::string> parentRunId;
    std::uint32_t attempt = 1;
    std::string planId;
    std::string planSha256;
    std::string startedAtUtc;
    std::string finishedAtUtc;
    std::uint64_t firstSequence = 0;
    std::uint64_t lastSequence = 0;
    std::uint64_t eventCount = 0;
    std::string eventStreamSha256;
    Outcome terminalOutcome;
    CorrelationRefs correlation;
    std::vector<RepositoryTransition> repositories;
    std::vector<AuditReference> policyRefs;
    std::vector<AuditReference> approvalRefs;
    std::vector<ArtifactReference> artifacts;

    auto operator==(const RunReceipt&) const -> bool = default;
};

struct ValidationIssue {
    std::string path;
    std::string code;
    std::string message;

    auto operator==(const ValidationIssue&) const -> bool = default;
};

struct ValidationResult {
    std::vector<ValidationIssue> issues;

    [[nodiscard]] auto ok() const noexcept -> bool;
};

struct AuditEventParseResult {
    std::optional<AuditEvent> value;
    ValidationResult validation;

    [[nodiscard]] auto ok() const noexcept -> bool;
};

struct RunReceiptParseResult {
    std::optional<RunReceipt> value;
    ValidationResult validation;

    [[nodiscard]] auto ok() const noexcept -> bool;
};

struct AuditEventsParseResult {
    std::vector<AuditEvent> values;
    ValidationResult validation;

    [[nodiscard]] auto ok() const noexcept -> bool;
};

struct SerializationResult {
    std::string json;
    ValidationResult validation;

    [[nodiscard]] auto ok() const noexcept -> bool;
};

[[nodiscard]] auto OutcomeStateName(OutcomeState InState) -> std::string_view;
[[nodiscard]] auto RedactionStatusName(RedactionStatus InStatus)
    -> std::string_view;
[[nodiscard]] auto WorktreeStateName(WorktreeState InState) -> std::string_view;
[[nodiscard]] auto CorrelationModeName(CorrelationMode InMode)
    -> std::string_view;

[[nodiscard]] auto ValidateAuditEvent(const AuditEvent& InEvent)
    -> ValidationResult;
[[nodiscard]] auto ValidateRunReceipt(const RunReceipt& InReceipt)
    -> ValidationResult;
[[nodiscard]] auto ValidateRunTrace(const RunReceipt& InReceipt,
                                    std::span<const AuditEvent> InEvents)
    -> ValidationResult;

[[nodiscard]] auto ParseAuditEventJson(std::string_view InJson)
    -> AuditEventParseResult;
[[nodiscard]] auto ParseAuditEventsJsonl(std::string_view InJsonl)
    -> AuditEventsParseResult;
[[nodiscard]] auto ParseRunReceiptJson(std::string_view InJson)
    -> RunReceiptParseResult;

[[nodiscard]] auto SerializeAuditEventJson(const AuditEvent& InEvent)
    -> SerializationResult;
[[nodiscard]] auto SerializeRunReceiptJson(const RunReceipt& InReceipt)
    -> SerializationResult;
[[nodiscard]] auto
SerializeAuditEventsJsonl(std::span<const AuditEvent> InEvents)
    -> SerializationResult;

} // namespace kano::git::audit
