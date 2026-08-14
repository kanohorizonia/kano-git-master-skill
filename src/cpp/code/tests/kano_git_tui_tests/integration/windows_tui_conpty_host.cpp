// Windows-only, out-of-process ConPTY owner for the TUI lifecycle smoke test.
//
// ClosePseudoConsole has hung on older Windows builds.  Keeping HPCON in this
// helper makes that uninterruptible call killable by the test's outer job
// controller without compromising the test runner itself.

#include <windows.h>
#include <consoleapi3.h>

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

constexpr DWORD kExitOk = 0;
constexpr DWORD kExitUsage = 2;
constexpr DWORD kExitCreatePipe = 10;
constexpr DWORD kExitConPty = 11;
constexpr DWORD kExitAttributes = 12;
constexpr DWORD kExitLaunch = 13;
constexpr DWORD kExitOutput = 14;
constexpr DWORD kExitFirstFrame = 15;
constexpr DWORD kExitResize = 16;
constexpr DWORD kExitResizedFrame = 17;
constexpr DWORD kExitInput = 18;
constexpr DWORD kExitChildWait = 19;
constexpr DWORD kExitChildStatus = 20;
constexpr DWORD kExitOutputEof = 21;
constexpr DWORD kExitInternal = 22;
constexpr DWORD kExitCleanupEvidence = 23;
constexpr DWORD kExitCancellationHarness = 24;
constexpr DWORD kNoFailure = MAXDWORD;
constexpr short kResizeColumns = 121;
constexpr short kResizeRows = 37;
constexpr std::size_t kHorizontalBorderGlyphs =
    static_cast<std::size_t>(kResizeColumns) - 2U;
constexpr std::size_t kUtf8BoxGlyphBytes = 3U;
constexpr auto kHostDeadline = std::chrono::milliseconds(6'500);
static_assert(kHorizontalBorderGlyphs == 119U);

class Handle final {
  public:
    Handle() = default;
    explicit Handle(const HANDLE InValue) : value_(InValue) {}
    ~Handle() { Reset(); }
    Handle(const Handle&) = delete;
    auto operator=(const Handle&) -> Handle& = delete;
    Handle(Handle&& Other) noexcept : value_(Other.Release()) {}
    auto operator=(Handle&& Other) noexcept -> Handle& {
        if (this != &Other) Reset(Other.Release());
        return *this;
    }
    [[nodiscard]] auto Get() const -> HANDLE { return value_; }
    [[nodiscard]] auto Release() -> HANDLE {
        const HANDLE result = value_;
        value_ = nullptr;
        return result;
    }
    auto Reset(HANDLE InValue = nullptr) -> void {
        if (value_ != nullptr && value_ != INVALID_HANDLE_VALUE) {
            (void)CloseHandle(value_);
        }
        value_ = InValue;
    }

  private:
    HANDLE value_ = nullptr;
};

class Needle final {
  public:
    explicit Needle(std::string InNeedle) : needle_(std::move(InNeedle)) {
        failure_.assign(needle_.size(), 0U);
        for (std::size_t index = 1U, prefix = 0U; index < needle_.size(); ++index) {
            while (prefix > 0U && needle_[index] != needle_[prefix]) {
                prefix = failure_[prefix - 1U];
            }
            if (needle_[index] == needle_[prefix]) ++prefix;
            failure_[index] = prefix;
        }
    }
    auto Consume(const std::string_view InBytes) -> void {
        if (found_) return;
        for (const char value : InBytes) {
            while (matched_ > 0U && value != needle_[matched_]) {
                matched_ = failure_[matched_ - 1U];
            }
            if (value == needle_[matched_]) ++matched_;
            if (matched_ == needle_.size()) {
                found_ = true;
                return;
            }
        }
    }
    [[nodiscard]] auto Found() const -> bool { return found_; }

  private:
    std::string needle_;
    std::vector<std::size_t> failure_;
    std::size_t matched_ = 0U;
    bool found_ = false;
};

auto ResizedFrame() -> std::string {
    std::string value = "\xE2\x95\xAD"; // ╭
    for (short column = 2; column < kResizeColumns; ++column) {
        value += "\xE2\x94\x80"; // ─
    }
    value += "\xE2\x95\xAE"; // ╮
    return value;
}

auto Quote(const std::wstring& InValue) -> std::wstring {
    std::wstring value = L"\"";
    std::size_t slashes = 0U;
    for (const wchar_t character : InValue) {
        if (character == L'\\') {
            ++slashes;
        } else if (character == L'\"') {
            value.append(slashes * 2U + 1U, L'\\');
            value.push_back(character);
            slashes = 0U;
        } else {
            value.append(slashes, L'\\');
            value.push_back(character);
            slashes = 0U;
        }
    }
    value.append(slashes * 2U, L'\\');
    value.push_back(L'\"');
    return value;
}

[[nodiscard]] auto EnvironmentIsOne(const char* InName) -> bool {
    char value[3]{};
    return GetEnvironmentVariableA(
               InName, value, static_cast<DWORD>(sizeof(value))) == 1 &&
        value[0] == '1';
}

[[nodiscard]] auto TestModeEnabled() -> bool {
    return EnvironmentIsOne("KOG_TEST_MODE");
}

auto PrintResult(const bool InOk, const DWORD InCode, const DWORD InWin32,
                 const DWORD InChildExit, const bool InOutputEof) -> int {
    // Keep this record deliberately path-, argv-, and environment-free: callers
    // parse it after forwarding unbounded terminal bytes on stdout.
    std::fprintf(stderr,
        "KOG_CONPTY_HOST/v1 result=%s code=%lu win32=%lu child_exit=%lu output_eof=%u\n",
        InOk ? "ok" : "error", static_cast<unsigned long>(InCode),
        static_cast<unsigned long>(InWin32), static_cast<unsigned long>(InChildExit),
        InOutputEof ? 1U : 0U);
    std::fflush(stderr);
    return static_cast<int>(InCode);
}

auto Win32FromHresult(const HRESULT InResult) -> DWORD {
    return HRESULT_FACILITY(InResult) == FACILITY_WIN32
        ? HRESULT_CODE(InResult)
        : ERROR_GEN_FAILURE;
}

auto PrintBeforeCloseStage() -> void {
    std::fputs("KOG_CONPTY_HOST/v1 stage=before-close\n", stderr);
    std::fflush(stderr);
}

auto RemainingDeadlineMilliseconds(
    const std::chrono::steady_clock::time_point InDeadline) -> DWORD {
    const auto now = std::chrono::steady_clock::now();
    if (now >= InDeadline) return 0U;
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
        InDeadline - now).count();
    return static_cast<DWORD>(remaining > 0 ? remaining : 1);
}

} // namespace

auto wmain(const int InArgumentCount, wchar_t** InArguments) -> int {
    DWORD code = kExitUsage;
    DWORD win32 = ERROR_SUCCESS;
    DWORD childExit = STILL_ACTIVE;
    bool outputEof = false;
    bool closeReturned = false;
    if (InArgumentCount != 4 && InArgumentCount != 5) {
        return PrintResult(false, code, win32, childExit, outputEof);
    }
    const bool sendEscape = std::wcscmp(InArguments[1], L"escape") == 0;
    if (!sendEscape && std::wcscmp(InArguments[1], L"q") != 0) {
        return PrintResult(false, code, win32, childExit, outputEof);
    }
    const bool stallBeforeClose = InArgumentCount == 5 &&
        std::wcscmp(InArguments[4], L"--test-stall-before-close") == 0;
    if (InArgumentCount == 5 && !stallBeforeClose) {
        return PrintResult(false, code, win32, childExit, outputEof);
    }
    // This helper's causal exit proof depends on the deterministic production
    // cancellation acknowledgement.  Fail closed outside the explicit test
    // harness instead of weakening cleanup ordering to timing assumptions.
    if (!TestModeEnabled() ||
        !EnvironmentIsOne("KOG_TUI_TEST_STARTUP_CANCEL_ACK")) {
        return PrintResult(false, code, ERROR_BAD_ENVIRONMENT,
            childExit, outputEof);
    }
    if (InArguments[2][0] == L'\0' || InArguments[3][0] == L'\0') {
        return PrintResult(false, code, win32, childExit, outputEof);
    }
    code = kNoFailure;

    const std::string resizedNeedle = ResizedFrame();
    if (resizedNeedle.size() !=
        static_cast<std::size_t>(kResizeColumns) * kUtf8BoxGlyphBytes) {
        return PrintResult(false, kExitInternal, ERROR_INVALID_DATA,
            childExit, outputEof);
    }
    const auto deadline = std::chrono::steady_clock::now() + kHostDeadline;

    HANDLE rawInputRead = nullptr;
    HANDLE rawInputWrite = nullptr;
    if (!CreatePipe(&rawInputRead, &rawInputWrite, nullptr, 0)) {
        win32 = GetLastError();
        return PrintResult(false, kExitCreatePipe, win32, childExit, outputEof);
    }
    Handle inputRead(rawInputRead);
    Handle inputWrite(rawInputWrite);

    HANDLE rawOutputRead = nullptr;
    HANDLE rawOutputWrite = nullptr;
    if (!CreatePipe(&rawOutputRead, &rawOutputWrite, nullptr, 0)) {
        win32 = GetLastError();
        return PrintResult(false, kExitCreatePipe, win32, childExit, outputEof);
    }
    Handle outputRead(rawOutputRead);
    Handle outputWrite(rawOutputWrite);
    // The wrapper inherits no ambient handles.  Its only terminal attachment is
    // the explicitly supplied pseudoconsole attribute.
    if (!SetHandleInformation(inputWrite.Get(), HANDLE_FLAG_INHERIT, 0) ||
        !SetHandleInformation(outputRead.Get(), HANDLE_FLAG_INHERIT, 0)) {
        return PrintResult(false, kExitCreatePipe, GetLastError(), childExit, outputEof);
    }
    HPCON pseudoConsole = nullptr;
    const HRESULT conpty = CreatePseudoConsole(
        COORD{100, 30}, inputRead.Get(), outputWrite.Get(), 0, &pseudoConsole);
    if (FAILED(conpty)) {
        return PrintResult(false, kExitConPty, Win32FromHresult(conpty), childExit, outputEof);
    }
    inputRead.Reset();
    outputWrite.Reset();

    SIZE_T bytes = 0U;
    (void)InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
    std::vector<std::byte> attributes(bytes);
    auto* list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes.data());
    bool attributeInitialized = false;
    if (bytes != 0U) {
        attributeInitialized = InitializeProcThreadAttributeList(
            list, 1, 0, &bytes) != FALSE;
    }
    if (!attributeInitialized) {
        win32 = GetLastError();
        // Safe cleanup itself may hang on affected Windows releases.  In that
        // case the outer controller kills the job and maps the absent final
        // record to its stable hard-timeout result.
        ClosePseudoConsole(pseudoConsole);
        return PrintResult(false, kExitAttributes, win32, childExit, outputEof);
    }
    if (!UpdateProcThreadAttribute(list, 0, PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE,
            pseudoConsole, sizeof(pseudoConsole), nullptr, nullptr)) {
        win32 = GetLastError();
        DeleteProcThreadAttributeList(list);
        ClosePseudoConsole(pseudoConsole);
        return PrintResult(false, kExitAttributes, win32, childExit, outputEof);
    }
    struct AttributeCleanup final {
        LPPROC_THREAD_ATTRIBUTE_LIST list;
        bool initialized;
        ~AttributeCleanup() {
            if (initialized) DeleteProcThreadAttributeList(list);
        }
    } attributeCleanup{list, attributeInitialized};

    std::wstring command = Quote(InArguments[2]);
    command += L" ";
    command += Quote(InArguments[3]);
    command += L" --test-cancel-ack";
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.lpAttributeList = list;
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE,
            EXTENDED_STARTUPINFO_PRESENT, nullptr, nullptr, &startup.StartupInfo, &process)) {
        win32 = GetLastError();
        ClosePseudoConsole(pseudoConsole);
        return PrintResult(false, kExitLaunch, win32, childExit, outputEof);
    }
    Handle child(process.hProcess);
    Handle childThread(process.hThread);

    std::mutex mutex;
    std::condition_variable changed;
    Needle firstFrame("q/Escape exits");
    Needle resizedFrame(resizedNeedle);
    Needle altScreenExit("\x1b[?1049l");
    Needle terminalStateRestored("KOG_TUI_TERMINAL_STATE_RESTORED");
    Needle cancellationAcknowledgement("startup cancellation acknowledged");
    Needle cancellationHarnessArmed("startup cancellation harness armed");
    bool resizePending = false;
    bool resizeCommitted = false;
    bool inputPending = false;
    bool inputCommitted = false;
    bool outputComplete = false;
    std::chrono::steady_clock::time_point outputCompletedAt{};
    DWORD outputError = ERROR_SUCCESS;
    DWORD forwardError = ERROR_SUCCESS;
    std::thread pump([&] {
        std::array<char, 4096> buffer{};
        bool forwardEnabled = true;
        while (true) {
            DWORD count = 0;
            const BOOL read = ReadFile(outputRead.Get(), buffer.data(),
                static_cast<DWORD>(buffer.size()), &count, nullptr);
            if (!read || count == 0U) {
                const DWORD error = read ? ERROR_SUCCESS : GetLastError();
                std::scoped_lock lock(mutex);
                outputError = error;
                outputEof = read || error == ERROR_BROKEN_PIPE || error == ERROR_HANDLE_EOF;
                outputComplete = true;
                outputCompletedAt = std::chrono::steady_clock::now();
                changed.notify_all();
                return;
            }
            const std::string_view chunk(buffer.data(), count);
            std::size_t forwarded = 0U;
            while (forwardEnabled && forwarded < chunk.size()) {
                DWORD written = 0;
                const BOOL wrote = WriteFile(GetStdHandle(STD_OUTPUT_HANDLE),
                    chunk.data() + forwarded,
                    static_cast<DWORD>(chunk.size() - forwarded), &written,
                    nullptr);
                if (!wrote || written == 0U) {
                    const DWORD error = wrote ? ERROR_WRITE_FAULT : GetLastError();
                    {
                        std::scoped_lock lock(mutex);
                        if (forwardError == ERROR_SUCCESS) forwardError = error;
                    }
                    forwardEnabled = false;
                    break;
                }
                forwarded += written;
            }
            {
                std::scoped_lock lock(mutex);
                firstFrame.Consume(chunk);
                cancellationHarnessArmed.Consume(chunk);
                if (resizePending) resizedFrame.Consume(chunk);
                if (inputPending) {
                    for (const char byte : chunk) {
                        const std::string_view oneByte(&byte, 1U);
                        // Production emits the acknowledgement only after
                        // RequestTuiAsyncExit observes q/Escape.  Requiring it
                        // before terminal cleanup makes this stream causally
                        // post-input even if the reader held an older chunk.
                        if (!cancellationAcknowledgement.Found()) {
                            cancellationAcknowledgement.Consume(oneByte);
                        } else if (!altScreenExit.Found()) {
                            altScreenExit.Consume(oneByte);
                        } else {
                            terminalStateRestored.Consume(oneByte);
                        }
                    }
                }
            }
            changed.notify_all();
        }
    });

    {
        std::unique_lock lock(mutex);
        const bool observed = changed.wait_until(
            lock, deadline, [&] { return firstFrame.Found() || outputComplete; });
        if (!firstFrame.Found()) {
            code = kExitFirstFrame;
            win32 = !observed
                ? ERROR_TIMEOUT
                : (outputError == ERROR_SUCCESS ? ERROR_HANDLE_EOF : outputError);
        }
    }
    if (code == kNoFailure) {
        {
            std::scoped_lock lock(mutex);
            resizedFrame = Needle(resizedNeedle);
            resizePending = true;
            resizeCommitted = false;
        }
        const HRESULT resized = ResizePseudoConsole(
            pseudoConsole, COORD{kResizeColumns, kResizeRows});
        {
            std::scoped_lock lock(mutex);
            resizeCommitted = SUCCEEDED(resized);
            if (FAILED(resized)) resizePending = false;
        }
        changed.notify_all();
        if (FAILED(resized)) {
            code = kExitResize;
            win32 = Win32FromHresult(resized);
        }
    }
    if (code == kNoFailure) {
        std::unique_lock lock(mutex);
        const bool observed = changed.wait_until(lock, deadline, [&] {
            return (resizeCommitted && resizedFrame.Found()) || outputComplete;
        });
        if (!(resizeCommitted && resizedFrame.Found())) {
            code = kExitResizedFrame;
            win32 = !observed
                ? ERROR_TIMEOUT
                : (outputError == ERROR_SUCCESS ? ERROR_HANDLE_EOF : outputError);
        }
    }
    if (code == kNoFailure) {
        std::unique_lock lock(mutex);
        const bool observed = changed.wait_until(lock, deadline, [&] {
            return cancellationHarnessArmed.Found() || outputComplete;
        });
        if (!cancellationHarnessArmed.Found()) {
            code = kExitCancellationHarness;
            win32 = !observed
                ? ERROR_TIMEOUT
                : (outputError == ERROR_SUCCESS ? ERROR_HANDLE_EOF : outputError);
        }
    }
    if (code == kNoFailure) {
        const char input = sendEscape ? '\x1b' : 'q';
        DWORD written = 0;
        BOOL wrote = FALSE;
        DWORD inputError = ERROR_SUCCESS;
        {
            // Keep semantic consumption excluded until the one-byte input is
            // actually written.  The input pipe is host-owned, so this short
            // write has no dependency on the output pump or evidence mutex.
            std::scoped_lock lock(mutex);
            altScreenExit = Needle("\x1b[?1049l");
            terminalStateRestored = Needle(
                "KOG_TUI_TERMINAL_STATE_RESTORED");
            cancellationAcknowledgement = Needle(
                "startup cancellation acknowledged");
            inputPending = false;
            inputCommitted = false;
            wrote = WriteFile(
                inputWrite.Get(), &input, 1, &written, nullptr);
            if (wrote && written == 1U) {
                inputPending = true;
                inputCommitted = true;
            } else {
                inputError = wrote ? ERROR_WRITE_FAULT : GetLastError();
            }
        }
        changed.notify_all();
        if (!wrote || written != 1U) {
            code = kExitInput;
            win32 = inputError;
        }
    }
    if (code == kNoFailure) {
        const DWORD waited = WaitForSingleObject(
            child.Get(), RemainingDeadlineMilliseconds(deadline));
        if (waited != WAIT_OBJECT_0) {
            code = kExitChildWait;
            win32 = waited == WAIT_TIMEOUT ? ERROR_TIMEOUT : GetLastError();
        }
    }
    if (code == kNoFailure) {
        if (!GetExitCodeProcess(child.Get(), &childExit)) {
            code = kExitChildStatus;
            win32 = GetLastError();
        } else if (childExit != 0U) {
            code = kExitChildStatus;
        }
    }

    inputWrite.Reset();
    bool childStopped = childExit != STILL_ACTIVE;
    if (code != kNoFailure && !childStopped) {
        (void)TerminateProcess(child.Get(), kExitChildStatus);
        const DWORD waited = WaitForSingleObject(
            child.Get(), RemainingDeadlineMilliseconds(deadline));
        childStopped = waited == WAIT_OBJECT_0;
        if (childStopped) {
            (void)GetExitCodeProcess(child.Get(), &childExit);
        }
    }

    if (code == kNoFailure) {
        std::scoped_lock lock(mutex);
        if (forwardError != ERROR_SUCCESS) {
            code = kExitOutput;
            win32 = forwardError;
        }
    }

    bool cleanupEvidenceObserved = false;
    if (code == kNoFailure) {
        std::unique_lock lock(mutex);
        (void)changed.wait_until(lock, deadline, [&] {
            return (inputCommitted && cancellationAcknowledgement.Found() &&
                    altScreenExit.Found() && terminalStateRestored.Found()) ||
                outputComplete;
        });
        cleanupEvidenceObserved = inputCommitted &&
            cancellationAcknowledgement.Found() && altScreenExit.Found() &&
            terminalStateRestored.Found();
        if (!cleanupEvidenceObserved) {
            code = kExitCleanupEvidence;
            const bool deadlineExpired =
                std::chrono::steady_clock::now() >= deadline;
            win32 = deadlineExpired
                ? ERROR_TIMEOUT
                : (outputError == ERROR_SUCCESS ? ERROR_HANDLE_EOF : outputError);
        }
    }

    // This mode exists solely to exercise the parent controller's hard timeout.
    // It requires both an explicit argument and the test-only environment gate.
    if (stallBeforeClose && code == kNoFailure && childExit == 0U &&
        cleanupEvidenceObserved) {
        PrintBeforeCloseStage();
        (void)WaitForSingleObject(GetCurrentProcess(), INFINITE);
    }

    if (!childStopped) {
        // Never close a pseudoconsole around a process still known to be active.
        // The outer job controller remains the final hard bound if cancellation
        // of this synchronous reader cannot complete.
        (void)CancelSynchronousIo(pump.native_handle());
        if (pump.joinable()) pump.join();
        return PrintResult(false, code, win32, childExit, outputEof);
    }

    // Even safe cleanup can hang inside ClosePseudoConsole on affected Windows
    // releases.  A missing final result is deliberately mapped to hard-timeout
    // by the outer job controller, which then terminates this entire job.
    ClosePseudoConsole(pseudoConsole);
    closeReturned = true;
    bool outputCompletedByDeadline = false;
    {
        std::unique_lock lock(mutex);
        outputCompletedByDeadline = changed.wait_until(
            lock, deadline, [&] { return outputComplete; });
        outputCompletedByDeadline = outputCompletedByDeadline &&
            outputCompletedAt <= deadline;
        if (!outputCompletedByDeadline && code == kNoFailure) {
            code = kExitOutputEof;
            win32 = ERROR_TIMEOUT;
        }
    }
    if (!outputCompletedByDeadline) {
        (void)CancelSynchronousIo(pump.native_handle());
    }
    if (pump.joinable()) pump.join();
    if (forwardError != ERROR_SUCCESS && code == kNoFailure) {
        code = kExitOutput;
        win32 = forwardError;
    }
    if (!outputEof && code == kNoFailure) {
        code = kExitOutputEof;
        win32 = outputError;
    }
    if (code == kNoFailure && closeReturned && outputEof && childExit == 0U &&
        cleanupEvidenceObserved) {
        return PrintResult(true, kExitOk, ERROR_SUCCESS, childExit, outputEof);
    }
    return PrintResult(false, code, win32, childExit, outputEof);
}
