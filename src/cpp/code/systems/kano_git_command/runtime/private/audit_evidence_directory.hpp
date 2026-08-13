#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace kano::git::commands {

struct OperationAuditPaths;

enum class AuditEvidenceOpenCode {
    None,
    Missing,
    InvalidComponent,
    LoopOrReparse,
    PermissionDenied,
    IoError,
    StatFailed,
    NotDirectory,
};

enum class AuditEvidenceReadCode {
    None,
    Missing,
    InvalidComponent,
    LoopOrReparse,
    PermissionDenied,
    IoError,
    StatFailed,
    NonRegular,
    Limit,
    Unstable,
};

struct AuditEvidenceInput {
    AuditEvidenceReadCode code = AuditEvidenceReadCode::IoError;
    std::string bytes;
    // Exact bounded-input diagnostic retained for the legacy verify wrapper.
    std::string diagnostic;
    [[nodiscard]] auto ready() const noexcept -> bool {
        return code == AuditEvidenceReadCode::None;
    }
};

struct AuditEvidenceProbe {
    AuditEvidenceReadCode code = AuditEvidenceReadCode::IoError;
    [[nodiscard]] auto ready() const noexcept -> bool {
        return code == AuditEvidenceReadCode::None;
    }
};

// Private, thread-local synchronization injection used only by deterministic
// reader tests. It is deliberately absent from the installed public ABI.
struct AuditEvidencePinnedTestHook {
    void (*onAttemptPinned)(void* InContext) = nullptr;
    void* context = nullptr;
};

class ScopedAuditEvidencePinnedTestHook {
public:
    explicit ScopedAuditEvidencePinnedTestHook(AuditEvidencePinnedTestHook InHook);
    ~ScopedAuditEvidencePinnedTestHook();
    ScopedAuditEvidencePinnedTestHook(const ScopedAuditEvidencePinnedTestHook&) = delete;
    auto operator=(const ScopedAuditEvidencePinnedTestHook&)
        -> ScopedAuditEvidencePinnedTestHook& = delete;

private:
    AuditEvidencePinnedTestHook mPrevious;
};

// Pins the trusted audit root's run/attempt descendants by native directory
// handle. Every child read is relative to the same attempt handle.
class AuditEvidenceDirectory {
public:
    AuditEvidenceDirectory() = default;
    ~AuditEvidenceDirectory();
    AuditEvidenceDirectory(AuditEvidenceDirectory&&) noexcept;
    auto operator=(AuditEvidenceDirectory&&) noexcept -> AuditEvidenceDirectory&;
    AuditEvidenceDirectory(const AuditEvidenceDirectory&) = delete;
    auto operator=(const AuditEvidenceDirectory&) -> AuditEvidenceDirectory& = delete;

    [[nodiscard]] static auto Open(const OperationAuditPaths& InPaths,
                                   AuditEvidenceOpenCode* OutCode)
        -> std::optional<AuditEvidenceDirectory>;
    [[nodiscard]] auto Probe(std::string_view InChildName) const
        -> AuditEvidenceProbe;
    [[nodiscard]] auto Read(std::string_view InChildName,
                            std::uintmax_t InLimit) const -> AuditEvidenceInput;

private:
    explicit AuditEvidenceDirectory(std::intptr_t InHandle) : mHandle(InHandle) {}
    std::intptr_t mHandle = -1;
};

[[nodiscard]] auto AuditEvidenceSystemDiagnostic(AuditEvidenceOpenCode InCode)
    -> std::string_view;
[[nodiscard]] auto AuditEvidenceSystemDiagnostic(AuditEvidenceReadCode InCode)
    -> std::string_view;

} // namespace kano::git::commands
