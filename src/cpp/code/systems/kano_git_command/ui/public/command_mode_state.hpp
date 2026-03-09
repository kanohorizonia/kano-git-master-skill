#pragma once

#include "autocomplete_engine.hpp"
#include <string>
#include <vector>
#include <optional>
#include <cstddef>
#include <algorithm>

namespace kano::git::commands {

/// InputBuffer manages text input with cursor position
/// Maintains invariant: 0 ≤ cursor_pos ≤ text.length()
struct InputBuffer {
    std::string text;
    size_t cursor_pos = 0;

    /// Insert a character at the current cursor position
    /// Advances cursor by 1
    auto Insert(char ch) -> void;

    /// Delete the character before the cursor (backspace)
    /// Does nothing if cursor is at position 0
    auto Backspace() -> void;

    /// Delete the character at the cursor position
    /// Does nothing if cursor is at end of text
    auto Delete() -> void;

    /// Clear all text and reset cursor to 0
    auto Clear() -> void;

    /// Move cursor by delta positions
    /// Clamps to valid range [0, text.length()]
    auto MoveCursor(int delta) -> void;

    /// Move cursor to beginning of text
    auto Home() -> void;

    /// Move cursor to end of text
    auto End() -> void;
};

/// CandidateSelection manages circular navigation through candidate items
struct CandidateSelection {
    std::vector<CandidateItem> items;
    int selected_index = 0;

    /// Select the next candidate (circular, wraps to first)
    auto SelectNext() -> void;

    /// Select the previous candidate (circular, wraps to last)
    auto SelectPrev() -> void;

    /// Get the currently selected candidate
    /// Returns nullopt if items is empty or index is invalid
    auto GetSelected() -> std::optional<CandidateItem>;

    /// Get the currently selected candidate (const version)
    auto GetSelected() const -> std::optional<CandidateItem>;

    /// Reset selection to first item
    auto Reset() -> void;

    /// Check if there are any candidates
    auto IsEmpty() const -> bool;

    /// Get the number of candidates
    auto Size() const -> size_t;
};

/// CommandModeState manages the state of command input mode
/// Handles input buffer, cursor position, and candidate selection
struct CommandModeState {
    InputBuffer buffer;
    CandidateSelection candidates;
    bool show_candidates = false;

    // Input handling
    auto OnCharacter(char ch) -> void;
    auto OnBackspace() -> void;
    auto OnDelete() -> void;
    auto OnClearBuffer() -> void;

    // Cursor navigation
    auto OnCursorLeft() -> void;
    auto OnCursorRight() -> void;
    auto OnHome() -> void;
    auto OnEnd() -> void;

    // Candidate navigation
    auto OnNextCandidate() -> void;
    auto OnPrevCandidate() -> void;
    auto OnAcceptCandidate() -> void;

    // Autocomplete trigger
    auto UpdateCandidates(AutocompleteEngine& engine) -> void;

    // State queries
    auto GetBuffer() const -> const std::string&;
    auto GetCursorPos() const -> size_t;
    auto HasCandidates() const -> bool;
};

}  // namespace kano::git::commands
