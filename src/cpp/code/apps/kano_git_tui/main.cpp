#include <CLI/CLI.hpp>

#include "command_registry.hpp"
#include "tui_dashboard_runner.hpp"

#include <exception>
#include <iostream>

int main(int InArgc, char* InArgv[]) {
    CLI::App app{"Kano Git standalone TUI dashboard", "kano-git-tui"};
    bool demo = false;
    app.add_flag("--demo", demo, "Print demo summary and exit (non-interactive)");

    try {
        kano::git::commands::RegisterAll(app);
        app.parse(InArgc, InArgv);
        if (demo) {
            kano::git::commands::PrintTuiDemoSummary();
            return 0;
        }
        return kano::git::commands::RunTuiDashboard(app);
    } catch (const CLI::ParseError& e) {
        return app.exit(e);
    } catch (const std::exception& e) {
        std::cerr << "Fatal error: " << e.what() << "\n";
        return 1;
    }
}
