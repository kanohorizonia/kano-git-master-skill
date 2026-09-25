#pragma once

#include "audit_handoff.hpp"

namespace kano::git::commands {

// Internal serializer seam. Production callers must go through
// BuildAuditHandoffJson(request), which obtains this result from exactly one
// existing pinned verification read. Kept private to the runtime module so a
// naked projection cannot be exported through the public ABI.
[[nodiscard]] auto SerializeVerifiedAuditHandoff(
    const OperationAuditRunReadResult& InRead,
    const AuditHandoffLimits& InLimits) -> AuditHandoffResult;

} // namespace kano::git::commands
