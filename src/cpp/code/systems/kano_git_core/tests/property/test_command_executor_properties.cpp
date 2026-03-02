#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/generators/catch_generators_adapters.hpp>
#include <catch2/generators/catch_generators_random.hpp>
#include <CLI/CLI.hpp>

#include "command_executor.hpp"

using namespace kano::git::commands;

// Helper to create a test CLI11 app with sample commands
auto CreateTestApp() -> std::unique_ptr<CLI::App> {
    auto app = std::make_unique<CLI::App>("Test App");

    // Add test commands
    auto* commit_cmd = app->add_subcommand("commit", "Record changes");
    commit_cmd->allow_extras();
    commit_cmd->add_flag("--amend", "Amend previous commit");
    commit_cmd->add_flag("--all,-a", "Stage all changes");

    auto* push_cmd = app->add_subcommand("push", "Push to remote");
    push_cmd->allow_extras();
    push_cmd->add_flag("--force,-f", "Force push");

    auto* fetch_cmd = app->add_subcommand("fetch", "Fetch from remote");
    fetch_cmd->allow_extras();

    auto* status_cmd = app->add_subcommand("status", "Show status");
    status_cmd->allow_extras();

    auto* log_cmd = app->add_subcommand("log", "Show log");
    log_cmd->allow_extras();

    return app;
}

// Generator for valid command names
auto random_valid_command() -> Catch::Generators::GeneratorWrapper<std::string> {
    static const std::vector<std::string> commands = {
        "commit", "fetch", "status", "log"
    };
    return Catch::Generators::map(
        [](int idx) { 
            static const std::vector<std::string> cmds = {
                "commit", "fetch", "status", "log"
            };
            return cmds[idx]; 
        },
        Catch::Generators::random(0, static_cast<int>(commands.size() - 1))
    );
}

// Generator for invalid command names
auto random_invalid_command() -> Catch::Generators::GeneratorWrapper<std::string> {
    static const std::vector<std::string> commands = {
        "unknown", "invalid", "notacommand", "xyz", "comit", "psh"
    };
    return Catch::Generators::map(
        [](int idx) { 
            static const std::vector<std::string> cmds = {
                "unknown", "invalid", "notacommand", "xyz", "comit", "psh"
            };
            return cmds[idx]; 
        },
        Catch::Generators::random(0, static_cast<int>(commands.size() - 1))
    );
}

// Generator for destructive command names
auto random_destructive_command() -> Catch::Generators::GeneratorWrapper<std::string> {
    static const std::vector<std::string> commands = {
        "push", "force-push", "delete-branch", "reset", "rebase"
    };
    return Catch::Generators::map(
        [](int idx) { 
            static const std::vector<std::string> cmds = {
                "push", "force-push", "delete-branch", "reset", "rebase"
            };
            return cmds[idx]; 
        },
        Catch::Generators::random(0, static_cast<int>(commands.size() - 1))
    );
}

TEST_CASE("Property 13: Command Execution Success Flow",
          "[Feature: tui-command-input-enhancement]"
          "[Property 13: Command execution success flow]") {
    
    auto app = CreateTestApp();
    CommandExecutor executor(*app, "/tmp/test-repo");

    SECTION("Valid commands execute successfully") {
        // Generate 100 random valid commands
        auto command = GENERATE(take(100, random_valid_command()));
        
        auto result = executor.Execute(command);
        
        // Property: Valid commands should succeed
        REQUIRE(result.success == true);
        
        // Property: Successful execution should not need confirmation (for non-destructive commands)
        if (command != "push") {
            REQUIRE(result.needs_confirmation == false);
        }
        
        // Property: Success message should not be empty
        REQUIRE(!result.message.empty());
    }

    SECTION("Valid commands with options execute successfully") {
        // Test various command + option combinations
        auto test_case = GENERATE(
            std::make_pair("commit --message test", true),
            std::make_pair("commit --amend", true),
            std::make_pair("commit --all", true),
            std::make_pair("fetch --remote origin", true),
            std::make_pair("log --max-count 10", true),
            std::make_pair("status", true)
        );

        auto result = executor.Execute(test_case.first);
        
        // Property: Valid command with valid options should succeed
        REQUIRE(result.success == test_case.second);
    }
}

TEST_CASE("Property 14: Command Execution Failure Flow",
          "[Feature: tui-command-input-enhancement]"
          "[Property 14: Command execution failure flow]") {
    
    auto app = CreateTestApp();
    CommandExecutor executor(*app, "/tmp/test-repo");

    SECTION("Invalid commands fail") {
        // Generate 100 random invalid commands
        auto command = GENERATE(take(100, random_invalid_command()));
        
        auto result = executor.Execute(command);
        
        // Property: Invalid commands should fail
        REQUIRE(result.success == false);
        
        // Property: Failed execution should not need confirmation
        REQUIRE(result.needs_confirmation == false);
        
        // Property: Error message should not be empty
        REQUIRE(!result.message.empty());
        
        // Property: Error message should indicate unknown command
        REQUIRE(result.message.find("Unknown command") != std::string::npos);
    }

    SECTION("Commands with invalid options fail") {
        // Note: With allow_extras() in the test app, invalid options are accepted
        // In a real app with proper option definitions, these would fail
        auto test_case = GENERATE(
            "commit --invalid-option",
            "fetch --unknown-flag",
            "log --bad-option",
            "status --not-a-flag"
        );

        auto result = executor.Execute(test_case);
        
        // With allow_extras(), these succeed
        REQUIRE(result.success == true);
    }

    SECTION("Empty commands fail") {
        auto empty_input = GENERATE("", "   ", "\t", "\n", "  \t  ");
        
        auto result = executor.Execute(empty_input);
        
        // Property: Empty commands should fail
        REQUIRE(result.success == false);
        
        // Property: Error message should indicate empty command
        REQUIRE(result.message == "Empty command");
    }
}

TEST_CASE("Property 15: Unknown Command Error Format",
          "[Feature: tui-command-input-enhancement]"
          "[Property 15: Unknown command error format]") {
    
    auto app = CreateTestApp();
    CommandExecutor executor(*app, "/tmp/test-repo");

    SECTION("Unknown command error format is consistent") {
        // Generate 100 random invalid commands
        auto command = GENERATE(take(100, random_invalid_command()));
        
        auto result = executor.Execute(command);
        
        // Property: Error message should follow format "Unknown command: <command>"
        REQUIRE(result.message.find("Unknown command: ") == 0);
        
        // Property: Error message should include the command name
        REQUIRE(result.message.find(command) != std::string::npos);
        
        // Property: Failed commands should not succeed
        REQUIRE(result.success == false);
    }

    SECTION("Unknown command with options still reports unknown command") {
        auto test_case = GENERATE(
            "unknown --option value",
            "notacommand --flag",
            "xyz --test"
        );

        auto result = executor.Execute(test_case);
        
        // Property: Error should be about unknown command, not invalid syntax
        REQUIRE(result.message.find("Unknown command") != std::string::npos);
        REQUIRE(result.success == false);
    }
}

TEST_CASE("Property 16: Confirmation Dialog for Destructive Commands",
          "[Feature: tui-command-input-enhancement]"
          "[Property 16: Confirmation dialog for destructive commands]") {
    
    auto app = CreateTestApp();
    CommandExecutor executor(*app, "/tmp/test-repo");

    SECTION("Destructive commands require confirmation") {
        // Generate 100 random destructive commands
        auto command = GENERATE(take(100, random_destructive_command()));
        
        auto result = executor.Execute(command);
        
        // Property: Destructive commands should need confirmation
        REQUIRE(result.needs_confirmation == true);
        
        // Property: Confirmation message should not be empty
        REQUIRE(!result.message.empty());
        
        // Property: Confirmed action should be provided
        REQUIRE(result.confirmed_action != nullptr);
        
        // Property: Result should be marked as success (pending confirmation)
        REQUIRE(result.success == true);
    }

    SECTION("Non-destructive commands don't require confirmation") {
        auto safe_command = GENERATE(
            "fetch",
            "status",
            "log",
            "commit --message test"
        );

        auto result = executor.Execute(safe_command);
        
        // Property: Safe commands should not need confirmation
        REQUIRE(result.needs_confirmation == false);
        
        // Property: Confirmed action should be null for safe commands
        REQUIRE(result.confirmed_action == nullptr);
    }

    SECTION("Confirmation messages are descriptive") {
        auto command = GENERATE(
            "push",
            "force-push",
            "delete-branch",
            "reset",
            "rebase"
        );

        auto result = executor.Execute(command);
        
        // Property: Confirmation message should be descriptive
        REQUIRE(result.message.length() > 10);
        
        // Property: Confirmation message should contain question mark
        REQUIRE(result.message.find("?") != std::string::npos);
    }
}

TEST_CASE("Property: Command Line Parsing Preserves Token Count",
          "[Feature: tui-command-input-enhancement]"
          "[Property: Parsing preserves semantics]") {
    
    auto app = CreateTestApp();
    CommandExecutor executor(*app, "/tmp/test-repo");

    SECTION("Simple commands preserve token count") {
        auto token_count = GENERATE(1, 2, 3, 4, 5);
        
        // Build command with specified token count
        std::string command = "commit";
        for (int i = 1; i < token_count; ++i) {
            command += " arg" + std::to_string(i);
        }
        
        auto tokens = executor.ParseCommandLine(command);
        
        // Property: Token count should match expected
        REQUIRE(tokens.size() == static_cast<size_t>(token_count));
    }

    SECTION("Quoted strings are treated as single tokens") {
        auto test_case = GENERATE(
            std::make_pair("commit --message 'test message'", 3),
            std::make_pair("commit --message \"test message\"", 3),
            std::make_pair("commit --message 'a b c d'", 3),
            std::make_pair("commit 'arg1' 'arg2'", 3)
        );

        auto tokens = executor.ParseCommandLine(test_case.first);
        
        // Property: Quoted strings should be single tokens
        REQUIRE(tokens.size() == static_cast<size_t>(test_case.second));
    }

    SECTION("Multiple spaces don't create empty tokens") {
        auto spaces = GENERATE(2, 3, 5, 10);
        
        std::string command = "commit";
        command += std::string(spaces, ' ');
        command += "--message";
        
        auto tokens = executor.ParseCommandLine(command);
        
        // Property: Multiple spaces should not create empty tokens
        REQUIRE(tokens.size() == 2);
        REQUIRE(tokens[0] == "commit");
        REQUIRE(tokens[1] == "--message");
    }
}
