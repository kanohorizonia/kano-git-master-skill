#pragma once

#include <filesystem>
#include <memory>
#include <optional>
#include <string>

namespace kano::git::commands {

class PlanAuditSink;

class PlanExecutionAdmission {
public:
    PlanExecutionAdmission(PlanExecutionAdmission&&) noexcept = default;
    auto operator=(PlanExecutionAdmission&&) noexcept
        -> PlanExecutionAdmission& = default;
    PlanExecutionAdmission(const PlanExecutionAdmission&) = delete;
    auto operator=(const PlanExecutionAdmission&)
        -> PlanExecutionAdmission& = delete;
    ~PlanExecutionAdmission() = default;

    [[nodiscard]] auto AdmittedSourceBytes() const -> const std::string&;

private:
    friend auto AcquireAuditedPlanExecutionAdmission(
        PlanAuditSink&,
        const std::filesystem::path&,
        const std::filesystem::path&,
        std::string*) -> std::optional<PlanExecutionAdmission>;

    PlanExecutionAdmission(std::shared_ptr<void> InLockState,
                           std::string InAdmittedSourceBytes);

    std::shared_ptr<void> mLockState;
    std::string mAdmittedSourceBytes;
};

// Acquires the persistent cooperating-writer lock, then re-reads the source
// through the bounded no-follow reader.  The returned token must remain alive
// through all mutations, source binding, and audit finalization.
auto AcquireAuditedPlanExecutionAdmission(
    PlanAuditSink& InAudit,
    const std::filesystem::path& InWorkspaceRoot,
    const std::filesystem::path& InPlanPath,
    std::string* OutError) -> std::optional<PlanExecutionAdmission>;

} // namespace kano::git::commands
