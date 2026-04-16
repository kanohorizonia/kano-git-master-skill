// Feature: plan-json-library-refactor, Property 5: ApplyCommitFillOps updates exactly the targeted entries
//
// Property 5: For any plan with N entries and fill ops with valid indices,
// ApplyCommitFillOps SHALL update exactly the targeted entries, leaving all
// others unchanged.
//
// **Validates: Requirements 3.4, 4.6, 4.7**

#include <catch2/catch_test_macros.hpp>

#include "plan_utils.hpp"

#include <nlohmann/json.hpp>
#include <rapidcheck.h>
#include <rapidcheck/catch.h>

#include <string>
#include <unordered_set>
#include <vector>

using namespace kano::git::commands;

namespace {

// Generate a random printable ASCII string.
std::string GenPrintableString(std::size_t maxLen = 32) {
    const auto len = *rc::gen::inRange<std::size_t>(0, maxLen + 1);
    std::string s;
    s.reserve(len);
    for (std::size_t i = 0; i < len; ++i) {
        s.push_back(static_cast<char>(*rc::gen::inRange<int>(0x20, 0x7F)));
    }
    return s;
}

// Build a plan JSON string with N commit entries (one repo, N commits).
std::string BuildPlanWithNEntries(int N) {
    nlohmann::json commitStage = nlohmann::json::array();
    nlohmann::json commits = nlohmann::json::array();
    for (int i = 0; i < N; ++i) {
        nlohmann::json c;
        c["message"]           = "original-message-" + std::to_string(i);
        c["include"]           = nlohmann::json::array();
        c["exclude"]           = nlohmann::json::array();
        c["review"]["verdict"] = "pass";
        c["review"]["reason"]  = "original-reason-" + std::to_string(i);
        c["planner"]["provider"] = "original-provider";
        c["planner"]["ai-model"] = "original-model";
        commits.push_back(c);
    }
    nlohmann::json repoObj;
    repoObj["repo"]    = ".";
    repoObj["commits"] = commits;
    commitStage.push_back(repoObj);

    nlohmann::json doc;
    doc["meta"]["schema_version"] = "3";
    doc["stages"]["ignore"]    = nlohmann::json::array();
    doc["stages"]["commit"]    = commitStage;
    doc["stages"]["post_sync"] = nlohmann::json::array();
    return doc.dump();
}

} // namespace

// ---------------------------------------------------------------------------
// Property 5: ApplyCommitFillOps updates exactly targeted entries
// ---------------------------------------------------------------------------

TEST_CASE("Property 5: ApplyCommitFillOps updates exactly the targeted entries",
          "[Feature: plan-json-library-refactor]"
          "[Property 5: ApplyCommitFillOps updates exactly the targeted entries]"
          "[property][ApplyCommitFillOps][req-3.4][req-4.6][req-4.7]") {
    // **Validates: Requirements 3.4, 4.6, 4.7**
    //
    // For any plan with N entries and fill ops with valid indices [0, N),
    // ApplyCommitFillOps SHALL update exactly the targeted entries and leave
    // all others unchanged.

    rc::prop("ApplyCommitFillOps updates exactly targeted entries", []() {
        const int N = *rc::gen::inRange<int>(1, 8);
        const std::string planText = BuildPlanWithNEntries(N);

        // Pick a random subset of indices to target
        const auto numOps = *rc::gen::inRange<int>(0, N + 1);
        std::unordered_set<int> targetedSet;
        std::vector<CommitFillOp> ops;
        for (int i = 0; i < numOps; ++i) {
            const int idx = *rc::gen::inRange<int>(0, N);
            if (targetedSet.count(idx)) continue;
            targetedSet.insert(idx);
            CommitFillOp op;
            op.index          = idx;
            op.message        = "new-message-" + std::to_string(idx);
            op.reviewVerdict  = "updated-verdict";
            op.reviewReason   = "updated-reason";
            op.plannerProvider = "new-provider";
            op.plannerModel   = "new-model";
            ops.push_back(op);
        }

        const std::string updated = ApplyCommitFillOps(planText, ops);

        // Parse both original and updated
        const auto origDoc    = nlohmann::json::parse(planText);
        const auto updatedDoc = nlohmann::json::parse(updated);

        const auto& origCommits    = origDoc["stages"]["commit"][0]["commits"];
        const auto& updatedCommits = updatedDoc["stages"]["commit"][0]["commits"];

        RC_ASSERT(origCommits.size() == updatedCommits.size());

        for (int i = 0; i < N; ++i) {
            if (targetedSet.count(i)) {
                // Targeted entry: message, review, planner must be updated
                RC_ASSERT(updatedCommits[i]["message"].get<std::string>() == "new-message-" + std::to_string(i));
                RC_ASSERT(updatedCommits[i]["review"]["verdict"].get<std::string>() == "updated-verdict");
                RC_ASSERT(updatedCommits[i]["review"]["reason"].get<std::string>()  == "updated-reason");
                RC_ASSERT(updatedCommits[i]["planner"]["provider"].get<std::string>() == "new-provider");
                RC_ASSERT(updatedCommits[i]["planner"]["ai-model"].get<std::string>() == "new-model");
            } else {
                // Non-targeted entry: must be unchanged
                RC_ASSERT(updatedCommits[i]["message"].get<std::string>() == origCommits[i]["message"].get<std::string>());
                RC_ASSERT(updatedCommits[i]["review"]["verdict"].get<std::string>() == origCommits[i]["review"]["verdict"].get<std::string>());
                RC_ASSERT(updatedCommits[i]["review"]["reason"].get<std::string>()  == origCommits[i]["review"]["reason"].get<std::string>());
            }
        }
    });
}

// ---------------------------------------------------------------------------
// Property 5b: Empty ops list leaves plan unchanged
// ---------------------------------------------------------------------------

TEST_CASE("Property 5: ApplyCommitFillOps with empty ops returns original plan",
          "[Feature: plan-json-library-refactor]"
          "[Property 5: ApplyCommitFillOps updates exactly the targeted entries]"
          "[unit][ApplyCommitFillOps][req-3.4]") {
    // **Validates: Requirements 3.4**

    const std::string planText = BuildPlanWithNEntries(3);
    const std::string result   = ApplyCommitFillOps(planText, {});
    REQUIRE(result == planText);
}

// ---------------------------------------------------------------------------
// Property 5c: Invalid JSON returns original string (no throw)
// ---------------------------------------------------------------------------

TEST_CASE("Property 5: ApplyCommitFillOps with invalid JSON returns original string",
          "[Feature: plan-json-library-refactor]"
          "[Property 5: ApplyCommitFillOps updates exactly the targeted entries]"
          "[unit][ApplyCommitFillOps][req-3.13]") {
    // **Validates: Requirements 3.13**

    CommitFillOp op;
    op.index   = 0;
    op.message = "new";

    REQUIRE_NOTHROW(ApplyCommitFillOps("not json", {op}));
    REQUIRE(ApplyCommitFillOps("not json", {op}) == "not json");
}
