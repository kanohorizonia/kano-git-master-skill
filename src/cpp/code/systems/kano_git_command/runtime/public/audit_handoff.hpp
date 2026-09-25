#pragma once

#include "audit_verification.hpp"

#include <cstdint>
#include <string>
#include <string_view>

namespace kano::git::commands {

inline constexpr std::string_view kAuditHandoffSchemaName = "kog.auditHandoff";
inline constexpr std::uint32_t kAuditHandoffSchemaVersion = 1;

// Limits applied by the handoff serializer after one successful pinned read.
// The pinned reader has its own independently bounded byte/record limits,
// carried here so the emitted generationLimits block records both layers.
struct AuditHandoffLimits {
    std::uint64_t maxSerializedBytes = 256U << 10U;
    std::size_t maxRepositories = 64;
    std::size_t maxEvidenceReferences = 64;
    std::size_t maxEvents = 64;
    OperationAuditRunReadLimits pinnedReadLimits{};
};

enum class AuditHandoffResultCode {
    None,
    InvalidConfiguration,
    ReadNotVerified,
    InvalidVerifiedProjection,
    UnsafeMetadata,
    ByteCapExceeded,
};

[[nodiscard]] auto AuditHandoffResultCodeName(AuditHandoffResultCode InCode)
    -> std::string_view;

struct AuditHandoffResult {
    AuditHandoffResultCode code = AuditHandoffResultCode::InvalidConfiguration;
    // Deterministic schema-v1 JSON. Empty for every unsuccessful result.
    std::string serialized;
    // Exact final byte count and SHA-256 of `serialized` on success.
    std::uint64_t sizeBytes = 0;
    std::string sha256;
    // Verification outcome from the single pinned reader call. On a
    // non-verified read, code/readState/readCode are returned without any
    // projection data or diagnostic path text.
    OperationAuditRunReadState readState = OperationAuditRunReadState::Invalid;
    OperationAuditRunReadCode readCode = OperationAuditRunReadCode::InvalidConfiguration;
    bool truncated = false;
    std::uint64_t retainedEvents = 0;
    std::uint64_t omittedEvents = 0;
    std::uint64_t retainedRepositories = 0;
    std::uint64_t omittedRepositories = 0;
    std::uint64_t retainedEvidenceReferences = 0;
    std::uint64_t omittedEvidenceReferences = 0;
};

// Performs exactly one existing bounded pinned verification read using the
// caller's identity request, then serializes only that verified projection.
// No API accepts a naked projection or an output path. JSON is returned for
// stdout; callers wishing to persist it choose and secure their own sink.
[[nodiscard]] auto BuildAuditHandoffJson(
    const OperationAuditVerificationRequest& InRequest,
    AuditHandoffLimits InLimits = {}) -> AuditHandoffResult;

} // namespace kano::git::commands
