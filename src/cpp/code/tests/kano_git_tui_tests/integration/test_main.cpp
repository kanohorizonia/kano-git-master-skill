// Integration tests main entry point for TUI Command Input Enhancement
// This file will be populated with integration test cases as components are implemented

#include <catch2/catch_test_macros.hpp>

#include "functional_test_support.hpp"
#include "audit_verification.hpp"
#include "discovery.hpp"
#include "operation_audit.hpp"
#include "tui_audit_frame.hpp"
#include "tui_async_lifecycle.hpp"
#include "tui_command_scope.hpp"
#include "tui_startup_snapshot.hpp"

#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <ctime>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <nlohmann/json.hpp>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace kano::git::commands;

namespace {

class ScopedStreamBuffer final {
  public:
    ScopedStreamBuffer(std::ostream& InStream, std::streambuf* InBuffer)
        : stream_(InStream), previous_(InStream.rdbuf(InBuffer)) {}

    ~ScopedStreamBuffer() {
        stream_.rdbuf(previous_);
    }

    ScopedStreamBuffer(const ScopedStreamBuffer&) = delete;
    auto operator=(const ScopedStreamBuffer&)
        -> ScopedStreamBuffer& = delete;

  private:
    std::ostream& stream_;
    std::streambuf* previous_;
};

class ScopedSandbox final {
  public:
    explicit ScopedSandbox(std::string InName)
        : context_(
              kano::git::tests::functional::CreateSandboxWorkspace(
                  std::move(InName))) {}

    ~ScopedSandbox() {
        kano::git::tests::functional::RemoveSandboxWorkspace(context_);
    }

    [[nodiscard]] auto Root() const -> const std::filesystem::path& {
        return context_.root;
    }

    ScopedSandbox(const ScopedSandbox&) = delete;
    auto operator=(const ScopedSandbox&) -> ScopedSandbox& = delete;

  private:
    kano::git::tests::functional::SandboxContext context_;
};

class ScopedEnvironmentVariable final {
  public:
    ScopedEnvironmentVariable(std::string InName, std::string InValue)
        : name_(std::move(InName)) {
        if (const char* value = std::getenv(name_.c_str()); value != nullptr) {
            hadPrevious_ = true;
            previous_ = value;
        }
        Set(std::move(InValue));
    }

    ~ScopedEnvironmentVariable() {
        if (hadPrevious_) {
            Set(previous_);
        } else {
            Unset();
        }
    }

    ScopedEnvironmentVariable(const ScopedEnvironmentVariable&) = delete;
    auto operator=(const ScopedEnvironmentVariable&)
        -> ScopedEnvironmentVariable& = delete;

  private:
    auto Set(const std::string& InValue) -> void {
#if defined(_WIN32)
        _putenv_s(name_.c_str(), InValue.c_str());
#else
        setenv(name_.c_str(), InValue.c_str(), 1);
#endif
    }

    auto Unset() -> void {
#if defined(_WIN32)
        _putenv_s(name_.c_str(), "");
#else
        unsetenv(name_.c_str());
#endif
    }

    std::string name_;
    bool hadPrevious_ = false;
    std::string previous_;
};

auto RequireCommandSuccess(
    const kano::git::tests::functional::CommandResult& InResult,
    const std::string& InContext) -> void {
    INFO(InContext);
    INFO("exit=" << InResult.exitCode);
    INFO("stdout=" << InResult.stdoutText);
    INFO("stderr=" << InResult.stderrText);
    REQUIRE(InResult.exitCode == 0);
}

auto WriteTextFile(
    const std::filesystem::path& InPath,
    const std::string& InText) -> void {
    std::filesystem::create_directories(InPath.parent_path());
    std::ofstream out(InPath, std::ios::binary | std::ios::trunc);
    REQUIRE(out.good());
    out << InText;
}

auto InitRepo(const std::filesystem::path& InRepo) -> void {
    using namespace kano::git::tests::functional;
    std::filesystem::create_directories(InRepo);
    RequireCommandSuccess(
        RunGit({"init", InRepo.string()}, InRepo.parent_path()),
        "initialize repository");
    RequireCommandSuccess(
        RunGit({"config", "user.name", "KOG TUI Test"}, InRepo),
        "configure repository user name");
    RequireCommandSuccess(
        RunGit(
            {"config", "user.email", "kog-tui-test@example.invalid"},
            InRepo),
        "configure repository user email");
    WriteTextFile(InRepo / "README.md", "startup inventory fixture\n");
    RequireCommandSuccess(
        RunGit({"add", "README.md"}, InRepo),
        "stage repository fixture");
    RequireCommandSuccess(
        RunGit({"commit", "-m", "seed repository"}, InRepo),
        "commit repository fixture");
}

auto CurrentUtcForAuditFixture() -> std::string {
    const auto now = std::chrono::system_clock::now();
    const auto value = std::chrono::system_clock::to_time_t(now);
    std::tm utc{};
#if defined(_WIN32)
    gmtime_s(&utc, &value);
#else
    gmtime_r(&value, &utc);
#endif
    char buffer[32]{};
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &utc);
    return buffer;
}

}  // namespace

// Placeholder test to verify integration test infrastructure is working
TEST_CASE("Integration test infrastructure is set up correctly", "[infrastructure]") {
    REQUIRE(true);
}

TEST_CASE(
    "TUI first frame schedules startup I/O after ScreenInteractive activation and handles q while pending",
    "[integration][tui_startup][KG-BUG-0091]") {
    using namespace ftxui;

    auto screen = ScreenInteractive::FixedSize(48, 4);
    screen.TrackMouse(false);

    std::atomic<bool> startupScheduled{false};
    std::atomic<bool> startupIoStarted{false};
    std::atomic<bool> startupIoFinished{false};
    std::atomic<bool> firstFrameFlushed{false};
    std::atomic<bool> qHandledWhilePending{false};
    std::atomic<bool> watchdogExpired{false};
    std::promise<void> firstRenderPromise;
    auto firstRender = firstRenderPromise.get_future().share();
    std::promise<void> qHandledPromise;
    auto qHandled = qHandledPromise.get_future().share();
    std::promise<void> releaseStartupPromise;
    auto releaseStartup = releaseStartupPromise.get_future().share();
    std::thread startupWorker;
    TuiAsyncLifecycleState lifecycle;
    std::uint64_t generation = 0;
    std::ostringstream terminalOutput;
    const ScopedStreamBuffer captureTerminalOutput(
        std::cout,
        terminalOutput.rdbuf());

    auto component = Renderer([&]() {
        if (!startupScheduled.exchange(true)) {
            firstRenderPromise.set_value();
            screen.Post(ftxui::Closure([&]() {
                firstFrameFlushed.store(
                    terminalOutput.str().find(
                        "first interactive audit frame") !=
                    std::string::npos);
                const auto started = TryBeginTuiAsyncOperation(
                    lifecycle,
                    TuiAsyncSurface::None,
                    true);
                if (!started.has_value()) {
                    screen.Exit();
                    return;
                }
                generation = *started;
                startupIoStarted.store(true);
                startupWorker = std::thread([&]() {
                    releaseStartup.wait();
                    startupIoFinished.store(true);
                });
                screen.PostEvent(Event::Character('q'));
            }));
        }
        return text("first interactive audit frame");
    });

    component = CatchEvent(component, [&](const Event& InEvent) {
        if (InEvent != Event::Character('q')) {
            return false;
        }
        const auto exitDecision = RequestTuiAsyncExit(lifecycle);
        qHandledWhilePending.store(
            startupIoStarted.load() &&
            !startupIoFinished.load() &&
            !exitDecision.bExitNow &&
            exitDecision.bRequestCancellation);
        releaseStartupPromise.set_value();
        startupWorker.join();
        const auto completion =
            CompleteTuiAsyncOperation(lifecycle, generation);
        qHandledPromise.set_value();
        if (completion.bExitNow) {
            screen.ExitLoopClosure()();
        }
        return true;
    });

    std::thread watchdog([&]() {
        firstRender.wait();
        if (qHandled.wait_for(std::chrono::seconds(5)) !=
            std::future_status::ready) {
            watchdogExpired.store(true);
            screen.Exit();
        }
    });

    screen.Loop(component);
    watchdog.join();
    if (startupWorker.joinable()) {
        releaseStartupPromise.set_value();
        startupWorker.join();
    }

    REQUIRE_FALSE(watchdogExpired.load());
    REQUIRE(startupScheduled.load());
    REQUIRE(firstFrameFlushed.load());
    REQUIRE(startupIoStarted.load());
    REQUIRE(startupIoFinished.load());
    REQUIRE(qHandledWhilePending.load());
}

TEST_CASE(
    "TUI structured receipt command projects a verified persisted audit run",
    "[integration][tui_audit][KG-TSK-0132]") {
    const ScopedSandbox sandbox("tui-audit-receipt-projection");
    const auto workspace = (sandbox.Root() / "workspace").lexically_normal();
    InitRepo(workspace);

    const auto planFile = workspace / "audit-plan.json";
    const auto planBytes = nlohmann::json({
        {"meta", {
            {"plan_id", "tui-audit-plan"},
            {"correlation", {
                {"mode", "koa"},
                {"product_id", "product"},
                {"topic_id", "topic"},
                {"item_id", "item"},
                {"work_order_id", "work"},
                {"request_id", "request"},
                {"run_id", "tui-audit-run"},
                {"parent_run_id", "parent-run"},
                {"producer_id", "producer"},
                {"route_id", "route"},
                {"attempt", 1},
            }},
        }},
    }).dump() + "\n";
    WriteTextFile(planFile, planBytes);

    OperationAuditSpec spec;
    spec.workspaceRoot = workspace;
    spec.sourcePath = planFile;
    spec.inputIdentity =
        std::filesystem::weakly_canonical(planFile).generic_string();
    spec.inputKind = "commit-plan";
    spec.route = "commit-push.plan";
    spec.planId = "tui-audit-plan";
    spec.sourceBytes = planBytes;
    spec.frozenBytes = planBytes;
    spec.frozenFileName = "frozen-plan.json";
    spec.correlation.mode = "koa";
    spec.correlation.productId = "product";
    spec.correlation.topicId = "topic";
    spec.correlation.itemId = "item";
    spec.correlation.workOrderId = "work";
    spec.correlation.requestId = "request";
    spec.correlation.runId = "tui-audit-run";
    spec.correlation.parentRunId = "parent-run";
    spec.correlation.producerId = "producer";
    spec.correlation.routeId = "route";
    spec.correlation.attempt = 1;

    std::string error;
    auto audit = OperationAuditContext::Reserve(spec, &error);
    INFO(error);
    REQUIRE(audit);
    const auto before = audit->Capture(workspace);
    REQUIRE(audit->Append(
        "tui.audit.inspect",
        workspace,
        before,
        CurrentUtcForAuditFixture(),
        0,
        &error));
    REQUIRE(audit->Finalize(0, &error));
    audit.reset();

    const TuiCommandScopeSnapshot scope{
        .mode = TuiCommandScopeMode::Workspace,
        .workspaceRoot = workspace,
        .selectedRepoPath = workspace,
        .selectedRepoDisplay = ".",
    };
    const auto command = BuildTuiAuditCommand(
        "audit verify --plan-file audit-plan.json "
        "--run-id tui-audit-run --attempt 1 --json",
        scope);
    REQUIRE(command.has_value());
    REQUIRE(command->auditVerification.has_value());

    const auto read = ReadOperationAuditVerification({
        .workspaceRoot = command->workingDirectory,
        .planFile = command->auditVerification->planFile,
        .runId = command->auditVerification->runId,
        .attempt = command->auditVerification->attempt,
    });
    REQUIRE(read.verified());

    auto model = ApplyTuiAuditRunReadResult(
        TuiAuditFrameModel{
            .view = TuiAuditView::Receipt,
            .load = TuiAuditLoad::Loading,
            .scope = "workspace",
            .repository = ".",
            .provenanceSource = "live",
            .provenanceFreshness = "fresh",
            .nextAction = "inspect only",
        },
        read);
    CHECK(model.receiptState == TuiAuditReceiptState::Linked);
    CHECK(model.runId == "tui-audit-run");
    CHECK(model.receiptId.size() == 64);
    CHECK(model.load == TuiAuditLoad::Ready);
    CHECK(model.scope == "workspace");
    CHECK(model.repository.find("workspace") != std::string::npos);
    CHECK(model.repository.find("->") != std::string::npos);
    const auto rendered = RenderTuiAuditFrameText(
        model,
        TuiAuditFrameGeometry{120, 36},
        TuiAuditSemanticTheme{true});
    CHECK(rendered.find("tui-audit-run") != std::string::npos);
    CHECK(rendered.find(model.receiptId) != std::string::npos);
    CHECK(rendered.find("koa") != std::string::npos);
    CHECK(rendered.find("Esc/q close") != std::string::npos);
}

TEST_CASE(
    "built KOG demo exposes the production audit frame contract",
    "[integration][tui_audit][production-path][KG-TSK-0132]") {
    using namespace kano::git::tests::functional;

    const ScopedSandbox sandbox("tui-audit-built-demo");
    const auto result = RunKog({"tui", "--demo"}, sandbox.Root());
    RequireCommandSuccess(result, "run built KOG TUI audit demo");
    CHECK(result.stdoutText.find("deterministic production audit frame") !=
          std::string::npos);
    CHECK(result.stdoutText.find("Repository Audit") != std::string::npos);
    CHECK(result.stdoutText.find("scope: workspace") != std::string::npos);
    CHECK(result.stdoutText.find("demo-repository") != std::string::npos);
    CHECK(result.stdoutText.find("receipt=missing") != std::string::npos);
    CHECK(result.stdoutText.find("q quit") != std::string::npos);
    CHECK(result.stdoutText.find('\x1b') == std::string::npos);
}

TEST_CASE(
    "redirected standard streams reject both interactive TUI entrypoints cleanly",
    "[integration][tui_terminal_guard][production-path][KG-BUG-0108]") {
    using namespace kano::git::tests::functional;

    const ScopedSandbox sandbox("tui-terminal-preflight-redirected");
    const auto standaloneTui =
        ResolveKogBinaryPath().parent_path() /
#if defined(_WIN32)
        "kano-git-tui.exe";
#else
        "kano-git-tui";
#endif
    const std::vector<std::pair<std::string, CommandResult>> results{
        {"kog tui", RunKog({"tui"}, sandbox.Root())},
        {"standalone tui", RunCommand(standaloneTui.string(), {}, sandbox.Root())},
    };

    for (const auto& [name, result] : results) {
        INFO(name);
        INFO("exit=" << result.exitCode);
        INFO("stdout=" << result.stdoutText);
        INFO("stderr=" << result.stderrText);
        CHECK(result.exitCode == 3);
        CHECK(result.stdoutText.empty());
        CHECK(result.stderrText.find("interactive stdin and stdout are required") !=
              std::string::npos);
        CHECK(result.stderrText.size() <= 256U);
        CHECK(result.stdoutText.find('\x1b') == std::string::npos);
        CHECK(result.stderrText.find('\x1b') == std::string::npos);
    }
}

TEST_CASE(
    "TUI production startup loads a disposable three-repository inventory through the built KOG binary",
    "[integration][tui_startup][production-path][KG-BUG-0091][KG-TSK-0131]") {
    using namespace kano::git::tests::functional;

    const ScopedSandbox sandbox("tui-startup-production-path");
    const auto workspace = (sandbox.Root() / "workspace").lexically_normal();
    InitRepo(workspace);
    InitRepo(workspace / "alpha");
    InitRepo(workspace / "nested" / "beta");

    WriteTextFile(
        workspace / ".gitmodules",
        "[submodule \"alpha\"]\n"
        "\tpath = alpha\n"
        "\turl = ./alpha\n"
        "[submodule \"nested-beta\"]\n"
        "\tpath = nested/beta\n"
        "\turl = ./nested/beta\n");
    RequireCommandSuccess(
        RunGit({"add", ".gitmodules"}, workspace),
        "stage registered repository inventory");
    RequireCommandSuccess(
        RunGit(
            {"commit", "-m", "register startup inventory"},
            workspace),
        "commit registered repository inventory");

    RequireCommandSuccess(
        RunKog(
            {
                "discover",
                "--repo-root",
                workspace.string(),
                "--format",
                "json",
                "--full",
                "--unregistered-depth",
                "2",
                "--no-cache",
            },
            workspace),
        "create trusted disposable workspace manifest");

    std::vector<TuiStartupRepoSnapshot> rows;
    TuiStartupSnapshotMetadata metadata;
#if !defined(_WIN32)
    const auto fakeBin = workspace / "fake-bin";
    const auto fakeGit = fakeBin / "git";
    const auto fakeGitMarker = workspace / "fake-git-invoked";
    WriteTextFile(
        fakeGit,
        "#!/bin/sh\n"
        "if [ \"$1\" = \"rev-list\" ]; then\n"
        "  printf invoked > \"$KOG_TUI_FAKE_GIT_MARKER\"\n"
        "  sleep 10\n"
        "  exit 88\n"
        "fi\n"
        "exec \"$KOG_TUI_REAL_GIT\" \"$@\"\n");
    std::error_code permissionError;
    std::filesystem::permissions(
        fakeGit,
        std::filesystem::perms::owner_exec |
            std::filesystem::perms::group_exec |
            std::filesystem::perms::others_exec,
        std::filesystem::perm_options::add,
        permissionError);
    REQUIRE_FALSE(permissionError);
    const auto inheritedPath = std::getenv("PATH");
    const ScopedEnvironmentVariable markerEnvironment(
        "KOG_TUI_FAKE_GIT_MARKER",
        fakeGitMarker.generic_string());
    const auto realGit = std::filesystem::path("/usr/bin/git");
    REQUIRE(std::filesystem::exists(realGit));
    const ScopedEnvironmentVariable realGitEnvironment(
        "KOG_TUI_REAL_GIT",
        realGit.generic_string());
    const ScopedEnvironmentVariable pathEnvironment(
        "PATH",
        fakeBin.generic_string() + ":" +
            (inheritedPath == nullptr ? "" : inheritedPath));
#endif
    REQUIRE_NOTHROW(rows = LoadTuiStartupSnapshot({
        .binaryCommand = ResolveKogBinaryPath().generic_string(),
        .workspaceRoot = workspace,
        .timeoutMs = 5000,
        .maxCaptureBytes = 1024U * 1024U,
    }, &metadata));
#if !defined(_WIN32)
    CHECK_FALSE(std::filesystem::exists(fakeGitMarker));
#endif
    REQUIRE(rows.size() == 3);
    CHECK(metadata.source == "trusted-workspace-manifest");
    CHECK(metadata.completeness == "workspace-inventory");
    CHECK(metadata.probeMode == "none");
    CHECK(metadata.statusKnown);
    REQUIRE(metadata.observedAtUtc.has_value());
    REQUIRE(metadata.observedAtUtcText.has_value());
    CHECK(ParseTuiObservedAtUtc(*metadata.observedAtUtcText) ==
          metadata.observedAtUtc);
    const auto rootRow = std::find_if(
        rows.begin(),
        rows.end(),
        [&](const TuiStartupRepoSnapshot& InRow) {
            return InRow.path == workspace;
        });
    REQUIRE(rootRow != rows.end());
    CHECK(rootRow->parentPath.empty());
    for (const auto& row : rows) {
        if (row.path != workspace) {
            CHECK(row.parentPath == workspace);
        }
    }
}

TEST_CASE(
    "TUI production startup exposes a bounded root fallback before first discovery",
    "[integration][tui_startup][production-path][KG-BUG-0091][KG-TSK-0131]") {
    using namespace kano::git::tests::functional;

    const ScopedSandbox sandbox("tui-startup-root-fallback");
    const auto workspace = (sandbox.Root() / "workspace").lexically_normal();
    InitRepo(workspace);

    std::vector<TuiStartupRepoSnapshot> rows;
    TuiStartupSnapshotMetadata metadata;
    REQUIRE_NOTHROW(rows = LoadTuiStartupSnapshot({
        .binaryCommand = ResolveKogBinaryPath().generic_string(),
        .workspaceRoot = workspace,
        .timeoutMs = 5000,
        .maxCaptureBytes = 1024U * 1024U,
    }, &metadata));

    REQUIRE(rows.size() == 1);
    CHECK(rows.front().path == workspace);
    CHECK(rows.front().type == "root");
    CHECK(rows.front().branch == "(unknown)");
    CHECK_FALSE(rows.front().statusKnown);
    CHECK(std::string(TuiAuditBooleanLabel(
              rows.front().statusKnown,
              rows.front().repoDirty)) == "unknown");
    CHECK(metadata.source == "root-fallback");
    CHECK(metadata.completeness == "root-only");
    CHECK(metadata.probeMode == "none");
    CHECK_FALSE(metadata.statusKnown);
    CHECK_FALSE(metadata.observedAtUtc.has_value());
    CHECK_FALSE(metadata.observedAtUtcText.has_value());
    CHECK(metadata.allowedExternalRoots.empty());

}

TEST_CASE(
    "TUI production startup rejects an invalid existing workspace manifest",
    "[integration][tui_startup][production-path][KG-BUG-0091]") {
    using namespace kano::git::tests::functional;

    const ScopedSandbox sandbox("tui-startup-invalid-manifest");
    const auto workspace = (sandbox.Root() / "workspace").lexically_normal();
    InitRepo(workspace);
    WriteTextFile(
        kano::git::workspace::WorkspaceManifestFilePath(workspace),
        "{invalid workspace state");
    std::string corruptManifestError;
    try {
        (void)LoadTuiStartupSnapshot({
            .binaryCommand = ResolveKogBinaryPath().generic_string(),
            .workspaceRoot = workspace,
            .timeoutMs = 5000,
            .maxCaptureBytes = 1024U * 1024U,
        });
    } catch (const std::exception& exception) {
        corruptManifestError = exception.what();
    }
    REQUIRE_FALSE(corruptManifestError.empty());
    CHECK(
        corruptManifestError.find(
            "workspace manifest invalid or unreadable") !=
        std::string::npos);
    CHECK(
        corruptManifestError.find("workspace manifest missing") ==
        std::string::npos);
}

TEST_CASE(
    "TUI production startup admits only configured external repository roots",
    "[integration][tui_startup][production-path][KG-BUG-0091]") {
    using namespace kano::git::tests::functional;

    const ScopedSandbox sandbox("tui-startup-external-root");
    const auto workspace = (sandbox.Root() / "workspace").lexically_normal();
    const auto externalRepo =
        (sandbox.Root() / "external" / "audit-repo").lexically_normal();
    InitRepo(workspace);
    InitRepo(externalRepo);
    WriteTextFile(
        workspace / ".kano" / "kog_config.toml",
        "[workspace.external]\n"
        "inherit = false\n"
        "roots = ['" + externalRepo.generic_string() + "']\n");
    RequireCommandSuccess(
        RunKog(
            {
                "discover",
                "--repo-root",
                workspace.string(),
                "--format",
                "json",
                "--full",
                "--unregistered-depth",
                "2",
                "--no-cache",
            },
            workspace),
        "create workspace manifest with a configured external root");

    std::vector<TuiStartupRepoSnapshot> rows;
    TuiStartupSnapshotMetadata metadata;
    REQUIRE_NOTHROW(rows = LoadTuiStartupSnapshot({
        .binaryCommand = ResolveKogBinaryPath().generic_string(),
        .workspaceRoot = workspace,
        .timeoutMs = 5000,
        .maxCaptureBytes = 1024U * 1024U,
    }, &metadata));

    REQUIRE(rows.size() == 2);
    CHECK(std::any_of(
        rows.begin(),
        rows.end(),
        [&](const TuiStartupRepoSnapshot& InRow) {
            return InRow.path == externalRepo;
        }));
    REQUIRE(metadata.allowedExternalRoots.size() == 1);
    CHECK(metadata.allowedExternalRoots.front() == externalRepo);
    CHECK(metadata.source == "trusted-workspace-manifest");
    CHECK(metadata.statusKnown);
}

#if !defined(_WIN32)
TEST_CASE(
    "TUI production overview JSON escapes POSIX control bytes in repository paths",
    "[integration][tui_startup][production-path][KG-BUG-0091]") {
    using namespace kano::git::tests::functional;

    std::string sandboxName = "tui-startup-control-byte-";
    sandboxName.push_back('\x01');
    const ScopedSandbox sandbox(std::move(sandboxName));
    const auto workspace = (sandbox.Root() / "workspace").lexically_normal();
    InitRepo(workspace);
    RequireCommandSuccess(
        RunKog(
            {
                "discover",
                "--repo-root",
                workspace.string(),
                "--format",
                "json",
                "--full",
                "--unregistered-depth",
                "2",
                "--no-cache",
            },
            workspace),
        "create control-byte workspace manifest");

    std::vector<TuiStartupRepoSnapshot> rows;
    REQUIRE_NOTHROW(rows = LoadTuiStartupSnapshot({
        .binaryCommand = ResolveKogBinaryPath().generic_string(),
        .workspaceRoot = workspace,
        .timeoutMs = 5000,
        .maxCaptureBytes = 1024U * 1024U,
    }));
    REQUIRE(rows.size() == 1);
    CHECK(rows.front().path == workspace);
}
#endif

// Future integration tests will be added here as components are implemented:
// - Complete flow: enter Command_Mode, type command, execute (Task 9.5)
// - Autocomplete trigger on typing (Task 9.5)
// - Candidate navigation and selection (Task 9.5)
// - Complete workflows using only single-key shortcuts (Task 16.4)
// - Test complete user workflows (Task 19.3)
//   - Enter command mode, type command, autocomplete, execute
//   - Open command palette, search, select command
//   - View help panel, navigate, close
//   - Error handling and recovery
