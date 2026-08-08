#include "commit_plan_execution_admission.hpp"

#include "commit_plan_audit.hpp"
#include "plan_utils.hpp"
#include "runtime_path_layout.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <format>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace kano::git::commands {
namespace {

auto CurrentUtcTimestampIso8601() -> std::string {
    const auto now = std::chrono::system_clock::now();
    const auto time = std::chrono::system_clock::to_time_t(now);
    std::tm utc{};
#if defined(_WIN32)
    gmtime_s(&utc, &time);
#else
    gmtime_r(&time, &utc);
#endif
    std::ostringstream out;
    out << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ");
    return out.str();
}

auto TestModeEnabled() -> bool {
    const auto* value = std::getenv("KOG_TEST_MODE");
    return value != nullptr && std::string(value) == "1";
}

auto ResolveTestHookPath(const std::filesystem::path& InWorkspaceRoot,
                         const char* InRawPath)
    -> std::optional<std::filesystem::path> {
    if (!TestModeEnabled() || InRawPath == nullptr || *InRawPath == '\0')
        return std::nullopt;
    const auto allowedRoot =
        (runtime_path::Layout::Resolve(InWorkspaceRoot)
             .WorkspaceGitTemporaryRoot() /
         "plan-execution-test-hooks")
            .lexically_normal();
    std::error_code ec;
    std::filesystem::create_directories(allowedRoot, ec);
    if (ec) return std::nullopt;
    const auto canonicalRoot = std::filesystem::weakly_canonical(allowedRoot, ec);
    if (ec) return std::nullopt;
    const auto canonicalCandidate = std::filesystem::weakly_canonical(
        std::filesystem::path(InRawPath), ec);
    if (ec) return std::nullopt;
    const auto relative = canonicalCandidate.lexically_relative(canonicalRoot);
    if (relative.empty() || relative.is_absolute()) return std::nullopt;
    for (const auto& component : relative) {
        if (component == "..") return std::nullopt;
    }
    return canonicalCandidate;
}

auto PlanExecutionLockKey(const std::filesystem::path& InPlanPath,
                          std::string* OutError)
    -> std::optional<std::string> {
#if defined(_WIN32)
    const auto sourceHandle = CreateFileW(
        InPlanPath.wstring().c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (sourceHandle == INVALID_HANDLE_VALUE) {
        if (OutError)
            *OutError = "cannot open plan source identity for execution lock";
        return std::nullopt;
    }
    BY_HANDLE_FILE_INFORMATION info{};
    const bool valid = GetFileInformationByHandle(sourceHandle, &info) != 0 &&
        (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0 &&
        (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
    CloseHandle(sourceHandle);
    if (!valid) {
        if (OutError)
            *OutError = "plan execution lock source must be a regular "
                        "non-reparse file";
        return std::nullopt;
    }
    const auto fileIndex =
        (static_cast<std::uint64_t>(info.nFileIndexHigh) << 32U) |
        static_cast<std::uint64_t>(info.nFileIndexLow);
    return std::format("win-{:08x}-{:016x}", info.dwVolumeSerialNumber,
                       fileIndex);
#else
    int sourceFlags = O_RDONLY;
#if defined(O_CLOEXEC)
    sourceFlags |= O_CLOEXEC;
#endif
#if defined(O_NOFOLLOW)
    sourceFlags |= O_NOFOLLOW;
#endif
    const auto sourceHandle = ::open(InPlanPath.c_str(), sourceFlags);
    if (sourceHandle < 0) {
        if (OutError)
            *OutError = "cannot open plan source identity for execution lock";
        return std::nullopt;
    }
    struct stat info {};
    const bool valid = ::fstat(sourceHandle, &info) == 0 &&
        S_ISREG(info.st_mode);
    (void)::close(sourceHandle);
    if (!valid) {
        if (OutError)
            *OutError = "plan execution lock source must be a regular file";
        return std::nullopt;
    }
    return std::format(
        "posix-{:x}-{:x}", static_cast<std::uintmax_t>(info.st_dev),
        static_cast<std::uintmax_t>(info.st_ino));
#endif
}

struct PlanExecutionLockLocation {
    std::filesystem::path root;
    std::filesystem::path anchorRoot;
    std::vector<std::filesystem::path> directoryComponents;
};

auto ResolvePlanExecutionLockRoot(
    const std::filesystem::path& InWorkspaceRoot,
    std::string* OutError) -> std::optional<PlanExecutionLockLocation> {
    const auto gitDirectoryResult = GitCapture(
        InWorkspaceRoot,
        {"rev-parse", "--git-common-dir"});
    const auto gitDirectoryText = Trim(gitDirectoryResult.stdoutStr);
    const bool forceProbeFailure =
        TestModeEnabled() &&
        [] {
            const auto* value = std::getenv(
                "KOG_TEST_ONLY_PLAN_EXECUTION_COMMON_DIR_FAILURE");
            return value != nullptr && std::string_view(value) == "1";
        }();
    if (!forceProbeFailure && gitDirectoryResult.exitCode == 0 &&
        !gitDirectoryText.empty()) {
        std::error_code ec;
        auto gitDirectoryCandidate = std::filesystem::path(gitDirectoryText);
        if (gitDirectoryCandidate.is_relative())
            gitDirectoryCandidate = InWorkspaceRoot / gitDirectoryCandidate;
        gitDirectoryCandidate =
            std::filesystem::absolute(gitDirectoryCandidate, ec);
        if (ec) {
            if (OutError)
                *OutError = "cannot resolve safe Git metadata directory for "
                            "plan execution locks";
            return std::nullopt;
        }
        const auto gitDirectory = std::filesystem::weakly_canonical(
            gitDirectoryCandidate, ec);
        if (ec || gitDirectory.empty() || !gitDirectory.is_absolute()) {
            if (OutError)
                *OutError = "cannot resolve safe Git metadata directory for "
                            "plan execution locks";
            return std::nullopt;
        }
        ec.clear();
        if (!std::filesystem::is_directory(gitDirectory, ec) || ec) {
            if (OutError)
                *OutError = "cannot resolve safe Git metadata directory for "
                            "plan execution locks";
            return std::nullopt;
        }

        PlanExecutionLockLocation location;
        location.anchorRoot = gitDirectory;
        location.directoryComponents = {"kog", "plan-execution-locks"};
        location.root =
            (gitDirectory / "kog" / "plan-execution-locks")
                .lexically_normal();
        return location;
    }

    const auto repositoryProbe =
        GitCapture(InWorkspaceRoot, {"rev-parse", "--is-inside-work-tree"});
    std::error_code markerError;
    const auto gitMarkerStatus = std::filesystem::symlink_status(
        InWorkspaceRoot / ".git", markerError);
    const bool hasGitMarker =
        !markerError &&
        gitMarkerStatus.type() != std::filesystem::file_type::not_found;
    if (repositoryProbe.exitCode == 0 || hasGitMarker) {
        if (OutError)
            *OutError = "cannot resolve common Git metadata directory for "
                        "plan execution locks";
        return std::nullopt;
    }

    std::error_code ec;
    const auto workspaceRoot =
        std::filesystem::weakly_canonical(InWorkspaceRoot, ec);
    if (ec || workspaceRoot.empty() || !workspaceRoot.is_absolute()) {
        if (OutError)
            *OutError = "cannot resolve non-Git workspace root for plan "
                        "execution locks";
        return std::nullopt;
    }
    ec.clear();
    if (!std::filesystem::is_directory(workspaceRoot, ec) || ec) {
        if (OutError)
            *OutError = "non-Git plan execution lock root must be a directory";
        return std::nullopt;
    }
    PlanExecutionLockLocation location;
    location.anchorRoot = workspaceRoot;
    location.root =
        (runtime_path::Layout::Resolve(workspaceRoot)
             .WorkspaceGitTemporaryRoot() /
         "plan-execution-locks")
            .lexically_normal();
    const auto relativeRoot = location.root.lexically_relative(workspaceRoot);
    if (relativeRoot.empty() || relativeRoot.is_absolute()) {
        if (OutError)
            *OutError = "cannot derive non-Git plan execution lock layout";
        return std::nullopt;
    }
    for (const auto& component : relativeRoot) {
        if (component.empty() || component == "." || component == "..") {
            if (OutError)
                *OutError = "cannot derive non-Git plan execution lock layout";
            return std::nullopt;
        }
        location.directoryComponents.push_back(component);
    }
    return location;
}

#if defined(_WIN32)
auto OpenVerifiedDirectoryHandle(const std::filesystem::path& InPath,
                                 std::string* OutError) -> HANDLE {
    const auto handle = CreateFileW(
        InPath.wstring().c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        if (OutError)
            *OutError = "cannot open plan execution lock directory";
        return INVALID_HANDLE_VALUE;
    }
    FILE_ATTRIBUTE_TAG_INFO tagInfo{};
    if (GetFileInformationByHandleEx(handle, FileAttributeTagInfo, &tagInfo,
                                     sizeof(tagInfo)) == 0 ||
        (tagInfo.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
        (tagInfo.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        CloseHandle(handle);
        if (OutError)
            *OutError = "plan execution lock directory must be a real "
                        "non-reparse directory";
        return INVALID_HANDLE_VALUE;
    }
    return handle;
}

auto EnsureAndOpenDirectoryHandle(const std::filesystem::path& InPath,
                                  std::string* OutError) -> HANDLE {
    if (CreateDirectoryW(InPath.wstring().c_str(), nullptr) == 0) {
        const auto createError = GetLastError();
        if (createError != ERROR_ALREADY_EXISTS) {
            if (OutError)
                *OutError = "cannot create plan execution lock directory";
            return INVALID_HANDLE_VALUE;
        }
    }
    return OpenVerifiedDirectoryHandle(InPath, OutError);
}

auto OpenAnchoredLockFile(const PlanExecutionLockLocation& InLocation,
                          const std::string& InFileName,
                          std::string* OutError) -> HANDLE {
    std::vector<HANDLE> directoryHandles;
    const auto closeDirectoryHandles = [&directoryHandles] {
        for (auto it = directoryHandles.rbegin();
             it != directoryHandles.rend(); ++it) {
            CloseHandle(*it);
        }
        directoryHandles.clear();
    };

    const auto anchorHandle =
        OpenVerifiedDirectoryHandle(InLocation.anchorRoot, OutError);
    if (anchorHandle == INVALID_HANDLE_VALUE) return INVALID_HANDLE_VALUE;
    directoryHandles.push_back(anchorHandle);

    auto currentPath = InLocation.anchorRoot;
    for (const auto& component : InLocation.directoryComponents) {
        currentPath /= component;
        const auto componentHandle =
            EnsureAndOpenDirectoryHandle(currentPath, OutError);
        if (componentHandle == INVALID_HANDLE_VALUE) {
            closeDirectoryHandles();
            return INVALID_HANDLE_VALUE;
        }
        directoryHandles.push_back(componentHandle);
    }

    const auto lockPath = currentPath / InFileName;
    const auto lockHandle = CreateFileW(
        lockPath.wstring().c_str(), GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    closeDirectoryHandles();
    if (lockHandle == INVALID_HANDLE_VALUE && OutError)
        *OutError = "cannot open plan execution lock file";
    return lockHandle;
}
#else
auto OpenDirectoryAt(const int InParent,
                     const char* InName,
                     const bool InCreate,
                     std::string* OutError) -> int {
    int flags = O_RDONLY;
#if defined(O_CLOEXEC)
    flags |= O_CLOEXEC;
#endif
#if defined(O_DIRECTORY)
    flags |= O_DIRECTORY;
#endif
#if defined(O_NOFOLLOW)
    flags |= O_NOFOLLOW;
#endif
    auto handle = ::openat(InParent, InName, flags);
    if (handle < 0 && InCreate && errno == ENOENT) {
        if (::mkdirat(InParent, InName, 0700) != 0 && errno != EEXIST) {
            if (OutError)
                *OutError = "cannot create plan execution lock directory";
            return -1;
        }
        handle = ::openat(InParent, InName, flags);
    }
    if (handle < 0) {
        if (OutError)
            *OutError = "cannot open plan execution lock directory";
        return -1;
    }
    struct stat info {};
    if (::fstat(handle, &info) != 0 || !S_ISDIR(info.st_mode)) {
        (void)::close(handle);
        if (OutError)
            *OutError = "plan execution lock directory must be a real "
                        "non-symlink directory";
        return -1;
    }
    return handle;
}

auto OpenAnchoredLockFile(const PlanExecutionLockLocation& InLocation,
                          const std::string& InFileName,
                          std::string* OutError) -> int {
    int rootFlags = O_RDONLY;
#if defined(O_CLOEXEC)
    rootFlags |= O_CLOEXEC;
#endif
#if defined(O_DIRECTORY)
    rootFlags |= O_DIRECTORY;
#endif
#if defined(O_NOFOLLOW)
    rootFlags |= O_NOFOLLOW;
#endif
    auto currentHandle =
        ::open(InLocation.anchorRoot.c_str(), rootFlags);
    if (currentHandle < 0) {
        if (OutError)
            *OutError = "cannot open plan execution lock anchor directory";
        return -1;
    }

    for (const auto& component : InLocation.directoryComponents) {
        const auto nextHandle =
            OpenDirectoryAt(currentHandle, component.c_str(), true, OutError);
        (void)::close(currentHandle);
        if (nextHandle < 0) return -1;
        currentHandle = nextHandle;
    }
    int fileFlags = O_CREAT | O_RDWR;
#if defined(O_CLOEXEC)
    fileFlags |= O_CLOEXEC;
#endif
#if defined(O_NOFOLLOW)
    fileFlags |= O_NOFOLLOW;
#endif
    const auto lockHandle =
        ::openat(currentHandle, InFileName.c_str(), fileFlags, 0600);
    (void)::close(currentHandle);
    if (lockHandle < 0 && OutError)
        *OutError = "cannot open plan execution lock file";
    return lockHandle;
}
#endif

struct PlanExecutionLockState {
    ~PlanExecutionLockState() {
#if defined(_WIN32)
        if (handle == INVALID_HANDLE_VALUE) return;
        if (owned) {
            OVERLAPPED overlapped{};
            (void)UnlockFileEx(handle, 0, MAXDWORD, MAXDWORD, &overlapped);
        }
        CloseHandle(handle);
#else
        if (fd < 0) return;
        if (owned) (void)::flock(fd, LOCK_UN);
        (void)::close(fd);
#endif
    }

    std::filesystem::path path;
    bool owned = false;
#if defined(_WIN32)
    HANDLE handle = INVALID_HANDLE_VALUE;
#else
    int fd = -1;
#endif
};

auto ReentrantLocks()
    -> std::unordered_map<std::string, std::weak_ptr<PlanExecutionLockState>>& {
    thread_local std::unordered_map<
        std::string, std::weak_ptr<PlanExecutionLockState>> locks;
    return locks;
}

auto AcquirePlanExecutionLock(const std::filesystem::path& InWorkspaceRoot,
                              const std::filesystem::path& InPlanPath,
                              std::string* OutError,
                              const int InTimeoutMs = 5000)
    -> std::shared_ptr<PlanExecutionLockState> {
    if (OutError) OutError->clear();

    const auto lockLocation =
        ResolvePlanExecutionLockRoot(InWorkspaceRoot, OutError);
    if (!lockLocation) return {};
    const auto key = PlanExecutionLockKey(InPlanPath, OutError);
    if (!key) return {};
    if (const auto found = ReentrantLocks().find(*key);
        found != ReentrantLocks().end()) {
        if (auto existing = found->second.lock()) return existing;
        ReentrantLocks().erase(found);
    }

    auto lock = std::make_shared<PlanExecutionLockState>();
    const auto lockFileName = *key + ".lock";
    lock->path =
        (lockLocation->root / lockFileName).lexically_normal();
#if defined(_WIN32)
    lock->handle =
        OpenAnchoredLockFile(*lockLocation, lockFileName, OutError);
    if (lock->handle == INVALID_HANDLE_VALUE) {
        if (OutError && OutError->empty())
            *OutError = "cannot open plan execution lock file";
        return {};
    }
    FILE_ATTRIBUTE_TAG_INFO tagInfo{};
    if (GetFileInformationByHandleEx(lock->handle, FileAttributeTagInfo,
                                     &tagInfo, sizeof(tagInfo)) == 0 ||
        (tagInfo.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 ||
        (tagInfo.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        if (OutError)
            *OutError = "plan execution lock must be a regular file";
        return {};
    }
#else
    lock->fd =
        OpenAnchoredLockFile(*lockLocation, lockFileName, OutError);
    if (lock->fd < 0) {
        if (OutError && OutError->empty())
            *OutError = "cannot open plan execution lock file";
        return {};
    }
    struct stat lockInfo {};
    if (::fstat(lock->fd, &lockInfo) != 0 || !S_ISREG(lockInfo.st_mode)) {
        if (OutError)
            *OutError = "plan execution lock must be a regular file";
        return {};
    }
    const auto descriptorFlags = ::fcntl(lock->fd, F_GETFD);
    if (descriptorFlags < 0 ||
        ::fcntl(lock->fd, F_SETFD, descriptorFlags | FD_CLOEXEC) != 0) {
        if (OutError)
            *OutError = "cannot make plan execution lock close-on-exec";
        return {};
    }
#endif

    const auto deadline =
        std::chrono::steady_clock::now() +
        std::chrono::milliseconds((std::max)(0, InTimeoutMs));
    while (true) {
#if defined(_WIN32)
        OVERLAPPED overlapped{};
        if (LockFileEx(lock->handle,
                       LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0,
                       MAXDWORD, MAXDWORD, &overlapped) != 0) {
            lock->owned = true;
            ReentrantLocks()[*key] = lock;
            return lock;
        }
        const auto lockError = GetLastError();
        if (lockError != ERROR_LOCK_VIOLATION &&
            lockError != ERROR_IO_PENDING) {
            if (OutError) *OutError = "cannot acquire plan execution lock";
            return {};
        }
#else
        if (::flock(lock->fd, LOCK_EX | LOCK_NB) == 0) {
            lock->owned = true;
            ReentrantLocks()[*key] = lock;
            return lock;
        }
        if (errno != EWOULDBLOCK && errno != EAGAIN) {
            if (OutError) *OutError = "cannot acquire plan execution lock";
            return {};
        }
#endif
        if (std::chrono::steady_clock::now() >= deadline) {
            if (OutError)
                *OutError = "plan execution lock busy: " +
                            lock->path.generic_string();
            return {};
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

auto AppendSourceRevalidationFailure(PlanAuditSink& InAudit,
                                     const std::filesystem::path& InWorkspaceRoot,
                                     const std::string& InSourceError,
                                     std::string* OutError) -> bool {
    const auto before = InAudit.Capture(InWorkspaceRoot);
    std::string appendError;
    if (!InAudit.Append("plan.source.revalidate", InWorkspaceRoot, before,
                        CurrentUtcTimestampIso8601(), 2, &appendError)) {
        if (OutError)
            *OutError =
                "cannot audit plan source revalidation failure: " + appendError;
        return false;
    }
    if (OutError) {
        *OutError = InSourceError.empty()
                        ? "plan source changed after audit reservation"
                        : InSourceError;
    }
    return false;
}

} // namespace

PlanExecutionAdmission::PlanExecutionAdmission(
    std::shared_ptr<void> InLockState, std::string InAdmittedSourceBytes)
    : mLockState(std::move(InLockState)),
      mAdmittedSourceBytes(std::move(InAdmittedSourceBytes)) {}

auto PlanExecutionAdmission::AdmittedSourceBytes() const
    -> const std::string& {
    return mAdmittedSourceBytes;
}

auto AcquireAuditedPlanExecutionAdmission(
    PlanAuditSink& InAudit,
    const std::filesystem::path& InWorkspaceRoot,
    const std::filesystem::path& InPlanPath,
    std::string* OutError) -> std::optional<PlanExecutionAdmission> {
    const auto startedAtUtc = CurrentUtcTimestampIso8601();
    const auto before = InAudit.Capture(InWorkspaceRoot);
    std::string lockError;
    int lockTimeoutMs = 5000;
    if (const auto* timeoutValue = TestModeEnabled()
            ? std::getenv("KOG_TEST_ONLY_PLAN_EXECUTION_LOCK_TIMEOUT_MS")
            : nullptr;
        timeoutValue != nullptr) {
        try {
            lockTimeoutMs =
                (std::min)(5000, (std::max)(0, std::stoi(timeoutValue)));
        } catch (...) {
        }
    }
    auto lock = AcquirePlanExecutionLock(
        InWorkspaceRoot, InPlanPath, &lockError, lockTimeoutMs);
    if (!lock) {
        std::string appendError;
        if (!InAudit.Append("plan.execution-lock", InWorkspaceRoot, before,
                            startedAtUtc, 2, &appendError)) {
            if (OutError)
                *OutError = "cannot audit plan execution lock failure: " +
                            appendError;
        } else if (OutError) {
            *OutError = lockError.empty() ? "cannot acquire plan execution lock"
                                          : lockError;
        }
        return std::nullopt;
    }

    std::string sourceError;
    std::string admittedSourceBytes;
    const bool sourceStable = InAudit.RevalidateAdmittedSource(
        InPlanPath, &sourceError, &admittedSourceBytes);
    std::string appendError;
    if (!InAudit.Append("plan.execution-lock", InWorkspaceRoot, before,
                        startedAtUtc, 0, &appendError)) {
        if (OutError)
            *OutError =
                "cannot audit plan execution lock acquisition: " + appendError;
        return std::nullopt;
    }
    if (!sourceStable) {
        AppendSourceRevalidationFailure(
            InAudit, InWorkspaceRoot, sourceError, OutError);
        return std::nullopt;
    }

    if (const auto* holdValue = TestModeEnabled()
            ? std::getenv("KOG_TEST_ONLY_PLAN_EXECUTION_LOCK_HOLD_MS")
            : nullptr;
        holdValue != nullptr) {
        try {
            const auto holdMs = (std::min)(5000, (std::max)(0, std::stoi(holdValue)));
            std::this_thread::sleep_for(std::chrono::milliseconds(holdMs));
        } catch (...) {
        }
    }
    if (const auto readyPath = ResolveTestHookPath(
            InWorkspaceRoot,
            std::getenv("KOG_TEST_ONLY_PLAN_EXECUTION_LOCK_READY_FILE"));
        readyPath.has_value()) {
        std::error_code ec;
        std::filesystem::create_directories(readyPath->parent_path(), ec);
        std::ofstream ready(*readyPath, std::ios::binary | std::ios::trunc);
        ready << "ready\n";
        ready.close();
        if (const auto releasePath = ResolveTestHookPath(
                InWorkspaceRoot,
                std::getenv("KOG_TEST_ONLY_PLAN_EXECUTION_LOCK_RELEASE_FILE"));
            releasePath.has_value()) {
            const auto deadline = std::chrono::steady_clock::now() +
                                  std::chrono::seconds(10);
            while (!std::filesystem::exists(*releasePath, ec) &&
                   std::chrono::steady_clock::now() < deadline) {
                ec.clear();
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        }
    }
    return PlanExecutionAdmission(std::move(lock),
                                  std::move(admittedSourceBytes));
}

} // namespace kano::git::commands
