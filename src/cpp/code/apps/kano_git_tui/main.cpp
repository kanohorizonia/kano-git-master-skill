#include <CLI/CLI.hpp>

#include "command_registry.hpp"
#include "tui_dashboard_runner.hpp"

#include <exception>
#include <cstdlib>
#include <iostream>
#include <string>

int main(int InArgc, char* InArgv[]) {
    CLI::App app{"Kano Git standalone TUI dashboard", "kano-git-tui"};
    bool demo = false;
    std::string theme = "auto";
    if (const char* configuredTheme = std::getenv("KOG_TUI_THEME");
        configuredTheme != nullptr && *configuredTheme != '\0') {
        theme = configuredTheme;
    }
    app.add_flag("--demo", demo, "Print demo summary and exit (non-interactive)");
    app.add_option("--theme", theme, "Terminal theme: auto, dark, light, or mono")
        ->check(CLI::IsMember({"auto", "dark", "light", "mono"}));

    try {
        kano::git::commands::RegisterAll(app);
        app.parse(InArgc, InArgv);
        if (demo) {
            kano::git::commands::PrintTuiDemoSummary();
            return 0;
        }
        return kano::git::commands::RunTuiDashboard(app, theme);
    } catch (const CLI::ParseError& e) {
        return app.exit(e);
    } catch (const std::exception& e) {
        std::cerr << "Fatal error: " << e.what() << "\n";
        return 1;
    }
}
