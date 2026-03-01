// kano-git — Kano Git Master CLI
// AI-powered Git CLI tools
//
// SPDX-License-Identifier: MIT

#include <CLI/CLI.hpp>
#include "command_registry.hpp"
#include <exception>
#include <iostream>

#if defined(KOG_USE_MODULES)
import kano.git.version;
#else
#include "version.hpp"
#endif

int main(int InArgc, char* InArgv[]) {
    CLI::App app{
        "Kano Git Master — AI-powered Git CLI tools\n"
        "Standalone: kano-git <command> or kog <command>\n"
        "Unified:    kano git <command> (future)",
        "kano-git"
    };

    app.set_version_flag("--version,-V", std::string{kano::git::GetBuildVersion()});
    app.require_subcommand(0);  // Allow running with no subcommand (shows help)
    app.fallthrough();

    try {
        // Register all commands
        kano::git::commands::RegisterAll(app);

        if (InArgc <= 1) {
            std::cout << app.help() << std::endl;
            return 0;
        }

        char** utf8Argv = app.ensure_utf8(InArgv);

        app.parse(InArgc, utf8Argv);
    } catch (const CLI::ParseError& e) {
        return app.exit(e);
    } catch (const std::exception& e) {
        std::cerr << "Fatal error: " << e.what() << "\n";
        return 1;
    } catch (...) {
        std::cerr << "Fatal error: unknown exception\n";
        return 1;
    }

    return 0;
}
