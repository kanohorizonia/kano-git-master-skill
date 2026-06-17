#include "ai_utils.hpp"
#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <fstream>
#include <future>
#include <sstream>
#include <iomanip>
#include <iostream>
#include <thread>

#include <format>

namespace kano::git::commands {

namespace {

auto LooksSensitiveKey(const std::string& InValue) -> bool {
    const auto value = ToLower(InValue);
    return value.find("token") != std::string::npos ||
           value.find("secret") != std::string::npos ||
           value.find("password") != std::string::npos ||
           value.find("authorization") != std::string::npos ||
           value.find("bearer") != std::string::npos ||
           value.find("api-key") != std::string::npos ||
           value.find("apikey") != std::string::npos ||
           value.find("access_token") != std::string::npos ||
           value.find("refresh_token") != std::string::npos;
}

auto QuoteForShellLog(const std::string& InValue) -> std::string {
    if (InValue.find_first_of(" \t\"'") == std::string::npos) {
        return InValue;
    }
    std::string quoted = "\"";
    for (const char ch : InValue) {
        if (ch == '"' || ch == '\\') {
            quoted.push_back('\\');
        }
        quoted.push_back(ch);
    }
    quoted.push_back('"');
    return quoted;
}

auto RedactCommandTokens(std::vector<std::string> InTokens) -> std::vector<std::string> {
    bool redactNext = false;
    for (auto& token : InTokens) {
        if (redactNext) {
            token = "<redacted>";
            redactNext = false;
            continue;
        }

        const auto lower = ToLower(token);
        const auto eqPos = token.find('=');
        if (eqPos != std::string::npos) {
            const auto key = token.substr(0, eqPos);
            if (LooksSensitiveKey(key)) {
                token = key + "=<redacted>";
                continue;
            }
        }

        if (LooksSensitiveKey(token) || lower == "--token" || lower == "--api-key" || lower == "--password" ||
            lower == "--authorization" || lower == "-h") {
            token = "<redacted>";
            redactNext = true;
            continue;
        }

        if (lower.rfind("bearer ", 0) == 0 || lower.find("authorization:") != std::string::npos) {
            token = "<redacted>";
        }
    }
    return InTokens;
}

auto ParseHeartbeatIntervalSeconds() -> std::chrono::seconds {
    const auto defaultInterval = std::chrono::seconds{30};
    const char* overrideRaw = std::getenv("KOG_AI_HEARTBEAT_SECONDS");
    if (overrideRaw == nullptr || overrideRaw[0] == '\0') {
        return defaultInterval;
    }
    try {
        const int value = std::stoi(overrideRaw);
        if (value <= 0) {
            return defaultInterval;
        }
        auto isTruthy = [](const char* InValue) {
            if (InValue == nullptr) {
                return false;
            }
            const std::string text = ToLower(Trim(std::string(InValue)));
            return text == "1" || text == "true" || text == "yes" || text == "on";
        };
        if (!isTruthy(std::getenv("KOG_DEBUG")) && !isTruthy(std::getenv("KOG_TEST_AI_HEARTBEAT"))) {
            return defaultInterval;
        }
        return std::chrono::seconds{value};
    } catch (...) {
        return defaultInterval;
    }
}

auto FormatOptionalPath(const std::optional<std::filesystem::path>& InPath) -> std::string {
    if (!InPath.has_value()) {
        return "<unknown>";
    }
    return InPath->lexically_normal().generic_string();
}

} // namespace

auto Trim(std::string InValue) -> std::string {
    while (!InValue.empty() && (InValue.back() == '\n' || InValue.back() == '\r' || InValue.back() == ' ' || InValue.back() == '\t')) {
        InValue.pop_back();
    }
    std::size_t start = 0;
    while (start < InValue.size() && (InValue[start] == ' ' || InValue[start] == '\t')) {
        start += 1;
    }
    return InValue.substr(start);
}

auto ToLower(std::string InValue) -> std::string {
    std::transform(InValue.begin(), InValue.end(), InValue.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return InValue;
}

auto ReplaceAll(std::string InText, const std::string& InFrom, const std::string& InTo) -> std::string {
    if (InFrom.empty()) return InText;
    std::size_t pos = 0;
    while ((pos = InText.find(InFrom, pos)) != std::string::npos) {
        InText.replace(pos, InFrom.size(), InTo);
        pos += InTo.size();
    }
    return InText;
}

auto Fnv1a64Hex(const std::string& InValue) -> std::string {
    uint64_t hash = 0xcbf29ce484222325ULL;
    for (const char c : InValue) {
        hash ^= static_cast<uint64_t>(static_cast<unsigned char>(c));
        hash *= 0x100000001b3ULL;
    }
    std::stringstream ss;
    ss << std::hex << std::setw(16) << std::setfill('0') << hash;
    return ss.str();
}

auto CurrentUtcCompact() -> std::string {
    const auto now = std::chrono::system_clock::now();
    const auto tt = std::chrono::system_clock::to_time_t(now);
    std::tm utc{};
#if defined(_WIN32)
    gmtime_s(&utc, &tt);
#else
    gmtime_r(&tt, &utc);
#endif
    char buffer[32] = {0};
    std::strftime(buffer, sizeof(buffer), "%Y%m%dT%H%M%SZ", &utc);
    return std::string(buffer);
}

auto IsAgentModeEnabled() -> bool {
    const char* value = std::getenv("KANO_AGENT_MODE");
    if (!value) return false;
    const auto v = ToLower(Trim(std::string(value)));
    return v == "1" || v == "true" || v == "yes" || v == "on";
}

auto ReadFileText(const std::filesystem::path& InPath) -> std::optional<std::string> {
    std::ifstream in(InPath, std::ios::in | std::ios::binary);
    if (!in) return std::nullopt;
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

auto WriteFileText(const std::filesystem::path& InPath, const std::string& InText, std::string* OutError) -> bool {
    std::error_code ec;
    const auto parent = InPath.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, ec);
        if (ec) {
            if (OutError) *OutError = ec.message();
            return false;
        }
    }
    std::ofstream out(InPath, std::ios::out | std::ios::binary | std::ios::trunc);
    if (!out) {
        if (OutError) *OutError = "Failed to open file for writing";
        return false;
    }
    out << InText;
    return out.good();
}

auto ResolveSkillRoot(const std::filesystem::path& InWorkspaceRoot) -> std::filesystem::path {
    if (const char* envRoot = std::getenv("KANO_GIT_SKILL_ROOT"); envRoot && std::string(envRoot).size() > 0) {
        return std::filesystem::path(envRoot).lexically_normal();
    }
    return (InWorkspaceRoot / ".agents" / "skills" / "kano" / "kano-git-master-skill").lexically_normal();
}

auto LoadPromptAssetText(const std::filesystem::path& InWorkspaceRoot,
                         const char* InEnvVar,
                         const std::filesystem::path& InRelativeAssetPath) -> std::optional<std::string> {
    std::vector<std::filesystem::path> candidates;
    if (InEnvVar) {
        if (const char* custom = std::getenv(InEnvVar); custom && std::string(custom).size() > 0) {
            candidates.emplace_back(std::filesystem::path(custom).lexically_normal());
        }
    }
    candidates.emplace_back((InWorkspaceRoot / InRelativeAssetPath).lexically_normal());
    candidates.emplace_back((ResolveSkillRoot(InWorkspaceRoot) / InRelativeAssetPath.filename()).lexically_normal());
    candidates.emplace_back((ResolveSkillRoot(InWorkspaceRoot) / InRelativeAssetPath).lexically_normal());
    for (const auto& candidate : candidates) {
        if (std::error_code ec; std::filesystem::exists(candidate, ec) && !ec) {
            if (const auto text = ReadFileText(candidate); text.has_value()) return *text;
        }
    }
    return std::nullopt;
}

auto CopilotStandaloneCommand() -> std::string {
    if (const char* overrideCommand = std::getenv("KOG_TEST_COPILOT_COMMAND");
        overrideCommand != nullptr && overrideCommand[0] != '\0') {
        return overrideCommand;
    }
#if defined(_WIN32)
    if (const char* appData = std::getenv("APPDATA"); appData != nullptr && appData[0] != '\0') {
        const auto candidate = (std::filesystem::path(appData) / "npm" / "copilot.cmd").lexically_normal();
        std::error_code ec;
        if (std::filesystem::exists(candidate, ec) && !ec) {
            return candidate.string();
        }
    }
    return "copilot.cmd";
#else
    return "copilot";
#endif
}

auto CodexStandaloneCommand() -> std::string {
#if defined(_WIN32)
    return "codex.cmd";
#else
    return "codex";
#endif
}

static auto HasCommand(const std::string& InCommand, const std::vector<std::string>& InArgs = {"--help"}) -> bool {
    const auto result = shell::ExecuteCommand(InCommand, InArgs, shell::ExecMode::Capture, std::filesystem::current_path());
    return result.exitCode == 0;
}

static auto GhCopilotAvailable() -> bool {
    return HasCommand("gh", {"copilot", "--help"});
}

static auto TryRunTestAiStubInAiUtils() -> std::optional<shell::ExecResult> {
    const char* stdoutRaw = std::getenv("KOG_TEST_AI_STDOUT");
    const char* exitRaw = std::getenv("KOG_TEST_AI_EXIT_CODE");
    if (stdoutRaw == nullptr && exitRaw == nullptr) {
        return std::nullopt;
    }
    shell::ExecResult out;
    if (stdoutRaw != nullptr) {
        out.stdoutStr = stdoutRaw;
    }
    if (exitRaw != nullptr && exitRaw[0] != '\0') {
        try {
            out.exitCode = std::stoi(exitRaw);
        } catch (...) {
            out.exitCode = 1;
            out.stderrStr = std::format("invalid KOG_TEST_AI_EXIT_CODE: {}", exitRaw);
        }
    }
    if (const char* logPath = std::getenv("KOG_TEST_AI_STUB_LOG"); logPath && logPath[0]) {
        std::ofstream lf(logPath, std::ios::app);
        lf << "copilot (in-process-stub via ExecuteStandaloneCopilot)\n";
    }
    return out;
}

auto FormatCommandLineForLog(const std::string& InBinary, const std::vector<std::string>& InArgs) -> std::string {
    std::vector<std::string> tokens;
    tokens.reserve(InArgs.size() + 1);
    tokens.push_back(InBinary);
    tokens.insert(tokens.end(), InArgs.begin(), InArgs.end());
    const auto redacted = RedactCommandTokens(std::move(tokens));
    std::ostringstream oss;
    for (std::size_t i = 0; i < redacted.size(); ++i) {
        if (i > 0) {
            oss << ' ';
        }
        oss << QuoteForShellLog(redacted[i]);
    }
    return oss.str();
}

auto PrintAiInvocationDiagnostics(const std::string& InBinary,
                                  const std::vector<std::string>& InArgs,
                                  const AiInvocationDiagnostics& InDiagnostics) -> void {
    std::cout << "\n[kog ai] -- AI Invocation (" << InDiagnostics.purpose << ") --\n";
    std::cout << "[kog ai] requested provider : " << InDiagnostics.requestedProvider << "\n";
    std::cout << "[kog ai] resolved provider  : " << InDiagnostics.resolvedProvider << "\n";
    std::cout << "[kog ai] requested model    : " << InDiagnostics.requestedModel << "\n";
    std::cout << "[kog ai] effective model    : " << InDiagnostics.effectiveModel << "\n";
    std::cout << "[kog ai] model mode         : " << InDiagnostics.modelMode << "\n";
    std::cout << "[kog ai] yolo               : " << (InDiagnostics.yolo ? "true" : "false") << "\n";
    std::cout << "[kog ai] timeout            : ";
    if (InDiagnostics.timeout.has_value()) {
        std::cout << InDiagnostics.timeout->count() << "s";
    } else {
        std::cout << "none";
    }
    std::cout << "\n";
    std::cout << "[kog ai] prompt dir         : ";
    if (InDiagnostics.promptFile.has_value()) {
        std::cout << InDiagnostics.promptFile->lexically_normal().parent_path().generic_string();
    } else {
        std::cout << "<unknown>";
    }
    std::cout << "\n";
    std::cout << "[kog ai] prompt file        : " << FormatOptionalPath(InDiagnostics.promptFile) << "\n";
    std::cout << "[kog ai] working plan       : " << FormatOptionalPath(InDiagnostics.workingFile) << "\n";
    std::cout << "[kog ai] response file      : " << FormatOptionalPath(InDiagnostics.responseFile) << "\n";
    std::cout << "[kog ai] response dir       : ";
    if (InDiagnostics.responseFile.has_value()) {
        std::cout << InDiagnostics.responseFile->lexically_normal().parent_path().generic_string();
    } else {
        std::cout << "<unknown>";
    }
    std::cout << "\n";
    std::cout << "[kog ai] command            : " << FormatCommandLineForLog(InBinary, InArgs) << "\n";
    std::cout.flush();
}

auto ExecuteCommandWithHeartbeat(const std::string& InBinary,
                                 const std::vector<std::string>& InArgs,
                                 shell::ExecMode InMode,
                                 std::optional<std::filesystem::path> InWorkingDir,
                                 const AiInvocationDiagnostics& InDiagnostics) -> shell::ExecResult {
    const auto heartbeatInterval = ParseHeartbeatIntervalSeconds();
    auto worker = std::async(std::launch::async, [&]() {
        return shell::ExecuteCommand(InBinary, InArgs, InMode, InWorkingDir);
    });

    const auto start = std::chrono::steady_clock::now();
    while (worker.wait_for(heartbeatInterval) != std::future_status::ready) {
        const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - start).count();
        std::cout << "[kog ai] waiting for " << InDiagnostics.resolvedProvider << " response... elapsed=" << elapsed << "s";
        if (InDiagnostics.responseFile.has_value()) {
            std::error_code ec;
            const auto& responsePath = *InDiagnostics.responseFile;
            const bool exists = std::filesystem::exists(responsePath, ec) && !ec;
            std::cout << " response_exists=" << (exists ? "true" : "false");
            if (exists) {
                std::cout << " response_size=" << std::filesystem::file_size(responsePath, ec);
            }
        }
        std::cout << "\n";
        std::cout.flush();
    }
    return worker.get();
}

auto ExecuteStandaloneCopilot(const std::vector<std::string>& InArgs,
                              std::optional<std::filesystem::path> InWorkingDir) -> shell::ExecResult {
    if (const auto testResult = TryRunTestAiStubInAiUtils(); testResult.has_value()) {
        return *testResult;
    }
    const auto standaloneCopilot = CopilotStandaloneCommand();
    if (HasCommand(standaloneCopilot, {"--help"})) {
        return shell::ExecuteCommand(standaloneCopilot, InArgs, shell::ExecMode::Capture, InWorkingDir);
    }
    if (GhCopilotAvailable()) {
        std::vector<std::string> ghArgs{"copilot", "--"};
        ghArgs.insert(ghArgs.end(), InArgs.begin(), InArgs.end());
        return shell::ExecuteCommand("gh", ghArgs, shell::ExecMode::Capture, InWorkingDir);
    }
    return shell::ExecuteCommand(standaloneCopilot, InArgs, shell::ExecMode::Capture, InWorkingDir);
}

auto ExecuteStandaloneCopilotWithDiagnostics(const std::vector<std::string>& InArgs,
                                             std::optional<std::filesystem::path> InWorkingDir,
                                             const AiInvocationDiagnostics& InDiagnostics) -> shell::ExecResult {
    if (const auto testResult = TryRunTestAiStubInAiUtils(); testResult.has_value()) {
        return *testResult;
    }
    const auto standaloneCopilot = CopilotStandaloneCommand();
    if (HasCommand(standaloneCopilot, {"--help"})) {
        return ExecuteCommandWithHeartbeat(standaloneCopilot, InArgs, shell::ExecMode::Capture, InWorkingDir, InDiagnostics);
    }
    if (GhCopilotAvailable()) {
        std::vector<std::string> ghArgs{"copilot", "--"};
        ghArgs.insert(ghArgs.end(), InArgs.begin(), InArgs.end());
        return ExecuteCommandWithHeartbeat("gh", ghArgs, shell::ExecMode::Capture, InWorkingDir, InDiagnostics);
    }
    return ExecuteCommandWithHeartbeat(standaloneCopilot, InArgs, shell::ExecMode::Capture, InWorkingDir, InDiagnostics);
}

static auto IsTruthyEnv(const char* InValue) -> bool {
    if (!InValue) return false;
    const auto v = ToLower(Trim(std::string(InValue)));
    return v == "1" || v == "true" || v == "yes" || v == "on";
}

static auto SplitEnvList(const char* InValue) -> std::vector<std::string> {
    std::vector<std::string> out;
    if (!InValue) return out;
    std::string raw = InValue;
    std::string token;
    std::istringstream iss(raw);
    while (std::getline(iss, token, ';')) {
        token = Trim(std::move(token));
        if (!token.empty()) out.push_back(std::move(token));
    }
    return out;
}

static void AppendBoolFlag(std::vector<std::string>* OutArgs, const char* InEnvVar, const std::string& InFlag) {
    if (IsTruthyEnv(std::getenv(InEnvVar))) OutArgs->push_back(InFlag);
}

static void AppendSingleValueFlag(std::vector<std::string>* OutArgs, const char* InEnvVar, const std::string& InFlag) {
    if (const char* value = std::getenv(InEnvVar); value) {
        const auto trimmed = Trim(std::string(value));
        if (!trimmed.empty()) {
            OutArgs->push_back(InFlag);
            OutArgs->push_back(trimmed);
        }
    }
}

static void AppendRepeatableFlag(std::vector<std::string>* OutArgs, const char* InEnvVar, const std::string& InFlag) {
    for (const auto& value : SplitEnvList(std::getenv(InEnvVar))) {
        OutArgs->push_back(InFlag);
        OutArgs->push_back(value);
    }
}

auto WriteCodexResponseFilePath(const std::filesystem::path& InWorkdir,
                                const std::string& InPurpose,
                                const std::string& InPrompt,
                                std::filesystem::path* OutPath,
                                std::string* OutError) -> bool {
    const auto responseDir = (InWorkdir / ".kano" / "tmp" / "git" / "codex-responses").lexically_normal();
    std::error_code ec;
    std::filesystem::create_directories(responseDir, ec);
    if (ec) {
        if (OutError) *OutError = ec.message();
        return false;
    }
    *OutPath = (responseDir / std::format("{}-{}-{}.txt", InPurpose, CurrentUtcCompact(), Fnv1a64Hex(InPrompt).substr(0, 8))).lexically_normal();
    return true;
}

void AppendModelArgs(std::vector<std::string>& OutArgs, const std::string& InModel) {
    if (InModel.empty() || InModel == "auto") {
        return;
    }
    std::string actualModel = InModel;
    std::string reasoningLevel;

    const auto qmPos = actualModel.find('?');
    if (qmPos != std::string::npos) {
        const auto query = actualModel.substr(qmPos + 1);
        actualModel = actualModel.substr(0, qmPos);
        
        const auto rPos = query.find("reasoning=");
        if (rPos != std::string::npos) {
            const auto ampPos = query.find('&', rPos);
            const auto valStart = rPos + 10;
            if (ampPos == std::string::npos) {
                reasoningLevel = query.substr(valStart);
            } else {
                reasoningLevel = query.substr(valStart, ampPos - valStart);
            }
        }
    }

    if (actualModel.rfind("copilot/", 0) == 0) {
        actualModel = actualModel.substr(8);
    } else if (actualModel.rfind("codex/", 0) == 0) {
        actualModel = actualModel.substr(6);
    } else if (actualModel.rfind("opencode/", 0) == 0) {
        actualModel = actualModel.substr(9);
    }

    OutArgs.push_back("--model");
    OutArgs.push_back(actualModel);

    if (!reasoningLevel.empty()) {
        OutArgs.push_back("--reasoning-effort");
        OutArgs.push_back(reasoningLevel);
    }
}

void AppendModelArgsForProvider(std::vector<std::string>& OutArgs,
                                const std::string& InProvider,
                                const std::string& InModelMode,
                                const std::string& InModel) {
    const auto provider = ToLower(Trim(InProvider));
    const auto mode = ToLower(Trim(InModelMode));

    if (mode == "provider-default") {
        return;
    }

    if (mode == "provider-auto") {
        if (provider == "copilot") {
            OutArgs.push_back("--model");
            OutArgs.push_back("auto");
        }
        return;
    }

    AppendModelArgs(OutArgs, InModel);
}

auto ExecuteCodexExec(std::optional<std::filesystem::path> InWorkingDir,
                      const std::string& InPrompt,
                      const std::string& InPurpose,
                      const std::string& InModel) -> shell::ExecResult {
    const auto workdir = InWorkingDir.value_or(std::filesystem::current_path());
    const auto effectivePrompt = BuildFileBackedPromptArgument(workdir, InPrompt, InPurpose);
    std::filesystem::path responsePath;
    std::string responseError;
    if (!WriteCodexResponseFilePath(workdir, InPurpose, InPrompt, &responsePath, &responseError)) {
        return shell::ExecResult{.exitCode = 1, .stderrStr = "codex response path error: " + responseError};
    }
    std::vector<std::string> args{"exec", "--skip-git-repo-check"};
    AppendBoolFlag(&args, "KOG_CODEX_FULL_AUTO", "--full-auto");
    AppendBoolFlag(&args, "KOG_CODEX_EPHEMERAL", "--ephemeral");
    AppendBoolFlag(&args, "KOG_CODEX_JSON", "--json");
    AppendSingleValueFlag(&args, "KOG_CODEX_SANDBOX", "--sandbox");
    AppendSingleValueFlag(&args, "KOG_CODEX_PROFILE", "--profile");
    AppendRepeatableFlag(&args, "KOG_CODEX_ADD_DIRS", "--add-dir");
    args.push_back("-o");
    args.push_back(responsePath.generic_string());
    args.push_back("--cd");
    args.push_back(workdir.generic_string());
    AppendModelArgs(args, InModel);
    args.push_back(effectivePrompt);
    auto result = shell::ExecuteCommand(CodexStandaloneCommand(), args, shell::ExecMode::Capture, InWorkingDir);
    if (result.exitCode == 0) {
        if (const auto responseText = ReadFileText(responsePath); responseText) result.stdoutStr = *responseText;
    }
    return result;
}

auto WritePromptFile(const std::filesystem::path& InWorkdir,
                      const std::string& InPrompt,
                      const std::string& InPurpose,
                      std::filesystem::path* OutPath,
                      std::string* OutError) -> bool {
    const auto promptDir = (InWorkdir / ".kano" / "tmp" / "git" / "provider-prompts").lexically_normal();
    std::error_code ec;
    std::filesystem::create_directories(promptDir, ec);
    if (ec) {
        if (OutError) *OutError = ec.message();
        return false;
    }
    // Default: overwrite a fixed filename so old prompts don't accumulate.
    // Set KOG_DEBUG_PROMPTS=1 to get timestamped filenames for debugging.
    const bool debugPrompts = [] {
        const char* v = std::getenv("KOG_DEBUG_PROMPTS");
        if (v == nullptr) return false;
        const auto s = std::string(v);
        return s == "1" || s == "true" || s == "yes";
    }();
    const auto filename = debugPrompts
        ? std::format("{}-{}-{}.md", InPurpose, CurrentUtcCompact(), Fnv1a64Hex(InPrompt).substr(0, 8))
        : std::format("{}-default.md", InPurpose);
    const auto promptPath = (promptDir / filename).lexically_normal();
    if (!WriteFileText(promptPath, InPrompt, OutError)) return false;
    if (OutPath) *OutPath = promptPath;
    return true;
}

auto BuildFileBackedPromptArgument(std::optional<std::filesystem::path> InWorkingDir,
                                   const std::string& InPrompt,
                                   const std::string& InPurpose) -> std::string {
    if (!InWorkingDir.has_value()) return InPrompt;
    std::filesystem::path promptPath;
    if (!WritePromptFile(*InWorkingDir, InPrompt, InPurpose, &promptPath)) return InPrompt;
    // Use absolute path so copilot can resolve the @file reference regardless of cwd.
    const auto refText = promptPath.lexically_normal().generic_string();
    return std::format("Read @{} and follow it exactly. Treat that file as the complete task. Do not ask clarifying questions. Output only the final answer required by that file.", refText);
}

auto AIResolveFile(const std::filesystem::path& InWorkspaceRoot,
                  const std::string& InFile,
                  const std::string& InProvider,
                  const std::string& InModel) -> bool {
    const auto filePath = (InWorkspaceRoot / InFile).lexically_normal();
    const auto content = ReadFileText(filePath);
    if (!content.has_value()) {
        std::cerr << "Error: failed to read file context for AI resolution: " << InFile << "\n";
        return false;
    }

    auto prompt = LoadPromptAssetText(InWorkspaceRoot, "KOG_RESOLVE_PROMPT_TEMPLATE", "assets/prompts/base/conflict-resolve.md");
    if (!prompt.has_value()) {
        std::cerr << "Error: failed to load conflict-resolve prompt template.\n";
        return false;
    }

    *prompt = ReplaceAll(*prompt, "{{FILE_CONTENT}}", *content);

    shell::ExecResult result;
    if (InProvider == "copilot") {
        const auto finalPrompt = BuildFileBackedPromptArgument(InWorkspaceRoot, *prompt, "resolve");
        std::vector<std::string> args;
        AppendModelArgs(args, InModel);
        args.push_back("-p");
        args.push_back(finalPrompt);
        result = ExecuteStandaloneCopilot(args, InWorkspaceRoot);
    } else if (InProvider == "codex") {
        result = ExecuteCodexExec(InWorkspaceRoot, *prompt, "resolve", InModel);
    } else {
        std::cerr << "Error: unsupported or missing AI provider for auto-resolve: " << InProvider << "\n";
        return false;
    }

    if (result.exitCode != 0) {
        std::cerr << "Error: AI resolution failed for " << InFile << ": " << result.stderrStr << "\n";
        return false;
    }

    auto resolved = Trim(result.stdoutStr);
    if (resolved.find("<<<<<<<") != std::string::npos || resolved.find(">>>>>>>") != std::string::npos) {
        std::cerr << "Error: AI output for " << InFile << " still contains conflict markers.\n";
        return false;
    }

    if (!WriteFileText(filePath, resolved)) {
        std::cerr << "Error: failed to write resolved content to " << InFile << "\n";
        return false;
    }

    shell::ExecuteCommand("git", {"add", InFile}, shell::ExecMode::Capture, InWorkspaceRoot);
    return true;
}

auto AIResolveConflicts(const std::filesystem::path& InWorkspaceRoot,
                        const std::string& InProvider,
                        const std::string& InModel) -> bool {
    const auto conflicts = shell::ExecuteCommand(
        "git",
        {"diff", "--name-only", "--diff-filter=U"},
        shell::ExecMode::Capture,
        InWorkspaceRoot
    );
    
    std::vector<std::string> files;
    std::istringstream iss(conflicts.stdoutStr);
    std::string line;
    while (std::getline(iss, line)) {
        line = Trim(line);
        if (!line.empty()) files.push_back(line);
    }

    if (files.empty()) {
        return true;
    }

    std::cout << "Found " << files.size() << " conflicted files. Starting AI resolution...\n";
    
    std::string activeProvider = InProvider;
    std::string activeModel = InModel;

    if (activeProvider == "auto") {
        if (const char* p = std::getenv("KOG_AI_PROVIDER")) activeProvider = p;
        else activeProvider = "copilot";
    }

    bool allResolved = true;
    for (const auto& file : files) {
        std::cout << "Resolving " << file << "...\n";
        if (!AIResolveFile(InWorkspaceRoot, file, activeProvider, activeModel)) {
            allResolved = false;
        }
    }
    return allResolved;
}

} // namespace kano::git::commands
