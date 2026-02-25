// kano-git — Kano Git Master CLI
// AI-powered Git CLI tools
//
// SPDX-License-Identifier: MIT

#include <CLI/CLI.hpp>
#include "KanoGit.Version.hpp"
#include "KanoGit.CommandRegistry.hpp"

int main(int argc, char* argv[]) {
    CLI::App app{
        "Kano Git Master — AI-powered Git CLI tools\n"
        "Standalone: kano-git <command> or kog <command>\n"
        "Unified:    kano git <command> (future)",
        "kano-git"
    };

    app.set_version_flag("--version,-V", std::string{kano::git::GetVersion()});
    app.require_subcommand(0);  // Allow running with no subcommand (shows help)
    app.fallthrough();

    // Register all commands
    kano::git::commands::RegisterAll(app);

    CLI11_PARSE(app, argc, argv);

    // If no subcommand was given, print help
    if (app.get_subcommands().empty()) {
        std::cout << app.help() << std::endl;
    }

    return 0;
}
