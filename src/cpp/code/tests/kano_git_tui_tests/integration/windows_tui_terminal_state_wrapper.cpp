// Windows-only companion for the ConPTY smoke test.  It owns the same
// pseudoconsole as the production binary and can therefore prove that the
// production process restored that terminal's modes and code pages.
//
// KOG-BUG-0107 round 5: stage checkpoint emitter.  When the env var
// KOG_TUI_TEST_STAGE_LOG is set (absolute file path), each major
// checkpoint appends "stage=N<tab>msg\n" so the test process can read
// it after the wrapper exits and determine exactly which stage the
// production launch reached.  This is the only new behaviour; the
// existing console-restoration contract is unchanged.

#include <windows.h>

#include <cstddef>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cwchar>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

namespace {

std::wstring GetStageLogPath() {
    wchar_t buffer[32767]{};
    const DWORD len = GetEnvironmentVariableW(L"KOG_TUI_TEST_STAGE_LOG",
        buffer, sizeof(buffer) / sizeof(buffer[0]));
    if (len == 0U || len >= sizeof(buffer) / sizeof(buffer[0])) {
        return {};
    }
    return std::wstring(buffer);
}

void WriteStage(const wchar_t* InTag) {
    const auto path = GetStageLogPath();
    if (path.empty()) return;
    // std::ofstream::open takes const char* (or filesystem::path in C++17).
    // Use Windows CreateFileW directly to avoid ambiguity and keep this
    // test-only writer minimal and dependency-free.
    HANDLE file = CreateFileW(
        path.c_str(),
        FILE_APPEND_DATA,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    std::string utf8;
    utf8.append("stage=");
    for (const wchar_t* p = InTag; *p != L'\0'; ++p) {
        const wchar_t c = *p;
        if (c < 0x80) {
            utf8.push_back(static_cast<char>(c));
        }
    }
    char buf[512];
    const int n = std::snprintf(
        buf, sizeof(buf), "%s\tpid=%lu\ttid=%lu\n",
        utf8.c_str(),
        static_cast<unsigned long>(GetCurrentProcessId()),
        static_cast<unsigned long>(GetCurrentThreadId()));
    if (n > 0) {
        DWORD written = 0;
        (void)WriteFile(file, buf, static_cast<DWORD>(n), &written, nullptr);
    }
    CloseHandle(file);
}

class ScopedHandle final {
  public:
    ScopedHandle() = default;
    ~ScopedHandle() {
        if (value_ != nullptr && value_ != INVALID_HANDLE_VALUE) {
            (void)CloseHandle(value_);
        }
    }

    ScopedHandle(const ScopedHandle&) = delete;
    auto operator=(const ScopedHandle&) -> ScopedHandle& = delete;

    auto Reset(const HANDLE InValue) -> void {
        if (value_ != nullptr && value_ != INVALID_HANDLE_VALUE) {
            (void)CloseHandle(value_);
        }
        value_ = InValue;
    }

    [[nodiscard]] auto Get() const -> HANDLE { return value_; }

  private:
    HANDLE value_ = INVALID_HANDLE_VALUE;
};

struct ConsoleState final {
    DWORD inputMode = 0;
    DWORD outputMode = 0;
    UINT inputCodePage = 0;
    UINT outputCodePage = 0;
};

auto OpenConsoleDevices(ScopedHandle& OutInput, ScopedHandle& OutOutput,
                        DWORD& OutError) -> bool {
    SECURITY_ATTRIBUTES security{};
    security.nLength = sizeof(security);
    security.bInheritHandle = TRUE;

    const HANDLE input = CreateFileW(
        L"CONIN$", GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, &security, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (input == INVALID_HANDLE_VALUE) {
        OutError = GetLastError();
        return false;
    }
    OutInput.Reset(input);

    const HANDLE output = CreateFileW(
        L"CONOUT$", GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, &security, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (output == INVALID_HANDLE_VALUE) {
        OutError = GetLastError();
        return false;
    }
    OutOutput.Reset(output);

    if (SetHandleInformation(input, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT) == 0 ||
        SetHandleInformation(output, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT) == 0) {
        OutError = GetLastError();
        return false;
    }
    return true;
}

auto CaptureConsoleState(ConsoleState& OutState, const ScopedHandle& InInput,
                         const ScopedHandle& InOutput, const char*& OutReason,
                         DWORD& OutError) -> bool {
    if (GetConsoleMode(InInput.Get(), &OutState.inputMode) == 0) {
        OutReason = "console-input-mode-unavailable";
        OutError = GetLastError();
        return false;
    }
    if (GetConsoleMode(InOutput.Get(), &OutState.outputMode) == 0) {
        OutReason = "console-output-mode-unavailable";
        OutError = GetLastError();
        return false;
    }
    OutState.inputCodePage = GetConsoleCP();
    if (OutState.inputCodePage == 0) {
        OutReason = "console-input-codepage-unavailable";
        OutError = GetLastError();
        return false;
    }
    OutState.outputCodePage = GetConsoleOutputCP();
    if (OutState.outputCodePage == 0) {
        OutReason = "console-output-codepage-unavailable";
        OutError = GetLastError();
        return false;
    }
    return true;
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

auto PrintWin32Failure(const char* InReason, const DWORD InError) -> int {
    std::cout << "KOG_TUI_TERMINAL_STATE_FAILED:" << InReason
              << ":win32=" << InError << '\n' << std::flush;
    return 2;
}

auto ParseInheritedEventHandle(const wchar_t* InText, HANDLE& OutHandle,
                               DWORD& OutError) -> bool {
    if (InText == nullptr || InText[0] == L'\0') {
        OutError = ERROR_INVALID_PARAMETER;
        return false;
    }
    for (const wchar_t* cursor = InText; *cursor != L'\0'; ++cursor) {
        if (*cursor < L'0' || *cursor > L'9') {
            OutError = ERROR_INVALID_PARAMETER;
            return false;
        }
    }
    errno = 0;
    wchar_t* end = nullptr;
    const auto value = std::wcstoull(InText, &end, 10);
    if (errno == ERANGE || end == InText || *end != L'\0' ||
        value > std::numeric_limits<std::uintptr_t>::max() || value == 0U ||
        value == reinterpret_cast<std::uintptr_t>(INVALID_HANDLE_VALUE)) {
        OutError = ERROR_INVALID_PARAMETER;
        return false;
    }
    const HANDLE handle = reinterpret_cast<HANDLE>(
        static_cast<std::uintptr_t>(value));
    DWORD flags = 0U;
    if (GetHandleInformation(handle, &flags) == 0) {
        OutError = GetLastError();
        return false;
    }
    if (WaitForSingleObject(handle, 0U) != WAIT_TIMEOUT) {
        OutError = ERROR_INVALID_HANDLE;
        return false;
    }
    if (SetHandleInformation(handle, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT) == 0) {
        OutError = GetLastError();
        return false;
    }
    OutHandle = handle;
    return true;
}

auto WriteConsoleEvidence(const ScopedHandle& InOutput, const char* InBytes,
                          const DWORD InSize, DWORD& OutError) -> bool {
    DWORD offset = 0;
    while (offset < InSize) {
        DWORD written = 0;
        const BOOL wrote = WriteFile(InOutput.Get(), InBytes + offset,
            InSize - offset, &written, nullptr);
        if (wrote == 0 || written == 0U) {
            OutError = wrote == 0 ? GetLastError() : ERROR_WRITE_FAULT;
            return false;
        }
        offset += written;
    }
    return true;
}

// The outer controller owns the only hard deadline for this process tree.  If
// a process handle cannot be waited after termination was requested, keep that
// handle alive and let the controller's kill-on-close job reclaim the tree.
[[noreturn]] auto StallForOuterController() -> void {
    for (;;) Sleep(INFINITE);
}

} // namespace

auto wmain(int InArgumentCount, wchar_t** InArguments) -> int {
    WriteStage(L"stage01_wrapper_entered");
    if (InArgumentCount < 2 || InArguments[1] == nullptr ||
        InArguments[1][0] == L'\0') {
        WriteStage(L"stage01_failed_missing_binary");
        return PrintFailure("missing-production-binary");
    }
    WriteStage(L"stage02_args_parsed");
    // KOG-BUG-0107: in --test-skip-startup-harness mode the wrapper still
    // sets KOG_TEST_MODE=1 (other test infrastructure needs it) but does
    // not set the startup cancel-ack env vars; the production TUI then
    // proceeds without the harness, so an owned subprocess is already
    // running when q/Esc arrives.
    const bool harnessMode = InArgumentCount == 5 &&
        InArguments[2] != nullptr &&
        std::wcscmp(InArguments[2], L"--test-cancel-ack") == 0;
    const bool skipHarnessMode = InArgumentCount == 3 &&
        InArguments[2] != nullptr &&
        std::wcscmp(InArguments[2], L"--test-skip-startup-harness") == 0;
    if (!harnessMode && !skipHarnessMode) {
        return PrintFailure("missing-test-mode-flag");
    }
    HANDLE armedEvent = nullptr;
    HANDLE acknowledgementEvent = nullptr;
    DWORD failureError = ERROR_SUCCESS;
    if (harnessMode &&
        (!ParseInheritedEventHandle(InArguments[3], armedEvent, failureError) ||
         !ParseInheritedEventHandle(InArguments[4], acknowledgementEvent, failureError) ||
         armedEvent == acknowledgementEvent)) {
        return PrintWin32Failure("invalid-cancellation-event-handle", failureError);
    }

    ScopedHandle consoleInput;
    ScopedHandle consoleOutput;
    WriteStage(L"stage03_console_devices_in_use");
    if (!OpenConsoleDevices(consoleInput, consoleOutput, failureError)) {
        WriteStage(L"stage03_failed_open_console_devices");
        return PrintWin32Failure("console-device-open-before-launch", failureError);
    }
    if (SetEnvironmentVariableW(L"KOG_TEST_MODE", L"1") == 0) {
        return PrintWin32Failure(
            "production-test-environment-unavailable", GetLastError());
    }
    if (harnessMode &&
        (SetEnvironmentVariableW(
             L"KOG_TUI_TEST_STARTUP_CANCEL_ACK", L"1") == 0 ||
         SetEnvironmentVariableW(L"KOG_TUI_TEST_STARTUP_CANCEL_ARMED_HANDLE",
             std::to_wstring(reinterpret_cast<std::uintptr_t>(armedEvent)).c_str()) == 0 ||
         SetEnvironmentVariableW(L"KOG_TUI_TEST_STARTUP_CANCEL_ACK_HANDLE",
             std::to_wstring(reinterpret_cast<std::uintptr_t>(acknowledgementEvent)).c_str()) == 0)) {
        return PrintWin32Failure(
            "production-test-environment-unavailable", GetLastError());
    }

    ConsoleState before{};
    const char* captureFailure = nullptr;
    if (!CaptureConsoleState(
            before, consoleInput, consoleOutput, captureFailure, failureError)) {
        WriteStage(L"stage04_failed_capture_console_state");
        return PrintWin32Failure(captureFailure, failureError);
    }
    WriteStage(L"stage04_console_state_captured");

    std::wstring commandLine = QuoteArgument(InArguments[1]);
    // KOG-BUG-0107 round 5: log the exact command line + cwd + env so we
    // can compare failing and passing paths without dumping arbitrary
    // host state.  Stripped to argv-only -- no host-private paths.
    WriteStage(L"stage03a_command_line_built");
    {
        HANDLE file = CreateFileW(
            GetStageLogPath().c_str(),
            FILE_APPEND_DATA,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE) {
            std::string cmdUtf8 = "argv=";
            for (const wchar_t* p = commandLine.data();
                 p != commandLine.data() + commandLine.size(); ++p) {
                const wchar_t c = *p;
                cmdUtf8.push_back(c < 0x80 ? static_cast<char>(c) : '?');
            }
            cmdUtf8 += "\n";
            DWORD written = 0;
            (void)WriteFile(file, cmdUtf8.data(),
                static_cast<DWORD>(cmdUtf8.size()), &written, nullptr);
            char cwd[1024]{};
            const DWORD cwdLen = GetCurrentDirectoryA(sizeof(cwd), cwd);
            if (cwdLen > 0U && cwdLen < sizeof(cwd)) {
                std::string cwdLine = "cwd=";
                cwdLine.append(cwd, cwdLen);
                cwdLine += "\n";
                (void)WriteFile(file, cwdLine.data(),
                    static_cast<DWORD>(cwdLine.size()), &written, nullptr);
            }
            CloseHandle(file);
        }
    }
    // In --test-skip-startup-harness mode the harness event handles are
    // nullptr; passing nullptr entries in PROC_THREAD_ATTRIBUTE_HANDLE_LIST
    // makes UpdateProcThreadAttribute fail with ERROR_INVALID_PARAMETER.
    // Build the smallest accurate list so the production launch succeeds.
    HANDLE inheritedHandles[4]{};
    SIZE_T inheritedHandleCount = 0;
    HANDLE inheritedHandlesBuf[2];
    SIZE_T inheritedHandleCountBuf = 0;
    if (harnessMode) {
        inheritedHandles[0] = consoleInput.Get();
        inheritedHandles[1] = consoleOutput.Get();
        inheritedHandles[2] = armedEvent;
        inheritedHandles[3] = acknowledgementEvent;
        inheritedHandleCount = 4;
    } else {
        inheritedHandlesBuf[0] = consoleInput.Get();
        inheritedHandlesBuf[1] = consoleOutput.Get();
        inheritedHandleCountBuf = 2;
    }
    SIZE_T attributeBytes = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attributeBytes);
    std::vector<std::byte> attributes(attributeBytes);
    auto* attributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes.data());
    if (attributeBytes == 0 ||
        InitializeProcThreadAttributeList(attributeList, 1, 0, &attributeBytes) == 0) {
        return PrintWin32Failure("production-attribute-list-init-failed", GetLastError());
    }
    struct AttributeListCleanup final {
        LPPROC_THREAD_ATTRIBUTE_LIST value = nullptr;
        ~AttributeListCleanup() {
            if (value != nullptr) DeleteProcThreadAttributeList(value);
        }
    } cleanup{attributeList};
    if (harnessMode) {
        if (UpdateProcThreadAttribute(
                attributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                inheritedHandles,
                inheritedHandleCount * sizeof(HANDLE),
                nullptr, nullptr) == 0) {
            return PrintWin32Failure("production-handle-list-init-failed", GetLastError());
        }
    } else {
        if (UpdateProcThreadAttribute(
                attributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                inheritedHandlesBuf,
                inheritedHandleCountBuf * sizeof(HANDLE),
                nullptr, nullptr) == 0) {
            return PrintWin32Failure("production-handle-list-init-failed", GetLastError());
        }
    }

    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = consoleInput.Get();
    startup.StartupInfo.hStdOutput = consoleOutput.Get();
    startup.StartupInfo.hStdError = consoleOutput.Get();
    startup.lpAttributeList = attributeList;
    PROCESS_INFORMATION process{};
    // KOG-BUG-0107 round 5: capture the full environment that the
    // wrapper inherits and passes to the production TUI so we can
    // compare with a known-passing production ConPTY test.
    WriteStage(L"stage05a_create_process_starting");
    {
        HANDLE file = CreateFileW(
            GetStageLogPath().c_str(),
            FILE_APPEND_DATA,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE) {
            const DWORD kBufSize = 4096;
            std::vector<wchar_t> buf(kBufSize);
            for (const wchar_t* name : {
                    L"KOG_TEST_MODE",
                    L"KOG_TUI_TEST_STARTUP_CANCEL_ACK",
                    L"PATH",
                    L"TMP",
                    L"TEMP",
                    L"USERPROFILE",
            }) {
                std::string nameUtf8;
                for (const wchar_t* p = name; *p != L'\0'; ++p) {
                    nameUtf8.push_back(static_cast<char>(*p));
                }
                const DWORD len = GetEnvironmentVariableW(name,
                    buf.data(), kBufSize);
                std::string line = "env ";
                line.append(nameUtf8);
                if (len == 0U || len >= kBufSize) {
                    line += "=<absent>\n";
                } else {
                    line += "=";
                    for (DWORD i = 0U; i < len; ++i) {
                        const wchar_t c = buf[i];
                        line.push_back(c < 0x80 ? static_cast<char>(c) : '?');
                    }
                    line += "\n";
                }
                DWORD written = 0;
                (void)WriteFile(file, line.data(),
                    static_cast<DWORD>(line.size()), &written, nullptr);
            }
            CloseHandle(file);
        }
    }
    if (!CreateProcessW(
            nullptr, commandLine.data(), nullptr, nullptr, TRUE,
            EXTENDED_STARTUPINFO_PRESENT, nullptr, nullptr,
            &startup.StartupInfo, &process)) {
        const DWORD err = GetLastError();
        WriteStage(L"stage05_failed_create_process");
        WriteStage((err == ERROR_INVALID_PARAMETER)
            ? L"stage05_create_process_error_87_invalid_parameter"
            : L"stage05_create_process_error_other");
        return PrintWin32Failure("production-launch-failed", err);
    }
    WriteStage(L"stage05_create_process_ok");
    CloseHandle(process.hThread);

    // This is an independent diagnostic deadline.  The outer controller is
    // the sole hard safety bound for this process tree.
    constexpr DWORD kProductionExitTimeoutMs = 5'000;
    constexpr DWORD kProductionTerminateJoinTimeoutMs = 500;
    WriteStage(L"stage06_waiting_for_production_exit");
    const auto waitResult =
        WaitForSingleObject(process.hProcess, kProductionExitTimeoutMs);
    if (waitResult != WAIT_OBJECT_0) {
        WriteStage((waitResult == WAIT_TIMEOUT)
            ? L"stage06_production_exit_timeout"
            : L"stage06_production_wait_failed");
        (void)TerminateProcess(process.hProcess, 253);
        const DWORD terminated = WaitForSingleObject(
            process.hProcess, kProductionTerminateJoinTimeoutMs);
        if (terminated != WAIT_OBJECT_0) {
            // Do not release an unconfirmed production handle.  An infinite
            // wait is safe only after termination has been requested; should
            // it fail, retain ownership and deliberately await the outer job.
            if (terminated == WAIT_TIMEOUT) {
                const DWORD joined =
                    WaitForSingleObject(process.hProcess, INFINITE);
                if (joined == WAIT_OBJECT_0) {
                    CloseHandle(process.hProcess);
                    return PrintFailure(waitResult == WAIT_TIMEOUT
                        ? "production-exit-timeout" : "production-wait-failed");
                }
            }
            StallForOuterController();
        }
        CloseHandle(process.hProcess);
        return PrintFailure(waitResult == WAIT_TIMEOUT
            ? "production-exit-timeout" : "production-wait-failed");
    }
    WriteStage(L"stage07_production_exited_clean");
    DWORD childExit = 0;
    const bool gotExit = GetExitCodeProcess(process.hProcess, &childExit) != 0;
    CloseHandle(process.hProcess);
    if (!gotExit || childExit != 0) {
        WriteStage((childExit == 259)
            ? L"stage08_production_exit_code_259"
            : L"stage08_production_exit_nonzero");
        return PrintFailure("production-exit-nonzero");
    }

    ConsoleState after{};
    captureFailure = nullptr;
    if (!CaptureConsoleState(
            after, consoleInput, consoleOutput, captureFailure, failureError)) {
        return PrintWin32Failure(captureFailure, failureError);
    }
    if (after.inputMode != before.inputMode ||
        after.outputMode != before.outputMode ||
        after.inputCodePage != before.inputCodePage ||
        after.outputCodePage != before.outputCodePage) {
        return PrintFailure("console-state-not-restored");
    }
    constexpr char kRestoredEvidence[] =
        "KOG_TUI_TERMINAL_STATE_RESTORED\n";
    DWORD writeError = ERROR_SUCCESS;
    if (!WriteConsoleEvidence(consoleOutput, kRestoredEvidence,
            static_cast<DWORD>(sizeof(kRestoredEvidence) - 1U), writeError)) {
        return PrintWin32Failure("restored-evidence-write-failed", writeError);
    }
    WriteStage(L"stage09_console_state_restored_emitted");
    return 0;
}
