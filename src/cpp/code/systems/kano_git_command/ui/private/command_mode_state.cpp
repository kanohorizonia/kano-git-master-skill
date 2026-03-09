#include "command_mode_state.hpp"
#include "autocomplete_engine.hpp"

namespace kano::git::commands {

// ============================================================================
// InputBuffer Implementation
// ============================================================================

auto InputBuffer::Insert(char ch) -> void {
    text.insert(cursor_pos, 1, ch);
    cursor_pos++;
}

auto InputBuffer::Backspace() -> void {
    if (cursor_pos > 0) {
        text.erase(cursor_pos - 1, 1);
        cursor_pos--;
    }
}

auto InputBuffer::Delete() -> void {
    if (cursor_pos < text.length()) {
        text.erase(cursor_pos, 1);
    }
}

auto InputBuffer::Clear() -> void {
    text.clear();
    cursor_pos = 0;
}

auto InputBuffer::MoveCursor(int delta) -> void {
    // Convert to signed for calculation
    auto new_pos = static_cast<int>(cursor_pos) + delta;
    
    // Clamp to valid range [0, text.length()]
    if (new_pos < 0) {
        cursor_pos = 0;
    } else if (new_pos > static_cast<int>(text.length())) {
        cursor_pos = text.length();
    } else {
        cursor_pos = static_cast<size_t>(new_pos);
    }
}

auto InputBuffer::Home() -> void {
    cursor_pos = 0;
}

auto InputBuffer::End() -> void {
    cursor_pos = text.length();
}

// ============================================================================
// CandidateSelection Implementation
// ============================================================================

auto CandidateSelection::SelectNext() -> void {
    if (items.empty()) return;
    selected_index = (selected_index + 1) % static_cast<int>(items.size());
}

auto CandidateSelection::SelectPrev() -> void {
    if (items.empty()) return;
    selected_index = (selected_index - 1 + static_cast<int>(items.size())) % static_cast<int>(items.size());
}

auto CandidateSelection::GetSelected() -> std::optional<CandidateItem> {
    if (items.empty() || selected_index < 0 || selected_index >= static_cast<int>(items.size())) {
        return std::nullopt;
    }
    return items[selected_index];
}

auto CandidateSelection::GetSelected() const -> std::optional<CandidateItem> {
    if (items.empty() || selected_index < 0 || selected_index >= static_cast<int>(items.size())) {
        return std::nullopt;
    }
    return items[selected_index];
}

auto CandidateSelection::Reset() -> void {
    selected_index = 0;
}

auto CandidateSelection::IsEmpty() const -> bool {
    return items.empty();
}

auto CandidateSelection::Size() const -> size_t {
    return items.size();
}

// ============================================================================
// CommandModeState Implementation
// ============================================================================

auto CommandModeState::OnCharacter(char ch) -> void {
    buffer.Insert(ch);
}

auto CommandModeState::OnBackspace() -> void {
    buffer.Backspace();
}

auto CommandModeState::OnDelete() -> void {
    buffer.Delete();
}

auto CommandModeState::OnClearBuffer() -> void {
    buffer.Clear();
}

auto CommandModeState::OnCursorLeft() -> void {
    buffer.MoveCursor(-1);
}

auto CommandModeState::OnCursorRight() -> void {
    buffer.MoveCursor(1);
}

auto CommandModeState::OnHome() -> void {
    buffer.Home();
}

auto CommandModeState::OnEnd() -> void {
    buffer.End();
}

auto CommandModeState::OnNextCandidate() -> void {
    candidates.SelectNext();
}

auto CommandModeState::OnPrevCandidate() -> void {
    candidates.SelectPrev();
}

auto CommandModeState::OnAcceptCandidate() -> void {
    auto selected = candidates.GetSelected();
    if (selected.has_value()) {
        // Replace buffer with candidate completion text
        buffer.text = selected->completion;
        buffer.cursor_pos = buffer.text.length();
        
        // Add trailing space for continued input
        buffer.Insert(' ');
        
        // Clear candidates after acceptance
        candidates.items.clear();
        candidates.Reset();
        show_candidates = false;
    }
}

auto CommandModeState::UpdateCandidates(AutocompleteEngine& engine) -> void {
    // Generate new candidates based on current buffer
    auto new_candidates = engine.GenerateCandidates(buffer.text);
    
    // Update candidate list
    candidates.items = std::move(new_candidates);
    candidates.Reset();
    
    // Show candidates if we have any
    show_candidates = !candidates.IsEmpty();
}

auto CommandModeState::GetBuffer() const -> const std::string& {
    return buffer.text;
}

auto CommandModeState::GetCursorPos() const -> size_t {
    return buffer.cursor_pos;
}

auto CommandModeState::HasCandidates() const -> bool {
    return show_candidates && !candidates.IsEmpty();
}

}  // namespace kano::git::commands
