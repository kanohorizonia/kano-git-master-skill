#include "command_executor.hpp"

#include <CLI/CLI.hpp>
#include <algorithm>
#include <cctype>
#include <iostream>
#include <sstream>

namespace kano::git::commands {

CommandExecutor::CommandExecutor(CLI::App& app, const std::filesystem::path& repo)
    : app_(app), repo_(repo) {}

auto CommandExecutor::ParseCommandLine(const std::string& input) -> std::vector<std::string> {
    std::vector<std::string> tokens;
    std::string current_token;
    bool in_quotes = false;
    bool escape_next = false;

    for (size_t i = 0; i < input.length(); ++i) {
        char ch = input[i];

        if (escape_next) {
            // Handle escaped character
            current_token += ch;
            escape_next = false;
            continue;
        }

        if (ch == '\\') {
            // Next character is escaped
            escape_next = true;
            continue;
        }

        if (ch == '"' || ch == '\'') {
            // Toggle quote mode
            in_quotes = !in_quotes;
            continue;
        }

        if (std::isspace(static_cast<unsigned char>(ch)) && !in_quotes) {
            // Whitespace outside quotes - end current token
            if (!current_token.empty()) {
                tokens.push_back(current_token);
                current_token.clear();
            }
            continue;
        }

        // Regular character - add to current token
        current_token += ch;
    }

    // Add final token if any
    if (!current_token.empty()) {
        tokens.push_back(current_token);
    }

    return tokens;
}

auto CommandExecutor::Execute(const std::string& command_line) -> ExecutionResult {
    try {
        // Parse command line into tokens
        auto tokens = ParseCommandLine(command_line);
        if (tokens.empty()) {
            return {false, "Empty command", false, nullptr};
        }

        // Check if command needs confirmation
        if (NeedsConfirmation(tokens[0])) {
            return {
                true,
                BuildConfirmationMessage(tokens[0]),
                true,
                [this, tokens]() { ExecuteInternal(tokens); }
            };
        }

        // Execute directly
        return ExecuteInternal(tokens);

    } catch (const std::exception& e) {
        return {false, "Command execution failed: " + std::string(e.what()), false, nullptr};
    }
}

auto CommandExecutor::ExecuteInternal(const std::vector<std::string>& tokens) -> ExecutionResult {
    if (tokens.empty()) {
        return {false, "Empty command", false, nullptr};
    }

    // Find command in CLI11 app tree
    CLI::App* cmd = nullptr;
    try {
        cmd = app_.get_subcommand(tokens[0]);
    } catch (const std::exception&) {
        return {false, "Unknown command: " + tokens[0], false, nullptr};
    }

    if (!cmd) {
        return {false, "Unknown command: " + tokens[0], false, nullptr};
    }

    // Parse command with CLI11
    try {
        // Reset the command state
        cmd->clear();

        // CLI11 parse expects only the arguments (not the command name)
        std::vector<std::string> args(tokens.begin() + 1, tokens.end());
        
        // Parse the arguments
        cmd->parse(args);

        // If we get here, parsing succeeded
        // In a real implementation, this would invoke the command handler
        // For now, we return success
        return {true, "Command executed successfully", false, nullptr};

    } catch (const CLI::ParseError& e) {
        // CLI11 parse error - invalid syntax
        return {false, "Invalid command syntax: " + std::string(e.what()), false, nullptr};
    } catch (const std::exception& e) {
        // Other execution error
        return {false, "Command execution failed: " + std::string(e.what()), false, nullptr};
    }
}

auto CommandExecutor::NeedsConfirmation(const std::string& command) -> bool {
    // List of commands that require confirmation
    static const std::vector<std::string> destructive_commands = {
        "push",
        "force-push",
        "delete-branch",
        "reset",
        "rebase"
    };

    // Check if command is in the destructive list
    return std::find(destructive_commands.begin(), destructive_commands.end(), command) !=
           destructive_commands.end();
}

auto CommandExecutor::BuildConfirmationMessage(const std::string& command) -> std::string {
    if (command == "push") {
        return "Push changes to remote? This will update the remote repository.";
    }
    if (command == "force-push") {
        return "Force push to remote? This will overwrite remote history!";
    }
    if (command == "delete-branch") {
        return "Delete branch? This action cannot be undone.";
    }
    if (command == "reset") {
        return "Reset repository state? Uncommitted changes may be lost.";
    }
    if (command == "rebase") {
        return "Rebase commits? This will rewrite commit history.";
    }

    return "Execute " + command + "? This is a potentially destructive operation.";
}

}  // namespace kano::git::commands

// benchmark-probe:20260319T061728Z
