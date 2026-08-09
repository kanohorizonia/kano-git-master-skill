#include "operation_audit.hpp"

#include "audit_run_reader.hpp"
#include "audit_evidence_directory.hpp"
#include "audit_run_reader_private.hpp"

#include "shell_executor.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cerrno>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <map>
#include <random>
#include <set>
#include <sstream>
#include <system_error>
#include <thread>

#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace kano::git::commands {
namespace {

thread_local OperationAuditContext* gActiveAudit = nullptr;

void CanonicalizeArtifacts(std::vector<audit::ArtifactReference>& InOutArtifacts) {
    std::sort(InOutArtifacts.begin(), InOutArtifacts.end(),
              [](const auto& left, const auto& right) {
                  return left.id < right.id;
              });
}

auto Trim(std::string InValue) -> std::string {
    const auto first = InValue.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const auto last = InValue.find_last_not_of(" \t\r\n");
    return InValue.substr(first, last - first + 1);
}

auto CurrentUtc() -> std::string {
    const auto now = std::chrono::system_clock::now();
    const auto time = std::chrono::system_clock::to_time_t(now);
    std::tm utc{};
#if defined(_WIN32)
    gmtime_s(&utc, &time);
#else
    gmtime_r(&time, &utc);
#endif
    char buffer[32]{};
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &utc);
    return buffer;
}

auto Git(const std::filesystem::path& InRepo,
         const std::vector<std::string>& InArgs) -> shell::ExecResult {
    return shell::ExecuteCommand("git", InArgs, shell::ExecMode::Capture, InRepo);
}

auto CorrelationRefsFor(const OperationCorrelationEnvelope& In) -> audit::CorrelationRefs {
    audit::CorrelationRefs out;
    if (In.mode != "koa") return out;
    out.mode = audit::CorrelationMode::Koa;
    out.productId = In.productId;
    out.topicId = In.topicId.empty() ? std::nullopt : std::optional<std::string>(In.topicId);
    out.itemId = In.itemId;
    out.workOrderId = In.workOrderId;
    out.requestId = In.requestId;
    out.producerId = In.producerId;
    out.routeId = In.routeId;
    return out;
}

auto OutcomeForExit(const int InExitCode) -> audit::Outcome {
    audit::Outcome out;
    out.status = InExitCode == 0 ? audit::OutcomeState::Succeeded : audit::OutcomeState::Failed;
    out.exitCode = InExitCode;
    if (InExitCode != 0) out.reasonCode = "pipeline-failed";
    return out;
}

auto SetSystemError(std::string* OutError, const std::string& InPrefix) -> void {
    if (!OutError) return;
#if defined(_WIN32)
    *OutError = InPrefix + ": win32 error " + std::to_string(GetLastError());
#else
    *OutError = InPrefix + ": " + std::strerror(errno);
#endif
}

auto TestModeEnabled() -> bool {
    const auto* value = std::getenv("KOG_TEST_MODE");
    return value != nullptr && std::string_view(value) == "1";
}

void TestOnlyDelayFromEnvironment(const char* InName) {
    if (!TestModeEnabled()) return;
    const auto* raw = std::getenv(InName);
    if (raw == nullptr || *raw == '\0') return;
    errno = 0;
    char* end = nullptr;
    const auto millis = std::strtol(raw, &end, 10);
    if (errno != 0 || end == raw || *end != '\0' || millis < 1 || millis > 5000)
        return;
    std::this_thread::sleep_for(std::chrono::milliseconds(millis));
}

auto SyncDirectory(const std::filesystem::path& InDirectory,
                   std::string* OutError) -> bool;

auto IsSafeDirectory(const std::filesystem::path& InPath, std::string* OutError) -> bool {
    std::error_code ec;
    const auto status = std::filesystem::symlink_status(InPath, ec);
    if (ec || status.type() == std::filesystem::file_type::not_found) {
        if (OutError) *OutError = "audit directory is missing or unreadable";
        return false;
    }
    if (std::filesystem::is_symlink(status) || !std::filesystem::is_directory(status)) {
        if (OutError) *OutError = "audit directory is not a real directory";
        return false;
    }
    return true;
}

auto EnsureSafeDirectory(const std::filesystem::path& InPath, std::string* OutError) -> bool {
    std::error_code ec;
    const auto status = std::filesystem::symlink_status(InPath, ec);
    if (!ec && status.type() != std::filesystem::file_type::not_found)
        return IsSafeDirectory(InPath, OutError);
    ec.clear();
    const bool created = std::filesystem::create_directory(InPath, ec);
    if (ec || (!created && !std::filesystem::exists(InPath))) {
        if (OutError) *OutError = "cannot create audit directory: " + ec.message();
        return false;
    }
    if (!IsSafeDirectory(InPath, OutError)) return false;
    if (created) {
        auto parent = InPath.parent_path();
        if (parent.empty()) parent = ".";
        // POSIX requires the containing directory to be synced so the new
        // hierarchy entry survives a crash. SyncDirectory documents the
        // corresponding best-effort boundary on Windows.
        if (!SyncDirectory(parent, OutError)) return false;
    }
    return true;
}

auto EnsureSafeHierarchy(const std::vector<std::filesystem::path>& InPaths,
                         std::string* OutError) -> bool {
    for (const auto& path : InPaths) if (!EnsureSafeDirectory(path, OutError)) return false;
    return true;
}

auto ReserveRunDirectory(const std::filesystem::path& InPath, std::string* OutError) -> bool {
    std::error_code ec;
    if (!std::filesystem::create_directory(InPath, ec) || ec) {
        if (OutError) *OutError = "audit run/attempt already exists or cannot be reserved";
        return false;
    }
    std::filesystem::permissions(InPath, std::filesystem::perms::owner_all,
                                 std::filesystem::perm_options::replace, ec);
    if (ec || !IsSafeDirectory(InPath, OutError)) {
        if (OutError && OutError->empty()) *OutError = "cannot secure audit run/attempt directory";
        return false;
    }
    return true;
}

auto OpenExclusiveFile(const std::filesystem::path& InPath, std::string* OutError)
    -> std::intptr_t {
#if defined(_WIN32)
    const auto handle = CreateFileW(
        InPath.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, CREATE_NEW,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_WRITE_THROUGH,
        nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        SetSystemError(OutError, "cannot exclusively reserve audit file");
        return -1;
    }
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(handle, &info) ||
        (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        CloseHandle(handle);
        if (OutError) *OutError = "audit file resolved through a reparse point";
        return -1;
    }
    return reinterpret_cast<std::intptr_t>(handle);
#else
    const int flags = O_RDWR | O_CREAT | O_EXCL | O_APPEND | O_CLOEXEC
#if defined(O_NOFOLLOW)
        | O_NOFOLLOW
#endif
        ;
    const int handle = ::open(InPath.c_str(), flags, S_IRUSR | S_IWUSR);
    if (handle < 0) {
        SetSystemError(OutError, "cannot exclusively reserve audit file");
        return -1;
    }
    struct stat info {};
    if (::fstat(handle, &info) != 0 || !S_ISREG(info.st_mode)) {
        ::close(handle);
        if (OutError) *OutError = "audit handle is not a regular file";
        return -1;
    }
    return handle;
#endif
}

auto CloseHandleValue(std::intptr_t& InOutHandle) -> void {
    if (InOutHandle < 0) return;
#if defined(_WIN32)
    CloseHandle(reinterpret_cast<HANDLE>(InOutHandle));
#else
    ::close(static_cast<int>(InOutHandle));
#endif
    InOutHandle = -1;
}

auto WriteAndSync(const std::intptr_t InHandle,
                  const std::string_view InBytes,
                  std::string* OutError) -> bool {
    if (InHandle < 0) {
        if (OutError) *OutError = "audit handle is closed";
        return false;
    }
#if defined(_WIN32)
    const auto handle = reinterpret_cast<HANDLE>(InHandle);
    LARGE_INTEGER end{};
    if (!SetFilePointerEx(handle, end, nullptr, FILE_END)) {
        SetSystemError(OutError, "cannot seek audit handle");
        return false;
    }
    std::size_t offset = 0;
    while (offset < InBytes.size()) {
        const DWORD chunk = static_cast<DWORD>(
            std::min<std::size_t>(InBytes.size() - offset, 1U << 30U));
        DWORD written = 0;
        if (!WriteFile(handle, InBytes.data() + offset, chunk, &written, nullptr) || written == 0) {
            SetSystemError(OutError, "cannot write audit bytes");
            return false;
        }
        offset += written;
    }
    if (!FlushFileBuffers(handle)) {
        SetSystemError(OutError, "cannot flush audit bytes");
        return false;
    }
#else
    std::size_t offset = 0;
    while (offset < InBytes.size()) {
        const auto written = ::write(static_cast<int>(InHandle),
                                     InBytes.data() + offset,
                                     InBytes.size() - offset);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) {
            SetSystemError(OutError, "cannot write audit bytes");
            return false;
        }
        offset += static_cast<std::size_t>(written);
    }
    if (::fsync(static_cast<int>(InHandle)) != 0) {
        SetSystemError(OutError, "cannot sync audit bytes");
        return false;
    }
#endif
    return true;
}

auto ReadHandleAll(const std::intptr_t InHandle, std::string* OutError)
    -> std::optional<std::string> {
    if (InHandle < 0) {
        if (OutError) *OutError = "audit handle is closed";
        return std::nullopt;
    }
#if defined(_WIN32)
    const auto handle = reinterpret_cast<HANDLE>(InHandle);
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(handle, &size) || size.QuadPart < 0 || size.QuadPart > (64LL << 20LL)) {
        SetSystemError(OutError, "cannot size persisted audit stream");
        return std::nullopt;
    }
    LARGE_INTEGER begin{};
    if (!SetFilePointerEx(handle, begin, nullptr, FILE_BEGIN)) {
        SetSystemError(OutError, "cannot seek persisted audit stream");
        return std::nullopt;
    }
    std::string bytes(static_cast<std::size_t>(size.QuadPart), '\0');
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        DWORD read = 0;
        const DWORD chunk = static_cast<DWORD>(
            std::min<std::size_t>(bytes.size() - offset, 1U << 30U));
        if (!ReadFile(handle, bytes.data() + offset, chunk, &read, nullptr) || read == 0) {
            SetSystemError(OutError, "cannot read persisted audit stream");
            return std::nullopt;
        }
        offset += read;
    }
    if (!SetFilePointerEx(handle, begin, nullptr, FILE_END)) {
        SetSystemError(OutError, "cannot restore audit append position");
        return std::nullopt;
    }
    return bytes;
#else
    struct stat info {};
    if (::fstat(static_cast<int>(InHandle), &info) != 0 || info.st_size < 0 ||
        info.st_size > (64LL << 20LL)) {
        SetSystemError(OutError, "cannot size persisted audit stream");
        return std::nullopt;
    }
    std::string bytes(static_cast<std::size_t>(info.st_size), '\0');
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const auto read = ::pread(static_cast<int>(InHandle), bytes.data() + offset,
                                  bytes.size() - offset, static_cast<off_t>(offset));
        if (read < 0 && errno == EINTR) continue;
        if (read <= 0) {
            SetSystemError(OutError, "cannot read persisted audit stream");
            return std::nullopt;
        }
        offset += static_cast<std::size_t>(read);
    }
    return bytes;
#endif
}

auto SyncDirectory(const std::filesystem::path& InDirectory, std::string* OutError) -> bool {
#if defined(_WIN32)
    const auto verifyHandle = CreateFileW(
        InDirectory.c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
        nullptr);
    if (verifyHandle == INVALID_HANDLE_VALUE) {
        SetSystemError(OutError, "cannot open audit directory for verification");
        return false;
    }
    BY_HANDLE_FILE_INFORMATION info{};
    const bool verified = GetFileInformationByHandle(verifyHandle, &info) != 0 &&
        (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 &&
        (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0;
    CloseHandle(verifyHandle);
    if (!verified) {
        if (OutError) *OutError = "audit directory handle is not a real non-reparse directory";
        return false;
    }

    // Directory flushing is not uniformly supported on Windows.  First prove
    // the path is a real directory using a read-attributes handle, then make a
    // separate best-effort flush attempt.  Inability to acquire or flush that
    // optional handle is accepted only for documented unsupported cases.
    const auto flushHandle = CreateFileW(
        InDirectory.c_str(), GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT |
            FILE_FLAG_WRITE_THROUGH,
        nullptr);
    if (flushHandle == INVALID_HANDLE_VALUE) {
        const auto openError = GetLastError();
        if (openError == ERROR_INVALID_FUNCTION || openError == ERROR_INVALID_HANDLE ||
            openError == ERROR_ACCESS_DENIED || openError == ERROR_NOT_SUPPORTED ||
            openError == ERROR_SHARING_VIOLATION) {
            return true;
        }
        SetLastError(openError);
        SetSystemError(OutError, "cannot open audit directory flush handle");
        return false;
    }
    bool ok = FlushFileBuffers(flushHandle) != 0;
    const auto flushError = ok ? ERROR_SUCCESS : GetLastError();
    CloseHandle(flushHandle);
    // Windows filesystems do not uniformly expose directory-handle flushing.
    // File handles remain write-through + explicitly flushed; unsupported
    // directory flush is a documented best-effort boundary, not a run blocker.
    if (!ok && (flushError == ERROR_INVALID_FUNCTION ||
                flushError == ERROR_INVALID_HANDLE ||
                flushError == ERROR_ACCESS_DENIED ||
                flushError == ERROR_NOT_SUPPORTED)) {
        ok = true;
    }
    if (!ok) {
        SetLastError(flushError);
        SetSystemError(OutError, "cannot sync audit directory");
    }
    return ok;
#else
    const int flags = O_RDONLY | O_CLOEXEC
#if defined(O_DIRECTORY)
        | O_DIRECTORY
#endif
#if defined(O_NOFOLLOW)
        | O_NOFOLLOW
#endif
        ;
    const int handle = ::open(InDirectory.c_str(), flags);
    if (handle < 0) {
        SetSystemError(OutError, "cannot open audit directory for sync");
        return false;
    }
    const bool ok = ::fsync(handle) == 0;
    if (!ok) SetSystemError(OutError, "cannot sync audit directory");
    ::close(handle);
    return ok;
#endif
}

auto SyncRegularFilePath(const std::filesystem::path& InPath,
                         std::string* OutError) -> bool {
#if defined(_WIN32)
    const auto handle = CreateFileW(
        InPath.c_str(), GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT |
            FILE_FLAG_WRITE_THROUGH,
        nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        SetSystemError(OutError, "cannot open published audit receipt for sync");
        return false;
    }
    BY_HANDLE_FILE_INFORMATION info{};
    const bool regular = GetFileInformationByHandle(handle, &info) != 0 &&
        (info.dwFileAttributes &
         (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) == 0;
    const bool synced = regular && FlushFileBuffers(handle) != 0;
    const auto error = synced ? ERROR_SUCCESS : GetLastError();
    CloseHandle(handle);
    if (!regular) {
        if (OutError) *OutError = "published audit receipt is not a regular non-reparse file";
        return false;
    }
    if (!synced) {
        SetLastError(error);
        SetSystemError(OutError, "cannot sync published audit receipt");
    }
    return synced;
#else
    const int flags = O_RDONLY | O_CLOEXEC
#if defined(O_NOFOLLOW)
        | O_NOFOLLOW
#endif
        ;
    const int handle = ::open(InPath.c_str(), flags);
    if (handle < 0) {
        SetSystemError(OutError, "cannot open published audit receipt for sync");
        return false;
    }
    struct stat info {};
    const bool regular = ::fstat(handle, &info) == 0 && S_ISREG(info.st_mode);
    const bool synced = regular && ::fsync(handle) == 0;
    if (!regular && OutError)
        *OutError = "published audit receipt is not a regular non-symlink file";
    else if (!synced)
        SetSystemError(OutError, "cannot sync published audit receipt");
    ::close(handle);
    return synced;
#endif
}

auto RemoveRegularFileAndSyncDirectory(const std::filesystem::path& InPath,
                                       std::string* OutError) -> bool {
    std::error_code ec;
    const auto status = std::filesystem::symlink_status(InPath, ec);
    if (ec || !std::filesystem::is_regular_file(status) ||
        std::filesystem::is_symlink(status)) {
        if (OutError) *OutError = "publication-pending sentinel is missing or unsafe";
        return false;
    }
    if (!std::filesystem::remove(InPath, ec) || ec) {
        if (OutError) *OutError = "cannot clear publication-pending sentinel: " + ec.message();
        return false;
    }
    return SyncDirectory(InPath.parent_path(), OutError);
}

auto PublishNoReplace(const std::filesystem::path& InTemp,
                      const std::filesystem::path& InFinal,
                      bool* OutPublished,
                      std::string* OutError) -> bool {
    if (OutPublished) *OutPublished = false;
#if defined(_WIN32)
    if (!MoveFileExW(InTemp.c_str(), InFinal.c_str(), MOVEFILE_WRITE_THROUGH)) {
        SetSystemError(OutError, "cannot atomically publish audit receipt");
        return false;
    }
    if (OutPublished) *OutPublished = true;
#else
    if (::link(InTemp.c_str(), InFinal.c_str()) != 0) {
        SetSystemError(OutError, "cannot atomically publish audit receipt");
        return false;
    }
    if (OutPublished) *OutPublished = true;
    if (::unlink(InTemp.c_str()) != 0) {
        SetSystemError(OutError, "cannot remove published audit receipt temporary");
        return false;
    }
#endif
    return true;
}

auto PhaseForAction(const std::string& InAction) -> std::string {
    if (InAction == "audit.reserve" || InAction.find("preflight") != std::string::npos)
        return "preflight";
    if (InAction.find("commit") != std::string::npos) return "commit";
    if (InAction.find("sync") != std::string::npos) return "sync";
    if (InAction.find("push") != std::string::npos ||
        InAction.find("publish") != std::string::npos) return "push";
    if (InAction.find("stamp") != std::string::npos ||
        InAction.find("final") != std::string::npos) return "finalize";
    return "mutation";
}

auto RepositoryIdFor(const std::filesystem::path& InWorkspaceRoot,
                     const std::filesystem::path& InRepo) -> std::string {
    auto relative = InRepo.lexically_normal().lexically_relative(InWorkspaceRoot).generic_string();
    if (relative.empty() || relative == ".") return "workspace";
    if (relative.starts_with("../") || relative == ".." ||
        (!relative.empty() && relative.front() == '/'))
        return "repository-" + audit::Sha256Hex(InRepo.lexically_normal().generic_string()).substr(0, 32);
    return relative;
}

auto NewRunId(const std::string_view InPlanId,
              const std::string_view InSourceSha256) -> std::string {
    const auto now = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    std::random_device entropy;
    const auto material = std::string(InPlanId) + "\n" + std::string(InSourceSha256) +
        "\n" + std::to_string(now) + "\n" + std::to_string(entropy()) +
        "\n" + std::to_string(entropy());
    return "standalone-" + audit::Sha256Hex(material).substr(0, 40);
}

auto IsClosedAuditRoute(const std::string_view InRoute) -> bool {
    static constexpr std::array<std::string_view, 7> routes = {
        "commit.plan", "commit-push.plan", "plan.apply", "converge.repos",
        "converge.branches.apply", "converge.branches.recover",
        "converge.branches.retire"};
    return std::find(routes.begin(), routes.end(), InRoute) != routes.end();
}

auto ValidateAuditSpecTokens(const OperationAuditSpec& InSpec,
                             std::string* OutError) -> bool {
    const bool commitPlan = InSpec.inputKind == "commit-plan";
    const bool operation = InSpec.inputKind == "operation-descriptor";
    if (!commitPlan && !operation) {
        if (OutError) *OutError = "unsupported closed audit input kind";
        return false;
    }
    if (!IsClosedAuditRoute(InSpec.route)) {
        if (OutError) *OutError = "unsupported closed audited route";
        return false;
    }
    const bool routeInputPairValid = commitPlan
        ? (InSpec.route == "commit.plan" ||
           InSpec.route == "commit-push.plan" ||
           InSpec.route == "plan.apply")
        : (InSpec.route == "converge.repos" ||
           InSpec.route == "converge.branches.apply" ||
           InSpec.route == "converge.branches.recover" ||
           InSpec.route == "converge.branches.retire");
    if (!routeInputPairValid) {
        if (OutError) *OutError = "unsupported closed audited route/input-kind pair";
        return false;
    }
    const auto expectedName = commitPlan ? "frozen-plan.json" : "frozen-operation.json";
    if (InSpec.frozenFileName != expectedName) {
        if (OutError) *OutError = "invalid frozen audit filename token";
        return false;
    }
    if (!audit::IsStableAuditId(InSpec.planId)) {
        if (OutError) *OutError = "audit plan id violates the stable-ID grammar";
        return false;
    }
    if (InSpec.inputIdentity.empty() || InSpec.inputIdentity.size() > 4096 ||
        InSpec.inputIdentity.find('\0') != std::string::npos) {
        if (OutError) *OutError = "audit input identity is invalid";
        return false;
    }
    return true;
}

auto Nullable(const std::string& InValue) -> nlohmann::json {
    return InValue.empty() ? nlohmann::json(nullptr) : nlohmann::json(InValue);
}

auto CorrelationJson(const OperationCorrelationEnvelope& In) -> nlohmann::json {
    return {
        {"mode", In.mode}, {"product_id", Nullable(In.productId)},
        {"topic_id", Nullable(In.topicId)}, {"item_id", Nullable(In.itemId)},
        {"work_order_id", Nullable(In.workOrderId)}, {"request_id", Nullable(In.requestId)},
        {"run_id", Nullable(In.runId)}, {"parent_run_id", Nullable(In.parentRunId)},
        {"producer_id", Nullable(In.producerId)}, {"route_id", Nullable(In.routeId)},
        {"attempt", In.attempt},
    };
}

auto StateJson(const audit::RepositoryState& In) -> nlohmann::json {
    const auto nullable = [](const auto& value) -> nlohmann::json {
        return value.has_value() ? nlohmann::json(*value) : nlohmann::json(nullptr);
    };
    return {
        {"headSha", nullable(In.headSha)}, {"branch", nullable(In.branch)},
        {"worktreeState", std::string(audit::WorktreeStateName(In.worktreeState))},
        {"dirtyFingerprint", nullable(In.dirtyFingerprint)},
        {"upstreamHeadSha", nullable(In.upstreamHeadSha)},
        {"ahead", nullable(In.ahead)}, {"behind", nullable(In.behind)},
    };
}

auto TransitionJson(const audit::RepositoryTransition& In) -> nlohmann::json {
    return {{"id", In.repositoryId}, {"before", StateJson(In.before)}, {"after", StateJson(In.after)}};
}

auto IsLowerHexOfSize(const nlohmann::json& InValue,
                      const std::size_t InSize) -> bool {
    if (!InValue.is_string()) return false;
    const auto& value = InValue.get_ref<const std::string&>();
    return value.size() == InSize &&
        std::all_of(value.begin(), value.end(), [](const char ch) {
            return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
        });
}

auto IsNullableSha256(const nlohmann::json& InValue) -> bool {
    return InValue.is_null() || IsLowerHexOfSize(InValue, 64);
}

auto HasExactKeys(const nlohmann::json& InObject,
                  const std::set<std::string>& InExpected) -> bool {
    if (!InObject.is_object() || InObject.size() != InExpected.size()) return false;
    return std::all_of(InExpected.begin(), InExpected.end(), [&](const auto& key) {
        return InObject.contains(key);
    });
}

auto ValidateConvergeDescriptorOptions(const std::string_view InRoute,
                                       const nlohmann::json& InOptions,
                                       std::string* OutError) -> bool {
    const auto fail = [&](const std::string_view message) {
        if (OutError) *OutError = std::string(message);
        return false;
    };
    const auto boolean = [&](const char* key) {
        return InOptions.at(key).is_boolean();
    };
    const auto jobs = [&]() {
        return InOptions.at("jobs").is_number_integer() &&
            InOptions.at("jobs").get<std::int64_t>() > 0 &&
            InOptions.at("jobs").get<std::int64_t>() <= 1000000;
    };

    if (InRoute == "converge.repos") {
        static const std::set<std::string> keys = {
            "abort", "agentIntentCommitMode", "forceWithLease", "jobs",
            "noVerify", "recursive", "remoteSelectorSha256", "resume",
            "settleWorktrees"};
        if (!HasExactKeys(InOptions, keys))
            return fail("invalid closed converge.repos descriptor options");
        if (!boolean("abort") || !boolean("agentIntentCommitMode") ||
            !boolean("forceWithLease") || !boolean("noVerify") ||
            !boolean("recursive") || !boolean("resume") ||
            !boolean("settleWorktrees") || !jobs() ||
            !IsNullableSha256(InOptions.at("remoteSelectorSha256")))
            return fail("invalid typed converge.repos descriptor options");
        return true;
    }
    if (InRoute == "converge.branches.apply") {
        static const std::set<std::string> keys = {
            "branchSelectorSha256", "confirm", "jobs",
            "recordReviewedIntegration", "recursive", "strategy",
            "syncTarget", "targetSelectorSha256"};
        if (!HasExactKeys(InOptions, keys))
            return fail("invalid closed branch-apply descriptor options");
        const auto& strategy = InOptions.at("strategy");
        if (!boolean("confirm") || !boolean("recordReviewedIntegration") ||
            !boolean("recursive") || !boolean("syncTarget") || !jobs() ||
            !IsNullableSha256(InOptions.at("branchSelectorSha256")) ||
            !IsNullableSha256(InOptions.at("targetSelectorSha256")) ||
            !strategy.is_string() ||
            (strategy != "rebase" && strategy != "merge" &&
             strategy != "cherry-pick"))
            return fail("invalid typed branch-apply descriptor options");
        return true;
    }
    if (InRoute == "converge.branches.recover") {
        static const std::set<std::string> keys = {
            "abort", "continue", "expectedHeadSha1", "remoteSelectorSha256",
            "restoreHeadSha1", "targetSelectorSha256"};
        if (!HasExactKeys(InOptions, keys))
            return fail("invalid closed branch-recover descriptor options");
        if (!boolean("abort") || !boolean("continue") ||
            InOptions.at("abort") == InOptions.at("continue") ||
            !IsLowerHexOfSize(InOptions.at("expectedHeadSha1"), 40) ||
            !IsLowerHexOfSize(InOptions.at("restoreHeadSha1"), 40) ||
            !IsNullableSha256(InOptions.at("remoteSelectorSha256")) ||
            !IsNullableSha256(InOptions.at("targetSelectorSha256")))
            return fail("invalid typed branch-recover descriptor options");
        return true;
    }
    if (InRoute == "converge.branches.retire") {
        static const std::set<std::string> keys = {
            "branchSelectorSha256", "confirm", "deleteRemote",
            "harvestBranchWorktrees", "harvestDetachedWorktrees", "jobs",
            "pruneWorktrees", "recursive", "removeWorktrees",
            "targetSelectorSha256"};
        if (!HasExactKeys(InOptions, keys))
            return fail("invalid closed branch-retire descriptor options");
        if (!boolean("confirm") || !boolean("deleteRemote") ||
            !boolean("harvestBranchWorktrees") ||
            !boolean("harvestDetachedWorktrees") || !boolean("pruneWorktrees") ||
            !boolean("recursive") || !boolean("removeWorktrees") || !jobs() ||
            !IsNullableSha256(InOptions.at("branchSelectorSha256")) ||
            !IsNullableSha256(InOptions.at("targetSelectorSha256")))
            return fail("invalid typed branch-retire descriptor options");
        return true;
    }
    return fail("unsupported operation descriptor route");
}

} // namespace

auto ReadBoundedAuditInput(const std::filesystem::path& InPath,
                           const std::uintmax_t InLimit,
                           std::string* OutError) -> std::optional<std::string> {
#if defined(_WIN32)
    const auto handle = CreateFileW(
        InPath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        SetSystemError(OutError, "cannot open bounded audit input");
        return std::nullopt;
    }
    BY_HANDLE_FILE_INFORMATION before{};
    LARGE_INTEGER size{};
    if (!GetFileInformationByHandle(handle, &before) ||
        (before.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 ||
        (before.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 ||
        !GetFileSizeEx(handle, &size) || size.QuadPart < 0 ||
        static_cast<std::uint64_t>(size.QuadPart) > InLimit) {
        CloseHandle(handle);
        if (OutError) *OutError = "audit input must be a bounded regular non-reparse file";
        return std::nullopt;
    }
    TestOnlyDelayFromEnvironment(
        "KOG_TEST_ONLY_AUDIT_INPUT_POST_STAT_DELAY_MS");
    std::string bytes(static_cast<std::size_t>(size.QuadPart), '\0');
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        DWORD read = 0;
        const DWORD chunk = static_cast<DWORD>(
            std::min<std::size_t>(bytes.size() - offset, 1U << 30U));
        if (!ReadFile(handle, bytes.data() + offset, chunk, &read, nullptr) || read == 0) {
            CloseHandle(handle);
            SetSystemError(OutError, "cannot read bounded audit input");
            return std::nullopt;
        }
        offset += read;
    }
    BY_HANDLE_FILE_INFORMATION after{};
    const bool stable = GetFileInformationByHandle(handle, &after) &&
        before.dwVolumeSerialNumber == after.dwVolumeSerialNumber &&
        before.nFileIndexHigh == after.nFileIndexHigh &&
        before.nFileIndexLow == after.nFileIndexLow &&
        before.nFileSizeHigh == after.nFileSizeHigh &&
        before.nFileSizeLow == after.nFileSizeLow &&
        before.ftLastWriteTime.dwHighDateTime == after.ftLastWriteTime.dwHighDateTime &&
        before.ftLastWriteTime.dwLowDateTime == after.ftLastWriteTime.dwLowDateTime;
    CloseHandle(handle);
    if (!stable) {
        if (OutError) *OutError = "audit input changed during bounded read";
        return std::nullopt;
    }
    return bytes;
#else
    const int flags = O_RDONLY | O_CLOEXEC
#if defined(O_NOFOLLOW)
        | O_NOFOLLOW
#endif
        ;
    const int handle = ::open(InPath.c_str(), flags);
    if (handle < 0) {
        SetSystemError(OutError, "cannot open bounded audit input");
        return std::nullopt;
    }
    struct stat before {};
    if (::fstat(handle, &before) != 0 || !S_ISREG(before.st_mode) ||
        before.st_size < 0 || static_cast<std::uintmax_t>(before.st_size) > InLimit) {
        ::close(handle);
        if (OutError) *OutError = "audit input must be a bounded regular non-symlink file";
        return std::nullopt;
    }
    TestOnlyDelayFromEnvironment(
        "KOG_TEST_ONLY_AUDIT_INPUT_POST_STAT_DELAY_MS");
    std::string bytes(static_cast<std::size_t>(before.st_size), '\0');
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const auto read = ::pread(handle, bytes.data() + offset, bytes.size() - offset,
                                  static_cast<off_t>(offset));
        if (read < 0 && errno == EINTR) continue;
        if (read <= 0) {
            ::close(handle);
            SetSystemError(OutError, "cannot read bounded audit input");
            return std::nullopt;
        }
        offset += static_cast<std::size_t>(read);
    }
    struct stat after {};
    const bool restat = ::fstat(handle, &after) == 0;
    bool timestampsStable = false;
#if defined(__APPLE__)
    timestampsStable = restat &&
        before.st_mtimespec.tv_sec == after.st_mtimespec.tv_sec &&
        before.st_mtimespec.tv_nsec == after.st_mtimespec.tv_nsec &&
        before.st_ctimespec.tv_sec == after.st_ctimespec.tv_sec &&
        before.st_ctimespec.tv_nsec == after.st_ctimespec.tv_nsec;
#else
    timestampsStable = restat && before.st_mtim.tv_sec == after.st_mtim.tv_sec &&
        before.st_mtim.tv_nsec == after.st_mtim.tv_nsec &&
        before.st_ctim.tv_sec == after.st_ctim.tv_sec &&
        before.st_ctim.tv_nsec == after.st_ctim.tv_nsec;
#endif
    const bool stable = restat && before.st_dev == after.st_dev &&
        before.st_ino == after.st_ino && before.st_size == after.st_size &&
        timestampsStable;
    ::close(handle);
    if (!stable) {
        if (OutError) *OutError = "audit input changed during bounded read";
        return std::nullopt;
    }
    return bytes;
#endif
}

auto ValidateOperationCorrelationEnvelope(const OperationCorrelationEnvelope& In,
                                          const bool InAllowResolvedStandaloneRun,
                                          std::string* OutError) -> bool {
    if (In.mode != "standalone" && In.mode != "koa") {
        if (OutError) *OutError = "correlation mode must be standalone or koa";
        return false;
    }
    if (In.attempt == 0) {
        if (OutError) *OutError = "correlation attempt must be positive";
        return false;
    }
    const std::array<const std::string*, 9> identifiers = {
        &In.productId, &In.topicId, &In.itemId, &In.workOrderId, &In.requestId,
        &In.runId, &In.parentRunId, &In.producerId, &In.routeId};
    for (const auto* value : identifiers) {
        if (value->empty()) continue;
        if (!audit::IsStableAuditId(*value)) {
            if (OutError) *OutError = "correlation identifier violates the stable-ID grammar";
            return false;
        }
    }
    if (In.mode == "koa") {
        for (const auto* required : {&In.productId, &In.itemId, &In.workOrderId,
                                     &In.requestId, &In.runId, &In.producerId,
                                     &In.routeId}) {
            if (required->empty()) {
                if (OutError) *OutError = "KOA correlation identity fields are incomplete";
                return false;
            }
        }
        if (!In.parentRunId.empty() && In.parentRunId == In.runId) {
            if (OutError) *OutError = "parent run cannot equal run";
            return false;
        }
        return true;
    }
    if (!In.productId.empty() || !In.topicId.empty() || !In.itemId.empty() ||
        !In.workOrderId.empty() || !In.requestId.empty() || !In.parentRunId.empty() ||
        !In.producerId.empty() || !In.routeId.empty() ||
        (!InAllowResolvedStandaloneRun && !In.runId.empty())) {
        if (OutError) *OutError = "standalone provenance fields must be null";
        return false;
    }
    return true;
}

auto ParseOperationCorrelationEnvelope(const std::string_view InJson,
                                       std::string* OutError)
    -> std::optional<OperationCorrelationEnvelope> {
    if (InJson.size() > (64U << 10U)) {
        if (OutError) *OutError = "correlation envelope exceeds 64 KiB";
        return std::nullopt;
    }
    bool duplicate = false;
    std::vector<std::set<std::string>> keys;
    const auto callback = [&](int, const nlohmann::json::parse_event_t event,
                              nlohmann::json& parsed) {
        if (event == nlohmann::json::parse_event_t::object_start) keys.emplace_back();
        else if (event == nlohmann::json::parse_event_t::key) {
            if (keys.empty() || !keys.back().insert(parsed.get<std::string>()).second)
                duplicate = true;
        } else if (event == nlohmann::json::parse_event_t::object_end && !keys.empty()) {
            keys.pop_back();
        }
        return true;
    };
    try {
        const auto doc = nlohmann::json::parse(InJson, callback);
        static const std::array<std::string_view, 11> expected = {
            "mode", "product_id", "topic_id", "item_id", "work_order_id",
            "request_id", "run_id", "parent_run_id", "producer_id", "route_id",
            "attempt"};
        if (duplicate || !doc.is_object() || doc.size() != expected.size())
            throw std::runtime_error("closed 11-field correlation envelope required");
        for (const auto key : expected) if (!doc.contains(key))
            throw std::runtime_error("correlation envelope is missing fields");
        if (!doc.at("mode").is_string() || !doc.at("attempt").is_number_unsigned() ||
            doc.at("attempt").get<std::uint64_t>() == 0 ||
            doc.at("attempt").get<std::uint64_t>() > UINT32_MAX)
            throw std::runtime_error("invalid correlation mode or attempt type");
        OperationCorrelationEnvelope out;
        out.mode = doc.at("mode").get<std::string>();
        out.attempt = static_cast<std::uint32_t>(doc.at("attempt").get<std::uint64_t>());
        const auto load = [&](const char* key, std::string& target) {
            const auto& value = doc.at(key);
            if (value.is_null()) return;
            if (!value.is_string()) throw std::runtime_error("invalid correlation identifier type");
            target = value.get<std::string>();
        };
        load("product_id", out.productId); load("topic_id", out.topicId);
        load("item_id", out.itemId); load("work_order_id", out.workOrderId);
        load("request_id", out.requestId); load("run_id", out.runId);
        load("parent_run_id", out.parentRunId); load("producer_id", out.producerId);
        load("route_id", out.routeId);
        std::string validationError;
        if (!ValidateOperationCorrelationEnvelope(out, false, &validationError))
            throw std::runtime_error(validationError);
        return out;
    } catch (const std::exception& ex) {
        if (OutError) *OutError = ex.what();
        return std::nullopt;
    }
}

auto ResolveStandaloneCorrelation(OperationCorrelationEnvelope In,
                                  const std::string_view InPlanId,
                                  const std::string_view InSourceSha256)
    -> OperationCorrelationEnvelope {
    if (In.mode == "standalone" && In.runId.empty())
        In.runId = NewRunId(InPlanId, InSourceSha256);
    return In;
}

auto SerializeOperationCorrelationEnvelope(const OperationCorrelationEnvelope& In)
    -> std::string {
    return CorrelationJson(In).dump();
}

auto BuildOperationDescriptor(const std::string_view InRoute,
                              const std::string_view InCanonicalOptionsJson,
                              const OperationCorrelationEnvelope& InCorrelation,
                              std::string* OutError) -> std::optional<std::string> {
    try {
        const auto options = nlohmann::json::parse(InCanonicalOptionsJson);
        if (!options.is_object()) throw std::runtime_error("operation options must be an object");
        std::string optionsError;
        if (!ValidateConvergeDescriptorOptions(InRoute, options, &optionsError))
            throw std::runtime_error(optionsError);
        std::string validationError;
        if (!ValidateOperationCorrelationEnvelope(InCorrelation, true, &validationError))
            throw std::runtime_error(validationError);
        nlohmann::json doc = {
            {"schema_name", "kog.operationAuditInput"}, {"schema_version", 1},
            {"audit_protocol", "kog.operation-audit/v1"},
            {"route", InRoute}, {"options", options},
            {"correlation", CorrelationJson(InCorrelation)},
        };
        return doc.dump() + '\n';
    } catch (const std::exception& ex) {
        if (OutError) *OutError = ex.what();
        return std::nullopt;
    }
}

auto ResolveOperationAuditPaths(const OperationAuditSpec& InSpec,
                                const std::string_view InRunId,
                                const std::uint32_t InAttempt,
                                std::string* OutError)
    -> std::optional<OperationAuditPaths> {
    if (!ValidateAuditSpecTokens(InSpec, OutError) || InRunId.empty() || InAttempt == 0) {
        if (OutError) *OutError = "audit path identity is incomplete";
        return std::nullopt;
    }
    OperationAuditPaths paths;
    const auto gitDirResult = Git(
        InSpec.workspaceRoot,
        {"rev-parse", "--path-format=absolute", "--git-common-dir"});
    const auto gitDirectoryText = Trim(gitDirResult.stdoutStr);
    if (gitDirResult.exitCode == 0 && !gitDirectoryText.empty()) {
        const auto gitDirectory = std::filesystem::path(gitDirectoryText).lexically_normal();
        if (!IsSafeDirectory(gitDirectory, OutError)) return std::nullopt;
        const auto prefix = InSpec.inputKind == "commit-plan" ? "plan-" : "operation-";
        paths.auditRoot = gitDirectory / "kog" / "audit" /
            (prefix + audit::Sha256Hex(InSpec.inputIdentity));
    } else if (InSpec.sourcePath.has_value()) {
        paths.auditRoot = InSpec.sourcePath->parent_path() /
            (InSpec.sourcePath->filename().string() + ".audit");
    } else {
        paths.auditRoot = InSpec.workspaceRoot / ".kano" / "tmp" / "git" /
            "audit" / ("operation-" + audit::Sha256Hex(InSpec.inputIdentity));
    }
    paths.runRoot = paths.auditRoot / ("run-" + audit::Sha256Hex(InRunId));
    paths.attemptRoot = paths.runRoot / ("attempt-" + std::to_string(InAttempt));
    paths.frozenInput = paths.attemptRoot /
        (InSpec.frozenFileName.empty() ? "frozen-input.json" : InSpec.frozenFileName);
    paths.events = paths.attemptRoot / "events.jsonl";
    paths.receipt = paths.attemptRoot / "receipt.json";
    paths.publicationPending = paths.attemptRoot / "publication-pending.json";
    paths.incomplete = paths.attemptRoot / "incomplete.json";
    return paths;
}

auto OperationAuditContext::Reserve(OperationAuditSpec InSpec, std::string* OutError)
    -> std::unique_ptr<OperationAuditContext> {
    if (gActiveAudit) {
        if (OutError) *OutError = "an operation audit owner is already active";
        return nullptr;
    }
    if (!ValidateAuditSpecTokens(InSpec, OutError)) return nullptr;
    if (InSpec.sourceBytes.size() > (4U << 20U) || InSpec.frozenBytes.size() > (4U << 20U) ||
        InSpec.sourceBytes.empty() || InSpec.frozenBytes.empty()) {
        if (OutError) *OutError = "audit input must be non-empty and at most 4 MiB";
        return nullptr;
    }
    std::string correlationError;
    if (!ValidateOperationCorrelationEnvelope(InSpec.correlation, true, &correlationError) ||
        InSpec.correlation.runId.empty()) {
        if (OutError) *OutError = correlationError.empty()
            ? "resolved audit correlation requires a run id" : correlationError;
        return nullptr;
    }
    auto sink = std::unique_ptr<OperationAuditContext>(new OperationAuditContext());
    sink->mSpec = std::move(InSpec);
    sink->mRunId = sink->mSpec.correlation.runId;
    if (!sink->mSpec.correlation.parentRunId.empty())
        sink->mParentRunId = sink->mSpec.correlation.parentRunId;
    sink->mSourceSha256 = audit::Sha256Hex(sink->mSpec.sourceBytes);
    sink->mPlanSha256 = audit::Sha256Hex(sink->mSpec.frozenBytes);
    sink->mCorrelation = CorrelationRefsFor(sink->mSpec.correlation);
    sink->mStartedAtUtc = CurrentUtc();
    const auto paths = ResolveOperationAuditPaths(
        sink->mSpec, sink->mRunId, sink->mSpec.correlation.attempt, OutError);
    if (!paths) return nullptr;
    sink->mPaths = *paths;

    std::vector<std::filesystem::path> hierarchy;
    const auto gitDirResult = Git(
        sink->mSpec.workspaceRoot,
        {"rev-parse", "--path-format=absolute", "--git-common-dir"});
    const auto gitDirectoryText = Trim(gitDirResult.stdoutStr);
    if (gitDirResult.exitCode == 0 && !gitDirectoryText.empty()) {
        const auto gitDirectory = std::filesystem::path(gitDirectoryText).lexically_normal();
        hierarchy = {gitDirectory, gitDirectory / "kog", gitDirectory / "kog" / "audit",
                     sink->mPaths.auditRoot, sink->mPaths.runRoot};
    } else if (sink->mSpec.sourcePath.has_value()) {
        hierarchy = {sink->mPaths.auditRoot, sink->mPaths.runRoot};
    } else {
        const auto kano = sink->mSpec.workspaceRoot / ".kano";
        hierarchy = {sink->mSpec.workspaceRoot, kano, kano / "tmp", kano / "tmp" / "git",
                     kano / "tmp" / "git" / "audit", sink->mPaths.auditRoot,
                     sink->mPaths.runRoot};
    }
    if (!EnsureSafeHierarchy(hierarchy, OutError)) return nullptr;
    if (!ReserveRunDirectory(sink->mPaths.attemptRoot, OutError)) return nullptr;
    if (!SyncDirectory(sink->mPaths.runRoot, OutError)) return nullptr;

    const nlohmann::json pendingMarker = {
        {"schemaName", "kog.auditPublicationPending"},
        {"schemaVersion", 1},
        {"runId", sink->mRunId},
        {"parentRunId", sink->mParentRunId
            ? nlohmann::json(*sink->mParentRunId) : nlohmann::json(nullptr)},
        {"attempt", sink->mSpec.correlation.attempt},
        {"planId", sink->mSpec.planId},
        {"planSha256", sink->mPlanSha256},
        {"reservedAtUtc", sink->mStartedAtUtc},
    };
    auto pendingHandle = OpenExclusiveFile(
        sink->mPaths.publicationPending, OutError);
    const auto pendingBytes = pendingMarker.dump() + '\n';
    if (pendingHandle < 0 ||
        !WriteAndSync(pendingHandle, pendingBytes, OutError)) {
        CloseHandleValue(pendingHandle);
        return nullptr;
    }
    CloseHandleValue(pendingHandle);
    if (!SyncDirectory(sink->mPaths.attemptRoot, OutError)) return nullptr;

    auto frozenHandle = OpenExclusiveFile(sink->mPaths.frozenInput, OutError);
    if (frozenHandle < 0 || !WriteAndSync(frozenHandle, sink->mSpec.frozenBytes, OutError)) {
        CloseHandleValue(frozenHandle);
        return nullptr;
    }
    CloseHandleValue(frozenHandle);
    sink->mEventsHandle = OpenExclusiveFile(sink->mPaths.events, OutError);
    if (sink->mEventsHandle < 0 || !WriteAndSync(sink->mEventsHandle, {}, OutError)) return nullptr;
    sink->mReceiptHandle = OpenExclusiveFile(sink->mPaths.receipt.string() + ".tmp", OutError);
    if (sink->mReceiptHandle < 0 || !WriteAndSync(sink->mReceiptHandle, {}, OutError)) return nullptr;
    if (!SyncDirectory(sink->mPaths.attemptRoot, OutError)) return nullptr;

    const auto bindMainInput = [&](const std::string_view role,
                                   const std::string& sha256,
                                   const std::size_t size) {
        audit::ArtifactReference artifact;
        artifact.id = std::string("audit-") + std::string(role) + "-" +
            sha256.substr(0, 20);
        artifact.kind = std::string("audit-") + std::string(role) + "-" +
            sink->mSpec.route + "-" + sink->mSpec.inputKind;
        artifact.sha256 = sha256;
        artifact.sizeBytes = size;
        artifact.contentType = "application/json";
        artifact.redactionStatus = audit::RedactionStatus::Withheld;
        sink->mSupplementalInputs.push_back(std::move(artifact));
    };
    bindMainInput("source", sink->mSourceSha256, sink->mSpec.sourceBytes.size());
    bindMainInput("frozen", sink->mPlanSha256, sink->mSpec.frozenBytes.size());
    CanonicalizeArtifacts(sink->mSupplementalInputs);

    sink->mPreviousActive = gActiveAudit;
    gActiveAudit = sink.get();
    const auto before = sink->Capture(sink->mSpec.workspaceRoot);
    if (!sink->Append("audit.reserve", sink->mSpec.workspaceRoot, before,
                      sink->mStartedAtUtc, 0, OutError)) {
        gActiveAudit = sink->mPreviousActive;
        return nullptr;
    }
    return sink;
}

OperationAuditContext::~OperationAuditContext() {
    if (!mFinalized && !mFinalizationAttempted) {
        std::string ignored;
        PublishReceipt(2, &ignored);
    }
    if (gActiveAudit == this) gActiveAudit = mPreviousActive;
    CloseHandleValue(mEventsHandle);
    CloseHandleValue(mReceiptHandle);
}

auto OperationAuditContext::Current() noexcept -> OperationAuditContext* {
    return gActiveAudit;
}

auto OperationAuditContext::Capture(const std::filesystem::path& InRepo) const
    -> audit::RepositoryState {
    audit::RepositoryState state;
    const auto head = Git(InRepo, {"rev-parse", "HEAD"});
    if (head.exitCode == 0) state.headSha = Trim(head.stdoutStr);
    const auto branch = Git(InRepo, {"symbolic-ref", "--quiet", "--short", "HEAD"});
    if (branch.exitCode == 0) state.branch = Trim(branch.stdoutStr);
    const auto status = Git(
        InRepo, {"status", "--porcelain=v1", "-z", "--untracked-files=all"});
    if (status.exitCode == 0) {
        state.worktreeState = Trim(status.stdoutStr).empty()
            ? audit::WorktreeState::Clean : audit::WorktreeState::Dirty;
        state.dirtyFingerprint = audit::Sha256Hex(status.stdoutStr);
    }
    const auto upstream = Git(
        InRepo, {"rev-parse", "--verify", "@{upstream}^{commit}"});
    if (upstream.exitCode == 0) {
        state.upstreamHeadSha = Trim(upstream.stdoutStr);
        const auto counts = Git(
            InRepo, {"rev-list", "--left-right", "--count", "HEAD...@{upstream}"});
        if (counts.exitCode == 0) {
            std::istringstream stream(counts.stdoutStr);
            std::uint64_t ahead = 0;
            std::uint64_t behind = 0;
            if (stream >> ahead >> behind) {
                state.ahead = ahead;
                state.behind = behind;
            } else {
                state.upstreamHeadSha.reset();
            }
        } else {
            state.upstreamHeadSha.reset();
        }
    }
    return state;
}

auto OperationAuditContext::Append(std::string InAction,
                                   const std::filesystem::path& InRepo,
                                   const audit::RepositoryState& InBefore,
                                   std::string InStartedAtUtc,
                                   const int InExitCode,
                                   std::string* OutError) -> bool {
    if (mFinalized || mFinalizationAttempted) {
        if (OutError) *OutError = "cannot append after audit terminalization";
        return false;
    }
    const auto repositoryId = RepositoryIdFor(mSpec.workspaceRoot, InRepo);
    const auto last = std::find_if(mEvents.rbegin(), mEvents.rend(), [&](const auto& event) {
        return event.repository.repositoryId == repositoryId;
    });
    if (last != mEvents.rend() && last->repository.after != InBefore) {
        audit::AuditEvent reconcile;
        reconcile.eventId = "event-" + audit::Sha256Hex(
            mRunId + "\n" + std::to_string(mSpec.correlation.attempt) + "\n" +
            std::to_string(mEvents.size() + 1)).substr(0, 40);
        reconcile.runId = mRunId; reconcile.parentRunId = mParentRunId;
        reconcile.attempt = mSpec.correlation.attempt; reconcile.planId = mSpec.planId;
        reconcile.planSha256 = mPlanSha256; reconcile.sequence = mEvents.size() + 1;
        reconcile.startedAtUtc = InStartedAtUtc; reconcile.finishedAtUtc = InStartedAtUtc;
        reconcile.repository.repositoryId = repositoryId;
        reconcile.repository.before = last->repository.after;
        reconcile.repository.after = InBefore;
        reconcile.phase = "mutation"; reconcile.action = "repository.reconcile-observed";
        reconcile.outcome = OutcomeForExit(0); reconcile.correlation = mCorrelation;
        const auto serialized = audit::SerializeAuditEventJson(reconcile);
        if (!serialized.ok() || !WriteAndSync(mEventsHandle, serialized.json + '\n', OutError)) {
            mPoisoned = true;
            if (OutError && OutError->empty()) *OutError = "cannot persist repository reconciliation";
            std::string ignored;
            PublishIncompleteMarker("event-append-failed", false, &ignored);
            return false;
        }
        mEvents.push_back(std::move(reconcile));
    }

    audit::AuditEvent event;
    event.eventId = "event-" + audit::Sha256Hex(
        mRunId + "\n" + std::to_string(mSpec.correlation.attempt) + "\n" +
        std::to_string(mEvents.size() + 1)).substr(0, 40);
    event.runId = mRunId; event.parentRunId = mParentRunId;
    event.attempt = mSpec.correlation.attempt; event.planId = mSpec.planId;
    event.planSha256 = mPlanSha256; event.sequence = mEvents.size() + 1;
    event.startedAtUtc = std::move(InStartedAtUtc); event.finishedAtUtc = CurrentUtc();
    event.repository.repositoryId = repositoryId;
    event.repository.before = InBefore; event.repository.after = Capture(InRepo);
    event.phase = PhaseForAction(InAction); event.action = std::move(InAction);
    event.outcome = OutcomeForExit(InExitCode); event.correlation = mCorrelation;
    event.artifacts = mSupplementalInputs;
    const auto serialized = audit::SerializeAuditEventJson(event);
    if (!serialized.ok() || !WriteAndSync(mEventsHandle, serialized.json + '\n', OutError)) {
        mPoisoned = true;
        if (OutError && OutError->empty()) *OutError = "observed audit event is invalid";
        std::string ignored;
        PublishIncompleteMarker("event-append-failed", false, &ignored);
        return false;
    }
    mEvents.push_back(std::move(event));
    return true;
}

auto OperationAuditContext::Finalize(const int InExitCode, std::string* OutError) -> bool {
    if (mFinalized || mFinalizationAttempted) {
        if (OutError) *OutError = "audit receipt already finalized or attempted";
        return false;
    }
    return PublishReceipt(InExitCode, OutError);
}

auto OperationAuditContext::PublishIncompleteMarker(
    const std::string_view InReasonCode,
    const bool InReceiptPublished,
    std::string* OutError) -> bool {
    if (mIncompletePublished) return true;
    const auto reason = audit::IsStableAuditId(InReasonCode)
        ? std::string(InReasonCode) : std::string("audit-publication-failed");
    const nlohmann::json marker = {
        {"schemaName", "kog.auditIncomplete"},
        {"schemaVersion", 1},
        {"runId", mRunId},
        {"parentRunId", mParentRunId ? nlohmann::json(*mParentRunId)
                                      : nlohmann::json(nullptr)},
        {"attempt", mSpec.correlation.attempt},
        {"planId", mSpec.planId},
        {"planSha256", mPlanSha256},
        {"reasonCode", reason},
        {"recoverable", true},
        {"receiptPublished", InReceiptPublished},
        {"observedEventCount", mEvents.size()},
        {"recordedAtUtc", CurrentUtc()},
    };
    auto handle = OpenExclusiveFile(mPaths.incomplete, OutError);
    if (handle < 0) {
        std::error_code ec;
        if (std::filesystem::is_regular_file(mPaths.incomplete, ec) && !ec) {
            mIncompletePublished = true;
            return true;
        }
        return false;
    }
    const auto bytes = marker.dump() + '\n';
    const bool written = WriteAndSync(handle, bytes, OutError);
    CloseHandleValue(handle);
    if (!written || !SyncDirectory(mPaths.attemptRoot, OutError)) return false;
    mIncompletePublished = true;
    return true;
}

auto OperationAuditContext::PublishReceipt(const int InExitCode, std::string* OutError) -> bool {
    mFinalizationAttempted = true;
    const auto fail = [&](const std::string_view InReasonCode,
                          const std::string_view InMessage) -> bool {
        if (OutError && !InMessage.empty() && OutError->empty()) {
            *OutError = std::string(InMessage);
        }
        std::string ignored;
        PublishIncompleteMarker(InReasonCode, false, &ignored);
        return false;
    };
    if (mPoisoned) {
        if (OutError) *OutError = "cannot publish a receipt after an audit append failure";
        return fail("event-stream-incomplete", {});
    }
    if (mEvents.empty()) {
        return fail("event-stream-empty", "cannot finalize audit run without events");
    }
    const auto canonicalEvents = audit::SerializeAuditEventsJsonl(mEvents);
    if (!canonicalEvents.ok()) {
        return fail("event-stream-invalid", "event stream validation failed");
    }
    const auto persisted = ReadHandleAll(mEventsHandle, OutError);
    if (!persisted || *persisted != canonicalEvents.json) {
        if (OutError && (persisted || OutError->empty()))
            *OutError = "persisted audit stream differs from observed events";
        return fail("event-stream-mismatch", {});
    }
    const auto parsed = audit::ParseAuditEventsJsonl(*persisted);
    if (!parsed.ok() || parsed.values != mEvents) {
        return fail("event-stream-invalid", "persisted audit stream failed closed validation");
    }
    audit::RunReceipt receipt;
    receipt.runId = mRunId; receipt.parentRunId = mParentRunId;
    receipt.attempt = mSpec.correlation.attempt; receipt.planId = mSpec.planId;
    receipt.planSha256 = mPlanSha256; receipt.startedAtUtc = mStartedAtUtc;
    receipt.finishedAtUtc = CurrentUtc(); receipt.firstSequence = 1;
    receipt.lastSequence = mEvents.size(); receipt.eventCount = mEvents.size();
    receipt.eventStreamSha256 = audit::Sha256Hex(canonicalEvents.json);
    receipt.terminalOutcome = OutcomeForExit(InExitCode); receipt.correlation = mCorrelation;
    receipt.artifacts = mSupplementalInputs;
    std::map<std::string, audit::RepositoryTransition> aggregate;
    for (const auto& event : mEvents) {
        auto [it, inserted] = aggregate.try_emplace(
            event.repository.repositoryId, event.repository);
        if (!inserted) it->second.after = event.repository.after;
    }
    for (auto& [id, transition] : aggregate)
        receipt.repositories.push_back(std::move(transition));
    if (!audit::ValidateRunTrace(receipt, mEvents).ok()) {
        return fail("receipt-reconciliation-failed",
                    "terminal receipt does not reconcile with observed events");
    }
    const auto serialized = audit::SerializeRunReceiptJson(receipt);
    if (!serialized.ok() || !WriteAndSync(mReceiptHandle, serialized.json, OutError)) {
        if (OutError && OutError->empty()) *OutError = "terminal receipt validation failed";
        return fail("receipt-write-failed", {});
    }
    const auto persistedBeforePublish = ReadHandleAll(mEventsHandle, OutError);
    if (!persistedBeforePublish || *persistedBeforePublish != canonicalEvents.json) {
        if (OutError && (persistedBeforePublish || OutError->empty()))
            *OutError = "persisted audit stream changed before receipt publication";
        return fail("event-stream-changed", {});
    }
    CloseHandleValue(mReceiptHandle);
    bool receiptPublished = false;
    if (!PublishNoReplace(mPaths.receipt.string() + ".tmp", mPaths.receipt,
                          &receiptPublished, OutError)) {
        std::string ignored;
        PublishIncompleteMarker(
            receiptPublished ? "receipt-durability-uncertain"
                             : "receipt-publication-failed",
            receiptPublished, &ignored);
        return false;
    }
    const auto* injectPostPublishSync = std::getenv(
        "KOG_TEST_ONLY_AUDIT_FAIL_POST_PUBLISH_DIR_SYNC");
    if (TestModeEnabled() && injectPostPublishSync != nullptr &&
        std::string_view(injectPostPublishSync) == "1") {
        if (OutError)
            *OutError = "injected post-publication directory sync failure";
        std::string ignored;
        PublishIncompleteMarker("receipt-durability-uncertain", true, &ignored);
        return false;
    }
    if (!SyncRegularFilePath(mPaths.receipt, OutError) ||
        !SyncDirectory(mPaths.attemptRoot, OutError)) {
        std::string ignored;
        PublishIncompleteMarker("receipt-durability-uncertain", true, &ignored);
        return false;
    }

    // The receipt is now file-flushed and its directory entry is durable, but
    // publication is not complete until the durable pending sentinel clears.
    // The bounded test-only pause makes this rejection window observable.
    TestOnlyDelayFromEnvironment(
        "KOG_TEST_ONLY_AUDIT_POST_PUBLISH_DELAY_MS");
    const auto* injectPendingClear = std::getenv(
        "KOG_TEST_ONLY_AUDIT_FAIL_PENDING_CLEAR");
    if (TestModeEnabled() && injectPendingClear != nullptr &&
        std::string_view(injectPendingClear) == "1") {
        if (OutError)
            *OutError = "injected publication-pending clear failure";
        std::string ignored;
        PublishIncompleteMarker("receipt-publication-pending", true, &ignored);
        return false;
    }
    if (!RemoveRegularFileAndSyncDirectory(
            mPaths.publicationPending, OutError)) {
        std::string ignored;
        PublishIncompleteMarker("receipt-publication-pending", true, &ignored);
        return false;
    }
    mFinalized = true;
    return true;
}

auto OperationAuditContext::FreezeSupplementalInput(
    const std::filesystem::path& InSource,
    const std::string_view InLabel,
    const std::string_view InIdentity,
    std::string* OutError) -> std::optional<std::filesystem::path> {
    const auto bytes = ReadBoundedAuditInput(InSource, 4U << 20U, OutError);
    if (!bytes) return std::nullopt;
    return FreezeSupplementalBytes(*bytes, InLabel, InIdentity, OutError);
}

auto OperationAuditContext::FreezeSupplementalBytes(
    const std::string_view InBytes,
    const std::string_view InLabel,
    const std::string_view InIdentity,
    std::string* OutError) -> std::optional<std::filesystem::path> {
    if (InBytes.empty() || InBytes.size() > (4U << 20U)) {
        if (OutError) *OutError = "supplemental audit input is empty or oversized";
        return std::nullopt;
    }
    const auto sha256 = audit::Sha256Hex(InBytes);
    if (sha256 == mPlanSha256) return mPaths.frozenInput;
    std::string label;
    for (const char ch : InLabel) {
        if ((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '-')
            label.push_back(ch);
    }
    if (label.empty()) label = "input";
    std::string identity = InIdentity.empty() ? label : std::string(InIdentity);
    if (!audit::IsStableAuditId(identity)) {
        if (OutError) *OutError = "supplemental input identity violates the stable-ID grammar";
        return std::nullopt;
    }
    if (identity.size() > 100) identity.resize(100);
    const auto artifactId = identity + "-" + sha256.substr(0, 20);
    const auto path = mPaths.attemptRoot /
        ("frozen-" + label + "-" + sha256.substr(0, 20) + ".json");
    std::error_code ec;
    if (std::filesystem::exists(path, ec) && !ec) {
        const auto existing = ReadBoundedAuditInput(path, 4U << 20U, OutError);
        if (existing && *existing == InBytes) {
            const auto found = std::find_if(
                mSupplementalInputs.begin(), mSupplementalInputs.end(),
                [&](const auto& artifact) { return artifact.id == artifactId; });
            if (found != mSupplementalInputs.end()) return path;
        }
        if (OutError && OutError->empty()) *OutError = "supplemental frozen input collision";
        return std::nullopt;
    }
    auto handle = OpenExclusiveFile(path, OutError);
    if (handle < 0 || !WriteAndSync(handle, InBytes, OutError)) {
        CloseHandleValue(handle);
        return std::nullopt;
    }
    CloseHandleValue(handle);
    if (!SyncDirectory(mPaths.attemptRoot, OutError)) return std::nullopt;
    audit::ArtifactReference artifact;
    artifact.id = artifactId;
    artifact.kind = "frozen-" + label;
    artifact.sha256 = sha256;
    artifact.sizeBytes = InBytes.size();
    artifact.contentType = "application/json";
    artifact.redactionStatus = audit::RedactionStatus::Withheld;
    mSupplementalInputs.push_back(std::move(artifact));
    CanonicalizeArtifacts(mSupplementalInputs);
    const auto before = Capture(mSpec.workspaceRoot);
    if (!Append("audit.input.freeze", mSpec.workspaceRoot, before, CurrentUtc(), 0,
                OutError)) {
        return std::nullopt;
    }
    return path;
}

auto OperationAuditContext::FrozenInputPath() const -> const std::filesystem::path& { return mPaths.frozenInput; }
auto OperationAuditContext::SourceSha256() const -> const std::string& { return mSourceSha256; }
auto OperationAuditContext::PlanSha256() const -> const std::string& { return mPlanSha256; }
auto OperationAuditContext::RunId() const -> const std::string& { return mRunId; }
auto OperationAuditContext::ParentRunId() const -> const std::optional<std::string>& { return mParentRunId; }
auto OperationAuditContext::Attempt() const -> std::uint32_t { return mSpec.correlation.attempt; }
auto OperationAuditContext::InputKind() const -> const std::string& { return mSpec.inputKind; }
auto OperationAuditContext::Route() const -> const std::string& { return mSpec.route; }
auto OperationAuditContext::PlanId() const -> const std::string& { return mSpec.planId; }
auto OperationAuditContext::Correlation() const -> const OperationCorrelationEnvelope& { return mSpec.correlation; }
auto OperationAuditContext::Paths() const -> const OperationAuditPaths& { return mPaths; }

auto OperationAuditCapabilityJson() -> std::string {
    const nlohmann::json doc = {
        {"schemaName", kOperationAuditCapabilitySchema}, {"schemaVersion", 1},
        {"protocolVersion", kOperationAuditProtocolVersion},
        {"correlationEnvelopeVersions", {1}}, {"auditEventVersions", {1}},
        {"runReceiptVersions", {1}}, {"auditVerificationVersions", {1}},
        {"supportedInputs", nlohmann::json::array({
            {{"route", "commit.plan"}, {"inputKind", "commit-plan"}},
            {{"route", "commit-push.plan"}, {"inputKind", "commit-plan"}},
            {{"route", "plan.apply"}, {"inputKind", "commit-plan"}},
            {{"route", "converge.repos"}, {"inputKind", "operation-descriptor"}},
            {{"route", "converge.branches.apply"}, {"inputKind", "operation-descriptor"}},
            {{"route", "converge.branches.recover"}, {"inputKind", "operation-descriptor"}},
            {{"route", "converge.branches.retire"}, {"inputKind", "operation-descriptor"}}
        })},
        {"provenanceGrantsAuthority", false},
        {"durability", {{"fileFlush", "required"},
                         {"directorySync", "required-posix-best-effort-windows"}}},
    };
    return doc.dump() + '\n';
}

namespace {

enum class LegacyMarkerProbe { Absent, Present, Rejected };

// `audit verify` predates the closed marker schema used by the public reader.
// Preserve its intentionally narrow marker contract here instead of exposing
// that permissive shape through ReadOperationAuditRun's default policy.
auto ProbeLegacyMarker(const AuditEvidenceDirectory& InDirectory,
                       const std::string_view InName,
                       const std::string_view InSchemaName,
                       const std::string_view InRunId,
                       const std::uint32_t InAttempt,
                       const std::string_view InPresentDiagnostic,
                       const std::string_view InInvalidDiagnostic,
                       const std::string_view InAbsenceDiagnostic,
                       const std::string_view InUnreadableDiagnostic,
                       std::string* OutError) -> LegacyMarkerProbe {
    const auto probe = InDirectory.Probe(InName);
    if (probe.code == AuditEvidenceReadCode::Missing)
        return LegacyMarkerProbe::Absent;
    if (!probe.ready() && probe.code != AuditEvidenceReadCode::LoopOrReparse &&
        probe.code != AuditEvidenceReadCode::NonRegular) {
        if (OutError) *OutError = std::string(InAbsenceDiagnostic);
        return LegacyMarkerProbe::Rejected;
    }
    const auto input = InDirectory.Read(InName, 64U << 10U);
    if (!input.ready()) {
        if (OutError) *OutError = input.diagnostic.empty()
            ? std::string(InUnreadableDiagnostic) : input.diagnostic;
        return LegacyMarkerProbe::Rejected;
    }
    try {
        const auto doc = nlohmann::json::parse(input.bytes);
        if (!doc.is_object() ||
            doc.value("schemaName", "") != InSchemaName ||
            doc.value("schemaVersion", 0) != 1 ||
            doc.value("runId", "") != InRunId ||
            doc.value("attempt", 0U) != InAttempt) {
            if (OutError) *OutError = std::string(InInvalidDiagnostic);
            return LegacyMarkerProbe::Rejected;
        }
    } catch (...) {
        if (OutError) *OutError = std::string(InInvalidDiagnostic);
        return LegacyMarkerProbe::Rejected;
    }
    if (OutError) *OutError = std::string(InPresentDiagnostic);
    return LegacyMarkerProbe::Present;
}

auto LegacyMarkersBlockVerification(const AuditEvidenceDirectory& InDirectory,
                                    const OperationAuditPaths& InPaths,
                                    const std::string_view InRunId,
                                    const std::uint32_t InAttempt,
                                    std::string* OutError) -> bool {
    // Preserve the original eager evaluation order. Both marker probes run
    // before their OR result is considered, so an incomplete diagnostic
    // overwrites a pending diagnostic when both markers are present.
    const auto pending = ProbeLegacyMarker(
        InDirectory, InPaths.publicationPending.filename().string(),
        "kog.auditPublicationPending", InRunId, InAttempt,
        "audit receipt publication is still pending",
        "audit publication-pending sentinel is invalid",
        "cannot prove absence of audit publication-pending sentinel",
        "audit publication-pending sentinel is unreadable", OutError);
    const auto incomplete = ProbeLegacyMarker(
        InDirectory, InPaths.incomplete.filename().string(),
        "kog.auditIncomplete", InRunId, InAttempt,
        "audit evidence is explicitly incomplete", "audit incomplete marker is invalid",
        "cannot prove absence of audit incomplete marker", "audit incomplete marker is unreadable",
        OutError);
    return pending != LegacyMarkerProbe::Absent ||
        incomplete != LegacyMarkerProbe::Absent;
}

auto LegacyMissingEvidence(const AuditEvidenceDirectory& InDirectory,
                           const OperationAuditPaths& InPaths,
                           std::string* OutError) -> bool {
    // Preserve the old sequential ReadBoundedAuditInput diagnostics, including
    // the underlying OS error text for a missing child evidence file.
    const auto events = InDirectory.Read(InPaths.events.filename().string(), 64U << 20U);
    if (!events.ready()) {
        if (OutError) *OutError = events.diagnostic.empty()
            ? std::string(AuditEvidenceSystemDiagnostic(events.code)) : events.diagnostic;
        return true;
    }
    const auto receipt = InDirectory.Read(InPaths.receipt.filename().string(), 4U << 20U);
    if (!receipt.ready()) {
        if (OutError) *OutError = receipt.diagnostic.empty()
            ? std::string(AuditEvidenceSystemDiagnostic(receipt.code)) : receipt.diagnostic;
        return true;
    }
    const auto frozen = InDirectory.Read(InPaths.frozenInput.filename().string(), 4U << 20U);
    if (!frozen.ready()) {
        if (OutError) *OutError = frozen.diagnostic.empty()
            ? std::string(AuditEvidenceSystemDiagnostic(frozen.code)) : frozen.diagnostic;
        return true;
    }
    return false;
}

auto LegacyDirectoryOpenDiagnostic(const AuditEvidenceOpenCode InCode)
    -> std::string {
    if (InCode == AuditEvidenceOpenCode::Missing) {
#if defined(_WIN32)
        // ReadBoundedAuditInput on a child whose parent attempt/root is absent
        // reports ERROR_PATH_NOT_FOUND, not ERROR_FILE_NOT_FOUND.
        return "cannot open bounded audit input: win32 error 3";
#else
        return std::string("cannot open bounded audit input: ") +
            std::strerror(ENOENT);
#endif
    }
    if (InCode == AuditEvidenceOpenCode::PermissionDenied ||
        InCode == AuditEvidenceOpenCode::IoError ||
        InCode == AuditEvidenceOpenCode::StatFailed ||
        InCode == AuditEvidenceOpenCode::NotDirectory) {
        // Both legacy marker probes ran before evidence reads; the incomplete
        // probe was last and therefore owned the final diagnostic.
        return "cannot prove absence of audit incomplete marker";
    }
    return std::string(AuditEvidenceSystemDiagnostic(InCode));
}

} // namespace

auto VerifyOperationAuditJson(const OperationAuditSpec& InSpec,
                              const std::string_view InRunId,
                              const std::uint32_t InAttempt,
                              bool* OutTraceValid,
                              std::string* OutError) -> std::optional<std::string> {
    if (OutTraceValid) *OutTraceValid = false;
    std::string pathError;
    const auto legacyPaths = ResolveOperationAuditPaths(InSpec, InRunId, InAttempt, &pathError);
    if (!legacyPaths) {
        if (OutError) *OutError = pathError;
        return std::nullopt;
    }
    AuditEvidenceOpenCode legacyOpenCode = AuditEvidenceOpenCode::IoError;
    auto legacyDirectory = AuditEvidenceDirectory::Open(*legacyPaths, &legacyOpenCode);
    if (!legacyDirectory) {
        if (OutError) *OutError = LegacyDirectoryOpenDiagnostic(legacyOpenCode);
        return std::nullopt;
    }
    if (LegacyMarkersBlockVerification(*legacyDirectory, *legacyPaths,
                                       InRunId, InAttempt, OutError))
        return std::nullopt;
    OperationAuditRunReadLimits limits;
    limits.maxPreviewBytes = 4U << 20U;
    limits.maxEventRecords = 0;
    limits.maxRepositories = 256;
    limits.maxEvidenceReferences = 128;
    // `audit verify` has always located a receipt by run/attempt.  Its
    // synthetic standalone correlation is deliberately a lookup wildcard,
    // not a demand that a KOA receipt be standalone.  Keep that compatibility
    // boundary explicit rather than weakening normal reader calls.
    OperationAuditRunReadPolicy readPolicy;
    // Legacy verification locates markers by run/attempt and accepts any
    // compatible route for the closed input kind. A standalone CLI lookup is
    // also a caller-correlation wildcard; a real KOA spec remains exact.
    readPolicy.markerMatch = OperationAuditMarkerMatchPolicy::IdentityOnly;
    readPolicy.callerCorrelation = InSpec.correlation.mode == "standalone"
        ? OperationAuditCallerCorrelationPolicy::StandaloneWildcard
        : OperationAuditCallerCorrelationPolicy::LegacyVerify;
    readPolicy.routeBinding =
        OperationAuditRouteBindingPolicy::CompatibleInputKind;
    const auto read = ReadOperationAuditRunFromPinnedDirectory(
        *legacyDirectory, *legacyPaths, InSpec, InRunId, InAttempt,
        limits, std::nullopt, readPolicy);
    if (!read.verified()) {
        const auto markerResult = [](const OperationAuditRunReadCode code) {
            switch (code) {
            case OperationAuditRunReadCode::PublicationPending:
            case OperationAuditRunReadCode::EvidenceIncomplete:
            case OperationAuditRunReadCode::PendingMarkerUnreadable:
            case OperationAuditRunReadCode::PendingMarkerInvalid:
            case OperationAuditRunReadCode::IncompleteMarkerUnreadable:
            case OperationAuditRunReadCode::IncompleteMarkerInvalid:
            case OperationAuditRunReadCode::PendingMarkerProbeFailed:
            case OperationAuditRunReadCode::IncompleteMarkerProbeFailed:
                return true;
            default:
                return false;
            }
        };
        // The marker may have appeared or changed after the pre-read legacy
        // probe. Reclassify through the legacy wrapper before mapping it.
        if (markerResult(read.code) &&
            LegacyMarkersBlockVerification(*legacyDirectory, *legacyPaths,
                                           InRunId, InAttempt, OutError)) {
            return std::nullopt;
        }
        if ((read.code == OperationAuditRunReadCode::AttemptMissing ||
             read.code == OperationAuditRunReadCode::EvidenceMissing) &&
            LegacyMissingEvidence(*legacyDirectory, *legacyPaths, OutError)) {
            return std::nullopt;
        }
        if (OutError) {
            switch (read.code) {
            case OperationAuditRunReadCode::PublicationPending:
                *OutError = "audit receipt publication is still pending";
                break;
            case OperationAuditRunReadCode::PendingMarkerProbeFailed:
                *OutError = "cannot prove absence of audit publication-pending sentinel";
                break;
            case OperationAuditRunReadCode::PendingMarkerUnreadable:
                *OutError = "audit publication-pending sentinel is unreadable";
                break;
            case OperationAuditRunReadCode::PendingMarkerInvalid:
                *OutError = "audit publication-pending sentinel is invalid";
                break;
            case OperationAuditRunReadCode::EvidenceIncomplete:
                *OutError = "audit evidence is explicitly incomplete";
                break;
            case OperationAuditRunReadCode::IncompleteMarkerProbeFailed:
                *OutError = "cannot prove absence of audit incomplete marker";
                break;
            case OperationAuditRunReadCode::IncompleteMarkerUnreadable:
                *OutError = "audit incomplete marker is unreadable";
                break;
            case OperationAuditRunReadCode::IncompleteMarkerInvalid:
                *OutError = "audit incomplete marker is invalid";
                break;
            case OperationAuditRunReadCode::AttemptMissing:
            case OperationAuditRunReadCode::EvidenceMissing:
                *OutError = "audit evidence is missing";
                break;
            case OperationAuditRunReadCode::PlanMismatch:
                *OutError = "current admitted plan id does not match the receipt";
                break;
            case OperationAuditRunReadCode::MultipleFrozenBindings:
                *OutError = "multiple main frozen input bindings are present";
                break;
            case OperationAuditRunReadCode::FrozenBindingMissing:
                *OutError = "main frozen route/input binding is missing or contradictory";
                break;
            case OperationAuditRunReadCode::SourceNotAdmitted:
            case OperationAuditRunReadCode::BindingMismatch:
                *OutError = "current plan bytes are not an admitted source state";
                break;
            case OperationAuditRunReadCode::FrozenCommitPlanIdentityMismatch:
                *OutError = "frozen commit-plan identity is contradictory";
                break;
            case OperationAuditRunReadCode::FrozenOperationIdentityMismatch:
                *OutError = "frozen operation descriptor identity is contradictory";
                break;
            case OperationAuditRunReadCode::FrozenCorrelationMismatch:
                *OutError = "frozen input correlation is contradictory";
                break;
            case OperationAuditRunReadCode::HashMismatch:
                *OutError = "frozen input hash does not match the receipt";
                break;
            case OperationAuditRunReadCode::ReceiptMismatch:
                *OutError = "audit receipt identity does not match requested run";
                break;
            case OperationAuditRunReadCode::CorrelationMismatch:
                *OutError = "requested correlation does not match verified evidence";
                break;
            case OperationAuditRunReadCode::UnsupportedSchema:
                *OutError = "audit evidence failed closed trace validation";
                break;
            case OperationAuditRunReadCode::FrozenOperationUnsupportedSchema:
                *OutError = "frozen operation descriptor identity is contradictory";
                break;
            case OperationAuditRunReadCode::InputLimit:
                *OutError = "audit evidence exceeds a bounded verification limit";
                break;
            case OperationAuditRunReadCode::MalformedEvidence:
            case OperationAuditRunReadCode::TraceInvalid:
                *OutError = "audit evidence failed closed trace validation";
                break;
            default:
                *OutError = "audit evidence failed closed verification";
                break;
            }
        }
        return std::nullopt;
    }

    if (LegacyMarkersBlockVerification(*legacyDirectory, *legacyPaths,
                                       InRunId, InAttempt, OutError))
        return std::nullopt;

    const auto& run = *read.run;
    const auto nullable = [](const auto& value) {
        return value ? nlohmann::json(*value) : nlohmann::json(nullptr);
    };
    const nlohmann::json correlation = {
        {"mode", audit::CorrelationModeName(run.correlation.mode)},
        {"productId", nullable(run.correlation.productId)}, {"topicId", nullable(run.correlation.topicId)},
        {"itemId", nullable(run.correlation.itemId)}, {"workOrderId", nullable(run.correlation.workOrderId)},
        {"requestId", nullable(run.correlation.requestId)}, {"producerId", nullable(run.correlation.producerId)},
        {"routeId", nullable(run.correlation.routeId)}, {"agentId", nullable(run.correlation.agentId)},
    };
    const nlohmann::json outcome = {
        {"status", audit::OutcomeStateName(run.terminalOutcome.status)},
        {"exitCode", nullable(run.terminalOutcome.exitCode)},
        {"reasonCode", nullable(run.terminalOutcome.reasonCode)},
        {"retryable", run.terminalOutcome.retryable},
    };
    nlohmann::json repositories = nlohmann::json::array();
    for (const auto& repository : run.repositories)
        repositories.push_back(TransitionJson(repository));
    nlohmann::json artifacts = nlohmann::json::array();
    for (const auto& artifact : run.evidence) {
        if (artifact.category != "artifact") continue;
        artifacts.push_back({{"id", artifact.id}, {"kind", artifact.kind},
                             {"sha256", artifact.sha256},
                             {"sizeBytes", artifact.sizeBytes},
                             {"contentType", artifact.contentType},
                             {"redactionStatus",
                              audit::RedactionStatusName(artifact.redactionStatus)}});
    }
    const nlohmann::json doc = {
        {"schemaName", kOperationAuditVerificationSchema}, {"schemaVersion", 1},
        {"ok", true}, {"error", nullptr}, {"traceValid", true},
        {"eventsValid", true}, {"receiptValid", true}, {"frozenInputValid", true},
        {"planId", run.planId}, {"planSha256", run.planSha256},
        {"frozenInputSha256", run.frozenInputSha256},
        {"runId", run.runId}, {"parentRunId", nullable(run.parentRunId)},
        {"attempt", run.attempt}, {"correlation", correlation},
        {"eventCount", run.totalEventRecords}, {"eventStreamSha256", run.eventStreamSha256},
        {"receiptSha256", run.receiptId}, {"terminalOutcome", outcome},
        {"repositories", repositories}, {"artifacts", artifacts},
    };
    if (OutTraceValid) *OutTraceValid = true;
    return doc.dump() + '\n';
}

} // namespace kano::git::commands
