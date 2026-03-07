// plan/ignore commands - native plan pipeline and ignore doctor

#include "command_registry.hpp"
#include "discovery.hpp"
#include "shell_executor.hpp"

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

auto ExtractObjectBodyForKey(const std::string& InText, const std::string& InKey) -> std::optional<std::string>;
auto ExtractArrayBodyForKey(const std::string& InText, const std::string& InKey) -> std::optional<std::string>;
auto SplitTopLevelObjects(const std::string& InArrayBody) -> std::vector<std::string>;
auto ExtractStringField(const std::string& InObjectText, const std::string& InField) -> std::optional<std::string>;
auto ResolveSkillRoot(const std::filesystem::path& InWorkspaceRoot) -> std::filesystem::path;
auto ComputeWorkspaceBaseHeadSha(const std::filesystem::path& InWorkspaceRoot) -> std::string;
auto ComputeWorkspaceDirtyFingerprint(const std::filesystem::path& InWorkspaceRoot) -> std::string;

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

auto ResolveAiProvider(const std::string& InRequested) -> std::string {
    const auto provider = ToLower(Trim(InRequested));
    if (!provider.empty() && provider != "auto") {
        if (provider == "copilot" || provider == "codex" || provider == "opencode") {
            return provider;
        }
        return {};
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

auto ResolveAiModel(const std::string& InProvider, const std::string& InRequested) -> std::string {
    const auto model = Trim(InRequested);
    if (!model.empty() && model != "auto") {
        return model;
    }
    if (InProvider == "copilot" || InProvider == "codex" || InProvider == "opencode") {
        return "gpt-5-mini";
    }
    return {};
}

auto RunAiGenerate(const std::string& InProvider,
                   const std::string& InModel,
                   const std::string& InPrompt,
                   const std::filesystem::path& InWorkdir) -> shell::ExecResult {
    if (InProvider == "opencode") {
        if (!InModel.empty()) {
            return shell::ExecuteCommand("opencode", {"run", "--model", InModel, InPrompt}, shell::ExecMode::Capture, InWorkdir);
        }
        return shell::ExecuteCommand("opencode", {"run", InPrompt}, shell::ExecMode::Capture, InWorkdir);
    }
    if (InProvider == "codex") {
        if (!InModel.empty()) {
            return shell::ExecuteCommand("codex", {"-q", "--model", InModel, InPrompt}, shell::ExecMode::Capture, InWorkdir);
        }
        return shell::ExecuteCommand("codex", {"-q", InPrompt}, shell::ExecMode::Capture, InWorkdir);
    }
    if (InProvider == "copilot") {
        if (HasCommand("copilot", {"--help"})) {
            if (!InModel.empty()) {
                return shell::ExecuteCommand("copilot",
                                             {"-s", "-p", InPrompt, "--model", InModel, "--no-color", "--stream", "off", "--no-ask-user"},
                                             shell::ExecMode::Capture,
                                             InWorkdir);
            }
            return shell::ExecuteCommand("copilot",
                                         {"-s", "-p", InPrompt, "--no-color", "--stream", "off", "--no-ask-user"},
                                         shell::ExecMode::Capture,
                                         InWorkdir);
        }
        if (HasCommand("gh", {"copilot", "--version"})) {
            if (!InModel.empty()) {
                return shell::ExecuteCommand(
                    "gh",
                    {"copilot", "--", "-s", "-p", InPrompt, "--model", InModel, "--no-color", "--stream", "off", "--no-ask-user"},
                    shell::ExecMode::Capture,
                    InWorkdir);
            }
            return shell::ExecuteCommand("gh",
                                         {"copilot", "--", "-s", "-p", InPrompt, "--no-color", "--stream", "off", "--no-ask-user"},
                                         shell::ExecMode::Capture,
                                         InWorkdir);
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

auto BuildPlanPrompt(const std::filesystem::path& InRoot,
                     const std::string& InProvider,
                     const std::string& InModel,
                     const std::string& InTemplateJson,
                     const std::string& InDirtyContext) -> std::string {
    std::vector<std::filesystem::path> candidates;
    if (const char* custom = std::getenv("KOG_PLAN_PROMPT_TEMPLATE"); custom != nullptr && std::string(custom).size() > 0) {
        candidates.emplace_back(std::filesystem::path(custom).lexically_normal());
    }
    candidates.emplace_back((InRoot / "assets" / "prompts" / "base" / "plan-init.md").lexically_normal());
    candidates.emplace_back(
        (InRoot / ".agents" / "skills" / "kano" / "kano-git-master-skill" / "assets" / "prompts" / "base" / "plan-init.md").lexically_normal());

    std::optional<std::filesystem::path> promptPath;
    for (const auto& candidate : candidates) {
        if (std::error_code ec; std::filesystem::exists(candidate, ec) && !ec) {
            promptPath = candidate;
            break;
        }
    }
    if (promptPath.has_value()) {
        if (const auto text = ReadFileText(*promptPath); text.has_value()) {
            auto prompt = *text;
            prompt = ReplaceAll(std::move(prompt), "{{PROVIDER}}", InProvider);
            prompt = ReplaceAll(std::move(prompt), "{{MODEL}}", InModel);
            prompt = ReplaceAll(std::move(prompt), "{{TEMPLATE_JSON}}", InTemplateJson);
            prompt = ReplaceAll(std::move(prompt), "{{DIRTY_CONTEXT}}", InDirtyContext);
            return prompt;
        }
    }
    std::ostringstream fallback;
    fallback << "You are generating a complete plan JSON for kano-git.\n";
    fallback << "Return STRICT JSON ONLY between markers:\nBEGIN_KOG_PLAN_JSON\n<json>\nEND_KOG_PLAN_JSON\n\n";
    fallback << "Template JSON:\n" << InTemplateJson << "\n\n";
    fallback << "Workspace dirty context:\n" << InDirtyContext << "\n";
    return fallback.str();
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
    std::ostringstream oss;
    oss << R"json({
  "meta": {
    "schema_version": "3",
    "plan_id": "replace-with-unique-id",
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
      "provider": "replace-with-provider",
      "ai-model": "replace-with-ai-model"
    },
    "review": {
      "verdict": "pass",
      "reason": "replace-with-review-summary"
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
    return ReplaceArrayBodyForKey(InPlanText, "commit", body);
}

auto BuildFallbackCommitEntriesJson(const std::filesystem::path& InWorkspaceRoot) -> std::string {
    return BuildCommitSeedEntriesJson(InWorkspaceRoot, false);
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

    std::ostringstream commitBody;
    for (std::size_t i = 0; i < repoObjects.size(); ++i) {
        if (i != 0) {
            commitBody << ",";
        }
        commitBody << repoObjects[i];
    }
    return ReplaceArrayBodyForKey(InPlanText, "commit", commitBody.str());
}

auto FillPlanByAi(const std::filesystem::path& InWorkspaceRoot,
                  const std::filesystem::path& InPlanPath,
                  const std::string& InRequestedProvider,
                  const std::string& InRequestedModel,
                  bool InDebugAi,
                  std::string* OutError = nullptr) -> bool {
    const auto provider = ResolveAiProvider(InRequestedProvider);
    if (provider.empty()) {
        if (OutError != nullptr) {
            *OutError = "Error: no AI provider found for plan new --ai (supported: codex/opencode/copilot).";
        }
        return false;
    }
    const auto model = ResolveAiModel(provider, InRequestedModel);
    const auto dirtyContext = CollectDirtyRepoContextText(InWorkspaceRoot);
    if (Trim(dirtyContext).empty()) {
        if (OutError != nullptr) {
            *OutError = "Error: no dirty repository changes found; nothing to generate for plan.";
        }
        return false;
    }
    const auto templateJson = ReadFileText(InPlanPath).value_or(std::string{});
    const auto prompt = BuildPlanPrompt(InWorkspaceRoot, provider, model, templateJson, dirtyContext);
    const auto aiRaw = RunAiGenerate(provider, model, prompt, InWorkspaceRoot);
    if (aiRaw.exitCode != 0) {
        if (OutError != nullptr) {
            std::ostringstream oss;
            oss << "Error: AI provider command failed for plan generation.";
            if (!Trim(aiRaw.stderrStr).empty()) {
                oss << " Detail: " << Trim(aiRaw.stderrStr);
            }
            *OutError = oss.str();
        }
        return false;
    }
    const auto aiCombined = aiRaw.stdoutStr + "\n" + aiRaw.stderrStr;
    const auto aiJson = ExtractJsonBetweenMarkers(aiCombined);
    std::string debugPrefix;
    if (InDebugAi) {
        const auto debugDir = (InWorkspaceRoot / ".kano" / "cache" / "git" / "plans" / "debug").lexically_normal();
        debugPrefix = (debugDir / std::format("pia-{}", CurrentUtcCompact())).generic_string();
        std::string debugError;
        WriteFileText(std::filesystem::path(debugPrefix + ".prompt.txt"), prompt, &debugError);
        WriteFileText(std::filesystem::path(debugPrefix + ".raw.txt"), aiCombined, &debugError);
        WriteFileText(std::filesystem::path(debugPrefix + ".extracted.json"), aiJson, &debugError);
    }
    if (aiJson.empty()) {
        if (OutError != nullptr) {
            *OutError = "Error: AI did not return JSON payload for plan.";
            if (!debugPrefix.empty()) {
                *OutError += std::format(" Debug: {}.raw.txt", debugPrefix);
            }
        }
        return false;
    }
    std::string validationReason;
    if (!ValidateAiReadyPlan(aiJson, &validationReason)) {
        if (OutError != nullptr) {
            *OutError = "Error: AI JSON payload schema invalid for plan generation.";
            if (!Trim(validationReason).empty()) {
                *OutError += " Detail: " + validationReason;
            }
            if (!debugPrefix.empty()) {
                *OutError += std::format(" Debug: {}.raw.txt {}.extracted.json", debugPrefix, debugPrefix);
            }
        }
        return false;
    }
    std::string error;
    if (!WriteFileText(InPlanPath, aiJson, &error)) {
        if (OutError != nullptr) {
            std::ostringstream oss;
            oss << "Error: failed to write AI plan output: " << InPlanPath.generic_string();
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
    }
    std::cout << "Filled plan with AI: provider=" << provider << " model=" << (model.empty() ? "auto" : model) << "\n";
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
        e.mergedOutputPath = (InWorkspaceRoot / ".kano" / "cache" / "git" / "plans" /
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
    return (InWorkspaceRoot / ".kano" / "cache" / "git" / "plans" / "default-plan.json").lexically_normal();
}

auto ResolveSkillRoot(const std::filesystem::path& InWorkspaceRoot) -> std::filesystem::path {
    if (const char* envRoot = std::getenv("KANO_GIT_SKILL_ROOT"); envRoot != nullptr && std::string(envRoot).size() > 0) {
        return std::filesystem::path(envRoot).lexically_normal();
    }
    return (InWorkspaceRoot / ".agents" / "skills" / "kano" / "kano-git-master-skill").lexically_normal();
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

    auto* init = cmd->add_subcommand("new", "Write plan template");
    auto* initOut = new std::string{};
    auto* initForce = new bool{false};
    auto* initAiAuto = new bool{false};
    auto* initAiProvider = new std::string{"auto"};
    auto* initAiModel = new std::string{"auto"};
    auto* initDebugAi = new bool{false};
    auto* initAllowIgnoreGate = new bool{false};
    auto* initDatasourceRoot = new std::string{};
    auto* initDatasourceManifest = new std::string{};
    init->add_option("--output,-o", *initOut, "Plan output path (default: .kano/cache/git/plans/default-plan.json)");
    init->add_flag("--force,-f", *initForce, "Overwrite existing output");
    init->add_flag("--ai-auto,--ai", *initAiAuto, "Generate and fill plan by AI");
    init->add_option("--ai-provider,--provider", *initAiProvider, "AI provider (copilot|codex|opencode|auto)")->default_str("auto");
    init->add_option("--ai-model,--model", *initAiModel, "AI model (default auto)")->default_str("auto");
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
            if (!FillPlanByAi(workspaceRoot, outPath, *initAiProvider, *initAiModel, *initDebugAi, &aiError)) {
                std::cerr << aiError << "\n";
                std::exit(2);
            }
        }

        std::cout << "Wrote plan template: " << outPath.generic_string() << "\n";
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
    addCommitEntry->add_option("--repo", *addCommitRepo, "Target repo path/key (e.g. . or .agents/skills/kano)")->required();
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
                lines.push_back(std::format("[plan] - {}: {}", repo, msg));
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
    auto* ensureModel = new std::string{"auto"};
    auto* ensureDebugAi = new bool{false};
    auto* ensureAllowIgnoreGate = new bool{false};
    auto* ensureForce = new bool{false};
    ensureAiReady->add_option("--plan-file", *ensureFile, "Plan file path");
    ensureAiReady->add_option("--ai-provider,--provider", *ensureProvider, "AI provider (copilot|codex|opencode|auto)")->default_str("auto");
    ensureAiReady->add_option("--ai-model,--model", *ensureModel, "AI model (default auto)")->default_str("auto");
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
            if (!FillPlanByAi(workspaceRoot, planPath, *ensureProvider, *ensureModel, *ensureDebugAi, &aiError)) {
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
                if (reason == "no commit entries in stages.commit" && latestPayload.has_value()) {
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
    auto* preflightModel = new std::string{"auto"};
    auto* preflightDebugAi = new bool{false};
    auto* preflightAllowIgnoreGate = new bool{false};
    auto* preflightMaxCommits = new int{10};
    preflightAiCommit->add_option("--plan-file", *preflightFile, "Plan file path");
    preflightAiCommit->add_option("--ai-provider,--provider", *preflightProvider, "AI provider (copilot|codex|opencode|auto)")
        ->default_str("auto");
    preflightAiCommit->add_option("--ai-model,--model", *preflightModel, "AI model (default auto)")->default_str("auto");
    preflightAiCommit->add_flag("--debug-ai", *preflightDebugAi, "Write AI prompt/raw/extracted debug artifacts");
    preflightAiCommit->add_flag("--allow-ignore-gate", *preflightAllowIgnoreGate, "Compatibility flag (currently no-op in runbook-commit)");
    preflightAiCommit->add_option("--max-commits", *preflightMaxCommits, "Max commit lines to print in summary")->default_val(10);
    const auto runCommitRunbook = [&](const std::filesystem::path& InWorkspaceRoot,
                                      const std::filesystem::path& InPlanPath,
                                      const std::string& InProvider,
                                      const std::string& InModel,
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
            if (!FillPlanByAi(InWorkspaceRoot, InPlanPath, InProvider, InModel, InDebugAi, &aiError)) {
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
        const auto code =
            runCommitRunbook(workspaceRoot, planPath, *preflightProvider, *preflightModel, *preflightDebugAi, *preflightMaxCommits);
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
    auto* runbookFullModel = new std::string{"auto"};
    auto* runbookFullDebugAi = new bool{false};
    auto* runbookFullAllowIgnoreGate = new bool{false};
    auto* runbookFullForce = new bool{false};
    auto* runbookFullMaxCommits = new int{10};
    auto* runbookFullMaxPerRepo = new int{200};
    runbookFull->add_option("--plan-file", *runbookFullFile, "Plan file path");
    runbookFull->add_option("--ai-provider,--provider", *runbookFullProvider, "AI provider (copilot|codex|opencode|auto)")->default_str("auto");
    runbookFull->add_option("--ai-model,--model", *runbookFullModel, "AI model (default auto)")->default_str("auto");
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
        const auto commitCode =
            runCommitRunbook(workspaceRoot, planPath, *runbookFullProvider, *runbookFullModel, *runbookFullDebugAi, *runbookFullMaxCommits);
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
    auto* rbCommitModel = new std::string{"auto"};
    auto* rbCommitDebugAi = new bool{false};
    auto* rbCommitAllowIgnoreGate = new bool{false};
    auto* rbCommitMaxCommits = new int{10};
    runbookCommit->add_option("--plan-file", *rbCommitFile, "Plan file path");
    runbookCommit->add_option("--ai-provider,--provider", *rbCommitProvider, "AI provider (copilot|codex|opencode|auto)")->default_str("auto");
    runbookCommit->add_option("--ai-model,--model", *rbCommitModel, "AI model (default auto)")->default_str("auto");
    runbookCommit->add_flag("--debug-ai", *rbCommitDebugAi, "Write AI prompt/raw/extracted debug artifacts");
    runbookCommit->add_flag("--allow-ignore-gate", *rbCommitAllowIgnoreGate, "Forward allow-ignore-gate to commit runbook");
    runbookCommit->add_option("--max-commits", *rbCommitMaxCommits, "Max commit lines to print in summary")->default_val(10);
    runbookCommit->callback([=]() {
        const auto workspaceRoot = std::filesystem::current_path().lexically_normal();
        const auto planPath = rbCommitFile->empty() ? DefaultPlanPath(workspaceRoot) : std::filesystem::path(*rbCommitFile).lexically_normal();
        const auto code = runCommitRunbook(workspaceRoot, planPath, *rbCommitProvider, *rbCommitModel, *rbCommitDebugAi, *rbCommitMaxCommits);
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
    auto* rbFullModel = new std::string{"auto"};
    auto* rbFullDebugAi = new bool{false};
    auto* rbFullAllowIgnoreGate = new bool{false};
    auto* rbFullForce = new bool{false};
    auto* rbFullMaxCommits = new int{10};
    auto* rbFullMaxPerRepo = new int{200};
    runbookFullPublic->add_option("--plan-file", *rbFullFile, "Plan file path");
    runbookFullPublic->add_option("--ai-provider,--provider", *rbFullProvider, "AI provider (copilot|codex|opencode|auto)")->default_str("auto");
    runbookFullPublic->add_option("--ai-model,--model", *rbFullModel, "AI model (default auto)")->default_str("auto");
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
            runCommitRunbook(workspaceRoot, planPath, *rbFullProvider, *rbFullModel, *rbFullDebugAi, *rbFullMaxCommits);
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
                    ? (workspaceRoot / ".kano" / "cache" / "git" / "plans" / std::format("ignore-merged-{}.gitignore", idx)).lexically_normal()
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
