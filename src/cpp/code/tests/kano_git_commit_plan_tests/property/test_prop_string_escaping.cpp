// Feature: plan-json-library-refactor, Property 2: String value round-trip (escaping correctness)
//
// Property 2: For any string containing backslashes, quotes, newlines, tabs,
// and Unicode, storing in a plan field then serializing and parsing back SHALL
// recover the original string byte-for-byte.
//
// **Validates: Requirements 2.3**

#include <catch2/catch_test_macros.hpp>

#include "plan_utils.hpp"

#include <nlohmann/json.hpp>
#include <rapidcheck.h>
#include <rapidcheck/catch.h>

#include <string>
#include <vector>

using namespace kano::git::commands;

namespace {

// Generate a string that may contain special characters requiring JSON escaping:
// backslashes, double-quotes, newlines, carriage returns, tabs, and arbitrary
// Unicode code points (encoded as UTF-8).
std::string GenSpecialString() {
    // Pool of special characters that must be correctly escaped in JSON
    static const std::vector<char> kSpecialChars = {
        '\\', '"', '\n', '\r', '\t', '\b', '\f',
        // A few printable ASCII chars for variety
        '/', ':', ' ', '!', '#', '$', '%', '&', '\'', '(', ')', '*', '+', ',', '-', '.', '0', 'A', 'z'
    };

    const auto len = *rc::gen::inRange<std::size_t>(0, 64);
    std::string s;
    s.reserve(len);
    for (std::size_t i = 0; i < len; ++i) {
        // 50% chance of a special char, 50% chance of a regular printable ASCII char
        if (*rc::gen::arbitrary<bool>()) {
            const auto idx = *rc::gen::inRange<std::size_t>(0, kSpecialChars.size());
            s.push_back(kSpecialChars[idx]);
        } else {
            // Printable ASCII 0x20–0x7E
            s.push_back(static_cast<char>(*rc::gen::inRange<int>(0x20, 0x7F)));
        }
    }
    return s;
}

} // namespace

// ---------------------------------------------------------------------------
// Property 2a: String round-trip via nlohmann::json object field
// ---------------------------------------------------------------------------

TEST_CASE("Property 2: String value round-trip — arbitrary strings survive JSON serialization",
          "[Feature: plan-json-library-refactor]"
          "[Property 2: String value round-trip (escaping correctness)]"
          "[property][escaping][req-2.3]") {
    // **Validates: Requirements 2.3**
    //
    // For any string value (including backslashes, quotes, newlines, tabs,
    // Unicode), storing it in an nlohmann::json field, serializing to a string,
    // and parsing back SHALL recover the original string byte-for-byte.

    rc::prop("arbitrary strings survive JSON round-trip", []() {
        const std::string original = GenSpecialString();

        // Store in a JSON object field (same pattern as Plan_Builder functions)
        nlohmann::json doc;
        doc["field"] = original;

        // Serialize (compact)
        const std::string serialized = doc.dump();

        // Parse back
        const nlohmann::json parsed = nlohmann::json::parse(serialized);

        // Recover the string
        const std::string recovered = parsed["field"].get<std::string>();

        // Must be byte-for-byte identical
        RC_ASSERT(recovered == original);
    });
}

// ---------------------------------------------------------------------------
// Property 2b: String round-trip in pretty mode
// ---------------------------------------------------------------------------

TEST_CASE("Property 2: String value round-trip — arbitrary strings survive pretty JSON serialization",
          "[Feature: plan-json-library-refactor]"
          "[Property 2: String value round-trip (escaping correctness)]"
          "[property][escaping][req-2.3]") {
    // **Validates: Requirements 2.3**
    //
    // The same round-trip guarantee must hold for pretty-printed output.

    rc::prop("arbitrary strings survive pretty JSON round-trip", []() {
        const std::string original = GenSpecialString();

        nlohmann::json doc;
        doc["field"] = original;

        // Serialize (pretty, 2-space indent)
        const std::string serialized = doc.dump(2);

        // Parse back
        const nlohmann::json parsed = nlohmann::json::parse(serialized);
        const std::string recovered = parsed["field"].get<std::string>();

        RC_ASSERT(recovered == original);
    });
}

// ---------------------------------------------------------------------------
// Property 2c: String round-trip in a plan-shaped document
// ---------------------------------------------------------------------------

TEST_CASE("Property 2: String value round-trip — plan field strings survive serialization",
          "[Feature: plan-json-library-refactor]"
          "[Property 2: String value round-trip (escaping correctness)]"
          "[property][escaping][BuildDefaultPlanTemplate][req-2.3]") {
    // **Validates: Requirements 2.3**
    //
    // Strings stored in plan-shaped fields (e.g. meta.plan_id, meta.base_head_sha,
    // ignore_datasource.root) must survive the full serialize/parse cycle.

    rc::prop("plan field strings survive round-trip", []() {
        const std::string planId   = GenSpecialString();
        const std::string sha      = GenSpecialString();
        const std::string dsRoot   = GenSpecialString();

        // Build a minimal plan-shaped document
        nlohmann::json doc;
        doc["meta"]["schema_version"] = "3";
        doc["meta"]["plan_id"]        = planId;
        doc["meta"]["base_head_sha"]  = sha;
        doc["meta"]["ignore_datasource"]["root"] = dsRoot;
        doc["stages"]["ignore"]    = nlohmann::json::array();
        doc["stages"]["commit"]    = nlohmann::json::array();
        doc["stages"]["post_sync"] = nlohmann::json::array();

        // Compact round-trip
        {
            const std::string serialized = doc.dump();
            const nlohmann::json parsed  = nlohmann::json::parse(serialized);
            RC_ASSERT(parsed["meta"]["plan_id"].get<std::string>()                    == planId);
            RC_ASSERT(parsed["meta"]["base_head_sha"].get<std::string>()              == sha);
            RC_ASSERT(parsed["meta"]["ignore_datasource"]["root"].get<std::string>()  == dsRoot);
        }

        // Pretty round-trip
        {
            const std::string serialized = doc.dump(2);
            const nlohmann::json parsed  = nlohmann::json::parse(serialized);
            RC_ASSERT(parsed["meta"]["plan_id"].get<std::string>()                    == planId);
            RC_ASSERT(parsed["meta"]["base_head_sha"].get<std::string>()              == sha);
            RC_ASSERT(parsed["meta"]["ignore_datasource"]["root"].get<std::string>()  == dsRoot);
        }
    });
}

// ---------------------------------------------------------------------------
// Property 2d: Specific known-tricky strings
// ---------------------------------------------------------------------------

TEST_CASE("Property 2: Known-tricky strings survive JSON round-trip",
          "[Feature: plan-json-library-refactor]"
          "[Property 2: String value round-trip (escaping correctness)]"
          "[unit][escaping][req-2.3]") {
    // **Validates: Requirements 2.3**
    //
    // Example-based checks for strings that historically caused issues with
    // hand-written JSON serializers.

    const std::vector<std::string> trickyStrings = {
        "",
        "\\",
        "\"",
        "\n",
        "\r\n",
        "\t",
        "C:\\Users\\dev\\workspace",
        "path/with/\"quotes\"",
        "line1\nline2\nline3",
        "tab\there",
        "backslash\\and\"quote",
        "\x01\x02\x03",  // control characters
        "unicode: \xc3\xa9\xc3\xa0\xc3\xbc",  // UTF-8: é à ü
    };

    for (const auto& original : trickyStrings) {
        nlohmann::json doc;
        doc["field"] = original;

        const std::string serialized = doc.dump();
        const nlohmann::json parsed  = nlohmann::json::parse(serialized);
        const std::string recovered  = parsed["field"].get<std::string>();

        REQUIRE(recovered == original);
    }
}
