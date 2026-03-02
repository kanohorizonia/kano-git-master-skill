#pragma once

#include "metadata_cache.hpp"
#include <memory>
#include <string>
#include <vector>

namespace kano::git::commands {

/// Type of completion candidate
enum class CandidateType {
    Command,      ///< Top-level command (e.g., "commit")
    Subcommand,   ///< Subcommand (e.g., "add" in "worktree add")
    Option,       ///< Command option (e.g., "--message")
    OptionValue   ///< Option value (future enhancement)
};

/// Completion phase based on input context
enum class CompletionPhase {
    Command,      ///< Completing command name (e.g., "com" -> "commit")
    Subcommand,   ///< Completing subcommand (e.g., "worktree " -> "add")
    Option,       ///< Completing option (e.g., "commit -" -> "--message")
    OptionValue   ///< Completing option value (future enhancement)
};

/// A single autocomplete candidate
struct CandidateItem {
    std::string text;           ///< Display text (e.g., "commit", "--message")
    std::string description;    ///< Human-readable description
    CandidateType type;         ///< Type of candidate
    std::string completion;     ///< Full text to insert when accepted
};

/// Context information parsed from input buffer
struct InputContext {
    std::vector<std::string> tokens;  ///< Tokenized input (excluding current token)
    std::string current_token;        ///< Token being completed (may be partial)
    CompletionPhase phase;            ///< Current completion phase
    std::string command_name;         ///< Name of command being completed (if applicable)
};

/// Autocomplete engine for command input
/// Generates context-aware completion candidates based on input buffer
class AutocompleteEngine {
public:
    /// Construct autocomplete engine with metadata cache
    /// @param metadata Shared pointer to metadata cache
    explicit AutocompleteEngine(std::shared_ptr<MetadataCache> metadata);

    /// Generate autocomplete candidates for given input
    /// @param input Current input buffer content
    /// @return Vector of completion candidates (max 10 items, sorted alphabetically)
    auto GenerateCandidates(const std::string& input) -> std::vector<CandidateItem>;

private:
    std::shared_ptr<MetadataCache> metadata_;

    /// Parse input to determine completion context
    /// @param input Input buffer content
    /// @return Parsed input context
    auto ParseInput(const std::string& input) -> InputContext;

    /// Complete command names
    /// @param prefix Partial command name
    /// @return Vector of matching command candidates
    auto CompleteCommand(const std::string& prefix) -> std::vector<CandidateItem>;

    /// Complete subcommand names
    /// @param command Parent command name
    /// @param prefix Partial subcommand name
    /// @return Vector of matching subcommand candidates
    auto CompleteSubcommand(const std::string& command, const std::string& prefix)
        -> std::vector<CandidateItem>;

    /// Complete option names
    /// @param command Command name
    /// @param prefix Partial option name (including leading '-')
    /// @return Vector of matching option candidates
    auto CompleteOption(const std::string& command, const std::string& prefix)
        -> std::vector<CandidateItem>;

    /// Filter and sort candidates
    /// @param candidates Input candidates
    /// @param max_count Maximum number of candidates to return
    /// @return Filtered and sorted candidates
    auto FilterAndSort(std::vector<CandidateItem> candidates, size_t max_count)
        -> std::vector<CandidateItem>;
};

} // namespace kano::git::commands
