#include <CLI/CLI.hpp>
#include <kano_unattended.hpp>

#include "command_registry.hpp"
#include "tui_dashboard_runner.hpp"

#include <exception>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {

#ifdef _WIN32
// KOG-BUG-0146: bounded test-only SEH diagnostic.  When the env var
// KOG_TUI_TEST_DIAG_LOG points at a writable file path AND KOG_TEST_MODE=1,
// every checkpoint in this translation unit appends one line to that file
// so the test can determine exactly which production-side stage the
// standalone TUI reached before the exit.  The behaviour is fully opt-in:
// a normal operator run never reads the env var and never opens the file.
void WriteDiag(const char* InPath, const char* InMessage) {
    if (InPath == nullptr || *InPath == '\0' || InMessage == nullptr) {
        return;
    }
    HANDLE file = CreateFileA(
        InPath, FILE_APPEND_DATA,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return;
    }
    const DWORD pid = GetCurrentProcessId();
    char buf[512];
    const int n = std::snprintf(
        buf, sizeof(buf), "%s\tpid=%lu\n", InMessage,
        static_cast<unsigned long>(pid));
    if (n > 0) {
        DWORD written = 0;
        (void)WriteFile(file, buf, static_cast<DWORD>(n), &written, nullptr);
    }
    CloseHandle(file);
}

bool DiagLoggingEnabled() {
    const char* testMode = std::getenv("KOG_TEST_MODE");
    if (testMode == nullptr || testMode[0] != '1' || testMode[1] != '\0') {
        return false;
    }
    const char* path = std::getenv("KOG_TUI_TEST_DIAG_LOG");
    return path != nullptr && *path != '\0';
}

const char* DiagPath() {
    return std::getenv("KOG_TUI_TEST_DIAG_LOG");
}
#else
void WriteDiag(const char*, const char*) {}
bool DiagLoggingEnabled() { return false; }
const char* DiagPath() { return nullptr; }
#endif

}  // namespace

#ifdef _WIN32
// KOG-BUG-0146: test-only structured-exception sink.  When diag logging is
// enabled, log the SEH code so the test can localise the crash before the
// unattended-execution filter terminates the process with that code as
// the exit code.  This handler always returns EXCEPTION_CONTINUE_SEARCH so
// the existing kano_unattended FailUnhandledException filter still
// runs and produces the documented exit code.
LONG WINAPI KogBug0146DiagSehSink(EXCEPTION_POINTERS* InException) noexcept {
    const char* path = DiagPath();
    if (path != nullptr) {
        const unsigned int code = InException && InException->ExceptionRecord
            ? InException->ExceptionRecord->ExceptionCode : 0xFFFFFFFFu;
        char buf[256];
        const int n = std::snprintf(
            buf, sizeof(buf), "seh=caught\tcode=0x%08X\taddr=%p\n",
            code,
            InException && InException->ExceptionRecord
                ? InException->ExceptionRecord->ExceptionAddress
                : nullptr);
        if (n > 0) {
            HANDLE file = CreateFileA(
                path, FILE_APPEND_DATA,
                FILE_SHARE_READ | FILE_SHARE_WRITE,
                nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file != INVALID_HANDLE_VALUE) {
                DWORD written = 0;
                (void)WriteFile(file, buf, static_cast<DWORD>(n), &written, nullptr);
                CloseHandle(file);
            }
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
#endif

int main(int InArgc, char* InArgv[]) {
    kano::infra::ConfigureUnattendedExecutionIfRequested();

    // KOG-BUG-0146: install the test-only SEH sink after unattended
    // configuration so it sees the same exception the unattended filter
    // sees, but log the code before delegating to the default handler.
    const bool diagEnabled = DiagLoggingEnabled();
    if (diagEnabled) {
        WriteDiag(DiagPath(), "diag=main_entered");
#ifdef _WIN32
        SetUnhandledExceptionFilter(KogBug0146DiagSehSink);
#endif
    }

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

    if (diagEnabled) {
        WriteDiag(DiagPath(), "diag=app_constructed");
    }

    try {
        kano::git::commands::RegisterAll(app);
        if (diagEnabled) {
            WriteDiag(DiagPath(), "diag=register_all_done");
        }
        app.parse(InArgc, InArgv);
        if (diagEnabled) {
            WriteDiag(DiagPath(), "diag=parse_done");
        }
        if (demo) {
            kano::git::commands::PrintTuiDemoSummary();
            return 0;
        }
        if (diagEnabled) {
            WriteDiag(DiagPath(), "diag=calling_run_tui_dashboard");
        }
        const int rc = kano::git::commands::RunTuiDashboard(app, theme);
        if (diagEnabled) {
            char buf[128];
            std::snprintf(buf, sizeof(buf), "diag=run_tui_dashboard_returned rc=%d", rc);
            WriteDiag(DiagPath(), buf);
        }
        return rc;
    } catch (const CLI::ParseError& e) {
        if (diagEnabled) {
            WriteDiag(DiagPath(), "diag=caught_cli_parse_error");
        }
        return app.exit(e);
    } catch (const std::exception& e) {
        if (diagEnabled) {
            const std::string msg = std::string("diag=caught_std_exception what=") + e.what();
            WriteDiag(DiagPath(), msg.c_str());
        }
        std::cerr << "Fatal error: " << e.what() << "\n";
        return 1;
    }
}
