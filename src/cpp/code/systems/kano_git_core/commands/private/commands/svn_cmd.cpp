// svn command — Git-Subversion bridge
// Delegates to: scripts/vcs-bridges/svn/*.sh

#include "command_registry.hpp"
#include "shell_executor.hpp"

#include <iostream>
#include <string>
#include <vector>

namespace {

auto RunGitSvnSubcommand(const std::string& InSubcommand, const std::vector<std::string>& InExtras) -> int {
    if (InExtras.empty()) {
        std::cerr << "Error: svn " << InSubcommand << " requires arguments\n";
        return 1;
    }

    std::vector<std::string> args = {"svn", InSubcommand};
    args.insert(args.end(), InExtras.begin(), InExtras.end());
    const auto result = kano::git::shell::ExecuteCommand("git", args, kano::git::shell::ExecMode::PassThrough);
    return result.exitCode;
}

} // namespace

namespace kano::git::commands {

void RegisterSvn(CLI::App& InApp) {
    auto* cmd = InApp.add_subcommand("svn", "Git-Subversion bridge (git-svn)");

    auto* clone = cmd->add_subcommand("clone", "Clone a SVN repository");
    clone->allow_extras();
    clone->callback([=]() {
        const auto extras = clone->remaining();
        const auto code = RunGitSvnSubcommand("clone", std::vector<std::string>(extras.begin(), extras.end()));
        if (code != 0) {
            throw CLI::RuntimeError(code);
        }
    });

    auto* fetch = cmd->add_subcommand("fetch", "Fetch from SVN");
    fetch->allow_extras();
    fetch->callback([=]() {
        const auto extras = fetch->remaining();
        const auto code = RunGitSvnSubcommand("fetch", std::vector<std::string>(extras.begin(), extras.end()));
        if (code != 0) {
            throw CLI::RuntimeError(code);
        }
    });

    auto* dcommit = cmd->add_subcommand("dcommit", "Push commits to SVN");
    dcommit->allow_extras();
    dcommit->callback([=]() {
        const auto extras = dcommit->remaining();
        const auto code = RunGitSvnSubcommand("dcommit", std::vector<std::string>(extras.begin(), extras.end()));
        if (code != 0) {
            throw CLI::RuntimeError(code);
        }
    });

    auto* rebase = cmd->add_subcommand("rebase", "Rebase from SVN");
    rebase->allow_extras();
    rebase->callback([=]() {
        const auto extras = rebase->remaining();
        const auto code = RunGitSvnSubcommand("rebase", std::vector<std::string>(extras.begin(), extras.end()));
        if (code != 0) {
            throw CLI::RuntimeError(code);
        }
    });
}

} // namespace kano::git::commands
