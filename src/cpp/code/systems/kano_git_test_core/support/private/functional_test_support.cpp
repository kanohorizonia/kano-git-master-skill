#include "functional_test_support.hpp"
#include "shell_executor.hpp"

#include <cstdlib>
#include <tuple>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace kano::git::tests::functional {
namespace {

auto UniqueSuffix() -> std::string {
    const auto now = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    return std::to_string(now);
}

auto CurrentExecutablePath() -> std::filesystem::path {
#if defined(_WIN32)
    std::string buffer(MAX_PATH, '\0');
    const auto written = GetModuleFileNameA(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (written == 0) {
        return {};
    }
    buffer.resize(written);
    return std::filesystem::path(buffer).lexically_normal();
#else
    return {};
#endif
}

#if defined(_WIN32)
auto CmdQuote(const std::string& InValue) -> std::string {
    std::string quoted = "\"";
    for (const char ch : InValue) {
        if (ch == '"') {
            quoted += "\\\"";
        } else {
            quoted.push_back(ch);
        }
    }
    quoted.push_back('"');
    return quoted;
}

auto BashQuote(const std::string& InValue) -> std::string {
    std::string quoted = "'";
    for (const char ch : InValue) {
        if (ch == '\'') {
            quoted += "'\\''";
        } else {
            quoted.push_back(ch);
        }
    }
    quoted.push_back('\'');
    return quoted;
}
#endif

} // namespace

auto CreateSandboxWorkspace(const std::string& InName) -> SandboxContext {
    auto base = std::filesystem::temp_directory_path() / "kano-git-functional";
    std::filesystem::create_directories(base);
    auto root = (base / (InName + "-" + UniqueSuffix())).lexically_normal();
    std::filesystem::create_directories(root);
    return SandboxContext{.root = root};
}

auto RemoveSandboxWorkspace(const SandboxContext& InSandbox) -> void {
    std::error_code ec;
    std::filesystem::remove_all(InSandbox.root, ec);
}

auto ResolveKogBinaryPath() -> std::filesystem::path {
    const auto exe = CurrentExecutablePath();
    if (exe.empty()) {
        return {};
    }
#if defined(_WIN32)
    return (exe.parent_path() / "kano-git.exe").lexically_normal();
#else
    return (exe.parent_path() / "kano-git").lexically_normal();
#endif
}

auto ResolveSkillRootFromKogBinary() -> std::filesystem::path {
    auto path = ResolveKogBinaryPath();
    if (path.empty()) {
        return {};
    }
    auto current = path.parent_path();
    for (int i = 0; i < 6 && !current.empty(); ++i) {
        current = current.parent_path();
    }
    return current.lexically_normal();
}

auto RunCommand(const std::string& InProgram,
                const std::vector<std::string>& InArgs,
                const std::filesystem::path& InWorkingDir) -> CommandResult {
    CommandResult result;
    const auto start = std::chrono::steady_clock::now();
    const auto previous = std::filesystem::current_path();
    std::filesystem::current_path(InWorkingDir);
    const auto exec = kano::git::shell::ExecuteCommand(
        InProgram,
        InArgs,
        kano::git::shell::ExecMode::Capture,
        std::nullopt
    );
    std::filesystem::current_path(previous);
    result.exitCode = exec.exitCode;
    result.stdoutText = exec.stdoutStr;
    result.stderrText = exec.stderrStr;

    result.elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start).count();
    return result;
}

auto RunGit(const std::vector<std::string>& InArgs,
            const std::filesystem::path& InWorkingDir) -> CommandResult {
    return RunCommand("git", InArgs, InWorkingDir);
}

auto RunKog(const std::vector<std::string>& InArgs,
            const std::filesystem::path& InWorkingDir) -> CommandResult {
    return RunKogWithEnv(InArgs, InWorkingDir, {});
}

auto RunKogWithEnv(const std::vector<std::string>& InArgs,
                   const std::filesystem::path& InWorkingDir,
                   const std::vector<std::pair<std::string, std::string>>& InEnv) -> CommandResult {
    auto env = InEnv;
    const auto skillRoot = ResolveSkillRootFromKogBinary();
    if (!skillRoot.empty()) {
        env.emplace_back("KANO_GIT_SKILL_ROOT", skillRoot.string());
    }
#if defined(_WIN32)
    std::vector<std::pair<std::string, std::optional<std::string>>> previousValues;
    previousValues.reserve(env.size() + 1);
    if (const char* previousRaw = std::getenv("KOG_GIT_INTERACTIVE"); previousRaw != nullptr) {
        previousValues.emplace_back("KOG_GIT_INTERACTIVE", std::string(previousRaw));
    } else {
        previousValues.emplace_back("KOG_GIT_INTERACTIVE", std::nullopt);
    }
    _putenv_s("KOG_GIT_INTERACTIVE", "0");
    for (const auto& [key, value] : env) {
        if (const char* envRaw = std::getenv(key.c_str()); envRaw != nullptr) {
            previousValues.emplace_back(key, std::string(envRaw));
        } else {
            previousValues.emplace_back(key, std::nullopt);
        }
        _putenv_s(key.c_str(), value.c_str());
    }
    const auto result = RunCommand(ResolveKogBinaryPath().string(), InArgs, InWorkingDir);
    for (const auto& [key, value] : previousValues) {
        if (value.has_value()) {
            _putenv_s(key.c_str(), value->c_str());
        } else {
            _putenv_s(key.c_str(), "");
        }
    }
    return result;
#else
    std::vector<std::tuple<std::string, std::string, bool>> previousValues;
    previousValues.reserve(env.size() + 1);
    const char* previousRaw = std::getenv("KOG_GIT_INTERACTIVE");
    previousValues.emplace_back("KOG_GIT_INTERACTIVE", previousRaw != nullptr ? previousRaw : "", previousRaw != nullptr);
    setenv("KOG_GIT_INTERACTIVE", "0", 1);
    for (const auto& [key, value] : env) {
        const char* envRaw = std::getenv(key.c_str());
        previousValues.emplace_back(key, envRaw != nullptr ? envRaw : "", envRaw != nullptr);
        setenv(key.c_str(), value.c_str(), 1);
    }
    const auto result = RunCommand(ResolveKogBinaryPath().string(), InArgs, InWorkingDir);
    for (const auto& [key, value, hadValue] : previousValues) {
        if (hadValue) {
            setenv(key.c_str(), value.c_str(), 1);
        } else {
            unsetenv(key.c_str());
        }
    }
    return result;
#endif
}

} // namespace kano::git::tests::functional
