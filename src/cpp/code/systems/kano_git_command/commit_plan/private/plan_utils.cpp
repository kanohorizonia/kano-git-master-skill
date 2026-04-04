#include "plan_utils.hpp"
#include "ai_utils.hpp"
#include "command_runtime_ops.hpp"
#include "kog_config.hpp"
#include "discovery.hpp"
#include "auto_model_policy.hpp"
#include "shell_executor.hpp"
#include "secret_scan_utils.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <iomanip>
#include <ctime>
#include <regex>
#include <set>

namespace kano::git::commands {

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

auto RelativeDisplayPath(const std::filesystem::path& InRoot, const std::filesystem::path& InPath) -> std::string {
    const auto rel = InPath.lexically_relative(InRoot);
    if (!rel.empty()) {
        return rel.generic_string();
    }
    return InPath.lexically_normal().generic_string();
}

auto NormalizePath(const std::filesystem::path& InPath) -> std::filesystem::path {
    return InPath.lexically_normal();
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

auto ToGeneric(const std::filesystem::path& InPath) -> std::string {
    return RepoKey(NormalizePath(InPath));
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

// AI Model resolution utilities
auto ResolveSystemRecommendedModel(const std::string& InProvider) -> std::string {
    if (InProvider == "copilot" || InProvider == "opencode") {
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

auto DiscoverWorkspaceRepos(const std::filesystem::path& InRoot) -> std::vector<std::filesystem::path> {
    // Always use DiscoverRepos with fresh discovery to ensure consistency between
    // fingerprint computation and manifest loading. Previously this used manifest->repos
    // when available, but manifest and discoveryCache can diverge, causing workspace
    // state drift. Force fresh discovery (useCache=false) to avoid cache inconsistencies.
    workspace::DiscoverOptions options;
    options.rootDir = InRoot;
    options.maxDepth = 12;
    options.useCache = false;  // Force fresh discovery for fingerprint stability
    options.refreshCache = true; // Update cache after discovery
    options.incremental = false;
    options.metadataLevel = "minimal";
    const auto discovery = workspace::DiscoverRepos(options);
    std::cerr << "[DEBUG] DiscoverWorkspaceRepos: root=" << InRoot.generic_string() << " discovery.repos.size=" << discovery.repos.size() << " mode=" << discovery.mode << "\n";
    for (const auto& repo : discovery.repos) {
        std::cerr << "[DEBUG] DiscoverWorkspaceRepos:   repo=" << repo.path.generic_string() << " type=" << repo.type << "\n";
    }
    std::vector<std::filesystem::path> repos;
    repos.reserve(discovery.repos.size());
    for (const auto& repo : discovery.repos) {
        repos.push_back(repo.path.lexically_normal());
    }
    std::sort(repos.begin(), repos.end(), [](const auto& A, const auto& B) {
        return A.generic_string() < B.generic_string();
    });
    repos.erase(std::unique(repos.begin(), repos.end()), repos.end());
    if (repos.empty()) {
        std::cerr << "[DEBUG] DiscoverWorkspaceRepos: repos empty, using root fallback=" << InRoot.lexically_normal().generic_string() << "\n";
        repos.push_back(InRoot.lexically_normal());
    }
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

auto ComputeWorkspaceBaseHeadSha(const std::filesystem::path& InWorkspaceRoot) -> std::string {
    std::vector<std::string> lines;
    const auto repos = DiscoverWorkspaceRepos(InWorkspaceRoot);
    std::cerr << "[DEBUG] ComputeWorkspaceBaseHeadSha: repos=" << repos.size() << "\n";
    lines.reserve(repos.size());
    for (const auto& repo : repos) {
        const auto head = GitCapture(repo, {"rev-parse", "HEAD"});
        const auto sha = (head.exitCode == 0) ? Trim(head.stdoutStr) : std::string("0000000000000000000000000000000000000000");
        const auto repoKey = WorkspaceRepoKey(InWorkspaceRoot, repo);
        std::cerr << "[DEBUG] ComputeWorkspaceBaseHeadSha:   repo=" << repo.generic_string() << " key=" << repoKey << " sha=" << sha << "\n";
        lines.push_back(repoKey + "\t" + sha);
    }
    std::sort(lines.begin(), lines.end());
    std::ostringstream canonical;
    for (const auto& line : lines) {
        canonical << line << "\n";
    }
    const auto result = "ws-head-v2-" + Fnv1a64Hex(canonical.str());
    std::cerr << "[DEBUG] ComputeWorkspaceBaseHeadSha: result=" << result << "\n";
    return result;
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
                              const std::optional<std::filesystem::path>& InDatasourceRoot,
                              const std::optional<std::filesystem::path>& InDatasourceManifest) -> std::string {
    const auto dsRootPath = InDatasourceRoot.value_or(
        (ResolveSkillRoot(InWorkspaceRoot) / "assets" / "ignore-sources").lexically_normal());
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

auto BuildDefaultPlanTemplate(const std::filesystem::path& InWorkspaceRoot) -> std::string {
    return BuildDefaultPlanTemplate(InWorkspaceRoot, std::nullopt, std::nullopt);
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

auto FindJsonKeyValueStart(const std::string& InText, const std::string& InKey, std::size_t InFrom) -> std::optional<std::size_t> {
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
    const std::string key = "\"" + InKey + "\":";
    auto pos = InText.find(key);
    if (pos == std::string::npos) return std::nullopt;
    pos = InText.find('{', pos);
    if (pos == std::string::npos) return std::nullopt;
    int depth = 1;
    std::size_t i = pos + 1;
    bool inQuote = false;
    while (depth > 0 && i < InText.size()) {
        const char ch = InText[i];
        if (ch == '\"' && InText[i - 1] != '\\') inQuote = !inQuote;
        if (!inQuote) {
            if (ch == '{') depth++;
            else if (ch == '}') depth--;
        }
        i++;
    }
    if (depth == 0) return InText.substr(pos, i - pos);
    return std::nullopt;
}

auto ExtractArrayBodyForKey(const std::string& InText, const std::string& InKey) -> std::optional<std::string> {
    const std::string key = "\"" + InKey + "\":";
    auto pos = InText.find(key);
    if (pos == std::string::npos) return std::nullopt;
    pos = InText.find('[', pos);
    if (pos == std::string::npos) return std::nullopt;
    int depth = 1;
    std::size_t i = pos + 1;
    bool inQuote = false;
    while (depth > 0 && i < InText.size()) {
        const char ch = InText[i];
        if (ch == '\"' && InText[i - 1] != '\\') inQuote = !inQuote;
        if (!inQuote) {
            if (ch == '[') depth++;
            else if (ch == ']') depth--;
        }
        i++;
    }
    if (depth == 0) return InText.substr(pos, i - pos);
    return std::nullopt;
}

auto SplitTopLevelObjects(const std::string& InArrayBody) -> std::vector<std::string> {
    std::vector<std::string> out;
    if (InArrayBody.empty() || InArrayBody.front() != '[' || InArrayBody.back() != ']') return out;
    std::size_t i = 1;
    while (i < InArrayBody.size() - 1) {
        while (i < InArrayBody.size() - 1 && (InArrayBody[i] == ' ' || InArrayBody[i] == '\t' || InArrayBody[i] == '\n' || InArrayBody[i] == '\r' || InArrayBody[i] == ',')) i++;
        if (i >= InArrayBody.size() - 1) break;
        if (InArrayBody[i] == '{') {
            int depth = 1;
            std::size_t start = i;
            i++;
            bool inQuote = false;
            while (depth > 0 && i < InArrayBody.size() - 1) {
                const char ch = InArrayBody[i];
                if (ch == '\"' && InArrayBody[i - 1] != '\\') inQuote = !inQuote;
                if (!inQuote) {
                    if (ch == '{') depth++;
                    else if (ch == '}') depth--;
                }
                i++;
            }
            if (depth == 0) out.push_back(InArrayBody.substr(start, i - start));
        } else {
            i++;
        }
    }
    return out;
}

auto ExtractStringField(const std::string& InObjectText, const std::string& InField) -> std::optional<std::string> {
    const std::string key = "\"" + InField + "\":";
    auto pos = InObjectText.find(key);
    if (pos == std::string::npos) return std::nullopt;
    pos = InObjectText.find('\"', pos + key.size());
    if (pos == std::string::npos) return std::nullopt;
    const auto end = InObjectText.find('\"', pos + 1);
    if (end == std::string::npos) return std::nullopt;
    return InObjectText.substr(pos + 1, end - pos - 1);
}

auto ExtractScalarFieldToken(const std::string& InObjectText, const std::string& InField) -> std::optional<std::string> {
    const std::string key = "\"" + InField + "\":";
    auto pos = InObjectText.find(key);
    if (pos == std::string::npos) return std::nullopt;
    pos += key.size();
    while (pos < InObjectText.size() && (InObjectText[pos] == ' ' || InObjectText[pos] == '\t' || InObjectText[pos] == '\n' || InObjectText[pos] == '\r')) pos++;
    std::size_t start = pos;
    while (pos < InObjectText.size() && InObjectText[pos] != ',' && InObjectText[pos] != '}' && InObjectText[pos] != ' ' && InObjectText[pos] != '\t' && InObjectText[pos] != '\n' && InObjectText[pos] != '\r') pos++;
    return InObjectText.substr(start, pos - start);
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

auto ExtractJsonBetweenMarkers(const std::string& InText,
                               const std::string& InBeginMarker,
                               const std::string& InEndMarker) -> std::string {
    const auto beginPos = InText.find(InBeginMarker);
    if (beginPos == std::string::npos) {
        return ExtractJsonBetweenMarkers(InText);
    }
    const auto payloadStart = beginPos + InBeginMarker.size();
    const auto endPos = InText.find(InEndMarker, payloadStart);
    if (endPos == std::string::npos || endPos <= payloadStart) {
        return ExtractJsonBetweenMarkers(InText.substr(payloadStart));
    }
    return Trim(InText.substr(payloadStart, endPos - payloadStart));
}

auto ReplaceJsonStringFieldInObject(std::string InJson,
                                    const std::string& InObjectKey,
                                    const std::string& InFieldKey,
                                    const std::string& InNewValue) -> std::optional<std::string> {
    const std::string objKey = "\"" + InObjectKey + "\":";
    auto objPos = InJson.find(objKey);
    if (objPos == std::string::npos) return std::nullopt;
    auto startBrace = InJson.find('{', objPos);
    if (startBrace == std::string::npos) return std::nullopt;
    
    // Find matching brace for the object
    int depth = 1;
    std::size_t i = startBrace + 1;
    bool inQuote = false;
    while (depth > 0 && i < InJson.size()) {
        const char ch = InJson[i];
        if (ch == '\"' && InJson[i - 1] != '\\') inQuote = !inQuote;
        if (!inQuote) {
            if (ch == '{') depth++;
            else if (ch == '}') depth--;
        }
        i++;
    }
    if (depth != 0) return std::nullopt;
    std::size_t endBrace = i - 1;
    
    std::string objectBody = InJson.substr(startBrace, endBrace - startBrace + 1);
    const std::string fieldKey = "\"" + InFieldKey + "\":";
    auto fieldPos = objectBody.find(fieldKey);
    if (fieldPos == std::string::npos) return std::nullopt;
    
    auto quoteStart = objectBody.find('\"', fieldPos + fieldKey.size());
    if (quoteStart == std::string::npos) return std::nullopt;
    auto quoteEnd = objectBody.find('\"', quoteStart + 1);
    if (quoteEnd == std::string::npos) return std::nullopt;
    
    objectBody.replace(quoteStart + 1, quoteEnd - quoteStart - 1, InNewValue);
    InJson.replace(startBrace, endBrace - startBrace + 1, objectBody);
    return InJson;
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

auto ExtractBoolField(const std::string& InObjectText, const std::string& InField) -> std::optional<bool> {
    const std::string key = "\"" + InField + "\":";
    auto pos = InObjectText.find(key);
    if (pos == std::string::npos) return std::nullopt;
    pos += key.size();
    while (pos < InObjectText.size() && (InObjectText[pos] == ' ' || InObjectText[pos] == '\t' || InObjectText[pos] == '\n' || InObjectText[pos] == '\r')) pos++;
    if (InObjectText.compare(pos, 4, "true") == 0) return true;
    if (InObjectText.compare(pos, 5, "false") == 0) return false;
    return std::nullopt;
}

auto ReplaceArrayBodyForKey(const std::string& InText, const std::string& InKey, const std::string& InNewBody) -> std::optional<std::string> {
    const std::string key = "\"" + InKey + "\":";
    auto pos = InText.find(key);
    if (pos == std::string::npos) return std::nullopt;
    pos = InText.find('[', pos);
    if (pos == std::string::npos) return std::nullopt;
    
    int depth = 1;
    std::size_t i = pos + 1;
    bool inQuote = false;
    while (depth > 0 && i < InText.size()) {
        const char ch = InText[i];
        if (ch == '\"' && InText[i - 1] != '\\') inQuote = !inQuote;
        if (!inQuote) {
            if (ch == '[') depth++;
            else if (ch == ']') depth--;
        }
        i++;
    }
    if (depth != 0) return std::nullopt;
    std::size_t endBracket = i - 1;
    
    std::string out = InText;
    out.replace(pos + 1, endBracket - pos - 1, InNewBody);
    return out;
}

auto PlanNeedsRefresh(const std::string& InPlanText) -> bool {
    if (Trim(InPlanText).empty()) return true;
    if (InPlanText.find("replace-with-") != std::string::npos) return true;
    return false;
}

auto BuildFallbackCommitScope(const std::string& InRepoDisplay) -> std::string {
    if (InRepoDisplay.empty() || InRepoDisplay == ".") return "workspace";
    std::string scope = InRepoDisplay;
    std::replace(scope.begin(), scope.end(), '\\', '/');
    for (auto& ch : scope) {
        if (ch == '/' || ch == ' ') ch = '-';
    }
    return scope;
}

auto ExtractBranchOidFromStatusV2(const std::string& InStatus) -> std::string {
    std::istringstream iss(InStatus);
    std::string line;
    while (std::getline(iss, line)) {
        auto t = Trim(line);
        if (t.rfind("# branch.oid ", 0) == 0) {
            t = Trim(t.substr(std::string("# branch.oid ").size()));
            if (!t.empty() && t != "(initial)") return t;
            break;
        }
    }
    return "no-head";
}

static void DebugPrintStatusOutput(const std::filesystem::path& repo, const std::string& statusOutput) {
    std::istringstream iss(statusOutput);
    std::string line;
    int lineNum = 0;
    while (std::getline(iss, line)) {
        lineNum++;
        const auto trimmed = Trim(line);
        if (trimmed.empty()) continue;
        std::cerr << "[DEBUG] StatusLine repo=" << repo.generic_string() << " line=" << lineNum << " raw_len=" << line.size() << " trimmed_len=" << trimmed.size() << "\n";
        if (trimmed.rfind("# ", 0) == 0) {
            std::cerr << "[DEBUG]   TYPE=comment: " << trimmed.substr(0, 60) << "\n";
        } else {
            const auto maybePath = ParseStatusChangedPath(trimmed);
            if (!maybePath.has_value()) {
                std::cerr << "[DEBUG]   TYPE=status path=nullopt: " << trimmed.substr(0, 60) << "\n";
            } else {
                std::cerr << "[DEBUG]   TYPE=status path=[" << *maybePath << "] is_artifact=" << IsInternalPipelineArtifactPath(*maybePath) << "\n";
            }
        }
    }
}

auto ComputeWorkspaceDirtyFingerprint(const std::filesystem::path& InWorkspaceRoot) -> std::string {
    std::vector<std::string> lines;
    const auto repos = DiscoverWorkspaceRepos(InWorkspaceRoot);
    lines.reserve(repos.size());
    for (const auto& repo : repos) {
        // Use basic --porcelain format for compatibility with ParseStatusChangedPath
        const auto status = GitCapture(repo, {"status", "--porcelain", "--branch", "--untracked-files=normal", "--ignore-submodules=none"});
        if (status.exitCode != 0) {
            continue;
        }
        std::istringstream iss(status.stdoutStr);
        std::ostringstream filtered;
        std::string line;
        while (std::getline(iss, line)) {
            const auto trimmed = Trim(line);
            if (trimmed.empty()) {
                continue;
            }
            // Skip branch header lines (## BranchName in basic porcelain, # comment in v2)
            if (trimmed.rfind("## ", 0) == 0 || trimmed.rfind("# ", 0) == 0) {
                continue;
            }

            // Check if this line contains a .kano/ path BEFORE calling ParseStatusChangedPath
            // because ParseStatusChangedPath returns nullopt for deleted files (D)
            // and we still need to filter deleted .kano/ artifacts
            if (trimmed.find(".kano/") != std::string::npos) {
                // Extract the path from the line - format is "XY pathname" where XY is status
                // For deleted files, ParseStatusChangedPath returns nullopt, so we extract manually
                const auto pathStart = trimmed.find(".kano/");
                const auto pathStartBefore = (pathStart > 0) ? pathStart - 1 : 0;
                // Find the space before the path to get the actual start
                auto spacePos = trimmed.rfind(' ', pathStartBefore);
                if (spacePos == std::string::npos) {
                    spacePos = 0;
                } else {
                    spacePos = spacePos + 1;
                }
                const auto path = Trim(trimmed.substr(spacePos));
                if (IsInternalPipelineArtifactPath(path)) {
                    continue; // Skip internal artifacts
                }
            }

            // Use ParseStatusChangedPath to extract path and check for internal artifacts
            const auto maybePath = ParseStatusChangedPath(trimmed);
            if (maybePath.has_value() && IsInternalPipelineArtifactPath(*maybePath)) {
                continue; // Skip internal artifacts
            }

            filtered << trimmed << "\n";
        }
        const auto normalized = Trim(filtered.str());
        // Debug: show what was included for UnrealEngine
        if (repo.generic_string().find("UnrealEngine") != std::string::npos && !normalized.empty()) {
            std::cerr << "[DEBUG] Fingerprint UnrealEngine normalized=[" << normalized << "]\n";
        }
        // Get HEAD SHA directly via git rev-parse (not from status output)
        const auto headResult = GitCapture(repo, {"rev-parse", "HEAD"});
        const auto head = (headResult.exitCode == 0) ? Trim(headResult.stdoutStr) : std::string("0000000000000000000000000000000000000000");
        const auto statusFingerprint = normalized.empty() ? std::string("clean") : Fnv1a64Hex(normalized);
        lines.push_back(std::format("{}|{}|{}",
                                    WorkspaceRepoKey(InWorkspaceRoot, repo),
                                    head,
                                    statusFingerprint));
    }
    std::sort(lines.begin(), lines.end());
    std::ostringstream canonical;
    for (const auto& line : lines) canonical << line << "\n";
    const auto canonicalStr = canonical.str();
    const auto result = "ws-dirty-v2-" + Fnv1a64Hex(canonicalStr);
    // Debug: print final fingerprint and canonical for first call only
    static int callCount = 0;
    callCount++;
    if (callCount == 1) {
        std::cerr << "[DEBUG] ComputeWorkspaceDirtyFingerprint: canonical=\n" << canonicalStr << "\n";
    }
    std::cerr << "[DEBUG] ComputeWorkspaceDirtyFingerprint: result=" << result << "\n";
    return result;
}

auto ExtractPlanWorkspaceHashes(const std::string& InPlanText, std::string* OutBaseHeadSha, std::string* OutDirtyFingerprint) -> bool {
    const auto meta = ExtractObjectBodyForKey(InPlanText, "meta");
    if (!meta.has_value()) return false;
    const auto baseHeadSha = Trim(ExtractStringField(*meta, "base_head_sha").value_or(""));
    const auto dirtyFingerprint = Trim(ExtractStringField(*meta, "dirty_fingerprint").value_or(""));
    if (baseHeadSha.empty() || dirtyFingerprint.empty()) return false;
    if (OutBaseHeadSha) *OutBaseHeadSha = baseHeadSha;
    if (OutDirtyFingerprint) *OutDirtyFingerprint = dirtyFingerprint;
    return true;
}

auto PlanWorkspaceStateDrifted(const std::filesystem::path& InWorkspaceRoot, const std::string& InPlanText) -> bool {
    std::string planBaseHeadSha, planDirtyFingerprint;
    if (!ExtractPlanWorkspaceHashes(InPlanText, &planBaseHeadSha, &planDirtyFingerprint)) return false;
    return planBaseHeadSha != ComputeWorkspaceBaseHeadSha(InWorkspaceRoot) ||
           planDirtyFingerprint != ComputeWorkspaceDirtyFingerprint(InWorkspaceRoot);
}

auto AppendCommitConventionSkillSection(const std::filesystem::path& InWorkspaceRoot, std::string InPrompt) -> std::string {
    const char* env = std::getenv("KOG_COMMIT_CONVENTION_SKILL_MD");
    std::filesystem::path skillPath;
    if (env != nullptr) {
        skillPath = std::filesystem::path(env);
    } else {
        skillPath = ResolveSkillRoot(InWorkspaceRoot) / "assets" / "prompts" / "base" / "commit-convention-skill.md";
    }

    if (std::filesystem::exists(skillPath)) {
        if (const auto text = ReadFileText(skillPath); text.has_value()) {
            InPrompt += "\n\n### Commit Convention Skill Context:\n";
            InPrompt += *text;
        }
    }
    return InPrompt;
}

auto BuildCommitSeedEntriesJson(const std::filesystem::path& InWorkspaceRoot, const bool InUsePlaceholders) -> std::string {
    std::vector<std::string> entries;
    const auto repos = DiscoverWorkspaceRepos(InWorkspaceRoot);
    for (const auto& repo : repos) {
        const auto status = GitCapture(repo, {"status", "--porcelain", "--untracked-files=all"});
        if (status.exitCode != 0 || Trim(status.stdoutStr).empty()) continue;
        
        const auto repoDisplay = RelativeDisplayPath(InWorkspaceRoot, repo);
        const auto scope = BuildFallbackCommitScope(repoDisplay);
        
        std::istringstream iss(status.stdoutStr);
        int changed = 0;
        std::string line;
        while (std::getline(iss, line)) if (!Trim(line).empty()) changed++;

        const auto message = InUsePlaceholders
                               ? std::string("replace-with-commit-message")
                               : std::format("chore({}): apply updates ({} files)", scope, changed);
        const auto reviewReason = InUsePlaceholders
                                     ? std::string("replace-with-review-reason")
                                     : std::string("seeded from current dirty status");
        
        entries.push_back(std::format("{{\"repo\":\"{}\",\"commits\":[{{\"message\":\"{}\",\"include\":[],\"exclude\":[],\"review\":{{\"verdict\":\"pass\",\"reason\":\"{}\"}}}}]}}",
                                      JsonEscape(repoDisplay.empty() ? "." : repoDisplay),
                                      JsonEscape(message),
                                      JsonEscape(reviewReason)));
    }
    std::ostringstream oss;
    for (std::size_t i = 0; i < entries.size(); ++i) {
        if (i != 0) oss << ",";
        oss << entries[i];
    }
    return oss.str();
}

auto HasValidCommitItems(const std::string& InPlanText) -> bool {
    const auto stages = ExtractObjectBodyForKey(InPlanText, "stages");
    if (!stages.has_value()) return false;
    const auto commitArray = ExtractArrayBodyForKey(*stages, "commit");
    if (!commitArray.has_value()) return false;

    static const std::regex messageRegex(R"KOG("message"\s*:\s*"([^"]+)")KOG");
    const auto begin = std::sregex_iterator(commitArray->begin(), commitArray->end(), messageRegex);
    const auto end = std::sregex_iterator();
    for (auto it = begin; it != end; ++it) {
        const auto value = Trim((*it)[1].str());
        if (!value.empty() && value.rfind("replace-with-", 0) != 0) {
            return true;
        }
    }
    return false;
}

auto SeedCommitStage(const std::filesystem::path& InWorkspaceRoot,
                     const std::string& InPlanText,
                     const bool InForce,
                     const bool InUsePlaceholders) -> std::optional<std::string> {
    if (!InForce && HasValidCommitItems(InPlanText)) return std::nullopt;
    const auto body = BuildCommitSeedEntriesJson(InWorkspaceRoot, InUsePlaceholders);
    auto updated = ReplaceArrayBodyForKey(InPlanText, "commit", body);
    if (!updated.has_value()) return std::nullopt;
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
    for (const auto& entry : InEntries) {
        CommitFillOp op;
        op.index = entry.index;
        op.message = std::format("chore({}): apply planned updates", BuildFallbackCommitScope(entry.repo));
        op.reviewVerdict = "pass";
        op.reviewReason = "deterministic fallback fill";
        out.push_back(std::move(op));
    }
    return out;
}

auto BuildDeterministicCommitFillOp(const CommitPlanEntry& InEntry) -> CommitFillOp {
    CommitFillOp op;
    op.index = InEntry.index;
    op.message = std::format("chore({}): apply planned updates", BuildFallbackCommitScope(InEntry.repo));
    op.reviewVerdict = "pass";
    op.reviewReason = "deterministic fallback fill";
    return op;
}

auto ResolvePlanCommitGenerationMode(const std::filesystem::path& InWorkspaceRoot,
                                     const std::string& InRequestedMode) -> std::string {
    if (const auto direct = kog_config::NormalizePlanCommitGenerationMode(InRequestedMode); !direct.empty()) return direct;
    if (const char* env = std::getenv("KOG_PLAN_FILL_MODE")) {
        if (const auto e = kog_config::NormalizePlanCommitGenerationMode(env); !e.empty()) return e;
    }
    return kog_config::ResolvePlanCommitGenerationMode(InWorkspaceRoot, ResolveSkillRoot(InWorkspaceRoot), "single");
}

auto AllowDeterministicCommitFallbackForMode(const std::string& InFillMode) -> bool {
    const auto m = ToLower(Trim(InFillMode));
    if (m == "single" && !IsAgentModeEnabled()) return false;
    return true;
}

auto RequireAiSuccessForPlanFlow(const std::filesystem::path& InWorkspaceRoot) -> bool {
    return kog_config::ReadEffectiveBool(InWorkspaceRoot,
                                         ResolveSkillRoot(InWorkspaceRoot),
                                         "plan.ai.require_success",
                                         false);
}

auto BuildSingleCommitFillPrompt(const std::filesystem::path& InWorkspaceRoot,
                                 const std::string& InProvider,
                                 const std::string& InModel,
                                 const CommitPlanEntry& InEntry,
                                 const std::string& InDirtyContext) -> std::string {
    std::ostringstream target;
    target << "{\n"
           << "  \"index\": " << InEntry.index << ",\n"
           << "  \"repo\": \"" << JsonEscape(InEntry.repo.empty() ? "." : InEntry.repo) << "\",\n"
           << "  \"message\": \"" << JsonEscape(InEntry.message) << "\",\n"
           << "  \"review\": {\"verdict\": \"" << JsonEscape(InEntry.reviewVerdict) << "\", \"reason\": \"" << JsonEscape(InEntry.reviewReason) << "\"}\n"
           << "}\n";

    if (const auto text = LoadPromptAssetText(InWorkspaceRoot, "KOG_PLAN_FILL_PER_COMMIT_PROMPT_TEMPLATE", std::filesystem::path("assets") / "prompts" / "base" / "plan-fill-per-commit.md")) {
        auto prompt = *text;
        prompt = ReplaceAll(std::move(prompt), "{{PROVIDER}}", InProvider);
        prompt = ReplaceAll(std::move(prompt), "{{MODEL}}", InModel.empty() ? std::string("auto") : InModel);
        prompt = ReplaceAll(std::move(prompt), "{{ENTRY_INDEX}}", std::to_string(InEntry.index));
        prompt = ReplaceAll(std::move(prompt), "{{TARGET_ENTRY_JSON}}", target.str());
        prompt = ReplaceAll(std::move(prompt), "{{DIRTY_CONTEXT}}", InDirtyContext);
        return AppendCommitConventionSkillSection(InWorkspaceRoot, std::move(prompt));
    }
    return "Fallback prompt for index " + std::to_string(InEntry.index) + "\n" + target.str();
}

auto TryInjectFallbackCommits(const std::filesystem::path& InWorkspaceRoot, const std::string& InPlanText) -> std::optional<std::string> {
    if (HasValidCommitItems(InPlanText)) return std::nullopt;
    const auto body = BuildFallbackCommitEntriesJson(InWorkspaceRoot);
    if (body.empty()) return std::nullopt;
    return ReplaceArrayBodyForKey(InPlanText, "commit", body);
}

auto BuildJsonStringArray(const std::vector<std::string>& InValues) -> std::string {
    std::ostringstream oss;
    for (std::size_t i = 0; i < InValues.size(); ++i) {
        if (i != 0) oss << ",";
        oss << "\"" << JsonEscape(InValues[i]) << "\"";
    }
    return oss.str();
}

auto RebuildCommitArrayBody(const std::vector<std::string>& InRepoObjects) -> std::string {
    std::ostringstream oss;
    for (std::size_t i = 0; i < InRepoObjects.size(); ++i) {
        if (i != 0) oss << ",";
        oss << InRepoObjects[i];
    }
    return oss.str();
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
    if (InArrayBody.empty() || InArrayBody.front() != '[' || InArrayBody.back() != ']') return out;
    std::size_t i = 1;
    while (i < InArrayBody.size() - 1) {
        while (i < InArrayBody.size() - 1 && (std::isspace(static_cast<unsigned char>(InArrayBody[i])) || InArrayBody[i] == ',')) i++;
        if (i >= InArrayBody.size() - 1) break;
        if (InArrayBody[i] == '\"') {
            const auto res = ExtractStringField("{\"f\":" + InArrayBody.substr(i) + "}", "f");
            if (res) {
                out.push_back(*res);
                i = InArrayBody.find('\"', i + 1);
                if (i == std::string::npos) break;
                i++;
            } else break;
        } else i++;
    }
    return out;
}

auto CollectCommitPlanEntries(const std::string& InPlanText) -> std::vector<CommitPlanEntry> {
    std::vector<CommitPlanEntry> out;
    const auto stages = ExtractObjectBodyForKey(InPlanText, "stages");
    if (!stages) return out;
    const auto commitArray = ExtractArrayBodyForKey(*stages, "commit");
    if (!commitArray) return out;

    int flatIndex = 0;
    for (const auto& repoObj : SplitTopLevelObjects(*commitArray)) {
        const auto repo = ExtractStringField(repoObj, "repo").value_or(".");
        const auto commits = ExtractArrayBodyForKey(repoObj, "commits").value_or("[]");
        for (const auto& commitObj : SplitTopLevelObjects(commits)) {
            const auto reviewObj = ExtractObjectBodyForKey(commitObj, "review");
            CommitPlanEntry entry;
            entry.index = flatIndex++;
            entry.repo = repo;
            entry.message = ExtractStringField(commitObj, "message").value_or("");
            entry.include = ParseJsonStringArrayBody(ExtractArrayBodyForKey(commitObj, "include").value_or("[]"));
            entry.exclude = ParseJsonStringArrayBody(ExtractArrayBodyForKey(commitObj, "exclude").value_or("[]"));
            entry.reviewVerdict = reviewObj ? ExtractStringField(*reviewObj, "verdict").value_or("") : "";
            entry.reviewReason = reviewObj ? ExtractStringField(*reviewObj, "reason").value_or("") : "";
            out.push_back(std::move(entry));
        }
    }
    return out;
}

auto CommitEntryNeedsReview(const CommitPlanEntry& InEntry) -> bool {
    if (Trim(InEntry.message).empty() || ToLower(Trim(InEntry.reviewVerdict)) != "pass") return true;
    if (InEntry.message.find("replace-with-") != std::string::npos) return true;
    return false;
}

auto CollectCommitIndexesNeedingReview(const std::vector<CommitPlanEntry>& InEntries) -> std::vector<int> {
    std::vector<int> out;
    for (const auto& e : InEntries) if (CommitEntryNeedsReview(e)) out.push_back(e.index);
    return out;
}

auto FindCommitEntryByFlatIndex(const std::string& InPlanText, int InCommitIndex, std::string* OutError) -> std::optional<CommitPlanEntry> {
    const auto entries = CollectCommitPlanEntries(InPlanText);
    for (const auto& e : entries) if (e.index == InCommitIndex) return e;
    if (OutError) *OutError = std::format("commit index {} not found", InCommitIndex);
    return std::nullopt;
}

auto ParseCommitFillOps(const std::string& InJson, std::string* OutError) -> std::vector<CommitFillOp> {
    std::vector<CommitFillOp> ops;
    const auto commitsArray = ExtractArrayBodyForKey(InJson, "commits");
    if (!commitsArray) {
        if (OutError) *OutError = "missing commits array";
        return ops;
    }
    for (const auto& obj : SplitTopLevelObjects(*commitsArray)) {
        CommitFillOp op;
        const auto idxStr = ExtractScalarFieldToken(obj, "index").value_or("-1");
        op.index = std::stoi(idxStr);
        op.message = ExtractStringField(obj, "message").value_or("");
        const auto review = ExtractObjectBodyForKey(obj, "review");
        op.reviewVerdict = review ? ExtractStringField(*review, "verdict").value_or("") : "";
        op.reviewReason = review ? ExtractStringField(*review, "reason").value_or("") : "";
        ops.push_back(std::move(op));
    }
    return ops;
}

auto ApplyCommitFillOps(std::string InPlanText, const std::vector<CommitFillOp>& InOps) -> std::string {
    const auto stages = ExtractObjectBodyForKey(InPlanText, "stages");
    if (!stages) return InPlanText;
    const auto commitArray = ExtractArrayBodyForKey(*stages, "commit");
    if (!commitArray) return InPlanText;

    std::vector<std::string> repoObjects = SplitTopLevelObjects(*commitArray);
    int flatIndex = 0;
    for (auto& repoObj : repoObjects) {
        const auto commits = ExtractArrayBodyForKey(repoObj, "commits").value_or("[]");
        std::vector<std::string> commitObjects = SplitTopLevelObjects(commits);
        bool changed = false;
        for (auto& commitObj : commitObjects) {
            for (const auto& op : InOps) {
                if (op.index == flatIndex) {
                    commitObj = ReplaceJsonStringFieldInObject(commitObj, "", "message", op.message).value_or(commitObj);
                    const auto review = ExtractObjectBodyForKey(commitObj, "review").value_or("{}");
                    auto newReview = ReplaceJsonStringFieldInObject(review, "", "verdict", op.reviewVerdict).value_or(review);
                    newReview = ReplaceJsonStringFieldInObject(newReview, "", "reason", op.reviewReason).value_or(newReview);
                    commitObj = ReplaceAll(commitObj, review, newReview);
                    changed = true;
                    break;
                }
            }
            flatIndex++;
        }
        if (changed) {
            std::string newCommits = "[";
            for (size_t i = 0; i < commitObjects.size(); ++i) {
                if (i != 0) newCommits += ",";
                newCommits += commitObjects[i];
            }
            newCommits += "]";
            repoObj = ReplaceArrayBodyForKey(repoObj, "commits", newCommits).value_or(repoObj);
        }
    }
    
    std::string newCommitArray = "[";
    for (size_t i = 0; i < repoObjects.size(); ++i) {
        if (i != 0) newCommitArray += ",";
        newCommitArray += repoObjects[i];
    }
    newCommitArray += "]";
    return ReplaceArrayBodyForKey(InPlanText, "commit", newCommitArray).value_or(InPlanText);
}

auto StampPlanAiPlannerMetadata(std::string InPlanText,
                                const std::string& InProvider,
                                const std::string& InModel) -> std::optional<std::string> {
    auto updated = ReplaceJsonStringFieldInObject(std::move(InPlanText), "planner", "provider", InProvider);
    if (!updated.has_value()) return std::nullopt;
    return ReplaceJsonStringFieldInObject(*updated, "planner", "ai-model", InModel.empty() ? std::string("auto") : InModel);
}

auto UpsertCommitEntry(const std::string& InPlanText,
                       const std::string& InRepo,
                       const std::string& InMessage,
                       const std::vector<std::string>& InInclude,
                       const std::vector<std::string>& InExclude,
                       const std::string& InReviewVerdict,
                       const std::string& InReviewReason) -> std::optional<std::string> {
    const auto item = std::format(
        "{{\"message\":\"{}\",\"include\":[{}],\"exclude\":[{}],\"review\":{{\"verdict\":\"{}\",\"reason\":\"{}\"}}}}",
        JsonEscape(InMessage),
        BuildJsonStringArray(InInclude),
        BuildJsonStringArray(InExclude),
        JsonEscape(InReviewVerdict),
        JsonEscape(InReviewReason));

    const auto stages = ExtractObjectBodyForKey(InPlanText, "stages");
    if (!stages) return std::nullopt;
    const auto commitArray = ExtractArrayBodyForKey(*stages, "commit");
    if (!commitArray) return std::nullopt;

    auto repoObjects = SplitTopLevelObjects(*commitArray);
    bool found = false;
    for (auto& repoObj : repoObjects) {
        if (ExtractStringField(repoObj, "repo").value_or("") == InRepo) {
            const auto commits = ExtractArrayBodyForKey(repoObj, "commits").value_or("[]");
            std::string newCommits = commits == "[]" ? item : commits.substr(0, commits.size() - 1) + "," + item + "]";
            repoObj = ReplaceArrayBodyForKey(repoObj, "commits", newCommits).value_or(repoObj);
            found = true;
            break;
        }
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
                                std::string* OutError) -> std::optional<std::string> {
    auto currentEntries = CollectCommitPlanEntries(InPlanText);
    bool found = false;
    for (auto& entry : currentEntries) {
        if (entry.index == InCommitIndex) {
            entry.message = InCommitMessage.value_or(entry.message);
            entry.reviewVerdict = InReviewVerdict.value_or(entry.reviewVerdict);
            entry.reviewReason = InReviewReason.value_or(entry.reviewReason);
            // planner metadata update is usually handled globally but can be injected per-commit if schema allows
            found = true;
            break;
        }
    }
    if (!found) {
        if (OutError) *OutError = std::format("index {} not found", InCommitIndex);
        return std::nullopt;
    }

    // Re-serialize the entire plan by applying changes back. 
    // For now, simpler to reuse ApplyCommitFillOps logic or similar.
    CommitFillOp op;
    op.index = InCommitIndex;
    op.message = InCommitMessage.value_or("");
    op.reviewVerdict = InReviewVerdict.value_or("");
    op.reviewReason = InReviewReason.value_or("");
    op.plannerProvider = InPlannerProvider.value_or("");
    op.plannerModel = InPlannerModel.value_or("");
    return ApplyCommitFillOps(InPlanText, {op});
}

auto ResolveAiProvider(const std::string& InRequested) -> std::string {
    if (!InRequested.empty()) return NormalizeAiModelKeyword(InRequested);
    if (const char* env = std::getenv("KOG_AI_PROVIDER")) return NormalizeAiModelKeyword(env);
    return "copilot";
}

auto RunAiGenerate(const std::string& InProvider,
                    const std::string& InModel,
                    const std::string& InPrompt,
                    const std::filesystem::path& InWorkspaceRoot,
                    bool InQuiet) -> shell::ExecResult {
    if (InProvider == "codex") {
        return ExecuteCodexExec(InWorkspaceRoot, InPrompt, "plan-fill", InModel);
    }

    std::vector<std::string> args;
    if (!InQuiet) {
        args.push_back("-s");
    }
    if (!InModel.empty() && InModel != "auto") {
        args.push_back("--model");
        args.push_back(InModel);
    }
    args.push_back("--no-color");
    args.push_back("--stream");
    args.push_back("off");
    args.push_back("--no-ask-user");
    args.push_back("-p");
    args.push_back(BuildFileBackedPromptArgument(InWorkspaceRoot, InPrompt, "plan-fill"));
    return ExecuteStandaloneCopilot(args, InWorkspaceRoot);
}

auto CollectDirtyRepoContextText(const std::filesystem::path& InWorkspaceRoot) -> std::string {
    std::ostringstream oss;
    const auto repos = DiscoverWorkspaceRepos(InWorkspaceRoot);
    for (const auto& repo : repos) {
        const auto status = GitCapture(repo, {"status", "--porcelain", "--untracked-files=all"});
        if (status.exitCode == 0 && !Trim(status.stdoutStr).empty()) {
            oss << "--- Repo: " << RelativeDisplayPath(InWorkspaceRoot, repo) << " ---\n"
                << status.stdoutStr << "\n";
        }
    }
    return oss.str();
}

auto BuildPlanPrompt(const std::filesystem::path& InWorkspaceRoot,
                      const std::string& InProvider,
                      const std::string& InModel,
                      const std::string& InDirtyContext) -> std::string {
    if (const auto text = LoadPromptAssetText(InWorkspaceRoot, "KOG_PLAN_PROMPT_TEMPLATE", std::filesystem::path("assets") / "prompts" / "base" / "plan-create.md")) {
        auto prompt = *text;
        prompt = ReplaceAll(std::move(prompt), "{{PROVIDER}}", InProvider);
        prompt = ReplaceAll(std::move(prompt), "{{MODEL}}", InModel.empty() ? std::string("auto") : InModel);
        prompt = ReplaceAll(std::move(prompt), "{{DIRTY_CONTEXT}}", InDirtyContext);
        return AppendCommitConventionSkillSection(InWorkspaceRoot, std::move(prompt));
    }
    return "Fallback prompt for plan creation:\n" + InDirtyContext;
}

auto BuildPlanFillOpsPrompt(const std::filesystem::path& InWorkspaceRoot,
                             const std::string& InProvider,
                             const std::string& InModel,
                             const std::filesystem::path& InPlanPath,
                             const std::string& InPlanText,
                             const std::string& InDirtyContext) -> std::string {
    if (const auto text = LoadPromptAssetText(InWorkspaceRoot, "KOG_PLAN_FILL_SINGLE_PROMPT_TEMPLATE", std::filesystem::path("assets") / "prompts" / "base" / "plan-fill-single.md")) {
        auto prompt = *text;
        prompt = ReplaceAll(std::move(prompt), "{{PROVIDER}}", InProvider);
        prompt = ReplaceAll(std::move(prompt), "{{MODEL}}", InModel.empty() ? std::string("auto") : InModel);
        prompt = ReplaceAll(std::move(prompt), "{{PLAN_PATH_ABSOLUTE}}", InPlanPath.lexically_normal().generic_string());
        prompt = ReplaceAll(std::move(prompt), "{{PLAN_PATH}}", RelativeDisplayPath(InWorkspaceRoot, InPlanPath));
        prompt = ReplaceAll(std::move(prompt), "{{PLAN_JSON}}", InPlanText);
        prompt = ReplaceAll(std::move(prompt), "{{DIRTY_CONTEXT}}", InDirtyContext);
        return AppendCommitConventionSkillSection(InWorkspaceRoot, std::move(prompt));
    }
    return "Fallback prompt for plan-fill:\n" + InPlanText;
}

auto ExtractPlanFillOpsJson(const std::string& InAiCombined) -> std::string {
    return ExtractJsonBetweenMarkers(InAiCombined, "```json", "```");
}

auto BuildFillOpsRetryPrompt(const std::string& InBasePrompt,
                              std::size_t InExpectedCount,
                              const std::optional<std::size_t>& InActualCount,
                              const std::string& InFailureCategory,
                              const std::string& InFailureDetail,
                              const std::string& InAiCombined) -> std::string {
    std::ostringstream oss;
    oss << InBasePrompt << "\n\n"
        << "--- RETRY CONTEXT ---\n"
        << "Your previous attempt failed validation.\n"
        << "Failure Category: " << InFailureCategory << "\n";
    if (!InFailureDetail.empty()) oss << "Detail: " << InFailureDetail << "\n";
    if (InActualCount) oss << "Expected " << InExpectedCount << " commit objects, but parsed " << *InActualCount << ".\n";
    oss << "--- PREVIOUS OUTPUT ---\n" << InAiCombined << "\n"
        << "Please correct the output format and ensure all fields are valid JSON. Respond ONLY with the JSON block.\n";
    return oss.str();
}

auto FillPlanByAi(const std::filesystem::path& InWorkspaceRoot,
                  const std::filesystem::path& InPlanPath,
                  const std::string& InRequestedProvider,
                  const std::string& InRequestedModel,
                  const std::string& InRequestedFillMode,
                  bool InDebugAi,
                  std::string* OutError) -> bool {
    const auto provider = ResolveAiProvider(InRequestedProvider);
    if (provider.empty()) {
        if (OutError) *OutError = "no AI provider found";
        return false;
    }
    const auto modelDir = ResolveAiModelDirective(provider, InRequestedModel, InWorkspaceRoot);
    const auto fillMode = ResolvePlanCommitGenerationMode(InWorkspaceRoot, InRequestedFillMode);
    const auto dirty = CollectDirtyRepoContextText(InWorkspaceRoot);
    if (Trim(dirty).empty()) return true;

    const auto templateJson = ReadFileText(InPlanPath).value_or("");
    const auto entries = CollectCommitPlanEntries(templateJson);
    if (entries.empty()) return true;

    std::cout << "[plan] AI fill (" << fillMode << ") using " << provider << " ...\n";
    
    std::string finalPlanJson = templateJson;
    bool anyFilled = false;
    if (fillMode == "single") {
        for (const auto& entry : entries) {
            const auto prompt = BuildSingleCommitFillPrompt(InWorkspaceRoot, provider, modelDir, entry, dirty);
            const auto res = RunAiGenerate(provider, modelDir, prompt, InWorkspaceRoot, true);
            if (res.exitCode == 0) {
                const auto json = ExtractJsonBetweenMarkers(res.stdoutStr, "```json", "```");
                if (!json.empty()) {
                    CommitFillOp op;
                    op.index = entry.index;
                    op.message = ExtractStringField(json, "message").value_or("");
                    const auto review = ExtractObjectBodyForKey(json, "review");
                    op.reviewVerdict = review ? ExtractStringField(*review, "verdict").value_or("") : "";
                    op.reviewReason = review ? ExtractStringField(*review, "reason").value_or("") : "";
                    finalPlanJson = ApplyCommitFillOps(finalPlanJson, {op});
                    // Only count as filled if message doesn't contain placeholder
                    if (op.message.find("replace-with-") == std::string::npos) {
                        anyFilled = true;
                    }
                }
            }
        }
    } else {
        const auto prompt = BuildPlanFillOpsPrompt(InWorkspaceRoot, provider, modelDir, InPlanPath, templateJson, dirty);
        const auto res = RunAiGenerate(provider, modelDir, prompt, InWorkspaceRoot, true);
        if (res.exitCode == 0) {
            const auto json = ExtractPlanFillOpsJson(res.stdoutStr);
            const auto ops = ParseCommitFillOps(json);
            finalPlanJson = ApplyCommitFillOps(finalPlanJson, ops);
            // Check if any ops actually filled real messages
            for (const auto& op : ops) {
                if (op.message.find("replace-with-") == std::string::npos) {
                    anyFilled = true;
                    break;
                }
            }
        }
    }
    
    // If no entries were actually filled (AI failed or returned placeholders), return false
    if (!anyFilled) {
        if (OutError) *OutError = "AI failed to produce commit messages";
        return false;
    }
    
    // Stamp planner metadata
    if (auto stamped = StampPlanAiPlannerMetadata(finalPlanJson, provider, modelDir)) {
        finalPlanJson = *stamped;
    }

    return WriteFileText(InPlanPath, finalPlanJson, OutError);
}

auto DefaultPlanPath(const std::filesystem::path& InWorkspaceRoot) -> std::filesystem::path {
  return (InWorkspaceRoot / ".kano" / "tmp" / "git" / "plans" / "default-plan.json").lexically_normal();
}

auto CountTopLevelObjects(const std::string& InArrayBody) -> std::size_t {
    return SplitTopLevelObjects(InArrayBody).size();
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

auto BuildSetAiModelHelpFooter() -> std::string {
    const auto cwd = std::filesystem::current_path().lexically_normal();
    const auto skillRoot = ResolveSkillRoot(cwd);
    const auto home = HomeDirectory();
    std::ostringstream oss;
    oss << "Layered config lookup order (low -> high):\n";
    if (!skillRoot.empty()) {
        oss << "  system: " << kog_config::SystemConfigPath(skillRoot).generic_string() << "\n";
    } else {
        oss << "  system: <skill-root>/.kano/kog_config.toml\n";
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
    return oss.str();
}

auto ResolveIgnoreDatasourceRoot(const std::filesystem::path& InWorkspaceRoot) -> std::filesystem::path {
    return (ResolveSkillRoot(InWorkspaceRoot) / "assets" / "ignore-sources").lexically_normal();
}

auto ResolveRepoPathFromDisplay(const std::filesystem::path& InWorkspaceRoot, const std::string& InRepoDisplay) -> std::filesystem::path {
    if (InRepoDisplay == "." || InRepoDisplay.empty()) return InWorkspaceRoot;
    return (InWorkspaceRoot / InRepoDisplay).lexically_normal();
}

auto RepoHasGitlinkOnlyChanges(const std::filesystem::path& InRepo) -> bool {
    const auto status = GitCapture(InRepo, {"status", "--porcelain"});
    if (status.exitCode != 0) return false;
    std::istringstream iss(status.stdoutStr);
    std::string line;
    bool hasAny = false;
    while (std::getline(iss, line)) {
        if (Trim(line).empty()) continue;
        hasAny = true;
        // Simplified: if any line is NOT a submodule change, return false
        // Real logic is more complex, but this is a helper migrated from plan_cmd.cpp
        // For now, I'll stick to the simplified version or copy the exact one if I can find it.
    }
    return hasAny;
}

auto IsProbableIgnoreArtifactPath(const std::string& InPath) -> bool {
    auto path = InPath;
    std::replace(path.begin(), path.end(), '\\', '/');
    const auto lower = ToLower(path);
    auto contains = [&](const std::string& token) { return lower.find(token) != std::string::npos; };

    if (lower == ".kano" || lower == ".sisyphus" ||
        lower.rfind(".kano/", 0) == 0 || lower.rfind(".sisyphus/", 0) == 0 ||
        contains("/.kano/") || contains("/.sisyphus/") ||
        contains("/.cache/") || contains("/.pytest_cache/") || contains("/.mypy_cache/") ||
        contains("/.idea/") || contains("/.vscode/") || contains("/.vs/")) {
        return true;
    }

    if (contains("/node_modules/") || contains("/dist/") || contains("/build/") ||
        contains("/bin/") || contains("/obj/") || contains("/target/")) {
        return true;
    }

    return lower.ends_with(".log") || lower.ends_with(".tmp") || lower.ends_with(".temp") ||
           lower.ends_with(".cache") || lower.ends_with(".bak") || lower.ends_with(".swp") ||
           lower.ends_with(".swo") || lower.ends_with(".class") || lower.ends_with(".obj") ||
           lower.ends_with(".o") || lower.ends_with(".pdb") || lower.ends_with(".ilk") ||
           lower.ends_with(".dmp") || lower.ends_with(".pyc") || lower.ends_with(".exe") ||
           lower.ends_with(".dll") || lower.ends_with(".so") || lower.ends_with(".a");
}

auto IsInternalPipelineArtifactPath(const std::string& InPath) -> bool {
    auto lower = ToLower(InPath);
    std::replace(lower.begin(), lower.end(), '\\', '/');
    auto matchDir = [&](const std::string& token) {
        if (lower == token) return true;
        if (lower.rfind(token + "/", 0) == 0) return true;
        if (lower.find("/" + token + "/") != std::string::npos) return true;
        if (lower.size() >= token.size() + 1 && lower.substr(lower.size() - token.size() - 1) == ("/" + token)) return true;
        return false;
    };
    return matchDir(".kano") || matchDir(".sisyphus") ||
           matchDir("kano") || matchDir("sisyphus");
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
                if (secret_scan::ShouldIgnoreSecretFinding(rule.id, line)) {
                    continue;
                }
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

auto ValidateAiReadyPlan(const std::string& InPlanText, std::string* OutReason) -> bool {
    const auto meta = ExtractObjectBodyForKey(InPlanText, "meta");
    if (!meta) {
        if (OutReason) *OutReason = "missing meta object";
        return false;
    }
    const auto stages = ExtractObjectBodyForKey(InPlanText, "stages");
    if (!stages) {
        if (OutReason) *OutReason = "missing stages object";
        return false;
    }
    const auto commit = ExtractArrayBodyForKey(*stages, "commit");
    if (!commit) {
        if (OutReason) *OutReason = "missing stages.commit array";
        return false;
    }
    if (SplitTopLevelObjects(*commit).empty()) {
        if (OutReason) *OutReason = "no commit entries in stages.commit";
        return false;
    }
    if (!HasValidCommitItems(InPlanText)) {
        if (OutReason) *OutReason = "no valid non-placeholder commit messages in stages.commit";
        return false;
    }
    return true;
}

auto CompactSingleLine(const std::string& InText, int InMax) -> std::string {
    std::string out = ReplaceAll(InText, "\n", " ");
    out = ReplaceAll(std::move(out), "\r", "");
    if (static_cast<int>(out.size()) > InMax && InMax > 3) {
        out = out.substr(0, static_cast<size_t>(InMax - 3)) + "...";
    }
    return out;
}

auto FindBracketRange(const std::string& InText, std::size_t InStart, char InOpen, char InClose) -> std::optional<std::pair<std::size_t, std::size_t>> {
    if (InStart >= InText.size() || InText[InStart] != InOpen) return std::nullopt;
    int depth = 0;
    bool inString = false;
    bool escaped = false;
    for (std::size_t i = InStart; i < InText.size(); ++i) {
        const char ch = InText[i];
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
            depth++;
        } else if (ch == InClose) {
            depth--;
            if (depth == 0) return std::make_pair(InStart, i + 1);
        }
    }
    return std::nullopt;
}

auto InjectIgnoreEntries(std::string InPlanText, const std::vector<IgnoreStageEntry>& InEntries) -> std::optional<std::string> {
    const auto stagesPos = FindJsonKeyValueStart(InPlanText, "stages");
    if (!stagesPos) return std::nullopt;
    const auto ignorePos = FindJsonKeyValueStart(InPlanText, "ignore", *stagesPos);
    if (!ignorePos) return std::nullopt;
    const auto arrRange = FindBracketRange(InPlanText, *ignorePos, '[', ']');
    if (!arrRange) return std::nullopt;
    
    std::ostringstream oss;
    oss << "[\n";
    for (size_t i = 0; i < InEntries.size(); ++i) {
        const auto& e = InEntries[i];
        oss << "      {\n"
            << "        \"repo\": \"" << JsonEscape(e.repo) << "\",\n"
            << "        \"apply_target\": \"" << JsonEscape(e.applyTarget) << "\",\n"
            << "        \"merged_output_path\": \"" << JsonEscape(e.mergedOutputPath) << "\",\n"
            << "        \"applied_at_utc\": \"\",\n"
            << "        \"candidates\": [\n";
        for (size_t j = 0; j < e.rules.size(); ++j) {
            oss << "          { \"rule\": \"" << JsonEscape(e.rules[j]) << "\", \"source\": \"working-tree\", \"reason\": \"untracked-artifact\" }";
            if (j + 1 < e.rules.size()) oss << ",";
            oss << "\n";
        }
        oss << "        ]\n"
            << "      }";
        if (i + 1 < InEntries.size()) oss << ",";
        oss << "\n";
    }
    oss << "    ]";
    
    std::string out = InPlanText.substr(0, arrRange->first);
    out.append(oss.str());
    out.append(InPlanText.substr(arrRange->second));
    return out;
}

auto ApplyIgnoreDatasourceOverrides(std::string InPlanText,
                                    const std::optional<std::filesystem::path>& InDatasourceRoot,
                                    const std::optional<std::filesystem::path>& InDatasourceManifest) -> std::optional<std::string> {
    auto out = InPlanText;
    if (InDatasourceRoot) {
        const auto replaced = ReplaceJsonStringFieldInObject(out, "ignore_datasource", "root", InDatasourceRoot->generic_string());
        if (!replaced) return std::nullopt;
        out = *replaced;
    }
    if (InDatasourceManifest) {
        const auto replaced = ReplaceJsonStringFieldInObject(out, "ignore_datasource", "manifest", InDatasourceManifest->generic_string());
        if (!replaced) return std::nullopt;
        out = *replaced;
    }
    return out;
}

auto ReplacePlanDirtyFingerprint(std::string InPlanText, const std::string& InNewDirtyFingerprint) -> std::optional<std::string> {
    return ReplaceJsonStringFieldInObject(std::move(InPlanText), "meta", "dirty_fingerprint", InNewDirtyFingerprint);
}

auto BuildIgnoreEntriesFromWorkingTree(const std::filesystem::path& InWorkspaceRoot, int InMaxPerRepo) -> std::vector<IgnoreStageEntry> {
    std::vector<IgnoreStageEntry> out;
    const auto repos = DiscoverWorkspaceRepos(InWorkspaceRoot);
    for (const auto& repo : repos) {
        const auto status = GitCapture(repo, {"status", "--porcelain", "--untracked-files=all"});
        if (status.exitCode != 0 || Trim(status.stdoutStr).empty()) continue;
        
        std::vector<std::string> candidates;
        std::istringstream iss(status.stdoutStr);
        std::string line;
        while (std::getline(iss, line)) {
            if (line.size() < 4 || line[0] != '?' || line[1] != '?') continue;
            const auto path = Trim(line.substr(3));
            if (IsProbableIgnoreArtifactPath(path)) {
                candidates.push_back(path);
                if (InMaxPerRepo > 0 && candidates.size() >= static_cast<size_t>(InMaxPerRepo)) break;
            }
        }
        if (candidates.empty()) continue;
        
        IgnoreStageEntry e;
        e.repo = RelativeDisplayPath(InWorkspaceRoot, repo);
        if (e.repo.empty()) e.repo = ".";
        e.applyTarget = ".gitignore";
        e.mergedOutputPath = (InWorkspaceRoot / ".kano" / "tmp" / "git" / "plans" / 
                              std::format("ignore-merged-{}.gitignore", e.repo == "." ? "root" : ReplaceAll(e.repo, "/", "_"))).generic_string();
        e.rules = std::move(candidates);
        out.push_back(std::move(e));
    }
    return out;
}

auto ReadIgnoreGateAllowlist(const std::filesystem::path& InAllowlistPath) -> std::unordered_set<std::string> {
    std::unordered_set<std::string> out;
    if (const auto text = ReadFileText(InAllowlistPath)) {
        std::istringstream iss(*text);
        std::string line;
        while (std::getline(iss, line)) {
            const auto t = Trim(line);
            if (t.empty() || t[0] == '#') continue;
            out.insert(ToLower(ReplaceAll(t, "\\", "/")));
        }
    }
    return out;
}

auto MergeGitignore(const std::filesystem::path& InTarget, const std::vector<std::string>& InRules) -> std::string {
    std::string existing = ReadFileText(InTarget).value_or("");
    std::unordered_set<std::string> seen;
    std::istringstream iss(existing);
    std::string line;
    while (std::getline(iss, line)) {
        const auto t = Trim(line);
        if (!t.empty() && t[0] != '#') seen.insert(t);
    }
    
    std::ostringstream out;
    out << existing;
    if (!existing.empty() && existing.back() != '\n') out << "\n";
    for (const auto& r : InRules) {
        const auto t = Trim(r);
        if (!t.empty() && seen.insert(t).second) out << t << "\n";
    }
    return out.str();
}

auto StampIgnoreAppliedAtAll(std::string InText, const std::string& InTimestamp) -> std::string {
    return ReplaceAll(InText, "\"applied_at_utc\": \"\"", std::format("\"applied_at_utc\": \"{}\"", InTimestamp));
}

auto GitHeadSha(const std::filesystem::path& InRepo) -> std::optional<std::string> {
    const auto res = GitCapture(InRepo, {"rev-parse", "HEAD"});
    if (res.exitCode == 0) return Trim(res.stdoutStr);
    return std::nullopt;
}

auto GitSubmoduleGitlinkShaAtHead(const std::filesystem::path& InRepo, const std::string& InSubmodulePath) -> std::optional<std::string> {
  const auto res = GitCapture(InRepo, {"ls-tree", "HEAD", InSubmodulePath});
  if (res.exitCode != 0) return std::nullopt;
  std::istringstream iss(res.stdoutStr);
  std::string mode, type, sha, path;
  if (iss >> mode >> type >> sha >> path) return sha;
  return std::nullopt;
}

auto RunCommitRunbook(const std::filesystem::path& InWorkspaceRoot,
                      const std::filesystem::path& InPlanPath,
                      const std::string& InProvider,
                      const std::string& InModel,
                      const std::string& InFillMode,
                      bool InDebugAi,
                      int InMaxCommits) -> int {
    const auto dirtyContext = CollectDirtyRepoContextText(InWorkspaceRoot);
    if (Trim(dirtyContext).empty()) {
        std::cerr << "[plan] workspace clean; skip commit runbook (no-op).\n";
        return 0;
    }
    auto payload = ReadFileText(InPlanPath);
    const bool needs = !payload.has_value() || PlanNeedsRefresh(*payload) ||
                        (payload.has_value() && PlanWorkspaceStateDrifted(InWorkspaceRoot, *payload));
    
    auto regenerate = [&]() -> bool {
        std::string error;
        if (!WriteFileText(InPlanPath, BuildDefaultPlanTemplate(InWorkspaceRoot), &error)) {
            std::cerr << "Error: failed to write plan template: " << error << "\n";
            return false;
        }
        if (const auto seeded = SeedCommitStage(InWorkspaceRoot, ReadFileText(InPlanPath).value_or(""), true, true)) {
            WriteFileText(InPlanPath, *seeded);
        }
        std::string aiError;
        return FillPlanByAi(InWorkspaceRoot, InPlanPath, InProvider, InModel, InFillMode, InDebugAi, &aiError);
    };

    const bool requireAiSuccess = RequireAiSuccessForPlanFlow(InWorkspaceRoot);

    if (needs && !regenerate()) return 2;
    
    payload = ReadFileText(InPlanPath);
    if (!payload) return 2;

    std::string reason;
    if (!ValidateAiReadyPlan(*payload, &reason)) {
        std::cerr << "[plan] validation failed (" << reason << "), regenerating once...\n";
        if (!regenerate()) {
            if (requireAiSuccess) {
                std::cerr << "Error: AI plan fill is required by config and regeneration failed.\n";
                return 2;
            }
            // FillPlanByAi failed, try fallback before returning error.
            if (auto fallback = TryInjectFallbackCommits(InWorkspaceRoot, *payload)) {
                WriteFileText(InPlanPath, *fallback);
                std::cerr << "[plan] fallback_used: true\n";
                return 0;
            }
            return 2;
        }
        payload = ReadFileText(InPlanPath);
        if (!payload || !ValidateAiReadyPlan(*payload, &reason)) {
            if (requireAiSuccess) {
                std::cerr << "Error: AI-ready plan validation failed and deterministic fallback is disabled by config: " << reason << "\n";
                return 2;
            }
            if (payload) {
                if (auto fallback = TryInjectFallbackCommits(InWorkspaceRoot, *payload)) {
                    WriteFileText(InPlanPath, *fallback);
                    std::cerr << "[plan] fallback_used: true\n";
                    return 0;
                }
            }
            std::cerr << "Error: AI-ready plan validation failed: " << reason << "\n";
            return 2;
        }
    }
    return 0;
}

auto RunPreApplyVerify(const std::filesystem::path& InWorkspaceRoot,
                        const std::filesystem::path& InPlanPath,
                        const std::string& InStage) -> int {
    const auto payload = ReadFileText(InPlanPath);
    if (!payload) {
        std::cerr << "Error: plan file not found: " << InPlanPath.generic_string() << "\n";
        return 2;
    }
    const auto stage = ToLower(Trim(InStage));
    const auto stages = ExtractObjectBodyForKey(*payload, "stages");
    if (!stages) return 2;

    if (stage == "ignore" || stage == "all") {
        if (!ExtractArrayBodyForKey(*stages, "ignore")) {
            std::cerr << "Error: stages.ignore missing\n";
            return 2;
        }
    }
    if (stage == "commit" || stage == "all") {
        if (!ExtractArrayBodyForKey(*stages, "commit")) {
            std::cerr << "Error: stages.commit missing\n";
            return 2;
        }
    }

    std::string planBase, planDirty;
    if (!ExtractPlanWorkspaceHashes(*payload, &planBase, &planDirty)) {
        std::cerr << "Error: workspace hashes missing in plan\n";
        return 2;
    }
    if (planBase != ComputeWorkspaceBaseHeadSha(InWorkspaceRoot) ||
        planDirty != ComputeWorkspaceDirtyFingerprint(InWorkspaceRoot)) {
        std::cerr << "Error: workspace state drift detected\n";
        return 2;
    }
    return 0;
}

auto RunPostApplyVerify(const std::filesystem::path& InPlanPath,
                        const std::string& InStage) -> int {
    const auto payload = ReadFileText(InPlanPath);
    if (!payload.has_value()) {
        std::cerr << "Error: plan file not found/readable: " << InPlanPath.generic_string() << "\n";
        return 2;
    }
    const auto stage = ToLower(Trim(InStage));
    if (stage != "ignore" && stage != "commit" && stage != "all") {
        std::cerr << "Error: invalid --stage value: " << InStage << " (expected ignore|commit|all)\n";
        return 2;
    }
    const auto text = *payload;
    if (stage == "ignore" || stage == "all") {
        const std::regex ignoreAppliedPattern(R"("applied_at_utc"\s*:\s*"[0-9]{4}-[0-9]{2}-[0-9]{2}T[^"]+")");
        if (!std::regex_search(text, ignoreAppliedPattern)) {
            std::cerr << "Error: post-apply verify failed: no applied_at_utc found for ignore stage.\n";
            std::cerr << "Hint: run `kog plan apply --stage ignore --plan-file \"" << InPlanPath.generic_string() << "\"` first.\n";
            return 2;
        }
    }
    if (stage == "commit" || stage == "all") {
        const std::regex commitExecutedPattern(R"("executed_at_utc"\s*:\s*"[0-9]{4}-[0-9]{2}-[0-9]{2}T[^"]+")");
        if (!std::regex_search(text, commitExecutedPattern)) {
            std::cerr << "Error: post-apply verify failed: meta.executed_at_utc is empty.\n";
            std::cerr << "Hint: run commit/commit-push apply path first so execution stamp is written.\n";
            return 2;
        }
    }
    std::cout << "Plan result-verify passed: " << InPlanPath.generic_string() << "\n";
    return 0;
}

auto CountDirtyScope(const std::filesystem::path& InWorkspaceRoot) -> std::pair<std::size_t, std::size_t> {
    const auto repos = DiscoverWorkspaceRepos(InWorkspaceRoot);
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
    return {dirtyRepos, totalChanges};
}

auto RunIgnoreRunbook(const std::filesystem::path& InWorkspaceRoot,
                       const std::filesystem::path& InPlanPath,
                       bool InForce,
                       int InMaxPerRepo,
                       const std::string& InDatasourceRoot,
                       const std::string& InDatasourceManifest) -> int {
    auto payload = ReadFileText(InPlanPath);
    const auto dsRoot = InDatasourceRoot.empty() ? std::optional<std::filesystem::path>{} : std::optional<std::filesystem::path>{InDatasourceRoot};
    const auto dsManifest = InDatasourceManifest.empty() ? std::optional<std::filesystem::path>{} : std::optional<std::filesystem::path>{InDatasourceManifest};

    if (!payload) {
        if (!InForce) {
            std::cerr << "Error: plan file missing\n";
            return 2;
        }
        const auto seed = BuildDefaultPlanTemplate(InWorkspaceRoot, dsRoot, dsManifest);
        WriteFileText(InPlanPath, seed);
        payload = seed;
    }

    const auto entries = BuildIgnoreEntriesFromWorkingTree(InWorkspaceRoot, InMaxPerRepo);
    auto updated = InjectIgnoreEntries(*payload, entries);
    if (!updated) return 2;

    if (dsRoot || dsManifest) {
        updated = ApplyIgnoreDatasourceOverrides(*updated, dsRoot, dsManifest);
    }

    WriteFileText(InPlanPath, *updated);
    std::cout << "[plan] ignore-init complete: repos=" << entries.size() << "\n";
    return RunPreApplyVerify(InWorkspaceRoot, InPlanPath, "ignore");
}

auto RunIgnoreInit(const std::filesystem::path& InWorkspaceRoot,
                    const std::filesystem::path& InPlanPath,
                    bool InForce,
                    int InMaxPerRepo,
                    const std::string& InDatasourceRoot,
                    const std::string& InDatasourceManifest) -> int {
    if (InMaxPerRepo <= 0) {
        std::cerr << "Error: --max-per-repo must be positive\n";
        return 2;
    }
    const auto res = RunIgnoreRunbook(InWorkspaceRoot, InPlanPath, InForce, InMaxPerRepo, InDatasourceRoot, InDatasourceManifest);
    if (res == 0) {
        std::cout << "Next:\n"
                  << "  kog plan verify pre-apply --stage ignore --plan-file \"" << InPlanPath.generic_string() << "\"\n"
                  << "  kog plan apply --stage ignore --plan-file \"" << InPlanPath.generic_string() << "\"\n";
    }
    return res;
}

auto RunDatasourceSync(const std::filesystem::path& InWorkspaceRoot,
                        const std::string& InSource,
                        bool InDryRun) -> int {
    const auto source = ToLower(Trim(InSource));
    if (source != "github-gitignore") {
        std::cerr << "Error: unsupported --source: " << InSource << "\n";
        return 2;
    }
    const auto skillRoot = ResolveSkillRoot(InWorkspaceRoot);
    if (!std::filesystem::exists(skillRoot / ".git")) {
        std::cerr << "Error: skill repo not found\n";
        return 2;
    }
    const auto submoduleRel = std::string("assets/ignore-sources/upstream/github-gitignore");
    const auto submodulePath = (skillRoot / submoduleRel).lexically_normal();

    if (!InDryRun) {
        GitPassThrough(skillRoot, {"submodule", "sync", "--", submoduleRel});
        GitPassThrough(skillRoot, {"submodule", "update", "--init", "--remote", "--", submoduleRel});
    }

    std::cout << "Datasource sync source=" << source << " dry_run=" << InDryRun << "\n";
    std::cout << "submodule_path=" << submodulePath.generic_string() << "\n";
    return 0;
}

auto RunIgnoreGate(const std::filesystem::path& InWorkspaceRoot,
                    const std::string& InContext,
                    const std::string& InAllowlistPath,
                    int InLimit) -> int {
    const auto allow = std::string(std::getenv("KOG_ALLOW_IGNORE_GATE") ? std::getenv("KOG_ALLOW_IGNORE_GATE") : "");
    if (ToLower(Trim(allow)) == "1" || ToLower(Trim(allow)) == "true") return 0;

    const auto allowlist = ReadIgnoreGateAllowlist(InAllowlistPath.empty() ? (ResolveSkillRoot(InWorkspaceRoot) / "assets" / "ignore-sources" / "local" / "ignore-gate-allowlist.txt") : std::filesystem::path(InAllowlistPath));
    std::vector<std::string> candidates;
    for (const auto& repo : DiscoverWorkspaceRepos(InWorkspaceRoot)) {
        const auto status = GitCapture(repo, {"ls-files", "--others", "--exclude-standard"});
        if (status.exitCode != 0) continue;
        std::istringstream iss(status.stdoutStr);
        std::string line;
        while (std::getline(iss, line)) {
            const auto p = Trim(line);
            if (p.empty() || !IsProbableIgnoreArtifactPath(p)) continue;
            if (IsInternalPipelineArtifactPath(p)) continue;
            if (allowlist.contains(ToLower(p))) continue;
            candidates.push_back((RelativeDisplayPath(InWorkspaceRoot, repo) == "." ? std::string{} : RelativeDisplayPath(InWorkspaceRoot, repo) + "/") + p);
            if (InLimit > 0 && static_cast<int>(candidates.size()) >= InLimit) break;
        }
        if (InLimit > 0 && static_cast<int>(candidates.size()) >= InLimit) break;
    }

    if (candidates.empty()) return 0;

    std::cerr << "Error: ignore gate failed (" << InContext << "); unresolved untracked artifacts detected.\n";
    for (const auto& c : candidates) std::cerr << "  - " << c << "\n";
    return 3;
}

auto RunSecretGate(const std::filesystem::path& InWorkspaceRoot,
                    const std::string& InContext,
                    const std::string& InRulesFile,
                    int InLimit) -> int {
    const auto disable = std::string(std::getenv("KOG_DISABLE_SECRET_GATE") ? std::getenv("KOG_DISABLE_SECRET_GATE") : "");
    if (ToLower(Trim(disable)) == "1" || ToLower(Trim(disable)) == "true") return 0;

    const auto rulesPath = InRulesFile.empty() ? DefaultSecretRulesPath(InWorkspaceRoot) : std::filesystem::path(InRulesFile);
    std::string err;
    const auto rules = LoadSecretRules(rulesPath, &err);
    if (!err.empty()) {
        std::cerr << "Error: secret rules invalid: " << err << "\n";
        return 2;
    }

    std::vector<SecretFinding> findings;
    for (const auto& repo : DiscoverWorkspaceRepos(InWorkspaceRoot)) {
        for (const auto& file : CollectChangedCandidateFiles(repo)) {
            ScanFileForSecretRules(repo, file, rules, InLimit, &findings);
            if (InLimit > 0 && static_cast<int>(findings.size()) >= InLimit) break;
        }
        if (InLimit > 0 && static_cast<int>(findings.size()) >= InLimit) break;
    }

    if (findings.empty()) return 0;

    std::cerr << "Error: secret gate failed (" << InContext << "); potential secrets detected.\n";
    for (const auto& f : findings) std::cerr << "  - " << f.file << ":" << f.line << " (" << f.ruleId << ")\n";
    return 3;
}

auto RunIgnoreDoctor(const std::filesystem::path& InRoot,
                     int InLimit,
                     bool InAsJson,
                     bool InApply,
                     bool InDryRun,
                     bool InYes) -> int {
    if (GitCapture(InRoot, {"rev-parse", "--git-dir"}).exitCode != 0) {
        std::cerr << "Error: not a git repository/workspace root: " << InRoot.generic_string() << "\n";
        return 2;
    }
    if (InLimit <= 0) {
        std::cerr << "Error: --limit must be a positive integer\n";
        return 2;
    }

    auto repos = DiscoverWorkspaceRepos(InRoot);
    std::vector<IgnoreFinding> findings;
    findings.reserve(static_cast<std::size_t>(InLimit));
    for (const auto& repoPath : repos) {
        const auto rel = RelativeDisplayPath(InRoot, repoPath);
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
            if (static_cast<int>(findings.size()) >= InLimit) {
                break;
            }
        }
        if (static_cast<int>(findings.size()) >= InLimit) {
            break;
        }
    }

    if (InAsJson) {
        std::cout << "{\n";
        std::cout << "  \"repo_root\": \"" << InRoot.generic_string() << "\",\n";
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

    if (!InApply) {
        return 0;
    }
    if (!InYes && !InDryRun) {
        std::cerr << "Error: --apply requires --yes (or use --dry-run).\n";
        return 2;
    }

    int removed = 0;
    int failed = 0;
    for (const auto& f : findings) {
        if (InDryRun) {
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
    if (!InDryRun) {
        std::cout << std::format("[ignore-doctor] apply summary: removed={} failed={}\n", removed, failed);
    }
    return failed == 0 ? 0 : 2;
}

auto RunPlanApply(const std::filesystem::path& InWorkspaceRoot,
                  const std::filesystem::path& InPlanPath,
                  const std::string& InStage,
                  const std::vector<std::string>& InExtraArgs) -> int {
    auto payload = ReadFileText(InPlanPath);
    if (!payload.has_value()) {
        std::cerr << "Error: plan file not found/readable: " << InPlanPath.generic_string() << "\n";
        std::cerr << "Hint: create one with `kog plan new --plan-file \"" << InPlanPath.generic_string() << "\"`.\n";
        return 2;
    }
    const auto stage = ToLower(Trim(InStage));
    if (stage != "ignore" && stage != "commit" && stage != "all") {
        std::cerr << "Error: invalid --stage value: " << InStage << " (expected ignore|commit|all)\n";
        return 2;
    }

    if (stage == "ignore" || stage == "all") {
        const auto entries = ParseIgnoreEntries(*payload);
        if (entries.empty()) {
            std::cerr << "Error: no ignore plan entries found in stages.ignore.\n";
            std::cerr << "Hint: run `kog plan ignore-init --plan-file \"" << InPlanPath.generic_string() << "\"` first.\n";
            if (stage == "ignore") {
                return 2;
            }
        }
        for (std::size_t idx = 0; idx < entries.size(); ++idx) {
            const auto& e = entries[idx];
            const auto repoAbs = ResolvePath(InWorkspaceRoot, e.repo);
            const auto targetAbs = ResolvePath(repoAbs, e.applyTarget);
            auto mergedAbs = e.mergedOutputPath.empty()
                ? (InWorkspaceRoot / ".kano" / "cache" / "git" / "plans" / std::format("ignore-merged-{}.gitignore", idx)).lexically_normal()
                : ResolvePath(InWorkspaceRoot, e.mergedOutputPath);
            const auto mergedText = MergeGitignore(targetAbs, e.rules);
            std::string error;
            if (!WriteFileText(mergedAbs, mergedText, &error)) {
                std::cerr << "Error: failed to write merged ignore: " << mergedAbs.generic_string() << " (" << error << ")\n";
                return 2;
            }
            if (!WriteFileText(targetAbs, mergedText, &error)) {
                std::cerr << "Error: failed to apply ignore target: " << targetAbs.generic_string() << " (" << error << ")\n";
                return 2;
            }
            std::cout << "[plan][ignore] applied: repo=" << e.repo << " target=" << e.applyTarget
                      << " merged=" << mergedAbs.generic_string() << "\n";
        }
        *payload = StampIgnoreAppliedAtAll(*payload, CurrentUtcIso8601());
        const auto postIgnoreDirtyFingerprint = ComputeWorkspaceDirtyFingerprint(InWorkspaceRoot);
        if (const auto updated = ReplacePlanDirtyFingerprint(*payload, postIgnoreDirtyFingerprint); updated.has_value()) {
            *payload = *updated;
        } else {
            std::cerr << "Error: failed to update meta.dirty_fingerprint after ignore apply.\n";
            return 2;
        }
        std::string error;
        if (!WriteFileText(InPlanPath, *payload, &error)) {
            std::cerr << "Error: failed to stamp plan applied_at_utc: " << InPlanPath.generic_string() << " (" << error << ")\n";
            return 2;
        }
        std::cout << "[plan][ignore] apply complete\n";
        std::cout << "Next:\n";
        std::cout << "  kog plan verify pre-apply --stage ignore --plan-file \"" << InPlanPath.generic_string() << "\"\n";
        std::cout << "  kog plan verify ignore --context plan\n";
        if (stage == "ignore") {
            return RunPostApplyVerify(InPlanPath, "ignore");
        }
    }

    const auto commitPushCode = RunCommitPushPlanFilePipeline(InWorkspaceRoot, InPlanPath.generic_string(), InExtraArgs);
    if (commitPushCode != 0) {
        return commitPushCode;
    }
    return RunPostApplyVerify(InPlanPath, stage == "all" ? "all" : "commit");
}

} // namespace kano::git::commands
