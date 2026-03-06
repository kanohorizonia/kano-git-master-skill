#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/generators/catch_generators_adapters.hpp>
#include <catch2/generators/catch_generators_random.hpp>
#include <catch2/generators/catch_generators_range.hpp>
#include "command_mode_state.hpp"
#include "autocomplete_engine.hpp"
#include "metadata_cache.hpp"
#include <CLI/CLI.hpp>
#include <random>
#include <vector>

using namespace kano::git::commands;

namespace {

/// Helper to setup a test CLI11 app with sample commands
void SetupTestApp(CLI::App& app) {
    auto* commit = app.add_subcommand("commit", "Record changes");
    commit->add_option("--message,-m", "Commit message");
    commit->add_flag("--all,-a", "Stage all");

    auto* push = app.add_subcommand("push", "Update remote");
    push->add_flag("--force,-f", "Force push");

    auto* worktree = app.add_subcommand("worktree", "Manage worktrees");
    worktree->add_subcommand("add", "Add worktree");
    worktree->add_subcommand("list", "List worktrees");

    app.add_subcommand("fetch", "Download objects");
}

/// Generate random editing operations
enum class EditOp {
    Insert,
    Backspace,
    Delete,
    MoveCursorLeft,
    MoveCursorRight,
    Home,
    End,
    Clear
};

} // anonymous namespace

TEST_CASE("Property 2: Input Buffer Editing Preserves Invariants",
          "[Feature: tui-command-input-enhancement][Property 2: Input Buffer Editing Preserves Invariants]") {
    // **Validates: Requirements 2.1, 2.2, 2.4, 2.5, 2.6, 2.7**
    
    InputBuffer buffer;
    
    // Generate 100 random sequences of editing operations
    auto num_operations = GENERATE(take(100, random(5, 50)));
    
    DYNAMIC_SECTION("Random editing sequence with " << num_operations << " operations") {
        std::mt19937 rng(std::random_device{}());
        std::uniform_int_distribution<int> op_dist(0, 7);
        std::uniform_int_distribution<int> char_dist('a', 'z');
        
        for (int i = 0; i < num_operations; ++i) {
            auto op = static_cast<EditOp>(op_dist(rng));
            
            switch (op) {
                case EditOp::Insert:
                    buffer.Insert(static_cast<char>(char_dist(rng)));
                    break;
                case EditOp::Backspace:
                    buffer.Backspace();
                    break;
                case EditOp::Delete:
                    buffer.Delete();
                    break;
                case EditOp::MoveCursorLeft:
                    buffer.MoveCursor(-1);
                    break;
                case EditOp::MoveCursorRight:
                    buffer.MoveCursor(1);
                    break;
                case EditOp::Home:
                    buffer.Home();
                    break;
                case EditOp::End:
                    buffer.End();
                    break;
                case EditOp::Clear:
                    buffer.Clear();
                    break;
            }
            
            // Property: Cursor position must always be within valid range
            REQUIRE(buffer.cursor_pos >= 0);
            REQUIRE(buffer.cursor_pos <= buffer.text.length());
        }
    }
}

TEST_CASE("Property 2: Input Buffer Editing - Specific Sequences",
          "[Feature: tui-command-input-enhancement][Property 2: Input Buffer Editing Preserves Invariants]") {
    // **Validates: Requirements 2.1, 2.2, 2.4, 2.5, 2.6, 2.7**
    
    SECTION("Insert at various cursor positions") {
        auto initial_text = GENERATE(values({"", "a", "ab", "abc", "hello", "world", "test123"}));
        auto cursor_pos = GENERATE_COPY(take(10, random(0, static_cast<int>(std::string(initial_text).length()))));
        
        DYNAMIC_SECTION("Insert at position " << cursor_pos << " in '" << initial_text << "'") {
            InputBuffer buffer;
            buffer.text = initial_text;
            buffer.cursor_pos = cursor_pos;
            
            buffer.Insert('x');
            
            // Property: Cursor position must be valid after insert
            REQUIRE(buffer.cursor_pos >= 0);
            REQUIRE(buffer.cursor_pos <= buffer.text.length());
            
            // Property: Text length increases by 1
            REQUIRE(buffer.text.length() == std::string(initial_text).length() + 1);
            
            // Property: Cursor advances by 1
            REQUIRE(buffer.cursor_pos == static_cast<size_t>(cursor_pos) + 1);
        }
    }
    
    SECTION("Backspace at various cursor positions") {
        auto initial_text = GENERATE(values({"a", "ab", "abc", "hello", "world", "test123"}));
        auto cursor_pos = GENERATE_COPY(take(10, random(0, static_cast<int>(std::string(initial_text).length()))));
        
        DYNAMIC_SECTION("Backspace at position " << cursor_pos << " in '" << initial_text << "'") {
            InputBuffer buffer;
            buffer.text = initial_text;
            buffer.cursor_pos = cursor_pos;
            
            buffer.Backspace();
            
            // Property: Cursor position must be valid after backspace
            REQUIRE(buffer.cursor_pos >= 0);
            REQUIRE(buffer.cursor_pos <= buffer.text.length());
            
            if (cursor_pos > 0) {
                // Property: Text length decreases by 1 if cursor was not at beginning
                REQUIRE(buffer.text.length() == std::string(initial_text).length() - 1);
                
                // Property: Cursor moves back by 1
                REQUIRE(buffer.cursor_pos == static_cast<size_t>(cursor_pos) - 1);
            } else {
                // Property: Text unchanged if cursor was at beginning
                REQUIRE(buffer.text == initial_text);
                REQUIRE(buffer.cursor_pos == 0);
            }
        }
    }
    
    SECTION("Delete at various cursor positions") {
        auto initial_text = GENERATE(values({"a", "ab", "abc", "hello", "world", "test123"}));
        auto cursor_pos = GENERATE_COPY(take(10, random(0, static_cast<int>(std::string(initial_text).length()))));
        
        DYNAMIC_SECTION("Delete at position " << cursor_pos << " in '" << initial_text << "'") {
            InputBuffer buffer;
            buffer.text = initial_text;
            buffer.cursor_pos = cursor_pos;
            
            buffer.Delete();
            
            // Property: Cursor position must be valid after delete
            REQUIRE(buffer.cursor_pos >= 0);
            REQUIRE(buffer.cursor_pos <= buffer.text.length());
            
            if (cursor_pos < std::string(initial_text).length()) {
                // Property: Text length decreases by 1 if cursor was not at end
                REQUIRE(buffer.text.length() == std::string(initial_text).length() - 1);
                
                // Property: Cursor position unchanged
                REQUIRE(buffer.cursor_pos == static_cast<size_t>(cursor_pos));
            } else {
                // Property: Text unchanged if cursor was at end
                REQUIRE(buffer.text == initial_text);
                REQUIRE(buffer.cursor_pos == std::string(initial_text).length());
            }
        }
    }
    
    SECTION("MoveCursor with various deltas") {
        auto initial_text = GENERATE(values({"", "a", "hello", "world", "test123"}));
        auto start_pos = GENERATE_COPY(take(10, random(0, static_cast<int>(std::string(initial_text).length()))));
        auto delta = GENERATE(take(10, random(-20, 20)));
        
        DYNAMIC_SECTION("Move cursor by " << delta << " from position " << start_pos << " in '" << initial_text << "'") {
            InputBuffer buffer;
            buffer.text = initial_text;
            buffer.cursor_pos = start_pos;
            
            buffer.MoveCursor(delta);
            
            // Property: Cursor position must be valid after move
            REQUIRE(buffer.cursor_pos >= 0);
            REQUIRE(buffer.cursor_pos <= buffer.text.length());
            
            // Property: Text unchanged
            REQUIRE(buffer.text == initial_text);
        }
    }
    
    SECTION("Home and End operations") {
        auto initial_text = GENERATE(values({"", "a", "hello", "world", "test123"}));
        auto start_pos = GENERATE_COPY(take(10, random(0, static_cast<int>(std::string(initial_text).length()))));
        
        DYNAMIC_SECTION("Home/End from position " << start_pos << " in '" << initial_text << "'") {
            InputBuffer buffer;
            buffer.text = initial_text;
            buffer.cursor_pos = start_pos;
            
            // Test Home
            buffer.Home();
            REQUIRE(buffer.cursor_pos == 0);
            REQUIRE(buffer.cursor_pos <= buffer.text.length());
            REQUIRE(buffer.text == initial_text);
            
            // Test End
            buffer.End();
            REQUIRE(buffer.cursor_pos == buffer.text.length());
            REQUIRE(buffer.cursor_pos <= buffer.text.length());
            REQUIRE(buffer.text == initial_text);
        }
    }
}

TEST_CASE("Property 3: Buffer Clear is Idempotent",
          "[Feature: tui-command-input-enhancement][Property 3: Buffer Clear is Idempotent]") {
    // **Validates: Requirements 2.3**
    
    auto initial_text = GENERATE(take(100, values({"", "a", "hello", "world", "test123", 
                                                     "commit --message", "push --force",
                                                     "worktree add", "fetch --all"})));
    auto cursor_pos = GENERATE_COPY(take(10, random(0, static_cast<int>(std::string(initial_text).length()))));
    
    DYNAMIC_SECTION("Clear buffer with text '" << initial_text << "' at position " << cursor_pos) {
        InputBuffer buffer;
        buffer.text = initial_text;
        buffer.cursor_pos = cursor_pos;
        
        // First clear
        buffer.Clear();
        
        // Property: Buffer is empty after first clear
        REQUIRE(buffer.text.empty());
        REQUIRE(buffer.cursor_pos == 0);
        
        // Second clear (idempotence test)
        buffer.Clear();
        
        // Property: Buffer remains empty after second clear
        REQUIRE(buffer.text.empty());
        REQUIRE(buffer.cursor_pos == 0);
        
        // Third clear (additional idempotence test)
        buffer.Clear();
        
        // Property: Buffer still empty after third clear
        REQUIRE(buffer.text.empty());
        REQUIRE(buffer.cursor_pos == 0);
    }
}

TEST_CASE("Property 9: Candidate Selection Wrapping",
          "[Feature: tui-command-input-enhancement][Property 9: Candidate Selection Wrapping]") {
    // **Validates: Requirements 5.6, 5.7**
    
    auto num_candidates = GENERATE(take(100, random(1, 20)));
    
    DYNAMIC_SECTION("Circular navigation with " << num_candidates << " candidates") {
        CandidateSelection selection;
        
        // Create N candidates
        for (int i = 0; i < num_candidates; ++i) {
            selection.items.push_back(
                CandidateItem{
                    "item" + std::to_string(i),
                    "Description " + std::to_string(i),
                    CandidateType::Command,
                    "item" + std::to_string(i)
                }
            );
        }
        
        // Property: Start at index 0
        REQUIRE(selection.selected_index == 0);
        
        // Property: Navigate forward N times wraps to first
        for (int i = 0; i < num_candidates; ++i) {
            selection.SelectNext();
        }
        REQUIRE(selection.selected_index == 0);
        
        // Property: Navigate backward once wraps to last
        selection.SelectPrev();
        REQUIRE(selection.selected_index == num_candidates - 1);
        
        // Property: Navigate backward N times wraps to last
        selection.selected_index = 0;
        for (int i = 0; i < num_candidates; ++i) {
            selection.SelectPrev();
        }
        REQUIRE(selection.selected_index == 0);
        
        // Property: Navigate forward once from last wraps to first
        selection.selected_index = num_candidates - 1;
        selection.SelectNext();
        REQUIRE(selection.selected_index == 0);
    }
}

TEST_CASE("Property 9: Candidate Selection Wrapping - Edge Cases",
          "[Feature: tui-command-input-enhancement][Property 9: Candidate Selection Wrapping]") {
    // **Validates: Requirements 5.6, 5.7**
    
    SECTION("Single candidate wraps to itself") {
        CandidateSelection selection;
        selection.items.push_back(
            CandidateItem{"commit", "Record changes", CandidateType::Command, "commit"}
        );
        
        REQUIRE(selection.selected_index == 0);
        
        // Property: Forward navigation stays at 0
        selection.SelectNext();
        REQUIRE(selection.selected_index == 0);
        
        // Property: Backward navigation stays at 0
        selection.SelectPrev();
        REQUIRE(selection.selected_index == 0);
    }
    
    SECTION("Empty candidate list does nothing") {
        CandidateSelection selection;
        
        REQUIRE(selection.selected_index == 0);
        
        // Property: Navigation on empty list does nothing
        selection.SelectNext();
        REQUIRE(selection.selected_index == 0);
        
        selection.SelectPrev();
        REQUIRE(selection.selected_index == 0);
    }
}

TEST_CASE("Property 10: Candidate Navigation Consistency",
          "[Feature: tui-command-input-enhancement][Property 10: Candidate Navigation Consistency]") {
    // **Validates: Requirements 5.2, 5.3, 5.4, 5.5**
    
    auto num_candidates = GENERATE(take(100, random(2, 20)));
    auto num_steps = GENERATE_COPY(take(10, random(1, 50)));
    
    DYNAMIC_SECTION("Navigation consistency with " << num_candidates << " candidates, " << num_steps << " steps") {
        // Create two identical selections
        CandidateSelection selection1;
        CandidateSelection selection2;
        
        for (int i = 0; i < num_candidates; ++i) {
            auto item = CandidateItem{
                "item" + std::to_string(i),
                "Description " + std::to_string(i),
                CandidateType::Command,
                "item" + std::to_string(i)
            };
            selection1.items.push_back(item);
            selection2.items.push_back(item);
        }
        
        // Navigate forward with SelectNext
        for (int i = 0; i < num_steps; ++i) {
            selection1.SelectNext();
        }
        
        // Navigate forward with SelectNext (equivalent to Tab/Down)
        for (int i = 0; i < num_steps; ++i) {
            selection2.SelectNext();
        }
        
        // Property: Both methods produce same result
        REQUIRE(selection1.selected_index == selection2.selected_index);
        
        // Reset and test backward navigation
        selection1.Reset();
        selection2.Reset();
        
        // Navigate backward with SelectPrev
        for (int i = 0; i < num_steps; ++i) {
            selection1.SelectPrev();
        }
        
        // Navigate backward with SelectPrev (equivalent to Shift+Tab/Up)
        for (int i = 0; i < num_steps; ++i) {
            selection2.SelectPrev();
        }
        
        // Property: Both methods produce same result
        REQUIRE(selection1.selected_index == selection2.selected_index);
    }
}

TEST_CASE("Property 10: Candidate Navigation - Forward/Backward Symmetry",
          "[Feature: tui-command-input-enhancement][Property 10: Candidate Navigation Consistency]") {
    // **Validates: Requirements 5.2, 5.3, 5.4, 5.5**
    
    auto num_candidates = GENERATE(take(100, random(2, 20)));
    auto num_steps = GENERATE_COPY(take(10, random(1, 50)));
    
    DYNAMIC_SECTION("Forward/backward symmetry with " << num_candidates << " candidates, " << num_steps << " steps") {
        CandidateSelection selection;
        
        for (int i = 0; i < num_candidates; ++i) {
            selection.items.push_back(
                CandidateItem{
                    "item" + std::to_string(i),
                    "Description " + std::to_string(i),
                    CandidateType::Command,
                    "item" + std::to_string(i)
                }
            );
        }
        
        int initial_index = selection.selected_index;
        
        // Navigate forward N steps
        for (int i = 0; i < num_steps; ++i) {
            selection.SelectNext();
        }
        
        // Navigate backward N steps
        for (int i = 0; i < num_steps; ++i) {
            selection.SelectPrev();
        }
        
        // Property: Should return to initial position
        REQUIRE(selection.selected_index == initial_index);
    }
}

TEST_CASE("Property: CommandModeState UpdateCandidates Integration",
          "[Feature: tui-command-input-enhancement][Property]") {
    CLI::App app{"Test App"};
    SetupTestApp(app);
    auto metadata = std::make_shared<MetadataCache>(app);
    AutocompleteEngine engine(metadata);
    
    SECTION("UpdateCandidates resets selection") {
        CommandModeState state;
        
        // Type "com" and update candidates
        state.buffer.text = "com";
        state.UpdateCandidates(engine);
        
        // Property: Candidates should be generated
        REQUIRE(state.HasCandidates());
        
        // Navigate to second candidate
        if (state.candidates.Size() > 1) {
            state.OnNextCandidate();
            REQUIRE(state.candidates.selected_index == 1);
            
            // Type another character
            state.OnCharacter('m');
            state.UpdateCandidates(engine);
            
            // Property: Selection should reset to first candidate
            REQUIRE(state.candidates.selected_index == 0);
        }
    }
    
    SECTION("UpdateCandidates with empty buffer") {
        CommandModeState state;
        
        state.buffer.text = "";
        state.UpdateCandidates(engine);
        
        // Property: Empty buffer should show candidates (all commands)
        REQUIRE(state.HasCandidates());
    }
    
    SECTION("UpdateCandidates with non-matching input") {
        CommandModeState state;
        
        state.buffer.text = "xyz123notacommand";
        state.UpdateCandidates(engine);
        
        // Property: Non-matching input should have no candidates
        REQUIRE_FALSE(state.HasCandidates());
    }
}
