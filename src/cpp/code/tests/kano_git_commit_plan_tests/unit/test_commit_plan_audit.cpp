#include <catch2/catch_test_macros.hpp>

#include "commit_plan_audit.hpp"
#include "plan_utils.hpp"

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <fstream>
#include <random>
#include <thread>

#if defined(_WIN32)
#include <cstdlib>
#endif

using namespace kano::git::commands;

namespace {

auto ReadText(const std::filesystem::path& InPath) -> std::string {
    std::ifstream input(InPath, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input), {});
}

auto RunRoot(const std::filesystem::path& InPlanPath,
             const std::string& InRunId,
             const std::uint32_t InAttempt) -> std::filesystem::path {
    return InPlanPath.parent_path() / (InPlanPath.filename().string() + ".audit") /
        ("run-" + kano::git::audit::Sha256Hex(InRunId)) /
        ("attempt-" + std::to_string(InAttempt));
}

auto UniqueRoot(const std::string& InLabel) -> std::filesystem::path {
    static std::atomic<std::uint64_t> sequence = 0;
    std::random_device entropy;
    const auto nonce = std::to_string(std::chrono::high_resolution_clock::now().time_since_epoch().count()) +
        "-" + std::to_string(sequence.fetch_add(1)) + "-" + std::to_string(entropy());
    return std::filesystem::temp_directory_path() /
        ("kog-plan-audit-" + InLabel + "-" + kano::git::audit::Sha256Hex(nonce).substr(0, 20));
}

auto KoaPlan(std::string InRunId, const std::uint32_t InAttempt = 2) -> CommitPlanPayload {
    CommitPlanPayload plan;
    plan.meta.planId = "plan-test";
    auto& correlation = plan.meta.correlation;
    correlation.present = true; correlation.mode = "koa"; correlation.productId = "koa";
    correlation.topicId = "topic"; correlation.itemId = "item"; correlation.workOrderId = "work";
    correlation.requestId = "request"; correlation.runId = std::move(InRunId);
    correlation.parentRunId = "parent"; correlation.producerId = "producer";
    correlation.routeId = "route"; correlation.attempt = InAttempt;
    return plan;
}

auto StandalonePlan() -> CommitPlanPayload {
    CommitPlanPayload plan;
    plan.meta.planId = "plan-standalone";
    plan.meta.correlation.present = true;
    plan.meta.correlation.mode = "standalone";
    plan.meta.correlation.attempt = 1;
    return plan;
}

auto WritePlan(const std::filesystem::path& InPath, const CommitPlanPayload& InPlan) -> void {
    const auto nullable = [](const std::string& value) -> nlohmann::json {
        return value.empty() ? nlohmann::json(nullptr) : nlohmann::json(value);
    };
    const auto& correlation = InPlan.meta.correlation;
    nlohmann::json doc = {
        {"meta", {
            {"plan_id", InPlan.meta.planId},
            {"executed_at_utc", InPlan.meta.executedAtUtc},
            {"correlation", {
                {"mode", correlation.mode},
                {"product_id", nullable(correlation.productId)},
                {"topic_id", nullable(correlation.topicId)},
                {"item_id", nullable(correlation.itemId)},
                {"work_order_id", nullable(correlation.workOrderId)},
                {"request_id", nullable(correlation.requestId)},
                {"run_id", nullable(correlation.runId)},
                {"parent_run_id", nullable(correlation.parentRunId)},
                {"producer_id", nullable(correlation.producerId)},
                {"route_id", nullable(correlation.routeId)},
                {"attempt", correlation.attempt},
            }},
        }},
        {"stages", {{"commit", nlohmann::json::array()}, {"post_sync", nlohmann::json::array()}}},
    };
    std::ofstream out(InPath, std::ios::binary);
    out << doc.dump(2) << '\n';
}

auto SetAuditTestMode(const bool InEnabled) -> void {
#if defined(_WIN32)
    _putenv_s("KOG_TEST_MODE", InEnabled ? "1" : "");
#else
    if (InEnabled) {
        setenv("KOG_TEST_MODE", "1", 1);
    } else {
        unsetenv("KOG_TEST_MODE");
    }
#endif
}

auto SetPostPublishSyncFault(const bool InEnabled) -> void {
    if (InEnabled) SetAuditTestMode(true);
#if defined(_WIN32)
    _putenv_s("KOG_TEST_ONLY_AUDIT_FAIL_POST_PUBLISH_DIR_SYNC",
              InEnabled ? "1" : "");
#else
    if (InEnabled) {
        setenv("KOG_TEST_ONLY_AUDIT_FAIL_POST_PUBLISH_DIR_SYNC", "1", 1);
    } else {
        unsetenv("KOG_TEST_ONLY_AUDIT_FAIL_POST_PUBLISH_DIR_SYNC");
    }
#endif
    if (!InEnabled) SetAuditTestMode(false);
}

auto SetPostPublishDelay(const int InMillis) -> void {
    const auto value = InMillis > 0 ? std::to_string(InMillis) : std::string{};
    if (InMillis > 0) SetAuditTestMode(true);
#if defined(_WIN32)
    _putenv_s("KOG_TEST_ONLY_AUDIT_POST_PUBLISH_DELAY_MS", value.c_str());
#else
    if (InMillis > 0) {
        setenv("KOG_TEST_ONLY_AUDIT_POST_PUBLISH_DELAY_MS", value.c_str(), 1);
    } else {
        unsetenv("KOG_TEST_ONLY_AUDIT_POST_PUBLISH_DELAY_MS");
    }
#endif
    if (InMillis <= 0) SetAuditTestMode(false);
}

auto BuildVerifySpec(const std::filesystem::path& InRoot,
                     const std::filesystem::path& InPlanPath,
                     const CommitPlanPayload& InPlan) -> OperationAuditSpec {
    OperationAuditSpec spec;
    spec.workspaceRoot = InRoot;
    spec.sourcePath = InPlanPath;
    spec.inputIdentity = InPlanPath.generic_string();
    spec.inputKind = "commit-plan";
    spec.route = "commit-push.plan";
    spec.planId = InPlan.meta.planId;
    spec.sourceBytes = ReadText(InPlanPath);
    spec.frozenBytes = spec.sourceBytes;
    spec.frozenFileName = "frozen-plan.json";
    spec.correlation.mode = "koa";
    spec.correlation.productId = "koa";
    spec.correlation.topicId = "topic";
    spec.correlation.itemId = "item";
    spec.correlation.workOrderId = "work";
    spec.correlation.requestId = "request";
    spec.correlation.runId = InPlan.meta.correlation.runId;
    spec.correlation.parentRunId = "parent";
    spec.correlation.producerId = "producer";
    spec.correlation.routeId = "route";
    spec.correlation.attempt = InPlan.meta.correlation.attempt;
    return spec;
}

} // namespace

TEST_CASE("KG-TSK-0125 new KOA correlation envelopes reject legacy opaque IDs",
          "[Unit][CommitPlan][Audit][Correlation][Compatibility]") {
    const std::string legacyOpaque =
        "legacy/" + std::string(140, 'a') + "#id";
    REQUIRE(legacyOpaque.size() > 128);
    REQUIRE_FALSE(kano::git::audit::IsStableAuditId(legacyOpaque));

    const nlohmann::json envelope = {
        {"mode", "koa"}, {"product_id", legacyOpaque},
        {"topic_id", "topic"}, {"item_id", "item"},
        {"work_order_id", "work"}, {"request_id", "request"},
        {"run_id", "run"}, {"parent_run_id", nullptr},
        {"producer_id", "producer"}, {"route_id", "route"},
        {"attempt", 1},
    };
    std::string error;
    REQUIRE_FALSE(ParseOperationCorrelationEnvelope(envelope.dump(), &error));
    REQUIRE(error.find("stable-ID grammar") != std::string::npos);
}

TEST_CASE("KG-TSK-0125 capability is an exact seven-pair closed reservation set",
          "[Unit][CommitPlan][Audit][Capability]") {
    const std::vector<std::pair<std::string, std::string>> supported = {
        {"commit.plan", "commit-plan"},
        {"commit-push.plan", "commit-plan"},
        {"plan.apply", "commit-plan"},
        {"converge.repos", "operation-descriptor"},
        {"converge.branches.apply", "operation-descriptor"},
        {"converge.branches.recover", "operation-descriptor"},
        {"converge.branches.retire", "operation-descriptor"},
    };
    const auto capability = nlohmann::json::parse(OperationAuditCapabilityJson());
    REQUIRE(capability.size() == 10);
    REQUIRE(capability.at("provenanceGrantsAuthority") == false);
    REQUIRE(capability.at("supportedInputs").size() == supported.size());
    for (std::size_t index = 0; index < supported.size(); ++index) {
        REQUIRE(capability.at("supportedInputs").at(index).at("route") ==
                supported[index].first);
        REQUIRE(capability.at("supportedInputs").at(index).at("inputKind") ==
                supported[index].second);

        const auto root = UniqueRoot("capability-" + std::to_string(index));
        REQUIRE(std::filesystem::create_directories(root));
        OperationAuditSpec spec;
        spec.workspaceRoot = root;
        spec.inputKind = supported[index].second;
        spec.route = supported[index].first;
        spec.planId = "plan-capability-" + std::to_string(index + 1);
        spec.inputIdentity = "input-capability-" + std::to_string(index + 1);
        spec.sourceBytes = "{\"capability\":true}";
        spec.frozenBytes = spec.sourceBytes;
        spec.frozenFileName = spec.inputKind == "commit-plan"
            ? "frozen-plan.json" : "frozen-operation.json";
        if (spec.inputKind == "commit-plan") spec.sourcePath = root / "plan.json";
        spec.correlation.mode = "koa";
        spec.correlation.productId = "product";
        spec.correlation.topicId = "topic";
        spec.correlation.itemId = "item";
        spec.correlation.workOrderId = "work";
        spec.correlation.requestId = "request";
        spec.correlation.runId = "run-capability-" + std::to_string(index + 1);
        spec.correlation.parentRunId = "parent";
        spec.correlation.producerId = "producer";
        spec.correlation.routeId = supported[index].first;
        spec.correlation.attempt = 1;

        std::string error;
        auto audit = OperationAuditContext::Reserve(spec, &error);
        INFO(error);
        REQUIRE(audit);
        const auto before = audit->Capture(root);
        REQUIRE(audit->Append(supported[index].first + ".mutation", root,
                              before, CurrentUtcIso8601(), 0, &error));
        const auto attemptRoot = audit->Paths().attemptRoot;
        REQUIRE(audit->Finalize(0, &error));
        audit.reset();
        const auto events = kano::git::audit::ParseAuditEventsJsonl(
            ReadText(attemptRoot / "events.jsonl"));
        const auto receipt = kano::git::audit::ParseRunReceiptJson(
            ReadText(attemptRoot / "receipt.json"));
        REQUIRE(events.ok());
        REQUIRE(receipt.ok());
        REQUIRE(kano::git::audit::ValidateRunTrace(*receipt.value,
                                                   events.values).ok());
        REQUIRE(events.values.back().action ==
                supported[index].first + ".mutation");
        for (const auto& event : events.values) {
            REQUIRE(event.runId == spec.correlation.runId);
            REQUIRE(event.correlation.routeId == spec.correlation.routeId);
        }

        auto mismatched = spec;
        mismatched.correlation.runId += "-mismatch";
        mismatched.inputKind = spec.inputKind == "commit-plan"
            ? "operation-descriptor" : "commit-plan";
        mismatched.frozenFileName = mismatched.inputKind == "commit-plan"
            ? "frozen-plan.json" : "frozen-operation.json";
        error.clear();
        REQUIRE_FALSE(OperationAuditContext::Reserve(std::move(mismatched),
                                                     &error));
        REQUIRE(error.find("route/input-kind pair") != std::string::npos);

        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }
}

TEST_CASE("KG-TSK-0125 observed audit phases join to one receipt",
          "[Unit][CommitPlan][Audit]") {
    const auto root = UniqueRoot("observed");
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    REQUIRE(std::filesystem::create_directories(root));
    const auto planPath = root / "plan.json";
    auto plan = KoaPlan("run");
    WritePlan(planPath, plan);

    std::string error;
    auto sink = PlanAuditSink::Reserve(root, planPath, &error);
    REQUIRE(sink);
    REQUIRE(ReadText(sink->FrozenPlanPath()) == ReadText(planPath));
    const auto runRoot = RunRoot(planPath, "run", 2);
    REQUIRE(std::filesystem::exists(runRoot / "publication-pending.json"));
    REQUIRE_FALSE(std::filesystem::exists(runRoot / "receipt.json"));
    for (const auto* action : {"plan.stage", "commit.apply", "push"}) {
        const auto startedAtUtc = CurrentUtcIso8601();
        const auto before = sink->Capture(root);
        REQUIRE(sink->Append(action, root, before, startedAtUtc, 0, &error));
    }
    REQUIRE(sink->Finalize(0, &error));
    sink.reset();

    REQUIRE_FALSE(std::filesystem::exists(runRoot / "publication-pending.json"));
    const auto parsedEvents = kano::git::audit::ParseAuditEventsJsonl(ReadText(runRoot / "events.jsonl"));
    REQUIRE(parsedEvents.ok());
    REQUIRE(parsedEvents.values.size() == 4);
    REQUIRE(parsedEvents.values[0].action == "audit.reserve");
    REQUIRE(parsedEvents.values[1].action == "plan.stage");
    REQUIRE(parsedEvents.values[2].action == "commit.apply");
    REQUIRE(parsedEvents.values[3].action == "push");
    for (const auto& event : parsedEvents.values) {
        REQUIRE(event.runId == "run"); REQUIRE(event.parentRunId == "parent");
        REQUIRE(event.attempt == 2); REQUIRE(event.correlation.mode == kano::git::audit::CorrelationMode::Koa);
        REQUIRE(event.correlation.productId == "koa"); REQUIRE(event.correlation.topicId == "topic");
        REQUIRE(event.correlation.itemId == "item"); REQUIRE(event.correlation.workOrderId == "work");
        REQUIRE(event.correlation.requestId == "request"); REQUIRE(event.correlation.producerId == "producer");
        REQUIRE(event.correlation.routeId == "route");
        REQUIRE(event.repository.repositoryId == "workspace");
    }
    const auto parsedReceipt = kano::git::audit::ParseRunReceiptJson(ReadText(runRoot / "receipt.json"));
    REQUIRE(parsedReceipt.ok());
    REQUIRE(parsedReceipt.value->runId == "run"); REQUIRE(parsedReceipt.value->parentRunId == "parent");
    REQUIRE(parsedReceipt.value->attempt == 2); REQUIRE(parsedReceipt.value->correlation.productId == "koa");
    REQUIRE(parsedReceipt.value->correlation.topicId == "topic");
    REQUIRE(parsedReceipt.value->correlation.itemId == "item");
    REQUIRE(parsedReceipt.value->correlation.workOrderId == "work");
    REQUIRE(parsedReceipt.value->correlation.requestId == "request");
    REQUIRE(parsedReceipt.value->correlation.producerId == "producer");
    REQUIRE(parsedReceipt.value->correlation.routeId == "route");
    REQUIRE(kano::git::audit::ValidateRunTrace(*parsedReceipt.value, parsedEvents.values).ok());

    error.clear();
    REQUIRE_FALSE(PlanAuditSink::Reserve(root, planPath, &error));
    REQUIRE(error.find("already exists") != std::string::npos);
    std::filesystem::remove_all(root, ec);
}

TEST_CASE("KG-TSK-0125 reservation terminalizes early failure and retry uses a new attempt",
          "[Unit][CommitPlan][Audit][Failure]") {
    const auto root = UniqueRoot("terminal");
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    REQUIRE(std::filesystem::create_directories(root));
    const auto planPath = root / "plan.json";

    std::string error;
    auto firstPlan = KoaPlan("run-retry", 1);
    WritePlan(planPath, firstPlan);
    {
        auto sink = PlanAuditSink::Reserve(root, planPath, &error);
        REQUIRE(sink);
        REQUIRE_FALSE(std::filesystem::exists(RunRoot(planPath, "run-retry", 1) / "receipt.json"));
    }
    const auto firstRoot = RunRoot(planPath, "run-retry", 1);
    const auto firstReceipt = kano::git::audit::ParseRunReceiptJson(ReadText(firstRoot / "receipt.json"));
    REQUIRE(firstReceipt.ok());
    REQUIRE(firstReceipt.value->terminalOutcome.status == kano::git::audit::OutcomeState::Failed);

    auto retryPlan = KoaPlan("run-retry", 2);
    WritePlan(planPath, retryPlan);
    auto retry = PlanAuditSink::Reserve(root, planPath, &error);
    REQUIRE(retry);
    REQUIRE(retry->Finalize(0, &error));
    retry.reset();
    REQUIRE(std::filesystem::exists(RunRoot(planPath, "run-retry", 2) / "receipt.json"));
    REQUIRE_FALSE(std::filesystem::exists(root / "retry"));
    std::filesystem::remove_all(root, ec);
}

TEST_CASE("KG-TSK-0125 independent standalone executions reserve unique runs",
          "[Unit][CommitPlan][Audit][Standalone]") {
    const auto root = UniqueRoot("standalone");
    REQUIRE(std::filesystem::create_directories(root));
    const auto planPath = root / "plan.json";
    WritePlan(planPath, StandalonePlan());

    std::string error;
    auto first = PlanAuditSink::Reserve(root, planPath, &error);
    REQUIRE(first);
    const auto firstPath = first->FrozenPlanPath();
    const auto firstRun = first->RunId();
    const auto firstFrozen = ParseCommitPlan(firstPath, &error);
    REQUIRE(firstFrozen);
    REQUIRE(firstFrozen->meta.correlation.runId == firstRun);
    REQUIRE(first->Finalize(0, &error));
    first.reset();

    auto second = PlanAuditSink::Reserve(root, planPath, &error);
    REQUIRE(second);
    REQUIRE(firstPath.parent_path() != second->FrozenPlanPath().parent_path());
    REQUIRE(firstRun != second->RunId());
    REQUIRE(second->Finalize(0, &error));
    second.reset();

    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}

TEST_CASE("KG-TSK-0125 legacy missing correlation freezes as standalone without source rewrite",
          "[Unit][CommitPlan][Audit][Standalone][Upgrade]") {
    const auto root = UniqueRoot("legacy-standalone");
    REQUIRE(std::filesystem::create_directories(root));
    const auto planPath = root / "legacy-plan.json";
    WritePlan(planPath, StandalonePlan());
    auto legacy = nlohmann::json::parse(ReadText(planPath));
    legacy.at("meta").erase("correlation");
    {
        std::ofstream out(planPath, std::ios::binary | std::ios::trunc);
        out << legacy.dump(2) << '\n';
    }
    const auto sourceBefore = ReadText(planPath);
    std::string parseError;
    const auto parsedSource = ParseCommitPlan(planPath, &parseError);
    REQUIRE(parsedSource);
    REQUIRE_FALSE(parsedSource->meta.correlation.present);
    REQUIRE(ValidateCommitPlanCorrelation(*parsedSource, &parseError));

    std::string error;
    auto audit = PlanAuditSink::Reserve(root, planPath, &error, "commit.plan");
    INFO(error);
    REQUIRE(audit);
    const auto runId = audit->RunId();
    const auto frozen = ParseCommitPlan(audit->FrozenPlanPath(), &error);
    REQUIRE(frozen);
    REQUIRE(frozen->meta.correlation.present);
    REQUIRE(frozen->meta.correlation.mode == "standalone");
    REQUIRE(frozen->meta.correlation.runId == runId);
    REQUIRE(frozen->meta.correlation.attempt == 1);
    REQUIRE(ReadText(planPath) == sourceBefore);
    REQUIRE(audit->Finalize(0, &error));
    audit.reset();
    REQUIRE(ReadText(planPath) == sourceBefore);

    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}

TEST_CASE("KG-TSK-0125 verification binds admitted stamped bytes and rejects replacement",
          "[Unit][CommitPlan][Audit][Verification]") {
    const auto root = UniqueRoot("verify-admitted-source");
    REQUIRE(std::filesystem::create_directories(root));
    const auto planPath = root / "plan.json";
    const auto plan = KoaPlan("verify-run", 7);
    WritePlan(planPath, plan);

    std::string error;
    auto audit = PlanAuditSink::Reserve(
        root, planPath, &error, "commit-push.plan");
    REQUIRE(audit);
    auto stamped = nlohmann::json::parse(ReadText(planPath));
    stamped["meta"]["executed_at_utc"] = "2026-08-01T00:00:00Z";
    const auto stampedBytes = stamped.dump(2) + '\n';
    {
        std::ofstream out(planPath, std::ios::binary | std::ios::trunc);
        out << stampedBytes;
    }
    REQUIRE(audit->BindSourceStateBytes(stampedBytes, &error));
    const auto before = audit->Capture(root);
    REQUIRE(audit->Append("plan.stamp", root, before, CurrentUtcIso8601(),
                          0, &error));
    REQUIRE(audit->Finalize(0, &error));
    audit.reset();

    OperationAuditSpec verifySpec;
    verifySpec.workspaceRoot = root;
    verifySpec.sourcePath = planPath;
    verifySpec.inputIdentity =
        std::filesystem::weakly_canonical(planPath).generic_string();
    verifySpec.inputKind = "commit-plan";
    verifySpec.route = "commit-push.plan";
    verifySpec.planId = plan.meta.planId;
    verifySpec.sourceBytes = ReadText(planPath);
    verifySpec.frozenBytes = verifySpec.sourceBytes;
    verifySpec.frozenFileName = "frozen-plan.json";
    verifySpec.correlation.mode = "koa";
    verifySpec.correlation.productId = "koa";
    verifySpec.correlation.topicId = "topic";
    verifySpec.correlation.itemId = "item";
    verifySpec.correlation.workOrderId = "work";
    verifySpec.correlation.requestId = "request";
    verifySpec.correlation.runId = "verify-run";
    verifySpec.correlation.parentRunId = "parent";
    verifySpec.correlation.producerId = "producer";
    verifySpec.correlation.routeId = "route";
    verifySpec.correlation.attempt = 7;

    bool traceValid = false;
    auto verified = VerifyOperationAuditJson(
        verifySpec, "verify-run", 7, &traceValid, &error);
    INFO(error);
    REQUIRE(verified);
    REQUIRE(traceValid);
    const auto verifiedJson = nlohmann::json::parse(*verified);
    REQUIRE(verifiedJson.at("ok") == true);
    REQUIRE(verifiedJson.at("receiptSha256").get<std::string>().size() == 64);

    const auto admittedStampedBytes = verifySpec.sourceBytes;
    auto replaced = nlohmann::json::parse(admittedStampedBytes);
    replaced["unbound"] = true;
    verifySpec.sourceBytes = replaced.dump(2) + '\n';
    traceValid = true;
    error.clear();
    REQUIRE_FALSE(VerifyOperationAuditJson(
        verifySpec, "verify-run", 7, &traceValid, &error));
    REQUIRE_FALSE(traceValid);
    REQUIRE(error.find("not an admitted source state") != std::string::npos);

    replaced = nlohmann::json::parse(admittedStampedBytes);
    replaced["meta"]["plan_id"] = "different-plan";
    verifySpec.sourceBytes = replaced.dump(2) + '\n';
    verifySpec.planId = "different-plan";
    traceValid = true;
    error.clear();
    REQUIRE_FALSE(VerifyOperationAuditJson(
        verifySpec, "verify-run", 7, &traceValid, &error));
    REQUIRE_FALSE(traceValid);
    REQUIRE(error.find("plan id") != std::string::npos);

    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}

TEST_CASE("KG-TSK-0125 receipt publication rejects externally changed event bytes",
          "[Unit][CommitPlan][Audit][Tamper]") {
    const auto root = UniqueRoot("tamper");
    REQUIRE(std::filesystem::create_directories(root));
    const auto planPath = root / "plan.json";
    WritePlan(planPath, KoaPlan("tamper-run", 1));

    std::string error;
    auto sink = PlanAuditSink::Reserve(root, planPath, &error);
    REQUIRE(sink);
    const auto attemptRoot = sink->FrozenPlanPath().parent_path();
    { std::ofstream out(attemptRoot / "events.jsonl", std::ios::binary | std::ios::app); out << "{}\n"; }
    REQUIRE_FALSE(sink->Finalize(0, &error));
    REQUIRE(error.find("differs") != std::string::npos);
    REQUIRE_FALSE(std::filesystem::exists(attemptRoot / "receipt.json"));
    REQUIRE(std::filesystem::exists(attemptRoot / "incomplete.json"));
    sink.reset();

    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}

TEST_CASE("KG-TSK-0125 failed audit append permanently blocks receipt publication",
          "[Unit][CommitPlan][Audit][Failure]") {
    const auto root = UniqueRoot("append-failure");
    REQUIRE(std::filesystem::create_directories(root));
    const auto planPath = root / "plan.json";
    WritePlan(planPath, KoaPlan("append-failure-run", 1));

    std::string error;
    auto sink = PlanAuditSink::Reserve(root, planPath, &error);
    REQUIRE(sink);
    const auto attemptRoot = sink->FrozenPlanPath().parent_path();
    const auto before = sink->Capture(root);
    REQUIRE_FALSE(sink->Append("commit.apply", root, before, "not-a-utc-timestamp", 0, &error));
    REQUIRE_FALSE(sink->Finalize(2, &error));
    REQUIRE(error.find("append failure") != std::string::npos);
    sink.reset();
    REQUIRE_FALSE(std::filesystem::exists(attemptRoot / "receipt.json"));
    REQUIRE(std::filesystem::exists(attemptRoot / "incomplete.json"));

    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}

TEST_CASE("KG-TSK-0125 injected post-reservation failure publishes one matching terminal receipt",
          "[Unit][CommitPlan][Audit][Failure]") {
    const auto root = UniqueRoot("injected-terminal");
    REQUIRE(std::filesystem::create_directories(root));
    const auto planPath = root / "plan.json";
    WritePlan(planPath, KoaPlan("injected-terminal-run", 4));

    std::string error;
    auto sink = PlanAuditSink::Reserve(root, planPath, &error);
    REQUIRE(sink);
    const auto attemptRoot = sink->FrozenPlanPath().parent_path();
    REQUIRE(sink->Finalize(73, &error));
    sink.reset();

    const auto receipt = kano::git::audit::ParseRunReceiptJson(
        ReadText(attemptRoot / "receipt.json"));
    REQUIRE(receipt.ok());
    REQUIRE(receipt.value->terminalOutcome.status ==
            kano::git::audit::OutcomeState::Failed);
    REQUIRE(receipt.value->terminalOutcome.exitCode == 73);
    std::size_t receiptCount = 0;
    for (const auto& entry : std::filesystem::directory_iterator(attemptRoot)) {
        if (entry.path().filename() == "receipt.json") ++receiptCount;
    }
    REQUIRE(receiptCount == 1);
    REQUIRE_FALSE(std::filesystem::exists(attemptRoot / "receipt.json.tmp"));
    REQUIRE_FALSE(std::filesystem::exists(attemptRoot / "incomplete.json"));

    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}

TEST_CASE("KG-TSK-0125 nested plan binds supplemental bytes and rejects correlation contradiction",
          "[Unit][CommitPlan][Audit][Nested]") {
    const auto root = UniqueRoot("nested-input");
    REQUIRE(std::filesystem::create_directories(root));
    const auto outerPath = root / "outer.json";
    const auto childPath = root / "child.json";
    const auto badPath = root / "bad.json";
    auto outerPlan = KoaPlan("nested-run", 3);
    outerPlan.meta.planId = "outer-operation";
    auto childPlan = outerPlan;
    childPlan.meta.planId = "child-plan";
    auto badPlan = childPlan;
    badPlan.meta.planId = "contradictory-plan";
    badPlan.meta.correlation.requestId = "different-request";
    WritePlan(outerPath, outerPlan);
    WritePlan(childPath, childPlan);
    WritePlan(badPath, badPlan);

    std::string error;
    auto outer = PlanAuditSink::Reserve(root, outerPath, &error, "plan.apply");
    REQUIRE(outer);
    auto child = PlanAuditSink::Reserve(root, childPath, &error);
    REQUIRE(child);
    REQUIRE(child->PlanId() == "child-plan");
    REQUIRE(child->FrozenPlanPath() != outer->FrozenPlanPath());
    const auto frozenChild = ParseCommitPlan(child->FrozenPlanPath(), &error);
    REQUIRE(frozenChild);
    REQUIRE(frozenChild->meta.planId == "child-plan");
    REQUIRE(frozenChild->meta.correlation.runId == "nested-run");

    error.clear();
    REQUIRE_FALSE(PlanAuditSink::Reserve(root, badPath, &error));
    REQUIRE(error.find("contradicts") != std::string::npos);

    child.reset();
    REQUIRE(outer->Finalize(0, &error));
    const auto attemptRoot = outer->FrozenPlanPath().parent_path();
    outer.reset();
    const auto receipt = kano::git::audit::ParseRunReceiptJson(
        ReadText(attemptRoot / "receipt.json"));
    REQUIRE(receipt.ok());
    REQUIRE(receipt.value->artifacts.size() == 3);
    const auto eventBytes = ReadText(attemptRoot / "events.jsonl");
    const auto events = kano::git::audit::ParseAuditEventsJsonl(eventBytes);
    REQUIRE(events.ok());
    const auto roundTrip =
        kano::git::audit::SerializeAuditEventsJsonl(events.values);
    REQUIRE(roundTrip.ok());
    REQUIRE(roundTrip.json == eventBytes);
    for (const auto& event : events.values) {
        REQUIRE(std::is_sorted(
            event.artifacts.begin(), event.artifacts.end(),
            [](const auto& left, const auto& right) {
                return left.id < right.id;
            }));
    }
    const auto childArtifact = std::find_if(
        receipt.value->artifacts.begin(), receipt.value->artifacts.end(),
        [](const auto& artifact) {
            return artifact.kind == "frozen-commit-plan";
        });
    REQUIRE(childArtifact != receipt.value->artifacts.end());
    REQUIRE(childArtifact->id.starts_with("child-plan-"));
    REQUIRE(childArtifact->sha256 ==
            kano::git::audit::Sha256Hex(ReadText(attemptRoot /
                ("frozen-commit-plan-" +
                 childArtifact->sha256.substr(0, 20) + ".json"))));

    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}

TEST_CASE("KG-TSK-0125 verification rejects a published receipt until pending clears",
          "[Unit][CommitPlan][Audit][Durability][Race]") {
    const auto root = UniqueRoot("publication-pending-race");
    REQUIRE(std::filesystem::create_directories(root));
    const auto planPath = root / "plan.json";
    const auto plan = KoaPlan("pending-race-run", 6);
    WritePlan(planPath, plan);

    std::string reserveError;
    auto sink = PlanAuditSink::Reserve(root, planPath, &reserveError);
    REQUIRE(sink);
    const auto attemptRoot = sink->FrozenPlanPath().parent_path();
    const auto verifySpec = BuildVerifySpec(root, planPath, plan);

    bool finalized = false;
    std::string finalizeError;
    SetPostPublishDelay(1000);
    std::thread finalizer([&]() {
        finalized = sink->Finalize(0, &finalizeError);
    });

    const auto deadline = std::chrono::steady_clock::now() +
        std::chrono::seconds(3);
    while (!std::filesystem::exists(attemptRoot / "receipt.json") &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    const bool receiptObserved =
        std::filesystem::exists(attemptRoot / "receipt.json");
    const bool pendingObserved =
        std::filesystem::exists(attemptRoot / "publication-pending.json");
    bool traceValidWhilePending = true;
    std::string pendingVerifyError;
    const auto verifiedWhilePending = receiptObserved
        ? VerifyOperationAuditJson(verifySpec, "pending-race-run", 6,
                                   &traceValidWhilePending,
                                   &pendingVerifyError)
        : std::optional<std::string>{};

    finalizer.join();
    SetPostPublishDelay(0);
    REQUIRE(receiptObserved);
    REQUIRE(pendingObserved);
    REQUIRE_FALSE(verifiedWhilePending.has_value());
    REQUIRE_FALSE(traceValidWhilePending);
    REQUIRE(pendingVerifyError.find("pending") != std::string::npos);
    INFO(finalizeError);
    REQUIRE(finalized);
    REQUIRE_FALSE(std::filesystem::exists(
        attemptRoot / "publication-pending.json"));

    bool traceValid = false;
    std::string verifyError;
    const auto verified = VerifyOperationAuditJson(
        verifySpec, "pending-race-run", 6, &traceValid, &verifyError);
    INFO(verifyError);
    REQUIRE(verified.has_value());
    REQUIRE(traceValid);
    sink.reset();

    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}

TEST_CASE("KG-TSK-0125 post-publish sync ambiguity is marked and verification fails closed",
          "[Unit][CommitPlan][Audit][Durability]") {
    const auto root = UniqueRoot("post-publish-sync");
    REQUIRE(std::filesystem::create_directories(root));
    const auto planPath = root / "plan.json";
    const auto plan = KoaPlan("durability-run", 5);
    WritePlan(planPath, plan);

    std::string error;
    auto sink = PlanAuditSink::Reserve(root, planPath, &error);
    REQUIRE(sink);
    const auto attemptRoot = sink->FrozenPlanPath().parent_path();
    SetPostPublishSyncFault(true);
    const auto finalized = sink->Finalize(0, &error);
    SetPostPublishSyncFault(false);
    REQUIRE_FALSE(finalized);
    REQUIRE(error.find("post-publication") != std::string::npos);
    sink.reset();
    REQUIRE(std::filesystem::exists(attemptRoot / "receipt.json"));
    REQUIRE(std::filesystem::exists(attemptRoot / "publication-pending.json"));
    REQUIRE(std::filesystem::exists(attemptRoot / "incomplete.json"));
    const auto marker = nlohmann::json::parse(ReadText(attemptRoot / "incomplete.json"));
    REQUIRE(marker.at("receiptPublished") == true);
    REQUIRE(marker.at("reasonCode") == "receipt-durability-uncertain");

    OperationAuditSpec verifySpec;
    verifySpec.workspaceRoot = root;
    verifySpec.sourcePath = planPath;
    verifySpec.inputIdentity = planPath.generic_string();
    verifySpec.inputKind = "commit-plan";
    verifySpec.route = "commit-push.plan";
    verifySpec.planId = plan.meta.planId;
    verifySpec.sourceBytes = ReadText(planPath);
    verifySpec.frozenBytes = verifySpec.sourceBytes;
    verifySpec.frozenFileName = "frozen-plan.json";
    verifySpec.correlation.mode = "koa";
    verifySpec.correlation.productId = "koa";
    verifySpec.correlation.topicId = "topic";
    verifySpec.correlation.itemId = "item";
    verifySpec.correlation.workOrderId = "work";
    verifySpec.correlation.requestId = "request";
    verifySpec.correlation.runId = "durability-run";
    verifySpec.correlation.parentRunId = "parent";
    verifySpec.correlation.producerId = "producer";
    verifySpec.correlation.routeId = "route";
    verifySpec.correlation.attempt = 5;
    bool traceValid = true;
    error.clear();
    REQUIRE_FALSE(VerifyOperationAuditJson(
        verifySpec, "durability-run", 5, &traceValid, &error));
    REQUIRE_FALSE(traceValid);
    REQUIRE(error.find("incomplete") != std::string::npos);

    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}

#if !defined(_WIN32)
TEST_CASE("KG-TSK-0125 audit reservation rejects a symlink sink",
          "[Unit][CommitPlan][Audit][Symlink]") {
    const auto root = UniqueRoot("symlink");
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    REQUIRE(std::filesystem::create_directories(root / "redirect"));
    const auto planPath = root / "plan.json";
    WritePlan(planPath, KoaPlan("run", 1));
    std::filesystem::create_directory_symlink(root / "redirect", root / "plan.json.audit", ec);
    REQUIRE_FALSE(ec);

    std::string error;
    REQUIRE_FALSE(PlanAuditSink::Reserve(root, planPath, &error));
    REQUIRE(error.find("real directory") != std::string::npos);
    REQUIRE(std::filesystem::is_empty(root / "redirect"));
    std::filesystem::remove_all(root, ec);
}
#endif
