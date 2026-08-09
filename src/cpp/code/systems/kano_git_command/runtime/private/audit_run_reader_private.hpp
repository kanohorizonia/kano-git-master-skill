#pragma once

#include "audit_evidence_directory.hpp"
#include "audit_run_reader.hpp"

namespace kano::git::commands {

// Internal entry for callers that already own the one pinned attempt handle.
// It performs no namespace reopen; all markers and evidence use InDirectory.
[[nodiscard]] auto ReadOperationAuditRunFromPinnedDirectory(
    const AuditEvidenceDirectory& InDirectory,
    const OperationAuditPaths& InPaths,
    const OperationAuditSpec& InSpec,
    std::string_view InRunId,
    std::uint32_t InAttempt,
    OperationAuditRunReadLimits InLimits = {},
    std::optional<std::string_view> InExpectedReceiptId = std::nullopt,
    OperationAuditRunReadPolicy InPolicy = {}) -> OperationAuditRunReadResult;

} // namespace kano::git::commands
