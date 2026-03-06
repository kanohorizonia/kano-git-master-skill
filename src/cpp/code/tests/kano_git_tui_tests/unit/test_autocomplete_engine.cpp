#include <catch2/catch_test_macros.hpp>
#include "autocomplete_engine.hpp"
#include "metadata_cache.hpp"
#include <CLI/CLI.hpp>

using namespace kano::git::commands;

namespace {

/// Helper to create a test CLI11 app with sample commands
/// Note: CLI::App is not copyable, so this must be called inline in each test
void SetupTestApp(CLI::App& app) {
    // Add "commit" command with options
    auto* commit = app.add_subcommand("commit", "Record changes to repository");
    commit->add_option("--message,-m", "Commit message");
    commit->add_flag("--all,-a", "Stage all modified files");
    commit->add_flag("--amend", "Amend previous commit");

    // Add "push" command with options
    auto* push = app.add_subcommand("push", "Update remote refs");
    push->add_flag("--force,-f", "Force push");
    push->add_option("--remote,-r", "Remote name");

    // Add "worktree" command with subcommands
    auto* worktree = app.add_subcommand("worktree", "Manage working trees");
    worktree->add_subcommand("add", "Add a new working tree");
    worktree->add_subcommand("list", "List working trees");
    worktree->add_subcommand("remove", "Remove a working tree");

    // Add "fetch" command
    app.add_subcommand("fetch", "Download objects from remote");

    // Add "complete" command (for testing prefix matching)
    app.add_subcommand("complete", "Generate completion scripts");
}

} // anonymous namespace

TEST_CASE("AutocompleteEngine - Command completion with various prefixes",
          "[Feature: tui-command-input-enhancement][Unit]") {
    CLI::App app{"Test App"};
    SetupTestApp(app);
    auto metadata = std::make_shared<MetadataCache>(app);
    AutocompleteEngine engine(metadata);

    SECTION("Empty prefix returns all commands") {
        auto candidates = engine.GenerateCandidates("");
        REQUIRE(candidates.size() == 5);  // commit, complete, fetch, push, worktree
        REQUIRE(candidates[0].type == CandidateType::Command);
    }

    SECTION("Prefix 'com' matches commit and complete") {
        auto candidates = engine.GenerateCandidates("com");
        REQUIRE(candidates.size() == 2);
        REQUIRE(candidates[0].text == "commit");
        REQUIRE(candidates[1].text == "complete");
        REQUIRE(candidates[0].type == CandidateType::Command);
    }

    SECTION("Prefix 'commit' matches only commit") {
        auto candidates = engine.GenerateCandidates("commit");
        REQUIRE(candidates.size() == 1);
        REQUIRE(candidates[0].text == "commit");
        REQUIRE(candidates[0].description == "Record changes to repository");
    }

    SECTION("Case-insensitive matching") {
        auto candidates = engine.GenerateCandidates("COM");
        REQUIRE(candidates.size() == 2);
        REQUIRE(candidates[0].text == "commit");
        REQUIRE(candidates[1].text == "complete");
    }

    SECTION("Non-matching prefix returns empty list") {
        auto candidates = engine.GenerateCandidates("xyz");
        REQUIRE(candidates.empty());
    }
}

TEST_CASE("AutocompleteEngine - Subcommand completion for multi-level commands",
          "[Feature: tui-command-input-enhancement][Unit]") {
    CLI::App app{"Test App"};
    SetupTestApp(app);
    auto metadata = std::make_shared<MetadataCache>(app);
    AutocompleteEngine engine(metadata);

    SECTION("Worktree with trailing space shows subcommands") {
        auto candidates = engine.GenerateCandidates("worktree ");
        REQUIRE(candidates.size() == 3);
        REQUIRE(candidates[0].text == "add");
        REQUIRE(candidates[1].text == "list");
        REQUIRE(candidates[2].text == "remove");
        REQUIRE(candidates[0].type == CandidateType::Subcommand);
    }

    SECTION("Worktree with partial subcommand filters results") {
        auto candidates = engine.GenerateCandidates("worktree a");
        REQUIRE(candidates.size() == 1);
        REQUIRE(candidates[0].text == "add");
    }

    SECTION("Worktree with partial 'rem' matches remove") {
        auto candidates = engine.GenerateCandidates("worktree rem");
        REQUIRE(candidates.size() == 1);
        REQUIRE(candidates[0].text == "remove");
    }

    SECTION("Command without subcommands returns empty list") {
        auto candidates = engine.GenerateCandidates("commit ");
        // commit has no subcommands, so should return empty
        REQUIRE(candidates.empty());
    }
}

TEST_CASE("AutocompleteEngine - Option completion with long and short forms",
          "[Feature: tui-command-input-enhancement][Unit]") {
    CLI::App app{"Test App"};
    SetupTestApp(app);
    auto metadata = std::make_shared<MetadataCache>(app);
    AutocompleteEngine engine(metadata);

    SECTION("Single dash shows short options") {
        auto candidates = engine.GenerateCandidates("commit -");
        REQUIRE(candidates.size() >= 2);  // At least -a and -m
        
        bool found_a = false;
        bool found_m = false;
        for (const auto& c : candidates) {
            if (c.text == "-a") found_a = true;
            if (c.text == "-m") found_m = true;
        }
        REQUIRE(found_a);
        REQUIRE(found_m);
    }

    SECTION("Double dash shows long options") {
        auto candidates = engine.GenerateCandidates("commit --");
        REQUIRE(candidates.size() >= 3);  // --all, --amend, --message
        
        bool found_all = false;
        bool found_amend = false;
        bool found_message = false;
        for (const auto& c : candidates) {
            if (c.text == "--all") found_all = true;
            if (c.text == "--amend") found_amend = true;
            if (c.text == "--message") found_message = true;
        }
        REQUIRE(found_all);
        REQUIRE(found_amend);
        REQUIRE(found_message);
    }

    SECTION("Partial option name filters results") {
        auto candidates = engine.GenerateCandidates("commit --am");
        REQUIRE(candidates.size() == 1);
        REQUIRE(candidates[0].text == "--amend");
    }

    SECTION("Options have descriptions") {
        auto candidates = engine.GenerateCandidates("commit --message");
        REQUIRE(candidates.size() == 1);
        REQUIRE(candidates[0].description == "Commit message");
        REQUIRE(candidates[0].type == CandidateType::Option);
    }
}

TEST_CASE("AutocompleteEngine - Empty candidate list for non-matching input",
          "[Feature: tui-command-input-enhancement][Unit]") {
    CLI::App app{"Test App"};
    SetupTestApp(app);
    auto metadata = std::make_shared<MetadataCache>(app);
    AutocompleteEngine engine(metadata);

    SECTION("Unknown command returns empty list") {
        auto candidates = engine.GenerateCandidates("unknown");
        REQUIRE(candidates.empty());
    }

    SECTION("Unknown subcommand returns empty list") {
        auto candidates = engine.GenerateCandidates("worktree unknown");
        REQUIRE(candidates.empty());
    }

    SECTION("Unknown option returns empty list") {
        auto candidates = engine.GenerateCandidates("commit --unknown");
        REQUIRE(candidates.empty());
    }
}

TEST_CASE("AutocompleteEngine - Edge cases",
          "[Feature: tui-command-input-enhancement][Unit]") {
    CLI::App app{"Test App"};
    SetupTestApp(app);
    auto metadata = std::make_shared<MetadataCache>(app);
    AutocompleteEngine engine(metadata);

    SECTION("Multiple spaces treated as single separator") {
        auto candidates = engine.GenerateCandidates("worktree  ");
        REQUIRE(candidates.size() == 3);  // Same as single space
    }

    SECTION("Trailing spaces indicate new token") {
        auto candidates1 = engine.GenerateCandidates("commit");
        auto candidates2 = engine.GenerateCandidates("commit ");
        
        // "commit" completes command names
        // "commit " completes subcommands/options
        REQUIRE(candidates1.size() >= 1);
        REQUIRE(candidates1[0].type == CandidateType::Command);
        
        // commit has no subcommands, so empty
        REQUIRE(candidates2.empty());
    }

    SECTION("Alphabetical sorting") {
        auto candidates = engine.GenerateCandidates("");
        
        // Verify alphabetical order
        for (size_t i = 1; i < candidates.size(); ++i) {
            REQUIRE(candidates[i-1].text <= candidates[i].text);
        }
    }

    SECTION("Maximum 10 candidates") {
        // Even if we have more matches, limit to 10
        auto candidates = engine.GenerateCandidates("");
        REQUIRE(candidates.size() <= 10);
    }
}
