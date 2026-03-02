#include "generators/command_string_generator.hpp"
#include <random>
#include <algorithm>

namespace tui_test {

// Random Command String Generator Implementation
RandomCommandStringGenerator::RandomCommandStringGenerator() {
    current_command_ = generate_random_command();
}

auto RandomCommandStringGenerator::get() const -> const std::string& {
    return current_command_;
}

auto RandomCommandStringGenerator::next() -> bool {
    if (++iteration_ >= max_iterations_) {
        return false;
    }
    current_command_ = generate_random_command();
    return true;
}

auto RandomCommandStringGenerator::generate_random_command() -> std::string {
    static std::random_device rd;
    static std::mt19937 gen(rd());
    
    // Common git-like commands
    static const std::vector<std::string> commands = {
        "commit", "push", "pull", "fetch", "status", "log", "diff",
        "branch", "checkout", "merge", "rebase", "cherry-pick",
        "worktree", "refresh", "history", "filter"
    };
    
    // Common options
    static const std::vector<std::string> options = {
        "--message", "--all", "--force", "--amend", "--no-verify",
        "-m", "-a", "-f", "--verbose", "--quiet"
    };
    
    std::uniform_int_distribution<> cmd_dist(0, commands.size() - 1);
    std::string result = commands[cmd_dist(gen)];
    
    // Randomly add options (0-3 options)
    std::uniform_int_distribution<> opt_count_dist(0, 3);
    size_t opt_count = opt_count_dist(gen);
    
    for (size_t i = 0; i < opt_count; ++i) {
        std::uniform_int_distribution<> opt_dist(0, options.size() - 1);
        result += " " + options[opt_dist(gen)];
        
        // Some options take values
        if (options[opt_dist(gen)] == "--message" || options[opt_dist(gen)] == "-m") {
            result += " \"test message\"";
        }
    }
    
    return result;
}

// Valid Command Prefix Generator Implementation
ValidCommandPrefixGenerator::ValidCommandPrefixGenerator() {
    // Generate prefixes for common commands
    prefixes_ = {
        "", "c", "co", "com", "comm", "commi", "commit",
        "p", "pu", "pus", "push",
        "f", "fe", "fet", "fetc", "fetch",
        "r", "re", "ref", "refr", "refre", "refres", "refresh",
        "h", "hi", "his", "hist", "histo", "histor", "history",
        "w", "wo", "wor", "work", "workt", "worktr", "worktre", "worktree"
    };
    current_index_ = 0;
}

auto ValidCommandPrefixGenerator::get() const -> const std::string& {
    return prefixes_[current_index_];
}

auto ValidCommandPrefixGenerator::next() -> bool {
    if (++current_index_ >= prefixes_.size()) {
        return false;
    }
    return true;
}

// Invalid Command String Generator Implementation
InvalidCommandStringGenerator::InvalidCommandStringGenerator() {
    invalid_commands_ = {
        "xyz123",           // Non-existent command
        "comit",            // Typo
        "psh",              // Typo
        "fetc",             // Incomplete but not a valid prefix
        "!!!",              // Special characters only
        "commit --xyz",     // Invalid option
        "push --force-xyz", // Invalid option variant
        "   ",              // Whitespace only
        "commit\n",         // Contains newline
        "push\t--force"     // Contains tab
    };
    current_index_ = 0;
}

auto InvalidCommandStringGenerator::get() const -> const std::string& {
    return invalid_commands_[current_index_];
}

auto InvalidCommandStringGenerator::next() -> bool {
    if (++current_index_ >= invalid_commands_.size()) {
        return false;
    }
    return true;
}

// Command With Options Generator Implementation
CommandWithOptionsGenerator::CommandWithOptionsGenerator() {
    current_command_ = generate_command_with_options();
}

auto CommandWithOptionsGenerator::get() const -> const CommandWithOptions& {
    return current_command_;
}

auto CommandWithOptionsGenerator::next() -> bool {
    if (++iteration_ >= max_iterations_) {
        return false;
    }
    current_command_ = generate_command_with_options();
    return true;
}

auto CommandWithOptionsGenerator::generate_command_with_options() -> CommandWithOptions {
    static std::random_device rd;
    static std::mt19937 gen(rd());
    
    CommandWithOptions result;
    
    // Base commands
    static const std::vector<std::string> commands = {
        "commit", "push", "fetch", "cherry-pick", "rebase"
    };
    
    // Options per command
    static const std::vector<std::vector<std::string>> command_options = {
        {"--message", "--all", "--amend", "--no-verify"},  // commit
        {"--force", "--force-with-lease", "--tags"},       // push
        {"--all", "--prune", "--tags"},                    // fetch
        {"--no-commit", "--edit"},                         // cherry-pick
        {"--interactive", "--continue", "--abort"}         // rebase
    };
    
    std::uniform_int_distribution<> cmd_dist(0, commands.size() - 1);
    size_t cmd_idx = cmd_dist(gen);
    
    result.command = commands[cmd_idx];
    result.full_string = result.command;
    
    // Add 1-3 random options
    std::uniform_int_distribution<> opt_count_dist(1, 3);
    size_t opt_count = std::min(static_cast<size_t>(opt_count_dist(gen)), command_options[cmd_idx].size());
    
    // Shuffle and take first N options
    auto options = command_options[cmd_idx];
    std::shuffle(options.begin(), options.end(), gen);
    
    for (size_t i = 0; i < opt_count; ++i) {
        result.options.push_back(options[i]);
        result.full_string += " " + options[i];
        
        // Add value for options that require it
        if (options[i] == "--message") {
            result.full_string += " \"test commit message\"";
        }
    }
    
    return result;
}

} // namespace tui_test
