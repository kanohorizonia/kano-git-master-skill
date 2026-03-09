#include "autocomplete_engine.hpp"
#include <algorithm>
#include <cctype>
#include <sstream>

namespace kano::git::commands {

namespace {

/// Convert string to lowercase (ASCII only)
auto ToLowerAscii(const std::string& str) -> std::string {
    std::string result = str;
    std::transform(result.begin(), result.end(), result.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return result;
}

/// Check if string starts with prefix (case-insensitive)
auto StartsWithIgnoreCase(const std::string& str, const std::string& prefix) -> bool {
    if (prefix.empty()) return true;
    if (str.length() < prefix.length()) return false;
    return ToLowerAscii(str.substr(0, prefix.length())) == ToLowerAscii(prefix);
}

} // anonymous namespace

AutocompleteEngine::AutocompleteEngine(std::shared_ptr<MetadataCache> metadata)
    : metadata_(std::move(metadata)) {}

auto AutocompleteEngine::GenerateCandidates(const std::string& input)
    -> std::vector<CandidateItem> {
    try {
        auto context = ParseInput(input);

        switch (context.phase) {
        case CompletionPhase::Command:
            return CompleteCommand(context.current_token);
        case CompletionPhase::Subcommand:
            return CompleteSubcommand(context.command_name, context.current_token);
        case CompletionPhase::Option:
            return CompleteOption(context.command_name, context.current_token);
        case CompletionPhase::OptionValue:
            // Future enhancement
            return {};
        }
    } catch (const std::exception& e) {
        // Log error but don't crash - graceful degradation
        // In production, this would use proper logging
        return {};
    }

    return {};
}

auto AutocompleteEngine::ParseInput(const std::string& input) -> InputContext {
    InputContext ctx;

    // Tokenize by whitespace
    std::istringstream iss(input);
    std::string token;
    while (iss >> token) {
        ctx.tokens.push_back(token);
    }

    // Determine current token (may be incomplete)
    if (input.empty() || std::isspace(static_cast<unsigned char>(input.back()))) {
        // Trailing space means starting new token
        ctx.current_token = "";
    } else {
        // Last token is incomplete
        if (!ctx.tokens.empty()) {
            ctx.current_token = ctx.tokens.back();
            ctx.tokens.pop_back();
        } else {
            ctx.current_token = "";
        }
    }

    // Determine completion phase
    if (ctx.tokens.empty()) {
        // No complete tokens yet - completing command name
        ctx.phase = CompletionPhase::Command;
        ctx.command_name = "";
    } else {
        // Have at least one complete token - it's the command name
        ctx.command_name = ctx.tokens[0];

        if (!ctx.current_token.empty() && ctx.current_token[0] == '-') {
            // Current token starts with '-' - completing option
            ctx.phase = CompletionPhase::Option;
        } else {
            // Completing subcommand or option value
            // For now, assume subcommand (option value is future enhancement)
            ctx.phase = CompletionPhase::Subcommand;
        }
    }

    return ctx;
}

auto AutocompleteEngine::CompleteCommand(const std::string& prefix)
    -> std::vector<CandidateItem> {
    std::vector<CandidateItem> candidates;

    auto all_commands = metadata_->GetAllCommands();
    for (const auto& cmd : all_commands) {
        if (StartsWithIgnoreCase(cmd.name, prefix)) {
            candidates.push_back({
                .text = cmd.name,
                .description = cmd.description,
                .type = CandidateType::Command,
                .completion = cmd.name
            });
        }
    }

    return FilterAndSort(std::move(candidates), 10);
}

auto AutocompleteEngine::CompleteSubcommand(const std::string& command,
                                             const std::string& prefix)
    -> std::vector<CandidateItem> {
    std::vector<CandidateItem> candidates;

    auto subcommands = metadata_->GetSubcommands(command);
    for (const auto& subcmd : subcommands) {
        if (StartsWithIgnoreCase(subcmd, prefix)) {
            candidates.push_back({
                .text = subcmd,
                .description = "Subcommand of " + command,
                .type = CandidateType::Subcommand,
                .completion = subcmd
            });
        }
    }

    return FilterAndSort(std::move(candidates), 10);
}

auto AutocompleteEngine::CompleteOption(const std::string& command,
                                         const std::string& prefix)
    -> std::vector<CandidateItem> {
    std::vector<CandidateItem> candidates;

    auto options = metadata_->GetOptions(command);
    for (const auto& opt : options) {
        // Check long names (--option)
        for (const auto& long_name : opt.long_names) {
            std::string full_name = "--" + long_name;
            if (StartsWithIgnoreCase(full_name, prefix)) {
                candidates.push_back({
                    .text = full_name,
                    .description = opt.description,
                    .type = CandidateType::Option,
                    .completion = full_name
                });
            }
        }

        // Check short names (-o)
        for (const auto& short_name : opt.short_names) {
            std::string full_name = "-" + short_name;
            if (StartsWithIgnoreCase(full_name, prefix)) {
                candidates.push_back({
                    .text = full_name,
                    .description = opt.description,
                    .type = CandidateType::Option,
                    .completion = full_name
                });
            }
        }
    }

    return FilterAndSort(std::move(candidates), 10);
}

auto AutocompleteEngine::FilterAndSort(std::vector<CandidateItem> candidates,
                                        size_t max_count)
    -> std::vector<CandidateItem> {
    // Sort alphabetically by text
    std::sort(candidates.begin(), candidates.end(),
              [](const auto& a, const auto& b) {
                  return a.text < b.text;
              });

    // Limit to max_count
    if (candidates.size() > max_count) {
        candidates.resize(max_count);
    }

    return candidates;
}

} // namespace kano::git::commands
