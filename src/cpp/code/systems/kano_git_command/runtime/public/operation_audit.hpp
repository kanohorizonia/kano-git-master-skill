#pragma once

#include "audit_contract.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace kano::git::commands {

inline constexpr std::string_view kOperationAuditCapabilitySchema =
    "kog.auditCapability";
inline constexpr std::string_view kOperationAuditVerificationSchema =
    "kog.auditVerification";
inline constexpr std::uint32_t kOperationAuditProtocolVersion = 1;

struct OperationCorrelationEnvelope {
    std::string mode = "standalone";
    std::string productId;
    std::string topicId;
    std::string itemId;
    std::string workOrderId;
    std::string requestId;
    std::string runId;
    std::string parentRunId;
    std::string producerId;
    std::string routeId;
    std::uint32_t attempt = 1;

    auto operator==(const OperationCorrelationEnvelope&) const -> bool = default;
};

struct OperationAuditSpec {
    std::filesystem::path workspaceRoot;
    std::optional<std::filesystem::path> sourcePath;
    std::string inputIdentity;
    std::string inputKind;
    std::string route;
    std::string planId;
    std::string sourceBytes;
    std::string frozenBytes;
    std::string frozenFileName;
    OperationCorrelationEnvelope correlation;
};

struct OperationAuditPaths {
    std::filesystem::path auditRoot;
    std::filesystem::path runRoot;
    std::filesystem::path attemptRoot;
    std::filesystem::path frozenInput;
    std::filesystem::path events;
    std::filesystem::path receipt;
    std::filesystem::path publicationPending;
    std::filesystem::path incomplete;
};

class OperationAuditContext {
public:
    static auto Reserve(OperationAuditSpec InSpec, std::string* OutError)
        -> std::unique_ptr<OperationAuditContext>;
    static auto Current() noexcept -> OperationAuditContext*;
    ~OperationAuditContext();

    OperationAuditContext(const OperationAuditContext&) = delete;
    auto operator=(const OperationAuditContext&) -> OperationAuditContext& = delete;

    auto Capture(const std::filesystem::path& InRepo) const
        -> audit::RepositoryState;
    auto Append(std::string InAction,
                const std::filesystem::path& InRepo,
                const audit::RepositoryState& InBefore,
                std::string InStartedAtUtc,
                int InExitCode,
                std::string* OutError) -> bool;
    auto Finalize(int InExitCode, std::string* OutError) -> bool;
    auto FreezeSupplementalInput(const std::filesystem::path& InSource,
                                 std::string_view InLabel,
                                 std::string_view InIdentity,
                                 std::string* OutError)
        -> std::optional<std::filesystem::path>;
    auto FreezeSupplementalBytes(std::string_view InBytes,
                                 std::string_view InLabel,
                                 std::string_view InIdentity,
                                 std::string* OutError)
        -> std::optional<std::filesystem::path>;

    [[nodiscard]] auto FrozenInputPath() const
        -> const std::filesystem::path&;
    [[nodiscard]] auto SourceSha256() const -> const std::string&;
    [[nodiscard]] auto PlanSha256() const -> const std::string&;
    [[nodiscard]] auto RunId() const -> const std::string&;
    [[nodiscard]] auto ParentRunId() const
        -> const std::optional<std::string>&;
    [[nodiscard]] auto Attempt() const -> std::uint32_t;
    [[nodiscard]] auto InputKind() const -> const std::string&;
    [[nodiscard]] auto Route() const -> const std::string&;
    [[nodiscard]] auto PlanId() const -> const std::string&;
    [[nodiscard]] auto Correlation() const
        -> const OperationCorrelationEnvelope&;
    [[nodiscard]] auto Spec() const -> const OperationAuditSpec&;
    [[nodiscard]] auto Paths() const -> const OperationAuditPaths&;

private:
    OperationAuditContext() = default;
    auto PublishReceipt(int InExitCode, std::string* OutError) -> bool;
    auto PublishIncompleteMarker(std::string_view InReasonCode,
                                 bool InReceiptPublished,
                                 std::string* OutError) -> bool;

    OperationAuditSpec mSpec;
    OperationAuditPaths mPaths;
    OperationAuditContext* mPreviousActive = nullptr;
    std::optional<std::string> mParentRunId;
    std::string mRunId;
    std::string mSourceSha256;
    std::string mPlanSha256;
    std::string mStartedAtUtc;
    audit::CorrelationRefs mCorrelation;
    std::vector<audit::AuditEvent> mEvents;
    std::vector<audit::ArtifactReference> mSupplementalInputs;
    std::intptr_t mEventsHandle = -1;
    std::intptr_t mReceiptHandle = -1;
    bool mFinalized = false;
    bool mFinalizationAttempted = false;
    bool mPoisoned = false;
    bool mIncompletePublished = false;
};

[[nodiscard]] auto ParseOperationCorrelationEnvelope(
    std::string_view InJson,
    std::string* OutError) -> std::optional<OperationCorrelationEnvelope>;
[[nodiscard]] auto ReadBoundedAuditInput(
    const std::filesystem::path& InPath,
    std::uintmax_t InLimit,
    std::string* OutError) -> std::optional<std::string>;
[[nodiscard]] auto ValidateOperationCorrelationEnvelope(
    const OperationCorrelationEnvelope& InEnvelope,
    bool InAllowResolvedStandaloneRun,
    std::string* OutError) -> bool;
[[nodiscard]] auto ResolveStandaloneCorrelation(
    OperationCorrelationEnvelope InEnvelope,
    std::string_view InPlanId,
    std::string_view InSourceSha256) -> OperationCorrelationEnvelope;
[[nodiscard]] auto SerializeOperationCorrelationEnvelope(
    const OperationCorrelationEnvelope& InEnvelope) -> std::string;
[[nodiscard]] auto BuildOperationDescriptor(
    std::string_view InRoute,
    std::string_view InCanonicalOptionsJson,
    const OperationCorrelationEnvelope& InCorrelation,
    std::string* OutError) -> std::optional<std::string>;
[[nodiscard]] auto ResolveOperationAuditPaths(
    const OperationAuditSpec& InSpec,
    std::string_view InRunId,
    std::uint32_t InAttempt,
    std::string* OutError) -> std::optional<OperationAuditPaths>;

[[nodiscard]] auto OperationAuditCapabilityJson() -> std::string;
[[nodiscard]] auto VerifyOperationAuditJson(
    const OperationAuditSpec& InSpec,
    std::string_view InRunId,
    std::uint32_t InAttempt,
    bool* OutTraceValid,
    std::string* OutError) -> std::optional<std::string>;

} // namespace kano::git::commands
