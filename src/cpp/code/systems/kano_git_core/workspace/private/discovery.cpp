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
#include <thread>
#include <cstdint>
#include <unordered_map>
#include <unordered_set>

#if defined(_WIN32)
#include <windows.h>
#include <process.h>
#else
#include <cerrno>
#include <csignal>
#include <unistd.h>
#endif

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

auto CurrentProcessId() -> long long {
#if defined(_WIN32)
    return static_cast<long long>(_getpid());
#else
    return static_cast<long long>(getpid());
#endif
}

auto CurrentProcessCommand() -> std::string {
    return "kano-git";
}

auto LockPathFor(const std::filesystem::path& InFile) -> std::filesystem::path {
    return std::filesystem::path(InFile.generic_string() + ".lock").lexically_normal();
}

auto LockOwnerMetadataPath(const std::filesystem::path& InLockPath) -> std::filesystem::path {
    return (InLockPath / "owner.txt").lexically_normal();
}

auto ProcessIsActive(const long long InPid) -> bool {
    if (InPid <= 0) {
        return false;
    }
#if defined(_WIN32)
    const HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(InPid));
    if (process == nullptr) {
        return false;
    }
    const DWORD wait = WaitForSingleObject(process, 0);
    CloseHandle(process);
    return wait == WAIT_TIMEOUT;
#else
    if (::kill(static_cast<pid_t>(InPid), 0) == 0) {
        return true;
    }
    return errno == EPERM;
#endif
}

auto FileAgeSeconds(const std::filesystem::path& InPath) -> long long {
    std::error_code ec;
    if (!std::filesystem::exists(InPath, ec)) {
        return -1;
    }
    const auto writeTime = std::filesystem::last_write_time(InPath, ec);
    if (ec) {
        return -1;
    }
    const auto now = decltype(writeTime)::clock::now();
    return std::chrono::duration_cast<std::chrono::seconds>(now - writeTime).count();
}

auto ParseOwnerMetadata(const std::filesystem::path& InLockPath) -> CacheLockInfo {
    CacheLockInfo out;
    out.lockPath = InLockPath;
    out.exists = true;
    out.targetPath = InLockPath;
    const auto owner = ReadFileText(LockOwnerMetadataPath(InLockPath));
    if (!owner) {
        out.ageSeconds = FileAgeSeconds(InLockPath);
        out.staleCandidate = out.ageSeconds >= 30;
        return out;
    }
    std::istringstream iss(*owner);
    std::string line;
    while (std::getline(iss, line)) {
        const auto pos = line.find('=');
        if (pos == std::string::npos) {
            continue;
        }
        const auto key = Trim(line.substr(0, pos));
        const auto value = Trim(line.substr(pos + 1));
        if (key == "pid") {
            try {
                out.ownerPid = std::stoll(value);
            } catch (...) {
                out.ownerPid = -1;
            }
        } else if (key == "command") {
            out.ownerCommand = value;
        } else if (key == "target") {
            out.targetPath = std::filesystem::path(value).lexically_normal();
        }
    }
    out.ageSeconds = FileAgeSeconds(InLockPath);
    out.activeProcessDetected = ProcessIsActive(out.ownerPid);
    out.staleCandidate = !out.activeProcessDetected && out.ageSeconds >= 30;
    return out;
}

auto WriteOwnerMetadata(const std::filesystem::path& InLockPath, const std::filesystem::path& InTargetPath) -> bool {
    const auto ownerPath = LockOwnerMetadataPath(InLockPath);
    std::ofstream out(ownerPath, std::ios::out | std::ios::binary | std::ios::trunc);
    if (!out) {
        return false;
    }
    out << "pid=" << CurrentProcessId() << "\n";
    out << "command=" << CurrentProcessCommand() << "\n";
    out << "target=" << InTargetPath.lexically_normal().generic_string() << "\n";
    out << "created_at=" << ToUtcIsoString(std::chrono::system_clock::now()) << "\n";
    return out.good();
}

auto RemoveLockPath(const std::filesystem::path& InLockPath) -> bool {
    std::error_code ec;
    std::filesystem::remove_all(InLockPath, ec);
    return !ec;
}

class ScopedCacheFileLock {
  public:
    ScopedCacheFileLock() = default;
    ScopedCacheFileLock(const ScopedCacheFileLock&) = delete;
    auto operator=(const ScopedCacheFileLock&) -> ScopedCacheFileLock& = delete;
    ScopedCacheFileLock(ScopedCacheFileLock&& InOther) noexcept
        : lockPath_(std::move(InOther.lockPath_)), owns_(InOther.owns_) {
        InOther.owns_ = false;
    }
    auto operator=(ScopedCacheFileLock&& InOther) noexcept -> ScopedCacheFileLock& {
        if (this != &InOther) {
            Release();
            lockPath_ = std::move(InOther.lockPath_);
            owns_ = InOther.owns_;
            InOther.owns_ = false;
        }
        return *this;
    }
    ~ScopedCacheFileLock() {
        Release();
    }

    static auto Acquire(const std::filesystem::path& InFile,
                        std::string* OutError = nullptr,
                        int InTimeoutMs = 5000,
                        int InStaleAgeSeconds = 30) -> std::optional<ScopedCacheFileLock> {
        ScopedCacheFileLock out;
        out.lockPath_ = LockPathFor(InFile);
        std::error_code ec;
        std::filesystem::create_directories(out.lockPath_.parent_path(), ec);
        if (ec) {
            if (OutError != nullptr) {
                *OutError = "cannot create cache lock parent directory";
            }
            return std::nullopt;
        }

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds((std::max)(0, InTimeoutMs));
        while (true) {
            ec.clear();
            if (std::filesystem::create_directory(out.lockPath_, ec)) {
                if (!WriteOwnerMetadata(out.lockPath_, InFile)) {
                    RemoveLockPath(out.lockPath_);
                    if (OutError != nullptr) {
                        *OutError = "cannot write cache lock owner metadata";
                    }
                    return std::nullopt;
                }
                out.owns_ = true;
                return out;
            }

            CacheLockInfo info = ParseOwnerMetadata(out.lockPath_);
            info.targetPath = InFile;
            if (!info.activeProcessDetected && info.ageSeconds >= InStaleAgeSeconds) {
                (void)RemoveLockPath(out.lockPath_);
                continue;
            }

            if (std::chrono::steady_clock::now() >= deadline) {
                if (OutError != nullptr) {
                    *OutError = std::format("cache lock busy: {} (pid={} age={}s command={})",
                                            out.lockPath_.generic_string(),
                                            info.ownerPid,
                                            info.ageSeconds,
                                            info.ownerCommand.empty() ? "-" : info.ownerCommand);
                }
                return std::nullopt;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }

  private:
    void Release() {
        if (!owns_) {
            return;
        }
        owns_ = false;
        (void)RemoveLockPath(lockPath_);
    }

    std::filesystem::path lockPath_;
    bool owns_ = false;
};

auto WriteFileTextUnlocked(const std::filesystem::path& InFile, const std::string& InText, std::string* OutError = nullptr) -> bool {
    std::error_code ec;
    std::filesystem::create_directories(InFile.parent_path(), ec);
    if (ec) {
        if (OutError != nullptr) {
            *OutError = "cannot create cache file parent directory";
        }
        return false;
    }
    const auto temp = InFile.parent_path() /
                      std::format("{}.tmp.{}.{}",
                                  InFile.filename().generic_string(),
                                  CurrentProcessId(),
                                  std::chrono::duration_cast<std::chrono::microseconds>(
                                      std::chrono::steady_clock::now().time_since_epoch()).count());
    {
        std::ofstream out(temp, std::ios::out | std::ios::binary | std::ios::trunc);
        if (!out) {
            if (OutError != nullptr) {
                *OutError = "cannot open temp output file";
            }
            return false;
        }
        out << InText;
        if (!out.good()) {
            if (OutError != nullptr) {
                *OutError = "failed while writing temp output file";
            }
            return false;
        }
    }
    std::filesystem::rename(temp, InFile, ec);
    if (ec) {
        if (std::filesystem::exists(InFile, ec)) {
            ec.clear();
            std::filesystem::remove(InFile, ec);
            ec.clear();
            std::filesystem::rename(temp, InFile, ec);
        }
    }
    if (ec) {
        std::filesystem::remove(temp, ec);
        if (OutError != nullptr) {
            *OutError = "failed to atomically replace target file";
        }
        return false;
    }
    return true;
}

auto WriteFileText(const std::filesystem::path& InFile, const std::string& InText) -> bool {
    std::string ignoredError;
    return kano::git::workspace::WriteCacheFileText(InFile, InText, &ignoredError);
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

struct IgnoreRule {
    std::string pattern;
    bool include = false;
    bool directoryOnly = false;
};

auto RunGitCapture(const std::filesystem::path& InRepoPath, const std::vector<std::string>& InArgs) -> shell::ExecResult;
auto IsGitRepo(const std::filesystem::path& InRepoPath) -> bool;

auto EscapeRegexChar(const char InChar) -> std::string {
    switch (InChar) {
        case '.':
        case '^':
        case '$':
        case '|':
        case '(':
        case ')':
        case '[':
        case ']':
        case '{':
        case '}':
        case '+':
        case '\\':
            return std::string("\\") + InChar;
        default:
            return std::string(1, InChar);
    }
}

auto GlobToRegex(const std::string& InPattern) -> std::string {
    std::string regex = "^";
    for (std::size_t i = 0; i < InPattern.size(); ++i) {
        const char ch = InPattern[i];
        if (ch == '*') {
            const bool isGlobStar = (i + 1 < InPattern.size() && InPattern[i + 1] == '*');
            if (isGlobStar) {
                regex += ".*";
                i += 1;
            } else {
                regex += "[^/]*";
            }
            continue;
        }
        if (ch == '?') {
            regex += "[^/]";
            continue;
        }
        regex += EscapeRegexChar(ch);
    }
    regex += "$";
    return regex;
}

auto GlobMatchesPath(const std::string& InRelPath, const std::string& InPattern) -> bool {
    try {
        return std::regex_match(InRelPath, std::regex(GlobToRegex(InPattern), std::regex::ECMAScript));
    } catch (const std::regex_error&) {
        return false;
    }
}

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

        const bool directoryOnly = !trimmed.empty() && trimmed.back() == '/';
        const auto normalized = NormalizeRulePattern(trimmed);
        if (normalized.empty()) {
            continue;
        }

        rules.push_back(IgnoreRule{.pattern = normalized, .include = include, .directoryOnly = directoryOnly});
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
        rules.push_back(IgnoreRule{.pattern = normalized, .include = false, .directoryOnly = false});
    }

    for (const auto& fileRule : LoadIgnoreRulesFromFile(InRoot / ".gitignore")) {
        rules.push_back(fileRule);
    }
    for (const auto& fileRule : LoadIgnoreRulesFromFile(InRoot / ".kogignore")) {
        rules.push_back(fileRule);
    }

    return rules;
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

    if (InRule.directoryOnly) {
        if (InRule.include) {
            return InRelPath == pattern;
        }
        return InRelPath == pattern || InRelPath.starts_with(pattern + "/");
    }

    if (pattern.size() > 3 && pattern.ends_with("/**")) {
        const auto base = pattern.substr(0, pattern.size() - 3);
        if (InRelPath == base || InRelPath.starts_with(base + "/")) {
            return true;
        }
    }

    if (pattern.find('/') == std::string::npos) {
        if (pattern.find('*') != std::string::npos || pattern.find('?') != std::string::npos) {
            std::size_t start = 0;
            while (start <= InRelPath.size()) {
                const auto slash = InRelPath.find('/', start);
                const auto end = (slash == std::string::npos) ? InRelPath.size() : slash;
                const auto segment = InRelPath.substr(start, end - start);
                if (!segment.empty() && GlobMatchesPath(segment, pattern)) {
                    return true;
                }
                if (slash == std::string::npos) {
                    break;
                }
                start = slash + 1;
            }
            return false;
        }
        return PathContainsSegment(InRelPath, pattern);
    }

    if (GlobMatchesPath(InRelPath, pattern)) {
        return true;
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

    auto owner = InPath.parent_path();
    while (!owner.empty()) {
        if (owner == InRoot.parent_path()) {
            break;
        }
        if (owner != InPath && IsGitRepo(owner)) {
            const auto ownerRel = std::filesystem::relative(InPath, owner, ec);
            if (!ec && !ownerRel.empty() && ownerRel != ".") {
                const auto gitIgnored = RunGitCapture(owner, {"check-ignore", "-q", "--no-index", PathKey(ownerRel)});
                if (gitIgnored.exitCode == 0) {
                    return true;
                }
            }
            break;
        }
        if (owner == InRoot) {
            break;
        }
        owner = owner.parent_path();
    }

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

    const std::vector<std::string> prePrune;
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

auto WorkspaceCacheDirFor(const std::filesystem::path& InRoot) -> std::filesystem::path {
    if (IsGitRepo(InRoot)) {
        const auto configured = RunGitCapture(InRoot, {"config", "--path", "--get", "kano.cache.local-dir"});
        if (configured.exitCode == 0) {
            const auto value = Trim(configured.stdoutStr);
            if (!value.empty()) {
                const std::filesystem::path configuredPath(value);
                if (configuredPath.is_absolute()) {
                    return configuredPath.lexically_normal();
                }
                return (InRoot / configuredPath).lexically_normal();
            }
        }
    }
    return (InRoot / ".kano" / "cache" / "git").lexically_normal();
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

    const std::vector<std::string> prePrune;
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

auto ExtractContainerRaw(const std::string& InPayload,
                         const std::string& InField,
                         const char InOpen,
                         const char InClose) -> std::optional<std::string> {
    const auto keyPos = InPayload.find(std::format("\"{}\":", InField));
    if (keyPos == std::string::npos) {
        return std::nullopt;
    }
    auto start = InPayload.find(InOpen, keyPos);
    if (start == std::string::npos) {
        return std::nullopt;
    }
    int depth = 0;
    bool inString = false;
    bool escaping = false;
    for (std::size_t idx = start; idx < InPayload.size(); ++idx) {
        const char ch = InPayload[idx];
        if (inString) {
            if (escaping) {
                escaping = false;
            } else if (ch == '\\') {
                escaping = true;
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
        } else if (ch == InClose) {
            depth -= 1;
            if (depth == 0) {
                return InPayload.substr(start, idx - start + 1);
            }
        }
    }
    return std::nullopt;
}

auto ExtractReposArrayRaw(const std::string& InPayload) -> std::optional<std::string> {
    return ExtractContainerRaw(InPayload, "repos", '[', ']');
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

auto ExtractArrayRaw(const std::string& InPayload, const std::string& InField) -> std::optional<std::string> {
    return ExtractContainerRaw(InPayload, InField, '[', ']');
}

auto ExtractObjectRaw(const std::string& InPayload, const std::string& InField) -> std::optional<std::string> {
    return ExtractContainerRaw(InPayload, InField, '{', '}');
}

auto ParseGitmodulesFingerprints(const std::string& InRawArray) -> std::unordered_map<std::string, std::string> {
    std::unordered_map<std::string, std::string> out;
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
        const auto path = ExtractStringField(obj, "path");
        const auto fingerprint = ExtractStringField(obj, "fingerprint");
        if (path && fingerprint) {
            out.emplace(*path, *fingerprint);
        }
        idx = objEnd + 1;
    }
    return out;
}

struct DiscoveryCacheSnapshot {
    long long generatedEpoch = 0;
    long long gitmodulesMtime = 0;
    std::string marker;
    std::vector<RepoRecord> repos;
};

struct WorkspaceStateDocument {
    std::filesystem::path workspaceRoot;
    std::filesystem::path filePath;
    std::optional<WorkspaceManifest> manifest;
    std::optional<DiscoveryCacheSnapshot> discoveryCache;
};

auto GitmodulesFingerprint(const std::filesystem::path& InRepoPath) -> std::string {
    const auto gitmodules = InRepoPath / ".gitmodules";
    const auto text = ReadFileText(gitmodules);
    if (!text) {
        return "missing";
    }
    return HashFNV1a(*text);
}

auto RelativePathKeyOrDot(const std::filesystem::path& InRoot, const std::filesystem::path& InPath) -> std::string {
    std::error_code ec;
    auto relative = std::filesystem::relative(InPath, InRoot, ec);
    if (ec || relative.empty()) {
        relative = InPath.lexically_relative(InRoot);
    }
    auto key = relative.empty() ? std::string(".") : PathKey(relative);
    if (key.empty()) {
        key = ".";
    }
    return key;
}

auto SortUniquePaths(std::vector<std::filesystem::path>* OutPaths) -> void {
    if (OutPaths == nullptr) {
        return;
    }
    std::sort(OutPaths->begin(), OutPaths->end(), [](const auto& A, const auto& B) {
        return PathKey(A) < PathKey(B);
    });
    OutPaths->erase(std::unique(OutPaths->begin(), OutPaths->end(), [](const auto& A, const auto& B) {
        return PathKey(A) == PathKey(B);
    }), OutPaths->end());
}

auto ManifestReposToJson(const WorkspaceManifest& InManifest) -> std::string {
    std::string json = "[";
    for (std::size_t idx = 0; idx < InManifest.repos.size(); ++idx) {
        if (idx > 0) {
            json += ",";
        }
        const auto& repo = InManifest.repos[idx];
        json += "{";
        json += std::format("\"path\":\"{}\",", EscapeJson(RelativePathKeyOrDot(InManifest.workspaceRoot, repo.path)));
        json += std::format("\"type\":\"{}\"", EscapeJson(repo.type));
        json += "}";
    }
    json += "]";
    return json;
}

auto RepoRecordsToRelativeJson(const std::filesystem::path& InWorkspaceRoot,
                               const std::vector<RepoRecord>& InRepos) -> std::string {
    std::string json = "[";
    for (std::size_t idx = 0; idx < InRepos.size(); ++idx) {
        if (idx > 0) {
            json += ",";
        }
        const auto& repo = InRepos[idx];
        json += "{";
        json += std::format("\"path\":\"{}\",", EscapeJson(RelativePathKeyOrDot(InWorkspaceRoot, repo.path)));
        json += std::format("\"type\":\"{}\",", EscapeJson(repo.type));
        json += std::format("\"current_branch\":\"{}\",", EscapeJson(repo.currentBranch));
        json += std::format("\"remotes\":\"{}\",", EscapeJson(repo.remotes));
        json += std::format("\"has_changes\":{},", repo.hasChanges ? "true" : "false");
        json += "\"dependencies\":[";
        for (std::size_t depIdx = 0; depIdx < repo.dependencies.size(); ++depIdx) {
            if (depIdx > 0) {
                json += ",";
            }
            json += std::format("\"{}\"", EscapeJson(RelativePathKeyOrDot(InWorkspaceRoot, repo.dependencies[depIdx])));
        }
        json += "]";
        json += "}";
    }
    json += "]";
    return json;
}

auto ManifestFingerprintsToJson(const WorkspaceManifest& InManifest) -> std::string {
    std::string json = "[";
    std::vector<std::pair<std::string, std::string>> sorted(
        InManifest.gitmodulesFingerprints.begin(), InManifest.gitmodulesFingerprints.end());
    std::sort(sorted.begin(), sorted.end(), [](const auto& A, const auto& B) {
        return A.first < B.first;
    });
    for (std::size_t idx = 0; idx < sorted.size(); ++idx) {
        if (idx > 0) {
            json += ",";
        }
        json += "{";
        json += std::format("\"path\":\"{}\",", EscapeJson(sorted[idx].first));
        json += std::format("\"fingerprint\":\"{}\"", EscapeJson(sorted[idx].second));
        json += "}";
    }
    json += "]";
    return json;
}

auto WorkspaceManifestToJson(const WorkspaceManifest& InManifest) -> std::string {
    std::string json = "{";
    json += "\"version\":3,";
    json += std::format("\"workspace_root\":\"{}\",", EscapeJson(PathKey(InManifest.workspaceRoot)));
    json += std::format("\"generated_at\":\"{}\",", EscapeJson(ToUtcIsoString(std::chrono::system_clock::now())));
    json += std::format("\"repos\":{},", RepoRecordsToRelativeJson(InManifest.workspaceRoot, InManifest.repos));
    json += std::format("\"gitmodules_fingerprints\":{}", ManifestFingerprintsToJson(InManifest));
    json += "}";
    return json;
}

auto DiscoveryStateToJson(const DiscoveryCacheSnapshot& InCache) -> std::string {
    return std::format(
        "{{\"generated_epoch\":{},\"gitmodules_mtime\":{},\"marker\":\"{}\"}}",
        InCache.generatedEpoch,
        InCache.gitmodulesMtime,
        EscapeJson(InCache.marker.empty() ? "none" : InCache.marker));
}

auto WorkspaceStateDocumentToJson(const WorkspaceStateDocument& InState) -> std::string {
    std::string json = "{";
    json += "\"version\":3,";
    json += std::format("\"workspace_root\":\"{}\",", EscapeJson(PathKey(InState.workspaceRoot)));
    json += std::format("\"generated_at\":\"{}\"", EscapeJson(ToUtcIsoString(std::chrono::system_clock::now())));
    const std::vector<RepoRecord>* repos = nullptr;
    if (InState.discoveryCache.has_value()) {
        repos = &InState.discoveryCache->repos;
    } else if (InState.manifest.has_value()) {
        repos = &InState.manifest->repos;
    }
    if (repos != nullptr) {
        json += std::format(",\"repos\":{}", RepoRecordsToRelativeJson(InState.workspaceRoot, *repos));
    }
    if (InState.manifest.has_value()) {
        json += std::format(",\"gitmodules_fingerprints\":{}", ManifestFingerprintsToJson(*InState.manifest));
    }
    if (InState.discoveryCache.has_value()) {
        json += std::format(",\"discovery_state\":{}", DiscoveryStateToJson(*InState.discoveryCache));
    }
    json += "}";
    return json;
}

auto AbsolutizeRepoRecords(const std::filesystem::path& InWorkspaceRoot, std::vector<RepoRecord>* OutRepos) -> void {
    if (OutRepos == nullptr) {
        return;
    }
    for (auto& repo : *OutRepos) {
        if (repo.path.is_relative()) {
            repo.path = (InWorkspaceRoot / repo.path).lexically_normal();
        } else {
            repo.path = repo.path.lexically_normal();
        }
        for (auto& dep : repo.dependencies) {
            if (dep.is_relative()) {
                dep = (InWorkspaceRoot / dep).lexically_normal();
            } else {
                dep = dep.lexically_normal();
            }
        }
    }
}

auto ParseWorkspaceManifest(const std::string& InPayload, const std::filesystem::path& InManifestFile) -> std::optional<WorkspaceManifest> {
    const auto workspaceRoot = ExtractStringField(InPayload, "workspace_root");
    const auto manifestRaw = ExtractObjectRaw(InPayload, "manifest");
    const auto topLevelReposRaw = ExtractArrayRaw(InPayload, "repos");
    const auto& manifestPayload = manifestRaw.value_or(InPayload);
    const auto reposRaw = topLevelReposRaw.has_value() ? topLevelReposRaw : ExtractArrayRaw(manifestPayload, "repos");
    const auto fingerprintsRaw = ExtractArrayRaw(manifestPayload, "gitmodules_fingerprints");
    if (!workspaceRoot || !reposRaw) {
        return std::nullopt;
    }

    WorkspaceManifest out;
    out.workspaceRoot = Normalize(std::filesystem::path(*workspaceRoot));
    out.manifestFile = InManifestFile;
    out.repos = ParseReposArray(*reposRaw);
    AbsolutizeRepoRecords(out.workspaceRoot, &out.repos);
    if (fingerprintsRaw) {
        out.gitmodulesFingerprints = ParseGitmodulesFingerprints(*fingerprintsRaw);
    }
    return out;
}

auto ParseDiscoveryCacheSnapshot(const std::string& InPayload) -> std::optional<DiscoveryCacheSnapshot> {
    const auto workspaceRoot = ExtractStringField(InPayload, "workspace_root");
    const auto topLevelReposRaw = ExtractArrayRaw(InPayload, "repos");
    const auto cacheRaw = ExtractObjectRaw(InPayload, "discovery_cache");
    const auto stateRaw = ExtractObjectRaw(InPayload, "discovery_state");
    const auto& cachePayload = stateRaw ? *stateRaw : (cacheRaw ? *cacheRaw : InPayload);
    const auto reposRaw = topLevelReposRaw.has_value() ? topLevelReposRaw : ExtractArrayRaw(cachePayload, "repos");
    if (!reposRaw || !workspaceRoot) {
        return std::nullopt;
    }

    DiscoveryCacheSnapshot out;
    out.generatedEpoch = ExtractIntField(cachePayload, "generated_epoch").value_or(0);
    out.gitmodulesMtime = ExtractIntField(cachePayload, "gitmodules_mtime").value_or(0);
    out.marker = ExtractStringField(cachePayload, "marker").value_or("none");
    out.repos = ParseReposArray(*reposRaw);
    AbsolutizeRepoRecords(Normalize(std::filesystem::path(*workspaceRoot)), &out.repos);
    return out;
}

auto IsWorkspaceManifestTrustedImpl(const WorkspaceManifest& InManifest,
                                    const std::filesystem::path& InRoot,
                                    std::string* OutReason) -> bool {
    const auto rootAbs = Normalize(std::filesystem::absolute(InRoot));
    if (Normalize(InManifest.workspaceRoot) != rootAbs) {
        if (OutReason != nullptr) {
            *OutReason = "workspace root mismatch";
        }
        return false;
    }

    for (const auto& repo : InManifest.repos) {
        std::error_code ec;
        if (!std::filesystem::exists(repo.path, ec) || !std::filesystem::is_directory(repo.path, ec)) {
            if (OutReason != nullptr) {
                *OutReason = std::format("repo path missing: {}", repo.path.generic_string());
            }
            return false;
        }
    }

    for (const auto& [relPath, stored] : InManifest.gitmodulesFingerprints) {
        const auto repoPath = (rootAbs / std::filesystem::path(relPath)).lexically_normal();
        const auto current = GitmodulesFingerprint(repoPath);
        if (current != stored) {
            if (OutReason != nullptr) {
                *OutReason = std::format(".gitmodules changed: {}", relPath);
            }
            return false;
        }
    }

    return true;
}

auto BuildDiscoveryCacheSnapshot(const std::filesystem::path& InRoot,
                                 const std::string& InMarker,
                                 const std::vector<RepoRecord>& InRepos) -> DiscoveryCacheSnapshot {
    const auto now = std::chrono::system_clock::now();
    const auto nowEpoch = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();
    return DiscoveryCacheSnapshot{
        .generatedEpoch = nowEpoch,
        .gitmodulesMtime = FileMtimeEpochSeconds(InRoot / ".gitmodules"),
        .marker = InMarker.empty() ? "none" : InMarker,
        .repos = InRepos,
    };
}

auto ParseCache(const std::string& InPayload) -> std::optional<std::vector<RepoRecord>> {
    const auto snapshot = ParseDiscoveryCacheSnapshot(InPayload);
    if (!snapshot.has_value()) {
        return std::nullopt;
    }
    return snapshot->repos;
}

auto CacheFilePath(const DiscoverOptions& InOptions, const std::filesystem::path& InRootAbs) -> std::filesystem::path {
    (void)InOptions;
    return WorkspaceManifestFilePath(InRootAbs);
}

auto LegacyDiscoverCacheFilePath(const std::filesystem::path& InRootAbs) -> std::filesystem::path {
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

auto LoadWorkspaceStateDocumentAny(const std::filesystem::path& InWorkspaceRoot) -> WorkspaceStateDocument {
    const auto rootAbs = Normalize(std::filesystem::absolute(InWorkspaceRoot));
    WorkspaceStateDocument out;
    out.workspaceRoot = rootAbs;
    out.filePath = WorkspaceManifestFilePath(rootAbs);

    if (const auto payload = ReadFileText(out.filePath); payload.has_value()) {
        out.manifest = ParseWorkspaceManifest(*payload, out.filePath);
        out.discoveryCache = ParseDiscoveryCacheSnapshot(*payload);
    }

    return out;
}

auto SaveWorkspaceStateDocument(const WorkspaceStateDocument& InState) -> bool {
    return WriteFileText(InState.filePath, WorkspaceStateDocumentToJson(InState));
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

    std::set<std::string> discoveredKeys;
    for (const auto& repoPath : sorted) {
        discoveredKeys.insert(PathKey(repoPath));
    }
    for (const auto& registeredKey : InRegistered) {
        if (discoveredKeys.contains(registeredKey)) {
            continue;
        }
        const std::filesystem::path registeredPath(registeredKey);
        RepoRecord record;
        record.path = Normalize(registeredPath);
        if (IsGitRepo(registeredPath)) {
            record.type = "registered";
            if (InMetadataLevel != "minimal") {
                record.currentBranch = CurrentBranch(registeredPath);
                record.remotes = JoinLinesWithComma(RunGitCapture(registeredPath, {"remote"}).stdoutStr);
                record.hasChanges = HasChanges(registeredPath);
            }
        } else {
            record.type = "registered-uninit";
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

auto DiscoverRegisteredPathsRecursive(const std::filesystem::path& InWorkspaceRoot) -> std::vector<std::filesystem::path> {
    std::vector<std::filesystem::path> out;
    std::vector<std::filesystem::path> queue{Normalize(std::filesystem::absolute(InWorkspaceRoot))};

    while (!queue.empty()) {
        const auto current = queue.back();
        queue.pop_back();

        const auto gitmodules = current / ".gitmodules";
        if (!std::filesystem::exists(gitmodules)) {
            continue;
        }

        const auto pathsResult = RunGitCapture(current, {"config", "-f", ".gitmodules", "--get-regexp", "^submodule\\..*\\.path$"});
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
            const auto full = Normalize(std::filesystem::weakly_canonical(current / relPath));
            if (std::find_if(out.begin(), out.end(), [&](const auto& candidate) {
                    return PathKey(candidate) == PathKey(full);
                }) == out.end()) {
                out.push_back(full);
                queue.push_back(full);
            }
        }
    }

    SortUniquePaths(&out);
    return out;
}

auto WorkspaceManifestFilePath(const std::filesystem::path& InWorkspaceRoot) -> std::filesystem::path {
    const auto rootAbs = Normalize(std::filesystem::absolute(InWorkspaceRoot));
    return (WorkspaceCacheDirFor(rootAbs) / "workspace-manifest.json").lexically_normal();
}

auto BuildWorkspaceManifest(const std::filesystem::path& InWorkspaceRoot, const std::vector<RepoRecord>& InRepos) -> WorkspaceManifest {
    WorkspaceManifest out;
    out.workspaceRoot = Normalize(std::filesystem::absolute(InWorkspaceRoot));
    out.manifestFile = WorkspaceManifestFilePath(out.workspaceRoot);

    out.repos = InRepos;
    std::sort(out.repos.begin(), out.repos.end(), [](const auto& A, const auto& B) {
        return PathKey(A.path) < PathKey(B.path);
    });

    out.gitmodulesFingerprints.emplace(".", GitmodulesFingerprint(out.workspaceRoot));
    for (const auto& repoPath : DiscoverRegisteredPathsRecursive(out.workspaceRoot)) {
        out.gitmodulesFingerprints.emplace(RelativePathKeyOrDot(out.workspaceRoot, repoPath), GitmodulesFingerprint(repoPath));
    }
    return out;
}

auto SaveWorkspaceManifest(const WorkspaceManifest& InManifest) -> bool {
    auto state = LoadWorkspaceStateDocumentAny(InManifest.workspaceRoot);
    auto manifest = InManifest;
    manifest.manifestFile = state.filePath;
    state.workspaceRoot = manifest.workspaceRoot;
    state.manifest = std::move(manifest);
    return SaveWorkspaceStateDocument(state);
}

auto LoadWorkspaceManifestAny(const std::filesystem::path& InWorkspaceRoot) -> std::optional<WorkspaceManifest> {
    return LoadWorkspaceStateDocumentAny(InWorkspaceRoot).manifest;
}

auto LoadTrustedWorkspaceManifest(const std::filesystem::path& InWorkspaceRoot, std::string* OutReason) -> std::optional<WorkspaceManifest> {
    const auto rootAbs = Normalize(std::filesystem::absolute(InWorkspaceRoot));
    const auto state = LoadWorkspaceStateDocumentAny(rootAbs);
    if (!state.manifest.has_value()) {
        if (OutReason != nullptr) {
            *OutReason = "workspace manifest missing";
        }
        return std::nullopt;
    }

    if (!IsWorkspaceManifestTrustedImpl(*state.manifest, rootAbs, OutReason)) {
        return std::nullopt;
    }
    return state.manifest;
}

auto UpsertUnregisteredRepoIntoWorkspaceManifest(const std::filesystem::path& InWorkspaceRoot,
                                                 const std::filesystem::path& InRepoPath) -> bool {
    const auto rootAbs = Normalize(std::filesystem::absolute(InWorkspaceRoot));
    const auto repoAbs = Normalize(std::filesystem::absolute(InRepoPath));
    std::string ignoredReason;
    auto manifest = LoadTrustedWorkspaceManifest(rootAbs, &ignoredReason).value_or(WorkspaceManifest{
        .workspaceRoot = rootAbs,
        .repos = {},
        .gitmodulesFingerprints = {{"." , GitmodulesFingerprint(rootAbs)}},
        .manifestFile = WorkspaceManifestFilePath(rootAbs),
    });

    const auto registered = DiscoverRegisteredPathsRecursive(rootAbs);
    const bool isRegistered = std::any_of(registered.begin(), registered.end(), [&](const auto& candidate) {
        return PathKey(candidate) == PathKey(repoAbs);
    });

    auto existing = std::find_if(manifest.repos.begin(), manifest.repos.end(), [&](const auto& repo) {
        return PathKey(repo.path) == PathKey(repoAbs);
    });
    if (existing == manifest.repos.end()) {
        RepoRecord record;
        record.path = repoAbs;
        record.type = isRegistered ? "registered" : (PathKey(repoAbs) == PathKey(rootAbs) ? "root" : "unregistered");
        manifest.repos.push_back(std::move(record));
    } else if (existing->type == "unregistered" && isRegistered) {
        existing->type = "registered";
    }

    manifest.gitmodulesFingerprints.clear();
    manifest.gitmodulesFingerprints.emplace(".", GitmodulesFingerprint(rootAbs));
    for (const auto& registeredPath : registered) {
        manifest.gitmodulesFingerprints.emplace(RelativePathKeyOrDot(rootAbs, registeredPath), GitmodulesFingerprint(registeredPath));
    }
    std::sort(manifest.repos.begin(), manifest.repos.end(), [](const auto& A, const auto& B) {
        return PathKey(A.path) < PathKey(B.path);
    });
    return SaveWorkspaceManifest(manifest);
}

auto RefreshWorkspaceManifestAfterRegisteredChange(const std::filesystem::path& InWorkspaceRoot) -> bool {
    const auto rootAbs = Normalize(std::filesystem::absolute(InWorkspaceRoot));
    const auto existing = LoadWorkspaceManifestAny(rootAbs);
    const auto registered = DiscoverRegisteredPathsRecursive(rootAbs);

    std::vector<RepoRecord> repos;
    RepoRecord rootRecord;
    rootRecord.path = rootAbs;
    rootRecord.type = "root";
    repos.push_back(rootRecord);

    for (const auto& repoPath : registered) {
        RepoRecord record;
        record.path = repoPath;
        record.type = "registered";
        repos.push_back(std::move(record));
    }

    if (existing.has_value()) {
        for (const auto& repo : existing->repos) {
            if (repo.type != "unregistered") {
                continue;
            }
            std::error_code ec;
            if (!std::filesystem::exists(repo.path, ec) || !std::filesystem::is_directory(repo.path, ec)) {
                continue;
            }
            const bool existsAlready = std::any_of(repos.begin(), repos.end(), [&](const auto& current) {
                return PathKey(current.path) == PathKey(repo.path);
            });
            if (!existsAlready) {
                repos.push_back(repo);
            }
        }
    }

    std::sort(repos.begin(), repos.end(), [](const auto& A, const auto& B) {
        return PathKey(A.path) < PathKey(B.path);
    });
    repos.erase(std::unique(repos.begin(), repos.end(), [](const auto& A, const auto& B) {
        return PathKey(A.path) == PathKey(B.path);
    }), repos.end());

    return SaveWorkspaceManifest(BuildWorkspaceManifest(rootAbs, repos));
}

auto WriteCacheFileText(const std::filesystem::path& InFile,
                        const std::string& InText,
                        std::string* OutError) -> bool {
    const auto lock = ScopedCacheFileLock::Acquire(InFile, OutError);
    if (!lock.has_value()) {
        return false;
    }
    return WriteFileTextUnlocked(InFile, InText, OutError);
}

auto InspectCacheLocks(const std::filesystem::path& InCacheRoot) -> std::vector<CacheLockInfo> {
    std::vector<CacheLockInfo> out;
    const auto root = Normalize(std::filesystem::absolute(InCacheRoot));
    std::error_code ec;
    if (!std::filesystem::exists(root, ec) || !std::filesystem::is_directory(root, ec)) {
        return out;
    }
    for (std::filesystem::recursive_directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_directory(ec)) {
            continue;
        }
        const auto path = it->path().lexically_normal();
        if (!path.filename().generic_string().ends_with(".lock")) {
            continue;
        }
        auto info = ParseOwnerMetadata(path);
        if (PathKey(info.targetPath) == PathKey(path)) {
            const auto stem = path.filename().generic_string();
            info.targetPath = path.parent_path() / stem.substr(0, stem.size() - std::string(".lock").size());
        }
        out.push_back(std::move(info));
        it.disable_recursion_pending();
    }
    std::sort(out.begin(), out.end(), [](const auto& A, const auto& B) {
        return PathKey(A.lockPath) < PathKey(B.lockPath);
    });
    return out;
}

auto CleanupStaleCacheLocks(const std::filesystem::path& InCacheRoot,
                            int InMinAgeSeconds) -> std::vector<CacheLockInfo> {
    auto locks = InspectCacheLocks(InCacheRoot);
    for (auto& lock : locks) {
        lock.staleCandidate = !lock.activeProcessDetected && lock.ageSeconds >= InMinAgeSeconds;
        if (lock.staleCandidate) {
            (void)RemoveLockPath(lock.lockPath);
            lock.exists = std::filesystem::exists(lock.lockPath) == true;
        }
    }
    return locks;
}

auto DiscoverRepos(const DiscoverOptions& InOptions) -> DiscoveryResult {
    DiscoveryResult result;
    auto reportProgress = [&](const std::string& InMessage) {
        if (InOptions.progressCallback) {
            InOptions.progressCallback(InMessage);
        }
    };

    auto rootAbs = std::filesystem::absolute(InOptions.rootDir).lexically_normal();
    if (rootAbs.empty()) {
        rootAbs = std::filesystem::current_path();
    }

    DiscoverOptions options = InOptions;
    const auto ignoreRules = BuildIgnoreRules(rootAbs, options.excludePatterns);

    if (options.maxDepth <= 0) {
        options.maxDepth = (std::numeric_limits<int>::max)();
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

    reportProgress(std::format("discover: preparing scan root={} depth={}", rootAbs.generic_string(), options.maxDepth));

    const auto marker = options.incremental ? ComputeMarker(rootAbs, options.maxDepth, ignoreRules) : std::string{};
    const auto cacheFile = CacheFilePath(options, rootAbs);
    PruneLegacyCacheFiles(LegacyDiscoverCacheFilePath(rootAbs));

    result.cacheFile = cacheFile;
    result.marker = marker;
    result.mode = "scan-miss";

    if (options.useCache && !options.refreshCache && options.cacheTtlSeconds > 0) {
        reportProgress("discover: checking cached workspace state");
        const auto state = LoadWorkspaceStateDocumentAny(rootAbs);
        if (state.discoveryCache.has_value()) {
            const auto nowEpoch = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            const auto cacheMtime = FileMtimeEpochSeconds(cacheFile);
            const auto age = nowEpoch - cacheMtime;

            bool valid = false;
            bool incrementalHit = false;
            if (age <= options.cacheTtlSeconds) {
                valid = true;
            } else if (options.incremental && options.maxStaleSeconds > options.cacheTtlSeconds && age <= options.maxStaleSeconds) {
                if (state.discoveryCache->marker == marker) {
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
                result.repos = state.discoveryCache->repos;
                std::sort(result.repos.begin(), result.repos.end(), [](const RepoRecord& A, const RepoRecord& B) {
                    return PathKey(A.path) < PathKey(B.path);
                });
                result.mode = incrementalHit ? "cache-incremental-hit" : "cache-fresh-hit";
                reportProgress(std::format("discover: cache hit ({}) repos={}", result.mode, result.repos.size()));
                return result;
            }
        }
    }

    reportProgress("discover: collecting registered submodules");
    std::set<std::string> registered;
    if (IsGitRepo(rootAbs)) {
        CollectRegisteredSubmodulesRecursive(rootAbs, registered);
    }

    reportProgress("discover: scanning filesystem for git repos");
    const auto discovered = DiscoverGitRepos(rootAbs, options.maxDepth, ignoreRules);
    reportProgress(std::format("discover: building repo metadata for {} repos", discovered.size()));
    result.repos = BuildRepoRecords(rootAbs, discovered, registered, options.metadataLevel);

    if (options.useCache && options.cacheTtlSeconds > 0) {
        reportProgress("discover: saving workspace discovery cache");
        auto state = LoadWorkspaceStateDocumentAny(rootAbs);
        state.workspaceRoot = rootAbs;
        state.filePath = cacheFile;
        state.discoveryCache = BuildDiscoveryCacheSnapshot(rootAbs, marker, result.repos);
        (void)SaveWorkspaceStateDocument(state);
    }

    reportProgress(std::format("discover: done mode={} repos={}", result.mode, result.repos.size()));

    return result;
}

} // namespace kano::git::workspace
