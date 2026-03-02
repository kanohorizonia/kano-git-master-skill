#include "metadata_cache.hpp"

#include <algorithm>

namespace kano::git::commands {

namespace {

/// Check if an option takes a value
bool OptionTakesValue(const CLI::Option* option) {
    return option->get_items_expected_max() != 0;
}

/// Check if an option can be specified multiple times
bool OptionIsMultiValue(const CLI::Option* option) {
    return option->get_expected_max() > 1 || option->get_items_expected_max() > 1;
}

} // namespace

MetadataCache::MetadataCache(const CLI::App& app) {
    ExtractMetadata(app);
}

auto MetadataCache::GetAllCommands() -> std::vector<CommandMetadata> {
    std::vector<CommandMetadata> result;
    result.reserve(commands_.size());
    
    for (const auto& [name, metadata] : commands_) {
        // Only include top-level commands (no spaces in name)
        if (name.find(' ') == std::string::npos) {
            result.push_back(metadata);
        }
    }
    
    // Sort alphabetically by name for consistent ordering
    std::sort(result.begin(), result.end(), 
              [](const CommandMetadata& a, const CommandMetadata& b) {
                  return a.name < b.name;
              });
    
    return result;
}

auto MetadataCache::GetCommand(const std::string& name) -> std::optional<CommandMetadata> {
    auto it = commands_.find(name);
    if (it != commands_.end()) {
        return it->second;
    }
    return std::nullopt;
}

auto MetadataCache::GetSubcommands(const std::string& command) -> std::vector<std::string> {
    auto it = commands_.find(command);
    if (it != commands_.end()) {
        return it->second.subcommands;
    }
    return {};
}

auto MetadataCache::GetOptions(const std::string& command) -> std::vector<OptionMetadata> {
    auto it = commands_.find(command);
    if (it != commands_.end()) {
        return it->second.options;
    }
    return {};
}

auto MetadataCache::Refresh(const CLI::App& app) -> void {
    commands_.clear();
    ExtractMetadata(app);
}

auto MetadataCache::ExtractMetadata(const CLI::App& app) -> void {
    // Get all top-level subcommands (excluding the root app itself)
    const auto subcommands = app.get_subcommands([](const CLI::App* sub) {
        return sub != nullptr && !sub->get_name().empty();
    });
    
    // Extract metadata for each top-level command
    for (const auto* cmd : subcommands) {
        auto metadata = ExtractCommand(cmd, cmd->get_name());
        commands_[metadata.name] = std::move(metadata);
    }
}

auto MetadataCache::ExtractCommand(const CLI::App* cmd, const std::string& path) 
    -> CommandMetadata {
    CommandMetadata metadata;
    metadata.name = cmd->get_name();
    metadata.description = cmd->get_description();
    metadata.allow_extras = cmd->get_allow_extras();
    
    // Extract options
    metadata.options = ExtractOptions(cmd);
    
    // Extract subcommands
    const auto subcommands = cmd->get_subcommands([](const CLI::App* sub) {
        return sub != nullptr && !sub->get_name().empty();
    });
    
    for (const auto* subcmd : subcommands) {
        metadata.subcommands.push_back(subcmd->get_name());
        
        // Recursively extract subcommand metadata
        // Store with full path as key (e.g., "worktree add")
        const std::string child_path = path + " " + subcmd->get_name();
        auto subcmd_metadata = ExtractCommand(subcmd, child_path);
        commands_[child_path] = std::move(subcmd_metadata);
    }
    
    // Sort subcommands alphabetically
    std::sort(metadata.subcommands.begin(), metadata.subcommands.end());
    
    return metadata;
}

auto MetadataCache::ExtractOptions(const CLI::App* cmd) -> std::vector<OptionMetadata> {
    std::vector<OptionMetadata> result;
    
    // Get all non-positional options, excluding help options
    const auto options = cmd->get_options([](const CLI::Option* opt) {
        if (opt == nullptr || !opt->nonpositional()) {
            return false;
        }
        
        // Filter out help options (--help, -h, etc.)
        const auto& lnames = opt->get_lnames();
        const auto& snames = opt->get_snames();
        
        for (const auto& name : lnames) {
            if (name == "help") {
                return false;
            }
        }
        for (const auto& name : snames) {
            if (name == "h") {
                return false;
            }
        }
        
        return true;
    });
    
    for (const auto* opt : options) {
        OptionMetadata opt_meta;
        
        // Extract long names (without -- prefix)
        opt_meta.long_names = opt->get_lnames();
        
        // Extract short names (without - prefix)
        opt_meta.short_names = opt->get_snames();
        
        // Extract description
        opt_meta.description = opt->get_description();
        
        // Extract properties
        opt_meta.takes_value = OptionTakesValue(opt);
        opt_meta.required = opt->get_required();
        opt_meta.multi = OptionIsMultiValue(opt);
        opt_meta.default_value = opt->get_default_str();
        
        result.push_back(std::move(opt_meta));
    }
    
    // Sort options alphabetically by first long name (or short name if no long name)
    std::sort(result.begin(), result.end(), 
              [](const OptionMetadata& a, const OptionMetadata& b) {
                  const std::string& a_name = a.long_names.empty() 
                      ? (a.short_names.empty() ? "" : a.short_names[0])
                      : a.long_names[0];
                  const std::string& b_name = b.long_names.empty() 
                      ? (b.short_names.empty() ? "" : b.short_names[0])
                      : b.long_names[0];
                  return a_name < b_name;
              });
    
    return result;
}

} // namespace kano::git::commands
