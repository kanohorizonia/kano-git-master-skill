// test_audit_handoff_publish.cpp
// Focused tests for the safe-path publication of a bounded audit handoff
// (KOG-TSK-0137). Verifies exclusive atomic publication, no-follow, no
// implicit overwrite, adversarial destination paths, and re-hash of the
// bytes that actually landed on disk.

#include <catch2/catch_test_macros.hpp>

#include "audit_contract.hpp"
#include "audit_handoff.hpp"

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <future>
#include <random>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

using namespace kano::git::commands;
using namespace kano::git::audit;

namespace fs = std::filesystem;

namespace {

auto UniqueRoot() -> fs::path {
    static std::atomic_uint64_t sequence = 0;
    const auto nonce = std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
        std::to_string(sequence.fetch_add(1));
    // Use a workspace-relative temp under the build's .tmp directory so
    // sandboxed CI environments can still write the publish test fixtures.
    // Tests in normal Windows hosts fall back to the system temp directory
    // when this override is unavailable.
    const auto fallback = fs::temp_directory_path() /
        ("kog-audit-handoff-" + Sha256Hex(nonce).substr(0, 20));
    const auto overrideRoot = fs::path("Z:/out/obj/win-ninja-msvc/kog-test-tmp");
    if (!fs::exists(overrideRoot)) {
        std::error_code ec;
        if (fs::create_directories(overrideRoot, ec)) {
            return overrideRoot /
                ("kog-audit-handoff-" + Sha256Hex(nonce).substr(0, 20));
        }
    } else {
        return overrideRoot /
            ("kog-audit-handoff-" + Sha256Hex(nonce).substr(0, 20));
    }
    return fallback;
}

auto WriteText(const fs::path& path, const std::string& bytes) -> void {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    REQUIRE(output.good());
    output << bytes;
    REQUIRE(output.good());
}

auto ReadText(const fs::path& path) -> std::string {
    std::ifstream input(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input), {});
}

auto MakeProjection() -> OperationAuditRunProjection {
    OperationAuditRunProjection projection;
    projection.runId = "publish-run";
    projection.attempt = 1;
    projection.planId = "publish-plan";
    projection.planSha256 = std::string(64, 'a');
    projection.frozenInputSha256 = std::string(64, 'b');
    projection.eventStreamSha256 = std::string(64, 'c');
    projection.receiptId = std::string(64, 'd');
    projection.finishedAtUtc = "2026-08-10T00:38:15Z";
    projection.repositoryIdentityHeadSha256 = std::string(64, 'e');
    projection.catalogRepositoryPreviewIdentityHeadSha256 = std::string(64, 'f');
    projection.correlation.mode = CorrelationMode::Standalone;
    projection.terminalOutcome.status = OutcomeState::Succeeded;
    projection.terminalOutcome.exitCode = 0;

    RepositoryTransition transition;
    transition.repositoryId = "primary";
    transition.before.headSha = std::string(64, '0');
    transition.before.worktreeState = WorktreeState::Clean;
    transition.before.dirtyFingerprint = std::string(64, '1');
    transition.after.headSha = std::string(64, '2');
    transition.after.worktreeState = WorktreeState::Clean;
    transition.after.dirtyFingerprint = std::string(64, '3');
    projection.repositories.push_back(transition);

    OperationAuditEvidencePreview evidence;
    evidence.category = "policy"; evidence.id = "policy-1";
    evidence.kind = "kind-policy"; evidence.sha256 = std::string(64, '4');
    evidence.sizeBytes = 1024; evidence.contentType = "text/plain";
    projection.evidence.push_back(evidence);

    OperationAuditEventPreview event;
    event.eventId = "event-1"; event.sequence = 1;
    event.repositoryId = "primary"; event.phase = "phase";
    event.action = "action"; event.outcome.status = OutcomeState::Succeeded;
    projection.events.push_back(event);

    projection.totalEventRecords = 1;
    projection.totalRepositories = 1;
    projection.totalEvidenceReferences = 1;
    projection.retainedEventRecords = 1;
    projection.retainedRepositories = 1;
    projection.retainedEvidenceReferences = 1;
    return projection;
}

#if !defined(_WIN32)
auto CreateSymlink(const fs::path& link, const fs::path& target) -> bool {
    std::error_code ec;
    fs::create_symlink(target, link, ec);
    return !ec;
}
#else
auto CreateJunction(const fs::path& junction, const fs::path& target) -> bool {
    if (!CreateDirectoryW(junction.c_str(), nullptr)) return false;
    const auto targetAbsolute = fs::absolute(target).wstring();
    const auto substitute = std::wstring(L"\\??\\") + targetAbsolute;
    const auto printName = targetAbsolute;

    struct MountPointData {
        DWORD reparseTag;
        WORD reparseDataLength;
        WORD reserved;
        WORD substituteNameOffset;
        WORD substituteNameLength;
        WORD printNameOffset;
        WORD printNameLength;
    };
    const std::size_t pathBytes =
        (substitute.size() + 1U + printName.size() + 1U) * sizeof(wchar_t);
    const std::size_t dataLength = sizeof(MountPointData) - sizeof(DWORD) +
        pathBytes;
    std::vector<char> buffer(sizeof(MountPointData) + pathBytes);
    auto* data = reinterpret_cast<MountPointData*>(buffer.data());
    data->reparseTag = IO_REPARSE_TAG_MOUNT_POINT;
    data->reparseDataLength = static_cast<WORD>(dataLength);
    data->substituteNameOffset = 0;
    data->substituteNameLength = static_cast<WORD>(
        substitute.size() * sizeof(wchar_t));
    data->printNameOffset = static_cast<WORD>(
        (substitute.size() + 1U) * sizeof(wchar_t));
    data->printNameLength = static_cast<WORD>(printName.size() * sizeof(wchar_t));
    std::memcpy(buffer.data() + sizeof(MountPointData),
                substitute.c_str(), (substitute.size() + 1U) * sizeof(wchar_t));
    std::memcpy(buffer.data() + sizeof(MountPointData) +
                    data->printNameOffset,
                printName.c_str(), (printName.size() + 1U) * sizeof(wchar_t));

    const HANDLE handle = CreateFileW(junction.c_str(), GENERIC_WRITE, 0,
                                      nullptr, OPEN_EXISTING,
                                      FILE_FLAG_OPEN_REPARSE_POINT |
                                          FILE_FLAG_BACKUP_SEMANTICS,
                                      nullptr);
    if (handle == INVALID_HANDLE_VALUE) return false;
    DWORD returned = 0;
    const auto ok = DeviceIoControl(handle, FSCTL_SET_REPARSE_POINT,
                                    buffer.data(),
                                    static_cast<DWORD>(buffer.size()),
                                    nullptr, 0, &returned, nullptr) != FALSE;
    CloseHandle(handle);
    return ok;
}
#endif

} // namespace

TEST_CASE("AuditHandoff publish creates a fresh file with verified hash",
          "[audit][handoff][kog-tsk-0137][publish][unit]") {
    const auto root = UniqueRoot();
    REQUIRE(fs::create_directories(root));
    const auto destination = root / "handoff.json";

    const auto projection = MakeProjection();
    const auto result = PublishAuditHandoff(projection, destination);
    REQUIRE(result.code == AuditHandoffResultCode::None);
    REQUIRE_FALSE(result.publishedPath.empty());
    REQUIRE(result.publishedPath == fs::absolute(destination));

    REQUIRE(fs::exists(destination));
    const auto bytes = ReadText(destination);
    REQUIRE(bytes == result.serialized);
    REQUIRE(result.sizeBytes == bytes.size());

    // Verify the canonical-bytes hash contract: parse the on-disk JSON,
    // replace integritySha256 with the all-zeros sentinel, re-serialize,
    // hash, and compare to the published integrity hash.
    auto parsed = nlohmann::json::parse(bytes);
    auto verify = parsed;
    verify["integritySha256"] = std::string(64, '0');
    REQUIRE(Sha256Hex(verify.dump(0, ' ', true)) == result.sha256);
    REQUIRE(parsed["integritySha256"] == result.sha256);

    fs::remove_all(root);
}

TEST_CASE("AuditHandoff publish refuses existing destination",
          "[audit][handoff][kog-tsk-0137][publish][unit][overwrite]") {
    const auto root = UniqueRoot();
    REQUIRE(fs::create_directories(root));
    const auto destination = root / "handoff.json";
    WriteText(destination, "pre-existing content\n");

    const auto projection = MakeProjection();
    const auto result = PublishAuditHandoff(projection, destination);
    REQUIRE(result.code == AuditHandoffResultCode::DestinationExists);
    // `serialized` may carry the build output for diagnostics; the
    // meaningful refusal signal is `code == DestinationExists` and the
    // `publishedPath` remaining empty.
    REQUIRE(result.publishedPath.empty());

    // Pre-existing bytes must remain unchanged.
    REQUIRE(ReadText(destination) == "pre-existing content\n");

    fs::remove_all(root);
}

TEST_CASE("AuditHandoff publish refuses missing parent directory",
          "[audit][handoff][kog-tsk-0137][publish][unit][adversarial]") {
    const auto root = UniqueRoot();
    const auto missingParent = root / "no-such-dir";
    const auto destination = missingParent / "handoff.json";

    const auto projection = MakeProjection();
    const auto result = PublishAuditHandoff(projection, destination);
    REQUIRE(result.code == AuditHandoffResultCode::DestinationPathInvalid);
    REQUIRE_FALSE(fs::exists(destination));
}

#if !defined(_WIN32)
TEST_CASE("AuditHandoff publish refuses symlink destination",
          "[audit][handoff][kog-tsk-0137][publish][unit][symlink]") {
    const auto root = UniqueRoot();
    REQUIRE(fs::create_directories(root));
    // Create a real file that the symlink will resolve to.
    const auto target = root / "target.json";
    WriteText(target, "real target content\n");
    // Create a symlink at the destination location.
    const auto destination = root / "handoff.json";
    REQUIRE(CreateSymlink(destination, target));

    const auto projection = MakeProjection();
    const auto result = PublishAuditHandoff(projection, destination);
    // The destination already exists (via the symlink) and points to a
    // regular file, so the safety boundary rejects the publication as
    // "destination exists" or refuses the symlink with a reparse refusal.
    REQUIRE((result.code == AuditHandoffResultCode::DestinationExists ||
             result.code == AuditHandoffResultCode::DestinationSymlinkOrReparse));

    // The symlink must not be replaced.
    REQUIRE(fs::exists(destination));
    REQUIRE(fs::is_symlink(destination));
    REQUIRE(ReadText(destination) == "real target content\n");

    fs::remove_all(root);
}
#else
TEST_CASE("AuditHandoff publish refuses reparse-point destination",
          "[audit][handoff][kog-tsk-0137][publish][unit][symlink]") {
    const auto root = UniqueRoot();
    REQUIRE(fs::create_directories(root));
    const auto target = root / "real-target";
    REQUIRE(fs::create_directories(target));
    const auto junction = root / "handoff.json";
    // Windows directory-junction creation requires the OS to permit
    // reparse-point creation (admin, developer mode, or
    // SeCreateSymbolicLinkPrivilege). On hosts where the policy refuses,
    // the test must skip rather than fail — the safety property under
    // test is symmetric and a real junction is required only as the
    // setup fixture.
    if (!CreateJunction(junction, target)) {
        WARN("junction creation refused by OS policy; skipping reparse refusal test");
        fs::remove_all(root);
        return;
    }

    const auto projection = MakeProjection();
    const auto result = PublishAuditHandoff(projection, junction);
    REQUIRE((result.code == AuditHandoffResultCode::DestinationExists ||
             result.code == AuditHandoffResultCode::DestinationSymlinkOrReparse));

    // Junction must not be replaced.
    REQUIRE(fs::exists(junction));
    const auto junctionBytes = ReadText(junction);
    REQUIRE_FALSE(junctionBytes.empty());
    if (junctionBytes.empty()) {
        REQUIRE(fs::is_directory(junction));
    }

    fs::remove_all(root);
}
#endif

TEST_CASE("AuditHandoff publish leaves no partial file on failure",
          "[audit][handoff][kog-tsk-0137][publish][unit][atomic]") {
    // A pre-existing destination must remain on disk after a refused
    // publication; no stray partial file must leak into the parent.
    const auto root = UniqueRoot();
    REQUIRE(fs::create_directories(root));
    const auto destination = root / "handoff.json";
    WriteText(destination, "untouched\n");

    const auto projection = MakeProjection();
    const auto result = PublishAuditHandoff(projection, destination);
    REQUIRE(result.code == AuditHandoffResultCode::DestinationExists);

    // Scan the parent for stray .handoff.tmp-* files. The implementation
    // removes its temporary before returning the failure code.
    for (const auto& entry : fs::directory_iterator(root)) {
        const auto leafName = entry.path().filename().string();
        REQUIRE_FALSE(leafName.starts_with(".handoff.tmp-"));
    }

    fs::remove_all(root);
}

TEST_CASE("AuditHandoff publish concurrent exports do not collide",
          "[audit][handoff][kog-tsk-0137][publish][unit][concurrency]") {
    const auto root = UniqueRoot();
    REQUIRE(fs::create_directories(root));
    const auto projection = MakeProjection();

    constexpr std::size_t kThreads = 4;
    std::vector<std::future<void>> workers;
    workers.reserve(kThreads);
    std::atomic<int> successCount{0};
    for (std::size_t index = 0; index < kThreads; ++index) {
        workers.push_back(std::async(std::launch::async, [&, index]() {
            const auto destination = root /
                ("handoff-" + std::to_string(index) + ".json");
            const auto result = PublishAuditHandoff(projection, destination);
            if (result.code == AuditHandoffResultCode::None) {
                successCount.fetch_add(1);
            }
        }));
    }
    for (auto& worker : workers) worker.get();
    REQUIRE(successCount.load() == static_cast<int>(kThreads));

    // Every published file must round-trip byte-for-byte. The integrity
    // hash is reported consistently across all concurrent exports because
    // it is derived from the canonical bytes of the document, not from
    // the export thread or wall-clock time.
    const auto baseline = BuildAuditHandoffJson(projection);
    for (std::size_t index = 0; index < kThreads; ++index) {
        const auto destination = root /
            ("handoff-" + std::to_string(index) + ".json");
        REQUIRE(fs::exists(destination));
        const auto bytes = ReadText(destination);
        REQUIRE(bytes == baseline.serialized);
    }

    fs::remove_all(root);
}
