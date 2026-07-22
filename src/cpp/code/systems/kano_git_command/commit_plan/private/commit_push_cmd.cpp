// commit-push command — Orchestrate commit -> sync -> push workflow

#include <CLI/CLI.hpp>
#include "commit_push_cmd.hpp"
#include "commit_push_post_sync.hpp"
#include "command_runtime_ops.hpp"
#include "ai_utils.hpp"
#include "discovery.hpp"
#include "secret_scan_utils.hpp"
#include "shell_executor.hpp"

#include "kog_timing.hpp"

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

auto IsProbableIgnoreArtifactPath(const std::string& InPath) -> bool;
auto IsInternalPipelineArtifactPath(const std::string& InPath) -> bool;
auto ParseStatusChangedPath(const std::string& InLine) -> std::optional<std::string>;

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

        const auto pathsResult = shell::ExecuteCommand(
            "git",
            {"config", "-f", ".gitmodules", "--get-regexp", "^submodule\\..*\\.path$"},
            shell::ExecMode::Capture,
            current);
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

auto DiscoverWorkspaceRepos(
    const std::filesystem::path& InRoot,
    const workspace::WorkspacePolicyFilter InPolicyFilter = workspace::WorkspacePolicyFilter::Commit)
    -> std::vector<std::filesystem::path> {
    workspace::WorkspaceInventoryOptions options;
    options.rootDir = InRoot;
    options.unregisteredDepth = 3;
    options.scope = workspace::DiscoverScope::Full;
    options.useCache = false;
    options.refreshCache = true;
    options.metadataLevel = "minimal";
    options.includeTrustedUnregistered = true;
    auto repos = workspace::DiscoverWorkspaceRepoPaths(options, InPolicyFilter);
    if (repos.empty()) {
        repos.push_back(InRoot.lexically_normal());
    }
    std::sort(repos.begin(), repos.end(), [](const auto& A, const auto& B) {
        return RepoKey(A) < RepoKey(B);
    });
    repos.erase(std::unique(repos.begin(), repos.end(), [](const auto& A, const auto& B) {
        return RepoKey(A) == RepoKey(B);
    }), repos.end());
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

auto CollectIgnoreGateCandidatePaths(const std::filesystem::path& InRepo) -> std::vector<std::string> {
    std::set<std::string> files;

    if (const auto untracked = shell::ExecuteCommand("git", {"ls-files", "--others", "--exclude-standard"}, shell::ExecMode::Capture, InRepo);
        untracked.exitCode == 0) {
        std::istringstream iss(untracked.stdoutStr);
        std::string line;
        while (std::getline(iss, line)) {
            auto path = Trim(line);
            if (!path.empty()) {
                files.insert(path);
            }
        }
    }

    if (const auto status = shell::ExecuteCommand("git", {"status", "--short"}, shell::ExecMode::Capture, InRepo);
        status.exitCode == 0) {
        std::istringstream iss(status.stdoutStr);
        std::string line;
        while (std::getline(iss, line)) {
            const auto maybePath = ParseStatusChangedPath(line);
            if (maybePath.has_value() && !maybePath->empty()) {
                files.insert(*maybePath);
            }
        }
    }

    return std::vector<std::string>(files.begin(), files.end());
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

auto IsAgentModeEnabledLocal() -> bool {
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
    return (InWorkspaceRoot / ".kano" / "tmp" / "git" / "plans" /
            ("plan-" + stamp + "-" + headShort + ".json"))
        .lexically_normal();
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
        if (value.empty()) {
            continue;
        }
        try {
            return std::stoll(value);
        } catch (const std::exception&) {
        }
    }
    return std::nullopt;
}

auto LogAutoPlanStageDetails(const std::string& InStage,
                             const std::string& InCommand,
                             const std::vector<std::pair<std::string, std::string>>& InFields) -> void {
    std::cout << "[commit-push][auto-plan] stage=" << InStage << " start\n";
    std::cout << "[commit-push][auto-plan] command: " << InCommand << "\n";
    for (const auto& [label, value] : InFields) {
        std::cout << "[commit-push][auto-plan] " << label << ": " << value << "\n";
    }
}

struct CommitRunbookResult {
    int exitCode = 0;
    std::optional<long long> aiFillMillis;
};

auto RunCommitPlanRunbookViaSelf(const std::filesystem::path& InWorkspaceRoot,
                                 const std::filesystem::path& InPlanPath,
                                 const std::string& InProvider,
                                 const std::string& InModel,
                                 const std::string& InFillMode,
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
    if (InYolo) {
        args.push_back("--yolo");
    }
    const auto selfBinary = ResolveSelfBinaryCommand();
    const auto workingPlan = (InPlanPath.parent_path() / std::format("{}.ai-working{}", InPlanPath.stem().generic_string(), InPlanPath.extension().generic_string())).lexically_normal();
    const auto promptDir = (InWorkspaceRoot / ".kano" / "tmp" / "git" / "provider-prompts").lexically_normal();
    const auto responseDir = (InWorkspaceRoot / ".kano" / "tmp" / "git" / "ai-responses").lexically_normal();
    LogAutoPlanStageDetails("commit-runbook",
                            FormatCommandLineForLog(selfBinary, args),
                            {{"plan file", InPlanPath.generic_string()},
                             {"working plan", workingPlan.generic_string()},
                             {"prompt dir", promptDir.generic_string()},
                             {"response dir", responseDir.generic_string()}});
    const auto result = shell::ExecuteCommand(selfBinary, args, shell::ExecMode::Capture, InWorkspaceRoot);
    CommitRunbookResult out;
    out.aiFillMillis = ExtractPlanAiFillMillis(result);
    const auto exitCode = FinalizeNestedSelfResult("AI commit runbook", result);
    out.exitCode = exitCode;
    if (exitCode != 0) {
        std::cerr << "Error: AI commit runbook failed via native binary (exit=" << exitCode << ").\n";
        if (ToLower(Trim(InFillMode)) == "single" && std::getenv("KANO_AGENT_MODE") == nullptr) {
            std::cerr << "Hint: human-mode single CPA forbids deterministic commit fallback; resolve the AI fill failure and rerun.\n";
        }
    }
    return out;
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
    const auto selfBinary = ResolveSelfBinaryCommand();
    LogAutoPlanStageDetails("plan-new",
                            FormatCommandLineForLog(selfBinary, args),
                            {{"plan file", InPlanPath.generic_string()}});
    const auto result = shell::ExecuteCommand(selfBinary, args, shell::ExecMode::Capture, InWorkspaceRoot);
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
        // In agent mode, if the plan needs to be filled, exit with 3 to signal the agent
        if (refreshReason.find("placeholder-or-empty") != std::string::npos) {
            std::cerr << "\n[AGENT_PLAN_REQUIRED] Commit messages need to be filled in:\n";
            std::cerr << "  " << InPlanPath.generic_string() << "\n";
            std::cerr << "After editing the plan, run the command again.\n\n";
            return 3;
        }
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
    const auto selfBinary = ResolveSelfBinaryCommand();
    LogAutoPlanStageDetails("ignore-runbook",
                            FormatCommandLineForLog(selfBinary, args),
                            {{"plan file", InPlanPath.generic_string()}});
    const auto result = shell::ExecuteCommand(selfBinary, args, shell::ExecMode::Capture, InWorkspaceRoot);
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
    const auto selfBinary = ResolveSelfBinaryCommand();
    LogAutoPlanStageDetails("ignore-apply",
                            FormatCommandLineForLog(selfBinary, args),
                            {{"plan file", InPlanPath.generic_string()}});
    const auto result = shell::ExecuteCommand(selfBinary, args, shell::ExecMode::Capture, InWorkspaceRoot);
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

auto ExtractStringArrayField(const std::string& InObjectText, const std::string& InField) -> std::vector<std::string> {
    std::vector<std::string> out;
    const auto valuePos = FindJsonKeyValueStart(InObjectText, InField);
    if (!valuePos.has_value()) {
        return out;
    }
    const auto arrayBody = ExtractBracketBody(InObjectText, *valuePos, '[', ']');
    if (!arrayBody.has_value()) {
        return out;
    }

    std::size_t pos = 1;
    while (pos < arrayBody->size()) {
        pos = SkipJsonWhitespace(*arrayBody, pos);
        if (pos >= arrayBody->size() || (*arrayBody)[pos] == ']') {
            break;
        }
        if ((*arrayBody)[pos] != '"') {
            pos += 1;
            continue;
        }
        const auto parsed = ParseJsonStringAt(*arrayBody, pos);
        if (!parsed.has_value()) {
            break;
        }
        out.push_back(parsed->first);
        pos = parsed->second;
    }
    return out;
}

void PrintCommitPushStageTimings(const std::string& InMode,
                                 long long InSafetyGatesMillis,
                                 long long InPreCommitMillis,
                                 long long InCommitMillis,
                                 long long InSyncMillis,
                                 long long InPostSyncMillis,
                                 long long InPushMillis,
                                 long long InTotalMillis) {
    std::cout << "\n=== commit-push stage timings ===\n";
    std::cout << "mode: " << InMode << "\n";
    std::cout << "safety_gates_ms: " << InSafetyGatesMillis << "\n";
    std::cout << "pre_commit_ms: " << InPreCommitMillis << "\n";
    std::cout << "commit_ms: " << InCommitMillis << "\n";
    std::cout << "sync_ms: " << InSyncMillis << "\n";
    std::cout << "post_sync_ms: " << InPostSyncMillis << "\n";
    std::cout << "push_ms: " << InPushMillis << "\n";
    std::cout << "total_ms: " << InTotalMillis << "\n";
}

auto HumanAutoPlanLooksDeterministic(const std::filesystem::path& InPlanPath,
                                     std::string* OutReason) -> bool {
    const auto payload = ReadTextFile(InPlanPath);
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
    const auto review = ExtractObjectBodyForKey(*meta, "review");
    const auto stages = ExtractObjectBodyForKey(*payload, "stages");
    const auto provider = ToLower(planner.has_value() ? ExtractStringField(*planner, "provider").value_or("") : std::string{});
    const auto model = ToLower(planner.has_value() ? ExtractStringField(*planner, "ai-model").value_or("") : std::string{});
    const auto reason = ToLower(review.has_value() ? ExtractStringField(*review, "reason").value_or("") : std::string{});

    const bool deterministicPlannerMeta = provider == "agent" ||
                                          model == "external-agent" ||
                                          model == "deterministic" ||
                                          reason.find("deterministic plan bootstrap") != std::string::npos;
    if (!deterministicPlannerMeta) {
        return false;
    }

    if (!stages.has_value()) {
        if (OutReason != nullptr) {
            *OutReason = std::format("provider={} ai-model={} review.reason={} stages=<missing>",
                                     provider.empty() ? "<unset>" : provider,
                                     model.empty() ? "<unset>" : model,
                                     reason.empty() ? "<unset>" : reason);
        }
        return true;
    }

    bool hasCommitItems = false;
    bool hasLikelyAiContent = false;
    const auto commitArray = ExtractArrayBodyForKey(*stages, "commit").value_or(std::string{});
    const std::regex fallbackMessagePattern(
        R"(^chore\(.+\): apply( workspace)? updates \([0-9]+ files?\)$)",
        std::regex::icase);
    for (const auto& repoObj : SplitTopLevelObjects(commitArray)) {
        const auto commits = ExtractArrayBodyForKey(repoObj, "commits").value_or(std::string{});
        for (const auto& commitObj : SplitTopLevelObjects(commits)) {
            hasCommitItems = true;
            const auto msg = ToLower(ExtractStringField(commitObj, "message").value_or(""));
            const auto commitReview = ExtractObjectBodyForKey(commitObj, "review");
            const auto commitReason = ToLower(
                commitReview.has_value() ? ExtractStringField(*commitReview, "reason").value_or("") : std::string{});
            const bool isFallbackMessage = std::regex_match(msg, fallbackMessagePattern);
            const bool isFallbackReason =
                commitReason.find("seeded by plan commit-seed") != std::string::npos ||
                commitReason.find("seeded from current dirty status") != std::string::npos;
            const bool isPlaceholder = msg.find("replace-with-") != std::string::npos ||
                                       commitReason.find("replace-with-") != std::string::npos;
            if (!isFallbackMessage && !isFallbackReason && !isPlaceholder) {
                hasLikelyAiContent = true;
            }
        }
    }

    if (hasLikelyAiContent) {
        if (OutReason != nullptr) {
            *OutReason = std::format("metadata deterministic but commit content appears AI-authored; allowing proceed (provider={} ai-model={})",
                                     provider.empty() ? "<unset>" : provider,
                                     model.empty() ? "<unset>" : model);
        }
        return false;
    }

    if (OutReason != nullptr) {
        *OutReason = std::format("provider={} ai-model={} review.reason={} has_commit_items={} content=fallback-only",
                                 provider.empty() ? "<unset>" : provider,
                                 model.empty() ? "<unset>" : model,
                                 reason.empty() ? "<unset>" : reason,
                                 hasCommitItems ? "true" : "false");
    }
    return true;
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

auto ResolveSafetyGateRepos(const std::filesystem::path& InWorkspaceRoot,
                            const std::vector<std::string>& InRepoList,
                            bool InNoRecursive) -> std::vector<std::filesystem::path> {
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

struct PlanSafetyCandidateFile {
    std::filesystem::path repo;
    std::string repoLabel;
    std::string path;
};

struct PlanSafetyScope {
    bool scoped = false;
    std::vector<PlanSafetyCandidateFile> files;
};

auto BuildPostSyncPlanPathScope(const std::filesystem::path& InWorkspaceRoot,
                                const std::filesystem::path& InPlanPath) -> PostSyncPlanPathScope {
    PostSyncPlanPathScope scope;
    const auto payload = ReadTextFile(InPlanPath);
    if (!payload.has_value()) {
        return scope;
    }

    const auto stages = ExtractObjectBodyForKey(*payload, "stages");
    if (!stages.has_value()) {
        return scope;
    }

    bool sawCommit = false;
    bool hasUnscopedCommit = false;
    const auto collectStage = [&](const std::string& stageKey) {
        const auto stageArray = ExtractArrayBodyForKey(*stages, stageKey).value_or(std::string{});
        for (const auto& repoObj : SplitTopLevelObjects(stageArray)) {
            const auto repoSpec = ExtractStringField(repoObj, "repo").value_or(".");
            const auto repoRoot = ResolveRepoFromSpec(InWorkspaceRoot, std::filesystem::path(repoSpec), 12, true).lexically_normal();
            const auto commits = ExtractArrayBodyForKey(repoObj, "commits").value_or(std::string{});
            for (const auto& commitObj : SplitTopLevelObjects(commits)) {
                sawCommit = true;
                const auto includes = ExtractStringArrayField(commitObj, "include");
                const auto excludes = ExtractStringArrayField(commitObj, "exclude");
                if (includes.empty()) {
                    hasUnscopedCommit = true;
                    continue;
                }

                auto& repoScope = scope.repos[CommitPushRepoScopeKey(repoRoot)];
                for (const auto& include : includes) {
                    const auto normalized = NormalizeCommitPushGitPath(include);
                    if (!normalized.empty()) {
                        repoScope.include.push_back(normalized);
                    }
                }
                for (const auto& exclude : excludes) {
                    const auto normalized = NormalizeCommitPushGitPath(exclude);
                    if (!normalized.empty()) {
                        repoScope.exclude.push_back(normalized);
                    }
                }
            }
        }
    };
    collectStage("commit");
    collectStage("post_sync");

    scope.scoped = sawCommit && !hasUnscopedCommit;
    if (!scope.scoped) {
        scope.repos.clear();
    }
    return scope;
}

auto CollectPlanPathspecFiles(const std::filesystem::path& InRepo,
                              const std::string& InIncludePathspec,
                              const std::vector<std::string>& InExcludePathspecs) -> std::vector<std::string> {
    std::set<std::string> files;
    const auto includePathspec = NormalizeCommitPushGitPath(InIncludePathspec);
    if (includePathspec.empty()) {
        return {};
    }

    const auto out = shell::ExecuteCommand(
        "git",
        {"ls-files", "--cached", "--modified", "--others", "--exclude-standard", "--", includePathspec},
        shell::ExecMode::Capture,
        InRepo);
    if (out.exitCode == 0) {
        std::istringstream iss(out.stdoutStr);
        std::string line;
        while (std::getline(iss, line)) {
            auto path = NormalizeCommitPushGitPath(line);
            if (path.empty()) {
                continue;
            }
            bool excluded = false;
            for (const auto& exclude : InExcludePathspecs) {
                if (CommitPushPathspecCoversPath(exclude, path)) {
                    excluded = true;
                    break;
                }
            }
            if (!excluded) {
                files.insert(path);
            }
        }
    }

    const auto abs = (InRepo / std::filesystem::path(includePathspec)).lexically_normal();
    std::error_code ec;
    if (std::filesystem::exists(abs, ec) && !std::filesystem::is_directory(abs, ec)) {
        bool excluded = false;
        for (const auto& exclude : InExcludePathspecs) {
            if (CommitPushPathspecCoversPath(exclude, includePathspec)) {
                excluded = true;
                break;
            }
        }
        if (!excluded) {
            files.insert(includePathspec);
        }
    }

    return std::vector<std::string>(files.begin(), files.end());
}

auto BuildPlanFileSafetyScope(const std::filesystem::path& InWorkspaceRoot,
                              const std::filesystem::path& InPlanPath,
                              std::string* OutError) -> PlanSafetyScope {
    PlanSafetyScope scope;
    const auto payload = ReadTextFile(InPlanPath);
    if (!payload.has_value()) {
        if (OutError != nullptr) {
            *OutError = "cannot read plan file";
        }
        return scope;
    }

    const auto stages = ExtractObjectBodyForKey(*payload, "stages");
    if (!stages.has_value()) {
        if (OutError != nullptr) {
            *OutError = "plan file missing stages object";
        }
        return scope;
    }

    std::set<std::pair<std::string, std::string>> seen;
    bool sawCommit = false;
    bool hasUnscopedCommit = false;
    const auto collectStage = [&](const std::string& stageKey) {
        const auto stageArray = ExtractArrayBodyForKey(*stages, stageKey).value_or(std::string{});
        for (const auto& repoObj : SplitTopLevelObjects(stageArray)) {
            const auto repoSpec = ExtractStringField(repoObj, "repo").value_or(".");
            const auto repoRoot = ResolveRepoFromSpec(InWorkspaceRoot, std::filesystem::path(repoSpec), 12, true);
            const auto repoRel = repoRoot.lexically_relative(InWorkspaceRoot).generic_string();
            const auto repoLabel = repoRel.empty() ? std::string(".") : repoRel;
            const auto commits = ExtractArrayBodyForKey(repoObj, "commits").value_or(std::string{});
            for (const auto& commitObj : SplitTopLevelObjects(commits)) {
                sawCommit = true;
                const auto includes = ExtractStringArrayField(commitObj, "include");
                const auto excludes = ExtractStringArrayField(commitObj, "exclude");
                if (includes.empty()) {
                    hasUnscopedCommit = true;
                    continue;
                }
                for (const auto& include : includes) {
                    for (const auto& path : CollectPlanPathspecFiles(repoRoot, include, excludes)) {
                        const auto key = std::make_pair(repoRoot.generic_string(), path);
                        if (seen.insert(key).second) {
                            scope.files.push_back(PlanSafetyCandidateFile{.repo = repoRoot, .repoLabel = repoLabel, .path = path});
                        }
                    }
                }
            }
        }
    };
    collectStage("commit");
    collectStage("post_sync");

    scope.scoped = sawCommit && !hasUnscopedCommit;
    return scope;
}

auto PlanSafetyCandidatePathExistsInHead(const PlanSafetyCandidateFile& InFile) -> bool {
    auto path = NormalizeCommitPushGitPath(InFile.path);
    if (path.empty()) {
        return false;
    }
    const auto result = shell::ExecuteCommand("git", {"cat-file", "-e", "HEAD:" + path}, shell::ExecMode::Capture, InFile.repo);
    return result.exitCode == 0;
}

auto CollectPlanFileRepoRoots(const std::filesystem::path& InWorkspaceRoot,
                              const std::filesystem::path& InPlanPath) -> std::vector<std::filesystem::path> {
    std::vector<std::filesystem::path> repos;
    std::set<std::string> seen;
    const auto payload = ReadTextFile(InPlanPath);
    if (!payload.has_value()) {
        return {InWorkspaceRoot.lexically_normal()};
    }
    const auto stages = ExtractObjectBodyForKey(*payload, "stages");
    if (!stages.has_value()) {
        return {InWorkspaceRoot.lexically_normal()};
    }

    const auto collectStage = [&](const std::string& stageKey) {
        const auto stageArray = ExtractArrayBodyForKey(*stages, stageKey).value_or(std::string{});
        for (const auto& repoObj : SplitTopLevelObjects(stageArray)) {
            const auto repoSpec = ExtractStringField(repoObj, "repo").value_or(".");
            const auto repoRoot = ResolveRepoFromSpec(InWorkspaceRoot, std::filesystem::path(repoSpec), 12, true).lexically_normal();
            const auto key = repoRoot.generic_string();
            if (!key.empty() && seen.insert(key).second) {
                repos.push_back(repoRoot);
            }
        }
    };
    collectStage("commit");
    collectStage("post_sync");

    if (repos.empty()) {
        repos.push_back(InWorkspaceRoot.lexically_normal());
    }
    return repos;
}

auto AnyRepoHasWorkingTreeChanges(const std::vector<std::filesystem::path>& InRepos) -> bool {
    for (const auto& repo : InRepos) {
        const auto status = shell::ExecuteCommand("git", {"status", "--porcelain"}, shell::ExecMode::Capture, repo);
        if (status.exitCode == 0 && !Trim(status.stdoutStr).empty()) {
            return true;
        }
    }
    return false;
}

auto PlanSafetyScopeHasWorkingTreeChanges(const PlanSafetyScope& InScope) -> std::optional<bool> {
    if (!InScope.scoped) {
        return std::nullopt;
    }
    for (const auto& file : InScope.files) {
        const auto status = shell::ExecuteCommand(
            "git",
            {"status", "--porcelain", "--untracked-files=all", "--", file.path},
            shell::ExecMode::Capture,
            file.repo);
        if (status.exitCode != 0) {
            return std::nullopt;
        }
        if (!Trim(status.stdoutStr).empty()) {
            return true;
        }
    }
    return false;
}

auto RunPlanFileExactSafetyGates(const std::filesystem::path& InWorkspaceRoot,
                                 const std::filesystem::path& InPlanPath) -> bool {
    std::string scopeError;
    const auto scope = BuildPlanFileSafetyScope(InWorkspaceRoot, InPlanPath, &scopeError);
    if (!scope.scoped) {
        if (!scopeError.empty()) {
            std::cerr << "[commit-push][plan-pipeline] scoped safety gate unavailable: " << scopeError << "\n";
        }
        return false;
    }

    const auto workspaceRoot = InWorkspaceRoot.lexically_normal();
    const auto allowIgnoreGate = ToLower(Trim(std::getenv("KOG_ALLOW_IGNORE_GATE") == nullptr ? "" : std::getenv("KOG_ALLOW_IGNORE_GATE")));
    const auto ignoreGateMode = ToLower(Trim(std::getenv("KOG_IGNORE_GATE") == nullptr ? "on" : std::getenv("KOG_IGNORE_GATE")));
    if (!(allowIgnoreGate == "1" || allowIgnoreGate == "true") && ignoreGateMode != "off") {
        const auto allowlistPath =
            (ResolveSkillRoot(workspaceRoot) / "assets" / "ignore-sources" / "local" / "ignore-gate-allowlist.txt").lexically_normal();
        const auto allowlist = LoadNormalizedLineSet(allowlistPath);

        std::vector<std::string> findings;
        findings.reserve(20);
        for (const auto& file : scope.files) {
            auto p = NormalizeCommitPushGitPath(file.path);
            if (p.empty() || !IsProbableIgnoreArtifactPath(p)) {
                continue;
            }
            if (PlanSafetyCandidatePathExistsInHead(file)) {
                continue;
            }
            if (IsInternalPipelineArtifactPath(p)) {
                continue;
            }
            const auto key = ToLower(file.repoLabel == "." ? p : (file.repoLabel + "/" + p));
            if (allowlist.find(key) != allowlist.end()) {
                continue;
            }
            findings.push_back(key);
            if (findings.size() >= 20) {
                break;
            }
        }
        if (!findings.empty()) {
            std::cerr << "Error: ignore gate failed (commit-push); unresolved artifact-like files in plan include set.\n";
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
        return true;
    }

    const auto rulesPath = (ResolveSkillRoot(workspaceRoot) / "assets" / "security" / "secret-blacklist.rules").lexically_normal();
    const auto rules = LoadSecretRules(rulesPath);
    if (rules.empty()) {
        return true;
    }

    std::vector<SecretFinding> findings;
    findings.reserve(20);
    for (const auto& file : scope.files) {
        if (static_cast<int>(findings.size()) >= 20) {
            break;
        }
        const auto before = findings.size();
        ScanFileForSecretRules(file.repo, file.path, rules, 20, &findings);
        for (std::size_t i = before; i < findings.size(); ++i) {
            findings[i].repo = file.repoLabel;
        }
    }
    if (!findings.empty()) {
        std::cerr << "Error: secret gate failed (commit-push); potential secrets detected in plan include set.\n";
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

    std::cout << "[commit-push][plan-pipeline] scoped safety gates checked files=" << scope.files.size() << "\n";
    return true;
}

auto RunPipelineSafetyGatesForNonAiCommitPush(const std::filesystem::path& InWorkspaceRoot,
                                              const std::vector<std::string>& InRepoList,
                                              bool InNoRecursive) -> void {
    const auto workspaceRoot = InWorkspaceRoot.lexically_normal();
    const auto repos = ResolveSafetyGateRepos(workspaceRoot, InRepoList, InNoRecursive);

    const auto allowIgnoreGate = ToLower(Trim(std::getenv("KOG_ALLOW_IGNORE_GATE") == nullptr ? "" : std::getenv("KOG_ALLOW_IGNORE_GATE")));
    const auto ignoreGateMode = ToLower(Trim(std::getenv("KOG_IGNORE_GATE") == nullptr ? "on" : std::getenv("KOG_IGNORE_GATE")));
    if (!(allowIgnoreGate == "1" || allowIgnoreGate == "true") && ignoreGateMode != "off") {
        const auto allowlistPath =
            (ResolveSkillRoot(workspaceRoot) / "assets" / "ignore-sources" / "local" / "ignore-gate-allowlist.txt").lexically_normal();
        const auto allowlist = LoadNormalizedLineSet(allowlistPath);

        std::vector<std::string> findings;
        findings.reserve(20);
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
            if (RepoHasPostSyncWorkingTreeChanges(repoRoot)) {
                return true;
            }
        }
        return false;
    }

    if (InNoRecursive) {
        return RepoHasPostSyncWorkingTreeChanges(InWorkspaceRoot);
    }

    const auto repos = DiscoverWorkspaceRepos(InWorkspaceRoot);
    for (const auto& repo : repos) {
        if (RepoHasPostSyncWorkingTreeChanges(repo)) {
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

auto RunCommitPushPlanFilePipelineImpl(const std::filesystem::path& InWorkspaceRoot,
                                       const std::string& InNormalizedPlanFile,
                                       const std::vector<std::string>& InExtraArgs) -> int {
    KOG_SCOPED_TIMING_LOG("commit-push.RunCommitPushPlanFilePipelineImpl");
    const bool agentMode = IsAgentModeEnabledLocal();
    const auto totalStart = std::chrono::steady_clock::now();
    long long safetyGatesMillis = 0;
    long long preCommitMillis = 0;
    long long commitMillis = 0;
    long long syncMillis = 0;
    long long postSyncMillis = 0;
    long long pushMillis = 0;
    if (InNormalizedPlanFile.empty()) {
        std::cerr << "Error: plan pipeline requires non-empty --plan-file\n";
        return 2;
    }
    std::cout << "[commit-push] using plan file: " << InNormalizedPlanFile << "\n";
    const auto logPipelineStage = [&](const std::string& stage, const std::vector<std::pair<std::string, std::string>>& fields) {
        std::cout << "[commit-push][plan-pipeline] stage=" << stage << " start\n";
        for (const auto& [label, value] : fields) {
            std::cout << "[commit-push][plan-pipeline] " << label << ": " << value << "\n";
        }
    };

    if (!InExtraArgs.empty()) {
        std::cerr << "Error: unsupported extra arguments in plan pipeline mode:";
        for (const auto& extra : InExtraArgs) {
            std::cerr << ' ' << extra;
        }
        std::cerr << "\n";
        return 2;
    }

    const auto planPath = std::filesystem::path(InNormalizedPlanFile).lexically_normal();
    const auto planRepoRoots = CollectPlanFileRepoRoots(InWorkspaceRoot, planPath);
    const auto postSyncScope = BuildPostSyncPlanPathScope(InWorkspaceRoot, planPath);
    std::string planSafetyScopeError;
    const auto planSafetyScope = BuildPlanFileSafetyScope(InWorkspaceRoot, planPath, &planSafetyScopeError);

    if (agentMode) {
        std::cout << "[commit-push] agent mode + --plan-file detected; using plan-driven flow.\n";
    }

    logPipelineStage("safety-gates",
                     {{"workspace root", InWorkspaceRoot.lexically_normal().generic_string()},
                      {"plan file", planPath.generic_string()}});
    const auto safetyStart = std::chrono::steady_clock::now();
    if (!RunPlanFileExactSafetyGates(InWorkspaceRoot, planPath)) {
        RunPipelineSafetyGatesForNonAiCommitPush(InWorkspaceRoot, {}, false);
    }
    const auto safetyEnd = std::chrono::steady_clock::now();
    safetyGatesMillis = std::chrono::duration_cast<std::chrono::milliseconds>(safetyEnd - safetyStart).count();

    logPipelineStage("pre-commit",
                     {{"workspace root", InWorkspaceRoot.lexically_normal().generic_string()},
                      {"plan file", planPath.generic_string()},
                      {"branch mode", "plan-file-exact"}});
    {
        const auto preCommitStart = std::chrono::steady_clock::now();
        const auto preCommitEnd = std::chrono::steady_clock::now();
        preCommitMillis = std::chrono::duration_cast<std::chrono::milliseconds>(preCommitEnd - preCommitStart).count();
        std::cout << "[commit-push][plan-pipeline] pre-commit skipped for explicit plan-file; "
                     "plan commit entries own exact include/exclude staging.\n";
    }

    if (agentMode && IsSharedDefaultPlanPath(InWorkspaceRoot, planPath)) {
        const auto refreshCode = EnsureAgentSharedPlanFresh(InWorkspaceRoot, planPath);
        if (refreshCode != 0) {
            return refreshCode;
        }
    }

    if (agentMode) {
        // Check the plan file directly for unfilled placeholder messages
        std::ifstream planFile(planPath);
        if (planFile.is_open()) {
            std::string planContent((std::istreambuf_iterator<char>(planFile)),
                                     std::istreambuf_iterator<char>());
            if (planContent.find("\"replace-with-commit-message\"") != std::string::npos) {
                std::cerr << "\n[AGENT_PLAN_REQUIRED] Commit messages need to be filled in:\n";
                std::cerr << "  " << planPath.generic_string() << "\n";
                std::cerr << "After editing the plan, run the command again.\n\n";
                return 3;
            }
        }
    }

    bool hasWorkingChanges = false;
    if (planSafetyScope.scoped) {
        const auto exactWorkingChanges = PlanSafetyScopeHasWorkingTreeChanges(planSafetyScope);
        if (!exactWorkingChanges.has_value()) {
            std::cerr << "Error: exact plan path status failed; refusing to treat an incomplete snapshot as clean.\n";
            return 2;
        }
        hasWorkingChanges = *exactWorkingChanges;
        std::cout << "[commit-push][plan-pipeline] exact plan working changes="
                  << (hasWorkingChanges ? "true" : "false") << "\n";
    } else {
        hasWorkingChanges = NeedsPostSyncCommitNonPlan(InWorkspaceRoot, {}, false);
    }
    if (!hasWorkingChanges) {
        std::cout << "[commit-push] workspace clean; skipping commit/sync/post-sync and proceeding to push check.\n";
    } else {
        logPipelineStage("commit",
                         {{"workspace root", InWorkspaceRoot.lexically_normal().generic_string()},
                          {"plan file", planPath.generic_string()}});
        {
            const auto commitStart = std::chrono::steady_clock::now();
            const auto commitCode = RunCommitNativePlanStage(InWorkspaceRoot, InNormalizedPlanFile, "commit", false, true);
            const auto commitEnd = std::chrono::steady_clock::now();
            commitMillis = std::chrono::duration_cast<std::chrono::milliseconds>(commitEnd - commitStart).count();
            if (commitCode != 0) {
                return commitCode;
            }
        }

        logPipelineStage("sync",
                         {{"workspace root", InWorkspaceRoot.lexically_normal().generic_string()},
                          {"plan file", planPath.generic_string()}});
        {
            const auto syncStart = std::chrono::steady_clock::now();
            int syncCode = 0;
            for (const auto& repoRoot : planRepoRoots) {
                std::cout << "[commit-push][plan-pipeline] sync repo: " << repoRoot.generic_string() << "\n";
                syncCode = RunSyncOriginLatestNative(repoRoot, false, false, false, false);
                if (syncCode != 0) {
                    break;
                }
            }
            const auto syncEnd = std::chrono::steady_clock::now();
            syncMillis = std::chrono::duration_cast<std::chrono::milliseconds>(syncEnd - syncStart).count();
            if (syncCode != 0) {
                return syncCode;
            }
        }

        logPipelineStage("post-sync",
                         {{"workspace root", InWorkspaceRoot.lexically_normal().generic_string()},
                          {"plan file", planPath.generic_string()}});
        {
            const auto postSyncStart = std::chrono::steady_clock::now();
            const bool hasPostSyncStage = PlanStageLikelyNonEmpty(std::filesystem::path(InNormalizedPlanFile), "post_sync");
            if (!hasPostSyncStage) {
                std::cout << "[commit-push] post-sync plan stage is empty; skipping.\n";
            } else {
                bool gitlinkOnly = false;
                bool semanticDrift = false;
                for (const auto& repoRoot : planRepoRoots) {
                    const auto candidateRepos = CollectPostSyncCandidateRepos(repoRoot, {}, true);
                    const auto summary = ClassifyPostSyncDelta(candidateRepos, &postSyncScope);
                    if (summary.kind == PostSyncDeltaKind::SemanticDrift) {
                        semanticDrift = true;
                        std::cout << "[commit-push] post-sync semantic changes detected; proceeding to post-sync commit stage.\n";
                        for (const auto& repo : summary.semanticRepos) {
                            std::cout << "  repo: " << repo.generic_string() << "\n";
                        }
                    }
                    if (summary.kind == PostSyncDeltaKind::GitlinkOnly) {
                        gitlinkOnly = true;
                        const auto amendResult = AutoAmendGitlinkOnlyPostSyncRepos(repoRoot, candidateRepos, &postSyncScope);
                        if (amendResult.first != 0) {
                            return 2;
                        }
                        std::cout << "[commit-push] post-sync gitlink-only auto-amend applied: repos=" << amendResult.second << "\n";
                    }
                }
                if (!gitlinkOnly) {
                    if (!AnyRepoHasPostSyncWorkingTreeChanges(planRepoRoots, &postSyncScope)) {
                        std::cout << "[commit-push] post-sync plan commit skipped (no working tree changes).\n";
                    } else {
                        if (semanticDrift) {
                            std::cout << "[commit-push] post-sync plan stage will run against plan repo scope.\n";
                        }
                        const auto postCommitCode = RunCommitNativePlanStage(InWorkspaceRoot, InNormalizedPlanFile, "post_sync", false, true);
                        if (postCommitCode != 0) {
                            return postCommitCode;
                        }
                    }
                }
            }
            const auto postSyncEnd = std::chrono::steady_clock::now();
            postSyncMillis = std::chrono::duration_cast<std::chrono::milliseconds>(postSyncEnd - postSyncStart).count();
        }
    }

    logPipelineStage("push",
                     {{"workspace root", InWorkspaceRoot.lexically_normal().generic_string()},
                      {"plan file", planPath.generic_string()}});
    {
        const auto pushStart = std::chrono::steady_clock::now();
        int pushCode = 0;
        for (const auto& repoRoot : planRepoRoots) {
            std::cout << "[commit-push][plan-pipeline] push repo: " << repoRoot.generic_string() << "\n";
            pushCode = RunPushNativeSimple(repoRoot, false, false, false, false, false, 0, false, "");
            if (pushCode != 0) {
                break;
            }
        }
        const auto pushEnd = std::chrono::steady_clock::now();
        pushMillis = std::chrono::duration_cast<std::chrono::milliseconds>(pushEnd - pushStart).count();
        const auto totalEnd = std::chrono::steady_clock::now();
        const auto totalMillis = std::chrono::duration_cast<std::chrono::milliseconds>(totalEnd - totalStart).count();
        std::error_code planEc;
        const bool planFileExists = std::filesystem::exists(planPath, planEc) && !planEc;
        if (hasWorkingChanges && planFileExists) {
            std::string stampError;
            if (!StampCommitPlanExecutedAt(planPath, &stampError)) {
                std::cerr << "Warning: failed to stamp plan executed_at_utc: " << planPath.generic_string();
                if (!stampError.empty()) {
                    std::cerr << " (" << stampError << ")";
                }
                std::cerr << "\n";
            }
        }
        PrintCommitPushStageTimings("plan-file",
                                    safetyGatesMillis,
                                    preCommitMillis,
                                    commitMillis,
                                    syncMillis,
                                    postSyncMillis,
                                    pushMillis,
                                    totalMillis);
        if (hasWorkingChanges && planFileExists) {
            PrintExecutedPlanSummary(std::filesystem::path(InNormalizedPlanFile).lexically_normal(), 10);
        }
        return pushCode;
    }
}

} // namespace

auto RunCommitPushPlanFilePipeline(const std::filesystem::path& InWorkspaceRoot,
                                   const std::string& InNormalizedPlanFile,
                                   const std::vector<std::string>& InExtraArgs) -> int {
    return RunCommitPushPlanFilePipelineImpl(InWorkspaceRoot, InNormalizedPlanFile, InExtraArgs);
}

auto MakeCommitPushCommandCallback(CLI::App& InCommand,
                                   const std::shared_ptr<CommitPushCommandOptions>& InOptions)
    -> std::function<void()> {
    auto* cmd = &InCommand;
    auto* repos = &InOptions->repos;
    auto* noRecursive = &InOptions->noRecursive;
    auto* message = &InOptions->message;
    auto* commitPlanFile = &InOptions->commitPlanFile;
    auto* writeCommitPlanTemplate = &InOptions->writeCommitPlanTemplate;
    auto* commitPlanOut = &InOptions->commitPlanOut;
    auto* aiProvider = &InOptions->aiProvider;
    auto* aiModel = &InOptions->aiModel;
    auto* aiFillMode = &InOptions->aiFillMode;
    auto* aiAuto = &InOptions->aiAuto;
    auto* noAiReview = &InOptions->noAiReview;
    auto* stagedOnly = &InOptions->stagedOnly;
    auto* dryRun = &InOptions->dryRun;
    auto* profile = &InOptions->profile;
    auto* branchMode = &InOptions->branchMode;
    auto* forceWithLease = &InOptions->forceWithLease;
    auto* noVerify = &InOptions->noVerify;
    auto* jobs = &InOptions->jobs;
    auto* verbose = &InOptions->verbose;
    auto* remote = &InOptions->remote;
    auto* repoRoot = &InOptions->repoRoot;
    auto* target = &InOptions->target;
    auto* yolo = &InOptions->yolo;

    return [=, optionsOwner = InOptions]() {
        (void)optionsOwner;
        const auto totalStart = std::chrono::steady_clock::now();
        long long safetyGatesMillis = 0;
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
        auto repoList = ResolveRepoList(workspaceRoot, ParseReposCsv(*repos));
        bool effectiveNoRecursive = *noRecursive;
        if (!effectiveNoRecursive && repoList.empty() && !target->empty()) {
            const auto scopedRepos = DiscoverWorkspaceRepos(workspaceRoot);
            if (scopedRepos.size() <= 1) {
                effectiveNoRecursive = true;
            }
        }
        if (effectiveNoRecursive && repoList.empty()) {
            repoList.push_back(".");
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
        const bool agentMode = IsAgentModeEnabledLocal();
        const bool aiModeRequested = *aiAuto || !aiProvider->empty() || !aiModel->empty();
        const bool autoPlanAiMode = aiModeRequested && !hasCommitPlan && message->empty();
        const bool hasPlanFileInput = !commitPlanFile->empty();
        const bool hasWorkingChangesAtStart = hasPlanFileInput
            ? true
            : NeedsPostSyncCommitNonPlan(workspaceRoot, repoList, effectiveNoRecursive);
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
                long long planNewMillis = 0;
                long long ignoreRunbookMillis = 0;
                long long ignoreApplyMillis = 0;
                long long commitRunbookMillis = 0;
                std::optional<long long> aiFillMillis;
                long long planPipelineMillis = 0;
                const auto autoPlanPath = DefaultSharedPlanPath(workspaceRoot);
                std::cout << "[commit-push] full-auto plan file: " << autoPlanPath.generic_string() << "\n";
                std::cout << "[commit-push][auto-plan] stage=plan-new start\n";
                const auto planNewStart = std::chrono::steady_clock::now();
                const auto planNewCode = RunPlanNewViaSelf(workspaceRoot, autoPlanPath);
                const auto planNewEnd = std::chrono::steady_clock::now();
                planNewMillis = std::chrono::duration_cast<std::chrono::milliseconds>(planNewEnd - planNewStart).count();
                if (planNewCode != 0) {
                    std::exit(planNewCode);
                }
                std::cout << "[commit-push][auto-plan] stage=plan-new done ms=" << planNewMillis << "\n";
                std::cout << "[commit-push][auto-plan] stage=ignore-runbook start\n";
                const auto ignoreRunbookStart = std::chrono::steady_clock::now();
                const auto ignoreRunbookCode = RunIgnorePlanRunbookViaSelf(workspaceRoot, autoPlanPath);
                const auto ignoreRunbookEnd = std::chrono::steady_clock::now();
                ignoreRunbookMillis = std::chrono::duration_cast<std::chrono::milliseconds>(ignoreRunbookEnd - ignoreRunbookStart).count();
                if (ignoreRunbookCode != 0) {
                    std::exit(ignoreRunbookCode);
                }
                std::cout << "[commit-push][auto-plan] stage=ignore-runbook done ms=" << ignoreRunbookMillis << "\n";
                if (PlanStageLikelyNonEmpty(autoPlanPath, "ignore")) {
                    std::cout << "[commit-push][auto-plan] stage=ignore-apply start\n";
                    const auto ignoreApplyStart = std::chrono::steady_clock::now();
                    const auto ignoreApplyCode = RunIgnorePlanApplyViaSelf(workspaceRoot, autoPlanPath);
                    const auto ignoreApplyEnd = std::chrono::steady_clock::now();
                    ignoreApplyMillis = std::chrono::duration_cast<std::chrono::milliseconds>(ignoreApplyEnd - ignoreApplyStart).count();
                    if (ignoreApplyCode != 0) {
                        std::exit(ignoreApplyCode);
                    }
                    std::cout << "[commit-push][auto-plan] stage=ignore-apply done ms=" << ignoreApplyMillis << "\n";
                } else {
                    std::cout << "[commit-push] ignore plan stage is empty; skipping ignore apply.\n";
                }
                std::cout << "[commit-push][auto-plan] stage=commit-runbook start\n";
                const auto commitRunbookStart = std::chrono::steady_clock::now();
                const auto runbookResult = RunCommitPlanRunbookViaSelf(workspaceRoot, autoPlanPath, *aiProvider, *aiModel, *aiFillMode, *yolo);
                const auto commitRunbookEnd = std::chrono::steady_clock::now();
                commitRunbookMillis = std::chrono::duration_cast<std::chrono::milliseconds>(commitRunbookEnd - commitRunbookStart).count();
                aiFillMillis = runbookResult.aiFillMillis;
                if (runbookResult.exitCode != 0) {
                    std::cerr << "[commit-push][auto-plan] stage=commit-runbook failed ms=" << commitRunbookMillis << "\n";
                    std::exit(runbookResult.exitCode);
                }
                std::cout << "[commit-push][auto-plan] stage=commit-runbook done ms=" << commitRunbookMillis << "\n";
                std::string deterministicReason;
                if (HumanAutoPlanLooksDeterministic(autoPlanPath, &deterministicReason)) {
                    std::cerr << "Error: AI commit runbook produced non-AI deterministic plan metadata; refusing to continue.\n";
                    std::cerr << "Hint: verify AI provider/auth and rerun plain `kog cpa`.\n";
                    std::cerr << "Hint: deterministic metadata: " << deterministicReason << "\n";
                    std::exit(2);
                }
                std::cout << "[commit-push][auto-plan] stage=plan-pipeline start\n";
                std::cout << "[commit-push][auto-plan] command: "
                          << FormatCommandLineForLog(ResolveSelfBinaryCommand(),
                                                     {"commit-push", "--plan-file", autoPlanPath.generic_string()})
                          << "\n";
                std::cout << "[commit-push][auto-plan] plan file: " << autoPlanPath.generic_string() << "\n";
                const auto planPipelineStart = std::chrono::steady_clock::now();
                const auto pipelineCode = RunCommitPushPlanFilePipeline(workspaceRoot, autoPlanPath.generic_string(), {});
                const auto planPipelineEnd = std::chrono::steady_clock::now();
                planPipelineMillis = std::chrono::duration_cast<std::chrono::milliseconds>(planPipelineEnd - planPipelineStart).count();
                const auto autoTotalEnd = std::chrono::steady_clock::now();
                const auto autoTotalMillis = std::chrono::duration_cast<std::chrono::milliseconds>(autoTotalEnd - totalStart).count();
                auto formatDuration = [](long long ms) -> std::string {
                    if (ms < 1000) return std::to_string(ms) + "ms";
                    if (ms < 60000) return std::to_string(ms / 1000) + "s";
                    const auto minutes = ms / 60000;
                    const auto seconds = (ms % 60000) / 1000;
                    if (minutes < 60) return std::to_string(minutes) + "m " + std::to_string(seconds) + "s";
                    const auto hours = minutes / 60;
                    const auto remainMins = minutes % 60;
                    return std::to_string(hours) + "h " + std::to_string(remainMins) + "m";
                };
                std::cout << "\n=== commit-push auto-plan timings ===\n";
                std::cout << "plan_new: " << formatDuration(planNewMillis) << "\n";
                std::cout << "ignore_runbook: " << formatDuration(ignoreRunbookMillis) << "\n";
                std::cout << "ignore_apply: " << formatDuration(ignoreApplyMillis) << "\n";
                std::cout << "commit_runbook: " << formatDuration(commitRunbookMillis) << "\n";
                if (aiFillMillis.has_value()) {
                    std::cout << "ai_fill: " << formatDuration(*aiFillMillis) << "\n";
                } else {
                    std::cout << "ai_fill: n/a\n";
                }
                std::cout << "plan_pipeline: " << formatDuration(planPipelineMillis) << "\n";
                std::cout << "total: " << formatDuration(autoTotalMillis) << "\n";
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

        if (agentMode && hasCommitPlan) {
            std::cout << "[commit-push] agent mode + --plan-file detected; using plan-driven flow.\n";
        }

        if (!effectiveAiModeRequested) {
            std::cout << "=== commit-push stage: safety-gates ===\n";
            {
                KOG_SCOPED_TIMING_LOG("commit-push.safety-gates");
                RunPipelineSafetyGatesForNonAiCommitPush(workspaceRoot, repoList, effectiveNoRecursive);
            }
        }

        std::cout << "=== commit-push stage: pre-commit ===\n";
        {
            KOG_SCOPED_TIMING_LOG("commit-push.pre-commit");
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
        }

        const bool hasWorkingChanges = NeedsPostSyncCommitNonPlan(workspaceRoot, repoList, effectiveNoRecursive);
        if (!hasWorkingChanges) {
            std::cout << "[commit-push] workspace clean; skipping commit/sync/post-sync and proceeding to push check.\n";
        } else {
            std::cout << "=== commit-push stage: commit ===\n";
            {
                KOG_SCOPED_TIMING_LOG("commit-push.commit");
                const auto commitResult = hasCommitPlan
                    ? shell::ExecResult{
                        RunCommitNativePlanStage(workspaceRoot, normalizedCommitPlanFile, "commit", false, true), "", ""}
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
                if (commitResult.exitCode != 0) {
                    std::exit(commitResult.exitCode);
                }
            }

            std::cout << "=== commit-push stage: sync ===\n";
            {
                KOG_SCOPED_TIMING_LOG("commit-push.sync");
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
            }

            std::cout << "=== commit-push stage: post-sync ===\n";
            {
                KOG_SCOPED_TIMING_LOG("commit-push.post-sync");
                shell::ExecResult postCommitResult{0, "", ""};
                if (*dryRun) {
                    if (hasCommitPlan) {
                        std::cout << "[commit-push] post-sync plan commit skipped in dry-run mode.\n";
                    } else {
                        std::cout << "[commit-push] post-sync commit skipped in dry-run mode.\n";
                    }
                } else {
                    const auto postSyncCandidateRepos =
                        CollectPostSyncCandidateRepos(workspaceRoot, repoList, effectiveNoRecursive);
                    const auto summary = ClassifyPostSyncDelta(postSyncCandidateRepos);
                    if (summary.kind == PostSyncDeltaKind::SemanticDrift) {
                        std::cout << "[commit-push] post-sync semantic changes detected; proceeding to post-sync commit stage.\n";
                        for (const auto& repo : summary.semanticRepos) {
                            std::cout << "  repo: " << repo.generic_string() << "\n";
                        }
                    }
                    if (summary.kind == PostSyncDeltaKind::GitlinkOnly) {
                        const auto amendResult =
                            AutoAmendGitlinkOnlyPostSyncRepos(workspaceRoot, postSyncCandidateRepos);
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
                                shell::ExecResult{RunCommitNativePlanStage(workspaceRoot, normalizedCommitPlanFile, "post_sync", false, true), "", ""};
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
                if (postCommitResult.exitCode != 0) {
                    std::exit(postCommitResult.exitCode);
                }
            }
        }

        std::cout << "=== commit-push stage: push ===\n";
        int pushExitCode = 0;
        {
            KOG_SCOPED_TIMING_LOG("commit-push.push");
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
                if (!effectiveNoRecursive) {
                    std::vector<std::string> pushRepoFilters;
                    for (const auto& repoPath : DiscoverWorkspaceRepos(
                             workspaceRoot, workspace::WorkspacePolicyFilter::Push)) {
                        pushRepoFilters.push_back(repoPath.generic_string());
                    }
                    pushExitCode = RunPushNativeSimpleDetailed(
                                       workspaceRoot,
                                       true,
                                       *dryRun,
                                       *profile,
                                       *forceWithLease,
                                       *noVerify,
                                       1,
                                       *verbose,
                                       *remote,
                                       pushRepoFilters)
                                       .first;
                } else {
                    pushExitCode = RunPushNativeSimple(
                        workspaceRoot,
                        false,
                        *dryRun,
                        *profile,
                        *forceWithLease,
                        *noVerify,
                        1,
                        *verbose,
                        *remote);
                }
            }
        }

        if (hasCommitPlan) {
            std::string stampError;
            const auto planPath = std::filesystem::path(normalizedCommitPlanFile).lexically_normal();
            std::error_code planEc;
            const bool planFileExists = std::filesystem::exists(planPath, planEc) && !planEc;
            if (hasWorkingChanges && planFileExists && !StampCommitPlanExecutedAt(planPath, &stampError)) {
                std::cerr << "Warning: failed to stamp plan executed_at_utc: "
                          << planPath.generic_string();
                if (!stampError.empty()) {
                    std::cerr << " (" << stampError << ")";
                }
                std::cerr << "\n";
            }
            if (hasWorkingChanges && planFileExists) {
                PrintExecutedPlanSummary(planPath, 10);
            }
        }

        std::exit(pushExitCode);
    };
}

} // namespace kano::git::commands
