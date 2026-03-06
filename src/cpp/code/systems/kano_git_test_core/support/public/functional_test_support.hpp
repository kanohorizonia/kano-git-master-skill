#pragma once

#include <chrono>
#include <filesystem>
#include <string>
#include <vector>

namespace kano::git::tests::functional {

struct CommandResult {
    int exitCode = -1;
    std::string stdoutText;
    std::string stderrText;
    long long elapsedMs = 0;
};

struct SandboxContext {
    std::filesystem::path root;
};

auto CreateSandboxWorkspace(const std::string& InName) -> SandboxContext;
auto RemoveSandboxWorkspace(const SandboxContext& InSandbox) -> void;
auto ResolveKogBinaryPath() -> std::filesystem::path;
auto RunCommand(const std::string& InProgram,
                const std::vector<std::string>& InArgs,
                const std::filesystem::path& InWorkingDir) -> CommandResult;
auto RunGit(const std::vector<std::string>& InArgs,
            const std::filesystem::path& InWorkingDir) -> CommandResult;
auto RunKog(const std::vector<std::string>& InArgs,
            const std::filesystem::path& InWorkingDir) -> CommandResult;
auto RunKogWithEnv(const std::vector<std::string>& InArgs,
                   const std::filesystem::path& InWorkingDir,
                   const std::vector<std::pair<std::string, std::string>>& InEnv) -> CommandResult;

} // namespace kano::git::tests::functional
