#pragma once

#include "commit_plan_payload.hpp"
#include "operation_audit.hpp"

#include <filesystem>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace kano::git::commands {

class PlanAuditSink {
public:
    static auto Reserve(const std::filesystem::path& InWorkspaceRoot,
                        const std::filesystem::path& InPlanPath,
                        std::string* OutError,
                        std::string InRoute = "commit-push.plan")
        -> std::unique_ptr<PlanAuditSink>;
    ~PlanAuditSink();

    PlanAuditSink(const PlanAuditSink&) = delete;
    auto operator=(const PlanAuditSink&) -> PlanAuditSink& = delete;

    auto Capture(const std::filesystem::path& InRepo) const -> audit::RepositoryState;
    auto Append(std::string InAction,
                const std::filesystem::path& InRepo,
                const audit::RepositoryState& InBefore,
                std::string InStartedAtUtc,
                int InExitCode,
                std::string* OutError) -> bool;
    auto Finalize(int InExitCode, std::string* OutError) -> bool;
    auto BindSourceStateBytes(std::string_view InSourceBytes,
                              std::string* OutError) -> bool;
    auto RevalidateAdmittedSource(const std::filesystem::path& InPlanPath,
                                  std::string* OutError,
                                  std::string* OutSourceBytes = nullptr) const
        -> bool;
    [[nodiscard]] auto FrozenPlanPath() const -> const std::filesystem::path&;
    [[nodiscard]] auto PlanSha256() const -> const std::string&;
    [[nodiscard]] auto RunId() const -> const std::string&;
    [[nodiscard]] auto PlanId() const -> const std::string&;

private:
    PlanAuditSink() = default;
    std::filesystem::path mFrozenPlanPath;
    std::string mSourceBytes;
    std::string mSourceSha256;
    std::string mPlanId;
    OperationAuditContext* mContext = nullptr;
    std::unique_ptr<OperationAuditContext> mOwnedContext;
};

} // namespace kano::git::commands
