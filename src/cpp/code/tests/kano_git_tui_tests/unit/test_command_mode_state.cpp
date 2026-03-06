#include <catch2/catch_test_macros.hpp>
#include "command_mode_state.hpp"
#include "autocomplete_engine.hpp"
#include "metadata_cache.hpp"

using namespace kano::git::commands;

// ============================================================================
// InputBuffer Unit Tests
// ============================================================================

TEST_CASE("InputBuffer: Insert character at beginning", "[Feature: tui-command-input-enhancement][Unit]") {
    InputBuffer buffer;
    buffer.Insert('a');
    
    REQUIRE(buffer.text == "a");
    REQUIRE(buffer.cursor_pos == 1);
}

TEST_CASE("InputBuffer: Insert multiple characters", "[Feature: tui-command-input-enhancement][Unit]") {
    InputBuffer buffer;
    buffer.Insert('h');
    buffer.Insert('e');
    buffer.Insert('l');
    buffer.Insert('l');
    buffer.Insert('o');
    
    REQUIRE(buffer.text == "hello");
    REQUIRE(buffer.cursor_pos == 5);
}

TEST_CASE("InputBuffer: Insert character in middle", "[Feature: tui-command-input-enhancement][Unit]") {
    InputBuffer buffer;
    buffer.text = "helo";
    buffer.cursor_pos = 3;
    
    buffer.Insert('l');
    
    REQUIRE(buffer.text == "hello");
    REQUIRE(buffer.cursor_pos == 4);
}

TEST_CASE("InputBuffer: Backspace at beginning does nothing", "[Feature: tui-command-input-enhancement][Unit]") {
    InputBuffer buffer;
    buffer.text = "hello";
    buffer.cursor_pos = 0;
    
    buffer.Backspace();
    
    REQUIRE(buffer.text == "hello");
    REQUIRE(buffer.cursor_pos == 0);
}

TEST_CASE("InputBuffer: Backspace at end", "[Feature: tui-command-input-enhancement][Unit]") {
    InputBuffer buffer;
    buffer.text = "hello";
    buffer.cursor_pos = 5;
    
    buffer.Backspace();
    
    REQUIRE(buffer.text == "hell");
    REQUIRE(buffer.cursor_pos == 4);
}

TEST_CASE("InputBuffer: Backspace in middle", "[Feature: tui-command-input-enhancement][Unit]") {
    InputBuffer buffer;
    buffer.text = "hello";
    buffer.cursor_pos = 3;
    
    buffer.Backspace();
    
    REQUIRE(buffer.text == "helo");
    REQUIRE(buffer.cursor_pos == 2);
}

TEST_CASE("InputBuffer: Delete at end does nothing", "[Feature: tui-command-input-enhancement][Unit]") {
    InputBuffer buffer;
    buffer.text = "hello";
    buffer.cursor_pos = 5;
    
    buffer.Delete();
    
    REQUIRE(buffer.text == "hello");
    REQUIRE(buffer.cursor_pos == 5);
}

TEST_CASE("InputBuffer: Delete at beginning", "[Feature: tui-command-input-enhancement][Unit]") {
    InputBuffer buffer;
    buffer.text = "hello";
    buffer.cursor_pos = 0;
    
    buffer.Delete();
    
    REQUIRE(buffer.text == "ello");
    REQUIRE(buffer.cursor_pos == 0);
}

TEST_CASE("InputBuffer: Delete in middle", "[Feature: tui-command-input-enhancement][Unit]") {
    InputBuffer buffer;
    buffer.text = "hello";
    buffer.cursor_pos = 2;
    
    buffer.Delete();
    
    REQUIRE(buffer.text == "helo");
    REQUIRE(buffer.cursor_pos == 2);
}

TEST_CASE("InputBuffer: Clear empties buffer and resets cursor", "[Feature: tui-command-input-enhancement][Unit]") {
    InputBuffer buffer;
    buffer.text = "hello world";
    buffer.cursor_pos = 5;
    
    buffer.Clear();
    
    REQUIRE(buffer.text.empty());
    REQUIRE(buffer.cursor_pos == 0);
}

TEST_CASE("InputBuffer: MoveCursor forward", "[Feature: tui-command-input-enhancement][Unit]") {
    InputBuffer buffer;
    buffer.text = "hello";
    buffer.cursor_pos = 0;
    
    buffer.MoveCursor(3);
    
    REQUIRE(buffer.cursor_pos == 3);
}

TEST_CASE("InputBuffer: MoveCursor backward", "[Feature: tui-command-input-enhancement][Unit]") {
    InputBuffer buffer;
    buffer.text = "hello";
    buffer.cursor_pos = 5;
    
    buffer.MoveCursor(-2);
    
    REQUIRE(buffer.cursor_pos == 3);
}

TEST_CASE("InputBuffer: MoveCursor clamps at beginning", "[Feature: tui-command-input-enhancement][Unit]") {
    InputBuffer buffer;
    buffer.text = "hello";
    buffer.cursor_pos = 2;
    
    buffer.MoveCursor(-10);
    
    REQUIRE(buffer.cursor_pos == 0);
}

TEST_CASE("InputBuffer: MoveCursor clamps at end", "[Feature: tui-command-input-enhancement][Unit]") {
    InputBuffer buffer;
    buffer.text = "hello";
    buffer.cursor_pos = 2;
    
    buffer.MoveCursor(10);
    
    REQUIRE(buffer.cursor_pos == 5);
}

TEST_CASE("InputBuffer: Home moves cursor to beginning", "[Feature: tui-command-input-enhancement][Unit]") {
    InputBuffer buffer;
    buffer.text = "hello";
    buffer.cursor_pos = 5;
    
    buffer.Home();
    
    REQUIRE(buffer.cursor_pos == 0);
}

TEST_CASE("InputBuffer: End moves cursor to end", "[Feature: tui-command-input-enhancement][Unit]") {
    InputBuffer buffer;
    buffer.text = "hello";
    buffer.cursor_pos = 0;
    
    buffer.End();
    
    REQUIRE(buffer.cursor_pos == 5);
}

TEST_CASE("InputBuffer: Cursor position invariant maintained after operations", "[Feature: tui-command-input-enhancement][Unit]") {
    InputBuffer buffer;
    
    // Insert characters
    buffer.Insert('a');
    REQUIRE(buffer.cursor_pos <= buffer.text.length());
    
    buffer.Insert('b');
    REQUIRE(buffer.cursor_pos <= buffer.text.length());
    
    // Backspace
    buffer.Backspace();
    REQUIRE(buffer.cursor_pos <= buffer.text.length());
    
    // Delete
    buffer.cursor_pos = 0;
    buffer.Delete();
    REQUIRE(buffer.cursor_pos <= buffer.text.length());
    
    // Move cursor
    buffer.MoveCursor(100);
    REQUIRE(buffer.cursor_pos <= buffer.text.length());
    
    buffer.MoveCursor(-100);
    REQUIRE(buffer.cursor_pos <= buffer.text.length());
}

// ============================================================================
// CandidateSelection Unit Tests
// ============================================================================

TEST_CASE("CandidateSelection: Empty list returns nullopt", "[Feature: tui-command-input-enhancement][Unit]") {
    CandidateSelection selection;
    
    auto selected = selection.GetSelected();
    
    REQUIRE_FALSE(selected.has_value());
}

TEST_CASE("CandidateSelection: SelectNext on empty list does nothing", "[Feature: tui-command-input-enhancement][Unit]") {
    CandidateSelection selection;
    
    selection.SelectNext();
    
    REQUIRE(selection.selected_index == 0);
}

TEST_CASE("CandidateSelection: SelectPrev on empty list does nothing", "[Feature: tui-command-input-enhancement][Unit]") {
    CandidateSelection selection;
    
    selection.SelectPrev();
    
    REQUIRE(selection.selected_index == 0);
}

TEST_CASE("CandidateSelection: Single item selection", "[Feature: tui-command-input-enhancement][Unit]") {
    CandidateSelection selection;
    selection.items = {
        CandidateItem{"commit", "Record changes", CandidateType::Command, "commit"}
    };
    
    auto selected = selection.GetSelected();
    
    REQUIRE(selected.has_value());
    REQUIRE(selected->text == "commit");
}

TEST_CASE("CandidateSelection: SelectNext wraps to first", "[Feature: tui-command-input-enhancement][Unit]") {
    CandidateSelection selection;
    selection.items = {
        CandidateItem{"commit", "Record changes", CandidateType::Command, "commit"},
        CandidateItem{"push", "Update remote", CandidateType::Command, "push"},
        CandidateItem{"fetch", "Download objects", CandidateType::Command, "fetch"}
    };
    
    // Start at first (index 0)
    REQUIRE(selection.selected_index == 0);
    
    // Move to second
    selection.SelectNext();
    REQUIRE(selection.selected_index == 1);
    
    // Move to third
    selection.SelectNext();
    REQUIRE(selection.selected_index == 2);
    
    // Wrap to first
    selection.SelectNext();
    REQUIRE(selection.selected_index == 0);
}

TEST_CASE("CandidateSelection: SelectPrev wraps to last", "[Feature: tui-command-input-enhancement][Unit]") {
    CandidateSelection selection;
    selection.items = {
        CandidateItem{"commit", "Record changes", CandidateType::Command, "commit"},
        CandidateItem{"push", "Update remote", CandidateType::Command, "push"},
        CandidateItem{"fetch", "Download objects", CandidateType::Command, "fetch"}
    };
    
    // Start at first (index 0)
    REQUIRE(selection.selected_index == 0);
    
    // Wrap to last
    selection.SelectPrev();
    REQUIRE(selection.selected_index == 2);
    
    // Move to second
    selection.SelectPrev();
    REQUIRE(selection.selected_index == 1);
    
    // Move to first
    selection.SelectPrev();
    REQUIRE(selection.selected_index == 0);
}

TEST_CASE("CandidateSelection: GetSelected returns correct item", "[Feature: tui-command-input-enhancement][Unit]") {
    CandidateSelection selection;
    selection.items = {
        CandidateItem{"commit", "Record changes", CandidateType::Command, "commit"},
        CandidateItem{"push", "Update remote", CandidateType::Command, "push"},
        CandidateItem{"fetch", "Download objects", CandidateType::Command, "fetch"}
    };
    
    // First item
    auto selected = selection.GetSelected();
    REQUIRE(selected.has_value());
    REQUIRE(selected->text == "commit");
    
    // Second item
    selection.SelectNext();
    selected = selection.GetSelected();
    REQUIRE(selected.has_value());
    REQUIRE(selected->text == "push");
    
    // Third item
    selection.SelectNext();
    selected = selection.GetSelected();
    REQUIRE(selected.has_value());
    REQUIRE(selected->text == "fetch");
}

TEST_CASE("CandidateSelection: Reset returns to first item", "[Feature: tui-command-input-enhancement][Unit]") {
    CandidateSelection selection;
    selection.items = {
        CandidateItem{"commit", "Record changes", CandidateType::Command, "commit"},
        CandidateItem{"push", "Update remote", CandidateType::Command, "push"}
    };
    
    // Move to second item
    selection.SelectNext();
    REQUIRE(selection.selected_index == 1);
    
    // Reset to first
    selection.Reset();
    REQUIRE(selection.selected_index == 0);
}

TEST_CASE("CandidateSelection: IsEmpty returns correct value", "[Feature: tui-command-input-enhancement][Unit]") {
    CandidateSelection selection;
    
    REQUIRE(selection.IsEmpty());
    
    selection.items = {
        CandidateItem{"commit", "Record changes", CandidateType::Command, "commit"}
    };
    
    REQUIRE_FALSE(selection.IsEmpty());
}

TEST_CASE("CandidateSelection: Size returns correct count", "[Feature: tui-command-input-enhancement][Unit]") {
    CandidateSelection selection;
    
    REQUIRE(selection.Size() == 0);
    
    selection.items = {
        CandidateItem{"commit", "Record changes", CandidateType::Command, "commit"},
        CandidateItem{"push", "Update remote", CandidateType::Command, "push"}
    };
    
    REQUIRE(selection.Size() == 2);
}

// ============================================================================
// CommandModeState Unit Tests
// ============================================================================

TEST_CASE("CommandModeState: OnCharacter adds to buffer", "[Feature: tui-command-input-enhancement][Unit]") {
    CommandModeState state;
    
    state.OnCharacter('a');
    
    REQUIRE(state.GetBuffer() == "a");
    REQUIRE(state.GetCursorPos() == 1);
}

TEST_CASE("CommandModeState: OnBackspace removes from buffer", "[Feature: tui-command-input-enhancement][Unit]") {
    CommandModeState state;
    state.buffer.text = "hello";
    state.buffer.cursor_pos = 5;
    
    state.OnBackspace();
    
    REQUIRE(state.GetBuffer() == "hell");
    REQUIRE(state.GetCursorPos() == 4);
}

TEST_CASE("CommandModeState: OnDelete removes from buffer", "[Feature: tui-command-input-enhancement][Unit]") {
    CommandModeState state;
    state.buffer.text = "hello";
    state.buffer.cursor_pos = 0;
    
    state.OnDelete();
    
    REQUIRE(state.GetBuffer() == "ello");
    REQUIRE(state.GetCursorPos() == 0);
}

TEST_CASE("CommandModeState: OnClearBuffer empties buffer", "[Feature: tui-command-input-enhancement][Unit]") {
    CommandModeState state;
    state.buffer.text = "hello world";
    state.buffer.cursor_pos = 5;
    
    state.OnClearBuffer();
    
    REQUIRE(state.GetBuffer().empty());
    REQUIRE(state.GetCursorPos() == 0);
}

TEST_CASE("CommandModeState: Cursor navigation methods", "[Feature: tui-command-input-enhancement][Unit]") {
    CommandModeState state;
    state.buffer.text = "hello";
    state.buffer.cursor_pos = 2;
    
    // Left
    state.OnCursorLeft();
    REQUIRE(state.GetCursorPos() == 1);
    
    // Right
    state.OnCursorRight();
    REQUIRE(state.GetCursorPos() == 2);
    
    // Home
    state.OnHome();
    REQUIRE(state.GetCursorPos() == 0);
    
    // End
    state.OnEnd();
    REQUIRE(state.GetCursorPos() == 5);
}

TEST_CASE("CommandModeState: Candidate navigation methods", "[Feature: tui-command-input-enhancement][Unit]") {
    CommandModeState state;
    state.candidates.items = {
        CandidateItem{"commit", "Record changes", CandidateType::Command, "commit"},
        CandidateItem{"push", "Update remote", CandidateType::Command, "push"}
    };
    
    // Next
    state.OnNextCandidate();
    REQUIRE(state.candidates.selected_index == 1);
    
    // Prev
    state.OnPrevCandidate();
    REQUIRE(state.candidates.selected_index == 0);
}

TEST_CASE("CommandModeState: OnAcceptCandidate updates buffer", "[Feature: tui-command-input-enhancement][Unit]") {
    CommandModeState state;
    state.buffer.text = "com";
    state.buffer.cursor_pos = 3;
    state.candidates.items = {
        CandidateItem{"commit", "Record changes", CandidateType::Command, "commit"}
    };
    state.show_candidates = true;
    
    state.OnAcceptCandidate();
    
    REQUIRE(state.GetBuffer() == "commit ");
    REQUIRE(state.GetCursorPos() == 7);
    REQUIRE_FALSE(state.HasCandidates());
}

TEST_CASE("CommandModeState: OnAcceptCandidate with no candidates does nothing", "[Feature: tui-command-input-enhancement][Unit]") {
    CommandModeState state;
    state.buffer.text = "hello";
    state.buffer.cursor_pos = 5;
    
    state.OnAcceptCandidate();
    
    REQUIRE(state.GetBuffer() == "hello");
    REQUIRE(state.GetCursorPos() == 5);
}

TEST_CASE("CommandModeState: HasCandidates returns correct value", "[Feature: tui-command-input-enhancement][Unit]") {
    CommandModeState state;
    
    REQUIRE_FALSE(state.HasCandidates());
    
    state.candidates.items = {
        CandidateItem{"commit", "Record changes", CandidateType::Command, "commit"}
    };
    state.show_candidates = true;
    
    REQUIRE(state.HasCandidates());
}
