#pragma once

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

// Forward declaration
namespace CLI {
class App;
}

namespace kano::git::commands {

/// Result of command execution
struct ExecutionResult {
    bool success;                                  ///< Whether execution succeeded
    std::string message;                           ///< Success or error message
    bool needs_confirmation;                       ///< Whether confirmation dialog is needed
    std::function<void()> confirmed_action;        ///< Action to execute after confirmation
};

/// Executes commands parsed from command input
class CommandExecutor {
public:
    /// Construct executor with CLI11 app and repository path
    /// @param app CLI11 application containing command definitions
    /// @param repo Path to git repository
    explicit CommandExecutor(CLI::App& app, const std::filesystem::path& repo);

    /// Execute a command line string
    /// @param command_line Full command line (e.g., "commit --message 'Fix bug'")
    /// @return Execution result with success status and message
    auto Execute(const std::string& command_line) -> ExecutionResult;

    /// Parse command line into tokens, handling quotes and escapes
    /// @param input Raw command line string
    /// @return Vector of tokens
    /// @note Made public for testing purposes
    auto ParseCommandLine(const std::string& input) -> std::vector<std::string>;

    /// Check if command needs confirmation before execution
    /// @param command Command name
    /// @return True if confirmation required
    /// @note Made public for testing purposes
    auto NeedsConfirmation(const std::string& command) -> bool;

    /// Build confirmation message for destructive command
    /// @param command Command name
    /// @return Confirmation prompt message
    /// @note Made public for testing purposes
    auto BuildConfirmationMessage(const std::string& command) -> std::string;

private:
    CLI::App& app_;                    ///< Reference to CLI11 app tree
    std::filesystem::path repo_;       ///< Repository path

    /// Execute command with parsed tokens
    /// @param tokens Command and arguments
    /// @return Execution result
    auto ExecuteInternal(const std::vector<std::string>& tokens) -> ExecutionResult;
};

}  // namespace kano::git::commands
