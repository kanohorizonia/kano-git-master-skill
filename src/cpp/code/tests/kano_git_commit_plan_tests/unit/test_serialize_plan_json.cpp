// Unit tests for SerializePlanJson (via IsTruthyEnv and BuildDefaultPlanTemplate).
//
// SerializePlanJson is a static file-local function in plan_utils.cpp.
// Its behaviour is fully determined by IsTruthyEnv("KOG_DEBUG_PLAN"), which IS
// part of the public API.  We therefore test:
//
//   1. IsTruthyEnv directly — covers the compact/pretty decision logic.
//   2. The observable output format of BuildDefaultPlanTemplate — the only
//      public function that calls SerializePlanJson and whose output we can
//      inspect for whitespace structure.
//
// Requirements: 6.1, 6.2, 6.3

#include <catch2/catch_test_macros.hpp>

#include "plan_utils.hpp"

#include <cstdlib>
#include <string>

using namespace kano::git::commands;

namespace {

// RAII helper: sets an environment variable for the duration of a test, then
// restores the previous value (or unsets it) on destruction.
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

// Returns true if the string looks like compact JSON (no newlines).
auto IsCompact(const std::string& InJson) -> bool {
    return InJson.find('\n') == std::string::npos;
}

// Returns true if the string looks like 2-space-indented pretty JSON.
// We check for the "\n  " sequence that nlohmann/json produces at indent=2.
auto IsPretty(const std::string& InJson) -> bool {
    return InJson.find("\n  ") != std::string::npos;
}

} // namespace

// ---------------------------------------------------------------------------
// IsTruthyEnv — unit tests for the public helper that drives the
// compact/pretty decision inside SerializePlanJson.
// ---------------------------------------------------------------------------

TEST_CASE("IsTruthyEnv returns false for nullptr", "[Unit][SerializePlanJson][req-6.1]") {
    // **Validates: Requirements 6.1**
    REQUIRE_FALSE(IsTruthyEnv(nullptr));
}

TEST_CASE("IsTruthyEnv returns false for empty string", "[Unit][SerializePlanJson][req-6.1]") {
    // **Validates: Requirements 6.1**
    REQUIRE_FALSE(IsTruthyEnv(""));
}

TEST_CASE("IsTruthyEnv returns true for '1'", "[Unit][SerializePlanJson][req-6.2]") {
    // **Validates: Requirements 6.2**
    REQUIRE(IsTruthyEnv("1"));
}

TEST_CASE("IsTruthyEnv returns true for 'true'", "[Unit][SerializePlanJson][req-6.2]") {
    // **Validates: Requirements 6.2**
    REQUIRE(IsTruthyEnv("true"));
}

TEST_CASE("IsTruthyEnv returns true for 'yes'", "[Unit][SerializePlanJson][req-6.2]") {
    // **Validates: Requirements 6.2**
    REQUIRE(IsTruthyEnv("yes"));
}

TEST_CASE("IsTruthyEnv returns true for 'on'", "[Unit][SerializePlanJson][req-6.2]") {
    // **Validates: Requirements 6.2**
    REQUIRE(IsTruthyEnv("on"));
}

TEST_CASE("IsTruthyEnv is case-insensitive for truthy values", "[Unit][SerializePlanJson][req-6.2]") {
    // **Validates: Requirements 6.2**
    REQUIRE(IsTruthyEnv("TRUE"));
    REQUIRE(IsTruthyEnv("True"));
    REQUIRE(IsTruthyEnv("YES"));
    REQUIRE(IsTruthyEnv("Yes"));
    REQUIRE(IsTruthyEnv("ON"));
    REQUIRE(IsTruthyEnv("On"));
}

TEST_CASE("IsTruthyEnv ignores leading/trailing whitespace", "[Unit][SerializePlanJson][req-6.2]") {
    // **Validates: Requirements 6.2**
    REQUIRE(IsTruthyEnv("  1  "));
    REQUIRE(IsTruthyEnv(" true "));
    REQUIRE(IsTruthyEnv("\tyes\t"));
    REQUIRE(IsTruthyEnv("  on  "));
}

TEST_CASE("IsTruthyEnv returns false for unrecognised values", "[Unit][SerializePlanJson][req-6.3]") {
    // **Validates: Requirements 6.3**
    // Unrecognised values must be treated as falsy → compact output.
    REQUIRE_FALSE(IsTruthyEnv("verbose"));
    REQUIRE_FALSE(IsTruthyEnv("2"));
    REQUIRE_FALSE(IsTruthyEnv("debug"));
    REQUIRE_FALSE(IsTruthyEnv("enabled"));
    REQUIRE_FALSE(IsTruthyEnv("0"));
    REQUIRE_FALSE(IsTruthyEnv("false"));
    REQUIRE_FALSE(IsTruthyEnv("no"));
    REQUIRE_FALSE(IsTruthyEnv("off"));
}

// ---------------------------------------------------------------------------
// SerializePlanJson observable output — tested via BuildDefaultPlanTemplate.
//
// BuildDefaultPlanTemplate calls SerializePlanJson internally, so its output
// format directly reflects the compact/pretty decision.
// ---------------------------------------------------------------------------

TEST_CASE("SerializePlanJson produces compact output when KOG_DEBUG_PLAN is unset",
          "[Unit][SerializePlanJson][req-6.1]") {
    // **Validates: Requirements 6.1**
    const ScopedEnv env("KOG_DEBUG_PLAN", nullptr);

    const auto output = BuildDefaultPlanTemplate(std::filesystem::temp_directory_path());

    REQUIRE_FALSE(output.empty());
    REQUIRE(IsCompact(output));
    REQUIRE_FALSE(IsPretty(output));
}

TEST_CASE("SerializePlanJson produces 2-space-indented output when KOG_DEBUG_PLAN=1",
          "[Unit][SerializePlanJson][req-6.2]") {
    // **Validates: Requirements 6.2**
    const ScopedEnv env("KOG_DEBUG_PLAN", "1");

    const auto output = BuildDefaultPlanTemplate(std::filesystem::temp_directory_path());

    REQUIRE_FALSE(output.empty());
    REQUIRE(IsPretty(output));
    REQUIRE_FALSE(IsCompact(output));
}

TEST_CASE("SerializePlanJson produces compact output for unrecognised KOG_DEBUG_PLAN value",
          "[Unit][SerializePlanJson][req-6.3]") {
    // **Validates: Requirements 6.3**
    // "verbose" is not a recognised truthy value; compact output must be used.
    const ScopedEnv env("KOG_DEBUG_PLAN", "verbose");

    const auto output = BuildDefaultPlanTemplate(std::filesystem::temp_directory_path());

    REQUIRE_FALSE(output.empty());
    REQUIRE(IsCompact(output));
    REQUIRE_FALSE(IsPretty(output));
}
