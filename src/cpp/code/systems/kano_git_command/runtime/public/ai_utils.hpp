#pragma once

#include <chrono>
#include <filesystem>
#include <string>
#include <vector>
#include <optional>
#include "shell_executor.hpp"

namespace kano::git::commands {

struct AiInvocationDiagnostics {
    std::string purpose;
    std::string requestedProvider;
    std::string resolvedProvider;
    std::string requestedModel;
    std::string effectiveModel;
    std::string modelMode;
    bool yolo = false;
    std::optional<std::filesystem::path> promptFile;
    std::optional<std::filesystem::path> workingFile;
    std::optional<std::filesystem::path> responseFile;
    std::optional<std::chrono::seconds> timeout;
};

// --- String & Path Utilities ---
auto Trim(std::string InValue) -> std::string;
auto ToLower(std::string InValue) -> std::string;
auto ReplaceAll(std::string InText, const std::string& InFrom, const std::string& InTo) -> std::string;
auto Fnv1a64Hex(const std::string& InValue) -> std::string;
auto CurrentUtcCompact() -> std::string;
// Canonical agent-mode environment contract: returns true when either
// KANO_AGENT_MODE or AGENT_MODE is set to a truthy value (1/true/yes/on,
// case-insensitive, trimmed). All KOG command paths must use this resolver
// rather than reimplementing env detection inline. See ai_utils.cpp for the
// single source of truth and KOG-BUG-0137 for context.
auto IsAgentModeEnabled() -> bool;
// Returns the env var name ("KANO_AGENT_MODE" or "AGENT_MODE") that
// resolved to a truthy value, or std::nullopt if neither is set or both
// are non-truthy. Useful for diagnostics and human review surfaces.
auto ResolveAgentModeEnvironment() -> std::optional<std::string>;
auto AIResolveConflicts(const std::filesystem::path& InWorkspaceRoot,
                        const std::string& InProvider = "auto",
                        const std::string& InModel = "auto") -> bool;

// --- File I/O ---
auto ReadFileText(const std::filesystem::path& InPath) -> std::optional<std::string>;
auto WriteFileText(const std::filesystem::path& InPath, const std::string& InText, std::string* OutError = nullptr) -> bool;

// --- Skill & Asset Resolution ---
auto ResolveSkillRoot(const std::filesystem::path& InWorkspaceRoot) -> std::filesystem::path;
auto LoadPromptAssetText(const std::filesystem::path& InWorkspaceRoot,
                         const char* InEnvVar,
                         const std::filesystem::path& InRelativeAssetPath) -> std::optional<std::string>;

// --- AI Provider Executors ---
void AppendModelArgs(std::vector<std::string>& OutArgs, const std::string& InModel);
void AppendModelArgsForProvider(std::vector<std::string>& OutArgs,
                                const std::string& InProvider,
                                const std::string& InModelMode,
                                const std::string& InModel);
auto ExecuteStandaloneCopilot(const std::vector<std::string>& InArgs,
                              std::optional<std::filesystem::path> InWorkingDir = std::nullopt) -> shell::ExecResult;
auto FormatCommandLineForLog(const std::string& InBinary, const std::vector<std::string>& InArgs) -> std::string;
auto PrintAiInvocationDiagnostics(const std::string& InBinary,
                                  const std::vector<std::string>& InArgs,
                                  const AiInvocationDiagnostics& InDiagnostics) -> void;
auto ExecuteCommandWithHeartbeat(const std::string& InBinary,
                                 const std::vector<std::string>& InArgs,
                                 shell::ExecMode InMode,
                                 std::optional<std::filesystem::path> InWorkingDir,
                                 const AiInvocationDiagnostics& InDiagnostics) -> shell::ExecResult;
auto ExecuteStandaloneCopilotWithDiagnostics(const std::vector<std::string>& InArgs,
                                             std::optional<std::filesystem::path> InWorkingDir,
                                             const AiInvocationDiagnostics& InDiagnostics) -> shell::ExecResult;
auto ExecuteCodexExec(std::optional<std::filesystem::path> InWorkingDir,
                      const std::string& InPrompt,
                      const std::string& InPurpose,
                      const std::string& InModel) -> shell::ExecResult;

// --- Prompt Handling ---
auto WritePromptFile(const std::filesystem::path& InWorkdir,
                      const std::string& InPrompt,
                      const std::string& InPurpose,
                      std::filesystem::path* OutPath,
                      std::string* OutError = nullptr) -> bool;
auto BuildFileBackedPromptArgument(std::optional<std::filesystem::path> InWorkingDir,
                                   const std::string& InPrompt,
                                   const std::string& InPurpose) -> std::string;

} // namespace kano::git::commands
