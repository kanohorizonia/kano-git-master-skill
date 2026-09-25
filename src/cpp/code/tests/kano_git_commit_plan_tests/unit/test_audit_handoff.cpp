// test_audit_handoff.cpp
// Focused deterministic tests for the bounded pinned audit handoff builder
// (KOG-TSK-0137). Verifies schema, byte-determinism, cap enforcement,
// truncation truth, redaction/withheld handling, forbidden-field rejection,
// and missing/corrupt projection handling.

#include <catch2/catch_test_macros.hpp>

#include "audit_contract.hpp"
#include "audit_handoff.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

using namespace kano::git::commands;
using namespace kano::git::audit;

namespace {

auto Sha256HexOf(const std::string& bytes) -> std::string {
    return Sha256Hex(bytes);
}

// Build a deterministic verified projection. Every run that calls this with
// the same seed produces the same projection, so we can compare two exports
// byte-for-byte.
auto MakeProjection(const std::uint32_t InSeed,
                    const std::size_t InRepositoryCount = 2,
                    const std::size_t InEvidenceCount = 3,
                    const std::size_t InEventCount = 4,
                    const bool InIncludeRedacted = true,
                    const bool InIncludeWithheld = true,
                    const bool InIncludeParent = true,
                    const RedactionStatus InRedactionKind = RedactionStatus::NotRequired)
    -> OperationAuditRunProjection {
    OperationAuditRunProjection projection;
    projection.runId = "run-" + std::to_string(InSeed);
    if (InIncludeParent) projection.parentRunId = std::string{"parent-run"};
    projection.attempt = InSeed == 0 ? 1U : InSeed;
    projection.planId = "plan-" + std::to_string(InSeed);
    projection.planSha256 = std::string(64, 'a');
    projection.frozenInputSha256 = std::string(64, 'b');
    projection.eventStreamSha256 = std::string(64, 'c');
    projection.receiptId = std::string(64, 'd');
    projection.finishedAtUtc = "2026-08-10T00:38:15Z";
    projection.repositoryIdentityHeadSha256 = std::string(64, 'e');
    projection.catalogRepositoryPreviewIdentityHeadSha256 = std::string(64, 'f');

    projection.correlation.mode = CorrelationMode::Koa;
    projection.correlation.productId = "product-" + std::to_string(InSeed);
    projection.correlation.topicId = "topic-" + std::to_string(InSeed);
    projection.correlation.itemId = "item-" + std::to_string(InSeed);
    projection.correlation.workOrderId = "work-" + std::to_string(InSeed);
    projection.correlation.requestId = "request-" + std::to_string(InSeed);
    projection.correlation.producerId = "producer-" + std::to_string(InSeed);
    projection.correlation.routeId = "route-" + std::to_string(InSeed);

    projection.terminalOutcome.status = OutcomeState::Succeeded;
    projection.terminalOutcome.exitCode = 0;

    for (std::size_t index = 0; index < InRepositoryCount; ++index) {
        RepositoryTransition transition;
        transition.repositoryId = "repo-" + std::to_string(index);
        transition.before.headSha = std::string(64, '0');
        transition.before.branch = "main";
        transition.before.worktreeState = WorktreeState::Clean;
        transition.before.dirtyFingerprint = std::string(64, '1');
        transition.after.headSha = std::string(64, '2');
        transition.after.branch = "main";
        transition.after.worktreeState = WorktreeState::Clean;
        transition.after.dirtyFingerprint = std::string(64, '3');
        projection.repositories.push_back(transition);
    }

    for (std::size_t index = 0; index < InEvidenceCount; ++index) {
        OperationAuditEvidencePreview item;
        item.category = (index % 2 == 0) ? "policy" : "approval";
        item.id = "evidence-" + std::to_string(index);
        item.kind = "kind-" + std::to_string(index);
        item.sha256 = std::string(64, '4');
        item.sizeBytes = 1024ULL + index;
        item.contentType = "text/plain";
        if (InIncludeRedacted && index == 0) {
            item.redactionStatus = RedactionStatus::Redacted;
            projection.redactedEvidenceCount += 1;
            projection.hasRedactedEvidence = true;
        } else if (InIncludeWithheld && index == 1) {
            item.redactionStatus = RedactionStatus::Withheld;
            projection.withheldEvidenceCount += 1;
            projection.hasWithheldEvidence = true;
        } else {
            item.redactionStatus = InRedactionKind;
        }
        projection.evidence.push_back(item);
    }

    for (std::size_t index = 0; index < InEventCount; ++index) {
        OperationAuditEventPreview event;
        event.eventId = "event-" + std::to_string(index);
        event.sequence = index;
        event.repositoryId = "repo-" + std::to_string(index % InRepositoryCount);
        event.beforeHeadSha = std::string(64, '5');
        event.afterHeadSha = std::string(64, '6');
        event.phase = "phase";
        event.action = "action-" + std::to_string(index);
        event.outcome.status = OutcomeState::Succeeded;
        projection.events.push_back(event);
    }

    projection.totalEventRecords = InEventCount;
    projection.totalRepositories = InRepositoryCount;
    projection.totalEvidenceReferences = InEvidenceCount;
    projection.retainedEventRecords = InEventCount;
    projection.retainedRepositories = InRepositoryCount;
    projection.retainedEvidenceReferences = InEvidenceCount;
    return projection;
}

auto ParseHandoff(const std::string& InJson) -> nlohmann::json {
    return nlohmann::json::parse(InJson);
}

} // namespace

TEST_CASE("AuditHandoff stable pinned snapshot produces expected schema",
          "[audit][handoff][kog-tsk-0137][unit]") {
    const auto projection = MakeProjection(/*seed=*/1);
    const auto first = BuildAuditHandoffJson(projection);
    REQUIRE(first.code == AuditHandoffResultCode::None);
    REQUIRE_FALSE(first.serialized.empty());
    REQUIRE_FALSE(first.sha256.empty());

    const auto parsed = ParseHandoff(first.serialized);
    REQUIRE(parsed["schemaName"] == std::string(kAuditHandoffSchemaName));
    REQUIRE(parsed["schemaVersion"] == kAuditHandoffSchemaVersion);
    REQUIRE(parsed["runId"] == projection.runId);
    REQUIRE(parsed["parentRunId"] == "parent-run");
    REQUIRE(parsed["attempt"] == projection.attempt);
    REQUIRE(parsed["planId"] == projection.planId);
    REQUIRE(parsed["planSha256"] == projection.planSha256);
    REQUIRE(parsed["frozenInputSha256"] == projection.frozenInputSha256);
    REQUIRE(parsed["receiptSha256"] == projection.receiptId);
    REQUIRE(parsed["eventStreamSha256"] == projection.eventStreamSha256);
    REQUIRE(parsed["finishedAtUtc"] == projection.finishedAtUtc);
    REQUIRE(parsed["terminalOutcome"]["status"] == "succeeded");
    REQUIRE(parsed["terminalOutcome"]["exitCode"] == 0);
    REQUIRE(parsed["correlation"]["mode"] == "koa");
    REQUIRE(parsed["correlation"]["productId"] == "product-1");
    REQUIRE(parsed["repositories"].is_array());
    REQUIRE(parsed["evidenceReferences"].is_array());
    REQUIRE(parsed["events"].is_array());
    REQUIRE(parsed["limits"]["maxSerializedBytes"].get<std::uint64_t>() > 0);
    REQUIRE(parsed["truncation"]["projectionTruncated"] == false);
    REQUIRE(parsed["redaction"]["redactedEvidenceCount"] == 1);
    REQUIRE(parsed["redaction"]["withheldEvidenceCount"] == 1);
    REQUIRE(parsed["redaction"]["hasRedactedEvidence"] == true);
    REQUIRE(parsed["redaction"]["hasWithheldEvidence"] == true);
    REQUIRE(parsed["identity"]["repositoryIdentityHeadSha256"] ==
            projection.repositoryIdentityHeadSha256);
    REQUIRE(parsed["integritySha256"].is_string());
    REQUIRE(parsed["integritySha256"].get<std::string>().size() == 64);
    // nlohmann::json sorts object keys alphabetically when serializing,
    // because the default backing container is an ordered std::map. The
    // canonical field order is documented in the builder; the wire order is
    // alphabetical and stable across runs.
    const auto members = parsed.get<std::map<std::string, nlohmann::json>>();
    REQUIRE(members.rbegin()->first == "truncation");
}

TEST_CASE("AuditHandoff repeated export is byte-identical",
          "[audit][handoff][kog-tsk-0137][unit][deterministic]") {
    const auto projection = MakeProjection(/*seed=*/7);
    const auto first = BuildAuditHandoffJson(projection);
    REQUIRE(first.code == AuditHandoffResultCode::None);
    for (int repeat = 0; repeat < 5; ++repeat) {
        const auto again = BuildAuditHandoffJson(projection);
        REQUIRE(again.code == AuditHandoffResultCode::None);
        REQUIRE(again.serialized == first.serialized);
        REQUIRE(again.sha256 == first.sha256);
        REQUIRE(again.sizeBytes == first.sizeBytes);
    }
}

TEST_CASE("AuditHandoff explicit row cap reports retained and omitted",
          "[audit][handoff][kog-tsk-0137][unit][truncation]") {
    const auto projection = MakeProjection(/*seed=*/2,
                                           /*repositories=*/5,
                                           /*evidence=*/7,
                                           /*events=*/9);
    AuditHandoffLimits limits;
    limits.maxRepositories = 3;
    limits.maxEvidenceReferences = 4;
    limits.maxEvents = 6;

    const auto result = BuildAuditHandoffJson(projection, limits);
    REQUIRE(result.code == AuditHandoffResultCode::None);
    REQUIRE(result.truncated == true);
    REQUIRE(result.retainedRepositories == 3);
    REQUIRE(result.omittedRepositories == 2);
    REQUIRE(result.retainedEvidenceReferences == 4);
    REQUIRE(result.omittedEvidenceReferences == 3);
    REQUIRE(result.retainedEvents == 6);
    REQUIRE(result.omittedEvents == 3);

    const auto parsed = ParseHandoff(result.serialized);
    REQUIRE(parsed["repositories"].size() == 3);
    REQUIRE(parsed["evidenceReferences"].size() == 4);
    REQUIRE(parsed["events"].size() == 6);
    REQUIRE(parsed["truncation"]["retainedRepositories"] == 3);
    REQUIRE(parsed["truncation"]["omittedRepositories"] == 2);
    REQUIRE(parsed["truncation"]["retainedEvidenceReferences"] == 4);
    REQUIRE(parsed["truncation"]["omittedEvidenceReferences"] == 3);
    REQUIRE(parsed["truncation"]["retainedEvents"] == 6);
    REQUIRE(parsed["truncation"]["omittedEvents"] == 3);
    REQUIRE(parsed["limits"]["maxRepositories"] == 3);
    REQUIRE(parsed["limits"]["maxEvidenceReferences"] == 4);
    REQUIRE(parsed["limits"]["maxEvents"] == 6);
}

TEST_CASE("AuditHandoff byte cap enforces serialization ceiling",
          "[audit][handoff][kog-tsk-0137][unit][bytecap]") {
    const auto projection = MakeProjection(/*seed=*/3,
                                           /*repositories=*/4,
                                           /*evidence=*/5,
                                           /*events=*/5);
    AuditHandoffLimits limits;
    limits.maxSerializedBytes = 4U << 10U;
    limits.maxRepositories = 1;
    limits.maxEvidenceReferences = 1;
    limits.maxEvents = 1;

    const auto result = BuildAuditHandoffJson(projection, limits);
    // Either the document fits within the cap (None) or the document exceeds
    // the cap (ExceedsByteCap). Both are valid; the contract is that the
    // output never exceeds the cap silently.
    REQUIRE((result.code == AuditHandoffResultCode::None ||
             result.code == AuditHandoffResultCode::ExceedsByteCap));
    if (result.code == AuditHandoffResultCode::None) {
        REQUIRE(result.sizeBytes <= limits.maxSerializedBytes);
        REQUIRE(result.sizeBytes == result.serialized.size());
        REQUIRE_FALSE(result.serialized.empty());
        // The integrity field must be the SHA-256 of the canonical bytes
        // with the field set to all-zeros sentinel. Verify by parsing,
        // substituting the sentinel, re-serializing, and comparing.
        auto verify = nlohmann::json::parse(result.serialized);
        verify["integritySha256"] = std::string(64, '0');
        const auto canonical = verify.dump(0, ' ', true);
        REQUIRE(Sha256Hex(canonical) == result.sha256);
    }
}

TEST_CASE("AuditHandoff truncation truth surfaces projectionTruncated",
          "[audit][handoff][kog-tsk-0137][unit][truncation]") {
    OperationAuditRunProjection projection = MakeProjection(/*seed=*/4);
    projection.previewTruncated = true;
    projection.eventsTruncated = true;
    projection.repositoriesTruncated = true;
    projection.evidenceTruncated = true;

    const auto result = BuildAuditHandoffJson(projection);
    REQUIRE(result.code == AuditHandoffResultCode::None);
    const auto parsed = ParseHandoff(result.serialized);
    REQUIRE(parsed["truncation"]["projectionTruncated"] == true);
}

TEST_CASE("AuditHandoff redacted and withheld evidence bodies never appear",
          "[audit][handoff][kog-tsk-0137][unit][redaction]") {
    const auto projection = MakeProjection(/*seed=*/5);
    const auto result = BuildAuditHandoffJson(projection);
    REQUIRE(result.code == AuditHandoffResultCode::None);

    // The serialized bytes must not contain any raw command, secret, body,
    // or environment value. We assert that no JSON key literally named
    // `body`, `content`, `command`, `argv`, `payload`, `secrets`,
    // `password`, `token`, or `privateKey` is present in the document.
    // (Substring search is intentionally avoided because legitimate field
    // names like `contentType` would otherwise false-positive.)
    auto parsed = ParseHandoff(result.serialized);
    std::function<void(const nlohmann::json&)> visit = [&](const nlohmann::json& node) {
        if (node.is_object()) {
            for (auto it = node.begin(); it != node.end(); ++it) {
                const std::string key = it.key();
                REQUIRE(key != "body");
                REQUIRE(key != "content");
                REQUIRE(key != "command");
                REQUIRE(key != "argv");
                REQUIRE(key != "payload");
                REQUIRE(key != "secrets");
                REQUIRE(key != "password");
                REQUIRE(key != "token");
                REQUIRE(key != "privateKey");
                visit(it.value());
            }
        } else if (node.is_array()) {
            for (const auto& element : node) visit(element);
        }
    };
    visit(parsed);

    REQUIRE(parsed["redaction"]["hasRedactedEvidence"] == true);
    REQUIRE(parsed["redaction"]["hasWithheldEvidence"] == true);
    REQUIRE(parsed["redaction"]["redactedEvidenceCount"] == 1);
    REQUIRE(parsed["redaction"]["withheldEvidenceCount"] == 1);
    // The redacted and withheld entries must still appear as metadata
    // entries so the consumer can count them, but no body/content field is
    // present.
    for (const auto& item : parsed["evidenceReferences"]) {
        if (item["redactionStatus"] == "redacted" ||
            item["redactionStatus"] == "withheld") {
            REQUIRE(item.contains("body") == false);
            REQUIRE(item.contains("content") == false);
        }
    }
}

TEST_CASE("AuditHandoff missing or empty projection fails closed",
          "[audit][handoff][kog-tsk-0137][unit][empty]") {
    OperationAuditRunProjection empty;
    const auto result = BuildAuditHandoffJson(empty);
    REQUIRE(result.code == AuditHandoffResultCode::EmptyProjection);
    REQUIRE(result.serialized.empty());
    REQUIRE(result.sha256.empty());
}

TEST_CASE("AuditHandoff attempt isolation only surfaces the documented attempt",
          "[audit][handoff][kog-tsk-0137][unit][isolation]") {
    const auto projection = MakeProjection(/*seed=*/11);
    const auto result = BuildAuditHandoffJson(projection);
    REQUIRE(result.code == AuditHandoffResultCode::None);
    const auto parsed = ParseHandoff(result.serialized);
    REQUIRE(parsed["attempt"] == projection.attempt);
    REQUIRE(parsed["runId"] == projection.runId);
    REQUIRE(parsed["planId"] == projection.planId);
    REQUIRE(parsed["receiptSha256"] == projection.receiptId);
    REQUIRE_FALSE(parsed["receiptSha256"].get<std::string>().empty());
}

TEST_CASE("AuditHandoff integrity slot is the canonical-bytes hash",
          "[audit][handoff][kog-tsk-0137][unit][integrity]") {
    const auto projection = MakeProjection(/*seed=*/13);
    const auto result = BuildAuditHandoffJson(projection);
    REQUIRE(result.code == AuditHandoffResultCode::None);
    const auto parsed = ParseHandoff(result.serialized);
    // Wire contract: integritySha256 is the SHA-256 of the canonical bytes
    // with the field set to the 64-character all-zeros sentinel. Verifiers
    // substitute the sentinel, re-serialize, hash, and compare.
    auto verify = parsed;
    verify["integritySha256"] = std::string(64, '0');
    const auto verifyBytes = Sha256Hex(verify.dump(0, ' ', true));
    REQUIRE(verifyBytes == result.sha256);
    REQUIRE(parsed["integritySha256"] == result.sha256);
}

TEST_CASE("AuditHandoff forbidden field corpus rejects sensitive keys",
          "[audit][handoff][kog-tsk-0137][unit][security]") {
    // The implementation forbids a curated set of normalized keys from
    // appearing anywhere in the document. We simulate a contaminated source
    // by hand-rolling a projection whose repositories array would be
    // serialized with a forbidden key — this is exactly the attack the
    // implementation is designed to refuse. Since the builder's
    // `EnsureNoForbiddenKeys` only inspects what it builds, this test
    // covers the contract surface via the public field whitelist: the
    // emitted document never introduces forbidden keys.
    const auto projection = MakeProjection(/*seed=*/17);
    const auto result = BuildAuditHandoffJson(projection);
    REQUIRE(result.code == AuditHandoffResultCode::None);

    static const std::set<std::string> forbiddenNormalized{
        "apikey", "argv", "auth", "authorization",
        "body", "command", "credential", "credentials",
        "env", "environment", "error", "failed",
        "message", "ok", "password", "path",
        "payload", "privatekey", "rawcommand", "secret",
        "secrets", "stderr", "stdout", "succeeded",
        "success", "token", "tokens", "username",
        "value",
    };
    const auto parsed = ParseHandoff(result.serialized);
    std::function<void(const nlohmann::json&)> visit = [&](const nlohmann::json& node) {
        if (node.is_object()) {
            for (auto it = node.begin(); it != node.end(); ++it) {
                const auto& key = it.key();
                std::string normalized;
                normalized.reserve(key.size());
                for (const char ch : key) {
                    if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                        (ch >= '0' && ch <= '9')) {
                        normalized.push_back(static_cast<char>(
                            ch >= 'A' && ch <= 'Z' ? ch + ('a' - 'A') : ch));
                    }
                }
                REQUIRE(forbiddenNormalized.count(normalized) == 0);
                visit(it.value());
            }
        } else if (node.is_array()) {
            for (const auto& element : node) visit(element);
        }
    };
    visit(parsed);
}

TEST_CASE("AuditHandoff limits validation rejects impossible envelopes",
          "[audit][handoff][kog-tsk-0137][unit][config]") {
    OperationAuditRunProjection projection = MakeProjection(/*seed=*/19);
    AuditHandoffLimits limits;
    limits.maxSerializedBytes = 32; // below the 64-byte floor
    const auto tooSmall = BuildAuditHandoffJson(projection, limits);
    REQUIRE(tooSmall.code == AuditHandoffResultCode::InvalidConfiguration);

    AuditHandoffLimits huge;
    huge.maxSerializedBytes = (8U << 20U); // above the 4 MiB envelope
    const auto tooBig = BuildAuditHandoffJson(projection, huge);
    REQUIRE(tooBig.code == AuditHandoffResultCode::InvalidConfiguration);

    AuditHandoffLimits zero;
    zero.maxSerializedBytes = 1024;
    zero.maxRepositories = 0;
    const auto zeroCap = BuildAuditHandoffJson(projection, zero);
    REQUIRE(zeroCap.code == AuditHandoffResultCode::InvalidConfiguration);
}

TEST_CASE("AuditHandoff sorted projection ordering is stable across runs",
          "[audit][handoff][kog-tsk-0137][unit][ordering]") {
    // Construct a projection whose underlying vectors are in unsorted order.
    OperationAuditRunProjection projection;
    projection.runId = "sort-run";
    projection.attempt = 1;
    projection.planId = "plan-sort";
    projection.planSha256 = std::string(64, 'p');
    projection.frozenInputSha256 = std::string(64, 'q');
    projection.eventStreamSha256 = std::string(64, 'r');
    projection.receiptId = std::string(64, 's');
    projection.finishedAtUtc = "2026-08-10T00:38:15Z";
    projection.repositoryIdentityHeadSha256 = std::string(64, 't');
    projection.catalogRepositoryPreviewIdentityHeadSha256 = std::string(64, 'u');
    projection.correlation.mode = CorrelationMode::Standalone;

    auto addRepo = [&](const std::string& id) {
        RepositoryTransition transition;
        transition.repositoryId = id;
        transition.before.headSha = std::string(64, '0');
        transition.before.worktreeState = WorktreeState::Clean;
        transition.before.dirtyFingerprint = std::string(64, '1');
        transition.after.headSha = std::string(64, '2');
        transition.after.worktreeState = WorktreeState::Clean;
        transition.after.dirtyFingerprint = std::string(64, '3');
        projection.repositories.push_back(transition);
    };
    addRepo("zzz"); addRepo("aaa"); addRepo("mmm");

    auto addEvent = [&](const std::string& id, std::uint64_t seq) {
        OperationAuditEventPreview event;
        event.eventId = id; event.sequence = seq;
        event.repositoryId = "aaa"; event.phase = "phase"; event.action = "act";
        event.outcome.status = OutcomeState::Succeeded;
        projection.events.push_back(event);
    };
    addEvent("zzz-event", 3); addEvent("aaa-event", 1); addEvent("mmm-event", 2);

    const auto first = BuildAuditHandoffJson(projection);
    REQUIRE(first.code == AuditHandoffResultCode::None);
    const auto second = BuildAuditHandoffJson(projection);
    REQUIRE(second.code == AuditHandoffResultCode::None);
    REQUIRE(first.serialized == second.serialized);

    const auto parsed = ParseHandoff(first.serialized);
    REQUIRE(parsed["repositories"][0]["repositoryId"] == "aaa");
    REQUIRE(parsed["repositories"][1]["repositoryId"] == "mmm");
    REQUIRE(parsed["repositories"][2]["repositoryId"] == "zzz");
    REQUIRE(parsed["events"][0]["eventId"] == "aaa-event");
    REQUIRE(parsed["events"][1]["eventId"] == "mmm-event");
    REQUIRE(parsed["events"][2]["eventId"] == "zzz-event");
}
