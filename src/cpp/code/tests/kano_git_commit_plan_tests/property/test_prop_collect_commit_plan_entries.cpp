// Feature: plan-json-library-refactor, Property 3: Commit entry round-trip
//
// Property 3: For any non-empty list of CommitPlanEntry values, serializing
// into a plan then calling CollectCommitPlanEntries SHALL return identical
// entries in the same order.
//
// **Validates: Requirements 3.2, 4.6, 4.7**

#include <catch2/catch_test_macros.hpp>

#include "plan_utils.hpp"

#include <nlohmann/json.hpp>
#include <rapidcheck.h>
#include <rapidcheck/catch.h>

#include <string>
#include <vector>

using namespace kano::git::commands;

namespace {

// Generate a random printable ASCII string (no control chars) for use as
// commit entry field values.
std::string GenPrintableString(std::size_t maxLen = 32) {
    const auto len = *rc::gen::inRange<std::size_t>(0, maxLen + 1);
    std::string s;
    s.reserve(len);
    for (std::size_t i = 0; i < len; ++i) {
        s.push_back(static_cast<char>(*rc::gen::inRange<int>(0x20, 0x7F)));
    }
    return s;
}

// Generate a random vector of printable strings.
std::vector<std::string> GenStringVector(std::size_t maxCount = 4) {
    const auto count = *rc::gen::inRange<std::size_t>(0, maxCount + 1);
    std::vector<std::string> v;
    v.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        v.push_back(GenPrintableString(16));
    }
    return v;
}

// Generate a random CommitPlanEntry (index is ignored; it's assigned by the parser).
CommitPlanEntry GenCommitPlanEntry(const std::string& InRepo) {
    CommitPlanEntry e;
    e.repo          = InRepo;
    e.message       = GenPrintableString(48);
    e.include       = GenStringVector(3);
    e.exclude       = GenStringVector(3);
    e.reviewVerdict = GenPrintableString(16);
    e.reviewReason  = GenPrintableString(32);
    return e;
}

// Serialize a list of CommitPlanEntry values into a minimal plan JSON document.
// Groups entries by repo (preserving order) and builds the stages.commit array.
std::string SerializeEntriesToPlan(const std::vector<CommitPlanEntry>& InEntries) {
    // Group by repo (preserving insertion order)
    std::vector<std::string> repoOrder;
    std::unordered_map<std::string, nlohmann::json> repoCommits;

    for (const auto& e : InEntries) {
        if (repoCommits.find(e.repo) == repoCommits.end()) {
            repoOrder.push_back(e.repo);
            repoCommits[e.repo] = nlohmann::json::array();
        }
        nlohmann::json commitObj;
        commitObj["message"]           = e.message;
        commitObj["include"]           = e.include;
        commitObj["exclude"]           = e.exclude;
        commitObj["review"]["verdict"] = e.reviewVerdict;
        commitObj["review"]["reason"]  = e.reviewReason;
        repoCommits[e.repo].push_back(commitObj);
    }

    nlohmann::json commitStage = nlohmann::json::array();
    for (const auto& repo : repoOrder) {
        nlohmann::json repoObj;
        repoObj["repo"]    = repo;
        repoObj["commits"] = repoCommits[repo];
        commitStage.push_back(repoObj);
    }

    nlohmann::json doc;
    doc["meta"]["schema_version"] = "3";
    doc["stages"]["ignore"]    = nlohmann::json::array();
    doc["stages"]["commit"]    = commitStage;
    doc["stages"]["post_sync"] = nlohmann::json::array();

    return doc.dump();
}

} // namespace

// ---------------------------------------------------------------------------
// Property 3: Commit entry round-trip
// ---------------------------------------------------------------------------

TEST_CASE("Property 3: Commit entry round-trip — entries survive serialize/parse cycle",
          "[Feature: plan-json-library-refactor]"
          "[Property 3: Commit entry round-trip]"
          "[property][CollectCommitPlanEntries][req-3.2][req-4.6][req-4.7]") {
    // **Validates: Requirements 3.2, 4.6, 4.7**
    //
    // For any non-empty list of CommitPlanEntry values, serializing into a plan
    // document and calling CollectCommitPlanEntries SHALL return entries with
    // identical field values in the same order.

    rc::prop("commit entries survive round-trip", []() {
        // Generate 1–5 repos, each with 1–3 commits
        const auto numRepos = *rc::gen::inRange<std::size_t>(1, 4);
        std::vector<CommitPlanEntry> original;
        for (std::size_t r = 0; r < numRepos; ++r) {
            const std::string repo = "repo-" + std::to_string(r);
            const auto numCommits = *rc::gen::inRange<std::size_t>(1, 4);
            for (std::size_t c = 0; c < numCommits; ++c) {
                original.push_back(GenCommitPlanEntry(repo));
            }
        }

        RC_PRE(!original.empty());

        const std::string planText = SerializeEntriesToPlan(original);
        const auto recovered = CollectCommitPlanEntries(planText);

        // Must have the same count
        RC_ASSERT(recovered.size() == original.size());

        // Each entry must match field-by-field (index is assigned by parser)
        for (std::size_t i = 0; i < original.size(); ++i) {
            RC_ASSERT(recovered[i].repo          == original[i].repo);
            RC_ASSERT(recovered[i].message       == original[i].message);
            RC_ASSERT(recovered[i].include       == original[i].include);
            RC_ASSERT(recovered[i].exclude       == original[i].exclude);
            RC_ASSERT(recovered[i].reviewVerdict == original[i].reviewVerdict);
            RC_ASSERT(recovered[i].reviewReason  == original[i].reviewReason);
            // Flat index must be assigned sequentially
            RC_ASSERT(recovered[i].index == static_cast<int>(i));
        }
    });
}

// ---------------------------------------------------------------------------
// Property 3b: Empty plan returns empty vector
// ---------------------------------------------------------------------------

TEST_CASE("Property 3: CollectCommitPlanEntries returns empty vector for empty commit stage",
          "[Feature: plan-json-library-refactor]"
          "[Property 3: Commit entry round-trip]"
          "[unit][CollectCommitPlanEntries][req-3.2]") {
    // **Validates: Requirements 3.2**

    nlohmann::json doc;
    doc["meta"]["schema_version"] = "3";
    doc["stages"]["ignore"]    = nlohmann::json::array();
    doc["stages"]["commit"]    = nlohmann::json::array();
    doc["stages"]["post_sync"] = nlohmann::json::array();

    const auto entries = CollectCommitPlanEntries(doc.dump());
    REQUIRE(entries.empty());
}

// ---------------------------------------------------------------------------
// Property 3c: Invalid JSON returns empty vector (no throw)
// ---------------------------------------------------------------------------

TEST_CASE("Property 3: CollectCommitPlanEntries returns empty vector for invalid JSON",
          "[Feature: plan-json-library-refactor]"
          "[Property 3: Commit entry round-trip]"
          "[unit][CollectCommitPlanEntries][req-3.13]") {
    // **Validates: Requirements 3.13**

    REQUIRE_NOTHROW(CollectCommitPlanEntries("not json"));
    REQUIRE(CollectCommitPlanEntries("not json").empty());
    REQUIRE(CollectCommitPlanEntries("").empty());
    REQUIRE(CollectCommitPlanEntries("{\"stages\":{}}").empty());
}
