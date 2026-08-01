#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace kano::git::commands {

enum class TuiCommandScopeMode {
    Workspace,
    SelectedRepo,
};

struct TuiCommandScopeSnapshot {
    TuiCommandScopeMode mode = TuiCommandScopeMode::Workspace;
    std::filesystem::path workspaceRoot;
    std::filesystem::path selectedRepoPath;
    std::string selectedRepoDisplay;
};

struct TuiScopedCommand {
    std::filesystem::path workingDirectory;
    std::vector<std::string> arguments;
    std::string scopeLabel;
};

[[nodiscard]] auto ParseTuiCommandLine(
    std::string_view InLine) -> std::vector<std::string>;

[[nodiscard]] auto BuildTuiCommandScopeLabel(
    const TuiCommandScopeSnapshot& InScope) -> std::string;

[[nodiscard]] auto BuildTuiScopedCommand(
    std::string_view InLine,
    const TuiCommandScopeSnapshot& InScope)
    -> std::optional<TuiScopedCommand>;

/// The TUI is an audit console, not a manual mutation client. Only explicitly
/// allowlisted read-only KOG commands may be launched from command mode.
[[nodiscard]] auto IsTuiAuditOnlyCommand(
    const std::vector<std::string>& InArguments) -> bool;

/// Validate untrusted command text, apply only TUI-owned repository scope,
/// and force ephemeral discovery for commands that otherwise maintain caches.
[[nodiscard]] auto BuildTuiAuditCommand(
    std::string_view InLine,
    const TuiCommandScopeSnapshot& InScope)
    -> std::optional<TuiScopedCommand>;

}  // namespace kano::git::commands
