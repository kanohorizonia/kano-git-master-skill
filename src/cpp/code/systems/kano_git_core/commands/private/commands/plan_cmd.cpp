// plan/ignore commands - native plan pipeline and ignore doctor

#include "command_registry.hpp"
#include "discovery.hpp"
#include "shell_executor.hpp"
#include "auto_model_policy.hpp"
#include "kog_config.hpp"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <format>
#include <iomanip>
#include <iostream>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace kano::git::commands {
namespace {

struct IgnoreFinding {
    std::filesystem::path repo;
    std::string repoRel;
    std::string repoPath;
    std::string display;
};

struct IgnoreStageEntry {
    std::string repo = ".";
    std::string applyTarget = ".gitignore";
    std::string mergedOutputPath;
    std::vector<std::string> rules;
};

struct SecretRule {
    std::string id;
    std::regex pattern;
};

struct IgnoreDatasourceSource {
    std::string id;
    std::string kind;
    std::string pathRaw;
    bool enabled = true;
    std::filesystem::path resolvedPath;
};

struct CommitPlanEntry {
    int index = -1;
    std::string repo;
    std::string message;
    std::vector<std::string> include;
    std::vector<std::string> exclude;
    std::string reviewVerdict;
    std::string reviewReason;
};

struct CommitFillOp {
    int index = -1;
    std::string message;
    std::string reviewVerdict;
    std::string reviewReason;
    std::string plannerProvider;
    std::string plannerModel;
};

auto ExtractObjectBodyForKey(const std::string& InText, const std::string& InKey) -> std::optional<std::string>;
auto ExtractArrayBodyForKey(const std::string& InText, const std::string& InKey) -> std::optional<std::string>;
auto SplitTopLevelObjects(const std::string& InArrayBody) -> std::vector<std::string>;
auto ExtractStringField(const std::string& InObjectText, const std::string& InField) -> std::optional<std::string>;
auto ExtractScalarFieldToken(const std::string& InObjectText, const std::string& InField) -> std::optional<std::string>;
auto ExtractJsonBetweenMarkers(const std::string& InText) -> std::string;
auto ReadFileText(const std::filesystem::path& InPath) -> std::optional<std::string>;
auto ResolveSkillRoot(const std::filesystem::path& InWorkspaceRoot) -> std::filesystem::path;
auto ComputeWorkspaceBaseHeadSha(const std::filesystem::path& InWorkspaceRoot) -> std::string;
auto ComputeWorkspaceDirtyFingerprint(const std::filesystem::path& InWorkspaceRoot) -> std::string;
auto Fnv1a64Hex(const std::string& InText) -> std::string;
auto DiscoverWorkspaceRepos(const std::filesystem::path& InRoot) -> std::vector<std::filesystem::path>;
auto GitCapture(const std::filesystem::path& InRepo, const std::vector<std::string>& InArgs) -> shell::ExecResult;
auto FillCommitEntryByFlatIndex(const std::string& InPlanText,
                                int InCommitIndex,
                                const std::optional<std::string>& InCommitMessage,
                                const std::optional<std::string>& InReviewVerdict,
                                const std::optional<std::string>& InReviewReason,
                                const std::optional<std::string>& InPlannerProvider,
                                const std::optional<std::string>& InPlannerModel,
                                std::string* OutError) -> std::optional<std::string>;
auto ReplaceJsonStringFieldInObject(std::string InJson,
                                    const std::string& InObjectKey,
                                    const std::string& InFieldKey,
                                    const std::string& InNewValue) -> std::optional<std::string>;

auto Trim(std::string InValue) -> std::string {
    while (!InValue.empty() &&
           (InValue.back() == '\n' || InValue.back() == '\r' || InValue.back() == ' ' || InValue.back() == '\t')) {
        InValue.pop_back();
    }
    std::size_t start = 0;
    while (start < InValue.size() && (InValue[start] == ' ' || InValue[start] == '\t')) {
        start += 1;
    }
    return InValue.substr(start);
}

auto ToLower(std::string InValue) -> std::string {
    std::transform(InValue.begin(), InValue.end(), InValue.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return InValue;
}

auto NormalizeAiModelKeyword(const std::string& InValue) -> std::string {
    return kog_config::NormalizeAiModelSelection(InValue);
}

auto IsTruthyEnv(const char* InValue) -> bool {
    if (InValue == nullptr) {
        return false;
    }
    const auto v = ToLower(Trim(std::string(InValue)));
    return v == "1" || v == "true" || v == "yes" || v == "on";
}

auto IsAgentModeEnabled() -> bool {
    return IsTruthyEnv(std::getenv("KANO_AGENT_MODE"));
}

auto SplitEnvList(const char* InValue) -> std::vector<std::string> {
    std::vector<std::string> out;
    if (InValue == nullptr) {
        return out;
    }
    std::string raw = InValue;
    std::string token;
    std::istringstream iss(raw);
    while (std::getline(iss, token, ';')) {
        token = Trim(std::move(token));
        if (!token.empty()) {
            out.push_back(std::move(token));
        }
    }
    return out;
}

void AppendRepeatableFlag(std::vector<std::string>* OutArgs,
                         const char* InEnvVar,
                         const std::string& InFlag) {
    if (OutArgs == nullptr) {
        return;
    }
    for (const auto& value : SplitEnvList(std::getenv(InEnvVar))) {
        OutArgs->push_back(InFlag);
        OutArgs->push_back(value);
    }
}

void AppendRepeatableFlagWithDefaults(std::vector<std::string>* OutArgs,
                                      const char* InEnvVar,
                                      const std::string& InFlag,
                                      const std::vector<std::string>& InDefaultValues) {
    if (OutArgs == nullptr) {
        return;
    }
    const char* value = std::getenv(InEnvVar);
    const auto values = value == nullptr ? InDefaultValues : SplitEnvList(value);
    for (const auto& item : values) {
        if (Trim(item).empty()) {
            continue;
        }
        OutArgs->push_back(InFlag);
        OutArgs->push_back(item);
    }
}

void AppendSingleValueFlag(std::vector<std::string>* OutArgs,
                           const char* InEnvVar,
                           const std::string& InFlag) {
    if (OutArgs == nullptr) {
        return;
    }
    if (const char* value = std::getenv(InEnvVar); value != nullptr) {
        const auto trimmed = Trim(std::string(value));
        if (!trimmed.empty()) {
            OutArgs->push_back(InFlag);
            OutArgs->push_back(trimmed);
        }
    }
}

void AppendBoolFlag(std::vector<std::string>* OutArgs,
                    const char* InEnvVar,
                    const std::string& InFlag) {
    if (OutArgs == nullptr) {
        return;
    }
    if (IsTruthyEnv(std::getenv(InEnvVar))) {
        OutArgs->push_back(InFlag);
    }
}

void AppendBoolFlagDefaultTrue(std::vector<std::string>* OutArgs,
                               const char* InEnvVar,
                               const std::string& InFlag) {
    if (OutArgs == nullptr) {
        return;
    }
    const char* value = std::getenv(InEnvVar);
    if (value == nullptr) {
        OutArgs->push_back(InFlag);
        return;
    }
    if (IsTruthyEnv(value)) {
        OutArgs->push_back(InFlag);
    }
}

void AppendSingleValueFlagWithDefault(std::vector<std::string>* OutArgs,
                                      const char* InEnvVar,
                                      const std::string& InFlag,
                                      const std::string& InDefaultValue) {
    if (OutArgs == nullptr) {
        return;
    }
    if (const char* value = std::getenv(InEnvVar); value != nullptr) {
        const auto trimmed = Trim(std::string(value));
        if (!trimmed.empty()) {
            OutArgs->push_back(InFlag);
            OutArgs->push_back(trimmed);
        }
        return;
    }
    if (!InDefaultValue.empty()) {
        OutArgs->push_back(InFlag);
        OutArgs->push_back(InDefaultValue);
    }
}

void AppendFlagOrSingleValueFlag(std::vector<std::string>* OutArgs,
                                 const char* InEnvVar,
                                 const std::string& InFlag) {
    if (OutArgs == nullptr) {
        return;
    }
    const char* value = std::getenv(InEnvVar);
    if (value == nullptr) {
        return;
    }
    const auto trimmed = Trim(std::string(value));
    if (trimmed.empty()) {
        return;
    }
    const auto lowered = ToLower(trimmed);
    if (lowered == "0" || lowered == "false" || lowered == "no" || lowered == "off") {
        return;
    }
    OutArgs->push_back(InFlag);
    if (!(lowered == "1" || lowered == "true" || lowered == "yes" || lowered == "on")) {
        OutArgs->push_back(trimmed);
    }
}

auto StartsWith(const std::string& InValue, const std::string& InPrefix) -> bool {
    return InValue.rfind(InPrefix, 0) == 0;
}

auto CurrentUtcIso8601() -> std::string {
    const auto now = std::chrono::system_clock::now();
    const auto tt = std::chrono::system_clock::to_time_t(now);
    std::tm utc{};
#if defined(_WIN32)
    gmtime_s(&utc, &tt);
#else
    gmtime_r(&tt, &utc);
#endif
    char buffer[32] = {0};
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &utc);
    return std::string(buffer);
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

auto HomeDirectory() -> std::filesystem::path {
    if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0') {
        return std::filesystem::path(home).lexically_normal();
    }
#if defined(_WIN32)
    if (const char* userProfile = std::getenv("USERPROFILE"); userProfile != nullptr && *userProfile != '\0') {
        return std::filesystem::path(userProfile).lexically_normal();
    }
    const char* homeDrive = std::getenv("HOMEDRIVE");
    const char* homePath = std::getenv("HOMEPATH");
    if (homeDrive != nullptr && *homeDrive != '\0' && homePath != nullptr && *homePath != '\0') {
        return (std::filesystem::path(homeDrive) / homePath).lexically_normal();
    }
#endif
    return {};
}

auto CountWorkspaceDirtyEntries(const std::filesystem::path& InRoot) -> int {
    auto countRepoDirtyEntries = [](const std::filesystem::path& InRepo) -> int {
        int total = 0;
        const auto status = GitCapture(InRepo, {"status", "--porcelain", "--untracked-files=all"});
        if (status.exitCode != 0 || Trim(status.stdoutStr).empty()) {
            return 0;
        }
        std::istringstream iss(status.stdoutStr);
        std::string line;
        while (std::getline(iss, line)) {
            if (!Trim(line).empty()) {
                total += 1;
            }
        }
        return total;
    };

    int total = 0;
    const auto repos = DiscoverWorkspaceRepos(InRoot);
    for (const auto& repo : repos) {
        total += countRepoDirtyEntries(repo);
    }
    return total;
}

auto ResolveSystemRecommendedModel(const std::string& InProvider) -> std::string {
    if (InProvider == "copilot" || InProvider == "opencode") {
        return "gpt-5-mini";
    }
    if (InProvider == "codex") {
        return "gpt-5.2-codex";
    }
    return {};
}

auto ResolveAutoModelByChangeCount(const std::string& InProvider,
                                   const std::filesystem::path& InWorkspaceRoot) -> std::string {
    const int changedEntries = CountWorkspaceDirtyEntries(InWorkspaceRoot);
    if (InProvider == "copilot") {
        const auto policy = auto_model_policy::ResolveAutoModelPolicy(InProvider, InWorkspaceRoot, ResolveSkillRoot(InWorkspaceRoot));
        return auto_model_policy::ResolveModelForChangeCount(policy, changedEntries);
    }
    if (InProvider == "opencode") {
        return "gpt-5-mini";
    }
    if (InProvider == "codex") {
        return "gpt-5.2-codex";
    }
    return {};
}

auto ResolveAiModelDirective(const std::string& InProvider,
                             const std::string& InRequested,
                             const std::filesystem::path& InWorkspaceRoot) -> std::string {
    auto requested = Trim(InRequested);
    auto requestedLower = NormalizeAiModelKeyword(requested);
    if (!requested.empty()) {
        if (requestedLower == "auto") {
            return "auto";
        }
        if (requestedLower == "provider-default") {
            return "provider-default";
        }
        return requested;
    }

    return kog_config::ResolveDefaultAiModelSelection(InProvider, InWorkspaceRoot, ResolveSkillRoot(InWorkspaceRoot), "auto");
}

auto ResolveAiModelForChangeCount(const std::string& InProvider,
                                  const std::string& InModelDirective,
                                  const std::filesystem::path& InWorkspaceRoot,
                                  const int InChangeCount) -> std::string {
    const auto directive = Trim(InModelDirective);
    const auto directiveLower = NormalizeAiModelKeyword(directive);
    if (directiveLower == "provider-default") {
        return ResolveSystemRecommendedModel(InProvider);
    }
    if (directiveLower == "auto") {
        if (InProvider == "copilot") {
            const auto policy = auto_model_policy::ResolveAutoModelPolicy(InProvider, InWorkspaceRoot, ResolveSkillRoot(InWorkspaceRoot));
            return auto_model_policy::ResolveModelForChangeCount(policy, InChangeCount);
        }
        return ResolveSystemRecommendedModel(InProvider);
    }
    return directive;
}

auto CompactSingleLine(std::string InText, std::size_t InMaxChars) -> std::string {
    for (char& ch : InText) {
        if (ch == '\r' || ch == '\n' || ch == '\t') {
            ch = ' ';
        }
    }
    InText = Trim(std::move(InText));
    std::string compact;
    compact.reserve(InText.size());
    bool lastWasSpace = false;
    for (const char ch : InText) {
        if (ch == ' ') {
            if (!lastWasSpace) {
                compact.push_back(ch);
            }
            lastWasSpace = true;
            continue;
        }
        compact.push_back(ch);
        lastWasSpace = false;
    }
    InText = Trim(std::move(compact));
    if (InText.size() > InMaxChars && InMaxChars > 3) {
        InText = InText.substr(0, InMaxChars - 3) + "...";
    }
    return InText;
}

auto ReadFileText(const std::filesystem::path& InPath) -> std::optional<std::string> {
    std::ifstream in(InPath, std::ios::in | std::ios::binary);
    if (!in) {
        return std::nullopt;
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

auto WriteFileText(const std::filesystem::path& InPath, const std::string& InText, std::string* OutError = nullptr) -> bool {
    return workspace::WriteCacheFileText(InPath, InText, OutError);
}

auto RelativeDisplayPath(const std::filesystem::path& InRoot, const std::filesystem::path& InPath) -> std::string {
    const auto rel = InPath.lexically_relative(InRoot);
    if (!rel.empty()) {
        return rel.generic_string();
    }
    return InPath.lexically_normal().generic_string();
}

auto NormalizeInputPathForCurrentPlatform(std::string InPath) -> std::string {
    auto path = Trim(std::move(InPath));
    if (path.empty()) {
        return path;
    }
#if defined(_WIN32)
    auto toWindowsPath = [](char drive, std::string rest) -> std::string {
        for (auto& ch : rest) {
            if (ch == '/') {
                ch = '\\';
            }
        }
        if (!rest.empty() && (rest.front() == '\\' || rest.front() == '/')) {
            rest.erase(rest.begin());
        }
        std::string out;
        out.reserve(rest.size() + 3);
        out.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(drive))));
        out.append(":\\");
        out.append(rest);
        return out;
    };

    if (path.rfind("/cygdrive/", 0) == 0 && path.size() > 11 && std::isalpha(static_cast<unsigned char>(path[10])) &&
        path[11] == '/') {
        return toWindowsPath(path[10], path.substr(12));
    }
    if (path.rfind("/mnt/", 0) == 0 && path.size() > 6 && std::isalpha(static_cast<unsigned char>(path[5])) &&
        path[6] == '/') {
        return toWindowsPath(path[5], path.substr(7));
    }
    if (path.size() > 3 && path[0] == '/' && std::isalpha(static_cast<unsigned char>(path[1])) && path[2] == '/') {
        return toWindowsPath(path[1], path.substr(3));
    }
#endif
    return path;
}

auto ResolvePath(const std::filesystem::path& InBase, const std::string& InPath) -> std::filesystem::path {
    const std::filesystem::path p(NormalizeInputPathForCurrentPlatform(InPath));
    if (p.is_absolute()) {
        return p.lexically_normal();
    }
    return (InBase / p).lexically_normal();
}

auto GitCapture(const std::filesystem::path& InRepo, const std::vector<std::string>& InArgs) -> shell::ExecResult {
    return shell::ExecuteCommand("git", InArgs, shell::ExecMode::Capture, InRepo);
}

auto GitPassThrough(const std::filesystem::path& InRepo, const std::vector<std::string>& InArgs) -> shell::ExecResult {
    return shell::ExecuteCommand("git", InArgs, shell::ExecMode::PassThrough, InRepo);
}

auto IsProbableIgnoreArtifactPath(const std::string& InPath) -> bool {
    const auto lower = ToLower(InPath);
    const auto containsAny = [&](const std::vector<std::string>& keys) {
        for (const auto& key : keys) {
            if (lower.find(key) != std::string::npos) {
                return true;
            }
        }
        return false;
    };
    if (containsAny({"/.kano/", ".kano/", "/.cache/", ".cache/", "/.pytest_cache/", ".pytest_cache/", "/.mypy_cache/", ".mypy_cache/", "/.idea/",
                     ".idea/", "/.vscode/", ".vscode/", "/node_modules/", "node_modules/", "/dist/", "dist/", "/obj/",
                     "obj/", "/target/", "target/", "/out/", "out/"})) {
        return true;
    }
    for (const auto& suffix : {".log", ".tmp", ".temp", ".cache", ".bak", ".swp", ".swo", ".class", ".obj", ".o", ".pdb",
                               ".ilk", ".dmp", ".pyc"}) {
        if (lower.size() >= std::strlen(suffix) && lower.ends_with(suffix)) {
            return true;
        }
    }
    return false;
}

auto IsInternalPipelineArtifactPath(const std::string& InPath) -> bool {
    auto lower = ToLower(InPath);
    std::replace(lower.begin(), lower.end(), '\\', '/');
    return lower == ".kano" || lower.rfind(".kano/", 0) == 0 || lower.find("/.kano/") != std::string::npos;
}

auto DefaultSecretRulesPath(const std::filesystem::path& InWorkspaceRoot) -> std::filesystem::path {
    return (ResolveSkillRoot(InWorkspaceRoot) / "assets" / "security" / "secret-blacklist.rules").lexically_normal();
}

auto LoadSecretRules(const std::filesystem::path& InRulesPath, std::string* OutError) -> std::vector<SecretRule> {
    std::vector<SecretRule> rules;
    const auto content = ReadFileText(InRulesPath);
    if (!content.has_value()) {
        if (OutError != nullptr) {
            *OutError = std::string("rules file not found/readable: ") + InRulesPath.generic_string();
        }
        return rules;
    }
    std::istringstream iss(*content);
    std::string line;
    int lineNo = 0;
    while (std::getline(iss, line)) {
        lineNo += 1;
        const auto t = Trim(line);
        if (t.empty() || t[0] == '#') {
            continue;
        }
        const auto delim = t.find('|');
        if (delim == std::string::npos || delim == 0 || delim + 1 >= t.size()) {
            if (OutError != nullptr) {
                *OutError = std::format("invalid rule format at {}:{} (expected id|regex)", InRulesPath.generic_string(), lineNo);
            }
            return {};
        }
        const auto id = Trim(t.substr(0, delim));
        const auto expr = Trim(t.substr(delim + 1));
        try {
            rules.push_back(SecretRule{.id = id, .pattern = std::regex(expr)});
        } catch (const std::exception& ex) {
            if (OutError != nullptr) {
                *OutError = std::format("invalid regex at {}:{}: {}", InRulesPath.generic_string(), lineNo, ex.what());
            }
            return {};
        }
    }
    return rules;
}

auto ParseStatusChangedPath(const std::string& InLine) -> std::optional<std::string> {
    if (InLine.size() < 4) {
        return std::nullopt;
    }
    const char x = InLine[0];
    const char y = InLine[1];
    if (x == 'D' || y == 'D') {
        return std::nullopt;
    }
    if (x == '?' && y == '?') {
        return Trim(InLine.substr(3));
    }
    auto path = Trim(InLine.substr(3));
    const auto arrow = path.find(" -> ");
    if (arrow != std::string::npos) {
        path = Trim(path.substr(arrow + 4));
    }
    if (path.empty()) {
        return std::nullopt;
    }
    return path;
}

auto CollectChangedCandidateFiles(const std::filesystem::path& InRepo) -> std::vector<std::string> {
    std::vector<std::string> files;
    std::unordered_set<std::string> dedup;
    const auto status = GitCapture(InRepo, {"status", "--porcelain", "--untracked-files=all"});
    if (status.exitCode != 0 || Trim(status.stdoutStr).empty()) {
        return files;
    }
    std::istringstream iss(status.stdoutStr);
    std::string line;
    while (std::getline(iss, line)) {
        const auto maybePath = ParseStatusChangedPath(line);
        if (!maybePath.has_value()) {
            continue;
        }
        auto path = *maybePath;
        std::replace(path.begin(), path.end(), '\\', '/');
        if (dedup.insert(path).second) {
            files.push_back(path);
        }
    }
    return files;
}

struct SecretFinding {
    std::string repo;
    std::string file;
    std::string ruleId;
    int line = 0;
    std::string preview;
};

auto ScanFileForSecretRules(const std::filesystem::path& InRepo,
                            const std::string& InFile,
                            const std::vector<SecretRule>& InRules,
                            int InMaxFindings,
                            std::vector<SecretFinding>* OutFindings) -> void {
    if (OutFindings == nullptr || InMaxFindings <= 0 || static_cast<int>(OutFindings->size()) >= InMaxFindings) {
        return;
    }
    const auto full = (InRepo / std::filesystem::path(InFile)).lexically_normal();
    const auto text = ReadFileText(full);
    if (!text.has_value()) {
        return;
    }
    std::istringstream iss(*text);
    std::string line;
    int lineNo = 0;
    while (std::getline(iss, line)) {
        lineNo += 1;
        for (const auto& rule : InRules) {
            if (std::regex_search(line, rule.pattern)) {
                SecretFinding f;
                f.file = InFile;
                f.ruleId = rule.id;
                f.line = lineNo;
                f.preview = Trim(line);
                OutFindings->push_back(std::move(f));
                if (static_cast<int>(OutFindings->size()) >= InMaxFindings) {
                    return;
                }
            }
        }
    }
}

auto DiscoverWorkspaceRepos(const std::filesystem::path& InRoot) -> std::vector<std::filesystem::path> {
    std::vector<std::filesystem::path> repos;
    std::string ignoredReason;
    if (const auto manifest = workspace::LoadTrustedWorkspaceManifest(InRoot, &ignoredReason); manifest.has_value()) {
        repos.reserve(manifest->repos.size());
        for (const auto& repo : manifest->repos) {
            repos.push_back(repo.path.lexically_normal());
        }
    } else {
        workspace::DiscoverOptions options;
        options.rootDir = InRoot;
        options.maxDepth = 12;
        options.useCache = true;
        options.cacheTtlSeconds = 900;
        options.incremental = true;
        options.maxStaleSeconds = 86400;
        options.metadataLevel = "minimal";
        const auto discovery = workspace::DiscoverRepos(options);
        repos.reserve(discovery.repos.size());
        for (const auto& repo : discovery.repos) {
            repos.push_back(repo.path.lexically_normal());
        }
    }
    std::sort(repos.begin(), repos.end(), [](const auto& A, const auto& B) {
        return A.generic_string() < B.generic_string();
    });
    repos.erase(std::unique(repos.begin(), repos.end()), repos.end());
    if (repos.empty()) {
        repos.push_back(InRoot.lexically_normal());
    }
    return repos;
}

auto HasCommand(const std::string& InCommand, const std::vector<std::string>& InArgs = {"--help"}) -> bool {
    return shell::ExecuteCommand(InCommand, InArgs, shell::ExecMode::Capture).exitCode == 0;
}

auto CopilotStandaloneCommand() -> std::string {
#if defined(_WIN32)
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

auto HasStandaloneCopilotCommand() -> bool {
    return HasCommand(CopilotStandaloneCommand(), {"--help"}) || HasCommand("copilot", {"--help"});
}

auto ExecuteStandaloneCopilot(const std::vector<std::string>& InArgs,
                              const std::filesystem::path& InWorkdir) -> shell::ExecResult {
    return shell::ExecuteCommand(CopilotStandaloneCommand(), InArgs, shell::ExecMode::Capture, InWorkdir);
}

auto WritePromptFile(const std::filesystem::path& InWorkdir,
                     const std::string& InPrompt,
                     const std::string& InPurpose,
                     std::filesystem::path* OutPath,
                     std::string* OutError = nullptr) -> bool {
    if (OutPath == nullptr) {
        if (OutError != nullptr) {
            *OutError = "missing output path";
        }
        return false;
    }

    const auto promptDir = (InWorkdir / ".kano" / "tmp" / "git" / "provider-prompts").lexically_normal();
    std::error_code ec;
    std::filesystem::create_directories(promptDir, ec);
    if (ec) {
        if (OutError != nullptr) {
            *OutError = std::format("create_directories failed: {}", ec.message());
        }
        return false;
    }

    const auto promptPath = (promptDir / std::format("{}-{}-{}.md",
                                                      InPurpose,
                                                      CurrentUtcCompact(),
                                                      Fnv1a64Hex(InPrompt).substr(0, 8)))
                                .lexically_normal();
    if (!WriteFileText(promptPath, InPrompt, OutError)) {
        return false;
    }

    *OutPath = promptPath;
    return true;
}

auto BuildFileBackedPromptArgument(const std::filesystem::path& InWorkdir,
                                   const std::string& InPrompt,
                                   const std::string& InPurpose) -> std::string {
    std::filesystem::path promptPath;
    if (!WritePromptFile(InWorkdir, InPrompt, InPurpose, &promptPath)) {
        return InPrompt;
    }

    auto refPath = promptPath.lexically_relative(InWorkdir);
    if (refPath.empty()) {
        refPath = promptPath.lexically_normal();
    }
    return std::format(
        "Read @{} and follow it exactly. Treat that file as the complete task. Do not ask clarifying questions. Output only the final answer required by that file.",
        refPath.generic_string());
}

auto WriteCodexResponseFilePath(const std::filesystem::path& InWorkdir,
                                const std::string& InPurpose,
                                const std::string& InPrompt,
                                std::filesystem::path* OutPath,
                                std::string* OutError = nullptr) -> bool {
    if (OutPath == nullptr) {
        if (OutError != nullptr) {
            *OutError = "missing output path";
        }
        return false;
    }

    const auto responseDir = (InWorkdir / ".kano" / "tmp" / "git" / "codex-responses").lexically_normal();
    std::error_code ec;
    std::filesystem::create_directories(responseDir, ec);
    if (ec) {
        if (OutError != nullptr) {
            *OutError = std::format("create_directories failed: {}", ec.message());
        }
        return false;
    }

    *OutPath = (responseDir / std::format("{}-{}-{}.txt",
                                          InPurpose,
                                          CurrentUtcCompact(),
                                          Fnv1a64Hex(InPrompt).substr(0, 8)))
                   .lexically_normal();
    return true;
}

auto ExecuteCodexExec(const std::filesystem::path& InWorkdir,
                      const std::string& InPrompt,
                      const std::string& InPurpose,
                      const std::string& InModel) -> shell::ExecResult {
    std::filesystem::path responsePath;
    std::string responseError;
    if (!WriteCodexResponseFilePath(InWorkdir, InPurpose, InPrompt, &responsePath, &responseError)) {
        return shell::ExecResult{.exitCode = 1, .stderrStr = std::format("codex response path error: {}", responseError)};
    }

    std::vector<std::string> args{"exec", "--skip-git-repo-check"};
    AppendBoolFlag(&args, "KOG_CODEX_FULL_AUTO", "--full-auto");
    AppendBoolFlag(&args, "KOG_CODEX_EPHEMERAL", "--ephemeral");
    AppendBoolFlag(&args, "KOG_CODEX_JSON", "--json");
    AppendSingleValueFlag(&args, "KOG_CODEX_SANDBOX", "--sandbox");
    AppendSingleValueFlag(&args, "KOG_CODEX_PROFILE", "--profile");
    AppendRepeatableFlag(&args, "KOG_CODEX_ADD_DIRS", "--add-dir");
    args.push_back("-o");
    args.push_back(responsePath.lexically_normal().generic_string());
    args.push_back("--cd");
    args.push_back(InWorkdir.lexically_normal().generic_string());
    if (!InModel.empty() && InModel != "auto") {
        args.push_back("--model");
        args.push_back(InModel);
    }
    args.push_back(InPrompt);

    auto result = shell::ExecuteCommand(CodexStandaloneCommand(), args, shell::ExecMode::Capture, InWorkdir);
    if (result.exitCode == 0) {
        if (const auto responseText = ReadFileText(responsePath); responseText.has_value()) {
            result.stdoutStr = *responseText;
        }
    }
    return result;
}

auto LooksLikeModelToken(const std::string& InToken) -> bool {
    if (InToken.empty()) {
        return false;
    }
    for (const char ch : InToken) {
        const bool ok = (ch >= 'a' && ch <= 'z') ||
                        (ch >= 'A' && ch <= 'Z') ||
                        (ch >= '0' && ch <= '9') ||
                        ch == '-' || ch == '_' || ch == '.';
        if (!ok) {
            return false;
        }
    }
    return true;
}

auto IsLikelyAiModelName(const std::string& InToken) -> bool {
    if (!LooksLikeModelToken(InToken)) {
        return false;
    }

    const auto lower = ToLower(InToken);
    const bool knownPrefix = StartsWith(lower, "gpt-") ||
                             StartsWith(lower, "claude-") ||
                             StartsWith(lower, "gemini-") ||
                             StartsWith(lower, "grok-") ||
                             StartsWith(lower, "o1") ||
                             StartsWith(lower, "o3") ||
                             StartsWith(lower, "o4") ||
                             StartsWith(lower, "o5");
    if (!knownPrefix) {
        return false;
    }

    return std::any_of(lower.begin(), lower.end(), [](char ch) {
        return ch >= '0' && ch <= '9';
    });
}

auto ExtractModelTokensFromText(const std::string& InText) -> std::vector<std::string> {
    std::set<std::string> out;
    const std::regex modelPattern(R"(([A-Za-z0-9]+(?:[._-][A-Za-z0-9]+)+))");

    std::istringstream iss(InText);
    std::string lineRaw;
    while (std::getline(iss, lineRaw)) {
        const auto line = Trim(lineRaw);
        if (line.empty() || line.rfind("===", 0) == 0) {
            continue;
        }

        const auto firstSpace = line.find(' ');
        if (firstSpace != std::string::npos) {
            const auto firstToken = line.substr(0, firstSpace);
            if (IsLikelyAiModelName(firstToken)) {
                out.insert(firstToken);
            }
        }

        for (std::sregex_iterator it(line.begin(), line.end(), modelPattern), end; it != end; ++it) {
            const auto token = Trim((*it)[1].str());
            if (IsLikelyAiModelName(token)) {
                out.insert(token);
            }
        }
    }

    return std::vector<std::string>(out.begin(), out.end());
}

auto FetchProviderModelsForHelp(const std::string& InProvider) -> std::vector<std::string> {
    const auto provider = ToLower(Trim(InProvider));
    std::set<std::string> models;

    if (provider == "copilot") {
        models.insert("claude-haiku-4.5");
        models.insert("claude-opus-4.5");
        models.insert("claude-opus-4.6");
        models.insert("claude-opus-4.6-fast");
        models.insert("claude-sonnet-4");
        models.insert("claude-sonnet-4.5");
        models.insert("claude-sonnet-4.6");
        models.insert("gemini-3-pro-preview");
        models.insert("gpt-4.1");
        models.insert("gpt-5-mini");
        models.insert("gpt-5.1");
        models.insert("gpt-5.1-codex");
        models.insert("gpt-5.1-codex-max");
        models.insert("gpt-5.1-codex-mini");
        models.insert("gpt-5.2");
        models.insert("gpt-5.2-codex");
        models.insert("gpt-5.3-codex");
        models.insert("gpt-5.4");
    } else if (provider == "codex") {
        models.insert("gpt-5.2-codex");
        models.insert("gpt-5.3-codex");
    } else if (provider == "opencode") {
        models.insert("gpt-5-mini");
    }

    return std::vector<std::string>(models.begin(), models.end());
}

auto BuildSetAiModelHelpFooter() -> std::string {
    const auto cwd = std::filesystem::current_path().lexically_normal();
    const auto skillRoot = ResolveSkillRoot(cwd);
    const auto home = HomeDirectory();
    std::ostringstream oss;
    oss << "Layered config lookup order (low -> high):\n";
    if (!skillRoot.empty()) {
        oss << "  system: " << kog_config::SystemConfigPath(skillRoot).generic_string() << "\n";
    } else {
        oss << "  system: <skill-root>/assets/kog_config.toml\n";
    }
    if (!home.empty()) {
        oss << "  global: " << kog_config::GlobalConfigPath().generic_string() << "\n";
    } else {
        oss << "  global: <home>/.kano/kog_config.toml\n";
    }
    oss << "  local:  " << kog_config::LocalConfigPath(cwd).generic_string() << "\n";
    oss << "\nSpecial AI model keywords:\n";
    oss << "  provider-default : use provider/system-recommended fixed default model\n";
    oss << "  auto             : auto-select model by workspace change volume\n";
    oss << "  note             : legacy alias 'default' is still accepted\n";
    oss << "\nDefault model selection + auto-model config format:\n";
    oss << "  [ai.model]\n";
    oss << "  selection = \"auto\"  # auto | provider-default | provider/model | model\n";
    oss << "  [ai.model.auto]\n";
    oss << "  change_thresholds = [5, 10]\n";
    oss << "  models = [\"copilot/gpt-5-mini\", \"copilot/claude-haiku-4.5\", \"copilot/gpt-5.4\"]\n";
    oss << "  # supports more ranges, e.g. change_thresholds = [5, 10, 20] with 4 models\n";
    oss << "  note: provider:model (colon) is not supported.\n";
    oss << "\nModel list source: built-in provider catalog (non-blocking help).\n";
    oss << "\nDetected models by provider:\n";
    for (const auto& provider : std::vector<std::string>{"copilot", "codex", "opencode"}) {
        const auto models = FetchProviderModelsForHelp(provider);
        oss << "  " << provider << ": ";
        if (models.empty()) {
            oss << "<none detected>";
        } else {
            for (std::size_t i = 0; i < models.size(); ++i) {
                if (i != 0) {
                    oss << ", ";
                }
                oss << models[i];
            }
        }
        oss << "\n";
    }
    return oss.str();
}

auto ResolveAiProvider(const std::string& InRequested) -> std::string {
    const auto provider = ToLower(Trim(InRequested));
    if (!provider.empty() && provider != "auto") {
        if (provider == "copilot" || provider == "codex" || provider == "opencode") {
            return provider;
        }
        return {};
    }
    if (HasStandaloneCopilotCommand() || HasCommand("gh", {"copilot", "--version"})) {
        return "copilot";
    }
    if (HasCommand(CodexStandaloneCommand(), {"--help"}) || HasCommand("codex", {"--help"})) {
        return "codex";
    }
    if (HasCommand("opencode", {"--help"})) {
        return "opencode";
    }
    return {};
}

auto ResolveAiModel(const std::string& InProvider,
                    const std::string& InRequested,
                    const std::filesystem::path& InWorkspaceRoot) -> std::string {
    const auto directive = ResolveAiModelDirective(InProvider, InRequested, InWorkspaceRoot);
    if (directive == "auto") {
        return ResolveAutoModelByChangeCount(InProvider, InWorkspaceRoot);
    }
    if (directive == "provider-default") {
        return ResolveSystemRecommendedModel(InProvider);
    }
    return directive;
}

auto RunAiGenerate(const std::string& InProvider,
                   const std::string& InModel,
                   const std::string& InPrompt,
                   const std::filesystem::path& InWorkdir,
                   const bool InStructuredJsonOnly = false) -> shell::ExecResult {
    if (const char* forcedStdout = std::getenv("KOG_TEST_AI_STDOUT"); forcedStdout != nullptr ||
        std::getenv("KOG_TEST_AI_STDERR") != nullptr ||
        std::getenv("KOG_TEST_AI_EXIT_CODE") != nullptr) {
        shell::ExecResult forced;
        if (forcedStdout != nullptr) {
            forced.stdoutStr = forcedStdout;
        }
        if (const char* forcedStderr = std::getenv("KOG_TEST_AI_STDERR"); forcedStderr != nullptr) {
            forced.stderrStr = forcedStderr;
        }
        if (const char* forcedExit = std::getenv("KOG_TEST_AI_EXIT_CODE"); forcedExit != nullptr) {
            forced.exitCode = std::atoi(forcedExit);
        }
        return forced;
    }

    auto debugArgs = [&](const std::string& InCmd, const std::vector<std::string>& InArgs) {
        if (!IsTruthyEnv(std::getenv("KOG_DEBUG_AI_ARGS"))) {
            return;
        }
        std::ostringstream oss;
        oss << "[ai-debug] cmd=" << InCmd << " args=";
        for (std::size_t i = 0; i < InArgs.size(); ++i) {
            if (i != 0) {
                oss << " ";
            }
            oss << "\"" << InArgs[i] << "\"";
        }
        oss << "\n";
        std::cerr << oss.str();
    };

    if (InProvider == "opencode") {
        std::vector<std::string> args{"run"};
        AppendBoolFlag(&args, "KOG_OPENCODE_CONTINUE", "--continue");
        AppendSingleValueFlag(&args, "KOG_OPENCODE_SESSION", "--session");
        AppendBoolFlag(&args, "KOG_OPENCODE_FORK", "--fork");
        AppendSingleValueFlag(&args, "KOG_OPENCODE_AGENT", "--agent");
        AppendSingleValueFlag(&args, "KOG_OPENCODE_ATTACH", "--attach");
        AppendSingleValueFlag(&args, "KOG_OPENCODE_VARIANT", "--variant");
        AppendSingleValueFlag(&args, "KOG_OPENCODE_FORMAT", "--format");
        AppendBoolFlag(&args, "KOG_OPENCODE_THINKING", "--thinking");
        args.push_back("--dir");
        args.push_back(InWorkdir.lexically_normal().generic_string());
        if (!InModel.empty() && InModel != "auto") {
            args.push_back("--model");
            args.push_back(InModel);
        }
        args.push_back(InPrompt);
        return shell::ExecuteCommand("opencode", args, shell::ExecMode::Capture, InWorkdir);
    }
    if (InProvider == "codex") {
        const auto codexPrompt = BuildFileBackedPromptArgument(InWorkdir, InPrompt, InStructuredJsonOnly ? "plan-fill-structured" : "plan-fill");
        return ExecuteCodexExec(InWorkdir, codexPrompt, InStructuredJsonOnly ? "plan-fill-structured" : "plan-fill", InModel);
    }
    if (InProvider == "copilot") {
        const auto copilotPrompt = BuildFileBackedPromptArgument(
            InWorkdir,
            InPrompt,
            InStructuredJsonOnly ? "plan-fill-structured" : "plan-fill");
        if (HasStandaloneCopilotCommand()) {
            std::vector<std::string> args{"-s"};
            if (!InModel.empty() && InModel != "auto") {
                args.push_back("--model");
                args.push_back(InModel);
            }
            if (InStructuredJsonOnly) {
                args.push_back("--no-custom-instructions");
            }
            if (!InStructuredJsonOnly) {
                const bool autopilotEnabled = IsTruthyEnv(std::getenv("KOG_COPILOT_AUTOPILOT"));
                AppendBoolFlag(&args, "KOG_COPILOT_AUTOPILOT", "--autopilot");
                if (autopilotEnabled) {
                    AppendSingleValueFlagWithDefault(&args,
                                                     "KOG_COPILOT_MAX_AUTOPILOT_CONTINUES",
                                                     "--max-autopilot-continues",
                                                     "12");
                }
                AppendFlagOrSingleValueFlag(&args, "KOG_COPILOT_RESUME", "--resume");
                if (std::getenv("KOG_COPILOT_RESUME") == nullptr) {
                    AppendBoolFlag(&args, "KOG_COPILOT_CONTINUE", "--continue");
                }
                AppendSingleValueFlag(&args, "KOG_COPILOT_AGENT", "--agent");
                AppendRepeatableFlag(&args, "KOG_COPILOT_ADD_DIRS", "--add-dir");
                AppendRepeatableFlagWithDefaults(&args,
                                                 "KOG_COPILOT_ALLOW_TOOLS",
                                                 "--allow-tool",
                                                 {"shell(git:*)", "write"});
                AppendRepeatableFlag(&args, "KOG_COPILOT_ALLOW_URLS", "--allow-url");
                AppendRepeatableFlag(&args, "KOG_COPILOT_AVAILABLE_TOOLS", "--available-tools");
                AppendRepeatableFlag(&args, "KOG_COPILOT_EXCLUDED_TOOLS", "--excluded-tools");
                AppendBoolFlag(&args, "KOG_COPILOT_ALLOW_ALL_TOOLS", "--allow-all-tools");
                AppendBoolFlag(&args, "KOG_COPILOT_ALLOW_ALL_PATHS", "--allow-all-paths");
                AppendBoolFlag(&args, "KOG_COPILOT_ALLOW_ALL_URLS", "--allow-all-urls");
                AppendBoolFlag(&args, "KOG_COPILOT_ALLOW_ALL", "--allow-all");
            }
            args.insert(args.end(), {"--no-color", "--stream", "off", "--no-ask-user", "-p", copilotPrompt});
            debugArgs(CopilotStandaloneCommand(), args);
            return ExecuteStandaloneCopilot(args, InWorkdir);
        }
        if (HasCommand("gh", {"copilot", "--version"})) {
            std::vector<std::string> args{"copilot", "--", "-s"};
            if (!InModel.empty() && InModel != "auto") {
                args.push_back("--model");
                args.push_back(InModel);
            }
            if (InStructuredJsonOnly) {
                args.push_back("--no-custom-instructions");
            }
            if (!InStructuredJsonOnly) {
                const bool autopilotEnabled = IsTruthyEnv(std::getenv("KOG_COPILOT_AUTOPILOT"));
                AppendBoolFlag(&args, "KOG_COPILOT_AUTOPILOT", "--autopilot");
                if (autopilotEnabled) {
                    AppendSingleValueFlagWithDefault(&args,
                                                     "KOG_COPILOT_MAX_AUTOPILOT_CONTINUES",
                                                     "--max-autopilot-continues",
                                                     "12");
                }
                AppendFlagOrSingleValueFlag(&args, "KOG_COPILOT_RESUME", "--resume");
                if (std::getenv("KOG_COPILOT_RESUME") == nullptr) {
                    AppendBoolFlag(&args, "KOG_COPILOT_CONTINUE", "--continue");
                }
                AppendSingleValueFlag(&args, "KOG_COPILOT_AGENT", "--agent");
                AppendRepeatableFlag(&args, "KOG_COPILOT_ADD_DIRS", "--add-dir");
                AppendRepeatableFlagWithDefaults(&args,
                                                 "KOG_COPILOT_ALLOW_TOOLS",
                                                 "--allow-tool",
                                                 {"shell(git:*)", "write"});
                AppendRepeatableFlag(&args, "KOG_COPILOT_ALLOW_URLS", "--allow-url");
                AppendRepeatableFlag(&args, "KOG_COPILOT_AVAILABLE_TOOLS", "--available-tools");
                AppendRepeatableFlag(&args, "KOG_COPILOT_EXCLUDED_TOOLS", "--excluded-tools");
                AppendBoolFlag(&args, "KOG_COPILOT_ALLOW_ALL_TOOLS", "--allow-all-tools");
                AppendBoolFlag(&args, "KOG_COPILOT_ALLOW_ALL_PATHS", "--allow-all-paths");
                AppendBoolFlag(&args, "KOG_COPILOT_ALLOW_ALL_URLS", "--allow-all-urls");
                AppendBoolFlag(&args, "KOG_COPILOT_ALLOW_ALL", "--allow-all");
            }
            args.insert(args.end(), {"--no-color", "--stream", "off", "--no-ask-user", "-p", copilotPrompt});
            debugArgs("gh", args);
            return shell::ExecuteCommand("gh", args, shell::ExecMode::Capture, InWorkdir);
        }
    }
    return shell::ExecResult{.exitCode = 1, .stderrStr = "provider unavailable"};
}

auto CollectDirtyRepoContextText(const std::filesystem::path& InRoot) -> std::string {
    std::ostringstream out;
    const auto repos = DiscoverWorkspaceRepos(InRoot);
    for (const auto& repo : repos) {
        const auto status = GitCapture(repo, {"status", "--porcelain", "--untracked-files=all"});
        if (status.exitCode != 0 || Trim(status.stdoutStr).empty()) {
            continue;
        }
        const auto rel = RelativeDisplayPath(InRoot, repo);
        int lines = 0;
        std::istringstream iss(status.stdoutStr);
        std::string line;
        while (std::getline(iss, line)) {
            if (!Trim(line).empty()) {
                lines += 1;
            }
        }
        out << "repo: " << rel << "\n";
        out << "changes: " << lines << "\n";
        out << "status:\n";
        out << status.stdoutStr << "\n";
    }
    return out.str();
}

auto ReplaceAll(std::string InText, const std::string& InFrom, const std::string& InTo) -> std::string {
    if (InFrom.empty()) {
        return InText;
    }
    std::size_t pos = 0;
    while ((pos = InText.find(InFrom, pos)) != std::string::npos) {
        InText.replace(pos, InFrom.size(), InTo);
        pos += InTo.size();
    }
    return InText;
}

auto LoadPromptAssetText(const std::filesystem::path& InWorkspaceRoot,
                         const char* InEnvVar,
                         const std::filesystem::path& InRelativeAssetPath) -> std::optional<std::string> {
    std::vector<std::filesystem::path> candidates;
    if (InEnvVar != nullptr) {
        if (const char* custom = std::getenv(InEnvVar); custom != nullptr && std::string(custom).size() > 0) {
            candidates.emplace_back(std::filesystem::path(custom).lexically_normal());
        }
    }
    candidates.emplace_back((InWorkspaceRoot / InRelativeAssetPath).lexically_normal());
    candidates.emplace_back((ResolveSkillRoot(InWorkspaceRoot) / InRelativeAssetPath.filename()).lexically_normal());
    candidates.emplace_back((ResolveSkillRoot(InWorkspaceRoot) / InRelativeAssetPath).lexically_normal());

    for (const auto& candidate : candidates) {
        if (std::error_code ec; std::filesystem::exists(candidate, ec) && !ec) {
            if (const auto text = ReadFileText(candidate); text.has_value()) {
                return *text;
            }
        }
    }
    return std::nullopt;
}

auto BuildPlanPrompt(const std::filesystem::path& InRoot,
                     const std::string& InProvider,
                     const std::string& InModel,
                     const std::string& InTemplateJson,
                     const std::string& InDirtyContext) -> std::string {
    if (const auto text = LoadPromptAssetText(InRoot, "KOG_PLAN_PROMPT_TEMPLATE", std::filesystem::path("assets") / "prompts" / "base" / "plan-init.md");
        text.has_value()) {
        auto prompt = *text;
        prompt = ReplaceAll(std::move(prompt), "{{PROVIDER}}", InProvider);
        prompt = ReplaceAll(std::move(prompt), "{{MODEL}}", InModel);
        prompt = ReplaceAll(std::move(prompt), "{{TEMPLATE_JSON}}", InTemplateJson);
        prompt = ReplaceAll(std::move(prompt), "{{DIRTY_CONTEXT}}", InDirtyContext);
        return prompt;
    }
    std::ostringstream fallback;
    fallback << "You are generating a complete plan JSON for kano-git.\n";
    fallback << "Return STRICT JSON ONLY between markers:\nBEGIN_KOG_PLAN_JSON\n<json>\nEND_KOG_PLAN_JSON\n\n";
    fallback << "Template JSON:\n" << InTemplateJson << "\n\n";
    fallback << "Workspace dirty context:\n" << InDirtyContext << "\n";
    return fallback.str();
}

auto BuildPlanFillOpsPrompt(const std::filesystem::path& InRoot,
                            const std::string& InProvider,
                            const std::string& InModel,
                            const std::filesystem::path& InPlanPath,
                            const std::string& InPlanJson,
                            const std::string& InDirtyContext) -> std::string {
    const auto planPath = InPlanPath.lexically_normal();
    auto absolutePlanPath = planPath;
    if (!absolutePlanPath.is_absolute()) {
        absolutePlanPath = (InRoot / absolutePlanPath).lexically_normal();
    }
    std::error_code planPathError;
    absolutePlanPath = std::filesystem::absolute(absolutePlanPath, planPathError);
    if (planPathError) {
        absolutePlanPath = (InRoot / planPath).lexically_normal();
    }

    if (const auto text = LoadPromptAssetText(InRoot,
                                              "KOG_PLAN_FILL_SINGLE_PROMPT_TEMPLATE",
                                              std::filesystem::path("assets") / "prompts" / "base" / "plan-fill-single.md");
        text.has_value()) {
        auto prompt = *text;
        prompt = ReplaceAll(std::move(prompt), "{{PROVIDER}}", InProvider);
        prompt = ReplaceAll(std::move(prompt), "{{MODEL}}", InModel.empty() ? std::string("auto") : InModel);
        prompt = ReplaceAll(std::move(prompt), "{{PLAN_PATH}}", planPath.generic_string());
        prompt = ReplaceAll(std::move(prompt), "{{PLAN_PATH_ABSOLUTE}}", absolutePlanPath.string());
        prompt = ReplaceAll(std::move(prompt), "{{PLAN_JSON}}", InPlanJson);
        prompt = ReplaceAll(std::move(prompt), "{{DIRTY_CONTEXT}}", InDirtyContext);
        return prompt;
    }

    std::ostringstream prompt;
    prompt << "You are a focused kano-git subagent invoked for one task only.\n";
    prompt << "The task is already fully specified in this prompt. Do not ask what task to perform.\n";
    prompt << "Task: produce semantic fill-ops for every existing commit entry in the authoritative plan file.\n";
    prompt << "Use only the plan snapshot and dirty workspace context included below.\n";
    prompt << "The plan already exists. Do not rewrite it; only return fill-ops JSON.\n\n";
    prompt << "This subagent has no external conversation context beyond this prompt.\n";
    prompt << "Authoritative plan file absolute path: " << absolutePlanPath.string() << "\n";
    prompt << "Workspace-relative plan file path: " << planPath.generic_string() << "\n";
    prompt << "Do not ask clarifying questions. Do not ask for more instructions. Execute the specified fill task directly.\n";
    prompt << "The Current plan JSON below is the exact plan file content snapshot. Do not reopen it, search for it, or inspect similarly named files.\n\n";
    prompt << "Do not inspect files, do not explore the repository, do not propose a plan, and do not describe actions.\n";
    prompt << "Do not use tools even if the provider supports tools. All required context is already included below.\n\n";
    prompt << "Do not ask the user to restate the task. Do not say you are ready to help.\n";
    prompt << "Return STRICT JSON ONLY between markers:\n";
    prompt << "BEGIN_KOG_PLAN_FILL_OPS\n<json>\nEND_KOG_PLAN_FILL_OPS\n\n";
    prompt << "Required JSON schema:\n";
    prompt << "{\n";
    prompt << "  \"commits\": [\n";
    prompt << "    {\n";
    prompt << "      \"index\": 0,\n";
    prompt << "      \"message\": \"feat(scope): concise commit message\",\n";
    prompt << "      \"review\": {\n";
    prompt << "        \"verdict\": \"pass\",\n";
    prompt << "        \"reason\": \"Specific review rationale for this commit.\"\n";
    prompt << "      }\n";
    prompt << "    }\n";
    prompt << "  ]\n";
    prompt << "}\n\n";
    prompt << "Rules:\n";
    prompt << "- Output exactly one item for EVERY existing commit entry in stages.commit.\n";
    prompt << "- Do not omit any index.\n";
    prompt << "- Do not invent new indexes.\n";
    prompt << "- Index values must map exactly to existing entries in Current plan JSON.\n";
    prompt << "- Return the JSON object only; no prose, no markdown fences, no commentary.\n";
    prompt << "- index may be a JSON integer or string, but integer is preferred.\n";
    prompt << "- review.verdict must be pass.\n";
    prompt << "- Do not use placeholders like replace-with-*.\n";
    prompt << "- Do not modify include/exclude/repo; they are read-only context.\n";
    prompt << "- Provider=" << InProvider << " model=" << (InModel.empty() ? "auto" : InModel) << "\n\n";
    prompt << "Semantic quality constraints:\n";
    prompt << "- message must be concrete and commit-ready (no TODO/placeholder).\n";
    prompt << "- review.reason must be specific to that commit index and repo scope.\n";
    prompt << "- Do not mention hidden/external tools; only reason from provided plan+dirty context.\n\n";
    prompt << "Current plan JSON:\n" << InPlanJson << "\n\n";
    prompt << "Workspace dirty context:\n" << InDirtyContext << "\n";
    return prompt.str();
}

auto BuildFillOpsRetryPrompt(const std::string& InBasePrompt,
                             std::size_t InExpectedCommits,
                             const std::optional<std::size_t>& InReturnedCommits,
                             const std::string& InFailureCategory,
                             const std::string& InFailureDetail,
                             const std::string& InPreviousRaw) -> std::string {
    const auto workspaceRoot = std::filesystem::current_path().lexically_normal();
    const auto failureCategoryLine = Trim(InFailureCategory).empty()
        ? std::string{}
        : std::string("- Failure category: ") + InFailureCategory + "\n";
    const auto failureDetailLine = Trim(InFailureDetail).empty()
        ? std::string{}
        : std::string("- Failure detail: ") + CompactSingleLine(InFailureDetail, 220) + "\n";
    const auto returnedCommitsLine = InReturnedCommits.has_value()
        ? std::format("- Returned commits count: {}\\n", *InReturnedCommits)
        : std::string{};
    const auto rawSnippet = CompactSingleLine(InPreviousRaw, 1000);
    const auto previousRawSection = rawSnippet.empty()
        ? std::string{}
        : std::string("\nPrevious raw response snippet:\n") + rawSnippet + "\n";

    if (const auto text = LoadPromptAssetText(workspaceRoot,
                                              "KOG_PLAN_FILL_RETRY_PROMPT_TEMPLATE",
                                              std::filesystem::path("assets") / "prompts" / "base" / "plan-fill-retry.md");
        text.has_value()) {
        auto prompt = *text;
        prompt = ReplaceAll(std::move(prompt), "{{BASE_PROMPT}}", InBasePrompt);
        prompt = ReplaceAll(std::move(prompt), "{{FAILURE_CATEGORY_LINE}}", failureCategoryLine);
        prompt = ReplaceAll(std::move(prompt), "{{FAILURE_DETAIL_LINE}}", failureDetailLine);
        prompt = ReplaceAll(std::move(prompt), "{{EXPECTED_COMMITS}}", std::to_string(InExpectedCommits));
        prompt = ReplaceAll(std::move(prompt), "{{RETURNED_COMMITS_LINE}}", returnedCommitsLine);
        prompt = ReplaceAll(std::move(prompt), "{{PREVIOUS_RAW_SECTION}}", previousRawSection);
        return prompt;
    }

    std::ostringstream prompt;
    prompt << InBasePrompt;
    prompt << "\n\nRetry directive (mandatory):\n";
    prompt << "- Previous response was rejected by validator.\n";
    prompt << failureCategoryLine;
    prompt << failureDetailLine;
    prompt << std::format("- Expected commits count: {}\\n", InExpectedCommits);
    prompt << returnedCommitsLine;
    prompt << "- Re-output a COMPLETE commits array covering ALL indexes exactly once.\n";
    prompt << "- Output STRICT JSON only between BEGIN_KOG_PLAN_FILL_OPS / END_KOG_PLAN_FILL_OPS.\n";
    prompt << previousRawSection;
    return prompt.str();
}

auto ExtractPlanFillOpsJson(const std::string& InText) -> std::string {
    const std::string begin = "BEGIN_KOG_PLAN_FILL_OPS";
    const std::string end = "END_KOG_PLAN_FILL_OPS";
    const auto b = InText.find(begin);
    const auto e = InText.find(end);
    if (b != std::string::npos && e != std::string::npos && e > b + begin.size()) {
        return Trim(InText.substr(b + begin.size(), e - (b + begin.size())));
    }
    return ExtractJsonBetweenMarkers(InText);
}

auto ExtractJsonBetweenMarkers(const std::string& InText) -> std::string {
    const std::string begin = "BEGIN_KOG_PLAN_JSON";
    const std::string end = "END_KOG_PLAN_JSON";
    const auto b = InText.find(begin);
    const auto e = InText.find(end);
    if (b != std::string::npos && e != std::string::npos && e > b + begin.size()) {
        return Trim(InText.substr(b + begin.size(), e - (b + begin.size())));
    }
    const auto firstBrace = InText.find('{');
    const auto lastBrace = InText.rfind('}');
    if (firstBrace != std::string::npos && lastBrace != std::string::npos && lastBrace > firstBrace) {
        return InText.substr(firstBrace, lastBrace - firstBrace + 1);
    }
    return {};
}

auto ValidateAiPlanPayload(const std::string& InJson) -> bool {
    if (!ExtractObjectBodyForKey(InJson, "meta").has_value()) {
        return false;
    }
    const auto stages = ExtractObjectBodyForKey(InJson, "stages");
    if (!stages.has_value()) {
        return false;
    }
    if (!ExtractArrayBodyForKey(*stages, "commit").has_value() || !ExtractArrayBodyForKey(*stages, "post_sync").has_value()) {
        return false;
    }
    return true;
}

auto ValidateAiReadyPlan(const std::string& InJson, std::string* OutReason = nullptr) -> bool {
    if (!ValidateAiPlanPayload(InJson)) {
        if (OutReason != nullptr) {
            *OutReason = "schema invalid: missing meta/stages/commit/post_sync";
        }
        return false;
    }
    if (InJson.find("replace-with-") != std::string::npos) {
        if (OutReason != nullptr) {
            *OutReason = "placeholder value detected (replace-with-*)";
        }
        return false;
    }
    const auto stages = ExtractObjectBodyForKey(InJson, "stages");
    if (!stages.has_value()) {
        if (OutReason != nullptr) {
            *OutReason = "schema invalid: missing stages";
        }
        return false;
    }
    const auto commitArray = ExtractArrayBodyForKey(*stages, "commit").value_or(std::string{});
    std::size_t commitCount = 0;
    for (const auto& repoObj : SplitTopLevelObjects(commitArray)) {
        const auto commits = ExtractArrayBodyForKey(repoObj, "commits").value_or(std::string{});
        for (const auto& commitObj : SplitTopLevelObjects(commits)) {
            const auto message = ExtractStringField(commitObj, "message").value_or("");
            const auto review = ExtractObjectBodyForKey(commitObj, "review");
            const auto verdict = review.has_value() ? ExtractStringField(*review, "verdict").value_or("") : "";
            const auto reason = review.has_value() ? ExtractStringField(*review, "reason").value_or("") : "";
            if (Trim(message).empty() || Trim(reason).empty() || ToLower(Trim(verdict)) != "pass") {
                if (OutReason != nullptr) {
                    *OutReason = "commit item missing required message/review fields (verdict must be pass)";
                }
                return false;
            }
            commitCount += 1;
        }
    }
    if (commitCount == 0) {
        if (OutReason != nullptr) {
            *OutReason = "no commit entries in stages.commit";
        }
        return false;
    }
    return true;
}

auto BuildDeterministicPlanId(const std::filesystem::path& InWorkspaceRoot,
                              const std::string& InBaseHeadSha,
                              const std::string& InDirtyFingerprint) -> std::string {
    const auto rootKey = InWorkspaceRoot.lexically_normal().generic_string();
    return std::format("plan-{}-{}",
                       CurrentUtcCompact(),
                       Fnv1a64Hex(rootKey + "|" + InBaseHeadSha + "|" + InDirtyFingerprint).substr(0, 8));
}

auto DeterministicPlannerProvider() -> std::string {
    if (IsAgentModeEnabled()) {
        return "agent";
    }
    return "native";
}

auto DeterministicPlannerModel() -> std::string {
    if (IsAgentModeEnabled()) {
        return "external-agent";
    }
    return "deterministic";
}

auto DeterministicReviewReason() -> std::string {
    if (IsAgentModeEnabled()) {
        return "agent-ready deterministic plan bootstrap generated by native tooling";
    }
    return "deterministic plan bootstrap generated by native tooling";
}

auto BuildDefaultPlanTemplate(const std::filesystem::path& InWorkspaceRoot,
                              const std::optional<std::filesystem::path>& InDatasourceRoot = std::nullopt,
                              const std::optional<std::filesystem::path>& InDatasourceManifest = std::nullopt) -> std::string {
    const auto dsRootPath = InDatasourceRoot.value_or(
        (InWorkspaceRoot / ".agents" / "skills" / "kano" / "kano-git-master-skill" / "assets" / "ignore-sources").lexically_normal());
    const auto dsManifestPath = InDatasourceManifest.value_or(
        (dsRootPath / "local" / "datasource.manifest.json").lexically_normal());
    const auto dsRoot = dsRootPath.lexically_normal().generic_string();
    const auto dsManifest = dsManifestPath.lexically_normal().generic_string();
    const auto baseHeadSha = ComputeWorkspaceBaseHeadSha(InWorkspaceRoot);
    const auto dirtyFingerprint = ComputeWorkspaceDirtyFingerprint(InWorkspaceRoot);
    const auto planId = BuildDeterministicPlanId(InWorkspaceRoot, baseHeadSha, dirtyFingerprint);
    std::ostringstream oss;
    oss << R"json({
  "meta": {
    "schema_version": "3",
    "plan_id": ")json"
        << planId << R"json(",
    "generated_at_utc": ")json"
        << CurrentUtcIso8601() << R"json(",
    "executed_at_utc": "",
    "base_head_sha": ")json"
        << baseHeadSha << R"json(",
    "dirty_fingerprint_pre_ignore": ")json"
        << dirtyFingerprint << R"json(",
    "dirty_fingerprint": ")json"
        << dirtyFingerprint << R"json(",
    "planner": {
      "provider": ")json"
        << DeterministicPlannerProvider() << R"json(",
      "ai-model": ")json"
        << DeterministicPlannerModel() << R"json("
    },
    "review": {
      "verdict": "pass",
      "reason": ")json"
        << DeterministicReviewReason() << R"json("
    },
    "ignore_datasource": {
      "root": ")json"
        << dsRoot << R"json(",
      "manifest": ")json"
        << dsManifest << R"json(",
      "prefer_sources": ["kano-local-rules", "github-gitignore"]
    }
  },
  "stages": {
    "ignore": [],
    "commit": [],
    "post_sync": []
  }
})json";
    return oss.str();
}

auto UnescapeJsonString(std::string InValue) -> std::string {
    std::string out;
    out.reserve(InValue.size());
    for (std::size_t i = 0; i < InValue.size(); ++i) {
        const char ch = InValue[i];
        if (ch != '\\' || i + 1 >= InValue.size()) {
            out.push_back(ch);
            continue;
        }
        const char next = InValue[i + 1];
        switch (next) {
        case '\\': out.push_back('\\'); break;
        case '"': out.push_back('"'); break;
        case '/': out.push_back('/'); break;
        case 'n': out.push_back('\n'); break;
        case 'r': out.push_back('\r'); break;
        case 't': out.push_back('\t'); break;
        default:
            // Be permissive for AI-emitted JSON-like payloads (e.g. "\s" in Windows paths):
            // preserve the backslash instead of silently dropping it.
            out.push_back('\\');
            out.push_back(next);
            break;
        }
        i += 1;
    }
    return out;
}

auto SkipJsonWhitespace(const std::string& InText, std::size_t InPos) -> std::size_t {
    std::size_t pos = InPos;
    while (pos < InText.size()) {
        const char ch = InText[pos];
        if (ch != ' ' && ch != '\t' && ch != '\n' && ch != '\r') {
            break;
        }
        pos += 1;
    }
    return pos;
}

auto ParseJsonStringAt(const std::string& InText, std::size_t InPos) -> std::optional<std::pair<std::string, std::size_t>> {
    if (InPos >= InText.size() || InText[InPos] != '"') {
        return std::nullopt;
    }
    std::string raw;
    std::size_t pos = InPos + 1;
    while (pos < InText.size()) {
        const char ch = InText[pos];
        if (ch == '\\') {
            if (pos + 1 >= InText.size()) {
                return std::nullopt;
            }
            raw.push_back(ch);
            raw.push_back(InText[pos + 1]);
            pos += 2;
            continue;
        }
        if (ch == '"') {
            return std::make_pair(UnescapeJsonString(raw), pos + 1);
        }
        raw.push_back(ch);
        pos += 1;
    }
    return std::nullopt;
}

auto FindJsonKeyValueStart(const std::string& InText, const std::string& InKey, std::size_t InFrom = 0) -> std::optional<std::size_t> {
    std::size_t pos = InFrom;
    while (pos < InText.size()) {
        pos = InText.find('"', pos);
        if (pos == std::string::npos) {
            return std::nullopt;
        }
        const auto parsed = ParseJsonStringAt(InText, pos);
        if (!parsed.has_value()) {
            return std::nullopt;
        }
        const auto& [key, nextPos] = *parsed;
        pos = SkipJsonWhitespace(InText, nextPos);
        if (key == InKey && pos < InText.size() && InText[pos] == ':') {
            return SkipJsonWhitespace(InText, pos + 1);
        }
    }
    return std::nullopt;
}

auto ExtractBracketBody(const std::string& InText, std::size_t InStart, char InOpen, char InClose) -> std::optional<std::string> {
    if (InStart >= InText.size() || InText[InStart] != InOpen) {
        return std::nullopt;
    }
    bool inString = false;
    bool escaped = false;
    int depth = 0;
    for (std::size_t pos = InStart; pos < InText.size(); ++pos) {
        const char ch = InText[pos];
        if (inString) {
            if (escaped) {
                escaped = false;
            } else if (ch == '\\') {
                escaped = true;
            } else if (ch == '"') {
                inString = false;
            }
            continue;
        }
        if (ch == '"') {
            inString = true;
            continue;
        }
        if (ch == InOpen) {
            depth += 1;
            continue;
        }
        if (ch == InClose) {
            depth -= 1;
            if (depth == 0) {
                return InText.substr(InStart + 1, pos - InStart - 1);
            }
        }
    }
    return std::nullopt;
}

auto ExtractObjectBodyForKey(const std::string& InText, const std::string& InKey) -> std::optional<std::string> {
    const auto valuePos = FindJsonKeyValueStart(InText, InKey);
    if (!valuePos.has_value()) {
        return std::nullopt;
    }
    return ExtractBracketBody(InText, *valuePos, '{', '}');
}

auto ExtractArrayBodyForKey(const std::string& InText, const std::string& InKey) -> std::optional<std::string> {
    const auto valuePos = FindJsonKeyValueStart(InText, InKey);
    if (!valuePos.has_value()) {
        return std::nullopt;
    }
    return ExtractBracketBody(InText, *valuePos, '[', ']');
}

auto SplitTopLevelObjects(const std::string& InArrayBody) -> std::vector<std::string> {
    std::vector<std::string> objects;
    bool inString = false;
    bool escaped = false;
    int depth = 0;
    std::size_t objectStart = std::string::npos;
    for (std::size_t pos = 0; pos < InArrayBody.size(); ++pos) {
        const char ch = InArrayBody[pos];
        if (inString) {
            if (escaped) {
                escaped = false;
            } else if (ch == '\\') {
                escaped = true;
            } else if (ch == '"') {
                inString = false;
            }
            continue;
        }
        if (ch == '"') {
            inString = true;
            continue;
        }
        if (ch == '{') {
            if (depth == 0) {
                objectStart = pos;
            }
            depth += 1;
            continue;
        }
        if (ch == '}') {
            depth -= 1;
            if (depth == 0 && objectStart != std::string::npos) {
                objects.push_back(InArrayBody.substr(objectStart, pos - objectStart + 1));
                objectStart = std::string::npos;
            }
        }
    }
    return objects;
}

auto ExtractStringField(const std::string& InObjectText, const std::string& InField) -> std::optional<std::string> {
    const auto valuePos = FindJsonKeyValueStart(InObjectText, InField);
    if (!valuePos.has_value()) {
        return std::nullopt;
    }
    const auto parsed = ParseJsonStringAt(InObjectText, *valuePos);
    if (!parsed.has_value()) {
        return std::nullopt;
    }
    return parsed->first;
}

auto ExtractScalarFieldToken(const std::string& InObjectText, const std::string& InField) -> std::optional<std::string> {
    const auto valuePos = FindJsonKeyValueStart(InObjectText, InField);
    if (!valuePos.has_value()) {
        return std::nullopt;
    }

    if (*valuePos < InObjectText.size() && InObjectText[*valuePos] == '"') {
        return ExtractStringField(InObjectText, InField);
    }

    std::size_t end = *valuePos;
    while (end < InObjectText.size()) {
        const char ch = InObjectText[end];
        if (ch == ',' || ch == '}' || ch == ']' || std::isspace(static_cast<unsigned char>(ch))) {
            break;
        }
        end += 1;
    }

    const auto token = Trim(InObjectText.substr(*valuePos, end - *valuePos));
    if (token.empty()) {
        return std::nullopt;
    }
    return token;
}

auto ExtractBoolField(const std::string& InObjectText, const std::string& InField) -> std::optional<bool> {
    const auto valuePos = FindJsonKeyValueStart(InObjectText, InField);
    if (!valuePos.has_value()) {
        return std::nullopt;
    }
    if (InObjectText.compare(*valuePos, 4, "true") == 0) {
        return true;
    }
    if (InObjectText.compare(*valuePos, 5, "false") == 0) {
        return false;
    }
    return std::nullopt;
}

auto ResolveDatasourceSourcePath(const std::filesystem::path& InManifestPath, const std::string& InSourcePath) -> std::filesystem::path {
    const std::filesystem::path candidate(NormalizeInputPathForCurrentPlatform(InSourcePath));
    if (candidate.is_absolute()) {
        return candidate.lexically_normal();
    }
    return (InManifestPath.parent_path() / candidate).lexically_normal();
}

auto ParseIgnoreDatasourceManifest(const std::filesystem::path& InManifestPath,
                                   std::string* OutError = nullptr) -> std::vector<IgnoreDatasourceSource> {
    std::vector<IgnoreDatasourceSource> out;
    const auto payload = ReadFileText(InManifestPath);
    if (!payload.has_value()) {
        if (OutError != nullptr) {
            *OutError = "manifest file not found/readable";
        }
        return out;
    }

    const auto sourcesArray = ExtractArrayBodyForKey(*payload, "sources");
    if (!sourcesArray.has_value()) {
        if (OutError != nullptr) {
            *OutError = "schema invalid: missing sources array";
        }
        return out;
    }

    for (const auto& sourceObj : SplitTopLevelObjects(*sourcesArray)) {
        IgnoreDatasourceSource source;
        source.id = Trim(ExtractStringField(sourceObj, "id").value_or(""));
        source.kind = Trim(ExtractStringField(sourceObj, "kind").value_or(""));
        source.pathRaw = Trim(ExtractStringField(sourceObj, "path").value_or(""));
        source.enabled = ExtractBoolField(sourceObj, "enabled").value_or(true);
        if (source.id.empty() || source.pathRaw.empty()) {
            if (OutError != nullptr) {
                *OutError = "schema invalid: each source must have id and path";
            }
            return {};
        }
        source.resolvedPath = ResolveDatasourceSourcePath(InManifestPath, source.pathRaw);
        out.push_back(std::move(source));
    }
    return out;
}

auto ParseIgnoreEntries(const std::string& InText) -> std::vector<IgnoreStageEntry> {
    std::vector<IgnoreStageEntry> out;
    const auto stages = ExtractObjectBodyForKey(InText, "stages");
    if (!stages.has_value()) {
        return out;
    }
    const auto ignoreArray = ExtractArrayBodyForKey(*stages, "ignore");
    if (!ignoreArray.has_value()) {
        return out;
    }
    for (const auto& item : SplitTopLevelObjects(*ignoreArray)) {
        IgnoreStageEntry entry;
        if (const auto repo = ExtractStringField(item, "repo"); repo.has_value()) {
            entry.repo = Trim(*repo);
        }
        if (const auto target = ExtractStringField(item, "apply_target"); target.has_value()) {
            entry.applyTarget = Trim(*target);
        }
        if (const auto merged = ExtractStringField(item, "merged_output_path"); merged.has_value()) {
            entry.mergedOutputPath = Trim(*merged);
        }
        if (const auto candidates = ExtractArrayBodyForKey(item, "candidates"); candidates.has_value()) {
            for (const auto& c : SplitTopLevelObjects(*candidates)) {
                if (const auto rule = ExtractStringField(c, "rule"); rule.has_value()) {
                    const auto v = Trim(*rule);
                    if (!v.empty()) {
                        entry.rules.push_back(v);
                    }
                }
            }
        }
        out.push_back(std::move(entry));
    }
    return out;
}

auto CountTopLevelObjects(const std::string& InArrayBody) -> std::size_t {
    return SplitTopLevelObjects(InArrayBody).size();
}

auto Fnv1a64Hex(const std::string& InText) -> std::string {
    std::uint64_t hash = 1469598103934665603ULL;
    for (const unsigned char ch : InText) {
        hash ^= static_cast<std::uint64_t>(ch);
        hash *= 1099511628211ULL;
    }
    std::ostringstream oss;
    oss << std::hex << std::setfill('0') << std::setw(16) << hash;
    return oss.str();
}

auto ExtractBranchOidFromStatusV2(const std::string& InStatus) -> std::string {
    std::istringstream iss(InStatus);
    std::string line;
    while (std::getline(iss, line)) {
        auto t = Trim(line);
        if (t.rfind("# branch.oid ", 0) == 0) {
            t = Trim(t.substr(std::string("# branch.oid ").size()));
            if (!t.empty() && t != "(initial)") {
                return t;
            }
            break;
        }
    }
    return "no-head";
}

auto ComputeWorkspaceBaseHeadSha(const std::filesystem::path& InWorkspaceRoot) -> std::string {
    std::vector<std::string> lines;
    const auto repos = DiscoverWorkspaceRepos(InWorkspaceRoot);
    lines.reserve(repos.size());
    for (const auto& repo : repos) {
        const auto rel = RelativeDisplayPath(InWorkspaceRoot, repo);
        const auto key = rel.empty() ? "." : rel;
        const auto head = GitCapture(repo, {"rev-parse", "HEAD"});
        const auto sha = (head.exitCode == 0) ? Trim(head.stdoutStr) : std::string("0000000000000000000000000000000000000000");
        lines.push_back(key + "\t" + sha);
    }
    std::sort(lines.begin(), lines.end());
    std::ostringstream canonical;
    for (const auto& line : lines) {
        canonical << line << "\n";
    }
    return "ws-head-v2-" + Fnv1a64Hex(canonical.str());
}

auto ComputeWorkspaceDirtyFingerprint(const std::filesystem::path& InWorkspaceRoot) -> std::string {
    std::vector<std::string> lines;
    const auto repos = DiscoverWorkspaceRepos(InWorkspaceRoot);
    lines.reserve(repos.size());
    for (const auto& repo : repos) {
        const auto rel = RelativeDisplayPath(InWorkspaceRoot, repo);
        const auto key = rel.empty() ? "." : rel;
        const auto status = GitCapture(repo, {"status", "--porcelain=v2", "--branch", "--untracked-files=normal", "--ignore-submodules=none"});
        if (status.exitCode != 0) {
            continue;
        }
        const auto normalized = Trim(status.stdoutStr);
        const auto head = ExtractBranchOidFromStatusV2(normalized);
        const auto statusFingerprint = normalized.empty() ? std::string("clean") : Fnv1a64Hex(normalized);
        lines.push_back(std::format("{}|{}|{}", key, head, statusFingerprint));
    }
    std::sort(lines.begin(), lines.end());
    std::ostringstream canonical;
    for (const auto& line : lines) {
        canonical << line << "\n";
    }
    return "ws-dirty-v2-" + Fnv1a64Hex(canonical.str());
}

auto IsPlaceholderPlanValue(const std::string& InValue) -> bool {
    const auto value = Trim(InValue);
    return value.empty() || value.rfind("replace-with-", 0) == 0;
}

auto ExtractPlanWorkspaceHashes(const std::string& InPlanText, std::string* OutBaseHeadSha, std::string* OutDirtyFingerprint) -> bool {
    const auto meta = ExtractObjectBodyForKey(InPlanText, "meta");
    if (!meta.has_value()) {
        return false;
    }
    const auto baseHeadSha = Trim(ExtractStringField(*meta, "base_head_sha").value_or(""));
    const auto dirtyFingerprint = Trim(ExtractStringField(*meta, "dirty_fingerprint").value_or(""));
    if (IsPlaceholderPlanValue(baseHeadSha) || IsPlaceholderPlanValue(dirtyFingerprint)) {
        return false;
    }
    if (OutBaseHeadSha != nullptr) {
        *OutBaseHeadSha = baseHeadSha;
    }
    if (OutDirtyFingerprint != nullptr) {
        *OutDirtyFingerprint = dirtyFingerprint;
    }
    return true;
}

auto PlanNeedsRefresh(const std::string& InPlanText) -> bool {
    if (Trim(InPlanText).empty()) {
        return true;
    }
    if (InPlanText.find("replace-with-") != std::string::npos) {
        return true;
    }
    return false;
}

auto PlanWorkspaceStateDrifted(const std::filesystem::path& InWorkspaceRoot, const std::string& InPlanText) -> bool {
    std::string planBaseHeadSha;
    std::string planDirtyFingerprint;
    if (!ExtractPlanWorkspaceHashes(InPlanText, &planBaseHeadSha, &planDirtyFingerprint)) {
        // Missing/placeholder workspace hashes are treated as schema/template refresh signals elsewhere.
        return false;
    }
    const auto currentBaseHeadSha = ComputeWorkspaceBaseHeadSha(InWorkspaceRoot);
    const auto currentDirtyFingerprint = ComputeWorkspaceDirtyFingerprint(InWorkspaceRoot);
    return planBaseHeadSha != currentBaseHeadSha || planDirtyFingerprint != currentDirtyFingerprint;
}

auto JsonEscape(std::string InValue) -> std::string {
    std::string out;
    out.reserve(InValue.size() + 8);
    for (const char ch : InValue) {
        switch (ch) {
        case '\\': out += "\\\\"; break;
        case '"': out += "\\\""; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default: out.push_back(ch); break;
        }
    }
    return out;
}

auto CountNonEmptyLines(const std::string& InText) -> int {
    int lines = 0;
    std::istringstream iss(InText);
    std::string line;
    while (std::getline(iss, line)) {
        if (!Trim(line).empty()) {
            lines += 1;
        }
    }
    return lines;
}

auto ReplaceArrayBodyForKey(const std::string& InText, const std::string& InKey, const std::string& InNewBody) -> std::optional<std::string>;
auto CountTopLevelObjects(const std::string& InArrayBody) -> std::size_t;

auto BuildFallbackCommitScope(const std::string& InRepoDisplay) -> std::string {
    if (InRepoDisplay.empty() || InRepoDisplay == ".") {
        return "workspace";
    }
    std::string scope = InRepoDisplay;
    std::replace(scope.begin(), scope.end(), '\\', '/');
    for (auto& ch : scope) {
        if (ch == '/' || ch == ' ') {
            ch = '-';
        }
    }
    return scope;
}

auto BuildCommitSeedEntriesJson(const std::filesystem::path& InWorkspaceRoot, const bool InUsePlaceholders) -> std::string {
    std::vector<std::string> entries;
    const auto repos = DiscoverWorkspaceRepos(InWorkspaceRoot);
    entries.reserve(repos.size());
    for (const auto& repo : repos) {
        const auto status = GitCapture(repo, {"status", "--porcelain", "--untracked-files=all"});
        if (status.exitCode != 0 || Trim(status.stdoutStr).empty()) {
            continue;
        }
        const auto repoDisplay = RelativeDisplayPath(InWorkspaceRoot, repo);
        const auto scope = BuildFallbackCommitScope(repoDisplay);
        const int changed = std::max(1, CountNonEmptyLines(status.stdoutStr));
        const auto message = InUsePlaceholders
                                 ? std::string("replace-with-commit-message")
                                 : std::format("chore({}): apply workspace updates ({} file{})",
                                               scope,
                                               changed,
                                               changed == 1 ? "" : "s");
        const auto reviewReason = InUsePlaceholders
                                      ? std::string("replace-with-review-reason-for-this-commit")
                                      : std::string("seeded by plan commit-seed from current dirty status");
        const auto repoJson = repoDisplay.empty() ? "." : repoDisplay;
        entries.push_back(
            std::format("{{\"repo\":\"{}\",\"commits\":[{{\"message\":\"{}\",\"include\":[],\"exclude\":[],\"review\":{{\"verdict\":\"pass\",\"reason\":\"{}\"}}}}]}}",
                        JsonEscape(repoJson),
                        JsonEscape(message),
                        JsonEscape(reviewReason)));
    }
    std::ostringstream oss;
    for (std::size_t i = 0; i < entries.size(); ++i) {
        if (i != 0) {
            oss << ",";
        }
        oss << entries[i];
    }
    return oss.str();
}

auto HasValidCommitItems(const std::string& InPlanText) -> bool {
    const auto stages = ExtractObjectBodyForKey(InPlanText, "stages");
    if (!stages.has_value()) {
        return false;
    }
    const auto commitArray = ExtractArrayBodyForKey(*stages, "commit").value_or(std::string{});
    for (const auto& repoObj : SplitTopLevelObjects(commitArray)) {
        if (const auto commits = ExtractArrayBodyForKey(repoObj, "commits"); commits.has_value()) {
            if (CountTopLevelObjects(*commits) > 0) {
                return true;
            }
        }
    }
    return false;
}

auto SeedCommitStage(const std::filesystem::path& InWorkspaceRoot,
                     const std::string& InPlanText,
                     const bool InForce,
                     const bool InUsePlaceholders) -> std::optional<std::string> {
    if (!InForce && HasValidCommitItems(InPlanText)) {
        return std::nullopt;
    }
    const auto body = BuildCommitSeedEntriesJson(InWorkspaceRoot, InUsePlaceholders);
    auto updated = ReplaceArrayBodyForKey(InPlanText, "commit", body);
    if (!updated.has_value()) {
        return std::nullopt;
    }
    if (!InUsePlaceholders && updated->find("replace-with-") != std::string::npos) {
        updated = BuildDefaultPlanTemplate(InWorkspaceRoot);
        updated = ReplaceArrayBodyForKey(*updated, "commit", body);
    }
    return updated;
}

auto BuildFallbackCommitEntriesJson(const std::filesystem::path& InWorkspaceRoot) -> std::string {
    return BuildCommitSeedEntriesJson(InWorkspaceRoot, false);
}

auto BuildDeterministicCommitFillOps(const std::vector<CommitPlanEntry>& InEntries) -> std::vector<CommitFillOp> {
    std::vector<CommitFillOp> out;
    out.reserve(InEntries.size());
    for (const auto& entry : InEntries) {
        const auto repoDisplay = entry.repo.empty() ? std::string(".") : entry.repo;
        const auto scope = BuildFallbackCommitScope(repoDisplay);
        CommitFillOp op;
        op.index = entry.index;
        op.message = std::format("chore({}): apply planned updates", scope);
        op.reviewVerdict = "pass";
        op.reviewReason = std::format("deterministic fallback fill for repo {} (index {}) after AI fill-op failure",
                                      repoDisplay,
                                      entry.index);
        out.push_back(std::move(op));
    }
    return out;
}

auto BuildDeterministicCommitFillOp(const CommitPlanEntry& InEntry) -> CommitFillOp {
    const auto repoDisplay = InEntry.repo.empty() ? std::string(".") : InEntry.repo;
    const auto scope = BuildFallbackCommitScope(repoDisplay);
    CommitFillOp op;
    op.index = InEntry.index;
    op.message = std::format("chore({}): apply planned updates", scope);
    op.reviewVerdict = "pass";
    op.reviewReason = std::format("deterministic fallback fill for repo {} (index {})", repoDisplay, InEntry.index);
    return op;
}

auto ResolvePlanCommitGenerationMode(const std::filesystem::path& InWorkspaceRoot,
                                     const std::string& InRequestedMode) -> std::string {
    if (const auto direct = kog_config::NormalizePlanCommitGenerationMode(InRequestedMode); !direct.empty()) {
        return direct;
    }
    if (const char* envMode = std::getenv("KOG_PLAN_FILL_MODE"); envMode != nullptr) {
        if (const auto envResolved = kog_config::NormalizePlanCommitGenerationMode(envMode); !envResolved.empty()) {
            return envResolved;
        }
    }
    if (const char* envMode = std::getenv("KOG_PLAN_COMMIT_GENERATION_MODE"); envMode != nullptr) {
        if (const auto envResolved = kog_config::NormalizePlanCommitGenerationMode(envMode); !envResolved.empty()) {
            return envResolved;
        }
    }
    return kog_config::ResolvePlanCommitGenerationMode(InWorkspaceRoot, ResolveSkillRoot(InWorkspaceRoot), "single");
}

auto AllowDeterministicCommitFallbackForMode(const std::string& InFillMode) -> bool {
    const auto fillMode = ToLower(Trim(InFillMode));
    if (fillMode == "single" && !IsAgentModeEnabled()) {
        return false;
    }
    return true;
}

auto ResolveRepoPathFromDisplay(const std::filesystem::path& InWorkspaceRoot, const std::string& InRepoDisplay) -> std::filesystem::path {
    if (InRepoDisplay.empty() || InRepoDisplay == ".") {
        return InWorkspaceRoot;
    }
    return (InWorkspaceRoot / InRepoDisplay).lexically_normal();
}

auto ParseStatusPathFromPorcelainLine(const std::string& InLine) -> std::string {
    if (InLine.size() < 4) {
        return {};
    }
    auto path = Trim(InLine.substr(3));
    const auto renamePos = path.find(" -> ");
    if (renamePos != std::string::npos) {
        path = Trim(path.substr(renamePos + 4));
    }
    return path;
}

auto IsGitlinkPath(const std::filesystem::path& InRepoPath, const std::string& InPath) -> bool {
    if (Trim(InPath).empty()) {
        return false;
    }
    const auto staged = GitCapture(InRepoPath, {"ls-files", "--stage", "--", InPath});
    if (staged.exitCode != 0) {
        return false;
    }
    std::istringstream iss(staged.stdoutStr);
    std::string mode;
    iss >> mode;
    return mode == "160000";
}

auto RepoHasGitlinkOnlyChanges(const std::filesystem::path& InRepoPath) -> bool {
    const auto status = GitCapture(InRepoPath, {"status", "--porcelain", "--untracked-files=all"});
    if (status.exitCode != 0 || Trim(status.stdoutStr).empty()) {
        return false;
    }
    bool hasChange = false;
    std::istringstream iss(status.stdoutStr);
    std::string line;
    while (std::getline(iss, line)) {
        const auto path = ParseStatusPathFromPorcelainLine(line);
        if (Trim(path).empty()) {
            continue;
        }
        hasChange = true;
        if (!IsGitlinkPath(InRepoPath, path)) {
            return false;
        }
    }
    return hasChange;
}

auto BuildSingleCommitFillPrompt(const std::filesystem::path& InWorkspaceRoot,
                                 const std::string& InProvider,
                                 const std::string& InModel,
                                 const CommitPlanEntry& InEntry,
                                 const std::string& InDirtyContext) -> std::string {
    std::ostringstream targetEntry;
    targetEntry << "{\n";
    targetEntry << "  \"index\": " << InEntry.index << ",\n";
    targetEntry << "  \"repo\": \"" << JsonEscape(InEntry.repo.empty() ? "." : InEntry.repo) << "\",\n";
    targetEntry << "  \"message\": \"" << JsonEscape(InEntry.message) << "\",\n";
    targetEntry << "  \"review\": {\"verdict\": \"" << JsonEscape(InEntry.reviewVerdict) << "\", \"reason\": \""
                << JsonEscape(InEntry.reviewReason) << "\"}\n";
    targetEntry << "}\n";

    if (const auto text = LoadPromptAssetText(InWorkspaceRoot,
                                              "KOG_PLAN_FILL_PER_COMMIT_PROMPT_TEMPLATE",
                                              std::filesystem::path("assets") / "prompts" / "base" / "plan-fill-per-commit.md");
        text.has_value()) {
        auto prompt = *text;
        prompt = ReplaceAll(std::move(prompt), "{{PROVIDER}}", InProvider);
        prompt = ReplaceAll(std::move(prompt), "{{MODEL}}", InModel.empty() ? std::string("auto") : InModel);
        prompt = ReplaceAll(std::move(prompt), "{{ENTRY_INDEX}}", std::to_string(InEntry.index));
        prompt = ReplaceAll(std::move(prompt), "{{TARGET_ENTRY_JSON}}", targetEntry.str());
        prompt = ReplaceAll(std::move(prompt), "{{DIRTY_CONTEXT}}", InDirtyContext);
        return prompt;
    }

    std::ostringstream prompt;
    prompt << "You are filling exactly ONE commit plan entry for kano-git.\\n";
    prompt << "Return STRICT JSON ONLY between markers:\\nBEGIN_KOG_PLAN_FILL_OPS\\n<json>\\nEND_KOG_PLAN_FILL_OPS\\n\\n";
    prompt << "Rules:\\n";
    prompt << "- Output exactly one commits item for index " << InEntry.index << ".\\n";
    prompt << "- Do not output any other index.\\n";
    prompt << "- review.verdict must be pass.\\n";
    prompt << "- No prose, no markdown fences, no commentary.\\n";
    prompt << "- Provider=" << InProvider << " model=" << (InModel.empty() ? "auto" : InModel) << "\\n\\n";
    prompt << "Target commit entry:\\n" << targetEntry.str() << "\\n";
    prompt << "Workspace dirty context:\\n" << InDirtyContext << "\\n";
    return prompt.str();
}

auto ReplaceArrayBodyForKey(const std::string& InText, const std::string& InKey, const std::string& InNewBody) -> std::optional<std::string> {
    const auto valuePos = FindJsonKeyValueStart(InText, InKey);
    if (!valuePos.has_value() || *valuePos >= InText.size() || InText[*valuePos] != '[') {
        return std::nullopt;
    }

    const std::size_t start = *valuePos;
    bool inString = false;
    bool escaped = false;
    int depth = 0;
    for (std::size_t pos = start; pos < InText.size(); ++pos) {
        const char ch = InText[pos];
        if (inString) {
            if (escaped) {
                escaped = false;
            } else if (ch == '\\') {
                escaped = true;
            } else if (ch == '"') {
                inString = false;
            }
            continue;
        }
        if (ch == '"') {
            inString = true;
            continue;
        }
        if (ch == '[') {
            depth += 1;
            continue;
        }
        if (ch == ']') {
            depth -= 1;
            if (depth == 0) {
                std::string out;
                out.reserve(InText.size() + InNewBody.size() + 8);
                out.append(InText.substr(0, start + 1));
                out.append(InNewBody);
                out.append(InText.substr(pos));
                return out;
            }
        }
    }
    return std::nullopt;
}

auto TryInjectFallbackCommits(const std::filesystem::path& InWorkspaceRoot, const std::string& InPlanText) -> std::optional<std::string> {
    const auto stages = ExtractObjectBodyForKey(InPlanText, "stages");
    if (!stages.has_value()) {
        return std::nullopt;
    }
    if (HasValidCommitItems(InPlanText)) {
        return std::nullopt;
    }
    const auto fallbackBody = BuildFallbackCommitEntriesJson(InWorkspaceRoot);
    if (Trim(fallbackBody).empty()) {
        return std::nullopt;
    }
    return ReplaceArrayBodyForKey(InPlanText, "commit", fallbackBody);
}

auto BuildJsonStringArray(const std::vector<std::string>& InValues) -> std::string {
    std::ostringstream oss;
    for (std::size_t i = 0; i < InValues.size(); ++i) {
        if (i != 0) {
            oss << ",";
        }
        oss << "\"" << JsonEscape(InValues[i]) << "\"";
    }
    return oss.str();
}

auto RebuildCommitArrayBody(const std::vector<std::string>& InRepoObjects) -> std::string {
    std::ostringstream commitBody;
    for (std::size_t i = 0; i < InRepoObjects.size(); ++i) {
        if (i != 0) {
            commitBody << ",";
        }
        commitBody << InRepoObjects[i];
    }
    return commitBody.str();
}

auto BuildCommitObjectJson(const std::string& InMessage,
                          const std::string& InIncludeArrayBody,
                          const std::string& InExcludeArrayBody,
                          const std::string& InReviewVerdict,
              const std::string& InReviewReason,
              const std::string& InPlannerProvider,
              const std::string& InPlannerModel) -> std::string {
    return std::format(
    "{{\"message\":\"{}\",\"include\":[{}],\"exclude\":[{}],\"review\":{{\"verdict\":\"{}\",\"reason\":\"{}\"}},\"planner\":{{\"provider\":\"{}\",\"ai-model\":\"{}\"}}}}",
        JsonEscape(InMessage),
        InIncludeArrayBody,
        InExcludeArrayBody,
        JsonEscape(InReviewVerdict),
    JsonEscape(InReviewReason),
    JsonEscape(InPlannerProvider),
    JsonEscape(InPlannerModel));
}

auto ParseJsonStringArrayBody(const std::string& InArrayBody) -> std::vector<std::string> {
    std::vector<std::string> out;
    std::size_t pos = 0;
    while (pos < InArrayBody.size()) {
        pos = InArrayBody.find('"', pos);
        if (pos == std::string::npos) {
            break;
        }
        const auto parsed = ParseJsonStringAt(InArrayBody, pos);
        if (!parsed.has_value()) {
            break;
        }
        out.push_back(parsed->first);
        pos = parsed->second;
    }
    return out;
}

auto CollectCommitPlanEntries(const std::string& InPlanText) -> std::vector<CommitPlanEntry> {
    std::vector<CommitPlanEntry> out;
    const auto stages = ExtractObjectBodyForKey(InPlanText, "stages");
    if (!stages.has_value()) {
        return out;
    }
    const auto commitArray = ExtractArrayBodyForKey(*stages, "commit");
    if (!commitArray.has_value()) {
        return out;
    }

    int flatIndex = 0;
    for (const auto& repoObj : SplitTopLevelObjects(*commitArray)) {
        const auto repo = ExtractStringField(repoObj, "repo").value_or(".");
        const auto commits = ExtractArrayBodyForKey(repoObj, "commits").value_or(std::string{});
        for (const auto& commitObj : SplitTopLevelObjects(commits)) {
            const auto reviewObj = ExtractObjectBodyForKey(commitObj, "review");
            const auto includeBody = ExtractArrayBodyForKey(commitObj, "include").value_or(std::string{});
            const auto excludeBody = ExtractArrayBodyForKey(commitObj, "exclude").value_or(std::string{});
            CommitPlanEntry entry;
            entry.index = flatIndex;
            entry.repo = repo;
            entry.message = ExtractStringField(commitObj, "message").value_or("");
            entry.include = ParseJsonStringArrayBody(includeBody);
            entry.exclude = ParseJsonStringArrayBody(excludeBody);
            entry.reviewVerdict = reviewObj.has_value() ? ExtractStringField(*reviewObj, "verdict").value_or("") : "";
            entry.reviewReason = reviewObj.has_value() ? ExtractStringField(*reviewObj, "reason").value_or("") : "";
            out.push_back(std::move(entry));
            flatIndex += 1;
        }
    }
    return out;
}

auto CommitEntryNeedsReview(const CommitPlanEntry& InEntry) -> bool {
    if (Trim(InEntry.message).empty() || Trim(InEntry.reviewVerdict).empty() || Trim(InEntry.reviewReason).empty()) {
        return true;
    }
    if (InEntry.message.find("replace-with-") != std::string::npos || InEntry.reviewReason.find("replace-with-") != std::string::npos) {
        return true;
    }
    if (ToLower(Trim(InEntry.reviewVerdict)) != "pass") {
        return true;
    }
    return false;
}

auto CollectCommitIndexesNeedingReview(const std::vector<CommitPlanEntry>& InEntries) -> std::vector<int> {
    std::vector<int> out;
    for (const auto& entry : InEntries) {
        if (CommitEntryNeedsReview(entry)) {
            out.push_back(entry.index);
        }
    }
    return out;
}

auto FindCommitEntryByFlatIndex(const std::string& InPlanText,
                                int InCommitIndex,
                                std::string* OutError = nullptr) -> std::optional<CommitPlanEntry> {
    if (InCommitIndex < 0) {
        if (OutError != nullptr) {
            *OutError = "--index must be >= 0";
        }
        return std::nullopt;
    }
    const auto stages = ExtractObjectBodyForKey(InPlanText, "stages");
    if (!stages.has_value()) {
        if (OutError != nullptr) {
            *OutError = "schema invalid: missing stages";
        }
        return std::nullopt;
    }
    const auto commitArray = ExtractArrayBodyForKey(*stages, "commit");
    if (!commitArray.has_value()) {
        if (OutError != nullptr) {
            *OutError = "schema invalid: missing stages.commit";
        }
        return std::nullopt;
    }

    int flatIndex = 0;
    for (const auto& repoObj : SplitTopLevelObjects(*commitArray)) {
        const auto repo = ExtractStringField(repoObj, "repo").value_or(".");
        const auto commits = ExtractArrayBodyForKey(repoObj, "commits").value_or(std::string{});
        for (const auto& commitObj : SplitTopLevelObjects(commits)) {
            if (flatIndex != InCommitIndex) {
                flatIndex += 1;
                continue;
            }
            const auto reviewObj = ExtractObjectBodyForKey(commitObj, "review");
            const auto includeBody = ExtractArrayBodyForKey(commitObj, "include").value_or(std::string{});
            const auto excludeBody = ExtractArrayBodyForKey(commitObj, "exclude").value_or(std::string{});
            CommitPlanEntry entry;
            entry.index = flatIndex;
            entry.repo = repo;
            entry.message = ExtractStringField(commitObj, "message").value_or("");
            entry.include = ParseJsonStringArrayBody(includeBody);
            entry.exclude = ParseJsonStringArrayBody(excludeBody);
            entry.reviewVerdict = reviewObj.has_value() ? ExtractStringField(*reviewObj, "verdict").value_or("") : "";
            entry.reviewReason = reviewObj.has_value() ? ExtractStringField(*reviewObj, "reason").value_or("") : "";
            return entry;
        }
    }

    if (OutError != nullptr) {
        *OutError = std::format("commit index out of range: {}", InCommitIndex);
    }
    return std::nullopt;
}

auto ParseCommitFillOps(const std::string& InJson, std::string* OutError = nullptr) -> std::vector<CommitFillOp> {
    std::vector<CommitFillOp> ops;
    const auto commitsArray = ExtractArrayBodyForKey(InJson, "commits");
    if (!commitsArray.has_value()) {
        if (OutError != nullptr) {
            *OutError = "schema invalid: missing commits array";
        }
        return {};
    }
    for (const auto& item : SplitTopLevelObjects(*commitsArray)) {
        CommitFillOp op;
        const auto indexText = Trim(ExtractScalarFieldToken(item, "index").value_or(""));
        const auto reviewObj = ExtractObjectBodyForKey(item, "review");
        if (indexText.empty() || !reviewObj.has_value()) {
            if (OutError != nullptr) {
                *OutError = "schema invalid: fill op missing index/review";
            }
            return {};
        }
        try {
            op.index = std::stoi(indexText);
        } catch (const std::exception&) {
            if (OutError != nullptr) {
                *OutError = std::format("invalid commit index: {}", indexText);
            }
            return {};
        }
        op.message = Trim(ExtractStringField(item, "message").value_or(""));
        op.reviewVerdict = Trim(ExtractStringField(*reviewObj, "verdict").value_or(""));
        op.reviewReason = Trim(ExtractStringField(*reviewObj, "reason").value_or(""));
        if (op.index < 0 || op.message.empty() || op.reviewVerdict.empty() || op.reviewReason.empty()) {
            if (OutError != nullptr) {
                *OutError = "fill op missing required message/review fields";
            }
            return {};
        }
        if (op.message.find("replace-with-") != std::string::npos || op.reviewReason.find("replace-with-") != std::string::npos) {
            if (OutError != nullptr) {
                *OutError = "placeholder value detected in fill op";
            }
            return {};
        }
        if (ToLower(op.reviewVerdict) != "pass") {
            if (OutError != nullptr) {
                *OutError = "review.verdict must be pass";
            }
            return {};
        }
        ops.push_back(std::move(op));
    }
    return ops;
}

auto ApplyCommitFillOps(const std::string& InPlanText,
                        const std::vector<CommitFillOp>& InOps,
                        std::string* OutError = nullptr) -> std::optional<std::string> {
    auto current = InPlanText;
    std::unordered_set<int> seen;
    for (const auto& op : InOps) {
        if (!seen.insert(op.index).second) {
            if (OutError != nullptr) {
                *OutError = std::format("duplicate fill op index: {}", op.index);
            }
            return std::nullopt;
        }
        std::string fillError;
        const auto updated = FillCommitEntryByFlatIndex(current,
                                                        op.index,
                                                        std::optional<std::string>{op.message},
                                                        std::optional<std::string>{op.reviewVerdict},
                                                        std::optional<std::string>{op.reviewReason},
                                                        std::optional<std::string>{op.plannerProvider},
                                                        std::optional<std::string>{op.plannerModel},
                                                        &fillError);
        if (!updated.has_value()) {
            if (OutError != nullptr) {
                *OutError = fillError.empty() ? std::format("failed to fill commit {}", op.index) : fillError;
            }
            return std::nullopt;
        }
        current = *updated;
    }
    return current;
}

auto StampPlanAiPlannerMetadata(std::string InPlanText,
                                const std::string& InProvider,
                                const std::string& InModel) -> std::optional<std::string> {
    auto updated = ReplaceJsonStringFieldInObject(std::move(InPlanText), "planner", "provider", InProvider);
    if (!updated.has_value()) {
        return std::nullopt;
    }
    return ReplaceJsonStringFieldInObject(*updated, "planner", "ai-model", InModel.empty() ? std::string("auto") : InModel);
}

auto UpsertCommitEntry(const std::string& InPlanText,
                       const std::string& InRepo,
                       const std::string& InMessage,
                       const std::vector<std::string>& InInclude,
                       const std::vector<std::string>& InExclude,
                       const std::string& InReviewVerdict,
                       const std::string& InReviewReason) -> std::optional<std::string> {
    const auto stages = ExtractObjectBodyForKey(InPlanText, "stages");
    if (!stages.has_value()) {
        return std::nullopt;
    }
    const auto commitArray = ExtractArrayBodyForKey(*stages, "commit");
    if (!commitArray.has_value()) {
        return std::nullopt;
    }

    const auto item = std::format(
        "{{\"message\":\"{}\",\"include\":[{}],\"exclude\":[{}],\"review\":{{\"verdict\":\"{}\",\"reason\":\"{}\"}}}}",
        JsonEscape(InMessage),
        BuildJsonStringArray(InInclude),
        BuildJsonStringArray(InExclude),
        JsonEscape(InReviewVerdict),
        JsonEscape(InReviewReason));

    auto repoObjects = SplitTopLevelObjects(*commitArray);
    bool found = false;
    for (auto& repoObj : repoObjects) {
        const auto repo = ExtractStringField(repoObj, "repo").value_or("");
        if (repo != InRepo) {
            continue;
        }
        const auto commits = ExtractArrayBodyForKey(repoObj, "commits").value_or(std::string{});
        std::string newCommits;
        if (Trim(commits).empty()) {
            newCommits = item;
        } else {
            newCommits = commits + "," + item;
        }
        if (const auto replaced = ReplaceArrayBodyForKey(repoObj, "commits", newCommits); replaced.has_value()) {
            repoObj = *replaced;
            found = true;
        }
        break;
    }

    if (!found) {
        repoObjects.push_back(std::format("{{\"repo\":\"{}\",\"commits\":[{}]}}", JsonEscape(InRepo), item));
    }

    return ReplaceArrayBodyForKey(InPlanText, "commit", RebuildCommitArrayBody(repoObjects));
}

auto FillCommitEntryByFlatIndex(const std::string& InPlanText,
                                int InCommitIndex,
                                const std::optional<std::string>& InCommitMessage,
                                const std::optional<std::string>& InReviewVerdict,
                                const std::optional<std::string>& InReviewReason,
                                const std::optional<std::string>& InPlannerProvider,
                                const std::optional<std::string>& InPlannerModel,
                                std::string* OutError = nullptr) -> std::optional<std::string> {
    if (InCommitIndex < 0) {
        if (OutError != nullptr) {
            *OutError = "--index must be >= 0";
        }
        return std::nullopt;
    }
    const auto stages = ExtractObjectBodyForKey(InPlanText, "stages");
    if (!stages.has_value()) {
        if (OutError != nullptr) {
            *OutError = "schema invalid: missing stages";
        }
        return std::nullopt;
    }
    const auto commitArray = ExtractArrayBodyForKey(*stages, "commit");
    if (!commitArray.has_value()) {
        if (OutError != nullptr) {
            *OutError = "schema invalid: missing stages.commit";
        }
        return std::nullopt;
    }

    auto repoObjects = SplitTopLevelObjects(*commitArray);
    int flatIndex = 0;
    for (auto& repoObj : repoObjects) {
        const auto commits = ExtractArrayBodyForKey(repoObj, "commits").value_or(std::string{});
        auto commitObjects = SplitTopLevelObjects(commits);
        for (auto& commitObj : commitObjects) {
            if (flatIndex != InCommitIndex) {
                flatIndex += 1;
                continue;
            }
            auto message = Trim(InCommitMessage.value_or(ExtractStringField(commitObj, "message").value_or("")));
            const auto reviewObj = ExtractObjectBodyForKey(commitObj, "review");
            auto reviewVerdict = Trim(InReviewVerdict.value_or(reviewObj.has_value() ? ExtractStringField(*reviewObj, "verdict").value_or("") : ""));
            auto reviewReason = Trim(InReviewReason.value_or(reviewObj.has_value() ? ExtractStringField(*reviewObj, "reason").value_or("") : ""));
            const auto includeBody = ExtractArrayBodyForKey(commitObj, "include").value_or(std::string{});
            const auto excludeBody = ExtractArrayBodyForKey(commitObj, "exclude").value_or(std::string{});
            const auto plannerObj = ExtractObjectBodyForKey(commitObj, "planner");
            auto plannerProvider = Trim(InPlannerProvider.value_or(
                plannerObj.has_value() ? ExtractStringField(*plannerObj, "provider").value_or("") : ""));
            auto plannerModel = Trim(InPlannerModel.value_or(
                plannerObj.has_value() ? ExtractStringField(*plannerObj, "ai-model").value_or("") : ""));

            if (message.empty()) {
                if (OutError != nullptr) {
                    *OutError = "commit message cannot be empty after fill";
                }
                return std::nullopt;
            }
            if (reviewVerdict.empty()) {
                if (OutError != nullptr) {
                    *OutError = "review.verdict cannot be empty after fill";
                }
                return std::nullopt;
            }
            if (reviewReason.empty()) {
                if (OutError != nullptr) {
                    *OutError = "review.reason cannot be empty after fill";
                }
                return std::nullopt;
            }
            if (plannerProvider.empty()) {
                plannerProvider = "replace-with-provider";
            }
            if (plannerModel.empty()) {
                plannerModel = "replace-with-ai-model";
            }

            commitObj = BuildCommitObjectJson(message,
                                              includeBody,
                                              excludeBody,
                                              reviewVerdict,
                                              reviewReason,
                                              plannerProvider,
                                              plannerModel);
            const auto updatedCommits = RebuildCommitArrayBody(commitObjects);
            const auto updatedRepoObj = ReplaceArrayBodyForKey(repoObj, "commits", updatedCommits);
            if (!updatedRepoObj.has_value()) {
                if (OutError != nullptr) {
                    *OutError = "failed to update repo commits array";
                }
                return std::nullopt;
            }
            repoObj = *updatedRepoObj;
            return ReplaceArrayBodyForKey(InPlanText, "commit", RebuildCommitArrayBody(repoObjects));
        }
    }

    if (OutError != nullptr) {
        *OutError = std::format("commit index out of range: {}", InCommitIndex);
    }
    return std::nullopt;
}

auto FillPlanByAi(const std::filesystem::path& InWorkspaceRoot,
                  const std::filesystem::path& InPlanPath,
                  const std::string& InRequestedProvider,
                  const std::string& InRequestedModel,
                  const std::string& InRequestedFillMode,
                  bool InDebugAi,
                  std::string* OutError = nullptr) -> bool {
    const auto fillStart = std::chrono::steady_clock::now();
    const auto provider = ResolveAiProvider(InRequestedProvider);
    if (provider.empty()) {
        if (OutError != nullptr) {
            *OutError = "Error: no AI provider found for plan new --ai (supported: codex/opencode/copilot).";
        }
        return false;
    }
    const auto modelDirective = ResolveAiModelDirective(provider, InRequestedModel, InWorkspaceRoot);
    const auto fillMode = ResolvePlanCommitGenerationMode(InWorkspaceRoot, InRequestedFillMode);
    const int workspaceChangeCount = CountWorkspaceDirtyEntries(InWorkspaceRoot);
    const auto model = ResolveAiModelForChangeCount(provider, modelDirective, InWorkspaceRoot, workspaceChangeCount);
    const auto dirtyContext = CollectDirtyRepoContextText(InWorkspaceRoot);
    if (Trim(dirtyContext).empty()) {
        if (OutError != nullptr) {
            *OutError = "Error: no dirty repository changes found; nothing to generate for plan.";
        }
        return false;
    }
    const auto templateJson = ReadFileText(InPlanPath).value_or(std::string{});
    const auto currentEntries = CollectCommitPlanEntries(templateJson);
    if (currentEntries.empty()) {
        if (OutError != nullptr) {
            *OutError = "Error: plan has no commit entries to fill; seed stages.commit first.";
        }
        return false;
    }
    std::cout << "[plan] AI fill start: provider=" << provider
              << " model=" << (fillMode == "single" ? model : modelDirective)
              << " mode=" << fillMode
              << " commits=" << currentEntries.size() << "\n";
    const auto prompt = BuildPlanFillOpsPrompt(InWorkspaceRoot, provider, model, InPlanPath, templateJson, dirtyContext);
    std::string debugPrefix;
    if (InDebugAi) {
        const auto debugDir = (InWorkspaceRoot / ".kano" / "tmp" / "git" / "plans" / "debug").lexically_normal();
        debugPrefix = (debugDir / std::format("pia-{}", CurrentUtcCompact())).generic_string();
        std::string debugError;
        WriteFileText(std::filesystem::path(debugPrefix + ".prompt.txt"), prompt, &debugError);
    }
    int maxAttempts = 1;
    if (const char* envAttempts = std::getenv("KOG_PLAN_FILL_MAX_ATTEMPTS"); envAttempts != nullptr) {
        try {
            const int parsedAttempts = std::stoi(Trim(std::string(envAttempts)));
            if (parsedAttempts > 0) {
                maxAttempts = parsedAttempts;
            }
        } catch (const std::exception&) {
        }
    }
    std::vector<CommitFillOp> ops;
    bool usedDeterministicFallback = false;
    const bool allowDeterministicFallback = AllowDeterministicCommitFallbackForMode(fillMode);
    if (fillMode == "single") {
        std::string lastFailureCategory;
        std::string lastFailureDetail;
        std::string lastAiCombined;
        std::string lastAiJson;
        std::optional<std::size_t> lastReturnedCount;
        bool fillReady = false;

        for (int attempt = 1; attempt <= maxAttempts; ++attempt) {
            const bool isRetry = (attempt > 1);
            const std::string attemptSuffix = isRetry ? std::format(".retry{}", attempt - 1) : std::string{};
            const auto attemptPrompt = isRetry
                ? BuildFillOpsRetryPrompt(prompt,
                                          currentEntries.size(),
                                          lastReturnedCount,
                                          lastFailureCategory,
                                          lastFailureDetail,
                                          lastAiCombined)
                : prompt;

            if (InDebugAi && !debugPrefix.empty() && isRetry) {
                std::string debugError;
                WriteFileText(std::filesystem::path(debugPrefix + attemptSuffix + ".prompt.txt"), attemptPrompt, &debugError);
            }

            const auto aiRaw = RunAiGenerate(provider, model, attemptPrompt, InWorkspaceRoot, true);
            if (aiRaw.exitCode != 0) {
                lastFailureCategory = "provider-command-failed";
                lastFailureDetail = Trim(aiRaw.stderrStr);
                lastAiCombined = aiRaw.stdoutStr + "\n" + aiRaw.stderrStr;
                lastAiJson.clear();
                lastReturnedCount.reset();
                if (InDebugAi && !debugPrefix.empty()) {
                    std::string debugError;
                    WriteFileText(std::filesystem::path(debugPrefix + attemptSuffix + ".raw.txt"), lastAiCombined, &debugError);
                    WriteFileText(std::filesystem::path(debugPrefix + attemptSuffix + ".extracted.json"), lastAiJson, &debugError);
                }
                continue;
            }

            lastAiCombined = aiRaw.stdoutStr + "\n" + aiRaw.stderrStr;
            lastAiJson = ExtractPlanFillOpsJson(lastAiCombined);
            if (InDebugAi && !debugPrefix.empty()) {
                std::string debugError;
                WriteFileText(std::filesystem::path(debugPrefix + attemptSuffix + ".raw.txt"), lastAiCombined, &debugError);
                WriteFileText(std::filesystem::path(debugPrefix + attemptSuffix + ".extracted.json"), lastAiJson, &debugError);
            }

            if (lastAiJson.empty()) {
                lastFailureCategory = "no-fill-ops-payload";
                lastFailureDetail = CompactSingleLine(lastAiCombined, 220);
                lastReturnedCount.reset();
                continue;
            }

            std::string parseError;
            ops = ParseCommitFillOps(lastAiJson, &parseError);
            if (ops.empty()) {
                lastFailureCategory = "schema-invalid-fill-ops";
                lastFailureDetail = Trim(parseError);
                lastReturnedCount.reset();
                continue;
            }

            if (ops.size() != currentEntries.size()) {
                lastFailureCategory = "incomplete-fill-ops";
                lastFailureDetail = std::format("expected {} commits, got {}", currentEntries.size(), ops.size());
                lastReturnedCount = ops.size();
                continue;
            }

            fillReady = true;
            break;
        }

        if (!fillReady) {
            if (!allowDeterministicFallback) {
                if (OutError != nullptr) {
                    std::ostringstream oss;
                    oss << "Error: AI fill-ops generation failed after retries.";
                    if (!Trim(lastFailureCategory).empty()) {
                        oss << " Category: " << lastFailureCategory << ".";
                    }
                    if (!Trim(lastFailureDetail).empty()) {
                        oss << " Detail: " << lastFailureDetail;
                    }
                    oss << " Human-mode CPA forbids deterministic commit fallback in single mode.";
                    if (!debugPrefix.empty()) {
                        oss << std::format(" Debug: {}.prompt.txt {}.raw.txt {}.extracted.json",
                                           debugPrefix,
                                           debugPrefix,
                                           debugPrefix);
                        for (int retryIndex = 1; retryIndex < maxAttempts; ++retryIndex) {
                            oss << std::format(" {}.retry{}.prompt.txt {}.retry{}.raw.txt {}.retry{}.extracted.json",
                                               debugPrefix,
                                               retryIndex,
                                               debugPrefix,
                                               retryIndex,
                                               debugPrefix,
                                               retryIndex);
                        }
                    }
                    *OutError = oss.str();
                }
                return false;
            }
            ops = BuildDeterministicCommitFillOps(currentEntries);
            if (ops.size() == currentEntries.size()) {
                usedDeterministicFallback = true;
                fillReady = true;
                std::cerr << "[plan] warning: AI fill-ops generation failed; using deterministic local fallback. category="
                          << (Trim(lastFailureCategory).empty() ? "unknown" : lastFailureCategory) << "\n";
                if (InDebugAi && !debugPrefix.empty()) {
                    std::ostringstream fallbackJson;
                    fallbackJson << "{\n  \"commits\": [\n";
                    for (std::size_t i = 0; i < ops.size(); ++i) {
                        const auto& op = ops[i];
                        fallbackJson << std::format("    {{\"index\":{},\"message\":\"{}\",\"review\":{{\"verdict\":\"{}\",\"reason\":\"{}\"}}}}",
                                                    op.index,
                                                    JsonEscape(op.message),
                                                    JsonEscape(op.reviewVerdict),
                                                    JsonEscape(op.reviewReason));
                        if (i + 1 < ops.size()) {
                            fallbackJson << ",";
                        }
                        fallbackJson << "\n";
                    }
                    fallbackJson << "  ]\n}\n";
                    std::string debugError;
                    WriteFileText(std::filesystem::path(debugPrefix + ".fallback.extracted.json"), fallbackJson.str(), &debugError);
                }
            } else if (OutError != nullptr) {
                std::ostringstream oss;
                oss << "Error: AI fill-ops generation failed after retries.";
                if (!Trim(lastFailureCategory).empty()) {
                    oss << " Category: " << lastFailureCategory << ".";
                }
                if (!Trim(lastFailureDetail).empty()) {
                    oss << " Detail: " << lastFailureDetail;
                }
                if (!debugPrefix.empty()) {
                    oss << std::format(" Debug: {}.prompt.txt {}.raw.txt {}.extracted.json",
                                       debugPrefix,
                                       debugPrefix,
                                       debugPrefix);
                    for (int retryIndex = 1; retryIndex < maxAttempts; ++retryIndex) {
                        oss << std::format(" {}.retry{}.prompt.txt {}.retry{}.raw.txt {}.retry{}.extracted.json",
                                           debugPrefix,
                                           retryIndex,
                                           debugPrefix,
                                           retryIndex,
                                           debugPrefix,
                                           retryIndex);
                    }
                }
                *OutError = oss.str();
                return false;
            }
        }
        for (auto& op : ops) {
            op.plannerProvider = provider;
            op.plannerModel = model;
        }
    } else {
        std::unordered_map<std::string, bool> repoGitlinkOnlyCache;
        auto isEntryGitlinkOnly = [&](const CommitPlanEntry& InEntry) -> bool {
            const auto repoDisplay = InEntry.repo.empty() ? std::string(".") : InEntry.repo;
            if (const auto it = repoGitlinkOnlyCache.find(repoDisplay); it != repoGitlinkOnlyCache.end()) {
                return it->second;
            }
            const auto repoPath = ResolveRepoPathFromDisplay(InWorkspaceRoot, repoDisplay);
            const bool gitlinkOnly = RepoHasGitlinkOnlyChanges(repoPath);
            repoGitlinkOnlyCache[repoDisplay] = gitlinkOnly;
            return gitlinkOnly;
        };

        for (const auto& entry : currentEntries) {
            const auto repoPathForCount = ResolveRepoPathFromDisplay(InWorkspaceRoot, entry.repo.empty() ? std::string(".") : entry.repo);
            int entryChangeCount = 0;
            {
                const auto status = GitCapture(repoPathForCount, {"status", "--porcelain", "--untracked-files=all"});
                if (status.exitCode == 0 && !Trim(status.stdoutStr).empty()) {
                    std::istringstream iss(status.stdoutStr);
                    std::string line;
                    while (std::getline(iss, line)) {
                        if (!Trim(line).empty()) {
                            entryChangeCount += 1;
                        }
                    }
                }
            }
            const auto entryModel = ResolveAiModelForChangeCount(provider, modelDirective, InWorkspaceRoot, entryChangeCount);
            const bool deterministicByMode = (fillMode == "adaptive" && isEntryGitlinkOnly(entry));
            if (deterministicByMode) {
                auto fallbackOp = BuildDeterministicCommitFillOp(entry);
                fallbackOp.plannerProvider = provider;
                fallbackOp.plannerModel = entryModel;
                ops.push_back(std::move(fallbackOp));
                usedDeterministicFallback = true;
                continue;
            }

            std::string entryFailureCategory;
            std::string entryFailureDetail;
            std::string entryAiCombined;
            bool resolved = false;
            const auto baseEntryPrompt = BuildSingleCommitFillPrompt(InWorkspaceRoot, provider, entryModel, entry, dirtyContext);
            if (InDebugAi && !debugPrefix.empty()) {
                std::string debugError;
                WriteFileText(std::filesystem::path(std::format("{}.item{}.prompt.txt", debugPrefix, entry.index)), baseEntryPrompt, &debugError);
            }

            for (int attempt = 1; attempt <= maxAttempts; ++attempt) {
                const bool isRetry = (attempt > 1);
                const std::string attemptSuffix = isRetry ? std::format(".item{}.retry{}", entry.index, attempt - 1)
                                                          : std::format(".item{}", entry.index);
                const auto attemptPrompt = isRetry
                    ? BuildFillOpsRetryPrompt(baseEntryPrompt, 1, std::nullopt, entryFailureCategory, entryFailureDetail, entryAiCombined)
                    : baseEntryPrompt;

                if (InDebugAi && !debugPrefix.empty() && isRetry) {
                    std::string debugError;
                    WriteFileText(std::filesystem::path(debugPrefix + attemptSuffix + ".prompt.txt"), attemptPrompt, &debugError);
                }

                const auto aiRaw = RunAiGenerate(provider, entryModel, attemptPrompt, InWorkspaceRoot, true);
                entryAiCombined = aiRaw.stdoutStr + "\n" + aiRaw.stderrStr;
                auto aiJson = std::string{};
                if (aiRaw.exitCode == 0) {
                    aiJson = ExtractPlanFillOpsJson(entryAiCombined);
                }
                if (InDebugAi && !debugPrefix.empty()) {
                    std::string debugError;
                    WriteFileText(std::filesystem::path(debugPrefix + attemptSuffix + ".raw.txt"), entryAiCombined, &debugError);
                    WriteFileText(std::filesystem::path(debugPrefix + attemptSuffix + ".extracted.json"), aiJson, &debugError);
                }

                if (aiRaw.exitCode != 0) {
                    entryFailureCategory = "provider-command-failed";
                    entryFailureDetail = Trim(aiRaw.stderrStr);
                    continue;
                }
                if (aiJson.empty()) {
                    entryFailureCategory = "no-fill-ops-payload";
                    entryFailureDetail = CompactSingleLine(entryAiCombined, 220);
                    continue;
                }

                std::string parseError;
                const auto entryOps = ParseCommitFillOps(aiJson, &parseError);
                if (entryOps.empty()) {
                    entryFailureCategory = "schema-invalid-fill-ops";
                    entryFailureDetail = Trim(parseError);
                    continue;
                }

                const auto it = std::find_if(entryOps.begin(), entryOps.end(), [&](const CommitFillOp& op) {
                    return op.index == entry.index;
                });
                if (it == entryOps.end()) {
                    entryFailureCategory = "missing-target-index";
                    entryFailureDetail = std::format("expected index {} not found in payload", entry.index);
                    continue;
                }

                auto resolvedOp = *it;
                resolvedOp.plannerProvider = provider;
                resolvedOp.plannerModel = entryModel;
                ops.push_back(std::move(resolvedOp));
                resolved = true;
                break;
            }

            if (!resolved) {
                if (OutError != nullptr) {
                    std::ostringstream oss;
                    oss << std::format("Error: AI per-commit fill failed at index {}.", entry.index);
                    if (!Trim(entryFailureCategory).empty()) {
                        oss << " Category: " << entryFailureCategory << ".";
                    }
                    if (!Trim(entryFailureDetail).empty()) {
                        oss << " Detail: " << entryFailureDetail;
                    }
                    if (!debugPrefix.empty()) {
                        oss << std::format(" Debug: {}.item{}.prompt.txt {}.item{}.raw.txt {}.item{}.extracted.json",
                                           debugPrefix,
                                           entry.index,
                                           debugPrefix,
                                           entry.index,
                                           debugPrefix,
                                           entry.index);
                    }
                    *OutError = oss.str();
                }
                return false;
            }
        }
    }

    std::unordered_set<int> expectedIndexes;
    for (const auto& entry : currentEntries) {
        expectedIndexes.insert(entry.index);
    }
    for (const auto& op : ops) {
        if (!expectedIndexes.contains(op.index)) {
            if (OutError != nullptr) {
                *OutError = std::format("Error: AI returned unknown fill op index: {}", op.index);
            }
            return false;
        }
    }

    std::string applyError;
    auto updatedPlan = ApplyCommitFillOps(templateJson, ops, &applyError);
    if (!updatedPlan.has_value()) {
        if (OutError != nullptr) {
            *OutError = "Error: failed to apply AI fill ops to plan.";
            if (!Trim(applyError).empty()) {
                *OutError += " Detail: " + applyError;
            }
        }
        return false;
    }

    const auto topLevelPlannerModel = (fillMode == "single") ? model : std::string("per-commit");
    if (const auto stamped = StampPlanAiPlannerMetadata(*updatedPlan, provider, topLevelPlannerModel); stamped.has_value()) {
        updatedPlan = *stamped;
    }

    std::string validationReason;
    if (!ValidateAiReadyPlan(*updatedPlan, &validationReason)) {
        if (OutError != nullptr) {
            *OutError = "Error: AI-applied plan validation failed.";
            if (!Trim(validationReason).empty()) {
                *OutError += " Detail: " + validationReason;
            }
        }
        return false;
    }

    std::string error;
    if (!WriteFileText(InPlanPath, *updatedPlan, &error)) {
        if (OutError != nullptr) {
            std::ostringstream oss;
            oss << "Error: failed to write AI-filled plan output: " << InPlanPath.generic_string();
            if (!error.empty()) {
                oss << " (" << error << ")";
            }
            *OutError = oss.str();
        }
        return false;
    }
    if (!debugPrefix.empty()) {
        std::cerr << "Debug: " << debugPrefix << ".prompt.txt\n";
        std::cerr << "Debug: " << debugPrefix << ".raw.txt\n";
        std::cerr << "Debug: " << debugPrefix << ".extracted.json\n";
        if (usedDeterministicFallback) {
            std::cerr << "Debug: " << debugPrefix << ".fallback.extracted.json\n";
        }
    }
    if (usedDeterministicFallback) {
        std::cout << "Filled plan commit entries with deterministic fallback ops: provider=" << provider
                  << " model=" << (fillMode == "single" ? model : modelDirective)
                  << " commits=" << ops.size() << "\n";
    } else {
        std::cout << "Filled plan commit entries with AI-safe ops: provider=" << provider
                  << " model=" << (fillMode == "single" ? model : modelDirective)
                  << " commits=" << ops.size() << "\n";
    }
    const auto fillEnd = std::chrono::steady_clock::now();
    const auto fillMillis = std::chrono::duration_cast<std::chrono::milliseconds>(fillEnd - fillStart).count();
    std::cout << "[plan] ai_fill_ms: " << fillMillis << "\n";
    return true;
}

auto EscapeJsonString(const std::string& InValue) -> std::string {
    std::string out;
    out.reserve(InValue.size() + 8);
    for (const char ch : InValue) {
        switch (ch) {
        case '\\': out += "\\\\"; break;
        case '"': out += "\\\""; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default: out.push_back(ch); break;
        }
    }
    return out;
}

auto FindBracketRange(const std::string& InText, std::size_t InStart, char InOpen, char InClose) -> std::optional<std::pair<std::size_t, std::size_t>> {
    if (InStart >= InText.size() || InText[InStart] != InOpen) {
        return std::nullopt;
    }
    bool inString = false;
    bool escaped = false;
    int depth = 0;
    for (std::size_t pos = InStart; pos < InText.size(); ++pos) {
        const char ch = InText[pos];
        if (inString) {
            if (escaped) {
                escaped = false;
            } else if (ch == '\\') {
                escaped = true;
            } else if (ch == '"') {
                inString = false;
            }
            continue;
        }
        if (ch == '"') {
            inString = true;
            continue;
        }
        if (ch == InOpen) {
            depth += 1;
            continue;
        }
        if (ch == InClose) {
            depth -= 1;
            if (depth == 0) {
                return std::make_pair(InStart, pos + 1);
            }
        }
    }
    return std::nullopt;
}

auto BuildIgnoreEntriesJson(const std::vector<IgnoreStageEntry>& InEntries) -> std::string {
    std::ostringstream out;
    out << "[\n";
    for (std::size_t i = 0; i < InEntries.size(); ++i) {
        const auto& e = InEntries[i];
        out << "      {\n";
        out << "        \"repo\": \"" << EscapeJsonString(e.repo) << "\",\n";
        out << "        \"apply_target\": \"" << EscapeJsonString(e.applyTarget) << "\",\n";
        out << "        \"merged_output_path\": \"" << EscapeJsonString(e.mergedOutputPath) << "\",\n";
        out << "        \"applied_at_utc\": \"\",\n";
        out << "        \"candidates\": [\n";
        for (std::size_t j = 0; j < e.rules.size(); ++j) {
            out << "          { \"rule\": \"" << EscapeJsonString(e.rules[j]) << "\", \"source\": \"working-tree\", \"reason\": \"untracked-artifact\" }";
            if (j + 1 < e.rules.size()) {
                out << ",";
            }
            out << "\n";
        }
        out << "        ]\n";
        out << "      }";
        if (i + 1 < InEntries.size()) {
            out << ",";
        }
        out << "\n";
    }
    out << "    ]";
    return out.str();
}

auto InjectIgnoreEntries(std::string InPlanText, const std::vector<IgnoreStageEntry>& InEntries) -> std::optional<std::string> {
    const auto stagesPos = FindJsonKeyValueStart(InPlanText, "stages");
    if (!stagesPos.has_value()) {
        return std::nullopt;
    }
    const auto ignorePos = FindJsonKeyValueStart(InPlanText, "ignore", *stagesPos);
    if (ignorePos.has_value()) {
        const auto arrRange = FindBracketRange(InPlanText, *ignorePos, '[', ']');
        if (!arrRange.has_value()) {
            return std::nullopt;
        }
        std::string out;
        out.reserve(InPlanText.size() + 256);
        out.append(InPlanText.substr(0, arrRange->first));
        out.append(BuildIgnoreEntriesJson(InEntries));
        out.append(InPlanText.substr(arrRange->second));
        return out;
    }

    const auto stagesObj = FindBracketRange(InPlanText, *stagesPos, '{', '}');
    if (!stagesObj.has_value() || stagesObj->second <= stagesObj->first + 1) {
        return std::nullopt;
    }

    bool hasMembers = false;
    for (std::size_t i = stagesObj->first + 1; i + 1 < stagesObj->second; ++i) {
        const auto ch = InPlanText[i];
        if (!std::isspace(static_cast<unsigned char>(ch))) {
            hasMembers = true;
            break;
        }
    }

    const auto ignoreJson = BuildIgnoreEntriesJson(InEntries);
    std::string insertion;
    if (hasMembers) {
        insertion = std::format(",\n    \"ignore\": {}", ignoreJson);
    } else {
        insertion = std::format("\n    \"ignore\": {}\n  ", ignoreJson);
    }

    std::string out;
    out.reserve(InPlanText.size() + insertion.size() + 32);
    out.append(InPlanText.substr(0, stagesObj->second - 1));
    out.append(insertion);
    out.append(InPlanText.substr(stagesObj->second - 1));
    return out;
}

auto ReplaceJsonStringFieldInObject(std::string InJson,
                                    const std::string& InObjectKey,
                                    const std::string& InFieldKey,
                                    const std::string& InNewValue) -> std::optional<std::string> {
    const auto objectPos = FindJsonKeyValueStart(InJson, InObjectKey);
    if (!objectPos.has_value()) {
        return std::nullopt;
    }
    const auto fieldValuePos = FindJsonKeyValueStart(InJson, InFieldKey, *objectPos);
    if (!fieldValuePos.has_value()) {
        return std::nullopt;
    }
    const auto parsed = ParseJsonStringAt(InJson, *fieldValuePos);
    if (!parsed.has_value()) {
        return std::nullopt;
    }
    const auto stringStart = *fieldValuePos;
    const auto stringEnd = parsed->second;
    std::string out;
    out.reserve(InJson.size() + InNewValue.size() + 8);
    out.append(InJson.substr(0, stringStart));
    out.append("\"");
    out.append(EscapeJsonString(InNewValue));
    out.append("\"");
    out.append(InJson.substr(stringEnd));
    return out;
}

auto ApplyIgnoreDatasourceOverrides(std::string InPlanText,
                                    const std::optional<std::filesystem::path>& InDatasourceRoot,
                                    const std::optional<std::filesystem::path>& InDatasourceManifest) -> std::optional<std::string> {
    std::string out = std::move(InPlanText);
    if (InDatasourceRoot.has_value()) {
        const auto replaced = ReplaceJsonStringFieldInObject(std::move(out),
                                                              "ignore_datasource",
                                                              "root",
                                                              InDatasourceRoot->lexically_normal().generic_string());
        if (!replaced.has_value()) {
            return std::nullopt;
        }
        out = *replaced;
    }
    if (InDatasourceManifest.has_value()) {
        const auto replaced = ReplaceJsonStringFieldInObject(std::move(out),
                                                              "ignore_datasource",
                                                              "manifest",
                                                              InDatasourceManifest->lexically_normal().generic_string());
        if (!replaced.has_value()) {
            return std::nullopt;
        }
        out = *replaced;
    }
    return out;
}

auto ReplacePlanDirtyFingerprint(std::string InPlanText, const std::string& InNewDirtyFingerprint) -> std::optional<std::string> {
    return ReplaceJsonStringFieldInObject(std::move(InPlanText), "meta", "dirty_fingerprint", InNewDirtyFingerprint);
}

auto ParseStatusUntrackedPath(const std::string& InLine) -> std::string {
    if (InLine.size() < 4 || InLine[0] != '?' || InLine[1] != '?') {
        return {};
    }
    return Trim(InLine.substr(3));
}

auto NormalizeIgnoreRuleCandidate(const std::filesystem::path& InRepo, const std::string& InPath) -> std::string {
    auto value = Trim(InPath);
    if (value.empty()) {
        return {};
    }
    std::replace(value.begin(), value.end(), '\\', '/');
    while (value.starts_with("./")) {
        value = value.substr(2);
    }
    if (value.empty()) {
        return {};
    }
    std::error_code ec;
    const auto abs = (InRepo / std::filesystem::path(value)).lexically_normal();
    if (std::filesystem::is_directory(abs, ec) && !value.ends_with('/')) {
        value.push_back('/');
    }
    return value;
}

auto BuildIgnoreEntriesFromWorkingTree(const std::filesystem::path& InWorkspaceRoot, int InMaxPerRepo) -> std::vector<IgnoreStageEntry> {
    std::vector<IgnoreStageEntry> out;
    const auto repos = DiscoverWorkspaceRepos(InWorkspaceRoot);
    for (const auto& repo : repos) {
        const auto status = GitCapture(repo, {"status", "--porcelain", "--untracked-files=all"});
        if (status.exitCode != 0 || Trim(status.stdoutStr).empty()) {
            continue;
        }
        std::unordered_set<std::string> seen;
        std::vector<std::string> rules;
        std::istringstream iss(status.stdoutStr);
        std::string line;
        while (std::getline(iss, line)) {
            const auto path = ParseStatusUntrackedPath(line);
            if (path.empty() || !IsProbableIgnoreArtifactPath(path)) {
                continue;
            }
            const auto rule = NormalizeIgnoreRuleCandidate(repo, path);
            if (rule.empty()) {
                continue;
            }
            if (seen.insert(rule).second) {
                rules.push_back(rule);
                if (InMaxPerRepo > 0 && static_cast<int>(rules.size()) >= InMaxPerRepo) {
                    break;
                }
            }
        }
        if (rules.empty()) {
            continue;
        }
        IgnoreStageEntry e;
        e.repo = RelativeDisplayPath(InWorkspaceRoot, repo);
        if (e.repo.empty()) {
            e.repo = ".";
        }
        e.applyTarget = ".gitignore";
        e.mergedOutputPath = (InWorkspaceRoot / ".kano" / "tmp" / "git" / "plans" /
                              std::format("ignore-merged-{}.gitignore", e.repo == "." ? "root" : e.repo))
                                 .lexically_normal()
                                 .generic_string();
        e.rules = std::move(rules);
        out.push_back(std::move(e));
    }
    return out;
}

auto NormalizePathSlashesLower(std::string InPath) -> std::string {
    std::replace(InPath.begin(), InPath.end(), '\\', '/');
    return ToLower(std::move(InPath));
}

auto ReadIgnoreGateAllowlist(const std::filesystem::path& InAllowlistPath) -> std::unordered_set<std::string> {
    std::unordered_set<std::string> out;
    const auto text = ReadFileText(InAllowlistPath);
    if (!text.has_value()) {
        return out;
    }
    std::istringstream iss(*text);
    std::string line;
    while (std::getline(iss, line)) {
        auto t = Trim(line);
        if (t.empty() || t[0] == '#') {
            continue;
        }
        out.insert(NormalizePathSlashesLower(std::move(t)));
    }
    return out;
}

auto NormalizeRule(std::string InRule) -> std::string {
    auto r = Trim(std::move(InRule));
    while (!r.empty() && (r.back() == '\r' || r.back() == '\n')) {
        r.pop_back();
    }
    return r;
}

auto MergeGitignore(const std::filesystem::path& InTarget,
                    const std::vector<std::string>& InRules) -> std::string {
    std::vector<std::string> existing;
    if (const auto text = ReadFileText(InTarget); text.has_value()) {
        std::istringstream iss(*text);
        std::string line;
        while (std::getline(iss, line)) {
            existing.push_back(line);
        }
    }

    std::unordered_set<std::string> seen;
    for (const auto& line : existing) {
        const auto norm = NormalizeRule(line);
        if (norm.empty() || norm[0] == '#') {
            continue;
        }
        seen.insert(norm);
    }

    std::ostringstream out;
    for (const auto& line : existing) {
        out << line << "\n";
    }
    for (const auto& rule : InRules) {
        const auto norm = NormalizeRule(rule);
        if (norm.empty() || norm[0] == '#') {
            continue;
        }
        if (seen.insert(norm).second) {
            out << norm << "\n";
        }
    }
    return out.str();
}

auto StampIgnoreAppliedAtAll(std::string InText, const std::string& InTimestamp) -> std::string {
    const std::regex pattern(R"("applied_at_utc"\s*:\s*"")");
    return std::regex_replace(InText, pattern, std::string("\"applied_at_utc\": \"") + InTimestamp + "\"");
}

auto DefaultPlanPath(const std::filesystem::path& InWorkspaceRoot) -> std::filesystem::path {
    return (InWorkspaceRoot / ".kano" / "tmp" / "git" / "plans" / "default-plan.json").lexically_normal();
}

auto ResolveSkillRoot(const std::filesystem::path& InWorkspaceRoot) -> std::filesystem::path {
    if (const char* envRoot = std::getenv("KANO_GIT_SKILL_ROOT"); envRoot != nullptr && std::string(envRoot).size() > 0) {
        return std::filesystem::path(envRoot).lexically_normal();
    }
    return (InWorkspaceRoot / ".agents" / "kano" / "kano-git-master-skill").lexically_normal();
}

auto ResolveIgnoreDatasourceRoot(const std::filesystem::path& InWorkspaceRoot) -> std::filesystem::path {
    return (ResolveSkillRoot(InWorkspaceRoot) / "assets" / "ignore-sources").lexically_normal();
}

auto GitHeadSha(const std::filesystem::path& InRepo) -> std::optional<std::string> {
    const auto result = GitCapture(InRepo, {"rev-parse", "HEAD"});
    if (result.exitCode != 0) {
        return std::nullopt;
    }
    const auto value = Trim(result.stdoutStr);
    if (value.empty()) {
        return std::nullopt;
    }
    return value;
}

auto GitSubmoduleGitlinkShaAtHead(const std::filesystem::path& InRepo, const std::string& InSubmodulePath) -> std::optional<std::string> {
    const auto result = GitCapture(InRepo, {"ls-tree", "HEAD", InSubmodulePath});
    if (result.exitCode != 0) {
        return std::nullopt;
    }
    std::istringstream iss(result.stdoutStr);
    std::string line;
    if (!std::getline(iss, line)) {
        return std::nullopt;
    }
    const auto tabPos = line.find('\t');
    const auto left = tabPos == std::string::npos ? line : line.substr(0, tabPos);
    std::istringstream leftStream(left);
    std::string mode;
    std::string type;
    std::string sha;
    leftStream >> mode >> type >> sha;
    if (sha.empty()) {
        return std::nullopt;
    }
    return sha;
}

} // namespace

void RegisterPlan(CLI::App& InApp) {
    auto* cmd = InApp.add_subcommand("plan", "Plan pipeline commands");
    const auto configRoot = std::filesystem::current_path().lexically_normal();
    const auto defaultCommitGenerationMode = kog_config::ResolvePlanCommitGenerationMode(configRoot,
                                                                                         ResolveSkillRoot(configRoot),
                                                                                         "adaptive");

    auto* init = cmd->add_subcommand("new", "Write plan template");
    auto* initOut = new std::string{};
    auto* initForce = new bool{false};
    auto* initAiAuto = new bool{false};
    auto* initAiProvider = new std::string{"auto"};
    auto* initAiModel = new std::string{};
    auto* initAiFillMode = new std::string{defaultCommitGenerationMode};
    auto* initDebugAi = new bool{false};
    auto* initAllowIgnoreGate = new bool{false};
    auto* initDatasourceRoot = new std::string{};
    auto* initDatasourceManifest = new std::string{};
    init->add_option("--output,-o", *initOut, "Plan output path (default: .kano/tmp/git/plans/default-plan.json)");
    init->add_flag("--force,-f", *initForce, "Overwrite existing output");
    init->add_flag("--ai-auto,--ai", *initAiAuto, "Generate and fill plan by AI");
    init->add_option("--ai-provider,--provider", *initAiProvider, "AI provider (copilot|codex|opencode|auto)")->default_str("auto");
    init->add_option("--ai-model,--model", *initAiModel, "AI model (default: layered kog_config -> auto policy)");
    init->add_option("--ai-commit-generation-mode,--ai-fill-mode",
                     *initAiFillMode,
                     "AI commit generation mode: single=one workspace-wide pass, per-commit=one AI pass per commit, adaptive=per-commit + deterministic gitlink fallback (single|per-commit|adaptive)")
        ->default_str(defaultCommitGenerationMode);
    init->add_flag("--debug-ai", *initDebugAi, "Write AI prompt/raw/extracted debug artifacts");
    init->add_flag("--allow-ignore-gate", *initAllowIgnoreGate, "Compatibility flag (currently no-op in native plan new)");
    init->add_option("--ignore-datasource-root",
                     *initDatasourceRoot,
                     "Override ignore datasource root in plan meta.ignore_datasource.root");
    init->add_option("--ignore-datasource-manifest",
                     *initDatasourceManifest,
                     "Override ignore datasource manifest in plan meta.ignore_datasource.manifest");
    init->callback([=]() {
        const auto workspaceRoot = std::filesystem::current_path().lexically_normal();
        const auto outPath = initOut->empty() ? DefaultPlanPath(workspaceRoot) : std::filesystem::path(*initOut).lexically_normal();
        const auto datasourceRoot = initDatasourceRoot->empty()
                                        ? std::optional<std::filesystem::path>{}
                                        : std::optional<std::filesystem::path>{ResolvePath(workspaceRoot, *initDatasourceRoot)};
        const auto datasourceManifest = initDatasourceManifest->empty()
                                            ? std::optional<std::filesystem::path>{}
                                            : std::optional<std::filesystem::path>{ResolvePath(workspaceRoot, *initDatasourceManifest)};
        if (std::filesystem::exists(outPath) && !*initForce) {
            std::cerr << "Error: output already exists: " << outPath.generic_string() << "\n";
            std::cerr << "Hint: pass --force to overwrite.\n";
            std::exit(2);
        }
        std::string error;
        if (!WriteFileText(outPath, BuildDefaultPlanTemplate(workspaceRoot, datasourceRoot, datasourceManifest), &error)) {
            std::cerr << "Error: failed to write plan template: " << outPath.generic_string();
            if (!error.empty()) {
                std::cerr << " (" << error << ")";
            }
            std::cerr << "\n";
            std::exit(2);
        }

        if (*initAiAuto) {
            std::string aiError;
            if (!FillPlanByAi(workspaceRoot, outPath, *initAiProvider, *initAiModel, *initAiFillMode, *initDebugAi, &aiError)) {
                std::cerr << aiError << "\n";
                std::exit(2);
            }
        }

        std::cout << "Wrote plan template: " << outPath.generic_string() << "\n";
    });

    auto* setAiModel = cmd->add_subcommand("set-ai-model", "Set local default AI model in .kano/kog_config.toml for auto plan/commit flows");
    setAiModel->footer([]() {
        return BuildSetAiModelHelpFooter();
    });
    auto* setAiProvider = new std::string{"auto"};
    auto* setAiModelValue = new std::string{};
    setAiModel->add_option("--ai-provider,--provider", *setAiProvider, "AI provider (copilot|codex|opencode|auto)")->default_str("auto");
    setAiModel->add_option("--ai-model,--model", *setAiModelValue, "AI model to write to local kog_config; see --help footer for detected models")->required();
    setAiModel->callback([=]() {
        const auto workspaceRoot = std::filesystem::current_path().lexically_normal();
        const auto provider = ResolveAiProvider(*setAiProvider);
        const auto model = Trim(*setAiModelValue);
        if (provider.empty()) {
            std::cerr << "Error: no AI provider found to write config for.\n";
            std::exit(2);
        }
        const auto modelLower = NormalizeAiModelKeyword(model);
        if (model.empty()) {
            std::cerr << "Error: --ai-model must be a model name or special keyword (provider-default|auto).\n";
            std::exit(2);
        }
        const auto configValue = (modelLower == "auto" || modelLower == "provider-default")
            ? modelLower
            : provider + "/" + model;
        const auto localConfig = kog_config::LocalConfigPath(workspaceRoot);
        if (!kog_config::WriteTomlValue(localConfig, "ai.model.selection", configValue)) {
            std::cerr << "Error: failed to persist local AI model config.\n";
            std::exit(2);
        }
        std::cout << "Updated local AI model config: key=ai.model.selection value=" << configValue
                  << " file=" << localConfig.generic_string() << "\n";
    });

    auto* commitSeed = cmd->add_subcommand("commit-seed", "Populate stages.commit skeleton from current dirty repos");
    auto* commitSeedFile = new std::string{};
    auto* commitSeedForce = new bool{false};
    auto* commitSeedDeterministic = new bool{false};
    commitSeed->add_option("--plan-file", *commitSeedFile, "Plan file path");
    commitSeed->add_flag("--force,-f", *commitSeedForce, "Overwrite existing stages.commit even when populated");
    commitSeed->add_flag("--deterministic", *commitSeedDeterministic, "Seed deterministic messages/review instead of placeholders");
    commitSeed->callback([=]() {
        const auto workspaceRoot = std::filesystem::current_path().lexically_normal();
        const auto planPath =
            commitSeedFile->empty() ? DefaultPlanPath(workspaceRoot) : std::filesystem::path(*commitSeedFile).lexically_normal();
        auto payload = ReadFileText(planPath);
        if (!payload.has_value()) {
            std::string error;
            if (!WriteFileText(planPath, BuildDefaultPlanTemplate(workspaceRoot), &error)) {
                std::cerr << "Error: failed to create plan template: " << planPath.generic_string();
                if (!error.empty()) {
                    std::cerr << " (" << error << ")";
                }
                std::cerr << "\n";
                std::exit(2);
            }
            payload = ReadFileText(planPath);
        }
        if (!payload.has_value()) {
            std::cerr << "Error: plan file not found/readable: " << planPath.generic_string() << "\n";
            std::exit(2);
        }
        const auto seeded = SeedCommitStage(workspaceRoot, *payload, *commitSeedForce, !*commitSeedDeterministic);
        if (!seeded.has_value()) {
            std::cout << "Plan commit-seed skipped: stages.commit already populated.\n";
            return;
        }
        std::string error;
        if (!WriteFileText(planPath, *seeded, &error)) {
            std::cerr << "Error: failed to write seeded commit stage: " << planPath.generic_string();
            if (!error.empty()) {
                std::cerr << " (" << error << ")";
            }
            std::cerr << "\n";
            std::exit(2);
        }
        const auto stages = ExtractObjectBodyForKey(*seeded, "stages").value_or(std::string{});
        const auto commitArray = ExtractArrayBodyForKey(stages, "commit").value_or(std::string{});
        std::cout << std::format("Plan commit-seed complete: repos={} file={}\n",
                                 CountTopLevelObjects(commitArray),
                                 planPath.generic_string());
    });

    auto* needsRefresh = cmd->add_subcommand("refresh-check", "Return 0 when plan should be regenerated");
    auto* needsRefreshFile = new std::string{};
    auto* needsRefreshVerbose = new bool{false};
    needsRefresh->add_option("--plan-file", *needsRefreshFile, "Plan file path");
    needsRefresh->add_flag("--verbose", *needsRefreshVerbose, "Print reason");
    needsRefresh->callback([=]() {
        const auto workspaceRoot = std::filesystem::current_path().lexically_normal();
        const auto planPath = needsRefreshFile->empty() ? DefaultPlanPath(workspaceRoot) : std::filesystem::path(*needsRefreshFile).lexically_normal();
        const auto payload = ReadFileText(planPath);
        if (!payload.has_value()) {
            if (*needsRefreshVerbose) {
                std::cout << "refresh-needed: missing-or-unreadable\n";
            }
            std::exit(0);
        }
        if (PlanNeedsRefresh(*payload)) {
            if (*needsRefreshVerbose) {
                std::cout << "refresh-needed: placeholder-or-empty\n";
            }
            std::exit(0);
        }
        std::string planBaseHeadSha;
        std::string planDirtyFingerprint;
        if (!ExtractPlanWorkspaceHashes(*payload, &planBaseHeadSha, &planDirtyFingerprint)) {
            if (*needsRefreshVerbose) {
                std::cout << "refresh-needed: missing-or-placeholder-workspace-hash\n";
            }
            std::exit(0);
        }
        const auto currentBaseHeadSha = ComputeWorkspaceBaseHeadSha(workspaceRoot);
        const auto currentDirtyFingerprint = ComputeWorkspaceDirtyFingerprint(workspaceRoot);
        if (planBaseHeadSha != currentBaseHeadSha || planDirtyFingerprint != currentDirtyFingerprint) {
            if (*needsRefreshVerbose) {
                std::cout << "refresh-needed: workspace-state-changed\n";
            }
            std::exit(0);
        }
        if (*needsRefreshVerbose) {
            std::cout << "refresh-not-needed\n";
        }
        std::exit(1);
    });

    auto* prepare = cmd->add_subcommand("prepare", "Prepare-stage plan editing utilities");
    auto* addCommitEntry = prepare->add_subcommand("add-commit-entry", "Add or append one commit entry to stages.commit");
    auto* addCommitFile = new std::string{};
    auto* addCommitRepo = new std::string{};
    auto* addCommitMessage = new std::string{};
    auto* addCommitInclude = new std::vector<std::string>{};
    auto* addCommitExclude = new std::vector<std::string>{};
    auto* addCommitReviewVerdict = new std::string{"pass"};
    auto* addCommitReviewReason = new std::string{};
    addCommitEntry->add_option("--plan-file", *addCommitFile, "Plan file path");
    addCommitEntry->add_option("--repo", *addCommitRepo, "Target repo path/key (e.g. . or .agents/kano)")->required();
    addCommitEntry->add_option("--commit-message,--commit.message", *addCommitMessage, "Commit message")->required();
    addCommitEntry->add_option("--commit-include,--commit.include", *addCommitInclude, "Include pathspec (repeatable)");
    addCommitEntry->add_option("--commit-exclude,--commit.exclude", *addCommitExclude, "Exclude pathspec (repeatable)");
    addCommitEntry->add_option("--commit-review-verdict,--commit.review.verdict",
                               *addCommitReviewVerdict,
                               "Review verdict (default: pass)")
        ->default_str("pass");
    addCommitEntry->add_option("--commit-review-reason,--commit.review.reason", *addCommitReviewReason, "Review reason")->required();
    addCommitEntry->callback([=]() {
        const auto workspaceRoot = std::filesystem::current_path().lexically_normal();
        const auto planPath =
            addCommitFile->empty() ? DefaultPlanPath(workspaceRoot) : std::filesystem::path(*addCommitFile).lexically_normal();
        auto payload = ReadFileText(planPath);
        if (!payload.has_value()) {
            std::string error;
            if (!WriteFileText(planPath, BuildDefaultPlanTemplate(workspaceRoot), &error)) {
                std::cerr << "Error: failed to create plan template: " << planPath.generic_string();
                if (!error.empty()) {
                    std::cerr << " (" << error << ")";
                }
                std::cerr << "\n";
                std::exit(2);
            }
            payload = ReadFileText(planPath);
        }
        if (!payload.has_value()) {
            std::cerr << "Error: plan file not found/readable: " << planPath.generic_string() << "\n";
            std::exit(2);
        }
        const auto updated = UpsertCommitEntry(*payload,
                                               Trim(*addCommitRepo),
                                               Trim(*addCommitMessage),
                                               *addCommitInclude,
                                               *addCommitExclude,
                                               Trim(*addCommitReviewVerdict),
                                               Trim(*addCommitReviewReason));
        if (!updated.has_value()) {
            std::cerr << "Error: failed to update stages.commit (schema missing?)\n";
            std::exit(2);
        }
        std::string error;
        if (!WriteFileText(planPath, *updated, &error)) {
            std::cerr << "Error: failed to write plan file: " << planPath.generic_string();
            if (!error.empty()) {
                std::cerr << " (" << error << ")";
            }
            std::cerr << "\n";
            std::exit(2);
        }
        std::cout << "Plan prepare add-commit-entry complete: " << planPath.generic_string() << "\n";
    });

    auto* fillCommit = cmd->add_subcommand("fill-commit", "Fill/update one stages.commit entry by global index");
    auto* fillCommitFile = new std::string{};
    auto* fillCommitIndex = new int{-1};
    auto* fillCommitMessage = new std::string{};
    auto* fillCommitReviewVerdict = new std::string{};
    auto* fillCommitReviewReason = new std::string{};
    fillCommit->add_option("--plan-file", *fillCommitFile, "Plan file path");
    fillCommit->add_option("index", *fillCommitIndex, "Global commit index from `kog plan finish-report`")->required();
    fillCommit->add_option("--commit-message,--commit.message,--message", *fillCommitMessage, "Commit message");
    fillCommit->add_option("--review-verdict,--review.verdict", *fillCommitReviewVerdict, "Review verdict");
    fillCommit->add_option("--review-reason,--review.reason", *fillCommitReviewReason, "Review reason");
    fillCommit->callback([=]() {
        const auto workspaceRoot = std::filesystem::current_path().lexically_normal();
        const auto planPath =
            fillCommitFile->empty() ? DefaultPlanPath(workspaceRoot) : std::filesystem::path(*fillCommitFile).lexically_normal();
        const auto payload = ReadFileText(planPath);
        if (!payload.has_value()) {
            std::cerr << "Error: plan file not found/readable: " << planPath.generic_string() << "\n";
            std::exit(2);
        }

        const auto message = Trim(*fillCommitMessage);
        const auto reviewVerdict = Trim(*fillCommitReviewVerdict);
        const auto reviewReason = Trim(*fillCommitReviewReason);
        const bool hasMessage = !message.empty();
        const bool hasVerdict = !reviewVerdict.empty();
        const bool hasReason = !reviewReason.empty();
        if (!hasMessage && !hasVerdict && !hasReason) {
            std::cerr << "Error: no fill fields provided. Use --commit-message and/or --review-verdict/--review-reason\n";
            std::exit(2);
        }

        std::string fillError;
        const auto updated = FillCommitEntryByFlatIndex(*payload,
                                                         *fillCommitIndex,
                                                         hasMessage ? std::optional<std::string>{message} : std::nullopt,
                                                         hasVerdict ? std::optional<std::string>{reviewVerdict} : std::nullopt,
                                                         hasReason ? std::optional<std::string>{reviewReason} : std::nullopt,
                                                         std::nullopt,
                                                         std::nullopt,
                                                         &fillError);
        if (!updated.has_value()) {
            std::cerr << "Error: failed to fill commit entry";
            if (!fillError.empty()) {
                std::cerr << ": " << fillError;
            }
            std::cerr << "\n";
            std::exit(2);
        }
        std::string error;
        if (!WriteFileText(planPath, *updated, &error)) {
            std::cerr << "Error: failed to write plan file: " << planPath.generic_string();
            if (!error.empty()) {
                std::cerr << " (" << error << ")";
            }
            std::cerr << "\n";
            std::exit(2);
        }
        std::cout << "Plan fill-commit complete: index=" << *fillCommitIndex << " file=" << planPath.generic_string() << "\n";
    });

    auto* getCommit = cmd->add_subcommand("get-commit", "Get one stages.commit entry by global index");
    auto* getCommitFile = new std::string{};
    auto* getCommitIndex = new int{-1};
    auto* getCommitJson = new bool{false};
    getCommit->add_option("--plan-file", *getCommitFile, "Plan file path");
    getCommit->add_option("index", *getCommitIndex, "Global commit index from `kog plan finish-report`")->required();
    getCommit->add_flag("--json", *getCommitJson, "Print machine-readable JSON output");
    getCommit->callback([=]() {
        const auto workspaceRoot = std::filesystem::current_path().lexically_normal();
        const auto planPath =
            getCommitFile->empty() ? DefaultPlanPath(workspaceRoot) : std::filesystem::path(*getCommitFile).lexically_normal();
        const auto payload = ReadFileText(planPath);
        if (!payload.has_value()) {
            std::cerr << "Error: plan file not found/readable: " << planPath.generic_string() << "\n";
            std::exit(2);
        }
        std::string lookupError;
        const auto entry = FindCommitEntryByFlatIndex(*payload, *getCommitIndex, &lookupError);
        if (!entry.has_value()) {
            std::cerr << "Error: failed to read commit entry";
            if (!lookupError.empty()) {
                std::cerr << ": " << lookupError;
            }
            std::cerr << "\n";
            std::exit(2);
        }

        if (*getCommitJson) {
            std::ostringstream oss;
            oss << "{\n";
            oss << "  \"index\": " << entry->index << ",\n";
            oss << "  \"repo\": \"" << JsonEscape(entry->repo) << "\",\n";
            oss << "  \"message\": \"" << JsonEscape(entry->message) << "\",\n";
            oss << "  \"include\": [";
            for (std::size_t i = 0; i < entry->include.size(); ++i) {
                if (i != 0) {
                    oss << ",";
                }
                oss << "\"" << JsonEscape(entry->include[i]) << "\"";
            }
            oss << "],\n";
            oss << "  \"exclude\": [";
            for (std::size_t i = 0; i < entry->exclude.size(); ++i) {
                if (i != 0) {
                    oss << ",";
                }
                oss << "\"" << JsonEscape(entry->exclude[i]) << "\"";
            }
            oss << "],\n";
            oss << "  \"review\": {\n";
            oss << "    \"verdict\": \"" << JsonEscape(entry->reviewVerdict) << "\",\n";
            oss << "    \"reason\": \"" << JsonEscape(entry->reviewReason) << "\"\n";
            oss << "  }\n";
            oss << "}\n";
            std::cout << oss.str();
            return;
        }

        std::cout << "[plan] commit[" << entry->index << "] repo=" << entry->repo << "\n";
        std::cout << "[plan] message: " << entry->message << "\n";
        std::cout << "[plan] review.verdict: " << entry->reviewVerdict << "\n";
        std::cout << "[plan] review.reason: " << entry->reviewReason << "\n";
        std::cout << "[plan] include:\n";
        for (const auto& path : entry->include) {
            std::cout << "  - " << path << "\n";
        }
        std::cout << "[plan] exclude:\n";
        for (const auto& path : entry->exclude) {
            std::cout << "  - " << path << "\n";
        }
    });

    auto* countCommits = cmd->add_subcommand("count-commits", "Count commit entries and review-needed items in stages.commit");
    auto* countCommitsFile = new std::string{};
    auto* countCommitsJson = new bool{false};
    countCommits->add_option("--plan-file", *countCommitsFile, "Plan file path");
    countCommits->add_flag("--json", *countCommitsJson, "Print machine-readable JSON output");
    countCommits->callback([=]() {
        const auto workspaceRoot = std::filesystem::current_path().lexically_normal();
        const auto planPath = countCommitsFile->empty() ? DefaultPlanPath(workspaceRoot)
                                                        : std::filesystem::path(*countCommitsFile).lexically_normal();
        const auto payload = ReadFileText(planPath);
        if (!payload.has_value()) {
            std::cerr << "Error: plan file not found/readable: " << planPath.generic_string() << "\n";
            std::exit(2);
        }
        const auto entries = CollectCommitPlanEntries(*payload);
        const auto reviewNeededIndexes = CollectCommitIndexesNeedingReview(entries);
        if (*countCommitsJson) {
            std::ostringstream oss;
            oss << "{\n";
            oss << "  \"total_commits\": " << entries.size() << ",\n";
            oss << "  \"review_needed_commits\": " << reviewNeededIndexes.size() << ",\n";
            oss << "  \"review_needed_indexes\": [";
            for (std::size_t i = 0; i < reviewNeededIndexes.size(); ++i) {
                if (i != 0) {
                    oss << ",";
                }
                oss << reviewNeededIndexes[i];
            }
            oss << "]\n";
            oss << "}\n";
            std::cout << oss.str();
            return;
        }
        std::cout << "[plan] total_commits: " << entries.size() << "\n";
        std::cout << "[plan] review_needed_commits: " << reviewNeededIndexes.size() << "\n";
        std::cout << "[plan] review_needed_indexes:";
        if (reviewNeededIndexes.empty()) {
            std::cout << " <none>\n";
            return;
        }
        for (const auto index : reviewNeededIndexes) {
            std::cout << " " << index;
        }
        std::cout << "\n";
    });

    auto* summary = cmd->add_subcommand("finish-report", "Print compact plan summary");
    auto* summaryFile = new std::string{};
    auto* summaryMax = new int{10};
    summary->add_option("--plan-file", *summaryFile, "Plan file path");
    summary->add_option("--max-commits", *summaryMax, "Max commit lines to print")->default_val(10);
    summary->callback([=]() {
        const auto workspaceRoot = std::filesystem::current_path().lexically_normal();
        const auto planPath = summaryFile->empty() ? DefaultPlanPath(workspaceRoot) : std::filesystem::path(*summaryFile).lexically_normal();
        const auto payload = ReadFileText(planPath);
        if (!payload.has_value()) {
            std::cerr << "Error: plan file not found/readable: " << planPath.generic_string() << "\n";
            std::exit(2);
        }
        const auto text = *payload;
        const auto meta = ExtractObjectBodyForKey(text, "meta");
        const auto stages = ExtractObjectBodyForKey(text, "stages");
        if (!meta.has_value() || !stages.has_value()) {
            std::cerr << "Error: plan schema invalid: missing meta/stages\n";
            std::exit(2);
        }
        const auto planner = ExtractObjectBodyForKey(*meta, "planner");
        const auto planId = ExtractStringField(*meta, "plan_id").value_or("-");
        const auto generated = ExtractStringField(*meta, "generated_at_utc").value_or("-");
        const auto provider = planner.has_value() ? ExtractStringField(*planner, "provider").value_or("-") : "-";
        const auto model = planner.has_value() ? ExtractStringField(*planner, "ai-model").value_or("-") : "-";
        std::cout << std::format("[plan] meta: plan_id={} generated={} provider={} ai-model={}\n", planId, generated, provider, model);

        const auto commitArray = ExtractArrayBodyForKey(*stages, "commit").value_or(std::string{});
        std::size_t repoCount = 0;
        std::size_t commitCount = 0;
        std::size_t flatIndex = 0;
        std::vector<std::string> lines;
        for (const auto& repoObj : SplitTopLevelObjects(commitArray)) {
            const auto repo = ExtractStringField(repoObj, "repo").value_or("?");
            const auto commits = ExtractArrayBodyForKey(repoObj, "commits").value_or(std::string{});
            const auto commitObjects = SplitTopLevelObjects(commits);
            if (!commitObjects.empty()) {
                repoCount += 1;
            }
            commitCount += commitObjects.size();
            for (const auto& commitObj : commitObjects) {
                const auto msg = ExtractStringField(commitObj, "message").value_or("");
                lines.push_back(std::format("[plan] - [{}] {}: {}", flatIndex, repo, msg));
                flatIndex += 1;
            }
        }
        std::cout << std::format("[plan] commits: repos={} total={}\n", repoCount, commitCount);
        if (*summaryMax < 0) {
            *summaryMax = 0;
        }
        const auto limit = std::min<std::size_t>(lines.size(), static_cast<std::size_t>(*summaryMax));
        for (std::size_t i = 0; i < limit; ++i) {
            std::cout << lines[i] << "\n";
        }
    });

    auto* ensureAiReady = cmd->add_subcommand("ensure-prepare-ready", "Ensure plan is prepared and AI-ready");
    auto* ensureFile = new std::string{};
    auto* ensureProvider = new std::string{"auto"};
    auto* ensureModel = new std::string{};
    auto* ensureFillMode = new std::string{defaultCommitGenerationMode};
    auto* ensureDebugAi = new bool{false};
    auto* ensureAllowIgnoreGate = new bool{false};
    auto* ensureForce = new bool{false};
    ensureAiReady->add_option("--plan-file", *ensureFile, "Plan file path");
    ensureAiReady->add_option("--ai-provider,--provider", *ensureProvider, "AI provider (copilot|codex|opencode|auto)")->default_str("auto");
    ensureAiReady->add_option("--ai-model,--model", *ensureModel, "AI model (default: layered kog_config -> auto policy)");
    ensureAiReady->add_option("--ai-commit-generation-mode,--ai-fill-mode",
                              *ensureFillMode,
                              "AI commit generation mode: single=one workspace-wide pass, per-commit=one AI pass per commit, adaptive=per-commit + deterministic gitlink fallback (single|per-commit|adaptive)")
        ->default_str(defaultCommitGenerationMode);
    ensureAiReady->add_flag("--debug-ai", *ensureDebugAi, "Write AI prompt/raw/extracted debug artifacts");
    ensureAiReady->add_flag("--allow-ignore-gate", *ensureAllowIgnoreGate, "Compatibility flag (currently no-op in prepare)");
    ensureAiReady->add_flag("--force,-f", *ensureForce, "Force regenerate even if existing plan looks complete");
    ensureAiReady->callback([=]() {
        const auto workspaceRoot = std::filesystem::current_path().lexically_normal();
        const auto planPath = ensureFile->empty() ? DefaultPlanPath(workspaceRoot) : std::filesystem::path(*ensureFile).lexically_normal();
        const auto payload = ReadFileText(planPath);
        const bool needs = *ensureForce || !payload.has_value() || PlanNeedsRefresh(*payload) ||
                           (payload.has_value() && PlanWorkspaceStateDrifted(workspaceRoot, *payload));
        std::optional<std::string> latestPayload = payload;
        auto regenerateOnce = [&]() -> bool {
            std::string error;
            if (!WriteFileText(planPath, BuildDefaultPlanTemplate(workspaceRoot), &error)) {
                std::cerr << "Error: failed to write plan template: " << planPath.generic_string();
                if (!error.empty()) {
                    std::cerr << " (" << error << ")";
                }
                std::cerr << "\n";
                return false;
            }
            if (const auto seeded = SeedCommitStage(workspaceRoot, ReadFileText(planPath).value_or(std::string{}), true, true);
                seeded.has_value()) {
                std::string seedError;
                if (!WriteFileText(planPath, *seeded, &seedError)) {
                    std::cerr << "Error: failed to seed commit stage: " << planPath.generic_string();
                    if (!seedError.empty()) {
                        std::cerr << " (" << seedError << ")";
                    }
                    std::cerr << "\n";
                    return false;
                }
            }
            std::string aiError;
            if (!FillPlanByAi(workspaceRoot, planPath, *ensureProvider, *ensureModel, *ensureFillMode, *ensureDebugAi, &aiError)) {
                std::cerr << aiError << "\n";
                return false;
            }
            latestPayload = ReadFileText(planPath);
            return latestPayload.has_value();
        };
        if (needs) {
            if (!regenerateOnce()) {
                std::exit(2);
            }
        }
        if (!latestPayload.has_value()) {
            std::cerr << "Error: plan file not found/readable: " << planPath.generic_string() << "\n";
            std::exit(2);
        }

        std::string reason;
        if (!ValidateAiReadyPlan(*latestPayload, &reason)) {
            std::cerr << std::format("[plan] validation failed ({}), regenerating once...\n", reason);
            if (!regenerateOnce()) {
                std::exit(2);
            }
            reason.clear();
            if (!latestPayload.has_value() || !ValidateAiReadyPlan(*latestPayload, &reason)) {
                if (reason == "no commit entries in stages.commit" && latestPayload.has_value() &&
                    AllowDeterministicCommitFallbackForMode(defaultCommitGenerationMode)) {
                    if (const auto fallback = TryInjectFallbackCommits(workspaceRoot, *latestPayload); fallback.has_value()) {
                        std::string writeError;
                        if (!WriteFileText(planPath, *fallback, &writeError)) {
                            std::cerr << "Error: failed to write fallback commit plan: " << planPath.generic_string();
                            if (!writeError.empty()) {
                                std::cerr << " (" << writeError << ")";
                            }
                            std::cerr << "\n";
                            std::exit(2);
                        }
                        latestPayload = ReadFileText(planPath);
                        reason.clear();
                        if (latestPayload.has_value() && ValidateAiReadyPlan(*latestPayload, &reason)) {
                            std::cerr << "[plan] fallback commit entries injected after empty AI commit stage.\n";
                            std::cout << "Plan ensure-prepare-ready passed: " << planPath.generic_string() << "\n";
                            return;
                        }
                    }
                }
                std::cerr << "Error: AI-ready plan validation failed: " << reason << "\n";
                std::exit(2);
            }
        }
        std::cout << "Plan ensure-prepare-ready passed: " << planPath.generic_string() << "\n";
    });

    auto* preflightAiCommit = cmd->add_subcommand("runbook-commit", "Run commit runbook (prepare + summary + validate)");
    preflightAiCommit->group("");
    auto* preflightFile = new std::string{};
    auto* preflightProvider = new std::string{"auto"};
    auto* preflightModel = new std::string{};
    auto* preflightFillMode = new std::string{defaultCommitGenerationMode};
    auto* preflightDebugAi = new bool{false};
    auto* preflightAllowIgnoreGate = new bool{false};
    auto* preflightMaxCommits = new int{10};
    preflightAiCommit->add_option("--plan-file", *preflightFile, "Plan file path");
    preflightAiCommit->add_option("--ai-provider,--provider", *preflightProvider, "AI provider (copilot|codex|opencode|auto)")
        ->default_str("auto");
    preflightAiCommit->add_option("--ai-model,--model", *preflightModel, "AI model (default: layered kog_config -> auto policy)");
    preflightAiCommit->add_option("--ai-commit-generation-mode,--ai-fill-mode",
                                  *preflightFillMode,
                                  "AI commit generation mode: single=one workspace-wide pass, per-commit=one AI pass per commit, adaptive=per-commit + deterministic gitlink fallback (single|per-commit|adaptive)")
        ->default_str(defaultCommitGenerationMode);
    preflightAiCommit->add_flag("--debug-ai", *preflightDebugAi, "Write AI prompt/raw/extracted debug artifacts");
    preflightAiCommit->add_flag("--allow-ignore-gate", *preflightAllowIgnoreGate, "Compatibility flag (currently no-op in runbook-commit)");
    preflightAiCommit->add_option("--max-commits", *preflightMaxCommits, "Max commit lines to print in summary")->default_val(10);
    const auto runCommitRunbook = [&](const std::filesystem::path& InWorkspaceRoot,
                                      const std::filesystem::path& InPlanPath,
                                      const std::string& InProvider,
                                      const std::string& InModel,
                                      const std::string& InFillMode,
                                      const bool InDebugAi,
                                      const int InMaxCommits) -> int {
        const auto dirtyContext = CollectDirtyRepoContextText(InWorkspaceRoot);
        if (Trim(dirtyContext).empty()) {
            std::cerr << "[plan] workspace clean; skip commit runbook (no-op).\n";
            return 0;
        }
        auto payload = ReadFileText(InPlanPath);
        const bool needs = !payload.has_value() || PlanNeedsRefresh(*payload) ||
                           (payload.has_value() && PlanWorkspaceStateDrifted(InWorkspaceRoot, *payload));
        auto regenerateOnce = [&]() -> bool {
            std::string error;
            if (!WriteFileText(InPlanPath, BuildDefaultPlanTemplate(InWorkspaceRoot), &error)) {
                std::cerr << "Error: failed to write plan template: " << InPlanPath.generic_string();
                if (!error.empty()) {
                    std::cerr << " (" << error << ")";
                }
                std::cerr << "\n";
                return false;
            }
            if (const auto seeded = SeedCommitStage(InWorkspaceRoot, ReadFileText(InPlanPath).value_or(std::string{}), true, true);
                seeded.has_value()) {
                std::string seedError;
                if (!WriteFileText(InPlanPath, *seeded, &seedError)) {
                    std::cerr << "Error: failed to seed commit stage: " << InPlanPath.generic_string();
                    if (!seedError.empty()) {
                        std::cerr << " (" << seedError << ")";
                    }
                    std::cerr << "\n";
                    return false;
                }
            }
            std::string aiError;
            if (!FillPlanByAi(InWorkspaceRoot, InPlanPath, InProvider, InModel, InFillMode, InDebugAi, &aiError)) {
                std::cerr << aiError << "\n";
                return false;
            }
            payload = ReadFileText(InPlanPath);
            return payload.has_value();
        };
        if (needs && !regenerateOnce()) {
            return 2;
        }
        if (!payload.has_value()) {
            std::cerr << "Error: plan file not found/readable: " << InPlanPath.generic_string() << "\n";
            return 2;
        }

        std::string reason;
        if (!ValidateAiReadyPlan(*payload, &reason)) {
            std::cerr << std::format("[plan] validation failed ({}), regenerating once...\n", reason);
            if (!regenerateOnce()) {
                return 2;
            }
            reason.clear();
            if (!payload.has_value() || !ValidateAiReadyPlan(*payload, &reason)) {
                if (reason == "no commit entries in stages.commit" && payload.has_value()) {
                    bool fixedByFallback = false;
                    if (AllowDeterministicCommitFallbackForMode(InFillMode)) {
                        if (const auto fallback = TryInjectFallbackCommits(InWorkspaceRoot, *payload); fallback.has_value()) {
                            std::string writeError;
                            if (!WriteFileText(InPlanPath, *fallback, &writeError)) {
                                std::cerr << "Error: failed to write fallback commit plan: " << InPlanPath.generic_string();
                                if (!writeError.empty()) {
                                    std::cerr << " (" << writeError << ")";
                                }
                                std::cerr << "\n";
                                return 2;
                            }
                            payload = ReadFileText(InPlanPath);
                            reason.clear();
                            if (payload.has_value() && ValidateAiReadyPlan(*payload, &reason)) {
                                std::cerr << "[plan] fallback commit entries injected after empty AI commit stage.\n";
                                fixedByFallback = true;
                            } else {
                                std::cerr << "Error: AI-ready plan validation failed: " << reason << "\n";
                                return 2;
                            }
                        }
                    }
                    if (!fixedByFallback) {
                        std::cerr << "Error: AI-ready plan validation failed: " << reason << "\n";
                        return 2;
                    }
                } else {
                    std::cerr << "Error: AI-ready plan validation failed: " << reason << "\n";
                    return 2;
                }
            }
        }

        return 0;
    };

    preflightAiCommit->callback([=]() {
        const auto workspaceRoot = std::filesystem::current_path().lexically_normal();
        const auto planPath = preflightFile->empty() ? DefaultPlanPath(workspaceRoot) : std::filesystem::path(*preflightFile).lexically_normal();
        const auto code = runCommitRunbook(
            workspaceRoot, planPath, *preflightProvider, *preflightModel, *preflightFillMode, *preflightDebugAi, *preflightMaxCommits);
        std::exit(code);
    });

    const auto runPreApplyVerify = [&](const std::filesystem::path& InWorkspaceRoot,
                                       const std::filesystem::path& InPlanPath,
                                       const std::string& InStage) -> int {
        const auto payload = ReadFileText(InPlanPath);
        if (!payload.has_value()) {
            std::cerr << "Error: plan file not found/readable: " << InPlanPath.generic_string() << "\n";
            std::cerr << "Hint: create one with `kog plan new --plan-file \"" << InPlanPath.generic_string() << "\"`.\n";
            return 2;
        }
        const auto text = *payload;
        const auto stages = ExtractObjectBodyForKey(text, "stages");
        if (!stages.has_value() || !ExtractObjectBodyForKey(text, "meta").has_value()) {
            std::cerr << "Error: plan schema invalid: missing meta/stages\n";
            std::cerr << "Hint: regenerate template with `kog plan new --force --plan-file \"" << InPlanPath.generic_string() << "\"`.\n";
            return 2;
        }
        const auto stage = ToLower(Trim(InStage));
        if (stage != "ignore" && stage != "commit" && stage != "all") {
            std::cerr << "Error: invalid --stage value: " << InStage << " (expected ignore|commit|all)\n";
            return 2;
        }
        if (stage == "ignore" || stage == "all") {
            if (!ExtractArrayBodyForKey(*stages, "ignore").has_value()) {
                std::cerr << "Error: plan schema invalid: stages.ignore missing\n";
                std::cerr << "Hint: run `kog plan ignore-init --force --plan-file \"" << InPlanPath.generic_string() << "\"`.\n";
                return 2;
            }
        }
        if (stage == "commit" || stage == "all") {
            if (!ExtractArrayBodyForKey(*stages, "commit").has_value() || !ExtractArrayBodyForKey(*stages, "post_sync").has_value()) {
                std::cerr << "Error: plan schema invalid: stages.commit/post_sync missing\n";
                std::cerr << "Hint: regenerate template with `kog plan new --force --plan-file \"" << InPlanPath.generic_string() << "\"`.\n";
                return 2;
            }
        }
        std::string planBaseHeadSha;
        std::string planDirtyFingerprint;
        if (!ExtractPlanWorkspaceHashes(text, &planBaseHeadSha, &planDirtyFingerprint)) {
            std::cerr << "Error: plan schema invalid: meta.base_head_sha/meta.dirty_fingerprint missing or placeholder\n";
            std::cerr << "Hint: regenerate and refill with `kog plan runbook commit --force --plan-file \"" << InPlanPath.generic_string() << "\"`.\n";
            return 2;
        }
        const auto currentBaseHeadSha = ComputeWorkspaceBaseHeadSha(InWorkspaceRoot);
        const auto currentDirtyFingerprint = ComputeWorkspaceDirtyFingerprint(InWorkspaceRoot);
        if (planBaseHeadSha != currentBaseHeadSha || planDirtyFingerprint != currentDirtyFingerprint) {
            std::cerr << "Error: plan schema invalid: workspace state drift detected.\n";
            std::cerr << "  plan.base_head_sha=" << planBaseHeadSha << "\n";
            std::cerr << "  current.base_head_sha=" << currentBaseHeadSha << "\n";
            std::cerr << "  plan.dirty_fingerprint=" << planDirtyFingerprint << "\n";
            std::cerr << "  current.dirty_fingerprint=" << currentDirtyFingerprint << "\n";
            std::cerr << "Hint: refresh plan via `kog plan runbook commit --force --plan-file \"" << InPlanPath.generic_string() << "\"`.\n";
            return 2;
        }
        return 0;
    };

    const auto runIgnoreRunbook = [&](const std::filesystem::path& InWorkspaceRoot,
                                      const std::filesystem::path& InPlanPath,
                                      const bool InForce,
                                      const int InMaxPerRepo,
                                      const std::string& InDatasourceRoot,
                                      const std::string& InDatasourceManifest) -> int {
        if (InMaxPerRepo <= 0) {
            std::cerr << "Error: --max-per-repo must be positive\n";
            return 2;
        }
        auto payload = ReadFileText(InPlanPath);
        const auto datasourceRoot = InDatasourceRoot.empty()
                                        ? std::optional<std::filesystem::path>{}
                                        : std::optional<std::filesystem::path>{ResolvePath(InWorkspaceRoot, InDatasourceRoot)};
        const auto datasourceManifest = InDatasourceManifest.empty()
                                            ? std::optional<std::filesystem::path>{}
                                            : std::optional<std::filesystem::path>{ResolvePath(InWorkspaceRoot, InDatasourceManifest)};
        if (!payload.has_value()) {
            if (!InForce) {
                std::cerr << "Error: plan file not found/readable: " << InPlanPath.generic_string() << "\n";
                std::cerr << "Hint: run `kog plan new --plan-file \"" << InPlanPath.generic_string()
                          << "\"` first, or rerun with `kog plan ignore-init --force --plan-file \"" << InPlanPath.generic_string()
                          << "\"`.\n";
                return 2;
            }
            std::string error;
            const auto seed = BuildDefaultPlanTemplate(InWorkspaceRoot, datasourceRoot, datasourceManifest);
            if (!WriteFileText(InPlanPath, seed, &error)) {
                std::cerr << "Error: failed to create plan template: " << InPlanPath.generic_string();
                if (!error.empty()) {
                    std::cerr << " (" << error << ")";
                }
                std::cerr << "\n";
                return 2;
            }
            payload = seed;
        }
        const auto entries = BuildIgnoreEntriesFromWorkingTree(InWorkspaceRoot, InMaxPerRepo);
        auto updated = InjectIgnoreEntries(*payload, entries);
        if (!updated.has_value()) {
            std::cerr << "Error: plan schema invalid: cannot locate stages.ignore array\n";
            std::cerr << "Hint: regenerate template with `kog plan new --force --plan-file \"" << InPlanPath.generic_string()
                      << "\"`, then rerun `kog plan ignore-init --plan-file \"" << InPlanPath.generic_string() << "\"`.\n";
            return 2;
        }
        if (datasourceRoot.has_value() || datasourceManifest.has_value()) {
            updated = ApplyIgnoreDatasourceOverrides(*updated, datasourceRoot, datasourceManifest);
            if (!updated.has_value()) {
                std::cerr << "Error: plan schema invalid: cannot update meta.ignore_datasource root/manifest\n";
                return 2;
            }
        }
        std::string error;
        if (!WriteFileText(InPlanPath, *updated, &error)) {
            std::cerr << "Error: failed to write plan ignore stage: " << InPlanPath.generic_string();
            if (!error.empty()) {
                std::cerr << " (" << error << ")";
            }
            std::cerr << "\n";
            return 2;
        }
        std::size_t totalRules = 0;
        for (const auto& e : entries) {
            totalRules += e.rules.size();
        }
        std::cout << std::format("Plan ignore-init complete: repos={} rules={} file={}\n",
                                 entries.size(),
                                 totalRules,
                                 InPlanPath.generic_string());
        return runPreApplyVerify(InWorkspaceRoot, InPlanPath, "ignore");
    };

    auto* runbookIgnore = cmd->add_subcommand("runbook-ignore", "Run ignore runbook (init + pre-apply verify)");
    runbookIgnore->group("");
    auto* runbookIgnoreFile = new std::string{};
    auto* runbookIgnoreForce = new bool{false};
    auto* runbookIgnoreMaxPerRepo = new int{200};
    auto* runbookIgnoreDatasourceRoot = new std::string{};
    auto* runbookIgnoreDatasourceManifest = new std::string{};
    runbookIgnore->add_option("--plan-file", *runbookIgnoreFile, "Plan file path");
    runbookIgnore->add_flag("--force,-f", *runbookIgnoreForce, "Create default plan when file missing");
    runbookIgnore->add_option("--max-per-repo", *runbookIgnoreMaxPerRepo, "Max ignore candidates per repo")->default_val(200);
    runbookIgnore->add_option("--ignore-datasource-root", *runbookIgnoreDatasourceRoot, "Override plan meta.ignore_datasource.root");
    runbookIgnore->add_option("--ignore-datasource-manifest", *runbookIgnoreDatasourceManifest, "Override plan meta.ignore_datasource.manifest");
    runbookIgnore->callback([=]() {
        const auto workspaceRoot = std::filesystem::current_path().lexically_normal();
        const auto planPath =
            runbookIgnoreFile->empty() ? DefaultPlanPath(workspaceRoot) : std::filesystem::path(*runbookIgnoreFile).lexically_normal();
        const auto code = runIgnoreRunbook(workspaceRoot,
                                           planPath,
                                           *runbookIgnoreForce,
                                           *runbookIgnoreMaxPerRepo,
                                           *runbookIgnoreDatasourceRoot,
                                           *runbookIgnoreDatasourceManifest);
        std::exit(code);
    });

    auto* runbookFull = cmd->add_subcommand("runbook-full", "Run full runbook (ignore + commit + pre-apply verify)");
    runbookFull->group("");
    auto* runbookFullFile = new std::string{};
    auto* runbookFullProvider = new std::string{"auto"};
    auto* runbookFullModel = new std::string{};
    auto* runbookFullFillMode = new std::string{defaultCommitGenerationMode};
    auto* runbookFullDebugAi = new bool{false};
    auto* runbookFullAllowIgnoreGate = new bool{false};
    auto* runbookFullForce = new bool{false};
    auto* runbookFullMaxCommits = new int{10};
    auto* runbookFullMaxPerRepo = new int{200};
    runbookFull->add_option("--plan-file", *runbookFullFile, "Plan file path");
    runbookFull->add_option("--ai-provider,--provider", *runbookFullProvider, "AI provider (copilot|codex|opencode|auto)")->default_str("auto");
    runbookFull->add_option("--ai-model,--model", *runbookFullModel, "AI model (default: layered kog_config -> auto policy)");
    runbookFull->add_option("--ai-commit-generation-mode,--ai-fill-mode",
                            *runbookFullFillMode,
                            "AI commit generation mode: single=one workspace-wide pass, per-commit=one AI pass per commit, adaptive=per-commit + deterministic gitlink fallback (single|per-commit|adaptive)")
        ->default_str(defaultCommitGenerationMode);
    runbookFull->add_flag("--debug-ai", *runbookFullDebugAi, "Write AI prompt/raw/extracted debug artifacts");
    runbookFull->add_flag("--allow-ignore-gate", *runbookFullAllowIgnoreGate, "Compatibility flag (forwarded to commit runbook)");
    runbookFull->add_flag("--force,-f", *runbookFullForce, "Create default plan when file missing during ignore runbook");
    runbookFull->add_option("--max-commits", *runbookFullMaxCommits, "Max commit lines to print in summary")->default_val(10);
    runbookFull->add_option("--max-per-repo", *runbookFullMaxPerRepo, "Max ignore candidates per repo")->default_val(200);
    runbookFull->callback([=]() {
        const auto workspaceRoot = std::filesystem::current_path().lexically_normal();
        const auto planPath =
            runbookFullFile->empty() ? DefaultPlanPath(workspaceRoot) : std::filesystem::path(*runbookFullFile).lexically_normal();
        const auto ignoreCode = runIgnoreRunbook(workspaceRoot, planPath, *runbookFullForce, *runbookFullMaxPerRepo, "", "");
        if (ignoreCode != 0) {
            std::exit(ignoreCode);
        }
        const auto commitCode = runCommitRunbook(workspaceRoot,
                                                 planPath,
                                                 *runbookFullProvider,
                                                 *runbookFullModel,
                                                 *runbookFullFillMode,
                                                 *runbookFullDebugAi,
                                                 *runbookFullMaxCommits);
        if (commitCode != 0) {
            std::exit(commitCode);
        }

        const auto verifyCode = runPreApplyVerify(workspaceRoot, planPath, "all");
        std::exit(verifyCode);
    });

    auto* runbook = cmd->add_subcommand("runbook", "Plan runbooks");
    auto* runbookCommit = runbook->add_subcommand("commit", "Run commit runbook (prepare + summary + pre-apply verify)");
    auto* rbCommitFile = new std::string{};
    auto* rbCommitProvider = new std::string{"auto"};
    auto* rbCommitModel = new std::string{};
    auto* rbCommitFillMode = new std::string{defaultCommitGenerationMode};
    auto* rbCommitDebugAi = new bool{false};
    auto* rbCommitAllowIgnoreGate = new bool{false};
    auto* rbCommitMaxCommits = new int{10};
    runbookCommit->add_option("--plan-file", *rbCommitFile, "Plan file path");
    runbookCommit->add_option("--ai-provider,--provider", *rbCommitProvider, "AI provider (copilot|codex|opencode|auto)")->default_str("auto");
    runbookCommit->add_option("--ai-model,--model", *rbCommitModel, "AI model (default: layered kog_config -> auto policy)");
    runbookCommit->add_option("--ai-commit-generation-mode,--ai-fill-mode",
                              *rbCommitFillMode,
                              "AI commit generation mode: single=one workspace-wide pass, per-commit=one AI pass per commit, adaptive=per-commit + deterministic gitlink fallback (single|per-commit|adaptive)")
        ->default_str(defaultCommitGenerationMode);
    runbookCommit->add_flag("--debug-ai", *rbCommitDebugAi, "Write AI prompt/raw/extracted debug artifacts");
    runbookCommit->add_flag("--allow-ignore-gate", *rbCommitAllowIgnoreGate, "Forward allow-ignore-gate to commit runbook");
    runbookCommit->add_option("--max-commits", *rbCommitMaxCommits, "Max commit lines to print in summary")->default_val(10);
    runbookCommit->callback([=]() {
        const auto workspaceRoot = std::filesystem::current_path().lexically_normal();
        const auto planPath = rbCommitFile->empty() ? DefaultPlanPath(workspaceRoot) : std::filesystem::path(*rbCommitFile).lexically_normal();
        const auto code =
            runCommitRunbook(workspaceRoot, planPath, *rbCommitProvider, *rbCommitModel, *rbCommitFillMode, *rbCommitDebugAi, *rbCommitMaxCommits);
        std::exit(code);
    });

    auto* runbookIgnorePublic = runbook->add_subcommand("ignore", "Run ignore runbook (init + pre-apply verify)");
    auto* rbIgnoreFile = new std::string{};
    auto* rbIgnoreForce = new bool{false};
    auto* rbIgnoreMaxPerRepo = new int{200};
    auto* rbIgnoreDatasourceRoot = new std::string{};
    auto* rbIgnoreDatasourceManifest = new std::string{};
    runbookIgnorePublic->add_option("--plan-file", *rbIgnoreFile, "Plan file path");
    runbookIgnorePublic->add_flag("--force,-f", *rbIgnoreForce, "Create default plan when file missing");
    runbookIgnorePublic->add_option("--max-per-repo", *rbIgnoreMaxPerRepo, "Max ignore candidates per repo")->default_val(200);
    runbookIgnorePublic->add_option("--ignore-datasource-root", *rbIgnoreDatasourceRoot, "Override plan meta.ignore_datasource.root");
    runbookIgnorePublic->add_option("--ignore-datasource-manifest", *rbIgnoreDatasourceManifest, "Override plan meta.ignore_datasource.manifest");
    runbookIgnorePublic->callback([=]() {
        const auto workspaceRoot = std::filesystem::current_path().lexically_normal();
        const auto planPath = rbIgnoreFile->empty() ? DefaultPlanPath(workspaceRoot) : std::filesystem::path(*rbIgnoreFile).lexically_normal();
        const auto code = runIgnoreRunbook(
            workspaceRoot, planPath, *rbIgnoreForce, *rbIgnoreMaxPerRepo, *rbIgnoreDatasourceRoot, *rbIgnoreDatasourceManifest);
        std::exit(code);
    });

    auto* runbookFullPublic = runbook->add_subcommand("full", "Run full runbook (ignore + commit + pre-apply verify)");
    auto* rbFullFile = new std::string{};
    auto* rbFullProvider = new std::string{"auto"};
    auto* rbFullModel = new std::string{};
    auto* rbFullFillMode = new std::string{defaultCommitGenerationMode};
    auto* rbFullDebugAi = new bool{false};
    auto* rbFullAllowIgnoreGate = new bool{false};
    auto* rbFullForce = new bool{false};
    auto* rbFullMaxCommits = new int{10};
    auto* rbFullMaxPerRepo = new int{200};
    runbookFullPublic->add_option("--plan-file", *rbFullFile, "Plan file path");
    runbookFullPublic->add_option("--ai-provider,--provider", *rbFullProvider, "AI provider (copilot|codex|opencode|auto)")->default_str("auto");
    runbookFullPublic->add_option("--ai-model,--model", *rbFullModel, "AI model (default: layered kog_config -> auto policy)");
    runbookFullPublic->add_option("--ai-commit-generation-mode,--ai-fill-mode",
                                  *rbFullFillMode,
                                  "AI commit generation mode: single=one workspace-wide pass, per-commit=one AI pass per commit, adaptive=per-commit + deterministic gitlink fallback (single|per-commit|adaptive)")
        ->default_str(defaultCommitGenerationMode);
    runbookFullPublic->add_flag("--debug-ai", *rbFullDebugAi, "Write AI prompt/raw/extracted debug artifacts");
    runbookFullPublic->add_flag("--allow-ignore-gate", *rbFullAllowIgnoreGate, "Forward allow-ignore-gate to commit runbook");
    runbookFullPublic->add_flag("--force,-f", *rbFullForce, "Create default plan when file missing during ignore runbook");
    runbookFullPublic->add_option("--max-commits", *rbFullMaxCommits, "Max commit lines to print in summary")->default_val(10);
    runbookFullPublic->add_option("--max-per-repo", *rbFullMaxPerRepo, "Max ignore candidates per repo")->default_val(200);
    runbookFullPublic->callback([=]() {
        const auto workspaceRoot = std::filesystem::current_path().lexically_normal();
        const auto planPath = rbFullFile->empty() ? DefaultPlanPath(workspaceRoot) : std::filesystem::path(*rbFullFile).lexically_normal();
        const auto ignoreCode = runIgnoreRunbook(workspaceRoot, planPath, *rbFullForce, *rbFullMaxPerRepo, "", "");
        if (ignoreCode != 0) {
            std::exit(ignoreCode);
        }
        const auto commitCode =
            runCommitRunbook(workspaceRoot, planPath, *rbFullProvider, *rbFullModel, *rbFullFillMode, *rbFullDebugAi, *rbFullMaxCommits);
        if (commitCode != 0) {
            std::exit(commitCode);
        }
        const auto verifyCode = runPreApplyVerify(workspaceRoot, planPath, "all");
        std::exit(verifyCode);
    });

    auto* ignoreInit = cmd->add_subcommand("ignore-init", "Populate stages.ignore from current working tree");
    auto* ignoreInitFile = new std::string{};
    auto* ignoreInitForce = new bool{false};
    auto* ignoreInitMaxPerRepo = new int{200};
    auto* ignoreInitDatasourceRoot = new std::string{};
    auto* ignoreInitDatasourceManifest = new std::string{};
    ignoreInit->add_option("--plan-file", *ignoreInitFile, "Plan file path");
    ignoreInit->add_flag("--force,-f", *ignoreInitForce, "Create default plan when file missing");
    ignoreInit->add_option("--max-per-repo", *ignoreInitMaxPerRepo, "Max ignore candidates per repo")->default_val(200);
    ignoreInit->add_option("--ignore-datasource-root",
                           *ignoreInitDatasourceRoot,
                           "Override plan meta.ignore_datasource.root");
    ignoreInit->add_option("--ignore-datasource-manifest",
                           *ignoreInitDatasourceManifest,
                           "Override plan meta.ignore_datasource.manifest");
    ignoreInit->callback([=]() {
        const auto workspaceRoot = std::filesystem::current_path().lexically_normal();
        const auto planPath = ignoreInitFile->empty() ? DefaultPlanPath(workspaceRoot) : std::filesystem::path(*ignoreInitFile).lexically_normal();
        auto payload = ReadFileText(planPath);
        const auto datasourceRoot = ignoreInitDatasourceRoot->empty()
                                        ? std::optional<std::filesystem::path>{}
                                        : std::optional<std::filesystem::path>{ResolvePath(workspaceRoot, *ignoreInitDatasourceRoot)};
        const auto datasourceManifest = ignoreInitDatasourceManifest->empty()
                                            ? std::optional<std::filesystem::path>{}
                                            : std::optional<std::filesystem::path>{ResolvePath(workspaceRoot, *ignoreInitDatasourceManifest)};
        if (!payload.has_value()) {
            if (!*ignoreInitForce) {
                std::cerr << "Error: plan file not found/readable: " << planPath.generic_string() << "\n";
                std::cerr << "Hint: run `kog plan new --plan-file \"" << planPath.generic_string()
                          << "\"` first, or rerun with `kog plan ignore-init --force --plan-file \"" << planPath.generic_string() << "\"`.\n";
                std::exit(2);
            }
            std::string error;
            const auto seed = BuildDefaultPlanTemplate(workspaceRoot, datasourceRoot, datasourceManifest);
            if (!WriteFileText(planPath, seed, &error)) {
                std::cerr << "Error: failed to create plan template: " << planPath.generic_string();
                if (!error.empty()) {
                    std::cerr << " (" << error << ")";
                }
                std::cerr << "\n";
                std::exit(2);
            }
            payload = seed;
        }
        if (*ignoreInitMaxPerRepo <= 0) {
            std::cerr << "Error: --max-per-repo must be positive\n";
            std::exit(2);
        }
        const auto entries = BuildIgnoreEntriesFromWorkingTree(workspaceRoot, *ignoreInitMaxPerRepo);
        auto updated = InjectIgnoreEntries(*payload, entries);
        if (!updated.has_value()) {
            std::cerr << "Error: plan schema invalid: cannot locate stages.ignore array\n";
            std::cerr << "Hint: regenerate template with `kog plan new --force --plan-file \"" << planPath.generic_string()
                      << "\"`, then rerun `kog plan ignore-init --plan-file \"" << planPath.generic_string() << "\"`.\n";
            std::exit(2);
        }
        if (datasourceRoot.has_value() || datasourceManifest.has_value()) {
            updated = ApplyIgnoreDatasourceOverrides(*updated, datasourceRoot, datasourceManifest);
            if (!updated.has_value()) {
                std::cerr << "Error: plan schema invalid: cannot update meta.ignore_datasource root/manifest\n";
                std::exit(2);
            }
        }
        std::string error;
        if (!WriteFileText(planPath, *updated, &error)) {
            std::cerr << "Error: failed to write plan ignore stage: " << planPath.generic_string();
            if (!error.empty()) {
                std::cerr << " (" << error << ")";
            }
            std::cerr << "\n";
            std::exit(2);
        }
        std::size_t totalRules = 0;
        for (const auto& e : entries) {
            totalRules += e.rules.size();
        }
        std::cout << std::format("Plan ignore-init complete: repos={} rules={} file={}\n",
                                 entries.size(),
                                 totalRules,
                                 planPath.generic_string());
        std::cout << "Next:\n";
        std::cout << "  kog plan verify pre-apply --stage ignore --plan-file \"" << planPath.generic_string() << "\"\n";
        std::cout << "  kog plan apply --stage ignore --plan-file \"" << planPath.generic_string() << "\"\n";
    });

    auto* datasourceSync = cmd->add_subcommand("datasource-sync", "Update ignore-plan reference datasource (upstream templates)");
    auto* datasourceSyncSource = new std::string{"github-gitignore"};
    auto* datasourceSyncDryRun = new bool{false};
    datasourceSync->add_option("--source", *datasourceSyncSource, "Datasource source id")->default_str("github-gitignore");
    datasourceSync->add_flag("--dry-run", *datasourceSyncDryRun, "Print revision metadata without updating");
    datasourceSync->callback([=]() {
        const auto workspaceRoot = std::filesystem::current_path().lexically_normal();
        const auto source = ToLower(Trim(*datasourceSyncSource));
        if (source != "github-gitignore") {
            std::cerr << "Error: unsupported --source value: " << *datasourceSyncSource << " (expected github-gitignore)\n";
            std::exit(2);
        }

        const auto skillRoot = ResolveSkillRoot(workspaceRoot);
        if (!std::filesystem::exists(skillRoot / ".git")) {
            std::cerr << "Error: skill repo not found: " << skillRoot.generic_string() << "\n";
            std::exit(2);
        }

        const auto dsRoot = ResolveIgnoreDatasourceRoot(workspaceRoot);
        const auto dsManifest = (dsRoot / "local" / "datasource.manifest.json").lexically_normal();
        const auto submoduleRel = std::string("assets/ignore-sources/upstream/github-gitignore");
        const auto submodulePath = (skillRoot / submoduleRel).lexically_normal();
        if (!std::filesystem::exists(submodulePath)) {
            std::cerr << "Error: ignore datasource path not found: " << submodulePath.generic_string() << "\n";
            std::cerr << "Hint: run `git -C \"" << skillRoot.generic_string() << "\" submodule update --init -- " << submoduleRel
                      << "` first.\n";
            std::exit(2);
        }

        const auto branchResult =
            GitCapture(skillRoot, {"config", "-f", ".gitmodules", "--get", "submodule." + submoduleRel + ".branch"});
        const auto trackingBranch = branchResult.exitCode == 0 ? Trim(branchResult.stdoutStr) : std::string{};
        const auto gitlinkBefore = GitSubmoduleGitlinkShaAtHead(skillRoot, submoduleRel);
        const auto headBefore = GitHeadSha(submodulePath);

        if (!*datasourceSyncDryRun) {
            const auto syncResult = GitPassThrough(skillRoot, {"submodule", "sync", "--", submoduleRel});
            if (syncResult.exitCode != 0) {
                std::cerr << "Error: submodule sync failed for " << submoduleRel << "\n";
                std::exit(syncResult.exitCode);
            }
            const auto updateResult = GitPassThrough(skillRoot, {"submodule", "update", "--init", "--remote", "--", submoduleRel});
            if (updateResult.exitCode != 0) {
                std::cerr << "Error: submodule update failed for " << submoduleRel << "\n";
                std::exit(updateResult.exitCode);
            }
        }

        const auto gitlinkAfter = GitSubmoduleGitlinkShaAtHead(skillRoot, submoduleRel);
        const auto headAfter = GitHeadSha(submodulePath);
        const bool changed = headBefore.value_or("") != headAfter.value_or("");
        std::cout << std::format("Datasource sync source={} dry_run={} changed={}\n",
                                 source,
                                 *datasourceSyncDryRun ? "true" : "false",
                                 changed ? "true" : "false");
        std::cout << std::format("skill_root={}\n", skillRoot.generic_string());
        std::cout << std::format("datasource_root={}\n", dsRoot.generic_string());
        std::cout << std::format("datasource_manifest={}\n", dsManifest.generic_string());
        std::cout << std::format("submodule_path={}\n", submodulePath.generic_string());
        std::cout << std::format("tracking_branch={}\n", trackingBranch.empty() ? "-" : trackingBranch);
        std::cout << std::format("gitlink_before={}\n", gitlinkBefore.value_or("-"));
        std::cout << std::format("gitlink_after={}\n", gitlinkAfter.value_or("-"));
        std::cout << std::format("submodule_head_before={}\n", headBefore.value_or("-"));
        std::cout << std::format("submodule_head_after={}\n", headAfter.value_or("-"));

        std::string manifestError;
        const auto sources = ParseIgnoreDatasourceManifest(dsManifest, &manifestError);
        if (sources.empty() && !manifestError.empty()) {
            std::cerr << "Error: ignore datasource manifest invalid: " << dsManifest.generic_string() << " (" << manifestError << ")\n";
            std::exit(2);
        }
        std::cout << std::format("datasource_sources={}\n", sources.size());
        for (const auto& source : sources) {
            const auto exists = std::filesystem::exists(source.resolvedPath);
            std::cout << std::format("  - id={} kind={} enabled={} path_raw={} path_resolved={} exists={}\n",
                                     source.id,
                                     source.kind.empty() ? "-" : source.kind,
                                     source.enabled ? "true" : "false",
                                     source.pathRaw,
                                     source.resolvedPath.generic_string(),
                                     exists ? "true" : "false");
        }
        if (!*datasourceSyncDryRun) {
            std::cout << "Next:\n";
            std::cout << "  review submodule pointer diff in skill repo, then commit the pin update\n";
        }
    });

    auto* dirtyScope = cmd->add_subcommand("prepare-scope", "Count dirty repos and total changed entries");
    auto* dirtyScopeRoot = new std::string{};
    dirtyScope->add_option("--workspace-root", *dirtyScopeRoot, "Workspace root path (default: cwd)");
    dirtyScope->callback([=]() {
        const auto workspaceRoot =
            dirtyScopeRoot->empty() ? std::filesystem::current_path().lexically_normal() : std::filesystem::path(*dirtyScopeRoot).lexically_normal();
        const auto repos = DiscoverWorkspaceRepos(workspaceRoot);
        std::size_t dirtyRepos = 0;
        std::size_t totalChanges = 0;
        for (const auto& repo : repos) {
            const auto status = GitCapture(repo, {"status", "--porcelain", "--untracked-files=all"});
            if (status.exitCode != 0 || Trim(status.stdoutStr).empty()) {
                continue;
            }
            std::size_t lineCount = 0;
            std::istringstream iss(status.stdoutStr);
            std::string line;
            while (std::getline(iss, line)) {
                if (!Trim(line).empty()) {
                    lineCount += 1;
                }
            }
            if (lineCount > 0) {
                dirtyRepos += 1;
                totalChanges += lineCount;
            }
        }
        std::cout << std::format("{} {}\n", dirtyRepos, totalChanges);
    });

    auto* checkIgnoreGate = cmd->add_subcommand("ignore-gate", "Check unresolved artifact-like untracked files");
    checkIgnoreGate->group("");
    auto* ignoreGateRoot = new std::string{};
    auto* ignoreGateContext = new std::string{"plan"};
    auto* ignoreGateAllowlist = new std::string{};
    auto* ignoreGateLimit = new int{20};
    checkIgnoreGate->add_option("--workspace-root", *ignoreGateRoot, "Workspace root path (default: cwd)");
    checkIgnoreGate->add_option("--context", *ignoreGateContext, "Context label (plan|ai-commit)")->default_str("plan");
    checkIgnoreGate->add_option("--allowlist", *ignoreGateAllowlist, "Allowlist file path");
    checkIgnoreGate->add_option("--limit", *ignoreGateLimit, "Max listed candidates")->default_val(20);
    checkIgnoreGate->callback([=]() {
        const auto allow = std::string(std::getenv("KOG_ALLOW_IGNORE_GATE") != nullptr ? std::getenv("KOG_ALLOW_IGNORE_GATE") : "");
        if (ToLower(Trim(allow)) == "1" || ToLower(Trim(allow)) == "true") {
            std::exit(0);
        }
        const auto gate = std::string(std::getenv("KOG_IGNORE_GATE") != nullptr ? std::getenv("KOG_IGNORE_GATE") : "");
        if (ToLower(Trim(gate)) == "off") {
            std::exit(0);
        }

        const auto workspaceRoot =
            ignoreGateRoot->empty() ? std::filesystem::current_path().lexically_normal() : std::filesystem::path(*ignoreGateRoot).lexically_normal();
        if (GitCapture(workspaceRoot, {"rev-parse", "--git-dir"}).exitCode != 0) {
            std::exit(0);
        }

        const auto allowlistPath = ignoreGateAllowlist->empty()
                                       ? (workspaceRoot / ".agents" / "skills" / "kano" / "kano-git-master-skill" / "assets" /
                                          "ignore-sources" / "local" / "ignore-gate-allowlist.txt")
                                             .lexically_normal()
                                       : std::filesystem::path(*ignoreGateAllowlist).lexically_normal();
        const auto allowlist = ReadIgnoreGateAllowlist(allowlistPath);
        std::vector<std::string> candidates;
        for (const auto& repo : DiscoverWorkspaceRepos(workspaceRoot)) {
            const auto rel = RelativeDisplayPath(workspaceRoot, repo);
            const auto untracked = GitCapture(repo, {"ls-files", "--others", "--exclude-standard"});
            if (untracked.exitCode != 0 || Trim(untracked.stdoutStr).empty()) {
                continue;
            }
            std::istringstream iss(untracked.stdoutStr);
            std::string path;
            while (std::getline(iss, path)) {
                auto raw = Trim(path);
                if (raw.empty()) {
                    continue;
                }
                if (!IsProbableIgnoreArtifactPath(raw)) {
                    continue;
                }
                if (IsInternalPipelineArtifactPath(raw)) {
                    continue;
                }
                const auto norm = NormalizePathSlashesLower(raw);
                if (allowlist.contains(norm)) {
                    continue;
                }
                if (rel == "." || rel.empty()) {
                    candidates.push_back(raw);
                } else {
                    candidates.push_back(std::format("{}/{}", rel, raw));
                }
            }
        }

        if (candidates.empty()) {
            std::exit(0);
        }

        const auto context = Trim(*ignoreGateContext);
        std::cerr << "Error: ignore gate failed (" << context << "); unresolved untracked artifact-like files detected.\n";
        const int limit = *ignoreGateLimit > 0 ? *ignoreGateLimit : 20;
        for (int i = 0; i < limit && i < static_cast<int>(candidates.size()); ++i) {
            std::cerr << "  - " << candidates[static_cast<std::size_t>(i)] << "\n";
        }
        if (static_cast<int>(candidates.size()) > limit) {
            std::cerr << "  ... and " << (static_cast<int>(candidates.size()) - limit) << " more\n";
        }
        if (context == "ai-commit") {
            std::cerr << "Reason: current plan-driven AI commit run is blocked by ignore gate to prevent accidental artifact commits.\n";
            std::cerr << "Action: either add/remove those files now, or bypass once on the same command with --allow-ignore-gate.\n";
        } else {
            std::cerr << "Reason: ignore gate requires artifact-like untracked files to be handled before proceeding.\n";
            std::cerr << "Action: add/remove those files, or bypass once with --allow-ignore-gate.\n";
        }
        std::exit(3);
    });

    auto* checkSecretGate = cmd->add_subcommand("secret-gate", "Check changed files for secret/token patterns");
    checkSecretGate->group("");
    auto* secretGateRoot = new std::string{};
    auto* secretGateContext = new std::string{"plan"};
    auto* secretGateRules = new std::string{};
    auto* secretGateLimit = new int{20};
    checkSecretGate->add_option("--workspace-root", *secretGateRoot, "Workspace root path (default: cwd)");
    checkSecretGate->add_option("--context", *secretGateContext, "Context label (plan|ai-commit|commit-push)")->default_str("plan");
    checkSecretGate->add_option("--rules-file", *secretGateRules, "Rule file path (format: id|regex)");
    checkSecretGate->add_option("--limit", *secretGateLimit, "Max listed findings")->default_val(20);
    checkSecretGate->callback([=]() {
        const auto disable =
            std::string(std::getenv("KOG_DISABLE_SECRET_GATE") != nullptr ? std::getenv("KOG_DISABLE_SECRET_GATE") : "");
        if (ToLower(Trim(disable)) == "1" || ToLower(Trim(disable)) == "true") {
            std::exit(0);
        }
        if (*secretGateLimit <= 0) {
            std::cerr << "Error: --limit must be positive\n";
            std::exit(2);
        }
        const auto workspaceRoot =
            secretGateRoot->empty() ? std::filesystem::current_path().lexically_normal() : std::filesystem::path(*secretGateRoot).lexically_normal();
        const auto rulesPath =
            secretGateRules->empty() ? DefaultSecretRulesPath(workspaceRoot) : ResolvePath(workspaceRoot, *secretGateRules);
        std::string rulesError;
        const auto rules = LoadSecretRules(rulesPath, &rulesError);
        if (!rulesError.empty()) {
            std::cerr << "Error: secret gate rules invalid: " << rulesError << "\n";
            std::exit(2);
        }
        if (rules.empty()) {
            std::cout << "Secret gate passed: no rules loaded\n";
            std::exit(0);
        }

        const auto repos = DiscoverWorkspaceRepos(workspaceRoot);
        std::vector<SecretFinding> findings;
        findings.reserve(static_cast<std::size_t>(*secretGateLimit));
        for (const auto& repo : repos) {
            const auto changedFiles = CollectChangedCandidateFiles(repo);
            if (changedFiles.empty()) {
                continue;
            }
            const auto repoRel = RelativeDisplayPath(workspaceRoot, repo);
            for (const auto& file : changedFiles) {
                if (static_cast<int>(findings.size()) >= *secretGateLimit) {
                    break;
                }
                const auto before = findings.size();
                ScanFileForSecretRules(repo, file, rules, *secretGateLimit, &findings);
                for (std::size_t i = before; i < findings.size(); ++i) {
                    findings[i].repo = repoRel.empty() ? "." : repoRel;
                }
            }
            if (static_cast<int>(findings.size()) >= *secretGateLimit) {
                break;
            }
        }

        if (findings.empty()) {
            std::cout << "Secret gate passed: no high-confidence findings in changed files\n";
            std::exit(0);
        }

        const auto context = Trim(*secretGateContext);
        std::cerr << "Error: secret gate failed (" << context << "); potential secrets detected.\n";
        for (const auto& f : findings) {
            std::cerr << std::format("  - [{}/{}:{}] rule={} preview={}\n",
                                     f.repo.empty() ? "." : f.repo,
                                     f.file,
                                     f.line,
                                     f.ruleId,
                                     f.preview);
        }
        std::cerr << "Hint: remove/redact secrets, rotate leaked credentials if needed, then rerun.\n";
        std::cerr << "Hint: disable once with KOG_DISABLE_SECRET_GATE=1 (not recommended).\n";
        std::exit(3);
    });

    auto* verify = cmd->add_subcommand("verify", "Plan verification stages");

    auto* verifyPreApply = verify->add_subcommand("pre-apply", "Verify plan schema before apply");
    auto* verifyPreFile = new std::string{};
    auto* verifyPreStage = new std::string{"all"};
    verifyPreApply->add_option("--plan-file", *verifyPreFile, "Plan file path");
    verifyPreApply->add_option("--stage", *verifyPreStage, "Stage: ignore|commit|all")->default_str("all");
    verifyPreApply->callback([=]() {
        const auto workspaceRoot = std::filesystem::current_path().lexically_normal();
        const auto planPath = verifyPreFile->empty() ? DefaultPlanPath(workspaceRoot) : std::filesystem::path(*verifyPreFile).lexically_normal();
        const auto payload = ReadFileText(planPath);
        if (!payload.has_value()) {
            std::cerr << "Error: plan file not found/readable: " << planPath.generic_string() << "\n";
            std::cerr << "Hint: create one with `kog plan new --plan-file \"" << planPath.generic_string() << "\"`.\n";
            std::exit(2);
        }
        const auto text = *payload;
        const auto stages = ExtractObjectBodyForKey(text, "stages");
        if (!stages.has_value() || !ExtractObjectBodyForKey(text, "meta").has_value()) {
            std::cerr << "Error: plan schema invalid: missing meta/stages\n";
            std::cerr << "Hint: regenerate template with `kog plan new --force --plan-file \"" << planPath.generic_string() << "\"`.\n";
            std::exit(2);
        }
        const auto stage = ToLower(Trim(*verifyPreStage));
        if (stage != "ignore" && stage != "commit" && stage != "all") {
            std::cerr << "Error: invalid --stage value: " << *verifyPreStage << " (expected ignore|commit|all)\n";
            std::exit(2);
        }
        if (stage == "ignore" || stage == "all") {
            if (!ExtractArrayBodyForKey(*stages, "ignore").has_value()) {
                std::cerr << "Error: plan schema invalid: stages.ignore missing\n";
                std::cerr << "Hint: run `kog plan ignore-init --force --plan-file \"" << planPath.generic_string() << "\"`.\n";
                std::exit(2);
            }
        }
        if (stage == "commit" || stage == "all") {
            if (!ExtractArrayBodyForKey(*stages, "commit").has_value() || !ExtractArrayBodyForKey(*stages, "post_sync").has_value()) {
                std::cerr << "Error: plan schema invalid: stages.commit/post_sync missing\n";
                std::cerr << "Hint: regenerate template with `kog plan new --force --plan-file \"" << planPath.generic_string() << "\"`.\n";
                std::exit(2);
            }
        }
        std::string planBaseHeadSha;
        std::string planDirtyFingerprint;
        if (!ExtractPlanWorkspaceHashes(text, &planBaseHeadSha, &planDirtyFingerprint)) {
            std::cerr << "Error: plan schema invalid: meta.base_head_sha/meta.dirty_fingerprint missing or placeholder\n";
            std::cerr << "Hint: regenerate and refill with `kog plan runbook commit --force --plan-file \"" << planPath.generic_string() << "\"`.\n";
            std::exit(2);
        }
        const auto currentBaseHeadSha = ComputeWorkspaceBaseHeadSha(workspaceRoot);
        const auto currentDirtyFingerprint = ComputeWorkspaceDirtyFingerprint(workspaceRoot);
        if (planBaseHeadSha != currentBaseHeadSha || planDirtyFingerprint != currentDirtyFingerprint) {
            std::cerr << "Error: plan schema invalid: workspace state drift detected.\n";
            std::cerr << "  plan.base_head_sha=" << planBaseHeadSha << "\n";
            std::cerr << "  current.base_head_sha=" << currentBaseHeadSha << "\n";
            std::cerr << "  plan.dirty_fingerprint=" << planDirtyFingerprint << "\n";
            std::cerr << "  current.dirty_fingerprint=" << currentDirtyFingerprint << "\n";
            std::cerr << "Hint: refresh plan via `kog plan runbook commit --force --plan-file \"" << planPath.generic_string() << "\"`.\n";
            std::exit(2);
        }
        std::cout << "Plan schema-verify passed: " << planPath.generic_string() << "\n";
    });

    auto* verifyPostApply = verify->add_subcommand("post-apply", "Verify result state after apply");
    auto* verifyPostFile = new std::string{};
    auto* verifyPostStage = new std::string{"all"};
    verifyPostApply->add_option("--plan-file", *verifyPostFile, "Plan file path");
    verifyPostApply->add_option("--stage", *verifyPostStage, "Stage: ignore|commit|all")->default_str("all");
    verifyPostApply->callback([=]() {
        const auto workspaceRoot = std::filesystem::current_path().lexically_normal();
        const auto planPath = verifyPostFile->empty() ? DefaultPlanPath(workspaceRoot) : std::filesystem::path(*verifyPostFile).lexically_normal();
        const auto payload = ReadFileText(planPath);
        if (!payload.has_value()) {
            std::cerr << "Error: plan file not found/readable: " << planPath.generic_string() << "\n";
            std::exit(2);
        }
        const auto stage = ToLower(Trim(*verifyPostStage));
        if (stage != "ignore" && stage != "commit" && stage != "all") {
            std::cerr << "Error: invalid --stage value: " << *verifyPostStage << " (expected ignore|commit|all)\n";
            std::exit(2);
        }
        const auto text = *payload;
        if (stage == "ignore" || stage == "all") {
            const std::regex ignoreAppliedPattern(R"("applied_at_utc"\s*:\s*"[0-9]{4}-[0-9]{2}-[0-9]{2}T[^"]+")");
            if (!std::regex_search(text, ignoreAppliedPattern)) {
                std::cerr << "Error: post-apply verify failed: no applied_at_utc found for ignore stage.\n";
                std::cerr << "Hint: run `kog plan apply --stage ignore --plan-file \"" << planPath.generic_string() << "\"` first.\n";
                std::exit(2);
            }
        }
        if (stage == "commit" || stage == "all") {
            const std::regex commitExecutedPattern(R"("executed_at_utc"\s*:\s*"[0-9]{4}-[0-9]{2}-[0-9]{2}T[^"]+")");
            if (!std::regex_search(text, commitExecutedPattern)) {
                std::cerr << "Error: post-apply verify failed: meta.executed_at_utc is empty.\n";
                std::cerr << "Hint: run commit/commit-push apply path first so execution stamp is written.\n";
                std::exit(2);
            }
        }
        std::cout << "Plan result-verify passed: " << planPath.generic_string() << "\n";
    });

    auto* verifyIgnore = verify->add_subcommand("ignore", "Run ignore gate verification");
    auto* verifyIgnoreRoot = new std::string{};
    auto* verifyIgnoreContext = new std::string{"plan"};
    auto* verifyIgnoreAllowlist = new std::string{};
    auto* verifyIgnoreLimit = new int{20};
    verifyIgnore->add_option("--workspace-root", *verifyIgnoreRoot, "Workspace root path (default: cwd)");
    verifyIgnore->add_option("--context", *verifyIgnoreContext, "Context label (plan|ai-commit)")->default_str("plan");
    verifyIgnore->add_option("--allowlist", *verifyIgnoreAllowlist, "Allowlist file path");
    verifyIgnore->add_option("--limit", *verifyIgnoreLimit, "Max listed candidates")->default_val(20);
    verifyIgnore->callback([=]() {
        const auto allow = std::string(std::getenv("KOG_ALLOW_IGNORE_GATE") != nullptr ? std::getenv("KOG_ALLOW_IGNORE_GATE") : "");
        if (ToLower(Trim(allow)) == "1" || ToLower(Trim(allow)) == "true") {
            std::exit(0);
        }
        const auto gate = std::string(std::getenv("KOG_IGNORE_GATE") != nullptr ? std::getenv("KOG_IGNORE_GATE") : "");
        if (ToLower(Trim(gate)) == "off") {
            std::exit(0);
        }
        const auto workspaceRoot =
            verifyIgnoreRoot->empty() ? std::filesystem::current_path().lexically_normal() : std::filesystem::path(*verifyIgnoreRoot).lexically_normal();
        if (GitCapture(workspaceRoot, {"rev-parse", "--git-dir"}).exitCode != 0) {
            std::exit(0);
        }
        const auto allowlistPath = verifyIgnoreAllowlist->empty()
                                       ? (workspaceRoot / ".agents" / "skills" / "kano" / "kano-git-master-skill" / "assets" / "gitignore" /
                                          "ignore-gate-allowlist.txt")
                                             .lexically_normal()
                                       : std::filesystem::path(*verifyIgnoreAllowlist).lexically_normal();
        const auto allowlist = ReadIgnoreGateAllowlist(allowlistPath);
        std::vector<std::string> candidates;
        for (const auto& repo : DiscoverWorkspaceRepos(workspaceRoot)) {
            const auto rel = RelativeDisplayPath(workspaceRoot, repo);
            const auto untracked = GitCapture(repo, {"ls-files", "--others", "--exclude-standard"});
            if (untracked.exitCode != 0 || Trim(untracked.stdoutStr).empty()) {
                continue;
            }
            std::istringstream iss(untracked.stdoutStr);
            std::string path;
            while (std::getline(iss, path)) {
                auto raw = Trim(path);
                if (raw.empty() || !IsProbableIgnoreArtifactPath(raw)) {
                    continue;
                }
                if (IsInternalPipelineArtifactPath(raw)) {
                    continue;
                }
                const auto norm = NormalizePathSlashesLower(raw);
                if (allowlist.contains(norm)) {
                    continue;
                }
                if (rel == "." || rel.empty()) {
                    candidates.push_back(raw);
                } else {
                    candidates.push_back(std::format("{}/{}", rel, raw));
                }
            }
        }
        if (candidates.empty()) {
            std::exit(0);
        }
        const auto context = Trim(*verifyIgnoreContext);
        std::cerr << "Error: ignore gate failed (" << context << "); unresolved untracked artifact-like files detected.\n";
        const int limit = *verifyIgnoreLimit > 0 ? *verifyIgnoreLimit : 20;
        for (int i = 0; i < limit && i < static_cast<int>(candidates.size()); ++i) {
            std::cerr << "  - " << candidates[static_cast<std::size_t>(i)] << "\n";
        }
        if (static_cast<int>(candidates.size()) > limit) {
            std::cerr << "  ... and " << (static_cast<int>(candidates.size()) - limit) << " more\n";
        }
        std::cerr << "Reason: ignore gate requires artifact-like untracked files to be handled before proceeding.\n";
        std::cerr << "Action: add/remove those files, or bypass once with --allow-ignore-gate.\n";
        std::exit(3);
    });

    auto* verifySecret = verify->add_subcommand("secret", "Run secret/token gate verification");
    auto* verifySecretRoot = new std::string{};
    auto* verifySecretContext = new std::string{"plan"};
    auto* verifySecretRules = new std::string{};
    auto* verifySecretLimit = new int{20};
    verifySecret->add_option("--workspace-root", *verifySecretRoot, "Workspace root path (default: cwd)");
    verifySecret->add_option("--context", *verifySecretContext, "Context label (plan|ai-commit|commit-push)")->default_str("plan");
    verifySecret->add_option("--rules-file", *verifySecretRules, "Rule file path (format: id|regex)");
    verifySecret->add_option("--limit", *verifySecretLimit, "Max listed findings")->default_val(20);
    verifySecret->callback([=]() {
        const auto disable =
            std::string(std::getenv("KOG_DISABLE_SECRET_GATE") != nullptr ? std::getenv("KOG_DISABLE_SECRET_GATE") : "");
        if (ToLower(Trim(disable)) == "1" || ToLower(Trim(disable)) == "true") {
            std::exit(0);
        }
        if (*verifySecretLimit <= 0) {
            std::cerr << "Error: --limit must be positive\n";
            std::exit(2);
        }
        const auto workspaceRoot =
            verifySecretRoot->empty() ? std::filesystem::current_path().lexically_normal() : std::filesystem::path(*verifySecretRoot).lexically_normal();
        const auto rulesPath =
            verifySecretRules->empty() ? DefaultSecretRulesPath(workspaceRoot) : ResolvePath(workspaceRoot, *verifySecretRules);
        std::string rulesError;
        const auto rules = LoadSecretRules(rulesPath, &rulesError);
        if (!rulesError.empty()) {
            std::cerr << "Error: secret gate rules invalid: " << rulesError << "\n";
            std::exit(2);
        }
        if (rules.empty()) {
            std::cout << "Secret gate passed: no rules loaded\n";
            std::exit(0);
        }
        const auto repos = DiscoverWorkspaceRepos(workspaceRoot);
        std::vector<SecretFinding> findings;
        findings.reserve(static_cast<std::size_t>(*verifySecretLimit));
        for (const auto& repo : repos) {
            const auto changedFiles = CollectChangedCandidateFiles(repo);
            if (changedFiles.empty()) {
                continue;
            }
            const auto repoRel = RelativeDisplayPath(workspaceRoot, repo);
            for (const auto& file : changedFiles) {
                if (static_cast<int>(findings.size()) >= *verifySecretLimit) {
                    break;
                }
                const auto before = findings.size();
                ScanFileForSecretRules(repo, file, rules, *verifySecretLimit, &findings);
                for (std::size_t i = before; i < findings.size(); ++i) {
                    findings[i].repo = repoRel.empty() ? "." : repoRel;
                }
            }
            if (static_cast<int>(findings.size()) >= *verifySecretLimit) {
                break;
            }
        }
        if (findings.empty()) {
            std::cout << "Secret gate passed: no high-confidence findings in changed files\n";
            std::exit(0);
        }
        const auto context = Trim(*verifySecretContext);
        std::cerr << "Error: secret gate failed (" << context << "); potential secrets detected.\n";
        for (const auto& f : findings) {
            std::cerr << std::format("  - [{}/{}:{}] rule={} preview={}\n",
                                     f.repo.empty() ? "." : f.repo,
                                     f.file,
                                     f.line,
                                     f.ruleId,
                                     f.preview);
        }
        std::cerr << "Hint: remove/redact secrets, rotate leaked credentials if needed, then rerun.\n";
        std::cerr << "Hint: disable once with KOG_DISABLE_SECRET_GATE=1 (not recommended).\n";
        std::exit(3);
    });

    auto* schemaVerify = cmd->add_subcommand("schema-verify", "Verify plan schema (pre-apply)");
    schemaVerify->group("");
    auto* verifyFile = new std::string{};
    auto* verifyStage = new std::string{"all"};
    schemaVerify->add_option("--plan-file", *verifyFile, "Plan file path");
    schemaVerify->add_option("--stage", *verifyStage, "Stage: ignore|commit|all")->default_str("all");
    schemaVerify->callback([=]() {
        const auto workspaceRoot = std::filesystem::current_path().lexically_normal();
        const auto planPath = verifyFile->empty() ? DefaultPlanPath(workspaceRoot) : std::filesystem::path(*verifyFile).lexically_normal();
        const auto payload = ReadFileText(planPath);
        if (!payload.has_value()) {
            std::cerr << "Error: plan file not found/readable: " << planPath.generic_string() << "\n";
            std::cerr << "Hint: create one with `kog plan new --plan-file \"" << planPath.generic_string() << "\"`.\n";
            std::exit(2);
        }
        const auto text = *payload;
        const auto stages = ExtractObjectBodyForKey(text, "stages");
        if (!stages.has_value() || !ExtractObjectBodyForKey(text, "meta").has_value()) {
            std::cerr << "Error: plan schema invalid: missing meta/stages\n";
            std::cerr << "Hint: regenerate template with `kog plan new --force --plan-file \"" << planPath.generic_string() << "\"`.\n";
            std::exit(2);
        }
        const auto stage = ToLower(Trim(*verifyStage));
        if (stage != "ignore" && stage != "commit" && stage != "all") {
            std::cerr << "Error: invalid --stage value: " << *verifyStage << " (expected ignore|commit|all)\n";
            std::exit(2);
        }
        if (stage == "ignore" || stage == "all") {
            if (!ExtractArrayBodyForKey(*stages, "ignore").has_value()) {
                std::cerr << "Error: plan schema invalid: stages.ignore missing\n";
                std::cerr << "Hint: run `kog plan ignore-init --force --plan-file \"" << planPath.generic_string() << "\"`.\n";
                std::exit(2);
            }
        }
        if (stage == "commit" || stage == "all") {
            if (!ExtractArrayBodyForKey(*stages, "commit").has_value() || !ExtractArrayBodyForKey(*stages, "post_sync").has_value()) {
                std::cerr << "Error: plan schema invalid: stages.commit/post_sync missing\n";
                std::cerr << "Hint: regenerate template with `kog plan new --force --plan-file \"" << planPath.generic_string() << "\"`.\n";
                std::exit(2);
            }
        }
        std::string planBaseHeadSha;
        std::string planDirtyFingerprint;
        if (!ExtractPlanWorkspaceHashes(text, &planBaseHeadSha, &planDirtyFingerprint)) {
            std::cerr << "Error: plan schema invalid: meta.base_head_sha/meta.dirty_fingerprint missing or placeholder\n";
            std::cerr << "Hint: regenerate and refill with `kog plan runbook commit --force --plan-file \"" << planPath.generic_string() << "\"`.\n";
            std::exit(2);
        }
        const auto currentBaseHeadSha = ComputeWorkspaceBaseHeadSha(workspaceRoot);
        const auto currentDirtyFingerprint = ComputeWorkspaceDirtyFingerprint(workspaceRoot);
        if (planBaseHeadSha != currentBaseHeadSha || planDirtyFingerprint != currentDirtyFingerprint) {
            std::cerr << "Error: plan schema invalid: workspace state drift detected.\n";
            std::cerr << "  plan.base_head_sha=" << planBaseHeadSha << "\n";
            std::cerr << "  current.base_head_sha=" << currentBaseHeadSha << "\n";
            std::cerr << "  plan.dirty_fingerprint=" << planDirtyFingerprint << "\n";
            std::cerr << "  current.dirty_fingerprint=" << currentDirtyFingerprint << "\n";
            std::cerr << "Hint: refresh plan via `kog plan runbook commit --force --plan-file \"" << planPath.generic_string() << "\"`.\n";
            std::exit(2);
        }
        std::cout << "Plan schema-verify passed: " << planPath.generic_string() << "\n";
        if (stage == "ignore") {
            std::cout << "Next:\n";
            std::cout << "  kog plan apply --stage ignore --plan-file \"" << planPath.generic_string() << "\"\n";
        } else if (stage == "commit") {
            std::cout << "Next:\n";
            std::cout << "  kog plan apply --stage commit --plan-file \"" << planPath.generic_string() << "\"\n";
        } else {
            std::cout << "Next:\n";
            std::cout << "  kog plan apply --stage all --plan-file \"" << planPath.generic_string() << "\"\n";
        }
    });

    auto* resultVerify = cmd->add_subcommand("result-verify", "Verify apply result state (post-apply)");
    resultVerify->group("");
    auto* resultVerifyFile = new std::string{};
    auto* resultVerifyStage = new std::string{"all"};
    resultVerify->add_option("--plan-file", *resultVerifyFile, "Plan file path");
    resultVerify->add_option("--stage", *resultVerifyStage, "Stage: ignore|commit|all")->default_str("all");
    resultVerify->callback([=]() {
        const auto workspaceRoot = std::filesystem::current_path().lexically_normal();
        const auto planPath = resultVerifyFile->empty() ? DefaultPlanPath(workspaceRoot) : std::filesystem::path(*resultVerifyFile).lexically_normal();
        const auto payload = ReadFileText(planPath);
        if (!payload.has_value()) {
            std::cerr << "Error: plan file not found/readable: " << planPath.generic_string() << "\n";
            std::exit(2);
        }

        const auto stage = ToLower(Trim(*resultVerifyStage));
        if (stage != "ignore" && stage != "commit" && stage != "all") {
            std::cerr << "Error: invalid --stage value: " << *resultVerifyStage << " (expected ignore|commit|all)\n";
            std::exit(2);
        }

        const auto text = *payload;
        if (stage == "ignore" || stage == "all") {
            const std::regex ignoreAppliedPattern(R"("applied_at_utc"\s*:\s*"[0-9]{4}-[0-9]{2}-[0-9]{2}T[^"]+")");
            if (!std::regex_search(text, ignoreAppliedPattern)) {
                std::cerr << "Error: post-apply verify failed: no applied_at_utc found for ignore stage.\n";
                std::cerr << "Hint: run `kog plan apply --stage ignore --plan-file \"" << planPath.generic_string() << "\"` first.\n";
                std::exit(2);
            }
        }
        if (stage == "commit" || stage == "all") {
            const std::regex commitExecutedPattern(R"("executed_at_utc"\s*:\s*"[0-9]{4}-[0-9]{2}-[0-9]{2}T[^"]+")");
            if (!std::regex_search(text, commitExecutedPattern)) {
                std::cerr << "Error: post-apply verify failed: meta.executed_at_utc is empty.\n";
                std::cerr << "Hint: run commit/commit-push apply path first so execution stamp is written.\n";
                std::exit(2);
            }
        }

        std::cout << "Plan result-verify passed: " << planPath.generic_string() << "\n";
    });

    auto* apply = cmd->add_subcommand("apply", "Apply plan stages");
    apply->allow_extras();
    auto* applyFile = new std::string{};
    auto* applyStage = new std::string{"all"};
    apply->add_option("--plan-file", *applyFile, "Plan file path");
    apply->add_option("--stage", *applyStage, "Stage: ignore|commit|all")->default_str("all");
    apply->callback([=]() {
        const auto workspaceRoot = std::filesystem::current_path().lexically_normal();
        const auto planPath = applyFile->empty() ? DefaultPlanPath(workspaceRoot) : std::filesystem::path(*applyFile).lexically_normal();
        auto payload = ReadFileText(planPath);
        if (!payload.has_value()) {
            std::cerr << "Error: plan file not found/readable: " << planPath.generic_string() << "\n";
            std::cerr << "Hint: create one with `kog plan new --plan-file \"" << planPath.generic_string() << "\"`.\n";
            std::exit(2);
        }
        const auto stage = ToLower(Trim(*applyStage));
        if (stage != "ignore" && stage != "commit" && stage != "all") {
            std::cerr << "Error: invalid --stage value: " << *applyStage << " (expected ignore|commit|all)\n";
            std::exit(2);
        }
        const auto runPostApplyVerify = [&](const std::string& stageValue) -> int {
            const auto verifyPayload = ReadFileText(planPath);
            if (!verifyPayload.has_value()) {
                std::cerr << "Error: plan file not found/readable: " << planPath.generic_string() << "\n";
                return 2;
            }
            const auto text = *verifyPayload;
            if (stageValue == "ignore" || stageValue == "all") {
                const std::regex ignoreAppliedPattern(R"("applied_at_utc"\s*:\s*"[0-9]{4}-[0-9]{2}-[0-9]{2}T[^"]+")");
                if (!std::regex_search(text, ignoreAppliedPattern)) {
                    std::cerr << "Error: post-apply verify failed: no applied_at_utc found for ignore stage.\n";
                    std::cerr << "Hint: run `kog plan apply --stage ignore --plan-file \"" << planPath.generic_string() << "\"` first.\n";
                    return 2;
                }
            }
            if (stageValue == "commit" || stageValue == "all") {
                const std::regex commitExecutedPattern(R"("executed_at_utc"\s*:\s*"[0-9]{4}-[0-9]{2}-[0-9]{2}T[^"]+")");
                if (!std::regex_search(text, commitExecutedPattern)) {
                    std::cerr << "Error: post-apply verify failed: meta.executed_at_utc is empty.\n";
                    std::cerr << "Hint: run commit/commit-push apply path first so execution stamp is written.\n";
                    return 2;
                }
            }
            return 0;
        };

        if (stage == "ignore" || stage == "all") {
            const auto entries = ParseIgnoreEntries(*payload);
            if (entries.empty()) {
                std::cerr << "Error: no ignore plan entries found in stages.ignore.\n";
                std::cerr << "Hint: run `kog plan ignore-init --plan-file \"" << planPath.generic_string() << "\"` first.\n";
                if (stage == "ignore") {
                    std::exit(2);
                }
            }
            for (std::size_t idx = 0; idx < entries.size(); ++idx) {
                const auto& e = entries[idx];
                const auto repoAbs = ResolvePath(workspaceRoot, e.repo);
                const auto targetAbs = ResolvePath(repoAbs, e.applyTarget);
                auto mergedAbs = e.mergedOutputPath.empty()
                    ? (workspaceRoot / ".kano" / "tmp" / "git" / "plans" / std::format("ignore-merged-{}.gitignore", idx)).lexically_normal()
                    : ResolvePath(workspaceRoot, e.mergedOutputPath);
                const auto mergedText = MergeGitignore(targetAbs, e.rules);
                std::string error;
                if (!WriteFileText(mergedAbs, mergedText, &error)) {
                    std::cerr << "Error: failed to write merged ignore: " << mergedAbs.generic_string() << " (" << error << ")\n";
                    std::exit(2);
                }
                if (!WriteFileText(targetAbs, mergedText, &error)) {
                    std::cerr << "Error: failed to apply ignore target: " << targetAbs.generic_string() << " (" << error << ")\n";
                    std::exit(2);
                }
                std::cout << "[plan][ignore] applied: repo=" << e.repo << " target=" << e.applyTarget
                          << " merged=" << mergedAbs.generic_string() << "\n";
            }
            *payload = StampIgnoreAppliedAtAll(*payload, CurrentUtcIso8601());
            const auto postIgnoreDirtyFingerprint = ComputeWorkspaceDirtyFingerprint(workspaceRoot);
            if (const auto updated = ReplacePlanDirtyFingerprint(*payload, postIgnoreDirtyFingerprint); updated.has_value()) {
                *payload = *updated;
            } else {
                std::cerr << "Error: failed to update meta.dirty_fingerprint after ignore apply.\n";
                std::exit(2);
            }
            std::string error;
            if (!WriteFileText(planPath, *payload, &error)) {
                std::cerr << "Error: failed to stamp plan applied_at_utc: " << planPath.generic_string() << " (" << error << ")\n";
                std::exit(2);
            }
            std::cout << "[plan][ignore] apply complete\n";
            std::cout << "Next:\n";
            std::cout << "  kog plan verify pre-apply --stage ignore --plan-file \"" << planPath.generic_string() << "\"\n";
            std::cout << "  kog plan verify ignore --context plan\n";
            if (stage == "ignore") {
                const auto verifyStatus = runPostApplyVerify("ignore");
                if (verifyStatus != 0) {
                    std::exit(verifyStatus);
                }
                std::exit(0);
            }
        }

        // Run commit-push plan pipeline in-process.
        const auto extras = apply->remaining();
        const auto commitPushCode = RunCommitPushPlanFilePipeline(workspaceRoot, planPath.generic_string(), extras);
        if (commitPushCode != 0) {
            std::exit(commitPushCode);
        }
        const auto verifyStatus = runPostApplyVerify(stage == "all" ? "all" : "commit");
        std::exit(verifyStatus);
    });
}

void RegisterIgnore(CLI::App& InApp) {
    auto* cmd = InApp.add_subcommand("ignore", "Ignore management commands");
    auto* doctor = cmd->add_subcommand("doctor", "Scan tracked files for likely ignore candidates");
    auto* repo = new std::string{};
    auto* limit = new int{200};
    auto* asJson = new bool{false};
    auto* apply = new bool{false};
    auto* dryRun = new bool{false};
    auto* yes = new bool{false};
    doctor->add_option("--repo", *repo, "Workspace/repo root path");
    doctor->add_option("--limit", *limit, "Max findings")->default_val(200);
    doctor->add_flag("--json", *asJson, "Output JSON");
    doctor->add_flag("--apply", *apply, "Apply untrack (git rm --cached) to findings");
    doctor->add_flag("--dry-run", *dryRun, "Print apply actions only");
    doctor->add_flag("--yes,-y", *yes, "Skip interactive confirmation gate");
    doctor->callback([=]() {
        const auto root = repo->empty() ? std::filesystem::current_path().lexically_normal() : std::filesystem::path(*repo).lexically_normal();
        if (GitCapture(root, {"rev-parse", "--git-dir"}).exitCode != 0) {
            std::cerr << "Error: not a git repository/workspace root: " << root.generic_string() << "\n";
            std::exit(2);
        }
        if (*limit <= 0) {
            std::cerr << "Error: --limit must be a positive integer\n";
            std::exit(2);
        }
        auto repos = DiscoverWorkspaceRepos(root);
        std::vector<IgnoreFinding> findings;
        findings.reserve(static_cast<std::size_t>(*limit));
        for (const auto& repoPath : repos) {
            const auto rel = RelativeDisplayPath(root, repoPath);
            const auto tracked = GitCapture(repoPath, {"ls-files"});
            if (tracked.exitCode != 0) {
                continue;
            }
            std::istringstream iss(tracked.stdoutStr);
            std::string line;
            while (std::getline(iss, line)) {
                auto path = Trim(line);
                if (path.empty()) {
                    continue;
                }
                if (!IsProbableIgnoreArtifactPath(path)) {
                    continue;
                }
                if (IsInternalPipelineArtifactPath(path)) {
                    continue;
                }
                IgnoreFinding f;
                f.repo = repoPath;
                f.repoRel = rel;
                f.repoPath = path;
                f.display = (rel == "." ? path : std::format("{}/{}", rel, path));
                findings.push_back(std::move(f));
                if (static_cast<int>(findings.size()) >= *limit) {
                    break;
                }
            }
            if (static_cast<int>(findings.size()) >= *limit) {
                break;
            }
        }

        if (*asJson) {
            std::cout << "{\n";
            std::cout << "  \"repo_root\": \"" << root.generic_string() << "\",\n";
            std::cout << "  \"count\": " << findings.size() << ",\n";
            std::cout << "  \"findings\": [\n";
            for (std::size_t i = 0; i < findings.size(); ++i) {
                std::cout << "    \"" << findings[i].display << "\"";
                if (i + 1 < findings.size()) {
                    std::cout << ",";
                }
                std::cout << "\n";
            }
            std::cout << "  ]\n";
            std::cout << "}\n";
        } else {
            if (findings.empty()) {
                std::cout << "[ignore-doctor] no tracked artifact-like paths found.\n";
            } else {
                std::cout << "[ignore-doctor] tracked artifact-like paths (review candidates):\n";
                for (const auto& f : findings) {
                    std::cout << "  - " << f.display << "\n";
                }
            }
        }

        if (!*apply) {
            std::exit(0);
        }
        if (!*yes && !*dryRun) {
            std::cerr << "Error: --apply requires --yes (or use --dry-run).\n";
            std::exit(2);
        }

        int removed = 0;
        int failed = 0;
        for (const auto& f : findings) {
            if (*dryRun) {
                std::cout << std::format("[ignore-doctor][dry-run] git -C {} rm --cached -- \"{}\"\n",
                                         f.repo.generic_string(),
                                         f.repoPath);
                continue;
            }
            const auto rm = GitPassThrough(f.repo, {"rm", "--cached", "--", f.repoPath});
            if (rm.exitCode == 0) {
                removed += 1;
                std::cout << "[ignore-doctor][applied] " << f.display << "\n";
            } else {
                failed += 1;
                std::cerr << "[ignore-doctor][failed] " << f.display << "\n";
            }
        }
        if (!*dryRun) {
            std::cout << std::format("[ignore-doctor] apply summary: removed={} failed={}\n", removed, failed);
        }
        std::exit(failed == 0 ? 0 : 2);
    });
}

} // namespace kano::git::commands
