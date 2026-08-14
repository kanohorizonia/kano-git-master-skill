#pragma once

#include "audit_run_reader.hpp"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace kano::git::commands {

// The catalog is a bounded discovery index only.  Its rows are never durable
// evidence and callers must revalidate a selected row through the separately
// pinned reader before displaying it as verified.
enum class OperationAuditCatalogState { Pending, Incomplete, Final };
enum class OperationAuditCatalogQueryCode {
    None, Missing, InvalidConfiguration, InvalidCursor, ExpiredCursor,
    Corrupt, Limit, Unsupported,
};

// Deliberately small, path-free projections.  These fields make catalog
// filtering useful without giving it any evidence-reading authority.
struct OperationAuditCatalogRepositoryCommit {
    std::string repositoryId;
    std::optional<std::string> afterHeadSha;
    auto operator==(const OperationAuditCatalogRepositoryCommit&) const -> bool = default;
};
struct OperationAuditCatalogRedactionSummary {
    std::uint32_t redacted = 0;
    std::uint32_t withheld = 0;
    auto operator==(const OperationAuditCatalogRedactionSummary&) const -> bool = default;
};
struct OperationAuditCatalogTruncationSummary {
    std::uint32_t omittedEvents = 0;
    std::uint32_t omittedRepositories = 0;
    std::uint32_t omittedEvidence = 0;
    auto operator==(const OperationAuditCatalogTruncationSummary&) const -> bool = default;
};

struct OperationAuditCatalogEntry {
    std::string runId;
    std::optional<std::string> parentRunId;
    std::uint32_t attempt = 0;
    std::string inputKind;
    std::string route;
    std::string planId;
    std::string planSha256;
    std::string sourceSha256;
    std::uint64_t sourceSizeBytes = 0;
    // A constrained audit-root leaf name, never an absolute or relative path.
    std::string auditRootSelector;
    std::string auditRootSha256;
    OperationAuditCatalogState state = OperationAuditCatalogState::Pending;
    std::optional<audit::OutcomeState> outcome;
    std::vector<OperationAuditCatalogRepositoryCommit> repositories;
    std::string repositoryIdentityHeadSha256;
    // Hash of the canonical correlation envelope; raw KOA identifiers are
    // neither exposed nor used as filesystem selectors.
    std::string correlationSha256;
    std::string observedAtUtc;
    std::optional<std::string> receiptSha256;
    std::optional<std::string> finishedAtUtc;
    OperationAuditCatalogRedactionSummary redaction;
    OperationAuditCatalogTruncationSummary truncation;
    auto operator==(const OperationAuditCatalogEntry&) const -> bool = default;
};

struct OperationAuditCatalogFilter {
    std::optional<std::string> runId;
    std::optional<std::string> planId;
    std::optional<OperationAuditCatalogState> state;
    std::optional<audit::OutcomeState> outcome;
    std::optional<std::string> repositoryId;
    std::optional<std::string> correlationSha256;
    std::optional<std::string> observedNotBeforeUtc;
    std::optional<std::string> observedBeforeUtc;
};

struct OperationAuditCatalogQueryLimits {
    std::size_t maxRows = 64;
    std::size_t maxBytes = 256U << 10U;
    std::chrono::seconds cursorLifetime = std::chrono::minutes(10);
    std::chrono::milliseconds maxQueryTime = std::chrono::milliseconds(250);
    std::size_t maxDiagnosticBytes = 192;
};

struct OperationAuditCatalogQueryResult {
    OperationAuditCatalogQueryCode code = OperationAuditCatalogQueryCode::InvalidConfiguration;
    std::vector<OperationAuditCatalogEntry> rows;
    std::optional<std::string> cursor;
    std::string generation;
    std::string diagnostic;
    [[nodiscard]] auto ready() const noexcept -> bool {
        return code == OperationAuditCatalogQueryCode::None;
    }
};

[[nodiscard]] auto QueryOperationAuditCatalog(
    const OperationAuditSpec& InSpec,
    const OperationAuditCatalogFilter& InFilter = {},
    std::optional<std::string_view> InCursor = std::nullopt,
    OperationAuditCatalogQueryLimits InLimits = {}) -> OperationAuditCatalogQueryResult;

// This is intentionally a distinct API: a catalog row cannot be upgraded to
// verified merely because it was listed.  The caller supplies the actual,
// trusted selected-run input spec; its source/frozen bytes are checked against
// the row before the existing pinned reader is invoked.
[[nodiscard]] auto RevalidateOperationAuditCatalogEntry(
    const OperationAuditSpec& InCatalogAnchorSpec,
    const OperationAuditSpec& InSelectedSpec,
    const OperationAuditCatalogEntry& InEntry,
    OperationAuditRunReadLimits InLimits = {}) -> OperationAuditRunReadResult;

} // namespace kano::git::commands
