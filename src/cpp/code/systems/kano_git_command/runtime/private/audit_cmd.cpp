// Read-only audit protocol commands.  Their JSON is intentionally a closed,
// path-free machine interface: evidence locations remain private to KOG.

#include <CLI/CLI.hpp>

#include "operation_audit.hpp"
#include "audit_verification.hpp"
#include "audit_verification_internal.hpp"
#include "shell_executor.hpp"

#include <filesystem>
#include <iostream>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>

namespace kano::git::commands {
namespace {

auto VerificationFailureJson() -> std::string {
    const nlohmann::json doc = {
        {"schemaName", kOperationAuditVerificationSchema}, {"schemaVersion", 1},
        {"ok", false}, {"error", {{"code", "verification-failed"},
                                      {"message", "verification failed"}}},
        {"traceValid", false}, {"eventsValid", false}, {"receiptValid", false},
        {"frozenInputValid", false}, {"planId", nullptr}, {"planSha256", nullptr},
        {"frozenInputSha256", nullptr}, {"runId", nullptr}, {"parentRunId", nullptr},
        {"attempt", nullptr}, {"correlation", nullptr}, {"eventCount", nullptr},
        {"eventStreamSha256", nullptr}, {"receiptSha256", nullptr},
        {"terminalOutcome", nullptr}, {"repositories", nlohmann::json::array()},
        {"artifacts", nlohmann::json::array()},
    };
    return doc.dump() + '\n';
}

struct AuditVerificationResult {
    int exitCode = 0;
    std::string json;
};

auto EvaluateAuditVerification(const std::filesystem::path& InPlanFile,
                               const std::string& InRunId,
                               const std::uint32_t InAttempt)
    -> AuditVerificationResult {
    // Machine-readable commands use the shell executor's existing per-thread
    // routing stack. Nonempty no-op callbacks consume command diagnostics
    // without redirecting global stdout/stderr or affecting concurrent work.
    shell::ScopedCommandLogCapture suppressCommandLogs({
        [](const std::string&) {}, [](const std::string&) {}});
    std::string error;
    const auto spec = MakeOperationAuditVerificationSpec(
        {
            .workspaceRoot = std::filesystem::current_path(),
            .planFile = InPlanFile,
            .runId = InRunId,
            .attempt = InAttempt,
        },
        &error);
    if (!spec || !audit::IsStableAuditId(InRunId) || InAttempt == 0)
        return {2, VerificationFailureJson()};

    bool traceValid = false;
    const auto result = VerifyOperationAuditJson(
        *spec, InRunId, InAttempt, &traceValid, &error);
    if (!result || !traceValid) return {1, VerificationFailureJson()};
    return {0, *result};
}

} // namespace

void RegisterAudit(CLI::App& InApp) {
    auto* command = InApp.add_subcommand("audit", "Read-only operation-audit protocol");
    command->require_subcommand(1);

    auto* capability = command->add_subcommand("capability", "Print the closed audit protocol capability");
    auto* capabilityJson = new bool{false};
    capability->add_flag("--json", *capabilityJson, "Emit closed machine-readable JSON")->required();
    capability->callback([=]() { std::cout << OperationAuditCapabilityJson(); });

    auto* verify = command->add_subcommand("verify", "Verify immutable audit evidence for a plan run");
    auto* planFile = new std::string{};
    auto* runId = new std::string{};
    auto* attempt = new std::uint32_t{1};
    auto* verifyJson = new bool{false};
    verify->add_option("--plan-file", *planFile, "Original commit-plan file identity")->required();
    verify->add_option("--run-id", *runId, "Resolved run identifier")->required();
    verify->add_option("--attempt", *attempt, "Positive execution attempt")->required();
    verify->add_flag("--json", *verifyJson, "Emit closed machine-readable JSON")->required();
    verify->callback([=]() {
        const auto result = EvaluateAuditVerification(
            std::filesystem::path(*planFile), *runId, *attempt);
        std::cout << result.json;
        if (result.exitCode != 0) std::exit(result.exitCode);
    });
}

} // namespace kano::git::commands
