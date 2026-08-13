#include <catch2/catch_test_macros.hpp>

#include "tui_command_scope.hpp"

#include <array>
#include <filesystem>
#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

namespace {

using kano::git::commands::BuildTuiCommandScopeLabel;
using kano::git::commands::BuildTuiAuditCommand;
using kano::git::commands::BuildTuiScopedCommand;
using kano::git::commands::IsTuiAuditOnlyCommand;
using kano::git::commands::ParseTuiCommandLine;
using kano::git::commands::TuiCommandScopeMode;
using kano::git::commands::TuiCommandScopeSnapshot;

auto WorkspaceScope() -> TuiCommandScopeSnapshot {
    return {
        .mode = TuiCommandScopeMode::Workspace,
        .workspaceRoot = "/workspace/root",
        .selectedRepoPath = "/external/actual/repo",
        .selectedRepoDisplay = "../external/repo",
    };
}

TEST_CASE(
    "TUI command mode allows audit commands and rejects mutations",
    "[unit][tui_command_scope][tui_pr_focus][KG-BUG-0088]") {
    for (const auto command : {
             "status", "log", "slog", "doctor", "version", "help",
             "--help", "-h", "--version"}) {
        CAPTURE(command);
        REQUIRE(IsTuiAuditOnlyCommand({command}));
    }

    for (const auto command : {
             "commit", "ca", "commit-push", "cp", "cpa", "push",
             "amend", "fetch", "converge", "init", "PUSH"}) {
        CAPTURE(command);
        REQUIRE_FALSE(IsTuiAuditOnlyCommand({command}));
    }
    REQUIRE_FALSE(IsTuiAuditOnlyCommand(
        {"doctor", "--fix-safe-directory"}));
    REQUIRE_FALSE(IsTuiAuditOnlyCommand(
        {"status", "--output", "/tmp/tui-status.json"}));
    REQUIRE_FALSE(IsTuiAuditOnlyCommand(
        {"status", "--refresh-cache"}));
    REQUIRE_FALSE(IsTuiAuditOnlyCommand(
        {"log", "--repo-root", "/tmp/workspace", "repo"}));
    REQUIRE_FALSE(IsTuiAuditOnlyCommand({}));
}

TEST_CASE(
    "TUI audit command builder rejects user options and forces no-cache discovery",
    "[unit][tui_command_scope][KG-BUG-0088]") {
    REQUIRE_FALSE(BuildTuiAuditCommand(
        "doctor --fix-safe-directory", WorkspaceScope()));
    REQUIRE_FALSE(BuildTuiAuditCommand(
        "status --output /tmp/report", WorkspaceScope()));

    const auto status = BuildTuiAuditCommand(
        "status", WorkspaceScope());
    REQUIRE(status.has_value());
    REQUIRE(status->arguments ==
            std::vector<std::string>{"status", "--no-cache"});

    auto selectedScope = WorkspaceScope();
    selectedScope.mode = TuiCommandScopeMode::SelectedRepo;
    const auto log = BuildTuiAuditCommand(
        "log", selectedScope);
    REQUIRE(log.has_value());
    REQUIRE(log->arguments == std::vector<std::string>{
        "log",
        "--no-cache",
        "--repo-root",
        "/workspace/root",
        "../external/repo",
    });

    const auto doctor = BuildTuiAuditCommand(
        "doctor", WorkspaceScope());
    REQUIRE(doctor.has_value());
    REQUIRE(doctor->arguments ==
            std::vector<std::string>{"doctor"});
}

TEST_CASE(
    "TUI audit command builder admits only closed structured verification",
    "[unit][tui_command_scope][KG-TSK-0132]") {
    const auto scope = WorkspaceScope();
    const auto command = BuildTuiAuditCommand(
        "audit verify --plan-file \"plans/audit plan.json\" "
        "--run-id run-0132 --attempt 7 --json",
        scope);
    REQUIRE(command.has_value());
    REQUIRE(command->auditVerification.has_value());
    CHECK(command->auditVerification->planFile ==
          std::filesystem::path("plans/audit plan.json"));
    CHECK(command->auditVerification->runId == "run-0132");
    CHECK(command->auditVerification->attempt == 7);

    auto selectedScope = scope;
    selectedScope.mode = TuiCommandScopeMode::SelectedRepo;
    const auto selectedCommand = BuildTuiAuditCommand(
        "audit verify --plan-file plan.json --run-id run-0132 "
        "--attempt 7 --json",
        selectedScope);
    REQUIRE(selectedCommand.has_value());
    CHECK(selectedCommand->workingDirectory == scope.workspaceRoot);
    CHECK(selectedCommand->scopeLabel == "workspace: /workspace/root");

    CHECK_FALSE(BuildTuiAuditCommand(
        "audit verify --run-id run-0132 --plan-file plan.json "
        "--attempt 7 --json", scope).has_value());
    CHECK_FALSE(BuildTuiAuditCommand(
        "audit verify --plan-file plan.json --run-id ../escape "
        "--attempt 7 --json", scope).has_value());
    CHECK_FALSE(BuildTuiAuditCommand(
        "audit verify --plan-file plan.json --run-id run-0132 "
        "--attempt 0 --json", scope).has_value());
    CHECK_FALSE(BuildTuiAuditCommand(
        "audit verify --plan-file plan.json --run-id run-0132 "
        "--attempt 7 --json --extra", scope).has_value());
}

auto SelectedScope() -> TuiCommandScopeSnapshot {
    auto scope = WorkspaceScope();
    scope.mode = TuiCommandScopeMode::SelectedRepo;
    return scope;
}

auto RequireTokens(
    const std::string_view InLine,
    const std::initializer_list<std::string_view> InExpected)
    -> void {
    const auto actual = ParseTuiCommandLine(InLine);
    REQUIRE(actual.size() == InExpected.size());
    std::size_t index = 0;
    for (const auto expected : InExpected) {
        CAPTURE(index);
        REQUIRE(actual[index] == std::string(expected));
        ++index;
    }
}

}  // namespace

TEST_CASE(
    "TUI command scope parser preserves current quote and escape behavior",
    "[unit][tui_command_scope][KG-TSK-0070]") {
    RequireTokens(
        R"(commit --message 'Fix bug')",
        {"commit", "--message", "Fix bug"});
    RequireTokens(
        R"(commit --message "Fix bug")",
        {"commit", "--message", "Fix bug"});
    RequireTokens(
        R"(commit --message=test\ value)",
        {"commit", "--message=test value"});
    RequireTokens(
        R"(commit \"quoted\")",
        {"commit", R"("quoted")"});
    RequireTokens(
        R"(commit path\\name)",
        {"commit", R"(path\name)"});
    RequireTokens(
        R"(commit path\)",
        {"commit", "path"});
    RequireTokens(R"(\)", {});
    RequireTokens(
        R"(commit "" tail)",
        {"commit", "tail"});
    RequireTokens(
        R"(commit "unterminated value)",
        {"commit", "unterminated value"});
    RequireTokens(
        R"(commit "a'b c")",
        {"commit", "ab", "c"});
    RequireTokens("", {});
    RequireTokens("   \t", {});
    RequireTokens(R"("")", {});
}

TEST_CASE(
    "TUI workspace command scope preserves cwd arguments and label",
    "[unit][tui_command_scope][KG-TSK-0070]") {
    const auto scope = WorkspaceScope();
    const auto command = BuildTuiScopedCommand(
        "log --max-count 2",
        scope);

    REQUIRE(command.has_value());
    REQUIRE(
        command->workingDirectory ==
        std::filesystem::path("/workspace/root"));
    const std::vector<std::string> expected{
        "log",
        "--max-count",
        "2",
    };
    REQUIRE(command->arguments == expected);
    REQUIRE(command->scopeLabel == "workspace: /workspace/root");
    REQUIRE(
        BuildTuiCommandScopeLabel(scope) ==
        command->scopeLabel);
}

TEST_CASE(
    "TUI selected scope injects every supported command target",
    "[unit][tui_command_scope][KG-TSK-0070]") {
    const auto scope = SelectedScope();
    constexpr std::array<std::string_view, 9> commands{
        "commit",
        "ca",
        "commit-push",
        "cp",
        "cpa",
        "push",
        "log",
        "slog",
        "amend",
    };

    for (const auto commandName : commands) {
        CAPTURE(std::string(commandName));
        const auto command = BuildTuiScopedCommand(
            commandName,
            scope);
        REQUIRE(command.has_value());
        REQUIRE(
            command->workingDirectory ==
            std::filesystem::path("/external/actual/repo"));
        const std::vector<std::string> expected{
            std::string(commandName),
            "--repo-root",
            "/workspace/root",
            "../external/repo",
        };
        REQUIRE(command->arguments == expected);
        REQUIRE(
            command->scopeLabel ==
            "selected: ../external/repo");
    }
}

TEST_CASE(
    "TUI selected scope respects every explicit target option form",
    "[unit][tui_command_scope][KG-TSK-0070]") {
    const auto scope = SelectedScope();
    constexpr std::array<std::string_view, 6> options{
        "--repos",
        "--repo",
        "--repo-root",
        "--root",
        "--source",
        "--target",
    };

    for (const auto option : options) {
        CAPTURE(std::string(option));

        const auto separate = BuildTuiScopedCommand(
            "commit " + std::string(option) + " value",
            scope);
        REQUIRE(separate.has_value());
        const std::vector<std::string> separateExpected{
            "commit",
            std::string(option),
            "value",
        };
        REQUIRE(separate->arguments == separateExpected);

        const auto equals = BuildTuiScopedCommand(
            "commit " + std::string(option) + "=value",
            scope);
        REQUIRE(equals.has_value());
        const std::vector<std::string> equalsExpected{
            "commit",
            std::string(option) + "=value",
        };
        REQUIRE(equals->arguments == equalsExpected);

        const auto missingValue = BuildTuiScopedCommand(
            "commit " + std::string(option),
            scope);
        REQUIRE(missingValue.has_value());
        const std::vector<std::string> missingValueExpected{
            "commit",
            std::string(option),
        };
        REQUIRE(
            missingValue->arguments ==
            missingValueExpected);
    }

    const auto nearMatch = BuildTuiScopedCommand(
        "commit --repository=value",
        scope);
    REQUIRE(nearMatch.has_value());
    const std::vector<std::string> nearMatchExpected{
        "commit",
        "--repository=value",
        "--repo-root",
        "/workspace/root",
        "../external/repo",
    };
    REQUIRE(nearMatch->arguments == nearMatchExpected);
}

TEST_CASE(
    "TUI selected scope keeps unsupported commands case sensitive",
    "[unit][tui_command_scope][KG-TSK-0070]") {
    const auto scope = SelectedScope();

    const auto fetch = BuildTuiScopedCommand(
        "fetch --all",
        scope);
    REQUIRE(fetch.has_value());
    REQUIRE(
        fetch->workingDirectory ==
        std::filesystem::path("/external/actual/repo"));
    const std::vector<std::string> fetchExpected{
        "fetch",
        "--all",
    };
    REQUIRE(fetch->arguments == fetchExpected);

    const auto uppercase = BuildTuiScopedCommand(
        "PUSH",
        scope);
    REQUIRE(uppercase.has_value());
    const std::vector<std::string> uppercaseExpected{
        "PUSH",
    };
    REQUIRE(uppercase->arguments == uppercaseExpected);
}

TEST_CASE(
    "TUI command scope preserves empty and no-selection behavior",
    "[unit][tui_command_scope][KG-TSK-0070]") {
    REQUIRE_FALSE(
        BuildTuiScopedCommand("", WorkspaceScope())
            .has_value());
    REQUIRE_FALSE(
        BuildTuiScopedCommand("   ", SelectedScope())
            .has_value());
    REQUIRE_FALSE(
        BuildTuiScopedCommand(R"("")", SelectedScope())
            .has_value());

    auto noSelection = SelectedScope();
    noSelection.selectedRepoPath =
        noSelection.workspaceRoot;
    noSelection.selectedRepoDisplay = "(none)";
    const auto command = BuildTuiScopedCommand(
        "commit",
        noSelection);
    REQUIRE(command.has_value());
    REQUIRE(
        command->workingDirectory ==
        noSelection.workspaceRoot);
    REQUIRE(command->scopeLabel == "selected: (none)");
    const std::vector<std::string> expected{
        "commit",
        "--repo-root",
        "/workspace/root",
        "(none)",
    };
    REQUIRE(command->arguments == expected);
}
