// Windows-only companion for the ConPTY smoke test.  It owns the same
// pseudoconsole as the production binary and can therefore prove that the
// production process restored that terminal's modes and code pages.

#include <windows.h>

#include <iostream>
#include <string>

namespace {

struct ConsoleState final {
    DWORD inputMode = 0;
    DWORD outputMode = 0;
    UINT inputCodePage = 0;
    UINT outputCodePage = 0;
};

auto CaptureConsoleState(ConsoleState& OutState) -> bool {
    const HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
    const HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
    if (input == nullptr || input == INVALID_HANDLE_VALUE ||
        output == nullptr || output == INVALID_HANDLE_VALUE ||
        GetConsoleMode(input, &OutState.inputMode) == 0 ||
        GetConsoleMode(output, &OutState.outputMode) == 0) {
        return false;
    }
    OutState.inputCodePage = GetConsoleCP();
    OutState.outputCodePage = GetConsoleOutputCP();
    return OutState.inputCodePage != 0 && OutState.outputCodePage != 0;
}

auto QuoteArgument(const std::wstring& InValue) -> std::wstring {
    std::wstring quoted = L"\"";
    std::size_t slashes = 0;
    for (const wchar_t character : InValue) {
        if (character == L'\\') {
            ++slashes;
            continue;
        }
        if (character == L'\"') {
            quoted.append(slashes * 2U + 1U, L'\\');
            quoted.push_back(L'\"');
            slashes = 0;
            continue;
        }
        quoted.append(slashes, L'\\');
        slashes = 0;
        quoted.push_back(character);
    }
    quoted.append(slashes * 2U, L'\\');
    quoted.push_back(L'\"');
    return quoted;
}

auto PrintFailure(const char* InReason) -> int {
    std::cout << "KOG_TUI_TERMINAL_STATE_FAILED:" << InReason << '\n' << std::flush;
    return 2;
}

} // namespace

auto wmain(int InArgumentCount, wchar_t** InArguments) -> int {
    if (InArgumentCount != 2 || InArguments[1] == nullptr ||
        InArguments[1][0] == L'\0') {
        return PrintFailure("missing-production-binary");
    }

    ConsoleState before{};
    if (!CaptureConsoleState(before)) {
        return PrintFailure("console-state-unavailable-before-launch");
    }

    std::wstring commandLine = QuoteArgument(InArguments[1]);
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(
            nullptr, commandLine.data(), nullptr, nullptr, FALSE, 0,
            nullptr, nullptr, &startup, &process)) {
        return PrintFailure("production-launch-failed");
    }
    CloseHandle(process.hThread);

    // The outer ConPTY harness owns an eight-second deadline.  Keep this
    // child deadline shorter so the wrapper always terminates and joins the
    // production process before the parent starts closing the pseudoconsole.
    const auto waitResult = WaitForSingleObject(process.hProcess, 6'000);
    if (waitResult != WAIT_OBJECT_0) {
        TerminateProcess(process.hProcess, 253);
        (void)WaitForSingleObject(process.hProcess, 1'000);
        CloseHandle(process.hProcess);
        return PrintFailure(waitResult == WAIT_TIMEOUT
            ? "production-exit-timeout" : "production-wait-failed");
    }
    DWORD childExit = 0;
    const bool gotExit = GetExitCodeProcess(process.hProcess, &childExit) != 0;
    CloseHandle(process.hProcess);
    if (!gotExit || childExit != 0) {
        return PrintFailure("production-exit-nonzero");
    }

    ConsoleState after{};
    if (!CaptureConsoleState(after)) {
        return PrintFailure("console-state-unavailable-after-exit");
    }
    if (after.inputMode != before.inputMode ||
        after.outputMode != before.outputMode ||
        after.inputCodePage != before.inputCodePage ||
        after.outputCodePage != before.outputCodePage) {
        return PrintFailure("console-state-not-restored");
    }
    std::cout << "KOG_TUI_TERMINAL_STATE_RESTORED\n" << std::flush;
    return 0;
}
