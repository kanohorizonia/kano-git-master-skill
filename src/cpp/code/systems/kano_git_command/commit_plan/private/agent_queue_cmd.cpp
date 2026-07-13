#include "agent_queue_cmd.hpp"

#include "plan_utils.hpp"

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

    explicit ScopedDirectoryLock(std::filesystem::path InPath)
        : path(std::move(InPath)) {
        std::error_code ec;
        owned = std::filesystem::create_directory(path, ec);
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
        : path(std::move(InOther.path)), owned(InOther.owned) {
        InOther.owned = false;
    }

    auto operator=(ScopedDirectoryLock&&) -> ScopedDirectoryLock& = delete;
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

auto AcquireQueueLock(const QueueContext& InContext) -> std::optional<ScopedDirectoryLock> {
    ScopedDirectoryLock lock(InContext.root / "mutation.lock");
    if (!lock.owned) {
        return std::nullopt;
    }
    return std::optional<ScopedDirectoryLock>(std::move(lock));
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
    auto lock = AcquireQueueLock(*context);
    if (!lock.has_value()) {
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
    auto lock = AcquireQueueLock(*context);
    if (!lock.has_value()) {
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
    auto lock = AcquireQueueLock(*context);
    if (!lock.has_value()) {
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
    auto lock = AcquireQueueLock(*context);
    if (!lock.has_value()) {
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
        std::vector<std::string> addArgs{"add", "-A", "--"};
        addArgs.insert(addArgs.end(), paths->begin(), paths->end());
        result = GitCapture(context->repo, addArgs);
        if (result.exitCode != 0) {
            return PrintError("exact_stage_failed", Trim(result.stderrStr));
        }
        result = GitCapture(context->repo, {"-c", "core.quotepath=false", "diff", "--cached", "--name-only"});
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
