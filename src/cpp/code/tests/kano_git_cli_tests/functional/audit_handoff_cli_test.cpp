// Stdout-only audit handoff command contract (KOG-TSK-0137).

#include "functional_test_support.hpp"
#include "operation_audit.hpp"
#include "audit_verification.hpp"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace kano::git::tests::functional {
namespace {

namespace fs = std::filesystem;
using namespace kano::git::commands;

auto RepoRoot() -> fs::path {
    return fs::weakly_canonical(fs::path(KANO_GIT_TEST_REPO_ROOT));
}

auto UniqueRoot() -> fs::path {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    return RepoRoot() / ".kano" / "tmp" /
        ("audit-handoff-cli-" + std::to_string(stamp));
}

auto WriteBytes(const fs::path& InPath, const std::string& InBytes) -> void {
    std::ofstream output(InPath, std::ios::binary | std::ios::trunc);
    REQUIRE(output.good());
    output.write(InBytes.data(), static_cast<std::streamsize>(InBytes.size()));
    REQUIRE(output.good());
}

auto ReadBytes(const fs::path& InPath) -> std::string {
    std::ifstream input(InPath, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input), {});
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

auto MakePlanBytes(const std::string& InRunId) -> std::string {
    const nlohmann::json correlation = {
        {"mode", "koa"}, {"product_id", "product"}, {"topic_id", "topic"},
        {"item_id", "item"}, {"work_order_id", "work-order"},
        {"request_id", "request"}, {"run_id", InRunId},
        {"parent_run_id", "parent"}, {"producer_id", "producer"},
        {"route_id", "route"}, {"attempt", 1},
    };
    return nlohmann::json({
        {"meta", {{"plan_id", "plan-handoff-cli"}, {"correlation", correlation}}},
    }).dump() + '\n';
}

auto PopulateVerifiedRun(const fs::path& InRoot,
                        const std::string& InRunId,
                        const fs::path& InPlanPath,
                        const std::string& InPlanBytes) -> OperationAuditPaths {
    auto git = RunGit({"init"}, InRoot);
    INFO(git.stdoutText); INFO(git.stderrText); REQUIRE(git.exitCode == 0);
    git = RunGit({"config", "user.email", "audit-handoff@example.invalid"}, InRoot);
    REQUIRE(git.exitCode == 0);
    git = RunGit({"config", "user.name", "Audit Handoff Tests"}, InRoot);
    REQUIRE(git.exitCode == 0);
    WriteBytes(InPlanPath, InPlanBytes);
    WriteBytes(InRoot / "tracked.txt", "stable\n");
    git = RunGit({"add", "plan.json", "tracked.txt"}, InRoot);
    REQUIRE(git.exitCode == 0);
    git = RunGit({"commit", "-m", "fixture"}, InRoot);
    REQUIRE(git.exitCode == 0);

    OperationAuditSpec spec;
    spec.workspaceRoot = InRoot;
    spec.sourcePath = InPlanPath;
    spec.inputIdentity = InPlanPath.generic_string();
    spec.inputKind = "commit-plan";
    spec.route = "commit-push.plan";
    spec.planId = "plan-handoff-cli";
    spec.sourceBytes = InPlanBytes;
    spec.frozenBytes = InPlanBytes;
    spec.frozenFileName = "frozen-plan.json";
    spec.correlation.mode = "koa";
    spec.correlation.productId = "product";
    spec.correlation.topicId = "topic";
    spec.correlation.itemId = "item";
    spec.correlation.workOrderId = "work-order";
    spec.correlation.requestId = "request";
    spec.correlation.runId = InRunId;
    spec.correlation.parentRunId = "parent";
    spec.correlation.producerId = "producer";
    spec.correlation.routeId = "route";
    spec.correlation.attempt = 1;

    std::string error;
    auto owner = OperationAuditContext::Reserve(spec, &error);
    INFO(error); REQUIRE(owner);
    const auto before = owner->Capture(InRoot);
    REQUIRE(owner->Append("commit-push.plan.commit", InRoot, before,
                          CurrentUtc(), 0, &error));
    REQUIRE(owner->Finalize(0, &error));
    const auto paths = owner->Paths();
    const auto attemptPaths = paths;
    owner.reset();
    return attemptPaths;
}

auto RunExport(const fs::path& InRoot, const fs::path& InPlan,
               const std::string& InRunId, const std::vector<std::string>& InTail = {})
    -> CommandResult {
    std::vector<std::string> args{
        "audit", "export", "--plan-file", InPlan.string(), "--run-id", InRunId,
        "--attempt", "1", "--json"};
    args.insert(args.end(), InTail.begin(), InTail.end());
    return RunKogWithEnv(args, InRoot,
                         {{"KOG_DEBUG", "0"}, {"KANO_AGENT_MODE", "1"}});
}

} // namespace

TEST_CASE("kog audit export emits a versioned handoff from one verified run",
          "[functional][audit][handoff][KOG-TSK-0137][stdout]") {
    const auto root = UniqueRoot();
    REQUIRE(fs::create_directories(root));
    const auto plan = root / "plan.json";
    const std::string runId = "audit-handoff-cli-run";
    const auto bytes = MakePlanBytes(runId);
    const auto paths = PopulateVerifiedRun(root, runId, plan, bytes);

    const auto first = RunExport(root, plan.filename(), runId);
    INFO(first.stdoutText); INFO(first.stderrText);
    REQUIRE(first.exitCode == 0);
    REQUIRE_FALSE(first.stdoutText.empty());
    const auto doc = nlohmann::json::parse(first.stdoutText);
    REQUIRE(doc["schemaName"] == "kog.auditHandoff");
    REQUIRE(doc["schemaVersion"] == 1);
    REQUIRE(doc["runId"] == runId);
    REQUIRE(doc["attempt"] == 1);
    REQUIRE(doc["receiptSha256"].get<std::string>().size() == 64);
    REQUIRE(doc["eventStreamSha256"].get<std::string>().size() == 64);
    REQUIRE(doc["generationLimits"]["maxSerializedBytes"] == (256U << 10U));
    REQUIRE(doc["repositoryTransitions"].is_array());
    REQUIRE(doc["events"].size() == 2);
    REQUIRE_FALSE(doc.contains("exportedAtUtc"));
    REQUIRE_FALSE(doc.contains("branch"));
    REQUIRE_FALSE(doc.contains("path"));
    REQUIRE(first.stdoutText.find(paths.attemptRoot.generic_string()) == std::string::npos);

    const auto second = RunExport(root, plan.filename(), runId);
    REQUIRE(second.exitCode == 0);
    REQUIRE(second.stdoutText == first.stdoutText);

    std::error_code ec;
    fs::remove_all(root, ec);
}

TEST_CASE("kog audit export applies explicit row and byte caps fail-closed",
          "[functional][audit][handoff][KOG-TSK-0137][caps]") {
    const auto root = UniqueRoot();
    REQUIRE(fs::create_directories(root));
    const auto plan = root / "plan.json";
    const std::string runId = "audit-handoff-cap-run";
    PopulateVerifiedRun(root, runId, plan, MakePlanBytes(runId));

    const auto capped = RunExport(root, plan.filename(), runId,
        {"--max-events", "1", "--max-repositories", "1",
         "--max-evidence-references", "1", "--max-bytes", "4096"});
    INFO(capped.stdoutText); INFO(capped.stderrText);
    REQUIRE(capped.exitCode == 0);
    const auto doc = nlohmann::json::parse(capped.stdoutText);
    REQUIRE(doc["events"].size() <= 1);
    REQUIRE(doc["repositoryTransitions"].size() <= 1);
    REQUIRE(doc["evidenceReferences"].size() <= 1);
    REQUIRE(doc["generationLimits"]["maxSerializedBytes"] == 4096);

    const auto tooSmall = RunExport(root, plan.filename(), runId,
                                   {"--max-bytes", "256"});
    REQUIRE(tooSmall.exitCode != 0);
    REQUIRE(tooSmall.stdoutText.empty());

    std::error_code ec;
    fs::remove_all(root, ec);
}

TEST_CASE("kog audit export rejects adversarial output-path argument",
          "[functional][audit][handoff][KOG-TSK-0137][path-surface]") {
    const auto root = UniqueRoot();
    REQUIRE(fs::create_directories(root));
    const auto existing = root / "existing.json";
    const std::string sentinel = "preserve-existing-target\n";
    WriteBytes(existing, sentinel);
    const auto plan = root / "unused-plan.json";
    const auto result = RunExport(root, plan, "run-adversarial",
                                  {"--output", existing.string()});
    REQUIRE(result.exitCode != 0);
    REQUIRE(ReadBytes(existing) == sentinel);
    std::error_code ec;
    fs::remove_all(root, ec);
}

} // namespace kano::git::tests::functional
