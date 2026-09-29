// Windows-only, out-of-process ConPTY owner for the TUI lifecycle smoke test.
//
// ClosePseudoConsole has hung on older Windows builds.  Keeping HPCON in this
// helper makes that uninterruptible call killable by the test's outer job
// controller without compromising the test runner itself.

#include <windows.h>
#include <consoleapi3.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <mutex>
#include <optional>
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
constexpr DWORD kExitNarrowResize = 16;
constexpr DWORD kExitNarrowFrame = 17;
constexpr DWORD kExitInput = 18;
constexpr DWORD kExitChildWait = 19;
constexpr DWORD kExitChildStatus = 20;
constexpr DWORD kExitOutputEof = 21;
constexpr DWORD kExitInternal = 22;
constexpr DWORD kExitCleanupEvidence = 23;
constexpr DWORD kExitCancellationHarness = 24;
constexpr DWORD kExitRestoreResize = 25;
constexpr DWORD kExitRestoredFrame = 26;
constexpr DWORD kExitCompactResize = 27;
constexpr DWORD kExitCompactFrame = 28;
constexpr DWORD kExitMinimumResize = 29;
constexpr DWORD kExitMinimumFrame = 30;
constexpr DWORD kExitInventoryFrame = 31;
constexpr DWORD kExitSelectedInput = 32;
constexpr DWORD kExitSelectedFrame = 33;
constexpr DWORD kExitAuditInput = 34;
constexpr DWORD kExitLinkedIdentityFrame = 35;
constexpr DWORD kExitPreviewCloseInput = 36;
constexpr DWORD kExitPreviewCloseFrame = 37;
constexpr DWORD kNoFailure = MAXDWORD;
constexpr short kInitialColumns = 120;
constexpr short kInitialRows = 36;
constexpr short kCompactColumns = 72;
constexpr short kCompactRows = 22;
constexpr short kNarrowColumns = 40;
constexpr short kNarrowRows = 12;
constexpr short kMinimumColumns = 24;
constexpr short kMinimumRows = 12;
constexpr short kRestoreColumns = 120;
constexpr short kRestoreRows = 36;
constexpr std::size_t kMaximumQaCaptureBytes = 16U << 20U;
constexpr std::size_t kUtf8BoxGlyphBytes = 3U;
constexpr auto kHostDeadline = std::chrono::milliseconds(6'500);
constexpr auto kIdentityHostDeadline = std::chrono::milliseconds(22'000);

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

class ResizeSemanticEvidence final {
  public:
    ResizeSemanticEvidence(std::string InResizeMarker,
                           std::string InAudit,
                           std::string InIdentity,
                           std::string InGuidance,
                           std::string InStatus)
        : resize_(std::move(InResizeMarker)),
          audit_(std::move(InAudit)),
          identity_(std::move(InIdentity)),
          guidance_(std::move(InGuidance)),
          status_(std::move(InStatus)) {}

    auto Consume(const std::string_view InBytes) -> void {
        for (const char byte : InBytes) {
            const std::string_view oneByte(&byte, 1U);
            resize_.Consume(oneByte);
            if (!resize_.Found()) {
                continue;
            }
            audit_.Consume(oneByte);
            identity_.Consume(oneByte);
            guidance_.Consume(oneByte);
            status_.Consume(oneByte);
        }
    }

    [[nodiscard]] auto Found() const -> bool {
        return resize_.Found() && audit_.Found() && identity_.Found() &&
            guidance_.Found() && status_.Found();
    }

  private:
    Needle resize_;
    Needle audit_;
    Needle identity_;
    Needle guidance_;
    Needle status_;
};

class FrameSemanticEvidence final {
  public:
    FrameSemanticEvidence() = default;
    explicit FrameSemanticEvidence(std::vector<std::string> InNeedles) {
        needles_.reserve(InNeedles.size());
        for (auto& needle : InNeedles) {
            needles_.emplace_back(std::move(needle));
        }
    }

    auto Consume(const std::string_view InBytes) -> void {
        for (auto& needle : needles_) {
            needle.Consume(InBytes);
        }
    }

    [[nodiscard]] auto Found() const -> bool {
        return !needles_.empty() &&
            std::all_of(
                needles_.begin(), needles_.end(),
                [](const Needle& InNeedle) { return InNeedle.Found(); });
    }

  private:
    std::vector<Needle> needles_;
};

auto TopBorderForColumns(const short InColumns) -> std::string {
    std::string value = "\xE2\x95\xAD"; // ╭
    for (short column = 2; column < InColumns; ++column) {
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

[[nodiscard]] auto QaCaptureDirectory()
    -> std::optional<std::filesystem::path> {
    if (!TestModeEnabled()) return std::nullopt;
    const DWORD required = GetEnvironmentVariableW(
        L"KOG_TUI_QA_CAPTURE_DIR", nullptr, 0U);
    if (required <= 1U || required > 32'768U) return std::nullopt;
    std::vector<wchar_t> buffer(required, L'\0');
    const DWORD written = GetEnvironmentVariableW(
        L"KOG_TUI_QA_CAPTURE_DIR", buffer.data(), required);
    if (written == 0U || written >= required) return std::nullopt;
    auto result = std::filesystem::path(
        std::wstring(buffer.data(), static_cast<std::size_t>(written)));
    if (result.empty()) return std::nullopt;
    return result.lexically_normal();
}

auto WriteQaCapture(
    const std::filesystem::path& InDirectory,
    const std::string_view InLabel,
    const std::string_view InBytes,
    const short InColumns,
    const short InRows,
    DWORD& OutError) -> bool {
    std::error_code directoryError;
    std::filesystem::create_directories(InDirectory, directoryError);
    if (directoryError) {
        OutError = static_cast<DWORD>(directoryError.value());
        return false;
    }
    auto stem = InDirectory / std::filesystem::u8path(InLabel);
    auto ansiPath = stem;
    ansiPath += ".ansi";
    std::ofstream ansi(
        ansiPath, std::ios::binary | std::ios::out | std::ios::trunc);
    if (!ansi) {
        OutError = ERROR_OPEN_FAILED;
        return false;
    }
    ansi.write(InBytes.data(), static_cast<std::streamsize>(InBytes.size()));
    if (!ansi) {
        OutError = ERROR_WRITE_FAULT;
        return false;
    }
    ansi.close();

    auto metadataPath = stem;
    metadataPath += ".json";
    std::ofstream metadata(
        metadataPath, std::ios::binary | std::ios::out | std::ios::trunc);
    if (!metadata) {
        OutError = ERROR_OPEN_FAILED;
        return false;
    }
    metadata << "{\"cols\":" << InColumns
             << ",\"rows\":" << InRows
             << ",\"ansi_bytes\":" << InBytes.size() << "}\n";
    if (!metadata) {
        OutError = ERROR_WRITE_FAULT;
        return false;
    }
    OutError = ERROR_SUCCESS;
    return true;
}

[[nodiscard]] auto BoundedAsciiArgument(
    const wchar_t* InValue,
    const std::size_t InMaximum) -> std::optional<std::string> {
    if (InValue == nullptr || InValue[0] == L'\0') return std::nullopt;
    std::string value;
    value.reserve(std::min<std::size_t>(InMaximum, 128U));
    for (const wchar_t character : std::wstring_view(InValue)) {
        if (character < 0x21 || character > 0x7e ||
            value.size() >= InMaximum) {
            return std::nullopt;
        }
        value.push_back(static_cast<char>(character));
    }
    return value;
}

[[nodiscard]] auto BoundedUtf8Argument(
    const wchar_t* InValue,
    const std::size_t InMaximumBytes) -> std::optional<std::string> {
    if (InValue == nullptr || InValue[0] == L'\0') return std::nullopt;
    const std::wstring_view input(InValue);
    if (input.size() > InMaximumBytes) return std::nullopt;
    const int required = WideCharToMultiByte(
        CP_UTF8,
        WC_ERR_INVALID_CHARS,
        input.data(),
        static_cast<int>(input.size()),
        nullptr,
        0,
        nullptr,
        nullptr);
    if (required <= 0 ||
        static_cast<std::size_t>(required) > InMaximumBytes) {
        return std::nullopt;
    }
    std::string value(static_cast<std::size_t>(required), '\0');
    if (WideCharToMultiByte(
            CP_UTF8, WC_ERR_INVALID_CHARS, input.data(),
            static_cast<int>(input.size()), value.data(), required,
            nullptr, nullptr) != required ||
        std::any_of(value.begin(), value.end(), [](const unsigned char InByte) {
            return InByte < 0x21U || InByte == 0x7fU;
        })) {
        return std::nullopt;
    }
    return value;
}

[[nodiscard]] auto IsSafePlanFileArgument(
    const std::string_view InValue) -> bool {
    return !InValue.empty() && InValue.size() <= 128U &&
        std::all_of(
            InValue.begin(), InValue.end(), [](const unsigned char InByte) {
                return (InByte >= 'a' && InByte <= 'z') ||
                    (InByte >= 'A' && InByte <= 'Z') ||
                    (InByte >= '0' && InByte <= '9') ||
                    InByte == '.' || InByte == '_' || InByte == '-';
            });
}

[[nodiscard]] auto IsHexReceipt(const std::string_view InValue) -> bool {
    return InValue.size() == 64U &&
        std::all_of(
            InValue.begin(), InValue.end(), [](const unsigned char InByte) {
                return (InByte >= '0' && InByte <= '9') ||
                    (InByte >= 'a' && InByte <= 'f') ||
                    (InByte >= 'A' && InByte <= 'F');
            });
}

auto WriteAll(const HANDLE InPipe, const std::string_view InBytes,
              DWORD& OutError) -> bool {
    std::size_t offset = 0U;
    while (offset < InBytes.size()) {
        DWORD written = 0U;
        const auto remaining = InBytes.size() - offset;
        const BOOL wrote = WriteFile(
            InPipe,
            InBytes.data() + offset,
            static_cast<DWORD>(std::min<std::size_t>(
                remaining, static_cast<std::size_t>(MAXDWORD))),
            &written,
            nullptr);
        if (!wrote || written == 0U) {
            OutError = wrote ? ERROR_WRITE_FAULT : GetLastError();
            return false;
        }
        offset += static_cast<std::size_t>(written);
    }
    return true;
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

// The host's diagnostic deadline is not a safety bound.  Preserve the ConPTY
// and the output pump until the outer controller's kill-on-close job reclaims
// the complete process tree.
[[noreturn]] auto StallForOuterController() -> void {
    for (;;) Sleep(INFINITE);
}

} // namespace

auto wmain(const int InArgumentCount, wchar_t** InArguments) -> int {
    DWORD code = kExitUsage;
    DWORD win32 = ERROR_SUCCESS;
    DWORD childExit = STILL_ACTIVE;
    bool outputEof = false;
    bool closeReturned = false;
    const bool identityMode = InArgumentCount == 10 &&
        std::wcscmp(InArguments[1], L"identity") == 0;
    if (!identityMode && InArgumentCount != 4 && InArgumentCount != 5) {
        return PrintResult(false, code, win32, childExit, outputEof);
    }
    const bool sendEscape = std::wcscmp(InArguments[1], L"escape") == 0;
    if (!identityMode && !sendEscape &&
        std::wcscmp(InArguments[1], L"q") != 0) {
        return PrintResult(false, code, win32, childExit, outputEof);
    }
    const bool stallBeforeClose = !identityMode && InArgumentCount == 5 &&
        std::wcscmp(InArguments[4], L"--test-stall-before-close") == 0;
    if (!identityMode && InArgumentCount == 5 && !stallBeforeClose) {
        return PrintResult(false, code, win32, childExit, outputEof);
    }
    // This helper's causal exit proof depends on the deterministic production
    // cancellation acknowledgement.  Fail closed outside the explicit test
    // harness instead of weakening cleanup ordering to timing assumptions.
    const bool cancellationHarness =
        EnvironmentIsOne("KOG_TUI_TEST_STARTUP_CANCEL_ACK");
    if (!TestModeEnabled() ||
        (!identityMode && !cancellationHarness) ||
        (identityMode && cancellationHarness)) {
        return PrintResult(false, code, ERROR_BAD_ENVIRONMENT,
            childExit, outputEof);
    }
    if (InArguments[2][0] == L'\0' || InArguments[3][0] == L'\0') {
        return PrintResult(false, code, win32, childExit, outputEof);
    }
    code = kNoFailure;

    std::optional<std::string> planFile;
    std::optional<std::string> repositoryToken;
    std::optional<std::string> runId;
    std::optional<std::string> receiptId;
    std::optional<std::string> compactRunId;
    std::optional<std::string> compactReceiptId;
    if (identityMode) {
        planFile = BoundedAsciiArgument(InArguments[4], 128U);
        repositoryToken = BoundedUtf8Argument(InArguments[5], 128U);
        runId = BoundedAsciiArgument(InArguments[6], 128U);
        receiptId = BoundedAsciiArgument(InArguments[7], 64U);
        compactRunId = BoundedAsciiArgument(InArguments[8], 10U);
        compactReceiptId = BoundedAsciiArgument(InArguments[9], 10U);
        if (!planFile.has_value() ||
            !repositoryToken.has_value() ||
            !runId.has_value() ||
            !receiptId.has_value() ||
            !compactRunId.has_value() ||
            !compactReceiptId.has_value() ||
            !IsSafePlanFileArgument(*planFile) ||
            !IsHexReceipt(*receiptId) ||
            compactRunId->size() != 10U ||
            compactReceiptId->size() != 10U) {
            return PrintResult(false, kExitUsage, ERROR_INVALID_DATA,
                childExit, outputEof);
        }
    }

    const std::string compactNeedle = TopBorderForColumns(kCompactColumns);
    const std::string narrowNeedle = TopBorderForColumns(kNarrowColumns);
    const std::string minimumNeedle = TopBorderForColumns(kMinimumColumns);
    const std::string restoredNeedle = TopBorderForColumns(kRestoreColumns);
    if (compactNeedle.size() !=
            static_cast<std::size_t>(kCompactColumns) * kUtf8BoxGlyphBytes ||
        narrowNeedle.size() !=
            static_cast<std::size_t>(kNarrowColumns) * kUtf8BoxGlyphBytes ||
        minimumNeedle.size() !=
            static_cast<std::size_t>(kMinimumColumns) * kUtf8BoxGlyphBytes ||
        restoredNeedle.size() !=
            static_cast<std::size_t>(kRestoreColumns) * kUtf8BoxGlyphBytes) {
        return PrintResult(false, kExitInternal, ERROR_INVALID_DATA,
            childExit, outputEof);
    }
    const auto deadline = std::chrono::steady_clock::now() +
        (identityMode ? kIdentityHostDeadline : kHostDeadline);
    const auto qaCaptureDirectory = identityMode
        ? QaCaptureDirectory() : std::nullopt;
    std::string qaCaptureBytes;
    bool qaCaptureOverflow = false;

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
    SECURITY_ATTRIBUTES inheritable{};
    inheritable.nLength = sizeof(inheritable);
    inheritable.bInheritHandle = TRUE;
    Handle cancellationArmed(CreateEventW(&inheritable, TRUE, FALSE, nullptr));
    Handle cancellationAcknowledged(CreateEventW(&inheritable, TRUE, FALSE, nullptr));
    if (cancellationArmed.Get() == nullptr ||
        cancellationAcknowledged.Get() == nullptr) {
        return PrintResult(false, kExitCancellationHarness, GetLastError(),
            childExit, outputEof);
    }
    // The wrapper inherits no ambient handles except the explicitly listed
    // test-only handshake events; its terminal attachment is the pseudoconsole.
    if (!SetHandleInformation(inputWrite.Get(), HANDLE_FLAG_INHERIT, 0) ||
        !SetHandleInformation(outputRead.Get(), HANDLE_FLAG_INHERIT, 0)) {
        return PrintResult(false, kExitCreatePipe, GetLastError(), childExit, outputEof);
    }
    HPCON pseudoConsole = nullptr;
    const HRESULT conpty = CreatePseudoConsole(
        COORD{kInitialColumns, kInitialRows}, inputRead.Get(), outputWrite.Get(), 0, &pseudoConsole);
    if (FAILED(conpty)) {
        return PrintResult(false, kExitConPty, Win32FromHresult(conpty), childExit, outputEof);
    }
    inputRead.Reset();
    outputWrite.Reset();

    const DWORD attributeCount = identityMode ? 1U : 2U;
    SIZE_T bytes = 0U;
    (void)InitializeProcThreadAttributeList(nullptr, attributeCount, 0, &bytes);
    std::vector<std::byte> attributes(bytes);
    auto* list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes.data());
    bool attributeInitialized = false;
    if (bytes != 0U) {
        attributeInitialized = InitializeProcThreadAttributeList(
            list, attributeCount, 0, &bytes) != FALSE;
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
    if (!identityMode) {
        HANDLE inheritedHandles[] = {cancellationArmed.Get(),
            cancellationAcknowledged.Get()};
        if (!UpdateProcThreadAttribute(list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                inheritedHandles, sizeof(inheritedHandles), nullptr, nullptr)) {
            win32 = GetLastError();
            DeleteProcThreadAttributeList(list);
            ClosePseudoConsole(pseudoConsole);
            return PrintResult(false, kExitAttributes, win32, childExit, outputEof);
        }
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
    if (identityMode) {
        command += L" --test-live-identity";
    } else {
        command += L" --test-cancel-ack";
        command += L" ";
        command += std::to_wstring(
            reinterpret_cast<std::uintptr_t>(cancellationArmed.Get()));
        command += L" ";
        command += std::to_wstring(
            reinterpret_cast<std::uintptr_t>(cancellationAcknowledged.Get()));
    }
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.lpAttributeList = list;
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, identityMode ? FALSE : TRUE,
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
    ResizeSemanticEvidence resizedFrame(
        narrowNeedle, "AUDIT", "receipt=missing", "q quit", "status=");
    FrameSemanticEvidence identityFrame(identityMode
        ? std::vector<std::string>{*repositoryToken}
        : std::vector<std::string>{});
    Needle altScreenExit("\x1b[?1049l");
    Needle terminalStateRestored("KOG_TUI_TERMINAL_STATE_RESTORED");
    bool resizePending = false;
    bool resizeCommitted = false;
    bool identityPending = identityMode;
    bool inputPending = false;
    bool inputCommitted = false;
    bool outputComplete = false;
    std::chrono::steady_clock::time_point lastOutputAt =
        std::chrono::steady_clock::now();
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
                if (qaCaptureDirectory.has_value() && !qaCaptureOverflow) {
                    if (qaCaptureBytes.size() + chunk.size() <=
                        kMaximumQaCaptureBytes) {
                        qaCaptureBytes.append(chunk);
                    } else {
                        qaCaptureOverflow = true;
                    }
                }
                lastOutputAt = std::chrono::steady_clock::now();
                firstFrame.Consume(chunk);
                if (resizePending) resizedFrame.Consume(chunk);
                if (identityPending) identityFrame.Consume(chunk);
                if (inputPending) {
                    for (const char byte : chunk) {
                        const std::string_view oneByte(&byte, 1U);
                        if (!altScreenExit.Found()) {
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
    const auto captureQaStage = [&]
        (const std::string_view InLabel,
         const short InColumns,
         const short InRows) {
        if (!identityMode || code != kNoFailure ||
            !qaCaptureDirectory.has_value()) {
            return;
        }
        std::string snapshot;
        {
            constexpr auto kQuietInterval = std::chrono::milliseconds(150);
            std::unique_lock lock(mutex);
            auto quietAt = lastOutputAt + kQuietInterval;
            while (std::chrono::steady_clock::now() < quietAt &&
                   std::chrono::steady_clock::now() < deadline) {
                (void)changed.wait_until(lock, std::min(quietAt, deadline));
                quietAt = lastOutputAt + kQuietInterval;
            }
            if (qaCaptureOverflow) {
                code = kExitInternal;
                win32 = ERROR_BUFFER_OVERFLOW;
                return;
            }
            if (std::chrono::steady_clock::now() < quietAt) {
                code = kExitInternal;
                win32 = ERROR_TIMEOUT;
                return;
            }
            snapshot = qaCaptureBytes;
        }
        DWORD captureError = ERROR_SUCCESS;
        if (!WriteQaCapture(*qaCaptureDirectory, InLabel, snapshot,
                InColumns, InRows, captureError)) {
            code = kExitInternal;
            win32 = captureError;
        }
    };
    const auto awaitIdentityFrame = [&](const DWORD InFrameFailure) {
        if (!identityMode || code != kNoFailure) return;
        std::unique_lock lock(mutex);
        const bool observed = changed.wait_until(lock, deadline, [&] {
            return identityFrame.Found() || outputComplete;
        });
        if (!identityFrame.Found()) {
            code = InFrameFailure;
            win32 = !observed
                ? ERROR_TIMEOUT
                : (outputError == ERROR_SUCCESS ? ERROR_HANDLE_EOF : outputError);
        }
        identityPending = false;
    };
    const auto writeIdentityInputAndAwait = [&]
        (const std::string_view InInput,
         std::vector<std::string> InNeedles,
         const DWORD InInputFailure,
         const DWORD InFrameFailure) {
        if (!identityMode || code != kNoFailure) return;
        {
            std::scoped_lock lock(mutex);
            identityFrame = FrameSemanticEvidence(std::move(InNeedles));
            identityPending = true;
        }
        DWORD inputError = ERROR_SUCCESS;
        if (!WriteAll(inputWrite.Get(), InInput, inputError)) {
            std::scoped_lock lock(mutex);
            identityPending = false;
            code = InInputFailure;
            win32 = inputError;
        }
        changed.notify_all();
        awaitIdentityFrame(InFrameFailure);
    };
    if (identityMode) {
        awaitIdentityFrame(kExitInventoryFrame);
        captureQaStage("01-inventory-120x36", kInitialColumns, kInitialRows);
        writeIdentityInputAndAwait(
            "s",
            {"repo=" + *repositoryToken},
            kExitSelectedInput,
            kExitSelectedFrame);
        captureQaStage("02-selected-120x36", kInitialColumns, kInitialRows);
        const std::string auditCommand =
            ":audit verify --plan-file " + *planFile +
            " --run-id " + *runId + " --attempt 1 --json\r";
        writeIdentityInputAndAwait(
            auditCommand,
            {"scope=workspace repo=repository-",
             "run=" + *compactRunId + " receipt=" + *compactReceiptId,
             "keys=Esc/q close"},
            kExitAuditInput,
            kExitLinkedIdentityFrame);
        captureQaStage("03-audit-120x36", kInitialColumns, kInitialRows);
    }
    const auto resizeAndAwait = [&](const short InColumns, const short InRows,
                                     const std::string& InMarker,
                                     const std::string& InAudit,
                                     const std::string& InIdentity,
                                     const std::string& InGuidance,
                                     const std::string& InStatus,
                                     const DWORD InResizeFailure,
                                     const DWORD InFrameFailure) {
        if (code != kNoFailure) return;
        {
            std::scoped_lock lock(mutex);
            resizedFrame = ResizeSemanticEvidence(
                InMarker, InAudit, InIdentity, InGuidance, InStatus);
            resizePending = true;
            resizeCommitted = false;
        }
        const HRESULT resized = ResizePseudoConsole(
            pseudoConsole, COORD{InColumns, InRows});
        {
            std::scoped_lock lock(mutex);
            resizeCommitted = SUCCEEDED(resized);
            if (FAILED(resized)) resizePending = false;
        }
        changed.notify_all();
        if (FAILED(resized)) {
            code = InResizeFailure;
            win32 = Win32FromHresult(resized);
        }
        if (code != kNoFailure) return;
        std::unique_lock lock(mutex);
        const bool observed = changed.wait_until(lock, deadline, [&] {
            return (resizeCommitted && resizedFrame.Found()) || outputComplete;
        });
        if (!(resizeCommitted && resizedFrame.Found())) {
            code = InFrameFailure;
            win32 = !observed
                ? ERROR_TIMEOUT
                : (outputError == ERROR_SUCCESS ? ERROR_HANDLE_EOF : outputError);
        }
    };
    const std::string compactMarker = "\x1b[8;22;72t";
    if (identityMode) {
        const std::string compactIdentity =
            "run=" + *compactRunId + " receipt=" + *compactReceiptId;
        resizeAndAwait(kCompactColumns, kCompactRows, compactMarker,
            "AUDIT", compactIdentity, "Esc/q close",
            "repo=repository-",
            kExitCompactResize, kExitCompactFrame);
        captureQaStage("04-audit-72x22", kCompactColumns, kCompactRows);
        resizeAndAwait(kNarrowColumns, kNarrowRows, narrowNeedle,
            "AUDIT", compactIdentity, "repo=repository-",
            "status=",
            kExitNarrowResize, kExitNarrowFrame);
        captureQaStage("05-audit-40x12", kNarrowColumns, kNarrowRows);
        resizeAndAwait(kMinimumColumns, kMinimumRows, minimumNeedle,
            "AUDIT verified", "receipt=linked", "run=" + *compactRunId,
            "keys=Esc/q close",
            kExitMinimumResize, kExitMinimumFrame);
        captureQaStage("06-audit-24x12", kMinimumColumns, kMinimumRows);
        resizeAndAwait(kRestoreColumns, kRestoreRows, restoredNeedle,
            "scope=workspace repo=repository-",
            compactIdentity, "Esc/q close", "status=",
            kExitRestoreResize, kExitRestoredFrame);
        captureQaStage("07-audit-restored-120x36",
            kRestoreColumns, kRestoreRows);
        writeIdentityInputAndAwait(
            "q",
            {"repo=" + *repositoryToken, "preview closed"},
            kExitPreviewCloseInput,
            kExitPreviewCloseFrame);
        captureQaStage("08-preview-closed-120x36",
            kRestoreColumns, kRestoreRows);
    } else {
        resizeAndAwait(kCompactColumns, kCompactRows, compactMarker,
            "AUDIT", "receipt=missing", "q quit", "status=",
            kExitCompactResize, kExitCompactFrame);
        resizeAndAwait(kNarrowColumns, kNarrowRows, narrowNeedle,
            "AUDIT", "receipt=missing", "q quit", "status=",
            kExitNarrowResize, kExitNarrowFrame);
        resizeAndAwait(kMinimumColumns, kMinimumRows, minimumNeedle,
            "AUDIT", "receipt=missing", "q quit", "status=",
            kExitMinimumResize, kExitMinimumFrame);
        resizeAndAwait(kRestoreColumns, kRestoreRows, restoredNeedle,
            "scope: workspace", "audit: receipt=missing", "exit: q quit", "inventory:",
            kExitRestoreResize, kExitRestoredFrame);
    }
    if (!identityMode && code == kNoFailure) {
        const DWORD armed = WaitForSingleObject(cancellationArmed.Get(),
            RemainingDeadlineMilliseconds(deadline));
        if (armed != WAIT_OBJECT_0) {
            code = kExitCancellationHarness;
            win32 = armed == WAIT_TIMEOUT ? ERROR_TIMEOUT : GetLastError();
        }
    }
    if (code == kNoFailure) {
        const char input = identityMode ? 'q' : (sendEscape ? '\x1b' : 'q');
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
    if (!identityMode && code == kNoFailure) {
        const DWORD acknowledged = WaitForSingleObject(cancellationAcknowledged.Get(),
            RemainingDeadlineMilliseconds(deadline));
        if (acknowledged != WAIT_OBJECT_0) {
            code = kExitCancellationHarness;
            win32 = acknowledged == WAIT_TIMEOUT ? ERROR_TIMEOUT : GetLastError();
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
    bool hostIssuedKill = false;
    if (code != kNoFailure && !childStopped) {
        hostIssuedKill = TerminateProcess(child.Get(), kExitChildStatus) != FALSE;
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
            return (inputCommitted && altScreenExit.Found() &&
                    terminalStateRestored.Found()) ||
                outputComplete;
        });
        cleanupEvidenceObserved = inputCommitted &&
            altScreenExit.Found() && terminalStateRestored.Found();
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

    if (hostIssuedKill || !childStopped) {
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
