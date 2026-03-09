// p4 command — Git-Perforce bridge
// Delegates to: scripts/vcs-bridges/p4/*.sh

#include <CLI/CLI.hpp>
#include "shell_executor.hpp"

#include <iostream>
#include <string>
#include <vector>

namespace {

auto RunGitP4Subcommand(
    const std::string& InSubcommand,
    const std::vector<std::string>& InExtras,
    bool InRequireArgs) -> int {
    if (InRequireArgs && InExtras.empty()) {
        std::cerr << "Error: p4 " << InSubcommand << " requires arguments\n";
        return 1;
    }

    std::vector<std::string> args = {"p4", InSubcommand};
    args.insert(args.end(), InExtras.begin(), InExtras.end());
    const auto result = kano::git::shell::ExecuteCommand("git", args, kano::git::shell::ExecMode::PassThrough);
    return result.exitCode;
}

} // namespace

namespace kano::git::commands {

void RegisterP4(CLI::App& InApp) {
    auto* cmd = InApp.add_subcommand("p4", "Git-Perforce bridge (git-p4)");

    auto* clone = cmd->add_subcommand("clone", "Clone a Perforce depot");
    clone->allow_extras();
    clone->callback([=]() {
        const auto extras = clone->remaining();
        std::exit(RunGitP4Subcommand("clone", std::vector<std::string>(extras.begin(), extras.end()), true));
    });

    auto* sync = cmd->add_subcommand("sync", "Sync from Perforce");
    sync->allow_extras();
    sync->callback([=]() {
        const auto extras = sync->remaining();
        std::exit(RunGitP4Subcommand("sync", std::vector<std::string>(extras.begin(), extras.end()), false));
    });

    auto* submit = cmd->add_subcommand("submit", "Submit to Perforce");
    submit->allow_extras();
    submit->callback([=]() {
        const auto extras = submit->remaining();
        std::exit(RunGitP4Subcommand("submit", std::vector<std::string>(extras.begin(), extras.end()), false));
    });

    auto* rebase = cmd->add_subcommand("rebase", "Rebase from Perforce");
    rebase->allow_extras();
    rebase->callback([=]() {
        const auto extras = rebase->remaining();
        std::exit(RunGitP4Subcommand("rebase", std::vector<std::string>(extras.begin(), extras.end()), false));
    });
}

} // namespace kano::git::commands
