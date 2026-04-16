// Feature: plan-json-library-refactor, Property 4: Fill-ops round-trip
//
// Property 4: For any list of CommitFillOp values, serializing to JSON array
// then calling ParseCommitFillOps SHALL return identical structs.
//
// **Validates: Requirements 3.3**

#include <catch2/catch_test_macros.hpp>

#include "plan_utils.hpp"

#include <nlohmann/json.hpp>
#include <rapidcheck.h>
#include <rapidcheck/catch.h>

#include <string>
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

// Generate a random CommitFillOp.
CommitFillOp GenCommitFillOp(int InIndex) {
    CommitFillOp op;
    op.index          = InIndex;
    op.message        = GenPrintableString(48);
    op.reviewVerdict  = GenPrintableString(16);
    op.reviewReason   = GenPrintableString(32);
    op.plannerProvider = GenPrintableString(16);
    op.plannerModel   = GenPrintableString(24);
    return op;
}

// Serialize a list of CommitFillOp values to the JSON format expected by
// ParseCommitFillOps: {"commits": [...]}
std::string SerializeFillOps(const std::vector<CommitFillOp>& InOps) {
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& op : InOps) {
        nlohmann::json obj;
        obj["index"]           = op.index;
        obj["message"]         = op.message;
        obj["review"]["verdict"] = op.reviewVerdict;
        obj["review"]["reason"]  = op.reviewReason;
        obj["plannerProvider"] = op.plannerProvider;
        obj["plannerModel"]    = op.plannerModel;
        arr.push_back(obj);
    }
    nlohmann::json doc;
    doc["commits"] = arr;
    return doc.dump();
}

} // namespace

// ---------------------------------------------------------------------------
// Property 4: Fill-ops round-trip
// ---------------------------------------------------------------------------

TEST_CASE("Property 4: Fill-ops round-trip — CommitFillOp values survive serialize/parse cycle",
          "[Feature: plan-json-library-refactor]"
          "[Property 4: Fill-ops round-trip]"
          "[property][ParseCommitFillOps][req-3.3]") {
    // **Validates: Requirements 3.3**
    //
    // For any list of CommitFillOp values, serializing them to JSON and calling
    // ParseCommitFillOps SHALL return structs with identical field values.

    rc::prop("fill-ops survive round-trip", []() {
        const auto count = *rc::gen::inRange<std::size_t>(0, 8);
        std::vector<CommitFillOp> original;
        original.reserve(count);
        for (std::size_t i = 0; i < count; ++i) {
            original.push_back(GenCommitFillOp(static_cast<int>(i)));
        }

        const std::string json = SerializeFillOps(original);
        const auto recovered = ParseCommitFillOps(json);

        RC_ASSERT(recovered.size() == original.size());
        for (std::size_t i = 0; i < original.size(); ++i) {
            RC_ASSERT(recovered[i].index          == original[i].index);
            RC_ASSERT(recovered[i].message        == original[i].message);
            RC_ASSERT(recovered[i].reviewVerdict  == original[i].reviewVerdict);
            RC_ASSERT(recovered[i].reviewReason   == original[i].reviewReason);
            RC_ASSERT(recovered[i].plannerProvider == original[i].plannerProvider);
            RC_ASSERT(recovered[i].plannerModel   == original[i].plannerModel);
        }
    });
}

// ---------------------------------------------------------------------------
// Property 4b: Empty commits array returns empty vector
// ---------------------------------------------------------------------------

TEST_CASE("Property 4: ParseCommitFillOps returns empty vector for empty commits array",
          "[Feature: plan-json-library-refactor]"
          "[Property 4: Fill-ops round-trip]"
          "[unit][ParseCommitFillOps][req-3.3]") {
    // **Validates: Requirements 3.3**

    const auto ops = ParseCommitFillOps("{\"commits\":[]}");
    REQUIRE(ops.empty());
}

// ---------------------------------------------------------------------------
// Property 4c: Invalid JSON returns empty vector (no throw)
// ---------------------------------------------------------------------------

TEST_CASE("Property 4: ParseCommitFillOps returns empty vector for invalid JSON",
          "[Feature: plan-json-library-refactor]"
          "[Property 4: Fill-ops round-trip]"
          "[unit][ParseCommitFillOps][req-3.13]") {
    // **Validates: Requirements 3.13**

    REQUIRE_NOTHROW(ParseCommitFillOps("not json"));
    REQUIRE(ParseCommitFillOps("not json").empty());
    REQUIRE(ParseCommitFillOps("").empty());
    REQUIRE(ParseCommitFillOps("{}").empty());  // missing commits key
}
