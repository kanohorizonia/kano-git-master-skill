#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/generators/catch_generators_adapters.hpp>
#include <catch2/generators/catch_generators_random.hpp>
#include <catch2/generators/catch_generators_range.hpp>
#include "autocomplete_engine.hpp"
#include "metadata_cache.hpp"
#include <CLI/CLI.hpp>
#include <algorithm>
#include <cctype>
#include <tuple>

using namespace kano::git::commands;

namespace {

/// Helper to setup a test CLI11 app with sample commands
void SetupTestApp(CLI::App& app) {
    auto* commit = app.add_subcommand("commit", "Record changes");
    commit->add_option("--message,-m", "Commit message");
    commit->add_flag("--all,-a", "Stage all");

    auto* push = app.add_subcommand("push", "Update remote");
    push->add_flag("--force,-f", "Force push");

    auto* worktree = app.add_subcommand("worktree", "Manage worktrees");
    worktree->add_subcommand("add", "Add worktree");
    worktree->add_subcommand("list", "List worktrees");

    app.add_subcommand("fetch", "Download objects");
    app.add_subcommand("complete", "Generate completions");
}

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

TEST_CASE("Property 4: Autocomplete Candidate Matching",
          "[Feature: tui-command-input-enhancement][Property 4: Autocomplete Candidate Matching]") {
    CLI::App app{"Test App"};
    SetupTestApp(app);
    auto metadata = std::make_shared<MetadataCache>(app);
    AutocompleteEngine engine(metadata);

    // Generate random command prefixes
    auto prefix = GENERATE(take(100, 
        values({"", "c", "co", "com", "comm", "commi", "commit",
                "f", "fe", "fet", "fetch",
                "p", "pu", "pus", "push",
                "w", "wo", "wor", "work", "workt", "worktr", "worktree",
                "COM", "COMMIT", "FET", "PUSH"})));

    DYNAMIC_SECTION("All candidates match prefix: '" << prefix << "'") {
        auto candidates = engine.GenerateCandidates(prefix);

        // Property: All candidates must match the input prefix (case-insensitive)
        for (const auto& candidate : candidates) {
            REQUIRE(StartsWithIgnoreCase(candidate.text, prefix));
        }

        // Property: All candidates must be valid commands from registry
        auto all_commands = metadata->GetAllCommands();
        for (const auto& candidate : candidates) {
            bool found = false;
            for (const auto& cmd : all_commands) {
                if (cmd.name == candidate.text) {
                    found = true;
                    break;
                }
            }
            REQUIRE(found);
        }
    }
}

TEST_CASE("Property 5: Context-Aware Completion",
          "[Feature: tui-command-input-enhancement][Property 5: Context-Aware Completion]") {
    CLI::App app{"Test App"};
    SetupTestApp(app);
    auto metadata = std::make_shared<MetadataCache>(app);
    AutocompleteEngine engine(metadata);

    SECTION("Subcommands belong to specified command") {
        // Test worktree subcommands with various prefixes
        auto prefix = GENERATE(take(100, values({"", "a", "ad", "add", "l", "li", "lis", "list"})));
        
        DYNAMIC_SECTION("Worktree subcommands with prefix: '" << prefix << "'") {
            std::string input = "worktree " + std::string(prefix);
            auto candidates = engine.GenerateCandidates(input);

            // Property: All candidates must be valid subcommands of worktree
            auto subcommands = metadata->GetSubcommands("worktree");
            for (const auto& candidate : candidates) {
                bool found = std::find(subcommands.begin(), subcommands.end(), 
                                       candidate.text) != subcommands.end();
                REQUIRE(found);
                REQUIRE(candidate.type == CandidateType::Subcommand);
            }

            // Property: No candidates from other commands should be included
            auto all_commands = metadata->GetAllCommands();
            for (const auto& candidate : candidates) {
                // Verify candidate is not a top-level command
                bool is_top_level_command = false;
                for (const auto& cmd : all_commands) {
                    if (cmd.name == candidate.text) {
                        is_top_level_command = true;
                        break;
                    }
                }
                REQUIRE_FALSE(is_top_level_command);
            }
        }
    }

    SECTION("Options belong to specified command") {
        auto command = GENERATE(values({"commit", "push"}));
        auto option_prefix = GENERATE(values({"--", "--m", "--me", "--a", "--al", "-", "-m", "-a", "-f"}));
        
        DYNAMIC_SECTION("Options for command '" << command << "' with prefix '" << option_prefix << "'") {
            std::string input = std::string(command) + " " + std::string(option_prefix);
            auto candidates = engine.GenerateCandidates(input);

            // Property: All candidates must be valid options for the command
            auto options = metadata->GetOptions(command);
            for (const auto& candidate : candidates) {
                bool found = false;
                for (const auto& opt : options) {
                    // Check long names
                    for (const auto& long_name : opt.long_names) {
                        if (candidate.text == "--" + long_name) {
                            found = true;
                            break;
                        }
                    }
                    // Check short names
                    if (!found) {
                        for (const auto& short_name : opt.short_names) {
                            if (candidate.text == "-" + short_name) {
                                found = true;
                                break;
                            }
                        }
                    }
                    if (found) break;
                }
                REQUIRE(found);
                REQUIRE(candidate.type == CandidateType::Option);
            }

            // Property: No options from other commands should be included
            // Get all commands and their options
            auto all_commands = metadata->GetAllCommands();
            for (const auto& candidate : candidates) {
                // For each candidate, verify it doesn't belong exclusively to another command
                for (const auto& other_cmd : all_commands) {
                    if (other_cmd.name == command) continue; // Skip the current command
                    
                    auto other_options = metadata->GetOptions(other_cmd.name);
                    bool exists_in_current = false;
                    bool exists_in_other = false;

                    // Check if option exists in current command
                    for (const auto& opt : options) {
                        for (const auto& long_name : opt.long_names) {
                            if (candidate.text == "--" + long_name) {
                                exists_in_current = true;
                                break;
                            }
                        }
                        if (!exists_in_current) {
                            for (const auto& short_name : opt.short_names) {
                                if (candidate.text == "-" + short_name) {
                                    exists_in_current = true;
                                    break;
                                }
                            }
                        }
                        if (exists_in_current) break;
                    }

                    // Check if option exists in other command
                    for (const auto& opt : other_options) {
                        for (const auto& long_name : opt.long_names) {
                            if (candidate.text == "--" + long_name) {
                                exists_in_other = true;
                                break;
                            }
                        }
                        if (!exists_in_other) {
                            for (const auto& short_name : opt.short_names) {
                                if (candidate.text == "-" + short_name) {
                                    exists_in_other = true;
                                    break;
                                }
                            }
                        }
                        if (exists_in_other) break;
                    }

                    // If option exists in other command but not in current, that's an error
                    if (exists_in_other && !exists_in_current) {
                        FAIL("Option '" << candidate.text << "' from command '" 
                             << other_cmd.name << "' incorrectly included in candidates for '" 
                             << command << "'");
                    }
                }
            }
        }
    }

    SECTION("No cross-contamination between commands") {
        // Test that completing one command doesn't return candidates from another
        auto test_cases = GENERATE(table<std::string, std::string>({
            {"commit ", "push"},
            {"push ", "commit"},
            {"worktree ", "commit"},
            {"fetch ", "push"}
        }));

        auto input = std::get<0>(test_cases);
        auto other_command = std::get<1>(test_cases);

        DYNAMIC_SECTION("Input '" << input << "' should not include candidates from '" << other_command << "'") {
            auto candidates = engine.GenerateCandidates(input);
            
            // Get subcommands and options from the other command
            auto other_subcommands = metadata->GetSubcommands(other_command);
            auto other_options = metadata->GetOptions(other_command);

            // Verify no subcommands from other command
            for (const auto& candidate : candidates) {
                bool is_other_subcommand = std::find(other_subcommands.begin(), 
                                                      other_subcommands.end(), 
                                                      candidate.text) != other_subcommands.end();
                REQUIRE_FALSE(is_other_subcommand);
            }

            // Verify no exclusive options from other command
            for (const auto& candidate : candidates) {
                if (candidate.type != CandidateType::Option) continue;

                // Check if this option belongs exclusively to the other command
                bool in_other = false;
                for (const auto& opt : other_options) {
                    for (const auto& long_name : opt.long_names) {
                        if (candidate.text == "--" + long_name) {
                            in_other = true;
                            break;
                        }
                    }
                    if (!in_other) {
                        for (const auto& short_name : opt.short_names) {
                            if (candidate.text == "-" + short_name) {
                                in_other = true;
                                break;
                            }
                        }
                    }
                    if (in_other) break;
                }

                if (in_other) {
                    // Verify it also exists in the current command (shared option is OK)
                    std::string current_command = input.substr(0, input.find(' '));
                    auto current_options = metadata->GetOptions(current_command);
                    bool in_current = false;
                    for (const auto& opt : current_options) {
                        for (const auto& long_name : opt.long_names) {
                            if (candidate.text == "--" + long_name) {
                                in_current = true;
                                break;
                            }
                        }
                        if (!in_current) {
                            for (const auto& short_name : opt.short_names) {
                                if (candidate.text == "-" + short_name) {
                                    in_current = true;
                                    break;
                                }
                            }
                        }
                        if (in_current) break;
                    }
                    REQUIRE(in_current);
                }
            }
        }
    }
}

TEST_CASE("Property 6: Candidate List Constraints",
          "[Feature: tui-command-input-enhancement][Property 6: Candidate List Constraints]") {
    CLI::App app{"Test App"};
    SetupTestApp(app);
    auto metadata = std::make_shared<MetadataCache>(app);
    AutocompleteEngine engine(metadata);

    // **Validates: Requirements 3.6, 3.7**
    // Generate 100+ test cases covering different completion contexts
    auto input = GENERATE(
        // Empty and command prefixes (20 cases)
        values({"", "c", "co", "com", "comm", "commi", "commit",
                "f", "fe", "fet", "fetc", "fetch",
                "p", "pu", "pus", "push",
                "w", "wo", "wor", "work", "workt"}),
        
        // Complete commands with space for subcommand completion (10 cases)
        values({"commit ", "push ", "fetch ", "worktree ", "complete ",
                "commit  ", "push  ", "worktree  ", "fetch  ", "complete  "}),
        
        // Subcommand prefixes (15 cases)
        values({"worktree a", "worktree ad", "worktree add",
                "worktree l", "worktree li", "worktree lis", "worktree list",
                "worktree ", "worktree  ", "worktree x", "worktree xyz",
                "worktree addx", "worktree listx", "worktree prune", "worktree remove"}),
        
        // Option prefixes (25 cases)
        values({"commit -", "commit --", "commit --m", "commit --me", "commit --mes",
                "commit --mess", "commit --messa", "commit --messag", "commit --message",
                "commit --a", "commit --al", "commit --all",
                "push -", "push --", "push -f", "push --f", "push --fo", "push --for",
                "push --forc", "push --force",
                "commit -m", "commit -a", "push -f",
                "commit --x", "push --x"}),
        
        // Mixed case and edge cases (15 cases)
        values({"C", "CO", "COM", "COMMIT", "COMMIT ", "COMMIT --",
                "Commit", "Push", "Fetch", "Worktree",
                "commit --MESSAGE", "push --FORCE",
                "xyz", "unknown", "notacommand"}),
        
        // Multiple options (15 cases)
        values({"commit --message ", "commit --all ", "push --force ",
                "commit -m ", "commit -a ", "push -f ",
                "commit --message x", "commit --all x", "push --force x",
                "commit -m x", "commit -a x", "push -f x",
                "commit --message x --", "commit --all x --", "push --force x --"})
    );

    DYNAMIC_SECTION("Candidate list constraints for: '" << input << "'") {
        auto candidates = engine.GenerateCandidates(input);

        // Property: Candidate list must contain at most 10 items
        REQUIRE(candidates.size() <= 10);

        // Property: Candidates must be sorted alphabetically
        for (size_t i = 1; i < candidates.size(); ++i) {
            REQUIRE(candidates[i-1].text <= candidates[i].text);
        }
    }
}

TEST_CASE("Property 7: Empty Candidate List for Non-Matching Input",
          "[Feature: tui-command-input-enhancement][Property 7: Empty Candidate List for Non-Matching Input]") {
    CLI::App app{"Test App"};
    SetupTestApp(app);
    auto metadata = std::make_shared<MetadataCache>(app);
    AutocompleteEngine engine(metadata);

    // **Validates: Requirements 3.5**
    
    SECTION("Unknown command prefixes") {
        // Test 100+ unknown command prefixes
        auto unknown_prefix = GENERATE(
            values({"xyz", "unknown", "notacommand", "zzz", "qwerty", "asdfgh",
                    "zxcvbn", "invalid", "badcmd", "nocmd", "wrongcmd", "fakecmd",
                    "xxx", "yyy", "qqq", "rrr", "ttt", "uuu", "iii", "ooo",
                    "aaa", "bbb", "ddd", "eee", "ggg", "hhh", "jjj", "kkk",
                    "lll", "mmm", "nnn", "sss", "vvv", "www", "abc123",
                    "test123", "cmd999", "x1", "y2", "z3", "q4", "w5", "e6",
                    "notfound", "missing", "absent", "nonexistent", "unavailable",
                    "gibberish", "nonsense", "random", "arbitrary", "undefined",
                    "null", "void", "empty", "blank", "nothing", "none",
                    "xyzabc", "abcxyz", "123abc", "abc123xyz", "test_cmd",
                    "cmd-test", "cmd.test", "cmd:test", "cmd;test", "cmd,test",
                    "cmd@test", "cmd#test", "cmd$test", "cmd%test", "cmd^test",
                    "cmd&test", "cmd*test", "cmd(test", "cmd)test", "cmd[test",
                    "cmd]test", "cmd{test", "cmd}test", "cmd<test", "cmd>test",
                    "cmd?test", "cmd/test", "cmd\\test", "cmd|test", "cmd~test",
                    "cmd`test", "cmd'test", "cmd\"test", "cmd test", "cmd\ttest",
                    "cmd\ntest", "UNKNOWN", "INVALID", "NOTFOUND", "MISSING",
                    "xyz1", "xyz2", "xyz3", "xyz4", "xyz5", "xyz6", "xyz7", "xyz8",
                    "unknown1", "unknown2", "unknown3", "unknown4", "unknown5"}));
        
        DYNAMIC_SECTION("Unknown command prefix: '" << unknown_prefix << "'") {
            auto candidates = engine.GenerateCandidates(unknown_prefix);
            
            // Property: Non-matching input must return empty candidate list
            REQUIRE(candidates.empty());
        }
    }

    SECTION("Unknown subcommands") {
        // Test 100+ cases for unknown subcommands
        auto test_case = GENERATE(
            table<std::string, std::string>({
                {"worktree", "unknown"},
                {"worktree", "invalid"},
                {"worktree", "notfound"},
                {"worktree", "missing"},
                {"worktree", "xyz"},
                {"worktree", "abc"},
                {"worktree", "test"},
                {"worktree", "fake"},
                {"worktree", "wrong"},
                {"worktree", "bad"},
                {"worktree", "addx"},
                {"worktree", "listx"},
                {"worktree", "removex"},
                {"worktree", "prunex"},
                {"worktree", "xadd"},
                {"worktree", "xlist"},
                {"worktree", "xremove"},
                {"worktree", "xprune"},
                {"worktree", "add123"},
                {"worktree", "list456"},
                {"worktree", "remove789"},
                {"worktree", "prune000"},
                {"worktree", "UNKNOWN"},
                {"worktree", "INVALID"},
                {"worktree", "NOTFOUND"},
                {"commit", "subcommand"},
                {"commit", "unknown"},
                {"commit", "invalid"},
                {"push", "subcommand"},
                {"push", "unknown"},
                {"push", "invalid"},
                {"fetch", "subcommand"},
                {"fetch", "unknown"},
                {"fetch", "invalid"},
                {"complete", "subcommand"},
                {"complete", "unknown"},
                {"complete", "invalid"},
                {"worktree", "delete"},
                {"worktree", "create"},
                {"worktree", "update"},
                {"worktree", "modify"},
                {"worktree", "change"},
                {"worktree", "edit"},
                {"worktree", "new"},
                {"worktree", "old"},
                {"worktree", "current"},
                {"worktree", "previous"},
                {"worktree", "next"},
                {"worktree", "first"},
                {"worktree", "last"},
                {"worktree", "main"},
                {"worktree", "master"},
                {"worktree", "develop"},
                {"worktree", "feature"},
                {"worktree", "bugfix"},
                {"worktree", "hotfix"},
                {"worktree", "release"},
                {"worktree", "staging"},
                {"worktree", "production"},
                {"worktree", "test"},
                {"worktree", "dev"},
                {"worktree", "qa"},
                {"worktree", "uat"},
                {"worktree", "demo"},
                {"worktree", "sandbox"},
                {"worktree", "experimental"},
                {"worktree", "prototype"},
                {"worktree", "alpha"},
                {"worktree", "beta"},
                {"worktree", "rc"},
                {"worktree", "stable"},
                {"worktree", "latest"},
                {"worktree", "current"},
                {"worktree", "legacy"},
                {"worktree", "deprecated"},
                {"worktree", "archived"},
                {"worktree", "backup"},
                {"worktree", "temp"},
                {"worktree", "tmp"},
                {"worktree", "cache"},
                {"worktree", "build"},
                {"worktree", "dist"},
                {"worktree", "out"},
                {"worktree", "bin"},
                {"worktree", "lib"},
                {"worktree", "src"},
                {"worktree", "docs"},
                {"worktree", "examples"},
                {"worktree", "samples"},
                {"worktree", "templates"},
                {"worktree", "assets"},
                {"worktree", "resources"},
                {"worktree", "config"},
                {"worktree", "settings"},
                {"worktree", "preferences"},
                {"worktree", "options"}
            }));
        
        auto command = std::get<0>(test_case);
        auto subcommand = std::get<1>(test_case);
        std::string input = command + " " + subcommand;
        
        DYNAMIC_SECTION("Unknown subcommand: '" << input << "'") {
            auto candidates = engine.GenerateCandidates(input);
            
            // Property: Non-matching subcommand must return empty list
            REQUIRE(candidates.empty());
        }
    }

    SECTION("Unknown options") {
        // Test 100+ cases for unknown options
        auto test_case = GENERATE(
            table<std::string, std::string>({
                {"commit", "--unknownoption"},
                {"commit", "--invalid"},
                {"commit", "--notfound"},
                {"commit", "--missing"},
                {"commit", "--xyz"},
                {"commit", "--abc"},
                {"commit", "--test"},
                {"commit", "--fake"},
                {"commit", "--wrong"},
                {"commit", "--bad"},
                {"commit", "-x"},
                {"commit", "-y"},
                {"commit", "-z"},
                {"commit", "-q"},
                {"commit", "-w"},
                {"commit", "-e"},
                {"commit", "-r"},
                {"commit", "-t"},
                {"commit", "-u"},
                {"commit", "-i"},
                {"push", "--unknownoption"},
                {"push", "--invalid"},
                {"push", "--notfound"},
                {"push", "--missing"},
                {"push", "--xyz"},
                {"push", "--abc"},
                {"push", "--test"},
                {"push", "--fake"},
                {"push", "--wrong"},
                {"push", "--bad"},
                {"push", "-x"},
                {"push", "-y"},
                {"push", "-z"},
                {"push", "-q"},
                {"push", "-w"},
                {"push", "-e"},
                {"push", "-r"},
                {"push", "-t"},
                {"push", "-u"},
                {"push", "-i"},
                {"fetch", "--unknownoption"},
                {"fetch", "--invalid"},
                {"fetch", "--notfound"},
                {"fetch", "--missing"},
                {"complete", "--unknownoption"},
                {"complete", "--invalid"},
                {"complete", "--notfound"},
                {"complete", "--missing"},
                {"commit", "--messagex"},
                {"commit", "--allx"},
                {"commit", "--xmessage"},
                {"commit", "--xall"},
                {"commit", "--message123"},
                {"commit", "--all456"},
                {"push", "--forcex"},
                {"push", "--xforce"},
                {"push", "--force123"},
                {"commit", "-mx"},
                {"commit", "-ax"},
                {"commit", "-xm"},
                {"commit", "-xa"},
                {"push", "-fx"},
                {"push", "-xf"},
                {"commit", "--UNKNOWN"},
                {"commit", "--INVALID"},
                {"push", "--UNKNOWN"},
                {"push", "--INVALID"},
                {"commit", "--verbose"},
                {"commit", "--quiet"},
                {"commit", "--debug"},
                {"commit", "--help"},
                {"commit", "--version"},
                {"push", "--verbose"},
                {"push", "--quiet"},
                {"push", "--debug"},
                {"push", "--help"},
                {"push", "--version"},
                {"commit", "--author"},
                {"commit", "--date"},
                {"commit", "--signoff"},
                {"commit", "--gpg-sign"},
                {"commit", "--no-verify"},
                {"push", "--tags"},
                {"push", "--all"},
                {"push", "--mirror"},
                {"push", "--delete"},
                {"push", "--prune"},
                {"fetch", "--all"},
                {"fetch", "--tags"},
                {"fetch", "--prune"},
                {"fetch", "--depth"},
                {"fetch", "--shallow"},
                {"complete", "--bash"},
                {"complete", "--zsh"},
                {"complete", "--fish"},
                {"complete", "--powershell"}
            }));
        
        auto command = std::get<0>(test_case);
        auto option = std::get<1>(test_case);
        std::string input = command + " " + option;
        
        DYNAMIC_SECTION("Unknown option: '" << input << "'") {
            auto candidates = engine.GenerateCandidates(input);
            
            // Property: Non-matching option must return empty list
            REQUIRE(candidates.empty());
        }
    }

    SECTION("Gibberish input") {
        // Test 100+ random gibberish inputs that truly don't match any commands
        auto gibberish = GENERATE(
            values({"!@#$%^&*()", "[]{}()<>", ";;;:::,,,", "///\\\\\\|||",
                    "~~~```'''", "\"\"\"\"\"",
                    "12345", "67890", "00000", "99999", "11111",
                    "!@#", "$%^", "&*(", "()_", "+-=",
                    "{}[]", "<>?", "/\\|", "~`'", "\"\"\"",
                    "a!b@c#", "x$y%z^", "1&2*3(", "4)5_6+", "7-8=9{",
                    "commit!@#", "push$%^", "fetch&*(", "worktree()_", "complete+-=",
                    "commit --message!@#", "push --force$%^", "fetch --all&*(",
                    "worktree add()_", "worktree list+-=", "worktree remove{}[]",
                    "commit -m!@#", "push -f$%^", "commit -a&*(",
                    "commit--message", "push--force", "fetch--all",
                    "commit-message", "push-force", "fetch-all",
                    "commit_message", "push_force", "fetch_all",
                    "commit.message", "push.force", "fetch.all",
                    "commit:message", "push:force", "fetch:all",
                    "commit;message", "push;force", "fetch;all",
                    "commit,message", "push,force", "fetch,all",
                    "commit@message", "push@force", "fetch@all",
                    "commit#message", "push#force", "fetch#all",
                    "commit$message", "push$force", "fetch$all",
                    "commit%message", "push%force", "fetch%all",
                    "commit^message", "push^force", "fetch^all",
                    "commit&message", "push&force", "fetch&all",
                    "commit*message", "push*force", "fetch*all",
                    "commit(message", "push(force", "fetch(all",
                    "commit)message", "push)force", "fetch)all",
                    "commit[message", "push[force", "fetch[all",
                    "commit]message", "push]force", "fetch]all",
                    "commit{message", "push{force", "fetch{all",
                    "commit}message", "push}force", "fetch}all",
                    "commit<message", "push<force", "fetch<all",
                    "commit>message", "push>force", "fetch>all",
                    "commit?message", "push?force", "fetch?all",
                    "commit/message", "push/force", "fetch/all",
                    "commit\\message", "push\\force", "fetch\\all",
                    "commit|message", "push|force", "fetch|all",
                    "commit~message", "push~force", "fetch~all",
                    "commit`message", "push`force", "fetch`all",
                    "commit'message", "push'force", "fetch'all",
                    "commit\"message", "push\"force", "fetch\"all",
                    "!commit", "@push", "#fetch", "$worktree", "%complete",
                    "^commit", "&push", "*fetch", "(worktree", ")complete",
                    "[commit", "]push", "{fetch", "}worktree", "<complete",
                    ">commit", "?push", "/fetch", "\\worktree", "|complete",
                    "~commit", "`push", "'fetch", "\"worktree", ":complete",
                    ";commit", ",push", ".fetch", "-worktree", "=complete"}));
        
        DYNAMIC_SECTION("Gibberish input: '" << gibberish << "'") {
            auto candidates = engine.GenerateCandidates(gibberish);
            
            // Property: Gibberish input must return empty candidate list
            REQUIRE(candidates.empty());
        }
    }

    SECTION("Commands without subcommands") {
        // Test commands that don't have subcommands
        auto command = GENERATE(values({"commit", "push", "fetch", "complete"}));
        
        DYNAMIC_SECTION("Command without subcommands: '" << command << " '") {
            std::string input = std::string(command) + " ";
            auto candidates = engine.GenerateCandidates(input);
            
            // Property: Command without subcommands returns empty list for subcommand completion
            REQUIRE(candidates.empty());
        }
    }
}

TEST_CASE("Property: Candidate completeness and consistency",
          "[Feature: tui-command-input-enhancement][Property]") {
    CLI::App app{"Test App"};
    SetupTestApp(app);
    auto metadata = std::make_shared<MetadataCache>(app);
    AutocompleteEngine engine(metadata);

    SECTION("All candidates have required fields") {
        auto input = GENERATE(values({"", "c", "commit --", "worktree "}));
        
        DYNAMIC_SECTION("Candidate fields for: '" << input << "'") {
            auto candidates = engine.GenerateCandidates(input);

            for (const auto& candidate : candidates) {
                // Property: All candidates must have non-empty text
                REQUIRE(!candidate.text.empty());
                
                // Property: All candidates must have a description
                REQUIRE(!candidate.description.empty());
                
                // Property: Completion text must match display text
                REQUIRE(candidate.completion == candidate.text);
            }
        }
    }
}
