#include <CLI/CLI.hpp>
#include <kano_unattended.hpp>

#include "command_registry.hpp"
#include "tui_dashboard_runner.hpp"

#include <cstdio>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <string>

namespace {

// KOG-BUG-0146: bounded test-only stage-trace file writer.  Uses only
// standard C stdio so the production TUI does not have to pull in
// <windows.h> in a cross-platform translation unit.  Opt-in: the
// helper is a no-op unless KOG_TEST_MODE=1 AND KOG_TUI_TEST_DIAG_LOG
// point at a writable file path.  Normal operator runs never read the
// env vars, so production semantics are unchanged.
void WriteDiag(const char* InMessage) {
    if (InMessage == nullptr) {
        return;
    }
    const char* testMode = std::getenv("KOG_TEST_MODE");
    if (testMode == nullptr || testMode[0] != '1' || testMode[1] != '\0') {
        return;
    }
    const char* path = std::getenv("KOG_TUI_TEST_DIAG_LOG");
    if (path == nullptr || *path == '\0') {
        return;
    }
    std::FILE* file = std::fopen(path, "a");
    if (file == nullptr) {
        return;
    }
    std::fprintf(file, "%s\n", InMessage);
    std::fclose(file);
}

}  // namespace

int main(int InArgc, char* InArgv[]) {
    kano::infra::ConfigureUnattendedExecutionIfRequested();

    // KOG-BUG-0146: test-only stage checkpoint.
    WriteDiag("diag=main_entered");

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

    WriteDiag("diag=app_constructed");

    try {
        kano::git::commands::RegisterAll(app);
        WriteDiag("diag=register_all_done");
        app.parse(InArgc, InArgv);
        WriteDiag("diag=parse_done");
        if (demo) {
            kano::git::commands::PrintTuiDemoSummary();
            return 0;
        }
        WriteDiag("diag=calling_run_tui_dashboard");
        const int rc = kano::git::commands::RunTuiDashboard(app, theme);
        char buf[128];
        std::snprintf(buf, sizeof(buf), "diag=run_tui_dashboard_returned rc=%d", rc);
        WriteDiag(buf);
        return rc;
    } catch (const CLI::ParseError& e) {
        WriteDiag("diag=caught_cli_parse_error");
        return app.exit(e);
    } catch (const std::exception& e) {
        const std::string msg = std::string("diag=caught_std_exception what=") + e.what();
        WriteDiag(msg.c_str());
        std::cerr << "Fatal error: " << e.what() << "\n";
        return 1;
    }
}
