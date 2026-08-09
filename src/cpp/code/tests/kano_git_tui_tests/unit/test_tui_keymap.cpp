#include <catch2/catch_test_macros.hpp>

#include "tui_keymap.hpp"

#include <string>
#include <string_view>

TEST_CASE("TUI keymap exposes canonical normal history and detail guidance",
          "[tdd][unit][feature:tui-key-guidance][KG-BUG-0088]") {
    using namespace kano::git::commands;

    const auto guidance = GetAllTuiKeyGuidance();
    REQUIRE(guidance.size() == 7);

    CHECK(guidance[0].context == TuiKeyContext::Normal);
    CHECK(std::string(guidance[0].label) == "normal");
    CHECK(std::string(guidance[0].controls) ==
          "Up/Down or j/k select | Enter history | r refresh selected repo | : audit | ? help | q quit");
    CHECK(std::string(guidance[0].compactControls) == "? help | q quit");

    CHECK(guidance[1].context == TuiKeyContext::History);
    CHECK(std::string(guidance[1].label) == "history");
    CHECK(std::string(guidance[1].controls) ==
          "Up/Down or j/k select | Left/Right page | / search | n next | o page order | Enter detail | [ previous repo | ] next repo | ? help | Esc/q back");
    CHECK(std::string(guidance[1].compactControls) == "Enter detail | Esc/q back");

    CHECK(guidance[2].context == TuiKeyContext::Detail);
    CHECK(std::string(guidance[2].label) == "detail");
    CHECK(std::string(guidance[2].controls) ==
          "Up/Down or j/k change | Left/Right page | m summary/patch | ? help | Esc/q back");
    CHECK(std::string(guidance[2].compactControls) == "? help | Esc/q back");

    CHECK(guidance[3].context == TuiKeyContext::Discover);
    CHECK(std::string(guidance[3].controls) ==
          "[ or PgDown prev page | ] or PgUp next page | Esc/q close");
    CHECK(std::string(guidance[3].compactControls) == "[ prev | ] next | Esc/q close");
    CHECK(guidance[4].context == TuiKeyContext::Preview);
    CHECK(std::string(guidance[4].controls) == "Esc/q close");
    CHECK(std::string(guidance[4].compactControls) == "Esc/q close");
    CHECK(guidance[5].context == TuiKeyContext::Command);
    CHECK(std::string(guidance[5].controls) ==
          "g scope | Tab complete | Up/Down candidates | Enter inspect | Esc cancel");
    CHECK(std::string(guidance[5].compactControls) == "Enter inspect | Esc cancel");
    CHECK(guidance[6].context == TuiKeyContext::Palette);
    CHECK(std::string(guidance[6].controls) ==
          "Up/Down select | Enter inspect | Esc close");
    CHECK(std::string(guidance[6].compactControls) ==
          "Enter inspect | Esc close");

    CHECK(&GetTuiKeyGuidance(TuiKeyContext::Normal) == &guidance[0]);
    CHECK(&GetTuiKeyGuidance(TuiKeyContext::History) == &guidance[1]);
    CHECK(&GetTuiKeyGuidance(TuiKeyContext::Detail) == &guidance[2]);
    CHECK(&GetTuiKeyGuidance(TuiKeyContext::Discover) == &guidance[3]);
    CHECK(&GetTuiKeyGuidance(TuiKeyContext::Preview) == &guidance[4]);
    CHECK(&GetTuiKeyGuidance(TuiKeyContext::Command) == &guidance[5]);
    CHECK(&GetTuiKeyGuidance(TuiKeyContext::Palette) == &guidance[6]);
}

TEST_CASE("TUI keymap exposes page-scoped history search and ordering controls",
          "[tdd][unit][feature:tui-key-guidance][KG-BUG-0097]") {
    using kano::git::commands::GetTuiKeyGuidance;
    using kano::git::commands::TuiKeyContext;

    const auto controls =
        GetTuiKeyGuidance(TuiKeyContext::History).controls;
    CHECK(controls.find("/ search") != std::string_view::npos);
    CHECK(controls.find("n next") != std::string_view::npos);
    CHECK(controls.find("o page order") != std::string_view::npos);
    CHECK(controls.find("sort") == std::string_view::npos);
}

TEST_CASE("TUI keymap omits retired fetch commit and push shortcut claims",
          "[tdd][unit][feature:tui-key-guidance][KG-BUG-0088]") {
    using kano::git::commands::GetAllTuiKeyGuidance;

    for (const auto& guidance : GetAllTuiKeyGuidance()) {
        INFO("context=" << std::string(guidance.label));
        CHECK(guidance.controls.find("f fetch") == std::string_view::npos);
        CHECK(guidance.controls.find("c/C commit") == std::string_view::npos);
        CHECK(guidance.controls.find("p/P push") == std::string_view::npos);
    }
}
