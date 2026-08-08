#include "commit_plan_source_rewrite.hpp"

#include "commit_plan_audit.hpp"
#include "runtime_path_layout.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string_view>
#include <thread>

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

constexpr std::size_t kMaximumPlanBytes = 4U << 20U;

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
    return value != nullptr && std::string_view(value) == "1";
}

auto TestOnlyFailAfterFirstWrite() -> bool {
    if (!TestModeEnabled()) return false;
    const auto* value = std::getenv(
        "KOG_TEST_ONLY_PLAN_SOURCE_REWRITE_FAIL_AFTER_FIRST_WRITE");
    return value != nullptr && std::string_view(value) == "1";
}

auto RewritePhaseName(const PlanSourceRewritePhase InPhase)
    -> std::string_view {
    switch (InPhase) {
    case PlanSourceRewritePhase::ClearExecutionStamp:
        return "clear";
    case PlanSourceRewritePhase::WriteCompletionStamp:
        return "stamp";
    }
    return "unknown";
}

auto TestHookPhaseMatches(const PlanSourceRewritePhase InPhase) -> bool {
    if (!TestModeEnabled()) return false;
    const auto* value =
        std::getenv("KOG_TEST_ONLY_PLAN_SOURCE_REWRITE_PHASE");
    return value == nullptr || *value == '\0' ||
        std::string_view(value) == RewritePhaseName(InPhase);
}

auto ResolveTestHookPath(const std::filesystem::path& InWorkspaceRoot,
                         const char* InRawPath)
    -> std::optional<std::filesystem::path> {
    if (!TestModeEnabled() || InRawPath == nullptr || *InRawPath == '\0')
        return std::nullopt;
    const auto allowedRoot =
        (runtime_path::Layout::Resolve(InWorkspaceRoot)
             .WorkspaceGitTemporaryRoot() /
         "plan-source-rewrite-test-hooks")
            .lexically_normal();
    std::error_code ec;
    std::filesystem::create_directories(allowedRoot, ec);
    if (ec) return std::nullopt;
    const auto canonicalRoot = std::filesystem::weakly_canonical(allowedRoot, ec);
    if (ec) return std::nullopt;
    const auto candidate = std::filesystem::weakly_canonical(
        std::filesystem::path(InRawPath), ec);
    if (ec) return std::nullopt;
    const auto relative = candidate.lexically_relative(canonicalRoot);
    if (relative.empty() || relative.is_absolute()) return std::nullopt;
    for (const auto& component : relative) {
        if (component == "..") return std::nullopt;
    }
    return candidate;
}

void WaitAtPreWriteTestHook(const std::filesystem::path& InWorkspaceRoot,
                            const PlanSourceRewritePhase InPhase) {
    if (!TestHookPhaseMatches(InPhase)) return;
    const auto readyPath = ResolveTestHookPath(
        InWorkspaceRoot,
        std::getenv("KOG_TEST_ONLY_PLAN_SOURCE_REWRITE_READY_FILE"));
    if (!readyPath) return;
    std::error_code ec;
    std::filesystem::create_directories(readyPath->parent_path(), ec);
    if (ec) return;
    std::ofstream ready(*readyPath, std::ios::binary | std::ios::trunc);
    ready << "ready\n";
    ready.close();
    const auto releasePath = ResolveTestHookPath(
        InWorkspaceRoot,
        std::getenv("KOG_TEST_ONLY_PLAN_SOURCE_REWRITE_RELEASE_FILE"));
    if (!releasePath) return;
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!std::filesystem::exists(*releasePath, ec) &&
           std::chrono::steady_clock::now() < deadline) {
        ec.clear();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

auto AppendFailure(PlanAuditSink& InAudit,
                   const std::filesystem::path& InWorkspaceRoot,
                   const std::string& InReason,
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
    if (OutError)
        *OutError = InReason.empty()
                        ? "plan source changed before conditional rewrite"
                        : InReason;
    return false;
}

#if defined(_WIN32)

auto SetWindowsError(std::string* OutError, const std::string& InPrefix) -> bool {
    if (OutError)
        *OutError = InPrefix + ": win32 error " +
                    std::to_string(GetLastError());
    return false;
}

auto ReadWindowsHandle(HANDLE InHandle,
                       const std::size_t InLimit,
                       std::string* OutError) -> std::optional<std::string> {
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(InHandle, &size) || size.QuadPart < 0 ||
        static_cast<std::uint64_t>(size.QuadPart) > InLimit)
        return SetWindowsError(OutError, "plan source size is invalid"),
               std::nullopt;
    LARGE_INTEGER start{};
    if (!SetFilePointerEx(InHandle, start, nullptr, FILE_BEGIN))
        return SetWindowsError(OutError, "cannot seek plan source"),
               std::nullopt;
    std::string bytes(static_cast<std::size_t>(size.QuadPart), '\0');
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        DWORD read = 0;
        const auto chunk = static_cast<DWORD>((std::min<std::size_t>)(
            bytes.size() - offset, 1U << 30U));
        if (!ReadFile(InHandle, bytes.data() + offset, chunk, &read, nullptr) ||
            read == 0)
            return SetWindowsError(OutError, "cannot read plan source"),
                   std::nullopt;
        offset += read;
    }
    return bytes;
}

auto ReadWindowsIdentity(HANDLE InHandle, FILE_ID_INFO* OutIdentity) -> bool {
    return GetFileInformationByHandleEx(
               InHandle, FileIdInfo, OutIdentity, sizeof(*OutIdentity)) != 0;
}

auto SameWindowsIdentity(const FILE_ID_INFO& InLeft,
                         const FILE_ID_INFO& InRight) -> bool {
    return InLeft.VolumeSerialNumber == InRight.VolumeSerialNumber &&
        std::memcmp(InLeft.FileId.Identifier, InRight.FileId.Identifier,
                    sizeof(InLeft.FileId.Identifier)) == 0;
}

auto OpenMatchingWindowsPathHandle(
    const std::filesystem::path& InPlanPath,
    const FILE_ID_INFO& InExpectedIdentity,
    std::string* OutError) -> HANDLE {
    const auto pathHandle = CreateFileW(
        InPlanPath.c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (pathHandle == INVALID_HANDLE_VALUE) {
        SetWindowsError(OutError, "cannot re-open plan source path");
        return INVALID_HANDLE_VALUE;
    }
    BY_HANDLE_FILE_INFORMATION attributes{};
    FILE_ID_INFO pathIdentity{};
    const bool matches = GetFileInformationByHandle(pathHandle, &attributes) &&
        (attributes.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0 &&
        (attributes.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 &&
        ReadWindowsIdentity(pathHandle, &pathIdentity) &&
        SameWindowsIdentity(InExpectedIdentity, pathIdentity);
    if (matches) return pathHandle;
    CloseHandle(pathHandle);
    if (OutError) *OutError = "plan source path identity changed";
    return INVALID_HANDLE_VALUE;
}

auto WriteWindowsHandle(HANDLE InHandle,
                        const std::string& InBytes,
                        const bool InAllowTestFault,
                        std::string* OutError) -> bool {
    LARGE_INTEGER start{};
    if (!SetFilePointerEx(InHandle, start, nullptr, FILE_BEGIN))
        return SetWindowsError(OutError, "cannot seek plan source");
    std::size_t offset = 0;
    while (offset < InBytes.size()) {
        DWORD written = 0;
        const auto chunk = static_cast<DWORD>((std::min<std::size_t>)(
            InBytes.size() - offset, 1U << 30U));
        if (!WriteFile(InHandle, InBytes.data() + offset, chunk, &written,
                       nullptr) || written == 0)
            return SetWindowsError(OutError, "cannot write plan source");
        offset += written;
        if (InAllowTestFault && TestOnlyFailAfterFirstWrite()) {
            if (OutError)
                *OutError =
                    "injected failure after first plan source write";
            return false;
        }
    }
    if (!SetEndOfFile(InHandle) || !FlushFileBuffers(InHandle))
        return SetWindowsError(OutError, "cannot flush plan source");
    return true;
}

auto RestoreWindowsHandleAfterFailure(
    HANDLE InHandle,
    const std::string& InExpectedBytes,
    const std::string& InFailure,
    std::string* OutError) -> bool {
    std::string restoreError;
    if (!WriteWindowsHandle(InHandle, InExpectedBytes, false, &restoreError)) {
        if (OutError)
            *OutError = InFailure +
                "; unrecoverable plan source corruption: admitted-byte "
                "rollback failed (" + restoreError + ")";
        return false;
    }
    if (OutError) *OutError = InFailure;
    return false;
}

auto RewriteConditionally(PlanAuditSink* InAudit,
                          const std::filesystem::path& InPlanPath,
                          const std::string& InExpectedBytes,
                          const std::string& InReplacementBytes,
                          const bool InAllowTestFault,
                          std::string* OutError) -> bool {
    const auto handle = CreateFileW(
        InPlanPath.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ,
        nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT |
            FILE_FLAG_WRITE_THROUGH,
        nullptr);
    if (handle == INVALID_HANDLE_VALUE)
        return SetWindowsError(OutError, "cannot safely open plan source");
    BY_HANDLE_FILE_INFORMATION attributes{};
    FILE_ID_INFO identity{};
    const bool regular = GetFileInformationByHandle(handle, &attributes) &&
        (attributes.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0 &&
        (attributes.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 &&
        ReadWindowsIdentity(handle, &identity);
    if (!regular) {
        CloseHandle(handle);
        if (OutError)
            *OutError = "plan source must be a regular non-reparse file";
        return false;
    }
    auto current = ReadWindowsHandle(handle, kMaximumPlanBytes, OutError);
    if (!current || *current != InExpectedBytes) {
        CloseHandle(handle);
        if (current && OutError) *OutError = "plan source exact bytes changed";
        return false;
    }
    current = ReadWindowsHandle(handle, kMaximumPlanBytes, OutError);
    if (!current || *current != InExpectedBytes) {
        CloseHandle(handle);
        if (current && OutError) *OutError = "plan source exact bytes changed";
        return false;
    }
    const auto pathHandle =
        OpenMatchingWindowsPathHandle(InPlanPath, identity, OutError);
    if (pathHandle == INVALID_HANDLE_VALUE) {
        CloseHandle(handle);
        return false;
    }
    std::string writeError;
    if (!WriteWindowsHandle(
            handle, InReplacementBytes, InAllowTestFault, &writeError)) {
        const auto result = RestoreWindowsHandleAfterFailure(
            handle, InExpectedBytes, writeError, OutError);
        CloseHandle(pathHandle);
        CloseHandle(handle);
        return result;
    }
    const auto written =
        ReadWindowsHandle(handle, kMaximumPlanBytes, OutError);
    FILE_ID_INFO after{};
    const auto finalPathHandle =
        OpenMatchingWindowsPathHandle(InPlanPath, identity, OutError);
    const bool identityStable = ReadWindowsIdentity(handle, &after) &&
        SameWindowsIdentity(identity, after) &&
        finalPathHandle != INVALID_HANDLE_VALUE;
    if (!written || *written != InReplacementBytes || !identityStable) {
        auto failure = OutError && !OutError->empty()
            ? *OutError
            : std::string("plan source changed during conditional rewrite");
        const auto result = RestoreWindowsHandleAfterFailure(
            handle, InExpectedBytes, failure, OutError);
        if (finalPathHandle != INVALID_HANDLE_VALUE)
            CloseHandle(finalPathHandle);
        CloseHandle(pathHandle);
        CloseHandle(handle);
        return result;
    }
    if (InAudit != nullptr &&
        !InAudit->BindSourceStateBytes(InReplacementBytes, OutError)) {
        const auto failure = OutError && !OutError->empty()
            ? *OutError
            : std::string("cannot bind exact replacement bytes to audit");
        const auto result = RestoreWindowsHandleAfterFailure(
            handle, InExpectedBytes, failure, OutError);
        CloseHandle(finalPathHandle);
        CloseHandle(pathHandle);
        CloseHandle(handle);
        return result;
    }
    CloseHandle(finalPathHandle);
    CloseHandle(pathHandle);
    CloseHandle(handle);
    return true;
}

#else

auto SetPosixError(std::string* OutError, const std::string& InPrefix) -> bool {
    if (OutError) *OutError = InPrefix + ": errno " + std::to_string(errno);
    return false;
}

auto SamePosixTimestamp(const struct stat& InLeft,
                        const struct stat& InRight,
                        const bool InModificationTime) -> bool {
#if defined(__APPLE__)
    const auto& left = InModificationTime ? InLeft.st_mtimespec
                                          : InLeft.st_ctimespec;
    const auto& right = InModificationTime ? InRight.st_mtimespec
                                           : InRight.st_ctimespec;
#else
    const auto& left = InModificationTime ? InLeft.st_mtim : InLeft.st_ctim;
    const auto& right = InModificationTime ? InRight.st_mtim : InRight.st_ctim;
#endif
    return left.tv_sec == right.tv_sec && left.tv_nsec == right.tv_nsec;
}

auto SameStablePosixSnapshot(const struct stat& InLeft,
                             const struct stat& InRight) -> bool {
    return InLeft.st_dev == InRight.st_dev &&
        InLeft.st_ino == InRight.st_ino &&
        InLeft.st_size == InRight.st_size &&
        SamePosixTimestamp(InLeft, InRight, true) &&
        SamePosixTimestamp(InLeft, InRight, false);
}

auto ReadPosixHandle(const int InHandle,
                     const std::size_t InLimit,
                     struct stat* OutStableState,
                     std::string* OutError) -> std::optional<std::string> {
    struct stat before {};
    if (::fstat(InHandle, &before) != 0)
        return SetPosixError(OutError, "cannot stat plan source"), std::nullopt;
    if (!S_ISREG(before.st_mode) || before.st_size < 0 ||
        static_cast<std::uintmax_t>(before.st_size) > InLimit) {
        if (OutError) *OutError = "plan source must be a bounded regular file";
        return std::nullopt;
    }
    std::string bytes(static_cast<std::size_t>(before.st_size), '\0');
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const auto read = ::pread(InHandle, bytes.data() + offset,
                                  bytes.size() - offset,
                                  static_cast<off_t>(offset));
        if (read < 0 && errno == EINTR) continue;
        if (read <= 0)
            return SetPosixError(OutError, "cannot read plan source"),
                   std::nullopt;
        offset += static_cast<std::size_t>(read);
    }
    struct stat after {};
    if (::fstat(InHandle, &after) != 0)
        return SetPosixError(OutError, "cannot re-stat plan source"),
               std::nullopt;
    if (!SameStablePosixSnapshot(before, after)) {
        if (OutError) *OutError = "plan source changed during stable read";
        return std::nullopt;
    }
    if (OutStableState) *OutStableState = after;
    return bytes;
}

auto SameIdentity(const struct stat& InLeft, const struct stat& InRight)
    -> bool {
    return InLeft.st_dev == InRight.st_dev && InLeft.st_ino == InRight.st_ino;
}

auto WritePosixHandle(const int InHandle,
                      const std::string& InBytes,
                      const bool InAllowTestFault,
                      std::string* OutError) -> bool {
    std::size_t offset = 0;
    while (offset < InBytes.size()) {
        const auto written = ::pwrite(
            InHandle, InBytes.data() + offset, InBytes.size() - offset,
            static_cast<off_t>(offset));
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0)
            return SetPosixError(OutError, "cannot write plan source");
        offset += static_cast<std::size_t>(written);
        if (InAllowTestFault && TestOnlyFailAfterFirstWrite()) {
            if (OutError)
                *OutError =
                    "injected failure after first plan source write";
            return false;
        }
    }
    if (::ftruncate(InHandle, static_cast<off_t>(InBytes.size())) != 0 ||
        ::fsync(InHandle) != 0)
        return SetPosixError(OutError, "cannot flush plan source");
    return true;
}

auto RestorePosixHandleAfterFailure(
    const int InHandle,
    const std::string& InExpectedBytes,
    const std::string& InFailure,
    std::string* OutError) -> bool {
    std::string restoreError;
    if (!WritePosixHandle(InHandle, InExpectedBytes, false, &restoreError)) {
        if (OutError)
            *OutError = InFailure +
                "; unrecoverable plan source corruption: admitted-byte "
                "rollback failed (" + restoreError + ")";
        return false;
    }
    if (OutError) *OutError = InFailure;
    return false;
}

auto RewriteConditionally(PlanAuditSink* InAudit,
                          const std::filesystem::path& InPlanPath,
                          const std::string& InExpectedBytes,
                          const std::string& InReplacementBytes,
                          const bool InAllowTestFault,
                          std::string* OutError) -> bool {
    int flags = O_RDWR;
#if defined(O_CLOEXEC)
    flags |= O_CLOEXEC;
#endif
#if defined(O_NOFOLLOW)
    flags |= O_NOFOLLOW;
#endif
    const int handle = ::open(InPlanPath.c_str(), flags);
    if (handle < 0)
        return SetPosixError(OutError, "cannot safely open plan source");
    const auto descriptorFlags = ::fcntl(handle, F_GETFD);
    // flock is advisory on POSIX. Never wait on a cooperating locker. The
    // mutation contract requires writers to cooperate with flock; stable
    // metadata and path checks narrow, but cannot eliminate, races from a
    // hostile writer that intentionally ignores the advisory lock.
    if (descriptorFlags < 0 ||
        ::fcntl(handle, F_SETFD, descriptorFlags | FD_CLOEXEC) != 0 ||
        ::flock(handle, LOCK_EX | LOCK_NB) != 0) {
        const auto result =
            SetPosixError(OutError, "cannot lock plan source handle");
        ::close(handle);
        return result;
    }
    struct stat identity {};
    if (::fstat(handle, &identity) != 0 || !S_ISREG(identity.st_mode)) {
        ::close(handle);
        if (OutError) *OutError = "plan source must be a regular file";
        return false;
    }
    auto current =
        ReadPosixHandle(handle, kMaximumPlanBytes, nullptr, OutError);
    if (!current || *current != InExpectedBytes) {
        ::close(handle);
        if (current && OutError) *OutError = "plan source exact bytes changed";
        return false;
    }
    struct stat stableReadBefore {};
    current = ReadPosixHandle(
        handle, kMaximumPlanBytes, &stableReadBefore, OutError);
    struct stat handleBefore {};
    struct stat pathBefore {};
    const bool identityStable = current && *current == InExpectedBytes &&
        ::fstat(handle, &handleBefore) == 0 &&
        ::lstat(InPlanPath.c_str(), &pathBefore) == 0 &&
        S_ISREG(pathBefore.st_mode) && SameIdentity(identity, handleBefore) &&
        SameIdentity(identity, pathBefore) &&
        SameStablePosixSnapshot(stableReadBefore, handleBefore) &&
        SameStablePosixSnapshot(stableReadBefore, pathBefore);
    if (!identityStable) {
        ::close(handle);
        if (OutError && OutError->empty())
            *OutError = "plan source bytes or path identity changed";
        return false;
    }
    std::string writeError;
    if (!WritePosixHandle(
            handle, InReplacementBytes, InAllowTestFault, &writeError)) {
        const auto result = RestorePosixHandleAfterFailure(
            handle, InExpectedBytes, writeError, OutError);
        ::close(handle);
        return result;
    }
    struct stat stableReadAfter {};
    const auto written = ReadPosixHandle(
        handle, kMaximumPlanBytes, &stableReadAfter, OutError);
    struct stat handleAfter {};
    struct stat pathAfter {};
    const bool handleStillOwnsReplacement =
        written && *written == InReplacementBytes &&
        ::fstat(handle, &handleAfter) == 0 &&
        SameIdentity(identity, handleAfter) &&
        SameStablePosixSnapshot(stableReadAfter, handleAfter);
    const bool pathStillNamesReplacement =
        handleStillOwnsReplacement &&
        ::lstat(InPlanPath.c_str(), &pathAfter) == 0 &&
        S_ISREG(pathAfter.st_mode) && SameIdentity(identity, pathAfter) &&
        SameStablePosixSnapshot(stableReadAfter, pathAfter);
    const bool finalStable =
        handleStillOwnsReplacement && pathStillNamesReplacement;
    if (!finalStable) {
        auto failure = OutError && !OutError->empty()
            ? *OutError
            : std::string("plan source changed during conditional rewrite");
        // If exact replacement ownership was lost, a non-cooperating writer
        // may own the current bytes. Preserve them rather than overwriting
        // them with an unconditional rollback.
        bool result = false;
        if (handleStillOwnsReplacement) {
            result = RestorePosixHandleAfterFailure(
                handle, InExpectedBytes, failure, OutError);
        } else if (OutError) {
            *OutError = failure;
        }
        ::close(handle);
        return result;
    }
    if (InAudit != nullptr &&
        !InAudit->BindSourceStateBytes(InReplacementBytes, OutError)) {
        const auto failure = OutError && !OutError->empty()
            ? *OutError
            : std::string("cannot bind exact replacement bytes to audit");
        const auto result = RestorePosixHandleAfterFailure(
            handle, InExpectedBytes, failure, OutError);
        ::close(handle);
        return result;
    }
    ::close(handle);
    return true;
}

#endif

} // namespace

auto RewriteAuditedPlanSourceConditionally(
    PlanAuditSink& InAudit,
    const std::filesystem::path& InWorkspaceRoot,
    const std::filesystem::path& InPlanPath,
    const std::string& InExpectedBytes,
    const std::string& InReplacementBytes,
    const PlanSourceRewritePhase InPhase,
    std::string* OutError) -> bool {
    if (InExpectedBytes.size() > kMaximumPlanBytes ||
        InReplacementBytes.size() > kMaximumPlanBytes)
        return AppendFailure(InAudit, InWorkspaceRoot,
                             "plan source exceeds conditional rewrite limit",
                             OutError);
    // This bounded, test-only handshake sits in the historical
    // verify-to-rewrite gap, before either platform opens the source for the
    // conditional operation. It makes an external replacement reproducible
    // on Windows, where the later writable handle intentionally denies
    // concurrent writers.
    WaitAtPreWriteTestHook(InWorkspaceRoot, InPhase);
    std::string rewriteError;
    if (RewriteConditionally(&InAudit, InPlanPath, InExpectedBytes,
                             InReplacementBytes,
                             TestHookPhaseMatches(InPhase), &rewriteError))
        return true;
    return AppendFailure(InAudit, InWorkspaceRoot, rewriteError, OutError);
}

auto RestorePlanSourceBytesConditionally(
    const std::filesystem::path& InPlanPath,
    const std::string& InExpectedStampedBytes,
    const std::string& InOriginalAdmittedBytes,
    std::string* OutError) -> bool {
    if (InExpectedStampedBytes.size() > kMaximumPlanBytes ||
        InOriginalAdmittedBytes.size() > kMaximumPlanBytes) {
        if (OutError)
            *OutError = "plan source exceeds conditional restore limit";
        return false;
    }
    return RewriteConditionally(
        nullptr, InPlanPath, InExpectedStampedBytes,
        InOriginalAdmittedBytes, false, OutError);
}

} // namespace kano::git::commands
