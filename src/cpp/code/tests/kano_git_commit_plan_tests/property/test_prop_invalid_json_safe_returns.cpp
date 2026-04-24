// Feature: plan-json-library-refactor, Property 6: Invalid JSON returns safe empty values
//
// Property 6: For any non-JSON string, calling any Plan_Parser function SHALL
// not throw and SHALL return the appropriate empty/nullopt/false value for the
// calling function's return type.
//
// **Validates: Requirements 3.13**

#include <catch2/catch_test_macros.hpp>

#include "plan_utils.hpp"

#include <nlohmann/json.hpp>
#include <rapidcheck.h>
#include <rapidcheck/catch.h>

#include <string>
#include <vector>

using namespace kano::git::commands;

namespace {

// Generate a string that is NOT valid JSON.
// Strategy: generate arbitrary byte sequences that are unlikely to be valid JSON.
std::string GenNonJsonString() {
    // Mix of strategies to produce invalid JSON
    const int strategy = *rc::gen::inRange<int>(0, 5);
    switch (strategy) {
    case 0: {
        // Random printable ASCII that doesn't start with { or [
        const auto len = *rc::gen::inRange<std::size_t>(1, 64);
        std::string s;
        s.reserve(len);
        // First char: not { or [
        char first = static_cast<char>(*rc::gen::inRange<int>(0x21, 0x7B)); // '!' to 'z', skip '{'
        if (first == '[') first = 'X';
        s.push_back(first);
        for (std::size_t i = 1; i < len; ++i) {
            s.push_back(static_cast<char>(*rc::gen::inRange<int>(0x20, 0x7F)));
        }
        return s;
    }
    case 1:
        return "not json at all";
    case 2:
        return "{unclosed";
    case 3:
        return "[1, 2, 3";  // unclosed array
    case 4:
        return "";
    default:
        return "null";  // valid JSON but not an object — parsers should handle gracefully
    }
}

} // namespace

// ---------------------------------------------------------------------------
// Property 6: Invalid JSON returns safe empty values — CollectCommitPlanEntries
// ---------------------------------------------------------------------------

TEST_CASE("Property 6: CollectCommitPlanEntries does not throw on invalid JSON",
          "[Feature: plan-json-library-refactor]"
          "[Property 6: Invalid JSON returns safe empty values]"
          "[property][CollectCommitPlanEntries][req-3.13]") {
    // **Validates: Requirements 3.13**

    rc::prop("CollectCommitPlanEntries returns empty vector for non-JSON input", []() {
        const std::string input = GenNonJsonString();
        std::vector<CommitPlanEntry> result;
        bool threw = false;
        try { result = CollectCommitPlanEntries(input); } catch (...) { threw = true; }
        RC_ASSERT(!threw);
        RC_ASSERT(result.empty());
    });
}

// ---------------------------------------------------------------------------
// Property 6: Invalid JSON returns safe empty values — ParseCommitFillOps
// ---------------------------------------------------------------------------

TEST_CASE("Property 6: ParseCommitFillOps does not throw on invalid JSON",
          "[Feature: plan-json-library-refactor]"
          "[Property 6: Invalid JSON returns safe empty values]"
          "[property][ParseCommitFillOps][req-3.13]") {
    // **Validates: Requirements 3.13**

    rc::prop("ParseCommitFillOps returns empty vector for non-JSON input", []() {
        const std::string input = GenNonJsonString();
        std::vector<CommitFillOp> result;
        bool threw = false;
        try { result = ParseCommitFillOps(input); } catch (...) { threw = true; }
        RC_ASSERT(!threw);
        RC_ASSERT(result.empty());
    });
}

// ---------------------------------------------------------------------------
// Property 6: Invalid JSON returns safe empty values — ParseCommitFillOpsBatch
// ---------------------------------------------------------------------------

TEST_CASE("Property 6: ParseCommitFillOpsBatch does not throw on invalid JSON",
          "[Feature: plan-json-library-refactor]"
          "[Property 6: Invalid JSON returns safe empty values]"
          "[property][ParseCommitFillOpsBatch][req-3.13]") {
    rc::prop("ParseCommitFillOpsBatch returns empty batch for non-JSON input", []() {
        const std::string input = GenNonJsonString();
        CommitFillOpsBatch result;
        bool threw = false;
        try { result = ParseCommitFillOpsBatch(input); } catch (...) { threw = true; }
        RC_ASSERT(!threw);
        RC_ASSERT(result.ops.empty());
        RC_ASSERT(!result.commitStageJson.has_value());
    });
}

// ---------------------------------------------------------------------------
// Property 6: Invalid JSON returns safe empty values — ExtractPlanWorkspaceHashes
// ---------------------------------------------------------------------------

TEST_CASE("Property 6: ExtractPlanWorkspaceHashes does not throw on invalid JSON",
          "[Feature: plan-json-library-refactor]"
          "[Property 6: Invalid JSON returns safe empty values]"
          "[property][ExtractPlanWorkspaceHashes][req-3.13]") {
    // **Validates: Requirements 3.13**

    rc::prop("ExtractPlanWorkspaceHashes returns false for non-JSON input", []() {
        const std::string input = GenNonJsonString();
        bool result = true;
        bool threw = false;
        try { result = ExtractPlanWorkspaceHashes(input, nullptr, nullptr); } catch (...) { threw = true; }
        RC_ASSERT(!threw);
        RC_ASSERT(!result);
    });
}

// ---------------------------------------------------------------------------
// Property 6: Invalid JSON returns safe empty values — HasValidCommitItems
// ---------------------------------------------------------------------------

TEST_CASE("Property 6: HasValidCommitItems does not throw on invalid JSON",
          "[Feature: plan-json-library-refactor]"
          "[Property 6: Invalid JSON returns safe empty values]"
          "[property][HasValidCommitItems][req-3.13]") {
    // **Validates: Requirements 3.13**

    rc::prop("HasValidCommitItems returns false for non-JSON input", []() {
        const std::string input = GenNonJsonString();
        bool result = true;
        bool threw = false;
        try { result = HasValidCommitItems(input); } catch (...) { threw = true; }
        RC_ASSERT(!threw);
        RC_ASSERT(!result);
    });
}

// ---------------------------------------------------------------------------
// Property 6: Invalid JSON returns safe empty values — ParseIgnoreEntries
// ---------------------------------------------------------------------------

TEST_CASE("Property 6: ParseIgnoreEntries does not throw on invalid JSON",
          "[Feature: plan-json-library-refactor]"
          "[Property 6: Invalid JSON returns safe empty values]"
          "[property][ParseIgnoreEntries][req-3.13]") {
    // **Validates: Requirements 3.13**

    rc::prop("ParseIgnoreEntries returns empty vector for non-JSON input", []() {
        const std::string input = GenNonJsonString();
        std::vector<IgnoreStageEntry> result;
        bool threw = false;
        try { result = ParseIgnoreEntries(input); } catch (...) { threw = true; }
        RC_ASSERT(!threw);
        RC_ASSERT(result.empty());
    });
}

// ---------------------------------------------------------------------------
// Property 6: Known-bad inputs — example-based checks
// ---------------------------------------------------------------------------

TEST_CASE("Property 6: All Plan_Parser functions handle known-bad inputs safely",
          "[Feature: plan-json-library-refactor]"
          "[Property 6: Invalid JSON returns safe empty values]"
          "[unit][req-3.13]") {
    // **Validates: Requirements 3.13**

    const std::vector<std::string> badInputs = {
        "",
        "not json",
        "{unclosed",
        "[1, 2",
        "null",
        "42",
        "\"just a string\"",
        "{}",
        "{\"meta\":{}}",
        "{\"stages\":{}}",
    };

    for (const auto& input : badInputs) {
        REQUIRE_NOTHROW(CollectCommitPlanEntries(input));
        REQUIRE(CollectCommitPlanEntries(input).empty());

        REQUIRE_NOTHROW(ParseCommitFillOps(input));
        REQUIRE(ParseCommitFillOps(input).empty());

        REQUIRE_NOTHROW(ParseCommitFillOpsBatch(input));
        REQUIRE(ParseCommitFillOpsBatch(input).ops.empty());
        REQUIRE_FALSE(ParseCommitFillOpsBatch(input).commitStageJson.has_value());

        REQUIRE_NOTHROW(ParseCommitFillOpsBatch(input));
        REQUIRE(ParseCommitFillOpsBatch(input).ops.empty());
        REQUIRE_FALSE(ParseCommitFillOpsBatch(input).commitStageJson.has_value());

        REQUIRE_NOTHROW(ExtractPlanWorkspaceHashes(input, nullptr, nullptr));
        REQUIRE_FALSE(ExtractPlanWorkspaceHashes(input, nullptr, nullptr));

        REQUIRE_NOTHROW(HasValidCommitItems(input));
        REQUIRE_FALSE(HasValidCommitItems(input));

        REQUIRE_NOTHROW(ParseIgnoreEntries(input));
        REQUIRE(ParseIgnoreEntries(input).empty());
    }
}
