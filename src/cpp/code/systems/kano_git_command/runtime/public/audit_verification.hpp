#pragma once

#include "audit_run_reader.hpp"

#include <cstdint>
#include <filesystem>
#include <string>

namespace kano::git::commands {

// Structured, read-only lookup used by non-CLI audit consumers.  Callers
// provide identity only; the implementation reads and verifies bounded
// evidence through OperationAuditRunReadResult.
struct OperationAuditVerificationRequest {
    std::filesystem::path workspaceRoot;
    std::filesystem::path planFile;
    std::string runId;
    std::uint32_t attempt = 0;
};

[[nodiscard]] auto ReadOperationAuditVerification(
    const OperationAuditVerificationRequest& InRequest,
    OperationAuditRunReadLimits InLimits = {})
    -> OperationAuditRunReadResult;

} // namespace kano::git::commands
