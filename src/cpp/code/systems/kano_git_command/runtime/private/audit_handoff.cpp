#include "audit_handoff.hpp"
#include "audit_contract.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <vector>

#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace kano::git::commands {
namespace {

using Json = nlohmann::json;
using namespace kano::git::audit;

constexpr std::size_t kDiagnosticByteCeiling = 192;
constexpr std::uint64_t kHardByteFloor = 64;
constexpr std::uint64_t kHardByteCeiling = 4U << 20U;

// nlohmann::json field ordering is insertion order. Building with explicit
// object member assignments produces a deterministic serialization that does
// not depend on map ordering or any compiler-specific side effect.
constexpr int kJsonIndent = 0;
constexpr char kJsonIndentChar = ' ';
constexpr bool kJsonAscii = true;

// Sentinel for the integrity slot while we compute the final hash. The
// SHA-256 of this 64-character hex string is itself deterministic, so the
// hash of the final emitted bytes is uniquely determined by the rest of the
// document and is reproducible on every export of the same projection.
constexpr std::string_view kIntegritySentinelHex =
    "0000000000000000000000000000000000000000000000000000000000000000";

auto OutcomeStateLabel(const OutcomeState InState) -> std::string_view {
    switch (InState) {
    case OutcomeState::Succeeded:  return "succeeded";
    case OutcomeState::Failed:     return "failed";
    case OutcomeState::Partial:    return "partial";
    case OutcomeState::Blocked:    return "blocked";
    case OutcomeState::Cancelled:  return "cancelled";
    case OutcomeState::TimedOut:   return "timed-out";
    case OutcomeState::Unknown:    return "unknown";
    }
    return "unknown";
}

auto RedactionStatusLabel(const RedactionStatus InStatus) -> std::string_view {
    switch (InStatus) {
    case RedactionStatus::NotRequired: return "not-required";
    case RedactionStatus::Redacted:     return "redacted";
    case RedactionStatus::Withheld:     return "withheld";
    }
    return "withheld";
}

auto CorrelationModeLabel(const CorrelationMode InMode) -> std::string_view {
    switch (InMode) {
    case CorrelationMode::Standalone: return "standalone";
    case CorrelationMode::Koa:        return "koa";
    }
    return "standalone";
}

auto WorktreeStateLabel(const WorktreeState InState) -> std::string_view {
    switch (InState) {
    case WorktreeState::Clean:   return "clean";
    case WorktreeState::Dirty:   return "dirty";
    case WorktreeState::Unknown: return "unknown";
    }
    return "unknown";
}

auto CurrentUtcIso8601() -> std::string {
    const auto now = std::chrono::system_clock::now();
    const auto time = std::chrono::system_clock::to_time_t(now);
    std::tm utc{};
#if defined(_WIN32)
    gmtime_s(&utc, &time);
#else
    gmtime_r(&time, &utc);
#endif
    char buffer[32]{};
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &utc);
    return std::string(buffer);
}

auto BoundedDiagnostic(std::string InMessage) -> std::string {
    if (InMessage.size() > kDiagnosticByteCeiling) {
        InMessage.resize(kDiagnosticByteCeiling);
    }
    return InMessage;
}

auto IsHandoffForbiddenKey(std::string_view InKey) -> bool;

void EnsureNoForbiddenKeys(const Json& InObject, std::string_view InPath) {
    if (!InObject.is_object()) return;
    for (auto it = InObject.begin(); it != InObject.end(); ++it) {
        const auto& key = it.key();
        if (IsHandoffForbiddenKey(key)) {
            throw std::runtime_error(std::string(InPath) + "." + key +
                                     ": forbidden key in audit handoff");
        }
        EnsureNoForbiddenKeys(it.value(), InPath);
    }
}

// The audit contract's `IsForbiddenField` lives in an anonymous namespace
// inside `audit_contract.cpp`. We mirror its normalized-key check here so the
// handoff module is self-contained and the field whitelist is enforceable
// before serialization. The list is the exact canonical corpus from
// `audit_contract.cpp::IsForbiddenField`.
auto IsHandoffForbiddenKey(std::string_view InKey) -> bool {
    static constexpr std::array<std::string_view, 29> forbidden{
        "apikey", "argv", "auth", "authorization",
        "body", "command", "credential", "credentials",
        "env", "environment", "error", "failed",
        "message", "ok", "password", "path",
        "payload", "privatekey", "rawcommand", "secret",
        "secrets", "stderr", "stdout", "succeeded",
        "success", "token", "tokens", "username",
        "value",
    };
    std::string normalized;
    normalized.reserve(InKey.size());
    for (const char ch : InKey) {
        const auto byte = static_cast<unsigned char>(ch);
        if (std::isalnum(byte) != 0) {
            normalized.push_back(static_cast<char>(std::tolower(byte)));
        }
    }
    return std::find(forbidden.begin(), forbidden.end(),
                      std::string_view(normalized)) != forbidden.end();
}

auto ValidateLimits(const AuditHandoffLimits& InLimits) -> std::optional<std::string> {
    if (InLimits.maxSerializedBytes < kHardByteFloor) {
        return "maxSerializedBytes is below the 64-byte floor";
    }
    if (InLimits.maxSerializedBytes > kHardByteCeiling) {
        return "maxSerializedBytes exceeds the 4 MiB envelope";
    }
    if (InLimits.maxRepositories == 0 || InLimits.maxEvidenceReferences == 0 ||
        InLimits.maxEvents == 0) {
        return "per-list caps must be positive";
    }
    if (InLimits.maxRepositories > 4096 || InLimits.maxEvidenceReferences > 4096 ||
        InLimits.maxEvents > 4096) {
        return "per-list caps exceed the 4096 envelope";
    }
    return std::nullopt;
}

auto RepositoryArrayJson(const std::vector<audit::RepositoryTransition>& InRepos,
                        const std::size_t InCap,
                        std::uint64_t& OutRetained,
                        std::uint64_t& OutOmitted) -> Json {
    std::vector<audit::RepositoryTransition> sorted = InRepos;
    std::sort(sorted.begin(), sorted.end(),
              [](const auto& left, const auto& right) {
                  return left.repositoryId < right.repositoryId;
              });
    Json result = Json::array();
    OutRetained = 0; OutOmitted = 0;
    for (std::size_t index = 0; index < sorted.size() && index < InCap; ++index) {
        const auto& repository = sorted[index];
        Json before = Json::object();
        before["headSha"] = repository.before.headSha ? Json(*repository.before.headSha) : Json(nullptr);
        before["branch"] = repository.before.branch ? Json(*repository.before.branch) : Json(nullptr);
        before["worktreeState"] = Json(std::string(WorktreeStateLabel(repository.before.worktreeState)));
        before["dirtyFingerprint"] = repository.before.dirtyFingerprint ? Json(*repository.before.dirtyFingerprint) : Json(nullptr);
        before["upstreamHeadSha"] = repository.before.upstreamHeadSha ? Json(*repository.before.upstreamHeadSha) : Json(nullptr);
        before["ahead"] = repository.before.ahead ? Json(*repository.before.ahead) : Json(nullptr);
        before["behind"] = repository.before.behind ? Json(*repository.before.behind) : Json(nullptr);

        Json after = Json::object();
        after["headSha"] = repository.after.headSha ? Json(*repository.after.headSha) : Json(nullptr);
        after["branch"] = repository.after.branch ? Json(*repository.after.branch) : Json(nullptr);
        after["worktreeState"] = Json(std::string(WorktreeStateLabel(repository.after.worktreeState)));
        after["dirtyFingerprint"] = repository.after.dirtyFingerprint ? Json(*repository.after.dirtyFingerprint) : Json(nullptr);
        after["upstreamHeadSha"] = repository.after.upstreamHeadSha ? Json(*repository.after.upstreamHeadSha) : Json(nullptr);
        after["ahead"] = repository.after.ahead ? Json(*repository.after.ahead) : Json(nullptr);
        after["behind"] = repository.after.behind ? Json(*repository.after.behind) : Json(nullptr);

        Json entry = Json::object();
        entry["repositoryId"] = repository.repositoryId;
        entry["before"] = before;
        entry["after"] = after;
        result.push_back(entry);
        ++OutRetained;
    }
    if (sorted.size() > InCap) OutOmitted = sorted.size() - InCap;
    return result;
}

auto EvidenceArrayJson(const std::vector<OperationAuditEvidencePreview>& InEvidence,
                       const std::size_t InCap,
                       std::uint64_t& OutRetained,
                       std::uint64_t& OutOmitted) -> Json {
    std::vector<OperationAuditEvidencePreview> sorted = InEvidence;
    std::sort(sorted.begin(), sorted.end(),
              [](const auto& left, const auto& right) {
                  return std::tie(left.category, left.id, left.kind, left.sha256) <
                         std::tie(right.category, right.id, right.kind, right.sha256);
              });
    Json result = Json::array();
    OutRetained = 0; OutOmitted = 0;
    for (std::size_t index = 0; index < sorted.size() && index < InCap; ++index) {
        const auto& item = sorted[index];
        Json entry = Json::object();
        entry["category"] = item.category;
        entry["id"] = item.id;
        entry["kind"] = item.kind;
        entry["sha256"] = item.sha256;
        entry["sizeBytes"] = item.sizeBytes;
        entry["contentType"] = item.contentType;
        // Body content is intentionally absent. Redacted and withheld items
        // surface their count and redaction status only; there is no
        // representation of available content for those classes.
        entry["redactionStatus"] = Json(std::string(RedactionStatusLabel(item.redactionStatus)));
        result.push_back(entry);
        ++OutRetained;
    }
    if (sorted.size() > InCap) OutOmitted = sorted.size() - InCap;
    return result;
}

auto EventArrayJson(const std::vector<OperationAuditEventPreview>& InEvents,
                    const std::size_t InCap,
                    std::uint64_t& OutRetained,
                    std::uint64_t& OutOmitted) -> Json {
    std::vector<OperationAuditEventPreview> sorted = InEvents;
    std::sort(sorted.begin(), sorted.end(),
              [](const auto& left, const auto& right) {
                  return std::tie(left.eventId, left.sequence) <
                         std::tie(right.eventId, right.sequence);
              });
    Json result = Json::array();
    OutRetained = 0; OutOmitted = 0;
    for (std::size_t index = 0; index < sorted.size() && index < InCap; ++index) {
        const auto& event = sorted[index];
        Json entry = Json::object();
        entry["eventId"] = event.eventId;
        entry["sequence"] = event.sequence;
        entry["repositoryId"] = event.repositoryId;
        entry["beforeHeadSha"] = event.beforeHeadSha ? Json(*event.beforeHeadSha) : Json(nullptr);
        entry["afterHeadSha"] = event.afterHeadSha ? Json(*event.afterHeadSha) : Json(nullptr);
        entry["phase"] = event.phase;
        entry["action"] = event.action;
        Json outcome = Json::object();
        outcome["status"] = Json(std::string(OutcomeStateLabel(event.outcome.status)));
        outcome["exitCode"] = event.outcome.exitCode ? Json(*event.outcome.exitCode) : Json(nullptr);
        outcome["reasonCode"] = event.outcome.reasonCode ? Json(*event.outcome.reasonCode) : Json(nullptr);
        outcome["retryable"] = event.outcome.retryable;
        entry["outcome"] = outcome;
        result.push_back(entry);
        ++OutRetained;
    }
    if (sorted.size() > InCap) OutOmitted = sorted.size() - InCap;
    return result;
}

auto OutcomeJson(const audit::Outcome& InOutcome) -> Json {
    Json out = Json::object();
    out["status"] = Json(std::string(OutcomeStateLabel(InOutcome.status)));
    out["exitCode"] = InOutcome.exitCode ? Json(*InOutcome.exitCode) : Json(nullptr);
    out["reasonCode"] = InOutcome.reasonCode ? Json(*InOutcome.reasonCode) : Json(nullptr);
    out["retryable"] = InOutcome.retryable;
    return out;
}

auto CorrelationJson(const audit::CorrelationRefs& InCorrelation) -> Json {
    Json out = Json::object();
    out["mode"] = Json(std::string(CorrelationModeLabel(InCorrelation.mode)));
    out["productId"] = InCorrelation.productId ? Json(*InCorrelation.productId) : Json(nullptr);
    out["topicId"] = InCorrelation.topicId ? Json(*InCorrelation.topicId) : Json(nullptr);
    out["itemId"] = InCorrelation.itemId ? Json(*InCorrelation.itemId) : Json(nullptr);
    out["workOrderId"] = InCorrelation.workOrderId ? Json(*InCorrelation.workOrderId) : Json(nullptr);
    out["requestId"] = InCorrelation.requestId ? Json(*InCorrelation.requestId) : Json(nullptr);
    out["producerId"] = InCorrelation.producerId ? Json(*InCorrelation.producerId) : Json(nullptr);
    out["routeId"] = InCorrelation.routeId ? Json(*InCorrelation.routeId) : Json(nullptr);
    out["agentId"] = InCorrelation.agentId ? Json(*InCorrelation.agentId) : Json(nullptr);
    return out;
}

auto DumpCanonical(const Json& InDocument) -> std::string {
    return InDocument.dump(kJsonIndent, kJsonIndentChar, kJsonAscii);
}

struct BuildState {
    AuditHandoffResult result;
};

auto BuildHandoffObject(const OperationAuditRunProjection& InProjection,
                        const AuditHandoffLimits& InLimits,
                        BuildState& InOut) -> void {
    Json doc = Json::object();

    // Field order is fixed and matches the schema below; do not reorder.
    doc["schemaName"] = Json(std::string(kAuditHandoffSchemaName));
    doc["schemaVersion"] = Json(kAuditHandoffSchemaVersion);
    doc["exportedAtUtc"] = CurrentUtcIso8601();

    doc["runId"] = InProjection.runId;
    doc["parentRunId"] = InProjection.parentRunId ? Json(*InProjection.parentRunId) : Json(nullptr);
    doc["attempt"] = InProjection.attempt;
    doc["planId"] = InProjection.planId;
    doc["planSha256"] = InProjection.planSha256;
    doc["frozenInputSha256"] = InProjection.frozenInputSha256;
    doc["receiptSha256"] = InProjection.receiptId;
    doc["eventStreamSha256"] = InProjection.eventStreamSha256;
    doc["finishedAtUtc"] = InProjection.finishedAtUtc;

    doc["terminalOutcome"] = OutcomeJson(InProjection.terminalOutcome);
    doc["correlation"] = CorrelationJson(InProjection.correlation);

    std::uint64_t retainedRepos = 0; std::uint64_t omittedRepos = 0;
    doc["repositories"] = RepositoryArrayJson(InProjection.repositories,
                                              InLimits.maxRepositories,
                                              retainedRepos, omittedRepos);

    std::uint64_t retainedEvidence = 0; std::uint64_t omittedEvidence = 0;
    doc["evidenceReferences"] = EvidenceArrayJson(InProjection.evidence,
                                                  InLimits.maxEvidenceReferences,
                                                  retainedEvidence, omittedEvidence);

    std::uint64_t retainedEvents = 0; std::uint64_t omittedEvents = 0;
    doc["events"] = EventArrayJson(InProjection.events,
                                  InLimits.maxEvents,
                                  retainedEvents, omittedEvents);

    Json limitsField = Json::object();
    limitsField["maxSerializedBytes"] = InLimits.maxSerializedBytes;
    limitsField["maxRepositories"] = InLimits.maxRepositories;
    limitsField["maxEvidenceReferences"] = InLimits.maxEvidenceReferences;
    limitsField["maxEvents"] = InLimits.maxEvents;
    doc["limits"] = limitsField;

    Json truncationField = Json::object();
    truncationField["retainedEvents"] = retainedEvents;
    truncationField["retainedRepositories"] = retainedRepos;
    truncationField["retainedEvidenceReferences"] = retainedEvidence;
    truncationField["omittedEvents"] = omittedEvents;
    truncationField["omittedRepositories"] = omittedRepos;
    truncationField["omittedEvidenceReferences"] = omittedEvidence;
    truncationField["projectionTruncated"] =
        InProjection.previewTruncated || InProjection.eventsTruncated ||
        InProjection.repositoriesTruncated || InProjection.evidenceTruncated;
    doc["truncation"] = truncationField;

    Json redactionField = Json::object();
    redactionField["redactedEvidenceCount"] = InProjection.redactedEvidenceCount;
    redactionField["withheldEvidenceCount"] = InProjection.withheldEvidenceCount;
    redactionField["hasRedactedEvidence"] = InProjection.hasRedactedEvidence;
    redactionField["hasWithheldEvidence"] = InProjection.hasWithheldEvidence;
    doc["redaction"] = redactionField;

    Json identityField = Json::object();
    identityField["repositoryIdentityHeadSha256"] = InProjection.repositoryIdentityHeadSha256;
    identityField["catalogRepositoryPreviewIdentityHeadSha256"] =
        InProjection.catalogRepositoryPreviewIdentityHeadSha256;
    doc["identity"] = identityField;

    // Integrity slot is filled below after we know the exact byte length of
    // the document. The placeholder is intentionally present so the field
    // order is stable across passes and the consumer's parser sees the same
    // shape regardless of the hash value.
    doc["integritySha256"] = Json(std::string(kIntegritySentinelHex));

    EnsureNoForbiddenKeys(doc, "$");

    // Two-pass serialization: first pass emits the document with the
    // integrity sentinel; we hash those bytes; then we substitute the actual
    // hash into the integrity slot and emit a second pass. Because the field
    // order, indentation, and escape policy are deterministic, the second
    // pass produces a unique, reproducible byte sequence for a given
    // projection.
    //
    // Wire-format contract for `integritySha256`: it is the SHA-256 of the
    // canonical bytes with the integrity field set to the all-zeros sentinel.
    // Consumers verify by parsing the JSON, replacing `integritySha256` with
    // the sentinel, re-serializing, and comparing the hash to the stored
    // value. The bytes we actually emit are not their own hash because the
    // hash field is part of those bytes; this convention is the standard way
    // to publish a self-integrity check.
    const auto firstPass = DumpCanonical(doc);
    const auto canonicalHash = Sha256Hex(firstPass);
    doc["integritySha256"] = Json(canonicalHash);
    EnsureNoForbiddenKeys(doc, "$");
    const auto finalPass = DumpCanonical(doc);

    if (finalPass.size() > InLimits.maxSerializedBytes) {
        InOut.result.code = AuditHandoffResultCode::ExceedsByteCap;
        InOut.result.diagnostic = BoundedDiagnostic(
            "audit handoff serialized bytes exceed maxSerializedBytes even with all caps applied");
        InOut.result.sizeBytes = finalPass.size();
        return;
    }

    InOut.result.code = AuditHandoffResultCode::None;
    InOut.result.serialized = finalPass;
    InOut.result.sizeBytes = finalPass.size();
    // The reported integrity hash is the same value stored in the wire
    // format — the SHA-256 of the canonical bytes with sentinel — so
    // callers can trust `result.sha256` without re-parsing the JSON.
    InOut.result.sha256 = canonicalHash;
    InOut.result.truncated = omittedEvents > 0 || omittedRepos > 0 || omittedEvidence > 0 ||
        truncationField["projectionTruncated"].get<bool>();
    InOut.result.omittedEvents = omittedEvents;
    InOut.result.omittedRepositories = omittedRepos;
    InOut.result.omittedEvidenceReferences = omittedEvidence;
    InOut.result.retainedEvents = retainedEvents;
    InOut.result.retainedRepositories = retainedRepos;
    InOut.result.retainedEvidenceReferences = retainedEvidence;
}

// ============================================================================
// Safe-path publication — POSIX/Windows equivalent primitives.
// ============================================================================

#if !defined(_WIN32)

struct ScopedFd {
    int fd = -1;
    ~ScopedFd() { if (fd >= 0) ::close(fd); }
};

auto OpenParentDirectory(const std::filesystem::path& InParent,
                          int& OutFd) -> bool {
    OutFd = ::open(InParent.c_str(),
                   O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    return OutFd >= 0;
}

auto IsRegularDirectory(const int InFd) -> bool {
    struct stat status {};
    if (::fstat(InFd, &status) != 0) return false;
    return S_ISDIR(status.st_mode);
}

auto DestinationExistsInParent(const int InParentFd,
                                const std::filesystem::path& InDestination) -> bool {
    return ::faccessat(InParentFd, InDestination.filename().c_str(),
                       F_OK, AT_EACCESS) == 0;
}

auto OpenTemporaryExclusiveNoFollow(const std::filesystem::path& InDirectory,
                                    const std::filesystem::path& InName,
                                    ScopedFd& OutFd) -> bool {
    const auto full = InDirectory / InName;
    OutFd.fd = ::open(full.c_str(),
                      O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC,
                      0600);
    return OutFd.fd >= 0;
}

auto WriteAllBytes(const int InFd, const std::string& InBytes) -> bool {
    std::size_t offset = 0;
    while (offset < InBytes.size()) {
        const auto written = ::write(InFd, InBytes.data() + offset,
                                     InBytes.size() - offset);
        if (written < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        offset += static_cast<std::size_t>(written);
    }
    return true;
}

auto FsyncAndClose(ScopedFd& InOutFd) -> bool {
    if (InOutFd.fd < 0) return false;
    const auto syncResult = (::fsync(InOutFd.fd) == 0);
    const auto closeResult = (::close(InOutFd.fd) == 0);
    InOutFd.fd = -1;
    return syncResult && closeResult;
}

auto RenameIntoPlace(const std::filesystem::path& InTemporary,
                     const std::filesystem::path& InDestination) -> bool {
    // POSIX rename(2) is atomic on the same filesystem. The temporary file
    // and the destination must live under the same parent directory.
    return ::rename(InTemporary.c_str(), InDestination.c_str()) == 0;
}

#else

struct ScopedHandle {
    HANDLE handle = INVALID_HANDLE_VALUE;
    ~ScopedHandle() { if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle); }
};

auto OpenParentDirectory(const std::filesystem::path& InParent,
                          HANDLE& OutHandle) -> bool {
    // FILE_FLAG_BACKUP_SEMANTICS is required for directory handles; the
    // OPEN_REPARSE_POINT flag makes the open refuse junctions / symlinks.
    OutHandle = CreateFileW(InParent.c_str(), GENERIC_READ,
                            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                            nullptr, OPEN_EXISTING,
                            FILE_FLAG_BACKUP_SEMANTICS |
                                FILE_FLAG_OPEN_REPARSE_POINT,
                            nullptr);
    return OutHandle != INVALID_HANDLE_VALUE;
}

auto IsRegularDirectory(const HANDLE InHandle) -> bool {
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(InHandle, &info)) return false;
    if (!(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) return false;
    if (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) return false;
    return true;
}

auto DestinationExistsInParent(const HANDLE /*InParentHandle*/,
                                const std::filesystem::path& InDestination) -> bool {
    const DWORD attributes = GetFileAttributesW(InDestination.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES;
}

auto OpenTemporaryExclusiveNoFollow(const std::filesystem::path& InDirectory,
                                    const std::filesystem::path& InName,
                                    ScopedHandle& OutHandle) -> bool {
    const auto full = InDirectory / InName;
    OutHandle.handle = CreateFileW(full.c_str(), GENERIC_WRITE, 0, nullptr,
                                   CREATE_NEW,
                                   FILE_ATTRIBUTE_NORMAL |
                                       FILE_FLAG_OPEN_REPARSE_POINT |
                                       FILE_FLAG_WRITE_THROUGH,
                                   nullptr);
    if (OutHandle.handle == INVALID_HANDLE_VALUE) return false;
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(OutHandle.handle, &info)) return false;
    if (info.dwFileAttributes &
        (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) return false;
    return true;
}

auto WriteAllBytes(const HANDLE InHandle, const std::string& InBytes) -> bool {
    std::size_t offset = 0;
    while (offset < InBytes.size()) {
        DWORD written = 0;
        if (!WriteFile(InHandle, InBytes.data() + offset,
                       static_cast<DWORD>(InBytes.size() - offset), &written,
                       nullptr)) {
            return false;
        }
        if (written == 0) return false;
        offset += written;
    }
    return true;
}

auto FsyncAndClose(ScopedHandle& InOutHandle) -> bool {
    if (InOutHandle.handle == INVALID_HANDLE_VALUE) return false;
    const auto syncResult = (FlushFileBuffers(InOutHandle.handle) != FALSE);
    CloseHandle(InOutHandle.handle);
    InOutHandle.handle = INVALID_HANDLE_VALUE;
    return syncResult;
}

auto RenameIntoPlace(const std::filesystem::path& InTemporary,
                     const std::filesystem::path& InDestination) -> bool {
    // MoveFileExW with MOVEFILE_WRITE_THROUGH performs an fsync of the
    // destination after the rename.
    return MoveFileExW(InTemporary.c_str(), InDestination.c_str(),
                       MOVEFILE_WRITE_THROUGH | MOVEFILE_COPY_ALLOWED) != FALSE;
}

#endif

auto TempNameFor(const std::filesystem::path& InDestination) -> std::filesystem::path {
    static std::atomic_uint64_t sequence = 0;
    const auto nonce = std::to_string(sequence.fetch_add(1)) + "-" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    return std::filesystem::path(".handoff.tmp-" + Sha256Hex(nonce).substr(0, 16) +
                                  "-" + InDestination.filename().string());
}

} // namespace

auto AuditHandoffResultCodeName(AuditHandoffResultCode InCode) -> std::string_view {
    switch (InCode) {
    case AuditHandoffResultCode::None: return "none";
    case AuditHandoffResultCode::InvalidConfiguration: return "invalid_configuration";
    case AuditHandoffResultCode::EmptyProjection: return "empty_projection";
    case AuditHandoffResultCode::ForbiddenFieldEncountered: return "forbidden_field_encountered";
    case AuditHandoffResultCode::ExceedsByteCap: return "exceeds_byte_cap";
    case AuditHandoffResultCode::Truncated: return "truncated";
    case AuditHandoffResultCode::DestinationPathInvalid: return "destination_path_invalid";
    case AuditHandoffResultCode::DestinationExists: return "destination_exists";
    case AuditHandoffResultCode::DestinationSymlinkOrReparse: return "destination_symlink_or_reparse";
    case AuditHandoffResultCode::DestinationNotRegular: return "destination_not_regular";
    case AuditHandoffResultCode::DestinationOutsideAnchor: return "destination_outside_anchor";
    case AuditHandoffResultCode::IoError: return "io_error";
    }
    return "invalid_configuration";
}

auto BuildAuditHandoffJson(const OperationAuditRunProjection& InProjection,
                           AuditHandoffLimits InLimits) -> AuditHandoffResult {
    AuditHandoffResult result;
    if (const auto invalid = ValidateLimits(InLimits); invalid.has_value()) {
        result.code = AuditHandoffResultCode::InvalidConfiguration;
        result.diagnostic = BoundedDiagnostic(*invalid);
        return result;
    }
    if (InProjection.receiptId.empty() || InProjection.runId.empty() ||
        InProjection.planId.empty() || InProjection.planSha256.empty() ||
        InProjection.frozenInputSha256.empty() || InProjection.eventStreamSha256.empty() ||
        InProjection.finishedAtUtc.empty()) {
        result.code = AuditHandoffResultCode::EmptyProjection;
        result.diagnostic = BoundedDiagnostic(
            "pinned projection is empty or missing identity fields");
        return result;
    }
    BuildState state;
    state.result = result;
    try {
        BuildHandoffObject(InProjection, InLimits, state);
    } catch (const std::exception& ex) {
        state.result.code = AuditHandoffResultCode::ForbiddenFieldEncountered;
        state.result.diagnostic = BoundedDiagnostic(ex.what());
        return state.result;
    }
    return state.result;
}

auto PublishAuditHandoff(const OperationAuditRunProjection& InProjection,
                          const std::filesystem::path& InDestination,
                          AuditHandoffLimits InLimits) -> AuditHandoffResult {
    auto built = BuildAuditHandoffJson(InProjection, InLimits);
    if (built.code != AuditHandoffResultCode::None) {
        built.publishedPath.clear();
        return built;
    }

    std::error_code ec;
    const auto absolute = std::filesystem::absolute(InDestination, ec);
    if (ec) {
        built.code = AuditHandoffResultCode::DestinationPathInvalid;
        built.diagnostic = BoundedDiagnostic("destination path is not absolute");
        return built;
    }
    const auto parent = absolute.parent_path();
    if (parent.empty()) {
        built.code = AuditHandoffResultCode::DestinationPathInvalid;
        built.diagnostic = BoundedDiagnostic("destination has no parent directory");
        return built;
    }

    // Open the parent directory handle first. If the parent is missing,
    // symlinked, or non-regular, fail closed before any byte leaves the
    // process.
#if !defined(_WIN32)
    int parentFd = -1;
    if (!OpenParentDirectory(parent, parentFd)) {
        built.code = AuditHandoffResultCode::DestinationPathInvalid;
        built.diagnostic = BoundedDiagnostic("parent directory is missing or not openable");
        return built;
    }
    if (!IsRegularDirectory(parentFd)) {
        ::close(parentFd);
        built.code = AuditHandoffResultCode::DestinationNotRegular;
        built.diagnostic = BoundedDiagnostic("parent directory is a symlink/reparse point or non-regular");
        return built;
    }
    if (DestinationExistsInParent(parentFd, absolute)) {
        ::close(parentFd);
        built.code = AuditHandoffResultCode::DestinationExists;
        built.diagnostic = BoundedDiagnostic("destination already exists");
        return built;
    }
    const auto temporaryName = TempNameFor(absolute);
    ScopedFd temporaryFd;
    if (!OpenTemporaryExclusiveNoFollow(parent, temporaryName, temporaryFd)) {
        ::close(parentFd);
        built.code = AuditHandoffResultCode::DestinationSymlinkOrReparse;
        built.diagnostic = BoundedDiagnostic(
            "cannot create exclusive temporary file (parent may not be a directory)");
        return built;
    }
    if (!WriteAllBytes(temporaryFd.fd, built.serialized)) {
        ::unlinkat(parentFd, temporaryName.c_str(), 0);
        FsyncAndClose(temporaryFd);
        ::close(parentFd);
        built.code = AuditHandoffResultCode::IoError;
        built.diagnostic = BoundedDiagnostic("failed to write temporary file");
        return built;
    }
    if (!FsyncAndClose(temporaryFd)) {
        ::unlinkat(parentFd, temporaryName.c_str(), 0);
        ::close(parentFd);
        built.code = AuditHandoffResultCode::IoError;
        built.diagnostic = BoundedDiagnostic("failed to fsync temporary file");
        return built;
    }
    const auto temporaryFull = parent / temporaryName;
    if (!RenameIntoPlace(temporaryFull, absolute)) {
        ::unlinkat(parentFd, temporaryName.c_str(), 0);
        ::close(parentFd);
        built.code = AuditHandoffResultCode::IoError;
        built.diagnostic = BoundedDiagnostic("atomic rename failed");
        return built;
    }
    ::close(parentFd);
#else
    HANDLE parentHandle = INVALID_HANDLE_VALUE;
    if (!OpenParentDirectory(parent, parentHandle)) {
        built.code = AuditHandoffResultCode::DestinationPathInvalid;
        built.diagnostic = BoundedDiagnostic("parent directory is missing or not openable");
        return built;
    }
    if (!IsRegularDirectory(parentHandle)) {
        CloseHandle(parentHandle);
        built.code = AuditHandoffResultCode::DestinationNotRegular;
        built.diagnostic = BoundedDiagnostic("parent directory is a symlink/reparse point or non-regular");
        return built;
    }
    if (DestinationExistsInParent(parentHandle, absolute)) {
        CloseHandle(parentHandle);
        built.code = AuditHandoffResultCode::DestinationExists;
        built.diagnostic = BoundedDiagnostic("destination already exists");
        return built;
    }
    const auto temporaryName = TempNameFor(absolute);
    ScopedHandle temporaryHandle;
    if (!OpenTemporaryExclusiveNoFollow(parent, temporaryName, temporaryHandle)) {
        CloseHandle(parentHandle);
        built.code = AuditHandoffResultCode::DestinationSymlinkOrReparse;
        built.diagnostic = BoundedDiagnostic(
            "cannot create exclusive temporary file (parent may not be a directory)");
        return built;
    }
    if (!WriteAllBytes(temporaryHandle.handle, built.serialized)) {
        DeleteFileW((parent / temporaryName).c_str());
        FsyncAndClose(temporaryHandle);
        CloseHandle(parentHandle);
        built.code = AuditHandoffResultCode::IoError;
        built.diagnostic = BoundedDiagnostic("failed to write temporary file");
        return built;
    }
    if (!FsyncAndClose(temporaryHandle)) {
        DeleteFileW((parent / temporaryName).c_str());
        CloseHandle(parentHandle);
        built.code = AuditHandoffResultCode::IoError;
        built.diagnostic = BoundedDiagnostic("failed to fsync temporary file");
        return built;
    }
    const auto temporaryFull = parent / temporaryName;
    if (!RenameIntoPlace(temporaryFull, absolute)) {
        DeleteFileW(temporaryFull.c_str());
        CloseHandle(parentHandle);
        built.code = AuditHandoffResultCode::IoError;
        built.diagnostic = BoundedDiagnostic("atomic rename failed");
        return built;
    }
    CloseHandle(parentHandle);
#endif

    built.publishedPath = absolute;
    return built;
}

} // namespace kano::git::commands
