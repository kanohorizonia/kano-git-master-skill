#include <catch2/catch_test_macros.hpp>

#include "tui_load_state.hpp"

#include <string>

using namespace kano::git::commands;

TEST_CASE(
    "TUI load lifecycle exposes every audit state",
    "[unit][tui_load_state][KG-BUG-0091]") {
    TuiLoadState state;
    REQUIRE(std::string(TuiLoadPhaseName(state.phase)) == "idle");

    BeginTuiLoad(state, 7, "startup discovery", ":refresh retry | q exit");
    REQUIRE(std::string(TuiLoadPhaseName(state.phase)) == "loading");
    REQUIRE(state.operation == "startup discovery");

    REQUIRE(CompleteTuiLoad(state, 7, 3));
    REQUIRE(std::string(TuiLoadPhaseName(state.phase)) == "ready");

    BeginTuiLoad(state, 8, "history page", "r retry | Esc return");
    REQUIRE(CompleteTuiLoad(state, 8, 0));
    REQUIRE(std::string(TuiLoadPhaseName(state.phase)) == "empty");

    BeginTuiLoad(state, 9, "history detail", "Enter retry | Esc return");
    REQUIRE(FailTuiLoad(state, 9, "cancelled by selection", true));
    REQUIRE(std::string(TuiLoadPhaseName(state.phase)) == "cancelled");

    BeginTuiLoad(state, 10, "discover", ":discover retry | q close");
    REQUIRE(FailTuiLoad(state, 10, "git failed", false));
    REQUIRE(std::string(TuiLoadPhaseName(state.phase)) == "failed");
    REQUIRE(state.diagnostic == "git failed");
}

TEST_CASE(
    "TUI load lifecycle rejects stale completion generations",
    "[unit][tui_load_state][KG-BUG-0091]") {
    TuiLoadState state;
    BeginTuiLoad(state, 41, "history page", "r retry");
    BeginTuiLoad(state, 42, "history page", "r retry");

    REQUIRE_FALSE(CompleteTuiLoad(state, 41, 5));
    REQUIRE_FALSE(FailTuiLoad(state, 41, "stale failure", false));
    REQUIRE(IsCurrentTuiLoad(state, 42));
    REQUIRE(CompleteTuiLoad(state, 42, 1));
    REQUIRE(state.phase == TuiLoadPhase::Ready);
}

TEST_CASE(
    "TUI load diagnostics are single-line bounded and UTF-8 safe",
    "[unit][tui_load_state][KG-BUG-0091]") {
    std::string diagnostic = "detail\nfailed\t";
    diagnostic.append(400, 'x');
    diagnostic += "人";

    const auto bounded = BoundTuiLoadDiagnostic(diagnostic);
    REQUIRE(bounded.size() <= kTuiLoadDiagnosticMaxBytes);
    REQUIRE(bounded.find('\n') == std::string::npos);
    REQUIRE(bounded.find('\t') == std::string::npos);
    REQUIRE(bounded.ends_with("... [bounded]"));
}
