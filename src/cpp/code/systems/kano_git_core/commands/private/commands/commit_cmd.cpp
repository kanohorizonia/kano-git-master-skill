// commit command — Native multi-repo commit workflow (pure C++)

#include "command_registry.hpp"
#include "discovery.hpp"
#include "shell_executor.hpp"
#include "auto_model_policy.hpp"
#include "kog_config.hpp"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <format>
#include <filesystem>
#include <iostream>
#include <iomanip>
#include <optional>
#include <print>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>
#include <future>
#include <unordered_map>
#include <set>
#include <functional>
#include <utility>

namespace kano::git::commands {
namespace {

struct CommitPreflightReport {
    bool inRepo = false;
    std::filesystem::path repoPath;
    int stagedCount = 0;
    int unstagedCount = 0;
    int untrackedCount = 0;
    std::vector<std::string> riskyFiles;
    std::vector<std::string> stagedFiles;
    std::vector<std::string> unstagedFiles;
    std::vector<std::string> untrackedFiles;
};

struct NativeAiConfig {
    bool enabled = false;
    bool reviewEnabled = true;
    std::string provider;
    std::string model;
};

auto DisplayRepoLabel(const std::filesystem::path& InWorkspaceRoot, const std::filesystem::path& InRepo) -> std::string;
auto DiscoverWorkspaceRepos(const std::filesystem::path& InRoot) -> std::vector<std::filesystem::path>;
auto ReadFileText(const std::filesystem::path& InPath) -> std::optional<std::string>;
auto ResolveSkillRoot(const std::filesystem::path& InWorkspaceRoot) -> std::filesystem::path;
auto Fnv1a64Hex(const std::string& InText) -> std::string;

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

auto ToLower(std::string InValue) -> std::string {
    std::transform(InValue.begin(), InValue.end(), InValue.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return InValue;
}

auto NormalizeAiModelKeyword(const std::string& InValue) -> std::string {
    return kog_config::NormalizeAiModelSelection(InValue);
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

auto LooksRiskyPath(const std::string& InPath) -> bool {
    const std::string lower = [&]() {
        std::string out = InPath;
        for (auto& c : out) {
            if (c >= 'A' && c <= 'Z') {
                c = static_cast<char>(c - 'A' + 'a');
            }
        }
        return out;
    }();

    return lower.find(".env") != std::string::npos ||
           lower.find("credentials") != std::string::npos ||
           lower.find("secret") != std::string::npos ||
           lower.find("id_rsa") != std::string::npos ||
           lower.ends_with(".pem") ||
           lower.ends_with(".key");
}

auto GitCapture(const std::filesystem::path& InRepo, const std::vector<std::string>& InArgs) -> shell::ExecResult {
    return shell::ExecuteCommand("git", InArgs, shell::ExecMode::Capture, InRepo);
}

auto IsGitRepo(const std::filesystem::path& InRepo) -> bool {
    return GitCapture(InRepo, {"rev-parse", "--git-dir"}).exitCode == 0;
}

auto GitPassThrough(const std::filesystem::path& InRepo, const std::vector<std::string>& InArgs) -> shell::ExecResult {
    return shell::ExecuteCommand("git", InArgs, shell::ExecMode::PassThrough, InRepo);
}

auto HasCommand(const std::string& InCommand, const std::vector<std::string>& InArgs = {"--help"}) -> bool {
    const auto result = shell::ExecuteCommand(InCommand, InArgs, shell::ExecMode::Capture, std::filesystem::current_path());
    return result.exitCode == 0;
}

auto CopilotStandaloneCommand() -> std::string {
#if defined(_WIN32)
    return "copilot.cmd";
#else
    return "copilot";
#endif
}

auto HasStandaloneCopilotCommand() -> bool {
    return HasCommand(CopilotStandaloneCommand(), {"--help"}) || HasCommand("copilot", {"--help"});
}

auto ExecuteStandaloneCopilot(const std::vector<std::string>& InArgs,
                              std::optional<std::filesystem::path> InWorkingDir) -> shell::ExecResult {
    return shell::ExecuteCommand(CopilotStandaloneCommand(), InArgs, shell::ExecMode::Capture, InWorkingDir);
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

auto WriteFileText(const std::filesystem::path& InPath, const std::string& InText, std::string* OutError = nullptr) -> bool {
    std::error_code ec;
    const auto parent = InPath.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, ec);
        if (ec) {
            if (OutError != nullptr) {
                *OutError = std::format("create_directories failed: {}", ec.message());
            }
            return false;
        }
    }

    std::ofstream out(InPath, std::ios::out | std::ios::binary | std::ios::trunc);
    if (!out) {
        if (OutError != nullptr) {
            *OutError = std::format("open failed: {}", InPath.generic_string());
        }
        return false;
    }
    out << InText;
    if (!out.good()) {
        if (OutError != nullptr) {
            *OutError = std::format("write failed: {}", InPath.generic_string());
        }
        return false;
    }
    return true;
}

auto WriteCopilotPromptFile(const std::filesystem::path& InWorkdir,
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

    const auto promptDir = (InWorkdir / ".kano" / "tmp" / "git" / "copilot-prompts").lexically_normal();
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

auto BuildCopilotPromptArgument(std::optional<std::filesystem::path> InWorkingDir,
                                const std::string& InPrompt,
                                const std::string& InPurpose) -> std::string {
    if (!InWorkingDir.has_value()) {
        return InPrompt;
    }

    std::filesystem::path promptPath;
    if (!WriteCopilotPromptFile(*InWorkingDir, InPrompt, InPurpose, &promptPath)) {
        return InPrompt;
    }

    auto refPath = promptPath.lexically_relative(*InWorkingDir);
    if (refPath.empty()) {
        refPath = promptPath.lexically_normal();
    }
    return std::format(
        "Read @{} and follow it exactly. Treat that file as the complete task. Do not ask clarifying questions. Output only the final answer required by that file.",
        refPath.generic_string());
}

auto IsTruthyEnv(const char* InValue) -> bool {
    if (InValue == nullptr) {
        return false;
    }
    const auto v = ToLower(Trim(std::string(InValue)));
    return v == "1" || v == "true" || v == "yes" || v == "on";
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

auto ResolveSkillRoot(const std::filesystem::path& InWorkspaceRoot) -> std::filesystem::path {
    if (const char* envRoot = std::getenv("KANO_GIT_SKILL_ROOT"); envRoot != nullptr && std::string(envRoot).size() > 0) {
        return std::filesystem::path(envRoot).lexically_normal();
    }
    return (InWorkspaceRoot / ".agents" / "skills" / "kano" / "kano-git-master-skill").lexically_normal();
}

auto LoadNormalizedLineSet(const std::filesystem::path& InFile) -> std::unordered_set<std::string> {
    std::unordered_set<std::string> out;
    std::ifstream in(InFile);
    if (!in) {
        return out;
    }
    std::string line;
    while (std::getline(in, line)) {
        auto t = Trim(line);
        if (t.empty() || t[0] == '#') {
            continue;
        }
        std::replace(t.begin(), t.end(), '\\', '/');
        out.insert(t);
    }
    return out;
}

auto IsProbableIgnoreArtifactPath(const std::string& InPath) -> bool {
    auto p = InPath;
    std::replace(p.begin(), p.end(), '\\', '/');
    const auto lower = ToLower(p);
    auto contains = [&](const std::string& token) { return lower.find(token) != std::string::npos; };
    if (lower == ".kano" || lower.rfind(".kano/", 0) == 0 || contains("/.kano/") ||
        contains("/.cache/") || contains("/.pytest_cache/") || contains("/.mypy_cache/") || contains("/.idea/") || contains("/.vscode/")) {
        return true;
    }
    if (contains("/node_modules/") || contains("/dist/") || contains("/build/") || contains("/bin/") || contains("/obj/") || contains("/target/")) {
        return true;
    }
    return lower.ends_with(".log") || lower.ends_with(".tmp") || lower.ends_with(".temp") || lower.ends_with(".cache") ||
           lower.ends_with(".bak") || lower.ends_with(".swp") || lower.ends_with(".swo") || lower.ends_with(".class") ||
           lower.ends_with(".obj") || lower.ends_with(".o") || lower.ends_with(".pdb") || lower.ends_with(".ilk") ||
           lower.ends_with(".dmp") || lower.ends_with(".pyc");
}

auto IsInternalPipelineArtifactPath(const std::string& InPath) -> bool {
    auto lower = ToLower(InPath);
    std::replace(lower.begin(), lower.end(), '\\', '/');
    return lower == ".kano" || lower.rfind(".kano/", 0) == 0 || lower.find("/.kano/") != std::string::npos;
}

struct SecretRule {
    std::string id;
    std::regex pattern;
};

struct SecretFinding {
    std::string repo;
    std::string file;
    int line = 0;
    std::string ruleId;
    std::string preview;
};

auto LoadSecretRules(const std::filesystem::path& InFile) -> std::vector<SecretRule> {
    std::vector<SecretRule> out;
    std::ifstream in(InFile);
    if (!in) {
        return out;
    }
    std::string line;
    while (std::getline(in, line)) {
        const auto t = Trim(line);
        if (t.empty() || t[0] == '#') {
            continue;
        }
        const auto delim = t.find('|');
        if (delim == std::string::npos) {
            continue;
        }
        const auto id = Trim(t.substr(0, delim));
        const auto expr = Trim(t.substr(delim + 1));
        if (id.empty() || expr.empty()) {
            continue;
        }
        try {
            out.push_back({id, std::regex(expr, std::regex::ECMAScript | std::regex::icase)});
        } catch (const std::regex_error&) {
            continue;
        }
    }
    return out;
}

auto CollectChangedCandidateFiles(const std::filesystem::path& InRepo) -> std::vector<std::string> {
    std::unordered_set<std::string> files;
    const std::vector<std::vector<std::string>> commands = {
        {"diff", "--cached", "--name-only"},
        {"diff", "--name-only"},
        {"ls-files", "--others", "--exclude-standard"},
    };
    for (const auto& args : commands) {
        const auto out = GitCapture(InRepo, args);
        if (out.exitCode != 0) {
            continue;
        }
        std::istringstream iss(out.stdoutStr);
        std::string line;
        while (std::getline(iss, line)) {
            auto path = Trim(line);
            if (path.empty()) {
                continue;
            }
            const auto abs = (InRepo / std::filesystem::path(path)).lexically_normal();
            std::error_code ec;
            if (!std::filesystem::exists(abs, ec) || std::filesystem::is_directory(abs, ec)) {
                continue;
            }
            files.insert(path);
        }
    }
    return std::vector<std::string>(files.begin(), files.end());
}

auto ScanFileForSecretRules(const std::filesystem::path& InRepo,
                            const std::string& InFile,
                            const std::vector<SecretRule>& InRules,
                            const int InLimit,
                            std::vector<SecretFinding>* OutFindings) -> void {
    if (OutFindings == nullptr || static_cast<int>(OutFindings->size()) >= InLimit) {
        return;
    }
    std::ifstream in((InRepo / std::filesystem::path(InFile)).lexically_normal());
    if (!in) {
        return;
    }
    std::string line;
    int lineNo = 0;
    while (std::getline(in, line) && static_cast<int>(OutFindings->size()) < InLimit) {
        lineNo += 1;
        for (const auto& rule : InRules) {
            if (std::regex_search(line, rule.pattern)) {
                SecretFinding f;
                f.file = InFile;
                f.line = lineNo;
                f.ruleId = rule.id;
                f.preview = Trim(line);
                if (f.preview.size() > 160) {
                    f.preview = f.preview.substr(0, 160) + "...";
                }
                OutFindings->push_back(std::move(f));
                break;
            }
        }
    }
}

auto RunPipelineSafetyGatesForNonAiCommit(const std::filesystem::path& InWorkspaceRoot) -> void {
    if (!IsTruthyEnv(std::getenv("KOG_ALLOW_IGNORE_GATE")) && ToLower(Trim(std::getenv("KOG_IGNORE_GATE") == nullptr ? "on" : std::getenv("KOG_IGNORE_GATE"))) != "off") {
        auto repos = DiscoverWorkspaceRepos(InWorkspaceRoot);
        if (repos.empty()) {
            repos.push_back(InWorkspaceRoot);
        }
        const auto allowlistPath = (ResolveSkillRoot(InWorkspaceRoot) / "assets" / "ignore-sources" / "local" / "ignore-gate-allowlist.txt").lexically_normal();
        const auto allowlist = LoadNormalizedLineSet(allowlistPath);
        std::vector<std::string> findings;
        for (const auto& repo : repos) {
            const auto rel = repo.lexically_relative(InWorkspaceRoot).generic_string();
            const auto repoLabel = rel.empty() ? "." : rel;
            const auto untracked = GitCapture(repo, {"ls-files", "--others", "--exclude-standard"});
            if (untracked.exitCode != 0) {
                continue;
            }
            std::istringstream iss(untracked.stdoutStr);
            std::string path;
            while (std::getline(iss, path)) {
                auto p = Trim(path);
                if (p.empty() || !IsProbableIgnoreArtifactPath(p)) {
                    continue;
                }
                if (IsInternalPipelineArtifactPath(p)) {
                    continue;
                }
                std::replace(p.begin(), p.end(), '\\', '/');
                const auto key = repoLabel == "." ? p : (repoLabel + "/" + p);
                if (allowlist.find(key) != allowlist.end()) {
                    continue;
                }
                findings.push_back(key);
                if (findings.size() >= 20) {
                    break;
                }
            }
            if (findings.size() >= 20) {
                break;
            }
        }
        if (!findings.empty()) {
            std::cerr << "Error: ignore gate failed (commit); unresolved untracked artifact-like files detected.\n";
            for (const auto& f : findings) {
                std::cerr << "  - " << f << "\n";
            }
            std::cerr << "Hint: update .gitignore first, then regenerate plan.\n";
            std::cerr << "Hint: override once with --allow-ignore-gate (or KOG_ALLOW_IGNORE_GATE=1).\n";
            std::exit(3);
        }
    }

    if (IsTruthyEnv(std::getenv("KOG_DISABLE_SECRET_GATE"))) {
        return;
    }
    const auto rulesPath = (ResolveSkillRoot(InWorkspaceRoot) / "assets" / "security" / "secret-blacklist.rules").lexically_normal();
    const auto rules = LoadSecretRules(rulesPath);
    if (rules.empty()) {
        return;
    }
    auto repos = DiscoverWorkspaceRepos(InWorkspaceRoot);
    if (repos.empty()) {
        repos.push_back(InWorkspaceRoot);
    }
    std::vector<SecretFinding> findings;
    findings.reserve(20);
    for (const auto& repo : repos) {
        const auto changedFiles = CollectChangedCandidateFiles(repo);
        if (changedFiles.empty()) {
            continue;
        }
        const auto rel = repo.lexically_relative(InWorkspaceRoot).generic_string();
        const auto repoLabel = rel.empty() ? "." : rel;
        for (const auto& file : changedFiles) {
            if (static_cast<int>(findings.size()) >= 20) {
                break;
            }
            const auto before = findings.size();
            ScanFileForSecretRules(repo, file, rules, 20, &findings);
            for (std::size_t i = before; i < findings.size(); ++i) {
                findings[i].repo = repoLabel;
            }
        }
        if (static_cast<int>(findings.size()) >= 20) {
            break;
        }
    }
    if (!findings.empty()) {
        std::cerr << "Error: secret gate failed (commit); potential secrets detected.\n";
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
    }
}

auto ResolveProvider(const std::string& InProviderRaw) -> std::string {
    const auto provider = ToLower(Trim(InProviderRaw));
    if (!provider.empty() && provider != "auto") {
        return provider;
    }

    if (HasStandaloneCopilotCommand() || HasCommand("gh", {"copilot", "--version"})) {
        return "copilot";
    }
    if (HasCommand("codex", {"--help"})) {
        return "codex";
    }
    if (HasCommand("opencode", {"--help"})) {
        return "opencode";
    }
    return {};
}

auto HomeDirectory() -> std::filesystem::path {
    if (const char* home = std::getenv("HOME"); home != nullptr && std::string(home).size() > 0) {
        return std::filesystem::path(home);
    }
    if (const char* userProfile = std::getenv("USERPROFILE"); userProfile != nullptr && std::string(userProfile).size() > 0) {
        return std::filesystem::path(userProfile);
    }
    return {};
}

auto GitConfigPath(const std::string& InKey) -> std::string {
    const auto out = shell::ExecuteCommand("git", {"config", "--path", "--get", InKey}, shell::ExecMode::Capture, std::filesystem::current_path());
    if (out.exitCode != 0) {
        return {};
    }
    return Trim(out.stdoutStr);
}

auto ResolveGlobalCacheRoot() -> std::filesystem::path {
    const auto configured = GitConfigPath("kano.cache.global-dir");
    if (!configured.empty()) {
        const std::filesystem::path configuredPath(configured);
        if (configuredPath.is_absolute()) {
            return configuredPath.lexically_normal();
        }
        return (std::filesystem::current_path() / configuredPath).lexically_normal();
    }

    const auto home = HomeDirectory();
    if (home.empty()) {
        return {};
    }
    return (home / ".kano" / "cache" / "git").lexically_normal();
}

auto ResolveModelForAi(const std::string& InProvider,
                       const std::string& InModelRaw,
                       bool InAiAuto,
                       const std::filesystem::path& InWorkspaceRoot) -> std::string {
    auto model = Trim(InModelRaw);
    const auto modelLower = NormalizeAiModelKeyword(model);
    const auto provider = ToLower(Trim(InProvider));

    auto resolvePolicy = [&]() -> auto_model_policy::AutoModelPolicy {
        return auto_model_policy::ResolveAutoModelPolicy(provider, InWorkspaceRoot, ResolveSkillRoot(InWorkspaceRoot));
    };
    auto countDirtyEntries = [&]() -> int {
        int total = 0;
        for (const auto& repo : DiscoverWorkspaceRepos(InWorkspaceRoot)) {
            const auto status = GitCapture(repo, {"status", "--porcelain", "--untracked-files=all"});
            if (status.exitCode != 0 || Trim(status.stdoutStr).empty()) {
                continue;
            }
            std::istringstream iss(status.stdoutStr);
            std::string line;
            while (std::getline(iss, line)) {
                if (!Trim(line).empty()) {
                    total += 1;
                }
            }
        }
        return total;
    };
    auto resolveDefaultModel = [&]() -> std::string {
        if (provider == "codex") {
            return "gpt-5.2-codex";
        }
        return "gpt-5-mini";
    };
    auto resolveAutoModel = [&]() -> std::string {
        if (provider != "copilot") {
            return resolveDefaultModel();
        }
        const auto policy = resolvePolicy();
        const int changedEntries = countDirtyEntries();
        return auto_model_policy::ResolveModelForChangeCount(policy, changedEntries);
    };

    if (!model.empty() && modelLower != "auto") {
        if (modelLower == "provider-default") {
            return resolveDefaultModel();
        }
        return model;
    }

    const auto configuredSelection = kog_config::ResolveDefaultAiModelSelection(provider,
                                                                                InWorkspaceRoot,
                                                                                ResolveSkillRoot(InWorkspaceRoot),
                                                                                "auto");
    const auto configuredLower = NormalizeAiModelKeyword(configuredSelection);
    if (configuredLower == "provider-default") {
        return resolveDefaultModel();
    }
    if (configuredLower == "auto") {
        return resolveAutoModel();
    }
    if (!Trim(configuredSelection).empty()) {
        return configuredSelection;
    }

    if (InAiAuto || modelLower == "auto") {
        return resolveAutoModel();
    }

    return {};
}

auto BuildAiCommitPrompt(const std::filesystem::path& InWorkspaceRoot,
                         const std::filesystem::path& InRepo,
                         const CommitPreflightReport& InReport) -> std::string {
    const auto label = DisplayRepoLabel(InWorkspaceRoot, InRepo);
    const auto diff = GitCapture(InRepo, {"diff", "--cached", "--", "."});
    std::string diffText = diff.stdoutStr;
    constexpr std::size_t kMaxDiffChars = 12000;
    if (diffText.size() > kMaxDiffChars) {
        diffText = diffText.substr(0, kMaxDiffChars) + "\n... (truncated)";
    }

    if (const auto text = LoadPromptAssetText(InWorkspaceRoot,
                                              "KOG_COMMIT_MESSAGE_PROMPT_TEMPLATE",
                                              std::filesystem::path("assets") / "prompts" / "base" / "commit-message.md");
        text.has_value()) {
        auto prompt = *text;
        prompt = ReplaceAll(std::move(prompt), "{{REPO_LABEL}}", label);
        prompt = ReplaceAll(std::move(prompt), "{{STAGED_COUNT}}", std::to_string(InReport.stagedCount));
        prompt = ReplaceAll(std::move(prompt), "{{UNSTAGED_COUNT}}", std::to_string(InReport.unstagedCount));
        prompt = ReplaceAll(std::move(prompt), "{{UNTRACKED_COUNT}}", std::to_string(InReport.untrackedCount));
        prompt = ReplaceAll(std::move(prompt), "{{STAGED_DIFF}}", diffText);
        return prompt;
    }

    std::ostringstream oss;
    oss << "You are generating ONE git commit message.\n"
        << "Requirements:\n"
        << "- Output exactly one line\n"
        << "- Use Conventional Commits format\n"
        << "- No markdown, no code fences, no explanation\n\n"
        << "Repo: " << label << "\n"
        << "Staged: " << InReport.stagedCount << "\n"
        << "Unstaged: " << InReport.unstagedCount << "\n"
        << "Untracked: " << InReport.untrackedCount << "\n\n"
        << "Staged diff:\n"
        << diffText << "\n";
    return oss.str();
}

auto ExtractSingleLineMessage(const std::string& InText) -> std::string {
    auto NormalizeAiLine = [](std::string line) -> std::string {
        line = Trim(std::move(line));
        if (line.empty()) {
            return {};
        }

        if (line.rfind("- ", 0) == 0 || line.rfind("* ", 0) == 0) {
            line = Trim(line.substr(2));
        }

        // Some providers return html-ish wrappers (e.g. "<p>...") or noisy prefixes ("??").
        while (!line.empty() && (line.front() == '?' || line.front() == '!' || line.front() == '#' || line.front() == '*')) {
            line.erase(line.begin());
            line = Trim(line);
        }

        while (!line.empty() && line.front() == '<') {
            const auto close = line.find('>');
            if (close == std::string::npos || close > 24) {
                break;
            }
            auto tag = line.substr(1, close - 1);
            tag = ToLower(Trim(tag));
            if (tag == "p" || tag == "/p" || tag == "div" || tag == "/div" || tag == "span" || tag == "/span") {
                line = Trim(line.substr(close + 1));
                continue;
            }
            break;
        }

        if (line.ends_with("</p>")) {
            line = Trim(line.substr(0, line.size() - 4));
        } else if (line.ends_with("</div>")) {
            line = Trim(line.substr(0, line.size() - 6));
        } else if (line.ends_with("</span>")) {
            line = Trim(line.substr(0, line.size() - 7));
        }

        return line;
    };

    std::istringstream iss(InText);
    std::string line;
    while (std::getline(iss, line)) {
        line = NormalizeAiLine(std::move(line));
        if (line.empty()) {
            continue;
        }
        if (line.rfind("```", 0) == 0) {
            continue;
        }
        return line;
    }
    return {};
}

auto RunAiGenerate(const std::string& InProvider,
                   const std::string& InModel,
                   const std::string& InPrompt,
                   std::optional<std::filesystem::path> InWorkingDir = std::nullopt) -> shell::ExecResult {
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
        if (InWorkingDir.has_value()) {
            args.push_back("--dir");
            args.push_back(InWorkingDir->lexically_normal().generic_string());
        }
        if (!InModel.empty() && InModel != "auto") {
            args.push_back("--model");
            args.push_back(InModel);
        }
        args.push_back(InPrompt);
        return shell::ExecuteCommand("opencode", args, shell::ExecMode::Capture, InWorkingDir);
    }

    if (InProvider == "codex") {
        if (IsTruthyEnv(std::getenv("KOG_CODEX_USE_EXEC"))) {
            std::vector<std::string> args{"exec"};
            AppendBoolFlag(&args, "KOG_CODEX_FULL_AUTO", "--full-auto");
            AppendBoolFlag(&args, "KOG_CODEX_EPHEMERAL", "--ephemeral");
            AppendBoolFlag(&args, "KOG_CODEX_JSON", "--json");
            AppendSingleValueFlag(&args, "KOG_CODEX_SANDBOX", "--sandbox");
            AppendSingleValueFlag(&args, "KOG_CODEX_APPROVAL", "--ask-for-approval");
            AppendSingleValueFlag(&args, "KOG_CODEX_PROFILE", "--profile");
            AppendRepeatableFlag(&args, "KOG_CODEX_ADD_DIRS", "--add-dir");
            if (InWorkingDir.has_value()) {
                args.push_back("--cd");
                args.push_back(InWorkingDir->lexically_normal().generic_string());
            }
            if (!InModel.empty() && InModel != "auto") {
                args.push_back("--model");
                args.push_back(InModel);
            }
            args.push_back(InPrompt);
            return shell::ExecuteCommand("codex", args, shell::ExecMode::Capture, InWorkingDir);
        }
        if (!InModel.empty() && InModel != "auto") {
            return shell::ExecuteCommand("codex", {"-q", "--model", InModel, InPrompt}, shell::ExecMode::Capture, InWorkingDir);
        }
        return shell::ExecuteCommand("codex", {"-q", InPrompt}, shell::ExecMode::Capture, InWorkingDir);
    }

    if (InProvider == "copilot") {
        const auto copilotPrompt = BuildCopilotPromptArgument(InWorkingDir, InPrompt, "commit-ai");
        if (HasStandaloneCopilotCommand()) {
            std::vector<std::string> args{"-s"};
            if (!InModel.empty() && InModel != "auto") {
                args.push_back("--model");
                args.push_back(InModel);
            }
            AppendBoolFlagDefaultTrue(&args, "KOG_COPILOT_AUTOPILOT", "--autopilot");
            AppendSingleValueFlagWithDefault(&args,
                                             "KOG_COPILOT_MAX_AUTOPILOT_CONTINUES",
                                             "--max-autopilot-continues",
                                             "12");
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
            args.insert(args.end(), {"--no-color", "--stream", "off", "--no-ask-user", "-p", copilotPrompt});
            debugArgs(CopilotStandaloneCommand(), args);
            return ExecuteStandaloneCopilot(args, InWorkingDir);
        }

        if (HasCommand("gh", {"copilot", "--version"})) {
            std::vector<std::string> args{"copilot", "--", "-s"};
            if (!InModel.empty() && InModel != "auto") {
                args.push_back("--model");
                args.push_back(InModel);
            }
            AppendBoolFlagDefaultTrue(&args, "KOG_COPILOT_AUTOPILOT", "--autopilot");
            AppendSingleValueFlagWithDefault(&args,
                                             "KOG_COPILOT_MAX_AUTOPILOT_CONTINUES",
                                             "--max-autopilot-continues",
                                             "12");
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
            args.insert(args.end(), {"--no-color", "--stream", "off", "--no-ask-user", "-p", copilotPrompt});
            debugArgs("gh", args);
            return shell::ExecuteCommand("gh", args, shell::ExecMode::Capture, InWorkingDir);
        }
    }

    return shell::ExecResult{.exitCode = 1, .stderrStr = "unsupported provider or provider command unavailable"};
}

auto SummarizeAiFailure(const shell::ExecResult& InResult) -> std::string {
    std::string detail = Trim(InResult.stderrStr);
    if (detail.empty()) {
        detail = Trim(InResult.stdoutStr);
    }
    if (detail.empty()) {
        return "ai provider returned no details";
    }
    constexpr std::size_t kMaxLen = 140;
    if (detail.size() > kMaxLen) {
        detail = detail.substr(0, kMaxLen) + "...";
    }
    return detail;
}

auto GenerateAiCommitMessage(const std::filesystem::path& InWorkspaceRoot,
                             const std::filesystem::path& InRepo,
                             const CommitPreflightReport& InReport,
                             const NativeAiConfig& InAi,
                             std::string* OutFailureReason = nullptr) -> std::string {
    if (!InAi.enabled) {
        if (OutFailureReason != nullptr) {
            *OutFailureReason = "ai is disabled";
        }
        return {};
    }

    const auto prompt = BuildAiCommitPrompt(InWorkspaceRoot, InRepo, InReport);
    const auto out = RunAiGenerate(InAi.provider, InAi.model, prompt, InRepo);
    if (out.exitCode != 0) {
        if (OutFailureReason != nullptr) {
            *OutFailureReason = SummarizeAiFailure(out);
        }
        return {};
    }

    auto message = ExtractSingleLineMessage(out.stdoutStr + "\n" + out.stderrStr);
    if (message.empty() && OutFailureReason != nullptr) {
        *OutFailureReason = "ai provider returned empty message";
    }
    return message;
}

auto ShouldBlockByAiReview(const std::filesystem::path& InRepo,
                           const std::string& InMessage,
                           const NativeAiConfig& InAi,
                           std::string& OutReason) -> bool {
    if (!InAi.enabled || !InAi.reviewEnabled) {
        return false;
    }

    auto stagedDiff = GitCapture(InRepo, {"diff", "--cached", "--", "."}).stdoutStr;
    constexpr std::size_t kMaxDiffChars = 10000;
    if (stagedDiff.size() > kMaxDiffChars) {
        stagedDiff = stagedDiff.substr(0, kMaxDiffChars) + "\n... (truncated)";
    }

    std::string promptText;
    if (const auto text = LoadPromptAssetText(std::filesystem::current_path().lexically_normal(),
                                              "KOG_COMMIT_REVIEW_PROMPT_TEMPLATE",
                                              std::filesystem::path("assets") / "prompts" / "base" / "review.md");
        text.has_value()) {
        promptText = *text;
        promptText = ReplaceAll(std::move(promptText), "{{MESSAGE}}", InMessage);
        promptText = ReplaceAll(std::move(promptText), "{{STAGED_DIFF}}", stagedDiff);
    } else {
        std::ostringstream prompt;
        prompt << "You are a commit safety reviewer.\n"
               << "Evaluate whether this commit message matches staged changes and is safe.\n"
               << "Respond with exactly one line: PASS or FAIL: <reason>.\n\n"
               << "Message:\n" << InMessage << "\n\n"
               << "Staged diff:\n" << stagedDiff << "\n";
        promptText = prompt.str();
    }

    const auto out = RunAiGenerate(InAi.provider, InAi.model, promptText, InRepo);
    if (out.exitCode != 0) {
        return false;
    }

    const auto verdict = ToLower(ExtractSingleLineMessage(out.stdoutStr + "\n" + out.stderrStr));
    if (verdict.empty()) {
        return false;
    }

    const auto startsWithAny = [&](const std::vector<std::string>& prefixes) {
        for (const auto& prefix : prefixes) {
            if (verdict.rfind(prefix, 0) == 0) {
                return true;
            }
        }
        return false;
    };

    const bool explicitFail = startsWithAny({
        "fail",
        "[fail]",
        "fail:",
        "fail -",
        "verdict: fail",
        "verdict fail",
    });
    if (explicitFail) {
        OutReason = verdict;
        return true;
    }

    const bool explicitPass = startsWithAny({
        "pass",
        "[pass]",
        "pass:",
        "pass -",
        "verdict: pass",
        "verdict pass",
    });
    if (explicitPass) {
        return false;
    }

    // Unknown/non-conforming verdict: fail-open to avoid false positives.
    return false;
}

auto ParseReposCsv(const std::string& InCsv) -> std::vector<std::filesystem::path> {
    std::vector<std::filesystem::path> out;
    std::istringstream iss(InCsv);
    std::string token;
    while (std::getline(iss, token, ',')) {
        const auto trimmed = Trim(token);
        if (trimmed.empty()) {
            continue;
        }
        out.emplace_back(trimmed);
    }
    return out;
}

auto JoinReposCsv(const std::vector<std::filesystem::path>& InRepos) -> std::string {
    std::string out;
    for (std::size_t idx = 0; idx < InRepos.size(); ++idx) {
        auto value = InRepos[idx].generic_string();
        if (value.empty()) {
            continue;
        }
        if (!out.empty()) {
            out += ',';
        }
        out += value;
    }
    return out;
}

auto DiscoverWorkspaceRepos(const std::filesystem::path& InRoot) -> std::vector<std::filesystem::path>;

auto NormalizePath(const std::filesystem::path& InPath) -> std::filesystem::path {
    return InPath.lexically_normal();
}

auto ToGeneric(const std::filesystem::path& InPath) -> std::string {
    return NormalizePath(InPath).generic_string();
}

auto ResolveRepoPath(const std::filesystem::path& InWorkspaceRoot, const std::filesystem::path& InPath) -> std::filesystem::path {
    if (InPath.empty() || InPath == ".") {
        return NormalizePath(InWorkspaceRoot);
    }
    if (InPath.is_absolute()) {
        return NormalizePath(InPath);
    }
    const auto candidate = NormalizePath(InWorkspaceRoot / InPath);
    if (std::filesystem::exists(candidate) && IsGitRepo(candidate)) {
        return candidate;
    }

    std::string manifestReason;
    if (const auto manifest = workspace::LoadTrustedWorkspaceManifest(InWorkspaceRoot, &manifestReason); manifest.has_value()) {
        const auto specText = InPath.generic_string();
        std::vector<std::filesystem::path> exactMatches;
        std::vector<std::filesystem::path> fuzzyMatches;
        for (const auto& repo : manifest->repos) {
            const auto repoPath = NormalizePath(repo.path);
            const auto repoName = repoPath.filename().generic_string();
            const auto repoKey = repoPath.generic_string();
            const auto relativeKey = repoPath.lexically_relative(NormalizePath(InWorkspaceRoot)).generic_string();
            if (repoName == specText || repoKey == specText || relativeKey == specText) {
                exactMatches.push_back(repoPath);
                continue;
            }
            if (repoKey.find(specText) != std::string::npos || relativeKey.find(specText) != std::string::npos) {
                fuzzyMatches.push_back(repoPath);
            }
        }
        auto matches = exactMatches.empty() ? fuzzyMatches : exactMatches;
        std::sort(matches.begin(), matches.end(), [](const auto& A, const auto& B) {
            return A.generic_string() < B.generic_string();
        });
        matches.erase(std::unique(matches.begin(), matches.end(), [](const auto& A, const auto& B) {
            return A.generic_string() == B.generic_string();
        }), matches.end());
        if (matches.size() == 1) {
            return matches.front();
        }
        if (matches.size() > 1) {
            std::ostringstream oss;
            oss << "repo spec is ambiguous: " << specText << "\nMatches:\n";
            for (const auto& match : matches) {
                oss << "  - " << match.generic_string() << "\n";
            }
            throw std::runtime_error(oss.str());
        }
    }

    workspace::DiscoverOptions options;
    options.rootDir = InWorkspaceRoot;
    options.maxDepth = 12;
    options.useCache = true;
    options.metadataLevel = "minimal";

    const auto discovery = workspace::DiscoverRepos(options);
    const auto specText = InPath.generic_string();
    std::vector<std::filesystem::path> exactMatches;
    std::vector<std::filesystem::path> fuzzyMatches;

    for (const auto& repo : discovery.repos) {
        const auto repoPath = NormalizePath(repo.path);
        const auto repoName = repoPath.filename().generic_string();
        const auto repoKey = repoPath.generic_string();
        const auto relativeKey = repoPath.lexically_relative(NormalizePath(InWorkspaceRoot)).generic_string();

        if (repoName == specText || repoKey == specText || relativeKey == specText) {
            exactMatches.push_back(repoPath);
            continue;
        }
        if (repoKey.find(specText) != std::string::npos || relativeKey.find(specText) != std::string::npos) {
            fuzzyMatches.push_back(repoPath);
        }
    }

    auto matches = exactMatches.empty() ? fuzzyMatches : exactMatches;
    std::sort(matches.begin(), matches.end(), [](const auto& A, const auto& B) {
        return A.generic_string() < B.generic_string();
    });
    matches.erase(std::unique(matches.begin(), matches.end(), [](const auto& A, const auto& B) {
        return A.generic_string() == B.generic_string();
    }), matches.end());

    if (matches.empty()) {
        return candidate;
    }
    if (matches.size() > 1) {
        std::ostringstream oss;
        oss << "repo spec is ambiguous: " << specText << "\nMatches:\n";
        for (const auto& match : matches) {
            oss << "  - " << match.generic_string() << "\n";
        }
        throw std::runtime_error(oss.str());
    }
    return matches.front();
}

auto PathDepth(const std::filesystem::path& InPath) -> std::size_t {
    std::size_t depth = 0;
    for (const auto& part : InPath) {
        if (!part.empty()) {
            depth += 1;
        }
    }
    return depth;
}

auto DisplayRepoLabel(const std::filesystem::path& InWorkspaceRoot, const std::filesystem::path& InRepo) -> std::string {
    const auto rootNorm = NormalizePath(InWorkspaceRoot);
    const auto repoNorm = NormalizePath(InRepo);
    if (ToGeneric(rootNorm) == ToGeneric(repoNorm)) {
        auto rootName = rootNorm.filename().generic_string();
        if (rootName.empty()) {
            rootName = rootNorm.generic_string();
        }
        return rootName + " (.)";
    }
    const auto rel = repoNorm.lexically_relative(rootNorm);
    if (!rel.empty() && rel != ".") {
        return rel.generic_string();
    }
    return repoNorm.generic_string();
}

auto BuildCommitScope(const std::filesystem::path& InWorkspaceRoot,
                      const std::filesystem::path& InRepo) -> std::string {
    const auto rootNorm = NormalizePath(InWorkspaceRoot);
    const auto repoNorm = NormalizePath(InRepo);

    std::string scope;
    if (ToGeneric(rootNorm) == ToGeneric(repoNorm)) {
        scope = "root";
    } else {
        scope = DisplayRepoLabel(InWorkspaceRoot, InRepo);
    }

    for (auto& c : scope) {
        if (c == '/' || c == '\\' || c == ' ') {
            c = '-';
        }
    }

    return scope.empty() ? "root" : scope;
}

auto BuildOrderedRepoList(const std::filesystem::path& InWorkspaceRoot, const std::string& InReposCsv) -> std::vector<std::filesystem::path> {
    std::vector<std::filesystem::path> repos;
    if (Trim(InReposCsv).empty()) {
        repos = DiscoverWorkspaceRepos(InWorkspaceRoot);
        if (repos.empty()) {
            repos.push_back(InWorkspaceRoot);
        }
    } else {
        const auto parsed = ParseReposCsv(InReposCsv);
        repos.reserve(parsed.size());
        for (const auto& item : parsed) {
            repos.push_back(ResolveRepoPath(InWorkspaceRoot, item));
        }
    }

    std::unordered_set<std::string> seen;
    std::vector<std::filesystem::path> deduped;
    deduped.reserve(repos.size());
    for (const auto& repo : repos) {
        const auto key = ToGeneric(repo);
        if (key.empty()) {
            continue;
        }
        if (seen.insert(key).second) {
            deduped.push_back(repo);
        }
    }

    const auto rootKey = ToGeneric(InWorkspaceRoot);
    std::sort(deduped.begin(), deduped.end(), [&](const auto& A, const auto& B) {
        const auto aKey = ToGeneric(A);
        const auto bKey = ToGeneric(B);
        const bool aIsRoot = aKey == rootKey;
        const bool bIsRoot = bKey == rootKey;
        if (aIsRoot != bIsRoot) {
            return !aIsRoot && bIsRoot;
        }
        const auto aDepth = PathDepth(A);
        const auto bDepth = PathDepth(B);
        if (aDepth != bDepth) {
            return aDepth > bDepth;
        }
        return aKey < bKey;
    });

    return deduped;
}

auto DiscoverWorkspaceRepoRecords(const std::filesystem::path& InRoot,
                                  const std::string& InMetadataLevel,
                                  const bool InUseCache = true,
                                  const bool InRefreshCache = false) -> std::vector<workspace::RepoRecord> {
    if (InUseCache && !InRefreshCache) {
        std::string manifestReason;
        if (const auto manifest = workspace::LoadTrustedWorkspaceManifest(InRoot, &manifestReason); manifest.has_value()) {
            std::vector<workspace::RepoRecord> repos = manifest->repos;
            if (repos.empty()) {
                workspace::RepoRecord root;
                root.path = InRoot.lexically_normal();
                root.type = "root";
                repos.push_back(std::move(root));
            }
            for (auto& repo : repos) {
                const auto status = GitCapture(repo.path, {"status", "--porcelain"});
                repo.hasChanges = status.exitCode == 0 && !Trim(status.stdoutStr).empty();
            }
            std::sort(repos.begin(), repos.end(), [](const auto& A, const auto& B) {
                return A.path.lexically_normal().generic_string() < B.path.lexically_normal().generic_string();
            });
            repos.erase(std::unique(repos.begin(), repos.end(), [](const auto& A, const auto& B) {
                return A.path.lexically_normal().generic_string() == B.path.lexically_normal().generic_string();
            }), repos.end());
            return repos;
        }
    }

    workspace::DiscoverOptions options;
    options.rootDir = InRoot;
    options.maxDepth = 12;
    options.useCache = InUseCache;
    options.refreshCache = InRefreshCache;
    if (!InUseCache) {
        options.incremental = false;
    }
    options.metadataLevel = InMetadataLevel;

    const auto discovery = workspace::DiscoverRepos(options);
    std::vector<workspace::RepoRecord> repos = discovery.repos;
    std::sort(repos.begin(), repos.end(), [](const auto& A, const auto& B) {
        return A.path.lexically_normal().generic_string() < B.path.lexically_normal().generic_string();
    });
    repos.erase(std::unique(repos.begin(), repos.end(), [](const auto& A, const auto& B) {
        return A.path.lexically_normal().generic_string() == B.path.lexically_normal().generic_string();
    }), repos.end());
    return repos;
}

auto DiscoverWorkspaceRepos(const std::filesystem::path& InRoot) -> std::vector<std::filesystem::path> {
    std::vector<std::filesystem::path> repos;
    std::string manifestReason;
    if (const auto manifest = workspace::LoadTrustedWorkspaceManifest(InRoot, &manifestReason); manifest.has_value()) {
        repos.reserve(manifest->repos.size());
        for (const auto& repo : manifest->repos) {
            repos.push_back(repo.path.lexically_normal());
        }
        if (repos.empty()) {
            repos.push_back(InRoot.lexically_normal());
        }
        std::sort(repos.begin(), repos.end(), [](const auto& A, const auto& B) {
            return A.generic_string() < B.generic_string();
        });
        repos.erase(std::unique(repos.begin(), repos.end(), [](const auto& A, const auto& B) {
            return A.generic_string() == B.generic_string();
        }), repos.end());
        return repos;
    }
    const auto discovered = DiscoverWorkspaceRepoRecords(InRoot, "minimal");
    repos.reserve(discovered.size());
    for (const auto& repo : discovered) {
        repos.push_back(repo.path.lexically_normal());
    }
    return repos;
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

auto WorkspaceRepoKey(const std::filesystem::path& InWorkspaceRoot, const std::filesystem::path& InRepo) -> std::string {
    const auto rootNorm = NormalizePath(InWorkspaceRoot);
    const auto repoNorm = NormalizePath(InRepo);
    if (ToGeneric(rootNorm) == ToGeneric(repoNorm)) {
        return ".";
    }
    const auto rel = repoNorm.lexically_relative(rootNorm);
    if (rel.empty()) {
        return repoNorm.generic_string();
    }
    return rel.generic_string();
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
        const auto head = GitCapture(repo, {"rev-parse", "HEAD"});
        const auto sha = (head.exitCode == 0) ? Trim(head.stdoutStr) : std::string("0000000000000000000000000000000000000000");
        lines.push_back(WorkspaceRepoKey(InWorkspaceRoot, repo) + "\t" + sha);
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
        const auto status = GitCapture(repo, {"status", "--porcelain=v2", "--branch", "--untracked-files=normal", "--ignore-submodules=none"});
        if (status.exitCode != 0) {
            continue;
        }
        const auto normalized = Trim(status.stdoutStr);
        const auto head = ExtractBranchOidFromStatusV2(normalized);
        const auto statusFingerprint = normalized.empty() ? std::string("clean") : Fnv1a64Hex(normalized);
        lines.push_back(std::format("{}|{}|{}",
                                    WorkspaceRepoKey(InWorkspaceRoot, repo),
                                    head,
                                    statusFingerprint));
    }
    std::sort(lines.begin(), lines.end());
    std::ostringstream canonical;
    for (const auto& line : lines) {
        canonical << line << "\n";
    }
    return "ws-dirty-v2-" + Fnv1a64Hex(canonical.str());
}

auto BuildCommitScopeRecords(const std::filesystem::path& InWorkspaceRoot,
                             const std::string& InReposCsv,
                             const bool InNoRecursive,
                             const bool InDirtyOnly) -> std::vector<workspace::RepoRecord> {
    const bool forceFreshDirtyScope = InDirtyOnly && Trim(InReposCsv).empty() && !InNoRecursive;
    auto all = DiscoverWorkspaceRepoRecords(
        InWorkspaceRoot,
        "full",
        forceFreshDirtyScope ? false : true,
        forceFreshDirtyScope ? true : false
    );
    // Recovery path:
    // If recursive commit scope unexpectedly resolves to only root repo, refresh once without cache.
    // This avoids stale-discovery cache causing agent-mode cp/cpa to skip dirty subrepos.
    if (Trim(InReposCsv).empty() && !InNoRecursive && all.size() <= 1) {
        const auto refreshed = DiscoverWorkspaceRepoRecords(InWorkspaceRoot, "full", false, true);
        if (refreshed.size() > all.size()) {
            all = refreshed;
        }
    }
    std::unordered_map<std::string, workspace::RepoRecord> byPath;
    byPath.reserve(all.size());
    for (const auto& repo : all) {
        byPath.emplace(ToGeneric(repo.path), repo);
    }

    std::vector<workspace::RepoRecord> selected;
    auto reposCsv = Trim(InReposCsv);
    if (!reposCsv.empty()) {
        for (const auto& item : ParseReposCsv(reposCsv)) {
            const auto resolved = ResolveRepoPath(InWorkspaceRoot, item);
            const auto key = ToGeneric(resolved);
            if (key.empty()) {
                continue;
            }
            const auto found = byPath.find(key);
            if (found != byPath.end()) {
                selected.push_back(found->second);
            } else {
                workspace::RepoRecord fallback;
                fallback.path = resolved;
                fallback.type = "explicit";
                selected.push_back(std::move(fallback));
            }
        }
    } else if (InNoRecursive) {
        const auto rootKey = ToGeneric(InWorkspaceRoot);
        const auto found = byPath.find(rootKey);
        if (found != byPath.end()) {
            selected.push_back(found->second);
        } else {
            workspace::RepoRecord fallback;
            fallback.path = InWorkspaceRoot;
            fallback.type = "root";
            selected.push_back(std::move(fallback));
        }
    } else {
        for (const auto& repo : all) {
            selected.push_back(repo);
        }
    }

    if (InDirtyOnly && Trim(InReposCsv).empty() && !InNoRecursive && selected.size() <= 1) {
        const auto rootStatus = GitCapture(InWorkspaceRoot, {"status", "--porcelain"});
        if (rootStatus.exitCode == 0) {
            std::istringstream iss(rootStatus.stdoutStr);
            std::string line;
            while (std::getline(iss, line)) {
                while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) {
                    line.pop_back();
                }
                if (line.size() < 4) {
                    continue;
                }
                auto relPath = Trim(line.substr(3));
                if (relPath.empty()) {
                    continue;
                }
                const auto arrowPos = relPath.find(" -> ");
                if (arrowPos != std::string::npos) {
                    relPath = Trim(relPath.substr(arrowPos + 4));
                }
                const auto candidate = ResolveRepoPath(InWorkspaceRoot, relPath);
                if (candidate.empty()) {
                    continue;
                }
                const auto inGitRepo = GitCapture(candidate, {"rev-parse", "--is-inside-work-tree"});
                if (inGitRepo.exitCode != 0 || Trim(inGitRepo.stdoutStr) != "true") {
                    continue;
                }
                workspace::RepoRecord fallback;
                fallback.path = candidate;
                fallback.type = "explicit-dirty-fallback";
                fallback.hasChanges = true;
                selected.push_back(std::move(fallback));
            }
        }
    }

    std::unordered_map<std::string, std::size_t> idxByPath;
    idxByPath.reserve(selected.size());
    for (std::size_t i = 0; i < selected.size(); ++i) {
        idxByPath.emplace(ToGeneric(selected[i].path), i);
    }

    if (InDirtyOnly && reposCsv.empty() && !InNoRecursive) {
        std::vector<std::vector<std::size_t>> children(selected.size());
        for (std::size_t i = 0; i < selected.size(); ++i) {
            for (const auto& dep : selected[i].dependencies) {
                const auto it = idxByPath.find(ToGeneric(dep));
                if (it != idxByPath.end()) {
                    children[it->second].push_back(i);
                }
            }
        }

        std::vector<int> keepMemo(selected.size(), -1);
        std::function<bool(std::size_t)> keepNode = [&](const std::size_t index) -> bool {
            if (keepMemo[index] != -1) {
                return keepMemo[index] == 1;
            }
            bool keep = selected[index].hasChanges;
            for (const auto child : children[index]) {
                if (keepNode(child)) {
                    keep = true;
                }
            }
            keepMemo[index] = keep ? 1 : 0;
            return keep;
        };

        std::vector<workspace::RepoRecord> filtered;
        filtered.reserve(selected.size());
        for (std::size_t i = 0; i < selected.size(); ++i) {
            if (keepNode(i)) {
                filtered.push_back(selected[i]);
            }
        }
        selected = std::move(filtered);
    }

    std::unordered_set<std::string> keep;
    keep.reserve(selected.size());
    for (const auto& repo : selected) {
        keep.insert(ToGeneric(repo.path));
    }

    for (auto& repo : selected) {
        std::vector<std::filesystem::path> deps;
        deps.reserve(repo.dependencies.size());
        std::unordered_set<std::string> seenDeps;
        for (const auto& dep : repo.dependencies) {
            const auto key = ToGeneric(dep);
            if (keep.contains(key) && seenDeps.insert(key).second) {
                deps.push_back(dep);
            }
        }
        repo.dependencies = std::move(deps);
    }

    std::sort(selected.begin(), selected.end(), [&](const auto& A, const auto& B) {
        const auto aKey = ToGeneric(A.path);
        const auto bKey = ToGeneric(B.path);
        const bool aIsRoot = aKey == ToGeneric(InWorkspaceRoot);
        const bool bIsRoot = bKey == ToGeneric(InWorkspaceRoot);
        if (aIsRoot != bIsRoot) {
            return !aIsRoot && bIsRoot;
        }
        const auto aDepth = PathDepth(A.path);
        const auto bDepth = PathDepth(B.path);
        if (aDepth != bDepth) {
            return aDepth > bDepth;
        }
        return aKey < bKey;
    });

    return selected;
}

auto BuildExecutionWaves(const std::vector<workspace::RepoRecord>& InRepos) -> std::vector<std::vector<std::size_t>> {
    std::vector<std::vector<std::size_t>> waves;
    if (InRepos.empty()) {
        return waves;
    }

    std::unordered_map<std::string, std::size_t> byPath;
    byPath.reserve(InRepos.size());
    for (std::size_t idx = 0; idx < InRepos.size(); ++idx) {
        byPath.emplace(ToGeneric(InRepos[idx].path), idx);
    }

    std::vector<std::vector<std::size_t>> outgoing(InRepos.size());
    std::vector<std::size_t> indegree(InRepos.size(), 0);

    for (std::size_t idx = 0; idx < InRepos.size(); ++idx) {
        std::set<std::size_t> uniqueDeps;
        for (const auto& dep : InRepos[idx].dependencies) {
            const auto it = byPath.find(ToGeneric(dep));
            if (it == byPath.end()) {
                continue;
            }
            const auto depIdx = it->second;
            if (depIdx == idx) {
                continue;
            }
            if (uniqueDeps.insert(depIdx).second) {
                outgoing[depIdx].push_back(idx);
                indegree[idx] += 1;
            }
        }
    }

    std::vector<std::size_t> ready;
    ready.reserve(InRepos.size());
    for (std::size_t i = 0; i < InRepos.size(); ++i) {
        if (indegree[i] == 0) {
            ready.push_back(i);
        }
    }

    auto sortByPath = [&](std::vector<std::size_t>& list) {
        std::sort(list.begin(), list.end(), [&](const auto A, const auto B) {
            const auto aDepth = PathDepth(InRepos[A].path);
            const auto bDepth = PathDepth(InRepos[B].path);
            if (aDepth != bDepth) {
                return aDepth > bDepth;
            }
            return ToGeneric(InRepos[A].path) < ToGeneric(InRepos[B].path);
        });
    };
    sortByPath(ready);

    std::size_t processed = 0;
    while (!ready.empty()) {
        waves.push_back(ready);
        processed += ready.size();

        std::vector<std::size_t> next;
        for (const auto node : ready) {
            for (const auto out : outgoing[node]) {
                if (indegree[out] == 0) {
                    continue;
                }
                indegree[out] -= 1;
                if (indegree[out] == 0) {
                    next.push_back(out);
                }
            }
        }
        sortByPath(next);
        next.erase(std::unique(next.begin(), next.end()), next.end());
        ready = std::move(next);
    }

    if (processed != InRepos.size()) {
        waves.clear();
        std::vector<std::size_t> fallback;
        fallback.reserve(InRepos.size());
        for (std::size_t i = 0; i < InRepos.size(); ++i) {
            fallback.push_back(i);
        }
        sortByPath(fallback);
        waves.push_back(std::move(fallback));
    }

    return waves;
}

auto IsParentRepoPath(const std::filesystem::path& InParent, const std::filesystem::path& InChild) -> bool {
    const auto parent = ToGeneric(InParent);
    const auto child = ToGeneric(InChild);
    if (parent.empty() || child.empty() || parent == child) {
        return false;
    }
    const std::string prefix = parent + "/";
    return child.rfind(prefix, 0) == 0;
}

enum class CommitPlanStage {
    Commit,
    PostSync,
    Both,
};

struct RepoCommitPlanEntry {
    struct CommitReviewMeta {
        std::string verdict;
        std::string reason;
    };

    struct CommitItem {
        std::string message;
        CommitReviewMeta review;
        std::vector<std::string> include;
        std::vector<std::string> exclude;
    };

    std::string repoKey;
    std::vector<CommitItem> commits;
};

struct CommitPlanPayload {
    struct PlannerMeta {
        std::string provider;
        std::string model;
        std::string requestId;
    };

    struct ReviewMeta {
        std::string verdict;
        std::string reason;
    };

    struct Meta {
        std::string schemaVersion;
        std::string planId;
        std::string generatedAtUtc;
        std::string executedAtUtc;
        std::string baseHeadSha;
        std::string dirtyFingerprintPreIgnore;
        std::string dirtyFingerprint;
        PlannerMeta planner;
        ReviewMeta review;
    };

    Meta meta;
    std::vector<RepoCommitPlanEntry> commitEntries;
    std::vector<RepoCommitPlanEntry> postSyncEntries;
};

auto NormalizePlanKey(std::string InValue) -> std::string {
    auto key = Trim(std::move(InValue));
    for (auto& ch : key) {
        if (ch == '\\') {
            ch = '/';
        }
    }
    while (key.size() > 1 && key.back() == '/') {
        key.pop_back();
    }
    if (key.empty()) {
        return ".";
    }
    return key;
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

auto NormalizePlanPathspecToken(std::string InValue) -> std::string {
    auto value = Trim(std::move(InValue));
    if (value.empty()) {
        return value;
    }

    for (auto& ch : value) {
        if (ch == '\\') {
            ch = '/';
        }
    }

    std::string cleaned;
    cleaned.reserve(value.size());
    for (const char ch : value) {
        if (ch == '\r' || ch == '\n' || ch == '\t') {
            continue;
        }
        cleaned.push_back(ch);
    }

    const bool looksLikePath = cleaned.find('/') != std::string::npos;
    if (!looksLikePath) {
        return Trim(cleaned);
    }

    // AI output may wrap long paths with indentation spaces; strip all spaces for pathspec stability.
    std::string compact;
    compact.reserve(cleaned.size());
    for (const char ch : cleaned) {
        if (ch == ' ') {
            continue;
        }
        compact.push_back(ch);
    }
    return Trim(compact);
}

auto ExtractStringArrayForKey(const std::string& InObjectText, const std::string& InField) -> std::vector<std::string> {
    std::vector<std::string> out;
    const auto arrayBody = ExtractArrayBodyForKey(InObjectText, InField);
    if (!arrayBody.has_value()) {
        return out;
    }
    std::size_t pos = 0;
    while (pos < arrayBody->size()) {
        pos = SkipJsonWhitespace(*arrayBody, pos);
        if (pos >= arrayBody->size()) {
            break;
        }
        if ((*arrayBody)[pos] == ',') {
            pos += 1;
            continue;
        }
        const auto parsed = ParseJsonStringAt(*arrayBody, pos);
        if (!parsed.has_value()) {
            break;
        }
        auto value = NormalizePlanPathspecToken(parsed->first);
        if (!value.empty()) {
            out.push_back(std::move(value));
        }
        pos = parsed->second;
    }
    return out;
}

auto ParseStageEntries(const std::string& InStageArrayBody) -> std::vector<RepoCommitPlanEntry> {
    std::vector<RepoCommitPlanEntry> entries;
    for (const auto& repoObject : SplitTopLevelObjects(InStageArrayBody)) {
        const auto repoField = ExtractStringField(repoObject, "repo");
        if (!repoField.has_value()) {
            continue;
        }

        RepoCommitPlanEntry entry;
        entry.repoKey = NormalizePlanKey(*repoField);

        const auto commitsArrayBody = ExtractArrayBodyForKey(repoObject, "commits");
        if (commitsArrayBody.has_value()) {
            for (const auto& commitObject : SplitTopLevelObjects(*commitsArrayBody)) {
                const auto messageField = ExtractStringField(commitObject, "message");
                if (!messageField.has_value()) {
                    continue;
                }
                const auto message = Trim(*messageField);
                if (!message.empty()) {
                    RepoCommitPlanEntry::CommitItem item;
                    item.message = message;
                    item.include = ExtractStringArrayForKey(commitObject, "include");
                    item.exclude = ExtractStringArrayForKey(commitObject, "exclude");
                    if (const auto reviewObject = ExtractObjectBodyForKey(commitObject, "review"); reviewObject.has_value()) {
                        if (const auto value = ExtractStringField(*reviewObject, "verdict"); value.has_value()) {
                            item.review.verdict = ToLower(Trim(*value));
                        }
                        if (const auto value = ExtractStringField(*reviewObject, "reason"); value.has_value()) {
                            item.review.reason = Trim(*value);
                        }
                    }
                    entry.commits.push_back(std::move(item));
                }
            }
        }

        if (!entry.repoKey.empty() && !entry.commits.empty()) {
            entries.push_back(std::move(entry));
        }
    }
    return entries;
}

auto ParseCommitPlanStage(const std::string& InValue) -> std::optional<CommitPlanStage> {
    const auto value = ToLower(Trim(InValue));
    if (value.empty() || value == "commit") {
        return CommitPlanStage::Commit;
    }
    if (value == "post_sync" || value == "post-sync") {
        return CommitPlanStage::PostSync;
    }
    if (value == "both") {
        return CommitPlanStage::Both;
    }
    return std::nullopt;
}

auto ParseCommitPlan(const std::filesystem::path& InFile,
                     std::string* OutError) -> std::optional<CommitPlanPayload> {
    const auto payload = ReadFileText(InFile);
    if (!payload.has_value()) {
        if (OutError != nullptr) {
            *OutError = "cannot read plan file";
        }
        return std::nullopt;
    }

    const auto text = Trim(*payload);
    if (text.empty()) {
        if (OutError != nullptr) {
            *OutError = "plan file is empty";
        }
        return std::nullopt;
    }

    const auto stagesObject = ExtractObjectBodyForKey(text, "stages");
    if (!stagesObject.has_value()) {
        if (OutError != nullptr) {
            *OutError = "missing \"stages\" object";
        }
        return std::nullopt;
    }

    CommitPlanPayload out;
    if (const auto metaObject = ExtractObjectBodyForKey(text, "meta"); metaObject.has_value()) {
        if (const auto schemaVersion = ExtractStringField(*metaObject, "schema_version"); schemaVersion.has_value()) {
            out.meta.schemaVersion = Trim(*schemaVersion);
        }
        if (const auto planId = ExtractStringField(*metaObject, "plan_id"); planId.has_value()) {
            out.meta.planId = Trim(*planId);
        }
        if (const auto generatedAtUtc = ExtractStringField(*metaObject, "generated_at_utc"); generatedAtUtc.has_value()) {
            out.meta.generatedAtUtc = Trim(*generatedAtUtc);
        }
        if (const auto executedAtUtc = ExtractStringField(*metaObject, "executed_at_utc"); executedAtUtc.has_value()) {
            out.meta.executedAtUtc = Trim(*executedAtUtc);
        }
        if (const auto baseHeadSha = ExtractStringField(*metaObject, "base_head_sha"); baseHeadSha.has_value()) {
            out.meta.baseHeadSha = Trim(*baseHeadSha);
        }
        if (const auto dirtyFingerprintPreIgnore = ExtractStringField(*metaObject, "dirty_fingerprint_pre_ignore");
            dirtyFingerprintPreIgnore.has_value()) {
            out.meta.dirtyFingerprintPreIgnore = Trim(*dirtyFingerprintPreIgnore);
        }
        if (const auto dirtyFingerprint = ExtractStringField(*metaObject, "dirty_fingerprint"); dirtyFingerprint.has_value()) {
            out.meta.dirtyFingerprint = Trim(*dirtyFingerprint);
        }
        if (const auto plannerObject = ExtractObjectBodyForKey(*metaObject, "planner"); plannerObject.has_value()) {
            if (const auto value = ExtractStringField(*plannerObject, "provider"); value.has_value()) {
                out.meta.planner.provider = Trim(*value);
            }
            if (const auto value = ExtractStringField(*plannerObject, "ai-model"); value.has_value()) {
                out.meta.planner.model = Trim(*value);
            } else if (const auto valueLegacy = ExtractStringField(*plannerObject, "model"); valueLegacy.has_value()) {
                // Backward compatibility for older plan schema.
                out.meta.planner.model = Trim(*valueLegacy);
            }
            if (const auto value = ExtractStringField(*plannerObject, "request_id"); value.has_value()) {
                out.meta.planner.requestId = Trim(*value);
            }
        }
        if (const auto reviewObject = ExtractObjectBodyForKey(*metaObject, "review"); reviewObject.has_value()) {
            if (const auto value = ExtractStringField(*reviewObject, "verdict"); value.has_value()) {
                out.meta.review.verdict = ToLower(Trim(*value));
            }
            if (const auto value = ExtractStringField(*reviewObject, "reason"); value.has_value()) {
                out.meta.review.reason = Trim(*value);
            }
        }
    }

    if (const auto commitArray = ExtractArrayBodyForKey(*stagesObject, "commit"); commitArray.has_value()) {
        out.commitEntries = ParseStageEntries(*commitArray);
    }
    if (const auto postSyncArray = ExtractArrayBodyForKey(*stagesObject, "post_sync"); postSyncArray.has_value()) {
        out.postSyncEntries = ParseStageEntries(*postSyncArray);
    }

    if (out.commitEntries.empty() && out.postSyncEntries.empty()) {
        if (OutError != nullptr) {
            *OutError = "no valid stage entries found";
        }
        return std::nullopt;
    }
    return out;
}

auto IsPlaceholderValue(const std::string& InValue) -> bool {
    const auto value = Trim(InValue);
    return value.rfind("replace-with-", 0) == 0;
}

auto IsValidRequiredValue(const std::string& InValue) -> bool {
    const auto value = Trim(InValue);
    return !value.empty() && !IsPlaceholderValue(value);
}

auto ValidateCommitPlanForAiMode(const CommitPlanPayload& InPlan,
                                 std::string* OutError) -> bool {
    if (!IsValidRequiredValue(InPlan.meta.planId)) {
        if (OutError != nullptr) {
            *OutError = "meta.plan_id is missing or placeholder";
        }
        return false;
    }
    if (!IsValidRequiredValue(InPlan.meta.generatedAtUtc)) {
        if (OutError != nullptr) {
            *OutError = "meta.generated_at_utc is missing or placeholder";
        }
        return false;
    }
    if (!IsValidRequiredValue(InPlan.meta.baseHeadSha)) {
        if (OutError != nullptr) {
            *OutError = "meta.base_head_sha is missing or placeholder";
        }
        return false;
    }
    if (!IsValidRequiredValue(InPlan.meta.dirtyFingerprint)) {
        if (OutError != nullptr) {
            *OutError = "meta.dirty_fingerprint is missing or placeholder";
        }
        return false;
    }
    if (!IsValidRequiredValue(InPlan.meta.planner.provider)) {
        if (OutError != nullptr) {
            *OutError = "meta.planner.provider is missing or placeholder";
        }
        return false;
    }
    if (!IsValidRequiredValue(InPlan.meta.planner.model)) {
        if (OutError != nullptr) {
            *OutError = "meta.planner.ai-model is missing or placeholder";
        }
        return false;
    }
    if (ToLower(Trim(InPlan.meta.review.verdict)) != "pass") {
        if (OutError != nullptr) {
            *OutError = "meta.review.verdict must be \"pass\"";
        }
        return false;
    }
    if (!IsValidRequiredValue(InPlan.meta.review.reason)) {
        if (OutError != nullptr) {
            *OutError = "meta.review.reason is missing or placeholder";
        }
        return false;
    }

    bool hasValidMessage = false;
    auto scanEntries = [&](const std::vector<RepoCommitPlanEntry>& InEntries) {
        for (const auto& entry : InEntries) {
            for (const auto& item : entry.commits) {
                if (IsValidRequiredValue(item.message)) {
                    hasValidMessage = true;
                    return;
                }
            }
        }
    };
    scanEntries(InPlan.commitEntries);
    if (!hasValidMessage) {
        scanEntries(InPlan.postSyncEntries);
    }
    if (!hasValidMessage) {
        if (OutError != nullptr) {
            *OutError = "no valid non-placeholder commit messages found in stages.commit/post_sync";
        }
        return false;
    }

    auto validateEntryReviews = [&](const std::vector<RepoCommitPlanEntry>& InEntries,
                                    const std::string& InStageName) -> bool {
        for (const auto& entry : InEntries) {
            for (std::size_t idx = 0; idx < entry.commits.size(); ++idx) {
                const auto& item = entry.commits[idx];
                if (!IsValidRequiredValue(item.message)) {
                    if (OutError != nullptr) {
                        *OutError = std::format("{}.repo({}).commits[{}].message is missing or placeholder", InStageName, entry.repoKey, idx);
                    }
                    return false;
                }
                if (ToLower(Trim(item.review.verdict)) != "pass") {
                    if (OutError != nullptr) {
                        *OutError = std::format("{}.repo({}).commits[{}].review.verdict must be \"pass\"", InStageName, entry.repoKey, idx);
                    }
                    return false;
                }
                if (!IsValidRequiredValue(item.review.reason)) {
                    if (OutError != nullptr) {
                        *OutError = std::format("{}.repo({}).commits[{}].review.reason is missing or placeholder", InStageName, entry.repoKey, idx);
                    }
                    return false;
                }
            }
        }
        return true;
    };

    if (!validateEntryReviews(InPlan.commitEntries, "stages.commit")) {
        return false;
    }
    if (!validateEntryReviews(InPlan.postSyncEntries, "stages.post_sync")) {
        return false;
    }

    return true;
}

auto BuildStageMessageMap(const CommitPlanPayload& InPlan,
                          const CommitPlanStage InStage) -> std::unordered_map<std::string, std::vector<RepoCommitPlanEntry::CommitItem>> {
    std::unordered_map<std::string, std::vector<RepoCommitPlanEntry::CommitItem>> out;
    auto appendEntries = [&](const std::vector<RepoCommitPlanEntry>& entries) {
        for (const auto& entry : entries) {
            auto& bucket = out[NormalizePlanKey(entry.repoKey)];
            for (const auto& item : entry.commits) {
                bucket.push_back(item);
            }
        }
    };

    if (InStage == CommitPlanStage::Commit || InStage == CommitPlanStage::Both) {
        appendEntries(InPlan.commitEntries);
    }
    if (InStage == CommitPlanStage::PostSync || InStage == CommitPlanStage::Both) {
        appendEntries(InPlan.postSyncEntries);
    }
    return out;
}

auto ResolveRepoMessages(const std::unordered_map<std::string, std::vector<RepoCommitPlanEntry::CommitItem>>& InStageMessages,
                         const std::filesystem::path& InWorkspaceRoot,
                         const std::filesystem::path& InRepo,
                         const std::string& InDefaultMessage) -> std::vector<RepoCommitPlanEntry::CommitItem> {
    std::vector<std::string> candidates;
    const auto rootNorm = NormalizePath(InWorkspaceRoot);
    const auto repoNorm = NormalizePath(InRepo);

    candidates.push_back(NormalizePlanKey(ToGeneric(repoNorm)));
    if (ToGeneric(rootNorm) == ToGeneric(repoNorm)) {
        candidates.push_back(".");
    } else {
        const auto rel = repoNorm.lexically_relative(rootNorm);
        if (!rel.empty() && rel != ".") {
            candidates.push_back(NormalizePlanKey(rel.generic_string()));
        }
    }
    candidates.push_back(NormalizePlanKey(repoNorm.filename().generic_string()));

    for (const auto& key : candidates) {
        if (const auto it = InStageMessages.find(key); it != InStageMessages.end() && !it->second.empty()) {
            return it->second;
        }
    }

    if (!InDefaultMessage.empty()) {
        RepoCommitPlanEntry::CommitItem one;
        one.message = InDefaultMessage;
        return {one};
    }
    RepoCommitPlanEntry::CommitItem one;
    one.message = "";
    return {one};
}

struct RepoCommitRunbook {
    std::size_t repoRecordIndex = 0;
    std::filesystem::path repo;
    std::vector<RepoCommitPlanEntry::CommitItem> commits;
    bool valid = true;
    std::string validationError;
};

struct CommitTaskNode {
    std::size_t repoRecordIndex = 0;
    std::size_t commitIndexInRepo = 0;
    std::size_t repoCommitCount = 0;
    std::filesystem::path repo;
    RepoCommitPlanEntry::CommitItem commit;
};

struct CommitTaskGraph {
    std::vector<CommitTaskNode> tasks;
    std::vector<std::vector<std::size_t>> waves;
    bool dependencyCycleDetected = false;
};

auto BuildRepoCommitRunbooks(const std::vector<workspace::RepoRecord>& InRepoRecords,
                             const std::unordered_map<std::string, std::vector<RepoCommitPlanEntry::CommitItem>>& InStageMessages,
                             const std::filesystem::path& InWorkspaceRoot,
                             const std::string& InDefaultMessage,
                             const bool InIsPlanMode) -> std::vector<RepoCommitRunbook> {
    std::vector<RepoCommitRunbook> out;
    out.reserve(InRepoRecords.size());
    for (std::size_t ridx = 0; ridx < InRepoRecords.size(); ++ridx) {
        RepoCommitRunbook runbook;
        runbook.repoRecordIndex = ridx;
        runbook.repo = InRepoRecords[ridx].path;
        runbook.commits = ResolveRepoMessages(InStageMessages, InWorkspaceRoot, runbook.repo, InDefaultMessage);
        if (InIsPlanMode && runbook.commits.size() > 1) {
            bool hasUnscoped = false;
            for (const auto& item : runbook.commits) {
                if (item.include.empty() && item.exclude.empty()) {
                    hasUnscoped = true;
                    break;
                }
            }
            if (hasUnscoped) {
                runbook.valid = false;
                runbook.validationError = "plan has multiple commits for one repo but some commit entries miss include/exclude scope";
                runbook.commits.clear();
            }
        }
        out.push_back(std::move(runbook));
    }
    return out;
}

auto BuildCommitTaskGraph(const std::vector<workspace::RepoRecord>& InRepoRecords,
                          const std::vector<RepoCommitRunbook>& InRunbooks) -> CommitTaskGraph {
    CommitTaskGraph graph;
    if (InRepoRecords.empty() || InRunbooks.empty()) {
        return graph;
    }

    std::vector<std::vector<std::size_t>> repoTaskIndices(InRepoRecords.size());
    for (const auto& runbook : InRunbooks) {
        if (!runbook.valid || runbook.commits.empty()) {
            continue;
        }
        for (std::size_t cidx = 0; cidx < runbook.commits.size(); ++cidx) {
            CommitTaskNode node;
            node.repoRecordIndex = runbook.repoRecordIndex;
            node.commitIndexInRepo = cidx;
            node.repoCommitCount = runbook.commits.size();
            node.repo = runbook.repo;
            node.commit = runbook.commits[cidx];
            const auto taskIndex = graph.tasks.size();
            graph.tasks.push_back(std::move(node));
            repoTaskIndices[runbook.repoRecordIndex].push_back(taskIndex);
        }
    }

    if (graph.tasks.empty()) {
        return graph;
    }

    std::vector<std::vector<std::size_t>> outgoing(graph.tasks.size());
    std::vector<std::unordered_set<std::size_t>> dedupOutgoing(graph.tasks.size());
    std::vector<std::size_t> indegree(graph.tasks.size(), 0);

    auto addEdge = [&](const std::size_t from, const std::size_t to) {
        if (from == to) {
            return;
        }
        if (dedupOutgoing[from].insert(to).second) {
            outgoing[from].push_back(to);
            indegree[to] += 1;
        }
    };

    for (const auto& taskList : repoTaskIndices) {
        for (std::size_t idx = 1; idx < taskList.size(); ++idx) {
            addEdge(taskList[idx - 1], taskList[idx]);
        }
    }

    std::unordered_map<std::string, std::size_t> repoByPath;
    repoByPath.reserve(InRepoRecords.size());
    for (std::size_t idx = 0; idx < InRepoRecords.size(); ++idx) {
        repoByPath.emplace(ToGeneric(InRepoRecords[idx].path), idx);
    }

    for (std::size_t ridx = 0; ridx < InRepoRecords.size(); ++ridx) {
        if (repoTaskIndices[ridx].empty()) {
            continue;
        }
        for (const auto& dep : InRepoRecords[ridx].dependencies) {
            const auto depIt = repoByPath.find(ToGeneric(dep));
            if (depIt == repoByPath.end()) {
                continue;
            }
            const auto depRepoIndex = depIt->second;
            if (depRepoIndex == ridx || repoTaskIndices[depRepoIndex].empty()) {
                continue;
            }
            // dependencies[] currently points to parent repos (superproject).
            // For commit apply, child commits must land first so parent pointer updates can commit afterward.
            const auto repoTail = repoTaskIndices[ridx].back();
            const auto depHead = repoTaskIndices[depRepoIndex].front();
            addEdge(repoTail, depHead);
        }
    }

    for (std::size_t parentIdx = 0; parentIdx < InRepoRecords.size(); ++parentIdx) {
        if (repoTaskIndices[parentIdx].empty()) {
            continue;
        }
        for (std::size_t childIdx = 0; childIdx < InRepoRecords.size(); ++childIdx) {
            if (parentIdx == childIdx || repoTaskIndices[childIdx].empty()) {
                continue;
            }
            if (!IsParentRepoPath(InRepoRecords[parentIdx].path, InRepoRecords[childIdx].path)) {
                continue;
            }
            const auto childTail = repoTaskIndices[childIdx].back();
            const auto parentHead = repoTaskIndices[parentIdx].front();
            addEdge(childTail, parentHead);
        }
    }

    auto nodeLess = [&](const std::size_t A, const std::size_t B) {
        const auto& taskA = graph.tasks[A];
        const auto& taskB = graph.tasks[B];
        const auto& repoA = InRepoRecords[taskA.repoRecordIndex].path;
        const auto& repoB = InRepoRecords[taskB.repoRecordIndex].path;
        const auto depthA = PathDepth(repoA);
        const auto depthB = PathDepth(repoB);
        if (depthA != depthB) {
            return depthA > depthB;
        }
        const auto keyA = ToGeneric(repoA);
        const auto keyB = ToGeneric(repoB);
        if (keyA != keyB) {
            return keyA < keyB;
        }
        return taskA.commitIndexInRepo < taskB.commitIndexInRepo;
    };

    std::vector<std::size_t> ready;
    ready.reserve(graph.tasks.size());
    for (std::size_t idx = 0; idx < graph.tasks.size(); ++idx) {
        if (indegree[idx] == 0) {
            ready.push_back(idx);
        }
    }
    std::sort(ready.begin(), ready.end(), nodeLess);

    std::size_t processed = 0;
    while (!ready.empty()) {
        graph.waves.push_back(ready);
        processed += ready.size();
        std::vector<std::size_t> next;
        for (const auto node : ready) {
            for (const auto out : outgoing[node]) {
                if (indegree[out] == 0) {
                    continue;
                }
                indegree[out] -= 1;
                if (indegree[out] == 0) {
                    next.push_back(out);
                }
            }
        }
        std::sort(next.begin(), next.end(), nodeLess);
        next.erase(std::unique(next.begin(), next.end()), next.end());
        ready = std::move(next);
    }

    if (processed != graph.tasks.size()) {
        graph.dependencyCycleDetected = true;
        graph.waves.clear();
        std::vector<std::size_t> fallback;
        fallback.reserve(graph.tasks.size());
        for (std::size_t idx = 0; idx < graph.tasks.size(); ++idx) {
            fallback.push_back(idx);
        }
        std::sort(fallback.begin(), fallback.end(), nodeLess);
        for (const auto idx : fallback) {
            graph.waves.push_back({idx});
        }
    }

    return graph;
}

auto StageCommitItemForPlan(const std::filesystem::path& InRepo,
                            const RepoCommitPlanEntry::CommitItem& InItem,
                            std::string* OutError) -> bool {
    const auto reset = GitPassThrough(InRepo, {"reset", "-q"});
    if (reset.exitCode != 0) {
        if (OutError != nullptr) {
            *OutError = "git reset failed before plan-staged commit";
        }
        return false;
    }

    std::vector<std::string> args{"add", "-A", "--"};
    if (!InItem.include.empty()) {
        args.insert(args.end(), InItem.include.begin(), InItem.include.end());
    }
    for (const auto& ex : InItem.exclude) {
        if (ex.rfind(":(exclude)", 0) == 0) {
            args.push_back(ex);
        } else {
            args.push_back(std::string(":(exclude)") + ex);
        }
    }

    const auto add = GitPassThrough(InRepo, args);
    if (add.exitCode != 0) {
        if (OutError != nullptr) {
            *OutError = "git add failed for plan include/exclude pathspec";
        }
        return false;
    }

    const auto staged = GitCapture(InRepo, {"diff", "--cached", "--name-only"});
    if (staged.exitCode != 0 || Trim(staged.stdoutStr).empty()) {
        if (OutError != nullptr) {
            *OutError = "plan commit staged no files (check include/exclude pathspec)";
        }
        return false;
    }
    return true;
}

auto ParseJobsValue(const std::string& InValue) -> std::optional<int> {
    const auto value = ToLower(Trim(InValue));
    if (value.empty() || value == "auto") {
        return std::nullopt;
    }
    try {
        const int jobs = std::stoi(value);
        if (jobs <= 0) {
            return std::nullopt;
        }
        return jobs;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

auto ResolveCommitJobs(const std::string& InJobs,
                       const std::size_t InRepoCount,
                       const bool InAiEnabled) -> int {
    if (InRepoCount == 0) {
        return 1;
    }

    if (const auto explicitJobs = ParseJobsValue(InJobs); explicitJobs.has_value()) {
        return std::max(1, std::min(*explicitJobs, static_cast<int>(InRepoCount)));
    }

    unsigned int cores = std::thread::hardware_concurrency();
    if (cores == 0) {
        cores = 4;
    }

    int cap = 1;
    if (InAiEnabled) {
        cap = static_cast<int>(std::max(1u, std::min(4u, cores / 2)));
    } else {
        cap = static_cast<int>(std::max(1u, std::min(8u, cores)));
    }

    return std::max(1, std::min(cap, static_cast<int>(InRepoCount)));
}

auto RunCommitPreflight(const std::filesystem::path& InRepo) -> CommitPreflightReport {
    CommitPreflightReport report;
    report.repoPath = InRepo;
    const auto inRepo = GitCapture(InRepo, {"rev-parse", "--is-inside-work-tree"});
    report.inRepo = (inRepo.exitCode == 0 && Trim(inRepo.stdoutStr) == "true");
    if (!report.inRepo) {
        return report;
    }

    const auto status = GitCapture(InRepo, {"-c", "color.status=false", "status", "--porcelain"});
    if (status.exitCode != 0) {
        return report;
    }

    std::string line;
    std::string content = status.stdoutStr;
    std::size_t start = 0;
    while (start <= content.size()) {
        const auto end = content.find('\n', start);
        line = (end == std::string::npos) ? content.substr(start) : content.substr(start, end - start);
        start = (end == std::string::npos) ? (content.size() + 1) : (end + 1);

        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) {
            line.pop_back();
        }
        if (line.size() < 3) {
            continue;
        }

        const char x = line[0];
        const char y = line[1];
        const auto filePath = Trim(line.substr(3));

        if (x == '?' && y == '?') {
            report.untrackedCount += 1;
            if (!filePath.empty()) {
                report.untrackedFiles.push_back(filePath);
            }
        }
        if (x != ' ' && x != '?') {
            report.stagedCount += 1;
            if (!filePath.empty()) {
                report.stagedFiles.push_back(filePath);
            }
        }
        if (y != ' ' || (x == '?' && y == '?')) {
            report.unstagedCount += 1;
            if (!filePath.empty() && !(x == '?' && y == '?')) {
                report.unstagedFiles.push_back(filePath);
            }
        }

        if (!filePath.empty() && LooksRiskyPath(filePath)) {
            report.riskyFiles.push_back(filePath);
        }
    }

    return report;
}

auto HasAnyChanges(const CommitPreflightReport& InReport) -> bool {
    return InReport.stagedCount > 0 || InReport.unstagedCount > 0 || InReport.untrackedCount > 0;
}

auto BuildAutoCommitMessage(const std::filesystem::path& InWorkspaceRoot,
                           const std::filesystem::path& InRepo,
                           const CommitPreflightReport& InReport) -> std::string {
    std::string type = "chore";
    bool hasFiles = false;
    bool docsOnly = true;

    auto inspectFile = [&](const std::string& path) {
        if (path.empty()) {
            return;
        }
        hasFiles = true;
        std::string lower = path;
        for (auto& c : lower) {
            if (c >= 'A' && c <= 'Z') {
                c = static_cast<char>(c - 'A' + 'a');
            }
        }
        const bool isDoc = lower.ends_with(".md") || lower.rfind("docs/", 0) == 0 || lower.find("/docs/") != std::string::npos;
        if (!isDoc) {
            docsOnly = false;
        }
        if (lower.find("test") != std::string::npos) {
            type = "test";
        }
    };

    for (const auto& file : InReport.stagedFiles) {
        inspectFile(file);
    }
    for (const auto& file : InReport.unstagedFiles) {
        inspectFile(file);
    }
    for (const auto& file : InReport.untrackedFiles) {
        inspectFile(file);
    }

    if (type != "test" && hasFiles && docsOnly) {
        type = "docs";
    }

    const auto scope = BuildCommitScope(InWorkspaceRoot, InRepo);

    const int changedFiles = static_cast<int>(InReport.stagedFiles.size() + InReport.unstagedFiles.size() + InReport.untrackedFiles.size());
    const int safeCount = changedFiles > 0 ? changedFiles : 1;
    return std::format("{}({}): update {} file{}", type, scope, safeCount, safeCount == 1 ? "" : "s");
}

auto CurrentBranch(const std::filesystem::path& InRepo) -> std::string {
    const auto out = GitCapture(InRepo, {"rev-parse", "--abbrev-ref", "HEAD"});
    if (out.exitCode != 0) {
        return {};
    }
    const auto value = Trim(out.stdoutStr);
    if (value == "HEAD") {
        return {};
    }
    return value;
}

auto ResolveUpstreamRef(const std::filesystem::path& InRepo) -> std::string {
    const auto out = GitCapture(InRepo, {"rev-parse", "--abbrev-ref", "--symbolic-full-name", "@{upstream}"});
    if (out.exitCode != 0) {
        return {};
    }
    return Trim(out.stdoutStr);
}

auto ParsePositiveInt(const std::string& InValue) -> int {
    try {
        const auto value = Trim(InValue);
        if (value.empty()) {
            return 0;
        }
        return std::max(0, std::stoi(value));
    } catch (const std::exception&) {
        return 0;
    }
}

auto CountUnpushedCommits(const std::filesystem::path& InRepo, const std::string& InUpstreamRef) -> int {
    if (InUpstreamRef.empty()) {
        return 0;
    }
    const auto out = GitCapture(InRepo, {"rev-list", "--count", InUpstreamRef + "..HEAD"});
    if (out.exitCode != 0) {
        return 0;
    }
    return ParsePositiveInt(out.stdoutStr);
}

auto HasRemote(const std::filesystem::path& InRepo, const std::string& InRemote) -> bool {
    const auto out = GitCapture(InRepo, {"remote", "get-url", InRemote});
    return out.exitCode == 0;
}

auto PushRepo(const std::filesystem::path& InRepo, const std::string& InBranch) -> bool {
    const std::vector<std::string> remotes = {"origin-ssh", "origin-http", "origin"};
    bool triedRemote = false;
    for (const auto& remote : remotes) {
        if (!HasRemote(InRepo, remote)) {
            continue;
        }
        triedRemote = true;
        const auto push = GitPassThrough(InRepo, {"push", remote, InBranch});
        if (push.exitCode == 0) {
            return true;
        }
    }
    return !triedRemote ? false : false;
}

auto HeadCommitTitle(const std::filesystem::path& InRepo) -> std::string {
    const auto out = GitCapture(InRepo, {"show", "-s", "--format=%s", "HEAD"});
    if (out.exitCode != 0) {
        return {};
    }
    return Trim(out.stdoutStr);
}

struct RepoCommitResult {
    std::filesystem::path repo;
    bool committed = false;
    bool pushed = false;
    bool failed = false;
    std::string note;
    std::string commitTitle;
};

struct RepoAmendResult {
    std::filesystem::path repo;
    bool amended = false;
    bool combined = false;
    bool failed = false;
    std::string note;
};

auto CommitSingleRepo(const std::filesystem::path& InWorkspaceRoot,
                     const std::filesystem::path& InRepo,
                     const std::string& InMessage,
                     const bool InStagedOnly,
                     const bool InPush,
                     const NativeAiConfig& InAi) -> RepoCommitResult {
    RepoCommitResult result;
    result.repo = InRepo;

    auto report = RunCommitPreflight(InRepo);
    if (!report.inRepo) {
        result.failed = true;
        result.note = "not a git repository";
        return result;
    }

    if (!HasAnyChanges(report)) {
        result.note = "no changes";
        return result;
    }

    if (InStagedOnly && report.stagedCount == 0) {
        result.note = "staged-only with nothing staged";
        return result;
    }

    if (!InStagedOnly && (report.unstagedCount > 0 || report.untrackedCount > 0)) {
        const auto add = GitPassThrough(InRepo, {"add", "-A"});
        if (add.exitCode != 0) {
            result.failed = true;
            result.note = "git add -A failed";
            return result;
        }
        report = RunCommitPreflight(InRepo);
    }

    if (report.stagedCount == 0) {
        result.note = "nothing staged after preparation";
        return result;
    }

    std::string commitMessage;
    if (!InMessage.empty()) {
        commitMessage = InMessage;
    } else {
        std::string aiFailureReason;
        commitMessage = GenerateAiCommitMessage(InWorkspaceRoot, InRepo, report, InAi, &aiFailureReason);
        if (commitMessage.empty()) {
            commitMessage = BuildAutoCommitMessage(InWorkspaceRoot, InRepo, report);
            result.note = "ai message unavailable (" + aiFailureReason + "); used native fallback";
        } else {
            result.note = "ai message generated";
        }
    }

    std::string reviewReason;
    if (ShouldBlockByAiReview(InRepo, commitMessage, InAi, reviewReason)) {
        result.failed = true;
        result.note = "blocked by ai review: " + reviewReason;
        return result;
    }

    const auto commit = GitPassThrough(InRepo, {"commit", "-m", commitMessage});
    if (commit.exitCode != 0) {
        const auto status = RunCommitPreflight(InRepo);
        if (status.stagedCount == 0) {
            result.note = "nothing to commit";
            return result;
        }
        result.failed = true;
        result.note = "git commit failed";
        return result;
    }

    result.committed = true;
    result.commitTitle = HeadCommitTitle(InRepo);
    if (result.note.empty()) {
        result.note = "committed";
    }

    if (InPush) {
        const auto branch = CurrentBranch(InRepo);
        if (branch.empty()) {
            result.failed = true;
            result.note = "cannot push: detached HEAD or unknown branch";
            return result;
        }

        if (!PushRepo(InRepo, branch)) {
            result.failed = true;
            result.note = "push failed on all origin remotes";
            return result;
        }
        result.pushed = true;
        result.note += result.note.empty() ? "committed + pushed" : " + pushed";
    }

    return result;
}

auto BuildCombineFallbackMessage(const std::filesystem::path& InWorkspaceRoot,
                                 const std::filesystem::path& InRepo,
                                 int InCombinedCommits,
                                 const CommitPreflightReport& InReport) -> std::string {
    const auto scope = BuildCommitScope(InWorkspaceRoot, InRepo);

    const int combined = std::max(1, InCombinedCommits);
    const int stagedFiles = std::max(1, InReport.stagedCount);
    return std::format("chore({}): combine {} local commit{} into {} file{} update",
                       scope,
                       combined,
                       combined == 1 ? "" : "s",
                       stagedFiles,
                       stagedFiles == 1 ? "" : "s");
}

auto AmendSingleRepo(const std::filesystem::path& InWorkspaceRoot,
                    const std::filesystem::path& InRepo,
                    const std::string& InMessage,
                    const bool InStagedOnly,
                    const bool InCombineUnpushed,
                    const NativeAiConfig& InAi) -> RepoAmendResult {
    RepoAmendResult result;
    result.repo = InRepo;

    auto report = RunCommitPreflight(InRepo);
    if (!report.inRepo) {
        result.failed = true;
        result.note = "not a git repository";
        return result;
    }

    if (InCombineUnpushed) {
        const auto upstream = ResolveUpstreamRef(InRepo);
        if (upstream.empty()) {
            result.failed = true;
            result.note = "combine requires tracking upstream (@{upstream})";
            return result;
        }

        const int unpushedCount = CountUnpushedCommits(InRepo, upstream);
        if (unpushedCount <= 0) {
            result.note = "no local unpushed commits to combine";
            return result;
        }

        const auto softReset = GitPassThrough(InRepo, {"reset", "--soft", upstream});
        if (softReset.exitCode != 0) {
            result.failed = true;
            result.note = "git reset --soft to upstream failed";
            return result;
        }

        if (!InStagedOnly) {
            const auto add = GitPassThrough(InRepo, {"add", "-A"});
            if (add.exitCode != 0) {
                result.failed = true;
                result.note = "git add -A failed after combine reset";
                return result;
            }
        }

        report = RunCommitPreflight(InRepo);
        if (report.stagedCount == 0) {
            result.note = "no staged content after combine preparation";
            return result;
        }

        std::string commitMessage;
        if (!InMessage.empty()) {
            commitMessage = InMessage;
        } else {
            std::string aiFailureReason;
            commitMessage = GenerateAiCommitMessage(InWorkspaceRoot, InRepo, report, InAi, &aiFailureReason);
            if (commitMessage.empty()) {
                commitMessage = BuildCombineFallbackMessage(InWorkspaceRoot, InRepo, unpushedCount, report);
                result.note = "combined with native fallback message (ai unavailable: " + aiFailureReason + ")";
            } else {
                result.note = "combined with ai-generated message";
            }
        }

        std::string reviewReason;
        if (ShouldBlockByAiReview(InRepo, commitMessage, InAi, reviewReason)) {
            result.failed = true;
            result.note = "blocked by ai review: " + reviewReason;
            return result;
        }

        const auto commit = GitPassThrough(InRepo, {"commit", "-m", commitMessage});
        if (commit.exitCode != 0) {
            result.failed = true;
            result.note = "git commit failed after combine";
            return result;
        }

        result.combined = true;
        result.amended = true;
        if (result.note.empty()) {
            result.note = "combined unpushed commits";
        }
        return result;
    }

    if (!InStagedOnly && (report.unstagedCount > 0 || report.untrackedCount > 0)) {
        const auto add = GitPassThrough(InRepo, {"add", "-A"});
        if (add.exitCode != 0) {
            result.failed = true;
            result.note = "git add -A failed";
            return result;
        }
        report = RunCommitPreflight(InRepo);
    }

    const auto headExists = GitCapture(InRepo, {"rev-parse", "--verify", "HEAD"});
    if (headExists.exitCode != 0) {
        result.failed = true;
        result.note = "amend requires at least one existing commit";
        return result;
    }

    std::string commitMessage = InMessage;
    if (commitMessage.empty() && InAi.enabled && report.stagedCount > 0) {
        std::string aiFailureReason;
        commitMessage = GenerateAiCommitMessage(InWorkspaceRoot, InRepo, report, InAi, &aiFailureReason);
        if (commitMessage.empty()) {
            result.note = "ai message unavailable (" + aiFailureReason + "); amend keeps previous message";
        } else {
            result.note = "amended with ai-generated message";
        }
    }

    if (!commitMessage.empty()) {
        std::string reviewReason;
        if (ShouldBlockByAiReview(InRepo, commitMessage, InAi, reviewReason)) {
            result.failed = true;
            result.note = "blocked by ai review: " + reviewReason;
            return result;
        }
    }

    std::vector<std::string> amendArgs = {"commit", "--amend"};
    if (!commitMessage.empty()) {
        amendArgs.push_back("-m");
        amendArgs.push_back(commitMessage);
    } else {
        amendArgs.push_back("--no-edit");
    }

    const auto amend = GitPassThrough(InRepo, amendArgs);
    if (amend.exitCode != 0) {
        result.failed = true;
        result.note = "git commit --amend failed";
        return result;
    }

    result.amended = true;
    if (result.note.empty()) {
        result.note = "amended HEAD";
    }
    return result;
}

auto PrintCommitSummary(const std::filesystem::path& InWorkspaceRoot,
                        const std::vector<RepoCommitResult>& InResults) -> int {
    int failed = 0;
    int committed = 0;
    int pushed = 0;
    int skipped = 0;

    std::cout << "\n=== Native Commit Summary ===\n";
    std::cout << std::left << std::setw(36) << "Repo"
              << std::setw(12) << "Result"
              << "Detail\n";
    std::cout << std::left << std::setw(36) << "----"
              << std::setw(12) << "------"
              << "------\n";

    bool printedAnyCommitted = false;
    for (const auto& item : InResults) {
        const auto repoLabel = DisplayRepoLabel(InWorkspaceRoot, item.repo);
        std::string status;
        if (item.failed) {
            status = "failed";
            failed += 1;
        } else if (item.committed) {
            status = item.pushed ? "pushed" : "committed";
            committed += 1;
            if (item.pushed) {
                pushed += 1;
            }
        } else {
            status = "skipped";
            skipped += 1;
        }

        if (!item.committed) {
            continue;
        }

        printedAnyCommitted = true;
        const auto detail = item.commitTitle.empty() ? item.note : item.commitTitle;

        std::cout << std::left << std::setw(36) << repoLabel
                  << std::setw(12) << status
                  << detail << "\n";
    }

    if (!printedAnyCommitted) {
        std::cout << "(no commits created)\n";
    }

    std::cout << "\nTotals: committed=" << committed
              << " pushed=" << pushed
              << " skipped=" << skipped
              << " failed=" << failed << "\n";

    return failed == 0 ? 0 : 1;
}

auto PrintAmendSummary(const std::filesystem::path& InWorkspaceRoot,
                       const std::vector<RepoAmendResult>& InResults) -> int {
    int failed = 0;
    int amended = 0;
    int combined = 0;
    int skipped = 0;

    std::cout << "\n=== Native Amend Summary ===\n";
    std::cout << std::left << std::setw(36) << "Repo"
              << std::setw(12) << "Result"
              << "Detail\n";
    std::cout << std::left << std::setw(36) << "----"
              << std::setw(12) << "------"
              << "------\n";

    for (const auto& item : InResults) {
        const auto repoLabel = DisplayRepoLabel(InWorkspaceRoot, item.repo);
        std::string status;
        if (item.failed) {
            status = "failed";
            failed += 1;
        } else if (item.amended) {
            status = item.combined ? "combined" : "amended";
            amended += 1;
            if (item.combined) {
                combined += 1;
            }
        } else {
            status = "skipped";
            skipped += 1;
        }

        std::cout << std::left << std::setw(36) << repoLabel
                  << std::setw(12) << status
                  << item.note << "\n";
    }

    std::cout << "\nTotals: amended=" << amended
              << " combined=" << combined
              << " skipped=" << skipped
              << " failed=" << failed << "\n";

    return failed == 0 ? 0 : 1;
}

auto PrintCommitPreflight(const CommitPreflightReport& InReport, bool InStagedOnly) -> void {
    std::cout << "=== Commit Preflight (native) ===\n";
    if (!InReport.inRepo) {
        std::cout << "repo: not a git repository\n";
        return;
    }

    std::cout << "staged: " << InReport.stagedCount << "\n";
    std::cout << "unstaged: " << InReport.unstagedCount << "\n";
    std::cout << "untracked: " << InReport.untrackedCount << "\n";
    std::cout << "mode: " << (InStagedOnly ? "staged-only" : "auto-stage shell path") << "\n";

    auto printFileTable = [](const std::string& title, const std::vector<std::string>& files) {
        if (files.empty()) {
            return;
        }
        std::cout << "\n" << title << "\n";
        std::cout << std::left << std::setw(6) << "No." << "Path\n";
        std::cout << std::left << std::setw(6) << "---" << "----\n";
        const std::size_t limit = 25;
        const std::size_t count = std::min(files.size(), limit);
        for (std::size_t i = 0; i < count; ++i) {
            std::cout << std::left << std::setw(6) << (i + 1) << files[i] << "\n";
        }
        if (files.size() > limit) {
            std::cout << "... and " << (files.size() - limit) << " more\n";
        }
    };

    printFileTable("Staged set preview", InReport.stagedFiles);
    printFileTable("Unstaged changes preview", InReport.unstagedFiles);
    printFileTable("Untracked files preview", InReport.untrackedFiles);

    if (InReport.riskyFiles.empty()) {
        std::cout << "risk: no obvious secret-like file names\n";
    } else {
        std::cout << "risk: potential secret-like files detected\n";
        for (const auto& file : InReport.riskyFiles) {
            std::cout << "  - " << file << "\n";
        }
    }

    std::cout << "policy hints:\n";
    if (InReport.stagedCount == 0) {
        std::cout << "  - Stage intended files before commit\n";
    }
    if (InReport.unstagedCount > 0) {
        std::cout << "  - Unstaged changes exist; commit scope may be incomplete\n";
    }
    if (InReport.untrackedCount > 0) {
        std::cout << "  - Untracked files exist; verify if they should be included\n";
    }
}

} // namespace

auto RunCommitNativePlanStage(const std::filesystem::path& InWorkspaceRoot,
                              const std::string& InPlanFile,
                              const std::string& InPlanStage,
                              const bool InProfile) -> int {
    using clock = std::chrono::steady_clock;
    const auto totalStart = clock::now();
    long long preflightMs = 0;
    long long planningMs = 0;
    long long commitMs = 0;
    long long summaryMs = 0;

    const auto workspaceRoot = InWorkspaceRoot.lexically_normal();

    const auto preflightStart = clock::now();
    const auto report = RunCommitPreflight(workspaceRoot);
    PrintCommitPreflight(report, false);
    if (!report.inRepo) {
        return 1;
    }
    preflightMs = std::chrono::duration_cast<std::chrono::milliseconds>(clock::now() - preflightStart).count();

    std::cout << "[native-commit] safety-gates: ignore + secret\n";
    RunPipelineSafetyGatesForNonAiCommit(workspaceRoot);

    std::string planError;
    const auto normalizedCommitPlanPath = NormalizeInputPathForCurrentPlatform(InPlanFile);
    const auto parsed = ParseCommitPlan(std::filesystem::path(normalizedCommitPlanPath), &planError);
    if (!parsed.has_value()) {
        std::cerr << "Error: invalid --plan-file: " << normalizedCommitPlanPath;
        if (!planError.empty()) {
            std::cerr << " (" << planError << ")";
        }
        std::cerr << "\n";
        return 2;
    }

    std::string validationError;
    if (!ValidateCommitPlanForAiMode(*parsed, &validationError)) {
        std::cerr << "Error: invalid --plan-file: " << normalizedCommitPlanPath;
        if (!validationError.empty()) {
            std::cerr << " (" << validationError << ")";
        }
        std::cerr << "\n";
        return 2;
    }
    const auto currentBaseHeadSha = ComputeWorkspaceBaseHeadSha(workspaceRoot);
    const auto currentDirtyFingerprint = ComputeWorkspaceDirtyFingerprint(workspaceRoot);
    if (Trim(parsed->meta.baseHeadSha) != currentBaseHeadSha ||
        Trim(parsed->meta.dirtyFingerprint) != currentDirtyFingerprint) {
        std::cerr << "Error: invalid --plan-file: workspace state drift detected.\n";
        std::cerr << "  plan.path=" << normalizedCommitPlanPath << "\n";
        std::cerr << "  plan.base_head_sha=" << parsed->meta.baseHeadSha << "\n";
        std::cerr << "  current.base_head_sha=" << currentBaseHeadSha << "\n";
        std::cerr << "  plan.dirty_fingerprint=" << parsed->meta.dirtyFingerprint << "\n";
        std::cerr << "  current.dirty_fingerprint=" << currentDirtyFingerprint << "\n";
        std::cerr << "Hint: regenerate/refill plan before commit apply.\n";
        return 2;
    }

    const auto stage = ParseCommitPlanStage(InPlanStage);
    if (!stage.has_value()) {
        std::cerr << "Error: invalid --plan-stage value: " << InPlanStage
                  << " (expected commit|post_sync|both)\n";
        return 2;
    }

    auto stageMessages = BuildStageMessageMap(*parsed, *stage);
    if (stageMessages.empty()) {
        std::println("[native-commit] no entries found for selected --plan-stage; skipping commit.");
        if (InProfile) {
            const auto totalMs = std::chrono::duration_cast<std::chrono::milliseconds>(clock::now() - totalStart).count();
            std::cout << "\n=== Commit Profile Summary ===\n";
            std::cout << "mode: native\n";
            std::cout << "repo_count: 0\n";
            std::cout << "preflight_ms: " << preflightMs << "\n";
            std::cout << "planning_ms: 0\n";
            std::cout << "commit_ms: 0\n";
            std::cout << "summary_ms: 0\n";
            std::cout << "total_ms: " << totalMs << "\n";
        }
        return 0;
    }

    const auto planningStart = clock::now();
    std::string planReposCsv;
    for (const auto& [repoKey, items] : stageMessages) {
        if (items.empty()) {
            continue;
        }
        if (!planReposCsv.empty()) {
            planReposCsv += ",";
        }
        planReposCsv += repoKey;
    }
    auto repoRecords = BuildCommitScopeRecords(workspaceRoot, planReposCsv, false, true);
    if (repoRecords.empty()) {
        workspace::RepoRecord fallback;
        fallback.path = workspaceRoot;
        fallback.type = "root";
        repoRecords.push_back(std::move(fallback));
    }
    planningMs = std::chrono::duration_cast<std::chrono::milliseconds>(clock::now() - planningStart).count();

    const auto repoWaves = BuildExecutionWaves(repoRecords);
    const auto runbooks = BuildRepoCommitRunbooks(repoRecords, stageMessages, workspaceRoot, "", true);
    const auto taskGraph = BuildCommitTaskGraph(repoRecords, runbooks);
    const int workers = ResolveCommitJobs("auto", taskGraph.tasks.size(), false);

    std::vector<RepoCommitResult> results;
    results.reserve(repoRecords.size() + taskGraph.tasks.size());
    for (const auto& runbook : runbooks) {
        if (runbook.valid) {
            continue;
        }
        RepoCommitResult failed;
        failed.repo = runbook.repo;
        failed.failed = true;
        failed.note = runbook.validationError;
        results.push_back(std::move(failed));
    }

    NativeAiConfig ai{};
    ai.enabled = false;
    ai.reviewEnabled = false;

    const auto commitStart = clock::now();
    std::cout << "[native-commit] plan: repos=" << repoRecords.size()
              << " repo_waves=" << repoWaves.size()
              << " commits=" << taskGraph.tasks.size()
              << " commit_waves=" << taskGraph.waves.size()
              << " jobs=" << workers
              << " dirty_only=on\n";
    if (taskGraph.dependencyCycleDetected) {
        std::cout << "[native-commit] warning: dependency cycle detected in commit graph; downgraded to serial fallback order.\n";
    }

    auto executeCommitTask = [&](const CommitTaskNode& InNode) -> RepoCommitResult {
        const auto& repo = InNode.repo;
        const auto& repoMessage = InNode.commit;
        const bool needsPlanStaging =
            InNode.repoCommitCount > 1 || !repoMessage.include.empty() || !repoMessage.exclude.empty();
        if (needsPlanStaging) {
            std::string stageError;
            if (!StageCommitItemForPlan(repo, repoMessage, &stageError)) {
                RepoCommitResult failed;
                failed.repo = repo;
                failed.failed = true;
                failed.note = std::format("plan commit[{}] stage failed: {}", InNode.commitIndexInRepo, stageError);
                return failed;
            }
        }
        return CommitSingleRepo(workspaceRoot, repo, repoMessage.message, needsPlanStaging, false, ai);
    };

    for (const auto& wave : taskGraph.waves) {
        if (wave.empty()) {
            continue;
        }
        const int waveWorkers = std::max(1, std::min(workers, static_cast<int>(wave.size())));
        if (waveWorkers == 1) {
            for (const auto nodeIndex : wave) {
                const auto& task = taskGraph.tasks[nodeIndex];
                const auto label = DisplayRepoLabel(workspaceRoot, task.repo);
                std::cout << "\n[commit] " << label
                          << " [" << (task.commitIndexInRepo + 1) << "/" << task.repoCommitCount << "]\n";
                results.push_back(executeCommitTask(task));
            }
            continue;
        }

        std::vector<std::future<std::pair<std::size_t, RepoCommitResult>>> active;
        active.reserve(static_cast<std::size_t>(waveWorkers));
        std::size_t cursor = 0;
        std::vector<std::pair<std::size_t, RepoCommitResult>> waveResults;
        waveResults.reserve(wave.size());

        while (cursor < wave.size() || !active.empty()) {
            while (cursor < wave.size() && static_cast<int>(active.size()) < waveWorkers) {
                const auto nodeIndex = wave[cursor++];
                const auto& task = taskGraph.tasks[nodeIndex];
                const auto label = DisplayRepoLabel(workspaceRoot, task.repo);
                std::cout << "\n[commit] " << label
                          << " [" << (task.commitIndexInRepo + 1) << "/" << task.repoCommitCount << "]\n";
                active.push_back(std::async(std::launch::async, [&, nodeIndex]() {
                    const auto one = executeCommitTask(taskGraph.tasks[nodeIndex]);
                    return std::make_pair(nodeIndex, one);
                }));
            }

            if (!active.empty()) {
                waveResults.push_back(active.front().get());
                active.erase(active.begin());
            }
        }

        std::sort(waveResults.begin(), waveResults.end(), [&](const auto& A, const auto& B) {
            return A.first < B.first;
        });
        for (auto& [idx, one] : waveResults) {
            static_cast<void>(idx);
            results.push_back(std::move(one));
        }
    }
    commitMs = std::chrono::duration_cast<std::chrono::milliseconds>(clock::now() - commitStart).count();

    const auto summaryStart = clock::now();
    const auto exitCode = PrintCommitSummary(workspaceRoot, results);
    summaryMs = std::chrono::duration_cast<std::chrono::milliseconds>(clock::now() - summaryStart).count();

    if (InProfile) {
        const auto totalMs = std::chrono::duration_cast<std::chrono::milliseconds>(clock::now() - totalStart).count();
        std::cout << "\n=== Commit Profile Summary ===\n";
        std::cout << "mode: native\n";
        std::cout << "repo_count: " << repoRecords.size() << "\n";
        std::cout << "preflight_ms: " << preflightMs << "\n";
        std::cout << "planning_ms: " << planningMs << "\n";
        std::cout << "commit_ms: " << commitMs << "\n";
        std::cout << "summary_ms: " << summaryMs << "\n";
        std::cout << "total_ms: " << totalMs << "\n";
    }

    return exitCode;
}

auto RunCommitNativeSimple(const std::filesystem::path& InWorkspaceRoot,
                           const std::string& InReposCsv,
                           const bool InNoRecursive,
                           const std::string& InMessage,
                           const bool InStagedOnly,
                           const bool InDryRun,
                           const std::string& InAiProvider,
                           const std::string& InAiModel,
                           const bool InAiAuto,
                           const bool InNoAiReview,
                           const bool InProfile) -> int {
    using clock = std::chrono::steady_clock;
    const auto totalStart = clock::now();
    long long preflightMs = 0;
    long long planningMs = 0;
    long long commitMs = 0;
    long long summaryMs = 0;

    const auto workspaceRoot = InWorkspaceRoot.lexically_normal();
    const auto report = RunCommitPreflight(workspaceRoot);
    PrintCommitPreflight(report, InStagedOnly);
    if (!report.inRepo) {
        return 1;
    }
    if (InStagedOnly && report.stagedCount == 0) {
        std::cerr << "Preflight blocked: --staged-only but nothing staged\n";
        return 2;
    }
    preflightMs = std::chrono::duration_cast<std::chrono::milliseconds>(clock::now() - totalStart).count();

    NativeAiConfig ai;
    const bool aiRequested = InAiAuto || !InAiProvider.empty() || !InAiModel.empty();
    ai.provider = aiRequested ? ResolveProvider(InAiProvider) : std::string{};
    ai.model = aiRequested ? ResolveModelForAi(ai.provider, InAiModel, InAiAuto, workspaceRoot) : std::string{};
    ai.reviewEnabled = !InNoAiReview;
    ai.enabled = aiRequested && !ai.provider.empty();

    if (aiRequested && !ai.enabled) {
        std::cerr << "Error: AI mode requested, but provider is unavailable.\n";
        std::cerr << "- provider resolved: " << (ai.provider.empty() ? "<none>" : ai.provider) << "\n";
        std::cerr << "- model: " << (ai.model.empty() ? "<none>" : ai.model) << "\n";
        return 2;
    }
    if (ai.enabled) {
        std::cout << "[native-commit] AI enabled: provider=" << ai.provider
                  << " model=" << ai.model
                  << " review=" << (ai.reviewEnabled ? "on" : "off") << "\n";
    } else {
        std::cout << "[native-commit] safety-gates: ignore + secret\n";
        RunPipelineSafetyGatesForNonAiCommit(workspaceRoot);
    }

    const auto planningStart = clock::now();
    const bool dirtyOnly = true;
    auto repoRecords = BuildCommitScopeRecords(workspaceRoot, Trim(InReposCsv), InNoRecursive, dirtyOnly);
    if (repoRecords.empty()) {
        workspace::RepoRecord fallback;
        fallback.path = workspaceRoot;
        fallback.type = "root";
        repoRecords.push_back(std::move(fallback));
    }
    planningMs = std::chrono::duration_cast<std::chrono::milliseconds>(clock::now() - planningStart).count();

    std::unordered_map<std::string, std::vector<RepoCommitPlanEntry::CommitItem>> emptyStages;
    const auto runbooks = BuildRepoCommitRunbooks(repoRecords, emptyStages, workspaceRoot, InMessage, false);
    const auto taskGraph = BuildCommitTaskGraph(repoRecords, runbooks);
    const int workers = ResolveCommitJobs("auto", taskGraph.tasks.size(), ai.enabled);

    if (InDryRun) {
        std::cout << "[native-commit] dry-run: planned commits=" << taskGraph.tasks.size()
                  << " repos=" << repoRecords.size() << "\n";
        for (const auto& task : taskGraph.tasks) {
            const auto label = DisplayRepoLabel(workspaceRoot, task.repo);
            std::cout << "  - " << label << ": " << task.commit.message << "\n";
        }
        return 0;
    }

    std::vector<RepoCommitResult> results;
    results.reserve(repoRecords.size() + taskGraph.tasks.size());
    for (const auto& runbook : runbooks) {
        if (runbook.valid) {
            continue;
        }
        RepoCommitResult failed;
        failed.repo = runbook.repo;
        failed.failed = true;
        failed.note = runbook.validationError;
        results.push_back(std::move(failed));
    }

    const auto commitStart = clock::now();
    std::cout << "[native-commit] plan: repos=" << repoRecords.size()
              << " commits=" << taskGraph.tasks.size()
              << " commit_waves=" << taskGraph.waves.size()
              << " jobs=" << workers
              << " dirty_only=on\n";

    auto executeCommitTask = [&](const CommitTaskNode& InNode) -> RepoCommitResult {
        const auto& repo = InNode.repo;
        const auto& repoMessage = InNode.commit;
        return CommitSingleRepo(workspaceRoot, repo, repoMessage.message, InStagedOnly, false, ai);
    };

    for (const auto& wave : taskGraph.waves) {
        if (wave.empty()) {
            continue;
        }
        const int waveWorkers = std::max(1, std::min(workers, static_cast<int>(wave.size())));
        if (waveWorkers == 1) {
            for (const auto nodeIndex : wave) {
                const auto& task = taskGraph.tasks[nodeIndex];
                const auto label = DisplayRepoLabel(workspaceRoot, task.repo);
                std::cout << "\n[commit] " << label
                          << " [" << (task.commitIndexInRepo + 1) << "/" << task.repoCommitCount << "]\n";
                results.push_back(executeCommitTask(task));
            }
            continue;
        }
        std::vector<std::future<std::pair<std::size_t, RepoCommitResult>>> active;
        active.reserve(static_cast<std::size_t>(waveWorkers));
        std::size_t cursor = 0;
        std::vector<std::pair<std::size_t, RepoCommitResult>> waveResults;
        waveResults.reserve(wave.size());
        while (cursor < wave.size() || !active.empty()) {
            while (cursor < wave.size() && static_cast<int>(active.size()) < waveWorkers) {
                const auto nodeIndex = wave[cursor++];
                const auto& task = taskGraph.tasks[nodeIndex];
                const auto label = DisplayRepoLabel(workspaceRoot, task.repo);
                std::cout << "\n[commit] " << label
                          << " [" << (task.commitIndexInRepo + 1) << "/" << task.repoCommitCount << "]\n";
                active.push_back(std::async(std::launch::async, [&, nodeIndex]() {
                    const auto one = executeCommitTask(taskGraph.tasks[nodeIndex]);
                    return std::make_pair(nodeIndex, one);
                }));
            }
            if (!active.empty()) {
                waveResults.push_back(active.front().get());
                active.erase(active.begin());
            }
        }
        std::sort(waveResults.begin(), waveResults.end(), [&](const auto& A, const auto& B) {
            return A.first < B.first;
        });
        for (auto& [idx, one] : waveResults) {
            static_cast<void>(idx);
            results.push_back(std::move(one));
        }
    }
    commitMs = std::chrono::duration_cast<std::chrono::milliseconds>(clock::now() - commitStart).count();

    const auto summaryStart = clock::now();
    const auto exitCode = PrintCommitSummary(workspaceRoot, results);
    summaryMs = std::chrono::duration_cast<std::chrono::milliseconds>(clock::now() - summaryStart).count();

    if (InProfile) {
        const auto totalMs = std::chrono::duration_cast<std::chrono::milliseconds>(clock::now() - totalStart).count();
        std::cout << "\n=== Commit Profile Summary ===\n";
        std::cout << "mode: native\n";
        std::cout << "repo_count: " << repoRecords.size() << "\n";
        std::cout << "preflight_ms: " << preflightMs << "\n";
        std::cout << "planning_ms: " << planningMs << "\n";
        std::cout << "commit_ms: " << commitMs << "\n";
        std::cout << "summary_ms: " << summaryMs << "\n";
        std::cout << "total_ms: " << totalMs << "\n";
    }

    return exitCode;
}

void RegisterCommit(CLI::App& InApp) {
    auto* cmd = InApp.add_subcommand("commit", "Native multi-repo commit workflow (pure C++, child-first for nested repos)");

    auto* repos = new std::string{};
    auto* repoRoot = new std::string{};
    auto* target = new std::string{};
    cmd->add_option("--repos", *repos, "Commit target repos (comma-separated). Default: auto-discover workspace repos");
    cmd->add_option("--repo-root", *repoRoot, "Workspace root/start path used for repo-name lookup");
    cmd->add_option("target", *target, "Optional repo target root (repo name or relative path)")->required(false);
    auto* bNoRecursive = new bool{false};
    cmd->add_flag("--no-recursive,-N", *bNoRecursive, "Commit only current repository when --repos is not provided");
    auto* bNoDirtyOnly = new bool{false};
    cmd->add_flag("--no-dirty-only", *bNoDirtyOnly, "Disable dirty-only pruning (default: dirty-only on)");
    auto* jobs = new std::string{"auto"};
    cmd->add_option("--jobs,-j", *jobs, "Parallel repo workers (auto|N)")->default_str("auto");
    auto* commitPlanFile = new std::string{};
    auto* planStage = new std::string{"commit"};
    cmd->add_option("--plan-file", *commitPlanFile, "Plan JSON file");
    cmd->add_option("--plan-stage", *planStage, "Plan stage: commit|post_sync|both")->default_str("commit");

    // Provider option
    auto* provider = new std::string{};
    cmd->add_option("--ai-provider", *provider, "AI provider (copilot, codex, opencode)")
        ->default_str("auto");

    // Model option
    auto* model = new std::string{};
    cmd->add_option("--ai-model", *model, "AI model to use");

    auto* bAiAuto = new bool{false};
    cmd->add_flag("--ai-auto", *bAiAuto, "Enable AI auto mode (provider auto + layered kog_config model selection)");

    // Message option
    auto* message = new std::string{};
    cmd->add_option("--message,-m", *message, "Commit message (skips AI generation)");

    // Agent proxy mode
    auto* agent = new std::string{};
    cmd->add_option("--agent", *agent, "Agent proxy mode (codex, copilot, cursor, kiro, claude)");

    // Flags
    auto* bPush = new bool{false};
    cmd->add_flag("--push", *bPush, "Push after commit");

    auto* bNoAiReview = new bool{false};
    cmd->add_flag("--no-ai-review", *bNoAiReview, "Skip AI review gate");

    auto* bStagedOnly = new bool{false};
    cmd->add_flag("--staged-only", *bStagedOnly, "Commit only already-staged changes (skip auto git add)");

    auto* bShell = new bool{false};
    cmd->add_flag("--shell", *bShell, "Deprecated compatibility flag (shell path removed)");

    auto* bPreflightOnly = new bool{false};
    cmd->add_flag("--preflight-only", *bPreflightOnly, "Run native preflight checks and exit without commit");

    auto* bNoNativePreflight = new bool{false};
    cmd->add_flag("--no-native-preflight", *bNoNativePreflight, "Skip native preflight checks before shell commit");

    auto* bProfile = new bool{false};
    cmd->add_flag("--profile", *bProfile, "Print native commit timing/profile summary");

    cmd->callback([=]() {
        using clock = std::chrono::steady_clock;
        const auto totalStart = clock::now();
        long long preflightMs = 0;
        long long planningMs = 0;
        long long commitMs = 0;
        long long summaryMs = 0;
        std::optional<CommitPreflightReport> cachedPreflightReport;

        if (*bShell) {
            std::cerr << "Error: --shell is no longer supported; commit workflow is fully native now\n";
            std::exit(2);
        }

        const auto invocationRoot = repoRoot->empty() ? std::filesystem::current_path() : std::filesystem::path(*repoRoot);
        const auto workspaceRoot = target->empty()
            ? invocationRoot.lexically_normal()
            : ResolveRepoPath(invocationRoot.lexically_normal(), std::filesystem::path(*target));

        if (!repos->empty() && !target->empty()) {
            std::cerr << "Error: positional target cannot be combined with --repos\n";
            std::exit(2);
        }

        if (!*bNoNativePreflight || *bPreflightOnly) {
            const auto preflightStart = clock::now();
            const auto report = RunCommitPreflight(workspaceRoot);
            cachedPreflightReport = report;
            PrintCommitPreflight(report, *bStagedOnly);
            if (!report.inRepo) {
                std::exit(1);
            }
            if (*bStagedOnly && report.stagedCount == 0) {
                std::cerr << "Preflight blocked: --staged-only but nothing staged\n";
                std::exit(2);
            }
            preflightMs = std::chrono::duration_cast<std::chrono::milliseconds>(clock::now() - preflightStart).count();
            if (*bPreflightOnly) {
                if (*bProfile) {
                    const auto totalMs = std::chrono::duration_cast<std::chrono::milliseconds>(clock::now() - totalStart).count();
                    std::cout << "\n=== Commit Profile Summary ===\n";
                    std::cout << "mode: native\n";
                    std::cout << "repo_count: 1\n";
                    std::cout << "preflight_ms: " << preflightMs << "\n";
                    std::cout << "planning_ms: 0\n";
                    std::cout << "commit_ms: 0\n";
                    std::cout << "summary_ms: 0\n";
                    std::cout << "total_ms: " << totalMs << "\n";
                }
                std::exit(0);
            }
        }

        NativeAiConfig ai;
        const bool aiRequested = *bAiAuto || !provider->empty() || !model->empty();
        ai.provider = aiRequested ? ResolveProvider(*provider) : std::string{};
        ai.model = aiRequested ? ResolveModelForAi(ai.provider, *model, *bAiAuto, workspaceRoot) : std::string{};
        ai.reviewEnabled = !*bNoAiReview;
        ai.enabled = aiRequested && !ai.provider.empty();

        if (!agent->empty()) {
            std::cout << "[native-commit] --agent is currently ignored in native mode.\n";
        }

        if (aiRequested && !ai.enabled) {
            std::cerr << "Error: AI mode requested, but provider is unavailable.\n";
            std::cerr << "- provider resolved: " << (ai.provider.empty() ? "<none>" : ai.provider) << "\n";
            std::cerr << "- model: " << (ai.model.empty() ? "<none>" : ai.model) << "\n";
            std::exit(2);
        }

        if (ai.enabled) {
            std::cout << "[native-commit] AI enabled: provider=" << ai.provider
                      << " model=" << ai.model
                      << " review=" << (ai.reviewEnabled ? "on" : "off") << "\n";
        }

        if (!aiRequested) {
            std::cout << "[native-commit] safety-gates: ignore + secret\n";
            RunPipelineSafetyGatesForNonAiCommit(workspaceRoot);
        }

        if (!commitPlanFile->empty()) {
            const auto report = cachedPreflightReport.has_value() ? *cachedPreflightReport : RunCommitPreflight(workspaceRoot);
            if (!HasAnyChanges(report)) {
                std::println("[native-commit] workspace clean; skipping --plan-file validation and commit.");
                if (*bProfile) {
                    const auto totalMs = std::chrono::duration_cast<std::chrono::milliseconds>(clock::now() - totalStart).count();
                    std::cout << "\n=== Commit Profile Summary ===\n";
                    std::cout << "mode: native\n";
                    std::cout << "repo_count: 0\n";
                    std::cout << "preflight_ms: " << preflightMs << "\n";
                    std::cout << "planning_ms: 0\n";
                    std::cout << "commit_ms: 0\n";
                    std::cout << "summary_ms: 0\n";
                    std::cout << "total_ms: " << totalMs << "\n";
                }
                std::exit(0);
            }
        }

        std::unordered_map<std::string, std::vector<RepoCommitPlanEntry::CommitItem>> stageMessages;
        if (!commitPlanFile->empty()) {
            if (!repos->empty()) {
                std::cerr << "Error: --plan-file cannot be combined with --repos\n";
                std::exit(2);
            }

            std::string planError;
            const auto normalizedCommitPlanPath = NormalizeInputPathForCurrentPlatform(*commitPlanFile);
            const auto parsed = ParseCommitPlan(std::filesystem::path(normalizedCommitPlanPath), &planError);
            if (!parsed.has_value()) {
                std::cerr << "Error: invalid --plan-file: " << normalizedCommitPlanPath;
                if (!planError.empty()) {
                    std::cerr << " (" << planError << ")";
                }
                std::cerr << "\n";
                std::exit(2);
            }

            std::string validationError;
            if (!ValidateCommitPlanForAiMode(*parsed, &validationError)) {
                std::cerr << "Error: invalid --plan-file: " << normalizedCommitPlanPath;
                if (!validationError.empty()) {
                    std::cerr << " (" << validationError << ")";
                }
                std::cerr << "\n";
                std::exit(2);
            }
            const auto currentBaseHeadSha = ComputeWorkspaceBaseHeadSha(workspaceRoot);
            const auto currentDirtyFingerprint = ComputeWorkspaceDirtyFingerprint(workspaceRoot);
            if (Trim(parsed->meta.baseHeadSha) != currentBaseHeadSha ||
                Trim(parsed->meta.dirtyFingerprint) != currentDirtyFingerprint) {
                std::cerr << "Error: invalid --plan-file: workspace state drift detected.\n";
                std::cerr << "  plan.path=" << normalizedCommitPlanPath << "\n";
                std::cerr << "  plan.base_head_sha=" << parsed->meta.baseHeadSha << "\n";
                std::cerr << "  current.base_head_sha=" << currentBaseHeadSha << "\n";
                std::cerr << "  plan.dirty_fingerprint=" << parsed->meta.dirtyFingerprint << "\n";
                std::cerr << "  current.dirty_fingerprint=" << currentDirtyFingerprint << "\n";
                std::cerr << "Hint: regenerate/refill plan before commit apply.\n";
                std::exit(2);
            }

            if (!parsed->meta.planner.provider.empty() ||
                !parsed->meta.planner.model.empty()) {
                std::cout << "[native-commit] plan meta: provider="
                          << (parsed->meta.planner.provider.empty() ? "<unset>" : parsed->meta.planner.provider)
                          << " ai-model="
                          << (parsed->meta.planner.model.empty() ? "<unset>" : parsed->meta.planner.model)
                          << "\n";
            }

            const auto stage = ParseCommitPlanStage(*planStage);
            if (!stage.has_value()) {
                std::cerr << "Error: invalid --plan-stage value: " << *planStage
                          << " (expected commit|post_sync|both)\n";
                std::exit(2);
            }

            stageMessages = BuildStageMessageMap(*parsed, *stage);
            if (stageMessages.empty()) {
                std::println("[native-commit] no entries found for selected --plan-stage; skipping commit.");
                if (*bProfile) {
                    const auto totalMs = std::chrono::duration_cast<std::chrono::milliseconds>(clock::now() - totalStart).count();
                    std::cout << "\n=== Commit Profile Summary ===\n";
                    std::cout << "mode: native\n";
                    std::cout << "repo_count: 0\n";
                    std::cout << "preflight_ms: " << preflightMs << "\n";
                    std::cout << "planning_ms: 0\n";
                    std::cout << "commit_ms: 0\n";
                    std::cout << "summary_ms: 0\n";
                    std::cout << "total_ms: " << totalMs << "\n";
                }
                std::exit(0);
            }
        }

        bool effectiveNoRecursive = *bNoRecursive;
        if (!effectiveNoRecursive && repos->empty() && !target->empty()) {
            const auto scopedRepos = DiscoverWorkspaceRepos(workspaceRoot);
            if (scopedRepos.size() <= 1) {
                effectiveNoRecursive = true;
            }
        }

        const auto planningStart = clock::now();
        const bool dirtyOnly = !*bNoDirtyOnly;
        auto reposCsv = Trim(*repos);
        if (!stageMessages.empty()) {
            std::string planReposCsv;
            for (const auto& [repoKey, items] : stageMessages) {
                if (items.empty()) {
                    continue;
                }
                if (!planReposCsv.empty()) {
                    planReposCsv += ",";
                }
                planReposCsv += repoKey;
            }
            reposCsv = std::move(planReposCsv);
        }
        auto repoRecords = BuildCommitScopeRecords(workspaceRoot, reposCsv, effectiveNoRecursive, dirtyOnly);
        if (repoRecords.empty()) {
            workspace::RepoRecord fallback;
            fallback.path = workspaceRoot;
            fallback.type = "root";
            repoRecords.push_back(std::move(fallback));
        }
        planningMs = std::chrono::duration_cast<std::chrono::milliseconds>(clock::now() - planningStart).count();

        const bool isPlanMode = !stageMessages.empty();
        const auto repoWaves = BuildExecutionWaves(repoRecords);
        const auto runbooks = BuildRepoCommitRunbooks(repoRecords, stageMessages, workspaceRoot, *message, isPlanMode);
        const auto taskGraph = BuildCommitTaskGraph(repoRecords, runbooks);
        const int workers = ResolveCommitJobs(*jobs, taskGraph.tasks.size(), ai.enabled);

        std::vector<RepoCommitResult> results;
        results.reserve(repoRecords.size() + taskGraph.tasks.size());
        for (const auto& runbook : runbooks) {
            if (runbook.valid) {
                continue;
            }
            RepoCommitResult failed;
            failed.repo = runbook.repo;
            failed.failed = true;
            failed.note = runbook.validationError;
            results.push_back(std::move(failed));
        }

        const auto commitStart = clock::now();
        std::cout << "[native-commit] plan: repos=" << repoRecords.size()
                  << " repo_waves=" << repoWaves.size()
                  << " commits=" << taskGraph.tasks.size()
                  << " commit_waves=" << taskGraph.waves.size()
                  << " jobs=" << workers
                  << " dirty_only=" << (dirtyOnly ? "on" : "off") << "\n";
        if (taskGraph.dependencyCycleDetected) {
            std::cout << "[native-commit] warning: dependency cycle detected in commit graph; downgraded to serial fallback order.\n";
        }

        auto executeCommitTask = [&](const CommitTaskNode& InNode) -> RepoCommitResult {
            const auto& repo = InNode.repo;
            const auto& repoMessage = InNode.commit;
            const bool needsPlanStaging =
                isPlanMode && (InNode.repoCommitCount > 1 || !repoMessage.include.empty() || !repoMessage.exclude.empty());
            if (needsPlanStaging) {
                std::string stageError;
                if (!StageCommitItemForPlan(repo, repoMessage, &stageError)) {
                    RepoCommitResult failed;
                    failed.repo = repo;
                    failed.failed = true;
                    failed.note = std::format("plan commit[{}] stage failed: {}", InNode.commitIndexInRepo, stageError);
                    return failed;
                }
            }
            return CommitSingleRepo(workspaceRoot,
                                    repo,
                                    repoMessage.message,
                                    needsPlanStaging ? true : *bStagedOnly,
                                    *bPush,
                                    ai);
        };

        for (const auto& wave : taskGraph.waves) {
            if (wave.empty()) {
                continue;
            }
            const int waveWorkers = std::max(1, std::min(workers, static_cast<int>(wave.size())));
            if (waveWorkers == 1) {
                for (const auto nodeIndex : wave) {
                    const auto& task = taskGraph.tasks[nodeIndex];
                    const auto label = DisplayRepoLabel(workspaceRoot, task.repo);
                    std::cout << "\n[commit] " << label
                              << " [" << (task.commitIndexInRepo + 1) << "/" << task.repoCommitCount << "]\n";
                    results.push_back(executeCommitTask(task));
                }
                continue;
            }

            std::vector<std::future<std::pair<std::size_t, RepoCommitResult>>> active;
            active.reserve(static_cast<std::size_t>(waveWorkers));
            std::size_t cursor = 0;
            std::vector<std::pair<std::size_t, RepoCommitResult>> waveResults;
            waveResults.reserve(wave.size());

            while (cursor < wave.size() || !active.empty()) {
                while (cursor < wave.size() && static_cast<int>(active.size()) < waveWorkers) {
                    const auto nodeIndex = wave[cursor++];
                    const auto& task = taskGraph.tasks[nodeIndex];
                    const auto label = DisplayRepoLabel(workspaceRoot, task.repo);
                    std::cout << "\n[commit] " << label
                              << " [" << (task.commitIndexInRepo + 1) << "/" << task.repoCommitCount << "]\n";
                    active.push_back(std::async(std::launch::async, [&, nodeIndex]() {
                        const auto one = executeCommitTask(taskGraph.tasks[nodeIndex]);
                        return std::make_pair(nodeIndex, one);
                    }));
                }

                if (!active.empty()) {
                    waveResults.push_back(active.front().get());
                    active.erase(active.begin());
                }
            }

            std::sort(waveResults.begin(), waveResults.end(), [&](const auto& A, const auto& B) {
                return A.first < B.first;
            });
            for (auto& [idx, one] : waveResults) {
                static_cast<void>(idx);
                results.push_back(std::move(one));
            }
        }
        commitMs = std::chrono::duration_cast<std::chrono::milliseconds>(clock::now() - commitStart).count();

        const auto summaryStart = clock::now();
        const auto exitCode = PrintCommitSummary(workspaceRoot, results);
        summaryMs = std::chrono::duration_cast<std::chrono::milliseconds>(clock::now() - summaryStart).count();

        if (*bProfile) {
            const auto totalMs = std::chrono::duration_cast<std::chrono::milliseconds>(clock::now() - totalStart).count();
            std::cout << "\n=== Commit Profile Summary ===\n";
            std::cout << "mode: native\n";
            std::cout << "repo_count: " << repoRecords.size() << "\n";
            std::cout << "preflight_ms: " << preflightMs << "\n";
            std::cout << "planning_ms: " << planningMs << "\n";
            std::cout << "commit_ms: " << commitMs << "\n";
            std::cout << "summary_ms: " << summaryMs << "\n";
            std::cout << "total_ms: " << totalMs << "\n";
        }

        std::exit(exitCode);
    });
}

void RegisterAmend(CLI::App& InApp) {
    auto* cmd = InApp.add_subcommand("amend", "Native amend workflow (default: amend previous commit)");

    auto* repos = new std::string{};
    cmd->add_option("--repos", *repos, "Amend target repos (comma-separated). Default: current repo only");

    auto* provider = new std::string{};
    cmd->add_option("--ai-provider", *provider, "AI provider (copilot, codex, opencode)")
        ->default_str("auto");

    auto* model = new std::string{};
    cmd->add_option("--ai-model", *model, "AI model to use");

    auto* bAiAuto = new bool{false};
    cmd->add_flag("--ai-auto", *bAiAuto, "Enable AI auto mode (provider auto + layered kog_config model selection)");

    auto* message = new std::string{};
    cmd->add_option("--message,-m", *message, "Amend commit message (skips AI generation)");

    auto* bNoAiReview = new bool{false};
    cmd->add_flag("--no-ai-review", *bNoAiReview, "Skip AI review gate");

    auto* bStagedOnly = new bool{false};
    cmd->add_flag("--staged-only", *bStagedOnly, "Amend only currently staged changes (skip auto git add)");

    auto* bCombineUnpushed = new bool{false};
    cmd->add_flag("--combine,--combine-unpushed,-U", *bCombineUnpushed, "Combine all local commits not pushed to upstream into one commit");

    cmd->callback([=]() {
        const auto workspaceRoot = std::filesystem::current_path();

        NativeAiConfig ai;
        const bool aiRequested = *bAiAuto || !provider->empty() || !model->empty();
        ai.provider = aiRequested ? ResolveProvider(*provider) : std::string{};
        ai.model = aiRequested ? ResolveModelForAi(ai.provider, *model, *bAiAuto, workspaceRoot) : std::string{};
        ai.reviewEnabled = !*bNoAiReview;
        ai.enabled = aiRequested && !ai.provider.empty();

        if (aiRequested && !ai.enabled) {
            std::cerr << "Error: AI mode requested, but provider is unavailable.\n";
            std::cerr << "- provider resolved: " << (ai.provider.empty() ? "<none>" : ai.provider) << "\n";
            std::cerr << "- model: " << (ai.model.empty() ? "<none>" : ai.model) << "\n";
            std::exit(2);
        }

        if (ai.enabled) {
            std::cout << "[native-amend] AI enabled: provider=" << ai.provider
                      << " model=" << ai.model
                      << " review=" << (ai.reviewEnabled ? "on" : "off") << "\n";
        }

        auto reposCsv = Trim(*repos);
        std::vector<std::filesystem::path> repoList;
        if (reposCsv.empty()) {
            repoList.push_back(workspaceRoot);
        } else {
            repoList = BuildOrderedRepoList(workspaceRoot, reposCsv);
            if (repoList.empty()) {
                repoList.push_back(workspaceRoot);
            }
        }

        std::vector<RepoAmendResult> results;
        results.reserve(repoList.size());

        for (const auto& repo : repoList) {
            const auto label = DisplayRepoLabel(workspaceRoot, repo);
            std::cout << "\n[amend] " << label << "\n";
            const auto one = AmendSingleRepo(workspaceRoot, repo, *message, *bStagedOnly, *bCombineUnpushed, ai);
            results.push_back(one);
        }

        const auto exitCode = PrintAmendSummary(workspaceRoot, results);
        std::exit(exitCode);
    });
}

} // namespace kano::git::commands
