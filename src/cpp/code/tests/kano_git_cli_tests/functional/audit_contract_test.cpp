#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include "audit_contract.hpp"

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>
#include <string_view>
#include <utility>

namespace {

using kano::git::audit::AuditEventsParseResult;
using kano::git::audit::ValidationResult;

auto RepoRoot() -> std::filesystem::path {
    return std::filesystem::weakly_canonical(
        std::filesystem::path(KANO_GIT_TEST_REPO_ROOT));
}

auto FixturePath(std::string_view InRelative) -> std::filesystem::path {
    return RepoRoot() / "assets" / "audit" / "fixtures" / InRelative;
}

auto SchemaPath(std::string_view InName) -> std::filesystem::path {
    return RepoRoot() / "assets" / "audit" / "schemas" / InName;
}

auto ReadBinary(const std::filesystem::path& InPath) -> std::string {
    std::ifstream input(InPath, std::ios::in | std::ios::binary);
    REQUIRE(input.good());
    return std::string(std::istreambuf_iterator<char>(input),
                       std::istreambuf_iterator<char>());
}

auto WithoutFinalLf(std::string InValue) -> std::string {
    if (!InValue.empty() && InValue.back() == '\n') {
        InValue.pop_back();
    }
    return InValue;
}

auto HasCode(const ValidationResult& InValidation, std::string_view InCode)
    -> bool {
    return std::any_of(
        InValidation.issues.begin(), InValidation.issues.end(),
        [InCode](const auto& issue) { return issue.code == InCode; });
}

auto HasPathAndCode(const ValidationResult& InValidation,
                    std::string_view InPathPart, std::string_view InCode)
    -> bool {
    return std::any_of(InValidation.issues.begin(), InValidation.issues.end(),
                       [InPathPart, InCode](const auto& issue) {
                           return issue.code == InCode &&
                               issue.path.find(InPathPart) != std::string::npos;
                       });
}

auto HasExactPathAndCode(const ValidationResult& InValidation,
                         std::string_view InPath, std::string_view InCode)
    -> bool {
    return std::any_of(InValidation.issues.begin(), InValidation.issues.end(),
                       [InPath, InCode](const auto& issue) {
                           return issue.code == InCode && issue.path == InPath;
                       });
}

auto LoadGoldenEvents() -> AuditEventsParseResult {
    return kano::git::audit::ParseAuditEventsJsonl(
        ReadBinary(FixturePath("golden/audit-events.v1.jsonl")));
}

} // namespace

TEST_CASE("KG-TSK-0124 publishes parseable closed-core v1 schemas",
          "[infrastructure][audit][output][KG-TSK-0124]") {
    const auto eventSchema = nlohmann::json::parse(
        ReadBinary(SchemaPath("kog.auditEvent.v1.schema.json")));
    const auto receiptSchema = nlohmann::json::parse(
        ReadBinary(SchemaPath("kog.runReceipt.v1.schema.json")));

    REQUIRE(eventSchema.at("$schema") ==
            "https://json-schema.org/draft/2020-12/schema");
    REQUIRE(eventSchema.at("properties").at("schemaName").at("const") ==
            "kog.auditEvent");
    REQUIRE(eventSchema.at("properties").at("schemaVersion").at("const") == 1);
    REQUIRE(eventSchema.at("additionalProperties") == false);
    REQUIRE(eventSchema.at("$defs")
                .at("artifactReference")
                .at("additionalProperties") == false);
    REQUIRE(eventSchema.at("$defs")
                .at("outcome")
                .at("properties")
                .at("status")
                .at("enum")
                .size() == 7);
    REQUIRE(eventSchema.at("$defs").at("positiveUint32").at("maximum") ==
            std::numeric_limits<std::uint32_t>::max());
    REQUIRE(eventSchema.at("properties").at("attempt").at("$ref") ==
            "#/$defs/positiveUint32");

    REQUIRE(receiptSchema.at("properties").at("schemaName").at("const") ==
            "kog.runReceipt");
    REQUIRE(receiptSchema.at("properties").at("schemaVersion").at("const") == 1);
    REQUIRE(receiptSchema.at("properties").contains("terminalOutcome"));
    REQUIRE(receiptSchema.at("properties").contains("eventStreamSha256"));
    REQUIRE(receiptSchema.at("additionalProperties") == false);
    REQUIRE(receiptSchema.at("properties").at("repositories").at("maxItems") ==
            256);
}

TEST_CASE(
    "KG-TSK-0124 golden events and receipt round-trip byte deterministically",
    "[infrastructure][audit][output][KG-TSK-0124]") {
    const auto eventBytes =
        ReadBinary(FixturePath("golden/audit-events.v1.jsonl"));
    const auto receiptBytes =
        ReadBinary(FixturePath("golden/run-receipt.v1.json"));

    auto events = kano::git::audit::ParseAuditEventsJsonl(eventBytes);
    auto receipt = kano::git::audit::ParseRunReceiptJson(receiptBytes);
    REQUIRE(events.ok());
    REQUIRE(events.values.size() == 2);
    REQUIRE(receipt.ok());
    REQUIRE(
        kano::git::audit::ValidateRunTrace(*receipt.value, events.values).ok());

    const auto serializedEvents =
        kano::git::audit::SerializeAuditEventsJsonl(events.values);
    REQUIRE(serializedEvents.ok());
    REQUIRE(serializedEvents.json == eventBytes);

    const auto serializedReceipt =
        kano::git::audit::SerializeRunReceiptJson(*receipt.value);
    REQUIRE(serializedReceipt.ok());
    REQUIRE(serializedReceipt.json == WithoutFinalLf(receiptBytes));

    std::reverse(receipt.value->artifacts.begin(),
                 receipt.value->artifacts.end());
    const auto reserialized =
        kano::git::audit::SerializeRunReceiptJson(*receipt.value);
    REQUIRE(reserialized.ok());
    REQUIRE(reserialized.json == WithoutFinalLf(receiptBytes));
}

TEST_CASE("KG-TSK-0124 maximum repository receipt remains parser round-trippable",
          "[infrastructure][audit][output][KG-TSK-0124]") {
    auto receipt = kano::git::audit::ParseRunReceiptJson(
        ReadBinary(FixturePath("golden/run-receipt.v1.json")));
    REQUIRE(receipt.ok());

    const auto prototype = receipt.value->repositories.front();
    std::string unicodeBranch;
    for (std::size_t index = 0; index < 512; ++index) {
        unicodeBranch += "\xF0\x9F\x9A\x80";
    }
    receipt.value->repositories.clear();
    for (std::size_t index = 0; index < 256; ++index) {
        auto repository = prototype;
        repository.repositoryId =
            "repos/r" + std::to_string(1000 + index).substr(1);
        repository.before.branch = unicodeBranch;
        repository.after.branch = unicodeBranch;
        receipt.value->repositories.push_back(std::move(repository));
    }

    const auto serialized =
        kano::git::audit::SerializeRunReceiptJson(*receipt.value);
    REQUIRE(serialized.ok());
    REQUIRE(serialized.json.size() < 4U * 1024U * 1024U);
    const auto reparsed =
        kano::git::audit::ParseRunReceiptJson(serialized.json);
    REQUIRE(reparsed.ok());
    REQUIRE(reparsed.value->repositories.size() == 256);
}

TEST_CASE("KG-TSK-0124 invalid fixtures fail closed with stable diagnostics",
          "[infrastructure][audit][output][KG-TSK-0124]") {
    const auto goldenEventBytes =
        ReadBinary(FixturePath("golden/audit-events.v1.jsonl"));
    const auto firstLf = goldenEventBytes.find('\n');
    const auto secondLf = goldenEventBytes.find('\n', firstLf + 1);
    REQUIRE(firstLf != std::string::npos);
    REQUIRE(secondLf != std::string::npos);
    const auto firstGolden = nlohmann::json::parse(
        goldenEventBytes.substr(0, firstLf));
    const auto secondGolden = nlohmann::json::parse(
        goldenEventBytes.substr(firstLf + 1, secondLf - firstLf - 1));

    auto malformedHashJson = firstGolden;
    malformedHashJson["planSha256"] = "ABC123";
    const auto malformedHash =
        kano::git::audit::ParseAuditEventJson(malformedHashJson.dump());
    REQUIRE_FALSE(malformedHash.ok());
    REQUIRE(
        HasPathAndCode(malformedHash.validation, "planSha256", "invalid_sha256"));

    auto missingTerminalJson = nlohmann::json::parse(
        ReadBinary(FixturePath("golden/run-receipt.v1.json")));
    missingTerminalJson.erase("terminalOutcome");
    const auto missingTerminal =
        kano::git::audit::ParseRunReceiptJson(missingTerminalJson.dump());
    REQUIRE_FALSE(missingTerminal.ok());
    REQUIRE(HasPathAndCode(missingTerminal.validation, "terminalOutcome",
                           "missing_required_field"));

    auto rawCommandJson = firstGolden;
    rawCommandJson["command"] = "git push origin main";
    const auto rawCommand =
        kano::git::audit::ParseAuditEventJson(rawCommandJson.dump());
    REQUIRE_FALSE(rawCommand.ok());
    REQUIRE(HasPathAndCode(rawCommand.validation, "command", "forbidden_field"));

    auto contradictorySuccessJson = firstGolden;
    contradictorySuccessJson["outcome"] = {
        {"status", "succeeded"},
        {"exitCode", 7},
        {"reasonCode", "REMOTE_PUSH_FAILED"},
        {"retryable", true},
    };
    const auto contradictorySuccess =
        kano::git::audit::ParseAuditEventJson(contradictorySuccessJson.dump());
    REQUIRE_FALSE(contradictorySuccess.ok());
    REQUIRE(HasCode(contradictorySuccess.validation, "contradictory_success"));

    auto duplicateSecond = secondGolden;
    duplicateSecond["sequence"] = firstGolden["sequence"];
    const auto duplicate = kano::git::audit::ParseAuditEventsJsonl(
        firstGolden.dump() + "\n" + duplicateSecond.dump() + "\n");
    REQUIRE_FALSE(duplicate.ok());
    REQUIRE(HasCode(duplicate.validation, "duplicate_or_non_monotonic_sequence"));

    auto nonMonotonicFirst = firstGolden;
    auto nonMonotonicSecond = secondGolden;
    nonMonotonicFirst["sequence"] = 2;
    nonMonotonicSecond["sequence"] = 1;
    const auto nonMonotonic = kano::git::audit::ParseAuditEventsJsonl(
        nonMonotonicFirst.dump() + "\n" + nonMonotonicSecond.dump() + "\n");
    REQUIRE_FALSE(nonMonotonic.ok());
    REQUIRE(
        HasCode(nonMonotonic.validation, "duplicate_or_non_monotonic_sequence"));

    const auto duplicateKey = kano::git::audit::ParseAuditEventJson(
        ReadBinary(FixturePath("invalid/duplicate-safety-field.json")));
    REQUIRE_FALSE(duplicateKey.ok());
    REQUIRE(
        HasExactPathAndCode(duplicateKey.validation, "$", "duplicate_json_key"));
}

TEST_CASE("KG-TSK-0124 JSONL framing and closed-root boundary are explicit",
          "[infrastructure][audit][output][KG-TSK-0124]") {
    const auto eventBytes =
        ReadBinary(FixturePath("golden/audit-events.v1.jsonl"));
    const auto firstLf = eventBytes.find('\n');
    REQUIRE(firstLf != std::string::npos);
    const auto firstEventBytes = eventBytes.substr(0, firstLf);

    auto missingLf = kano::git::audit::ParseAuditEventsJsonl(firstEventBytes);
    REQUIRE_FALSE(missingLf.ok());
    REQUIRE(HasCode(missingLf.validation, "missing_final_lf"));

    auto crlfBytes = firstEventBytes + "\r\n";
    auto crlf = kano::git::audit::ParseAuditEventsJsonl(crlfBytes);
    REQUIRE_FALSE(crlf.ok());
    REQUIRE(HasCode(crlf.validation, "cr_not_allowed"));

    auto additiveJson = nlohmann::json::parse(firstEventBytes);
    additiveJson["producerBuild"] = "0.0.1.941";
    const auto additive =
        kano::git::audit::ParseAuditEventJson(additiveJson.dump());
    REQUIRE_FALSE(additive.ok());
    REQUIRE(HasExactPathAndCode(additive.validation, "$.producerBuild",
                                "unknown_field"));

    additiveJson.erase("producerBuild");
    additiveJson["rawCommand"] = "git push";
    const auto forbidden =
        kano::git::audit::ParseAuditEventJson(additiveJson.dump());
    REQUIRE_FALSE(forbidden.ok());
    REQUIRE(HasCode(forbidden.validation, "forbidden_field"));

    additiveJson.erase("rawCommand");
    additiveJson["producerMetadata"] = {
        {"rawCommand", "git push"},
        {"password", "do-not-log"},
    };
    const auto nestedSensitive =
        kano::git::audit::ParseAuditEventJson(additiveJson.dump());
    REQUIRE_FALSE(nestedSensitive.ok());
    REQUIRE(HasExactPathAndCode(nestedSensitive.validation, "$.producerMetadata",
                                "unknown_field"));

    additiveJson.erase("producerMetadata");
    additiveJson["attempt"] = 9007199254740991ULL;
    const auto overflowing =
        kano::git::audit::ParseAuditEventJson(additiveJson.dump());
    REQUIRE_FALSE(overflowing.ok());
    REQUIRE(HasCode(overflowing.validation, "integer_out_of_range"));

    additiveJson["attempt"] = 1;
    additiveJson["outcome"]["exitCode"] =
        std::numeric_limits<std::uint64_t>::max();
    const auto unsignedExitOverflow =
        kano::git::audit::ParseAuditEventJson(additiveJson.dump());
    REQUIRE_FALSE(unsignedExitOverflow.ok());
    REQUIRE(HasExactPathAndCode(unsignedExitOverflow.validation,
                                "$.outcome.exitCode", "integer_out_of_range"));

    std::string duplicateAttempt = firstEventBytes;
    const auto attemptPosition = duplicateAttempt.find("\"attempt\":1");
    REQUIRE(attemptPosition != std::string::npos);
    duplicateAttempt.replace(attemptPosition, std::string("\"attempt\":1").size(),
                             "\"attempt\":1,\"attempt\":2");
    const auto duplicateKey =
        kano::git::audit::ParseAuditEventJson(duplicateAttempt);
    REQUIRE_FALSE(duplicateKey.ok());
    REQUIRE(HasCode(duplicateKey.validation, "duplicate_json_key"));

    const std::string excessiveRecords(1'000'001, '\n');
    const auto bounded =
        kano::git::audit::ParseAuditEventsJsonl(excessiveRecords);
    REQUIRE_FALSE(bounded.ok());
    REQUIRE(HasCode(bounded.validation, "too_many_events"));

    auto malformedLine = nlohmann::json::parse(firstEventBytes);
    malformedLine["planSha256"] = "not-a-hash";
    const auto malformedStream =
        kano::git::audit::ParseAuditEventsJsonl(malformedLine.dump() + "\n");
    REQUIRE_FALSE(malformedStream.ok());
    REQUIRE(HasExactPathAndCode(malformedStream.validation, "$[0].planSha256",
                                "invalid_sha256"));

    std::string malformedRecords;
    for (std::size_t index = 0; index < 4096; ++index) {
        malformedRecords += "{}\n";
    }
    const auto boundedMalformed =
        kano::git::audit::ParseAuditEventsJsonl(malformedRecords);
    REQUIRE_FALSE(boundedMalformed.ok());
    REQUIRE(boundedMalformed.values.empty());
}

TEST_CASE("KG-TSK-0124 parser rejects oversized arrays before materialization",
          "[infrastructure][audit][output][KG-TSK-0124]") {
    const auto eventBytes =
        ReadBinary(FixturePath("golden/audit-events.v1.jsonl"));
    const auto firstLf = eventBytes.find('\n');
    REQUIRE(firstLf != std::string::npos);
    const auto goldenEvent =
        nlohmann::json::parse(eventBytes.substr(0, firstLf));

    auto tooManyReferences = goldenEvent;
    const auto reference = goldenEvent["policyRefs"].front();
    tooManyReferences["policyRefs"] = nlohmann::json::array();
    for (std::size_t index = 0; index < 33; ++index) {
        tooManyReferences["policyRefs"].push_back(reference);
    }
    const auto references =
        kano::git::audit::ParseAuditEventJson(tooManyReferences.dump());
    REQUIRE_FALSE(references.ok());
    REQUIRE(HasExactPathAndCode(references.validation, "$.policyRefs",
                                "too_many_references"));
    REQUIRE(references.value->policyRefs.empty());

    auto tooManyArtifacts = goldenEvent;
    const auto artifact = goldenEvent["artifacts"].front();
    tooManyArtifacts["artifacts"] = nlohmann::json::array();
    for (std::size_t index = 0; index < 65; ++index) {
        tooManyArtifacts["artifacts"].push_back(artifact);
    }
    const auto artifacts =
        kano::git::audit::ParseAuditEventJson(tooManyArtifacts.dump());
    REQUIRE_FALSE(artifacts.ok());
    REQUIRE(HasExactPathAndCode(artifacts.validation, "$.artifacts",
                                "too_many_artifacts"));
    REQUIRE(artifacts.value->artifacts.empty());

    auto tooManyRepositories = nlohmann::json::parse(
        ReadBinary(FixturePath("golden/run-receipt.v1.json")));
    const auto repository = tooManyRepositories["repositories"].front();
    tooManyRepositories["repositories"] = nlohmann::json::array();
    for (std::size_t index = 0; index < 257; ++index) {
        tooManyRepositories["repositories"].push_back(repository);
    }
    const auto receipt =
        kano::git::audit::ParseRunReceiptJson(tooManyRepositories.dump());
    REQUIRE_FALSE(receipt.ok());
    REQUIRE(HasExactPathAndCode(receipt.validation, "$.repositories",
                                "too_many_repositories"));
    REQUIRE(receipt.value->repositories.empty());
}

TEST_CASE(
    "KG-TSK-0124 run validator rejects false clean, success, and trace claims",
    "[infrastructure][audit][output][KG-TSK-0124]") {
    auto events = LoadGoldenEvents();
    auto receipt = kano::git::audit::ParseRunReceiptJson(
        ReadBinary(FixturePath("golden/run-receipt.v1.json")));
    REQUIRE(events.ok());
    REQUIRE(receipt.ok());

    SECTION("unknown worktree cannot carry a dirty fingerprint") {
        events.values.front().repository.before.worktreeState =
            kano::git::audit::WorktreeState::Unknown;
        const auto validation =
            kano::git::audit::ValidateAuditEvent(events.values.front());
        REQUIRE(HasCode(validation, "unknown_worktree_conflict"));
    }

    SECTION("succeeded receipt cannot contain a failed event") {
        events.values.back().outcome.status =
            kano::git::audit::OutcomeState::Failed;
        events.values.back().outcome.exitCode = 1;
        events.values.back().outcome.reasonCode = "REMOTE_PUSH_FAILED";
        const auto validation =
            kano::git::audit::ValidateRunTrace(*receipt.value, events.values);
        REQUIRE(HasCode(validation, "contradictory_success"));
    }

    SECTION("repository transitions must chain") {
        events.values.back().repository.before.headSha =
            "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
        const auto validation =
            kano::git::audit::ValidateRunTrace(*receipt.value, events.values);
        REQUIRE(HasCode(validation, "broken_repository_transition"));
    }

    SECTION("receipt hash binds canonical JSONL bytes") {
        receipt.value->eventStreamSha256 =
            "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
        const auto validation =
            kano::git::audit::ValidateRunTrace(*receipt.value, events.values);
        REQUIRE(HasCode(validation, "event_stream_hash_mismatch"));
    }

    SECTION("event IDs are unique within the run") {
        events.values.back().eventId = events.values.front().eventId;
        const auto validation =
            kano::git::audit::ValidateRunTrace(*receipt.value, events.values);
        REQUIRE(HasCode(validation, "duplicate_event_id"));
    }

    SECTION("receipt cannot invent a repository transition") {
        auto fabricated = receipt.value->repositories.front();
        fabricated.repositoryId = "repos/fabricated";
        receipt.value->repositories.push_back(std::move(fabricated));
        const auto validation =
            kano::git::audit::ValidateRunTrace(*receipt.value, events.values);
        REQUIRE(HasCode(validation, "receipt_repository_unobserved"));
    }

    SECTION("one evidence ID cannot bind two hashes") {
        events.values.back().policyRefs.front().sha256 =
            "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
        const auto validation =
            kano::git::audit::ValidateRunTrace(*receipt.value, events.values);
        REQUIRE(HasCode(validation, "evidence_equivocation"));
    }

    SECTION("receipt evidence is the exact event-stream union") {
        receipt.value->approvalRefs.clear();
        const auto validation =
            kano::git::audit::ValidateRunTrace(*receipt.value, events.values);
        REQUIRE(HasExactPathAndCode(validation, "$.approvalRefs",
                                    "receipt_evidence_mismatch"));
    }

    SECTION("standalone cannot claim KOA identity") {
        events.values.front().correlation.mode =
            kano::git::audit::CorrelationMode::Standalone;
        const auto validation =
            kano::git::audit::ValidateAuditEvent(events.values.front());
        REQUIRE(HasCode(validation, "standalone_correlation_conflict"));
    }

    SECTION("real-calendar UTC and time order are validated") {
        events.values.front().startedAtUtc = "2026-02-30T08:00:00Z";
        events.values.front().finishedAtUtc = "2026-02-28T08:00:00Z";
        const auto validation =
            kano::git::audit::ValidateAuditEvent(events.values.front());
        REQUIRE(HasCode(validation, "invalid_utc_timestamp"));
    }
}

TEST_CASE("KG-TSK-0124 zero-event crash receipt binds empty truth",
          "[infrastructure][audit][output][KG-TSK-0124]") {
    auto receipt = kano::git::audit::ParseRunReceiptJson(
        ReadBinary(FixturePath("golden/run-receipt.v1.json")));
    REQUIRE(receipt.ok());

    receipt.value->firstSequence = 0;
    receipt.value->lastSequence = 0;
    receipt.value->eventCount = 0;
    receipt.value->eventStreamSha256 = "e3b0c44298fc1c149afbf4c8996fb924"
                                       "27ae41e4649b934ca495991b7852b855";
    receipt.value->terminalOutcome.status =
        kano::git::audit::OutcomeState::Unknown;
    receipt.value->terminalOutcome.exitCode = std::nullopt;
    receipt.value->terminalOutcome.reasonCode = "CRASH_BEFORE_FIRST_EVENT";
    receipt.value->terminalOutcome.retryable = true;
    receipt.value->repositories.clear();
    receipt.value->policyRefs.clear();
    receipt.value->approvalRefs.clear();
    receipt.value->artifacts.clear();

    REQUIRE(kano::git::audit::ValidateRunTrace(*receipt.value, {}).ok());

    SECTION("arbitrary empty-stream hash is rejected") {
        receipt.value->eventStreamSha256 =
            "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
        const auto validation =
            kano::git::audit::ValidateRunTrace(*receipt.value, {});
        REQUIRE(HasCode(validation, "event_stream_hash_mismatch"));
    }

    SECTION("repository claims without events are rejected") {
        auto golden = kano::git::audit::ParseRunReceiptJson(
            ReadBinary(FixturePath("golden/run-receipt.v1.json")));
        REQUIRE(golden.ok());
        receipt.value->repositories = golden.value->repositories;
        const auto validation =
            kano::git::audit::ValidateRunTrace(*receipt.value, {});
        REQUIRE(HasCode(validation, "receipt_repository_unobserved"));
    }
}

TEST_CASE("KG-TSK-0124 typed API rejects invalid enum and UTF-8 values",
          "[infrastructure][audit][output][KG-TSK-0124]") {
    auto events = LoadGoldenEvents();
    REQUIRE(events.ok());

    SECTION("invalid outcome enum cannot canonicalize to unknown") {
        events.values.front().outcome.status =
            static_cast<kano::git::audit::OutcomeState>(99);
        const auto serialized =
            kano::git::audit::SerializeAuditEventJson(events.values.front());
        REQUIRE_FALSE(serialized.ok());
        REQUIRE(HasCode(serialized.validation, "invalid_outcome"));
    }

    SECTION("invalid worktree enum cannot canonicalize to unknown") {
        events.values.front().repository.before.worktreeState =
            static_cast<kano::git::audit::WorktreeState>(99);
        const auto serialized =
            kano::git::audit::SerializeAuditEventJson(events.values.front());
        REQUIRE_FALSE(serialized.ok());
        REQUIRE(HasCode(serialized.validation, "invalid_worktree_state"));
    }

    SECTION("invalid redaction enum cannot canonicalize to withheld") {
        events.values.front().artifacts.front().redactionStatus =
            static_cast<kano::git::audit::RedactionStatus>(99);
        const auto serialized =
            kano::git::audit::SerializeAuditEventJson(events.values.front());
        REQUIRE_FALSE(serialized.ok());
        REQUIRE(HasCode(serialized.validation, "invalid_redaction_status"));
    }

    SECTION("invalid correlation enum cannot canonicalize to standalone") {
        events.values.front().correlation.mode =
            static_cast<kano::git::audit::CorrelationMode>(99);
        const auto serialized =
            kano::git::audit::SerializeAuditEventJson(events.values.front());
        REQUIRE_FALSE(serialized.ok());
        REQUIRE(HasCode(serialized.validation, "invalid_correlation_mode"));
    }

    SECTION("invalid UTF-8 is a validation result, never an exception") {
        events.values.front().repository.before.branch = std::string("\xC3\x28", 2);
        const auto serialized =
            kano::git::audit::SerializeAuditEventJson(events.values.front());
        REQUIRE_FALSE(serialized.ok());
        REQUIRE(HasCode(serialized.validation, "invalid_branch"));
    }
}

TEST_CASE("KG-TSK-0124 standalone and KOA correlation modes fail closed",
          "[infrastructure][audit][output][KG-TSK-0124]") {
    auto events = LoadGoldenEvents();
    REQUIRE(events.ok());
    auto event = events.values.front();

    event.correlation.mode = kano::git::audit::CorrelationMode::Standalone;
    event.correlation.productId.reset();
    event.correlation.topicId.reset();
    event.correlation.itemId.reset();
    event.correlation.workOrderId.reset();
    event.correlation.requestId.reset();
    event.correlation.producerId.reset();
    event.correlation.routeId.reset();
    event.correlation.agentId.reset();
    REQUIRE(kano::git::audit::ValidateAuditEvent(event).ok());
    const auto standalone = kano::git::audit::SerializeAuditEventJson(event);
    REQUIRE(standalone.ok());
    REQUIRE(kano::git::audit::ParseAuditEventJson(standalone.json).ok());

    const auto koa = events.values.front().correlation;
    using OptionalMember =
        std::optional<std::string> kano::git::audit::CorrelationRefs::*;
    const std::array<OptionalMember, 6> requiredFields{
        &kano::git::audit::CorrelationRefs::productId,
        &kano::git::audit::CorrelationRefs::itemId,
        &kano::git::audit::CorrelationRefs::workOrderId,
        &kano::git::audit::CorrelationRefs::requestId,
        &kano::git::audit::CorrelationRefs::producerId,
        &kano::git::audit::CorrelationRefs::routeId,
    };
    for (const auto field : requiredFields) {
        event = events.values.front();
        event.correlation = koa;
        event.correlation.*field = std::nullopt;
        const auto validation = kano::git::audit::ValidateAuditEvent(event);
        REQUIRE(HasCode(validation, "incomplete_koa_correlation"));
    }
}
