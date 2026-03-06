#include <catch2/catch_test_macros.hpp>
#include "tui_state.hpp"

using namespace kano::git::commands;

TEST_CASE("TuiState mode transitions", "[tui_state][unit]") {
    TuiState state;
    
    SECTION("Initial state is Normal mode") {
        REQUIRE(state.GetMode() == TuiMode::Normal);
        REQUIRE(state.footer_message.empty());
        REQUIRE_FALSE(state.footer_is_error);
    }
    
    SECTION("Enter Command_Mode from Normal_Mode on ':' key") {
        REQUIRE(state.GetMode() == TuiMode::Normal);
        
        bool handled = state.HandleEvent("character", ':');
        
        REQUIRE(handled);
        REQUIRE(state.GetMode() == TuiMode::Command);
        REQUIRE(state.command_state.GetBuffer().empty());
    }
    
    SECTION("Exit Command_Mode via Escape") {
        // Enter command mode
        state.HandleEvent("character", ':');
        REQUIRE(state.GetMode() == TuiMode::Command);
        
        // Type some text
        state.HandleEvent("character", 'c');
        state.HandleEvent("character", 'o');
        state.HandleEvent("character", 'm');
        REQUIRE(state.command_state.GetBuffer() == "com");
        
        // Exit via Escape
        bool handled = state.HandleEvent("escape");
        
        REQUIRE(handled);
        REQUIRE(state.GetMode() == TuiMode::Normal);
        REQUIRE(state.command_state.GetBuffer().empty());  // Buffer cleared
    }
    
    SECTION("Exit Command_Mode via Enter with empty buffer") {
        // Enter command mode
        state.HandleEvent("character", ':');
        REQUIRE(state.GetMode() == TuiMode::Command);
        REQUIRE(state.command_state.GetBuffer().empty());
        
        // Press Enter with empty buffer
        bool handled = state.HandleEvent("enter");
        
        REQUIRE(handled);
        REQUIRE(state.GetMode() == TuiMode::Normal);
    }
    
    SECTION("Enter with 'help' in Command_Mode opens Help mode") {
        // Enter command mode
        state.HandleEvent("character", ':');
        
        // Type a command
        state.HandleEvent("character", 'h');
        state.HandleEvent("character", 'e');
        state.HandleEvent("character", 'l');
        state.HandleEvent("character", 'p');
        REQUIRE(state.command_state.GetBuffer() == "help");
        
        // Press Enter
        bool handled = state.HandleEvent("enter");
        
        REQUIRE(handled);
        REQUIRE(state.GetMode() == TuiMode::Help);
        REQUIRE(state.help_state.visible);
    }

    SECTION("Enter with non-empty non-help buffer requires executor and stays in Command_Mode when missing") {
        state.HandleEvent("character", ':');
        state.HandleEvent("character", 'f');
        state.HandleEvent("character", 'e');
        state.HandleEvent("character", 't');
        state.HandleEvent("character", 'c');
        state.HandleEvent("character", 'h');
        REQUIRE(state.command_state.GetBuffer() == "fetch");

        bool handled = state.HandleEvent("enter");

        REQUIRE(handled);
        REQUIRE(state.GetMode() == TuiMode::Command);
        REQUIRE(state.footer_is_error);
        REQUIRE(state.footer_message == "Command executor unavailable");
    }
    
    SECTION("Round-trip Normal → Command → Normal clears buffer") {
        // Start in Normal mode
        REQUIRE(state.GetMode() == TuiMode::Normal);
        
        // Enter Command mode
        state.HandleEvent("character", ':');
        REQUIRE(state.GetMode() == TuiMode::Command);
        
        // Type some text
        state.HandleEvent("character", 't');
        state.HandleEvent("character", 'e');
        state.HandleEvent("character", 's');
        state.HandleEvent("character", 't');
        REQUIRE(state.command_state.GetBuffer() == "test");
        
        // Exit via Escape
        state.HandleEvent("escape");
        REQUIRE(state.GetMode() == TuiMode::Normal);
        REQUIRE(state.command_state.GetBuffer().empty());
        
        // Enter Command mode again
        state.HandleEvent("character", ':');
        REQUIRE(state.GetMode() == TuiMode::Command);
        REQUIRE(state.command_state.GetBuffer().empty());  // Buffer is clean
    }
}

TEST_CASE("TuiState Command_Mode input handling", "[tui_state][unit]") {
    TuiState state;
    state.HandleEvent("character", ':');  // Enter command mode
    
    SECTION("Character input adds to buffer") {
        state.HandleEvent("character", 'a');
        REQUIRE(state.command_state.GetBuffer() == "a");
        
        state.HandleEvent("character", 'b');
        REQUIRE(state.command_state.GetBuffer() == "ab");
        
        state.HandleEvent("character", 'c');
        REQUIRE(state.command_state.GetBuffer() == "abc");
    }
    
    SECTION("Backspace removes last character") {
        state.HandleEvent("character", 'a');
        state.HandleEvent("character", 'b');
        state.HandleEvent("character", 'c');
        REQUIRE(state.command_state.GetBuffer() == "abc");
        
        state.HandleEvent("backspace");
        REQUIRE(state.command_state.GetBuffer() == "ab");
        
        state.HandleEvent("backspace");
        REQUIRE(state.command_state.GetBuffer() == "a");
    }
    
    SECTION("Ctrl+U clears entire buffer") {
        state.HandleEvent("character", 'h');
        state.HandleEvent("character", 'e');
        state.HandleEvent("character", 'l');
        state.HandleEvent("character", 'l');
        state.HandleEvent("character", 'o');
        REQUIRE(state.command_state.GetBuffer() == "hello");
        
        state.HandleEvent("ctrl_u");
        REQUIRE(state.command_state.GetBuffer().empty());
    }
    
    SECTION("Cursor navigation keys are handled") {
        state.HandleEvent("character", 'a');
        state.HandleEvent("character", 'b');
        state.HandleEvent("character", 'c');
        
        bool handled_left = state.HandleEvent("left");
        REQUIRE(handled_left);
        
        bool handled_right = state.HandleEvent("right");
        REQUIRE(handled_right);
        
        bool handled_home = state.HandleEvent("home");
        REQUIRE(handled_home);
        
        bool handled_end = state.HandleEvent("end");
        REQUIRE(handled_end);
    }
}

TEST_CASE("TuiState other mode transitions", "[tui_state][unit]") {
    TuiState state;
    
    SECTION("Enter Command Palette on Ctrl+P") {
        REQUIRE(state.GetMode() == TuiMode::Normal);
        
        bool handled = state.HandleEvent("ctrl_p");
        
        REQUIRE(handled);
        REQUIRE(state.GetMode() == TuiMode::CommandPalette);
    }
    
    SECTION("Exit Command Palette on Escape") {
        state.HandleEvent("ctrl_p");
        REQUIRE(state.GetMode() == TuiMode::CommandPalette);
        
        bool handled = state.HandleEvent("escape");
        
        REQUIRE(handled);
        REQUIRE(state.GetMode() == TuiMode::Normal);
    }
    
    SECTION("Enter Help on '?' key") {
        REQUIRE(state.GetMode() == TuiMode::Normal);
        
        bool handled = state.HandleEvent("character", '?');
        
        REQUIRE(handled);
        REQUIRE(state.GetMode() == TuiMode::Help);
        REQUIRE(state.help_state.visible);
    }
    
    SECTION("Exit Help on Escape") {
        state.HandleEvent("character", '?');
        REQUIRE(state.GetMode() == TuiMode::Help);
        
        bool handled = state.HandleEvent("escape");
        
        REQUIRE(handled);
        REQUIRE(state.GetMode() == TuiMode::Normal);
        REQUIRE_FALSE(state.help_state.visible);
    }
    
    SECTION("Exit Help on 'q' key") {
        state.HandleEvent("character", '?');
        REQUIRE(state.GetMode() == TuiMode::Help);
        
        bool handled = state.HandleEvent("character", 'q');
        
        REQUIRE(handled);
        REQUIRE(state.GetMode() == TuiMode::Normal);
    }
}

TEST_CASE("TuiState footer message management", "[tui_state][unit]") {
    TuiState state;
    
    SECTION("Set and clear footer message") {
        state.SetFooterMessage("Test message", false);
        REQUIRE(state.footer_message == "Test message");
        REQUIRE_FALSE(state.footer_is_error);
        
        state.ClearFooterMessage();
        REQUIRE(state.footer_message.empty());
        REQUIRE_FALSE(state.footer_is_error);
    }
    
    SECTION("Set error message") {
        state.SetFooterMessage("Error occurred", true);
        REQUIRE(state.footer_message == "Error occurred");
        REQUIRE(state.footer_is_error);
    }
    
    SECTION("Error message clears when typing in Command_Mode") {
        // Set an error message
        state.SetFooterMessage("Unknown command", true);
        REQUIRE(state.footer_is_error);
        
        // Enter command mode
        state.HandleEvent("character", ':');
        
        // Type a character
        state.HandleEvent("character", 'h');
        
        // Error should be cleared
        REQUIRE(state.footer_message.empty());
        REQUIRE_FALSE(state.footer_is_error);
    }
    
    SECTION("Footer clears on mode transitions") {
        state.SetFooterMessage("Some message", false);
        
        // Enter command mode
        state.HandleEvent("character", ':');
        REQUIRE(state.footer_message.empty());
        
        // Set message again
        state.SetFooterMessage("Another message", false);
        
        // Exit command mode
        state.HandleEvent("escape");
        REQUIRE(state.footer_message.empty());
    }
}

TEST_CASE("TuiState mode isolation", "[tui_state][unit]") {
    TuiState state;
    
    SECTION("Single-key shortcuts don't trigger in Command_Mode") {
        // Enter command mode
        state.HandleEvent("character", ':');
        REQUIRE(state.GetMode() == TuiMode::Command);
        
        // Type characters that would be shortcuts in Normal mode
        state.HandleEvent("character", 'r');  // Would be 'refresh' in Normal
        state.HandleEvent("character", 'd');  // Would be 'dirty-only' in Normal
        state.HandleEvent("character", 'f');  // Would be 'fetch' in Normal
        state.HandleEvent("character", 'q');  // Would be 'quit' in Normal
        
        // Should still be in Command mode with text in buffer
        REQUIRE(state.GetMode() == TuiMode::Command);
        REQUIRE(state.command_state.GetBuffer() == "rdfq");
    }
    
    SECTION("':' character in Command_Mode adds to buffer") {
        state.HandleEvent("character", ':');
        REQUIRE(state.GetMode() == TuiMode::Command);
        
        // Type ':' again - should add to buffer, not enter command mode again
        state.HandleEvent("character", ':');
        REQUIRE(state.GetMode() == TuiMode::Command);
        REQUIRE(state.command_state.GetBuffer() == ":");
    }
}
