// commit-push command — Orchestrate commit -> sync -> push workflow

#include "command_registry.hpp"
#include "discovery.hpp"
#include "shell_executor.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
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
    std::transform(InValue.begin(), InValue.end(), InValue.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return InValue;
}

auto ParseNonNegativeInt(const std::string& InValue) -> int {
    const auto trimmed = Trim(InValue);
    if (trimmed.empty()) {
        return 0;
    }
    try {
        return std::max(0, std::stoi(trimmed));
    } catch (const std::exception&) {
        return 0;
    }
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

auto ParseReposCsv(const std::string& InCsv) -> std::vector<std::string> {
    std::vector<std::string> repos;
    std::istringstream iss(InCsv);
    std::string token;
    while (std::getline(iss, token, ',')) {
        token = Trim(token);
        if (token.empty()) {
            continue;
        }
        repos.push_back(token);
    }
    return repos;
}

auto GitCapture(const std::filesystem::path& InRepo, const std::vector<std::string>& InArgs) -> shell::ExecResult {
    return shell::ExecuteCommand("git", InArgs, shell::ExecMode::Capture, InRepo);
}

auto IsGitRepo(const std::filesystem::path& InRepo) -> bool {
    return GitCapture(InRepo, {"rev-parse", "--git-dir"}).exitCode == 0;
}

auto RelativeDisplayPath(const std::filesystem::path& InRoot, const std::filesystem::path& InPath) -> std::filesystem::path {
    auto normalizedRoot = InRoot.lexically_normal();
    if (!normalizedRoot.is_absolute()) {
        normalizedRoot = std::filesystem::absolute(normalizedRoot).lexically_normal();
    }
    const auto normalizedPath = InPath.lexically_normal();
    const auto relative = normalizedPath.lexically_relative(normalizedRoot);
    if (!relative.empty()) {
        return relative;
    }
    return normalizedPath;
}

auto IsParentPath(const std::filesystem::path& InParent, const std::filesystem::path& InChild) -> bool {
    const auto parent = InParent.lexically_normal().generic_string();
    const auto child = InChild.lexically_normal().generic_string();
    if (parent.empty() || child.empty() || parent == child) {
        return false;
    }
    const std::string prefix = parent + "/";
    return child.rfind(prefix, 0) == 0;
}

auto RepoNameFromPath(const std::filesystem::path& InPath) -> std::string {
    const auto normalized = InPath.lexically_normal();
    auto name = normalized.filename().generic_string();
    if (name.empty()) {
        name = normalized.parent_path().filename().generic_string();
    }
    if (!name.empty()) {
        return name;
    }
    return normalized.generic_string();
}

auto ResolveRepoFromSpec(const std::filesystem::path& InRoot,
                         const std::filesystem::path& InSpec,
                         const int InMaxDepth,
                         const bool InUseCache) -> std::filesystem::path {
    if (InSpec.empty() || InSpec == ".") {
        return InRoot.lexically_normal();
    }

    const auto specText = InSpec.generic_string();
    const auto candidate = (InSpec.is_absolute() ? InSpec : (InRoot / InSpec)).lexically_normal();
    if (std::filesystem::exists(candidate) && IsGitRepo(candidate)) {
        return candidate;
    }

    std::string manifestReason;
    if (const auto manifest = workspace::LoadTrustedWorkspaceManifest(InRoot, &manifestReason); manifest.has_value()) {
        std::vector<std::filesystem::path> exactMatches;
        std::vector<std::filesystem::path> fuzzyMatches;
        for (const auto& repo : manifest->repos) {
            const auto repoPath = repo.path.lexically_normal();
            const auto repoName = RepoNameFromPath(repoPath);
            const auto repoKey = repoPath.generic_string();
            const auto relativeKey = RelativeDisplayPath(InRoot, repoPath).generic_string();
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
    options.rootDir = InRoot;
    options.maxDepth = InMaxDepth;
    options.useCache = InUseCache;
    options.metadataLevel = "minimal";

    const auto discovery = workspace::DiscoverRepos(options);
    std::vector<std::filesystem::path> exactMatches;
    std::vector<std::filesystem::path> fuzzyMatches;

    for (const auto& repo : discovery.repos) {
        const auto repoPath = repo.path.lexically_normal();
        const auto repoName = RepoNameFromPath(repoPath);
        const auto repoKey = repoPath.generic_string();
        const auto relativeKey = RelativeDisplayPath(InRoot, repoPath).generic_string();

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
        throw std::runtime_error("repo not found: " + specText);
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

auto ResolveRepoList(const std::filesystem::path& InRoot, const std::vector<std::string>& InRepoList) -> std::vector<std::string> {
    std::vector<std::string> out;
    out.reserve(InRepoList.size());
    for (const auto& repo : InRepoList) {
        out.push_back(ResolveRepoFromSpec(InRoot, std::filesystem::path(repo), 12, true).generic_string());
    }
    return out;
}

auto JoinReposCsv(const std::vector<std::string>& InRepoList) -> std::string {
    std::ostringstream oss;
    for (std::size_t i = 0; i < InRepoList.size(); ++i) {
        if (i > 0) {
            oss << ",";
        }
        oss << InRepoList[i];
    }
    return oss.str();
}

auto SelfBinaryPath() -> std::string {
    if (const char* path = std::getenv("KANO_GIT_BINARY_PATH"); path != nullptr && *path != '\0') {
        return std::string(path);
    }
    return "kano-git";
}

auto DiscoverWorkspaceRepos(const std::filesystem::path& InRoot) -> std::vector<std::filesystem::path> {
    static std::unordered_map<std::string, std::vector<std::filesystem::path>> cache;
    const auto cacheKey = InRoot.lexically_normal().generic_string();
    if (const auto cached = cache.find(cacheKey); cached != cache.end()) {
        return cached->second;
    }

    std::string manifestReason;
    if (const auto manifest = workspace::LoadTrustedWorkspaceManifest(InRoot, &manifestReason); manifest.has_value()) {
        std::vector<std::filesystem::path> repos;
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
        repos.erase(std::unique(repos.begin(), repos.end()), repos.end());
        cache.emplace(cacheKey, repos);
        return repos;
    }

    std::cerr << "[commit-push] workspace manifest untrusted (" << manifestReason
              << "); running full scan under " << cacheKey << "...\n";
    std::cerr.flush();

    workspace::DiscoverOptions options;
    options.rootDir = InRoot;
    options.maxDepth = 12;
    options.useCache = false;
    options.refreshCache = true;
    options.incremental = false;
    options.metadataLevel = "minimal";
    const auto discovery = workspace::DiscoverRepos(options);
    std::vector<std::filesystem::path> repos;
    repos.reserve(discovery.repos.size());
    for (const auto& repo : discovery.repos) {
        repos.push_back(repo.path.lexically_normal());
    }
    if (repos.empty()) {
        repos.push_back(InRoot.lexically_normal());
    }
    std::sort(repos.begin(), repos.end(), [](const auto& A, const auto& B) {
        return A.generic_string() < B.generic_string();
    });
    repos.erase(std::unique(repos.begin(), repos.end()), repos.end());
    const auto manifest = workspace::BuildWorkspaceManifest(InRoot, discovery.repos);
    if (!workspace::SaveWorkspaceManifest(manifest)) {
        std::cerr << "[commit-push] WARN: failed to refresh workspace manifest at "
                  << manifest.manifestFile.lexically_normal().generic_string() << "\n";
    }
    cache.emplace(cacheKey, repos);
    return repos;
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
        out.insert(ToLower(t));
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
        const auto out = shell::ExecuteCommand("git", args, shell::ExecMode::Capture, InRepo);
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

auto IsAgentModeEnabled() -> bool {
    const char* raw = std::getenv("KANO_AGENT_MODE");
    if (raw == nullptr) {
        return false;
    }
    const auto value = Trim(raw);
    if (value.empty() || value == "0" || value == "false" || value == "FALSE") {
        return false;
    }
    return true;
}

auto CaptureHeadShortSha(const std::filesystem::path& InWorkingDir) -> std::string {
    const auto out = shell::ExecuteCommand("git", {"rev-parse", "--short", "HEAD"}, shell::ExecMode::Capture, InWorkingDir);
    if (out.exitCode != 0) {
        return "nohead";
    }
    auto value = Trim(out.stdoutStr);
    if (value.empty()) {
        return "nohead";
    }
    for (auto& ch : value) {
        if (!std::isalnum(static_cast<unsigned char>(ch))) {
            ch = '_';
        }
    }
    return value;
}

auto CurrentUtcTimestampCompact() -> std::string {
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

auto CurrentUtcTimestampIso8601() -> std::string {
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

auto DefaultCommitPlanOutputPath(const std::filesystem::path& InWorkspaceRoot) -> std::filesystem::path {
    const auto headShort = CaptureHeadShortSha(InWorkspaceRoot);
    const auto stamp = CurrentUtcTimestampCompact();
    return (InWorkspaceRoot / ".kano" / "cache" / "git" / "plans" /
            ("plan-" + stamp + "-" + headShort + ".json"))
        .lexically_normal();
}

auto DefaultSharedPlanPath(const std::filesystem::path& InWorkspaceRoot) -> std::filesystem::path {
    if (const char* explicitPlan = std::getenv("KOG_PLAN_FILE"); explicitPlan != nullptr && *explicitPlan != '\0') {
        return std::filesystem::path(explicitPlan).lexically_normal();
    }
    return (InWorkspaceRoot / ".kano" / "cache" / "git" / "plans" / "default-plan.json").lexically_normal();
}

auto ResolveSelfBinaryCommand() -> std::string {
    if (const char* binaryPath = std::getenv("KANO_GIT_BINARY_PATH"); binaryPath != nullptr) {
        const std::filesystem::path p(binaryPath);
        if (std::filesystem::exists(p)) {
            return p.generic_string();
        }
    }
#if defined(_WIN32)
    return "kano-git.exe";
#else
    return "kano-git";
#endif
}

auto EmitCapturedSelfResult(const shell::ExecResult& InResult) -> void {
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

auto ContainsNestedSelfLauncherFailure(const shell::ExecResult& InResult) -> bool {
    const auto stderrLower = ToLower(InResult.stderrStr);
    return stderrLower.find("find_binary: command not found") != std::string::npos;
}

auto FinalizeNestedSelfResult(const char* InLabel,
                              const shell::ExecResult& InResult) -> int {
    EmitCapturedSelfResult(InResult);
    if (ContainsNestedSelfLauncherFailure(InResult)) {
        std::cerr << "Error: " << InLabel
                  << " hit nested launcher shell failure (`find_binary` leaked into sh); aborting.\n";
        return InResult.exitCode != 0 ? InResult.exitCode : 127;
    }
    return InResult.exitCode;
}

auto RunCommitPlanRunbookViaSelf(const std::filesystem::path& InWorkspaceRoot,
                                 const std::filesystem::path& InPlanPath,
                                 const std::string& InProvider,
                                 const std::string& InModel) -> int {
    std::vector<std::string> args = {
        "plan", "runbook", "commit",
        "--plan-file", InPlanPath.generic_string(),
        "--ai-provider", InProvider.empty() ? "auto" : InProvider,
        "--ai-model", InModel.empty() ? "auto" : InModel,
    };
    const auto result = shell::ExecuteCommand(ResolveSelfBinaryCommand(), args, shell::ExecMode::Capture, InWorkspaceRoot);
    const auto exitCode = FinalizeNestedSelfResult("AI commit runbook", result);
    if (exitCode != 0) {
        std::cerr << "Error: AI commit runbook failed via native binary (exit=" << exitCode << ").\n";
    }
    return exitCode;
}

auto RunPlanNewViaSelf(const std::filesystem::path& InWorkspaceRoot,
                       const std::filesystem::path& InPlanPath) -> int {
    std::vector<std::string> args = {
        "plan", "new",
        "--force",
        "--output", InPlanPath.generic_string(),
    };
    const auto result = shell::ExecuteCommand(ResolveSelfBinaryCommand(), args, shell::ExecMode::Capture, InWorkspaceRoot);
    const auto exitCode = FinalizeNestedSelfResult("plan new", result);
    if (exitCode != 0) {
        std::cerr << "Error: plan new failed via native binary (exit=" << exitCode << ").\n";
    }
    return exitCode;
}

auto RunPlanCommitSeedViaSelf(const std::filesystem::path& InWorkspaceRoot,
                              const std::filesystem::path& InPlanPath,
                              const bool InDeterministic) -> int {
    std::vector<std::string> args = {
        "plan", "commit-seed",
        "--force",
        "--plan-file", InPlanPath.generic_string(),
    };
    if (InDeterministic) {
        args.push_back("--deterministic");
    }
    const auto result = shell::ExecuteCommand(ResolveSelfBinaryCommand(), args, shell::ExecMode::Capture, InWorkspaceRoot);
    const auto exitCode = FinalizeNestedSelfResult("plan commit-seed", result);
    if (exitCode != 0) {
        std::cerr << "Error: plan commit-seed failed via native binary (exit=" << exitCode << ").\n";
    }
    return exitCode;
}

auto CheckPlanRefreshNeededViaSelf(const std::filesystem::path& InWorkspaceRoot,
                                   const std::filesystem::path& InPlanPath,
                                   std::string* OutReason) -> int {
    std::vector<std::string> args = {
        "plan", "refresh-check",
        "--plan-file", InPlanPath.generic_string(),
        "--verbose",
    };
    const auto result = shell::ExecuteCommand(ResolveSelfBinaryCommand(), args, shell::ExecMode::Capture, InWorkspaceRoot);
    EmitCapturedSelfResult(result);
    if (OutReason != nullptr) {
        *OutReason = Trim(!result.stdoutStr.empty() ? result.stdoutStr : result.stderrStr);
    }
    return result.exitCode;
}

auto IsSharedDefaultPlanPath(const std::filesystem::path& InWorkspaceRoot,
                             const std::filesystem::path& InPlanPath) -> bool {
    return InPlanPath.lexically_normal() == DefaultSharedPlanPath(InWorkspaceRoot).lexically_normal();
}

auto EnsureAgentSharedPlanFresh(const std::filesystem::path& InWorkspaceRoot,
                                const std::filesystem::path& InPlanPath) -> int {
    std::string refreshReason;
    const auto refreshCheckExit = CheckPlanRefreshNeededViaSelf(InWorkspaceRoot, InPlanPath, &refreshReason);
    if (refreshCheckExit == 1) {
        return 0;
    }
    if (refreshCheckExit != 0) {
        std::cerr << "Error: plan refresh-check failed for " << InPlanPath.generic_string() << " (exit=" << refreshCheckExit << ")\n";
        return refreshCheckExit;
    }

    std::cout << "[commit-push] refreshing shared agent plan: " << InPlanPath.generic_string();
    if (!refreshReason.empty()) {
        std::cout << " (" << refreshReason << ")";
    }
    std::cout << "\n";

    const auto planNewCode = RunPlanNewViaSelf(InWorkspaceRoot, InPlanPath);
    if (planNewCode != 0) {
        return planNewCode;
    }
    const auto commitSeedCode = RunPlanCommitSeedViaSelf(InWorkspaceRoot, InPlanPath, true);
    if (commitSeedCode != 0) {
        return commitSeedCode;
    }

    refreshReason.clear();
    const auto verifyExit = CheckPlanRefreshNeededViaSelf(InWorkspaceRoot, InPlanPath, &refreshReason);
    if (verifyExit != 1) {
        std::cerr << "Error: refreshed shared agent plan is still not ready: " << InPlanPath.generic_string();
        if (!refreshReason.empty()) {
            std::cerr << " (" << refreshReason << ")";
        }
        std::cerr << "\n";
        return verifyExit == 0 ? 2 : verifyExit;
    }
    return 0;
}

auto RunIgnorePlanRunbookViaSelf(const std::filesystem::path& InWorkspaceRoot,
                                 const std::filesystem::path& InPlanPath) -> int {
    std::vector<std::string> args = {
        "plan", "runbook", "ignore",
        "--force",
        "--plan-file", InPlanPath.generic_string(),
    };
    const auto result = shell::ExecuteCommand(ResolveSelfBinaryCommand(), args, shell::ExecMode::Capture, InWorkspaceRoot);
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
    const auto exitCode = FinalizeNestedSelfResult("ignore apply", result);
    if (exitCode != 0) {
        std::cerr << "Error: ignore apply failed via native binary (exit=" << exitCode << ").\n";
    }
    return exitCode;
}

auto BuildCommitPlanTemplateJson(const std::string& InGeneratedAtUtc) -> std::string {
    std::ostringstream oss;
    oss << R"json({
  "meta": {
    "schema_version": "2",
    "plan_id": "replace-with-unique-id",
    "generated_at_utc": ")json" << InGeneratedAtUtc << R"json(",
    "executed_at_utc": "",
    "base_head_sha": "replace-with-head-sha",
    "dirty_fingerprint": "replace-with-dirty-fingerprint",
    "planner": {
      "provider": "human",
      "ai-model": ""
    },
    "review": {
      "verdict": "pass",
      "reason": "replace-with-review-summary"
    }
  },
  "stages": {
    "commit": [
      {
        "repo": ".",
        "commits": [
          { "message": "feat(scope): replace-with-commit-message" }
        ]
      }
    ],
    "post_sync": []
  }
}
)json";
    return oss.str();
}

auto WriteTextFile(const std::filesystem::path& InPath,
                   const std::string& InText,
                   std::string* OutError) -> bool {
    std::error_code ec;
    std::filesystem::create_directories(InPath.parent_path(), ec);
    if (ec) {
        if (OutError != nullptr) {
            *OutError = "cannot create parent directories";
        }
        return false;
    }

    std::ofstream out(InPath, std::ios::trunc | std::ios::binary);
    if (!out) {
        if (OutError != nullptr) {
            *OutError = "cannot open output file";
        }
        return false;
    }
    out << InText;
    if (!out.good()) {
        if (OutError != nullptr) {
            *OutError = "failed while writing output file";
        }
        return false;
    }
    return true;
}

auto ReadTextFile(const std::filesystem::path& InPath) -> std::optional<std::string> {
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
                return InText.substr(InStart, pos - InStart + 1);
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

void PrintExecutedPlanSummary(const std::filesystem::path& InPlanPath, const int InMaxCommits = 10) {
    const auto payload = ReadTextFile(InPlanPath);
    if (!payload.has_value()) {
        std::cerr << "Warning: executed plan summary unavailable: cannot read plan file: "
                  << InPlanPath.generic_string() << "\n";
        return;
    }

    const auto meta = ExtractObjectBodyForKey(*payload, "meta");
    const auto stages = ExtractObjectBodyForKey(*payload, "stages");
    if (!meta.has_value() || !stages.has_value()) {
        std::cerr << "Warning: executed plan summary unavailable: plan schema missing meta/stages\n";
        return;
    }

    const auto planner = ExtractObjectBodyForKey(*meta, "planner");
    const auto planId = ExtractStringField(*meta, "plan_id").value_or("-");
    const auto generated = ExtractStringField(*meta, "generated_at_utc").value_or("-");
    const auto executed = ExtractStringField(*meta, "executed_at_utc").value_or("-");
    const auto provider = planner.has_value() ? ExtractStringField(*planner, "provider").value_or("-") : "-";
    const auto model = planner.has_value() ? ExtractStringField(*planner, "ai-model").value_or("-") : "-";

    std::cout << "=== plan summary ===\n";
    std::cout << "[plan] file: " << InPlanPath.generic_string() << "\n";
    std::cout << "[plan] meta: plan_id=" << planId
              << " generated=" << generated
              << " executed=" << executed
              << " provider=" << provider
              << " ai-model=" << model << "\n";

    const auto commitArray = ExtractArrayBodyForKey(*stages, "commit").value_or(std::string{});
    std::size_t repoCount = 0;
    std::size_t commitCount = 0;
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
            lines.push_back("[plan] - " + repo + ": " + msg);
        }
    }
    std::cout << "[plan] commits: repos=" << repoCount << " total=" << commitCount << "\n";
    const auto limit = std::min<std::size_t>(lines.size(), static_cast<std::size_t>(std::max(InMaxCommits, 0)));
    for (std::size_t i = 0; i < limit; ++i) {
        std::cout << lines[i] << "\n";
    }
}

auto StampCommitPlanExecutedAt(const std::filesystem::path& InPath,
                               std::string* OutError) -> bool {
    std::ifstream in(InPath, std::ios::in | std::ios::binary);
    if (!in) {
        if (OutError != nullptr) {
            *OutError = "cannot open plan file";
        }
        return false;
    }

    std::ostringstream buffer;
    buffer << in.rdbuf();
    auto text = buffer.str();
    if (text.empty()) {
        if (OutError != nullptr) {
            *OutError = "plan file is empty";
        }
        return false;
    }

    const auto executedAt = CurrentUtcTimestampIso8601();
    const std::regex executedPattern(R"("executed_at_utc"\s*:\s*"[^"]*")");
    if (std::regex_search(text, executedPattern)) {
        text = std::regex_replace(
            text,
            executedPattern,
            std::string("\"executed_at_utc\": \"") + executedAt + "\"",
            std::regex_constants::format_first_only);
    } else {
        const std::regex metaPattern(R"("meta"\s*:\s*\{)");
        if (std::regex_search(text, metaPattern)) {
            text = std::regex_replace(
                text,
                metaPattern,
                std::string("\"meta\": {\n    \"executed_at_utc\": \"") + executedAt + "\",",
                std::regex_constants::format_first_only);
        } else {
            if (OutError != nullptr) {
                *OutError = "cannot locate meta object in plan";
            }
            return false;
        }
    }

    return WriteTextFile(InPath, text, OutError);
}

auto RunPipelineSafetyGatesForNonAiCommitPush(const std::filesystem::path& InWorkspaceRoot) -> void {
    const auto workspaceRoot = InWorkspaceRoot.lexically_normal();

    const auto allowIgnoreGate = ToLower(Trim(std::getenv("KOG_ALLOW_IGNORE_GATE") == nullptr ? "" : std::getenv("KOG_ALLOW_IGNORE_GATE")));
    const auto ignoreGateMode = ToLower(Trim(std::getenv("KOG_IGNORE_GATE") == nullptr ? "on" : std::getenv("KOG_IGNORE_GATE")));
    if (!(allowIgnoreGate == "1" || allowIgnoreGate == "true") && ignoreGateMode != "off") {
        const auto allowlistPath =
            (ResolveSkillRoot(workspaceRoot) / "assets" / "ignore-sources" / "local" / "ignore-gate-allowlist.txt").lexically_normal();
        const auto allowlist = LoadNormalizedLineSet(allowlistPath);

        auto repos = DiscoverWorkspaceRepos(workspaceRoot);
        std::vector<std::string> findings;
        findings.reserve(20);
        for (const auto& repo : repos) {
            const auto rel = repo.lexically_relative(workspaceRoot).generic_string();
            const auto repoLabel = rel.empty() ? "." : rel;
            const auto untracked = shell::ExecuteCommand("git", {"ls-files", "--others", "--exclude-standard"}, shell::ExecMode::Capture, repo);
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
                const auto key = ToLower(repoLabel == "." ? p : (repoLabel + "/" + p));
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
            std::cerr << "Error: ignore gate failed (commit-push); unresolved untracked artifact-like files detected.\n";
            for (const auto& f : findings) {
                std::cerr << "  - " << f << "\n";
            }
            std::cerr << "Hint: update .gitignore first, then regenerate plan.\n";
            std::cerr << "Hint: override once with --allow-ignore-gate (or KOG_ALLOW_IGNORE_GATE=1).\n";
            std::exit(3);
        }
    }

    const auto disableSecretGate = ToLower(Trim(std::getenv("KOG_DISABLE_SECRET_GATE") == nullptr ? "" : std::getenv("KOG_DISABLE_SECRET_GATE")));
    if (disableSecretGate == "1" || disableSecretGate == "true") {
        return;
    }

    const auto rulesPath = (ResolveSkillRoot(workspaceRoot) / "assets" / "security" / "secret-blacklist.rules").lexically_normal();
    const auto rules = LoadSecretRules(rulesPath);
    if (rules.empty()) {
        return;
    }
    auto repos = DiscoverWorkspaceRepos(workspaceRoot);
    std::vector<SecretFinding> findings;
    findings.reserve(20);
    for (const auto& repo : repos) {
        const auto changedFiles = CollectChangedCandidateFiles(repo);
        if (changedFiles.empty()) {
            continue;
        }
        const auto rel = repo.lexically_relative(workspaceRoot).generic_string();
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
        std::cerr << "Error: secret gate failed (commit-push); potential secrets detected.\n";
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

auto RepoHasAnyWorkingTreeChanges(const std::filesystem::path& InRepoRoot) -> bool {
    const auto status = shell::ExecuteCommand("git", {"status", "--porcelain"}, shell::ExecMode::Capture, InRepoRoot);
    return status.exitCode == 0 && !Trim(status.stdoutStr).empty();
}

auto PlanStageLikelyNonEmpty(const std::filesystem::path& InPlanFile, const std::string& InStageKey) -> bool {
    std::ifstream in(InPlanFile, std::ios::in | std::ios::binary);
    if (!in) {
        // Conservative fallback: if we cannot inspect the file, keep existing execution path.
        return true;
    }
    std::ostringstream oss;
    oss << in.rdbuf();
    const auto text = oss.str();
    if (text.empty()) {
        return true;
    }

    const std::regex emptyStagePattern(
        "\"" + InStageKey + "\"\\s*:\\s*\\[\\s*\\]",
        std::regex::icase);
    return !std::regex_search(text, emptyStagePattern);
}

auto NeedsPostSyncCommitNonPlan(const std::filesystem::path& InWorkspaceRoot,
                                const std::vector<std::string>& InRepoList,
                                const bool InNoRecursive) -> bool {
    if (!InRepoList.empty()) {
        for (const auto& repo : InRepoList) {
            const auto repoRoot = ResolveRepoFromSpec(InWorkspaceRoot, std::filesystem::path(repo), 12, true);
            if (RepoHasAnyWorkingTreeChanges(repoRoot)) {
                return true;
            }
        }
        return false;
    }

    if (InNoRecursive) {
        return RepoHasAnyWorkingTreeChanges(InWorkspaceRoot);
    }

    const auto repos = DiscoverWorkspaceRepos(InWorkspaceRoot);
    for (const auto& repo : repos) {
        if (RepoHasAnyWorkingTreeChanges(repo)) {
            return true;
        }
    }
    return false;
}

auto CollectPostSyncCandidateRepos(const std::filesystem::path& InWorkspaceRoot,
                                   const std::vector<std::string>& InRepoList,
                                   const bool InNoRecursive) -> std::vector<std::filesystem::path> {
    std::vector<std::filesystem::path> repos;
    if (!InRepoList.empty()) {
        repos.reserve(InRepoList.size());
        for (const auto& repo : InRepoList) {
            repos.push_back(ResolveRepoFromSpec(InWorkspaceRoot, std::filesystem::path(repo), 12, true));
        }
        return repos;
    }
    if (InNoRecursive) {
        repos.push_back(InWorkspaceRoot.lexically_normal());
        return repos;
    }
    return DiscoverWorkspaceRepos(InWorkspaceRoot);
}

auto ParsePorcelainChangedPaths(const std::string& InStatusPorcelain) -> std::vector<std::string> {
    std::vector<std::string> out;
    std::istringstream iss(InStatusPorcelain);
    std::string line;
    while (std::getline(iss, line)) {
        if (line.size() < 4) {
            continue;
        }
        if (line.rfind("?? ", 0) == 0) {
            // Untracked file means it is not a pure gitlink-pointer-only update.
            return {};
        }
        std::string path = Trim(line.substr(3));
        if (path.empty()) {
            continue;
        }
        const auto renamePos = path.find(" -> ");
        if (renamePos != std::string::npos) {
            path = Trim(path.substr(renamePos + 4));
        }
        if (!path.empty()) {
            out.push_back(path);
        }
    }
    return out;
}

auto IsGitlinkPathInHead(const std::filesystem::path& InRepoRoot, const std::string& InPath) -> bool {
    const auto tree = shell::ExecuteCommand("git", {"ls-tree", "HEAD", "--", InPath}, shell::ExecMode::Capture, InRepoRoot);
    if (tree.exitCode != 0) {
        return false;
    }
    const auto out = Trim(tree.stdoutStr);
    if (out.empty()) {
        return false;
    }
    // Expected line starts with mode; gitlink mode is 160000.
    return out.rfind("160000 ", 0) == 0;
}

auto CollectGitlinkOnlyChangedPaths(const std::filesystem::path& InRepoRoot) -> std::vector<std::string> {
    const auto status = shell::ExecuteCommand("git", {"status", "--porcelain"}, shell::ExecMode::Capture, InRepoRoot);
    if (status.exitCode != 0) {
        return {};
    }
    const auto& porcelain = status.stdoutStr;
    if (Trim(porcelain).empty()) {
        return {};
    }
    const auto changedPaths = ParsePorcelainChangedPaths(porcelain);
    if (changedPaths.empty()) {
        return {};
    }
    for (const auto& path : changedPaths) {
        if (!IsGitlinkPathInHead(InRepoRoot, path)) {
            return {};
        }
    }
    return changedPaths;
}

auto BuildNestedRepoWaves(const std::vector<std::filesystem::path>& InRepos) -> std::vector<std::vector<std::filesystem::path>> {
    const std::size_t count = InRepos.size();
    if (count <= 1) {
        return {InRepos};
    }

    std::vector<int> indegree(count, 0);
    std::vector<std::vector<std::size_t>> reverseEdges(count);
    for (std::size_t parent = 0; parent < count; ++parent) {
        for (std::size_t child = 0; child < count; ++child) {
            if (parent == child) {
                continue;
            }
            if (IsParentPath(InRepos[parent], InRepos[child])) {
                indegree[parent] += 1;
                reverseEdges[child].push_back(parent);
            }
        }
    }

    std::vector<std::size_t> frontier;
    frontier.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        if (indegree[i] == 0) {
            frontier.push_back(i);
        }
    }

    std::vector<std::vector<std::filesystem::path>> waves;
    std::size_t processed = 0;
    while (!frontier.empty()) {
        std::sort(frontier.begin(), frontier.end(), [&](const std::size_t A, const std::size_t B) {
            return InRepos[A].lexically_normal().generic_string() < InRepos[B].lexically_normal().generic_string();
        });

        std::vector<std::filesystem::path> wave;
        std::vector<std::size_t> next;
        for (const auto idx : frontier) {
            wave.push_back(InRepos[idx]);
            processed += 1;
            for (const auto dependent : reverseEdges[idx]) {
                indegree[dependent] -= 1;
                if (indegree[dependent] == 0) {
                    next.push_back(dependent);
                }
            }
        }
        waves.push_back(std::move(wave));
        frontier = std::move(next);
    }

    if (processed != count) {
        return {InRepos};
    }
    return waves;
}

auto RepoCurrentBranch(const std::filesystem::path& InRepoRoot) -> std::string {
    const auto branch = shell::ExecuteCommand("git", {"symbolic-ref", "--quiet", "--short", "HEAD"}, shell::ExecMode::Capture, InRepoRoot);
    if (branch.exitCode != 0) {
        return {};
    }
    return Trim(branch.stdoutStr);
}

auto RepoHasRemote(const std::filesystem::path& InRepoRoot, const std::string& InRemote) -> bool {
    return shell::ExecuteCommand("git", {"remote", "get-url", InRemote}, shell::ExecMode::Capture, InRepoRoot).exitCode == 0;
}

auto RepoPushRemotes(const std::filesystem::path& InRepoRoot) -> std::vector<std::string> {
    std::vector<std::string> remotes;
    if (RepoHasRemote(InRepoRoot, "origin-ssh")) {
        remotes.push_back("origin-ssh");
    }
    if (RepoHasRemote(InRepoRoot, "origin-http")) {
        remotes.push_back("origin-http");
    }
    if (RepoHasRemote(InRepoRoot, "origin")) {
        remotes.push_back("origin");
    }
    return remotes;
}

auto RepoHasCommitsToPushToRemote(const std::filesystem::path& InRepoRoot,
                                  const std::string& InRemote,
                                  const std::string& InBranch) -> bool {
    if (InRemote.empty() || InBranch.empty()) {
        return false;
    }
    const auto remoteRef = std::format("refs/remotes/{}/{}", InRemote, InBranch);
    const auto localRef = std::format("refs/heads/{}", InBranch);
    const auto hasRemoteRef =
        shell::ExecuteCommand("git", {"show-ref", "--verify", "--quiet", remoteRef}, shell::ExecMode::Capture, InRepoRoot).exitCode == 0;
    if (!hasRemoteRef) {
        return true;
    }
    const auto ahead =
        shell::ExecuteCommand("git", {"rev-list", "--count", std::format("{}..{}", remoteRef, localRef)}, shell::ExecMode::Capture, InRepoRoot);
    if (ahead.exitCode != 0) {
        return true;
    }
    return ParseNonNegativeInt(ahead.stdoutStr) > 0;
}

auto RepoHeadIsUnpublishedAcrossPushRemotes(const std::filesystem::path& InRepoRoot) -> bool {
    const auto branch = RepoCurrentBranch(InRepoRoot);
    if (branch.empty()) {
        return false;
    }
    const auto remotes = RepoPushRemotes(InRepoRoot);
    if (remotes.empty()) {
        return false;
    }
    for (const auto& remote : remotes) {
        if (!RepoHasCommitsToPushToRemote(InRepoRoot, remote, branch)) {
            return false;
        }
    }
    return true;
}

auto BuildGitlinkOnlyFollowupCommitMessage(const std::filesystem::path& InWorkspaceRoot,
                                           const std::filesystem::path& InRepoRoot) -> std::string {
    const auto relative = RelativeDisplayPath(InWorkspaceRoot, InRepoRoot).generic_string();
    if (relative.empty() || relative == ".") {
        return "chore(workspace): update submodule pointers";
    }
    return std::format("chore({}): update submodule pointers", RepoNameFromPath(InRepoRoot));
}

enum class PostSyncDeltaKind {
    None,
    GitlinkOnly,
    SemanticDrift,
};

struct PostSyncDeltaSummary {
    PostSyncDeltaKind kind = PostSyncDeltaKind::None;
    int gitlinkOnlyRepoCount = 0;
    std::vector<std::filesystem::path> semanticRepos;
};

auto ClassifyPostSyncDelta(const std::filesystem::path& InWorkspaceRoot,
                           const std::vector<std::string>& InRepoList,
                           const bool InNoRecursive) -> PostSyncDeltaSummary {
    PostSyncDeltaSummary summary;
    const auto repos = CollectPostSyncCandidateRepos(InWorkspaceRoot, InRepoList, InNoRecursive);
    for (const auto& repo : repos) {
        const auto status = shell::ExecuteCommand("git", {"status", "--porcelain"}, shell::ExecMode::Capture, repo);
        if (status.exitCode != 0) {
            continue;
        }
        const auto& porcelain = status.stdoutStr;
    if (Trim(porcelain).empty()) {
            continue;
        }
        const auto gitlinkPaths = CollectGitlinkOnlyChangedPaths(repo);
        if (!gitlinkPaths.empty()) {
            summary.gitlinkOnlyRepoCount += 1;
            continue;
        }
        summary.semanticRepos.push_back(repo);
    }
    if (!summary.semanticRepos.empty()) {
        summary.kind = PostSyncDeltaKind::SemanticDrift;
    } else if (summary.gitlinkOnlyRepoCount > 0) {
        summary.kind = PostSyncDeltaKind::GitlinkOnly;
    }
    return summary;
}

auto AutoAmendGitlinkOnlyPostSyncRepos(const std::filesystem::path& InWorkspaceRoot,
                                       const std::vector<std::string>& InRepoList,
                                       const bool InNoRecursive) -> std::pair<int, int> {
    int updatedCount = 0;
    const auto repoWaves = BuildNestedRepoWaves(CollectPostSyncCandidateRepos(InWorkspaceRoot, InRepoList, InNoRecursive));
    for (const auto& wave : repoWaves) {
        for (const auto& repo : wave) {
            const auto gitlinkPaths = CollectGitlinkOnlyChangedPaths(repo);
            if (gitlinkPaths.empty()) {
                continue;
            }
            std::vector<std::string> addArgs = {"add", "--"};
            addArgs.insert(addArgs.end(), gitlinkPaths.begin(), gitlinkPaths.end());
            const auto addResult = shell::ExecuteCommand("git", addArgs, shell::ExecMode::PassThrough, repo);
            if (addResult.exitCode != 0) {
                std::cerr << "[commit-push] post-sync gitlink-only auto-amend failed: git add -- <gitlink paths> failed in "
                          << repo.generic_string() << "\n";
                return {-1, updatedCount};
            }

            if (RepoHeadIsUnpublishedAcrossPushRemotes(repo)) {
                const auto amendResult = shell::ExecuteCommand("git", {"commit", "--amend", "--no-edit"}, shell::ExecMode::PassThrough, repo);
                if (amendResult.exitCode != 0) {
                    std::cerr << "[commit-push] post-sync gitlink-only auto-amend failed: git commit --amend --no-edit failed in "
                              << repo.generic_string() << "\n";
                    return {-1, updatedCount};
                }
            } else {
                const auto commitResult = shell::ExecuteCommand(
                    "git",
                    {"commit", "-m", BuildGitlinkOnlyFollowupCommitMessage(InWorkspaceRoot, repo)},
                    shell::ExecMode::PassThrough,
                    repo);
                if (commitResult.exitCode != 0) {
                    std::cerr << "[commit-push] post-sync gitlink-only follow-up commit failed in "
                              << repo.generic_string() << "\n";
                    return {-1, updatedCount};
                }
            }
            updatedCount += 1;
        }
    }
    return {0, updatedCount};
}

auto RunCommitPushPlanFilePipelineImpl(const std::filesystem::path& InWorkspaceRoot,
                                       const std::string& InNormalizedPlanFile,
                                       const std::vector<std::string>& InExtraArgs) -> int {
    const bool agentMode = IsAgentModeEnabled();
    if (InNormalizedPlanFile.empty()) {
        std::cerr << "Error: plan pipeline requires non-empty --plan-file\n";
        return 2;
    }
    std::cout << "[commit-push] using plan file: " << InNormalizedPlanFile << "\n";

    if (!InExtraArgs.empty()) {
        std::cerr << "Error: unsupported extra arguments in plan pipeline mode:";
        for (const auto& extra : InExtraArgs) {
            std::cerr << ' ' << extra;
        }
        std::cerr << "\n";
        return 2;
    }

    const auto planPath = std::filesystem::path(InNormalizedPlanFile).lexically_normal();

    if (agentMode && IsSharedDefaultPlanPath(InWorkspaceRoot, planPath)) {
        const auto refreshCode = EnsureAgentSharedPlanFresh(InWorkspaceRoot, planPath);
        if (refreshCode != 0) {
            return refreshCode;
        }
    }

    {
        std::string stampError;
        if (!StampCommitPlanExecutedAt(planPath, &stampError)) {
            std::cerr << "Warning: failed to stamp plan executed_at_utc: " << planPath.generic_string();
            if (!stampError.empty()) {
                std::cerr << " (" << stampError << ")";
            }
            std::cerr << "\n";
        }
    }

    if (agentMode) {
        std::cout << "[commit-push] agent mode + --plan-file detected; using plan-driven flow.\n";
    }

    std::cout << "=== commit-push stage: safety-gates ===\n";
    RunPipelineSafetyGatesForNonAiCommitPush(InWorkspaceRoot);

    std::cout << "=== commit-push stage: pre-commit ===\n";
    {
        const auto preCommitCode = RunSyncPreCommitNative(InWorkspaceRoot, true, false, "default");
        if (preCommitCode != 0) {
            return preCommitCode;
        }
    }

    const bool hasWorkingChanges = NeedsPostSyncCommitNonPlan(InWorkspaceRoot, {}, false);
    if (!hasWorkingChanges) {
        std::cout << "[commit-push] workspace clean; skipping commit/sync/post-sync and proceeding to push check.\n";
    } else {
        std::cout << "=== commit-push stage: commit ===\n";
        {
            const auto commitCode = RunCommitNativePlanStage(InWorkspaceRoot, InNormalizedPlanFile, "commit", false);
            if (commitCode != 0) {
                return commitCode;
            }
        }

        std::cout << "=== commit-push stage: sync ===\n";
        {
            const auto syncCode = RunSyncOriginLatestNative(InWorkspaceRoot, true, false);
            if (syncCode != 0) {
                return syncCode;
            }
        }

        std::cout << "=== commit-push stage: post-sync ===\n";
        {
            const auto summary = ClassifyPostSyncDelta(InWorkspaceRoot, {}, false);
            if (summary.kind == PostSyncDeltaKind::SemanticDrift) {
                std::cerr << "[commit-push] post-sync semantic drift detected after sync; manual review required.\n";
                for (const auto& repo : summary.semanticRepos) {
                    std::cerr << "  repo: " << repo.generic_string() << "\n";
                }
                return 2;
            }
            if (summary.kind == PostSyncDeltaKind::GitlinkOnly) {
                const auto amendResult = AutoAmendGitlinkOnlyPostSyncRepos(InWorkspaceRoot, {}, false);
                if (amendResult.first != 0) {
                    return 2;
                }
                std::cout << "[commit-push] post-sync gitlink-only auto-amend applied: repos=" << amendResult.second << "\n";
            } else {
                const bool hasPostSyncStage = PlanStageLikelyNonEmpty(std::filesystem::path(InNormalizedPlanFile), "post_sync");
                if (!hasPostSyncStage) {
                    std::cout << "[commit-push] post-sync plan stage is empty; skipping.\n";
                } else if (!NeedsPostSyncCommitNonPlan(InWorkspaceRoot, {}, false)) {
                    std::cout << "[commit-push] post-sync plan commit skipped (no working tree changes).\n";
                } else {
                    const auto postCommitCode = RunCommitNativePlanStage(InWorkspaceRoot, InNormalizedPlanFile, "post_sync", false);
                    if (postCommitCode != 0) {
                        return postCommitCode;
                    }
                }
            }
        }
    }

    std::cout << "=== commit-push stage: push ===\n";
    {
        const auto pushCode = RunPushNativeSimple(InWorkspaceRoot, true, false, false, false, false, 0, false, "");
        PrintExecutedPlanSummary(std::filesystem::path(InNormalizedPlanFile).lexically_normal(), 10);
        return pushCode;
    }
}

} // namespace

auto RunCommitPushPlanFilePipeline(const std::filesystem::path& InWorkspaceRoot,
                                   const std::string& InNormalizedPlanFile,
                                   const std::vector<std::string>& InExtraArgs) -> int {
    return RunCommitPushPlanFilePipelineImpl(InWorkspaceRoot, InNormalizedPlanFile, InExtraArgs);
}

void RegisterCommitPush(CLI::App& InApp) {
    auto* cmd = InApp.add_subcommand("commit-push", "Run pre-commit, commit, sync, post-sync, then push in one command");
    cmd->allow_extras();

    auto* repos = new std::string{};
    auto* noRecursive = new bool{false};
    auto* message = new std::string{};
    auto* commitPlanFile = new std::string{};
    auto* writeCommitPlanTemplate = new bool{false};
    auto* commitPlanOut = new std::string{};
    auto* aiProvider = new std::string{};
    auto* aiModel = new std::string{};
    auto* aiAuto = new bool{false};
    auto* noAiReview = new bool{false};
    auto* stagedOnly = new bool{false};
    auto* dryRun = new bool{false};
    auto* profile = new bool{false};
    auto* branchMode = new std::string{"default"};
    auto* forceWithLease = new bool{false};
    auto* noVerify = new bool{false};
    auto* jobs = new int{0};
    auto* verbose = new bool{false};
    auto* remote = new std::string{};
    auto* repoRoot = new std::string{};
    auto* target = new std::string{};

    cmd->add_option("--repos", *repos, "Target repos (comma-separated)");
    cmd->add_option("--repo-root", *repoRoot, "Workspace root/start path used for repo-name lookup");
    cmd->add_option("target", *target, "Optional repo target root (repo name or relative path)")->required(false);
    cmd->add_flag("--no-recursive,-N", *noRecursive, "Only operate on current repository (or provided --repos)");
    cmd->add_option("--message,-m", *message, "Commit message (skip AI generation)");
    cmd->add_option("--plan-file", *commitPlanFile, "Plan JSON file (stage-aware)");
    cmd->add_flag("--write-plan-template", *writeCommitPlanTemplate, "Write plan template JSON and exit");
    cmd->add_option("--plan-out", *commitPlanOut, "Template output path (default: .kano/cache/git/plans/plan-<utc>-<head>.json)");
    cmd->add_option("--ai-provider", *aiProvider, "AI provider for commit (copilot, codex, opencode)");
    cmd->add_option("--ai-model", *aiModel, "AI model for commit");
    cmd->add_flag("--ai-auto", *aiAuto, "Enable commit AI auto mode");
    cmd->add_flag("--no-ai-review", *noAiReview, "Skip AI review gate for commit");
    cmd->add_flag("--staged-only", *stagedOnly, "Commit only staged changes");
    cmd->add_flag("--dry-run", *dryRun, "Preview commit/sync/push actions without modifying repositories");
    cmd->add_flag("--profile", *profile, "Print commit-push stage timing summary");
    cmd->add_option("--branch-mode", *branchMode, "Detached branch inference mode for pre-commit: default|stable-dev");
    cmd->add_flag("--force-with-lease", *forceWithLease, "Use force-with-lease for push");
    cmd->add_flag("--no-verify", *noVerify, "Pass --no-verify to push");
    cmd->add_option("--jobs", *jobs, "Push parallel workers");
    cmd->add_flag("--verbose", *verbose, "Verbose push output");
    cmd->add_option("--remote", *remote, "Remote filter for push");

    cmd->callback([=]() {
        const auto totalStart = std::chrono::steady_clock::now();
        long long preCommitMillis = 0;
        long long commitMillis = 0;
        long long syncMillis = 0;
        long long postSyncMillis = 0;
        long long pushMillis = 0;

        const auto extras = cmd->remaining();
        if (!extras.empty()) {
            std::cerr << "Error: unsupported extra arguments in commit-push mode:";
            for (const auto& extra : extras) {
                std::cerr << ' ' << extra;
            }
            std::cerr << "\n";
            std::exit(2);
        }

        const auto invocationRoot = repoRoot->empty() ? std::filesystem::current_path() : std::filesystem::path(*repoRoot);
        const auto workspaceRoot = target->empty()
            ? invocationRoot.lexically_normal()
            : ResolveRepoFromSpec(invocationRoot.lexically_normal(), std::filesystem::path(*target), 12, true);
        if (!repos->empty() && !target->empty()) {
            std::cerr << "Error: positional target cannot be combined with --repos\n";
            std::exit(2);
        }
        const auto repoList = ResolveRepoList(workspaceRoot, ParseReposCsv(*repos));
        bool effectiveNoRecursive = *noRecursive;
        if (!effectiveNoRecursive && repoList.empty() && !target->empty()) {
            const auto scopedRepos = DiscoverWorkspaceRepos(workspaceRoot);
            if (scopedRepos.size() <= 1) {
                effectiveNoRecursive = true;
            }
        }

        if (*writeCommitPlanTemplate) {
            const auto outPath = commitPlanOut->empty()
                ? DefaultCommitPlanOutputPath(workspaceRoot)
                : std::filesystem::path(NormalizeInputPathForCurrentPlatform(*commitPlanOut)).lexically_normal();
            std::string error;
            if (!WriteTextFile(outPath, BuildCommitPlanTemplateJson(CurrentUtcTimestampIso8601()), &error)) {
                std::cerr << "Error: failed to write plan template to " << outPath.generic_string();
                if (!error.empty()) {
                    std::cerr << " (" << error << ")";
                }
                std::cerr << "\n";
                std::exit(2);
            }
            std::cout << "Wrote plan template: " << outPath.generic_string() << "\n";
            std::exit(0);
        }
        if (!commitPlanOut->empty()) {
            std::cerr << "Error: --plan-out requires --write-plan-template\n";
            std::exit(2);
        }

        const auto normalizedCommitPlanFile = NormalizeInputPathForCurrentPlatform(*commitPlanFile);
        const bool hasCommitPlan = !normalizedCommitPlanFile.empty();
        const bool agentMode = IsAgentModeEnabled();
        const bool aiModeRequested = *aiAuto || !aiProvider->empty() || !aiModel->empty();
        const bool autoPlanAiMode = aiModeRequested && !hasCommitPlan && message->empty();
        const bool hasWorkingChangesAtStart = NeedsPostSyncCommitNonPlan(workspaceRoot, repoList, effectiveNoRecursive);
        const bool effectiveAiModeRequested = aiModeRequested && !(autoPlanAiMode && !hasWorkingChangesAtStart);
        if (agentMode && autoPlanAiMode) {
            std::cerr << "Error: agent mode cpa/commit-push cannot invoke internal AI auto-plan.\n";
            std::cerr << "Hint: prepare the default plan file first, then run agent mode cpa/commit-push with --plan-file.\n";
            std::cerr << "Hint: bare `KANO_AGENT_MODE=1 kog cpa` is reserved for the agent-prepared default plan path.\n";
            std::exit(2);
        }
        if (agentMode && !hasCommitPlan && message->empty() && !autoPlanAiMode) {
            std::cerr << "Error: agent mode commit-push requires either --plan-file or --message/-m.\n";
            std::cerr << "Hint: prepare/fill plan first, then run with --plan-file.\n";
            std::cerr << "Hint: or provide explicit --message/-m for single-commit non-plan flow.\n";
            std::exit(2);
        }
        if (hasCommitPlan && aiModeRequested) {
            std::cerr << "Error: --plan-file cannot be combined with --ai-* flags.\n";
            std::cerr << "Hint: fill plan first (kog plan runbook commit), then run commit-push with --plan-file only.\n";
            std::exit(2);
        }
        if (hasCommitPlan && !message->empty()) {
            std::cerr << "Error: --plan-file cannot be combined with --message/-m.\n";
            std::cerr << "Hint: set commit messages in plan entries.\n";
            std::exit(2);
        }
        if (hasCommitPlan && *stagedOnly) {
            std::cerr << "Error: --plan-file cannot be combined with --staged-only.\n";
            std::cerr << "Hint: plan apply handles staging per plan entries.\n";
            std::exit(2);
        }
        if (hasCommitPlan && *noAiReview) {
            std::cerr << "Error: --plan-file cannot be combined with --no-ai-review.\n";
            std::cerr << "Hint: AI review policy is captured when plan is prepared.\n";
            std::exit(2);
        }
        if (hasCommitPlan && *dryRun) {
            std::cerr << "Error: --plan-file cannot be combined with --dry-run yet.\n";
            std::cerr << "Hint: use `kog plan verify pre-apply --plan-file <file>` for no-write validation.\n";
            std::exit(2);
        }

        if (autoPlanAiMode) {
            if (!repoList.empty() || *noRecursive || *dryRun || *profile || *branchMode != "default" ||
                *forceWithLease || *noVerify || *jobs > 0 || *verbose || !remote->empty()) {
                std::cerr << "Error: full-auto plan mode currently supports only the default `kog cpa` shape.\n";
                std::cerr << "Hint: use plain `kog cpa`, or run `kog plan runbook commit` then `kog commit-push --plan-file <file>`.\n";
                std::exit(2);
            }

            if (!hasWorkingChangesAtStart) {
                std::cout << "[commit-push] workspace clean; skip AI commit runbook (no-op) and proceed to push check.\n";
            } else {
                const auto autoPlanPath = DefaultSharedPlanPath(workspaceRoot);
                std::cout << "[commit-push] full-auto plan file: " << autoPlanPath.generic_string() << "\n";
                const auto planNewCode = RunPlanNewViaSelf(workspaceRoot, autoPlanPath);
                if (planNewCode != 0) {
                    std::exit(planNewCode);
                }
                const auto ignoreRunbookCode = RunIgnorePlanRunbookViaSelf(workspaceRoot, autoPlanPath);
                if (ignoreRunbookCode != 0) {
                    std::exit(ignoreRunbookCode);
                }
                if (PlanStageLikelyNonEmpty(autoPlanPath, "ignore")) {
                    const auto ignoreApplyCode = RunIgnorePlanApplyViaSelf(workspaceRoot, autoPlanPath);
                    if (ignoreApplyCode != 0) {
                        std::exit(ignoreApplyCode);
                    }
                } else {
                    std::cout << "[commit-push] ignore plan stage is empty; skipping ignore apply.\n";
                }
                const auto runbookCode = RunCommitPlanRunbookViaSelf(workspaceRoot, autoPlanPath, *aiProvider, *aiModel);
                if (runbookCode != 0) {
                    std::exit(runbookCode);
                }
                const auto pipelineCode = RunCommitPushPlanFilePipeline(workspaceRoot, autoPlanPath.generic_string(), {});
                std::exit(pipelineCode);
            }
        }

        const bool canUsePlanPipelineFastPath = hasCommitPlan &&
                                                repos->empty() &&
                                                !*noRecursive &&
                                                message->empty() &&
                                                !*dryRun &&
                                                !*profile &&
                                                *branchMode == "default" &&
                                                !*forceWithLease &&
                                                !*noVerify &&
                                                *jobs <= 0 &&
                                                !*verbose &&
                                                remote->empty();

        if (canUsePlanPipelineFastPath) {
            const auto code = RunCommitPushPlanFilePipeline(workspaceRoot, normalizedCommitPlanFile, {});
            std::exit(code);
        }

        if (hasCommitPlan) {
            std::string stampError;
            const auto planPath = std::filesystem::path(normalizedCommitPlanFile).lexically_normal();
            if (!StampCommitPlanExecutedAt(planPath, &stampError)) {
                std::cerr << "Warning: failed to stamp plan executed_at_utc: "
                          << planPath.generic_string();
                if (!stampError.empty()) {
                    std::cerr << " (" << stampError << ")";
                }
                std::cerr << "\n";
            }
        }

        if (agentMode && hasCommitPlan) {
            std::cout << "[commit-push] agent mode + --plan-file detected; using plan-driven flow.\n";
        }

        if (!effectiveAiModeRequested) {
            std::cout << "=== commit-push stage: safety-gates ===\n";
            RunPipelineSafetyGatesForNonAiCommitPush(workspaceRoot);
        }

        std::cout << "=== commit-push stage: pre-commit ===\n";
        const auto preCommitStart = std::chrono::steady_clock::now();
        if (!repoList.empty()) {
            for (const auto& repo : repoList) {
                const auto repoRoot = (workspaceRoot / std::filesystem::path(repo)).lexically_normal();
                const auto preCommitCode = RunSyncPreCommitNative(repoRoot, false, *dryRun, *branchMode);
                if (preCommitCode != 0) {
                    std::exit(preCommitCode);
                }
            }
        } else {
            const auto preCommitCode = RunSyncPreCommitNative(workspaceRoot, !*noRecursive, *dryRun, *branchMode);
            if (preCommitCode != 0) {
                std::exit(preCommitCode);
            }
        }
        const auto preCommitEnd = std::chrono::steady_clock::now();
        preCommitMillis = std::chrono::duration_cast<std::chrono::milliseconds>(preCommitEnd - preCommitStart).count();

        const bool hasWorkingChanges = NeedsPostSyncCommitNonPlan(workspaceRoot, repoList, effectiveNoRecursive);
        if (!hasWorkingChanges) {
            std::cout << "[commit-push] workspace clean; skipping commit/sync/post-sync and proceeding to push check.\n";
        } else {
            std::cout << "=== commit-push stage: commit ===\n";
            const auto commitStart = std::chrono::steady_clock::now();
            const auto commitResult = hasCommitPlan
                ? shell::ExecResult{
                    RunCommitNativePlanStage(workspaceRoot, normalizedCommitPlanFile, "commit", false), "", ""}
                : shell::ExecResult{
                    RunCommitNativeSimple(
                        workspaceRoot,
                        *repos,
                        effectiveNoRecursive,
                        *message,
                        *stagedOnly,
                        *dryRun,
                        *aiProvider,
                        *aiModel,
                        *aiAuto,
                        *noAiReview,
                        false),
                    "", ""};
            const auto commitEnd = std::chrono::steady_clock::now();
            commitMillis = std::chrono::duration_cast<std::chrono::milliseconds>(commitEnd - commitStart).count();
            if (commitResult.exitCode != 0) {
                std::exit(commitResult.exitCode);
            }

            std::cout << "=== commit-push stage: sync ===\n";
            const auto syncStart = std::chrono::steady_clock::now();
            if (!repoList.empty()) {
                for (const auto& repo : repoList) {
                    const auto repoRoot = (workspaceRoot / std::filesystem::path(repo)).lexically_normal();
                    const auto syncCode = RunSyncOriginLatestNative(repoRoot, false, *dryRun);
                    if (syncCode != 0) {
                        std::exit(syncCode);
                    }
                }
            } else {
                const auto syncCode = RunSyncOriginLatestNative(workspaceRoot, !effectiveNoRecursive, *dryRun);
                if (syncCode != 0) {
                    std::exit(syncCode);
                }
            }
            const auto syncEnd = std::chrono::steady_clock::now();
            syncMillis = std::chrono::duration_cast<std::chrono::milliseconds>(syncEnd - syncStart).count();

            std::cout << "=== commit-push stage: post-sync ===\n";
            const auto postCommitStart = std::chrono::steady_clock::now();
            shell::ExecResult postCommitResult{0, "", ""};
            if (*dryRun) {
                if (hasCommitPlan) {
                    std::cout << "[commit-push] post-sync plan commit skipped in dry-run mode.\n";
                } else {
                    std::cout << "[commit-push] post-sync commit skipped in dry-run mode.\n";
                }
            } else {
                const auto summary = ClassifyPostSyncDelta(workspaceRoot, repoList, effectiveNoRecursive);
                if (summary.kind == PostSyncDeltaKind::SemanticDrift) {
                    std::cerr << "[commit-push] post-sync semantic drift detected after sync; manual review required.\n";
                    for (const auto& repo : summary.semanticRepos) {
                        std::cerr << "  repo: " << repo.generic_string() << "\n";
                    }
                    std::exit(2);
                }
                if (summary.kind == PostSyncDeltaKind::GitlinkOnly) {
                    const auto amendResult = AutoAmendGitlinkOnlyPostSyncRepos(workspaceRoot, repoList, effectiveNoRecursive);
                    if (amendResult.first != 0) {
                        std::exit(2);
                    }
                    std::cout << "[commit-push] post-sync gitlink-only auto-amend applied: repos=" << amendResult.second << "\n";
                } else if (hasCommitPlan) {
                    const bool hasPostSyncStage = PlanStageLikelyNonEmpty(std::filesystem::path(normalizedCommitPlanFile), "post_sync");
                    if (!hasPostSyncStage) {
                        std::cout << "[commit-push] post-sync plan stage is empty; skipping.\n";
                    } else if (NeedsPostSyncCommitNonPlan(workspaceRoot, repoList, effectiveNoRecursive)) {
                        postCommitResult =
                            shell::ExecResult{RunCommitNativePlanStage(workspaceRoot, normalizedCommitPlanFile, "post_sync", false), "", ""};
                    } else {
                        std::cout << "[commit-push] post-sync plan commit skipped (no working tree changes).\n";
                    }
                } else if (NeedsPostSyncCommitNonPlan(workspaceRoot, repoList, effectiveNoRecursive)) {
                    postCommitResult =
                        shell::ExecResult{
                            RunCommitNativeSimple(
                                workspaceRoot,
                                *repos,
                                effectiveNoRecursive,
                                *message,
                                *stagedOnly,
                                *dryRun,
                                *aiProvider,
                                *aiModel,
                                *aiAuto,
                                *noAiReview,
                                false),
                            "", ""};
                } else {
                    std::cout << "[commit-push] post-sync commit skipped (no working tree changes).\n";
                }
            }
            const auto postCommitEnd = std::chrono::steady_clock::now();
            postSyncMillis = std::chrono::duration_cast<std::chrono::milliseconds>(postCommitEnd - postCommitStart).count();
            if (postCommitResult.exitCode != 0) {
                std::exit(postCommitResult.exitCode);
            }
        }

        std::cout << "=== commit-push stage: push ===\n";
        const auto pushStart = std::chrono::steady_clock::now();
        int pushExitCode = 0;
        if (!repoList.empty()) {
            std::vector<std::string> pushArgs = {"push", "--repos", JoinReposCsv(repoList), "--no-recursive"};
            if (*dryRun) {
                pushArgs.push_back("--dry-run");
            }
            if (*profile) {
                pushArgs.push_back("--profile");
            }
            if (*forceWithLease) {
                pushArgs.push_back("--force-with-lease");
            }
            if (*noVerify) {
                pushArgs.push_back("--no-verify");
            }
            if (*verbose) {
                pushArgs.push_back("--verbose");
            }
            if (!remote->empty()) {
                pushArgs.push_back("--remote");
                pushArgs.push_back(*remote);
            }
            const auto pushResult = shell::ExecuteCommand(SelfBinaryPath(), pushArgs, shell::ExecMode::PassThrough, workspaceRoot);
            pushExitCode = pushResult.exitCode;
        } else {
            // Keep commit-push convergence deterministic and avoid the known
            // parallel push worker hang path. Standalone `kog push` still exposes
            // configurable parallelism for operator-driven use.
            pushExitCode = RunPushNativeSimple(
                workspaceRoot,
                !effectiveNoRecursive,
                *dryRun,
                *profile,
                *forceWithLease,
                *noVerify,
                1,
                *verbose,
                *remote);
        }

        const auto pushEnd = std::chrono::steady_clock::now();
        pushMillis = std::chrono::duration_cast<std::chrono::milliseconds>(pushEnd - pushStart).count();

        if (*profile) {
            const auto totalEnd = std::chrono::steady_clock::now();
            const auto totalMillis = std::chrono::duration_cast<std::chrono::milliseconds>(totalEnd - totalStart).count();
            std::cout << "\n=== Commit-Push Profile Summary ===\n";
            std::cout << "mode: native\n";
            std::cout << "pre_commit_ms: " << preCommitMillis << "\n";
            std::cout << "commit_ms: " << commitMillis << "\n";
            std::cout << "sync_ms: " << syncMillis << "\n";
            std::cout << "post_sync_ms: " << postSyncMillis << "\n";
            std::cout << "push_ms: " << pushMillis << "\n";
            std::cout << "total_ms: " << totalMillis << "\n";
        }

        if (hasCommitPlan) {
            PrintExecutedPlanSummary(std::filesystem::path(normalizedCommitPlanFile).lexically_normal(), 10);
        }

        std::exit(pushExitCode);
    });
}

} // namespace kano::git::commands
