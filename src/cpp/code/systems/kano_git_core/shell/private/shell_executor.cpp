// Shell script executor implementation
// Cross-platform process spawning for shell scripts
// Delegates to KanoInfra::process (kano_process_run_ex)

#include "shell_executor.hpp"
#include <kano_platform.h>
#include <kano_process.h>

#include <format>
#include <cstdlib>
#include <array>
#include <stdexcept>
#include <iostream>
#include <fstream>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <algorithm>
#include <cctype>
#include <charconv>
#include <limits>
#include <mutex>
#include <thread>
#include <string_view>
#include <cerrno>
#include <cstring>
#include <memory>
#include <array>

#ifdef KOG_PLATFORM_WINDOWS
#include <windows.h>
#else
#include <unistd.h>
#include <sys/wait.h>
#endif

namespace kano::git::shell {

namespace {

auto GetEnvPath(const char* InKey) -> std::optional<std::filesystem::path> {
    const char* raw = kano_platform_get_env(InKey);
    if (raw == nullptr || raw[0] == '\0') {
        return std::nullopt;
    }
    return std::filesystem::path(raw);
}

auto GetEnvTimeoutMs(const char* InKey) -> std::optional<DWORD> {
    int raw = 0;
    if (!kano_platform_get_env_int(InKey, &raw)) {
        return std::nullopt;
    }
    if (raw <= 0) {
        return std::nullopt;
    }
    return static_cast<DWORD>(raw);
}

}

auto GetScriptsDir() -> std::filesystem::path {
    namespace fs = std::filesystem;

    // Strategy: walk up from the executable to find src/shell/ directory
    // Binary is typically at src/cpp/build/<config>/kog-cli
    // Scripts are at ../../shell/ relative to the project root

    // 1. Try KANO_GIT_SCRIPTS_DIR env var first
    if (auto env = GetEnvPath("KANO_GIT_SCRIPTS_DIR")) {
        fs::path p = *env;
        if (fs::is_directory(p)) return p;
    }

    // 2. Try KANO_GIT_MASTER_ROOT env var
    if (auto env = GetEnvPath("KANO_GIT_MASTER_ROOT")) {
        fs::path p = *env / "src" / "shell";
        if (fs::is_directory(p)) return p;
    }

    // 3. Walk up from current working directory
    auto cwd = fs::current_path();
    for (auto dir = cwd; dir.has_parent_path() && dir != dir.root_path(); dir = dir.parent_path()) {
        auto candidate = dir / "src" / "shell";
        if (fs::is_directory(candidate) && fs::exists(candidate / "lib" / "git-helpers.sh")) {
            return candidate;
        }
    }

    // 4. Fallback: relative to cwd
    auto fallback = cwd / "src" / "shell";
    if (fs::is_directory(fallback)) return fallback;

    throw std::runtime_error(
        "Cannot locate src/shell/ directory. Set KANO_GIT_SCRIPTS_DIR or "
        "KANO_GIT_MASTER_ROOT environment variable, or run from the project root."
    );
}

#ifdef KOG_PLATFORM_WINDOWS

class ThreadSafeLogBuffer {
public:
    ThreadSafeLogBuffer() = default;

    void AppendStdout(std::string_view InChunk) {
        AppendLocked(stdoutBuffer_, InChunk);
    }

    void AppendStderr(std::string_view InChunk) {
        AppendLocked(stderrBuffer_, InChunk);
    }

    [[nodiscard]] auto Stdout() const -> std::string {
        std::scoped_lock lock(mu_);
        return stdoutBuffer_;
    }

    [[nodiscard]] auto Stderr() const -> std::string {
        std::scoped_lock lock(mu_);
        return stderrBuffer_;
    }

private:
    void AppendLocked(std::string& InTarget, std::string_view InChunk) {
        if (InChunk.empty()) {
            return;
        }
        std::scoped_lock lock(mu_);
        InTarget.append(InChunk.data(), InChunk.size());
    }

    mutable std::mutex mu_;
    std::string stdoutBuffer_;
    std::string stderrBuffer_;
};

auto ToLower(std::string in) -> std::string;
auto BaseNameLower(const std::string& InCommand) -> std::string;

auto ParseTimeoutMsRaw(const char* InValue) -> std::optional<DWORD> {
    if (InValue == nullptr) {
        return std::nullopt;
    }
    const std::string raw(InValue);
    if (raw.empty()) {
        return std::nullopt;
    }
    unsigned long long parsed = 0;
    const auto* begin = raw.data();
    const auto* end = raw.data() + raw.size();
    const auto [ptr, ec] = std::from_chars(begin, end, parsed);
    if (ec != std::errc() || ptr != end) {
        return std::nullopt;
    }
    if (parsed > static_cast<unsigned long long>((std::numeric_limits<DWORD>::max)())) {
        return (std::numeric_limits<DWORD>::max)();
    }
    return static_cast<DWORD>(parsed);
}

auto FirstGitSubcommand(const std::vector<std::string>& InArgs) -> std::string {
    for (std::size_t i = 0; i < InArgs.size(); ++i) {
        const auto token = InArgs[i];
        if (token.empty()) {
            continue;
        }
        if (token == "--") {
            if ((i + 1) < InArgs.size()) {
                return ToLower(InArgs[i + 1]);
            }
            break;
        }
        if (token == "-c" || token == "-C" || token == "--work-tree" || token == "--git-dir" || token == "--namespace") {
            i += 1;
            continue;
        }
        if (token.rfind("-", 0) == 0) {
            continue;
        }
        return ToLower(token);
    }
    return {};
}

auto IsLongRunningGitOperation(const std::string& InCommand,
                               const std::vector<std::string>& InArgs) -> bool {
    const auto base = BaseNameLower(InCommand);
    if (base != "git" && base != "git.exe") {
        return false;
    }

    const auto sub = FirstGitSubcommand(InArgs);
    return sub == "fetch" ||
           sub == "pull" ||
           sub == "clone" ||
           sub == "submodule" ||
           sub == "lfs";
}

auto NormalizeTimeoutOverride(const std::optional<DWORD>& InValue) -> std::optional<DWORD> {
    if (!InValue.has_value()) {
        return std::nullopt;
    }
    if (*InValue == 0) {
        return std::nullopt;
    }
    return InValue;
}

auto ResolveTimeoutMs(const std::string& InCommand,
                      const std::vector<std::string>& InArgs,
                      const ExecMode InMode) -> std::optional<DWORD> {
    if (const auto timeoutRaw = GetEnvTimeoutMs("KOG_SHELL_TIMEOUT_MS"); timeoutRaw.has_value()) {
        return NormalizeTimeoutOverride(timeoutRaw);
    }

    if (InMode == ExecMode::Capture) {
        if (const auto timeoutRaw = GetEnvTimeoutMs("KOG_SHELL_CAPTURE_TIMEOUT_MS"); timeoutRaw.has_value()) {
            return NormalizeTimeoutOverride(timeoutRaw);
        }
        if (IsLongRunningGitOperation(InCommand, InArgs)) {
            return std::nullopt;
        }
        // Capture path keeps a default safety timeout, but large enough for most operations.
        return static_cast<DWORD>(30 * 60 * 1000);
    }
    // PassThrough: default no timeout.
    return std::nullopt;
}

auto IsProcessDiagnosticsEnabled() -> bool {
    if (const auto* raw = std::getenv("KOG_PROCESS_DIAGNOSTICS"); raw != nullptr) {
        const auto value = ToLower(std::string(raw));
        return value == "1" || value == "true" || value == "yes" || value == "on";
    }
    return false;
}

auto CurrentUtcTimestamp() -> std::string {
    const auto now = std::chrono::system_clock::now();
    const auto tt = std::chrono::system_clock::to_time_t(now);
    std::tm utc{};
#if defined(_WIN32)
    gmtime_s(&utc, &tt);
#else
    gmtime_r(&tt, &utc);
#endif
    std::ostringstream oss;
    oss << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ");
    return oss.str();
}

auto JoinArgs(const std::vector<std::string>& InArgs) -> std::string {
    std::ostringstream oss;
    for (std::size_t i = 0; i < InArgs.size(); ++i) {
        if (i > 0) {
            oss << ' ';
        }
        oss << InArgs[i];
    }
    return oss.str();
}

auto BuildProcessDiagBlock(const std::string& InStartTs,
                           const std::string& InEndTs,
                           const std::filesystem::path& InCwd,
                           const std::string& InExecutable,
                           const std::vector<std::string>& InArgv,
                           const int InExitCode,
                           const bool InTimedOut) -> std::string {
    std::ostringstream oss;
    const auto commandLine = InArgv.empty() ? InExecutable : (InExecutable + " " + JoinArgs(InArgv));
    const auto pathRaw = std::getenv("PATH");

    oss << "[process-diag] cwd=" << InCwd.generic_string() << "\n";
    oss << "[process-diag] executable=" << InExecutable << "\n";
    oss << "[process-diag] argv=" << JoinArgs(InArgv) << "\n";
    oss << "[process-diag] selected_PATH=" << (pathRaw != nullptr ? pathRaw : "") << "\n";
    oss << "[process-diag] start_ts=" << InStartTs << "\n";
    oss << "[process-diag] end_ts=" << InEndTs << "\n";
    oss << "[process-diag] exit_code=" << InExitCode << "\n";
    if (InTimedOut) {
        oss << "[process-diag] timeout_kill_marker=1\n";
        oss << "[process-diag] last_running_child_command=" << commandLine << "\n";
    }

    static constexpr std::array<const char*, 10> kRelevantEnvKeys = {
        "KOG_GIT_INTERACTIVE",
        "GIT_TERMINAL_PROMPT",
        "GCM_INTERACTIVE",
        "GIT_ASKPASS",
        "SSH_ASKPASS",
        "KANO_GIT_SKILL_ROOT",
        "KOG_PROCESS_DIAGNOSTICS",
        "KOG_PROCESS_DIAGNOSTICS_LOG",
        "KOG_TEST_AI_STDOUT",
        "KOG_TEST_AI_EXIT_CODE",
    };
    for (const auto* key : kRelevantEnvKeys) {
        if (const auto* value = std::getenv(key); value != nullptr) {
            oss << "[process-diag] env_" << key << "=" << value << "\n";
        }
    }

    return oss.str();
}

auto EmitProcessDiag(const std::string& InText) -> void {
    std::cerr << InText;
    if (const auto* pathRaw = std::getenv("KOG_PROCESS_DIAGNOSTICS_LOG"); pathRaw != nullptr && pathRaw[0] != '\0') {
        std::error_code ec;
        const std::filesystem::path path(pathRaw);
        std::filesystem::create_directories(path.parent_path(), ec);
        std::ofstream ofs(path, std::ios::app);
        if (ofs) {
            ofs << InText;
        }
    }
}

auto RunProcess(const std::string& cmdLine, ExecMode InMode,
                const std::optional<unsigned int>& InTimeoutMs,
                const std::optional<std::filesystem::path>& InWorkingDir,
                ProgressCallback InProgressCallback) -> ExecResult
{
    // Convert cmdLine narrow string to wide for CommandLineToArgvW
    int wideLen = ::MultiByteToWideChar(CP_UTF8, 0, cmdLine.c_str(), -1, nullptr, 0);
    if (wideLen == 0) {
        return ExecResult{-1, {}, "Failed to convert command line to wide char"};
    }
    std::vector<wchar_t> wideCmd(wideLen);
    if (::MultiByteToWideChar(CP_UTF8, 0, cmdLine.c_str(), -1, wideCmd.data(), wideLen) == 0) {
        return ExecResult{-1, {}, "Failed to convert command line to wide char"};
    }

    // Parse wide cmdLine into argv
    LPWSTR* wideArgv = nullptr;
    int argc = 0;
    wideArgv = ::CommandLineToArgvW(wideCmd.data(), &argc);

    if (!wideArgv) {
        return ExecResult{-1, {}, "Failed to parse command line"};
    }

    int argcOut = 0;
    for (wchar_t** p = wideArgv; *p; ++p) {
        ++argcOut;
    }

    // Convert wide argv to narrow UTF-8 for kano_process
    std::vector<std::string> narrowArgs;
    narrowArgs.reserve(static_cast<std::size_t>(argcOut));
    for (int i = 0; i < argcOut; ++i) {
        int needed = ::WideCharToMultiByte(CP_UTF8, 0, wideArgv[i], -1, nullptr, 0, nullptr, nullptr);
        if (needed > 0) {
            std::string narrow(static_cast<std::size_t>(needed - 1), '\0');
            ::WideCharToMultiByte(CP_UTF8, 0, wideArgv[i], -1, &narrow[0], needed, nullptr, nullptr);
            narrowArgs.push_back(std::move(narrow));
        }
    }

    if (narrowArgs.empty()) {
        return ExecResult{-1, {}, "Empty command line"};
    }

    const char* executable = narrowArgs[0].c_str();
    std::vector<const char*> argv;
    argv.reserve(narrowArgs.size() + 1);
    for (const auto& arg : narrowArgs) {
        argv.push_back(arg.c_str());
    }
    argv.push_back(nullptr);

    std::string workingDirStorage;
    if (InWorkingDir.has_value()) {
        auto preferred = InWorkingDir->lexically_normal();
        preferred.make_preferred();
        workingDirStorage = preferred.string();
    }

    KanoProcessOptions opts{};
    opts.executable = executable;
    opts.working_dir = workingDirStorage.empty() ? nullptr : workingDirStorage.c_str();
    opts.argv = argv.data();
    opts.argv_count = argv.size() - 1;  // exclude terminating nullptr
    opts.mode = (InMode == ExecMode::Capture) ? KANO_PROCESS_MODE_CAPTURE : KANO_PROCESS_MODE_PASS_THROUGH;
    opts.timeout_ms = InTimeoutMs ? static_cast<int>(*InTimeoutMs) : 0;
    opts.output_callback = InProgressCallback ? [](KanoProcessStream stream, const char* chunk, size_t chunk_size, void* user_data) {
        reinterpret_cast<ProgressCallback*>(user_data)->operator()(
            std::string_view(chunk, chunk_size), stream == KANO_PROCESS_STREAM_STDERR);
    } : nullptr;
    opts.user_data = InProgressCallback ? reinterpret_cast<void*>(&InProgressCallback) : nullptr;

    KanoProcessResult kresult{};
    bool ok = kano_process_run_ex(&opts, &kresult);

    if (!ok) {
        return ExecResult{-1, {}, "kano_process_run_ex failed"};
    }

    ExecResult result;
    result.exitCode = kresult.exit_code;
    if (kresult.stdout_data) {
        result.stdoutStr = kresult.stdout_data;
    }
    if (kresult.stderr_data) {
        result.stderrStr = kresult.stderr_data;
    }
    if (kresult.timed_out) {
        if (!result.stderrStr.empty()) result.stderrStr += "\n";
        result.stderrStr += "Process timeout exceeded";
    }
    kano_process_free_result(&kresult);
    return result;
}

auto RunProcess(const std::string& cmdLine, ExecMode InMode,
                const std::optional<unsigned int>& InTimeoutMs,
                const std::optional<std::filesystem::path>& InWorkingDir) -> ExecResult
{
    return RunProcess(cmdLine, InMode, InTimeoutMs, InWorkingDir, ProgressCallback{});
}

#else  // Unix

auto RunProcessUnix(const std::string& InCommand,
                    const std::vector<std::string>& InArgs,
                    ExecMode InMode,
                    const std::optional<unsigned int>& InTimeoutMs,
                    const std::optional<std::filesystem::path>& InWorkingDir,
                    ProgressCallback InProgressCallback) -> ExecResult {
    // Build argv array: [InCommand, InArgs...]
    std::vector<const char*> argv;
    argv.reserve(InArgs.size() + 2);
    argv.push_back(InCommand.c_str());
    for (const auto& arg : InArgs) {
        argv.push_back(arg.c_str());
    }
    argv.push_back(nullptr);

    KanoProcessOptions opts{};
    opts.executable = InCommand.c_str();
    opts.working_dir = InWorkingDir ? InWorkingDir->string().c_str() : nullptr;
    opts.argv = argv.data();
    opts.argv_count = argv.size() - 1;  // exclude terminating nullptr
    opts.mode = (InMode == ExecMode::Capture) ? KANO_PROCESS_MODE_CAPTURE : KANO_PROCESS_MODE_PASS_THROUGH;
    opts.timeout_ms = InTimeoutMs ? static_cast<int>(*InTimeoutMs) : 0;
    opts.output_callback = InProgressCallback ? [](KanoProcessStream stream, const char* chunk, size_t chunk_size, void* user_data) {
        reinterpret_cast<ProgressCallback*>(user_data)->operator()(
            std::string_view(chunk, chunk_size), stream == KANO_PROCESS_STREAM_STDERR);
    } : nullptr;
    opts.user_data = InProgressCallback ? reinterpret_cast<void*>(&InProgressCallback) : nullptr;

    KanoProcessResult kresult{};
    bool ok = kano_process_run_ex(&opts, &kresult);

    if (!ok) {
        return ExecResult{-1, {}, "kano_process_run_ex failed"};
    }

    ExecResult result;
    result.exitCode = kresult.exit_code;
    if (kresult.stdout_data) {
        result.stdoutStr = kresult.stdout_data;
    }
    if (kresult.stderr_data) {
        result.stderrStr = kresult.stderr_data;
    }
    if (kresult.timed_out) {
        if (!result.stderrStr.empty()) result.stderrStr += "\n";
        result.stderrStr += "Process timed out";
    }
    kano_process_free_result(&kresult);
    return result;
}

auto RunProcessUnix(const std::string& InCommand,
                    const std::vector<std::string>& InArgs,
                    ExecMode InMode,
                    const std::optional<unsigned int>& InTimeoutMs,
                    const std::optional<std::filesystem::path>& InWorkingDir) -> ExecResult {
    return RunProcessUnix(InCommand, InArgs, InMode, InTimeoutMs, InWorkingDir, ProgressCallback{});
}

#endif

auto QuoteWindowsArg(const std::string& InArg) -> std::string {
    if (InArg.empty()) {
        return "\"\"";
    }

    const bool needsQuotes = std::any_of(InArg.begin(), InArg.end(), [](char ch) {
        return std::isspace(static_cast<unsigned char>(ch)) || ch == '"';
    });
    if (!needsQuotes) {
        return InArg;
    }

    std::string out;
    out.reserve(InArg.size() + 8);
    out.push_back('"');

    std::size_t backslashCount = 0;
    for (const char ch : InArg) {
        if (ch == '\\') {
            backslashCount += 1;
            continue;
        }
        if (ch == '"') {
            out.append(backslashCount * 2 + 1, '\\');
            out.push_back('"');
            backslashCount = 0;
            continue;
        }
        if (backslashCount > 0) {
            out.append(backslashCount, '\\');
            backslashCount = 0;
        }
        out.push_back(ch);
    }

    if (backslashCount > 0) {
        out.append(backslashCount * 2, '\\');
    }
    out.push_back('"');
    return out;
}

auto BuildCommandLine(const std::string& program, const std::vector<std::string>& InArgs) -> std::string {
#ifdef KOG_PLATFORM_WINDOWS
    std::string cmd = QuoteWindowsArg(program);
    for (const auto& arg : InArgs) {
        cmd += " ";
        cmd += QuoteWindowsArg(arg);
    }
    return cmd;
#else
    std::string cmd = program;
    for (const auto& arg : InArgs) {
        // Simple quoting — handles most cases
        if (arg.find(' ') != std::string::npos || arg.find('"') != std::string::npos) {
            cmd += " \"" + arg + "\"";
        } else {
            cmd += " " + arg;
        }
    }
    return cmd;
#endif
}

auto BuildWindowsCommandProcessorLine(const std::vector<std::string>& InArgs) -> std::string {
    std::string cmd = "cmd.exe";
    for (const auto& arg : InArgs) {
        cmd += ' ';
        if (!arg.empty() && (arg[0] == '/' || arg[0] == '-')) {
            cmd += arg;
        } else {
            cmd += QuoteWindowsArg(arg);
        }
    }
    return cmd;
}

auto BuildWindowsBatchCommandLine(const std::string& InCommand,
                                  const std::vector<std::string>& InArgs) -> std::string {
    std::vector<std::string> wrappedArgs{"/d", "/s", "/c", BuildCommandLine(InCommand, InArgs)};
    return BuildWindowsCommandProcessorLine(wrappedArgs);
}

auto ToLower(std::string in) -> std::string {
    std::transform(in.begin(), in.end(), in.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return in;
}

auto BaseNameLower(const std::string& InCommand) -> std::string {
    auto pos = InCommand.find_last_of("/\\");
    const std::string base = (pos == std::string::npos) ? InCommand : InCommand.substr(pos + 1);
    return ToLower(base);
}

auto NormalizeWindowsExecutable(const std::string& InCommand) -> std::string {
    const auto base = BaseNameLower(InCommand);
    if (base == "cmd") {
        return "cmd.exe";
    }
    if (base == "powershell") {
        return "powershell.exe";
    }
    return InCommand;
}

auto IsWindowsCommandProcessor(const std::string& InCommand) -> bool {
    const auto base = BaseNameLower(InCommand);
    return base == "cmd" || base == "cmd.exe";
}

auto IsCmdScriptCommand(const std::string& InCommand) -> bool {
    const auto base = BaseNameLower(InCommand);
    return (base.size() > 4 && base.ends_with(".cmd")) || (base.size() > 4 && base.ends_with(".bat"));
}

auto WithGitNonInteractiveDefaults(const std::string& InCommand,
                                   const std::vector<std::string>& InArgs) -> std::vector<std::string> {
    auto args = InArgs;
    if (BaseNameLower(InCommand) != "git" && BaseNameLower(InCommand) != "git.exe") {
        return args;
    }

    // Interactive switch:
    // - KOG_GIT_INTERACTIVE=1|true   => interactive
    // - KOG_GIT_INTERACTIVE=0|false  => non-interactive
    // - KOG_GIT_INTERACTIVE=auto/unset => agent mode non-interactive, human mode interactive
    bool forceNonInteractive = false;
    if (const auto* interactive = std::getenv("KOG_GIT_INTERACTIVE"); interactive != nullptr) {
        const auto value = ToLower(std::string(interactive));
        if (value == "1" || value == "true") {
            return args;
        }
        if (value == "0" || value == "false") {
            forceNonInteractive = true;
        } else {
            const auto* agent = std::getenv("KANO_AGENT_MODE");
            if (agent != nullptr) {
                const auto agentValue = ToLower(std::string(agent));
                forceNonInteractive = (agentValue == "1" || agentValue == "true");
            }
        }
    } else {
        const auto* agent = std::getenv("KANO_AGENT_MODE");
        if (agent != nullptr) {
            const auto agentValue = ToLower(std::string(agent));
            forceNonInteractive = (agentValue == "1" || agentValue == "true");
        }
    }

    if (!forceNonInteractive) {
        return args;
    }

    // Force non-interactive git behavior in automation:
    // - no editor popup for rebase/cherry-pick/commit continue
    // - no terminal credential prompt
    // Keep these as command-scoped `-c` to avoid global env side effects in parallel jobs.
    std::vector<std::string> prefixed{
        "-c", "core.editor=true",
        "-c", "sequence.editor=true",
        "-c", "credential.interactive=false",
        "-c", "advice.waitingForEditor=false"
    };
    prefixed.insert(prefixed.end(), args.begin(), args.end());
    return prefixed;
}

} // anonymous namespace

namespace kano::git::shell {

auto ExecuteScript(
    std::string_view InRelativeScript,
    const std::vector<std::string>& InArgs,
    ExecMode InMode,
    std::optional<std::filesystem::path> InWorkingDir
) -> ExecResult
{
    (void)InArgs;
    (void)InMode;
    (void)InWorkingDir;

    return ExecResult{
        .exitCode = 78,
        .stderrStr = std::format(
            "Shell execution is disabled in strict native-only mode: {}",
            std::string(InRelativeScript))
    };
}

auto ExecuteCommand(
    const std::string& InCommand,
    const std::vector<std::string>& InArgs,
    ExecMode InMode,
    std::optional<std::filesystem::path> InWorkingDir
) -> ExecResult
{
    return ExecuteCommand(InCommand, InArgs, InMode, InWorkingDir, ProgressCallback{});
}

auto ExecuteCommand(
    const std::string& InCommand,
    const std::vector<std::string>& InArgs,
    ExecMode InMode,
    std::optional<std::filesystem::path> InWorkingDir,
    ProgressCallback InProgressCallback
) -> ExecResult
{
    const auto effectiveArgs = WithGitNonInteractiveDefaults(InCommand, InArgs);
    const bool processDiagEnabled = IsProcessDiagnosticsEnabled();
    const auto processDiagStartTs = processDiagEnabled ? CurrentUtcTimestamp() : std::string{};
    const auto processDiagCwd = InWorkingDir.has_value() ? InWorkingDir->lexically_normal() : std::filesystem::current_path();

    // Global command logging
    static const bool logEnabled = std::getenv("KOG_LOG_COMMANDS") != nullptr || 
                                   std::getenv("KANO_AGENT_MODE") != nullptr ||
                                   std::getenv("KOG_DEBUG") != nullptr;
    
    if (logEnabled) {
        static const bool verbose = std::getenv("KOG_VERBOSE") != nullptr;
        
        // Identify noisy commands
        bool isNoisy = false;
        if (BaseNameLower(InCommand) == "git" || BaseNameLower(InCommand) == "git.exe") {
            const std::string sub = FirstGitSubcommand(InArgs);
            if (sub == "rev-parse") {
                for (const auto& arg : InArgs) {
                    if (arg == "--is-inside-work-tree" || arg == "--show-toplevel" || arg == "HEAD") {
                        isNoisy = true;
                        break;
                    }
                }
            } else if (sub == "check-ignore") {
                isNoisy = true;
            } else if (sub == "config") {
                for (const auto& arg : InArgs) {
                    if (arg == "kano.cache.local-dir" || arg == ".gitmodules" || arg == "--file") {
                        isNoisy = true;
                        break;
                    }
                }
            } else if (sub == "remote") {
                isNoisy = true; // remote get-url etc
            }
        }

        if (verbose || !isNoisy) {
            std::string logLine = "[run] " + BuildCommandLine(InCommand, InArgs);
            if (InWorkingDir) {
                logLine += " (cwd: " + InWorkingDir->generic_string() + ")";
            }
            
            // Console output
            std::cout << logLine << std::endl;

            // File output if KOG_DEBUG is on
            static const bool debugFile = std::getenv("KOG_DEBUG") != nullptr;
            if (debugFile) {
                // Determine log file path: we look for .kano/tmp/git relative to CWD or workspace
                static std::filesystem::path logPath;
                static std::once_flag logPathOnce;
                std::call_once(logPathOnce, [&]() {
                    // Generate timestamp: YYYYMMDDTHHMMSS
                    auto now = std::chrono::system_clock::now();
                    auto in_time_t = std::chrono::system_clock::to_time_t(now);
                    std::stringstream ss;
                    ss << std::put_time(std::gmtime(&in_time_t), "%Y%m%dT%H%M%SZ");
                    std::string timestamp = ss.str();

                    // Try to find .kano directory by walking up
                    auto search = std::filesystem::current_path();
                    while (search.has_parent_path() && search != search.root_path()) {
                        if (std::filesystem::exists(search / ".kano")) {
                            logPath = search / ".kano" / "tmp" / "git" / "log" / ("execute-" + timestamp + ".log");
                            break;
                        }
                        search = search.parent_path();
                    }
                    if (logPath.empty()) {
                        logPath = std::filesystem::temp_directory_path() / ("kog-execute-" + timestamp + ".log");
                    }
                    
                    // Ensure directory exists
                    std::error_code ec;
                    std::filesystem::create_directories(logPath.parent_path(), ec);
                    
                    std::ofstream ofs(logPath, std::ios::app);
                    if (ofs) {
                        ofs << "--- Session Started: " << std::filesystem::current_path().string() << " ---\n";
                    }
                    
                    std::cout << "[debug] commands logged to: " << logPath.generic_string() << std::endl;
                });

                if (!logPath.empty()) {
                    static std::mutex logMu;
                    std::lock_guard<std::mutex> lock(logMu);
                    std::ofstream ofs(logPath, std::ios::app);
                    if (ofs) {
                        ofs << logLine << std::endl;
                    }
                }
            }
        }
    }

#ifdef KOG_PLATFORM_WINDOWS
    const auto effectiveCommand = NormalizeWindowsExecutable(InCommand);
    const auto timeoutMs = ResolveTimeoutMs(effectiveCommand, effectiveArgs, InMode);
    ExecResult result;
    if (IsWindowsCommandProcessor(effectiveCommand) && !effectiveArgs.empty()) {
        std::vector<std::string> wrappedArgs{"/d", "/s"};
        wrappedArgs.insert(wrappedArgs.end(), effectiveArgs.begin(), effectiveArgs.end());
        const auto wrapped = BuildWindowsCommandProcessorLine(wrappedArgs);
        result = RunProcess(wrapped, InMode, timeoutMs, InWorkingDir, InProgressCallback);
    } else if (IsCmdScriptCommand(effectiveCommand)) {
        const auto wrapped = BuildWindowsBatchCommandLine(effectiveCommand, effectiveArgs);
        result = RunProcess(wrapped, InMode, timeoutMs, InWorkingDir, InProgressCallback);
    } else {
        // Build command line with executable at start (for CreateProcessA parsing)
        auto cmd = BuildCommandLine(effectiveCommand, effectiveArgs);
        result = RunProcess(cmd, InMode, timeoutMs, InWorkingDir, InProgressCallback);
    }

    if (processDiagEnabled) {
        const bool timedOut = result.stderrStr.find("Process timeout") != std::string::npos ||
                              result.stderrStr.find("Process timed out") != std::string::npos;
        EmitProcessDiag(BuildProcessDiagBlock(
            processDiagStartTs,
            CurrentUtcTimestamp(),
            processDiagCwd,
            effectiveCommand,
            effectiveArgs,
            result.exitCode,
            timedOut));
    }
    return result;
#else
    const auto timeoutMs = ResolveTimeoutMs(InCommand, effectiveArgs, InMode);
    auto result = RunProcessUnix(InCommand, effectiveArgs, InMode, timeoutMs, InWorkingDir, InProgressCallback);
    if (processDiagEnabled) {
        const bool timedOut = result.stderrStr.find("Process timeout") != std::string::npos ||
                              result.stderrStr.find("Process timed out") != std::string::npos;
        EmitProcessDiag(BuildProcessDiagBlock(
            processDiagStartTs,
            CurrentUtcTimestamp(),
            processDiagCwd,
            InCommand,
            effectiveArgs,
            result.exitCode,
            timedOut));
    }
    return result;
#endif
}

} // namespace kano::git::shell
