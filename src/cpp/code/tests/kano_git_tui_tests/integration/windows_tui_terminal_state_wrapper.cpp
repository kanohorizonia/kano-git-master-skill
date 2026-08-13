// Windows-only companion for the ConPTY smoke test.  It owns the same
// pseudoconsole as the production binary and can therefore prove that the
// production process restored that terminal's modes and code pages.

#include <windows.h>

#include <cstddef>
#include <iostream>
#include <string>
#include <vector>

namespace {

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

} // namespace

auto wmain(int InArgumentCount, wchar_t** InArguments) -> int {
    if (InArgumentCount != 2 || InArguments[1] == nullptr ||
        InArguments[1][0] == L'\0') {
        return PrintFailure("missing-production-binary");
    }

    ScopedHandle consoleInput;
    ScopedHandle consoleOutput;
    DWORD failureError = ERROR_SUCCESS;
    if (!OpenConsoleDevices(consoleInput, consoleOutput, failureError)) {
        return PrintWin32Failure("console-device-open-before-launch", failureError);
    }

    ConsoleState before{};
    const char* captureFailure = nullptr;
    if (!CaptureConsoleState(
            before, consoleInput, consoleOutput, captureFailure, failureError)) {
        return PrintWin32Failure(captureFailure, failureError);
    }

    std::wstring commandLine = QuoteArgument(InArguments[1]);
    HANDLE inheritedHandles[] = {consoleInput.Get(), consoleOutput.Get()};
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
    if (UpdateProcThreadAttribute(
            attributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
            inheritedHandles, sizeof(inheritedHandles), nullptr, nullptr) == 0) {
        return PrintWin32Failure("production-handle-list-init-failed", GetLastError());
    }

    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = consoleInput.Get();
    startup.StartupInfo.hStdOutput = consoleOutput.Get();
    startup.StartupInfo.hStdError = consoleOutput.Get();
    startup.lpAttributeList = attributeList;
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(
            nullptr, commandLine.data(), nullptr, nullptr, TRUE,
            EXTENDED_STARTUPINFO_PRESENT, nullptr, nullptr,
            &startup.StartupInfo, &process)) {
        return PrintWin32Failure("production-launch-failed", GetLastError());
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
    return 0;
}
