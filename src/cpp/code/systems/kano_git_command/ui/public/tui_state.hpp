#pragma once

#include "command_mode_state.hpp"
#include <string>
#include <memory>
#include <functional>
#include <vector>

namespace kano::git::commands {

// Forward declarations
class AutocompleteEngine;
class CommandExecutor;

struct PaletteItem {
    std::string name;
    std::string description;
    std::string category;
};

/// TUI operating modes
enum class TuiMode {
    Normal,          // Default mode with single-key shortcuts
    Command,         // Command input mode (vim-style ':' commands)
    CommandPalette,  // Command browser/palette (Ctrl+P)
    Help,            // Help panel display
    Confirm          // Confirmation dialog for destructive operations
};

/// State for command palette mode
struct CommandPaletteState {
    std::vector<PaletteItem> all_commands;
    std::vector<PaletteItem> filtered_commands;
    std::string search_query;
    int selected_index = 0;
};

/// State for help panel mode
struct HelpPanelState {
    bool visible = false;
};

/// State for confirmation dialogs (already exists in tui_cmd.cpp)
struct ConfirmState {
    bool active = false;
    std::string message;
    std::function<void()> on_confirm;
    std::function<void()> on_cancel;
};

/// Main TUI state manager
/// Coordinates mode transitions and event routing
struct TuiState {
    TuiMode mode = TuiMode::Normal;
    
    // Mode-specific state
    CommandModeState command_state;
    CommandPaletteState palette_state;
    HelpPanelState help_state;
    ConfirmState confirm_state;
    
    // Footer message display
    std::string footer_message;
    bool footer_is_error = false;
    
    // Dependencies (injected)
    std::shared_ptr<AutocompleteEngine> autocomplete_engine;
    std::shared_ptr<CommandExecutor> command_executor;
    
    /// Handle keyboard event based on current mode
    /// Returns true if event was handled, false otherwise
    auto HandleEvent(const std::string& event_type, char ch = '\0') -> bool;
    
    /// Get current mode
    auto GetMode() const -> TuiMode;
    
    /// Set footer message
    auto SetFooterMessage(const std::string& message, bool is_error = false) -> void;
    
    /// Clear footer message
    auto ClearFooterMessage() -> void;

private:
    // Mode-specific event handlers
    auto HandleNormalMode(const std::string& event_type, char ch) -> bool;
    auto HandleCommandMode(const std::string& event_type, char ch) -> bool;
    auto HandlePaletteMode(const std::string& event_type, char ch) -> bool;
    auto HandleHelpMode(const std::string& event_type, char ch) -> bool;
    auto HandleConfirmMode(const std::string& event_type, char ch) -> bool;
    
    // Mode transitions
    auto EnterCommandMode() -> void;
    auto ExitCommandMode() -> void;
    auto EnterPaletteMode() -> void;
    auto ExitPaletteMode() -> void;
    auto EnterHelpMode() -> void;
    auto ExitHelpMode() -> void;

    auto BuildPaletteItems() -> std::vector<PaletteItem>;
    auto UpdatePaletteFilter() -> void;
    auto CategorizeCommand(const std::string& name) -> std::string;
};

}  // namespace kano::git::commands
