// tui command

#include <CLI/CLI.hpp>
#include "tui_dashboard_runner.hpp"

#include <cstdlib>

namespace kano::git::commands {

void RegisterTui(CLI::App& InApp) {
    auto* cmd = InApp.add_subcommand("tui", "Launch interactive KOG terminal dashboard");

    auto* demo = new bool{false};
    cmd->add_flag("--demo", *demo, "Print demo summary and exit (non-interactive)");

    cmd->callback([demo, &InApp]() {
        if (*demo) {
            PrintTuiDemoSummary();
            return;
        }
        std::exit(RunTuiDashboard(InApp));
    });
}

} // namespace kano::git::commands
