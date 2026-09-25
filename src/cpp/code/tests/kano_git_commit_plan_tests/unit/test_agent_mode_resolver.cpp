// Table-driven unit test for the canonical agent-mode environment contract.
// See KOG-BUG-0137 for context. All KOG command paths must use
// IsAgentModeEnabled() and ResolveAgentModeEnvironment() from ai_utils.hpp
// instead of reimplementing env detection inline.

#include <catch2/catch_test_macros.hpp>

#include "ai_utils.hpp"

#include <cstdlib>
#include <optional>
#include <string>
#include <utility>

using namespace kano::git::commands;

namespace {

// RAII helper that snapshots an env var, clears it, and restores the
// original value on destruction. This isolates each TEST_CASE from any
// inherited environment (developer shell, CI runner, prior test, ...).
class ScopedEnv {
public:
    ScopedEnv(const char* InName)
        : name_(InName) {
        if (const char* current = std::getenv(name_); current != nullptr) {
            had_value_ = true;
            saved_ = current;
        }
#if defined(_WIN32)
        _putenv_s(name_, "");
#else
        ::unsetenv(name_);
#endif
    }
    ~ScopedEnv() {
        if (had_value_) {
#if defined(_WIN32)
            _putenv_s(name_, saved_.c_str());
#else
            ::setenv(name_, saved_.c_str(), 1);
#endif
        } else {
#if defined(_WIN32)
            _putenv_s(name_, "");
#else
            ::unsetenv(name_);
#endif
        }
    }

    void Set(const std::string& InValue) {
#if defined(_WIN32)
        _putenv_s(name_, InValue.c_str());
#else
        ::setenv(name_, InValue.c_str(), 1);
#endif
    }

    void Clear() {
#if defined(_WIN32)
        _putenv_s(name_, "");
#else
        ::unsetenv(name_);
#endif
    }

private:
    const char* name_;
    bool had_value_ = false;
    std::string saved_;
};

struct ResolverCase {
    std::string kano;
    std::optional<std::string> agent; // std::nullopt => unset
    bool expected;
    std::optional<std::string> expected_source; // std::nullopt => none
    const char* label;
};

const std::vector<ResolverCase>& AllCases() {
    static const std::vector<ResolverCase> cases = {
        // Unset / empty are always false.
        {"", std::nullopt, false, std::nullopt, "both unset"},
        {"", std::string{""}, false, std::nullopt, "empty-string values"},
        {"0", std::nullopt, false, std::nullopt, "explicit zero"},
        {"false", std::nullopt, false, std::nullopt, "lowercase false"},
        {"FALSE", std::nullopt, false, std::nullopt, "uppercase FALSE"},
        {"no", std::nullopt, false, std::nullopt, "no"},
        {"off", std::nullopt, false, std::nullopt, "off"},
        {"garbage", std::nullopt, false, std::nullopt, "garbage"},

        // KANO_AGENT_MODE truthy spellings.
        {"1", std::nullopt, true, std::string{"KANO_AGENT_MODE"}, "KANO=1"},
        {"true", std::nullopt, true, std::string{"KANO_AGENT_MODE"}, "KANO=true"},
        {"TRUE", std::nullopt, true, std::string{"KANO_AGENT_MODE"}, "KANO=TRUE"},
        {"yes", std::nullopt, true, std::string{"KANO_AGENT_MODE"}, "KANO=yes"},
        {"YES", std::nullopt, true, std::string{"KANO_AGENT_MODE"}, "KANO=YES"},
        {"on", std::nullopt, true, std::string{"KANO_AGENT_MODE"}, "KANO=on"},
        {" 1 ", std::nullopt, true, std::string{"KANO_AGENT_MODE"}, "KANO with surrounding whitespace"},

        // AGENT_MODE truthy spellings.
        {"", std::string{"1"}, true, std::string{"AGENT_MODE"}, "AGENT=1"},
        {"", std::string{"True"}, true, std::string{"AGENT_MODE"}, "AGENT=True mixed case"},
        {"", std::string{"YES"}, true, std::string{"AGENT_MODE"}, "AGENT=YES"},
        {"", std::string{"on"}, true, std::string{"AGENT_MODE"}, "AGENT=on"},

        // KANO_AGENT_MODE takes precedence over AGENT_MODE when both are truthy.
        {"1", std::string{"yes"}, true, std::string{"KANO_AGENT_MODE"}, "KANO=1 wins over AGENT=yes"},

        // When KANO_AGENT_MODE is false-like, AGENT_MODE may still resolve true.
        {"0", std::string{"1"}, true, std::string{"AGENT_MODE"}, "KANO=0 fallback AGENT=1"},
        {"false", std::string{"on"}, true, std::string{"AGENT_MODE"}, "KANO=false fallback AGENT=on"},
    };
    return cases;
}

} // namespace

TEST_CASE("IsAgentModeEnabled matches the canonical contract table",
          "[tdd][unit][feature:agent-mode-resolver][agent-mode][KOG-BUG-0137]") {
    ScopedEnv kano{"KANO_AGENT_MODE"};
    ScopedEnv agent{"AGENT_MODE"};

    for (const auto& c : AllCases()) {
        SECTION(c.label) {
            kano.Set(c.kano);
            if (c.agent.has_value()) {
                agent.Set(*c.agent);
            } else {
                agent.Clear();
            }
            REQUIRE(IsAgentModeEnabled() == c.expected);
            REQUIRE(ResolveAgentModeEnvironment().has_value() == c.expected);
            if (c.expected_source.has_value()) {
                REQUIRE(ResolveAgentModeEnvironment().value() == *c.expected_source);
            } else {
                REQUIRE_FALSE(ResolveAgentModeEnvironment().has_value());
            }
        }
    }
}

TEST_CASE("ResolveAgentModeEnvironment prefers KANO_AGENT_MODE over AGENT_MODE",
          "[tdd][unit][feature:agent-mode-resolver][agent-mode][KOG-BUG-0137]") {
    ScopedEnv kano{"KANO_AGENT_MODE"};
    ScopedEnv agent{"AGENT_MODE"};

    kano.Set("1");
    agent.Set("yes");
    REQUIRE(ResolveAgentModeEnvironment().value() == "KANO_AGENT_MODE");

    kano.Set("");
    agent.Set("1");
    REQUIRE(ResolveAgentModeEnvironment().value() == "AGENT_MODE");

    kano.Set("0");
    agent.Set("on");
    REQUIRE(ResolveAgentModeEnvironment().value() == "AGENT_MODE");

    kano.Set("");
    agent.Clear();
    REQUIRE_FALSE(ResolveAgentModeEnvironment().has_value());

    kano.Clear();
    agent.Clear();
    REQUIRE_FALSE(IsAgentModeEnabled());
    REQUIRE_FALSE(ResolveAgentModeEnvironment().has_value());
}