// Unit tests for MetadataCache component
// Tests metadata extraction from CLI11 app tree and query interface

#include "metadata_cache.hpp"

#include <catch2/catch_test_macros.hpp>
#include <CLI/CLI.hpp>

using namespace kano::git::commands;

namespace {

// Helper function to create a sample CLI11 app for testing
void PopulateSampleApp(CLI::App& app) {
    // Add a simple command with options
    auto* commit = app.add_subcommand("commit", "Record changes to repository");
    commit->add_option("-m,--message", "Commit message")->required();
    commit->add_flag("-a,--all", "Stage all modified files");
    commit->add_option("--author", "Override author")->default_str("default-author");
    
    // Add a command with subcommands
    auto* worktree = app.add_subcommand("worktree", "Manage working trees");
    worktree->add_subcommand("add", "Add a new working tree");
    worktree->add_subcommand("list", "List working trees");
    worktree->add_subcommand("remove", "Remove a working tree");
    
    // Add a command that allows extras
    auto* push = app.add_subcommand("push", "Update remote refs");
    push->allow_extras();
    push->add_flag("-f,--force", "Force push");
    
    // Add a command with multi-value option
    auto* fetch = app.add_subcommand("fetch", "Download objects from remote");
    fetch->add_option("--remote", "Remote names")->expected(1, -1);
}

} // namespace

TEST_CASE("MetadataCache extracts top-level commands", "[unit][metadata_cache]") {
    CLI::App app{"Test Application"};
    PopulateSampleApp(app);
    MetadataCache cache(app);
    
    auto commands = cache.GetAllCommands();
    
    REQUIRE(commands.size() == 4);
    
    // Commands should be sorted alphabetically
    REQUIRE(commands[0].name == "commit");
    REQUIRE(commands[1].name == "fetch");
    REQUIRE(commands[2].name == "push");
    REQUIRE(commands[3].name == "worktree");
}

TEST_CASE("MetadataCache extracts command descriptions", "[unit][metadata_cache]") {
    CLI::App app{"Test Application"};
    PopulateSampleApp(app);
    MetadataCache cache(app);
    
    auto commit = cache.GetCommand("commit");
    REQUIRE(commit.has_value());
    REQUIRE(commit->description == "Record changes to repository");
    
    auto worktree = cache.GetCommand("worktree");
    REQUIRE(worktree.has_value());
    REQUIRE(worktree->description == "Manage working trees");
}

TEST_CASE("MetadataCache extracts command options", "[unit][metadata_cache]") {
    CLI::App app{"Test Application"};
    PopulateSampleApp(app);
    MetadataCache cache(app);
    
    auto options = cache.GetOptions("commit");
    
    REQUIRE(options.size() == 3);
    
    // Options should be sorted alphabetically
    REQUIRE(options[0].long_names[0] == "all");
    REQUIRE(options[1].long_names[0] == "author");
    REQUIRE(options[2].long_names[0] == "message");
}

TEST_CASE("MetadataCache extracts option properties", "[unit][metadata_cache]") {
    CLI::App app{"Test Application"};
    PopulateSampleApp(app);
    MetadataCache cache(app);
    
    auto options = cache.GetOptions("commit");
    
    SECTION("Required option with value") {
        // Find --message option
        auto it = std::find_if(options.begin(), options.end(), 
                               [](const OptionMetadata& opt) {
                                   return !opt.long_names.empty() && opt.long_names[0] == "message";
                               });
        REQUIRE(it != options.end());
        
        REQUIRE(it->long_names == std::vector<std::string>{"message"});
        REQUIRE(it->short_names == std::vector<std::string>{"m"});
        REQUIRE(it->takes_value == true);
        REQUIRE(it->required == true);
        REQUIRE(it->multi == false);
    }
    
    SECTION("Flag option (no value)") {
        // Find --all option
        auto it = std::find_if(options.begin(), options.end(), 
                               [](const OptionMetadata& opt) {
                                   return !opt.long_names.empty() && opt.long_names[0] == "all";
                               });
        REQUIRE(it != options.end());
        
        REQUIRE(it->long_names == std::vector<std::string>{"all"});
        REQUIRE(it->short_names == std::vector<std::string>{"a"});
        REQUIRE(it->takes_value == false);
        REQUIRE(it->required == false);
        REQUIRE(it->multi == false);
    }
    
    SECTION("Option with default value") {
        // Find --author option
        auto it = std::find_if(options.begin(), options.end(), 
                               [](const OptionMetadata& opt) {
                                   return !opt.long_names.empty() && opt.long_names[0] == "author";
                               });
        REQUIRE(it != options.end());
        
        REQUIRE(it->default_value == "default-author");
    }
}

TEST_CASE("MetadataCache extracts multi-value options", "[unit][metadata_cache]") {
    CLI::App app{"Test Application"};
    PopulateSampleApp(app);
    MetadataCache cache(app);
    
    auto options = cache.GetOptions("fetch");
    
    REQUIRE(options.size() == 1);
    REQUIRE(options[0].long_names[0] == "remote");
    REQUIRE(options[0].multi == true);
}

TEST_CASE("MetadataCache extracts subcommands", "[unit][metadata_cache]") {
    CLI::App app{"Test Application"};
    PopulateSampleApp(app);
    MetadataCache cache(app);
    
    auto subcommands = cache.GetSubcommands("worktree");
    
    REQUIRE(subcommands.size() == 3);
    
    // Subcommands should be sorted alphabetically
    REQUIRE(subcommands[0] == "add");
    REQUIRE(subcommands[1] == "list");
    REQUIRE(subcommands[2] == "remove");
}

TEST_CASE("MetadataCache extracts allow_extras property", "[unit][metadata_cache]") {
    CLI::App app{"Test Application"};
    PopulateSampleApp(app);
    MetadataCache cache(app);
    
    auto commit = cache.GetCommand("commit");
    REQUIRE(commit.has_value());
    REQUIRE(commit->allow_extras == false);
    
    auto push = cache.GetCommand("push");
    REQUIRE(push.has_value());
    REQUIRE(push->allow_extras == true);
}

TEST_CASE("MetadataCache handles nested subcommands", "[unit][metadata_cache]") {
    CLI::App app{"Test Application"};
    PopulateSampleApp(app);
    MetadataCache cache(app);
    
    // Subcommands should be accessible via full path
    auto add = cache.GetCommand("worktree add");
    REQUIRE(add.has_value());
    REQUIRE(add->name == "add");
    REQUIRE(add->description == "Add a new working tree");
    
    auto list = cache.GetCommand("worktree list");
    REQUIRE(list.has_value());
    REQUIRE(list->name == "list");
}

TEST_CASE("MetadataCache returns nullopt for unknown commands", "[unit][metadata_cache]") {
    CLI::App app{"Test Application"};
    PopulateSampleApp(app);
    MetadataCache cache(app);
    
    auto unknown = cache.GetCommand("unknown");
    REQUIRE_FALSE(unknown.has_value());
}

TEST_CASE("MetadataCache returns empty vectors for unknown commands", "[unit][metadata_cache]") {
    CLI::App app{"Test Application"};
    PopulateSampleApp(app);
    MetadataCache cache(app);
    
    auto subcommands = cache.GetSubcommands("unknown");
    REQUIRE(subcommands.empty());
    
    auto options = cache.GetOptions("unknown");
    REQUIRE(options.empty());
}

TEST_CASE("MetadataCache Refresh updates cache", "[unit][metadata_cache]") {
    CLI::App app{"Test Application"};
    PopulateSampleApp(app);
    MetadataCache cache(app);
    
    // Initial state
    auto commands = cache.GetAllCommands();
    REQUIRE(commands.size() == 4);
    
    // Add a new command to the app
    app.add_subcommand("newcmd", "New command");
    
    // Refresh cache
    cache.Refresh(app);
    
    // Verify new command is available
    commands = cache.GetAllCommands();
    REQUIRE(commands.size() == 5);
    
    auto newcmd = cache.GetCommand("newcmd");
    REQUIRE(newcmd.has_value());
    REQUIRE(newcmd->description == "New command");
}

TEST_CASE("MetadataCache handles empty app", "[unit][metadata_cache]") {
    CLI::App app{"Empty App"};
    MetadataCache cache(app);
    
    auto commands = cache.GetAllCommands();
    REQUIRE(commands.empty());
}

TEST_CASE("MetadataCache handles command with no options", "[unit][metadata_cache]") {
    CLI::App app{"Test App"};
    app.add_subcommand("simple", "Simple command with no options");
    
    MetadataCache cache(app);
    
    auto options = cache.GetOptions("simple");
    REQUIRE(options.empty());
}

TEST_CASE("MetadataCache handles command with no subcommands", "[unit][metadata_cache]") {
    CLI::App app{"Test Application"};
    PopulateSampleApp(app);
    MetadataCache cache(app);
    
    auto subcommands = cache.GetSubcommands("commit");
    REQUIRE(subcommands.empty());
}
