#include "audit_evidence_directory.hpp"

#include "operation_audit.hpp"

#include <algorithm>
#include <cstring>
#include <limits>

#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#include <winternl.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace kano::git::commands {
namespace {

thread_local AuditEvidencePinnedTestHook gPinnedTestHook;

auto InvokePinnedTestHook() -> void {
    if (gPinnedTestHook.onAttemptPinned != nullptr)
        gPinnedTestHook.onAttemptPinned(gPinnedTestHook.context);
}

auto IsClosedComponent(const std::string_view InName) -> bool {
    return !InName.empty() && InName != "." && InName != ".." &&
        InName.find('/') == std::string_view::npos &&
        InName.find('\\') == std::string_view::npos &&
        InName.find('\0') == std::string_view::npos;
}

#if defined(_WIN32)

auto BoundedSystemDiagnostic(const std::string_view InPrefix,
                             const DWORD InError) -> std::string {
    return std::string(InPrefix) + ": win32 error " + std::to_string(InError);
}

auto BoundedRegularDiagnostic() -> std::string {
    return "audit input must be a bounded regular non-reparse file";
}

class ScopedHandle {
public:
    ScopedHandle(HANDLE InHandle = INVALID_HANDLE_VALUE) : mHandle(InHandle) {}
    ~ScopedHandle() { if (valid()) CloseHandle(mHandle); }
    ScopedHandle(const ScopedHandle&) = delete;
    auto operator=(const ScopedHandle&) -> ScopedHandle& = delete;
    ScopedHandle(ScopedHandle&& InOther) noexcept : mHandle(InOther.release()) {}
    [[nodiscard]] auto get() const -> HANDLE { return mHandle; }
    [[nodiscard]] auto valid() const -> bool {
        return mHandle != nullptr && mHandle != INVALID_HANDLE_VALUE;
    }
    auto release() -> HANDLE { const auto value = mHandle; mHandle = INVALID_HANDLE_VALUE; return value; }
private:
    HANDLE mHandle = INVALID_HANDLE_VALUE;
};

auto WinErrorToOpenCode(const DWORD InError) -> AuditEvidenceOpenCode {
    switch (InError) {
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND: return AuditEvidenceOpenCode::Missing;
    case ERROR_ACCESS_DENIED:
    case ERROR_SHARING_VIOLATION: return AuditEvidenceOpenCode::PermissionDenied;
    case ERROR_CANT_ACCESS_FILE:
    case ERROR_REPARSE_TAG_INVALID: return AuditEvidenceOpenCode::LoopOrReparse;
    case ERROR_DIRECTORY: return AuditEvidenceOpenCode::NotDirectory;
    default: return AuditEvidenceOpenCode::IoError;
    }
}

auto WinErrorToReadCode(const DWORD InError) -> AuditEvidenceReadCode {
    switch (InError) {
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND: return AuditEvidenceReadCode::Missing;
    case ERROR_ACCESS_DENIED:
    case ERROR_SHARING_VIOLATION: return AuditEvidenceReadCode::PermissionDenied;
    case ERROR_CANT_ACCESS_FILE:
    case ERROR_REPARSE_TAG_INVALID: return AuditEvidenceReadCode::LoopOrReparse;
    case ERROR_DIRECTORY: return AuditEvidenceReadCode::NonRegular;
    default: return AuditEvidenceReadCode::IoError;
    }
}

auto MakeUnicodeString(std::wstring& InStorage, UNICODE_STRING* OutName) -> bool {
    const auto byteCount = InStorage.size() * sizeof(wchar_t);
    if (byteCount > std::numeric_limits<USHORT>::max()) return false;
    OutName->Buffer = InStorage.data();
    OutName->Length = static_cast<USHORT>(byteCount);
    OutName->MaximumLength = static_cast<USHORT>(byteCount);
    return true;
}

auto NtOpenRelative(const HANDLE InParent, const std::string_view InName,
                    const ACCESS_MASK InAccess, const ULONG InOptions,
                    DWORD* OutError) -> ScopedHandle {
    std::wstring name = std::filesystem::path(std::string(InName)).wstring();
    UNICODE_STRING unicode{};
    if (!MakeUnicodeString(name, &unicode)) {
        if (OutError) *OutError = ERROR_INVALID_NAME;
        return {};
    }
    OBJECT_ATTRIBUTES attributes{};
    InitializeObjectAttributes(&attributes, &unicode, OBJ_CASE_INSENSITIVE,
                               InParent, nullptr);
    IO_STATUS_BLOCK statusBlock{};
    HANDLE handle = INVALID_HANDLE_VALUE;
    const NTSTATUS status = NtOpenFile(
        &handle, InAccess, &attributes, &statusBlock,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, InOptions);
    if (status < 0) {
        if (OutError) *OutError = RtlNtStatusToDosError(status);
        return {};
    }
    if (OutError) *OutError = ERROR_SUCCESS;
    return ScopedHandle(handle);
}

auto IsStable(const BY_HANDLE_FILE_INFORMATION& InBefore,
              const BY_HANDLE_FILE_INFORMATION& InAfter) -> bool {
    return InBefore.dwVolumeSerialNumber == InAfter.dwVolumeSerialNumber &&
        InBefore.nFileIndexHigh == InAfter.nFileIndexHigh &&
        InBefore.nFileIndexLow == InAfter.nFileIndexLow &&
        InBefore.nFileSizeHigh == InAfter.nFileSizeHigh &&
        InBefore.nFileSizeLow == InAfter.nFileSizeLow &&
        InBefore.ftLastWriteTime.dwHighDateTime == InAfter.ftLastWriteTime.dwHighDateTime &&
        InBefore.ftLastWriteTime.dwLowDateTime == InAfter.ftLastWriteTime.dwLowDateTime &&
        InBefore.ftCreationTime.dwHighDateTime == InAfter.ftCreationTime.dwHighDateTime &&
        InBefore.ftCreationTime.dwLowDateTime == InAfter.ftCreationTime.dwLowDateTime;
}

#else

auto BoundedSystemDiagnostic(const std::string_view InPrefix,
                             const int InError) -> std::string {
    return std::string(InPrefix) + ": " + std::strerror(InError);
}

auto BoundedRegularDiagnostic() -> std::string {
    return "audit input must be a bounded regular non-symlink file";
}

class ScopedFd {
public:
    ScopedFd(const int InFd = -1) : mFd(InFd) {}
    ~ScopedFd() { if (mFd >= 0) ::close(mFd); }
    ScopedFd(const ScopedFd&) = delete;
    auto operator=(const ScopedFd&) -> ScopedFd& = delete;
    ScopedFd(ScopedFd&& InOther) noexcept : mFd(InOther.release()) {}
    [[nodiscard]] auto get() const -> int { return mFd; }
    [[nodiscard]] auto valid() const -> bool { return mFd >= 0; }
    auto release() -> int { const int value = mFd; mFd = -1; return value; }
private:
    int mFd = -1;
};

auto ErrnoToOpenCode(const int InError) -> AuditEvidenceOpenCode {
    switch (InError) {
    case ENOENT: return AuditEvidenceOpenCode::Missing;
    case ELOOP: return AuditEvidenceOpenCode::LoopOrReparse;
    case EACCES:
    case EPERM: return AuditEvidenceOpenCode::PermissionDenied;
    case ENOTDIR: return AuditEvidenceOpenCode::NotDirectory;
    default: return AuditEvidenceOpenCode::IoError;
    }
}

auto ErrnoToReadCode(const int InError) -> AuditEvidenceReadCode {
    switch (InError) {
    case ENOENT: return AuditEvidenceReadCode::Missing;
    case ELOOP: return AuditEvidenceReadCode::LoopOrReparse;
    case EACCES:
    case EPERM: return AuditEvidenceReadCode::PermissionDenied;
    default: return AuditEvidenceReadCode::IoError;
    }
}

auto OpenDirectoryAt(const int InParent, const std::string& InName,
                     AuditEvidenceOpenCode* OutCode) -> ScopedFd {
    if (!IsClosedComponent(InName)) {
        if (OutCode) *OutCode = AuditEvidenceOpenCode::InvalidComponent;
        return {};
    }
    ScopedFd fd(::openat(InParent, InName.c_str(),
                         O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK));
    if (!fd.valid()) {
        const int savedError = errno;
        if (OutCode) *OutCode = ErrnoToOpenCode(savedError);
        return {};
    }
    struct stat info {};
    if (::fstat(fd.get(), &info) != 0) {
        const int savedError = errno;
        (void)savedError;
        if (OutCode) *OutCode = AuditEvidenceOpenCode::StatFailed;
        return {};
    }
    if (!S_ISDIR(info.st_mode)) {
        if (OutCode) *OutCode = AuditEvidenceOpenCode::NotDirectory;
        return {};
    }
    if (OutCode) *OutCode = AuditEvidenceOpenCode::None;
    return fd;
}

auto IsStable(const struct stat& InBefore, const struct stat& InAfter) -> bool {
#if defined(__APPLE__)
    const bool sameMtime = InBefore.st_mtimespec.tv_sec == InAfter.st_mtimespec.tv_sec &&
        InBefore.st_mtimespec.tv_nsec == InAfter.st_mtimespec.tv_nsec;
    const bool sameCtime = InBefore.st_ctimespec.tv_sec == InAfter.st_ctimespec.tv_sec &&
        InBefore.st_ctimespec.tv_nsec == InAfter.st_ctimespec.tv_nsec;
#else
    const bool sameMtime = InBefore.st_mtim.tv_sec == InAfter.st_mtim.tv_sec &&
        InBefore.st_mtim.tv_nsec == InAfter.st_mtim.tv_nsec;
    const bool sameCtime = InBefore.st_ctim.tv_sec == InAfter.st_ctim.tv_sec &&
        InBefore.st_ctim.tv_nsec == InAfter.st_ctim.tv_nsec;
#endif
    return InBefore.st_dev == InAfter.st_dev &&
        InBefore.st_ino == InAfter.st_ino && InBefore.st_size == InAfter.st_size &&
        sameMtime && sameCtime;
}

#endif

} // namespace

ScopedAuditEvidencePinnedTestHook::ScopedAuditEvidencePinnedTestHook(
    const AuditEvidencePinnedTestHook InHook)
    : mPrevious(gPinnedTestHook) {
    gPinnedTestHook = InHook;
}

ScopedAuditEvidencePinnedTestHook::~ScopedAuditEvidencePinnedTestHook() {
    gPinnedTestHook = mPrevious;
}

AuditEvidenceDirectory::~AuditEvidenceDirectory() {
#if defined(_WIN32)
    if (mHandle != -1) CloseHandle(reinterpret_cast<HANDLE>(mHandle));
#else
    if (mHandle >= 0) ::close(static_cast<int>(mHandle));
#endif
}

AuditEvidenceDirectory::AuditEvidenceDirectory(AuditEvidenceDirectory&& InOther) noexcept
    : mHandle(InOther.mHandle) { InOther.mHandle = -1; }

auto AuditEvidenceDirectory::operator=(AuditEvidenceDirectory&& InOther) noexcept
    -> AuditEvidenceDirectory& {
    if (this == &InOther) return *this;
#if defined(_WIN32)
    if (mHandle != -1) CloseHandle(reinterpret_cast<HANDLE>(mHandle));
#else
    if (mHandle >= 0) ::close(static_cast<int>(mHandle));
#endif
    mHandle = InOther.mHandle;
    InOther.mHandle = -1;
    return *this;
}

auto AuditEvidenceDirectory::Open(const OperationAuditPaths& InPaths,
                                  AuditEvidenceOpenCode* OutCode)
    -> std::optional<AuditEvidenceDirectory> {
    if (OutCode) *OutCode = AuditEvidenceOpenCode::IoError;
    const auto runName = InPaths.runRoot.filename().string();
    const auto attemptName = InPaths.attemptRoot.filename().string();
    if (!IsClosedComponent(runName) || !IsClosedComponent(attemptName)) {
        if (OutCode) *OutCode = AuditEvidenceOpenCode::InvalidComponent;
        return std::nullopt;
    }
#if defined(_WIN32)
    ScopedHandle anchor(CreateFileW(
        InPaths.auditRoot.c_str(), FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES | SYNCHRONIZE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (!anchor.valid()) {
        if (OutCode) *OutCode = WinErrorToOpenCode(GetLastError());
        return std::nullopt;
    }
    BY_HANDLE_FILE_INFORMATION anchorInfo{};
    if (!GetFileInformationByHandle(anchor.get(), &anchorInfo)) {
        if (OutCode) *OutCode = AuditEvidenceOpenCode::StatFailed;
        return std::nullopt;
    }
    if ((anchorInfo.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        if (OutCode) *OutCode = AuditEvidenceOpenCode::LoopOrReparse;
        return std::nullopt;
    }
    if ((anchorInfo.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
        if (OutCode) *OutCode = AuditEvidenceOpenCode::NotDirectory;
        return std::nullopt;
    }
    DWORD error = ERROR_SUCCESS;
    auto run = NtOpenRelative(anchor.get(), runName,
        FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES | SYNCHRONIZE,
        FILE_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT | FILE_OPEN_REPARSE_POINT,
        &error);
    if (!run.valid()) {
        if (OutCode) *OutCode = WinErrorToOpenCode(error);
        return std::nullopt;
    }
    BY_HANDLE_FILE_INFORMATION runInfo{};
    if (!GetFileInformationByHandle(run.get(), &runInfo)) {
        if (OutCode) *OutCode = AuditEvidenceOpenCode::StatFailed;
        return std::nullopt;
    }
    if ((runInfo.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        if (OutCode) *OutCode = AuditEvidenceOpenCode::LoopOrReparse;
        return std::nullopt;
    }
    auto attempt = NtOpenRelative(run.get(), attemptName,
        FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES | SYNCHRONIZE,
        FILE_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT | FILE_OPEN_REPARSE_POINT,
        &error);
    if (!attempt.valid()) {
        if (OutCode) *OutCode = WinErrorToOpenCode(error);
        return std::nullopt;
    }
    BY_HANDLE_FILE_INFORMATION attemptInfo{};
    if (!GetFileInformationByHandle(attempt.get(), &attemptInfo)) {
        if (OutCode) *OutCode = AuditEvidenceOpenCode::StatFailed;
        return std::nullopt;
    }
    if ((attemptInfo.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        if (OutCode) *OutCode = AuditEvidenceOpenCode::LoopOrReparse;
        return std::nullopt;
    }
    if (OutCode) *OutCode = AuditEvidenceOpenCode::None;
    AuditEvidenceDirectory result(
        reinterpret_cast<std::intptr_t>(attempt.release()));
    InvokePinnedTestHook();
    return result;
#else
    const int rawAnchor = ::open(InPaths.auditRoot.c_str(),
        O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
    if (rawAnchor < 0) {
        const int savedError = errno;
        if (OutCode) *OutCode = ErrnoToOpenCode(savedError);
        return std::nullopt;
    }
    ScopedFd anchor(rawAnchor);
    struct stat anchorInfo {};
    if (::fstat(anchor.get(), &anchorInfo) != 0) {
        const int savedError = errno;
        (void)savedError;
        if (OutCode) *OutCode = AuditEvidenceOpenCode::StatFailed;
        return std::nullopt;
    }
    if (!S_ISDIR(anchorInfo.st_mode)) {
        if (OutCode) *OutCode = AuditEvidenceOpenCode::NotDirectory;
        return std::nullopt;
    }
    auto run = OpenDirectoryAt(anchor.get(), runName, OutCode);
    if (!run.valid()) return std::nullopt;
    auto attempt = OpenDirectoryAt(run.get(), attemptName, OutCode);
    if (!attempt.valid()) return std::nullopt;
    if (OutCode) *OutCode = AuditEvidenceOpenCode::None;
    AuditEvidenceDirectory result(attempt.release());
    InvokePinnedTestHook();
    return result;
#endif
}

auto AuditEvidenceDirectory::Probe(const std::string_view InChildName) const
    -> AuditEvidenceProbe {
    if (!IsClosedComponent(InChildName))
        return {.code = AuditEvidenceReadCode::InvalidComponent};
#if defined(_WIN32)
    if (mHandle == -1) return {.code = AuditEvidenceReadCode::IoError};
    DWORD error = ERROR_SUCCESS;
    auto file = NtOpenRelative(reinterpret_cast<HANDLE>(mHandle), InChildName,
        FILE_READ_ATTRIBUTES | SYNCHRONIZE,
        FILE_SYNCHRONOUS_IO_NONALERT | FILE_OPEN_REPARSE_POINT, &error);
    if (!file.valid()) return {.code = WinErrorToReadCode(error)};
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(file.get(), &info))
        return {.code = AuditEvidenceReadCode::StatFailed};
    if ((info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
        return {.code = AuditEvidenceReadCode::LoopOrReparse};
    if ((info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
        return {.code = AuditEvidenceReadCode::NonRegular};
    return {.code = AuditEvidenceReadCode::None};
#else
    if (mHandle < 0) return {.code = AuditEvidenceReadCode::IoError};
    const std::string name(InChildName);
    struct stat info {};
    if (::fstatat(static_cast<int>(mHandle), name.c_str(), &info,
                  AT_SYMLINK_NOFOLLOW) != 0) {
        const int savedError = errno;
        return {.code = ErrnoToReadCode(savedError)};
    }
    if (S_ISLNK(info.st_mode))
        return {.code = AuditEvidenceReadCode::LoopOrReparse};
    if (!S_ISREG(info.st_mode))
        return {.code = AuditEvidenceReadCode::NonRegular};
    return {.code = AuditEvidenceReadCode::None};
#endif
}

auto AuditEvidenceDirectory::Read(const std::string_view InChildName,
                                  const std::uintmax_t InLimit) const
    -> AuditEvidenceInput {
#if defined(_WIN32)
    if (!IsClosedComponent(InChildName))
        return {.code = AuditEvidenceReadCode::InvalidComponent,
                .diagnostic = BoundedSystemDiagnostic(
                    "cannot open bounded audit input", ERROR_INVALID_NAME)};
    if (mHandle == -1)
        return {.code = AuditEvidenceReadCode::IoError,
                .diagnostic = BoundedSystemDiagnostic(
                    "cannot open bounded audit input", ERROR_INVALID_HANDLE)};
    DWORD error = ERROR_SUCCESS;
    auto file = NtOpenRelative(reinterpret_cast<HANDLE>(mHandle), InChildName,
        FILE_READ_DATA | FILE_READ_ATTRIBUTES | SYNCHRONIZE,
        FILE_NON_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT | FILE_OPEN_REPARSE_POINT,
        &error);
    if (!file.valid())
        return {.code = WinErrorToReadCode(error),
                .diagnostic = BoundedSystemDiagnostic(
                    "cannot open bounded audit input", error)};
    BY_HANDLE_FILE_INFORMATION before{};
    if (!GetFileInformationByHandle(file.get(), &before))
        return {.code = AuditEvidenceReadCode::StatFailed,
                .diagnostic = BoundedRegularDiagnostic()};
    if ((before.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
        return {.code = AuditEvidenceReadCode::LoopOrReparse,
                .diagnostic = BoundedRegularDiagnostic()};
    if ((before.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
        return {.code = AuditEvidenceReadCode::NonRegular,
                .diagnostic = BoundedRegularDiagnostic()};
    const std::uint64_t size = (static_cast<std::uint64_t>(before.nFileSizeHigh) << 32U) |
        before.nFileSizeLow;
    if (size > InLimit || size > std::numeric_limits<std::size_t>::max())
        return {.code = AuditEvidenceReadCode::Limit,
                .diagnostic = BoundedRegularDiagnostic()};
    std::string bytes(static_cast<std::size_t>(size), '\0');
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        DWORD count = 0;
        const auto chunk = static_cast<DWORD>(std::min<std::size_t>(
            bytes.size() - offset, 1U << 30U));
        if (!ReadFile(file.get(), bytes.data() + offset, chunk, &count, nullptr)) {
            const DWORD savedError = GetLastError();
            return {.code = AuditEvidenceReadCode::IoError,
                    .diagnostic = BoundedSystemDiagnostic(
                        "cannot read bounded audit input", savedError)};
        }
        if (count == 0)
            return {.code = AuditEvidenceReadCode::IoError,
                    .diagnostic = BoundedSystemDiagnostic(
                        "cannot read bounded audit input", ERROR_HANDLE_EOF)};
        offset += count;
    }
    BY_HANDLE_FILE_INFORMATION after{};
    if (!GetFileInformationByHandle(file.get(), &after))
        return {.code = AuditEvidenceReadCode::StatFailed,
                .diagnostic = "audit input changed during bounded read"};
    if (!IsStable(before, after))
        return {.code = AuditEvidenceReadCode::Unstable,
                .diagnostic = "audit input changed during bounded read"};
    return {.code = AuditEvidenceReadCode::None, .bytes = std::move(bytes)};
#else
    if (!IsClosedComponent(InChildName))
        return {.code = AuditEvidenceReadCode::InvalidComponent,
                .diagnostic = BoundedSystemDiagnostic(
                    "cannot open bounded audit input", EINVAL)};
    if (mHandle < 0)
        return {.code = AuditEvidenceReadCode::IoError,
                .diagnostic = BoundedSystemDiagnostic(
                    "cannot open bounded audit input", EBADF)};
    const std::string name(InChildName);
    const int rawFile = ::openat(static_cast<int>(mHandle), name.c_str(),
        O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
    if (rawFile < 0) {
        const int savedError = errno;
        return {.code = ErrnoToReadCode(savedError),
                .diagnostic = BoundedSystemDiagnostic(
                    "cannot open bounded audit input", savedError)};
    }
    ScopedFd file(rawFile);
    struct stat before {};
    if (::fstat(file.get(), &before) != 0) {
        const int savedError = errno;
        (void)savedError;
        return {.code = AuditEvidenceReadCode::StatFailed,
                .diagnostic = BoundedRegularDiagnostic()};
    }
    if (!S_ISREG(before.st_mode))
        return {.code = AuditEvidenceReadCode::NonRegular,
                .diagnostic = BoundedRegularDiagnostic()};
    if (before.st_size < 0 || static_cast<std::uintmax_t>(before.st_size) > InLimit ||
        static_cast<std::uintmax_t>(before.st_size) > std::numeric_limits<std::size_t>::max())
        return {.code = AuditEvidenceReadCode::Limit,
                .diagnostic = BoundedRegularDiagnostic()};
    std::string bytes(static_cast<std::size_t>(before.st_size), '\0');
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const auto count = ::pread(file.get(), bytes.data() + offset,
                                   bytes.size() - offset,
                                   static_cast<off_t>(offset));
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) {
            const int savedError = errno;
            return {.code = AuditEvidenceReadCode::IoError,
                    .diagnostic = BoundedSystemDiagnostic(
                        "cannot read bounded audit input", savedError)};
        }
        if (count == 0)
            return {.code = AuditEvidenceReadCode::IoError,
                    .diagnostic = BoundedSystemDiagnostic(
                        "cannot read bounded audit input", EIO)};
        offset += static_cast<std::size_t>(count);
    }
    struct stat after {};
    if (::fstat(file.get(), &after) != 0) {
        const int savedError = errno;
        (void)savedError;
        return {.code = AuditEvidenceReadCode::StatFailed,
                .diagnostic = "audit input changed during bounded read"};
    }
    if (!IsStable(before, after))
        return {.code = AuditEvidenceReadCode::Unstable,
                .diagnostic = "audit input changed during bounded read"};
    return {.code = AuditEvidenceReadCode::None, .bytes = std::move(bytes)};
#endif
}

auto AuditEvidenceSystemDiagnostic(const AuditEvidenceOpenCode InCode)
    -> std::string_view {
    switch (InCode) {
    case AuditEvidenceOpenCode::None: return {};
    case AuditEvidenceOpenCode::Missing: return "audit evidence is missing";
    case AuditEvidenceOpenCode::InvalidComponent: return "audit evidence component is invalid";
    case AuditEvidenceOpenCode::LoopOrReparse: return "audit evidence path is a link or reparse point";
    case AuditEvidenceOpenCode::PermissionDenied: return "audit evidence access is denied";
    case AuditEvidenceOpenCode::IoError: return "audit evidence directory open failed";
    case AuditEvidenceOpenCode::StatFailed: return "audit evidence directory identity probe failed";
    case AuditEvidenceOpenCode::NotDirectory: return "audit evidence parent is not a directory";
    }
    return "audit evidence directory open failed";
}

auto AuditEvidenceSystemDiagnostic(const AuditEvidenceReadCode InCode)
    -> std::string_view {
    switch (InCode) {
    case AuditEvidenceReadCode::None: return {};
#if defined(_WIN32)
    case AuditEvidenceReadCode::Missing: return "cannot open bounded audit input: win32 error 2";
#else
    case AuditEvidenceReadCode::Missing: return "cannot open bounded audit input: No such file or directory";
#endif
    case AuditEvidenceReadCode::InvalidComponent: return "audit evidence component is invalid";
    case AuditEvidenceReadCode::LoopOrReparse: return "audit evidence child is a link or reparse point";
    case AuditEvidenceReadCode::PermissionDenied: return "audit evidence child access is denied";
    case AuditEvidenceReadCode::IoError: return "cannot read bounded audit input";
    case AuditEvidenceReadCode::StatFailed: return "cannot prove bounded audit input identity";
    case AuditEvidenceReadCode::NonRegular: return "audit input must be a bounded regular non-link file";
    case AuditEvidenceReadCode::Limit: return "audit input exceeds bounded read limit";
    case AuditEvidenceReadCode::Unstable: return "audit input changed during bounded read";
    }
    return "cannot read bounded audit input";
}

} // namespace kano::git::commands
