#pragma once

#include "audit_verification.hpp"
#include "operation_audit.hpp"

#include <optional>
#include <string>

namespace kano::git::commands {

[[nodiscard]] auto MakeOperationAuditVerificationSpec(
    const OperationAuditVerificationRequest& InRequest,
    std::string* OutError) -> std::optional<OperationAuditSpec>;

} // namespace kano::git::commands
