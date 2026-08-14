#pragma once

#include "audit_run_catalog.hpp"

#include <functional>

namespace kano::git::commands {

// Writer-only publication.  It never enumerates the audit root; a generation
// is reconstructed solely from the previous immutable generation plus the
// supplied current attempt row.
auto PublishOperationAuditCatalogEntry(const OperationAuditSpec& InSpec,
                                       const OperationAuditPaths& InPaths,
                                       OperationAuditCatalogEntry InEntry,
                                       std::string* OutError) -> bool;

// Private deterministic fault/concurrency seam; production callers never set
// these hooks.  Repair is limited to current.json/previous.json only.
struct AuditRunCatalogTestHooks {
    std::function<std::chrono::system_clock::time_point()> systemNow;
    std::function<std::chrono::steady_clock::time_point()> monotonicNow;
    std::function<void(std::string_view)> publicationStage;
    std::function<void(std::string_view)> publicationFailure;
    std::function<bool(std::string_view)> failStage;
    std::function<void()> writerBarrier;
    std::size_t* repairVisitCounter = nullptr;
};
void SetAuditRunCatalogTestHooks(AuditRunCatalogTestHooks InHooks);
void ResetAuditRunCatalogTestHooks();
void ReportAuditRunCatalogPublicationFailureForTest(std::string_view InDiagnostic);

} // namespace kano::git::commands
