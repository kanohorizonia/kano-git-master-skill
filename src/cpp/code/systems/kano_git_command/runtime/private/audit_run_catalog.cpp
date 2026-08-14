#include "audit_run_catalog.hpp"
#include "audit_run_catalog_private.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <fstream>
#include <map>
#include <limits>
#include <mutex>
#include <set>
#include <system_error>
#include <tuple>
#include <thread>
#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#include <winternl.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace kano::git::commands {
namespace {
constexpr std::string_view kCatalogSchema = "kog.auditRunCatalog";
constexpr std::string_view kPointerSchema = "kog.auditRunCatalogPointer";
constexpr std::uint64_t kCatalogCursorSchemaVersion = 2;
constexpr auto kCatalogCursorLifetimeMaximum = std::chrono::seconds(600);
constexpr std::size_t kCatalogStorageByteCeiling = 256U << 10U;
constexpr std::size_t kCatalogEntryCeiling = 4096;
std::atomic_uint64_t gCatalogTemporarySequence = 0;
std::mutex gCatalogHooksMutex;
AuditRunCatalogTestHooks gCatalogHooks;
auto Hooks() -> AuditRunCatalogTestHooks { std::lock_guard lock(gCatalogHooksMutex); return gCatalogHooks; }
auto SystemNow() -> std::chrono::system_clock::time_point { const auto hooks = Hooks(); return hooks.systemNow ? hooks.systemNow() : std::chrono::system_clock::now(); }
auto MonotonicNow() -> std::chrono::steady_clock::time_point { const auto hooks = Hooks(); return hooks.monotonicNow ? hooks.monotonicNow() : std::chrono::steady_clock::now(); }
auto Stage(const std::string_view name) -> bool { const auto hooks = Hooks(); if (hooks.publicationStage) hooks.publicationStage(name); return hooks.failStage && hooks.failStage(name); }

#if defined(_WIN32)
auto NtCreateRelative(const HANDLE parent, const std::string& name,
                      const ACCESS_MASK access, const ULONG disposition,
                      const ULONG options) -> HANDLE {
    std::wstring storage=std::filesystem::path(name).wstring();
    const auto byteCount=storage.size()*sizeof(wchar_t);
    if (storage.empty() || byteCount>std::numeric_limits<USHORT>::max()) return INVALID_HANDLE_VALUE;
    UNICODE_STRING unicode{static_cast<USHORT>(byteCount),static_cast<USHORT>(byteCount),storage.data()};
    OBJECT_ATTRIBUTES attributes{}; InitializeObjectAttributes(&attributes,&unicode,OBJ_CASE_INSENSITIVE,parent,nullptr);
    IO_STATUS_BLOCK statusBlock{}; HANDLE handle=INVALID_HANDLE_VALUE;
    const auto status=NtCreateFile(&handle,access,&attributes,&statusBlock,nullptr,
        FILE_ATTRIBUTE_NORMAL,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
        disposition,options,nullptr,0);
    return status>=0 ? handle : INVALID_HANDLE_VALUE;
}
auto SafeWindowsDirectoryHandle(const HANDLE handle) -> bool {
    BY_HANDLE_FILE_INFORMATION info{};
    return handle!=INVALID_HANDLE_VALUE && GetFileInformationByHandle(handle,&info) &&
        (info.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY) && !(info.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT);
}
auto SafeWindowsRegularHandle(const HANDLE handle) -> bool {
    BY_HANDLE_FILE_INFORMATION info{};
    return handle!=INVALID_HANDLE_VALUE && GetFileInformationByHandle(handle,&info) &&
        !(info.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT));
}
#endif

auto IsLowerHex(const std::string_view value, const std::size_t length) -> bool {
    return value.size() == length && std::all_of(value.begin(), value.end(), [](const char ch) {
        return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
    });
}
auto IsSchemaVersionOne(const nlohmann::json& value) -> bool {
    if (value.is_number_unsigned()) return value.get<std::uint64_t>() == 1;
    if (value.is_number_integer()) return value.get<std::int64_t>() == 1;
    return false;
}
auto IsCatalogCursorSchemaVersion(const nlohmann::json& value) -> bool {
    if (value.is_number_unsigned()) return value.get<std::uint64_t>() == kCatalogCursorSchemaVersion;
    if (value.is_number_integer()) return value.get<std::int64_t>() == static_cast<std::int64_t>(kCatalogCursorSchemaVersion);
    return false;
}
auto NonnegativeInt64(const nlohmann::json& value) -> std::optional<std::int64_t> {
    if (value.is_number_unsigned()) {
        const auto raw=value.get<std::uint64_t>();
        if(raw<=static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) return static_cast<std::int64_t>(raw);
    } else if(value.is_number_integer()) {
        const auto raw=value.get<std::int64_t>(); if(raw>=0)return raw;
    }
    return std::nullopt;
}
auto BoundedSize(const nlohmann::json& value) -> std::optional<std::size_t> {
    std::uint64_t unsignedRaw=0;
    if(value.is_number_unsigned()) unsignedRaw=value.get<std::uint64_t>();
    else if(value.is_number_integer()){const auto raw=value.get<std::int64_t>();if(raw<0)return std::nullopt;unsignedRaw=static_cast<std::uint64_t>(raw);}
    else return std::nullopt;
    if(unsignedRaw>std::numeric_limits<std::size_t>::max())return std::nullopt;
    return static_cast<std::size_t>(unsignedRaw);
}
auto IsStrictUtcTimestamp(const std::string_view value) -> bool {
    if (value.size() != 20 || value[4] != '-' || value[7] != '-' || value[10] != 'T' || value[13] != ':' || value[16] != ':' || value[19] != 'Z') return false;
    const auto number = [&](const std::size_t offset, const std::size_t count) { int out = 0; for (std::size_t i = 0; i < count; ++i) { const char ch = value[offset + i]; if (ch < '0' || ch > '9') return -1; out = out * 10 + ch - '0'; } return out; };
    const int year = number(0, 4), month = number(5, 2), day = number(8, 2), hour = number(11, 2), minute = number(14, 2), second = number(17, 2);
    if (year < 1 || month < 1 || month > 12 || day < 1 || hour > 23 || minute > 59 || second > 59) return false;
    static constexpr std::array<int, 12> days = {31,28,31,30,31,30,31,31,30,31,30,31};
    return day <= days[month - 1] + (month == 2 && (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0)) ? 1 : 0);
}
auto IsSupportedRouteInputPair(const std::string_view route, const std::string_view input) -> bool {
    static constexpr std::array<std::pair<std::string_view, std::string_view>, 7> pairs = {{{"commit.plan","commit-plan"},{"commit-push.plan","commit-plan"},{"plan.apply","commit-plan"},{"converge.repos","operation-descriptor"},{"converge.branches.apply","operation-descriptor"},{"converge.branches.recover","operation-descriptor"},{"converge.branches.retire","operation-descriptor"}}};
    return std::any_of(pairs.begin(), pairs.end(), [&](const auto& pair) { return pair.first == route && pair.second == input; });
}

auto CatalogRoot(const OperationAuditSpec& InSpec, const OperationAuditPaths& InPaths) -> std::filesystem::path {
    const auto selector = (InSpec.inputKind == "commit-plan" ? "plan-" : "operation-") + audit::Sha256Hex(InSpec.inputIdentity);
    if (InPaths.auditRoot.filename() == selector)
        return InPaths.auditRoot.parent_path().parent_path() / "catalog-v1";
    return InSpec.workspaceRoot / ".kano" / "tmp" / "git" / "catalog-v1";
}
auto CatalogAnchor(const OperationAuditSpec& spec, const OperationAuditPaths& paths)
    -> std::optional<std::filesystem::path> {
    const auto workspace = spec.workspaceRoot.lexically_normal();
    const auto normalizedAuditRoot = paths.auditRoot.lexically_normal();
    if (spec.sourcePath) {
        const auto sourceAdjacent = (spec.sourcePath->parent_path() /
            (spec.sourcePath->filename().string() + ".audit")).lexically_normal();
        if (normalizedAuditRoot == sourceAdjacent) return workspace;
    }
    const auto selector = (spec.inputKind == "commit-plan" ? "plan-" : "operation-") + audit::Sha256Hex(spec.inputIdentity);
    if (paths.auditRoot.filename() != selector) return std::nullopt;
    // ResolveOperationAuditPaths already admitted this Git common-dir-derived
    // audit root. Derive its anchor from that resolved trusted layout only;
    // catalog queries must not add an independent shell/probing boundary.
    const auto auditDirectory = paths.auditRoot.parent_path();
    const auto kogDirectory = auditDirectory.parent_path();
    const auto anchor = kogDirectory.parent_path();
    if (auditDirectory.filename() != "audit") return std::nullopt;
    if (kogDirectory.filename() == "kog" && !anchor.empty()) return anchor.lexically_normal();
    // No-Git fallback is strictly workspace/.kano/tmp/git/audit/<selector>.
    const auto fallbackAudit = (workspace / ".kano" / "tmp" / "git" / "audit").lexically_normal();
    if (auditDirectory.lexically_normal() == fallbackAudit) return workspace;
    return std::nullopt;
}
auto IsWithinAnchor(const std::filesystem::path& root, const std::filesystem::path& anchor) -> bool {
    const auto normalizedRoot = root.lexically_normal(); const auto normalizedAnchor = anchor.lexically_normal();
    auto rootIt = normalizedRoot.begin(); const auto rootEnd = normalizedRoot.end();
    auto anchorIt = normalizedAnchor.begin(); const auto anchorEnd = normalizedAnchor.end();
    for (; anchorIt != anchorEnd; ++anchorIt, ++rootIt)
        if (rootIt == rootEnd || *rootIt != *anchorIt) return false;
    return true;
}
class PinnedCatalogRoot {
public:
    ~PinnedCatalogRoot() {
#if !defined(_WIN32)
        if (mHandle >= 0) ::close(mHandle);
#else
        if (mHandle != INVALID_HANDLE_VALUE) CloseHandle(mHandle);
#endif
    }
    PinnedCatalogRoot(const PinnedCatalogRoot&) = delete;
    auto operator=(const PinnedCatalogRoot&) -> PinnedCatalogRoot& = delete;
    PinnedCatalogRoot(PinnedCatalogRoot&& other) noexcept : mHandle(other.release()) {}
    static auto Open(const std::filesystem::path& root, const std::filesystem::path& anchor,
                     const bool create) -> std::optional<PinnedCatalogRoot> {
        if (!IsWithinAnchor(root, anchor)) return std::nullopt;
#if !defined(_WIN32)
        int current = ::open(anchor.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (current < 0) return std::nullopt;
        const auto normalizedRoot = root.lexically_normal(); const auto normalizedAnchor = anchor.lexically_normal();
        auto it = normalizedRoot.begin(); for (auto anchorIt = normalizedAnchor.begin(); anchorIt != normalizedAnchor.end(); ++anchorIt, ++it) {}
        for (; it != normalizedRoot.end(); ++it) {
            const auto component = it->string();
            int next = ::openat(current, component.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            if (next < 0 && create && errno == ENOENT) {
                if (::mkdirat(current, component.c_str(), 0700) != 0 && errno != EEXIST) { ::close(current); return std::nullopt; }
                next = ::openat(current, component.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
                if (next >= 0) (void)::fsync(current);
            }
            ::close(current); if (next < 0) return std::nullopt; current = next;
        }
        return PinnedCatalogRoot(current);
#else
        const auto anchorAccess = FILE_LIST_DIRECTORY | FILE_TRAVERSE |
            FILE_READ_ATTRIBUTES | SYNCHRONIZE |
            (create ? FILE_ADD_SUBDIRECTORY : 0);
        HANDLE current = CreateFileW(anchor.c_str(), anchorAccess,
                                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                        OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (!SafeWindowsDirectoryHandle(current)) { if(current!=INVALID_HANDLE_VALUE)CloseHandle(current); return std::nullopt; }
        const auto normalizedRoot=root.lexically_normal(); const auto normalizedAnchor=anchor.lexically_normal();
        auto it=normalizedRoot.begin(); for(auto anchorIt=normalizedAnchor.begin();anchorIt!=normalizedAnchor.end();++anchorIt,++it){}
        for(;it!=normalizedRoot.end();++it){
            auto nextIt = it; ++nextIt;
            const auto finalComponent = nextIt == normalizedRoot.end();
            // FILE_TRAVERSE is required both for child opens relative to this
            // handle and when the final root is used by FileRenameInfo.
            const auto access = FILE_LIST_DIRECTORY | FILE_TRAVERSE |
                FILE_READ_ATTRIBUTES | SYNCHRONIZE |
                (create ? (finalComponent ? FILE_ADD_FILE | FILE_DELETE_CHILD : FILE_ADD_SUBDIRECTORY) : 0);
            // FILE_DIRECTORY_FILE cannot be combined with FILE_OPEN_REPARSE_POINT.
            // First admit an existing component as the reparse point itself so a
            // junction/symlink is observed and rejected instead of traversed.
            HANDLE next=NtCreateRelative(current,it->string(),access,FILE_OPEN,
                FILE_OPEN_REPARSE_POINT|FILE_SYNCHRONOUS_IO_NONALERT);
            if (next == INVALID_HANDLE_VALUE && create) {
                // Creation needs FILE_DIRECTORY_FILE, which intentionally omits
                // FILE_OPEN_REPARSE_POINT.  A racing creator is admitted only by
                // retrying the secure existing-component open above.
                next=NtCreateRelative(current,it->string(),access,FILE_CREATE,
                    FILE_DIRECTORY_FILE|FILE_SYNCHRONOUS_IO_NONALERT);
                if (next == INVALID_HANDLE_VALUE) {
                    next=NtCreateRelative(current,it->string(),access,FILE_OPEN,
                        FILE_OPEN_REPARSE_POINT|FILE_SYNCHRONOUS_IO_NONALERT);
                }
            }
            CloseHandle(current);
            if(!SafeWindowsDirectoryHandle(next)){if(next!=INVALID_HANDLE_VALUE)CloseHandle(next);return std::nullopt;}
            current=next;
        }
        return PinnedCatalogRoot(current);
#endif
    }
#if !defined(_WIN32)
    [[nodiscard]] auto get() const -> int { return mHandle; }
#else
    [[nodiscard]] auto get() const -> HANDLE { return mHandle; }
#endif
private:
#if !defined(_WIN32)
    explicit PinnedCatalogRoot(const int handle) : mHandle(handle) {}
    auto release() -> int { const auto value=mHandle; mHandle=-1; return value; }
    int mHandle = -1;
#else
    explicit PinnedCatalogRoot(const HANDLE handle) : mHandle(handle) {}
    auto release() -> HANDLE { const auto value=mHandle; mHandle=INVALID_HANDLE_VALUE; return value; }
    HANDLE mHandle = INVALID_HANDLE_VALUE;
#endif
};

class ScopedCatalogWriterLock {
public:
    explicit ScopedCatalogWriterLock(const PinnedCatalogRoot& pinned,
                                     const std::filesystem::path& root)
    {
        (void)root;
#if !defined(_WIN32)
        mHandle = ::openat(pinned.get(), "writer.lock", O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK, 0600);
        if (mHandle >= 0) {
            struct stat info{};
            if (::fstat(mHandle, &info) != 0 || !S_ISREG(info.st_mode)) { ::close(mHandle); mHandle = -1; return; }
            int result = -1;
            do {
                result = ::flock(mHandle, LOCK_EX | LOCK_NB);
            } while (result != 0 && errno == EINTR);
            mOwned = result == 0;
        }
#else
        mHandle = NtCreateRelative(pinned.get(), "writer.lock", GENERIC_READ|GENERIC_WRITE|SYNCHRONIZE,
                                   FILE_OPEN_IF, FILE_NON_DIRECTORY_FILE|FILE_OPEN_REPARSE_POINT|FILE_SYNCHRONOUS_IO_NONALERT);
        BY_HANDLE_FILE_INFORMATION info{};
        if (mHandle == INVALID_HANDLE_VALUE || !GetFileInformationByHandle(mHandle, &info) ||
            (info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY))) {
            if (mHandle != INVALID_HANDLE_VALUE) CloseHandle(mHandle);
            mHandle=INVALID_HANDLE_VALUE; return;
        }
        OVERLAPPED overlap{}; mOwned=LockFileEx(mHandle,LOCKFILE_EXCLUSIVE_LOCK|LOCKFILE_FAIL_IMMEDIATELY,0,MAXDWORD,MAXDWORD,&overlap)!=FALSE;
#endif
    }
    ~ScopedCatalogWriterLock() {
#if !defined(_WIN32)
        if (mHandle>=0) { if(mOwned)(void)::flock(mHandle,LOCK_UN); ::close(mHandle); }
#else
        if(mHandle!=INVALID_HANDLE_VALUE){ if(mOwned){OVERLAPPED overlap{};(void)UnlockFileEx(mHandle,0,MAXDWORD,MAXDWORD,&overlap);}CloseHandle(mHandle);}
#endif
    }
    [[nodiscard]] auto owned() const noexcept -> bool { return mOwned; }
private:
    bool mOwned=false;
#if !defined(_WIN32)
    int mHandle=-1;
#else
    HANDLE mHandle=INVALID_HANDLE_VALUE;
#endif
};
auto StateName(const OperationAuditCatalogState InState) -> std::string_view {
    switch (InState) {
    case OperationAuditCatalogState::Pending: return "pending";
    case OperationAuditCatalogState::Incomplete: return "incomplete";
    case OperationAuditCatalogState::Final: return "final";
    }
    return "";
}
auto ParseState(const std::string_view InValue) -> std::optional<OperationAuditCatalogState> {
    if (InValue == "pending") return OperationAuditCatalogState::Pending;
    if (InValue == "incomplete") return OperationAuditCatalogState::Incomplete;
    if (InValue == "final") return OperationAuditCatalogState::Final;
    return std::nullopt;
}
auto ParseOutcome(const std::string_view value) -> std::optional<audit::OutcomeState> {
    using audit::OutcomeState;
    if (value == "succeeded") return OutcomeState::Succeeded;
    if (value == "failed") return OutcomeState::Failed;
    if (value == "partial") return OutcomeState::Partial;
    if (value == "blocked") return OutcomeState::Blocked;
    if (value == "cancelled") return OutcomeState::Cancelled;
    if (value == "timed-out") return OutcomeState::TimedOut;
    if (value == "unknown") return OutcomeState::Unknown;
    return std::nullopt;
}
auto CursorIntegrity(const std::string_view generation, const std::string_view generationSha256,
                     const std::string_view filter, const std::int64_t issued,
                     const std::int64_t expires, const std::size_t offset) -> std::string {
    return audit::Sha256Hex(std::string(generation) + "\n" + std::string(generationSha256) + "\n" +
                            std::string(filter) + "\n" + std::to_string(issued) + "\n" +
                            std::to_string(expires) + "\n" + std::to_string(offset));
}
auto ReadBoundedAt(const PinnedCatalogRoot& rootHandle, const std::filesystem::path& root,
                   const std::string& name, const std::size_t limit) -> std::optional<std::string> {
    (void)root;
#if !defined(_WIN32)
    const auto handle = ::openat(rootHandle.get(), name.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW);
    if (handle < 0) return std::nullopt;
    struct stat before{}; if (::fstat(handle, &before) != 0 || !S_ISREG(before.st_mode) || before.st_size < 0 || static_cast<std::uintmax_t>(before.st_size) > limit) { ::close(handle); return std::nullopt; }
    std::string bytes(static_cast<std::size_t>(before.st_size), '\0'); std::size_t offset=0;
    while (offset < bytes.size()) { const auto count=::read(handle, bytes.data()+offset, bytes.size()-offset); if (count <= 0) { ::close(handle); return std::nullopt; } offset += static_cast<std::size_t>(count); }
    struct stat after{}; const bool stable=::fstat(handle,&after)==0 && before.st_dev==after.st_dev && before.st_ino==after.st_ino && before.st_size==after.st_size;
    ::close(handle); return stable ? std::optional<std::string>(std::move(bytes)) : std::nullopt;
#else
    const auto handle=NtCreateRelative(rootHandle.get(),name,GENERIC_READ|FILE_READ_ATTRIBUTES|SYNCHRONIZE,
        FILE_OPEN,FILE_NON_DIRECTORY_FILE|FILE_OPEN_REPARSE_POINT|FILE_SYNCHRONOUS_IO_NONALERT);
    if(!SafeWindowsRegularHandle(handle)) { if(handle!=INVALID_HANDLE_VALUE)CloseHandle(handle); return std::nullopt; }
    BY_HANDLE_FILE_INFORMATION before{}; if(!GetFileInformationByHandle(handle,&before)){CloseHandle(handle);return std::nullopt;}
    const auto size=(static_cast<std::uint64_t>(before.nFileSizeHigh)<<32U)|before.nFileSizeLow;
    if(size>limit||size>MAXDWORD){CloseHandle(handle);return std::nullopt;}
    std::string bytes(static_cast<std::size_t>(size),'\0'); DWORD read=0;
    const bool ok=bytes.empty()||(ReadFile(handle,bytes.data(),static_cast<DWORD>(bytes.size()),&read,nullptr)&&read==bytes.size());
    BY_HANDLE_FILE_INFORMATION after{}; const bool stable=GetFileInformationByHandle(handle,&after)&&before.nFileIndexHigh==after.nFileIndexHigh&&before.nFileIndexLow==after.nFileIndexLow&&before.nFileSizeHigh==after.nFileSizeHigh&&before.nFileSizeLow==after.nFileSizeLow;
    CloseHandle(handle); return ok&&stable?std::optional<std::string>(std::move(bytes)):std::nullopt;
#endif
}
auto ChildExistsAt(const PinnedCatalogRoot& rootHandle, const std::string& name) -> bool {
#if !defined(_WIN32)
    struct stat info{}; return ::fstatat(rootHandle.get(),name.c_str(),&info,AT_SYMLINK_NOFOLLOW)==0;
#else
    const auto handle=NtCreateRelative(rootHandle.get(),name,FILE_READ_ATTRIBUTES|SYNCHRONIZE,FILE_OPEN,
        FILE_OPEN_REPARSE_POINT|FILE_SYNCHRONOUS_IO_NONALERT);
    const bool exists=handle!=INVALID_HANDLE_VALUE; if(exists)CloseHandle(handle); return exists;
#endif
}
auto WriteDurableNewAt(const PinnedCatalogRoot& rootHandle, const std::filesystem::path& root,
                       const std::string& name, const std::string& bytes) -> bool {
    (void)root;
#if !defined(_WIN32)
    const auto handle=::openat(rootHandle.get(), name.c_str(), O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW,0600);
    if (handle < 0) return false; std::size_t offset=0;
    while (offset<bytes.size()) { const auto count=::write(handle,bytes.data()+offset,bytes.size()-offset); if (count<=0) { ::close(handle); return false; } offset+=static_cast<std::size_t>(count); }
    const bool ok=::fsync(handle)==0; ::close(handle); return ok;
#else
    const auto handle=NtCreateRelative(rootHandle.get(),name,GENERIC_WRITE|FILE_READ_ATTRIBUTES|SYNCHRONIZE,
        FILE_CREATE,FILE_NON_DIRECTORY_FILE|FILE_OPEN_REPARSE_POINT|FILE_SYNCHRONOUS_IO_NONALERT);
    if(!SafeWindowsRegularHandle(handle)||bytes.size()>MAXDWORD){if(handle!=INVALID_HANDLE_VALUE)CloseHandle(handle);return false;}
    DWORD written=0; const bool ok=(bytes.empty()||(WriteFile(handle,bytes.data(),static_cast<DWORD>(bytes.size()),&written,nullptr)&&written==bytes.size()))&&FlushFileBuffers(handle);
    CloseHandle(handle); return ok;
#endif
}
#if defined(_WIN32)
auto RenameWindowsAt(const PinnedCatalogRoot& rootHandle, const std::string& from,
                     const std::string& to, const bool replace, bool* outAlreadyExists=nullptr) -> bool {
    if(outAlreadyExists)*outAlreadyExists=false;
    const auto source=NtCreateRelative(rootHandle.get(),from,DELETE|FILE_READ_ATTRIBUTES|SYNCHRONIZE,FILE_OPEN,
        FILE_NON_DIRECTORY_FILE|FILE_OPEN_REPARSE_POINT|FILE_SYNCHRONOUS_IO_NONALERT);
    if(!SafeWindowsRegularHandle(source)){if(source!=INVALID_HANDLE_VALUE)CloseHandle(source);return false;}
    const auto target=std::filesystem::path(to).wstring();
    const auto targetBytes=target.size()*sizeof(wchar_t);
    std::vector<unsigned char> storage(sizeof(FILE_RENAME_INFO)+targetBytes);
    auto* info=reinterpret_cast<FILE_RENAME_INFO*>(storage.data());
    info->ReplaceIfExists=replace?TRUE:FALSE; info->RootDirectory=rootHandle.get();
    info->FileNameLength=static_cast<DWORD>(targetBytes);
    std::memcpy(info->FileName,target.data(),targetBytes);
    const bool renamed=SetFileInformationByHandle(source,FileRenameInfo,info,static_cast<DWORD>(storage.size()))!=FALSE;
    const auto renameError=renamed?ERROR_SUCCESS:GetLastError();
    if(outAlreadyExists)*outAlreadyExists=renameError==ERROR_ALREADY_EXISTS||renameError==ERROR_FILE_EXISTS;
    if(!renamed){FILE_DISPOSITION_INFO dispose{TRUE};(void)SetFileInformationByHandle(source,FileDispositionInfo,&dispose,sizeof(dispose));}
    CloseHandle(source); if(!renamed)return false;
    const auto installed=NtCreateRelative(rootHandle.get(),to,GENERIC_READ|GENERIC_WRITE|SYNCHRONIZE,FILE_OPEN,
        FILE_NON_DIRECTORY_FILE|FILE_OPEN_REPARSE_POINT|FILE_SYNCHRONOUS_IO_NONALERT);
    const bool durable=SafeWindowsRegularHandle(installed)&&FlushFileBuffers(installed);
    if(installed!=INVALID_HANDLE_VALUE)CloseHandle(installed); return durable;
}
#endif
enum class InstallResult { Installed, AlreadyExists, Error };
auto RenameReplaceAt(const PinnedCatalogRoot& rootHandle, const std::filesystem::path& root,
                     const std::string& from, const std::string& to) -> bool {
    (void)root;
#if !defined(_WIN32)
    return ::renameat(rootHandle.get(), from.c_str(), rootHandle.get(), to.c_str()) == 0 && ::fsync(rootHandle.get()) == 0;
#else
    return RenameWindowsAt(rootHandle,from,to,true);
#endif
}
auto InstallNoReplaceAt(const PinnedCatalogRoot& rootHandle, const std::filesystem::path& root,
                        const std::string& from, const std::string& to) -> InstallResult {
    (void)root;
#if !defined(_WIN32)
    if (::linkat(rootHandle.get(), from.c_str(), rootHandle.get(), to.c_str(), 0) != 0)
        return errno==EEXIST?InstallResult::AlreadyExists:InstallResult::Error;
    return ::unlinkat(rootHandle.get(),from.c_str(),0)==0 && ::fsync(rootHandle.get())==0
        ? InstallResult::Installed : InstallResult::Error;
#else
    bool alreadyExists=false;
    if(RenameWindowsAt(rootHandle,from,to,false,&alreadyExists))return InstallResult::Installed;
    return alreadyExists?InstallResult::AlreadyExists:InstallResult::Error;
#endif
}
auto EntryJson(const OperationAuditCatalogEntry& InEntry) -> nlohmann::json {
    auto out = nlohmann::json{{"runId", InEntry.runId}, {"parentRunId", InEntry.parentRunId ? nlohmann::json(*InEntry.parentRunId) : nlohmann::json(nullptr)},
            {"attempt", InEntry.attempt}, {"inputKind", InEntry.inputKind}, {"route", InEntry.route}, {"planId", InEntry.planId},
            {"planSha256", InEntry.planSha256}, {"sourceSha256", InEntry.sourceSha256}, {"sourceSizeBytes", InEntry.sourceSizeBytes}, {"auditRootSelector", InEntry.auditRootSelector}, {"auditRootSha256", InEntry.auditRootSha256},
            {"state", StateName(InEntry.state)}, {"receiptSha256", InEntry.receiptSha256 ? nlohmann::json(*InEntry.receiptSha256) : nlohmann::json(nullptr)},
            {"outcome", InEntry.outcome ? nlohmann::json(audit::OutcomeStateName(*InEntry.outcome)) : nlohmann::json(nullptr)},
            {"repositories", nlohmann::json::array()}, {"repositoryIdentityHeadSha256", InEntry.repositoryIdentityHeadSha256}, {"correlationSha256", InEntry.correlationSha256},
            {"observedAtUtc", InEntry.observedAtUtc},
            {"finishedAtUtc", InEntry.finishedAtUtc ? nlohmann::json(*InEntry.finishedAtUtc) : nlohmann::json(nullptr)},
            {"redaction", {{"redacted", InEntry.redaction.redacted}, {"withheld", InEntry.redaction.withheld}}},
            {"truncation", {{"omittedEvents", InEntry.truncation.omittedEvents}, {"omittedRepositories", InEntry.truncation.omittedRepositories}, {"omittedEvidence", InEntry.truncation.omittedEvidence}}}};
    for (const auto& repository : InEntry.repositories) {
        out["repositories"].push_back({{"repositoryId", repository.repositoryId},
                                        {"afterHeadSha", repository.afterHeadSha ? nlohmann::json(*repository.afterHeadSha) : nlohmann::json(nullptr)}});
    }
    return out;
}
auto ParseEntry(const nlohmann::json& InDoc) -> std::optional<OperationAuditCatalogEntry> {
    static const std::set<std::string> kFields = {"runId", "parentRunId", "attempt", "inputKind", "route", "planId", "planSha256", "sourceSha256", "sourceSizeBytes", "auditRootSelector", "auditRootSha256", "state", "outcome", "repositories", "repositoryIdentityHeadSha256", "correlationSha256", "observedAtUtc", "receiptSha256", "finishedAtUtc", "redaction", "truncation"};
    if (InDoc.is_object()) { std::set<std::string> actual; for (auto it = InDoc.begin(); it != InDoc.end(); ++it) actual.insert(it.key()); if (actual != kFields) return std::nullopt; }
    if (!InDoc.is_object() || !InDoc.contains("runId") || !InDoc["runId"].is_string() ||
        !InDoc.contains("attempt") || !InDoc["attempt"].is_number_unsigned() ||
        InDoc["attempt"].get<std::uint64_t>() > std::numeric_limits<std::uint32_t>::max() ||
        !InDoc.contains("inputKind") || !InDoc["inputKind"].is_string() || !InDoc.contains("route") || !InDoc["route"].is_string() || !InDoc.contains("planId") || !InDoc["planId"].is_string() || !InDoc.contains("sourceSha256") || !InDoc["sourceSha256"].is_string() || !InDoc.contains("sourceSizeBytes") || !InDoc["sourceSizeBytes"].is_number_unsigned() ||
        !InDoc.contains("planSha256") || !InDoc["planSha256"].is_string() ||
        !InDoc.contains("auditRootSelector") || !InDoc["auditRootSelector"].is_string() ||
        !InDoc.contains("auditRootSha256") || !InDoc["auditRootSha256"].is_string() ||
        !InDoc.contains("state") || !InDoc["state"].is_string()) return std::nullopt;
    const auto state = ParseState(InDoc["state"].get<std::string>()); if (!state) return std::nullopt;
    OperationAuditCatalogEntry out;
    out.runId=InDoc["runId"].get<std::string>(); out.attempt=InDoc["attempt"].get<std::uint32_t>();
    out.inputKind=InDoc["inputKind"].get<std::string>(); out.route=InDoc["route"].get<std::string>();
    out.planId=InDoc["planId"].get<std::string>(); out.planSha256=InDoc["planSha256"].get<std::string>();
    out.sourceSha256=InDoc["sourceSha256"].get<std::string>(); out.sourceSizeBytes=InDoc["sourceSizeBytes"].get<std::uint64_t>();
    out.auditRootSelector=InDoc["auditRootSelector"].get<std::string>(); out.auditRootSha256=InDoc["auditRootSha256"].get<std::string>(); out.state=*state;
    const auto optionalString = [&](const char* name, std::optional<std::string>* target) -> bool {
        if (!InDoc.contains(name) || InDoc[name].is_null()) return InDoc.contains(name);
        if (!InDoc[name].is_string()) return false; *target = InDoc[name].get<std::string>(); return true;
    };
    if (!optionalString("parentRunId", &out.parentRunId) || !optionalString("receiptSha256", &out.receiptSha256) ||
        !optionalString("finishedAtUtc", &out.finishedAtUtc) || out.runId.empty() || out.attempt == 0 ||
        !audit::IsStableAuditId(out.runId) || !audit::IsStableAuditId(out.planId) ||
        !IsSupportedRouteInputPair(out.route, out.inputKind) || !IsLowerHex(out.sourceSha256, 64) || out.sourceSizeBytes > (4U << 20U) || !IsLowerHex(out.auditRootSha256, 64) || !IsLowerHex(out.planSha256, 64) ||
        !((out.inputKind == "commit-plan" && out.auditRootSelector.starts_with("plan-") && IsLowerHex(out.auditRootSelector.substr(5), 64)) || (out.inputKind == "operation-descriptor" && out.auditRootSelector.starts_with("operation-") && IsLowerHex(out.auditRootSelector.substr(10), 64))) ||
        out.auditRootSelector.find('/') != std::string::npos || out.auditRootSelector.find('\\') != std::string::npos || out.auditRootSelector.find("..") != std::string::npos) return std::nullopt;
    if (!InDoc.contains("outcome") || (!InDoc["outcome"].is_null() && !InDoc["outcome"].is_string()) ||
        !InDoc.contains("repositories") || !InDoc["repositories"].is_array() || InDoc["repositories"].size() > 64 ||
        !InDoc.contains("repositoryIdentityHeadSha256") || !InDoc["repositoryIdentityHeadSha256"].is_string() || !InDoc.contains("correlationSha256") || !InDoc["correlationSha256"].is_string() ||
        !InDoc.contains("observedAtUtc") || !InDoc["observedAtUtc"].is_string() ||
        !InDoc.contains("redaction") || !InDoc["redaction"].is_object() ||
        !InDoc.contains("truncation") || !InDoc["truncation"].is_object()) return std::nullopt;
    if (!InDoc["outcome"].is_null()) { out.outcome = ParseOutcome(InDoc["outcome"].get<std::string>()); if (!out.outcome) return std::nullopt; }
    out.repositoryIdentityHeadSha256 = InDoc["repositoryIdentityHeadSha256"].get<std::string>(); out.correlationSha256 = InDoc["correlationSha256"].get<std::string>(); out.observedAtUtc = InDoc["observedAtUtc"].get<std::string>();
    if (!IsLowerHex(out.correlationSha256, 64) || !IsStrictUtcTimestamp(out.observedAtUtc) ||
        (out.parentRunId && !audit::IsStableAuditId(*out.parentRunId)) ||
        (out.receiptSha256 && !IsLowerHex(*out.receiptSha256, 64)) ||
        (out.finishedAtUtc && !IsStrictUtcTimestamp(*out.finishedAtUtc))) return std::nullopt;
    for (const auto& repository : InDoc["repositories"]) {
        static const std::set<std::string> kRepositoryFields = {"repositoryId", "afterHeadSha"}; std::set<std::string> actual;
        if (repository.is_object()) for (auto it = repository.begin(); it != repository.end(); ++it) actual.insert(it.key());
        if (!repository.is_object() || actual != kRepositoryFields || !repository.contains("repositoryId") || !repository["repositoryId"].is_string() ||
            !repository.contains("afterHeadSha") || (!repository["afterHeadSha"].is_null() && !repository["afterHeadSha"].is_string())) return std::nullopt;
        OperationAuditCatalogRepositoryCommit value{.repositoryId = repository["repositoryId"].get<std::string>()};
        if (!audit::IsStableAuditId(value.repositoryId)) return std::nullopt;
        if (!repository["afterHeadSha"].is_null()) { value.afterHeadSha = repository["afterHeadSha"].get<std::string>(); if ((value.afterHeadSha->size() != 40 && value.afterHeadSha->size() != 64) || !IsLowerHex(*value.afterHeadSha, value.afterHeadSha->size())) return std::nullopt; }
        if (std::any_of(out.repositories.begin(), out.repositories.end(), [&](const auto& existing) { return existing.repositoryId == value.repositoryId; })) return std::nullopt;
        out.repositories.push_back(std::move(value));
    }
    const auto parseCount = [](const nlohmann::json& object, const char* name, std::uint32_t* target) -> bool {
        if (!object.contains(name) || !object[name].is_number_unsigned() ||
            object[name].get<std::uint64_t>() > std::numeric_limits<std::uint32_t>::max()) return false;
        *target = object[name].get<std::uint32_t>(); return true;
    };
    static const std::set<std::string> kRedactionFields = {"redacted", "withheld"}; static const std::set<std::string> kTruncationFields = {"omittedEvents", "omittedRepositories", "omittedEvidence"}; std::set<std::string> redactionFields, truncationFields; for (auto it=InDoc["redaction"].begin(); it!=InDoc["redaction"].end(); ++it) redactionFields.insert(it.key()); for (auto it=InDoc["truncation"].begin(); it!=InDoc["truncation"].end(); ++it) truncationFields.insert(it.key());
    if (redactionFields != kRedactionFields || truncationFields != kTruncationFields || !parseCount(InDoc["redaction"], "redacted", &out.redaction.redacted) || !parseCount(InDoc["redaction"], "withheld", &out.redaction.withheld) ||
        !parseCount(InDoc["truncation"], "omittedEvents", &out.truncation.omittedEvents) || !parseCount(InDoc["truncation"], "omittedRepositories", &out.truncation.omittedRepositories) || !parseCount(InDoc["truncation"], "omittedEvidence", &out.truncation.omittedEvidence)) return std::nullopt;
    if ((*state == OperationAuditCatalogState::Final) != (out.receiptSha256.has_value() && out.outcome.has_value() && out.finishedAtUtc.has_value())) return std::nullopt;
    if (*state == OperationAuditCatalogState::Final && !IsLowerHex(out.repositoryIdentityHeadSha256, 64)) return std::nullopt;
    if (*state != OperationAuditCatalogState::Final && !out.repositoryIdentityHeadSha256.empty()) return std::nullopt;
    if (*state == OperationAuditCatalogState::Pending && (out.receiptSha256 || out.outcome || out.finishedAtUtc)) return std::nullopt;
    if (*state == OperationAuditCatalogState::Incomplete &&
        (out.receiptSha256 || out.outcome || !out.finishedAtUtc)) return std::nullopt;
    return out;
}
auto LoadGeneration(const PinnedCatalogRoot& InPinnedRoot, const std::filesystem::path& InRoot, const std::size_t InLimit,
                    std::string* OutGeneration, std::vector<OperationAuditCatalogEntry>* OutEntries,
                    const bool InAllowBoundedRepair = false) -> bool {
    const auto loadPointer = [&](const std::string_view name, std::string* generationOut,
                                 std::vector<OperationAuditCatalogEntry>* entriesOut) -> bool {
    const auto pointer = ReadBoundedAt(InPinnedRoot, InRoot, std::string(name), InLimit);
    if (!pointer) return false;
    try {
        const auto doc = nlohmann::json::parse(*pointer);
        static const std::set<std::string> kPointerFields = {"schemaName", "schemaVersion", "generation", "sha256"};
        std::set<std::string> pointerFields; if (doc.is_object()) for (auto it = doc.begin(); it != doc.end(); ++it) pointerFields.insert(it.key());
        if (!doc.is_object() || !doc.contains("schemaVersion") || !IsSchemaVersionOne(doc["schemaVersion"]) || doc.value("schemaName", "") != kPointerSchema ||
            pointerFields != kPointerFields || !doc.contains("generation") || !doc["generation"].is_string() || !doc.contains("sha256") || !doc["sha256"].is_string()) return false;
        const auto generation = doc["generation"].get<std::string>();
        if (!audit::IsStableAuditId(generation)) return false;
        const auto bytes = ReadBoundedAt(InPinnedRoot, InRoot, generation + ".json", InLimit);
        if (!bytes || audit::Sha256Hex(*bytes) != doc.value("sha256", "") ||
            generation != "generation-" + audit::Sha256Hex(*bytes)) return false;
        const auto catalog = nlohmann::json::parse(*bytes);
        static const std::set<std::string> kCatalogFields = {"schemaName", "schemaVersion", "entries"};
        std::set<std::string> catalogFields; if (catalog.is_object()) for (auto it = catalog.begin(); it != catalog.end(); ++it) catalogFields.insert(it.key());
        if (!catalog.is_object() || !catalog.contains("schemaVersion") || !IsSchemaVersionOne(catalog["schemaVersion"]) || catalog.value("schemaName", "") != kCatalogSchema ||
            catalogFields != kCatalogFields || !catalog.contains("entries") || !catalog["entries"].is_array() || catalog["entries"].size() > kCatalogEntryCeiling) return false;
        std::vector<OperationAuditCatalogEntry> entries;
        std::set<std::pair<std::string, std::uint32_t>> identities;
        for (const auto& entry : catalog["entries"]) { auto parsed = ParseEntry(entry); if (!parsed || !identities.insert({parsed->runId, parsed->attempt}).second) return false; entries.push_back(std::move(*parsed)); }
        if (generationOut) *generationOut = generation; if (entriesOut) *entriesOut = std::move(entries); return true;
    } catch (...) { return false; }
    };
    if (loadPointer("current.json", OutGeneration, OutEntries)) return true;
    if (!InAllowBoundedRepair) return false;
    const auto hooks = Hooks(); if (hooks.repairVisitCounter) ++*hooks.repairVisitCounter;
    return loadPointer("previous.json", OutGeneration, OutEntries);
}
auto LoadNamedGeneration(const PinnedCatalogRoot& InPinnedRoot, const std::filesystem::path& InRoot, const std::string_view InGeneration,
                         const std::size_t InLimit, std::vector<OperationAuditCatalogEntry>* OutEntries) -> bool {
    if (!audit::IsStableAuditId(InGeneration)) return false;
    const auto bytes = ReadBoundedAt(InPinnedRoot, InRoot, std::string(InGeneration) + ".json", InLimit);
    if (!bytes || InGeneration != "generation-" + audit::Sha256Hex(*bytes)) return false;
    try {
        const auto catalog = nlohmann::json::parse(*bytes);
        static const std::set<std::string> kCatalogFields = {"schemaName", "schemaVersion", "entries"};
        std::set<std::string> catalogFields; if (catalog.is_object()) for (auto it = catalog.begin(); it != catalog.end(); ++it) catalogFields.insert(it.key());
        if (!catalog.is_object() || !catalog.contains("schemaVersion") || !IsSchemaVersionOne(catalog["schemaVersion"]) || catalog.value("schemaName", "") != kCatalogSchema || catalogFields != kCatalogFields || !catalog.contains("entries") || !catalog["entries"].is_array() || catalog["entries"].size() > kCatalogEntryCeiling) return false;
        std::vector<OperationAuditCatalogEntry> entries;
        std::set<std::pair<std::string, std::uint32_t>> identities;
        for (const auto& entry : catalog["entries"]) { auto parsed = ParseEntry(entry); if (!parsed || !identities.insert({parsed->runId, parsed->attempt}).second) return false; entries.push_back(std::move(*parsed)); }
        *OutEntries = std::move(entries); return true;
    } catch (...) { return false; }
}
auto SortEntries(std::vector<OperationAuditCatalogEntry>* InOut) -> void {
    std::sort(InOut->begin(), InOut->end(), [](const auto& a, const auto& b) {
        // RFC3339 UTC timestamps compare lexically. Identity is the stable
        // tie-breaker, so pages cannot duplicate or skip equal-time rows.
        return std::tie(a.observedAtUtc, a.runId, a.attempt) >
               std::tie(b.observedAtUtc, b.runId, b.attempt);
    });
}
auto FilterFingerprint(const OperationAuditCatalogFilter& InFilter) -> std::string {
    return audit::Sha256Hex((InFilter.runId ? *InFilter.runId : "") + "\n" +
                            (InFilter.planId ? *InFilter.planId : "") + "\n" +
                            (InFilter.state ? std::string(StateName(*InFilter.state)) : "") + "\n" +
                            (InFilter.outcome ? std::string(audit::OutcomeStateName(*InFilter.outcome)) : "") + "\n" +
                            (InFilter.repositoryId ? *InFilter.repositoryId : "") + "\n" +
                            (InFilter.correlationSha256 ? *InFilter.correlationSha256 : "") + "\n" +
                            (InFilter.observedNotBeforeUtc ? *InFilter.observedNotBeforeUtc : "") + "\n" +
                            (InFilter.observedBeforeUtc ? *InFilter.observedBeforeUtc : ""));
}
} // namespace

void SetAuditRunCatalogTestHooks(AuditRunCatalogTestHooks InHooks) {
    std::lock_guard lock(gCatalogHooksMutex); gCatalogHooks = std::move(InHooks);
}
void ResetAuditRunCatalogTestHooks() {
    std::lock_guard lock(gCatalogHooksMutex); gCatalogHooks = {};
}

auto PublishOperationAuditCatalogEntry(const OperationAuditSpec& InSpec, const OperationAuditPaths& InPaths,
                                       OperationAuditCatalogEntry InEntry, std::string* OutError) -> bool {
    const auto checkedEntry = ParseEntry(EntryJson(InEntry));
    if (!checkedEntry || *checkedEntry != InEntry) { if (OutError) *OutError = "catalog publication row violates closed schema"; return false; }
    const auto publishedIdentity = std::pair{InEntry.runId, InEntry.attempt};
    const auto root = CatalogRoot(InSpec, InPaths);
    const auto anchor = CatalogAnchor(InSpec, InPaths);
    auto pinnedRoot = anchor ? PinnedCatalogRoot::Open(root, *anchor, true) : std::nullopt;
    if (!pinnedRoot) { if (OutError) *OutError = "cannot safely admit audit catalog root"; return false; }
    ScopedCatalogWriterLock lock(*pinnedRoot, root);
    if (!lock.owned()) { if (OutError) *OutError = "audit catalog writer is busy"; return false; }
    if (const auto hooks = Hooks(); hooks.writerBarrier) hooks.writerBarrier();
    std::vector<OperationAuditCatalogEntry> entries; std::string priorGeneration;
    const bool loaded = LoadGeneration(*pinnedRoot, root, kCatalogStorageByteCeiling, &priorGeneration, &entries, true);
    // Absent starts an index; an existing unreadable/torn pointer is never
    // overwritten because repair is a separate writer-owned operation.
    if (!loaded && ChildExistsAt(*pinnedRoot, "current.json")) { if (OutError) *OutError = "audit catalog current generation is corrupt"; return false; }
    const auto same = std::find_if(entries.begin(), entries.end(), [&](const auto& entry) { return entry.runId == InEntry.runId && entry.attempt == InEntry.attempt; });
    if (same == entries.end()) {
        entries.push_back(std::move(InEntry));
    } else {
        // A durable final receipt is immutable.  A retry that attempts to
        // reuse its logical key with a different receipt is catalog corrupt,
        // never an overwrite.  Pending -> terminal is the only state advance.
        if (same->receiptSha256 && InEntry.receiptSha256 &&
            *same->receiptSha256 != *InEntry.receiptSha256) {
            if (OutError) *OutError = "audit catalog receipt collision";
            return false;
        }
        const auto rank = [](const OperationAuditCatalogState state) { return state == OperationAuditCatalogState::Pending ? 0 : state == OperationAuditCatalogState::Incomplete ? 1 : 2; };
        // Identity/binding fields are write-once.  The only mutable facts are
        // a monotonic lifecycle advance and terminal receipt-derived fields.
        if (same->parentRunId != InEntry.parentRunId || same->planId != InEntry.planId ||
            same->inputKind != InEntry.inputKind || same->route != InEntry.route ||
            same->planSha256 != InEntry.planSha256 || same->sourceSha256 != InEntry.sourceSha256 ||
            same->sourceSizeBytes != InEntry.sourceSizeBytes || same->auditRootSelector != InEntry.auditRootSelector || same->auditRootSha256 != InEntry.auditRootSha256 ||
            same->correlationSha256 != InEntry.correlationSha256 || rank(InEntry.state) < rank(same->state)) {
            if (OutError) *OutError = "audit catalog identity or lifecycle collision";
            return false;
        }
        InEntry.observedAtUtc = same->observedAtUtc;
        if (rank(InEntry.state) == rank(same->state) && *same != InEntry) {
            if (OutError) *OutError = "audit catalog duplicate row disagrees";
            return false;
        }
        if (same->state == OperationAuditCatalogState::Final && InEntry.state != OperationAuditCatalogState::Final) return true;
        *same = std::move(InEntry);
    }
    SortEntries(&entries);
    // Retention is applied to the already loaded generation only.  It never
    // walks, rebuilds, or guesses rows from the audit root.
    while (entries.size() > kCatalogEntryCeiling) {
        const auto victim = std::find_if(entries.rbegin(), entries.rend(), [&](const auto& entry) {
            return std::pair{entry.runId, entry.attempt} != publishedIdentity;
        });
        if (victim == entries.rend()) { if (OutError) *OutError = "audit catalog row exceeds bounded discovery window"; return false; }
        entries.erase(std::next(victim).base());
    }
    nlohmann::json doc;
    std::string bytes;
    // The catalog is a recent discovery window, not evidence retention. Evict
    // only the deterministic oldest non-current row until the fixed storage
    // ceiling fits; the just-published lifecycle row is never dropped.
    for (;;) {
        doc = {{"schemaName", kCatalogSchema}, {"schemaVersion", 1}, {"entries", nlohmann::json::array()}};
        for (const auto& entry : entries) doc["entries"].push_back(EntryJson(entry));
        bytes = doc.dump() + '\n';
        if (bytes.size() <= kCatalogStorageByteCeiling) break;
        const auto victim = std::find_if(entries.rbegin(), entries.rend(), [&](const auto& entry) {
            return std::pair{entry.runId, entry.attempt} != publishedIdentity;
        });
        if (victim == entries.rend()) { if (OutError) *OutError = "audit catalog row exceeds bounded discovery window"; return false; }
        entries.erase(std::next(victim).base());
    }
    for (const auto& entry : entries) {
        const auto checked = ParseEntry(EntryJson(entry));
        if (!checked || *checked != entry) { if (OutError) *OutError = "catalog generation violates closed schema"; return false; }
    }
    const auto generation = "generation-" + audit::Sha256Hex(bytes);
    if (Stage("before-generation")) { if (OutError) *OutError = "injected catalog generation failure"; return false; }
    const auto existingGeneration = ReadBoundedAt(*pinnedRoot, root, generation + ".json", kCatalogStorageByteCeiling);
    if (existingGeneration) {
        if (*existingGeneration != bytes) { if (OutError) *OutError = "catalog generation name collision"; return false; }
    } else {
        const auto generationTempName = "generation-install-" + audit::Sha256Hex(
            generation + std::to_string(MonotonicNow().time_since_epoch().count()) +
            std::to_string(gCatalogTemporarySequence.fetch_add(1))).substr(0,20) + ".tmp";
        if (!WriteDurableNewAt(*pinnedRoot, root, generationTempName, bytes)) {
            if (OutError) *OutError = "cannot durably publish immutable audit catalog generation";
            return false;
        }
        const auto install=InstallNoReplaceAt(*pinnedRoot, root, generationTempName, generation + ".json");
        if (install==InstallResult::AlreadyExists) {
            const auto collided = ReadBoundedAt(*pinnedRoot, root, generation + ".json", kCatalogStorageByteCeiling);
            if (!collided || *collided != bytes) { if (OutError) *OutError = "cannot durably publish immutable audit catalog generation"; return false; }
        } else if(install==InstallResult::Error) {
            if(OutError)*OutError="cannot durably publish immutable audit catalog generation"; return false;
        }
    }
    const auto temporary = root / ("current-" + audit::Sha256Hex(
        generation + std::to_string(MonotonicNow().time_since_epoch().count()) +
        std::to_string(gCatalogTemporarySequence.fetch_add(1))).substr(0, 20) + ".tmp");
    if (Stage("before-pointer")) { if (OutError) *OutError = "injected catalog pointer failure"; return false; }
    const auto pointerBytes = nlohmann::json({{"schemaName", kPointerSchema}, {"schemaVersion", 1}, {"generation", generation}, {"sha256", audit::Sha256Hex(bytes)}}).dump() + '\n';
    // Preserve exactly one previously verified pointer. Repair therefore
    // visits at most two known pointer files and never enumerates any root.
    if (!priorGeneration.empty()) {
        const auto priorBytes = ReadBoundedAt(*pinnedRoot, root, priorGeneration + ".json", kCatalogStorageByteCeiling);
        if (priorBytes) {
            const auto previousBytes = nlohmann::json({{"schemaName", kPointerSchema}, {"schemaVersion", 1}, {"generation", priorGeneration}, {"sha256", audit::Sha256Hex(*priorBytes)}}).dump() + '\n';
            const auto previousTemp = root / ("previous-" + audit::Sha256Hex(previousBytes + std::to_string(gCatalogTemporarySequence.fetch_add(1))).substr(0, 20) + ".tmp");
            if (!WriteDurableNewAt(*pinnedRoot, root, previousTemp.filename().string(), previousBytes)) { if (OutError) *OutError = "cannot write bounded catalog repair pointer"; return false; }
            if (!RenameReplaceAt(*pinnedRoot, root, previousTemp.filename().string(), "previous.json")) { if (OutError) *OutError = "cannot publish bounded catalog repair pointer"; return false; }
        }
    }
    if (!WriteDurableNewAt(*pinnedRoot, root, temporary.filename().string(), pointerBytes)) { if (OutError) *OutError = "cannot durably write audit catalog pointer"; return false; }
    // POSIX rename replaces current atomically.  On Windows MoveFileEx's
    // replace flag provides the corresponding single pointer swap; never
    // remove current first because readers must retain one valid generation.
    if (!RenameReplaceAt(*pinnedRoot, root, temporary.filename().string(), "current.json")) { if (OutError) *OutError = "cannot atomically and durably publish audit catalog pointer"; return false; }
    if (Stage("after-pointer")) { if (OutError) *OutError = "injected post-pointer publication failure"; return false; }
    return true;
}

auto QueryOperationAuditCatalog(const OperationAuditSpec& InSpec, const OperationAuditCatalogFilter& InFilter,
                                const std::optional<std::string_view> InCursor, const OperationAuditCatalogQueryLimits InLimits) -> OperationAuditCatalogQueryResult {
    const auto started = MonotonicNow();
    const auto timedOut = [&] { return MonotonicNow() - started > InLimits.maxQueryTime; };
    OperationAuditCatalogQueryResult out; if (InLimits.maxRows == 0 || InLimits.maxBytes == 0 || InLimits.maxQueryTime.count() <= 0 || InLimits.maxDiagnosticBytes == 0 || InLimits.maxRows > kCatalogEntryCeiling || InLimits.maxBytes > (4U << 20U) || (!InCursor && (InLimits.cursorLifetime.count() <= 0 || InLimits.cursorLifetime > kCatalogCursorLifetimeMaximum))) { out.code = OperationAuditCatalogQueryCode::InvalidConfiguration; return out; }
    const auto validIdFilter = [](const std::optional<std::string>& value) { return !value || audit::IsStableAuditId(*value); };
    if (!validIdFilter(InFilter.runId) || !validIdFilter(InFilter.planId) || !validIdFilter(InFilter.repositoryId) ||
        (InFilter.correlationSha256 && !IsLowerHex(*InFilter.correlationSha256, 64)) ||
        (InFilter.observedNotBeforeUtc && !IsStrictUtcTimestamp(*InFilter.observedNotBeforeUtc)) ||
        (InFilter.observedBeforeUtc && !IsStrictUtcTimestamp(*InFilter.observedBeforeUtc))) { out.code = OperationAuditCatalogQueryCode::InvalidConfiguration; return out; }
    std::string error; const auto paths = ResolveOperationAuditPaths(InSpec, "catalog-query", 1, &error);
    if (!paths) { out.code = OperationAuditCatalogQueryCode::InvalidConfiguration; out.diagnostic = error.substr(0, InLimits.maxDiagnosticBytes); return out; }
    const auto root = CatalogRoot(InSpec, *paths);
    const auto anchor = CatalogAnchor(InSpec, *paths);
    auto pinnedRoot = anchor ? PinnedCatalogRoot::Open(root, *anchor, false) : std::nullopt;
    if (!pinnedRoot) {
        std::error_code rootEc;
        out.code = std::filesystem::exists(root, rootEc) && !rootEc
            ? OperationAuditCatalogQueryCode::Corrupt : OperationAuditCatalogQueryCode::Missing;
        return out;
    }
    std::vector<OperationAuditCatalogEntry> entries; std::size_t start = 0; const auto fingerprint = FilterFingerprint(InFilter); std::int64_t cursorIssued = -1; std::int64_t cursorExpires = -1;
    if (InCursor) try {
        if (InCursor->size() > 2048) { out.code=OperationAuditCatalogQueryCode::InvalidCursor; return out; }
        const auto cursor=nlohmann::json::parse(*InCursor);
        static const std::set<std::string> fields={"schemaName","schemaVersion","generation","generationSha256","filter","issuedAtEpoch","expiresAtEpoch","offset","integrity"};
        std::set<std::string> actual; if(cursor.is_object())for(auto it=cursor.begin();it!=cursor.end();++it)actual.insert(it.key());
        if(actual!=fields || !cursor.contains("schemaVersion") || !IsCatalogCursorSchemaVersion(cursor["schemaVersion"]) ||
           !cursor["schemaName"].is_string() || cursor["schemaName"].get<std::string>()!="kog.auditRunCatalogCursor" ||
           !cursor["generation"].is_string() || !audit::IsStableAuditId(cursor["generation"].get<std::string>()) ||
           !cursor["generationSha256"].is_string() || !IsLowerHex(cursor["generationSha256"].get<std::string>(),64) ||
           !cursor["filter"].is_string() || cursor["filter"].get<std::string>()!=fingerprint || !cursor["integrity"].is_string()) {
            out.code=OperationAuditCatalogQueryCode::InvalidCursor; return out;
        }
        const auto issuedValue=NonnegativeInt64(cursor["issuedAtEpoch"]); const auto expiresValue=NonnegativeInt64(cursor["expiresAtEpoch"]); const auto offsetValue=BoundedSize(cursor["offset"]);
        if(!issuedValue||!expiresValue||!offsetValue){out.code=OperationAuditCatalogQueryCode::InvalidCursor;return out;}
        const auto issued=*issuedValue; const auto expires=*expiresValue; const auto offset=*offsetValue;
        if (expires <= issued || expires - issued > kCatalogCursorLifetimeMaximum.count()) { out.code=OperationAuditCatalogQueryCode::InvalidCursor; return out; }
        const auto integrity=CursorIntegrity(cursor["generation"].get<std::string>(), cursor["generationSha256"].get<std::string>(), cursor["filter"].get<std::string>(), issued, expires, offset);
        if(cursor["integrity"].get<std::string>()!=integrity){out.code=OperationAuditCatalogQueryCode::InvalidCursor;return out;}
        const auto now=std::chrono::duration_cast<std::chrono::seconds>(SystemNow().time_since_epoch()).count();
        if(issued>now||now>expires){out.code=OperationAuditCatalogQueryCode::ExpiredCursor;return out;}
        out.generation=cursor["generation"].get<std::string>(); start=offset; cursorIssued=issued; cursorExpires=expires;
        const auto bytes=ReadBoundedAt(*pinnedRoot,root,out.generation+".json",kCatalogStorageByteCeiling);
        if(!bytes||audit::Sha256Hex(*bytes)!=cursor["generationSha256"].get<std::string>()||!LoadNamedGeneration(*pinnedRoot,root,out.generation,kCatalogStorageByteCeiling,&entries)){out.code=OperationAuditCatalogQueryCode::Corrupt;return out;}
    } catch (...) { out.code=OperationAuditCatalogQueryCode::InvalidCursor; return out; }
    else if (!LoadGeneration(*pinnedRoot, root, kCatalogStorageByteCeiling, &out.generation, &entries)) { out.code = ChildExistsAt(*pinnedRoot, "current.json") ? OperationAuditCatalogQueryCode::Corrupt : OperationAuditCatalogQueryCode::Missing; return out; }
    if (timedOut()) { out.code = OperationAuditCatalogQueryCode::Limit; return out; }
    std::vector<OperationAuditCatalogEntry> filtered;
    for (const auto& entry : entries) {
        if (timedOut()) { out.code = OperationAuditCatalogQueryCode::Limit; return out; }
        const bool matchesOtherPredicates =
            (!InFilter.runId || entry.runId == *InFilter.runId) &&
            (!InFilter.planId || entry.planId == *InFilter.planId) &&
            (!InFilter.state || entry.state == *InFilter.state) &&
            (!InFilter.outcome || entry.outcome == InFilter.outcome) &&
            (!InFilter.correlationSha256 || entry.correlationSha256 == *InFilter.correlationSha256) &&
            (!InFilter.observedNotBeforeUtc || entry.observedAtUtc >= *InFilter.observedNotBeforeUtc) &&
            (!InFilter.observedBeforeUtc || entry.observedAtUtc < *InFilter.observedBeforeUtc);
        if (!matchesOtherPredicates) continue;
        const bool repositoryMatches = !InFilter.repositoryId || std::any_of(
            entry.repositories.begin(), entry.repositories.end(), [&](const auto& repository) {
                return repository.repositoryId == *InFilter.repositoryId;
            });
        if (InFilter.repositoryId && !repositoryMatches && entry.truncation.omittedRepositories != 0) {
            out.code = OperationAuditCatalogQueryCode::Unsupported;
            return out;
        }
        if (repositoryMatches) filtered.push_back(entry);
    }
    if (start > filtered.size()) { out.code = OperationAuditCatalogQueryCode::InvalidCursor; return out; }
    const auto end = std::min(filtered.size(), start + InLimits.maxRows); std::size_t bytes = 0; for (auto index = start; index < end; ++index) { bytes += EntryJson(filtered[index]).dump().size(); if (bytes > InLimits.maxBytes || timedOut()) { out.code = OperationAuditCatalogQueryCode::Limit; return out; } }
    std::int64_t issued = -1; std::int64_t expires = -1;
    if (end < filtered.size()) {
        issued = cursorIssued >= 0 ? cursorIssued : std::chrono::duration_cast<std::chrono::seconds>(SystemNow().time_since_epoch()).count();
        if (cursorIssued < 0 && issued > std::numeric_limits<std::int64_t>::max() - InLimits.cursorLifetime.count()) { out.code = OperationAuditCatalogQueryCode::Limit; return out; }
        expires = cursorExpires >= 0 ? cursorExpires : issued + InLimits.cursorLifetime.count();
    }
    out.rows.assign(filtered.begin() + static_cast<std::ptrdiff_t>(start), filtered.begin() + static_cast<std::ptrdiff_t>(end));
    if (end < filtered.size()) { const auto generationBytes = ReadBoundedAt(*pinnedRoot, root, out.generation + ".json", kCatalogStorageByteCeiling); if (!generationBytes) { out.rows.clear(); out.code = OperationAuditCatalogQueryCode::Corrupt; return out; } const auto generationHash=audit::Sha256Hex(*generationBytes); const auto integrity=CursorIntegrity(out.generation, generationHash, fingerprint, issued, expires, end); out.cursor = nlohmann::json({{"schemaName", "kog.auditRunCatalogCursor"}, {"schemaVersion", kCatalogCursorSchemaVersion}, {"generation", out.generation}, {"generationSha256", generationHash}, {"filter", fingerprint}, {"issuedAtEpoch", issued}, {"expiresAtEpoch", expires}, {"offset", end}, {"integrity", integrity}}).dump(); }
    // maxBytes bounds the serialized public result envelope, not just row
    // projections.  Storage always has its own fixed ceiling above.
    nlohmann::json envelope = {{"generation", out.generation}, {"cursor", out.cursor ? nlohmann::json(*out.cursor) : nlohmann::json(nullptr)}, {"diagnostic", out.diagnostic}, {"rows", nlohmann::json::array()}};
    for (const auto& row : out.rows) envelope["rows"].push_back(EntryJson(row));
    const auto envelopeBytes = envelope.dump();
    if (timedOut()) { out.rows.clear(); out.cursor.reset(); out.code = OperationAuditCatalogQueryCode::Limit; return out; }
    if (envelopeBytes.size() > InLimits.maxBytes) { out.rows.clear(); out.cursor.reset(); out.code = OperationAuditCatalogQueryCode::Limit; return out; }
    out.code = OperationAuditCatalogQueryCode::None; return out;
}

auto RevalidateOperationAuditCatalogEntry(const OperationAuditSpec& InCatalogAnchorSpec,
                                          const OperationAuditSpec& InSelectedSpec,
                                          const OperationAuditCatalogEntry& InEntry,
                                          OperationAuditRunReadLimits InLimits) -> OperationAuditRunReadResult {
    const auto bindingFailure = [](const std::string_view diagnostic) {
        return OperationAuditRunReadResult{.state = OperationAuditRunReadState::Invalid,
                                           .code = OperationAuditRunReadCode::BindingMismatch,
                                           .diagnostic = std::string(diagnostic)};
    };
    // Only a terminal row with its immutable receipt digest can be upgraded
    // to verified evidence. Pending/incomplete projections remain discovery.
    if (InEntry.state != OperationAuditCatalogState::Final || !InEntry.receiptSha256 ||
        !InEntry.outcome || !InEntry.finishedAtUtc)
        return bindingFailure("catalog row is not a final receipted attempt");
    const auto validatedEntry = ParseEntry(EntryJson(InEntry));
    if (!validatedEntry || *validatedEntry != InEntry)
        return bindingFailure("catalog row violates the closed catalog schema");
    // The anchor is only a trusted workspace/catalog scope.  It is never a
    // substitute for the selected attempt's real source and frozen bytes.
    if (InCatalogAnchorSpec.workspaceRoot.empty() || InSelectedSpec.workspaceRoot.empty() ||
        InCatalogAnchorSpec.workspaceRoot.lexically_normal() != InSelectedSpec.workspaceRoot.lexically_normal())
        return bindingFailure("catalog anchor and selected audit workspace differ");
    std::string anchorError; const auto anchorPaths = ResolveOperationAuditPaths(InCatalogAnchorSpec, "catalog-query", 1, &anchorError);
    std::string selectedError; const auto selectedPaths = ResolveOperationAuditPaths(InSelectedSpec, InEntry.runId, InEntry.attempt, &selectedError);
    if (!anchorPaths || !selectedPaths || CatalogRoot(InCatalogAnchorSpec, *anchorPaths) != CatalogRoot(InSelectedSpec, *selectedPaths))
        return bindingFailure("catalog selector is outside the trusted anchor");
    const auto selectedCorrelation = audit::Sha256Hex(SerializeOperationCorrelationEnvelope(InSelectedSpec.correlation));
    if (InSelectedSpec.correlation.runId != InEntry.runId || InSelectedSpec.correlation.attempt != InEntry.attempt ||
        InSelectedSpec.inputKind != InEntry.inputKind || InSelectedSpec.route != InEntry.route ||
        InSelectedSpec.planId != InEntry.planId || audit::Sha256Hex(InSelectedSpec.sourceBytes) != InEntry.sourceSha256 ||
        InSelectedSpec.sourceBytes.size() != InEntry.sourceSizeBytes || audit::Sha256Hex(InSelectedSpec.frozenBytes) != InEntry.planSha256 ||
        selectedCorrelation != InEntry.correlationSha256 ||
        ((InSelectedSpec.inputKind == "commit-plan" ? "plan-" : "operation-") + audit::Sha256Hex(InSelectedSpec.inputIdentity)) != InEntry.auditRootSelector ||
        audit::Sha256Hex(selectedPaths->auditRoot.generic_string()) != InEntry.auditRootSha256)
        return bindingFailure("selected audit spec does not match catalog row binding");
    auto result = ReadOperationAuditRun(InSelectedSpec, InEntry.runId, InEntry.attempt, InLimits,
                                        InEntry.receiptSha256 ? std::optional<std::string_view>(*InEntry.receiptSha256) : std::nullopt);
    auto catalogRepositoryPreviewDigest = [&](const std::vector<OperationAuditCatalogRepositoryCommit>& repositories) {
        auto ordered = repositories;
        std::sort(ordered.begin(), ordered.end(), [](const auto& left, const auto& right) { return left.repositoryId < right.repositoryId; });
        std::string summary;
        for (const auto& repository : ordered)
            summary += repository.repositoryId + "\n" + repository.afterHeadSha.value_or("") + "\n";
        return audit::Sha256Hex(summary);
    };
    const auto projectionMatches = [&](const OperationAuditRunProjection& run) {
        if (run.planSha256 != InEntry.planSha256 || run.runId != InEntry.runId || run.attempt != InEntry.attempt ||
            run.parentRunId != InEntry.parentRunId || run.planId != InEntry.planId ||
            run.terminalOutcome.status != *InEntry.outcome ||
            run.finishedAtUtc != *InEntry.finishedAtUtc ||
            run.repositoryIdentityHeadSha256 != InEntry.repositoryIdentityHeadSha256 ||
            run.catalogRepositoryPreviewIdentityHeadSha256 != catalogRepositoryPreviewDigest(InEntry.repositories) ||
            run.totalRepositories != InEntry.repositories.size() + InEntry.truncation.omittedRepositories ||
            run.totalEventRecords != InEntry.truncation.omittedEvents ||
            run.totalEvidenceReferences != InEntry.truncation.omittedEvidence ||
            run.redactedEvidenceCount != InEntry.redaction.redacted ||
            run.withheldEvidenceCount != InEntry.redaction.withheld) return false;
        return true;
    };
    if (result.run && !projectionMatches(*result.run)) {
        result.state = OperationAuditRunReadState::Corrupt;
        result.code = OperationAuditRunReadCode::BindingMismatch;
        result.run.reset(); result.diagnostic = "catalog row does not bind pinned audit evidence";
    }
    return result;
}

} // namespace kano::git::commands
