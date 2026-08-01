#include "tui_command_scope.hpp"

#include <array>
#include <cctype>
#include <utility>

namespace kano::git::commands {
namespace {

constexpr std::array<std::string_view, 9> kTargetScopedCommands{
    "commit",
    "ca",
    "commit-push",
    "cp",
    "cpa",
    "push",
    "log",
    "slog",
    "amend",
};

constexpr std::array<std::string_view, 6> kExplicitTargetOptions{
    "--repos",
    "--repo",
    "--repo-root",
    "--root",
    "--source",
    "--target",
};

constexpr std::array<std::string_view, 9> kAuditOnlyCommands{
    "status",
    "log",
    "slog",
    "doctor",
    "version",
    "help",
    "--help",
    "-h",
    "--version",
};

[[nodiscard]] auto SupportsTargetScope(
    const std::string_view InCommand) -> bool {
    for (const auto command : kTargetScopedCommands) {
        if (InCommand == command) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] auto HasOption(
    const std::vector<std::string>& InArguments,
    const std::string_view InOption) -> bool {
    for (const auto& argument : InArguments) {
        if (argument == InOption) {
            return true;
        }
        if (argument.size() > InOption.size() &&
            argument.compare(0, InOption.size(), InOption) == 0 &&
            argument[InOption.size()] == '=') {
            return true;
        }
    }
    return false;
}

[[nodiscard]] auto HasExplicitTarget(
    const std::vector<std::string>& InArguments) -> bool {
    for (const auto option : kExplicitTargetOptions) {
        if (HasOption(InArguments, option)) {
            return true;
        }
    }
    return false;
}

}  // namespace

auto IsTuiAuditOnlyCommand(
    const std::vector<std::string>& InArguments) -> bool {
    // Options on otherwise read-oriented commands can still write files,
    // repair configuration, or refresh caches. Accept only the user-provided
    // command token; trusted scope arguments are added after this gate.
    if (InArguments.size() != 1) {
        return false;
    }
    for (const auto command : kAuditOnlyCommands) {
        if (InArguments.front() == command) {
            return true;
        }
    }
    return false;
}

auto ParseTuiCommandLine(
    const std::string_view InLine) -> std::vector<std::string> {
    std::vector<std::string> tokens;
    std::string currentToken;
    bool bInQuotes = false;
    bool bEscapeNext = false;
    for (const char ch : InLine) {
        if (bEscapeNext) {
            currentToken += ch;
            bEscapeNext = false;
            continue;
        }
        if (ch == '\\') {
            bEscapeNext = true;
            continue;
        }
        if (ch == '"' || ch == '\'') {
            bInQuotes = !bInQuotes;
            continue;
        }
        if (std::isspace(static_cast<unsigned char>(ch)) != 0 &&
            !bInQuotes) {
            if (!currentToken.empty()) {
                tokens.push_back(currentToken);
                currentToken.clear();
            }
            continue;
        }
        currentToken += ch;
    }
    if (!currentToken.empty()) {
        tokens.push_back(std::move(currentToken));
    }
    return tokens;
}

auto BuildTuiCommandScopeLabel(
    const TuiCommandScopeSnapshot& InScope) -> std::string {
    if (InScope.mode == TuiCommandScopeMode::SelectedRepo) {
        return "selected: " + InScope.selectedRepoDisplay;
    }
    return "workspace: " +
        InScope.workspaceRoot.generic_string();
}

auto BuildTuiScopedCommand(
    const std::string_view InLine,
    const TuiCommandScopeSnapshot& InScope)
    -> std::optional<TuiScopedCommand> {
    auto arguments = ParseTuiCommandLine(InLine);
    if (arguments.empty()) {
        return std::nullopt;
    }

    if (InScope.mode == TuiCommandScopeMode::SelectedRepo &&
        SupportsTargetScope(arguments.front()) &&
        !HasExplicitTarget(arguments)) {
        arguments.push_back("--repo-root");
        arguments.push_back(
            InScope.workspaceRoot.generic_string());
        arguments.push_back(InScope.selectedRepoDisplay);
    }

    return TuiScopedCommand{
        .workingDirectory =
            InScope.mode == TuiCommandScopeMode::SelectedRepo
            ? InScope.selectedRepoPath
            : InScope.workspaceRoot,
        .arguments = std::move(arguments),
        .scopeLabel = BuildTuiCommandScopeLabel(InScope),
    };
}

auto BuildTuiAuditCommand(
    const std::string_view InLine,
    const TuiCommandScopeSnapshot& InScope)
    -> std::optional<TuiScopedCommand> {
    const auto userArguments = ParseTuiCommandLine(InLine);
    if (!IsTuiAuditOnlyCommand(userArguments)) {
        return std::nullopt;
    }

    auto command = BuildTuiScopedCommand(InLine, InScope);
    if (!command.has_value() || command->arguments.empty()) {
        return std::nullopt;
    }
    const auto& name = command->arguments.front();
    if (name == "status" || name == "log" || name == "slog") {
        command->arguments.insert(
            command->arguments.begin() + 1,
            "--no-cache");
    }
    return command;
}

}  // namespace kano::git::commands
