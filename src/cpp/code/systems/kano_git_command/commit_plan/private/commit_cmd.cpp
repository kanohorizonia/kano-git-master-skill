// commit command ??Native multi-repo commit workflow (pure C++)

#include <CLI/CLI.hpp>
#include "discovery.hpp"
#include "shell_executor.hpp"
#include "auto_model_policy.hpp"
#include "kog_config.hpp"
#include "command_runtime_ops.hpp"
#include "secret_scan_utils.hpp"
#include "commit_ai_utils.hpp"
#include "ai_utils.hpp"
#include "plan_utils.hpp"
#include "terminal_color.hpp"
#include <kano_timing.h>

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

auto IsProbableIgnoreArtifactPath(const std::string& InPath) -> bool;
auto IsInternalPipelineArtifactPath(const std::string& InPath) -> bool;
auto ParseStatusChangedPath(const std::string& InLine) -> std::optional<std::string>;
auto RunAmendNativePlanStage(const std::filesystem::path& InWorkspaceRoot,
                              const std::string& InPlanFile,
                              const std::string& InPlanStage,
                              const bool InProfile) -> int;

auto IsKogDebugEnabled() -> bool;

namespace {

auto DisplayRepoLabel(const std::filesystem::path& InWorkspaceRoot, const std::filesystem::path& InRepo) -> std::string;
auto RunCommitPreflight(const std::filesystem::path& InRepo) -> CommitPreflightReport;

auto WarnPlanWorkspaceStateDrift(const std::string& InNormalizedCommitPlanPath,
                                 const std::string& InPlanBaseHeadSha,
                                 const std::string& InCurrentBaseHeadSha,
                                 const std::string& InPlanDirtyFingerprint,
                                 const std::string& InCurrentDirtyFingerprint) -> void {
    std::cerr << "Warning: invalid --plan-file: workspace state drift detected. Continuing anyway.\n";
    std::cerr << "  plan.path=" << InNormalizedCommitPlanPath << "\n";
    std::cerr << "  plan.base_head_sha=" << InPlanBaseHeadSha << "\n";
    std::cerr << "  current.base_head_sha=" << InCurrentBaseHeadSha << "\n";
    std::cerr << "  plan.dirty_fingerprint=" << InPlanDirtyFingerprint << "\n";
    std::cerr << "  current.dirty_fingerprint=" << InCurrentDirtyFingerprint << "\n";
    std::cerr << "Hint: regenerate/refill plan before commit apply.\n";
}

auto IsAgentProxyMode(const std::string& InAgent) -> bool {
    const auto normalized = ToLower(Trim(InAgent));
    return !normalized.empty() && normalized != "manual";
}

auto WasOptionExplicitlyPassed(const std::string& InLongFlag) -> bool {
#if defined(_WIN32)
    const int argc = *__p___argc();
    char** argv = *__p___argv();
    for (int idx = 1; idx < argc; ++idx) {
        const std::string arg = argv[idx] == nullptr ? "" : std::string(argv[idx]);
        if (arg == InLongFlag) {
            return true;
        }
        if (arg.rfind(InLongFlag + "=", 0) == 0) {
            return true;
        }
    }
#endif
    return false;
}

auto LoadOptionalCommitConventionSkillText(const std::filesystem::path& InWorkspaceRoot) -> std::optional<std::string> {
    std::vector<std::filesystem::path> candidates;
    candidates.emplace_back((InWorkspaceRoot / ".agents" / "kano" / "kano-commit-convention" / "SKILL.md").lexically_normal());
    candidates.emplace_back((ResolveSkillRoot(InWorkspaceRoot).parent_path() / "kano-commit-convention" / "SKILL.md").lexically_normal());
    if (const char* codexHome = std::getenv("CODEX_HOME"); codexHome != nullptr && std::string(codexHome).size() > 0) {
        const auto root = std::filesystem::path(codexHome).lexically_normal();
        candidates.emplace_back((root / "skills" / "kano-commit-convention" / "SKILL.md").lexically_normal());
        candidates.emplace_back((root / "skills" / ".system" / "kano-commit-convention" / "SKILL.md").lexically_normal());
    }
    if (const auto home = HomeDirectory(); !home.empty()) {
        candidates.emplace_back((home / ".codex" / "skills" / "kano-commit-convention" / "SKILL.md").lexically_normal());
        candidates.emplace_back((home / ".codex" / "skills" / ".system" / "kano-commit-convention" / "SKILL.md").lexically_normal());
        candidates.emplace_back((home / ".kano" / "skills" / "kano-commit-convention" / "SKILL.md").lexically_normal());
    }
    if (const char* custom = std::getenv("KOG_COMMIT_CONVENTION_SKILL_MD"); custom != nullptr && std::string(custom).size() > 0) {
        candidates.emplace_back(std::filesystem::path(custom).lexically_normal());
    }

    for (const auto& candidate : candidates) {
        if (std::error_code ec; std::filesystem::exists(candidate, ec) && !ec) {
            if (const auto text = ReadFileText(candidate); text.has_value()) {
                return *text;
            }
        }
    }
    return std::nullopt;
}

auto AppendCommitConventionSkillSection(const std::filesystem::path& InWorkspaceRoot,
                                        std::string InPrompt) -> std::string {
    const auto skillText = LoadOptionalCommitConventionSkillText(InWorkspaceRoot);
    if (!skillText.has_value()) {
        return InPrompt;
    }

    InPrompt += "\n\nFollow this commit convention skill when generating or reviewing the commit message:\n";
    InPrompt += "--- BEGIN kano-commit-convention/SKILL.md ---\n";
    InPrompt += *skillText;
    if (!InPrompt.empty() && InPrompt.back() != '\n') {
        InPrompt += '\n';
    }
    InPrompt += "--- END kano-commit-convention/SKILL.md ---\n";
    return InPrompt;
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

auto TrimTrailingWindowsPathChars(std::string InValue) -> std::string {
    while (!InValue.empty() && (InValue.back() == ' ' || InValue.back() == '.')) {
        InValue.pop_back();
    }
    return InValue;
}

auto IsWindowsReservedDeviceComponent(std::string InComponent) -> bool {
#if defined(_WIN32)
    InComponent = TrimTrailingWindowsPathChars(ToLower(Trim(std::move(InComponent))));
    if (InComponent.empty() || InComponent == "." || InComponent == "..") {
        return false;
    }

    const auto colon = InComponent.find(':');
    if (colon != std::string::npos) {
        InComponent = InComponent.substr(0, colon);
    }

    const auto dot = InComponent.find('.');
    const auto stem = dot == std::string::npos ? InComponent : InComponent.substr(0, dot);
    static const std::unordered_set<std::string> reserved = {
        "con", "prn", "aux", "nul",
        "com1", "com2", "com3", "com4", "com5", "com6", "com7", "com8", "com9",
        "lpt1", "lpt2", "lpt3", "lpt4", "lpt5", "lpt6", "lpt7", "lpt8", "lpt9"
    };
    return reserved.contains(stem);
#else
    (void)InComponent;
    return false;
#endif
}

auto PathHasWindowsReservedDeviceComponent(const std::string& InPath) -> bool {
#if defined(_WIN32)
    if (InPath.empty()) {
        return false;
    }

    std::string normalized = InPath;
    for (auto& ch : normalized) {
        if (ch == '\\') {
            ch = '/';
        }
    }

    std::size_t start = 0;
    while (start <= normalized.size()) {
        const auto end = normalized.find('/', start);
        auto component = end == std::string::npos ? normalized.substr(start) : normalized.substr(start, end - start);
        if (IsWindowsReservedDeviceComponent(component)) {
            return true;
        }
        if (end == std::string::npos) {
            break;
        }
        start = end + 1;
    }

    return false;
#else
    (void)InPath;
    return false;
#endif
}

auto IsInternalPipelineArtifactPathForStaging(const std::string& InPath) -> bool {
    auto path = InPath;
    std::replace(path.begin(), path.end(), '\\', '/');
    const auto lower = ToLower(path);
    auto matchDir = [&](const std::string& token) {
        if (lower == token) return true;
        if (lower.rfind(token + "/", 0) == 0) return true;
        if (lower.find("/" + token + "/") != std::string::npos) return true;
        if (lower.size() >= token.size() + 1 && lower.substr(lower.size() - token.size() - 1) == ("/" + token)) return true;
        return false;
    };
    return matchDir(".kano/tmp") || matchDir(".kano/cache") ||
           matchDir(".kano/launcher") || matchDir(".kano/reports") ||
           matchDir(".sisyphus/tmp") || matchDir(".sisyphus/cache");
}

auto BuildGitAddAllArgs(const CommitPreflightReport& InReport, std::vector<std::string>* OutExcluded = nullptr)
    -> std::vector<std::string> {
    std::vector<std::string> args{"add", "-A"};

    std::vector<std::string> excluded;
    std::set<std::string> seen;

#if defined(_WIN32)
    auto collectReserved = [&](const std::vector<std::string>& paths) {
        for (const auto& path : paths) {
            if (!PathHasWindowsReservedDeviceComponent(path)) continue;
            if (!seen.insert(path).second) continue;
            excluded.push_back(path);
        }
    };
    collectReserved(InReport.untrackedFiles);
    collectReserved(InReport.unstagedFiles);
    collectReserved(InReport.stagedFiles);
#endif

    // Always exclude internal pipeline artifacts on all platforms, but ONLY for untracked files.
    // If a user has already tracked/force-added a file, we respect that.
    auto collectArtifact = [&](const std::vector<std::string>& paths) {
        for (const auto& path : paths) {
            if (!IsInternalPipelineArtifactPathForStaging(path)) continue;
            if (!seen.insert(path).second) continue;
            excluded.push_back(path);
        }
    };
    collectArtifact(InReport.untrackedFiles);

    if (!excluded.empty()) {
        args.push_back("--");
        args.push_back(".");
        for (const auto& path : excluded) {
            args.push_back(std::string(":(exclude)") + path);
        }
    }

    if (OutExcluded != nullptr) {
        *OutExcluded = std::move(excluded);
    }
    return args;
}

auto RequireAiSuccessForCommitFlow(const std::filesystem::path& InWorkspaceRoot) -> bool {
    return kog_config::ReadEffectiveBool(InWorkspaceRoot,
                                         ResolveSkillRoot(InWorkspaceRoot),
                                         "plan.ai.require_success",
                                         false);
}

auto MaybeWarnAboutExcludedPaths(const std::filesystem::path& InRepo,
                                 const std::vector<std::string>& InExcluded) -> void {
    if (InExcluded.empty()) {
        return;
    }

    std::ostringstream stream;
    for (std::size_t index = 0; index < InExcluded.size(); ++index) {
        if (index > 0) {
            stream << ", ";
        }
        stream << InExcluded[index];
    }
    std::cerr << "[kog commit] warning: skipped internal artifacts or Windows reserved path(s) in "
              << InRepo.generic_string()
              << ": "
              << stream.str()
              << '\n';
}

auto IsGitRepo(const std::filesystem::path& InRepo) -> bool {
    const auto inside = GitCapture(InRepo, {"rev-parse", "--is-inside-work-tree"});
    if (inside.exitCode != 0 || Trim(inside.stdoutStr) != "true") {
        return false;
    }
    const auto topLevel = GitCapture(InRepo, {"rev-parse", "--show-toplevel"});
    if (topLevel.exitCode != 0) {
        return false;
    }
    std::error_code ec;
    const auto repoAbs = std::filesystem::weakly_canonical(InRepo, ec);
    const auto topAbs = std::filesystem::weakly_canonical(std::filesystem::path(Trim(topLevel.stdoutStr)), ec);
    return repoAbs == topAbs;
}

auto ResolveWorkspaceRootFromInvocation(const std::filesystem::path& InStartPath) -> std::filesystem::path {
    auto current = InStartPath.lexically_normal();
    if (current.empty()) {
        current = std::filesystem::current_path().lexically_normal();
    }

    if (!std::filesystem::is_directory(current)) {
        current = current.parent_path();
    }

    std::filesystem::path bestGitRoot;
    auto cursor = current;
    while (!cursor.empty()) {
        if (IsGitRepo(cursor)) {
            bestGitRoot = cursor;

            const auto hasSkillRepo = std::filesystem::exists((cursor / ".agents" / "kano" / "kano-git-master-skill").lexically_normal());
            const auto hasConventionSkill = std::filesystem::exists((cursor / ".agents" / "kano" / "kano-commit-convention" / "SKILL.md").lexically_normal());
            if (hasSkillRepo || hasConventionSkill) {
                return cursor.lexically_normal();
            }
        }

        const auto parent = cursor.parent_path();
        if (parent == cursor) {
            break;
        }
        cursor = parent;
    }

    if (!bestGitRoot.empty()) {
        return bestGitRoot.lexically_normal();
    }
    return current.lexically_normal();
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

void AppendRepeatableFlag(std::vector<std::string>* OutArgs,
                          const char* InEnvVar,
                          const std::string& InFlag);
void AppendSingleValueFlag(std::vector<std::string>* OutArgs,
                           const char* InEnvVar,
                           const std::string& InFlag);
void AppendBoolFlag(std::vector<std::string>* OutArgs,
                    const char* InEnvVar,
                    const std::string& InFlag);

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

auto CollectIgnoreGateCandidatePaths(const std::filesystem::path& InRepo) -> std::vector<std::string> {
    std::set<std::string> files;

    // Only check untracked files for the ignore gate.
    // Tracked modified files are legitimate changes and should not be blocked.
    if (const auto untracked = GitCapture(InRepo, {"ls-files", "--others", "--exclude-standard"}); untracked.exitCode == 0) {
        std::istringstream iss(untracked.stdoutStr);
        std::string line;
        while (std::getline(iss, line)) {
            auto path = Trim(line);
            if (!path.empty()) {
                files.insert(path);
            }
        }
    }

    return std::vector<std::string>(files.begin(), files.end());
}

auto ParseStatusChangedPath(const std::string& InLine) -> std::optional<std::string> {
    if (InLine.size() < 4) {
        return std::nullopt;
    }
    const char x = InLine[0];
    const char y = InLine[1];
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
                if (secret_scan::ShouldIgnoreSecretFinding(rule.id, line)) {
                    continue;
                }
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
            for (auto p : CollectIgnoreGateCandidatePaths(repo)) {
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

auto AiCacheDir() -> std::filesystem::path {
    const auto root = ResolveGlobalCacheRoot();
    if (root.empty()) {
        return {};
    }
    return (root / "ai").lexically_normal();
}

auto ExtractSingleLineMessage(const std::string& InText) -> std::string {
    // --- Strip leading Unicode bullet/symbol characters (multi-byte UTF-8) ---
    auto StripLeadingUnicodeBullets = [](std::string s) -> std::string {
        // Common AI-output bullet/symbol UTF-8 byte sequences:
        //   ??U+25CF (E2 97 8F)   ??U+25C6 (E2 97 86)   ??U+25B6 (E2 96 B6)
        //   ??U+25BA (E2 96 BA)   ??U+25CB (E2 97 8B)   ??U+25C9 (E2 97 89)
        //   ??U+2192 (E2 86 92)   ??U+2022 (E2 80 A2)   ??U+2023 (E2 80 A3)
        //   ??U+2713 (E2 9C 93)   ??U+2717 (E2 9C 97)   ??U+2726 (E2 9C A6)
        //   ??U+2605 (E2 98 85)   ??U+2606 (E2 98 86)
        // All are 3-byte sequences starting with 0xE2.
        static const std::string kBulletPrefixes[] = {
            "\xE2\x97\x8F", "\xE2\x97\x86", "\xE2\x96\xB6", "\xE2\x96\xBA",
            "\xE2\x97\x8B", "\xE2\x97\x89", "\xE2\x86\x92", "\xE2\x80\xA2",
            "\xE2\x80\xA3", "\xE2\x9C\x93", "\xE2\x9C\x97", "\xE2\x9C\xA6",
            "\xE2\x98\x85", "\xE2\x98\x86",
        };
        bool stripped = true;
        while (stripped && s.size() >= 3) {
            stripped = false;
            for (const auto& prefix : kBulletPrefixes) {
                if (s.rfind(prefix, 0) == 0) {
                    s.erase(0, prefix.size());
                    stripped = true;
                    break;
                }
            }
        }
        return s;
    };

    auto NormalizeAiLine = [&StripLeadingUnicodeBullets](std::string line) -> std::string {
        line = Trim(std::move(line));
        if (line.empty()) {
            return {};
        }

        if (line.rfind("- ", 0) == 0 || line.rfind("* ", 0) == 0) {
            line = Trim(line.substr(2));
        }

        // Strip leading Unicode bullet/symbol characters before ASCII-prefix cleanup.
        line = Trim(StripLeadingUnicodeBullets(std::move(line)));
        if (line.empty()) {
            return {};
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

    // --- Conventional Commits / Bracketed Tag detection ---
    // Matches: type(scope): msg, type: msg, type(scope)!: msg
    static const std::regex kConventionalRe(
        R"(^(feat|fix|refactor|chore|docs|test|ci|build|perf|style|revert)(\([^)]*\))?!?:\s+.+)",
        std::regex_constants::icase);
    // Matches: [Tag] msg, [Tag][SubTag] msg
    static const std::regex kBracketedRe(R"(^\[[A-Za-z][^\]]*\].+)");

    auto IsConventionalCommitLine = [&](const std::string& s) -> bool {
        return std::regex_match(s, kConventionalRe) || std::regex_match(s, kBracketedRe);
    };

    // --- AI preamble / conversational narration detector ---
    auto IsAiPreambleLine = [](const std::string& s) -> bool {
        // Case-insensitive prefix check for common AI narration patterns.
        auto lower = ToLower(s);
        static const std::string kPreamblePrefixes[] = {
            "reading",         "let me ",        "i'll ",         "i will ",
            "here's ",         "here is ",       "based on ",     "looking at ",
            "analyzing ",      "certainly",       "sure",          "of course",
            "the commit ",     "this commit ",    "i've ",         "i have ",
            "after reviewing", "after analyzing", "inspecting ",   "examining ",
        };
        for (const auto& prefix : kPreamblePrefixes) {
            if (lower.rfind(prefix, 0) == 0) {
                return true;
            }
        }
        // Also reject lines that look like AI internal status (e.g. "Reading the provider prompt file...")
        if (lower.find("provider prompt") != std::string::npos) {
            return true;
        }
        if (lower.find("commit message") != std::string::npos
            && (lower.find("reading") != std::string::npos || lower.find("generating") != std::string::npos
                || lower.find("creating") != std::string::npos)) {
            return true;
        }
        if (lower == "reading" || lower == "thinking" || lower == "analyzing" || lower == "processing") {
            return true;
        }
        return false;
    };

    // --- Two-pass extraction ---
    // Pass 1: collect all normalized non-empty, non-code-fence lines.
    std::vector<std::string> candidates;
    {
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
            candidates.push_back(std::move(line));
        }
    }

    if (candidates.empty()) {
        return {};
    }

    // Pass 2a: prefer a line matching Conventional Commits or Bracketed Tag format.
    for (const auto& c : candidates) {
        if (IsConventionalCommitLine(c)) {
            return c;
        }
    }

    // Pass 2b: fall back to first non-preamble line.
    for (const auto& c : candidates) {
        if (!IsAiPreambleLine(c)) {
            return c;
        }
    }

    // Pass 2c: no meaningful message found.
    return {};
}

auto RunAiGenerate(const std::string& InProvider,
                   const std::string& InModel,
                   const std::string& InPrompt,
                   std::optional<std::filesystem::path> InWorkingDir = std::nullopt,
                   const std::string& InPurpose = "commit-ai") -> shell::ExecResult {
    const auto effectivePrompt = BuildFileBackedPromptArgument(InWorkingDir, InPrompt, InPurpose);

    auto LogInvocation = [&](const std::string& binary, const std::vector<std::string>& args) {
        static constexpr std::string_view kDivider = "?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�";
        std::cerr << "\n[kog ai] ?�?� AI Invocation (" << InPurpose << ") ?�?�\n";
        std::cerr << "[kog ai] command : " << binary;
        for (const auto& a : args) {
            if (a.find(' ') != std::string::npos || a.empty()) std::cerr << " \"" << a << "\"";
            else std::cerr << " " << a;
        }
        std::cerr << "\n[kog ai] model   : " << (InModel.empty() ? "auto" : InModel) << "\n";
        if (IsTruthyEnv(std::getenv("KOG_DEBUG_AI_PROMPT")) || IsTruthyEnv(std::getenv("KOG_DEBUG"))) {
            std::cerr << "[kog ai] prompt  :\n" << kDivider << "\n" << InPrompt << "\n" << kDivider << "\n";
        }
        std::cerr << "[kog ai] Waiting for " << InProvider << " response...\n";
        std::cerr.flush();
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
        args.push_back(effectivePrompt);
        LogInvocation("opencode", args);
        return shell::ExecuteCommand("opencode", args, shell::ExecMode::Capture, InWorkingDir);
    }

    if (InProvider == "codex") {
        if (IsTruthyEnv(std::getenv("KOG_CODEX_USE_EXEC"))) {
            return ExecuteCodexExec(InWorkingDir, effectivePrompt, InPurpose, InModel);
        }
        std::vector<std::string> args;
        if (!InModel.empty() && InModel != "auto") {
            args = {"-q", "--model", InModel, effectivePrompt};
        } else {
            args = {"-q", effectivePrompt};
        }
        LogInvocation("codex", args);
        return shell::ExecuteCommand("codex", args, shell::ExecMode::Capture, InWorkingDir);
    }

    if (InProvider == "copilot") {
        if (HasCommand("copilot", {"--help"})) {
            std::vector<std::string> args{"-s", "-p", effectivePrompt};
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
            args.insert(args.end(), {"--no-color", "--stream", "off", "--no-ask-user"});
            LogInvocation("copilot", args);
            return shell::ExecuteCommand("copilot", args, shell::ExecMode::Capture, InWorkingDir);
        }

        if (HasCommand("gh", {"copilot", "--version"})) {
            std::vector<std::string> args{"copilot", "--", "-s", "-p", effectivePrompt};
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
            args.insert(args.end(), {"--no-color", "--stream", "off", "--no-ask-user"});
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

auto RepoKey(const std::filesystem::path& InPath) -> std::string {
    std::error_code ec;
    const auto canonical = std::filesystem::weakly_canonical(InPath, ec);
    const auto normalized = (ec ? InPath : canonical).lexically_normal().generic_string();
#if defined(_WIN32)
    return ToLower(normalized);
#else
    return normalized;
#endif
}

auto DiscoverRegisteredPathsRecursive(const std::filesystem::path& InWorkspaceRoot) -> std::vector<std::filesystem::path> {
    std::vector<std::filesystem::path> out;
    std::vector<std::filesystem::path> queue{std::filesystem::weakly_canonical(InWorkspaceRoot)};

    while (!queue.empty()) {
        const auto current = queue.back();
        queue.pop_back();

        const auto gitmodules = current / ".gitmodules";
        if (!std::filesystem::exists(gitmodules)) {
            continue;
        }

        const auto pathsResult = GitCapture(current, {"config", "-f", ".gitmodules", "--get-regexp", "^submodule\\..*\\.path$"});
        if (pathsResult.exitCode != 0) {
            continue;
        }

        std::istringstream iss(pathsResult.stdoutStr);
        std::string line;
        while (std::getline(iss, line)) {
            line = Trim(line);
            if (line.empty()) {
                continue;
            }
            const auto sp = line.find(' ');
            if (sp == std::string::npos || sp + 1 >= line.size()) {
                continue;
            }
            const auto relPath = line.substr(sp + 1);
            const auto full = std::filesystem::weakly_canonical(current / relPath).lexically_normal();
            const auto fullKey = RepoKey(full);
            const bool existsAlready = std::any_of(out.begin(), out.end(), [&](const auto& candidate) {
                return RepoKey(candidate) == fullKey;
            });
            if (!existsAlready) {
                out.push_back(full);
                queue.push_back(full);
            }
        }
    }

    std::sort(out.begin(), out.end(), [](const auto& A, const auto& B) {
        return RepoKey(A) < RepoKey(B);
    });
    out.erase(std::unique(out.begin(), out.end(), [](const auto& A, const auto& B) {
        return RepoKey(A) == RepoKey(B);
    }), out.end());
    return out;
}

auto NormalizePath(const std::filesystem::path& InPath) -> std::filesystem::path {
    return InPath.lexically_normal();
}

auto ToGeneric(const std::filesystem::path& InPath) -> std::string {
    return RepoKey(NormalizePath(InPath));
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
                return RepoKey(A.path) < RepoKey(B.path);
            });
            repos.erase(std::unique(repos.begin(), repos.end(), [](const auto& A, const auto& B) {
                return RepoKey(A.path) == RepoKey(B.path);
            }), repos.end());
            for (const auto& registeredPath : DiscoverRegisteredPathsRecursive(InRoot)) {
                const auto key = RepoKey(registeredPath);
                if (std::none_of(repos.begin(), repos.end(), [&](const auto& repo) {
                        return RepoKey(repo.path) == key;
                    })) {
                    workspace::RepoRecord fallback;
                    fallback.path = registeredPath;
                    fallback.type = "registered";
                    const auto status = GitCapture(registeredPath, {"status", "--porcelain"});
                    fallback.hasChanges = status.exitCode == 0 && !Trim(status.stdoutStr).empty();
                    repos.push_back(std::move(fallback));
                }
            }
            std::sort(repos.begin(), repos.end(), [](const auto& A, const auto& B) {
                return RepoKey(A.path) < RepoKey(B.path);
            });
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
    for (const auto& registeredPath : DiscoverRegisteredPathsRecursive(InRoot)) {
        const auto key = RepoKey(registeredPath);
        if (std::none_of(repos.begin(), repos.end(), [&](const auto& repo) {
                return RepoKey(repo.path) == key;
            })) {
            workspace::RepoRecord fallback;
            fallback.path = registeredPath;
            fallback.type = "registered";
            const auto status = GitCapture(registeredPath, {"status", "--porcelain"});
            fallback.hasChanges = status.exitCode == 0 && !Trim(status.stdoutStr).empty();
            repos.push_back(std::move(fallback));
        }
    }
    std::sort(repos.begin(), repos.end(), [](const auto& A, const auto& B) {
        return RepoKey(A.path) < RepoKey(B.path);
    });
    repos.erase(std::unique(repos.begin(), repos.end(), [](const auto& A, const auto& B) {
        return RepoKey(A.path) == RepoKey(B.path);
    }), repos.end());
    return repos;
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

auto JsonEscape(const std::string& InValue) -> std::string {
    std::string out;
    out.reserve(InValue.size() + 16);
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

auto CurrentUtcTimestampCompactForSyntheticPlan() -> std::string {
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

auto CurrentUtcTimestampIso8601ForSyntheticPlan() -> std::string {
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
                const auto message = CompactSingleLine(Trim(*messageField), 200);
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

auto PlanStageNeedsPreCommit(const CommitPlanStage InStage) -> bool {
    return InStage == CommitPlanStage::Commit || InStage == CommitPlanStage::Both;
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

auto DefaultSharedPlanPath(const std::filesystem::path& InWorkspaceRoot) -> std::filesystem::path {
    if (const char* explicitPlan = std::getenv("KOG_PLAN_FILE"); explicitPlan != nullptr && *explicitPlan != '\0') {
        return std::filesystem::path(explicitPlan).lexically_normal();
    }
    return (InWorkspaceRoot / ".kano" / "tmp" / "git" / "plans" / "default-plan.json").lexically_normal();
}

auto ResolveSelfBinaryCommand() -> std::string {
    if (const char* binaryPath = std::getenv("KANO_GIT_BINARY_PATH"); binaryPath != nullptr) {
        const std::filesystem::path p(binaryPath);
        if (std::filesystem::exists(p)) {
            return p.generic_string();
        }
    }
#if defined(_WIN32)
    // Try to find kano-git.exe in the same directory as the current executable
    char selfPath[MAX_PATH];
    if (GetModuleFileNameA(NULL, selfPath, MAX_PATH) > 0) {
        std::filesystem::path self(selfPath);
        std::filesystem::path candidate = self.parent_path() / "kano-git.exe";
        if (std::filesystem::exists(candidate)) {
            return candidate.generic_string();
        }
    }
    return "kano-git.exe";
#else
    return "kano-git";
#endif
}

void EmitCapturedSelfResult(const shell::ExecResult& InResult) {
    if (!InResult.stdoutStr.empty()) {
        std::cout << InResult.stdoutStr;
        if (InResult.stdoutStr.back() != '\n') {
            std::cout << '\n';
        }
    }
    if (!InResult.stderrStr.empty()) {
        std::cerr << InResult.stderrStr;
        if (InResult.stderrStr.back() != '\n') {
            std::cerr << '\n';
        }
    }
}

auto FinalizeNestedSelfResult(const char* InLabel, const shell::ExecResult& InResult) -> int {
    EmitCapturedSelfResult(InResult);
    const auto stderrLower = ToLower(InResult.stderrStr);
    if (stderrLower.find("find_binary: command not found") != std::string::npos) {
        std::cerr << "Error: " << InLabel
                  << " hit nested launcher shell failure (`find_binary` leaked into sh); aborting.\n";
        return InResult.exitCode != 0 ? InResult.exitCode : 127;
    }
    return InResult.exitCode;
}

struct CommitRunbookResult {
    int exitCode = 0;
    std::optional<long long> aiFillMillis;
    bool fallbackUsed = false;  // true if fallback commits were injected after AI fill failed
};

auto ExtractPlanAiFillMillis(const shell::ExecResult& InResult) -> std::optional<long long> {
    std::istringstream iss(InResult.stdoutStr + "\n" + InResult.stderrStr);
    std::string line;
    while (std::getline(iss, line)) {
        const auto trimmed = Trim(line);
        constexpr std::string_view kPrefix = "[plan] ai_fill_ms:";
        if (!trimmed.starts_with(kPrefix)) {
            continue;
        }
        const auto value = Trim(trimmed.substr(kPrefix.size()));
        if (value.empty() || value == "n/a") {
            return std::nullopt;
        }
        try {
            return std::stoll(value);
        } catch (const std::exception&) {
        }
    }
    return std::nullopt;
}

auto ExtractFallbackUsed(const shell::ExecResult& InResult) -> bool {
    const auto combined = InResult.stdoutStr + "\n" + InResult.stderrStr;
    return combined.find("[plan] fallback_used: true") != std::string::npos;
}

auto RunPlanNewViaSelf(const std::filesystem::path& InWorkspaceRoot,
                       const std::filesystem::path& InPlanPath) -> int {
    const auto selfCmd = ResolveSelfBinaryCommand();
    std::vector<std::string> args = {
        "plan", "new",
        "--force",
        "--output", InPlanPath.generic_string(),
    };
    if (IsKogDebugEnabled()) {
        std::cerr << "[DEBUG] RunPlanNewViaSelf: self=" << selfCmd << "\n";
        std::cerr << "[DEBUG] RunPlanNewViaSelf: workspace=" << InWorkspaceRoot.generic_string() << "\n";
        std::cerr << "[DEBUG] RunPlanNewViaSelf: plan_path=" << InPlanPath.generic_string() << "\n";
        for (std::size_t i = 0; i < args.size(); ++i) {
            std::cerr << "[DEBUG] RunPlanNewViaSelf: args[" << i << "]=" << args[i] << "\n";
        }
    }
    const auto result = shell::ExecuteCommand(selfCmd, args, shell::ExecMode::Capture, InWorkspaceRoot);
    if (IsKogDebugEnabled()) {
        std::cerr << "[DEBUG] RunPlanNewViaSelf: exitCode=" << result.exitCode << "\n";
        std::cerr << "[DEBUG] RunPlanNewViaSelf: stdout=" << result.stdoutStr << "\n";
        std::cerr << "[DEBUG] RunPlanNewViaSelf: stderr=" << result.stderrStr << "\n";
    }
    const auto exitCode = FinalizeNestedSelfResult("plan new", result);
    if (exitCode != 0) {
        std::cerr << "Error: plan new failed via native binary (exit=" << exitCode << ").\n";
    }
    return exitCode;
}

auto RunCommitSeedViaSelf(const std::filesystem::path& InWorkspaceRoot,
                          const std::filesystem::path& InPlanPath) -> int {
    std::vector<std::string> args = {
        "plan", "commit-seed",
        "--force",
        "--deterministic",  // Generate actual messages, not placeholders (needed for --combine mode)
        "--plan-file", InPlanPath.generic_string(),
    };
    const auto result = shell::ExecuteCommand(ResolveSelfBinaryCommand(), args, shell::ExecMode::Capture, InWorkspaceRoot);
    const auto exitCode = FinalizeNestedSelfResult("commit-seed", result);
    if (exitCode != 0) {
        std::cerr << "Error: commit-seed failed via native binary (exit=" << exitCode << ").\n";
    }
    return exitCode;
}

auto RunIgnorePlanRunbookViaSelf(const std::filesystem::path& InWorkspaceRoot,
                                 const std::filesystem::path& InPlanPath) -> int {
    if (IsKogDebugEnabled()) {
        std::cerr << "[DEBUG] RunIgnorePlanRunbookViaSelf ENTERED" << std::endl;
    }
    std::vector<std::string> args = {
        "plan", "runbook", "ignore",
        "--force",
        "--plan-file", InPlanPath.generic_string(),
    };
    const auto result = shell::ExecuteCommand(ResolveSelfBinaryCommand(), args, shell::ExecMode::Capture, InWorkspaceRoot);
    if (result.exitCode != 0) {
        const auto combinedOutput = result.stdoutStr + "\n" + result.stderrStr;
        
        static const std::regex driftRegex("state drift", std::regex_constants::icase);
        static const std::regex entriesRegex("ignore plan entries", std::regex_constants::icase);
        
        if (std::regex_search(combinedOutput, driftRegex) ||
            std::regex_search(combinedOutput, entriesRegex)) {
            EmitCapturedSelfResult(result);
            std::cout << "[native-commit] ignore runbook: no artifact candidates or plan already up-to-date; skipping.\n";
            return 0;
        }
    }
    const auto exitCode = FinalizeNestedSelfResult("ignore runbook", result);
    if (exitCode != 0) {
        std::cerr << "Error: ignore runbook failed via native binary (exit=" << exitCode << ").\n";
    }
    return exitCode;
}

auto RunIgnorePlanApplyViaSelf(const std::filesystem::path& InWorkspaceRoot,
                               const std::filesystem::path& InPlanPath) -> int {
    std::vector<std::string> args = {
        "plan", "apply", "--stage", "ignore",
        "--plan-file", InPlanPath.generic_string(),
    };
    const auto result = shell::ExecuteCommand(ResolveSelfBinaryCommand(), args, shell::ExecMode::Capture, InWorkspaceRoot);
    const auto combinedOutput = result.stdoutStr + "\n" + result.stderrStr;
    static const std::regex entriesRegex("ignore plan entries", std::regex_constants::icase);
    
    if (result.exitCode != 0 &&
        std::regex_search(combinedOutput, entriesRegex)) {
        EmitCapturedSelfResult(result);
        std::cout << "[native-commit] ignore plan stage is empty; skipping ignore apply.\n";
        return 0;
    }
    const auto exitCode = FinalizeNestedSelfResult("ignore apply", result);
    if (exitCode != 0) {
        std::cerr << "Error: ignore apply failed via native binary (exit=" << exitCode << ").\n";
    }
    return exitCode;
}

auto RunCommitPlanRunbookViaSelf(const std::filesystem::path& InWorkspaceRoot,
                                 const std::filesystem::path& InPlanPath,
                                 const std::string& InProvider,
                                 const std::string& InModel,
                                 const std::string& InFillMode,
                                 bool InAllowEmptyDirty,
                                 bool InYolo) -> CommitRunbookResult {
    std::vector<std::string> args = {
        "plan", "runbook", "commit",
        "--plan-file", InPlanPath.generic_string(),
        "--ai-provider", InProvider.empty() ? "auto" : InProvider,
    };
    if (!InModel.empty()) {
        args.push_back("--ai-model");
        args.push_back(InModel);
    }
    if (!InFillMode.empty()) {
        args.push_back("--ai-fill-mode");
        args.push_back(InFillMode);
    }
    if (InAllowEmptyDirty) {
        args.push_back("--allow-empty-dirty");
    }
    if (InYolo) {
        args.push_back("--yolo");
    }

    const auto binary = ResolveSelfBinaryCommand();
    std::cout << "[plan] invoking internal AI runbook: " << binary;
    for (const auto& a : args) {
        if (a.find(' ') != std::string::npos || a.empty()) std::cout << " \"" << a << "\"";
        else std::cout << " " << a;
    }
    std::cout << std::endl;

    const auto result = shell::ExecuteCommand(binary, args, shell::ExecMode::Capture, InWorkspaceRoot);
    CommitRunbookResult out;
    out.aiFillMillis = ExtractPlanAiFillMillis(result);
    out.fallbackUsed = ExtractFallbackUsed(result);
    const auto exitCode = FinalizeNestedSelfResult("AI commit runbook", result);
    out.exitCode = exitCode;
    if (exitCode != 0) {
        std::cerr << "Error: AI commit runbook failed via native binary (exit=" << exitCode << ").\n";
    }
    return out;
}

auto HumanAutoPlanLooksDeterministic(const std::filesystem::path& InPlanPath,
                                     std::string* OutReason) -> bool {
    const auto payload = ReadFileText(InPlanPath);
    if (!payload.has_value()) {
        if (OutReason != nullptr) {
            *OutReason = "cannot read plan file";
        }
        return true;
    }

    const auto meta = ExtractObjectBodyForKey(*payload, "meta");
    if (!meta.has_value()) {
        if (OutReason != nullptr) {
            *OutReason = "plan meta missing";
        }
        return true;
    }

    const auto planner = ExtractObjectBodyForKey(*meta, "planner");
    const auto provider = ToLower(planner.has_value() ? ExtractStringField(*planner, "provider").value_or("") : std::string{});
    const auto model = ToLower(planner.has_value() ? ExtractStringField(*planner, "ai-model").value_or("") : std::string{});

    const bool deterministicPlannerMeta = provider == "agent" ||
                                          model == "external-agent" ||
                                          model == "deterministic" ||
                                          provider == "native";
    if (deterministicPlannerMeta && OutReason != nullptr) {
        *OutReason = std::format("provider={} model={}", provider, model);
    }
    return deterministicPlannerMeta;
}

auto RunCommitAutoPlanPipeline(const std::filesystem::path& InWorkspaceRoot,
                               const NativeAiConfig& InAi,
                               const std::string& InAiFillMode,
                               const bool InProfile,
                               const bool InAllowEmptyDirty) -> int {
    using clock = std::chrono::steady_clock;
    const auto totalStart = std::chrono::steady_clock::now();
    long long planNewMillis = 0;
    long long ignoreRunbookMillis = 0;
    long long ignoreApplyMillis = 0;
    long long commitRunbookMillis = 0;
    long long preCommitMillis = 0;
    long long commitApplyMillis = 0;
    std::optional<long long> aiFillMillis;

    if (IsKogDebugEnabled()) {
        std::cerr << "[DEBUG] RunCommitAutoPlanPipeline ENTERED" << std::endl;
        std::cerr << "[DEBUG] InWorkspaceRoot=" << InWorkspaceRoot.generic_string() << std::endl;
        std::cerr << "[DEBUG] InAi.enabled=" << InAi.enabled << " provider=" << InAi.provider << " model=" << InAi.model << std::endl;
        std::cerr << "[DEBUG] InAiFillMode=" << InAiFillMode << std::endl;
        std::cerr << "[DEBUG] InProfile=" << InProfile << std::endl;
    }
    std::cerr.flush();

    const auto autoPlanPath = DefaultSharedPlanPath(InWorkspaceRoot);
    std::cout << "[native-commit] auto-plan file: " << autoPlanPath.generic_string() << "\n";

    const auto planNewStart = std::chrono::steady_clock::now();
    const auto planNewCode = RunPlanNewViaSelf(InWorkspaceRoot, autoPlanPath);
    planNewMillis = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - planNewStart).count();
    if (planNewCode != 0) {
        return planNewCode;
    }

    std::cout << "[plan] initializing ignore stages...\n";
    const auto ignoreRunbookStart = std::chrono::steady_clock::now();
    const auto ignoreRunbookCode = RunIgnorePlanRunbookViaSelf(InWorkspaceRoot, autoPlanPath);
    ignoreRunbookMillis = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - ignoreRunbookStart).count();
    if (ignoreRunbookCode != 0) {
        return ignoreRunbookCode;
    }

    std::cout << "[plan] applying ignore rules...\n";
    const auto ignoreApplyStart = std::chrono::steady_clock::now();
    const auto ignoreApplyCode = RunIgnorePlanApplyViaSelf(InWorkspaceRoot, autoPlanPath);
    ignoreApplyMillis = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - ignoreApplyStart).count();
    if (ignoreApplyCode != 0) {
        return ignoreApplyCode;
    }

    if (const auto seedCode = RunCommitSeedViaSelf(InWorkspaceRoot, autoPlanPath); seedCode != 0) {
        return seedCode;
    }

    CommitRunbookResult runbookResult;
    {
        double elapsed = 0.0;
        SCOPED_TIMING_LOG_WITH_ELAPSED("plan-utils.preparing-commit-plan-via-ai", elapsed);
        runbookResult = RunCommitPlanRunbookViaSelf(InWorkspaceRoot, autoPlanPath, InAi.provider, InAi.model, InAiFillMode, InAllowEmptyDirty, InAi.yolo);
        commitRunbookMillis = static_cast<long long>(elapsed);
        aiFillMillis = runbookResult.aiFillMillis;
        if (runbookResult.exitCode != 0) {
            return runbookResult.exitCode;
        }
    }

    std::string deterministicReason;
    // Skip deterministic check if fallback commits were used (fallback has provider=native model=deterministic metadata)
    if (!runbookResult.fallbackUsed && HumanAutoPlanLooksDeterministic(autoPlanPath, &deterministicReason)) {
        std::cerr << "Error: AI commit runbook produced non-AI deterministic plan metadata; refusing to continue.\n";
        std::cerr << "Hint: verify AI provider/auth and rerun plain `kog commit --ai-auto`.\n";
        std::cerr << "Hint: deterministic metadata: " << deterministicReason << "\n";
        return 2;
    }

    const auto preCommitStart = std::chrono::steady_clock::now();
    const auto preCommitCode = RunSyncPreCommitNative(InWorkspaceRoot, true, false, "default");
    preCommitMillis = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - preCommitStart).count();
    if (preCommitCode != 0) {
        return preCommitCode;
    }

    const auto commitApplyStart = std::chrono::steady_clock::now();
    const auto commitApplyCode = RunCommitNativePlanStage(InWorkspaceRoot, autoPlanPath.generic_string(), "commit", false);
    commitApplyMillis = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - commitApplyStart).count();
    if (commitApplyCode != 0) {
        return commitApplyCode;
    }

    if (InProfile) {
        const auto totalMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - totalStart).count();
        std::cout << "\n=== Commit Auto-Plan Profile Summary ===\n";
        std::cout << "mode: plan-first\n";
        std::cout << "plan_new_ms: " << planNewMillis << "\n";
        std::cout << "ignore_runbook_ms: " << ignoreRunbookMillis << "\n";
        std::cout << "ignore_apply_ms: " << ignoreApplyMillis << "\n";
        std::cout << "commit_runbook_ms: " << commitRunbookMillis << "\n";
        if (aiFillMillis.has_value()) {
            std::cout << "ai_fill_ms: " << *aiFillMillis << "\n";
        } else {
            std::cout << "ai_fill_ms: n/a\n";
        }
        std::cout << "pre_commit_ms: " << preCommitMillis << "\n";
        std::cout << "commit_apply_ms: " << commitApplyMillis << "\n";
        std::cout << "total_ms: " << totalMs << "\n";
    }

    return 0;
}

auto RunAmendAutoPlanPipeline(const std::filesystem::path& InWorkspaceRoot,
                               const NativeAiConfig& InAi,
                               const std::string& InAiFillMode,
                               const bool InProfile,
                               const bool InAllowEmptyDirty) -> int {
    using clock = std::chrono::steady_clock;
    const auto totalStart = std::chrono::steady_clock::now();
    long long planNewMillis = 0;
    long long ignoreRunbookMillis = 0;
    long long ignoreApplyMillis = 0;
    long long commitRunbookMillis = 0;
    long long amendApplyMillis = 0;
    std::optional<long long> aiFillMillis;

    if (IsKogDebugEnabled()) {
        std::cerr << "[DEBUG] RunAmendAutoPlanPipeline ENTERED" << std::endl;
        std::cerr << "[DEBUG] InWorkspaceRoot=" << InWorkspaceRoot.generic_string() << std::endl;
        std::cerr << "[DEBUG] InAi.enabled=" << InAi.enabled << " provider=" << InAi.provider << " model=" << InAi.model << std::endl;
        std::cerr << "[DEBUG] InAiFillMode=" << InAiFillMode << std::endl;
        std::cerr << "[DEBUG] InProfile=" << InProfile << std::endl;
    }
    std::cerr.flush();

    const auto autoPlanPath = DefaultSharedPlanPath(InWorkspaceRoot);
    std::cout << "[native-amend] auto-plan file: " << autoPlanPath.generic_string() << "\n";

    const auto planNewStart = std::chrono::steady_clock::now();
    const auto planNewCode = RunPlanNewViaSelf(InWorkspaceRoot, autoPlanPath);
    planNewMillis = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - planNewStart).count();
    if (planNewCode != 0) {
        return planNewCode;
    }

    std::cout << "[plan] initializing ignore stages...\n";
    const auto ignoreRunbookStart = std::chrono::steady_clock::now();
    const auto ignoreRunbookCode = RunIgnorePlanRunbookViaSelf(InWorkspaceRoot, autoPlanPath);
    ignoreRunbookMillis = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - ignoreRunbookStart).count();
    if (ignoreRunbookCode != 0) {
        return ignoreRunbookCode;
    }

    std::cout << "[plan] applying ignore rules...\n";
    const auto ignoreApplyStart = std::chrono::steady_clock::now();
    const auto ignoreApplyCode = RunIgnorePlanApplyViaSelf(InWorkspaceRoot, autoPlanPath);
    ignoreApplyMillis = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - ignoreApplyStart).count();
    if (ignoreApplyCode != 0) {
        return ignoreApplyCode;
    }

    if (const auto seedCode = RunCommitSeedViaSelf(InWorkspaceRoot, autoPlanPath); seedCode != 0) {
        return seedCode;
    }

    CommitRunbookResult runbookResult;
    {
        double elapsed = 0.0;
        SCOPED_TIMING_LOG_WITH_ELAPSED("plan-utils.preparing-commit-plan-via-ai", elapsed);
        runbookResult = RunCommitPlanRunbookViaSelf(InWorkspaceRoot, autoPlanPath, InAi.provider, InAi.model, InAiFillMode, InAllowEmptyDirty, InAi.yolo);
        commitRunbookMillis = static_cast<long long>(elapsed);
        aiFillMillis = runbookResult.aiFillMillis;
        if (runbookResult.exitCode != 0) {
            return runbookResult.exitCode;
        }
    }

    std::string deterministicReason;
    if (!runbookResult.fallbackUsed && HumanAutoPlanLooksDeterministic(autoPlanPath, &deterministicReason)) {
        std::cerr << "Error: AI commit runbook produced non-AI deterministic plan metadata; refusing to continue.\n";
        std::cerr << "Hint: verify AI provider/auth and rerun plain `kog amend --ai-auto`.\n";
        std::cerr << "Hint: deterministic metadata: " << deterministicReason << "\n";
        return 2;
    }

    // Apply plan via soft-reset + commit (rebuild history) instead of amend
    const auto amendApplyStart = std::chrono::steady_clock::now();
    const auto amendApplyCode = RunAmendNativePlanStage(InWorkspaceRoot, autoPlanPath.generic_string(), "commit", false);
    amendApplyMillis = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - amendApplyStart).count();
    if (amendApplyCode != 0) {
        return amendApplyCode;
    }

    if (InProfile) {
        const auto totalMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - totalStart).count();
        std::cout << "\n=== Amend Auto-Plan Profile Summary ===\n";
        std::cout << "mode: plan-first\n";
        std::cout << "plan_new_ms: " << planNewMillis << "\n";
        std::cout << "ignore_runbook_ms: " << ignoreRunbookMillis << "\n";
        std::cout << "ignore_apply_ms: " << ignoreApplyMillis << "\n";
        std::cout << "commit_runbook_ms: " << commitRunbookMillis << "\n";
        if (aiFillMillis.has_value()) {
            std::cout << "ai_fill_ms: " << *aiFillMillis << "\n";
        } else {
            std::cout << "ai_fill_ms: n/a\n";
        }
        std::cout << "amend_apply_ms: " << amendApplyMillis << "\n";
        std::cout << "total_ms: " << totalMs << "\n";
    }

    return 0;
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

auto StageCommitItemForPlan(const std::filesystem::path& InWorkspaceRoot,
                            const std::filesystem::path& InRepo,
                            const RepoCommitPlanEntry::CommitItem& InItem,
                            const std::unordered_set<std::string>& InPlanRepoKeys,
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

    // Auto-include dirty gitlinks if they correspond to submodules being committed in this plan
    const auto status = GitCapture(InRepo, {"status", "--porcelain"});
    if (status.exitCode == 0) {
        std::istringstream iss(status.stdoutStr);
        std::string line;
        while (std::getline(iss, line)) {
            if (auto p = ParseStatusChangedPath(line)) {
                const auto subPath = *p;
                if (IsGitlinkPathInHead(InRepo, subPath)) {
                    // Check if this submodule is part of the current workspace plan
                    const auto absSub = (InRepo / subPath).lexically_normal();
                    if (InPlanRepoKeys.contains(RepoKey(absSub))) {
                        // Check if it's already in include or exclude
                        bool alreadyHandled = false;
                        for (const auto& inc : InItem.include) {
                            if (inc == subPath) { alreadyHandled = true; break; }
                        }
                        if (!alreadyHandled) {
                            for (const auto& exc : InItem.exclude) {
                                if (exc == subPath || exc == (std::string(":(exclude)") + subPath)) {
                                    alreadyHandled = true; break;
                                }
                            }
                        }
                        if (!alreadyHandled) {
                            args.push_back(subPath);
                        }
                    }
                }
            }
        }
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
    // Gitlink-only: all changes are submodule pointer updates
    if (RepoHasGitlinkOnlyChanges(InRepo)) {
        const auto names = CollectChangedSubmoduleNames(InRepo);
        return BuildSubmoduleUpdateMessage(names);
    }

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
        std::vector<std::string> excludedReserved;
        const auto add = GitPassThrough(InRepo, BuildGitAddAllArgs(report, &excludedReserved));
        if (add.exitCode != 0) {
            result.failed = true;
            result.note = "git add -A failed";
            return result;
        }
        MaybeWarnAboutExcludedPaths(InRepo, excludedReserved);
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
            if (InAi.enabled) {
                result.failed = true;
                result.note = "ai message generation failed: " + aiFailureReason;
                return result;
            }
            commitMessage = BuildAutoCommitMessage(InWorkspaceRoot, InRepo, report);
        } else {
            result.note = "ai message generated";
        }
    }

    std::string reviewReason;
    if (!InAi.enabled && ShouldBlockByAiReview(InRepo, commitMessage, InAi, reviewReason)) {
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
            std::vector<std::string> excludedReserved;
            const auto add = GitPassThrough(InRepo, BuildGitAddAllArgs(report, &excludedReserved));
            if (add.exitCode != 0) {
                result.failed = true;
                result.note = "git add -A failed after combine reset";
                return result;
            }
            MaybeWarnAboutExcludedPaths(InRepo, excludedReserved);
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
                if (InAi.enabled) {
                    result.failed = true;
                    result.note = "ai message generation failed: " + aiFailureReason;
                    return result;
                }
                commitMessage = BuildCombineFallbackMessage(InWorkspaceRoot, InRepo, unpushedCount, report);
            } else {
                result.note = "combined with ai-generated message";
            }
        }

        std::string reviewReason;
        if (!InAi.enabled && ShouldBlockByAiReview(InRepo, commitMessage, InAi, reviewReason)) {
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
        result.commitTitle = HeadCommitTitle(InRepo);
        if (result.note.empty()) {
            result.note = "combined unpushed commits";
        }
        return result;
    }

    if (!InStagedOnly && (report.unstagedCount > 0 || report.untrackedCount > 0)) {
        std::vector<std::string> excludedReserved;
        const auto add = GitPassThrough(InRepo, BuildGitAddAllArgs(report, &excludedReserved));
        if (add.exitCode != 0) {
            result.failed = true;
            result.note = "git add -A failed";
            return result;
        }
        MaybeWarnAboutExcludedPaths(InRepo, excludedReserved);
        report = RunCommitPreflight(InRepo);
    }

    const auto headExists = GitCapture(InRepo, {"rev-parse", "--verify", "HEAD"});
    if (headExists.exitCode != 0) {
        result.failed = true;
        result.note = "amend requires at least one existing commit";
        return result;
    }

    std::string commitMessage = InMessage;
    if (commitMessage.empty() && InAi.enabled) {
        std::string aiFailureReason;
        commitMessage = GenerateAiCommitMessage(InWorkspaceRoot, InRepo, report, InAi, &aiFailureReason);
        if (commitMessage.empty()) {
            result.failed = true;
            result.note = "ai message generation failed: " + aiFailureReason;
            return result;
        } else {
            result.note = "amended with ai-generated message";
        }
    }

    if (!commitMessage.empty()) {
        std::string reviewReason;
        if (!InAi.enabled && ShouldBlockByAiReview(InRepo, commitMessage, InAi, reviewReason)) {
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
    result.commitTitle = HeadCommitTitle(InRepo);
    if (result.note.empty()) {
        result.note = "amended HEAD";
    }
    return result;
}

auto PrintCommitSummary(const std::filesystem::path& InWorkspaceRoot,
                        const std::vector<RepoCommitResult>& InResults,
                        long long InTotalMs) -> int {
    int failed = 0;
    int committed = 0;
    int pushed = 0;
    int skipped = 0;

    std::cout << "\n" << kano::terminal::PreflightHeader("Native Commit Summary") << "\n";

    struct CommitRow {
        std::string group;
        std::string repoName;
        std::string status;
        std::string detail;
        bool isCommitted = false;
    };

    std::vector<CommitRow> rows;
    for (const auto& item : InResults) {
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

        const auto relativePath = NormalizePath(item.repo).lexically_relative(NormalizePath(InWorkspaceRoot));
        const auto parent = relativePath.parent_path().generic_string();
        const std::string group = (parent.empty() || parent == ".") ? "." : parent;
        const auto repoName = NormalizePath(item.repo).filename().generic_string();
        const auto detail = item.commitTitle.empty() ? item.note : item.commitTitle;

        rows.push_back(CommitRow{group, repoName, status, detail, item.committed});
    }

    if (rows.empty()) {
        std::cout << "(no commits created)\n";
    } else {
        std::string currentGroup;
        for (const auto& row : rows) {
            if (row.group != currentGroup) {
                currentGroup = row.group;
                std::cout << "\nGROUP: " << currentGroup << "\n";
                std::cout << std::left << std::setw(28) << "Repo"
                          << std::setw(12) << "Result"
                          << "Detail\n";
                std::cout << std::left << std::setw(28) << "----"
                          << std::setw(12) << "------"
                          << "------\n";
            }
            std::cout << std::left << std::setw(28) << row.repoName
                      << std::setw(12) << row.status
                      << row.detail << "\n";
        }
    }

    std::cout << "\nTotals: committed=" << committed
              << " pushed=" << pushed
              << " skipped=" << skipped
              << " failed=" << failed << "\n";

    if (InTotalMs > 0) {
        char timeBuf[64];
        double elapsed = static_cast<double>(InTotalMs);
        if (elapsed < 1000.0) {
            std::snprintf(timeBuf, sizeof(timeBuf), "%.2fms", elapsed);
        } else if (elapsed < 60000.0) {
            std::snprintf(timeBuf, sizeof(timeBuf), "%.2fs", elapsed / 1000.0);
        } else if (elapsed < 3600000.0) {
            std::snprintf(timeBuf, sizeof(timeBuf), "%.2fm", elapsed / 60000.0);
        } else {
            std::snprintf(timeBuf, sizeof(timeBuf), "%.2fh", elapsed / 3600000.0);
        }
        std::cout << "Total elapsed time: " << timeBuf << "\n";
    }

    return failed == 0 ? 0 : 1;
}

auto PrintAmendSummary(const std::filesystem::path& InWorkspaceRoot,
                       const std::vector<RepoAmendResult>& InResults,
                       long long InTotalMs) -> int {
    int failed = 0;
    int amended = 0;
    int combined = 0;
    int skipped = 0;

    std::cout << "\n" << kano::terminal::PreflightHeader("Native Amend Summary") << "\n";
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

        const auto detail = item.commitTitle.empty() ? item.note : item.commitTitle;
        std::cout << std::left << std::setw(36) << repoLabel
                  << std::setw(12) << status
                  << detail << "\n";
    }

    std::cout << "\nTotals: amended=" << amended
              << " combined=" << combined
              << " skipped=" << skipped
              << " failed=" << failed << "\n";

    if (InTotalMs > 0) {
        char timeBuf[64];
        double elapsed = static_cast<double>(InTotalMs);
        if (elapsed < 1000.0) {
            std::snprintf(timeBuf, sizeof(timeBuf), "%.2fms", elapsed);
        } else if (elapsed < 60000.0) {
            std::snprintf(timeBuf, sizeof(timeBuf), "%.2fs", elapsed / 1000.0);
        } else if (elapsed < 3600000.0) {
            std::snprintf(timeBuf, sizeof(timeBuf), "%.2fm", elapsed / 60000.0);
        } else {
            std::snprintf(timeBuf, sizeof(timeBuf), "%.2fh", elapsed / 3600000.0);
        }
        std::cout << "Total elapsed time: " << timeBuf << "\n";
    }

    return failed == 0 ? 0 : 1;
}

auto PrintCommitPreflight(const CommitPreflightReport& InReport, bool InStagedOnly) -> void {
    std::cout << kano::terminal::PreflightHeader("Commit Preflight (native)") << "\n";
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
    const auto totalStart = std::chrono::steady_clock::now();
    long long preflightMs = 0;
    long long planningMs = 0;
    long long commitMs = 0;
    long long summaryMs = 0;

    const auto workspaceRoot = InWorkspaceRoot.lexically_normal();

    const auto preflightStart = std::chrono::steady_clock::now();
    const auto report = RunCommitPreflight(workspaceRoot);
    PrintCommitPreflight(report, false);
    if (!report.inRepo) {
        return 1;
    }
    preflightMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - preflightStart).count();

    const auto disableSecretGate = [&]() {
        const auto* value = std::getenv("KOG_DISABLE_SECRET_GATE");
        if (value == nullptr) {
            return false;
        }
        const auto normalized = ToLower(Trim(std::string(value)));
        return normalized == "1" || normalized == "true" || normalized == "yes" || normalized == "on";
    }();
    if (disableSecretGate) {
        std::cout << "[native-commit] safety-gates: ignore only (secret gate disabled)\n";
        const auto allowIgnoreGate = [&]() {
            const auto* value = std::getenv("KOG_ALLOW_IGNORE_GATE");
            if (value == nullptr) {
                return false;
            }
            const auto normalized = ToLower(Trim(std::string(value)));
            return normalized == "1" || normalized == "true" || normalized == "yes" || normalized == "on";
        }();
        if (!allowIgnoreGate &&
            ToLower(Trim(std::getenv("KOG_IGNORE_GATE") == nullptr ? "on" : std::getenv("KOG_IGNORE_GATE"))) != "off") {
            auto repos = DiscoverWorkspaceRepos(workspaceRoot);
            if (repos.empty()) {
                repos.push_back(workspaceRoot);
            }
            const auto allowlistPath = (ResolveSkillRoot(workspaceRoot) / "assets" / "ignore-sources" / "local" / "ignore-gate-allowlist.txt").lexically_normal();
            const auto allowlist = LoadNormalizedLineSet(allowlistPath);
            std::vector<std::string> findings;
            for (const auto& repo : repos) {
                const auto rel = repo.lexically_relative(workspaceRoot).generic_string();
                const auto repoLabel = rel.empty() ? "." : rel;
                for (auto p : CollectIgnoreGateCandidatePaths(repo)) {
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
    } else {
        std::cout << "[native-commit] safety-gates: ignore + secret\n";
        RunPipelineSafetyGatesForNonAiCommit(workspaceRoot);
    }

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
        WarnPlanWorkspaceStateDrift(normalizedCommitPlanPath,
                                    parsed->meta.baseHeadSha,
                                    currentBaseHeadSha,
                                    parsed->meta.dirtyFingerprint,
                                    currentDirtyFingerprint);
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
            const auto totalMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - totalStart).count();
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

    const auto planningStart = std::chrono::steady_clock::now();
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
    planningMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - planningStart).count();

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

    const auto commitStart = std::chrono::steady_clock::now();
    std::cout << "[native-commit] plan: repos=" << repoRecords.size()
              << " repo_waves=" << repoWaves.size()
              << " commits=" << taskGraph.tasks.size()
              << " commit_waves=" << taskGraph.waves.size()
              << " jobs=" << workers
              << " dirty_only=on\n";
    if (taskGraph.dependencyCycleDetected) {
        std::cout << "[native-commit] warning: dependency cycle detected in commit graph; downgraded to serial fallback order.\n";
    }

        std::unordered_set<std::string> planRepoKeys;
        for (const auto& rec : repoRecords) {
            planRepoKeys.insert(kano::git::commands::RepoKey(rec.path));
        }

        auto executeCommitTask = [&](const CommitTaskNode& InNode) -> RepoCommitResult {
            const auto& repo = InNode.repo;
            const auto& repoMessage = InNode.commit;
            const bool needsPlanStaging =
                InNode.repoCommitCount > 1 || !repoMessage.include.empty() || !repoMessage.exclude.empty();
            if (needsPlanStaging) {
                std::string stageError;
                if (!StageCommitItemForPlan(workspaceRoot, repo, repoMessage, planRepoKeys, &stageError)) {
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
    commitMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - commitStart).count();

    const auto totalElapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - totalStart).count();
    const auto summaryStart = std::chrono::steady_clock::now();
    const auto exitCode = PrintCommitSummary(workspaceRoot, results, totalElapsedMs);
    summaryMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - summaryStart).count();

    if (InProfile) {
        const auto totalMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - totalStart).count();
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

auto RunAmendNativePlanStage(const std::filesystem::path& InWorkspaceRoot,
                              const std::string& InPlanFile,
                              const std::string& InPlanStage,
                              const bool InProfile) -> int {
    using clock = std::chrono::steady_clock;
    const auto totalStart = std::chrono::steady_clock::now();
    long long preflightMs = 0;
    long long planningMs = 0;
    long long amendMs = 0;
    long long summaryMs = 0;

    const auto workspaceRoot = InWorkspaceRoot.lexically_normal();

    const auto preflightStart = std::chrono::steady_clock::now();
    const auto report = RunCommitPreflight(workspaceRoot);
    PrintCommitPreflight(report, false);
    if (!report.inRepo) {
        return 1;
    }
    preflightMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - preflightStart).count();

    std::cout << "[native-amend] safety-gates: ignore + secret\n";
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

    const auto stage = ParseCommitPlanStage(InPlanStage);
    if (!stage.has_value()) {
        std::cerr << "Error: invalid --plan-stage value: " << InPlanStage
                  << " (expected commit|post_sync|both)\n";
        return 2;
    }

    auto stageMessages = BuildStageMessageMap(*parsed, *stage);
    if (stageMessages.empty()) {
        std::println("[native-amend] no entries found for selected --plan-stage; skipping amend.");
        if (InProfile) {
            const auto totalMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - totalStart).count();
            std::cout << "\n=== Amend Profile Summary ===\n";
            std::cout << "mode: native\n";
            std::cout << "repo_count: 0\n";
            std::cout << "preflight_ms: " << preflightMs << "\n";
            std::cout << "planning_ms: 0\n";
            std::cout << "amend_ms: 0\n";
            std::cout << "summary_ms: 0\n";
            std::cout << "total_ms: " << totalMs << "\n";
        }
        return 0;
    }

    const auto planningStart = std::chrono::steady_clock::now();
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
    planningMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - planningStart).count();

    const auto repoWaves = BuildExecutionWaves(repoRecords);
    const auto runbooks = BuildRepoCommitRunbooks(repoRecords, stageMessages, workspaceRoot, "", true);
    const auto taskGraph = BuildCommitTaskGraph(repoRecords, runbooks);
    const int workers = ResolveCommitJobs("auto", taskGraph.tasks.size(), false);

    std::vector<RepoAmendResult> results;
    results.reserve(repoRecords.size() + taskGraph.tasks.size());
    for (const auto& runbook : runbooks) {
        if (runbook.valid) {
            continue;
        }
        RepoAmendResult failed;
        failed.repo = runbook.repo;
        failed.failed = true;
        failed.note = runbook.validationError;
        results.push_back(std::move(failed));
    }

    NativeAiConfig ai{};
    ai.enabled = false;
    ai.reviewEnabled = false;

    const auto amendStart = std::chrono::steady_clock::now();
    std::cout << "[native-amend] plan: repos=" << repoRecords.size()
              << " repo_waves=" << repoWaves.size()
              << " amends=" << taskGraph.tasks.size()
              << " amend_waves=" << taskGraph.waves.size()
              << " jobs=" << workers
              << " dirty_only=on\n";
    if (taskGraph.dependencyCycleDetected) {
        std::cout << "[native-amend] warning: dependency cycle detected in amend graph; downgraded to serial fallback order.\n";
    }

    auto executeAmendTask = [&](const CommitTaskNode& InNode) -> RepoAmendResult {
        const auto& repo = InNode.repo;
        const auto& repoMessage = InNode.commit;

        RepoAmendResult result;
        result.repo = repo;

        // Soft reset to parent of HEAD to "undo" the last commit
        const auto headExists = GitCapture(repo, {"rev-parse", "--verify", "HEAD"});
        if (headExists.exitCode != 0) {
            result.failed = true;
            result.note = "amend requires at least one existing commit";
            return result;
        }

        // Get the upstream ref for combining unpushed commits
        const auto upstream = ResolveUpstreamRef(repo);
        const auto resetTarget = upstream.empty() ? "HEAD~1" : upstream;

        const auto softReset = GitPassThrough(repo, {"reset", "--soft", resetTarget});
        if (softReset.exitCode != 0) {
            result.failed = true;
            result.note = "git reset --soft failed";
            return result;
        }

        // Stage all changes
        std::vector<std::string> excludedReserved;
        const auto add = GitPassThrough(repo, BuildGitAddAllArgs(RunCommitPreflight(repo), &excludedReserved));
        if (add.exitCode != 0) {
            result.failed = true;
            result.note = "git add -A failed after reset";
            return result;
        }
        MaybeWarnAboutExcludedPaths(repo, excludedReserved);

        const auto stagedReport = RunCommitPreflight(repo);
        if (stagedReport.stagedCount == 0) {
            result.note = "no staged content after amend preparation";
            return result;
        }

        // Commit with the plan message
        const auto commitMessage = repoMessage.message;
        if (commitMessage.empty()) {
            result.failed = true;
            result.note = "plan message is empty";
            return result;
        }

        const auto commit = GitPassThrough(repo, {"commit", "-m", commitMessage});
        if (commit.exitCode != 0) {
            result.failed = true;
            result.note = "git commit failed after reset";
            return result;
        }

        result.amended = true;
        result.note = "amended via plan";
        result.commitTitle = CompactSingleLine(commitMessage, 200);
        return result;
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
                std::cout << "\n[amend] " << label
                          << " [" << (task.commitIndexInRepo + 1) << "/" << task.repoCommitCount << "]\n";
                results.push_back(executeAmendTask(task));
            }
            continue;
        }

        std::vector<std::future<std::pair<std::size_t, RepoAmendResult>>> active;
        active.reserve(static_cast<std::size_t>(waveWorkers));
        std::size_t cursor = 0;
        std::vector<std::pair<std::size_t, RepoAmendResult>> waveResults;
        waveResults.reserve(wave.size());

        while (cursor < wave.size() || !active.empty()) {
            while (cursor < wave.size() && static_cast<int>(active.size()) < waveWorkers) {
                const auto nodeIndex = wave[cursor++];
                const auto& task = taskGraph.tasks[nodeIndex];
                const auto label = DisplayRepoLabel(workspaceRoot, task.repo);
                std::cout << "\n[amend] " << label
                          << " [" << (task.commitIndexInRepo + 1) << "/" << task.repoCommitCount << "]\n";
                active.push_back(std::async(std::launch::async, [&, nodeIndex]() {
                    const auto one = executeAmendTask(taskGraph.tasks[nodeIndex]);
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
    amendMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - amendStart).count();

    const auto totalElapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - totalStart).count();
    const auto summaryStart = std::chrono::steady_clock::now();
    const auto exitCode = PrintAmendSummary(workspaceRoot, results, totalElapsedMs);
    summaryMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - summaryStart).count();

    if (InProfile) {
        const auto totalMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - totalStart).count();
        std::cout << "\n=== Amend Profile Summary ===\n";
        std::cout << "mode: native\n";
        std::cout << "repo_count: " << repoRecords.size() << "\n";
        std::cout << "preflight_ms: " << preflightMs << "\n";
        std::cout << "planning_ms: " << planningMs << "\n";
        std::cout << "amend_ms: " << amendMs << "\n";
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
    const auto totalStart = std::chrono::steady_clock::now();
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
    preflightMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - totalStart).count();

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

    const auto planningStart = std::chrono::steady_clock::now();
    const bool dirtyOnly = true;
    auto repoRecords = BuildCommitScopeRecords(workspaceRoot, Trim(InReposCsv), InNoRecursive, dirtyOnly);
    if (repoRecords.empty()) {
        workspace::RepoRecord fallback;
        fallback.path = workspaceRoot;
        fallback.type = "root";
        repoRecords.push_back(std::move(fallback));
    }
    planningMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - planningStart).count();

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

    const auto commitStart = std::chrono::steady_clock::now();
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
    commitMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - commitStart).count();

    const auto totalElapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - totalStart).count();
    const auto summaryStart = std::chrono::steady_clock::now();
    const auto exitCode = PrintCommitSummary(workspaceRoot, results, totalElapsedMs);
    summaryMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - summaryStart).count();

    if (InProfile) {
        const auto totalMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - totalStart).count();
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
    auto* cmdAiAlias = InApp.add_subcommand("ca", "Alias of commit --ai-auto");

    auto* repos = new std::string{};
    auto* repoRoot = new std::string{};
    auto* target = new std::string{};
    auto* bNoRecursive = new bool{false};
    auto* bNoDirtyOnly = new bool{false};
    auto* jobs = new std::string{"auto"};
    auto* commitPlanFile = new std::string{};
    auto* planStage = new std::string{"commit"};
    auto* provider = new std::string{};
    auto* model = new std::string{};
    auto* bAiAuto = new bool{false};
    auto* aiFillMode = new std::string{};
    auto* message = new std::string{};
    auto* agent = new std::string{};
    auto* bPush = new bool{false};
    auto* bNoAiReview = new bool{false};
    auto* bStagedOnly = new bool{false};
    auto* bShell = new bool{false};
    auto* bPreflightOnly = new bool{false};
    auto* bNoNativePreflight = new bool{false};
    auto* bProfile = new bool{false};
    auto* bAllowEmptyDirty = new bool{false};
    auto* bYolo = new bool{false};

    auto configure = [&](CLI::App* InCmd) {
        InCmd->add_option("--repos", *repos, "Commit target repos (comma-separated). Default: auto-discover workspace repos");
        InCmd->add_option("--repo-root", *repoRoot, "Workspace root/start path used for repo-name lookup");
        InCmd->add_option("target", *target, "Optional repo target root (repo name or relative path)")->required(false);
        InCmd->add_flag("--no-recursive,-N", *bNoRecursive, "Commit only current repository when --repos is not provided");
        InCmd->add_flag("--no-dirty-only", *bNoDirtyOnly, "Disable dirty-only pruning (default: dirty-only on)");
        InCmd->add_option("--jobs,-j", *jobs, "Parallel repo workers (auto|N)")->default_str("auto");
        InCmd->add_option("--plan-file", *commitPlanFile, "Plan JSON file");
        InCmd->add_option("--plan-stage", *planStage, "Plan stage: commit|post_sync|both")->default_str("commit");
        InCmd->add_option("--ai-provider", *provider, "AI provider (copilot, codex, opencode)")->default_str("auto");
        InCmd->add_option("--ai-model", *model, "AI model to use");
        InCmd->add_flag("--ai-auto", *bAiAuto, "Enable AI auto mode (provider auto + layered kog_config model selection)");
        InCmd->add_option("--ai-commit-generation-mode,--ai-fill-mode", *aiFillMode, "AI commit generation mode override (single|per-commit|adaptive)");
        InCmd->add_option("--message,-m", *message, "Commit message (synthesizes minimal commit plan; skips AI generation)");
        InCmd->add_option("--agent", *agent, "Agent proxy mode (codex, copilot, cursor, kiro, claude)");
        InCmd->add_flag("--push", *bPush, "Push after commit");
        InCmd->add_flag("--no-ai-review", *bNoAiReview, "Skip AI review gate");
        InCmd->add_flag("--staged-only", *bStagedOnly, "Commit only already-staged changes (skip auto git add)");
        InCmd->add_flag("--shell", *bShell, "Deprecated compatibility flag (shell path removed)");
        InCmd->add_flag("--preflight-only", *bPreflightOnly, "Run native preflight checks and exit without commit");
        InCmd->add_flag("--no-native-preflight", *bNoNativePreflight, "Skip native preflight checks before shell commit");
        InCmd->add_flag("--profile", *bProfile, "Print native commit timing/profile summary");
        InCmd->add_flag("--allow-empty-dirty", *bAllowEmptyDirty, "Allow AI plan-fill to run even when workspace dirty context is empty");
        InCmd->add_flag("--yolo", *bYolo, "Enable all permissions for AI sub-agents (Option A: direct file editing)");
    };

    configure(cmd);
    configure(cmdAiAlias);

    auto run = [=, &InApp]() {
        using clock = std::chrono::steady_clock;
        const auto totalStart = std::chrono::steady_clock::now();
        long long preflightMs = 0;
        long long planningMs = 0;
        long long commitMs = 0;
        long long summaryMs = 0;
        std::optional<CommitPreflightReport> cachedPreflightReport;

        if (IsKogDebugEnabled()) {
            std::cerr << "[DEBUG] run lambda ENTERED" << std::endl;
        }
        std::cerr.flush();

        if (InApp.got_subcommand(cmdAiAlias)) {
            *bAiAuto = true;
        }

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
        if (!commitPlanFile->empty() && !message->empty()) {
            std::cerr << "Error: --plan-file cannot be combined with --message/-m\n";
            std::cerr << "Hint: use --plan-file for plan-driven commit, or use -m to synthesize a minimal plan.\n";
            std::exit(2);
        }

        if (!*bNoNativePreflight || *bPreflightOnly) {
            const auto preflightStart = std::chrono::steady_clock::now();
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
            preflightMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - preflightStart).count();
            if (*bPreflightOnly) {
                if (*bProfile) {
                    const auto totalMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - totalStart).count();
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
        const bool agentProxyMode = IsAgentProxyMode(*agent);
        const bool aiRequested = *bAiAuto || !provider->empty() || !model->empty();
        ai.provider = aiRequested ? ResolveProvider(*provider) : std::string{};
        ai.model = aiRequested ? ResolveModelForAi(ai.provider, *model, *bAiAuto, workspaceRoot) : std::string{};
        ai.reviewEnabled = !*bNoAiReview && !agentProxyMode;
        ai.yolo = *bYolo;
        ai.enabled = aiRequested && !ai.provider.empty();

        if (agentProxyMode && commitPlanFile->empty() && message->empty()) {
            std::cerr << "Error: agent proxy mode commit requires either --plan-file or --message/-m.\n";
            std::cerr << "Hint: prepare/fill plan first, then run with --plan-file.\n";
            std::cerr << "Hint: or provide explicit --message/-m for non-plan commit apply.\n";
            std::exit(2);
        }

        if (agentProxyMode) {
            std::cout << "[native-commit] agent proxy mode: agent=" << *agent
                      << " review=off\n";
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

        const auto defaultSharedPlanPath = DefaultSharedPlanPath(workspaceRoot).lexically_normal();
        const auto currentPlanPath = commitPlanFile->empty() ? std::filesystem::path{} : std::filesystem::path(*commitPlanFile).lexically_normal();
        const bool usingDefaultSharedPlanPath = !commitPlanFile->empty() && currentPlanPath == defaultSharedPlanPath;
        const bool explicitPlanFileRequested = !commitPlanFile->empty() && !usingDefaultSharedPlanPath;
        if (IsKogDebugEnabled()) {
            std::cerr << "[DEBUG] BEFORE CLEAR: explicitPlanFileRequested=" << explicitPlanFileRequested
                      << " commitPlanFile=[" << *commitPlanFile << "]" << std::endl;
        }
        if (usingDefaultSharedPlanPath && ai.enabled && message->empty()) {
            if (IsKogDebugEnabled()) {
                std::cerr << "[DEBUG] AI auto mode ignoring cached default shared plan path" << std::endl;
            }
            commitPlanFile->clear();
        } else if (explicitPlanFileRequested && !std::filesystem::exists(currentPlanPath)) {
            if (IsKogDebugEnabled()) {
                std::cerr << "[DEBUG] explicit plan file doesn't exist, clearing commitPlanFile" << std::endl;
            }
            commitPlanFile->clear();
        }
        if (IsKogDebugEnabled()) {
            std::cerr << "[DEBUG] autoPlanAiMode check: ai.enabled=" << ai.enabled 
                      << " commitPlanFile=[" << *commitPlanFile << "]" << std::endl;
        }
        const bool autoPlanAiMode = ai.enabled && message->empty() && commitPlanFile->empty();
        if (IsKogDebugEnabled()) {
            std::cerr << "[DEBUG] autoPlanAiMode check: ai.enabled=" << ai.enabled 
                      << " message->empty()=" << message->empty() 
                      << " explicitPlanFileRequested=" << explicitPlanFileRequested 
                      << " commitPlanFile=[" << *commitPlanFile << "]"
                      << " => autoPlanAiMode=" << autoPlanAiMode << std::endl;
        }
        std::cerr.flush();
        if (autoPlanAiMode) {
            if (agentProxyMode) {
                std::cerr << "Error: agent proxy mode commit cannot invoke internal AI auto-plan.\n";
                std::cerr << "Hint: prepare the plan first, then run with --plan-file.\n";
                std::cerr << "Hint: or provide explicit --message/-m for non-plan commit apply.\n";
                std::exit(2);
            }
            if (*bNoRecursive || *bNoDirtyOnly || *bStagedOnly || *bPush) {
                std::cerr << "Error: commit auto-plan mode currently supports only workspace-scoped commit apply without --push.\n";
                std::cerr << "Hint: use `kog plan runbook commit --plan-file <file>` then `kog commit --plan-file <file>` for custom scope.\n";
                std::exit(2);
            }

            const auto report = cachedPreflightReport.has_value() ? *cachedPreflightReport : RunCommitPreflight(workspaceRoot);
            if (!HasAnyChanges(report)) {
                std::println("[native-commit] workspace clean; skipping auto-plan generation and commit.");
                std::exit(0);
            }

            commitPlanFile->clear();
            const auto code = RunCommitAutoPlanPipeline(workspaceRoot, ai, *aiFillMode, *bProfile, *bAllowEmptyDirty);
            std::exit(code);
        }

        if (!aiRequested) {
            std::cout << "[native-commit] safety-gates: ignore + secret\n";
            RunPipelineSafetyGatesForNonAiCommit(workspaceRoot);
        }

        bool effectiveNoRecursive = *bNoRecursive;
        if (!effectiveNoRecursive && repos->empty() && !target->empty()) {
            const auto scopedRepos = DiscoverWorkspaceRepos(workspaceRoot);
            if (scopedRepos.size() <= 1) {
                effectiveNoRecursive = true;
            }
        }

        bool synthesizedMessagePlan = false;
        if (commitPlanFile->empty() && !message->empty()) {
            const bool dirtyOnly = !*bNoDirtyOnly;
            auto planRepos = BuildCommitScopeRecords(workspaceRoot, Trim(*repos), effectiveNoRecursive, dirtyOnly);
            if (planRepos.empty()) {
                workspace::RepoRecord fallback;
                fallback.path = workspaceRoot;
                fallback.type = "root";
                planRepos.push_back(std::move(fallback));
            }

            const auto syntheticPlanPath = DefaultMessagePlanOutputPath(workspaceRoot, *message);
            std::string syntheticPlanError;
            if (!WriteSyntheticMessageCommitPlan(workspaceRoot, planRepos, *message, syntheticPlanPath, &syntheticPlanError)) {
                std::cerr << "Error: failed to synthesize message-driven commit plan: " << syntheticPlanError << "\n";
                std::exit(2);
            }
            *commitPlanFile = syntheticPlanPath.generic_string();
            synthesizedMessagePlan = true;
            std::cout << "[native-commit] synthesized plan file: " << *commitPlanFile << "\n";
        }

        if (!commitPlanFile->empty()) {
            const auto report = cachedPreflightReport.has_value() ? *cachedPreflightReport : RunCommitPreflight(workspaceRoot);
            if (!HasAnyChanges(report)) {
                std::println("[native-commit] workspace clean; skipping --plan-file validation and commit.");
                if (*bProfile) {
                    const auto totalMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - totalStart).count();
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
        std::optional<CommitPlanStage> selectedPlanStage;
        if (!commitPlanFile->empty()) {
            if (!repos->empty() && !synthesizedMessagePlan) {
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
                WarnPlanWorkspaceStateDrift(normalizedCommitPlanPath,
                                            parsed->meta.baseHeadSha,
                                            currentBaseHeadSha,
                                            parsed->meta.dirtyFingerprint,
                                            currentDirtyFingerprint);
            }

            if (!parsed->meta.planner.provider.empty() ||
                !parsed->meta.planner.model.empty()) {
                std::cout << "[native-commit] plan meta: provider="
                          << (parsed->meta.planner.provider.empty() ? "<unset>" : parsed->meta.planner.provider)
                          << " ai-model="
                          << (parsed->meta.planner.model.empty() ? "<unset>" : parsed->meta.planner.model)
                          << "\n";
            }

            selectedPlanStage = ParseCommitPlanStage(*planStage);
            if (!selectedPlanStage.has_value()) {
                std::cerr << "Error: invalid --plan-stage value: " << *planStage
                          << " (expected commit|post_sync|both)\n";
                std::exit(2);
            }

            stageMessages = BuildStageMessageMap(*parsed, *selectedPlanStage);
            if (stageMessages.empty()) {
                std::println("[native-commit] no entries found for selected --plan-stage; skipping commit.");
                if (*bProfile) {
                    const auto totalMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - totalStart).count();
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

            if (PlanStageNeedsPreCommit(*selectedPlanStage)) {
                const auto preCommitCode = RunSyncPreCommitNative(workspaceRoot, true, false, "default");
                if (preCommitCode != 0) {
                    std::exit(preCommitCode);
                }
            }
        }

        const auto planningStart = std::chrono::steady_clock::now();
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
        planningMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - planningStart).count();

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

        const auto commitStart = std::chrono::steady_clock::now();
        std::cout << "[native-commit] plan: repos=" << repoRecords.size()
                  << " repo_waves=" << repoWaves.size()
                  << " commits=" << taskGraph.tasks.size()
                  << " commit_waves=" << taskGraph.waves.size()
                  << " jobs=" << workers
                  << " dirty_only=" << (dirtyOnly ? "on" : "off") << "\n";
        if (taskGraph.dependencyCycleDetected) {
            std::cout << "[native-commit] warning: dependency cycle detected in commit graph; downgraded to serial fallback order.\n";
        }

        std::unordered_set<std::string> planRepoKeys;
        for (const auto& rec : repoRecords) {
            planRepoKeys.insert(kano::git::commands::RepoKey(rec.path));
        }

        auto executeCommitTask = [&](const CommitTaskNode& InNode) -> RepoCommitResult {
            const auto& repo = InNode.repo;
            const auto& repoMessage = InNode.commit;
            const bool needsPlanStaging =
                isPlanMode && (InNode.repoCommitCount > 1 || !repoMessage.include.empty() || !repoMessage.exclude.empty());
            if (needsPlanStaging) {
                std::string stageError;
                if (!StageCommitItemForPlan(workspaceRoot, repo, repoMessage, planRepoKeys, &stageError)) {
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
        commitMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - commitStart).count();

        const auto totalElapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - totalStart).count();
        const auto summaryStart = std::chrono::steady_clock::now();
        const auto exitCode = PrintCommitSummary(workspaceRoot, results, totalElapsedMs);
        summaryMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - summaryStart).count();

        if (*bProfile) {
            const auto totalMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - totalStart).count();
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
    };
    cmd->callback(run);
    cmdAiAlias->callback(run);
}

void RegisterAmend(CLI::App& InApp) {
    auto* cmd = InApp.add_subcommand("amend", "Native amend workflow (default: amend previous commit)");

    auto* repos = new std::string{};
    cmd->add_option("--repos", *repos, "Amend target repos (comma-separated). Default: current repo only");

    auto* repoRoot = new std::string{};
    cmd->add_option("--repo-root", *repoRoot, "Workspace root/start path used for repo-name lookup");

    auto* target = new std::string{};
    cmd->add_option("target", *target, "Optional repo target root (repo name or relative path)")->required(false);

    auto* provider = new std::string{};
    cmd->add_option("--ai-provider", *provider, "AI provider (copilot, codex, opencode)")
        ->default_str("auto");

    auto* model = new std::string{};
    cmd->add_option("--ai-model", *model, "AI model to use");

    auto* bAiAuto = new bool{false};
    cmd->add_flag("--ai-auto,--ai", *bAiAuto, "Enable AI auto mode (provider auto + layered kog_config model selection)");

    auto* amendPlanFile = new std::string{};
    cmd->add_option("--plan-file", *amendPlanFile, "Plan JSON file used to source amend message(s)");

    auto* amendPlanStage = new std::string{"commit"};
    cmd->add_option("--plan-stage", *amendPlanStage, "Plan stage: commit|post_sync|both")->default_str("commit");

    auto* message = new std::string{};
    cmd->add_option("--message,-m", *message, "Amend commit message (skips AI generation)");

    auto* bNoAiReview = new bool{false};
    cmd->add_flag("--no-ai-review", *bNoAiReview, "Skip AI review gate");

    auto* bStagedOnly = new bool{false};
    cmd->add_flag("--staged-only", *bStagedOnly, "Amend only currently staged changes (skip auto git add)");

    auto* bCombineUnpushed = new bool{false};
    cmd->add_flag("--combine,--combine-unpushed,-U", *bCombineUnpushed, "Combine all local commits not pushed to upstream into one commit");
    auto* bAllowEmptyDirty = new bool{false};
    cmd->add_flag("--allow-empty-dirty", *bAllowEmptyDirty, "Allow AI plan-fill to run even when workspace dirty context is empty");

    auto* bYolo = new bool{false};
    cmd->add_flag("--yolo", *bYolo, "Enable all permissions for AI sub-agents");

    cmd->callback([=]() {
        const auto totalStart = std::chrono::steady_clock::now();
        const auto invocationRoot = repoRoot->empty() ? std::filesystem::current_path() : std::filesystem::path(*repoRoot);
        const auto resolvedTarget = target->empty()
            ? invocationRoot.lexically_normal()
            : ResolveRepoPath(invocationRoot.lexically_normal(), std::filesystem::path(*target));
        // Use cwd as workspace root for simple amend (no --combine).
        // ResolveWorkspaceRootFromInvocation is only needed for combine/plan flows
        // that require a superproject context; using it unconditionally causes amend
        // to treat the parent repo as workspace root when run inside a submodule.
        const auto workspaceRoot = invocationRoot.lexically_normal();

        NativeAiConfig ai;
        const bool aiRequested = *bAiAuto || !provider->empty() || !model->empty();
        ai.provider = aiRequested ? ResolveProvider(*provider) : std::string{};
        ai.model = aiRequested ? ResolveModelForAi(ai.provider, *model, *bAiAuto, workspaceRoot) : std::string{};
        ai.reviewEnabled = !*bNoAiReview;
        ai.yolo = *bYolo;
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

        // autoPlanAiMode: ai.enabled && message.empty() && amendPlanFile.empty() && combineUnpushed
        const bool autoPlanAiMode = ai.enabled && message->empty() && amendPlanFile->empty();
        if (autoPlanAiMode && *bCombineUnpushed) {
            const auto code = RunAmendAutoPlanPipeline(workspaceRoot, ai, "commit", false, *bAllowEmptyDirty);
            std::exit(code);
        }

        // Agent Mode: generate plan and pause for agent to fill messages
        if (IsAgentModeEnabled() && message->empty() && amendPlanFile->empty()) {
            const auto agentPlanPath = DefaultSharedPlanPath(workspaceRoot);
            std::cout << "[native-amend] agent mode detected; generating amend plan for agent review.\n";
            std::cout << "[native-amend] plan file: " << agentPlanPath.generic_string() << "\n";
            const auto planNewCode = RunPlanNewViaSelf(workspaceRoot, agentPlanPath);
            if (planNewCode != 0) std::exit(planNewCode);
            const auto seedCode = RunCommitSeedViaSelf(workspaceRoot, agentPlanPath);
            if (seedCode != 0) std::exit(seedCode);

            // Check if there is anything to commit
            std::string parseErr;
            const auto parsed = ParseCommitPlan(agentPlanPath, &parseErr);
            if (!parsed.has_value() || parsed->commitEntries.empty()) {
                std::cout << "[native-amend] workspace clean; nothing to amend.\n";
                std::exit(0);
            }

            std::cerr << "\n[AGENT_PLAN_REQUIRED] Please fill commit messages in:\n";
            std::cerr << "  " << agentPlanPath.generic_string() << "\n";
            std::cerr << "After editing, run: kog amend --plan-file " << agentPlanPath.generic_string() << "\n\n";
            std::exit(3);
        }

        if (!repos->empty() && !target->empty()) {
            std::cerr << "Error: positional target cannot be combined with --repos\n";
            std::exit(2);
        }

        if (!amendPlanFile->empty() && !message->empty()) {
            std::cerr << "Error: --plan-file cannot be combined with --message/-m\n";
            std::exit(2);
        }

        auto reposCsv = Trim(*repos);
        std::vector<std::filesystem::path> repoList;
        if (!reposCsv.empty()) {
            repoList = BuildOrderedRepoList(workspaceRoot, reposCsv);
            if (repoList.empty()) {
                repoList.push_back(resolvedTarget);
            }
        } else if (!target->empty()) {
            repoList.push_back(resolvedTarget);
        } else {
            repoList.push_back(std::filesystem::current_path().lexically_normal());
        }

        std::unordered_map<std::string, std::vector<RepoCommitPlanEntry::CommitItem>> stageMessages;
        if (!amendPlanFile->empty()) {
            std::string planError;
            const auto normalizedPlanPath = NormalizeInputPathForCurrentPlatform(*amendPlanFile);
            const auto parsed = ParseCommitPlan(std::filesystem::path(normalizedPlanPath), &planError);
            if (!parsed.has_value()) {
                std::cerr << "Error: invalid --plan-file: " << normalizedPlanPath;
                if (!planError.empty()) {
                    std::cerr << " (" << planError << ")";
                }
                std::cerr << "\n";
                std::exit(2);
            }

            std::string validationError;
            if (!ValidateCommitPlanForAiMode(*parsed, &validationError)) {
                std::cerr << "Error: invalid --plan-file: " << normalizedPlanPath;
                if (!validationError.empty()) {
                    std::cerr << " (" << validationError << ")";
                }
                std::cerr << "\n";
                std::exit(2);
            }

            const auto selectedPlanStage = ParseCommitPlanStage(*amendPlanStage);
            if (!selectedPlanStage.has_value()) {
                std::cerr << "Error: invalid --plan-stage value: " << *amendPlanStage
                          << " (expected commit|post_sync|both)\n";
                std::exit(2);
            }

            stageMessages = BuildStageMessageMap(*parsed, *selectedPlanStage);
            if (stageMessages.empty()) {
                std::cerr << "Error: no amend messages found in selected --plan-stage\n";
                std::exit(2);
            }
        }

        std::vector<RepoAmendResult> results;
        results.reserve(repoList.size());

        for (const auto& repo : repoList) {
            const auto label = DisplayRepoLabel(workspaceRoot, repo);
            std::cout << "\n[amend] " << label << "\n";
            std::string effectiveMessage = *message;
            if (effectiveMessage.empty() && !stageMessages.empty()) {
                const auto planned = ResolveRepoMessages(stageMessages, workspaceRoot, repo, "");
                if (!planned.empty()) {
                    effectiveMessage = Trim(planned.front().message);
                }
            }
            const auto one = AmendSingleRepo(workspaceRoot, repo, effectiveMessage, *bStagedOnly, *bCombineUnpushed, ai);
            results.push_back(one);
        }

        const auto totalElapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - totalStart).count();
        const auto exitCode = PrintAmendSummary(workspaceRoot, results, totalElapsedMs);
        std::exit(exitCode);
    });
}

} // namespace kano::git::commands
