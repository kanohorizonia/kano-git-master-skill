// Unit tests for the private deterministic serializer. Public callers cannot
// provide a naked projection; BuildAuditHandoffJson accepts a verification
// request and performs the pinned read itself. The pinned-reader integration
// test lives beside its Fixture in test_audit_run_reader.cpp.

#include <catch2/catch_test_macros.hpp>

#include "audit_contract.hpp"
#include "audit_handoff_private.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <vector>

using namespace kano::git::commands;
using namespace kano::git::audit;

namespace {

auto MakeProjection(const std::uint32_t seed,
                    const std::size_t repositories = 2,
                    const std::size_t evidenceCount = 3,
                    const std::size_t eventCount = 4) -> OperationAuditRunProjection {
    OperationAuditRunProjection projection;
    projection.runId = "run-" + std::to_string(seed);
    projection.parentRunId = "parent-run";
    projection.attempt = seed == 0 ? 1U : seed;
    projection.planId = "plan-" + std::to_string(seed);
    projection.planSha256 = std::string(64, 'a');
    projection.frozenInputSha256 = std::string(64, 'b');
    projection.eventStreamSha256 = std::string(64, 'c');
    projection.receiptId = std::string(64, 'd');
    projection.finishedAtUtc = "2026-08-10T00:38:15Z";
    projection.repositoryIdentityHeadSha256 = std::string(64, 'e');
    projection.catalogRepositoryPreviewIdentityHeadSha256 = std::string(64, 'f');
    projection.correlation.mode = CorrelationMode::Koa;
    projection.correlation.productId = "product-" + std::to_string(seed);
    projection.correlation.topicId = "topic-" + std::to_string(seed);
    projection.correlation.itemId = "item-" + std::to_string(seed);
    projection.correlation.workOrderId = "work-" + std::to_string(seed);
    projection.correlation.requestId = "request-" + std::to_string(seed);
    projection.correlation.producerId = "producer-" + std::to_string(seed);
    projection.correlation.routeId = "route-" + std::to_string(seed);
    projection.terminalOutcome.status = OutcomeState::Succeeded;
    projection.terminalOutcome.exitCode = 0;

    for (std::size_t index = 0; index < repositories; ++index) {
        RepositoryTransition transition;
        transition.repositoryId = "repos/repo-" + std::to_string(index);
        transition.before.headSha = std::string(40, '0');
        transition.before.worktreeState = WorktreeState::Clean;
        transition.before.dirtyFingerprint = std::string(64, '1');
        transition.after.headSha = std::string(40, '2');
        transition.after.worktreeState = WorktreeState::Clean;
        transition.after.dirtyFingerprint = std::string(64, '3');
        projection.repositories.push_back(transition);
    }

    for (std::size_t index = 0; index < evidenceCount; ++index) {
        OperationAuditEvidencePreview item;
        item.category = index % 2 == 0 ? "policy" : "approval";
        item.id = "evidence-" + std::to_string(index);
        item.kind = "kind-" + std::to_string(index);
        item.sha256 = std::string(64, '4');
        item.sizeBytes = 1024 + index;
        item.contentType = "text/plain";
        if (index == 0) {
            item.redactionStatus = RedactionStatus::Redacted;
            ++projection.redactedEvidenceCount;
            projection.hasRedactedEvidence = true;
        } else if (index == 1) {
            item.redactionStatus = RedactionStatus::Withheld;
            ++projection.withheldEvidenceCount;
            projection.hasWithheldEvidence = true;
        }
        projection.evidence.push_back(std::move(item));
    }

    for (std::size_t index = 0; index < eventCount; ++index) {
        OperationAuditEventPreview event;
        event.eventId = "event-" + std::to_string(index);
        event.sequence = index + 1;
        event.repositoryId = "repos/repo-" + std::to_string(index % repositories);
        event.beforeHeadSha = std::string(40, '5');
        event.afterHeadSha = std::string(40, '6');
        event.phase = "phase";
        event.action = "action-" + std::to_string(index);
        event.outcome.status = OutcomeState::Succeeded;
        projection.events.push_back(std::move(event));
    }
    projection.totalEventRecords = projection.retainedEventRecords = eventCount;
    projection.totalRepositories = projection.retainedRepositories = repositories;
    projection.totalEvidenceReferences = projection.retainedEvidenceReferences = evidenceCount;
    return projection;
}

auto VerifiedRead(OperationAuditRunProjection projection,
                  OperationAuditRunReadState state = OperationAuditRunReadState::Ready)
    -> OperationAuditRunReadResult {
    return {.state = state, .code = OperationAuditRunReadCode::None,
            .run = std::move(projection)};
}

auto Serialize(const OperationAuditRunProjection& projection,
               AuditHandoffLimits limits = {}) -> AuditHandoffResult {
    return SerializeVerifiedAuditHandoff(VerifiedRead(projection), limits);
}

} // namespace

TEST_CASE("AuditHandoff serializer emits closed v1 schema projection",
          "[audit][handoff][KOG-TSK-0137][unit]") {
    const auto projection = MakeProjection(1);
    const auto result = Serialize(projection);
    REQUIRE(result.code == AuditHandoffResultCode::None);
    REQUIRE(result.sizeBytes == result.serialized.size());
    REQUIRE(result.sha256 == Sha256Hex(result.serialized));

    const auto doc = nlohmann::json::parse(result.serialized);
    REQUIRE(doc["schemaName"] == std::string(kAuditHandoffSchemaName));
    REQUIRE(doc["schemaVersion"] == kAuditHandoffSchemaVersion);
    REQUIRE(doc["runId"] == projection.runId);
    REQUIRE(doc["attempt"] == projection.attempt);
    REQUIRE(doc["receiptSha256"] == projection.receiptId);
    REQUIRE(doc["frozenInputSha256"] == projection.frozenInputSha256);
    REQUIRE(doc["eventStreamSha256"] == projection.eventStreamSha256);
    REQUIRE(doc["correlation"]["productId"] == "product-1");
    REQUIRE(doc["terminalOutcome"]["status"] == "succeeded");
    REQUIRE(doc["repositoryTransitions"].size() == 2);
    REQUIRE(doc["events"].size() == 4);
    REQUIRE(doc["evidenceReferences"].size() == 3);
    REQUIRE_FALSE(doc.contains("exportedAtUtc"));
    REQUIRE_FALSE(doc.contains("integritySha256"));
    REQUIRE(doc["generationLimits"]["maxSerializedBytes"] == (256U << 10U));
}

TEST_CASE("AuditHandoff stable verified projection serializes byte-identically",
          "[audit][handoff][KOG-TSK-0137][unit][deterministic]") {
    const auto projection = MakeProjection(7);
    const auto first = Serialize(projection);
    REQUIRE(first.code == AuditHandoffResultCode::None);
    const auto second = Serialize(projection);
    REQUIRE(second.code == AuditHandoffResultCode::None);
    REQUIRE(first.serialized == second.serialized);
    REQUIRE(first.sha256 == second.sha256);
}

TEST_CASE("AuditHandoff reports exact total, reader and handoff omissions",
          "[audit][handoff][KOG-TSK-0137][unit][truncation]") {
    auto projection = MakeProjection(2, 3, 4, 5);
    projection.totalEventRecords = 10;
    projection.eventsTruncated = true;
    projection.totalRepositories = 8;
    projection.repositoriesTruncated = true;
    projection.totalEvidenceReferences = 12;
    projection.evidenceTruncated = true;
    projection.previewTruncated = true;

    AuditHandoffLimits limits;
    limits.maxEvents = 2;
    limits.maxRepositories = 2;
    limits.maxEvidenceReferences = 3;
    const auto result = Serialize(projection, limits);
    REQUIRE(result.code == AuditHandoffResultCode::None);
    REQUIRE(result.truncated);
    const auto doc = nlohmann::json::parse(result.serialized);
    const auto& counts = doc["truncation"];
    REQUIRE(counts["totalEvents"] == 10);
    REQUIRE(counts["readerRetainedEvents"] == 5);
    REQUIRE(counts["retainedEvents"] == 2);
    REQUIRE(counts["readerOmittedEvents"] == 5);
    REQUIRE(counts["handoffOmittedEvents"] == 3);
    REQUIRE(counts["omittedEvents"] == 8);
    REQUIRE(counts["truncatedEvents"] == 8);
    REQUIRE(counts["totalRepositories"] == 8);
    REQUIRE(counts["readerRetainedRepositories"] == 3);
    REQUIRE(counts["retainedRepositories"] == 2);
    REQUIRE(counts["omittedRepositories"] == 6);
    REQUIRE(counts["totalEvidenceReferences"] == 12);
    REQUIRE(counts["readerRetainedEvidenceReferences"] == 4);
    REQUIRE(counts["retainedEvidenceReferences"] == 3);
    REQUIRE(counts["omittedEvidenceReferences"] == 9);
}

TEST_CASE("AuditHandoff byte cap rejects whole output without partial JSON",
          "[audit][handoff][KOG-TSK-0137][unit][bytecap]") {
    const auto projection = MakeProjection(3);
    AuditHandoffLimits limits;
    limits.maxSerializedBytes = 256;
    const auto result = Serialize(projection, limits);
    REQUIRE(result.code == AuditHandoffResultCode::ByteCapExceeded);
    REQUIRE(result.serialized.empty());
    REQUIRE(result.sha256.empty());
}

TEST_CASE("AuditHandoff row caps are enforced before serialization",
          "[audit][handoff][KOG-TSK-0137][unit][rowcap]") {
    const auto projection = MakeProjection(4, 5, 7, 9);
    AuditHandoffLimits limits;
    limits.maxRepositories = 3;
    limits.maxEvidenceReferences = 4;
    limits.maxEvents = 6;
    const auto result = Serialize(projection, limits);
    REQUIRE(result.code == AuditHandoffResultCode::None);
    const auto doc = nlohmann::json::parse(result.serialized);
    REQUIRE(doc["repositoryTransitions"].size() == 3);
    REQUIRE(doc["evidenceReferences"].size() == 4);
    REQUIRE(doc["events"].size() == 6);
    REQUIRE(result.omittedRepositories == 2);
    REQUIRE(result.omittedEvidenceReferences == 3);
    REQUIRE(result.omittedEvents == 3);
}

TEST_CASE("AuditHandoff redacted and withheld references contain metadata only",
          "[audit][handoff][KOG-TSK-0137][unit][redaction]") {
    const auto result = Serialize(MakeProjection(5));
    REQUIRE(result.code == AuditHandoffResultCode::None);
    const auto doc = nlohmann::json::parse(result.serialized);
    REQUIRE(doc["redaction"]["redactedEvidenceCount"] == 1);
    REQUIRE(doc["redaction"]["withheldEvidenceCount"] == 1);
    for (const auto& evidence : doc["evidenceReferences"]) {
        REQUIRE_FALSE(evidence.contains("body"));
        REQUIRE_FALSE(evidence.contains("content"));
    }
}

TEST_CASE("AuditHandoff hashes legacy event and evidence IDs instead of echoing values",
          "[audit][handoff][KOG-TSK-0137][unit][secret-corpus]") {
    auto projection = MakeProjection(6);
    projection.events.front().eventId = "ghp_A1B2C3D4secret-token";
    projection.evidence.front().id = "AWS_SECRET_ACCESS_KEY=do-not-export";
    const auto result = Serialize(projection);
    REQUIRE(result.code == AuditHandoffResultCode::None);
    REQUIRE(result.serialized.find("ghp_A1B2C3D4secret-token") == std::string::npos);
    REQUIRE(result.serialized.find("AWS_SECRET_ACCESS_KEY=do-not-export") == std::string::npos);
    REQUIRE(result.serialized.find("eventIdentitySha256") != std::string::npos);
    REQUIRE(result.serialized.find("referenceIdentitySha256") != std::string::npos);
}

TEST_CASE("AuditHandoff excludes branch path text from repository transitions",
          "[audit][handoff][KOG-TSK-0137][unit][path-leak]") {
    auto projection = MakeProjection(7);
    projection.repositories.front().before.branch = "C:/Users/private/worktree";
    projection.repositories.front().after.branch = "../../outside";
    const auto result = Serialize(projection);
    REQUIRE(result.code == AuditHandoffResultCode::None);
    REQUIRE(result.serialized.find("C:/Users/private/worktree") == std::string::npos);
    REQUIRE(result.serialized.find("../../outside") == std::string::npos);
    const auto doc = nlohmann::json::parse(result.serialized);
    REQUIRE_FALSE(doc["repositoryTransitions"][0].contains("branch"));
}

TEST_CASE("AuditHandoff refuses credential-like correlation and command metadata",
          "[audit][handoff][KOG-TSK-0137][unit][fail-closed]") {
    auto credential = MakeProjection(8);
    credential.correlation.requestId = "request-secret-value";
    const auto credentialResult = Serialize(credential);
    REQUIRE(credentialResult.code == AuditHandoffResultCode::UnsafeMetadata);

    auto command = MakeProjection(9);
    command.events.front().action = "git push origin main";
    const auto commandResult = Serialize(command);
    REQUIRE(commandResult.code == AuditHandoffResultCode::UnsafeMetadata);
}

TEST_CASE("AuditHandoff refuses non-verified reader outcomes without data",
          "[audit][handoff][KOG-TSK-0137][unit][verification]") {
    const auto projection = MakeProjection(10);
    auto read = VerifiedRead(projection, OperationAuditRunReadState::Corrupt);
    read.code = OperationAuditRunReadCode::MalformedEvidence;
    const auto result = SerializeVerifiedAuditHandoff(read, {});
    REQUIRE(result.code == AuditHandoffResultCode::ReadNotVerified);
    REQUIRE(result.readState == OperationAuditRunReadState::Corrupt);
    REQUIRE(result.readCode == OperationAuditRunReadCode::MalformedEvidence);
    REQUIRE(result.serialized.empty());
}

TEST_CASE("AuditHandoff fails closed on inconsistent source counts",
          "[audit][handoff][KOG-TSK-0137][unit][count-integrity]") {
    auto projection = MakeProjection(11);
    projection.retainedEventRecords = 1; // vector still contains four
    const auto result = Serialize(projection);
    REQUIRE(result.code == AuditHandoffResultCode::InvalidVerifiedProjection);
    REQUIRE(result.serialized.empty());
}
