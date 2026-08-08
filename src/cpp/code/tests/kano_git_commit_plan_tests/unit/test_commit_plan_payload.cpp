#include <catch2/catch_test_macros.hpp>

#include "commit_plan_payload.hpp"
#include "plan_utils.hpp"

#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

using namespace kano::git::commands;

namespace {

class TempPlan {
public:
    explicit TempPlan(const std::string& InName) {
        static unsigned counter = 0;
        mRoot = (std::filesystem::temp_directory_path() /
                 "kog-commit-plan-payload-tests" /
                 (InName + "-" + std::to_string(++counter)))
                    .lexically_normal();
        std::error_code ec;
        std::filesystem::remove_all(mRoot, ec);
        ec.clear();
        std::filesystem::create_directories(mRoot, ec);
        REQUIRE_FALSE(ec);
    }

    ~TempPlan() {
        std::error_code ec;
        std::filesystem::remove_all(mRoot, ec);
    }

    auto Write(const std::string& InText, const std::string& InName = "plan.json") const -> std::filesystem::path {
        const auto path = mRoot / InName;
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        REQUIRE(output.good());
        output << InText;
        output.close();
        REQUIRE(output.good());
        return path;
    }

    auto MissingPath() const -> std::filesystem::path {
        return mRoot / "missing.json";
    }

private:
    std::filesystem::path mRoot;
};

auto BuildValidPayload() -> CommitPlanPayload {
    CommitPlanPayload payload;
    payload.meta.planId = "plan-1";
    payload.meta.generatedAtUtc = "2026-07-28T00:00:00Z";
    payload.meta.baseHeadSha = "base-head";
    payload.meta.dirtyFingerprint = "dirty-fingerprint";
    payload.meta.planner.provider = "agent";
    payload.meta.planner.model = "model-1";
    payload.meta.review.verdict = "pass";
    payload.meta.review.reason = "reviewed";
    payload.meta.correlation.present = true;

    RepoCommitPlanEntry commitEntry;
    commitEntry.repoKey = "repo-a";
    RepoCommitPlanEntry::CommitItem commitItem;
    commitItem.message = "feat(payload): characterize parser";
    commitItem.review.verdict = "pass";
    commitItem.review.reason = "reviewed";
    commitEntry.commits.push_back(std::move(commitItem));
    payload.commitEntries.push_back(std::move(commitEntry));

    RepoCommitPlanEntry postSyncEntry;
    postSyncEntry.repoKey = "repo-b";
    RepoCommitPlanEntry::CommitItem postSyncItem;
    postSyncItem.message = "chore(payload): preserve post sync";
    postSyncItem.review.verdict = "pass";
    postSyncItem.review.reason = "reviewed";
    postSyncEntry.commits.push_back(std::move(postSyncItem));
    payload.postSyncEntries.push_back(std::move(postSyncEntry));

    return payload;
}

void RequireValidationError(const CommitPlanPayload& InPayload,
                            const std::string& InExpected) {
    std::string error = "unchanged";
    REQUIRE_FALSE(ValidateCommitPlanForAiMode(InPayload, &error));
    REQUIRE(error == InExpected);
}

} // namespace

TEST_CASE("commit plan payload parser preserves dual-stage metadata and normalization quirks",
          "[Unit][CommitPlan][Payload]") {
    TempPlan temp("dual-stage");
    const auto planPath = temp.Write(R"JSON(
{
  "meta": {
    "schema_version": " 1 ",
    "plan_id": " plan-123 ",
    "generated_at_utc": " 2026-07-28T00:00:00Z ",
    "executed_at_utc": " 2026-07-28T00:01:00Z ",
    "base_head_sha": " base-head ",
    "dirty_fingerprint_pre_ignore": " dirty-before ",
    "dirty_fingerprint": " dirty-after ",
    "freshness_scope": " repo ",
    "scope_repo": " repo\\nested/// ",
    "planner": {
      "provider": " Agent ",
      "ai-model": " chosen-model ",
      "model": " legacy-model ",
      "request_id": " request-7 "
    },
    "review": {
      "verdict": " PASS ",
      "reason": " reviewed "
    }
  },
  "stages": {
    "commit": [
      {
        "repo": "repo\\nested///",
        "commits": [
          {
            "message": "  feat(payload): first\nline  ",
            "review": {
              "verdict": " PASS ",
              "reason": " entry reviewed "
            },
            "include": [" src\\main.cpp ", "broken\\spath", "\n"],
            "exclude": [" docs\\old.md "]
          },
          {
            "include": ["ignored-without-message.txt"]
          }
        ]
      },
      {
        "commits": [
          {
            "message": "ignored-without-repo"
          }
        ]
      }
    ],
    "post_sync": [
      {
        "repo": ".",
        "commits": [
          {
            "message": "chore(payload): post sync",
            "review": {
              "verdict": "pass",
              "reason": "reviewed"
            },
            "include": [],
            "exclude": []
          }
        ]
      }
    ]
  }
}
)JSON");

    std::string error = "unchanged";
    const auto parsed = ParseCommitPlan(planPath, &error);

    REQUIRE(parsed.has_value());
    REQUIRE(error == "unchanged");
    REQUIRE(parsed->meta.schemaVersion == "1");
    REQUIRE(parsed->meta.planId == "plan-123");
    REQUIRE(parsed->meta.generatedAtUtc == "2026-07-28T00:00:00Z");
    REQUIRE(parsed->meta.executedAtUtc == "2026-07-28T00:01:00Z");
    REQUIRE(parsed->meta.baseHeadSha == "base-head");
    REQUIRE(parsed->meta.dirtyFingerprintPreIgnore == "dirty-before");
    REQUIRE(parsed->meta.dirtyFingerprint == "dirty-after");
    REQUIRE(parsed->meta.freshnessScope == "repo");
    REQUIRE(parsed->meta.scopeRepo == "repo/nested");
    REQUIRE(parsed->meta.planner.provider == "Agent");
    REQUIRE(parsed->meta.planner.model == "chosen-model");
    REQUIRE(parsed->meta.planner.requestId == "request-7");
    REQUIRE(parsed->meta.review.verdict == "pass");
    REQUIRE(parsed->meta.review.reason == "reviewed");

    REQUIRE(parsed->commitEntries.size() == 1);
    REQUIRE(parsed->commitEntries[0].repoKey == "repo/nested");
    REQUIRE(parsed->commitEntries[0].commits.size() == 1);
    const auto& commit = parsed->commitEntries[0].commits[0];
    REQUIRE(commit.message == "feat(payload): first line");
    REQUIRE(commit.review.verdict == "pass");
    REQUIRE(commit.review.reason == "entry reviewed");
    REQUIRE(commit.include == std::vector<std::string>{"src/main.cpp", "broken/spath"});
    REQUIRE(commit.exclude == std::vector<std::string>{"docs/old.md"});

    REQUIRE(parsed->postSyncEntries.size() == 1);
    REQUIRE(parsed->postSyncEntries[0].repoKey == ".");
    REQUIRE(parsed->postSyncEntries[0].commits.size() == 1);
    REQUIRE(parsed->postSyncEntries[0].commits[0].message == "chore(payload): post sync");
}

TEST_CASE("commit plan payload parser preserves legacy model fallback",
          "[Unit][CommitPlan][Payload]") {
    TempPlan temp("legacy-model");
    const auto planPath = temp.Write(R"JSON(
{
  "meta": {
    "planner": {
      "provider": "agent",
      "model": "legacy-model"
    }
  },
  "stages": {
    "commit": [
      {
        "repo": ".",
        "commits": [
          {
            "message": "test(payload): legacy model"
          }
        ]
      }
    ]
  }
}
)JSON");

    const auto parsed = ParseCommitPlan(planPath, nullptr);

    REQUIRE(parsed.has_value());
    REQUIRE(parsed->meta.planner.model == "legacy-model");
}

TEST_CASE("commit plan payload parser preserves exact read and structure diagnostics",
          "[Unit][CommitPlan][Payload]") {
    TempPlan temp("parse-errors");

    std::string error;
    REQUIRE_FALSE(ParseCommitPlan(temp.MissingPath(), &error).has_value());
    REQUIRE(error == "cannot read plan file");

    error.clear();
    REQUIRE_FALSE(ParseCommitPlan(temp.Write("", "empty.json"), &error).has_value());
    REQUIRE(error == "plan file is empty");

    error.clear();
    REQUIRE_FALSE(ParseCommitPlan(temp.Write("{}", "missing-stages.json"), &error).has_value());
    REQUIRE(error == "missing \"stages\" object");

    error.clear();
    REQUIRE_FALSE(ParseCommitPlan(
                      temp.Write(R"({"stages":{"commit":[{"repo":".","commits":[{"include":["README.md"]}]}]}})",
                                 "no-valid-entries.json"),
                      &error)
                      .has_value());
    REQUIRE(error == "no valid stage entries found");
}

TEST_CASE("commit plan payload parser accepts an explicit empty push-only stage",
          "[Unit][CommitPlan][Payload][KG-TSK-0125]") {
    TempPlan temp("push-only");
    const auto planPath = temp.Write(R"JSON(
{
  "meta": {"plan_id": "push-only"},
  "stages": {"commit": [], "post_sync": []}
}
)JSON");

    std::string error = "unchanged";
    const auto parsed = ParseCommitPlan(planPath, &error);
    REQUIRE(parsed.has_value());
    REQUIRE(error == "unchanged");
    REQUIRE(parsed->commitEntries.empty());
    REQUIRE(parsed->postSyncEntries.empty());

    error.clear();
    REQUIRE_FALSE(ParseCommitPlan(
        temp.Write(R"JSON({"meta":{},"stages":{"commit":[]}})JSON", "missing-sibling.json"),
        &error));
    REQUIRE(error == "no valid stage entries found");
    error.clear();
    REQUIRE_FALSE(ParseCommitPlan(
        temp.Write(R"JSON({"meta":{},"stages":{"commit":[],"post_sync":{}}})JSON", "malformed-sibling.json"),
        &error));
    REQUIRE(error == "commit and post_sync stages must be arrays");
    error.clear();
    REQUIRE_FALSE(ParseCommitPlan(
        temp.Write(R"JSON({"meta":{},"stages":{"commit":[],"commit":[],"post_sync":[]}})JSON", "duplicate-stage.json"),
        &error));
    REQUIRE(error == "duplicate JSON object field");
}

TEST_CASE("KG-TSK-0125 correlation envelope is closed and fail-closed",
          "[Unit][CommitPlan][Correlation]") {
    const std::string plan = R"({"meta":{},"stages":{"commit":[],"post_sync":[]}})";
    const std::string standalone = R"({"mode":"standalone","product_id":null,"topic_id":null,"item_id":null,"work_order_id":null,"request_id":null,"run_id":null,"parent_run_id":null,"producer_id":null,"route_id":null,"attempt":1})";
    std::string error;
    const auto standalonePlan = ApplyCorrelationEnvelopeToPlan(plan, standalone, &error);
    REQUIRE(standalonePlan.has_value());
    REQUIRE(standalonePlan->find("\"mode\":\"standalone\"") != std::string::npos);
    const std::string koa = R"({"mode":"koa","product_id":"koa","topic_id":null,"item_id":"item","work_order_id":"work","request_id":"request","run_id":"run","parent_run_id":"parent","producer_id":"producer","route_id":"route","attempt":2})";
    REQUIRE(ApplyCorrelationEnvelopeToPlan(plan, koa, &error).has_value());
    const std::string unknown = R"({"mode":"standalone","product_id":null,"topic_id":null,"item_id":null,"work_order_id":null,"request_id":null,"run_id":null,"parent_run_id":null,"producer_id":null,"route_id":null,"attempt":1,"secret":"no"})";
    REQUIRE_FALSE(ApplyCorrelationEnvelopeToPlan(plan, unknown, &error).has_value());
    REQUIRE_FALSE(ApplyCorrelationEnvelopeToPlan(plan, R"({"mode":"koa","product_id":null})", &error).has_value());
    REQUIRE_FALSE(ApplyCorrelationEnvelopeToPlan(
        plan,
        R"({"mode":"standalone","product_id":null,"topic_id":null,"item_id":null,"work_order_id":null,"request_id":null,"run_id":null,"parent_run_id":null,"producer_id":null,"route_id":null,"attempt":1,"attempt":2})",
        &error));
    REQUIRE_FALSE(ApplyCorrelationEnvelopeToPlan(
        plan,
        R"({"mode":"koa","product_id":"koa","topic_id":null,"item_id":"item","work_order_id":"work","request_id":"request","run_id":"run","parent_run_id":"run","producer_id":"producer","route_id":"route","attempt":2})",
        &error));
    REQUIRE_FALSE(ApplyCorrelationEnvelopeToPlan(
        plan,
        R"({"mode":"koa","product_id":"koa","topic_id":null,"item_id":"item","work_order_id":"work","request_id":"request","run_id":"run id","parent_run_id":null,"producer_id":"producer","route_id":"route","attempt":2})",
        &error));

    auto exactlyBounded = standalone;
    exactlyBounded.resize(64U << 10U, ' ');
    REQUIRE(ApplyCorrelationEnvelopeToPlan(plan, exactlyBounded, &error).has_value());
    error.clear();
    REQUIRE_FALSE(ApplyCorrelationEnvelopeToPlan(
        plan, std::string((64U << 10U) + 1U, ' '), &error));
    REQUIRE(error == "correlation envelope exceeds 64 KiB");
}

TEST_CASE("KG-TSK-0125 parser loads attempt and rejects malformed correlation bytes",
          "[Unit][CommitPlan][Correlation][Parser]") {
    TempPlan temp("correlation-parser");
    const std::string valid = R"JSON({
      "meta":{"plan_id":"plan","correlation":{"mode":"koa","product_id":"product","topic_id":"topic","item_id":"item","work_order_id":"work","request_id":"request","run_id":"run","parent_run_id":"parent","producer_id":"producer","route_id":"route","attempt":3}},
      "stages":{"commit":[],"post_sync":[]}
    })JSON";
    std::string error;
    const auto parsed = ParseCommitPlan(temp.Write(valid), &error);
    REQUIRE(parsed);
    REQUIRE(parsed->meta.correlation.present);
    REQUIRE(parsed->meta.correlation.attempt == 3);
    REQUIRE(parsed->meta.correlation.parentRunId == "parent");
    REQUIRE(ValidateCommitPlanCorrelation(*parsed, &error));

    error.clear();
    REQUIRE_FALSE(ParseCommitPlan(temp.Write(valid + " trailing", "trailing.json"), &error));
    REQUIRE(error == "invalid or trailing JSON document");
    error.clear();
    REQUIRE_FALSE(ParseCommitPlan(
        temp.Write(R"JSON({"meta":{"correlation":[]},"stages":{"commit":[],"post_sync":[]}})JSON", "typed.json"),
        &error));
    REQUIRE(error == "meta.correlation must be an object");
    error.clear();
    auto duplicate = valid;
    const auto attempt = duplicate.find("\"attempt\":3");
    REQUIRE(attempt != std::string::npos);
    duplicate.replace(attempt, std::string("\"attempt\":3").size(), "\"attempt\":3,\"attempt\":4");
    REQUIRE_FALSE(ParseCommitPlan(temp.Write(duplicate, "duplicate.json"), &error));
    REQUIRE(error == "duplicate JSON object field");
    error.clear();
    auto escapedDuplicate = valid;
    const auto requestId = escapedDuplicate.find("\"request_id\":\"request\"");
    REQUIRE(requestId != std::string::npos);
    escapedDuplicate.replace(
        requestId,
        std::string("\"request_id\":\"request\"").size(),
        "\"request_id\":\"request\",\"request\\u005fid\":\"other\"");
    REQUIRE_FALSE(ParseCommitPlan(temp.Write(escapedDuplicate, "escaped-duplicate.json"), &error));
    REQUIRE(error == "duplicate JSON object field");
    error.clear();
    auto wrongType = valid;
    const auto wrongAttempt = wrongType.find("\"attempt\":3");
    wrongType.replace(wrongAttempt, std::string("\"attempt\":3").size(), "\"attempt\":\"3\"");
    REQUIRE_FALSE(ParseCommitPlan(temp.Write(wrongType, "wrong-attempt.json"), &error));
    REQUIRE(error == "meta.correlation.attempt must be a positive uint32");
    error.clear();
    auto nonAscii = valid;
    const auto run = nonAscii.find("\"run_id\":\"run\"");
    nonAscii.replace(run, std::string("\"run_id\":\"run\"").size(), "\"run_id\":\"rún\"");
    REQUIRE_FALSE(ParseCommitPlan(temp.Write(nonAscii, "non-ascii.json"), &error));
    REQUIRE(error == "meta.correlation identifiers must be null or stable IDs");

    error.clear();
    const auto nestedOnly = ParseCommitPlan(
        temp.Write(
            R"JSON({"meta":{"review":{"correlation":{"mode":"koa"}}},"stages":{"commit":[],"post_sync":[]}})JSON",
            "nested-only.json"),
        &error);
    REQUIRE(nestedOnly.has_value());
    REQUIRE_FALSE(nestedOnly->meta.correlation.present);
    REQUIRE(ValidateCommitPlanCorrelation(*nestedOnly, &error));
    REQUIRE(error.empty());

    error.clear();
    REQUIRE_FALSE(ParseCommitPlan(
        temp.Write(
            R"JSON({"wrapper":{"stages":{"commit":[],"post_sync":[]}},"meta":{}})JSON",
            "nested-stages.json"),
        &error));
    REQUIRE(error == "missing \"stages\" object");
}

TEST_CASE("commit plan stage aliases and plan keys retain existing defaults",
          "[Unit][CommitPlan][Payload]") {
    REQUIRE(ParseCommitPlanStage("").value() == CommitPlanStage::Commit);
    REQUIRE(ParseCommitPlanStage(" COMMIT ").value() == CommitPlanStage::Commit);
    REQUIRE(ParseCommitPlanStage("post_sync").value() == CommitPlanStage::PostSync);
    REQUIRE(ParseCommitPlanStage("POST-SYNC").value() == CommitPlanStage::PostSync);
    REQUIRE(ParseCommitPlanStage(" both ").value() == CommitPlanStage::Both);
    REQUIRE_FALSE(ParseCommitPlanStage("pre_commit").has_value());

    REQUIRE(PlanStageNeedsPreCommit(CommitPlanStage::Commit));
    REQUIRE_FALSE(PlanStageNeedsPreCommit(CommitPlanStage::PostSync));
    REQUIRE(PlanStageNeedsPreCommit(CommitPlanStage::Both));

    REQUIRE(NormalizePlanKey("") == ".");
    REQUIRE(NormalizePlanKey("  repo\\nested///  ") == "repo/nested");
    REQUIRE(NormalizePlanKey("/") == "/");
}

TEST_CASE("AI commit plan validation preserves exact metadata diagnostics",
          "[Unit][CommitPlan][Payload][Validation]") {
    {
        auto payload = BuildValidPayload();
        payload.meta.planId = "replace-with-plan-id";
        RequireValidationError(payload, "meta.plan_id is missing or placeholder");
    }
    {
        auto payload = BuildValidPayload();
        payload.meta.generatedAtUtc.clear();
        RequireValidationError(payload, "meta.generated_at_utc is missing or placeholder");
    }
    {
        auto payload = BuildValidPayload();
        payload.meta.baseHeadSha.clear();
        RequireValidationError(payload, "meta.base_head_sha is missing or placeholder");
    }
    {
        auto payload = BuildValidPayload();
        payload.meta.dirtyFingerprint.clear();
        RequireValidationError(payload, "meta.dirty_fingerprint is missing or placeholder");
    }
    {
        auto payload = BuildValidPayload();
        payload.meta.planner.provider.clear();
        RequireValidationError(payload, "meta.planner.provider is missing or placeholder");
    }
    {
        auto payload = BuildValidPayload();
        payload.meta.planner.model = "replace-with-ai-model";
        RequireValidationError(payload, "meta.planner.ai-model is missing or placeholder");
    }
    {
        auto payload = BuildValidPayload();
        payload.meta.review.verdict = "fail";
        RequireValidationError(payload, "meta.review.verdict must be \"pass\"");
    }
    {
        auto payload = BuildValidPayload();
        payload.meta.review.reason = "replace-with-review-reason";
        RequireValidationError(payload, "meta.review.reason is missing or placeholder");
    }
}

TEST_CASE("AI commit plan validation preserves exact stage diagnostics and scan order",
          "[Unit][CommitPlan][Payload][Validation]") {
    {
        auto payload = BuildValidPayload();
        payload.commitEntries[0].commits[0].message = "replace-with-message";
        payload.postSyncEntries[0].commits[0].message = "replace-with-post-sync-message";
        RequireValidationError(payload, "no valid non-placeholder commit messages found in stages.commit/post_sync");
    }
    {
        auto payload = BuildValidPayload();
        payload.commitEntries[0].commits[0].message = "replace-with-message";
        RequireValidationError(payload, "stages.commit.repo(repo-a).commits[0].message is missing or placeholder");
    }
    {
        auto payload = BuildValidPayload();
        payload.commitEntries[0].commits[0].review.verdict = "fail";
        RequireValidationError(payload, "stages.commit.repo(repo-a).commits[0].review.verdict must be \"pass\"");
    }
    {
        auto payload = BuildValidPayload();
        payload.commitEntries[0].commits[0].review.reason.clear();
        RequireValidationError(payload, "stages.commit.repo(repo-a).commits[0].review.reason is missing or placeholder");
    }
    {
        auto payload = BuildValidPayload();
        payload.postSyncEntries[0].commits[0].review.reason.clear();
        RequireValidationError(payload, "stages.post_sync.repo(repo-b).commits[0].review.reason is missing or placeholder");
    }

    auto valid = BuildValidPayload();
    std::string untouched = "unchanged";
    REQUIRE(ValidateCommitPlanForAiMode(valid, &untouched));
    REQUIRE(untouched == "unchanged");
}

TEST_CASE("repo-scoped freshness predicate remains exact",
          "[Unit][CommitPlan][Payload][Freshness]") {
    CommitPlanPayload payload;
    payload.meta.freshnessScope = " RePo ";
    payload.meta.scopeRepo = " repo-a ";
    payload.meta.planner.provider = " NATIVE ";
    payload.meta.planner.model = " CONVERGE-INTENT-CLASSIFIER-V1 ";
    REQUIRE(UsesRepoScopedFreshness(payload));

    payload.meta.freshnessScope = "workspace";
    REQUIRE_FALSE(UsesRepoScopedFreshness(payload));
    payload.meta.freshnessScope = "repo";

    payload.meta.scopeRepo = " ";
    REQUIRE_FALSE(UsesRepoScopedFreshness(payload));
    payload.meta.scopeRepo = "repo-a";

    payload.meta.planner.provider = "agent";
    REQUIRE_FALSE(UsesRepoScopedFreshness(payload));
    payload.meta.planner.provider = "native";

    payload.meta.planner.model = "other-model";
    REQUIRE_FALSE(UsesRepoScopedFreshness(payload));
}

TEST_CASE("human auto-plan metadata classifier preserves deterministic and fail-open behavior",
          "[Unit][CommitPlan][Payload][Deterministic]") {
    TempPlan temp("deterministic");

    std::string reason = "unchanged";
    REQUIRE(HumanAutoPlanLooksDeterministic(temp.MissingPath(), &reason));
    REQUIRE(reason == "cannot read plan file");

    reason = "unchanged";
    REQUIRE(HumanAutoPlanLooksDeterministic(temp.Write("{}", "missing-meta.json"), &reason));
    REQUIRE(reason == "plan meta missing");

    reason = "unchanged";
    REQUIRE(HumanAutoPlanLooksDeterministic(
        temp.Write(R"({"meta":{"planner":{"provider":"agent","ai-model":"model"}}})", "agent.json"),
        &reason));
    REQUIRE(reason == "provider=agent model=model");

    reason = "unchanged";
    REQUIRE(HumanAutoPlanLooksDeterministic(
        temp.Write(R"({"meta":{"planner":{"provider":"other","ai-model":"deterministic"}}})",
                   "deterministic-model.json"),
        &reason));
    REQUIRE(reason == "provider=other model=deterministic");

    reason = "unchanged";
    REQUIRE_FALSE(HumanAutoPlanLooksDeterministic(
        temp.Write(R"({"meta":{"planner":{"provider":"other","model":"deterministic"}}})",
                   "legacy-model-ignored.json"),
        &reason));
    REQUIRE(reason == "unchanged");

    reason = "unchanged";
    REQUIRE_FALSE(HumanAutoPlanLooksDeterministic(
        temp.Write(R"({"meta":{"planner":{"provider":" Agent ","ai-model":"model"}}})",
                   "provider-whitespace.json"),
        &reason));
    REQUIRE(reason == "unchanged");
}
