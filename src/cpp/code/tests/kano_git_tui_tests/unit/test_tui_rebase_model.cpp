#include <catch2/catch_test_macros.hpp>

#include "tui_rebase_model.hpp"

#include <filesystem>
#include <string>
#include <vector>

using kano::git::commands::BuildRebasePlanPreview;
using kano::git::commands::BuildRebasePlanner;
using kano::git::commands::RebasePlanItem;
using kano::git::commands::RebasePlannerState;
using kano::git::commands::RebasePreflightState;

TEST_CASE(
    "TUI rebase planner preserves upstream and candidate parsing",
    "[unit][tui_rebase_model][KG-TSK-0070]") {
    RebasePreflightState preflight;
    preflight.branch = "feature/tui";
    preflight.upstream = "origin/main";
    preflight.candidates = {
        "abc123 first commit",
        "malformed",
        "def456 second commit with spaces",
    };

    const auto planner = BuildRebasePlanner(
        std::filesystem::path("workspace/repo/../repo"),
        preflight);

    REQUIRE(planner.active);
    REQUIRE(planner.repo == std::filesystem::path("workspace/repo/../repo"));
    REQUIRE(planner.baseRef == "origin/main");
    REQUIRE(planner.selectedIndex == 0);
    REQUIRE(planner.items.size() == 2);
    REQUIRE(planner.items[0].sha == "abc123");
    REQUIRE(planner.items[0].title == "first commit");
    REQUIRE(planner.items[0].action == "pick");
    REQUIRE(planner.items[1].sha == "def456");
    REQUIRE(planner.items[1].title == "second commit with spaces");
    REQUIRE(
        planner.preview ==
        "Rebase Plan Preview\n"
        "repo: workspace/repo\n"
        "base: origin/main\n\n"
        "# interactive todo style\n"
        "pick abc123 first commit\n"
        "pick def456 second commit with spaces\n");
}

TEST_CASE(
    "TUI rebase planner preserves current fallback base rules",
    "[unit][tui_rebase_model][KG-TSK-0070]") {
    RebasePreflightState mainPreflight;
    mainPreflight.branch = "main";
    mainPreflight.upstream = "(none)";
    REQUIRE(
        BuildRebasePlanner("/repo", mainPreflight).baseRef ==
        "main");

    RebasePreflightState featurePreflight;
    featurePreflight.branch = "feature/tui";
    featurePreflight.upstream = "(none)";
    REQUIRE(
        BuildRebasePlanner("/repo", featurePreflight).baseRef ==
        "master");
}

TEST_CASE(
    "TUI rebase plan preview renders every supported action",
    "[unit][tui_rebase_model][KG-TSK-0070]") {
    RebasePlannerState planner;
    planner.repo = "/repo";
    planner.baseRef = "origin/main";
    planner.items = {
        RebasePlanItem{
            .sha = "1111111",
            .title = "pick title",
            .action = "pick",
        },
        RebasePlanItem{
            .sha = "2222222",
            .title = "squash title",
            .action = "squash",
        },
        RebasePlanItem{
            .sha = "3333333",
            .title = "fixup title",
            .action = "fixup",
        },
        RebasePlanItem{
            .sha = "4444444",
            .title = "drop title",
            .action = "drop",
        },
    };

    REQUIRE(
        BuildRebasePlanPreview(planner) ==
        "Rebase Plan Preview\n"
        "repo: /repo\n"
        "base: origin/main\n\n"
        "# interactive todo style\n"
        "pick 1111111 pick title\n"
        "squash 2222222 squash title\n"
        "fixup 3333333 fixup title\n"
        "drop 4444444 drop title\n");
}

TEST_CASE(
    "TUI rebase plan preview preserves the empty-plan sentinel",
    "[unit][tui_rebase_model][KG-TSK-0070]") {
    RebasePlannerState planner;
    planner.repo = "/repo";
    planner.baseRef = "main";

    REQUIRE(
        BuildRebasePlanPreview(planner) ==
        "Rebase Plan Preview\n"
        "repo: /repo\n"
        "base: main\n\n"
        "# interactive todo style\n"
        "(no plan items)\n");
}
