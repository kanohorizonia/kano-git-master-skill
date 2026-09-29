#include "agent_queue_cmd.hpp"

#include "plan_utils.hpp"
#include "secret_scan_utils.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <process.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace kano::git::commands {
namespace {

using Json = nlohmann::json;

struct QueueContext {
    std::filesystem::path repo;
    std::filesystem::path commonDir;
    std::filesystem::path root;
    std::filesystem::path statePath;
};

struct ScopedDirectoryLock {
    std::filesystem::path path;
    bool owned = false;
    std::error_code error;

    explicit ScopedDirectoryLock(std::filesystem::path InPath)
        : path(std::move(InPath)) {
        owned = std::filesystem::create_directory(path, error);
    }

    ~ScopedDirectoryLock() {
        if (!owned) {
            return;
        }
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }

    ScopedDirectoryLock(const ScopedDirectoryLock&) = delete;
    auto operator=(const ScopedDirectoryLock&) -> ScopedDirectoryLock& = delete;

    ScopedDirectoryLock(ScopedDirectoryLock&& InOther) noexcept
        : path(std::move(InOther.path)), owned(InOther.owned), error(InOther.error) {
        InOther.owned = false;
    }

    auto operator=(ScopedDirectoryLock&&) -> ScopedDirectoryLock& = delete;
};

struct ScopedIndexLock {
    std::filesystem::path path;
    bool owned = false;
    std::error_code error;

    explicit ScopedIndexLock(std::filesystem::path InPath) : path(std::move(InPath)) {
        owned = std::filesystem::create_directory(path, error);
    }
    ~ScopedIndexLock() {
        if (owned) {
            std::error_code ignored;
            std::filesystem::remove(path, ignored);
        }
    }
    ScopedIndexLock(const ScopedIndexLock&) = delete;
    auto operator=(const ScopedIndexLock&) -> ScopedIndexLock& = delete;
};

struct ScopedEnvironment {
    std::string name;
    std::optional<std::string> previous;

    ScopedEnvironment(std::string InName, const std::string& InValue)
        : name(std::move(InName)) {
        if (const char* value = std::getenv(name.c_str()); value != nullptr) {
            previous = value;
        }
        Set(InValue);
    }

    ~ScopedEnvironment() {
        if (previous.has_value()) {
            Set(*previous);
        } else {
#if defined(_WIN32)
            _putenv_s(name.c_str(), "");
#else
            unsetenv(name.c_str());
#endif
        }
    }

    ScopedEnvironment(const ScopedEnvironment&) = delete;
    auto operator=(const ScopedEnvironment&) -> ScopedEnvironment& = delete;

private:
    auto Set(const std::string& InValue) const -> void {
#if defined(_WIN32)
        _putenv_s(name.c_str(), InValue.c_str());
#else
        setenv(name.c_str(), InValue.c_str(), 1);
#endif
    }
};

auto ProcessId() -> long long {
#if defined(_WIN32)
    return static_cast<long long>(_getpid());
#else
    return static_cast<long long>(getpid());
#endif
}

auto TimestampId() -> std::string {
    static std::atomic<unsigned long long> sequence{0};
    const auto now = std::chrono::system_clock::now();
    const auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
    return std::to_string(millis) + "-" + std::to_string(ProcessId()) + "-" +
           std::to_string(sequence.fetch_add(1, std::memory_order_relaxed));
}

auto IsSafeId(const std::string& InValue) -> bool {
    if (InValue.empty()) {
        return false;
    }
    return std::all_of(InValue.begin(), InValue.end(), [](const unsigned char Ch) {
        return std::isalnum(Ch) != 0 || Ch == '.' || Ch == '_' || Ch == '-';
    });
}

auto GitValue(const std::filesystem::path& InRepo,
              const std::vector<std::string>& InArgs,
              std::string* OutError = nullptr) -> std::optional<std::string> {
    const auto result = GitCapture(InRepo, InArgs);
    if (result.exitCode != 0) {
        if (OutError != nullptr) {
            *OutError = Trim(result.stderrStr.empty() ? result.stdoutStr : result.stderrStr);
        }
        return std::nullopt;
    }
    return Trim(result.stdoutStr);
}

auto CanonicalRepo(const std::filesystem::path& InRepo, std::string* OutError) -> std::optional<std::filesystem::path> {
    const auto start = InRepo.empty() ? std::filesystem::current_path() : InRepo;
    const auto top = GitValue(start, {"rev-parse", "--show-toplevel"}, OutError);
    if (!top.has_value()) {
        return std::nullopt;
    }
    std::error_code ec;
    const auto canonical = std::filesystem::weakly_canonical(std::filesystem::path(*top), ec);
    if (ec) {
        if (OutError != nullptr) {
            *OutError = "cannot resolve repository root: " + ec.message();
        }
        return std::nullopt;
    }
    return canonical;
}

auto ResolveQueueContext(const std::filesystem::path& InRepo, std::string* OutError) -> std::optional<QueueContext> {
    const auto repo = CanonicalRepo(InRepo, OutError);
    if (!repo.has_value()) {
        return std::nullopt;
    }
    const auto commonRaw = GitValue(*repo, {"rev-parse", "--git-common-dir"}, OutError);
    if (!commonRaw.has_value()) {
        return std::nullopt;
    }
    auto commonDir = std::filesystem::path(*commonRaw);
    if (commonDir.is_relative()) {
        commonDir = (*repo / commonDir).lexically_normal();
    }
    std::error_code ec;
    commonDir = std::filesystem::weakly_canonical(commonDir, ec);
    if (ec) {
        if (OutError != nullptr) {
            *OutError = "cannot resolve Git common directory: " + ec.message();
        }
        return std::nullopt;
    }
    QueueContext context;
    context.repo = *repo;
    context.commonDir = commonDir;
    context.root = commonDir / "kano-agent-queue";
    context.statePath = context.root / "state.json";
    std::filesystem::create_directories(context.root, ec);
    if (ec) {
        if (OutError != nullptr) {
            *OutError = "cannot create queue directory: " + ec.message();
        }
        return std::nullopt;
    }
    return context;
}

auto EmptyState() -> Json {
    return Json{
        {"schema", "kog-agent-mutation-queue-v1"},
        {"pending", Json::array()},
        {"active", nullptr},
        {"receipts", Json::array()},
    };
}

auto LoadState(const QueueContext& InContext, std::string* OutError) -> std::optional<Json> {
    std::error_code ec;
    if (!std::filesystem::exists(InContext.statePath, ec)) {
        return EmptyState();
    }
    try {
        std::ifstream stream(InContext.statePath, std::ios::binary);
        if (!stream) {
            throw std::runtime_error("open failed");
        }
        auto state = Json::parse(stream);
        if (state.value("schema", "") != "kog-agent-mutation-queue-v1" ||
            !state.contains("pending") || !state["pending"].is_array() ||
            !state.contains("active") ||
            !state.contains("receipts") || !state["receipts"].is_array()) {
            throw std::runtime_error("unsupported or malformed schema");
        }
        return state;
    } catch (const std::exception& error) {
        if (OutError != nullptr) {
            *OutError = std::string("cannot read queue state: ") + error.what();
        }
        return std::nullopt;
    }
}

auto ReplaceFileAtomically(const std::filesystem::path& InSource,
                           const std::filesystem::path& InTarget,
                           std::string* OutError) -> bool {
#if defined(_WIN32)
    if (!MoveFileExW(InSource.c_str(), InTarget.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        if (OutError != nullptr) {
            *OutError = "atomic state replacement failed: " + std::to_string(GetLastError());
        }
        return false;
    }
    return true;
#else
    std::error_code ec;
    std::filesystem::rename(InSource, InTarget, ec);
    if (ec) {
        if (OutError != nullptr) {
            *OutError = "atomic state replacement failed: " + ec.message();
        }
        return false;
    }
    return true;
#endif
}

auto SaveState(const QueueContext& InContext, const Json& InState, std::string* OutError) -> bool {
    const auto temporary = InContext.root / ("state." + TimestampId() + ".tmp");
    {
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        if (!stream) {
            if (OutError != nullptr) {
                *OutError = "cannot create temporary queue state";
            }
            return false;
        }
        stream << InState.dump(2) << '\n';
        stream.flush();
        if (!stream) {
            if (OutError != nullptr) {
                *OutError = "cannot flush temporary queue state";
            }
            return false;
        }
    }
    if (!ReplaceFileAtomically(temporary, InContext.statePath, OutError)) {
        std::error_code ec;
        std::filesystem::remove(temporary, ec);
        return false;
    }
    return true;
}

auto NormalizeExactPath(const std::filesystem::path& InRepo,
                        const std::string& InInput,
                        std::string* OutError) -> std::optional<std::string> {
    if (Trim(InInput).empty()) {
        if (OutError != nullptr) {
            *OutError = "empty path selector";
        }
        return std::nullopt;
    }
    auto candidate = std::filesystem::path(InInput);
    if (candidate.is_relative()) {
        candidate = InRepo / candidate;
    }
    std::error_code ec;
    candidate = std::filesystem::weakly_canonical(candidate, ec);
    if (ec) {
        candidate = std::filesystem::absolute(candidate, ec).lexically_normal();
    }
    if (ec) {
        if (OutError != nullptr) {
            *OutError = "cannot resolve path '" + InInput + "': " + ec.message();
        }
        return std::nullopt;
    }
    auto relative = candidate.lexically_relative(InRepo);
    if (relative.empty() || relative == "." || relative.is_absolute()) {
        if (OutError != nullptr) {
            *OutError = "path must identify a file inside the repository: " + InInput;
        }
        return std::nullopt;
    }
    const auto generic = relative.generic_string();
    if (generic == ".." || generic.starts_with("../") || generic == ".git" || generic.starts_with(".git/")) {
        if (OutError != nullptr) {
            *OutError = "path escapes the repository or targets Git metadata: " + InInput;
        }
        return std::nullopt;
    }
    return generic;
}

auto NormalizePaths(const std::filesystem::path& InRepo,
                     const std::vector<std::string>& InPaths,
                     const bool InRequireFile,
                     std::string* OutError) -> std::optional<std::vector<std::string>> {
    std::vector<std::string> normalized;
    normalized.reserve(InPaths.size());
    for (const auto& input : InPaths) {
        const auto one = NormalizeExactPath(InRepo, input, OutError);
        if (!one.has_value()) {
            return std::nullopt;
        }
        normalized.push_back(*one);
    }
    std::sort(normalized.begin(), normalized.end());
    for (std::size_t index = 0; index < normalized.size(); ++index) {
        if (index > 0) {
            const auto& previous = normalized[index - 1];
            if (normalized[index] == previous || normalized[index].starts_with(previous + "/")) {
                if (OutError != nullptr) {
                    *OutError = "overlapping path selectors are not allowed: " + previous + " and " + normalized[index];
                }
                return std::nullopt;
            }
        }
    }
    if (!InRequireFile) {
        return normalized;
    }
    for (const auto& path : normalized) {
        std::error_code ec;
        if (std::filesystem::is_directory(InRepo / path, ec)) {
            const auto tracked = GitCapture(InRepo, {"-c", "core.quotepath=false", "ls-files", "--stage", "--", path});
            bool isTrackedGitlink = false;
            if (tracked.exitCode == 0) {
                std::istringstream records(tracked.stdoutStr);
                std::string record;
                while (std::getline(records, record)) {
                    const auto separator = record.find('\t');
                    if (separator != std::string::npos && record.starts_with("160000 ") &&
                        record.substr(separator + 1) == path) {
                        isTrackedGitlink = true;
                        break;
                    }
                }
            }
            if (!isTrackedGitlink) {
                if (OutError != nullptr) {
                    *OutError = "exact-path selectors must identify files or tracked gitlinks, not directories: " + path;
                }
                return std::nullopt;
            }
        }
        if (!std::filesystem::exists(InRepo / path, ec)) {
            const auto tracked = GitCapture(InRepo, {"ls-files", "--error-unmatch", "--", path});
            if (tracked.exitCode != 0) {
                if (OutError != nullptr) {
                    *OutError = "path does not exist and is not a tracked deletion: " + path;
                }
                return std::nullopt;
            }
        }
    }
    return normalized;
}

auto ParseKeyValue(const std::string& InValue, std::string* OutKey, std::string* OutValue) -> bool {
    const auto split = InValue.find('=');
    if (split == std::string::npos || split == 0 || split + 1 >= InValue.size()) {
        return false;
    }
    *OutKey = InValue.substr(0, split);
    *OutValue = InValue.substr(split + 1);
    return true;
}

auto ParseChunk(const std::string& InValue,
                std::string* OutPath,
                long long* OutStart,
                long long* OutEnd) -> bool {
    const auto colon = InValue.rfind(':');
    if (colon == std::string::npos || colon == 0 || colon + 1 >= InValue.size()) {
        return false;
    }
    const auto dash = InValue.find('-', colon + 1);
    if (dash == std::string::npos || dash == colon + 1 || dash + 1 >= InValue.size()) {
        return false;
    }
    try {
        *OutPath = InValue.substr(0, colon);
        *OutStart = std::stoll(InValue.substr(colon + 1, dash - colon - 1));
        *OutEnd = std::stoll(InValue.substr(dash + 1));
        return *OutStart > 0 && *OutEnd >= *OutStart;
    } catch (...) {
        return false;
    }
}

auto JsonStringSet(const Json& InValue) -> std::set<std::string> {
    std::set<std::string> values;
    if (!InValue.is_array()) {
        return values;
    }
    for (const auto& item : InValue) {
        if (item.is_string()) {
            values.insert(item.get<std::string>());
        }
    }
    return values;
}

auto ChunksDoNotOverlap(const Json& InFirst, const Json& InSecond) -> bool {
    if (!InFirst.is_array() || !InSecond.is_array() || InFirst.empty() || InSecond.empty()) {
        return false;
    }
    for (const auto& first : InFirst) {
        for (const auto& second : InSecond) {
            const auto firstStart = first.value("start", 0LL);
            const auto firstEnd = first.value("end", 0LL);
            const auto secondStart = second.value("start", 0LL);
            const auto secondEnd = second.value("end", 0LL);
            if (!(firstEnd < secondStart || secondEnd < firstStart)) {
                return false;
            }
        }
    }
    return true;
}

auto Compatible(const Json& InFirst, const Json& InSecond, std::string* OutReason) -> bool {
    const auto firstFiles = JsonStringSet(InFirst.value("files", Json::array()));
    const auto secondFiles = JsonStringSet(InSecond.value("files", Json::array()));
    for (const auto& path : firstFiles) {
        if (!secondFiles.contains(path)) {
            continue;
        }
        const auto firstPost = InFirst.value("postconditions", Json::object()).value(path, "");
        const auto secondPost = InSecond.value("postconditions", Json::object()).value(path, "");
        if (!firstPost.empty() && firstPost == secondPost) {
            continue;
        }
        const auto firstChunks = InFirst.value("chunks", Json::object()).value(path, Json::array());
        const auto secondChunks = InSecond.value("chunks", Json::object()).value(path, Json::array());
        if (ChunksDoNotOverlap(firstChunks, secondChunks)) {
            continue;
        }
        if (OutReason != nullptr) {
            *OutReason = "incompatible overlap on " + path + " between " +
                         InFirst.value("id", "<unknown>") + " and " + InSecond.value("id", "<unknown>");
        }
        return false;
    }
    return true;
}

auto ParseLineList(const std::string& InValue) -> std::vector<std::string> {
    std::vector<std::string> values;
    std::istringstream stream(InValue);
    std::string line;
    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (!line.empty()) {
            values.push_back(std::move(line));
        }
    }
    return values;
}

auto ChangedPaths(const std::filesystem::path& InRepo, std::string* OutError) -> std::optional<std::set<std::string>> {
    const auto tracked = GitCapture(InRepo, {"-c", "core.quotepath=false", "diff", "--name-only", "HEAD", "--"});
    if (tracked.exitCode != 0) {
        if (OutError != nullptr) {
            *OutError = Trim(tracked.stderrStr);
        }
        return std::nullopt;
    }
    const auto untracked = GitCapture(InRepo, {"-c", "core.quotepath=false", "ls-files", "--others", "--exclude-standard"});
    if (untracked.exitCode != 0) {
        if (OutError != nullptr) {
            *OutError = Trim(untracked.stderrStr);
        }
        return std::nullopt;
    }
    std::set<std::string> paths;
    const auto trackedPaths = ParseLineList(tracked.stdoutStr);
    const auto untrackedPaths = ParseLineList(untracked.stdoutStr);
    paths.insert(trackedPaths.begin(), trackedPaths.end());
    paths.insert(untrackedPaths.begin(), untrackedPaths.end());
    return paths;
}

auto ParseIndexEntries(const std::filesystem::path& InRepo, std::string* OutError)
    -> std::optional<std::map<std::string, std::string>> {
    const auto staged = GitCapture(InRepo, {"-c", "core.quotepath=false", "ls-files", "--stage"});
    const auto flags = GitCapture(InRepo, {"-c", "core.quotepath=false", "ls-files", "-v"});
    if (staged.exitCode != 0 || flags.exitCode != 0) {
        if (OutError != nullptr) {
            *OutError = Trim(staged.exitCode != 0 ? staged.stderrStr : flags.stderrStr);
        }
        return std::nullopt;
    }
    std::map<std::string, std::string> records;
    for (const auto& record : ParseLineList(staged.stdoutStr)) {
        const auto tab = record.find('\t');
        if (tab != std::string::npos) {
            records[record.substr(tab + 1)] = record.substr(0, tab);
        }
    }
    for (const auto& record : ParseLineList(flags.stdoutStr)) {
        if (record.size() >= 3 && record[1] == ' ') {
            records[record.substr(2)] += "|flag=" + record.substr(0, 1);
        }
    }
    return records;
}

auto FilterUnrelated(const std::map<std::string, std::string>& InEntries,
                     const std::set<std::string>& InSelected) -> std::map<std::string, std::string> {
    std::map<std::string, std::string> result;
    for (const auto& [path, entry] : InEntries) {
        if (!InSelected.contains(path)) {
            result[path] = entry;
        }
    }
    return result;
}

auto PrintError(const std::string& InBlocker, const std::string& InMessage, const int InCode = 2) -> int {
    Json output{
        {"ok", false},
        {"status", "blocked"},
        {"blocker", InBlocker},
        {"message", InMessage},
    };
    std::cerr << output.dump(2) << '\n';
    return InCode;
}

auto AcquireQueueLock(const QueueContext& InContext, std::string* OutError) -> std::optional<ScopedDirectoryLock> {
    if (OutError != nullptr) {
        OutError->clear();
    }
    ScopedDirectoryLock lock(InContext.root / "mutation.lock");
    if (!lock.owned) {
        if (OutError != nullptr && lock.error) {
            *OutError = "cannot create mutation lock directory: " + lock.error.message();
        }
        return std::nullopt;
    }
    return std::optional<ScopedDirectoryLock>(std::move(lock));
}

constexpr std::uintmax_t kCheckpointMaxBlobBytes = 32ULL * 1024ULL * 1024ULL;

auto IndexLockPath(const QueueContext& InContext, std::string* OutError) -> std::optional<std::filesystem::path> {
    const auto raw = GitValue(InContext.repo, {"rev-parse", "--git-path", "index.lock"}, OutError);
    if (!raw.has_value()) {
        return std::nullopt;
    }
    auto path = std::filesystem::path(*raw);
    if (path.is_relative()) {
        path = InContext.repo / path;
    }
    return path.lexically_normal();
}

auto IsSensitiveCheckpointPath(const std::string& InPath) -> bool {
    const auto lower = ToLower(InPath);
    const auto filename = std::filesystem::path(lower).filename().string();
    return filename == ".env" || filename.starts_with(".env.") || filename == "id_rsa" ||
           filename == "credentials.json" || lower.ends_with(".pem") || lower.ends_with(".key") ||
           lower.ends_with(".p12") || lower.ends_with(".pfx") ||
           lower.find("/secrets/") != std::string::npos ||
           lower.find("/credentials/") != std::string::npos;
}

auto HasSecret(const std::string& InBytes, const std::vector<SecretRule>& InRules) -> bool {
    std::istringstream lines(InBytes);
    std::string line;
    while (std::getline(lines, line)) {
        for (const auto& rule : InRules) {
            if (std::regex_search(line, rule.pattern) && !secret_scan::ShouldIgnoreSecretFinding(rule.id, line)) {
                return true;
            }
        }
    }
    return false;
}

auto ReadCheckpointBytes(const std::filesystem::path& InPath, std::string* OutError) -> std::optional<std::string> {
    std::error_code ec;
    const auto size = std::filesystem::file_size(InPath, ec);
    if (ec || size > kCheckpointMaxBlobBytes) {
        if (OutError != nullptr) {
            *OutError = ec ? "cannot inspect checkpoint file: " + ec.message() : "checkpoint file exceeds the 32 MiB bound";
        }
        return std::nullopt;
    }
    std::ifstream stream(InPath, std::ios::binary);
    if (!stream) {
        if (OutError != nullptr) *OutError = "cannot read checkpoint file";
        return std::nullopt;
    }
    std::string bytes(std::istreambuf_iterator<char>{stream}, std::istreambuf_iterator<char>{});
    if (stream.bad() || bytes.size() != size) {
        if (OutError != nullptr) *OutError = "checkpoint file changed while reading";
        return std::nullopt;
    }
    return bytes;
}

auto GitBlobBytes(const std::filesystem::path& InRepo, const std::string& InOid, std::string* OutError)
    -> std::optional<std::string> {
    const auto blob = GitCapture(InRepo, {"cat-file", "blob", InOid});
    if (blob.exitCode != 0 || blob.stdoutStr.size() > kCheckpointMaxBlobBytes) {
        if (OutError != nullptr) *OutError = "checkpoint blob is missing or exceeds the 32 MiB bound";
        return std::nullopt;
    }
    return blob.stdoutStr;
}

auto GitPathEntry(const std::filesystem::path& InRepo,
                  const std::string& InPath,
                  const bool InIndex,
                  std::string* OutError,
                  const std::string& InTreeish = "HEAD") -> std::optional<Json> {
    const auto result = InIndex
        ? GitCapture(InRepo, {"ls-files", "--stage", "-z", "--", InPath})
        : GitCapture(InRepo, {"ls-tree", "-z", InTreeish, "--", InPath});
    if (result.exitCode != 0) {
        if (OutError != nullptr) *OutError = "cannot inspect checkpoint Git entry";
        return std::nullopt;
    }
    if (result.stdoutStr.empty()) {
        return Json{{"exists", false}};
    }
    const auto nul = result.stdoutStr.find('\0');
    const auto tab = result.stdoutStr.find('\t');
    if (nul == std::string::npos || nul + 1 != result.stdoutStr.size() ||
        tab == std::string::npos || tab > nul || result.stdoutStr.substr(tab + 1, nul - tab - 1) != InPath) {
        if (OutError != nullptr) *OutError = "checkpoint selector does not identify one exact Git entry";
        return std::nullopt;
    }
    std::istringstream prefix(result.stdoutStr.substr(0, tab));
    std::string mode, kind, oid, stage;
    prefix >> mode;
    if (InIndex) {
        prefix >> oid >> stage;
    } else {
        prefix >> kind >> oid;
    }
    if ((mode != "100644" && mode != "100755") || oid.empty() ||
        (InIndex ? stage != "0" : kind != "blob")) {
        if (OutError != nullptr) *OutError = "checkpoint supports only stage-0 regular-file blobs";
        return std::nullopt;
    }
    return Json{{"exists", true}, {"mode", mode}, {"oid", oid}};
}

auto MatchesVersion(const Json& InActual, const Json& InExpected) -> bool {
    if (InActual.value("exists", false) != InExpected.value("exists", false)) return false;
    return !InActual.value("exists", false) ||
           (InActual.value("mode", "") == InExpected.value("mode", "") &&
            InActual.value("oid", "") == InExpected.value("oid", ""));
}

auto ExactCommitReadback(const std::filesystem::path& InRepo,
                         const std::string& InCommit,
                         const std::string& InParent,
                         const std::string& InTree,
                         const std::vector<std::string>& InPaths,
                         const std::vector<Json>& InVersions,
                         std::string* OutError) -> bool {
    const auto parents = GitValue(InRepo, {"rev-list", "--parents", "-n", "1", InCommit}, OutError);
    const auto tree = GitValue(InRepo, {"rev-parse", InCommit + "^{tree}"}, OutError);
    if (parents != std::optional<std::string>{InCommit + " " + InParent} ||
        tree != std::optional<std::string>{InTree}) {
        if (OutError != nullptr) *OutError = "commit parent or complete tree differs from the prepared index";
        return false;
    }
    for (std::size_t index = 0; index < InPaths.size(); ++index) {
        const auto entry = GitPathEntry(InRepo, InPaths[index], false, OutError, InCommit);
        if (!entry.has_value() || !MatchesVersion(*entry, InVersions[index])) {
            if (OutError != nullptr) *OutError = "commit path presence, mode, or blob differs from the prepared version";
            return false;
        }
    }
    return true;
}

auto WorkingMode(const std::filesystem::path& InPath, const Json& InStaged, const Json& InHead) -> std::string {
#if defined(_WIN32)
    (void)InPath;
    if (InStaged.value("exists", false)) return InStaged.value("mode", "100644");
    return InHead.value("mode", "100644");
#else
    std::error_code ec;
    const auto permissions = std::filesystem::status(InPath, ec).permissions();
    if (ec) return InStaged.value("mode", InHead.value("mode", "100644"));
    return (permissions & std::filesystem::perms::owner_exec) != std::filesystem::perms::none ? "100755" : "100644";
#endif
}

auto WorkingEntry(const std::filesystem::path& InRepo,
                  const std::string& InPath,
                  const Json& InStaged,
                  const Json& InHead,
                  const std::vector<SecretRule>& InRules,
                  const bool InWriteBlob,
                  std::string* OutError) -> std::optional<Json> {
    const auto full = InRepo / InPath;
    std::error_code ec;
    if (!std::filesystem::exists(full, ec)) {
        if (ec) {
            if (OutError != nullptr) *OutError = "cannot inspect checkpoint working file";
            return std::nullopt;
        }
        return Json{{"exists", false}};
    }
    if (std::filesystem::is_symlink(full, ec) || !std::filesystem::is_regular_file(full, ec)) {
        if (OutError != nullptr) *OutError = "checkpoint supports only regular working files";
        return std::nullopt;
    }
    const auto bytes = ReadCheckpointBytes(full, OutError);
    if (!bytes.has_value()) return std::nullopt;
    if (HasSecret(*bytes, InRules)) {
        if (OutError != nullptr) *OutError = "secret_detected";
        return std::nullopt;
    }
    struct TemporaryBlobInput {
        std::filesystem::path path;
        ~TemporaryBlobInput() {
            if (!path.empty()) {
                std::error_code ignored;
                std::filesystem::remove(path, ignored);
            }
        }
    } safeInput;
    auto hashPath = full;
    if (InWriteBlob) {
        const auto tempRoot = std::filesystem::temp_directory_path(ec);
        if (ec) {
            if (OutError != nullptr) *OutError = "cannot allocate safe checkpoint blob input";
            return std::nullopt;
        }
        safeInput.path = tempRoot / ("kog-checkpoint-" + TimestampId() + ".blob");
        std::ofstream stream(safeInput.path, std::ios::binary | std::ios::trunc);
        stream.write(bytes->data(), static_cast<std::streamsize>(bytes->size()));
        stream.flush();
        if (!stream) {
            if (OutError != nullptr) *OutError = "cannot write safe checkpoint blob input";
            return std::nullopt;
        }
        stream.close();
        hashPath = safeInput.path;
    }
    auto hashArgs = std::vector<std::string>{"hash-object", "--no-filters"};
    if (InWriteBlob) hashArgs.push_back("-w");
    hashArgs.insert(hashArgs.end(), {"--", hashPath.string()});
    const auto hash = GitValue(InRepo, hashArgs, OutError);
    if (!hash.has_value()) return std::nullopt;
    const auto after = ReadCheckpointBytes(full, OutError);
    if (!after.has_value() || *after != *bytes) {
        if (OutError != nullptr) *OutError = "checkpoint working file changed during capture";
        return std::nullopt;
    }
    if (InWriteBlob) {
        const auto restored = GitBlobBytes(InRepo, *hash, OutError);
        if (!restored.has_value() || *restored != *bytes) {
            if (OutError != nullptr) *OutError = "checkpoint raw-byte restore verification failed";
            return std::nullopt;
        }
    }
    return Json{{"exists", true}, {"mode", WorkingMode(full, InStaged, InHead)},
                {"oid", *hash}, {"rawBytes", bytes->size()}};
}

auto CheckpointManifestPath(const QueueContext& InContext, const std::string& InId) -> std::filesystem::path {
    return InContext.root / "checkpoints" / InId / "manifest.json";
}

auto WriteCheckpointManifest(const std::filesystem::path& InPath, const Json& InManifest, std::string* OutError) -> bool {
    std::error_code ec;
    std::filesystem::create_directories(InPath.parent_path().parent_path(), ec);
    if (ec || !std::filesystem::create_directory(InPath.parent_path(), ec)) {
        if (OutError != nullptr) *OutError = "checkpoint id already exists or cannot be reserved";
        return false;
    }
    const auto temporary = InPath.parent_path() / "manifest.tmp";
    {
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        stream << InManifest.dump(2) << '\n';
        stream.flush();
        if (!stream) {
            if (OutError != nullptr) *OutError = "cannot persist checkpoint manifest";
            return false;
        }
    }
    return ReplaceFileAtomically(temporary, InPath, OutError);
}

auto ReadCheckpointManifest(const std::filesystem::path& InPath, std::string* OutError) -> std::optional<Json> {
    try {
        std::ifstream stream(InPath, std::ios::binary);
        if (!stream) throw std::runtime_error("manifest missing");
        auto manifest = Json::parse(stream);
        if (manifest.value("schema", "") != "kog-cooperative-checkpoint-v1" ||
            !manifest.contains("paths") || !manifest["paths"].is_array()) {
            throw std::runtime_error("unsupported checkpoint schema");
        }
        return manifest;
    } catch (const std::exception&) {
        if (OutError != nullptr) *OutError = "checkpoint manifest missing or invalid";
        return std::nullopt;
    }
}

auto RunAdmit(const std::filesystem::path& InRepo,
              const std::string& InId,
              const std::string& InWorkItem,
              const std::string& InAgent,
              const std::vector<std::string>& InFiles,
              const std::vector<std::string>& InChunks,
              const std::vector<std::string>& InPostconditions,
              const std::vector<std::string>& InValidation) -> int {
    std::string error;
    const auto context = ResolveQueueContext(InRepo, &error);
    if (!context.has_value()) {
        return PrintError("not_a_repository", error);
    }
    if (Trim(InWorkItem).empty() || Trim(InAgent).empty()) {
        return PrintError("invalid_admission", "--work-item and --agent are required");
    }
    const auto id = InId.empty() ? TimestampId() : InId;
    if (!IsSafeId(id)) {
        return PrintError("invalid_admission", "--id may contain only letters, digits, dot, underscore, and dash");
    }
    const auto files = NormalizePaths(context->repo, InFiles, false, &error);
    if (!files.has_value() || files->empty()) {
        return PrintError("invalid_admission", error.empty() ? "at least one --file is required" : error);
    }
    const std::set<std::string> fileSet(files->begin(), files->end());
    Json chunks = Json::object();
    for (const auto& raw : InChunks) {
        std::string pathInput;
        long long start = 0;
        long long end = 0;
        if (!ParseChunk(raw, &pathInput, &start, &end)) {
            return PrintError("invalid_admission", "invalid --chunk selector: " + raw);
        }
        const auto path = NormalizeExactPath(context->repo, pathInput, &error);
        if (!path.has_value() || !fileSet.contains(*path)) {
            return PrintError("invalid_admission", error.empty() ? "chunk path is not declared by --file: " + pathInput : error);
        }
        chunks[*path].push_back(Json{{"start", start}, {"end", end}});
    }
    Json postconditions = Json::object();
    for (const auto& raw : InPostconditions) {
        std::string pathInput;
        std::string value;
        if (!ParseKeyValue(raw, &pathInput, &value)) {
            return PrintError("invalid_admission", "invalid --postcondition selector: " + raw);
        }
        const auto path = NormalizeExactPath(context->repo, pathInput, &error);
        if (!path.has_value() || !fileSet.contains(*path)) {
            return PrintError("invalid_admission", error.empty() ? "postcondition path is not declared by --file: " + pathInput : error);
        }
        postconditions[*path] = value;
    }
    auto lock = AcquireQueueLock(*context, &error);
    if (!lock.has_value()) {
        if (!error.empty()) {
            return PrintError("queue_lock_failed", error, 1);
        }
        return PrintError("queue_locked", "another KOG queue mutation is active", 1);
    }
    auto state = LoadState(*context, &error);
    if (!state.has_value()) {
        return PrintError("queue_state_invalid", error);
    }
    for (const auto& item : (*state)["pending"]) {
        if (item.value("id", "") == id) {
            return PrintError("duplicate_admission", "queue item already exists: " + id);
        }
    }
    if (!(*state)["active"].is_null() && (*state)["active"].value("id", "") == id) {
        return PrintError("duplicate_admission", "queue batch already exists: " + id);
    }
    const auto head = GitValue(context->repo, {"rev-parse", "HEAD"}, &error);
    if (!head.has_value()) {
        return PrintError("head_unavailable", error);
    }
    Json item{
        {"id", id},
        {"repo", context->repo.generic_string()},
        {"baseHead", *head},
        {"workItem", Trim(InWorkItem)},
        {"agent", Trim(InAgent)},
        {"files", *files},
        {"chunks", chunks},
        {"postconditions", postconditions},
        {"validation", InValidation},
        {"admittedAt", TimestampId()},
    };
    (*state)["pending"].push_back(item);
    if (!SaveState(*context, *state, &error)) {
        return PrintError("queue_state_write_failed", error);
    }
    std::cout << Json{{"ok", true}, {"status", "admitted"}, {"item", item}}.dump(2) << '\n';
    return 0;
}

auto RunStatus(const std::filesystem::path& InRepo) -> int {
    std::string error;
    const auto context = ResolveQueueContext(InRepo, &error);
    if (!context.has_value()) {
        return PrintError("not_a_repository", error);
    }
    const auto state = LoadState(*context, &error);
    if (!state.has_value()) {
        return PrintError("queue_state_invalid", error);
    }
    Json output = *state;
    output["ok"] = true;
    output["repo"] = context->repo.generic_string();
    output["pendingCount"] = (*state)["pending"].size();
    output["activeCount"] = (*state)["active"].is_null() ? 0 : 1;
    std::cout << output.dump(2) << '\n';
    return 0;
}

auto BuildBatch(const Json& InPending, const std::string& InHead, std::string* OutError) -> std::optional<Json> {
    for (std::size_t first = 0; first < InPending.size(); ++first) {
        if (InPending[first].value("baseHead", "") != InHead) {
            if (OutError != nullptr) {
                *OutError = "stale base HEAD for " + InPending[first].value("id", "<unknown>");
            }
            return std::nullopt;
        }
        for (std::size_t second = first + 1; second < InPending.size(); ++second) {
            if (!Compatible(InPending[first], InPending[second], OutError)) {
                return std::nullopt;
            }
        }
    }
    std::set<std::string> files;
    std::set<std::string> validation;
    Json itemIds = Json::array();
    Json workItems = Json::array();
    Json agents = Json::array();
    for (const auto& item : InPending) {
        itemIds.push_back(item.value("id", ""));
        workItems.push_back(item.value("workItem", ""));
        agents.push_back(item.value("agent", ""));
        const auto itemFiles = JsonStringSet(item.value("files", Json::array()));
        files.insert(itemFiles.begin(), itemFiles.end());
        const auto itemValidation = JsonStringSet(item.value("validation", Json::array()));
        validation.insert(itemValidation.begin(), itemValidation.end());
    }
    return Json{
        {"id", "batch-" + TimestampId()},
        {"baseHead", InHead},
        {"itemIds", itemIds},
        {"workItems", workItems},
        {"agents", agents},
        {"files", std::vector<std::string>(files.begin(), files.end())},
        {"validation", std::vector<std::string>(validation.begin(), validation.end())},
        {"status", "planned"},
    };
}

auto RunDrain(const std::filesystem::path& InRepo, const bool InConfirm) -> int {
    std::string error;
    const auto context = ResolveQueueContext(InRepo, &error);
    if (!context.has_value()) {
        return PrintError("not_a_repository", error);
    }
    auto lock = AcquireQueueLock(*context, &error);
    if (!lock.has_value()) {
        if (!error.empty()) {
            return PrintError("queue_lock_failed", error, 1);
        }
        return PrintError("queue_locked", "another KOG queue mutation is active", 1);
    }
    auto state = LoadState(*context, &error);
    if (!state.has_value()) {
        return PrintError("queue_state_invalid", error);
    }
    if (!(*state)["active"].is_null()) {
        return PrintError("active_batch", "complete the active batch before draining more work");
    }
    if ((*state)["pending"].empty()) {
        std::cout << Json{{"ok", true}, {"status", "empty"}, {"pendingCount", 0}}.dump(2) << '\n';
        return 0;
    }
    const auto head = GitValue(context->repo, {"rev-parse", "HEAD"}, &error);
    if (!head.has_value()) {
        return PrintError("head_unavailable", error);
    }
    auto batch = BuildBatch((*state)["pending"], *head, &error);
    if (!batch.has_value()) {
        return PrintError(error.starts_with("stale base HEAD") ? "stale_base_head" : "queue_conflict",
                          error + "; replan the item or use an isolated worktree");
    }
    if (!InConfirm) {
        (*batch)["status"] = "preview";
        std::cout << Json{{"ok", true}, {"status", "preview"}, {"batch", *batch}}.dump(2) << '\n';
        return 0;
    }
    (*batch)["status"] = "active";
    (*batch)["startedAt"] = TimestampId();
    (*batch)["items"] = (*state)["pending"];
    (*state)["active"] = *batch;
    (*state)["pending"] = Json::array();
    if (!SaveState(*context, *state, &error)) {
        return PrintError("queue_state_write_failed", error);
    }
    std::cout << Json{{"ok", true}, {"status", "active"}, {"batch", *batch}}.dump(2) << '\n';
    return 0;
}

auto RunComplete(const std::filesystem::path& InRepo,
                 const std::string& InBatch,
                 const std::string& InStatus) -> int {
    std::string error;
    const auto context = ResolveQueueContext(InRepo, &error);
    if (!context.has_value()) {
        return PrintError("not_a_repository", error);
    }
    if (!IsSafeId(InBatch) || (InStatus != "succeeded" && InStatus != "failed" && InStatus != "cancelled")) {
        return PrintError("invalid_completion", "--batch and --status succeeded|failed|cancelled are required");
    }
    auto lock = AcquireQueueLock(*context, &error);
    if (!lock.has_value()) {
        if (!error.empty()) {
            return PrintError("queue_lock_failed", error, 1);
        }
        return PrintError("queue_locked", "another KOG queue mutation is active", 1);
    }
    auto state = LoadState(*context, &error);
    if (!state.has_value()) {
        return PrintError("queue_state_invalid", error);
    }
    if ((*state)["active"].is_null() || (*state)["active"].value("id", "") != InBatch) {
        return PrintError("batch_mismatch", "requested batch is not active");
    }
    auto receipt = (*state)["active"];
    receipt["status"] = InStatus;
    receipt["completedAt"] = TimestampId();
    (*state)["receipts"].push_back(receipt);
    if ((*state)["receipts"].size() > 100) {
        (*state)["receipts"].erase((*state)["receipts"].begin());
    }
    (*state)["active"] = nullptr;
    if (!SaveState(*context, *state, &error)) {
        return PrintError("queue_state_write_failed", error);
    }
    std::cout << Json{{"ok", true}, {"status", "completed"}, {"receipt", receipt}}.dump(2) << '\n';
    return 0;
}

auto RunCheckpointCapture(const std::filesystem::path& InRepo,
                          const std::string& InId,
                          const std::vector<std::string>& InPaths,
                          const std::string& InSource,
                          const std::string& InWorkItem,
                          const bool InOwnerStable) -> int {
    std::string error;
    const auto context = ResolveQueueContext(InRepo, &error);
    if (!context.has_value()) return PrintError("not_a_repository", error);
    if (!IsSafeId(InId) || Trim(InSource).empty() || InSource.size() > 128 || Trim(InWorkItem).empty() ||
        !std::regex_match(InSource, std::regex(R"([A-Za-z0-9._/-]+)")) ||
        !std::regex_match(InWorkItem, std::regex(R"([A-Za-z0-9._-]+)"))) {
        return PrintError("invalid_checkpoint", "safe --id, nonempty --source and --work-item are required");
    }
    if (!InOwnerStable) {
        return PrintError("owner_not_stable", "capture requires a verified stable owner and --owner-stable");
    }
    const auto paths = NormalizePaths(context->repo, InPaths, true, &error);
    if (!paths.has_value() || paths->empty()) {
        return PrintError("invalid_exact_path", error.empty() ? "at least one --path is required" : error);
    }
    for (const auto& path : *paths) {
        if (path.find_first_of("\r\n\t") != std::string::npos || IsSensitiveCheckpointPath(path)) {
            return PrintError("excluded_checkpoint_path", "checkpoint path is unsafe or excluded: " + path);
        }
    }
    auto lock = AcquireQueueLock(*context, &error);
    if (!lock.has_value()) return PrintError(error.empty() ? "queue_locked" : "queue_lock_failed", error, 1);
    const auto state = LoadState(*context, &error);
    if (!state.has_value()) return PrintError("queue_state_invalid", error);
    if (!(*state)["active"].is_null()) return PrintError("active_batch", "an active mutation batch owns this repository");
    const auto indexLock = IndexLockPath(*context, &error);
    if (!indexLock.has_value()) return PrintError("index_path_unavailable", error);
    std::error_code ec;
    if (std::filesystem::exists(*indexLock, ec)) {
        return PrintError("git_index_lock", "Git index lock exists; KOG will not delete it", 1);
    }
    const auto head = GitValue(context->repo, {"rev-parse", "HEAD"}, &error);
    const auto branchRef = GitValue(context->repo, {"symbolic-ref", "-q", "HEAD"}, &error);
    if (!head.has_value() || !branchRef.has_value()) {
        return PrintError("checkpoint_base_unavailable", "capture requires an attached branch and readable HEAD");
    }
    const auto unmerged = GitCapture(context->repo, {"ls-files", "--unmerged"});
    if (unmerged.exitCode != 0 || !unmerged.stdoutStr.empty()) {
        return PrintError("unmerged_index", "checkpoint capture requires a resolved index");
    }
    const auto originalIndex = ParseIndexEntries(context->repo, &error);
    if (!originalIndex.has_value()) return PrintError("index_read_failed", error);
    const auto rules = LoadSecretRules(DefaultSecretRulesPath(context->repo), &error);
    if (!error.empty() || rules.empty()) return PrintError("secret_rules_unavailable", "checkpoint secret rules are unavailable");
    if (HasSecret(InSource, rules) || HasSecret(InWorkItem, rules)) {
        return PrintError("secret_detected", "checkpoint provenance matched a secret rule");
    }

    Json entries = Json::array();
    for (const auto& path : *paths) {
        const auto ignored = GitCapture(context->repo, {"check-ignore", "--no-index", "-q", "--", path});
        if (ignored.exitCode == 0) return PrintError("excluded_checkpoint_path", "ignored paths cannot be archived: " + path);
        if (ignored.exitCode != 1) return PrintError("ignore_check_failed", "cannot verify checkpoint exclusion rules");
        const auto base = GitPathEntry(context->repo, path, false, &error);
        const auto staged = GitPathEntry(context->repo, path, true, &error);
        if (!base.has_value() || !staged.has_value()) return PrintError("checkpoint_entry_failed", error);
        const auto working = WorkingEntry(context->repo, path, *staged, *base, rules, false, &error);
        if (!working.has_value()) {
            return PrintError(error == "secret_detected" ? "secret_detected" : "checkpoint_read_failed",
                              error == "secret_detected" ? "checkpoint content matched a secret rule" : error);
        }
        if (staged->value("exists", false)) {
            const auto bytes = GitBlobBytes(context->repo, staged->value("oid", ""), &error);
            if (!bytes.has_value()) return PrintError("checkpoint_restore_failed", error);
            if (HasSecret(*bytes, rules)) return PrintError("secret_detected", "checkpoint content matched a secret rule");
        }
        entries.push_back(Json{{"path", path}, {"head", *base}, {"staged", *staged}, {"working", *working},
                               {"indexRecord", originalIndex->contains(path) ? Json(originalIndex->at(path)) : Json(nullptr)}});
    }

    if (GitValue(context->repo, {"rev-parse", "HEAD"}, &error) != head ||
        GitValue(context->repo, {"symbolic-ref", "-q", "HEAD"}, &error) != branchRef ||
        ParseIndexEntries(context->repo, &error) != originalIndex ||
        std::filesystem::exists(*indexLock, ec)) {
        return PrintError("checkpoint_snapshot_drift", "HEAD, branch, index, or index lock changed during capture");
    }
    for (const auto& entry : entries) {
        const auto current = WorkingEntry(context->repo, entry.value("path", ""), entry["staged"], entry["head"], rules, false, &error);
        if (!current.has_value() || *current != entry["working"]) {
            return PrintError("checkpoint_snapshot_drift", "working file changed during capture");
        }
    }
    for (const auto& entry : entries) {
        const auto stored = WorkingEntry(context->repo, entry.value("path", ""), entry["staged"], entry["head"], rules, true, &error);
        if (!stored.has_value() || *stored != entry["working"]) {
            return PrintError("checkpoint_snapshot_drift", "working file changed before raw-blob storage");
        }
    }
    const Json manifest{{"schema", "kog-cooperative-checkpoint-v1"},
                        {"id", InId}, {"repo", context->repo.generic_string()},
                        {"baseHead", *head}, {"branchRef", *branchRef},
                        {"source", Trim(InSource)}, {"workItem", Trim(InWorkItem)},
                        {"ownerStable", true}, {"capturedAt", TimestampId()}, {"paths", entries},
                        {"stagingIntent", "staged and working versions retained separately"}};
    const auto manifestPath = CheckpointManifestPath(*context, InId);
    if (!WriteCheckpointManifest(manifestPath, manifest, &error)) {
        return PrintError("checkpoint_write_failed", error);
    }
    const auto readback = ReadCheckpointManifest(manifestPath, &error);
    if (!readback.has_value() || *readback != manifest) {
        return PrintError("checkpoint_restore_failed", "saved manifest did not read back byte-identically");
    }
    std::cout << Json{{"ok", true}, {"status", "saved"}, {"checkpoint", manifest},
                      {"manifest", manifestPath.generic_string()}, {"restoreVerified", true},
                      {"saved", true}, {"tested", false}, {"integrated", false},
                      {"published", false}, {"consumed", false}}.dump(2) << '\n';
    return 0;
}

struct PreparedOverlap {
    std::string path;
    Json head;
    Json staged;
    Json working;
    Json finalWorking;
    std::string ownBytes;
    std::string rebasedStagedBytes;
    std::string ownOid;
    std::string rebasedStagedOid;
};

struct ScopedCheckpointFiles {
    std::vector<std::filesystem::path> paths;
    ~ScopedCheckpointFiles() {
        for (const auto& path : paths) {
            std::error_code ec;
            std::filesystem::remove(path, ec);
        }
    }
};

auto WriteCheckpointBytes(const std::filesystem::path& InPath, const std::string& InBytes) -> bool {
    std::error_code ec;
    std::filesystem::create_directories(InPath.parent_path(), ec);
    if (ec) return false;
    std::ofstream stream(InPath, std::ios::binary | std::ios::trunc);
    if (!stream) return false;
    stream.write(InBytes.data(), static_cast<std::streamsize>(InBytes.size()));
    stream.flush();
    return static_cast<bool>(stream);
}

auto MergeCheckpointText(const std::filesystem::path& InRepo,
                         const std::filesystem::path& InCurrent,
                         const std::filesystem::path& InBase,
                         const std::filesystem::path& InOther,
                         std::string* OutError) -> std::optional<std::string> {
    const auto merge = GitCapture(InRepo, {"merge-file", "-p", InCurrent.string(), InBase.string(), InOther.string()});
    if (merge.exitCode != 0) {
        if (OutError != nullptr) *OutError = merge.exitCode == 1 ? "inseparable_overlap" : "merge_proof_failed";
        return std::nullopt;
    }
    return merge.stdoutStr;
}

auto IsBinaryCheckpointOverlap(const std::string& InPath, const std::string& InBytes) -> bool {
    const auto lower = ToLower(InPath);
    return InBytes.find('\0') != std::string::npos || lower.ends_with(".uasset") || lower.ends_with(".umap") ||
           lower.ends_with(".png") || lower.ends_with(".jpg") || lower.ends_with(".jpeg") ||
           lower.ends_with(".zip") || lower.ends_with(".pdf");
}

auto PrepareOwnOverlap(const QueueContext& InContext,
                       const Json& InManifest,
                       const std::vector<SecretRule>& InRules,
                       std::string* OutBlocker,
                       std::string* OutError) -> std::optional<std::vector<PreparedOverlap>> {
    std::vector<PreparedOverlap> prepared;
    ScopedCheckpointFiles temporary;
    const auto tempRoot = InContext.root / "tmp";
    std::error_code ec;
    std::filesystem::create_directories(tempRoot, ec);
    if (ec) {
        if (OutBlocker != nullptr) *OutBlocker = "checkpoint_temp_failed";
        return std::nullopt;
    }
    bool anyOwnChange = false;
    for (std::size_t index = 0; index < InManifest["paths"].size(); ++index) {
        const auto& entry = InManifest["paths"][index];
        PreparedOverlap path;
        path.path = entry.value("path", "");
        path.head = entry.value("head", Json::object());
        path.staged = entry.value("staged", Json::object());
        path.working = entry.value("working", Json::object());
        if (!path.head.value("exists", false) || !path.staged.value("exists", false) ||
            !path.working.value("exists", false)) {
            if (OutBlocker != nullptr) *OutBlocker = "unsupported_overlap_shape";
            if (OutError != nullptr) *OutError = "own-only recovery currently requires an existing regular text file in all three captured versions";
            return std::nullopt;
        }
        const auto base = GitBlobBytes(InContext.repo, path.head.value("oid", ""), OutError);
        const auto staged = GitBlobBytes(InContext.repo, path.staged.value("oid", ""), OutError);
        const auto before = GitBlobBytes(InContext.repo, path.working.value("oid", ""), OutError);
        if (!base.has_value() || !staged.has_value() || !before.has_value()) {
            if (OutBlocker != nullptr) *OutBlocker = "checkpoint_restore_failed";
            return std::nullopt;
        }
        const auto finalEntry = WorkingEntry(InContext.repo, path.path, path.staged, path.head, InRules, false, OutError);
        if (!finalEntry.has_value()) {
            if (OutBlocker != nullptr) *OutBlocker = OutError != nullptr && *OutError == "secret_detected"
                ? "secret_detected" : "checkpoint_read_failed";
            return std::nullopt;
        }
        path.finalWorking = *finalEntry;
        if (!path.finalWorking.value("exists", false)) {
            if (OutBlocker != nullptr) *OutBlocker = "unsupported_overlap_shape";
            return std::nullopt;
        }
        const auto final = ReadCheckpointBytes(InContext.repo / path.path, OutError);
        if (!final.has_value()) {
            if (OutBlocker != nullptr) *OutBlocker = "checkpoint_read_failed";
            return std::nullopt;
        }
        if (HasSecret(*final, InRules) || HasSecret(*staged, InRules) || HasSecret(*before, InRules)) {
            if (OutBlocker != nullptr) *OutBlocker = "secret_detected";
            if (OutError != nullptr) *OutError = "checkpoint content matched a secret rule";
            return std::nullopt;
        }
        if (IsBinaryCheckpointOverlap(path.path, *base) || IsBinaryCheckpointOverlap(path.path, *staged) ||
            IsBinaryCheckpointOverlap(path.path, *before) || IsBinaryCheckpointOverlap(path.path, *final)) {
            if (OutBlocker != nullptr) *OutBlocker = "binary_overlap";
            if (OutError != nullptr) *OutError = "binary overlap remains saved whole; no hunk split is attempted";
            return std::nullopt;
        }
        if (path.finalWorking.value("mode", "") != path.working.value("mode", "")) {
            if (OutBlocker != nullptr) *OutBlocker = "mode_overlap";
            if (OutError != nullptr) *OutError = "concurrent mode and text changes cannot be separated";
            return std::nullopt;
        }
        const auto suffix = InManifest.value("id", "") + "-" + std::to_string(index) + "-" + TimestampId();
        const auto baseFile = tempRoot / (suffix + ".base");
        const auto stagedFile = tempRoot / (suffix + ".stage");
        const auto beforeFile = tempRoot / (suffix + ".before");
        const auto finalFile = tempRoot / (suffix + ".final");
        const auto ownFile = tempRoot / (suffix + ".own");
        temporary.paths.insert(temporary.paths.end(), {baseFile, stagedFile, beforeFile, finalFile, ownFile});
        if (!WriteCheckpointBytes(baseFile, *base) || !WriteCheckpointBytes(stagedFile, *staged) ||
            !WriteCheckpointBytes(beforeFile, *before) || !WriteCheckpointBytes(finalFile, *final)) {
            if (OutBlocker != nullptr) *OutBlocker = "checkpoint_temp_failed";
            return std::nullopt;
        }
        auto own = MergeCheckpointText(InContext.repo, finalFile, beforeFile, baseFile, OutBlocker);
        if (!own.has_value()) return std::nullopt;
        if (!WriteCheckpointBytes(ownFile, *own)) {
            if (OutBlocker != nullptr) *OutBlocker = "checkpoint_temp_failed";
            return std::nullopt;
        }
        const auto replay = MergeCheckpointText(InContext.repo, beforeFile, baseFile, ownFile, OutBlocker);
        if (!replay.has_value() || *replay != *final) {
            if (OutBlocker != nullptr) *OutBlocker = "inseparable_overlap";
            if (OutError != nullptr) *OutError = "own and pre-existing text could not be replayed byte-identically";
            return std::nullopt;
        }
        const auto restaged = MergeCheckpointText(InContext.repo, stagedFile, baseFile, ownFile, OutBlocker);
        if (!restaged.has_value()) return std::nullopt;
        path.ownBytes = std::move(*own);
        path.rebasedStagedBytes = std::move(*restaged);
        anyOwnChange |= path.ownBytes != *base;
        prepared.push_back(std::move(path));
    }
    if (!anyOwnChange) {
        if (OutBlocker != nullptr) *OutBlocker = "no_selected_changes";
        if (OutError != nullptr) *OutError = "caller made no separable own change after checkpoint capture";
        return std::nullopt;
    }
    return prepared;
}

auto InstallCheckpointVersion(const std::filesystem::path& InCheckout,
                              const std::string& InPath,
                              const Json& InVersion,
                              const std::filesystem::path& InObjectRepo,
                              std::string* OutError) -> bool {
    const auto target = InCheckout / InPath;
    if (!InVersion.value("exists", false)) {
        std::error_code ec;
        std::filesystem::remove(target, ec);
        if (ec) {
            if (OutError != nullptr) *OutError = "cannot remove selected checkpoint file";
            return false;
        }
        const auto remove = GitCapture(InCheckout, {"update-index", "--force-remove", "--", InPath});
        if (remove.exitCode != 0) {
            if (OutError != nullptr) *OutError = "cannot stage checkpoint deletion";
            return false;
        }
        return true;
    }
    const auto bytes = GitBlobBytes(InObjectRepo, InVersion.value("oid", ""), OutError);
    if (!bytes.has_value() || !WriteCheckpointBytes(target, *bytes)) {
        if (OutError != nullptr) *OutError = "cannot restore selected checkpoint blob";
        return false;
    }
    const auto mode = InVersion.value("mode", "");
    const auto oid = InVersion.value("oid", "");
#if !defined(_WIN32)
    std::error_code ec;
    if (mode == "100755") {
        std::filesystem::permissions(target, std::filesystem::perms::owner_exec,
                                     std::filesystem::perm_options::add, ec);
    } else {
        std::filesystem::permissions(target, std::filesystem::perms::owner_exec,
                                     std::filesystem::perm_options::remove, ec);
    }
    if (ec) {
        if (OutError != nullptr) *OutError = "cannot restore checkpoint executable mode";
        return false;
    }
#endif
    const auto stage = GitCapture(InCheckout, {"update-index", "--add", "--cacheinfo", mode + "," + oid + "," + InPath});
    if (stage.exitCode != 0) {
        if (OutError != nullptr) *OutError = "cannot stage selected checkpoint blob";
        return false;
    }
    return true;
}

auto CheckpointError(const std::string& InBlocker,
                     const std::string& InMessage,
                     const std::string& InRef,
                     const Json& InCommits,
                     const std::string& InOwnCommit = {}) -> int {
    std::cerr << Json{{"ok", false}, {"status", "blocked"}, {"blocker", InBlocker},
                      {"message", InMessage}, {"saved", true}, {"tested", false},
                      {"integrated", false}, {"published", false}, {"consumed", false},
                      {"checkpointRef", InRef.empty() ? Json(nullptr) : Json(InRef)},
                      {"checkpointCommits", InCommits},
                      {"ownCommit", InOwnCommit.empty() ? Json(nullptr) : Json(InOwnCommit)}}.dump(2) << '\n';
    return 2;
}

auto CheckpointRecoveryRequired(const QueueContext& InContext,
                                const std::string& InId,
                                const std::string& InReason,
                                const std::string& InBaseHead,
                                const std::string& InBranchRef,
                                const std::string& InOwnCommit,
                                const std::string& InCheckpointRef,
                                const Json& InCheckpointCommits) -> int {
    const auto recoveryRef = "refs/kog/recovery/" + InId;
    const auto zero = std::string(InOwnCommit.size(), '0');
    const auto pinned = !InOwnCommit.empty() &&
        GitCapture(InContext.repo, {"update-ref", recoveryRef, InOwnCommit, zero}).exitCode == 0;
    std::cerr << Json{{"ok", false}, {"status", "recovery_required"}, {"blocker", InReason},
                      {"message", InOwnCommit.empty()
                          ? "commit outcome is unknown; inspect branch and index manually before retry"
                          : "own commit exists; inspect receipt and reconcile index manually before retry"},
                      {"baseHead", InBaseHead}, {"branchRef", InBranchRef},
                      {"ownCommit", InOwnCommit.empty() ? Json(nullptr) : Json(InOwnCommit)},
                      {"checkpointRef", InCheckpointRef},
                      {"checkpointCommits", InCheckpointCommits},
                      {"recoveryRef", pinned ? Json(recoveryRef) : Json(nullptr)},
                      {"manifest", CheckpointManifestPath(InContext, InId).generic_string()},
                      {"saved", true}, {"tested", false}, {"integrated", false},
                      {"published", false}, {"consumed", false}}.dump(2) << '\n';
    return 2;
}

auto CreateCheckpointRef(const QueueContext& InContext,
                         const Json& InManifest,
                         std::string* OutRef,
                         Json* OutCommits,
                         std::string* OutError) -> bool {
    const auto id = InManifest.value("id", "");
    const auto ref = "refs/kog/checkpoints/" + id;
    const auto refCheck = GitCapture(InContext.repo, {"check-ref-format", ref});
    if (refCheck.exitCode != 0 || GitCapture(InContext.repo, {"show-ref", "--verify", "--quiet", ref}).exitCode == 0) {
        if (OutError != nullptr) *OutError = "checkpoint ref is invalid or already exists";
        return false;
    }
    const auto checkout = InContext.root / "checkouts" / id;
    std::error_code ec;
    std::filesystem::create_directories(checkout.parent_path(), ec);
    if (ec || std::filesystem::exists(checkout, ec)) {
        if (OutError != nullptr) *OutError = "checkpoint checkout exists or cannot be created; inspect and retire only this task-owned checkout before retry: " + checkout.generic_string();
        return false;
    }
    const auto add = GitCapture(InContext.repo, {"worktree", "add", "--detach", checkout.string(), InManifest.value("baseHead", "")});
    if (add.exitCode != 0) {
        if (OutError != nullptr) *OutError = "cannot create isolated checkpoint worktree";
        return false;
    }
    for (const auto& phase : {std::string{"staged"}, std::string{"working"}}) {
        bool changed = false;
        std::vector<std::string> paths;
        std::vector<Json> versions;
        for (const auto& entry : InManifest["paths"]) {
            const auto& prior = phase == "staged" ? entry["head"] : entry["staged"];
            const auto& version = entry[phase];
            paths.push_back(entry.value("path", ""));
            versions.push_back(version);
            if (MatchesVersion(version, prior)) continue;
            changed = true;
            if (!InstallCheckpointVersion(checkout, entry.value("path", ""), version, InContext.repo, OutError)) {
                if (OutError != nullptr) *OutError += "; inspect preserved checkpoint checkout: " + checkout.generic_string();
                return false;
            }
        }
        if (!changed) continue;
        const auto parent = GitValue(checkout, {"rev-parse", "HEAD"}, OutError);
        const auto expectedTree = GitValue(checkout, {"write-tree"}, OutError);
        if (!parent.has_value() || !expectedTree.has_value()) {
            if (OutError != nullptr) *OutError += "; inspect preserved checkpoint checkout: " + checkout.generic_string();
            return false;
        }
        for (std::size_t index = 0; index < paths.size(); ++index) {
            const auto actual = GitPathEntry(checkout, paths[index], true, OutError);
            if (!actual.has_value() || !MatchesVersion(*actual, versions[index])) {
                if (OutError != nullptr) *OutError = "checkpoint index differs from captured version; inspect preserved checkout: " + checkout.generic_string();
                return false;
            }
        }
        const auto message = "[Checkpoint][WIP] Preserve " + phase + " overlap (" + InManifest.value("workItem", "") + ", " + id + ")";
        const auto commit = GitCapture(checkout, {"commit", "-m", message,
                                                  "--author", "Unattributed pre-existing work <unattributed@invalid>"});
        if (commit.exitCode != 0) {
            if (OutError != nullptr) *OutError = "checkpoint commit failed; hooks or signing may have rejected it; inspect preserved checkout: " + checkout.generic_string();
            return false;
        }
        const auto sha = GitValue(checkout, {"rev-parse", "HEAD"}, OutError);
        if (!sha.has_value()) return false;
        OutCommits->push_back(Json{{"version", phase}, {"commit", *sha}});
        if (!ExactCommitReadback(checkout, *sha, *parent, *expectedTree, paths, versions, OutError)) {
            if (OutError != nullptr) *OutError += "; inspect preserved checkpoint checkout: " + checkout.generic_string();
            return false;
        }
    }
    if (OutCommits->empty()) {
        if (OutError != nullptr) *OutError = "captured paths have no pre-existing change to checkpoint";
        return false;
    }
    const auto checkpointHead = GitValue(checkout, {"rev-parse", "HEAD"}, OutError);
    if (!checkpointHead.has_value()) return false;
    const auto zero = std::string(checkpointHead->size(), '0');
    const auto pin = GitCapture(InContext.repo, {"update-ref", ref, *checkpointHead, zero});
    if (pin.exitCode != 0) {
        if (OutError != nullptr) *OutError = "checkpoint ref update was rejected; inspect preserved checkout: " + checkout.generic_string();
        return false;
    }
    *OutRef = ref;
    for (const auto& entry : InManifest["paths"]) {
        const auto path = entry.value("path", "");
        const auto expected = entry["working"];
        const auto actual = GitPathEntry(InContext.repo, path, false, OutError, ref);
        if (!actual.has_value() || !MatchesVersion(*actual, expected)) {
            if (OutError != nullptr) *OutError = "checkpoint ref readback did not match captured working presence, mode, and blob; inspect preserved checkout: " + checkout.generic_string();
            return false;
        }
    }
    const auto remove = GitCapture(InContext.repo, {"worktree", "remove", checkout.string()});
    if (remove.exitCode != 0) {
        if (OutError != nullptr) *OutError = "checkpoint ref is saved, but its task-owned checkout could not be retired cleanly";
        return false;
    }
    return true;
}

auto RunCooperativeOverlapCommit(const QueueContext& InContext,
                                 const ExactPathCommitOptions& InOptions,
                                 const std::vector<std::string>& InPaths,
                                 const std::string& InInitialHead,
                                 const std::map<std::string, std::string>& InBeforeEntries,
                                 const std::map<std::string, std::string>& InUnrelatedBefore) -> int {
    std::string error;
    if (!IsSafeId(InOptions.overlapCheckpoint) || !InOptions.queueBatch.empty()) {
        return PrintError("invalid_checkpoint", "overlap checkpoint id must be safe and cannot share an active queue batch");
    }
    const auto manifest = ReadCheckpointManifest(CheckpointManifestPath(InContext, InOptions.overlapCheckpoint), &error);
    if (!manifest.has_value()) return PrintError("checkpoint_missing", error);
    const auto ref = GitValue(InContext.repo, {"symbolic-ref", "-q", "HEAD"}, &error);
    if (!ref.has_value() || (!ref->starts_with("refs/heads/codex/") && !ref->starts_with("refs/heads/feature/"))) {
        return PrintError("protected_branch", "overlap commits require an owned codex/ or feature/ branch");
    }
    if (manifest->value("repo", "") != InContext.repo.generic_string() ||
        manifest->value("id", "") != InOptions.overlapCheckpoint ||
        manifest->value("branchRef", "") != *ref ||
        manifest->value("baseHead", "") != InInitialHead || !manifest->value("ownerStable", false)) {
        return PrintError("stale_base_head", "checkpoint repository, branch, or HEAD differs from capture");
    }
    std::vector<std::string> capturedPaths;
    for (const auto& entry : (*manifest)["paths"]) capturedPaths.push_back(entry.value("path", ""));
    if (capturedPaths != InPaths) return PrintError("checkpoint_scope_mismatch", "exact paths must equal the captured overlap scope");
    for (const auto& entry : (*manifest)["paths"]) {
        const auto staged = GitPathEntry(InContext.repo, entry.value("path", ""), true, &error);
        const auto path = entry.value("path", "");
        const auto indexRecord = InBeforeEntries.contains(path) ? Json(InBeforeEntries.at(path)) : Json(nullptr);
        if (!staged.has_value() || *staged != entry["staged"] ||
            !entry.contains("indexRecord") || entry["indexRecord"] != indexRecord) {
            return PrintError("staged_contamination", "selected staged version changed after capture");
        }
    }
    const auto rules = LoadSecretRules(DefaultSecretRulesPath(InContext.repo), &error);
    if (!error.empty() || rules.empty()) return PrintError("secret_rules_unavailable", "checkpoint secret rules are unavailable");
    std::string blocker;
    auto prepared = PrepareOwnOverlap(InContext, *manifest, rules, &blocker, &error);
    if (!prepared.has_value()) {
        return CheckpointError(blocker.empty() ? "checkpoint_proof_failed" : blocker,
                               error.empty() ? "own-only separation could not be proven" : error, "", Json::array());
    }
    for (auto& path : *prepared) {
        if (HasSecret(path.ownBytes, rules) || HasSecret(path.rebasedStagedBytes, rules)) {
            return CheckpointError("secret_detected", "reconstructed content matched a secret rule", "", Json::array());
        }
    }
    if (InOptions.dryRun) {
        std::cout << Json{{"ok", true}, {"status", "preview"}, {"head", InInitialHead},
                          {"checkpointStatus", "saved"}, {"checkpointId", InOptions.overlapCheckpoint},
                          {"ownOnlyPaths", InPaths}, {"saved", true}, {"tested", false},
                          {"integrated", false}, {"published", false}, {"consumed", false}}.dump(2) << '\n';
        return 0;
    }
    const auto indexLock = IndexLockPath(InContext, &error);
    std::error_code ec;
    if (!indexLock.has_value() || std::filesystem::exists(*indexLock, ec)) {
        return CheckpointError("git_index_lock", "index lock appeared before checkpoint commit", "", Json::array());
    }
    if (GitValue(InContext.repo, {"rev-parse", "HEAD"}, &error) != std::optional<std::string>{InInitialHead} ||
        ParseIndexEntries(InContext.repo, &error) != std::optional<std::map<std::string, std::string>>{InBeforeEntries}) {
        return CheckpointError("checkpoint_snapshot_drift", "HEAD or index changed before checkpoint materialization", "", Json::array());
    }
    for (const auto& path : *prepared) {
        const auto current = WorkingEntry(InContext.repo, path.path, path.staged, path.head, rules, false, &error);
        if (!current.has_value() || *current != path.finalWorking) {
            return CheckpointError("checkpoint_snapshot_drift", "working file changed before checkpoint materialization", "", Json::array());
        }
    }

    Json checkpointCommits = Json::array();
    std::string checkpointRef;
    if (!CreateCheckpointRef(InContext, *manifest, &checkpointRef, &checkpointCommits, &error)) {
        return CheckpointError("checkpoint_commit_failed", error, checkpointRef, checkpointCommits);
    }
    if (GitValue(InContext.repo, {"rev-parse", "HEAD"}, &error) != std::optional<std::string>{InInitialHead} ||
        GitValue(InContext.repo, {"symbolic-ref", "-q", "HEAD"}, &error) != ref ||
        ParseIndexEntries(InContext.repo, &error) != std::optional<std::map<std::string, std::string>>{InBeforeEntries} ||
        std::filesystem::exists(*indexLock, ec)) {
        return CheckpointError("checkpoint_snapshot_drift", "caller branch or index changed during checkpoint creation",
                               checkpointRef, checkpointCommits);
    }
    for (auto& path : *prepared) {
        const auto current = WorkingEntry(InContext.repo, path.path, path.staged, path.head, rules, false, &error);
        if (!current.has_value() || *current != path.finalWorking) {
            return CheckpointError("checkpoint_snapshot_drift", "working file changed during checkpoint creation",
                                   checkpointRef, checkpointCommits);
        }
    }

    ScopedCheckpointFiles temporary;
    const auto tempRoot = InContext.root / "tmp";
    for (std::size_t index = 0; index < prepared->size(); ++index) {
        auto& path = (*prepared)[index];
        const auto prefix = InOptions.overlapCheckpoint + "-own-" + std::to_string(index) + "-" + TimestampId();
        const auto ownFile = tempRoot / (prefix + ".own");
        const auto stageFile = tempRoot / (prefix + ".stage");
        temporary.paths.insert(temporary.paths.end(), {ownFile, stageFile});
        if (!WriteCheckpointBytes(ownFile, path.ownBytes) ||
            !WriteCheckpointBytes(stageFile, path.rebasedStagedBytes)) {
            return CheckpointError("checkpoint_temp_failed", "cannot write verified own-only content", checkpointRef, checkpointCommits);
        }
        const auto ownOid = GitValue(InContext.repo, {"hash-object", "--no-filters", "-w", "--", ownFile.string()}, &error);
        const auto stageOid = GitValue(InContext.repo, {"hash-object", "--no-filters", "-w", "--", stageFile.string()}, &error);
        if (!ownOid.has_value() || !stageOid.has_value() ||
            GitBlobBytes(InContext.repo, *ownOid, &error) != std::optional<std::string>{path.ownBytes} ||
            GitBlobBytes(InContext.repo, *stageOid, &error) != std::optional<std::string>{path.rebasedStagedBytes}) {
            return CheckpointError("checkpoint_restore_failed", "own-only or restaged blob failed readback", checkpointRef, checkpointCommits);
        }
        path.ownOid = *ownOid;
        path.rebasedStagedOid = *stageOid;
    }

    const auto rawIndexPath = GitValue(InContext.repo, {"rev-parse", "--git-path", "index"}, &error);
    if (!rawIndexPath.has_value()) {
        return CheckpointError("index_path_unavailable", error, checkpointRef, checkpointCommits);
    }
    auto originalIndexPath = std::filesystem::path(*rawIndexPath);
    if (originalIndexPath.is_relative()) originalIndexPath = InContext.repo / originalIndexPath;
    if (std::filesystem::is_symlink(originalIndexPath, ec) || !std::filesystem::is_regular_file(originalIndexPath, ec)) {
        return CheckpointError("index_unavailable", "original Git index must be a regular non-link file", checkpointRef, checkpointCommits);
    }
    const auto originalIndexHash = GitValue(InContext.repo,
        {"hash-object", "--no-filters", "--", originalIndexPath.string()}, &error);
    if (!originalIndexHash.has_value()) {
        return CheckpointError("index_read_failed", error, checkpointRef, checkpointCommits);
    }
    const auto stageIndex = tempRoot / ("index-reconciled-" + TimestampId());
    temporary.paths.push_back(stageIndex);
    temporary.paths.push_back(stageIndex.string() + ".lock");
    std::filesystem::copy_file(originalIndexPath, stageIndex, std::filesystem::copy_options::none, ec);
    if (ec || GitValue(InContext.repo,
        {"hash-object", "--no-filters", "--", originalIndexPath.string()}, &error) != originalIndexHash) {
        return CheckpointError("index_snapshot_drift", "cannot copy a stable original index", checkpointRef, checkpointCommits);
    }
    {
        ScopedEnvironment indexEnvironment("GIT_INDEX_FILE", stageIndex.string());
        for (const auto& path : *prepared) {
            const auto info = path.staged.value("mode", "") + "," + path.rebasedStagedOid + "," + path.path;
            if (GitCapture(InContext.repo, {"update-index", "--add", "--cacheinfo", info}).exitCode != 0) {
                return CheckpointError("index_reconcile_failed", "cannot prepare selected staging intent", checkpointRef, checkpointCommits);
            }
        }
        const auto entries = ParseIndexEntries(InContext.repo, &error);
        const std::set<std::string> selected(InPaths.begin(), InPaths.end());
        if (!entries.has_value() || FilterUnrelated(*entries, selected) != InUnrelatedBefore) {
            return CheckpointError("staged_preservation_failed", "prepared index changed unrelated staged entries", checkpointRef, checkpointCommits);
        }
        for (const auto& path : *prepared) {
            const auto entry = GitPathEntry(InContext.repo, path.path, true, &error);
            const Json expected{{"exists", true}, {"mode", path.staged.value("mode", "")},
                                {"oid", path.rebasedStagedOid}};
            const auto beforeRecord = InBeforeEntries.find(path.path);
            const auto afterRecord = entries->find(path.path);
            const auto beforeFlag = beforeRecord == InBeforeEntries.end() ? std::string::npos : beforeRecord->second.find("|flag=");
            const auto afterFlag = afterRecord == entries->end() ? std::string::npos : afterRecord->second.find("|flag=");
            if (!entry.has_value() || !MatchesVersion(*entry, expected) ||
                beforeFlag == std::string::npos || afterFlag == std::string::npos ||
                beforeRecord->second.substr(beforeFlag) != afterRecord->second.substr(afterFlag)) {
                return CheckpointError("staged_preservation_failed", "prepared selected stage lost blob, mode, or index flag", checkpointRef, checkpointCommits);
            }
        }
    }

    const auto tempIndex = tempRoot / ("index-own-" + TimestampId());
    struct TempIndexCleanup {
        std::filesystem::path path;
        ~TempIndexCleanup() {
            std::error_code ignored;
            std::filesystem::remove(path, ignored);
            std::filesystem::remove(path.string() + ".lock", ignored);
        }
    } cleanup{tempIndex};
    if (GitValue(InContext.repo, {"rev-parse", "HEAD"}, &error) != std::optional<std::string>{InInitialHead} ||
        ParseIndexEntries(InContext.repo, &error) != std::optional<std::map<std::string, std::string>>{InBeforeEntries} ||
        std::filesystem::exists(*indexLock, ec) ||
        GitValue(InContext.repo, {"hash-object", "--no-filters", "--", originalIndexPath.string()}, &error) != originalIndexHash) {
        return CheckpointError("checkpoint_snapshot_drift", "caller branch or index changed before own isolated staging",
                               checkpointRef, checkpointCommits);
    }
    ScopedIndexLock originalIndexLock(*indexLock);
    if (!originalIndexLock.owned) {
        return CheckpointError("git_index_lock", "another writer acquired the original index lock", checkpointRef, checkpointCommits);
    }
    std::string preparedOwnTree;
    {
        ScopedEnvironment indexEnvironment("GIT_INDEX_FILE", tempIndex.string());
        if (GitCapture(InContext.repo, {"read-tree", "HEAD"}).exitCode != 0) {
            return CheckpointError("temporary_index_failed", "cannot seed own-only isolated index", checkpointRef, checkpointCommits);
        }
        for (const auto& path : *prepared) {
            const auto info = path.head.value("mode", "") + "," + path.ownOid + "," + path.path;
            if (GitCapture(InContext.repo, {"update-index", "--add", "--cacheinfo", info}).exitCode != 0) {
                return CheckpointError("temporary_index_failed", "cannot stage own-only blob", checkpointRef, checkpointCommits);
            }
        }
        const auto changed = GitCapture(InContext.repo, {"diff", "--cached", "--quiet"});
        if (changed.exitCode != 1) {
            return CheckpointError("no_selected_changes", "own-only isolated index has no committable delta", checkpointRef, checkpointCommits);
        }
        const auto tree = GitValue(InContext.repo, {"write-tree"}, &error);
        if (!tree.has_value()) {
            return CheckpointError("temporary_index_failed", "cannot seal own-only tree", checkpointRef, checkpointCommits);
        }
        preparedOwnTree = *tree;
        if (GitValue(InContext.repo, {"rev-parse", "HEAD"}, &error) != std::optional<std::string>{InInitialHead} ||
            GitValue(InContext.repo, {"hash-object", "--no-filters", "--", originalIndexPath.string()}, &error) != originalIndexHash) {
            return CheckpointError("checkpoint_snapshot_drift", "caller branch or index lock changed before own commit", checkpointRef, checkpointCommits);
        }
        for (const auto& path : *prepared) {
            const auto current = WorkingEntry(InContext.repo, path.path, path.staged, path.head, rules, false, &error);
            if (!current.has_value() || *current != path.finalWorking) {
                return CheckpointError("checkpoint_snapshot_drift", "working file changed before own commit", checkpointRef, checkpointCommits);
            }
        }
        const auto commit = GitCapture(InContext.repo, {"commit", "-m", InOptions.message});
        if (commit.exitCode != 0) {
            return CheckpointError("git_commit_failed", "own commit failed; normal hooks or signing may have rejected it",
                                   checkpointRef, checkpointCommits);
        }
    }
    const auto ownCommit = GitValue(InContext.repo, {"rev-parse", "HEAD"}, &error);
    if (!ownCommit.has_value()) {
        return CheckpointRecoveryRequired(InContext, InOptions.overlapCheckpoint, "commit_outcome_unknown",
                                          InInitialHead, *ref, "", checkpointRef, checkpointCommits);
    }
    std::vector<Json> ownVersions;
    for (const auto& path : *prepared) {
        ownVersions.push_back(Json{{"exists", true}, {"mode", path.head.value("mode", "")}, {"oid", path.ownOid}});
    }
    if (GitValue(InContext.repo, {"symbolic-ref", "-q", "HEAD"}, &error) != ref ||
        GitValue(InContext.repo, {"rev-parse", *ref}, &error) != ownCommit ||
        !ExactCommitReadback(InContext.repo, *ownCommit, InInitialHead, preparedOwnTree, InPaths, ownVersions, &error)) {
        return CheckpointRecoveryRequired(InContext, InOptions.overlapCheckpoint, "own_commit_readback_failed",
                                          InInitialHead, *ref, *ownCommit, checkpointRef, checkpointCommits);
    }
    if (ParseIndexEntries(InContext.repo, &error) != std::optional<std::map<std::string, std::string>>{InBeforeEntries}) {
        return CheckpointRecoveryRequired(InContext, InOptions.overlapCheckpoint, "staged_contamination",
                                          InInitialHead, *ref, *ownCommit, checkpointRef, checkpointCommits);
    }
    for (const auto& path : *prepared) {
        const auto current = WorkingEntry(InContext.repo, path.path, path.staged, path.head, rules, false, &error);
        if (!current.has_value() || *current != path.finalWorking) {
            return CheckpointRecoveryRequired(InContext, InOptions.overlapCheckpoint, "checkpoint_snapshot_drift",
                                              InInitialHead, *ref, *ownCommit, checkpointRef, checkpointCommits);
        }
    }
    if (GitValue(InContext.repo, {"hash-object", "--no-filters", "--", originalIndexPath.string()}, &error) != originalIndexHash ||
        GitValue(InContext.repo, {"rev-parse", "HEAD"}, &error) != ownCommit ||
        !ReplaceFileAtomically(stageIndex, originalIndexPath, &error)) {
        return CheckpointRecoveryRequired(InContext, InOptions.overlapCheckpoint, "index_reconcile_failed",
                                          InInitialHead, *ref, *ownCommit, checkpointRef, checkpointCommits);
    }
    const std::set<std::string> selected(InPaths.begin(), InPaths.end());
    const auto afterEntries = ParseIndexEntries(InContext.repo, &error);
    if (!afterEntries.has_value() || FilterUnrelated(*afterEntries, selected) != InUnrelatedBefore) {
        return CheckpointRecoveryRequired(InContext, InOptions.overlapCheckpoint, "staged_preservation_failed",
                                          InInitialHead, *ref, *ownCommit, checkpointRef, checkpointCommits);
    }
    std::cout << Json{{"ok", true}, {"status", "committed"}, {"commit", *ownCommit},
                      {"checkpointStatus", "saved"}, {"checkpointRef", checkpointRef},
                      {"checkpointCommits", checkpointCommits}, {"restoreVerified", true},
                      {"saved", true}, {"tested", false}, {"integrated", false},
                      {"published", false}, {"consumed", false},
                      {"unrelatedStagedPreserved", true}, {"included", InPaths}}.dump(2) << '\n';
    return 0;
}

} // namespace

auto RunExactPathCommit(const ExactPathCommitOptions& InOptions) -> int {
    std::string error;
    const auto context = ResolveQueueContext(InOptions.repo, &error);
    if (!context.has_value()) {
        return PrintError("not_a_repository", error);
    }
    const auto paths = NormalizePaths(context->repo, InOptions.paths, true, &error);
    if (!paths.has_value() || paths->empty()) {
        return PrintError("invalid_exact_path", error.empty() ? "at least one --exact-path is required" : error);
    }
    auto lock = AcquireQueueLock(*context, &error);
    if (!lock.has_value()) {
        if (!error.empty()) {
            return PrintError("queue_lock_failed", error, 1);
        }
        return PrintError("queue_locked", "another KOG queue or exact-path mutation is active", 1);
    }
    auto state = LoadState(*context, &error);
    if (!state.has_value()) {
        return PrintError("queue_state_invalid", error);
    }
    std::string activeBaseHead;
    if (!(*state)["active"].is_null()) {
        const auto activeId = (*state)["active"].value("id", "");
        if (InOptions.queueBatch != activeId) {
            return PrintError("active_batch", "exact-path commit must declare the active --queue-batch " + activeId);
        }
        const auto allowed = JsonStringSet((*state)["active"].value("files", Json::array()));
        for (const auto& path : *paths) {
            if (!allowed.contains(path)) {
                return PrintError("batch_scope_mismatch", "path is outside active queue batch: " + path);
            }
        }
        activeBaseHead = (*state)["active"].value("baseHead", "");
    } else if (!InOptions.queueBatch.empty()) {
        return PrintError("batch_mismatch", "--queue-batch was provided but no queue batch is active");
    }

    const auto indexLockRaw = GitValue(context->repo, {"rev-parse", "--git-path", "index.lock"}, &error);
    if (!indexLockRaw.has_value()) {
        return PrintError("index_path_unavailable", error);
    }
    auto indexLock = std::filesystem::path(*indexLockRaw);
    if (indexLock.is_relative()) {
        indexLock = context->repo / indexLock;
    }
    std::error_code ec;
    if (std::filesystem::exists(indexLock, ec)) {
        return PrintError("git_index_lock", "Git index lock exists; KOG will not delete it: " + indexLock.generic_string(), 1);
    }

    const auto initialHead = GitValue(context->repo, {"rev-parse", "HEAD"}, &error);
    if (!initialHead.has_value()) {
        return PrintError("head_unavailable", error);
    }
    const auto expectedHead = InOptions.expectedHead.empty() ? *initialHead : Trim(InOptions.expectedHead);
    if (*initialHead != expectedHead) {
        return PrintError("stale_base_head", "expected HEAD " + expectedHead + " but found " + *initialHead);
    }
    if (!activeBaseHead.empty() && *initialHead != activeBaseHead) {
        return PrintError("stale_queue_batch", "active queue batch was planned at a different HEAD; cancel and re-admit the work");
    }
    const auto unmerged = GitCapture(context->repo, {"ls-files", "--unmerged"});
    if (unmerged.exitCode != 0) {
        return PrintError("index_read_failed", Trim(unmerged.stderrStr));
    }
    if (!Trim(unmerged.stdoutStr).empty()) {
        return PrintError("unmerged_index", "exact-path commit requires all index conflicts to be resolved first");
    }
    const auto beforeEntries = ParseIndexEntries(context->repo, &error);
    if (!beforeEntries.has_value()) {
        return PrintError("index_read_failed", error);
    }
    const std::set<std::string> selected(paths->begin(), paths->end());
    const auto unrelatedBefore = FilterUnrelated(*beforeEntries, selected);
    const auto allChanged = ChangedPaths(context->repo, &error);
    if (!allChanged.has_value()) {
        return PrintError("status_failed", error);
    }
    if (!InOptions.overlapCheckpoint.empty()) {
        return RunCooperativeOverlapCommit(*context, InOptions, *paths, *initialHead,
                                           *beforeEntries, unrelatedBefore);
    }

    const auto tempDir = context->root / "tmp";
    std::filesystem::create_directories(tempDir, ec);
    if (ec) {
        return PrintError("temporary_index_failed", ec.message());
    }
    const auto tempIndex = tempDir / ("index-" + TimestampId());
    struct TempIndexCleanup {
        std::filesystem::path path;
        ~TempIndexCleanup() {
            std::error_code ignored;
            std::filesystem::remove(path, ignored);
            std::filesystem::remove(path.string() + ".lock", ignored);
        }
    } cleanup{tempIndex};

    std::vector<std::string> included;
    {
        ScopedEnvironment indexEnvironment("GIT_INDEX_FILE", tempIndex.string());
        auto result = GitCapture(context->repo, {"read-tree", "HEAD"});
        if (result.exitCode != 0) {
            return PrintError("temporary_index_failed", Trim(result.stderrStr));
        }

        std::vector<std::string> trackedPaths;
        std::vector<std::string> addedPaths;
        for (const auto& path : *paths) {
            const auto tracked = GitCapture(
                context->repo,
                {"-c", "core.quotepath=false", "ls-tree", "--name-only", "HEAD", "--", path});
            if (tracked.exitCode != 0) {
                return PrintError("exact_stage_failed", Trim(tracked.stderrStr));
            }
            if (Trim(tracked.stdoutStr).empty()) {
                addedPaths.push_back(path);
            } else {
                trackedPaths.push_back(path);
            }
        }

        if (!trackedPaths.empty()) {
            std::vector<std::string> updateArgs{"add", "-u", "--"};
            updateArgs.insert(updateArgs.end(), trackedPaths.begin(), trackedPaths.end());
            result = GitCapture(context->repo, updateArgs);
            if (result.exitCode != 0) {
                return PrintError("exact_stage_failed", Trim(result.stderrStr));
            }
        }
        if (!addedPaths.empty()) {
            std::vector<std::string> addArgs{"add", "--"};
            addArgs.insert(addArgs.end(), addedPaths.begin(), addedPaths.end());
            result = GitCapture(context->repo, addArgs);
            if (result.exitCode != 0) {
                return PrintError("exact_stage_failed", Trim(result.stderrStr));
            }
        }
        result = GitCapture(context->repo, {"-c", "core.quotepath=false", "diff", "--cached", "--name-only", "--no-renames"});
        if (result.exitCode != 0) {
            return PrintError("exact_stage_failed", Trim(result.stderrStr));
        }
        included = ParseLineList(result.stdoutStr);
        if (included.empty()) {
            return PrintError("no_selected_changes", "selected exact paths contain no changes");
        }
        std::sort(included.begin(), included.end());
    }

    std::vector<std::string> excluded;
    std::set_difference(allChanged->begin(), allChanged->end(), included.begin(), included.end(),
                        std::back_inserter(excluded));
    if (InOptions.dryRun) {
        std::cout << Json{
            {"ok", true},
            {"status", "preview"},
            {"head", *initialHead},
            {"included", included},
            {"excluded", excluded},
            {"unrelatedStagedPreserved", true},
        }.dump(2) << '\n';
        return 0;
    }

    const auto currentHead = GitValue(context->repo, {"rev-parse", "HEAD"}, &error);
    const auto currentEntries = ParseIndexEntries(context->repo, &error);
    if (!currentHead.has_value() || !currentEntries.has_value()) {
        return PrintError("precommit_recheck_failed", error);
    }
    if (*currentHead != expectedHead) {
        return PrintError("stale_base_head", "HEAD changed while preparing the exact-path commit");
    }
    if (FilterUnrelated(*currentEntries, selected) != unrelatedBefore) {
        return PrintError("staged_contamination", "unrelated staged entries changed while preparing the exact-path commit");
    }
    {
        ScopedEnvironment indexEnvironment("GIT_INDEX_FILE", tempIndex.string());
        const auto result = GitCapture(context->repo, {"commit", "-m", InOptions.message});
        if (result.exitCode != 0) {
            return PrintError("git_commit_failed", Trim(result.stderrStr.empty() ? result.stdoutStr : result.stderrStr), 1);
        }
    }

    std::vector<std::string> resetArgs{"reset", "-q", "HEAD", "--"};
    resetArgs.insert(resetArgs.end(), paths->begin(), paths->end());
    const auto reset = GitCapture(context->repo, resetArgs);
    if (reset.exitCode != 0) {
        return PrintError("index_reconcile_failed", Trim(reset.stderrStr), 1);
    }
    const auto afterEntries = ParseIndexEntries(context->repo, &error);
    if (!afterEntries.has_value() || FilterUnrelated(*afterEntries, selected) != unrelatedBefore) {
        return PrintError("staged_preservation_failed", error.empty() ? "unrelated index entries changed" : error, 1);
    }
    const auto commit = GitValue(context->repo, {"rev-parse", "HEAD"}, &error);
    if (!commit.has_value()) {
        return PrintError("commit_receipt_failed", error, 1);
    }
    std::cout << Json{
        {"ok", true},
        {"status", "committed"},
        {"commit", *commit},
        {"included", included},
        {"excluded", excluded},
        {"unrelatedStagedPreserved", true},
        {"queueBatch", InOptions.queueBatch.empty() ? Json(nullptr) : Json(InOptions.queueBatch)},
    }.dump(2) << '\n';
    return 0;
}

void RegisterAgentQueue(CLI::App& InApp) {
    auto* queue = InApp.add_subcommand("agent-queue", "Coordinate low-conflict coding-agent mutations per Git repository");

    auto* checkpoint = queue->add_subcommand("checkpoint", "Preserve stable pre-existing overlap separately from an own-change commit");
    auto* capture = checkpoint->add_subcommand("capture", "Save staged and working versions before editing an overlap");
    auto* captureRepo = new std::string{};
    auto* captureId = new std::string{};
    auto* capturePaths = new std::vector<std::string>{};
    auto* captureSource = new std::string{};
    auto* captureWorkItem = new std::string{};
    auto* captureOwnerStable = new bool{false};
    capture->add_option("--repo", *captureRepo, "Repository root; defaults to current repository");
    capture->add_option("--id", *captureId, "Unique checkpoint id")->required();
    capture->add_option("--path", *capturePaths, "Exact pre-existing overlap path; repeatable")->required();
    capture->add_option("--source", *captureSource, "Verified provenance label, or pre-existing/unknown")->required();
    capture->add_option("--work-item", *captureWorkItem, "Current work item id")->required();
    capture->add_flag("--owner-stable", *captureOwnerStable, "Assert the prior owner is not actively editing the selected paths");
    capture->callback([=]() {
        std::exit(RunCheckpointCapture(captureRepo->empty() ? std::filesystem::current_path() : std::filesystem::path(*captureRepo),
                                       *captureId, *capturePaths, *captureSource, *captureWorkItem, *captureOwnerStable));
    });

    auto* admit = queue->add_subcommand("admit", "Atomically admit a scoped agent mutation");
    auto* admitRepo = new std::string{};
    auto* admitId = new std::string{};
    auto* workItem = new std::string{};
    auto* agent = new std::string{};
    auto* files = new std::vector<std::string>{};
    auto* chunks = new std::vector<std::string>{};
    auto* postconditions = new std::vector<std::string>{};
    auto* validation = new std::vector<std::string>{};
    admit->add_option("--repo", *admitRepo, "Repository root; defaults to current repository");
    admit->add_option("--id", *admitId, "Stable admission id for idempotent callers");
    admit->add_option("--work-item", *workItem, "Backlog or work item id")->required();
    admit->add_option("--agent", *agent, "Coding agent/session id")->required();
    admit->add_option("--file", *files, "Exact repo-relative file intent; repeatable")->required();
    admit->add_option("--chunk", *chunks, "Non-overlapping chunk intent path:start-end; repeatable");
    admit->add_option("--postcondition", *postconditions, "Compatible postcondition path=value; repeatable");
    admit->add_option("--validate", *validation, "Validation command intent; repeatable");
    admit->callback([=]() {
        std::exit(RunAdmit(admitRepo->empty() ? std::filesystem::current_path() : std::filesystem::path(*admitRepo),
                           *admitId, *workItem, *agent, *files, *chunks, *postconditions, *validation));
    });

    auto* status = queue->add_subcommand("status", "Show pending, active, and recent queue receipts");
    auto* statusRepo = new std::string{};
    status->add_option("--repo", *statusRepo, "Repository root; defaults to current repository");
    status->callback([=]() {
        std::exit(RunStatus(statusRepo->empty() ? std::filesystem::current_path() : std::filesystem::path(*statusRepo)));
    });

    auto* drain = queue->add_subcommand("drain", "Plan or activate one compatible per-repository batch");
    auto* drainRepo = new std::string{};
    auto* confirm = new bool{false};
    drain->add_option("--repo", *drainRepo, "Repository root; defaults to current repository");
    drain->add_flag("--confirm", *confirm, "Activate the compatible batch; default is preview only");
    drain->callback([=]() {
        std::exit(RunDrain(drainRepo->empty() ? std::filesystem::current_path() : std::filesystem::path(*drainRepo), *confirm));
    });

    auto* complete = queue->add_subcommand("complete", "Close the active batch with a durable receipt");
    auto* completeRepo = new std::string{};
    auto* batch = new std::string{};
    auto* completionStatus = new std::string{};
    complete->add_option("--repo", *completeRepo, "Repository root; defaults to current repository");
    complete->add_option("--batch", *batch, "Active batch id")->required();
    complete->add_option("--status", *completionStatus, "succeeded|failed|cancelled")->required();
    complete->callback([=]() {
        std::exit(RunComplete(completeRepo->empty() ? std::filesystem::current_path() : std::filesystem::path(*completeRepo),
                              *batch, *completionStatus));
    });
}

} // namespace kano::git::commands
