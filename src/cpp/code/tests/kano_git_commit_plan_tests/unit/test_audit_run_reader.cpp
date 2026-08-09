#include <catch2/catch_test_macros.hpp>

#include "audit_run_reader.hpp"
#include "shell_executor.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <vector>

using namespace kano::git::commands;

namespace {

auto UniqueRoot() -> std::filesystem::path {
    static std::atomic_uint64_t sequence = 0;
    const auto nonce = std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
        std::to_string(sequence.fetch_add(1));
    return std::filesystem::temp_directory_path() /
        ("kog-audit-reader-" + kano::git::audit::Sha256Hex(nonce).substr(0, 20));
}

auto CurrentUtc() -> std::string {
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
    return buffer;
}

auto ReadText(const std::filesystem::path& path) -> std::string {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), {}};
}

auto WriteText(const std::filesystem::path& path, const std::string& bytes) -> void {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    REQUIRE(output.good());
    output << bytes;
    REQUIRE(output.good());
}

auto RequireGit(const std::filesystem::path& root,
                const std::vector<std::string>& args) -> void {
    const auto result = kano::git::shell::ExecuteCommand(
        "git", args, kano::git::shell::ExecMode::Capture, root);
    INFO(result.stdoutStr); INFO(result.stderrStr);
    REQUIRE(result.exitCode == 0);
}

auto InitialiseGitRepository(const std::filesystem::path& root) -> void {
    RequireGit(root, {"init"});
    RequireGit(root, {"config", "user.email", "audit-reader@example.invalid"});
    RequireGit(root, {"config", "user.name", "Audit Reader Tests"});
    WriteText(root / "tracked.txt", "initial\n");
    RequireGit(root, {"add", "tracked.txt"});
    RequireGit(root, {"commit", "-m", "initial"});
}

auto PayloadBytes(const OperationAuditRunProjection& run) -> std::size_t {
    // This deliberately mirrors the documented retained UTF-8 string payload
    // metric rather than JSON encoding size or container/object overhead.
    std::size_t bytes = 0;
    const auto add = [&](const std::string& value) { bytes += value.size(); };
    const auto addOptional = [&](const std::optional<std::string>& value) {
        if (value) add(*value);
    };
    for (const auto& repository : run.repositories) {
        add(repository.repositoryId);
        for (const auto* state : {&repository.before, &repository.after}) {
            addOptional(state->headSha); addOptional(state->branch);
            addOptional(state->dirtyFingerprint); addOptional(state->upstreamHeadSha);
        }
    }
    for (const auto& evidence : run.evidence) {
        add(evidence.category); add(evidence.id); add(evidence.kind);
        add(evidence.sha256); add(evidence.contentType);
    }
    for (const auto& event : run.events) {
        add(event.eventId); add(event.repositoryId); addOptional(event.beforeHeadSha);
        addOptional(event.afterHeadSha); add(event.phase); add(event.action);
        addOptional(event.outcome.reasonCode);
    }
    return bytes;
}

auto MakeSpec(const std::filesystem::path& root, const std::string& runId,
              const std::uint32_t attempt) -> OperationAuditSpec {
    REQUIRE(std::filesystem::create_directories(root));
    const auto sourcePath = root / "plan.json";
    const nlohmann::json correlation = {
        {"mode", "koa"}, {"product_id", "product"}, {"topic_id", "topic"},
        {"item_id", "item"}, {"work_order_id", "work"}, {"request_id", "request"},
        {"run_id", runId}, {"parent_run_id", "parent"},
        {"producer_id", "producer"}, {"route_id", "route"}, {"attempt", attempt},
    };
    const auto bytes = nlohmann::json({
        {"meta", {{"plan_id", "plan-reader"}, {"correlation", correlation}}},
    }).dump() + '\n';
    WriteText(sourcePath, bytes);

    OperationAuditSpec spec;
    spec.workspaceRoot = root; spec.sourcePath = sourcePath;
    spec.inputIdentity = sourcePath.generic_string(); spec.inputKind = "commit-plan";
    spec.route = "commit-push.plan"; spec.planId = "plan-reader";
    spec.sourceBytes = bytes; spec.frozenBytes = bytes; spec.frozenFileName = "frozen-plan.json";
    spec.correlation.mode = "koa"; spec.correlation.productId = "product";
    spec.correlation.topicId = "topic"; spec.correlation.itemId = "item";
    spec.correlation.workOrderId = "work"; spec.correlation.requestId = "request";
    spec.correlation.runId = runId; spec.correlation.parentRunId = "parent";
    spec.correlation.producerId = "producer"; spec.correlation.routeId = "route";
    spec.correlation.attempt = attempt;
    return spec;
}

struct Fixture {
    std::filesystem::path root = UniqueRoot();
    OperationAuditSpec spec;
    OperationAuditPaths paths;

    Fixture() : spec(MakeSpec(root, "reader-run", 3)) {
        std::string error;
        const auto resolved = ResolveOperationAuditPaths(spec, "reader-run", 3, &error);
        INFO(error); REQUIRE(resolved); paths = *resolved;
    }
    ~Fixture() { std::error_code ec; std::filesystem::remove_all(root, ec); }

    auto Finalize(const int exitCode = 0, const int appended = 2,
                  const bool supplemental = true) -> void {
        std::string error;
        auto context = OperationAuditContext::Reserve(spec, &error);
        INFO(error); REQUIRE(context);
        if (supplemental) {
            REQUIRE(context->FreezeSupplementalBytes("withheld receipt evidence", "reader-evidence",
                                                      "evidence-reader", &error));
        }
        for (int index = 0; index < appended; ++index) {
            const auto before = context->Capture(root);
            REQUIRE(context->Append("reader.phase." + std::to_string(index), root, before,
                                    CurrentUtc(), index + 1 == appended ? exitCode : 0, &error));
        }
        REQUIRE(context->Finalize(exitCode, &error));
        context.reset();
    }
};

auto PendingMarker(const Fixture& fixture) -> nlohmann::json {
    return {
        {"schemaName", "kog.auditPublicationPending"},
        {"schemaVersion", 1},
        {"runId", "reader-run"},
        {"parentRunId", "parent"},
        {"attempt", 3},
        {"planId", "plan-reader"},
        {"planSha256", kano::git::audit::Sha256Hex(fixture.spec.frozenBytes)},
        {"reservedAtUtc", CurrentUtc()},
    };
}

auto IncompleteMarker(const Fixture& fixture) -> nlohmann::json {
    return {
        {"schemaName", "kog.auditIncomplete"},
        {"schemaVersion", 1},
        {"runId", "reader-run"},
        {"parentRunId", "parent"},
        {"attempt", 3},
        {"planId", "plan-reader"},
        {"planSha256", kano::git::audit::Sha256Hex(fixture.spec.frozenBytes)},
        {"reasonCode", "event-stream-invalid"},
        {"recoverable", true},
        {"receiptPublished", false},
        {"observedEventCount", 0},
        {"recordedAtUtc", CurrentUtc()},
    };
}

auto RequireState(const OperationAuditRunReadResult& result,
                  const OperationAuditRunReadState state,
                  const OperationAuditRunReadCode code) -> void {
    REQUIRE(result.state == state); REQUIRE(result.code == code);
}

template <typename Mutator>
auto RewriteCanonicalTrace(const Fixture& fixture, Mutator&& mutate) -> void {
    auto events = kano::git::audit::ParseAuditEventsJsonl(ReadText(fixture.paths.events));
    auto receipt = kano::git::audit::ParseRunReceiptJson(ReadText(fixture.paths.receipt));
    REQUIRE(events.ok()); REQUIRE(receipt.ok()); REQUIRE(receipt.value);
    mutate(events.values, *receipt.value);
    const auto eventBytes = kano::git::audit::SerializeAuditEventsJsonl(events.values);
    REQUIRE(eventBytes.ok());
    receipt.value->eventStreamSha256 = kano::git::audit::Sha256Hex(eventBytes.json);
    REQUIRE(kano::git::audit::ValidateRunTrace(*receipt.value, events.values).ok());
    const auto receiptBytes = kano::git::audit::SerializeRunReceiptJson(*receipt.value);
    REQUIRE(receiptBytes.ok());
    WriteText(fixture.paths.events, eventBytes.json);
    WriteText(fixture.paths.receipt, receiptBytes.json);
}

auto RewriteZeroEventCrash(const Fixture& fixture) -> void {
    auto receipt = kano::git::audit::ParseRunReceiptJson(ReadText(fixture.paths.receipt));
    REQUIRE(receipt.ok()); REQUIRE(receipt.value);
    receipt.value->firstSequence = 0; receipt.value->lastSequence = 0;
    receipt.value->eventCount = 0;
    receipt.value->eventStreamSha256 =
        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";
    receipt.value->terminalOutcome.status = kano::git::audit::OutcomeState::Unknown;
    receipt.value->terminalOutcome.exitCode = std::nullopt;
    receipt.value->terminalOutcome.reasonCode = "crash-before-first-event";
    receipt.value->terminalOutcome.retryable = true;
    receipt.value->repositories.clear(); receipt.value->policyRefs.clear();
    receipt.value->approvalRefs.clear(); receipt.value->artifacts.clear();
    REQUIRE(kano::git::audit::ValidateRunTrace(*receipt.value, {}).ok());
    const auto receiptBytes = kano::git::audit::SerializeRunReceiptJson(*receipt.value);
    REQUIRE(receiptBytes.ok());
    WriteText(fixture.paths.events, "");
    WriteText(fixture.paths.receipt, receiptBytes.json);
}

} // namespace

TEST_CASE("KG-TSK-0130 audit run reader exposes durable lifecycle states",
          "[Unit][Audit][Reader][KG-TSK-0130]") {
    Fixture fixture;

    SECTION("missing attempt and existing empty attempt stay distinct") {
        const auto missing = ReadOperationAuditRun(fixture.spec, "reader-run", 3);
        RequireState(missing, OperationAuditRunReadState::Missing,
                     OperationAuditRunReadCode::AttemptMissing);
        REQUIRE_FALSE(missing.run);
        REQUIRE(std::filesystem::create_directories(fixture.paths.attemptRoot));
        const auto empty = ReadOperationAuditRun(fixture.spec, "reader-run", 3);
        RequireState(empty, OperationAuditRunReadState::Missing,
                     OperationAuditRunReadCode::EvidenceMissing);
        REQUIRE_FALSE(empty.run);
    }

    SECTION("ready projection retains correlation, receipt identity, outcome, phase, and withheld evidence") {
        fixture.Finalize();
        const auto result = ReadOperationAuditRun(fixture.spec, "reader-run", 3);
        RequireState(result, OperationAuditRunReadState::Ready, OperationAuditRunReadCode::None);
        REQUIRE(result.verified()); REQUIRE(result.run);
        const auto& run = *result.run;
        REQUIRE_FALSE(run.receiptId.empty()); REQUIRE(run.runId == "reader-run");
        REQUIRE(run.attempt == 3); REQUIRE(run.correlation.productId == "product");
        REQUIRE(run.correlation.routeId == "route");
        REQUIRE(run.terminalOutcome.status == kano::git::audit::OutcomeState::Succeeded);
        REQUIRE(run.events.size() == run.totalEventRecords);
        REQUIRE(run.events.front().phase == "preflight");
        REQUIRE(run.events.back().phase == "mutation");
        REQUIRE_FALSE(run.evidence.empty());
        REQUIRE(run.evidence.back().redactionStatus == kano::git::audit::RedactionStatus::Withheld);

        bool traceValid = false;
        std::string error;
        const auto verified = VerifyOperationAuditJson(
            fixture.spec, "reader-run", 3, &traceValid, &error);
        INFO(error); REQUIRE(verified); REQUIRE(traceValid);
        const auto json = nlohmann::json::parse(*verified);
        REQUIRE(json.at("receiptSha256") == run.receiptId);
        REQUIRE(json.at("eventCount") == run.totalEventRecords);
        REQUIRE(json.at("repositories").size() == run.totalRepositories);
        const auto artifactCount = std::count_if(
            run.evidence.begin(), run.evidence.end(),
            [](const auto& item) { return item.category == "artifact"; });
        REQUIRE(json.at("artifacts").size() == artifactCount);
    }

    SECTION("legacy verification JSON is the complete canonical 22-field shape") {
        fixture.Finalize();
        const auto receiptBytes = ReadText(fixture.paths.receipt);
        const auto frozenBytes = ReadText(fixture.paths.frozenInput);
        const auto parsedReceipt = kano::git::audit::ParseRunReceiptJson(receiptBytes);
        const auto parsedEvents = kano::git::audit::ParseAuditEventsJsonl(
            ReadText(fixture.paths.events));
        REQUIRE(parsedReceipt.ok()); REQUIRE(parsedReceipt.value); REQUIRE(parsedEvents.ok());
        const auto& receipt = *parsedReceipt.value;
        bool traceValid = false;
        std::string error;
        const auto verified = VerifyOperationAuditJson(fixture.spec, "reader-run", 3,
                                                       &traceValid, &error);
        INFO(error); REQUIRE(verified); REQUIRE(traceValid);
        const auto actual = nlohmann::json::parse(*verified);
        const auto nullable = [](const auto& value) {
            return value ? nlohmann::json(*value) : nlohmann::json(nullptr);
        };
        const auto state = [&](const auto& value) {
            return nlohmann::json{{"headSha", nullable(value.headSha)},
                {"branch", nullable(value.branch)},
                {"worktreeState", kano::git::audit::WorktreeStateName(value.worktreeState)},
                {"dirtyFingerprint", nullable(value.dirtyFingerprint)},
                {"upstreamHeadSha", nullable(value.upstreamHeadSha)},
                {"ahead", nullable(value.ahead)}, {"behind", nullable(value.behind)}};
        };
        nlohmann::json repositories = nlohmann::json::array();
        for (const auto& repository : receipt.repositories)
            repositories.push_back({{"id", repository.repositoryId},
                                    {"before", state(repository.before)},
                                    {"after", state(repository.after)}});
        nlohmann::json artifacts = nlohmann::json::array();
        for (const auto& item : receipt.artifacts)
            artifacts.push_back({{"id", item.id}, {"kind", item.kind},
                                 {"sha256", item.sha256}, {"sizeBytes", item.sizeBytes},
                                 {"contentType", item.contentType},
                                 {"redactionStatus", kano::git::audit::RedactionStatusName(item.redactionStatus)}});
        const nlohmann::json expected = {
            {"schemaName", "kog.auditVerification"}, {"schemaVersion", 1},
            {"ok", true}, {"error", nullptr}, {"traceValid", true},
            {"eventsValid", true}, {"receiptValid", true}, {"frozenInputValid", true},
            {"planId", receipt.planId}, {"planSha256", receipt.planSha256},
            {"frozenInputSha256", kano::git::audit::Sha256Hex(frozenBytes)}, {"runId", receipt.runId},
            {"parentRunId", nullable(receipt.parentRunId)}, {"attempt", receipt.attempt},
            {"correlation", {{"mode", kano::git::audit::CorrelationModeName(receipt.correlation.mode)},
                {"productId", nullable(receipt.correlation.productId)}, {"topicId", nullable(receipt.correlation.topicId)},
                {"itemId", nullable(receipt.correlation.itemId)}, {"workOrderId", nullable(receipt.correlation.workOrderId)},
                {"requestId", nullable(receipt.correlation.requestId)}, {"producerId", nullable(receipt.correlation.producerId)},
                {"routeId", nullable(receipt.correlation.routeId)}, {"agentId", nullable(receipt.correlation.agentId)}}},
            {"eventCount", parsedEvents.values.size()}, {"eventStreamSha256", receipt.eventStreamSha256},
            {"receiptSha256", kano::git::audit::Sha256Hex(receiptBytes)},
            {"terminalOutcome", {{"status", kano::git::audit::OutcomeStateName(receipt.terminalOutcome.status)},
                {"exitCode", nullable(receipt.terminalOutcome.exitCode)},
                {"reasonCode", nullable(receipt.terminalOutcome.reasonCode)}, {"retryable", receipt.terminalOutcome.retryable}}},
            {"repositories", repositories}, {"artifacts", artifacts},
        };
        REQUIRE(actual.size() == expected.size());
        REQUIRE(actual == expected);
    }

    SECTION("terminal success and failure outcomes are explicit") {
        fixture.Finalize(17, 1, false);
        const auto result = ReadOperationAuditRun(fixture.spec, "reader-run", 3);
        REQUIRE(result.verified()); REQUIRE(result.run);
        REQUIRE(result.run->terminalOutcome.status == kano::git::audit::OutcomeState::Failed);
        REQUIRE(result.run->terminalOutcome.exitCode == 17);
        REQUIRE(result.run->terminalOutcome.reasonCode == "pipeline-failed");
    }

    SECTION("contract-valid zero-event crash receipt is verified without JSONL parser rejection") {
        fixture.Finalize(); RewriteZeroEventCrash(fixture);
        const auto result = ReadOperationAuditRun(fixture.spec, "reader-run", 3);
        RequireState(result, OperationAuditRunReadState::Ready, OperationAuditRunReadCode::None);
        REQUIRE(result.run); REQUIRE(result.run->totalEventRecords == 0);
        REQUIRE(result.run->events.empty());
        REQUIRE(result.run->terminalOutcome.status == kano::git::audit::OutcomeState::Unknown);
        bool traceValid = false;
        std::string error;
        const auto verified = VerifyOperationAuditJson(
            fixture.spec, "reader-run", 3, &traceValid, &error);
        INFO(error); REQUIRE(verified); REQUIRE(traceValid);
        const auto json = nlohmann::json::parse(*verified);
        REQUIRE(json.at("eventCount") == 0); REQUIRE(json.at("artifacts").empty());
    }

    SECTION("zero-event crash still validates frozen source, plan, and correlation semantics") {
        fixture.Finalize(); RewriteZeroEventCrash(fixture);
        const auto rewriteFrozen = [&](const auto mutate) {
            auto frozen = nlohmann::json::parse(fixture.spec.frozenBytes);
            mutate(frozen);
            const auto bytes = frozen.dump() + '\n';
            fixture.spec.sourceBytes = bytes;
            fixture.spec.frozenBytes = bytes;
            WriteText(fixture.paths.frozenInput, bytes);
            auto receipt = nlohmann::json::parse(ReadText(fixture.paths.receipt));
            receipt["planSha256"] = kano::git::audit::Sha256Hex(bytes);
            WriteText(fixture.paths.receipt, receipt.dump());
        };
        SECTION("contradictory frozen plan is corrupt") {
            rewriteFrozen([](auto& frozen) { frozen["meta"]["plan_id"] = "other-plan"; });
            const auto result = ReadOperationAuditRun(fixture.spec, "reader-run", 3);
            RequireState(result, OperationAuditRunReadState::Corrupt,
                         OperationAuditRunReadCode::FrozenCommitPlanIdentityMismatch);
        }
        SECTION("contradictory frozen correlation is corrupt") {
            rewriteFrozen([](auto& frozen) {
                frozen["meta"]["correlation"]["product_id"] = "wrong-product";
            });
            const auto result = ReadOperationAuditRun(fixture.spec, "reader-run", 3);
            RequireState(result, OperationAuditRunReadState::Corrupt,
                         OperationAuditRunReadCode::FrozenCorrelationMismatch);
        }
    }

    SECTION("zero-event crash rejects receipt evidence claims") {
        fixture.Finalize(); RewriteZeroEventCrash(fixture);
        const auto zeroReceipt = ReadText(fixture.paths.receipt);
        const auto reject = [&](const std::string_view field, const nlohmann::json& claim,
                                const OperationAuditRunReadCode expected =
                                    OperationAuditRunReadCode::TraceInvalid) {
            auto receipt = nlohmann::json::parse(ReadText(fixture.paths.receipt));
            receipt[std::string(field)] = nlohmann::json::array({claim});
            WriteText(fixture.paths.receipt, receipt.dump());
            const auto result = ReadOperationAuditRun(fixture.spec, "reader-run", 3);
            RequireState(result, OperationAuditRunReadState::Corrupt,
                         expected);
            WriteText(fixture.paths.receipt, zeroReceipt);
        };
        const auto sha = std::string(64, 'a');
        const nlohmann::json state = {{"headSha", nullptr}, {"branch", nullptr},
            {"worktreeState", "unknown"}, {"dirtyFingerprint", nullptr},
            {"upstreamHeadSha", nullptr}, {"ahead", nullptr}, {"behind", nullptr}};
        reject("repositories", {{"id", "claimed-repository"},
                                {"before", state}, {"after", state}},
               OperationAuditRunReadCode::MalformedEvidence);
        reject("policyRefs", {{"id", "claimed-policy"}, {"sha256", sha}});
        reject("approvalRefs", {{"id", "claimed-approval"}, {"sha256", sha}});
        reject("artifacts", {{"id", "claimed-artifact"}, {"kind", "claimed"},
                              {"sha256", sha}, {"sizeBytes", 0},
                              {"contentType", "application/json"},
                              {"redactionStatus", "not-required"}});
    }

    SECTION("reserved evidence remains pending until receipt publication") {
        std::string error;
        auto context = OperationAuditContext::Reserve(fixture.spec, &error);
        INFO(error); REQUIRE(context);
        const auto result = ReadOperationAuditRun(
            fixture.spec, "reader-run", 3);
        RequireState(result, OperationAuditRunReadState::Pending,
                     OperationAuditRunReadCode::PublicationPending);
        REQUIRE_FALSE(result.run);
        bool traceValid = true;
        REQUIRE_FALSE(VerifyOperationAuditJson(fixture.spec, "reader-run", 3,
                                               &traceValid, &error));
        REQUIRE_FALSE(traceValid);
        REQUIRE(error == "audit receipt publication is still pending");

        auto standalone = fixture.spec;
        standalone.correlation.mode = "standalone";
        standalone.correlation.productId.clear(); standalone.correlation.topicId.clear();
        standalone.correlation.itemId.clear(); standalone.correlation.workOrderId.clear();
        standalone.correlation.requestId.clear(); standalone.correlation.parentRunId.clear();
        standalone.correlation.producerId.clear(); standalone.correlation.routeId.clear();
        error.clear(); traceValid = true;
        REQUIRE_FALSE(VerifyOperationAuditJson(standalone, "reader-run", 3,
                                               &traceValid, &error));
        REQUIRE_FALSE(traceValid);
        REQUIRE(error == "audit receipt publication is still pending");
    }

    SECTION("standalone lookup remains a lookup wildcard for a KOA receipt") {
        fixture.Finalize();
        auto lookup = fixture.spec;
        lookup.correlation.mode = "standalone";
        lookup.correlation.productId.clear(); lookup.correlation.topicId.clear();
        lookup.correlation.itemId.clear(); lookup.correlation.workOrderId.clear();
        lookup.correlation.requestId.clear(); lookup.correlation.parentRunId.clear();
        lookup.correlation.producerId.clear(); lookup.correlation.routeId.clear();
        const auto result = ReadOperationAuditRun(
            lookup, "reader-run", 3, {}, std::nullopt,
            {.callerCorrelation = OperationAuditCallerCorrelationPolicy::StandaloneWildcard});
        RequireState(result, OperationAuditRunReadState::Ready,
                     OperationAuditRunReadCode::None);
        REQUIRE(result.run); REQUIRE(result.run->correlation.mode == kano::git::audit::CorrelationMode::Koa);

        bool traceValid = false;
        std::string error;
        const auto verified = VerifyOperationAuditJson(lookup, "reader-run", 3,
                                                       &traceValid, &error);
        INFO(error); REQUIRE(verified); REQUIRE(traceValid);
    }

    SECTION("KOA verification retains exact caller-correlation matching") {
        fixture.Finalize();
        auto mismatched = fixture.spec;
        mismatched.correlation.productId = "wrong-product";
        bool traceValid = true;
        std::string error;
        const auto verified = VerifyOperationAuditJson(mismatched, "reader-run", 3,
                                                       &traceValid, &error);
        REQUIRE_FALSE(verified); REQUIRE_FALSE(traceValid);
        REQUIRE(error == "requested correlation does not match verified evidence");
    }

    SECTION("legacy KOA verification treats an omitted parent run as a lookup wildcard") {
        fixture.Finalize();
        auto lookup = fixture.spec; lookup.correlation.parentRunId.clear();
        const auto strict = ReadOperationAuditRun(lookup, "reader-run", 3);
        RequireState(strict, OperationAuditRunReadState::Corrupt,
                     OperationAuditRunReadCode::CorrelationMismatch);
        bool traceValid = false;
        std::string error;
        const auto verified = VerifyOperationAuditJson(lookup, "reader-run", 3,
                                                       &traceValid, &error);
        INFO(error); REQUIRE(verified); REQUIRE(traceValid);
    }

    SECTION("incomplete wins over pending") {
        REQUIRE(std::filesystem::create_directories(fixture.paths.attemptRoot));
        WriteText(fixture.paths.publicationPending, PendingMarker(fixture).dump());
        WriteText(fixture.paths.incomplete, IncompleteMarker(fixture).dump());
        const auto result = ReadOperationAuditRun(fixture.spec, "reader-run", 3);
        RequireState(result, OperationAuditRunReadState::Incomplete,
                     OperationAuditRunReadCode::EvidenceIncomplete);
        REQUIRE_FALSE(result.run);
    }

    SECTION("closed marker schema rejects reduced and extra records") {
        REQUIRE(std::filesystem::create_directories(fixture.paths.attemptRoot));
        auto marker = PendingMarker(fixture);
        marker.erase("reservedAtUtc");
        WriteText(fixture.paths.publicationPending, marker.dump());
        auto reduced = ReadOperationAuditRun(fixture.spec, "reader-run", 3);
        RequireState(reduced, OperationAuditRunReadState::Corrupt,
                     OperationAuditRunReadCode::PendingMarkerInvalid);

        marker = PendingMarker(fixture);
        marker["extra"] = true;
        WriteText(fixture.paths.publicationPending, marker.dump());
        auto extra = ReadOperationAuditRun(fixture.spec, "reader-run", 3);
        RequireState(extra, OperationAuditRunReadState::Corrupt,
                     OperationAuditRunReadCode::PendingMarkerInvalid);

        marker = PendingMarker(fixture);
        const auto canonical = marker.dump();
        WriteText(fixture.paths.publicationPending,
                  "{\"schemaName\":\"kog.auditPublicationPending\"," +
                      canonical.substr(1));
        auto duplicate = ReadOperationAuditRun(fixture.spec, "reader-run", 3);
        RequireState(duplicate, OperationAuditRunReadState::Corrupt,
                     OperationAuditRunReadCode::PendingMarkerInvalid);

        marker = PendingMarker(fixture);
        marker["planSha256"] = std::string(64, '0');
        WriteText(fixture.paths.publicationPending, marker.dump());
        auto wrongPlan = ReadOperationAuditRun(fixture.spec, "reader-run", 3);
        RequireState(wrongPlan, OperationAuditRunReadState::Corrupt,
                     OperationAuditRunReadCode::PendingMarkerInvalid);
    }

    SECTION("marker timestamps and observed counts are semantic protocol values") {
        REQUIRE(std::filesystem::create_directories(fixture.paths.attemptRoot));
        auto marker = PendingMarker(fixture);
        marker["reservedAtUtc"] = "2026-99-99T99:99:99Z";
        WriteText(fixture.paths.publicationPending, marker.dump());
        const auto invalidTime = ReadOperationAuditRun(fixture.spec, "reader-run", 3);
        RequireState(invalidTime, OperationAuditRunReadState::Corrupt,
                     OperationAuditRunReadCode::PendingMarkerInvalid);

        marker = IncompleteMarker(fixture);
        marker["observedEventCount"] = 1'000'001;
        WriteText(fixture.paths.publicationPending, "");
        std::filesystem::remove(fixture.paths.publicationPending);
        WriteText(fixture.paths.incomplete, marker.dump());
        const auto invalidCount = ReadOperationAuditRun(fixture.spec, "reader-run", 3);
        RequireState(invalidCount, OperationAuditRunReadState::Corrupt,
                     OperationAuditRunReadCode::IncompleteMarkerInvalid);
    }
}

TEST_CASE("KG-TSK-0130 audit reader bounds projections deterministically",
          "[Unit][Audit][Reader][KG-TSK-0130]") {
    Fixture fixture; fixture.Finalize(0, 2, true);
    const auto full = ReadOperationAuditRun(fixture.spec, "reader-run", 3);
    REQUIRE(full.verified()); REQUIRE(full.run);
    const auto& expected = *full.run;

    const auto bounded = [&](const std::size_t events, const std::size_t repositories,
                             const std::size_t evidence, const std::size_t bytes) {
        auto limits = OperationAuditRunReadLimits{};
        limits.maxEventRecords = events; limits.maxRepositories = repositories;
        limits.maxEvidenceReferences = evidence; limits.maxPreviewBytes = bytes;
        return ReadOperationAuditRun(fixture.spec, "reader-run", 3, limits);
    };

    SECTION("zero caps preserve verified receipt and truncate all previews") {
        const auto result = bounded(0, 0, 0, 0);
        REQUIRE(result.state == OperationAuditRunReadState::Truncated); REQUIRE(result.verified());
        REQUIRE(result.run); REQUIRE(result.run->events.empty()); REQUIRE(result.run->repositories.empty());
        REQUIRE(result.run->evidence.empty()); REQUIRE(result.run->previewTruncated);
    }
    SECTION("exact and plus-one caps retain stable ordered projections") {
        const auto exact = bounded(expected.events.size(), expected.repositories.size(),
                                   expected.evidence.size(), expected.retainedPreviewBytes + 64);
        RequireState(exact, OperationAuditRunReadState::Ready, OperationAuditRunReadCode::None);
        REQUIRE(exact.run); REQUIRE(exact.run->events.size() == expected.events.size());
        for (std::size_t index = 0; index < expected.events.size(); ++index) {
            REQUIRE(exact.run->events[index].sequence == expected.events[index].sequence);
            REQUIRE(exact.run->events[index].phase == expected.events[index].phase);
            REQUIRE(exact.run->events[index].action == expected.events[index].action);
        }
        const auto plus = bounded(expected.events.size() + 1, expected.repositories.size() + 1,
                                  expected.evidence.size() + 1, expected.retainedPreviewBytes + 65);
        RequireState(plus, OperationAuditRunReadState::Ready, OperationAuditRunReadCode::None);
        REQUIRE(plus.run); REQUIRE(plus.run->events.size() == expected.events.size());
        for (std::size_t index = 0; index < expected.events.size(); ++index)
            REQUIRE(plus.run->events[index].sequence == expected.events[index].sequence);
    }
    SECTION("one-less record caps report retained counts and typed truncation") {
        const auto result = bounded(expected.events.size() - 1, expected.repositories.size() - 1,
                                    expected.evidence.size() - 1,
                                    expected.retainedPreviewBytes + 64);
        REQUIRE(result.state == OperationAuditRunReadState::Truncated); REQUIRE(result.verified());
        REQUIRE(result.run); REQUIRE(result.run->retainedEventRecords + 1 == expected.events.size());
        REQUIRE(result.run->eventsTruncated); REQUIRE(result.run->repositoriesTruncated);
        REQUIRE(result.run->evidenceTruncated);
    }
    SECTION("byte cap retains only whole records") {
        const auto result = bounded(expected.events.size(), expected.repositories.size(),
                                    expected.evidence.size(), 1);
        REQUIRE(result.state == OperationAuditRunReadState::Truncated);
        REQUIRE(result.verified()); REQUIRE(result.run);
        REQUIRE(result.run->events.empty());
        REQUIRE(result.run->repositories.empty());
        REQUIRE(result.run->evidence.empty());
        REQUIRE(result.run->retainedPreviewBytes == 0);
    }
}

TEST_CASE("KG-TSK-0130 audit reader retains event-to-commit correlation",
          "[Unit][Audit][Reader][KG-TSK-0130]") {
    Fixture fixture;
    InitialiseGitRepository(fixture.root);
    fixture.Finalize();

    const auto result = ReadOperationAuditRun(fixture.spec, "reader-run", 3);
    REQUIRE(result.verified()); REQUIRE(result.run);
    REQUIRE_FALSE(result.run->events.empty());
    for (const auto& event : result.run->events) {
        // The reader is an audit API: event ordering alone is insufficient to
        // correlate a phase to its durable repository transition.
        REQUIRE_FALSE(event.eventId.empty());
        REQUIRE_FALSE(event.repositoryId.empty());
        REQUIRE(event.beforeHeadSha); REQUIRE(event.afterHeadSha);
        const auto sha = [](const std::string& value) {
            return (value.size() == 40 || value.size() == 64) &&
                std::all_of(value.begin(), value.end(), [](const char ch) {
                    return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
                });
        };
        REQUIRE(sha(*event.beforeHeadSha));
        REQUIRE(sha(*event.afterHeadSha));
    }
}

TEST_CASE("KG-TSK-0130 audit reader uses an exact retained payload metric",
          "[Unit][Audit][Reader][KG-TSK-0130]") {
    Fixture fixture;
    fixture.Finalize();
    const auto full = ReadOperationAuditRun(fixture.spec, "reader-run", 3);
    REQUIRE(full.verified()); REQUIRE(full.run);
    const auto fullBytes = PayloadBytes(*full.run);
    REQUIRE(full.run->retainedPreviewBytes == fullBytes);

    // Quotes and backslashes are legal content-type payload. This exercises
    // the distinction between retained UTF-8 bytes and larger JSON-escaped bytes.
    auto events = kano::git::audit::ParseAuditEventsJsonl(ReadText(fixture.paths.events));
    auto receipt = kano::git::audit::ParseRunReceiptJson(ReadText(fixture.paths.receipt));
    REQUIRE(events.ok()); REQUIRE(receipt.ok()); REQUIRE(receipt.value);
    const auto artifact = std::find_if(receipt.value->artifacts.begin(),
                                       receipt.value->artifacts.end(),
        [](const auto& item) { return item.kind == "frozen-reader-evidence"; });
    REQUIRE(artifact != receipt.value->artifacts.end());
    const auto artifactId = artifact->id;
    artifact->contentType = "application/quote-\\\"-slash-\\\\";
    for (auto& event : events.values) {
        for (auto& item : event.artifacts) {
            if (item.id == artifactId) item.contentType = artifact->contentType;
        }
    }
    const auto serializedEvents = kano::git::audit::SerializeAuditEventsJsonl(events.values);
    REQUIRE(serializedEvents.ok());
    receipt.value->eventStreamSha256 = kano::git::audit::Sha256Hex(serializedEvents.json);
    const auto serializedReceipt = kano::git::audit::SerializeRunReceiptJson(*receipt.value);
    REQUIRE(serializedReceipt.ok());
    REQUIRE(kano::git::audit::ValidateRunTrace(*receipt.value, events.values).ok());
    WriteText(fixture.paths.events, serializedEvents.json);
    WriteText(fixture.paths.receipt, serializedReceipt.json);

    const auto escaped = ReadOperationAuditRun(fixture.spec, "reader-run", 3);
    REQUIRE(escaped.verified()); REQUIRE(escaped.run);
    const auto exactBytes = PayloadBytes(*escaped.run);
    REQUIRE(escaped.run->retainedPreviewBytes == exactBytes);

    auto limits = OperationAuditRunReadLimits{};
    limits.maxPreviewBytes = exactBytes;
    const auto exact = ReadOperationAuditRun(fixture.spec, "reader-run", 3, limits);
    REQUIRE(exact.verified()); REQUIRE(exact.run);
    REQUIRE(exact.run->retainedPreviewBytes == exactBytes);
    REQUIRE_FALSE(exact.run->previewTruncated);

    REQUIRE(exactBytes > 0);
    limits.maxPreviewBytes = exactBytes - 1;
    const auto oneLess = ReadOperationAuditRun(fixture.spec, "reader-run", 3, limits);
    REQUIRE(oneLess.verified()); REQUIRE(oneLess.run);
    REQUIRE(oneLess.state == OperationAuditRunReadState::Truncated);
    REQUIRE(oneLess.run->previewTruncated);
    REQUIRE(oneLess.run->retainedPreviewBytes == PayloadBytes(*oneLess.run));
    REQUIRE(oneLess.run->retainedPreviewBytes <= limits.maxPreviewBytes);
}

TEST_CASE("KG-TSK-0130 audit reader bounds inputs, diagnostics, and receipt identity",
          "[Unit][Audit][Reader][KG-TSK-0130]") {
    Fixture fixture;

    SECTION("diagnostics permit zero and exact caps without raw paths") {
        auto limits = OperationAuditRunReadLimits{};
        limits.maxDiagnosticBytes = 4;
        auto bounded = ReadOperationAuditRun(fixture.spec, "reader-run", 3, limits);
        REQUIRE(bounded.diagnostic.size() == 4);
        REQUIRE(bounded.diagnosticTruncated);
        REQUIRE(bounded.diagnostic.find(fixture.root.string()) == std::string::npos);

        limits.maxDiagnosticBytes = 0;
        auto empty = ReadOperationAuditRun(fixture.spec, "reader-run", 3, limits);
        REQUIRE(empty.diagnostic.empty());
        REQUIRE(empty.diagnosticTruncated);
    }

    SECTION("configured byte ceilings accept exact and plus-one but reject one-less") {
        fixture.Finalize();
        const auto eventBytes = ReadText(fixture.paths.events).size();
        const auto receiptBytes = ReadText(fixture.paths.receipt).size();
        const auto frozenBytes = ReadText(fixture.paths.frozenInput).size();

        auto limits = OperationAuditRunReadLimits{};
        limits.maxEventStreamBytes = eventBytes;
        limits.maxInputBytes = std::max(receiptBytes, frozenBytes);
        REQUIRE(ReadOperationAuditRun(fixture.spec, "reader-run", 3, limits).verified());

        ++limits.maxEventStreamBytes;
        ++limits.maxInputBytes;
        REQUIRE(ReadOperationAuditRun(fixture.spec, "reader-run", 3, limits).verified());

        limits.maxEventStreamBytes = eventBytes - 1;
        const auto truncated = ReadOperationAuditRun(
            fixture.spec, "reader-run", 3, limits);
        RequireState(truncated, OperationAuditRunReadState::Truncated,
                     OperationAuditRunReadCode::InputLimit);
        REQUIRE_FALSE(truncated.run);

        limits.maxEventStreamBytes = eventBytes;
        limits.maxInputBytes = std::max(receiptBytes, frozenBytes) - 1;
        const auto inputTruncated = ReadOperationAuditRun(
            fixture.spec, "reader-run", 3, limits);
        RequireState(inputTruncated, OperationAuditRunReadState::Truncated,
                     OperationAuditRunReadCode::InputLimit);
        REQUIRE_FALSE(inputTruncated.run);
    }

    SECTION("content-addressed receipt identity is enforced when supplied") {
        fixture.Finalize();
        const auto initial = ReadOperationAuditRun(fixture.spec, "reader-run", 3);
        REQUIRE(initial.verified()); REQUIRE(initial.run);
        const auto receiptId = initial.run->receiptId;
        const auto matched = ReadOperationAuditRun(
            fixture.spec, "reader-run", 3, {},
            std::optional<std::string_view>{receiptId});
        REQUIRE(matched.verified());

        const std::string otherId(64, '0');
        const auto mismatched = ReadOperationAuditRun(
            fixture.spec, "reader-run", 3, {},
            std::optional<std::string_view>{otherId});
        RequireState(mismatched, OperationAuditRunReadState::Corrupt,
                     OperationAuditRunReadCode::ReceiptMismatch);

        const auto invalid = ReadOperationAuditRun(
            fixture.spec, "reader-run", 3, {},
            std::optional<std::string_view>{"not-a-sha256"});
        RequireState(invalid, OperationAuditRunReadState::Invalid,
                     OperationAuditRunReadCode::InvalidIdentity);
    }

    SECTION("invalid lookup identity and oversized caller configuration are rejected before reads") {
        const auto badRun = ReadOperationAuditRun(fixture.spec, "bad run id", 3);
        RequireState(badRun, OperationAuditRunReadState::Invalid,
                     OperationAuditRunReadCode::InvalidIdentity);
        const auto zeroAttempt = ReadOperationAuditRun(fixture.spec, "reader-run", 0);
        RequireState(zeroAttempt, OperationAuditRunReadState::Invalid,
                     OperationAuditRunReadCode::InvalidIdentity);

        auto oversized = fixture.spec;
        oversized.sourceBytes.assign((4U << 20U) + 1U, 'x');
        const auto rejected = ReadOperationAuditRun(oversized, "reader-run", 3);
        RequireState(rejected, OperationAuditRunReadState::Invalid,
                     OperationAuditRunReadCode::InvalidConfiguration);
    }

    SECTION("independent read policies enforce exact boundaries") {
        fixture.Finalize();

        auto noParent = fixture.spec;
        noParent.correlation.parentRunId.clear();
        const auto parentMismatch = ReadOperationAuditRun(
            noParent, "reader-run", 3);
        RequireState(parentMismatch, OperationAuditRunReadState::Corrupt,
                     OperationAuditRunReadCode::CorrelationMismatch);

        auto otherRoute = fixture.spec;
        otherRoute.route = "commit.plan";
        const auto exactRoute = ReadOperationAuditRun(
            otherRoute, "reader-run", 3);
        RequireState(exactRoute, OperationAuditRunReadState::Corrupt,
                     OperationAuditRunReadCode::FrozenBindingMissing);

        OperationAuditRunReadPolicy compatible;
        compatible.routeBinding =
            OperationAuditRouteBindingPolicy::CompatibleInputKind;
        REQUIRE(ReadOperationAuditRun(
            otherRoute, "reader-run", 3, {}, std::nullopt,
            compatible).verified());

        auto invalidPolicy = OperationAuditRunReadPolicy{};
        invalidPolicy.markerMatch =
            static_cast<OperationAuditMarkerMatchPolicy>(255);
        const auto invalid = ReadOperationAuditRun(
            fixture.spec, "reader-run", 3, {}, std::nullopt,
            invalidPolicy);
        RequireState(invalid, OperationAuditRunReadState::Invalid,
                     OperationAuditRunReadCode::InvalidConfiguration);
    }
}

TEST_CASE("KG-TSK-0130 audit reader preserves every terminal outcome",
          "[Unit][Audit][Reader][KG-TSK-0130]") {
    Fixture fixture;
    fixture.Finalize();
    const auto parsed = kano::git::audit::ParseRunReceiptJson(
        ReadText(fixture.paths.receipt));
    REQUIRE(parsed.ok()); REQUIRE(parsed.value);
    const std::vector<kano::git::audit::OutcomeState> states = {
        kano::git::audit::OutcomeState::Succeeded,
        kano::git::audit::OutcomeState::Failed,
        kano::git::audit::OutcomeState::Partial,
        kano::git::audit::OutcomeState::Blocked,
        kano::git::audit::OutcomeState::Cancelled,
        kano::git::audit::OutcomeState::TimedOut,
        kano::git::audit::OutcomeState::Unknown,
    };
    for (const auto state : states) {
        auto receipt = *parsed.value;
        receipt.terminalOutcome.status = state;
        receipt.terminalOutcome.retryable = false;
        if (state == kano::git::audit::OutcomeState::Succeeded) {
            receipt.terminalOutcome.exitCode = 0;
            receipt.terminalOutcome.reasonCode.reset();
        } else {
            receipt.terminalOutcome.exitCode = 17;
            receipt.terminalOutcome.reasonCode = "test-outcome";
        }
        const auto serialized = kano::git::audit::SerializeRunReceiptJson(receipt);
        REQUIRE(serialized.ok());
        WriteText(fixture.paths.receipt, serialized.json);
        const auto result = ReadOperationAuditRun(fixture.spec, "reader-run", 3);
        INFO(kano::git::audit::OutcomeStateName(state));
        REQUIRE(result.verified()); REQUIRE(result.run);
        REQUIRE(result.run->terminalOutcome.status == state);
    }
}

TEST_CASE("KG-TSK-0130 audit reader keeps redaction explicit",
          "[Unit][Audit][Reader][KG-TSK-0130]") {
    Fixture fixture;
    fixture.Finalize();
    auto events = kano::git::audit::ParseAuditEventsJsonl(
        ReadText(fixture.paths.events));
    auto receipt = kano::git::audit::ParseRunReceiptJson(
        ReadText(fixture.paths.receipt));
    REQUIRE(events.ok()); REQUIRE(receipt.ok()); REQUIRE(receipt.value);

    const auto artifact = std::find_if(
        receipt.value->artifacts.begin(), receipt.value->artifacts.end(),
        [](const auto& item) { return item.kind == "frozen-reader-evidence"; });
    REQUIRE(artifact != receipt.value->artifacts.end());
    const auto artifactId = artifact->id;
    artifact->redactionStatus = kano::git::audit::RedactionStatus::Redacted;
    for (auto& event : events.values) {
        for (auto& item : event.artifacts) {
            if (item.id == artifactId)
                item.redactionStatus = kano::git::audit::RedactionStatus::Redacted;
        }
    }
    const auto serializedEvents = kano::git::audit::SerializeAuditEventsJsonl(
        events.values);
    REQUIRE(serializedEvents.ok());
    receipt.value->eventStreamSha256 = kano::git::audit::Sha256Hex(
        serializedEvents.json);
    const auto serializedReceipt = kano::git::audit::SerializeRunReceiptJson(
        *receipt.value);
    REQUIRE(serializedReceipt.ok());
    REQUIRE(kano::git::audit::ValidateRunTrace(
        *receipt.value, events.values).ok());
    WriteText(fixture.paths.events, serializedEvents.json);
    WriteText(fixture.paths.receipt, serializedReceipt.json);

    const auto result = ReadOperationAuditRun(fixture.spec, "reader-run", 3);
    REQUIRE(result.verified()); REQUIRE(result.run);
    REQUIRE(result.run->hasRedactedEvidence);
    const auto projected = std::find_if(
        result.run->evidence.begin(), result.run->evidence.end(),
        [&](const auto& item) { return item.id == artifactId; });
    REQUIRE(projected != result.run->evidence.end());
    REQUIRE(projected->redactionStatus ==
            kano::git::audit::RedactionStatus::Redacted);
}

TEST_CASE("KG-TSK-0130 audit reader fails closed on evidence mutations",
          "[Unit][Audit][Reader][KG-TSK-0130]") {
    Fixture fixture; fixture.Finalize();

    const auto corrupt = [&](const std::filesystem::path& path, const std::string& bytes,
                             const OperationAuditRunReadCode expected) {
        WriteText(path, bytes);
        const auto result = ReadOperationAuditRun(fixture.spec, "reader-run", 3);
        REQUIRE(result.state == OperationAuditRunReadState::Corrupt);
        REQUIRE(result.code == expected); REQUIRE_FALSE(result.run);
    };

    SECTION("malformed events are corrupt") {
        corrupt(fixture.paths.events, "{not-json}\n", OperationAuditRunReadCode::MalformedEvidence);
    }
    SECTION("tampered frozen input has a typed hash mismatch") {
        corrupt(fixture.paths.frozenInput, fixture.spec.frozenBytes + " ",
                OperationAuditRunReadCode::HashMismatch);
    }
    SECTION("a removed required record is missing, not corrupt") {
        REQUIRE(std::filesystem::remove(fixture.paths.receipt));
        const auto result = ReadOperationAuditRun(
            fixture.spec, "reader-run", 3);
        RequireState(result, OperationAuditRunReadState::Missing,
                     OperationAuditRunReadCode::EvidenceMissing);
        REQUIRE_FALSE(result.run);
    }
    SECTION("a non-regular evidence child fails closed") {
        REQUIRE(std::filesystem::remove(fixture.paths.events));
        REQUIRE(std::filesystem::create_directory(fixture.paths.events));
        const auto result = ReadOperationAuditRun(
            fixture.spec, "reader-run", 3);
        RequireState(result, OperationAuditRunReadState::Corrupt,
                     OperationAuditRunReadCode::NonRegularEvidence);
        REQUIRE_FALSE(result.run);
    }
    SECTION("receipt correlation mismatch is corrupt") {
        auto receipt = nlohmann::json::parse(ReadText(fixture.paths.receipt));
        receipt["correlation"]["productId"] = "wrong";
        corrupt(fixture.paths.receipt, receipt.dump(),
                OperationAuditRunReadCode::TraceInvalid);
    }
    SECTION("unsupported evidence schema is incompatible") {
        auto receipt = nlohmann::json::parse(ReadText(fixture.paths.receipt));
        receipt["schemaVersion"] = 99;
        WriteText(fixture.paths.receipt, receipt.dump());
        const auto result = ReadOperationAuditRun(fixture.spec, "reader-run", 3);
        REQUIRE(result.state == OperationAuditRunReadState::Incompatible);
        REQUIRE(result.code == OperationAuditRunReadCode::UnsupportedSchema); REQUIRE_FALSE(result.run);
        bool traceValid = true;
        std::string error;
        REQUIRE_FALSE(VerifyOperationAuditJson(
            fixture.spec, "reader-run", 3, &traceValid, &error));
        REQUIRE_FALSE(traceValid);
        REQUIRE(error == "audit evidence failed closed trace validation");
    }
    SECTION("out-of-order event records are rejected") {
        const auto bytes = ReadText(fixture.paths.events);
        std::vector<std::string> lines;
        std::size_t cursor = 0;
        while (cursor < bytes.size()) {
            const auto end = bytes.find('\n', cursor);
            REQUIRE(end != std::string::npos);
            lines.push_back(bytes.substr(cursor, end - cursor));
            cursor = end + 1;
        }
        REQUIRE(lines.size() > 1);
        std::swap(lines[0], lines[1]);
        std::string reordered;
        for (const auto& line : lines) reordered += line + '\n';
        corrupt(fixture.paths.events, reordered,
                OperationAuditRunReadCode::MalformedEvidence);
    }
    SECTION("duplicate event records are rejected") {
        const auto bytes = ReadText(fixture.paths.events);
        const auto newline = bytes.find('\n');
        REQUIRE(newline != std::string::npos);
        const auto duplicate = bytes.substr(0, newline + 1) + bytes;
        corrupt(fixture.paths.events, duplicate,
                OperationAuditRunReadCode::MalformedEvidence);
    }
}

TEST_CASE("KG-TSK-0130 legacy audit verify keeps exact error diagnostics",
          "[Unit][Audit][Reader][KG-TSK-0130]") {
    const auto verifyError = [](const OperationAuditSpec& spec) {
        bool traceValid = true;
        std::string error;
        const auto value = VerifyOperationAuditJson(spec, "reader-run", 3,
                                                    &traceValid, &error);
        REQUIRE_FALSE(value); REQUIRE_FALSE(traceValid);
        return error;
    };

    SECTION("legacy markers retain identity-only diagnostics despite extra semantic fields") {
        Fixture fixture;
        REQUIRE(std::filesystem::create_directories(fixture.paths.attemptRoot));
        auto pending = PendingMarker(fixture); pending["reservedAtUtc"] = "invalid";
        WriteText(fixture.paths.publicationPending, pending.dump());
        REQUIRE(verifyError(fixture.spec) == "audit receipt publication is still pending");
        REQUIRE(std::filesystem::remove(fixture.paths.publicationPending));
        auto incomplete = IncompleteMarker(fixture); incomplete["recordedAtUtc"] = "invalid";
        WriteText(fixture.paths.incomplete, incomplete.dump());
        REQUIRE(verifyError(fixture.spec) == "audit evidence is explicitly incomplete");
        REQUIRE(std::filesystem::remove(fixture.paths.incomplete));
        pending = PendingMarker(fixture); pending["runId"] = "other-run";
        WriteText(fixture.paths.publicationPending, pending.dump());
        REQUIRE(verifyError(fixture.spec) == "audit publication-pending sentinel is invalid");
        REQUIRE(std::filesystem::remove(fixture.paths.publicationPending));
        incomplete = IncompleteMarker(fixture); incomplete["schemaVersion"] = 2;
        WriteText(fixture.paths.incomplete, incomplete.dump());
        REQUIRE(verifyError(fixture.spec) == "audit incomplete marker is invalid");
    }

    SECTION("reduced, extended, and legacy marker shapes retain old verification semantics") {
        const auto verify = [&](Fixture& fixture) {
            bool traceValid = true;
            std::string error;
            REQUIRE_FALSE(VerifyOperationAuditJson(
                fixture.spec, "reader-run", 3, &traceValid, &error));
            REQUIRE_FALSE(traceValid);
            return error;
        };
        {
            Fixture fixture;
            REQUIRE(std::filesystem::create_directories(fixture.paths.attemptRoot));
            auto reduced = PendingMarker(fixture);
            reduced.erase("parentRunId"); reduced.erase("planId");
            reduced.erase("planSha256"); reduced.erase("reservedAtUtc");
            WriteText(fixture.paths.publicationPending, reduced.dump());
            REQUIRE(verify(fixture) == "audit receipt publication is still pending");
            const auto strict = ReadOperationAuditRun(fixture.spec, "reader-run", 3);
            RequireState(strict, OperationAuditRunReadState::Corrupt,
                         OperationAuditRunReadCode::PendingMarkerInvalid);
        }
        {
            Fixture fixture;
            REQUIRE(std::filesystem::create_directories(fixture.paths.attemptRoot));
            auto extended = IncompleteMarker(fixture); extended["legacyExtension"] = true;
            WriteText(fixture.paths.incomplete, extended.dump());
            REQUIRE(verify(fixture) == "audit evidence is explicitly incomplete");
            const auto strict = ReadOperationAuditRun(fixture.spec, "reader-run", 3);
            RequireState(strict, OperationAuditRunReadState::Corrupt,
                         OperationAuditRunReadCode::IncompleteMarkerInvalid);
        }
        {
            Fixture fixture;
            REQUIRE(std::filesystem::create_directories(fixture.paths.attemptRoot));
            auto pending = PendingMarker(fixture); pending.erase("reservedAtUtc");
            auto incomplete = IncompleteMarker(fixture); incomplete.erase("recordedAtUtc");
            WriteText(fixture.paths.publicationPending, pending.dump());
            WriteText(fixture.paths.incomplete, incomplete.dump());
            REQUIRE(verify(fixture) == "audit evidence is explicitly incomplete");
        }
    }

    SECTION("missing child evidence keeps the legacy bounded-read diagnostic") {
        Fixture fixture; fixture.Finalize();
        REQUIRE(std::filesystem::remove(fixture.paths.events));
        std::string baseline;
        REQUIRE_FALSE(kano::git::commands::ReadBoundedAuditInput(
            fixture.paths.events, 64U << 20U, &baseline));
        bool traceValid = true;
        std::string error;
        REQUIRE_FALSE(VerifyOperationAuditJson(
            fixture.spec, "reader-run", 3, &traceValid, &error));
        REQUIRE_FALSE(traceValid);
        REQUIRE(error == baseline);
    }

    SECTION("malformed trace, receipt identity, plan, frozen hash and correlation retain legacy messages") {
        Fixture fixture; fixture.Finalize();
        WriteText(fixture.paths.events, "{not-json}\n");
        REQUIRE(verifyError(fixture.spec) == "audit evidence failed closed trace validation");
    }

    SECTION("receipt identity, plan, frozen hash and correlation map exactly") {
        const auto exercise = [&](const auto mutate, const std::string& expected) {
            Fixture fixture; fixture.Finalize();
            auto receipt = nlohmann::json::parse(ReadText(fixture.paths.receipt));
            mutate(receipt);
            WriteText(fixture.paths.receipt, receipt.dump());
            REQUIRE(verifyError(fixture.spec) == expected);
        };
        exercise([](auto& receipt) { receipt["runId"] = "other-run"; },
                 "audit evidence failed closed trace validation");
        exercise([](auto& receipt) { receipt["planId"] = "other-plan"; },
                 "audit evidence failed closed trace validation");
        {
            Fixture fixture; fixture.Finalize();
            RewriteCanonicalTrace(fixture, [](auto& events, auto& receipt) {
                receipt.runId = "other-run";
                for (auto& event : events) event.runId = "other-run";
            });
            REQUIRE(verifyError(fixture.spec) ==
                    "audit receipt identity does not match requested run");
        }
        {
            Fixture fixture; fixture.Finalize();
            RewriteCanonicalTrace(fixture, [](auto& events, auto& receipt) {
                receipt.planId = "other-plan";
                for (auto& event : events) event.planId = "other-plan";
            });
            REQUIRE(verifyError(fixture.spec) ==
                    "current admitted plan id does not match the receipt");
        }
        {
            Fixture fixture; fixture.Finalize();
            WriteText(fixture.paths.frozenInput, fixture.spec.frozenBytes + " ");
            REQUIRE(verifyError(fixture.spec) == "frozen input hash does not match the receipt");
        }
        {
            Fixture fixture; fixture.Finalize();
            auto sourceChanged = fixture.spec;
            sourceChanged.sourceBytes += " ";
            REQUIRE(verifyError(sourceChanged) == "current plan bytes are not an admitted source state");
        }
        {
            Fixture fixture; fixture.Finalize();
            RewriteCanonicalTrace(fixture, [](auto& events, auto& receipt) {
                const auto frozen = std::find_if(receipt.artifacts.begin(), receipt.artifacts.end(),
                    [](const auto& item) { return item.kind.starts_with("audit-frozen-"); });
                REQUIRE(frozen != receipt.artifacts.end());
                auto duplicate = *frozen; duplicate.id = "duplicate-frozen-binding";
                receipt.artifacts.push_back(duplicate);
                REQUIRE_FALSE(events.empty());
                events.back().artifacts.push_back(duplicate);
            });
            REQUIRE(verifyError(fixture.spec) == "multiple main frozen input bindings are present");
        }
        {
            Fixture fixture; fixture.Finalize();
            RewriteCanonicalTrace(fixture, [](auto& events, auto& receipt) {
                const auto frozen = std::find_if(receipt.artifacts.begin(), receipt.artifacts.end(),
                    [](const auto& item) { return item.kind.starts_with("audit-frozen-"); });
                REQUIRE(frozen != receipt.artifacts.end());
                const auto id = frozen->id;
                receipt.artifacts.erase(frozen);
                for (auto& event : events) {
                    std::erase_if(event.artifacts, [&](const auto& artifact) { return artifact.id == id; });
                }
            });
            REQUIRE(verifyError(fixture.spec) == "main frozen route/input binding is missing or contradictory");
        }
    }
}

TEST_CASE("KG-TSK-0130 operation descriptor frozen schema is typed and closed",
          "[Unit][Audit][Reader][KG-TSK-0130]") {
    const auto makeDescriptorFixture = [](Fixture& fixture) {
        nlohmann::json options = {{"abort", false}, {"agentIntentCommitMode", false},
            {"forceWithLease", false}, {"jobs", 1}, {"noVerify", false},
            {"recursive", false}, {"remoteSelectorSha256", nullptr}, {"resume", false},
            {"settleWorktrees", false}};
        std::string error;
        const auto descriptor = BuildOperationDescriptor("converge.repos", options.dump(),
                                                         fixture.spec.correlation, &error);
        INFO(error); REQUIRE(descriptor);
        fixture.spec.inputKind = "operation-descriptor";
        fixture.spec.route = "converge.repos";
        fixture.spec.planId = "operation-reader";
        fixture.spec.inputIdentity = "operation-reader-input";
        fixture.spec.sourcePath.reset(); fixture.spec.sourceBytes = *descriptor;
        fixture.spec.frozenBytes = *descriptor; fixture.spec.frozenFileName = "frozen-operation.json";
        const auto paths = ResolveOperationAuditPaths(fixture.spec, "reader-run", 3, &error);
        INFO(error); REQUIRE(paths); fixture.paths = *paths;
    };
    const auto rewriteFrozen = [](Fixture& fixture, const auto mutate) {
        auto doc = nlohmann::json::parse(fixture.spec.frozenBytes);
        mutate(doc);
        const auto frozen = doc.dump() + '\n';
        fixture.spec.frozenBytes = frozen;
        WriteText(fixture.paths.frozenInput, frozen);
        const auto sha = kano::git::audit::Sha256Hex(frozen);
        RewriteCanonicalTrace(fixture, [&](auto& events, auto& receipt) {
            receipt.planSha256 = sha;
            for (auto& event : events) {
                event.planSha256 = sha;
                for (auto& artifact : event.artifacts) if (artifact.kind.starts_with("audit-frozen-")) {
                    artifact.sha256 = sha; artifact.sizeBytes = frozen.size();
                }
            }
            for (auto& artifact : receipt.artifacts) if (artifact.kind.starts_with("audit-frozen-")) {
                artifact.sha256 = sha; artifact.sizeBytes = frozen.size();
            }
        });
    };

    SECTION("well-formed but unsupported schema is incompatible") {
        Fixture fixture; makeDescriptorFixture(fixture); fixture.Finalize();
        rewriteFrozen(fixture, [](auto& doc) { doc["schema_version"] = 2; });
        const auto result = ReadOperationAuditRun(fixture.spec, "reader-run", 3);
        RequireState(result, OperationAuditRunReadState::Incompatible,
                     OperationAuditRunReadCode::FrozenOperationUnsupportedSchema);
        bool traceValid = true;
        std::string error;
        REQUIRE_FALSE(VerifyOperationAuditJson(
            fixture.spec, "reader-run", 3, &traceValid, &error));
        REQUIRE_FALSE(traceValid);
        REQUIRE(error == "frozen operation descriptor identity is contradictory");
    }
    SECTION("missing or wrong-type schema is corrupt operation identity") {
        Fixture fixture; makeDescriptorFixture(fixture); fixture.Finalize();
        rewriteFrozen(fixture, [](auto& doc) { doc.erase("schema_name"); });
        const auto missing = ReadOperationAuditRun(fixture.spec, "reader-run", 3);
        RequireState(missing, OperationAuditRunReadState::Corrupt,
                     OperationAuditRunReadCode::FrozenOperationIdentityMismatch);
    }
    SECTION("zero-event crash verifies a typed operation descriptor route") {
        Fixture fixture; makeDescriptorFixture(fixture); fixture.Finalize();
        RewriteZeroEventCrash(fixture);
        const auto result = ReadOperationAuditRun(fixture.spec, "reader-run", 3);
        RequireState(result, OperationAuditRunReadState::Ready,
                     OperationAuditRunReadCode::None);
        REQUIRE(result.run); REQUIRE(result.run->totalEventRecords == 0);
    }
}
