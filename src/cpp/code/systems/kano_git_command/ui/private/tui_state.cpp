#include "tui_state.hpp"
#include "autocomplete_engine.hpp"
#include "tui_command_scope.hpp"

#include <algorithm>
#include <cctype>

namespace kano::git::commands {

namespace {

auto TuiStateToLower(const std::string& value) -> std::string {
    std::string out = value;
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return out;
}

auto TuiStateTrim(const std::string& value) -> std::string {
    std::size_t start = 0;
    while (start < value.size() && std::isspace(static_cast<unsigned char>(value[start])) != 0) {
        start += 1;
    }
    std::size_t end = value.size();
    while (end > start && std::isspace(static_cast<unsigned char>(value[end - 1])) != 0) {
        end -= 1;
    }
    return value.substr(start, end - start);
}

} // namespace

auto TuiState::HandleEvent(const std::string& event_type, char ch) -> bool {
    switch (mode) {
        case TuiMode::Normal:
            return HandleNormalMode(event_type, ch);
        case TuiMode::Command:
            return HandleCommandMode(event_type, ch);
        case TuiMode::CommandPalette:
            return HandlePaletteMode(event_type, ch);
        case TuiMode::Help:
            return HandleHelpMode(event_type, ch);
        case TuiMode::Confirm:
            return HandleConfirmMode(event_type, ch);
        default:
            return false;
    }
}

auto TuiState::GetMode() const -> TuiMode {
    return mode;
}

auto TuiState::SetFooterMessage(const std::string& message, bool is_error) -> void {
    footer_message = message;
    footer_is_error = is_error;
}

auto TuiState::ClearFooterMessage() -> void {
    footer_message.clear();
    footer_is_error = false;
}

// Mode-specific event handlers

auto TuiState::HandleNormalMode(const std::string& event_type, char ch) -> bool {
    // Enter Command_Mode on ':' key
    if (event_type == "character" && ch == ':') {
        EnterCommandMode();
        return true;
    }
    
    // Enter Command Palette on Ctrl+P
    if (event_type == "ctrl_p") {
        EnterPaletteMode();
        return true;
    }
    
    // Enter Help on '?' key
    if (event_type == "character" && ch == '?') {
        EnterHelpMode();
        return true;
    }
    
    // Single-key shortcuts are handled by the TUI main loop
    // We don't handle them here to maintain backward compatibility
    return false;
}

auto TuiState::HandleCommandMode(const std::string& event_type, char ch) -> bool {
    // Exit on Escape
    if (event_type == "escape") {
        ExitCommandMode();
        return true;
    }
    
    // Exit on Enter with empty buffer
    if (event_type == "enter" && command_state.GetBuffer().empty()) {
        ExitCommandMode();
        return true;
    }
    
    // Accept selected candidate on Enter before command execution
    if (event_type == "enter" && command_state.HasCandidates()) {
        command_state.OnAcceptCandidate();
        UpdateCommandCandidates();
        return true;
    }

    // Execute command on Enter with non-empty buffer
    if (event_type == "enter" && !command_state.GetBuffer().empty()) {
        const auto commandLine = TuiStateTrim(command_state.GetBuffer());
        if (TuiStateToLower(commandLine) == "help" || TuiStateToLower(commandLine) == ":help") {
            EnterHelpMode();
            return true;
        }

        if (!IsTuiAuditOnlyCommand(ParseTuiCommandLine(commandLine))) {
            SetFooterMessage(
                "Audit-only TUI accepts a read-only command with no options",
                true);
            return true;
        }

        // TuiState owns input only. The dashboard validates scope, injects
        // no-cache guards, and owns the single bounded subprocess runner.
        SetFooterMessage("Audit command runner unavailable", true);
        return true;
    }
    
    // Handle character input
    if (event_type == "character") {
        command_state.OnCharacter(ch);
        // Trigger autocomplete if engine is available
        UpdateCommandCandidates();
        // Clear error message when user starts typing
        if (footer_is_error) {
            ClearFooterMessage();
        }
        return true;
    }
    
    // Handle backspace
    if (event_type == "backspace") {
        command_state.OnBackspace();
        UpdateCommandCandidates();
        return true;
    }
    
    // Handle delete
    if (event_type == "delete") {
        command_state.OnDelete();
        UpdateCommandCandidates();
        return true;
    }
    
    // Handle Ctrl+U (clear buffer)
    if (event_type == "ctrl_u") {
        command_state.OnClearBuffer();
        UpdateCommandCandidates();
        return true;
    }
    
    // Handle cursor movement
    if (event_type == "left") {
        command_state.OnCursorLeft();
        return true;
    }
    if (event_type == "right") {
        command_state.OnCursorRight();
        return true;
    }
    if (event_type == "home") {
        command_state.OnHome();
        return true;
    }
    if (event_type == "end") {
        command_state.OnEnd();
        return true;
    }
    
    // Handle candidate navigation
    if (event_type == "tab") {
        if (command_state.HasCandidates() && command_state.candidates.Size() == 1) {
            command_state.OnAcceptCandidate();
            UpdateCommandCandidates();
            return true;
        }
        command_state.OnNextCandidate();
        return true;
    }
    if (event_type == "down") {
        command_state.OnNextCandidate();
        return true;
    }
    if (event_type == "shift_tab" || event_type == "up") {
        command_state.OnPrevCandidate();
        return true;
    }
    
    return false;
}

auto TuiState::HandlePaletteMode(const std::string& event_type, char ch) -> bool {
    // Exit on Escape
    if (event_type == "escape") {
        ExitPaletteMode();
        return true;
    }

    if (event_type == "up") {
        if (!palette_state.filtered_commands.empty()) {
            const int size = static_cast<int>(palette_state.filtered_commands.size());
            palette_state.selected_index = (palette_state.selected_index - 1 + size) % size;
        }
        return true;
    }

    if (event_type == "down") {
        if (!palette_state.filtered_commands.empty()) {
            const int size = static_cast<int>(palette_state.filtered_commands.size());
            palette_state.selected_index = (palette_state.selected_index + 1) % size;
        }
        return true;
    }

    if (event_type == "backspace") {
        if (!palette_state.search_query.empty()) {
            palette_state.search_query.pop_back();
            UpdatePaletteFilter();
        }
        return true;
    }

    if (event_type == "character") {
        if (ch >= 32 && ch <= 126) {
            palette_state.search_query.push_back(ch);
            UpdatePaletteFilter();
        }
        return true;
    }

    if (event_type == "enter") {
        if (palette_state.filtered_commands.empty()) {
            ExitPaletteMode();
            return true;
        }

        const auto selected = palette_state.filtered_commands[std::clamp(
            palette_state.selected_index,
            0,
            static_cast<int>(palette_state.filtered_commands.size()) - 1)];

        mode = TuiMode::Command;
        command_state = CommandModeState{};
        for (const char c : selected.name) {
            command_state.OnCharacter(c);
        }
        command_state.OnCharacter(' ');
        UpdateCommandCandidates();
        return true;
    }

    return false;
}

auto TuiState::HandleHelpMode(const std::string& event_type, char ch) -> bool {
    // Exit on Escape or 'q'
    if (event_type == "escape" || (event_type == "character" && ch == 'q')) {
        ExitHelpMode();
        return true;
    }
    
    return false;
}

auto TuiState::HandleConfirmMode(const std::string& event_type, char ch) -> bool {
    if (!confirm_state.active) {
        mode = TuiMode::Normal;
        return false;
    }

    if (event_type == "character" && (ch == 'y' || ch == 'Y')) {
        if (confirm_state.on_confirm) {
            confirm_state.on_confirm();
        }
        confirm_state.active = false;
        mode = TuiMode::Normal;
        SetFooterMessage("Command confirmed", false);
        return true;
    }

    if (event_type == "character" && (ch == 'n' || ch == 'N')) {
        if (confirm_state.on_cancel) {
            confirm_state.on_cancel();
        } else {
            mode = TuiMode::Command;
        }
        confirm_state.active = false;
        return true;
    }

    if (event_type == "escape") {
        if (confirm_state.on_cancel) {
            confirm_state.on_cancel();
        } else {
            mode = TuiMode::Command;
        }
        confirm_state.active = false;
        return true;
    }

    return false;
}

// Mode transitions

auto TuiState::EnterCommandMode() -> void {
    mode = TuiMode::Command;
    command_state = CommandModeState{};  // Reset state
    ClearFooterMessage();
}

auto TuiState::ExitCommandMode() -> void {
    mode = TuiMode::Normal;
    command_state = CommandModeState{};  // Clear buffer
    ClearFooterMessage();
}

auto TuiState::EnterPaletteMode() -> void {
    mode = TuiMode::CommandPalette;
    palette_state = CommandPaletteState{};  // Reset state
    palette_state.all_commands = BuildPaletteItems();
    palette_state.filtered_commands = palette_state.all_commands;
    palette_state.selected_index = 0;
    ClearFooterMessage();
}

auto TuiState::ExitPaletteMode() -> void {
    mode = TuiMode::Normal;
    palette_state = CommandPaletteState{};  // Clear state
    ClearFooterMessage();
}

auto TuiState::EnterHelpMode() -> void {
    mode = TuiMode::Help;
    help_state = HelpPanelState{};  // Reset state
    help_state.visible = true;
    ClearFooterMessage();
}

auto TuiState::ExitHelpMode() -> void {
    mode = TuiMode::Normal;
    help_state.visible = false;
    ClearFooterMessage();
}

auto TuiState::BuildPaletteItems() -> std::vector<PaletteItem> {
    std::vector<PaletteItem> items;
    if (!autocomplete_engine) {
        return items;
    }

    const auto candidates = autocomplete_engine->GenerateCandidates("");
    for (const auto& candidate : candidates) {
        if (candidate.type != CandidateType::Command ||
            !IsTuiAuditOnlyCommand({candidate.completion})) {
            continue;
        }
        items.push_back(PaletteItem{
            .name = candidate.completion,
            .description = candidate.description,
            .category = CategorizeCommand(candidate.completion),
        });
    }

    std::sort(items.begin(), items.end(), [](const PaletteItem& a, const PaletteItem& b) {
        if (a.category != b.category) {
            return a.category < b.category;
        }
        return a.name < b.name;
    });

    return items;
}

auto TuiState::UpdateCommandCandidates() -> void {
    if (!autocomplete_engine) {
        command_state.candidates.items.clear();
        command_state.candidates.Reset();
        command_state.show_candidates = false;
        return;
    }

    command_state.UpdateCandidates(*autocomplete_engine);
    const bool completingCommand =
        command_state.GetBuffer().find_first_of(" \t\r\n") ==
        std::string::npos;
    auto& candidates = command_state.candidates.items;
    candidates.erase(
        std::remove_if(
            candidates.begin(),
            candidates.end(),
            [&](const CandidateItem& InCandidate) {
                return !completingCommand ||
                    InCandidate.type != CandidateType::Command ||
                    !IsTuiAuditOnlyCommand({InCandidate.completion});
            }),
        candidates.end());
    command_state.candidates.Reset();
    command_state.show_candidates = !candidates.empty();
}

auto TuiState::UpdatePaletteFilter() -> void {
    palette_state.filtered_commands.clear();

    const auto query = TuiStateToLower(TuiStateTrim(palette_state.search_query));
    for (const auto& item : palette_state.all_commands) {
        if (query.empty()) {
            palette_state.filtered_commands.push_back(item);
            continue;
        }
        const auto name = TuiStateToLower(item.name);
        const auto desc = TuiStateToLower(item.description);
        if (name.find(query) != std::string::npos || desc.find(query) != std::string::npos) {
            palette_state.filtered_commands.push_back(item);
        }
    }

    if (palette_state.filtered_commands.empty()) {
        palette_state.selected_index = 0;
        return;
    }

    palette_state.selected_index = std::clamp(
        palette_state.selected_index,
        0,
        static_cast<int>(palette_state.filtered_commands.size()) - 1);
}

auto TuiState::CategorizeCommand(const std::string& name) -> std::string {
    const auto normalized = TuiStateToLower(name);
    if (normalized == "status" || normalized == "workspace" || normalized == "sync" || normalized == "fetch") {
        return "Repository";
    }
    if (normalized == "tui" || normalized == "guide" || normalized == "doctor" || normalized == "meta") {
        return "System";
    }
    return "Other";
}

}  // namespace kano::git::commands
