#pragma once

#include "audit_run_reader.hpp"

#include <cstdint>
#include <filesystem>
#include <string>

namespace kano::git::commands {

// ============================================================================
// Audit handoff v1 — bounded, deterministic, pinned-snapshot export.
//
// The handoff is generated from exactly one verified `OperationAuditRunProjection`
// (the canonical pinned reader in `audit_run_reader.hpp`). It carries the
// receipt/attempt/run/correlation identity, repository transitions, retained
// evidence metadata, integrity hashes, and explicit omitted / redacted /
// withheld / truncated counts. It never embeds raw commands, secrets,
// unredacted evidence bodies, arbitrary filesystem paths, unverified catalog
// rows, or content from another attempt or generation.
//
// Fail-closed security boundary:
//   - redacted / withheld evidence is reported by count and metadata only;
//     its body is never represented as available content;
//   - the export never follows symlinks or reparse points at the destination;
//   - the export never overwrites an existing destination;
//   - atomic publication is exclusive create-then-rename with fsync;
//   - the integrity field is a hash of a canonical subset of the document so
//     consumers can verify by re-deriving it; the wire bytes are not their
//     own hash because the hash field is part of those bytes.
//
// Integrity wire contract: `integritySha256` is the SHA-256 of the canonical
// bytes of the document with `integritySha256` set to a 64-character
// all-zeros sentinel. Consumers verify by parsing the JSON, replacing the
// field with the sentinel, re-serializing, hashing, and comparing.
//
// Serialization ordering: the canonical field order is fixed in the builder,
// but the wire JSON is rendered in alphabetical key order because
// nlohmann::json uses an ordered std::map for object storage. The field
// whitelist and the canonical contents are stable regardless of key order.
//
// Repeated export of the exact same stable pinned snapshot produces
// byte-identical output.
// ============================================================================

inline constexpr std::string_view kAuditHandoffSchemaName = "kog.auditHandoff.v1";
inline constexpr std::uint32_t kAuditHandoffSchemaVersion = 1;

struct AuditHandoffLimits {
    // Hard ceiling on serialized bytes. The handoff never exceeds this value;
    // truncation is reported explicitly via the `truncation` block.
    std::uint64_t maxSerializedBytes = 256U << 10U;
    // Per-list ceilings enforced before serialization. These exist so a
    // maliciously-large verified projection cannot consume unbounded output
    // budget. The pinned reader's own limits already cap the projection;
    // these values are an outer envelope.
    std::size_t maxRepositories = 64;
    std::size_t maxEvidenceReferences = 64;
    std::size_t maxEvents = 64;
};

// Operation status returned to the caller. Distinct from the audit
// `OutcomeState` enum so the export contract can evolve independently.
enum class AuditHandoffResultCode {
    None,
    InvalidConfiguration,
    EmptyProjection,
    ForbiddenFieldEncountered,
    ExceedsByteCap,
    Truncated,
    DestinationPathInvalid,
    DestinationExists,
    DestinationSymlinkOrReparse,
    DestinationNotRegular,
    DestinationOutsideAnchor,
    IoError,
};

[[nodiscard]] auto AuditHandoffResultCodeName(AuditHandoffResultCode InCode)
    -> std::string_view;

struct AuditHandoffResult {
    AuditHandoffResultCode code = AuditHandoffResultCode::InvalidConfiguration;
    // Deterministic serialized bytes. Empty when the result code is not None.
    std::string serialized;
    // Final size in bytes, equal to `serialized.size()` when None.
    std::uint64_t sizeBytes = 0;
    // SHA-256 hex of the exact serialized bytes. Empty when not None.
    std::string sha256;
    // True iff the projection was truncated to fit the byte cap or per-list
    // caps. Truncated outputs are still valid; the `truncation` block in the
    // document reports exact retained / omitted counts.
    bool truncated = false;
    // Exact counts that did not fit, surfaced for the caller even when the
    // truncation block in the document is the authoritative source of truth.
    std::uint64_t omittedEvents = 0;
    std::uint64_t omittedRepositories = 0;
    std::uint64_t omittedEvidenceReferences = 0;
    // Exact retained counts after caps.
    std::uint64_t retainedEvents = 0;
    std::uint64_t retainedRepositories = 0;
    std::uint64_t retainedEvidenceReferences = 0;
    // Bounded diagnostic, never exceeds 192 bytes.
    std::string diagnostic;
    // Final published path; populated only by PublishAuditHandoff.
    std::filesystem::path publishedPath;
};

// Build the deterministic handoff document from one verified pinned
// projection. The returned `serialized` bytes are byte-identical for repeated
// calls with the same projection and limits. The integrity hash is computed
// over those exact bytes.
[[nodiscard]] auto BuildAuditHandoffJson(
    const OperationAuditRunProjection& InProjection,
    AuditHandoffLimits InLimits = {}) -> AuditHandoffResult;

// Atomically publish the deterministic handoff to disk. Destination handling:
//   - parent directory must exist and be a non-symlink directory;
//   - destination must not exist (no implicit overwrite);
//   - destination must not be a symlink or reparse point;
//   - publication is exclusive create + fsync + atomic rename, so a partial
//     file is never observable;
//   - on success, `publishedPath` holds the final path and `sha256` /
//     `sizeBytes` describe the bytes that landed on disk.
//
// The destination's parent directory must satisfy the anchor policy of the
// underlying audit evidence directory; arbitrary filesystem browsing is not
// permitted.
[[nodiscard]] auto PublishAuditHandoff(
    const OperationAuditRunProjection& InProjection,
    const std::filesystem::path& InDestination,
    AuditHandoffLimits InLimits = {}) -> AuditHandoffResult;

} // namespace kano::git::commands
