#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/generators/catch_generators_adapters.hpp>
#include <catch2/generators/catch_generators_range.hpp>
#include <catch2/generators/catch_generators_random.hpp>
#include "tui_state.hpp"
#include "command_string_generator.hpp"
#include <random>
#include <string>

using namespace kano::git::commands;
using namespace tui_test;

TEST_CASE("Property 1: Mode Transition Correctness", 
          "[Feature: tui-command-input-enhancement]"
          "[Property 1: Mode transition round-trip]"
          "[tui_state][property]") {
    
    // Generate random input sequences
    auto random_input = GENERATE(take(100, random_command_string()));
    
    TuiState state;
    REQUIRE(state.GetMode() == TuiMode::Normal);
    
    // Enter Command_Mode
    state.HandleEvent("character", ':');
    REQUIRE(state.GetMode() == TuiMode::Command);
    REQUIRE(state.command_state.GetBuffer().empty());
    
    // Type random input
    for (char ch : random_input) {
        if (ch >= 32 && ch <= 126) {  // Printable ASCII
            state.HandleEvent("character", ch);
        }
    }
    
    // Buffer should contain the input
    REQUIRE_FALSE(state.command_state.GetBuffer().empty());
    
    // Exit via Escape
    state.HandleEvent("escape");
    
    // Should return to Normal_Mode with cleared buffer
    REQUIRE(state.GetMode() == TuiMode::Normal);
    REQUIRE(state.command_state.GetBuffer().empty());
    
    // Enter Command_Mode again
    state.HandleEvent("character", ':');
    REQUIRE(state.GetMode() == TuiMode::Command);
    
    // Buffer should still be empty (clean state)
    REQUIRE(state.command_state.GetBuffer().empty());
}

TEST_CASE("Property 1b: Mode Transition via Enter with empty buffer", 
          "[Feature: tui-command-input-enhancement]"
          "[Property 1: Mode transition round-trip]"
          "[tui_state][property]") {
    
    // Run multiple iterations
    auto iteration = GENERATE(range(0, 100));
    (void)iteration;  // Suppress unused warning
    
    TuiState state;
    REQUIRE(state.GetMode() == TuiMode::Normal);
    
    // Enter Command_Mode
    state.HandleEvent("character", ':');
    REQUIRE(state.GetMode() == TuiMode::Command);
    
    // Exit via Enter with empty buffer
    state.HandleEvent("enter");
    
    // Should return to Normal_Mode
    REQUIRE(state.GetMode() == TuiMode::Normal);
    REQUIRE(state.command_state.GetBuffer().empty());
}

TEST_CASE("Property 1c: Mode Transition with buffer operations", 
          "[Feature: tui-command-input-enhancement]"
          "[Property 1: Mode transition round-trip]"
          "[tui_state][property]") {
    
    auto random_input = GENERATE(take(100, random_command_string()));
    
    TuiState state;
    
    // Enter Command_Mode
    state.HandleEvent("character", ':');
    
    // Type random input
    for (char ch : random_input) {
        if (ch >= 32 && ch <= 126) {
            state.HandleEvent("character", ch);
        }
    }
    
    // Perform random buffer operations
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> op_dist(0, 3);
    
    for (int i = 0; i < 10; ++i) {
        int op = op_dist(gen);
        switch (op) {
            case 0:
                state.HandleEvent("backspace");
                break;
            case 1:
                state.HandleEvent("left");
                break;
            case 2:
                state.HandleEvent("right");
                break;
            case 3:
                state.HandleEvent("character", 'x');
                break;
        }
    }
    
    // Exit and re-enter
    state.HandleEvent("escape");
    REQUIRE(state.GetMode() == TuiMode::Normal);
    REQUIRE(state.command_state.GetBuffer().empty());
    
    state.HandleEvent("character", ':');
    REQUIRE(state.GetMode() == TuiMode::Command);
    REQUIRE(state.command_state.GetBuffer().empty());
}

TEST_CASE("Property 1d: Footer message clears on mode transitions", 
          "[Feature: tui-command-input-enhancement]"
          "[Property 1: Mode transition round-trip]"
          "[tui_state][property]") {
    
    auto random_message = GENERATE(take(50, random_command_string()));
    
    TuiState state;
    
    // Set a footer message
    state.SetFooterMessage(random_message, false);
    REQUIRE(state.footer_message == random_message);
    
    // Enter Command_Mode - footer should clear
    state.HandleEvent("character", ':');
    REQUIRE(state.footer_message.empty());
    
    // Set another message
    state.SetFooterMessage(random_message, true);
    REQUIRE(state.footer_is_error);
    
    // Exit Command_Mode - footer should clear
    state.HandleEvent("escape");
    REQUIRE(state.footer_message.empty());
    REQUIRE_FALSE(state.footer_is_error);
}

TEST_CASE("Property 1e: Multiple mode transitions maintain consistency", 
          "[Feature: tui-command-input-enhancement]"
          "[Property 1: Mode transition round-trip]"
          "[tui_state][property]") {
    
    auto num_transitions = GENERATE(take(20, random(5, 50)));
    
    TuiState state;
    
    for (int i = 0; i < num_transitions; ++i) {
        // Enter Command_Mode
        REQUIRE(state.GetMode() == TuiMode::Normal);
        state.HandleEvent("character", ':');
        REQUIRE(state.GetMode() == TuiMode::Command);
        REQUIRE(state.command_state.GetBuffer().empty());
        
        // Type something
        state.HandleEvent("character", 'a');
        REQUIRE_FALSE(state.command_state.GetBuffer().empty());
        
        // Exit
        state.HandleEvent("escape");
        REQUIRE(state.GetMode() == TuiMode::Normal);
        REQUIRE(state.command_state.GetBuffer().empty());
    }
}

TEST_CASE("Property 1f: Ctrl+U clears buffer and maintains mode", 
          "[Feature: tui-command-input-enhancement]"
          "[Property 1: Mode transition round-trip]"
          "[tui_state][property]") {
    
    auto random_input = GENERATE(take(100, random_command_string()));
    
    TuiState state;
    state.HandleEvent("character", ':');
    
    // Type random input
    for (char ch : random_input) {
        if (ch >= 32 && ch <= 126) {
            state.HandleEvent("character", ch);
        }
    }
    
    // Clear buffer with Ctrl+U
    state.HandleEvent("ctrl_u");
    
    // Should still be in Command_Mode but with empty buffer
    REQUIRE(state.GetMode() == TuiMode::Command);
    REQUIRE(state.command_state.GetBuffer().empty());
    
    // Exit and verify clean state
    state.HandleEvent("escape");
    REQUIRE(state.GetMode() == TuiMode::Normal);
    REQUIRE(state.command_state.GetBuffer().empty());
}

TEST_CASE("Property 1g: Other mode transitions maintain consistency", 
          "[Feature: tui-command-input-enhancement]"
          "[Property 1: Mode transition round-trip]"
          "[tui_state][property]") {
    
    auto iteration = GENERATE(range(0, 50));
    (void)iteration;
    
    TuiState state;
    
    SECTION("Command Palette round-trip") {
        REQUIRE(state.GetMode() == TuiMode::Normal);
        
        state.HandleEvent("ctrl_p");
        REQUIRE(state.GetMode() == TuiMode::CommandPalette);
        
        state.HandleEvent("escape");
        REQUIRE(state.GetMode() == TuiMode::Normal);
    }
    
    SECTION("Help panel round-trip") {
        REQUIRE(state.GetMode() == TuiMode::Normal);
        
        state.HandleEvent("character", '?');
        REQUIRE(state.GetMode() == TuiMode::Help);
        REQUIRE(state.help_state.visible);
        
        state.HandleEvent("escape");
        REQUIRE(state.GetMode() == TuiMode::Normal);
        REQUIRE_FALSE(state.help_state.visible);
    }
    
    SECTION("Help panel exit via 'q'") {
        state.HandleEvent("character", '?');
        REQUIRE(state.GetMode() == TuiMode::Help);
        
        state.HandleEvent("character", 'q');
        REQUIRE(state.GetMode() == TuiMode::Normal);
    }
}

TEST_CASE("Property 22: Mode Isolation for Shortcuts", 
          "[Feature: tui-command-input-enhancement]"
          "[Property 22: Mode isolation]"
          "[tui_state][property]") {
    
    // Generate random sequences of shortcut characters
    auto iteration = GENERATE(range(0, 100));
    (void)iteration;
    
    TuiState state;
    
    // Single-key shortcuts that should NOT trigger in Command_Mode
    const std::vector<char> shortcuts = {'r', 'd', 'f', 'c', 'C', 'p', 'P', 'q'};
    
    // Enter Command_Mode
    state.HandleEvent("character", ':');
    REQUIRE(state.GetMode() == TuiMode::Command);
    
    // Type all shortcut characters
    std::string expected_buffer;
    for (char shortcut : shortcuts) {
        state.HandleEvent("character", shortcut);
        expected_buffer += shortcut;
    }
    
    // Should still be in Command_Mode with all characters in buffer
    REQUIRE(state.GetMode() == TuiMode::Command);
    REQUIRE(state.command_state.GetBuffer() == expected_buffer);
    
    // Verify no side effects occurred (would have if shortcuts triggered)
    // The buffer contains the characters, proving they were treated as input
}

TEST_CASE("Property 22b: Special characters don't trigger mode changes in Command_Mode", 
          "[Feature: tui-command-input-enhancement]"
          "[Property 22: Mode isolation]"
          "[tui_state][property]") {
    
    auto iteration = GENERATE(range(0, 100));
    (void)iteration;
    
    TuiState state;
    state.HandleEvent("character", ':');
    REQUIRE(state.GetMode() == TuiMode::Command);
    
    // Type ':' again - should add to buffer, not enter command mode again
    state.HandleEvent("character", ':');
    REQUIRE(state.GetMode() == TuiMode::Command);
    REQUIRE(state.command_state.GetBuffer() == ":");
    
    // Type '?' - should add to buffer, not open help
    state.HandleEvent("character", '?');
    REQUIRE(state.GetMode() == TuiMode::Command);
    REQUIRE(state.command_state.GetBuffer() == ":?");
}

TEST_CASE("Property 22c: Random character sequences in Command_Mode", 
          "[Feature: tui-command-input-enhancement]"
          "[Property 22: Mode isolation]"
          "[tui_state][property]") {
    
    auto random_input = GENERATE(take(100, random_command_string()));
    
    TuiState state;
    state.HandleEvent("character", ':');
    REQUIRE(state.GetMode() == TuiMode::Command);
    
    std::string expected_buffer;
    
    // Type random characters including potential shortcuts
    for (char ch : random_input) {
        if (ch >= 32 && ch <= 126) {  // Printable ASCII
            state.HandleEvent("character", ch);
            expected_buffer += ch;
        }
    }
    
    // Should still be in Command_Mode
    REQUIRE(state.GetMode() == TuiMode::Command);
    
    // Buffer should contain all typed characters
    REQUIRE(state.command_state.GetBuffer() == expected_buffer);
}

TEST_CASE("Property 22d: Enter key behavior in Command_Mode", 
          "[Feature: tui-command-input-enhancement]"
          "[Property 22: Mode isolation]"
          "[tui_state][property]") {
    
    auto random_input = GENERATE(take(50, random_command_string()));
    
    TuiState state;
    state.HandleEvent("character", ':');
    
    if (random_input.empty()) {
        // Empty buffer: Enter should exit to Normal_Mode
        state.HandleEvent("enter");
        REQUIRE(state.GetMode() == TuiMode::Normal);
    } else {
        // Non-empty buffer without executor: stay in Command_Mode and set error
        for (char ch : random_input) {
            if (ch >= 32 && ch <= 126) {
                state.HandleEvent("character", ch);
            }
        }
        
        REQUIRE_FALSE(state.command_state.GetBuffer().empty());
        state.HandleEvent("enter");

        REQUIRE(state.GetMode() == TuiMode::Command);
        REQUIRE(state.footer_is_error);
        REQUIRE(state.footer_message == "Command executor unavailable");
    }
}

TEST_CASE("Property 22e: Ctrl+P doesn't trigger in Command_Mode", 
          "[Feature: tui-command-input-enhancement]"
          "[Property 22: Mode isolation]"
          "[tui_state][property]") {
    
    auto iteration = GENERATE(range(0, 50));
    (void)iteration;
    
    TuiState state;
    state.HandleEvent("character", ':');
    REQUIRE(state.GetMode() == TuiMode::Command);
    
    // Type some text
    state.HandleEvent("character", 't');
    state.HandleEvent("character", 'e');
    state.HandleEvent("character", 's');
    state.HandleEvent("character", 't');
    
    // Ctrl+P should not open command palette from Command_Mode
    // (It's not handled in Command_Mode, so it returns false)
    bool handled = state.HandleEvent("ctrl_p");
    
    // Should still be in Command_Mode
    REQUIRE(state.GetMode() == TuiMode::Command);
    REQUIRE(state.command_state.GetBuffer() == "test");
}

TEST_CASE("Property 22f: Mode isolation across all modes", 
          "[Feature: tui-command-input-enhancement]"
          "[Property 22: Mode isolation]"
          "[tui_state][property]") {
    
    auto iteration = GENERATE(range(0, 50));
    (void)iteration;
    
    TuiState state;
    
    SECTION("Help mode isolates input") {
        state.HandleEvent("character", '?');
        REQUIRE(state.GetMode() == TuiMode::Help);
        
        // Typing characters in Help mode shouldn't affect anything
        // (Help mode doesn't handle character input except 'q')
        state.HandleEvent("character", 'r');
        REQUIRE(state.GetMode() == TuiMode::Help);
        
        state.HandleEvent("character", 'd');
        REQUIRE(state.GetMode() == TuiMode::Help);
        
        // Only 'q' and Escape exit
        state.HandleEvent("character", 'q');
        REQUIRE(state.GetMode() == TuiMode::Normal);
    }
    
    SECTION("Command Palette mode isolates input") {
        state.HandleEvent("ctrl_p");
        REQUIRE(state.GetMode() == TuiMode::CommandPalette);
        
        // Typing ':' shouldn't enter Command_Mode
        state.HandleEvent("character", ':');
        REQUIRE(state.GetMode() == TuiMode::CommandPalette);
        
        // Only Escape exits (for now)
        state.HandleEvent("escape");
        REQUIRE(state.GetMode() == TuiMode::Normal);
    }
}
