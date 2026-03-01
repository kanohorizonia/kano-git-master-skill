// commit command — Native multi-repo commit workflow (pure C++)

#include "command_registry.hpp"
#include "discovery.hpp"
#include "shell_executor.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <format>
#include <filesystem>
#include <iostream>
#include <iomanip>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <vector>

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

auto GitPassThrough(const std::filesystem::path& InRepo, const std::vector<std::string>& InArgs) -> shell::ExecResult {
    return shell::ExecuteCommand("git", InArgs, shell::ExecMode::PassThrough, InRepo);
}

auto HasCommand(const std::string& InCommand, const std::vector<std::string>& InArgs = {"--help"}) -> bool {
    const auto result = shell::ExecuteCommand(InCommand, InArgs, shell::ExecMode::Capture, std::filesystem::current_path());
    return result.exitCode == 0;
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

auto AiCacheDir() -> std::filesystem::path {
    const auto root = ResolveGlobalCacheRoot();
    if (root.empty()) {
        return {};
    }
    return (root / "ai").lexically_normal();
}

auto ModelCacheFilePath(const std::string& InProvider) -> std::filesystem::path {
    if (InProvider.empty()) {
        return {};
    }
    auto provider = ToLower(Trim(InProvider));
    if (provider.empty()) {
        return {};
    }
    return AiCacheDir() / ("last-model-" + provider + ".txt");
}

auto ReadRememberedModel(const std::string& InProvider) -> std::string {
    const auto cacheFile = ModelCacheFilePath(InProvider);
    if (!cacheFile.empty() && std::filesystem::exists(cacheFile)) {
        std::ifstream in(cacheFile);
        if (!in) {
            return {};
        }

        std::string line;
        std::getline(in, line);
        return Trim(line);
    }

    const auto home = HomeDirectory();
    if (home.empty()) {
        return {};
    }
    const auto legacyFile = (home / ".cache" / "kano-git-master-skill" / "ai" / ("last-model-" + ToLower(Trim(InProvider)) + ".txt")).lexically_normal();
    if (!std::filesystem::exists(legacyFile)) {
        return {};
    }

    std::ifstream in(legacyFile);
    if (!in) {
        return {};
    }
    std::string line;
    std::getline(in, line);
    return Trim(line);
}

void RememberModelChoice(const std::string& InProvider, const std::string& InModel) {
    const auto provider = ToLower(Trim(InProvider));
    const auto model = Trim(InModel);
    if (provider.empty() || model.empty() || model == "auto") {
        return;
    }

    const auto cacheDir = AiCacheDir();
    const auto cacheFile = ModelCacheFilePath(provider);
    if (cacheDir.empty() || cacheFile.empty()) {
        return;
    }

    std::error_code ec;
    std::filesystem::create_directories(cacheDir, ec);

    std::ofstream out(cacheFile, std::ios::trunc);
    if (!out) {
        return;
    }
    out << model << "\n";
}

auto ResolveModelForAi(const std::string& InProvider,
                       const std::string& InModelRaw,
                       bool InAiAuto) -> std::string {
    auto model = Trim(InModelRaw);
    const auto provider = ToLower(Trim(InProvider));

    if (!model.empty() && model != "auto") {
        RememberModelChoice(provider, model);
        return model;
    }

    const auto remembered = ReadRememberedModel(provider);
    if (!remembered.empty()) {
        return remembered;
    }

    if (InAiAuto || model == "auto") {
        if (provider == "copilot") {
            return "gpt-5-mini";
        }
        if (provider == "codex") {
            return "gpt-5-mini";
        }
        if (provider == "opencode") {
            return "gpt-5-mini";
        }
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
    std::istringstream iss(InText);
    std::string line;
    while (std::getline(iss, line)) {
        line = Trim(line);
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
    if (InProvider == "opencode") {
        if (!InModel.empty() && InModel != "auto") {
            return shell::ExecuteCommand("opencode", {"run", "--model", InModel, InPrompt}, shell::ExecMode::Capture, InWorkingDir);
        }
        return shell::ExecuteCommand("opencode", {"run", InPrompt}, shell::ExecMode::Capture, InWorkingDir);
    }

    if (InProvider == "codex") {
        if (!InModel.empty() && InModel != "auto") {
            return shell::ExecuteCommand("codex", {"-q", "--model", InModel, InPrompt}, shell::ExecMode::Capture, InWorkingDir);
        }
        return shell::ExecuteCommand("codex", {"-q", InPrompt}, shell::ExecMode::Capture, InWorkingDir);
    }

    if (InProvider == "copilot") {
        if (HasCommand("copilot", {"--help"})) {
            if (!InModel.empty() && InModel != "auto") {
                return shell::ExecuteCommand("copilot", {"-s", "-p", InPrompt, "--model", InModel, "--no-color", "--stream", "off", "--no-ask-user"}, shell::ExecMode::Capture, InWorkingDir);
            }
            return shell::ExecuteCommand("copilot", {"-s", "-p", InPrompt, "--no-color", "--stream", "off", "--no-ask-user"}, shell::ExecMode::Capture, InWorkingDir);
        }

        if (HasCommand("gh", {"copilot", "--version"})) {
            if (!InModel.empty() && InModel != "auto") {
                return shell::ExecuteCommand("gh", {"copilot", "--", "-s", "-p", InPrompt, "--model", InModel, "--no-color", "--stream", "off", "--no-ask-user"}, shell::ExecMode::Capture, InWorkingDir);
            }
            return shell::ExecuteCommand("gh", {"copilot", "--", "-s", "-p", InPrompt, "--no-color", "--stream", "off", "--no-ask-user"}, shell::ExecMode::Capture, InWorkingDir);
        }
    }

    return shell::ExecResult{.exitCode = 1, .stderrStr = "unsupported provider or provider command unavailable"};
}

auto GenerateAiCommitMessage(const std::filesystem::path& InWorkspaceRoot,
                             const std::filesystem::path& InRepo,
                             const CommitPreflightReport& InReport,
                             const NativeAiConfig& InAi) -> std::string {
    if (!InAi.enabled) {
        return {};
    }

    const auto prompt = BuildAiCommitPrompt(InWorkspaceRoot, InRepo, InReport);
    const auto out = RunAiGenerate(InAi.provider, InAi.model, prompt, InRepo);
    if (out.exitCode != 0) {
        return {};
    }

    return ExtractSingleLineMessage(out.stdoutStr + "\n" + out.stderrStr);
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

    std::ostringstream prompt;
    prompt << "You are a commit safety reviewer.\n"
           << "Evaluate whether this commit message matches staged changes and is safe.\n"
           << "Respond with exactly one line: PASS or FAIL: <reason>.\n\n"
           << "Message:\n" << InMessage << "\n\n"
           << "Staged diff:\n" << stagedDiff << "\n";

    const auto out = RunAiGenerate(InAi.provider, InAi.model, prompt.str(), InRepo);
    if (out.exitCode != 0) {
        return false;
    }

    const auto verdict = ToLower(ExtractSingleLineMessage(out.stdoutStr + "\n" + out.stderrStr));
    if (verdict.rfind("fail", 0) == 0 || verdict.find(" fail") != std::string::npos) {
        OutReason = verdict;
        return true;
    }
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
    return NormalizePath(InWorkspaceRoot / InPath);
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
        return ".";
    }
    const auto rel = repoNorm.lexically_relative(rootNorm);
    if (!rel.empty() && rel != ".") {
        return rel.generic_string();
    }
    return repoNorm.generic_string();
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

auto DiscoverWorkspaceRepos(const std::filesystem::path& InRoot) -> std::vector<std::filesystem::path> {
    workspace::DiscoverOptions options;
    options.rootDir = InRoot;
    options.maxDepth = 12;
    options.useCache = true;
    options.metadataLevel = "minimal";

    const auto discovery = workspace::DiscoverRepos(options);
    std::vector<std::filesystem::path> repos;
    repos.reserve(discovery.repos.size());

    for (const auto& repo : discovery.repos) {
        repos.push_back(repo.path);
    }

    std::sort(repos.begin(), repos.end(), [](const auto& A, const auto& B) {
        return A.lexically_normal().generic_string() < B.lexically_normal().generic_string();
    });

    repos.erase(std::unique(repos.begin(), repos.end(), [](const auto& A, const auto& B) {
        return A.lexically_normal().generic_string() == B.lexically_normal().generic_string();
    }), repos.end());

    return repos;
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

    auto scope = DisplayRepoLabel(InWorkspaceRoot, InRepo);
    if (scope == ".") {
        scope = "root";
    }
    for (auto& c : scope) {
        if (c == '/' || c == '\\' || c == ' ') {
            c = '-';
        }
    }

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

struct RepoCommitResult {
    std::filesystem::path repo;
    bool committed = false;
    bool pushed = false;
    bool failed = false;
    std::string note;
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
        commitMessage = GenerateAiCommitMessage(InWorkspaceRoot, InRepo, report, InAi);
        if (commitMessage.empty()) {
            commitMessage = BuildAutoCommitMessage(InWorkspaceRoot, InRepo, report);
            result.note = "ai message unavailable; used native fallback";
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
    auto scope = DisplayRepoLabel(InWorkspaceRoot, InRepo);
    if (scope == ".") {
        scope = "root";
    }
    for (auto& c : scope) {
        if (c == '/' || c == '\\' || c == ' ') {
            c = '-';
        }
    }

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
            commitMessage = GenerateAiCommitMessage(InWorkspaceRoot, InRepo, report, InAi);
            if (commitMessage.empty()) {
                commitMessage = BuildCombineFallbackMessage(InWorkspaceRoot, InRepo, unpushedCount, report);
                result.note = "combined with native fallback message";
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
        commitMessage = GenerateAiCommitMessage(InWorkspaceRoot, InRepo, report, InAi);
        if (commitMessage.empty()) {
            result.note = "ai message unavailable; amend keeps previous message";
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

        std::cout << std::left << std::setw(36) << repoLabel
                  << std::setw(12) << status
                  << item.note << "\n";
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

void RegisterCommit(CLI::App& InApp) {
    auto* cmd = InApp.add_subcommand("commit", "Native multi-repo commit workflow (pure C++)");

    auto* repos = new std::string{};
    cmd->add_option("--repos", *repos, "Commit target repos (comma-separated). Default: auto-discover workspace repos");
    auto* bNoRecursive = new bool{false};
    cmd->add_flag("--no-recursive,-N", *bNoRecursive, "Commit only current repository when --repos is not provided");

    // Provider option
    auto* provider = new std::string{};
    cmd->add_option("--ai-provider", *provider, "AI provider (copilot, codex, opencode)")
        ->default_str("auto");

    // Model option
    auto* model = new std::string{};
    cmd->add_option("--ai-model", *model, "AI model to use");

    auto* bAiAuto = new bool{false};
    cmd->add_flag("--ai-auto", *bAiAuto, "Enable AI auto mode (auto provider/model with remembered model preference)");

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

    cmd->callback([=]() {
        if (*bShell) {
            std::cerr << "Error: --shell is no longer supported; commit workflow is fully native now\n";
            std::exit(2);
        }

        const auto workspaceRoot = std::filesystem::current_path();

        if (!*bNoNativePreflight || *bPreflightOnly) {
            const auto report = RunCommitPreflight(workspaceRoot);
            PrintCommitPreflight(report, *bStagedOnly);
            if (!report.inRepo) {
                std::exit(1);
            }
            if (*bStagedOnly && report.stagedCount == 0) {
                std::cerr << "Preflight blocked: --staged-only but nothing staged\n";
                std::exit(2);
            }
            if (*bPreflightOnly) {
                std::exit(0);
            }
        }

        NativeAiConfig ai;
        const bool aiRequested = *bAiAuto || !provider->empty() || !model->empty();
        ai.provider = aiRequested ? ResolveProvider(*provider) : std::string{};
        ai.model = aiRequested ? ResolveModelForAi(ai.provider, *model, *bAiAuto) : std::string{};
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

        auto reposCsv = Trim(*repos);
        std::vector<std::filesystem::path> repoList;
        if (reposCsv.empty()) {
            if (*bNoRecursive) {
                repoList.push_back(workspaceRoot);
            } else {
                repoList = BuildOrderedRepoList(workspaceRoot, reposCsv);
                if (repoList.empty()) {
                    repoList.push_back(workspaceRoot);
                }
            }
        } else {
            repoList = BuildOrderedRepoList(workspaceRoot, reposCsv);
            if (repoList.empty()) {
                repoList.push_back(workspaceRoot);
            }
        }

        std::vector<RepoCommitResult> results;
        results.reserve(repoList.size());

        for (const auto& repo : repoList) {
            const auto label = DisplayRepoLabel(workspaceRoot, repo);
            std::cout << "\n[commit] " << label << "\n";
            const auto one = CommitSingleRepo(workspaceRoot, repo, *message, *bStagedOnly, *bPush, ai);
            results.push_back(one);
        }

        const auto exitCode = PrintCommitSummary(workspaceRoot, results);
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
    cmd->add_flag("--ai-auto", *bAiAuto, "Enable AI auto mode (auto provider/model with remembered model preference)");

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
        ai.model = aiRequested ? ResolveModelForAi(ai.provider, *model, *bAiAuto) : std::string{};
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
