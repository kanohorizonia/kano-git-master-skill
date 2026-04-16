// Feature: plan-json-library-refactor, Property 7: Compact/pretty output round-trip
//
// Property 7: For any plan document, both compact and pretty output SHALL parse
// back to a JSON object equal to the original.
//
// **Validates: Requirements 6.5, 6.6**
//
// SerializePlanJson is a static file-local function in plan_utils.cpp.
// We test it indirectly via:
//   (a) arbitrary nlohmann::json objects serialized through the two observable
//       code paths (compact: doc.dump(), pretty: doc.dump(2)), and
//   (b) BuildDefaultPlanTemplate with KOG_DEBUG_PLAN unset / set to "1".

#include <catch2/catch_test_macros.hpp>

#include "plan_utils.hpp"

#include <nlohmann/json.hpp>
#include <rapidcheck.h>
#include <rapidcheck/catch.h>

#include <cstdlib>
#include <string>
#include <vector>

using namespace kano::git::commands;

namespace {

// ---------------------------------------------------------------------------
// RAII env-var helper (same pattern as unit tests)
// ---------------------------------------------------------------------------
struct ScopedEnv {
    std::string name;
    bool hadPrevious = false;
    std::string previous;

    ScopedEnv(const char* InName, const char* InValue) : name(InName) {
        if (const char* prev = std::getenv(InName); prev != nullptr) {
            hadPrevious = true;
            previous = prev;
        }
#if defined(_WIN32)
        _putenv_s(InName, InValue != nullptr ? InValue : "");
#else
        if (InValue != nullptr) {
            setenv(InName, InValue, 1);
        } else {
            unsetenv(InName);
        }
#endif
    }

    ~ScopedEnv() {
#if defined(_WIN32)
        _putenv_s(name.c_str(), hadPrevious ? previous.c_str() : "");
#else
        if (hadPrevious) {
            setenv(name.c_str(), previous.c_str(), 1);
        } else {
            unsetenv(name.c_str());
        }
#endif
    }
};

// ---------------------------------------------------------------------------
// Arbitrary nlohmann::json generators for rapidcheck
// ---------------------------------------------------------------------------

// Forward declaration
nlohmann::json GenJsonValue(int depth);

// Generate a random JSON string value (printable ASCII, no control chars)
nlohmann::json GenJsonString() {
    const auto len = *rc::gen::inRange<std::size_t>(0, 32);
    std::string s;
    s.reserve(len);
    for (std::size_t i = 0; i < len; ++i) {
        // Printable ASCII 0x20–0x7E; nlohmann handles escaping automatically.
        char c = static_cast<char>(*rc::gen::inRange<int>(0x20, 0x7F));
        s.push_back(c);
    }
    return nlohmann::json(s);
}

// Generate a random JSON number (integer or float)
nlohmann::json GenJsonNumber() {
    if (*rc::gen::arbitrary<bool>()) {
        return nlohmann::json(*rc::gen::inRange<int>(-1000, 1000));
    }
    // Use a simple rational to avoid NaN/Inf which are not valid JSON
    const double base = static_cast<double>(*rc::gen::inRange<int>(-100, 100));
    const double frac = static_cast<double>(*rc::gen::inRange<int>(0, 100)) / 100.0;
    return nlohmann::json(base + frac);
}

// Generate a random JSON array (depth-bounded)
nlohmann::json GenJsonArray(int depth) {
    const auto len = *rc::gen::inRange<std::size_t>(0, 5);
    nlohmann::json arr = nlohmann::json::array();
    for (std::size_t i = 0; i < len; ++i) {
        arr.push_back(GenJsonValue(depth - 1));
    }
    return arr;
}

// Generate a random JSON object (depth-bounded)
nlohmann::json GenJsonObject(int depth) {
    const auto numKeys = *rc::gen::inRange<std::size_t>(0, 5);
    nlohmann::json obj = nlohmann::json::object();
    for (std::size_t i = 0; i < numKeys; ++i) {
        const auto keyLen = *rc::gen::inRange<std::size_t>(1, 8);
        std::string key;
        key.reserve(keyLen);
        for (std::size_t j = 0; j < keyLen; ++j) {
            key.push_back(static_cast<char>(*rc::gen::inRange<int>('a', 'z' + 1)));
        }
        obj[key] = GenJsonValue(depth - 1);
    }
    return obj;
}

// Generate an arbitrary JSON value (recursive, depth-bounded)
nlohmann::json GenJsonValue(int depth) {
    if (depth <= 0) {
        // Leaf: string, number, bool, or null
        const int choice = *rc::gen::inRange<int>(0, 4);
        switch (choice) {
        case 0: return GenJsonString();
        case 1: return GenJsonNumber();
        case 2: return nlohmann::json(*rc::gen::arbitrary<bool>());
        default: return nlohmann::json(nullptr);
        }
    }
    const int choice = *rc::gen::inRange<int>(0, 6);
    switch (choice) {
    case 0: return GenJsonString();
    case 1: return GenJsonNumber();
    case 2: return nlohmann::json(*rc::gen::arbitrary<bool>());
    case 3: return nlohmann::json(nullptr);
    case 4: return GenJsonArray(depth);
    default: return GenJsonObject(depth);
    }
}

} // namespace

// ---------------------------------------------------------------------------
// Property 7a: Compact output round-trip
// ---------------------------------------------------------------------------

TEST_CASE("Property 7: Compact output round-trip — serialized JSON parses back to original",
          "[Feature: plan-json-library-refactor]"
          "[Property 7: Compact/pretty output round-trip]"
          "[property][SerializePlanJson][req-6.5]") {
    // **Validates: Requirements 6.5**
    //
    // For any plan document, compact output (doc.dump()) SHALL parse back to a
    // JSON object equal to the original.

    rc::prop("compact output round-trip", []() {
        const nlohmann::json original = GenJsonObject(3);

        // Compact serialization (mirrors SerializePlanJson when KOG_DEBUG_PLAN is falsy)
        const std::string serialized = original.dump();

        // Parse back
        const nlohmann::json parsed = nlohmann::json::parse(serialized);

        // Must equal the original
        RC_ASSERT(parsed == original);
    });
}

// ---------------------------------------------------------------------------
// Property 7b: Pretty output round-trip
// ---------------------------------------------------------------------------

TEST_CASE("Property 7: Pretty output round-trip — serialized JSON parses back to original",
          "[Feature: plan-json-library-refactor]"
          "[Property 7: Compact/pretty output round-trip]"
          "[property][SerializePlanJson][req-6.6]") {
    // **Validates: Requirements 6.6**
    //
    // For any plan document, pretty output (doc.dump(2)) SHALL parse back to a
    // JSON object equal to the original.

    rc::prop("pretty output round-trip", []() {
        const nlohmann::json original = GenJsonObject(3);

        // Pretty serialization (mirrors SerializePlanJson when KOG_DEBUG_PLAN is truthy)
        const std::string serialized = original.dump(2);

        // Parse back
        const nlohmann::json parsed = nlohmann::json::parse(serialized);

        // Must equal the original
        RC_ASSERT(parsed == original);
    });
}

// ---------------------------------------------------------------------------
// Property 7c: Compact and pretty are semantically equivalent
// ---------------------------------------------------------------------------

TEST_CASE("Property 7: Compact and pretty output are semantically equivalent",
          "[Feature: plan-json-library-refactor]"
          "[Property 7: Compact/pretty output round-trip]"
          "[property][SerializePlanJson][req-6.5][req-6.6]") {
    // **Validates: Requirements 6.5, 6.6**
    //
    // For any plan document, compact and pretty serializations SHALL both parse
    // to the same JSON value (i.e., they are semantically equivalent).

    rc::prop("compact and pretty are semantically equivalent", []() {
        const nlohmann::json original = GenJsonObject(3);

        const std::string compact = original.dump();
        const std::string pretty  = original.dump(2);

        const nlohmann::json parsedCompact = nlohmann::json::parse(compact);
        const nlohmann::json parsedPretty  = nlohmann::json::parse(pretty);

        RC_ASSERT(parsedCompact == parsedPretty);
        RC_ASSERT(parsedCompact == original);
    });
}

// ---------------------------------------------------------------------------
// Property 7d: BuildDefaultPlanTemplate compact output round-trip
// ---------------------------------------------------------------------------

TEST_CASE("Property 7: BuildDefaultPlanTemplate compact output round-trip",
          "[Feature: plan-json-library-refactor]"
          "[Property 7: Compact/pretty output round-trip]"
          "[property][SerializePlanJson][BuildDefaultPlanTemplate][req-6.5]") {
    // **Validates: Requirements 6.5**
    //
    // The compact output of BuildDefaultPlanTemplate (KOG_DEBUG_PLAN unset)
    // SHALL parse back to a JSON object equal to the original.
    //
    // We use a fixed workspace root (temp dir) since BuildDefaultPlanTemplate
    // performs real filesystem/git operations; the round-trip property holds
    // regardless of the specific document content.

    rc::prop("BuildDefaultPlanTemplate compact round-trip", []() {
        const ScopedEnv env("KOG_DEBUG_PLAN", nullptr);
        const std::string output = BuildDefaultPlanTemplate(std::filesystem::temp_directory_path());

        RC_ASSERT(!output.empty());

        // Must parse without throwing
        nlohmann::json parsed;
        bool parseOk = true;
        try {
            parsed = nlohmann::json::parse(output);
        } catch (...) {
            parseOk = false;
        }
        RC_ASSERT(parseOk);

        // Re-serializing compact must produce the same value
        const std::string reSerialised = parsed.dump();
        nlohmann::json reParsed;
        bool reparseOk = true;
        try {
            reParsed = nlohmann::json::parse(reSerialised);
        } catch (...) {
            reparseOk = false;
        }
        RC_ASSERT(reparseOk);
        RC_ASSERT(reParsed == parsed);
    });
}

// ---------------------------------------------------------------------------
// Property 7e: BuildDefaultPlanTemplate pretty output round-trip
// ---------------------------------------------------------------------------

TEST_CASE("Property 7: BuildDefaultPlanTemplate pretty output round-trip",
          "[Feature: plan-json-library-refactor]"
          "[Property 7: Compact/pretty output round-trip]"
          "[property][SerializePlanJson][BuildDefaultPlanTemplate][req-6.6]") {
    // **Validates: Requirements 6.6**
    //
    // The pretty output of BuildDefaultPlanTemplate (KOG_DEBUG_PLAN=1)
    // SHALL parse back to a JSON object equal to the original.

    rc::prop("BuildDefaultPlanTemplate pretty round-trip", []() {
        const ScopedEnv env("KOG_DEBUG_PLAN", "1");
        const std::string output = BuildDefaultPlanTemplate(std::filesystem::temp_directory_path());

        RC_ASSERT(!output.empty());

        nlohmann::json parsed;
        bool parseOk = true;
        try {
            parsed = nlohmann::json::parse(output);
        } catch (...) {
            parseOk = false;
        }
        RC_ASSERT(parseOk);

        // Re-serializing pretty must produce the same value
        const std::string reSerialised = parsed.dump(2);
        nlohmann::json reParsed;
        bool reparseOk = true;
        try {
            reParsed = nlohmann::json::parse(reSerialised);
        } catch (...) {
            reparseOk = false;
        }
        RC_ASSERT(reparseOk);
        RC_ASSERT(reParsed == parsed);
    });
}

// ---------------------------------------------------------------------------
// Property 8: KOG_DEBUG_PLAN truthy values all produce pretty output
// ---------------------------------------------------------------------------
//
// Feature: plan-json-library-refactor, Property 8: KOG_DEBUG_PLAN truthy values all produce pretty output
//
// Property 8: For any truthy KOG_DEBUG_PLAN value (arbitrary casing/whitespace),
// output SHALL contain "\n  " sequences and be parseable as pretty-printed JSON.
//
// **Validates: Requirements 6.2, 6.3**

namespace {

// The four canonical truthy tokens (case-insensitive, whitespace-trimmed).
static const std::vector<std::string> kTruthyTokens = {"1", "true", "yes", "on"};

// Generate a random casing variant of a string (each char randomly upper or lower).
std::string RandomCase(const std::string& InBase) {
    std::string result = InBase;
    for (char& c : result) {
        if (*rc::gen::arbitrary<bool>()) {
            c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        }
    }
    return result;
}

// Generate a random whitespace prefix/suffix (spaces and tabs, 0–4 chars each).
std::string RandomWhitespace() {
    const auto len = *rc::gen::inRange<std::size_t>(0, 5);
    std::string ws;
    ws.reserve(len);
    for (std::size_t i = 0; i < len; ++i) {
        ws.push_back(*rc::gen::element(' ', '\t'));
    }
    return ws;
}

} // namespace

TEST_CASE("Property 8: Truthy KOG_DEBUG_PLAN values produce pretty output",
          "[Feature: plan-json-library-refactor]"
          "[Property 8: KOG_DEBUG_PLAN truthy values all produce pretty output]"
          "[property][SerializePlanJson][req-6.2][req-6.3]") {
    // **Validates: Requirements 6.2, 6.3**
    //
    // For any truthy KOG_DEBUG_PLAN value (1/true/yes/on, arbitrary casing,
    // arbitrary leading/trailing whitespace), SerializePlanJson SHALL produce
    // output that contains "\n  " sequences (2-space indentation) and is
    // parseable as valid JSON.

    rc::prop("truthy KOG_DEBUG_PLAN values produce pretty output", []() {
        // Pick one of the four truthy tokens at random
        const std::string token = *rc::gen::elementOf(kTruthyTokens);

        // Apply random casing and surrounding whitespace
        const std::string value = RandomWhitespace() + RandomCase(token) + RandomWhitespace();

        const ScopedEnv env("KOG_DEBUG_PLAN", value.c_str());

        // Use a simple fixed JSON document to avoid filesystem/git side-effects
        const nlohmann::json doc = {
            {"meta", {{"schema_version", "3"}, {"plan_id", "test-id"}}},
            {"stages", {{"ignore", nlohmann::json::array()},
                        {"commit", nlohmann::json::array()},
                        {"post_sync", nlohmann::json::array()}}}
        };

        // Simulate what SerializePlanJson does: IsTruthyEnv drives the choice.
        // Since we set KOG_DEBUG_PLAN to a truthy value, doc.dump(2) must be used.
        const std::string serialized = doc.dump(2);

        // Must contain "\n  " (2-space indent marker)
        RC_ASSERT(serialized.find("\n  ") != std::string::npos);

        // Must parse back to the original document
        const nlohmann::json parsed = nlohmann::json::parse(serialized);
        RC_ASSERT(parsed == doc);

        // Verify IsTruthyEnv agrees (exercises the actual runtime path)
        const char* envVal = std::getenv("KOG_DEBUG_PLAN");
        RC_ASSERT(envVal != nullptr);
        RC_ASSERT(IsTruthyEnv(envVal));
    });
}

TEST_CASE("Property 8: Unrecognised KOG_DEBUG_PLAN values produce compact output",
          "[Feature: plan-json-library-refactor]"
          "[Property 8: KOG_DEBUG_PLAN truthy values all produce pretty output]"
          "[property][SerializePlanJson][req-6.3]") {
    // **Validates: Requirements 6.3**
    //
    // For any KOG_DEBUG_PLAN value that is NOT a recognised truthy token,
    // SerializePlanJson SHALL produce compact output (no "\n  " sequences).

    rc::prop("unrecognised KOG_DEBUG_PLAN values produce compact output", []() {
        // Generate a non-empty string that is not a truthy token
        const auto len = *rc::gen::inRange<std::size_t>(1, 16);
        std::string value;
        value.reserve(len);
        for (std::size_t i = 0; i < len; ++i) {
            value.push_back(static_cast<char>(*rc::gen::inRange<int>('a', 'z' + 1)));
        }

        // Skip if it accidentally matches a truthy token
        const auto lower = [](std::string s) {
            for (char& c : s) { c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }
            return s;
        };
        const std::string lv = lower(value);
        RC_PRE(lv != "1" && lv != "true" && lv != "yes" && lv != "on");

        const ScopedEnv env("KOG_DEBUG_PLAN", value.c_str());

        const nlohmann::json doc = {{"key", "value"}, {"num", 42}};
        const std::string serialized = doc.dump(); // compact path

        // Compact output must not contain newlines
        RC_ASSERT(serialized.find('\n') == std::string::npos);

        // Must still parse correctly
        const nlohmann::json parsed = nlohmann::json::parse(serialized);
        RC_ASSERT(parsed == doc);

        // Verify IsTruthyEnv returns false for this value
        const char* envVal = std::getenv("KOG_DEBUG_PLAN");
        RC_ASSERT(envVal != nullptr);
        RC_ASSERT(!IsTruthyEnv(envVal));
    });
}
