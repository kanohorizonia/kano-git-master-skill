// tui command

#include <CLI/CLI.hpp>
#include "tui_dashboard_runner.hpp"

#include <cstdlib>
#include <string>

namespace kano::git::commands {

void RegisterTui(CLI::App& InApp) {
    auto* cmd = InApp.add_subcommand("tui", "Launch interactive KOG terminal dashboard");

    auto* demo = new bool{false};
    cmd->add_flag("--demo", *demo, "Print demo summary and exit (non-interactive)");

    auto* theme = new std::string{"auto"};
    if (const char* configuredTheme = std::getenv("KOG_TUI_THEME");
        configuredTheme != nullptr && *configuredTheme != '\0') {
        *theme = configuredTheme;
    }
    cmd->add_option("--theme", *theme, "Terminal theme: auto, dark, light, or mono")
        ->check(CLI::IsMember({"auto", "dark", "light", "mono"}));

    cmd->callback([demo, theme, &InApp]() {
        if (*demo) {
            PrintTuiDemoSummary();
            return;
        }
        std::exit(RunTuiDashboard(InApp, *theme));
    });
}

} // namespace kano::git::commands
