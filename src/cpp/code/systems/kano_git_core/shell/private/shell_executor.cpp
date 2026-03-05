// Shell script executor implementation
// Cross-platform process spawning for shell scripts

#include "shell_executor.hpp"
#include <format>
#include <cstdlib>
#include <array>
#include <stdexcept>
#include <iostream>
#include <algorithm>
#include <cctype>

#ifdef KOG_PLATFORM_WINDOWS
#include <windows.h>
#else
#include <unistd.h>
#include <sys/wait.h>
#endif

namespace kano::git::shell {

auto GetScriptsDir() -> std::filesystem::path {
    namespace fs = std::filesystem;

    // Strategy: walk up from the executable to find src/shell/ directory
    // Binary is typically at src/cpp/build/<config>/kog-cli
    // Scripts are at ../../shell/ relative to the project root

    // 1. Try KANO_GIT_SCRIPTS_DIR env var first
    if (auto* env = std::getenv("KANO_GIT_SCRIPTS_DIR")) {
        fs::path p{env};
        if (fs::is_directory(p)) return p;
    }

    // 2. Try KANO_GIT_MASTER_ROOT env var
    if (auto* env = std::getenv("KANO_GIT_MASTER_ROOT")) {
        fs::path p = fs::path{env} / "src" / "shell";
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

namespace {

#ifdef KOG_PLATFORM_WINDOWS

auto RunProcess(const std::string& cmdLine, ExecMode InMode,
                 const std::optional<std::filesystem::path>& InWorkingDir) -> ExecResult
{
    ExecResult result;

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE hStdOutRead = nullptr, hStdOutWrite = nullptr;
    HANDLE hStdErrRead = nullptr, hStdErrWrite = nullptr;

    if (InMode == ExecMode::Capture) {
        CreatePipe(&hStdOutRead, &hStdOutWrite, &sa, 0);
        CreatePipe(&hStdErrRead, &hStdErrWrite, &sa, 0);
        SetHandleInformation(hStdOutRead, HANDLE_FLAG_INHERIT, 0);
        SetHandleInformation(hStdErrRead, HANDLE_FLAG_INHERIT, 0);
    }

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    if (InMode == ExecMode::Capture) {
        si.dwFlags |= STARTF_USESTDHANDLES;
        si.hStdOutput = hStdOutWrite;
        si.hStdError = hStdErrWrite;
        si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    }

    PROCESS_INFORMATION pi{};

    std::string mutable_cmd = cmdLine;  // CreateProcess needs mutable string

    std::string work_dir_str;
    LPCSTR work_dir_ptr = nullptr;
    if (InWorkingDir) {
        work_dir_str = InWorkingDir->string();
        work_dir_ptr = work_dir_str.c_str();
    }

    BOOL ok = CreateProcessA(
        nullptr,
        mutable_cmd.data(),
        nullptr, nullptr,
        InMode == ExecMode::Capture ? TRUE : FALSE,
        0,
        nullptr,
        work_dir_ptr,
        &si, &pi
    );

    if (!ok) {
        result.exitCode = -1;
        result.stderrStr = std::format("Failed to create process: {}", GetLastError());
        return result;
    }

    if (InMode == ExecMode::Capture) {
        CloseHandle(hStdOutWrite);
        CloseHandle(hStdErrWrite);

        // Read stdout
        std::array<char, 4096> buf{};
        DWORD bytesRead = 0;
        while (ReadFile(hStdOutRead, buf.data(), static_cast<DWORD>(buf.size()), &bytesRead, nullptr) && bytesRead > 0) {
            result.stdoutStr.append(buf.data(), bytesRead);
        }
        // Read stderr
        while (ReadFile(hStdErrRead, buf.data(), static_cast<DWORD>(buf.size()), &bytesRead, nullptr) && bytesRead > 0) {
            result.stderrStr.append(buf.data(), bytesRead);
        }

        CloseHandle(hStdOutRead);
        CloseHandle(hStdErrRead);
    }

    WaitForSingleObject(pi.hProcess, INFINITE);

    DWORD exitCode = 0;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    result.exitCode = static_cast<int>(exitCode);

    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    return result;
}

#else  // Unix

auto RunProcess(const std::string& cmdLine, ExecMode InMode,
                 const std::optional<std::filesystem::path>& InWorkingDir) -> ExecResult
{
    ExecResult result;

    std::string full_cmd = cmdLine;
    if (InWorkingDir) {
        full_cmd = std::format("cd '{}' && {}", InWorkingDir->string(), cmdLine);
    }

    if (InMode == ExecMode::PassThrough) {
        int rc = std::system(full_cmd.c_str());
        result.exitCode = WIFEXITED(rc) ? WEXITSTATUS(rc) : -1;
        return result;
    }

    // Capture mode
    std::string capture_cmd = full_cmd + " 2>&1";
    FILE* pipe = popen(capture_cmd.c_str(), "r");
    if (!pipe) {
        result.exitCode = -1;
        result.stderrStr = "Failed to open pipe";
        return result;
    }

    std::array<char, 4096> buf{};
    while (fgets(buf.data(), static_cast<int>(buf.size()), pipe) != nullptr) {
        result.stdoutStr += buf.data();
    }

    int rc = pclose(pipe);
    result.exitCode = WIFEXITED(rc) ? WEXITSTATUS(rc) : -1;

    return result;
}

#endif

auto BuildCommandLine(const std::string& program, const std::vector<std::string>& InArgs) -> std::string {
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
    const auto effectiveArgs = WithGitNonInteractiveDefaults(InCommand, InArgs);
    auto cmd = BuildCommandLine(InCommand, effectiveArgs);
    return RunProcess(cmd, InMode, InWorkingDir);
}

} // namespace kano::git::shell
