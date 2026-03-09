#pragma once

#include <CLI/CLI.hpp>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace kano::git::commands {

/// Metadata for a command-line option
struct OptionMetadata {
    std::vector<std::string> long_names;   ///< Long option names (e.g., "message")
    std::vector<std::string> short_names;  ///< Short option names (e.g., "m")
    std::string description;               ///< Human-readable description
    bool takes_value;                      ///< Whether the option requires a value
    bool required;                         ///< Whether the option is required
    bool multi;                            ///< Whether the option can be specified multiple times
    std::string default_value;             ///< Default value if any
};

/// Metadata for a command
struct CommandMetadata {
    std::string name;                      ///< Command name
    std::string description;               ///< Human-readable description
    std::vector<std::string> subcommands;  ///< Names of subcommands
    std::vector<OptionMetadata> options;   ///< Available options
    bool allow_extras;                     ///< Whether the command allows extra arguments
};

/// Cache for command metadata extracted from CLI11 app tree
/// Provides efficient query interface for autocomplete and command introspection
class MetadataCache {
public:
    /// Construct cache and extract metadata from CLI11 app
    /// @param app The CLI11 application to extract metadata from
    explicit MetadataCache(const CLI::App& app);

    /// Get all top-level commands
    /// @return Vector of all command metadata
    auto GetAllCommands() -> std::vector<CommandMetadata>;

    /// Get metadata for a specific command
    /// @param name Command name (e.g., "commit", "worktree")
    /// @return Command metadata if found, nullopt otherwise
    auto GetCommand(const std::string& name) -> std::optional<CommandMetadata>;

    /// Get subcommand names for a specific command
    /// @param command Parent command name
    /// @return Vector of subcommand names
    auto GetSubcommands(const std::string& command) -> std::vector<std::string>;

    /// Get options for a specific command
    /// @param command Command name
    /// @return Vector of option metadata
    auto GetOptions(const std::string& command) -> std::vector<OptionMetadata>;

    /// Refresh cache from CLI11 app (call after registry changes)
    /// @param app The CLI11 application to extract metadata from
    auto Refresh(const CLI::App& app) -> void;

private:
    /// Cached command metadata indexed by command name
    std::unordered_map<std::string, CommandMetadata> commands_;

    /// Extract all metadata from CLI11 app tree
    /// @param app The CLI11 application to extract from
    auto ExtractMetadata(const CLI::App& app) -> void;

    /// Extract metadata for a single command
    /// @param cmd CLI11 command to extract from
    /// @param path Full command path (e.g., "worktree add")
    /// @return Extracted command metadata
    auto ExtractCommand(const CLI::App* cmd, const std::string& path) -> CommandMetadata;

    /// Extract option metadata from a command
    /// @param cmd CLI11 command to extract options from
    /// @return Vector of extracted option metadata
    auto ExtractOptions(const CLI::App* cmd) -> std::vector<OptionMetadata>;
};

} // namespace kano::git::commands
