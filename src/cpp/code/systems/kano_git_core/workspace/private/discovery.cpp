#include "discovery.hpp"

#include "shell_executor.hpp"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <format>
#include <fstream>
#include <iomanip>
#include <limits>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <cstdint>
#include <unordered_set>

namespace kano::git::workspace {
namespace {

auto Normalize(const std::filesystem::path& InPath) -> std::filesystem::path {
    return InPath.lexically_normal();
}

auto PathKey(const std::filesystem::path& InPath) -> std::string {
    auto key = Normalize(InPath).generic_string();
    while (key.size() > 1 && key.back() == '/') {
        key.pop_back();
    }
    return key;
}

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

auto EscapeJson(std::string InValue) -> std::string {
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

auto UnescapeJson(const std::string& InValue) -> std::string {
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
        case 'n': out.push_back('\n'); break;
        case 'r': out.push_back('\r'); break;
        case 't': out.push_back('\t'); break;
        default: out.push_back(next); break;
        }
        i += 1;
    }
    return out;
}

auto ToUtcIsoString(const std::chrono::system_clock::time_point InTimePoint) -> std::string {
    const std::time_t raw = std::chrono::system_clock::to_time_t(InTimePoint);
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &raw);
#else
    gmtime_r(&raw, &tm);
#endif
    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
    return oss.str();
}

auto ReadFileText(const std::filesystem::path& InFile) -> std::optional<std::string> {
    std::ifstream in(InFile, std::ios::in | std::ios::binary);
    if (!in) {
        return std::nullopt;
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

auto WriteFileText(const std::filesystem::path& InFile, const std::string& InText) -> bool {
    std::filesystem::create_directories(InFile.parent_path());
    const auto temp = InFile.parent_path() / (InFile.filename().generic_string() + ".tmp");
    {
        std::ofstream out(temp, std::ios::out | std::ios::binary | std::ios::trunc);
        if (!out) {
            return false;
        }
        out << InText;
        if (!out.good()) {
            return false;
        }
    }
    std::error_code ec;
    std::filesystem::rename(temp, InFile, ec);
    if (ec) {
        std::filesystem::remove(temp, ec);
        return false;
    }
    return true;
}

auto FileMtimeEpochSeconds(const std::filesystem::path& InPath) -> long long {
    std::error_code ec;
    if (!std::filesystem::exists(InPath, ec)) {
        return 0;
    }
    const auto writeTime = std::filesystem::last_write_time(InPath, ec);
    if (ec) {
        return 0;
    }
    const auto sysNow = std::chrono::system_clock::now();
    const auto fileNow = decltype(writeTime)::clock::now();
    const auto converted = sysNow + (writeTime - fileNow);
    return std::chrono::duration_cast<std::chrono::seconds>(converted.time_since_epoch()).count();
}

auto HashFNV1a(const std::string& InValue) -> std::string {
    std::uint64_t hash = 1469598103934665603ull;
    for (const unsigned char ch : InValue) {
        hash ^= ch;
        hash *= 1099511628211ull;
    }
    return std::format("{:016x}", hash);
}

auto DefaultExcludePatterns() -> std::vector<std::string> {
    return {
        "node_modules",
        ".cache",
        "build",
        "dist",
        ".venv",
        "venv",
        "__pycache__",
    };
}

struct IgnoreRule {
    std::string pattern;
    bool include = false;
};

auto NormalizeRulePattern(std::string InPattern) -> std::string {
    auto pattern = Trim(std::move(InPattern));
    while (!pattern.empty() && pattern.front() == '/') {
        pattern.erase(pattern.begin());
    }
    while (!pattern.empty() && pattern.back() == '/') {
        pattern.pop_back();
    }
    return pattern;
}

auto LoadIgnoreRulesFromFile(const std::filesystem::path& InFile) -> std::vector<IgnoreRule> {
    std::vector<IgnoreRule> rules;
    if (!std::filesystem::exists(InFile)) {
        return rules;
    }

    std::ifstream in(InFile);
    if (!in) {
        return rules;
    }

    std::string line;
    while (std::getline(in, line)) {
        auto trimmed = Trim(line);
        if (trimmed.empty() || trimmed.starts_with("#")) {
            continue;
        }

        bool include = false;
        if (!trimmed.empty() && trimmed.front() == '!') {
            include = true;
            trimmed.erase(trimmed.begin());
            trimmed = Trim(trimmed);
        }

        const auto normalized = NormalizeRulePattern(trimmed);
        if (normalized.empty()) {
            continue;
        }

        rules.push_back(IgnoreRule{.pattern = normalized, .include = include});
    }

    return rules;
}

auto BuildIgnoreRules(const std::filesystem::path& InRoot, const std::vector<std::string>& InExcludePatterns) -> std::vector<IgnoreRule> {
    std::vector<IgnoreRule> rules;
    rules.reserve(InExcludePatterns.size() + 32);

    for (const auto& pattern : InExcludePatterns) {
        const auto normalized = NormalizeRulePattern(pattern);
        if (normalized.empty()) {
            continue;
        }
        rules.push_back(IgnoreRule{.pattern = normalized, .include = false});
    }

    for (const auto& fileRule : LoadIgnoreRulesFromFile(InRoot / ".gitignore")) {
        rules.push_back(fileRule);
    }
    for (const auto& fileRule : LoadIgnoreRulesFromFile(InRoot / ".kogignore")) {
        rules.push_back(fileRule);
    }

    return rules;
}

auto PrePrunePatterns() -> std::vector<std::string> {
    return {
        ".kano",
        "node_modules",
        ".cache",
        "build",
        "dist",
        ".venv",
        "venv",
        "__pycache__",
    };
}

auto PathContainsSegment(const std::string& InPath, const std::string& InSegment) -> bool {
    if (InSegment.empty()) {
        return false;
    }
    std::size_t start = 0;
    while (start <= InPath.size()) {
        const auto slash = InPath.find('/', start);
        const auto end = (slash == std::string::npos) ? InPath.size() : slash;
        if (end > start && InPath.substr(start, end - start) == InSegment) {
            return true;
        }
        if (slash == std::string::npos) {
            break;
        }
        start = slash + 1;
    }
    return false;
}

auto RuleMatchesPath(const std::string& InRelPath, const IgnoreRule& InRule) -> bool {
    const auto& pattern = InRule.pattern;
    if (pattern.empty()) {
        return false;
    }

    if (pattern.find('/') == std::string::npos) {
        return PathContainsSegment(InRelPath, pattern);
    }

    if (InRelPath == pattern) {
        return true;
    }
    if (InRelPath.starts_with(pattern + "/")) {
        return true;
    }
    if (InRelPath.ends_with("/" + pattern)) {
        return true;
    }
    return InRelPath.find("/" + pattern + "/") != std::string::npos;
}

auto ShouldExcludePath(const std::filesystem::path& InRoot, const std::filesystem::path& InPath, const std::vector<IgnoreRule>& InRules) -> bool {
    std::error_code ec;
    const auto rel = std::filesystem::relative(InPath, InRoot, ec);
    const auto relKey = (!ec && !rel.empty() && rel != ".") ? PathKey(rel) : PathKey(InPath);

    bool excluded = false;
    for (const auto& rule : InRules) {
        if (RuleMatchesPath(relKey, rule)) {
            excluded = !rule.include;
        }
    }
    return excluded;
}

auto ShouldPrePrune(const std::filesystem::path& InRoot, const std::filesystem::path& InPath, const std::vector<std::string>& InPrePrune, const std::vector<IgnoreRule>& InRules) -> bool {
    const auto base = InPath.filename().generic_string();
    for (const auto& name : InPrePrune) {
        if (base == name) {
            return true;
        }
    }
    return ShouldExcludePath(InRoot, InPath, InRules);
}

auto RunGitCapture(const std::filesystem::path& InRepoPath, const std::vector<std::string>& InArgs) -> shell::ExecResult {
    std::vector<std::string> args;
    args.reserve(InArgs.size() + 2);
    args.push_back("-C");
    args.push_back(PathKey(InRepoPath));
    args.insert(args.end(), InArgs.begin(), InArgs.end());
    return shell::ExecuteCommand("git", args, shell::ExecMode::Capture);
}

auto IsGitRepo(const std::filesystem::path& InRepoPath) -> bool {
    const auto result = RunGitCapture(InRepoPath, {"rev-parse", "--git-dir"});
    return result.exitCode == 0;
}

auto CurrentBranch(const std::filesystem::path& InRepoPath) -> std::string {
    auto result = RunGitCapture(InRepoPath, {"symbolic-ref", "--short", "HEAD"});
    if (result.exitCode != 0) {
        return "";
    }
    auto value = result.stdoutStr;
    while (!value.empty() && (value.back() == '\n' || value.back() == '\r')) {
        value.pop_back();
    }
    return value;
}

auto JoinLinesWithComma(const std::string& InText) -> std::string {
    std::istringstream iss(InText);
    std::string line;
    std::string out;
    while (std::getline(iss, line)) {
        if (line.empty()) {
            continue;
        }
        if (!out.empty()) {
            out += ',';
        }
        out += line;
    }
    return out;
}

auto HasChanges(const std::filesystem::path& InRepoPath) -> bool {
    const auto result = RunGitCapture(InRepoPath, {"status", "--porcelain"});
    return result.exitCode == 0 && !result.stdoutStr.empty();
}

auto ParseGitConfigPaths(const std::string& InText) -> std::vector<std::string> {
    std::vector<std::string> values;
    std::istringstream iss(InText);
    std::string line;
    while (std::getline(iss, line)) {
        if (line.empty()) {
            continue;
        }
        const auto pos = line.find(' ');
        if (pos == std::string::npos || pos + 1 >= line.size()) {
            continue;
        }
        values.push_back(line.substr(pos + 1));
    }
    return values;
}

auto CollectRegisteredSubmodulesRecursive(const std::filesystem::path& InRepoPath, std::set<std::string>& IoPaths) -> void {
    const auto gitmodulesPath = InRepoPath / ".gitmodules";
    if (!std::filesystem::exists(gitmodulesPath)) {
        return;
    }

    const auto config = RunGitCapture(InRepoPath, {"config", "--file", ".gitmodules", "--get-regexp", "path"});
    if (config.exitCode != 0) {
        return;
    }

    for (const auto& subPathRaw : ParseGitConfigPaths(config.stdoutStr)) {
        const auto fullPath = Normalize(InRepoPath / subPathRaw);
        const auto fullKey = PathKey(fullPath);
        const auto inserted = IoPaths.insert(fullKey).second;
        if (inserted && IsGitRepo(fullPath)) {
            CollectRegisteredSubmodulesRecursive(fullPath, IoPaths);
        }
    }
}

auto DiscoverGitRepos(const std::filesystem::path& InRoot, const int InMaxDepth, const std::vector<IgnoreRule>& InIgnoreRules) -> std::vector<std::filesystem::path> {
    std::vector<std::filesystem::path> repos;
    std::set<std::string> unique;

    if (IsGitRepo(InRoot)) {
        unique.insert(PathKey(InRoot));
    }

    const auto prePrune = PrePrunePatterns();
    std::error_code ec;
    std::filesystem::recursive_directory_iterator it(
        InRoot,
        std::filesystem::directory_options::skip_permission_denied,
        ec);
    std::filesystem::recursive_directory_iterator end;

    for (; it != end; ++it) {
        const auto currentPath = it->path();
        const auto depth = static_cast<int>(it.depth()) + 1;

        if (depth > InMaxDepth) {
            it.disable_recursion_pending();
            continue;
        }

        if (currentPath.filename() == ".git") {
            const auto repoPath = Normalize(currentPath.parent_path());
            unique.insert(PathKey(repoPath));
            if (it->is_directory(ec)) {
                it.disable_recursion_pending();
            }
            continue;
        }

        if (!it->is_directory(ec)) {
            continue;
        }

        if (ShouldPrePrune(InRoot, currentPath, prePrune, InIgnoreRules)) {
            it.disable_recursion_pending();
            continue;
        }

    }

    for (const auto& key : unique) {
        repos.emplace_back(key);
    }
    return repos;
}

auto CacheDirFor(const std::filesystem::path& InRoot) -> std::filesystem::path {
    if (IsGitRepo(InRoot)) {
        const auto configured = RunGitCapture(InRoot, {"config", "--path", "--get", "kano.cache.local-dir"});
        if (configured.exitCode == 0) {
            const auto value = Trim(configured.stdoutStr);
            if (!value.empty()) {
                const std::filesystem::path configuredPath(value);
                if (configuredPath.is_absolute()) {
                    return (configuredPath / "discover-repos").lexically_normal();
                }
                return (InRoot / configuredPath / "discover-repos").lexically_normal();
            }
        }
    }
    return (InRoot / ".kano" / "cache" / "git" / "discover-repos").lexically_normal();
}

auto ComputeMarker(const std::filesystem::path& InRoot, const int InMaxDepth, const std::vector<IgnoreRule>& InIgnoreRules) -> std::string {
    std::string markerInput;
    markerInput += PathKey(InRoot);
    markerInput += "|";
    markerInput += std::to_string(InMaxDepth);
    markerInput += std::format("|root:{}", FileMtimeEpochSeconds(InRoot));
    markerInput += std::format("|gitmodules:{}", FileMtimeEpochSeconds(InRoot / ".gitmodules"));

    std::vector<std::filesystem::path> dirs;
    std::error_code ec;
    std::filesystem::recursive_directory_iterator it(
        InRoot,
        std::filesystem::directory_options::skip_permission_denied,
        ec);
    std::filesystem::recursive_directory_iterator end;

    const auto prePrune = PrePrunePatterns();
    for (; it != end; ++it) {
        if (it.depth() + 1 > 2) {
            it.disable_recursion_pending();
            continue;
        }
        if (!it->is_directory(ec)) {
            continue;
        }
        const auto path = it->path();
        if (ShouldPrePrune(InRoot, path, prePrune, InIgnoreRules)) {
            it.disable_recursion_pending();
            continue;
        }
        dirs.push_back(path);
    }

    std::sort(dirs.begin(), dirs.end(), [](const auto& A, const auto& B) {
        return PathKey(A) < PathKey(B);
    });

    for (const auto& dir : dirs) {
        markerInput += std::format("|{}:{}", PathKey(dir), FileMtimeEpochSeconds(dir));
    }
    return HashFNV1a(markerInput);
}

auto ExtractStringField(const std::string& InPayload, const std::string& InField) -> std::optional<std::string> {
    const std::regex re(std::format("\\\"{}\\\":\\\"([^\\\"]*)\\\"", InField));
    std::smatch match;
    if (!std::regex_search(InPayload, match, re) || match.size() < 2) {
        return std::nullopt;
    }
    return UnescapeJson(match[1].str());
}

auto ExtractIntField(const std::string& InPayload, const std::string& InField) -> std::optional<long long> {
    const std::regex re(std::format("\\\"{}\\\":([0-9]+)", InField));
    std::smatch match;
    if (!std::regex_search(InPayload, match, re) || match.size() < 2) {
        return std::nullopt;
    }
    return std::stoll(match[1].str());
}

auto ExtractReposArrayRaw(const std::string& InPayload) -> std::optional<std::string> {
    const auto keyPos = InPayload.find("\"repos\":");
    if (keyPos == std::string::npos) {
        return std::nullopt;
    }
    const auto start = InPayload.find('[', keyPos);
    if (start == std::string::npos) {
        return std::nullopt;
    }
    int depth = 0;
    for (std::size_t idx = start; idx < InPayload.size(); ++idx) {
        const char ch = InPayload[idx];
        if (ch == '[') {
            depth += 1;
        } else if (ch == ']') {
            depth -= 1;
            if (depth == 0) {
                return InPayload.substr(start, idx - start + 1);
            }
        }
    }
    return std::nullopt;
}

auto ParseStringArrayField(const std::string& InObject, const std::string& InField) -> std::vector<std::string> {
    std::vector<std::string> values;
    const std::regex fieldRe(std::format("\\\"{}\\\":\\[(.*?)\\]", InField));
    std::smatch fieldMatch;
    if (!std::regex_search(InObject, fieldMatch, fieldRe) || fieldMatch.size() < 2) {
        return values;
    }
    const std::string content = fieldMatch[1].str();
    const std::regex itemRe("\\\"([^\\\"]*)\\\"");
    for (auto it = std::sregex_iterator(content.begin(), content.end(), itemRe); it != std::sregex_iterator(); ++it) {
        values.push_back(UnescapeJson((*it)[1].str()));
    }
    return values;
}

auto ParseReposArray(const std::string& InRawArray) -> std::vector<RepoRecord> {
    std::vector<RepoRecord> repos;
    std::size_t idx = 0;
    while (idx < InRawArray.size()) {
        const auto objStart = InRawArray.find('{', idx);
        if (objStart == std::string::npos) {
            break;
        }
        int depth = 0;
        std::size_t objEnd = objStart;
        for (; objEnd < InRawArray.size(); ++objEnd) {
            const char ch = InRawArray[objEnd];
            if (ch == '{') {
                depth += 1;
            } else if (ch == '}') {
                depth -= 1;
                if (depth == 0) {
                    break;
                }
            }
        }
        if (objEnd >= InRawArray.size()) {
            break;
        }

        const std::string obj = InRawArray.substr(objStart, objEnd - objStart + 1);
        RepoRecord record;
        const auto path = ExtractStringField(obj, "path");
        if (!path) {
            idx = objEnd + 1;
            continue;
        }
        record.path = Normalize(std::filesystem::path(*path));
        record.type = ExtractStringField(obj, "type").value_or("unregistered");
        record.currentBranch = ExtractStringField(obj, "current_branch").value_or("");
        record.remotes = ExtractStringField(obj, "remotes").value_or("");
        const auto hasChangesField = std::regex("\\\"has_changes\\\":(true|false)");
        std::smatch hasChangesMatch;
        if (std::regex_search(obj, hasChangesMatch, hasChangesField) && hasChangesMatch.size() >= 2) {
            record.hasChanges = hasChangesMatch[1].str() == "true";
        }

        for (const auto& dep : ParseStringArrayField(obj, "dependencies")) {
            record.dependencies.push_back(std::filesystem::path(dep));
        }

        repos.push_back(std::move(record));
        idx = objEnd + 1;
    }
    return repos;
}

auto CachePayloadFromRepos(const std::filesystem::path& InRoot, const std::string& InMarker, const std::vector<RepoRecord>& InRepos) -> std::string {
    const auto now = std::chrono::system_clock::now();
    const auto nowEpoch = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();
    const auto gitmodulesMtime = FileMtimeEpochSeconds(InRoot / ".gitmodules");
    return std::format(
        "{{\"version\":1,\"generated_epoch\":{},\"gitmodules_mtime\":{},\"marker\":\"{}\",\"repos\":{}}}",
        nowEpoch,
        gitmodulesMtime,
        EscapeJson(InMarker.empty() ? "none" : InMarker),
        ReposToJson(InRepos));
}

auto ParseCache(const std::string& InPayload) -> std::optional<std::vector<RepoRecord>> {
    const auto reposRaw = ExtractReposArrayRaw(InPayload);
    if (!reposRaw) {
        return std::nullopt;
    }
    return ParseReposArray(*reposRaw);
}

auto CacheFilePath(const DiscoverOptions& InOptions, const std::filesystem::path& InRootAbs) -> std::filesystem::path {
    (void)InOptions;
    return CacheDirFor(InRootAbs) / "discover-repos.json";
}

auto PruneLegacyCacheFiles(const std::filesystem::path& InCacheFile) -> void {
    std::error_code ec;
    const auto parent = InCacheFile.parent_path();
    if (!std::filesystem::exists(parent, ec)) {
        return;
    }

    std::filesystem::directory_iterator it(parent, std::filesystem::directory_options::skip_permission_denied, ec);
    std::filesystem::directory_iterator end;
    for (; it != end; it.increment(ec)) {
        if (ec) {
            ec.clear();
            continue;
        }
        const auto path = it->path();
        if (path == InCacheFile) {
            continue;
        }
        if (!it->is_regular_file(ec)) {
            ec.clear();
            continue;
        }
        if (path.extension() != ".json") {
            continue;
        }
        std::filesystem::remove(path, ec);
        ec.clear();
    }
}

auto IsPrefixPath(const std::filesystem::path& InParent, const std::filesystem::path& InChild) -> bool {
    const auto parent = Normalize(InParent);
    const auto child = Normalize(InChild);
    auto pit = parent.begin();
    auto cit = child.begin();
    for (; pit != parent.end() && cit != child.end(); ++pit, ++cit) {
        if (*pit != *cit) {
            return false;
        }
    }
    return pit == parent.end() && parent != child;
}

auto BuildRepoRecords(
    const std::filesystem::path& InRootAbs,
    const std::vector<std::filesystem::path>& InDiscovered,
    const std::set<std::string>& InRegistered,
    const std::string& InMetadataLevel) -> std::vector<RepoRecord> {
    std::vector<std::filesystem::path> sorted = InDiscovered;
    std::sort(sorted.begin(), sorted.end(), [](const auto& A, const auto& B) {
        return PathKey(A) < PathKey(B);
    });

    const bool rootIsRepo = IsGitRepo(InRootAbs);
    std::vector<RepoRecord> repos;
    repos.reserve(sorted.size());

    for (const auto& repoPath : sorted) {
        RepoRecord record;
        record.path = Normalize(repoPath);
        if (rootIsRepo && PathKey(repoPath) == PathKey(InRootAbs)) {
            record.type = "root";
        } else if (InRegistered.contains(PathKey(repoPath))) {
            record.type = "registered";
        } else {
            record.type = "unregistered";
        }

        if (InMetadataLevel != "minimal") {
            record.currentBranch = CurrentBranch(repoPath);
            record.remotes = JoinLinesWithComma(RunGitCapture(repoPath, {"remote"}).stdoutStr);
            record.hasChanges = HasChanges(repoPath);
        }
        repos.push_back(std::move(record));
    }

    for (std::size_t idx = 0; idx < repos.size(); ++idx) {
        std::optional<std::size_t> nearestParent;
        for (std::size_t cand = 0; cand < repos.size(); ++cand) {
            if (cand == idx) {
                continue;
            }
            if (!IsPrefixPath(repos[cand].path, repos[idx].path)) {
                continue;
            }
            if (!nearestParent || repos[cand].path.generic_string().size() > repos[*nearestParent].path.generic_string().size()) {
                nearestParent = cand;
            }
        }
        if (nearestParent) {
            repos[idx].dependencies.push_back(repos[*nearestParent].path);
        }
    }

    return repos;
}

} // namespace

auto ReposToJson(const std::vector<RepoRecord>& InRepos) -> std::string {
    std::string json = "[";
    for (std::size_t idx = 0; idx < InRepos.size(); ++idx) {
        if (idx > 0) {
            json += ",";
        }
        const auto& repo = InRepos[idx];
        json += "{";
        json += std::format("\"path\":\"{}\",", EscapeJson(PathKey(repo.path)));
        json += std::format("\"type\":\"{}\",", EscapeJson(repo.type));
        json += std::format("\"current_branch\":\"{}\",", EscapeJson(repo.currentBranch));
        json += std::format("\"remotes\":\"{}\",", EscapeJson(repo.remotes));
        json += std::format("\"has_changes\":{},", repo.hasChanges ? "true" : "false");
        json += "\"dependencies\":[";
        for (std::size_t depIdx = 0; depIdx < repo.dependencies.size(); ++depIdx) {
            if (depIdx > 0) {
                json += ",";
            }
            json += std::format("\"{}\"", EscapeJson(PathKey(repo.dependencies[depIdx])));
        }
        json += "]";
        json += "}";
    }
    json += "]";
    return json;
}

auto ManifestToJson(const std::filesystem::path& InWorkspaceRoot, const std::vector<RepoRecord>& InRepos) -> std::string {
    std::string json = "{";
    json += "\"version\":\"1.0\",";
    json += std::format("\"workspace_root\":\"{}\",", EscapeJson(PathKey(InWorkspaceRoot)));
    json += std::format("\"generated_at\":\"{}\",", EscapeJson(ToUtcIsoString(std::chrono::system_clock::now())));
    json += "\"repos\":[";
    for (std::size_t idx = 0; idx < InRepos.size(); ++idx) {
        if (idx > 0) {
            json += ",";
        }
        json += "{";
        json += std::format("\"path\":\"{}\",", EscapeJson(PathKey(InRepos[idx].path)));
        json += std::format("\"type\":\"{}\"", EscapeJson(InRepos[idx].type));
        json += "}";
    }
    json += "]}";
    return json;
}

auto DiscoverRepos(const DiscoverOptions& InOptions) -> DiscoveryResult {
    DiscoveryResult result;

    auto rootAbs = std::filesystem::absolute(InOptions.rootDir).lexically_normal();
    if (rootAbs.empty()) {
        rootAbs = std::filesystem::current_path();
    }

    DiscoverOptions options = InOptions;
    if (options.excludePatterns.empty()) {
        options.excludePatterns = DefaultExcludePatterns();
    }
    const auto ignoreRules = BuildIgnoreRules(rootAbs, options.excludePatterns);

    if (options.maxDepth <= 0) {
        options.maxDepth = std::numeric_limits<int>::max();
    }
    if (options.metadataLevel != "minimal" && options.metadataLevel != "full") {
        options.metadataLevel = "full";
    }
    if (options.cacheTtlSeconds < 0) {
        options.cacheTtlSeconds = 0;
    }
    if (options.maxStaleSeconds < 0) {
        options.maxStaleSeconds = 0;
    }

    const auto marker = options.incremental ? ComputeMarker(rootAbs, options.maxDepth, ignoreRules) : std::string{};
    const auto cacheFile = CacheFilePath(options, rootAbs);
    PruneLegacyCacheFiles(cacheFile);

    result.cacheFile = cacheFile;
    result.marker = marker;
    result.mode = "scan-miss";

    if (options.useCache && !options.refreshCache && options.cacheTtlSeconds > 0) {
        const auto payload = ReadFileText(cacheFile);
        if (payload) {
            const auto nowEpoch = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            const auto cacheMtime = FileMtimeEpochSeconds(cacheFile);
            const auto age = nowEpoch - cacheMtime;

            bool valid = false;
            bool incrementalHit = false;
            if (age <= options.cacheTtlSeconds) {
                valid = true;
            } else if (options.incremental && options.maxStaleSeconds > options.cacheTtlSeconds && age <= options.maxStaleSeconds) {
                const auto cachedMarker = ExtractStringField(*payload, "marker");
                if (cachedMarker && *cachedMarker == marker) {
                    valid = true;
                    incrementalHit = true;
                }
            }

            if (valid) {
                if (FileMtimeEpochSeconds(rootAbs / ".gitmodules") > cacheMtime) {
                    valid = false;
                }
            }

            if (valid) {
                const auto parsed = ParseCache(*payload);
                if (parsed) {
                    result.repos = *parsed;
                    std::sort(result.repos.begin(), result.repos.end(), [](const RepoRecord& A, const RepoRecord& B) {
                        return PathKey(A.path) < PathKey(B.path);
                    });
                    result.mode = incrementalHit ? "cache-incremental-hit" : "cache-fresh-hit";
                    return result;
                }
            }
        }
    }

    std::set<std::string> registered;
    if (IsGitRepo(rootAbs)) {
        CollectRegisteredSubmodulesRecursive(rootAbs, registered);
    }

    const auto discovered = DiscoverGitRepos(rootAbs, options.maxDepth, ignoreRules);
    result.repos = BuildRepoRecords(rootAbs, discovered, registered, options.metadataLevel);

    if (options.useCache && options.cacheTtlSeconds > 0) {
        const auto payload = CachePayloadFromRepos(rootAbs, marker, result.repos);
        (void)WriteFileText(cacheFile, payload);
    }

    return result;
}

} // namespace kano::git::workspace
