// Windows-only fake git executable for the KOG-BUG-0107 q/Esc acceptance test.
//
// The production TUI spawns git through CreateProcessA.  CreateProcessA
// appends ".exe" to a bare "git" command name and does NOT consult
// PATHEXT, so a batch file named "git.cmd" on PATH is never used: the
// real git.exe always wins.  The acceptance chain under test is
//
//   production TUI first render
//   -> kick r
//   -> real owned fake-git launches
//   -> READY marker written by fake git itself
//   -> q / Esc
//   -> CancellationObserver
//   -> Job Object kills owned child tree
//   -> result = Cancelled
//   -> pipes drain
//   -> terminal restored
//
// so the fake git has to be a real .exe that the production TUI can
// actually spawn and own.
//
// This helper is copied to <fake-bin>/git.exe by the test.  It has two
// modes, selected by whether a gate file exists:
//
//   * gate absent -> delegate to the REAL git, so the production TUI's
//     startup inventory discovery still behaves normally and can reach
//     its "root repo ready" footer;
//   * gate present -> write the READY marker and block forever, so the
//     production TUI owns a genuinely running child process when the
//     ConPTY host sends q / Esc.
//
// The gate file path is "<marker path>.gate".  The ConPTY host creates
// it immediately before it writes the "r" kick byte, which is exactly
// when the refresh that owns the in-flight subprocess starts.
//
// Environment:
//   KOG_TEST_FAKE_GIT_MARKER  path of the READY marker file to write
//                              (required in gate mode)

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

constexpr char kMarkerEnvironment[] = "KOG_TEST_FAKE_GIT_MARKER";
constexpr char kGateSuffix[] = ".gate";
constexpr char kReadyPayload[] = "KOG_FAKE_GIT_READY\n";

auto ModuleDirectory() -> std::string {
    std::vector<char> buffer(32768);
    const DWORD length = GetModuleFileNameA(
        nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0U || length >= buffer.size()) {
        return {};
    }
    std::string path(buffer.data(), length);
    const auto separator = path.find_last_of("\\/");
    if (separator == std::string::npos) {
        return {};
    }
    return path.substr(0, separator);
}

auto Lowered(std::string value) -> std::string {
    for (auto& character : value) {
        if (character >= 'A' && character <= 'Z') {
            character = static_cast<char>(character - 'A' + 'a');
        }
    }
    return value;
}

auto EnvironmentPath() -> std::string {
    char buffer[32767]{};
    const DWORD length = GetEnvironmentVariableA(
        "PATH", buffer, static_cast<DWORD>(sizeof(buffer)));
    if (length == 0U || length >= sizeof(buffer)) {
        return {};
    }
    return std::string(buffer, length);
}

// Locate the real git.exe, skipping this helper's own directory so the
// fake never delegates to itself.
auto FindRealGit(const std::string& InOwnDirectory) -> std::string {
    const auto own = Lowered(InOwnDirectory);
    const auto path = EnvironmentPath();
    std::size_t start = 0U;
    while (start <= path.size()) {
        const auto end = path.find(';', start);
        const auto raw = path.substr(
            start, end == std::string::npos ? std::string::npos : end - start);
        if (!raw.empty()) {
            std::string directory = raw;
            while (!directory.empty() &&
                   (directory.back() == '\\' || directory.back() == '/')) {
                directory.pop_back();
            }
            if (Lowered(directory) != own) {
                const auto candidate = directory + "\\git.exe";
                if (GetFileAttributesA(candidate.c_str()) !=
                    INVALID_FILE_ATTRIBUTES) {
                    return candidate;
                }
            }
        }
        if (end == std::string::npos) {
            break;
        }
        start = end + 1U;
    }
    return {};
}

auto WriteReadyMarker(const std::string& InMarkerPath) -> bool {
    std::FILE* file = std::fopen(InMarkerPath.c_str(), "wb");
    if (file == nullptr) {
        return false;
    }
    const auto written = std::fwrite(kReadyPayload, 1U,
                                     sizeof(kReadyPayload) - 1U, file);
    std::fclose(file);
    return written == sizeof(kReadyPayload) - 1U;
}

// Block until the Job Object kills us.  The TUI's CancellationObserver
// is what makes that happen, and it is exactly what the acceptance
// chain needs to prove.
auto BlockForever() -> void {
    for (;;) {
        Sleep(INFINITE);
    }
}

auto SpawnRealGit(const std::string& InRealGit,
                  const std::vector<std::string>& InArguments) -> int {
    std::string commandLine = "\"" + InRealGit + "\"";
    for (const auto& argument : InArguments) {
        commandLine += " \"";
        commandLine += argument;
        commandLine += "\"";
    }
    std::vector<char> mutableCommand(commandLine.begin(), commandLine.end());
    mutableCommand.push_back('\0');
    STARTUPINFOA startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessA(
            nullptr, mutableCommand.data(), nullptr, nullptr, TRUE, 0U,
            nullptr, nullptr, &startup, &process)) {
        return 1;
    }
    CloseHandle(process.hThread);
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD exitCode = 1U;
    (void)GetExitCodeProcess(process.hProcess, &exitCode);
    CloseHandle(process.hProcess);
    return static_cast<int>(exitCode);
}

}  // namespace

auto wmain(int InArgumentCount, wchar_t** InArguments) -> int {
    char markerPath[32767]{};
    const DWORD markerLength = GetEnvironmentVariableA(
        kMarkerEnvironment, markerPath, static_cast<DWORD>(sizeof(markerPath)));
    if (markerLength == 0U || markerLength >= sizeof(markerPath)) {
        return 2;
    }
    const std::string marker(markerPath, markerLength);
    const std::string gatePath = marker + kGateSuffix;

    if (GetFileAttributesA(gatePath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        // Startup phase: behave exactly like the real git.
        const auto realGit = FindRealGit(ModuleDirectory());
        if (realGit.empty()) {
            return 3;
        }
        std::vector<std::string> narrowArguments;
        for (int index = 1; index < InArgumentCount; ++index) {
            const int length = WideCharToMultiByte(
                CP_UTF8, 0, InArguments[index], -1, nullptr, 0, nullptr, nullptr);
            if (length <= 0) {
                return 4;
            }
            std::string narrow(static_cast<std::size_t>(length), '\0');
            (void)WideCharToMultiByte(CP_UTF8, 0, InArguments[index], -1,
                                      narrow.data(), length, nullptr, nullptr);
            narrow.resize(static_cast<std::size_t>(length - 1));
            narrowArguments.push_back(std::move(narrow));
        }
        return SpawnRealGit(realGit, narrowArguments);
    }

    // Refresh phase: announce readiness and stay alive until the
    // production TUI's cancellation (or the Job Object) kills us.
    if (!WriteReadyMarker(marker)) {
        return 5;
    }
    BlockForever();
}
