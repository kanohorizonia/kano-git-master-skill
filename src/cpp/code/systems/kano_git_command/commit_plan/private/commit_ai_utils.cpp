#include "commit_ai_utils.hpp"
#include "ai_utils.hpp"
#include "plan_utils.hpp"
#include "kog_config.hpp"
#include "auto_model_policy.hpp"
#include "discovery.hpp"

#include <CLI/CLI.hpp>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <ctime>
#include <format>
#include <fstream>
#include <iomanip>
#include <optional>
#include <print>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

namespace kano::git::commands {

auto HasCommand(const std::string& InCommand, const std::vector<std::string>& InArgs = {"--help"}) -> bool {
    const auto result = shell::ExecuteCommand(InCommand, InArgs, shell::ExecMode::Capture, std::filesystem::current_path());
    return result.exitCode == 0;
}

auto IsGitRepo(const std::filesystem::path& InRepo) -> bool {
    return GitCapture(InRepo, {"rev-parse", "--git-dir"}).exitCode == 0;
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

auto ResolveProvider(const std::string& InProviderRaw) -> std::string {
    const auto provider = ToLower(Trim(InProviderRaw));
    if (!provider.empty() && provider != "auto") {
        return provider;
    }

    if (HasCommand("copilot", {"--help"}) || HasCommand("gh", {"copilot", "--version"})) {
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

auto IsParentRepoPath(const std::filesystem::path& InParent, const std::filesystem::path& InChild) -> bool {
    const auto parent = ToGeneric(InParent);
    const auto child = ToGeneric(InChild);
    if (parent.empty() || child.empty() || parent == child) {
        return false;
    }
    const std::string prefix = parent + "/";
    return child.rfind(prefix, 0) == 0;
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

auto RepoHasPorcelainChanges(const std::filesystem::path& InRepo) -> bool {
    const auto status = GitCapture(InRepo, {"status", "--porcelain"});
    return status.exitCode == 0 && !Trim(status.stdoutStr).empty();
}

auto ParseRootStatusPath(std::string InLine) -> std::string {
    while (!InLine.empty() && (InLine.back() == '\r' || InLine.back() == '\n')) {
        InLine.pop_back();
    }
    if (InLine.size() < 4) {
        return {};
    }

    auto relPath = Trim(InLine.substr(3));
    const auto arrowPos = relPath.find(" -> ");
    if (arrowPos != std::string::npos) {
        relPath = Trim(relPath.substr(arrowPos + 4));
    }
    return relPath;
}

auto AddDirtyNestedReposFromRootStatus(const std::filesystem::path& InWorkspaceRoot,
                                      const std::unordered_map<std::string, workspace::RepoRecord>& InRecordsByPath,
                                      std::vector<workspace::RepoRecord>* InOutSelected) -> void {
    if (InOutSelected == nullptr) {
        return;
    }

    std::unordered_map<std::string, std::size_t> selectedByPath;
    selectedByPath.reserve(InOutSelected->size());
    for (std::size_t index = 0; index < InOutSelected->size(); ++index) {
        selectedByPath.emplace(ToGeneric((*InOutSelected)[index].path), index);
    }

    const auto rootStatus = GitCapture(InWorkspaceRoot, {"status", "--porcelain"});
    if (rootStatus.exitCode != 0) {
        return;
    }

    std::istringstream iss(rootStatus.stdoutStr);
    std::string line;
    while (std::getline(iss, line)) {
        const auto relPath = ParseRootStatusPath(std::move(line));
        if (relPath.empty()) {
            continue;
        }

        const auto candidate = ResolveRepoPath(InWorkspaceRoot, relPath);
        if (candidate.empty()) {
            continue;
        }
        const auto candidateKey = ToGeneric(candidate);
        if (candidateKey.empty() || candidateKey == ToGeneric(InWorkspaceRoot)) {
            continue;
        }

        const auto inGitRepo = GitCapture(candidate, {"rev-parse", "--is-inside-work-tree"});
        if (inGitRepo.exitCode != 0 || Trim(inGitRepo.stdoutStr) != "true") {
            continue;
        }
        if (!RepoHasPorcelainChanges(candidate)) {
            continue;
        }

        if (const auto selected = selectedByPath.find(candidateKey); selected != selectedByPath.end()) {
            (*InOutSelected)[selected->second].hasChanges = true;
            continue;
        }

        workspace::RepoRecord record;
        const auto found = InRecordsByPath.find(candidateKey);
        if (found != InRecordsByPath.end()) {
            record = found->second;
        } else {
            record.path = candidate;
            record.type = "explicit-dirty-fallback";
        }
        record.hasChanges = true;
        InOutSelected->push_back(std::move(record));
        selectedByPath.emplace(candidateKey, InOutSelected->size() - 1);
    }
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





auto ExtractSingleLineMessage(const std::string& InText) -> std::string {
    // --- Strip leading Unicode bullet/symbol characters (multi-byte UTF-8) ---
    auto StripLeadingUnicodeBullets = [](std::string s) -> std::string {
        // Common AI-output bullet/symbol UTF-8 byte sequences:
        //   ● U+25CF (E2 97 8F)   ◆ U+25C6 (E2 97 86)   ▶ U+25B6 (E2 96 B6)
        //   ► U+25BA (E2 96 BA)   ○ U+25CB (E2 97 8B)   ◉ U+25C9 (E2 97 89)
        //   → U+2192 (E2 86 92)   • U+2022 (E2 80 A2)   ‣ U+2023 (E2 80 A3)
        //   ✓ U+2713 (E2 9C 93)   ✗ U+2717 (E2 9C 97)   ✦ U+2726 (E2 9C A6)
        //   ★ U+2605 (E2 98 85)   ☆ U+2606 (E2 98 86)
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
            "reading the ",    "let me ",        "i'll ",         "i will ",
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

    // Pass 2c: last resort — return first candidate even if it looks like preamble.
    return candidates.front();
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

auto RunAiGenerate(const std::string& InProvider,
                   const std::string& InModel,
                   const std::string& InPrompt,
                   std::optional<std::filesystem::path> InWorkingDir,
                   const std::string& InPurpose) -> shell::ExecResult {
    const auto effectivePrompt = BuildFileBackedPromptArgument(InWorkingDir, InPrompt, InPurpose);

    auto LogInvocation = [&](const std::string& binary, const std::vector<std::string>& args) {
        static constexpr std::string_view kDivider = "----------------------------------------";
        std::cout << "\n[kog ai] -- AI Invocation (" << InPurpose << ") --\n";
        std::cout << "[kog ai] command : " << binary;
        for (const auto& a : args) {
            if (a.find(' ') != std::string::npos || a.empty()) std::cout << " \"" << a << "\"";
            else std::cout << " " << a;
        }
        std::cout << "\n[kog ai] model   : " << (InModel.empty() ? "auto" : InModel) << "\n";
        if (IsTruthyEnv(std::getenv("KOG_DEBUG_AI_PROMPT")) || IsTruthyEnv(std::getenv("KOG_DEBUG"))) {
            std::cout << "[kog ai] prompt  :\n" << kDivider << "\n" << InPrompt << "\n" << kDivider << "\n";
        }
        std::cout << "[kog ai] Waiting for " << InProvider << " response...\n";
        std::cout.flush();
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
        auto result = shell::ExecuteCommand("opencode", args, shell::ExecMode::Capture, InWorkingDir);
        if (IsTruthyEnv(std::getenv("KOG_DEBUG_AI_PROMPT")) || IsTruthyEnv(std::getenv("KOG_DEBUG"))) {
            std::cerr << "[kog ai] response:\n" << "----------------------------------------\n" << result.stdoutStr << "\n" << result.stderrStr << "\n----------------------------------------\n";
        }
        return result;
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
            LogInvocation("gh", args);
            return shell::ExecuteCommand("gh", args, shell::ExecMode::Capture, InWorkingDir);
        }
    }

    return shell::ExecResult{.exitCode = 1, .stderrStr = "unsupported provider or provider command unavailable"};
}

auto GenerateAiCommitMessage(const std::filesystem::path& InWorkspaceRoot,
                             const std::filesystem::path& InRepo,
                             const CommitPreflightReport& InReport,
                             const NativeAiConfig& InAi,
                             std::string* OutFailureReason) -> std::string {
    if (!InAi.enabled) {
        if (OutFailureReason != nullptr) {
            *OutFailureReason = "ai is disabled";
        }
        return {};
    }

    const auto prompt = BuildAiCommitPrompt(InWorkspaceRoot, InRepo, InReport);
    const auto out = RunAiGenerate(InAi.provider, InAi.model, prompt, std::optional<std::filesystem::path>{InRepo}, "commit-message");
    if (out.exitCode != 0) {
        const auto reason = SummarizeAiFailure(out);
        if (OutFailureReason != nullptr) {
            *OutFailureReason = reason;
        }
        return {};
    }

    auto message = ExtractSingleLineMessage(out.stdoutStr + "\n" + out.stderrStr);
    if (message.empty()) {
        const std::string reason = "ai provider returned empty message";
        if (OutFailureReason != nullptr) {
            *OutFailureReason = reason;
        }
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
    if (Trim(stagedDiff).empty()) {
        const auto head = GitCapture(InRepo, {"show", "--format=", "--stat", "--patch", "HEAD", "--", "."});
        if (head.exitCode == 0 && !Trim(head.stdoutStr).empty()) {
            stagedDiff = "[context] worktree is clean; using HEAD patch for amend/reword.\n" + head.stdoutStr;
        }
    }
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

    promptText = AppendCommitConventionSkillSection(std::filesystem::current_path().lexically_normal(), std::move(promptText));

    const auto out = RunAiGenerate(InAi.provider, InAi.model, promptText, std::optional<std::filesystem::path>{InRepo}, "commit-review");
    if (out.exitCode != 0) {
        const auto reason = SummarizeAiFailure(out);
        std::cerr << "[kog commit] AI review generation failed (exit=" << out.exitCode << "): " << reason << "\n";
        OutReason = "AI review skipped due to execution error: " + reason;
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



auto DiscoverWorkspaceRepoRecords(const std::filesystem::path& InRoot,
                                  const std::string& InMetadataLevel,
                                  const bool InUseCache,
                                  const bool InRefreshCache) -> std::vector<workspace::RepoRecord> {
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

auto BuildAiCommitPrompt(const std::filesystem::path& InWorkspaceRoot,
                         const std::filesystem::path& InRepo,
                         const CommitPreflightReport& InReport) -> std::string {
    const auto label = DisplayRepoLabel(InWorkspaceRoot, InRepo);
    const auto diff = GitCapture(InRepo, {"diff", "--cached", "--", "."});
    std::string diffText = diff.stdoutStr;
    if (Trim(diffText).empty()) {
        const auto head = GitCapture(InRepo, {"show", "--format=", "--stat", "--patch", "HEAD", "--", "."});
        if (head.exitCode == 0 && !Trim(head.stdoutStr).empty()) {
            diffText = "[context] worktree is clean; using HEAD patch for amend/reword.\n" + head.stdoutStr;
        }
    }
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
        return AppendCommitConventionSkillSection(InWorkspaceRoot, std::move(prompt));
    }

    std::ostringstream oss;
    oss << "You are a git commit assistant.\n"
        << "Before generating the commit message, inspect git status and untracked files.\n"
        << "If any untracked files are clearly build artifacts, cache files, generated files, logs, temp files, editor noise, or other local-only noise that should not be committed, update .gitignore first and exclude them before deciding the commit message.\n"
        << "If .gitignore needs cleanup, do that work before writing the message. Only after ignore cleanup is complete should you summarize the remaining intended commit changes.\n"
        << "Do not ignore real source files, config files, assets, or user-intended project files unless they are obviously local-only noise.\n\n"
        << "Generate ONE git commit message following Kano Commit Convention (KCC) format:\n"
        << "  [<Subsystem>][<Type>] <Summary>\n"
        << "  Examples:\n"
        << "    [UGS][BugFix] Retry tagged output parsing on proxy glitch\n"
        << "    [Build][Chore] Update CI bootstrap script\n"
        << "    [Core][Refactor] Extract shared path resolver\n\n"
        << "Rules:\n"
        << "- Subsystem: 2-24 chars, alphanumeric, PascalCase recommended (e.g. Core, UI, Build, Tools)\n"
        << "- Type: Feature | BugFix | Refactor | Perf | Chore | Test | Docs\n"
        << "- Summary: Start with verb (Add/Fix/Update/Remove/Refactor), ~50-72 chars\n"
        << "- Output exactly one line, no markdown, no code fences, no explanation\n"
        << "- The one-line output must describe the post-.gitignore-cleanup commit intent, not the ignored noise\n\n"
        << "Repo: " << label << "\n"
        << "Staged: " << InReport.stagedCount << "\n"
        << "Unstaged: " << InReport.unstagedCount << "\n"
        << "Untracked: " << InReport.untrackedCount << "\n\n"
        << "Staged diff:\n"
        << diffText << "\n";
    return AppendCommitConventionSkillSection(InWorkspaceRoot, oss.str());
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
        if (provider == "opencode") {
            return "github-copilot/gpt-5-mini";
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
    if (!Trim(configuredSelection).empty()) {
        if (configuredLower == "provider-default") {
            return resolveDefaultModel();
        }
        if (configuredLower == "auto") {
            return resolveAutoModel();
        }
        return configuredSelection;
    }

    if (InAiAuto || modelLower == "auto") {
        return resolveAutoModel();
    }

    return {};
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

auto DefaultMessagePlanOutputPath(const std::filesystem::path& InWorkspaceRoot,
                                  const std::string& InMessage) -> std::filesystem::path {
    const auto stamp = CurrentUtcTimestampCompactForSyntheticPlan();
    const auto digest = Fnv1a64Hex(InMessage).substr(0, 8);
    auto root = ResolveGlobalCacheRoot();
    if (root.empty()) {
        root = (InWorkspaceRoot / ".kano" / "cache" / "git").lexically_normal();
    }
    return (root / "plans" / ("message-plan-" + stamp + "-" + digest + ".json")).lexically_normal();
}

auto WriteSyntheticMessageCommitPlan(const std::filesystem::path& InWorkspaceRoot,
                                     const std::vector<workspace::RepoRecord>& InRepoRecords,
                                     const std::string& InMessage,
                                     const std::filesystem::path& InOutPath,
                                     std::string* OutError) -> bool {
    const auto generatedAtUtc = CurrentUtcTimestampIso8601ForSyntheticPlan();
    const auto baseHeadSha = ComputeWorkspaceBaseHeadSha(InWorkspaceRoot);
    const auto dirtyFingerprint = ComputeWorkspaceDirtyFingerprint(InWorkspaceRoot);
    std::string repoKeySeed;
    for (const auto& repo : InRepoRecords) {
        repoKeySeed += WorkspaceRepoKey(InWorkspaceRoot, repo.path);
        repoKeySeed += "\n";
    }
    const auto planId = std::string{"message-plan-"} +
                        Fnv1a64Hex(baseHeadSha + "\n" + dirtyFingerprint + "\n" + InMessage + "\n" + repoKeySeed);
    constexpr std::string_view kReviewReason = "message shorthand plan synthesized by kog commit";

    std::ostringstream json;
    json << "{\n";
    json << "  \"meta\": {\n";
    json << "    \"schema_version\": \"1\",\n";
    json << "    \"plan_id\": \"" << JsonEscape(planId) << "\",\n";
    json << "    \"generated_at_utc\": \"" << JsonEscape(generatedAtUtc) << "\",\n";
    json << "    \"base_head_sha\": \"" << JsonEscape(baseHeadSha) << "\",\n";
    json << "    \"dirty_fingerprint_pre_ignore\": \"" << JsonEscape(dirtyFingerprint) << "\",\n";
    json << "    \"dirty_fingerprint\": \"" << JsonEscape(dirtyFingerprint) << "\",\n";
    json << "    \"planner\": {\n";
    json << "      \"provider\": \"native\",\n";
    json << "      \"ai-model\": \"message-shorthand\",\n";
    json << "      \"request_id\": \"message-shorthand\"\n";
    json << "    },\n";
    json << "    \"review\": {\n";
    json << "      \"verdict\": \"pass\",\n";
    json << "      \"reason\": \"" << JsonEscape(std::string(kReviewReason)) << "\"\n";
    json << "    }\n";
    json << "  },\n";
    json << "  \"stages\": {\n";
    json << "    \"commit\": [\n";
    for (std::size_t idx = 0; idx < InRepoRecords.size(); ++idx) {
        const auto& repo = InRepoRecords[idx];
        json << "      {\n";
        json << "        \"repo\": \"" << JsonEscape(WorkspaceRepoKey(InWorkspaceRoot, repo.path)) << "\",\n";
        json << "        \"commits\": [\n";
        json << "          {\n";
        json << "            \"message\": \"" << JsonEscape(InMessage) << "\",\n";
        json << "            \"review\": {\n";
        json << "              \"verdict\": \"pass\",\n";
        json << "              \"reason\": \"" << JsonEscape(std::string(kReviewReason)) << "\"\n";
        json << "            }\n";
        json << "          }\n";
        json << "        ]\n";
        json << "      }" << (idx + 1 < InRepoRecords.size() ? "," : "") << "\n";
    }
    json << "    ],\n";
    json << "    \"post_sync\": []\n";
    json << "  }\n";
    json << "}\n";

    std::error_code ec;
    std::filesystem::create_directories(InOutPath.parent_path(), ec);
    if (ec) {
        if (OutError != nullptr) {
            *OutError = std::string{"failed to create synthetic plan directory: "} + ec.message();
        }
        return false;
    }

    std::ofstream out(InOutPath, std::ios::out | std::ios::binary | std::ios::trunc);
    if (!out) {
        if (OutError != nullptr) {
            *OutError = "failed to open synthetic plan file for write";
        }
        return false;
    }
    out << json.str();
    out.close();
    if (!out) {
        if (OutError != nullptr) {
            *OutError = "failed to write synthetic plan file";
        }
        return false;
    }
    return true;
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

    if (InDirtyOnly && Trim(InReposCsv).empty() && !InNoRecursive) {
        AddDirtyNestedReposFromRootStatus(InWorkspaceRoot, byPath, &selected);
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

} // namespace kano::git::commands
