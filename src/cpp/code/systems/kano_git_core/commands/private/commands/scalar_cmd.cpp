// scalar command — Git Scalar integration for mono-repo performance
// Delegates to: scripts/mono-repo/scalar/*.sh

#include "command_registry.hpp"
#include "shell_executor.hpp"

#include <algorithm>
#include <cctype>
#include <format>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {

struct ScalarStatus {
    std::string repoRoot;
    bool scalarRegistered = false;
    std::string partialClone;
    std::string sparseCheckout;
    std::string fsmonitor;
    std::string multipackIndex;
    std::string commitGraph;
    std::string maintenanceEnabled;
    std::string prefetchSchedule;
    std::string commitGraphSchedule;
    std::string looseObjectsSchedule;
    std::string incrementalRepackSchedule;
    int objectCount = 0;
    int packCount = 0;
    int sizeMb = 0;
};

auto Trim(std::string InValue) -> std::string {
    const auto notSpace = [](unsigned char ch) { return !std::isspace(ch); };
    InValue.erase(InValue.begin(), std::find_if(InValue.begin(), InValue.end(), notSpace));
    InValue.erase(std::find_if(InValue.rbegin(), InValue.rend(), notSpace).base(), InValue.end());
    return InValue;
}

auto JsonEscape(const std::string& InValue) -> std::string {
    std::string out;
    out.reserve(InValue.size() + 16);
    for (const char ch : InValue) {
        switch (ch) {
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out.push_back(ch); break;
        }
    }
    return out;
}

auto RunGit(const std::vector<std::string>& InArgs) -> kano::git::shell::ExecResult {
    return kano::git::shell::ExecuteCommand("git", InArgs, kano::git::shell::ExecMode::Capture);
}

auto GetGitConfigValue(const std::string& InKey, const std::string& InDefault) -> std::string {
    const auto result = RunGit({"config", "--get", InKey});
    if (result.exitCode != 0) {
        return InDefault;
    }
    const auto value = Trim(result.stdoutStr);
    return value.empty() ? InDefault : value;
}

auto EnsureGitRepository() -> bool {
    const auto result = RunGit({"rev-parse", "--git-dir"});
    if (result.exitCode == 0) {
        return true;
    }
    std::cerr << "Error: Not in a git repository\n";
    return false;
}

auto ScalarAvailable() -> bool {
    const auto result = RunGit({"scalar", "--help"});
    return result.exitCode == 0;
}

auto ParseCountObjects(const std::string& InText, const std::string& InKey) -> int {
    std::istringstream iss(InText);
    std::string line;
    const std::string prefix = InKey + ":";
    while (std::getline(iss, line)) {
        const auto trimmed = Trim(line);
        if (trimmed.rfind(prefix, 0) == 0) {
            const auto value = Trim(trimmed.substr(prefix.size()));
            try {
                return std::stoi(value);
            } catch (...) {
                return 0;
            }
        }
    }
    return 0;
}

auto CollectScalarStatus() -> ScalarStatus {
    ScalarStatus status;
    status.repoRoot = Trim(RunGit({"rev-parse", "--show-toplevel"}).stdoutStr);

    const auto maintenanceCheck = RunGit({"config", "--get-regexp", "maintenance\\."});
    status.scalarRegistered = maintenanceCheck.exitCode == 0;

    status.partialClone = GetGitConfigValue("remote.origin.promisor", "false");
    status.sparseCheckout = GetGitConfigValue("core.sparseCheckout", "false");
    status.fsmonitor = GetGitConfigValue("core.fsmonitor", "false");
    status.multipackIndex = GetGitConfigValue("core.multiPackIndex", "false");
    status.commitGraph = GetGitConfigValue("core.commitGraph", "false");

    status.maintenanceEnabled = GetGitConfigValue("maintenance.auto", "false");
    status.prefetchSchedule = GetGitConfigValue("maintenance.prefetch.schedule", "none");
    status.commitGraphSchedule = GetGitConfigValue("maintenance.commit-graph.schedule", "none");
    status.looseObjectsSchedule = GetGitConfigValue("maintenance.loose-objects.schedule", "none");
    status.incrementalRepackSchedule = GetGitConfigValue("maintenance.incremental-repack.schedule", "none");

    const auto countObjects = RunGit({"count-objects", "-v"});
    status.objectCount = ParseCountObjects(countObjects.stdoutStr, "count");
    status.packCount = ParseCountObjects(countObjects.stdoutStr, "packs");
    const int sizeKb = ParseCountObjects(countObjects.stdoutStr, "size-pack");
    status.sizeMb = sizeKb / 1024;

    return status;
}

auto RunNativeScalarOptimize(bool InDryRun) -> int {
    if (!EnsureGitRepository()) {
        return 1;
    }
    if (!ScalarAvailable()) {
        std::cerr << "Error: Git Scalar is not available\n";
        std::cerr << "Git Scalar requires Git 2.38 or higher.\n";
        return 1;
    }

    const auto maintenanceCheck = RunGit({"config", "--get-regexp", "maintenance\\."});
    if (maintenanceCheck.exitCode != 0) {
        std::cerr << "Error: Repository is not registered with Scalar\n\n";
        std::cerr << "Register first with: ./scalar/register.sh\n";
        return 1;
    }

    const auto repoRoot = Trim(RunGit({"rev-parse", "--show-toplevel"}).stdoutStr);
    std::cout << "Running Git Scalar optimizations...\n";
    std::cout << "  Repository: " << repoRoot << "\n\n";

    if (InDryRun) {
        std::cout << "[DRY RUN] Would execute: git maintenance run --task=prefetch,commit-graph,loose-objects,incremental-repack,pack-refs\n\n";
        std::cout << "[DRY RUN] Tasks that would run:\n";
        std::cout << "  1. prefetch          - Fetch new objects from remote\n";
        std::cout << "  2. commit-graph      - Update commit graph\n";
        std::cout << "  3. loose-objects     - Pack loose objects\n";
        std::cout << "  4. incremental-repack - Repack objects incrementally\n";
        std::cout << "  5. pack-refs         - Pack references\n\n";
        std::cout << "[DRY RUN] This may take several minutes for large repositories\n";
        return 0;
    }

    const auto runTask = [](const std::string& taskName) -> int {
        auto result = kano::git::shell::ExecuteCommand(
            "git",
            {"maintenance", "run", "--task=" + taskName},
            kano::git::shell::ExecMode::PassThrough
        );
        return result.exitCode;
    };

    std::cout << "Task 1/5: Prefetch (fetching new objects)...\n";
    if (runTask("prefetch") != 0) {
        return 1;
    }
    std::cout << "\nTask 2/5: Commit Graph (updating commit graph)...\n";
    if (runTask("commit-graph") != 0) {
        return 1;
    }
    std::cout << "\nTask 3/5: Loose Objects (packing loose objects)...\n";
    if (runTask("loose-objects") != 0) {
        return 1;
    }
    std::cout << "\nTask 4/5: Incremental Repack (repacking objects)...\n";
    if (runTask("incremental-repack") != 0) {
        return 1;
    }
    std::cout << "\nTask 5/5: Pack Refs (packing references)...\n";
    if (runTask("pack-refs") != 0) {
        return 1;
    }

    std::cout << "\n✓ All optimization tasks completed\n";
    return 0;
}

auto RunNativeScalarRegister(bool InDryRun) -> int {
    if (!EnsureGitRepository()) {
        return 1;
    }
    if (!ScalarAvailable()) {
        std::cerr << "Error: Git Scalar is not available\n\n";
        std::cerr << "Git Scalar requires Git 2.38 or higher.\n";
        std::cerr << "Please upgrade Git or use manual configuration.\n\n";
        std::cerr << "Check your Git version:\n";
        std::cerr << "  git --version\n";
        return 1;
    }

    const auto repoRoot = Trim(RunGit({"rev-parse", "--show-toplevel"}).stdoutStr);
    std::cout << "Registering repository with Git Scalar...\n";
    std::cout << "  Repository: " << repoRoot << "\n\n";

    if (InDryRun) {
        std::cout << "[DRY RUN] Would execute: git scalar register\n\n";
        std::cout << "[DRY RUN] Would configure:\n";
        std::cout << "  - Partial clone (blob:none filter)\n";
        std::cout << "  - Sparse checkout (cone mode)\n";
        std::cout << "  - Background maintenance (hourly)\n";
        std::cout << "  - FSMonitor (if available)\n";
        std::cout << "  - Multi-pack index\n";
        std::cout << "  - Commit graph\n\n";
        std::cout << "[DRY RUN] Would enable background tasks:\n";
        std::cout << "  - prefetch: Fetch new objects in background\n";
        std::cout << "  - commit-graph: Update commit graph\n";
        std::cout << "  - loose-objects: Pack loose objects\n";
        std::cout << "  - incremental-repack: Repack incrementally\n";
        return 0;
    }

    std::cout << "Executing: git scalar register\n\n";
    auto registerResult = kano::git::shell::ExecuteCommand(
        "git",
        {"scalar", "register"},
        kano::git::shell::ExecMode::PassThrough
    );
    if (registerResult.exitCode != 0) {
        return registerResult.exitCode;
    }

    std::cout << "\n✓ Repository registered with Git Scalar\n\n";
    std::cout << "Optimizations enabled:\n";
    std::cout << "  ✓ Partial clone configuration\n";
    std::cout << "  ✓ Sparse checkout configuration\n";
    std::cout << "  ✓ Background maintenance scheduled\n";
    std::cout << "  ✓ FSMonitor enabled (if available)\n";
    std::cout << "  ✓ Multi-pack index enabled\n";
    std::cout << "  ✓ Commit graph enabled\n\n";
    std::cout << "Background maintenance tasks:\n";
    std::cout << "  - prefetch: Hourly\n";
    std::cout << "  - commit-graph: Hourly\n";
    std::cout << "  - loose-objects: Daily\n";
    std::cout << "  - incremental-repack: Daily\n\n";
    std::cout << "Check status with: ./scalar/status.sh\n";
    std::cout << "Run manual optimization: ./scalar/optimize.sh\n";
    return 0;
}

auto RunNativeScalarUnregister(bool InDryRun) -> int {
    if (!EnsureGitRepository()) {
        return 1;
    }
    if (!ScalarAvailable()) {
        std::cerr << "Error: Git Scalar is not available\n";
        std::cerr << "Git Scalar requires Git 2.38 or higher.\n";
        return 1;
    }

    const auto maintenanceCheck = RunGit({"config", "--get-regexp", "maintenance\\."});
    if (maintenanceCheck.exitCode != 0) {
        std::cout << "Repository is not registered with Scalar\n";
        std::cout << "Nothing to do.\n";
        return 0;
    }

    const auto repoRoot = Trim(RunGit({"rev-parse", "--show-toplevel"}).stdoutStr);
    std::cout << "Unregistering repository from Git Scalar...\n";
    std::cout << "  Repository: " << repoRoot << "\n\n";

    if (InDryRun) {
        std::cout << "[DRY RUN] Would execute: git scalar unregister\n\n";
        std::cout << "[DRY RUN] Would remove:\n";
        std::cout << "  - Background maintenance schedule\n";
        std::cout << "  - Scalar-specific configuration\n";
        std::cout << "  - Automatic optimization tasks\n\n";
        std::cout << "[DRY RUN] Would preserve:\n";
        std::cout << "  - All repository data\n";
        std::cout << "  - All commits and history\n";
        std::cout << "  - Working tree files\n";
        std::cout << "  - Standard Git configuration\n";
        return 0;
    }

    std::cout << "Executing: git scalar unregister\n\n";
    auto unregisterResult = kano::git::shell::ExecuteCommand(
        "git",
        {"scalar", "unregister"},
        kano::git::shell::ExecMode::PassThrough
    );
    if (unregisterResult.exitCode != 0) {
        return unregisterResult.exitCode;
    }

    std::cout << "\n✓ Repository unregistered from Git Scalar\n\n";
    std::cout << "Changes:\n";
    std::cout << "  ✓ Background maintenance disabled\n";
    std::cout << "  ✓ Scalar optimizations removed\n";
    std::cout << "  ✓ Repository reverted to standard Git\n\n";
    std::cout << "Repository data preserved:\n";
    std::cout << "  ✓ All commits and history\n";
    std::cout << "  ✓ All branches and tags\n";
    std::cout << "  ✓ Working tree files\n\n";
    std::cout << "To re-enable Scalar: ./scalar/register.sh\n";
    return 0;
}

void PrintScalarJson(const ScalarStatus& InStatus) {
    std::cout << "{\n";
    std::cout << std::format("  \"repository\": \"{}\",\n", JsonEscape(InStatus.repoRoot));
    std::cout << std::format("  \"scalar_registered\": {},\n", InStatus.scalarRegistered ? "true" : "false");
    std::cout << "  \"optimizations\": {\n";
    std::cout << std::format("    \"partial_clone\": \"{}\",\n", JsonEscape(InStatus.partialClone));
    std::cout << std::format("    \"sparse_checkout\": \"{}\",\n", JsonEscape(InStatus.sparseCheckout));
    std::cout << std::format("    \"fsmonitor\": \"{}\",\n", JsonEscape(InStatus.fsmonitor));
    std::cout << std::format("    \"multipack_index\": \"{}\",\n", JsonEscape(InStatus.multipackIndex));
    std::cout << std::format("    \"commit_graph\": \"{}\"\n", JsonEscape(InStatus.commitGraph));
    std::cout << "  },\n";
    std::cout << "  \"maintenance\": {\n";
    std::cout << std::format("    \"enabled\": \"{}\",\n", JsonEscape(InStatus.maintenanceEnabled));
    std::cout << "    \"schedules\": {\n";
    std::cout << std::format("      \"prefetch\": \"{}\",\n", JsonEscape(InStatus.prefetchSchedule));
    std::cout << std::format("      \"commit_graph\": \"{}\",\n", JsonEscape(InStatus.commitGraphSchedule));
    std::cout << std::format("      \"loose_objects\": \"{}\",\n", JsonEscape(InStatus.looseObjectsSchedule));
    std::cout << std::format("      \"incremental_repack\": \"{}\"\n", JsonEscape(InStatus.incrementalRepackSchedule));
    std::cout << "    }\n";
    std::cout << "  },\n";
    std::cout << "  \"statistics\": {\n";
    std::cout << std::format("    \"object_count\": {},\n", InStatus.objectCount);
    std::cout << std::format("    \"pack_count\": {},\n", InStatus.packCount);
    std::cout << std::format("    \"size_mb\": {}\n", InStatus.sizeMb);
    std::cout << "  }\n";
    std::cout << "}\n";
}

void PrintScalarText(const ScalarStatus& InStatus) {
    std::cout << "Git Scalar Status\n";
    std::cout << "=================\n\n";
    std::cout << "Repository: " << InStatus.repoRoot << "\n\n";
    if (InStatus.scalarRegistered) {
        std::cout << "Status: Registered with Scalar\n";
    } else {
        std::cout << "Status: Not registered with Scalar\n\n";
        std::cout << "To register: ./scalar/register.sh\n";
        return;
    }

    std::cout << "\nOptimizations:\n";
    std::cout << "  Partial Clone:     " << InStatus.partialClone << "\n";
    std::cout << "  Sparse Checkout:   " << InStatus.sparseCheckout << "\n";
    std::cout << "  FSMonitor:         " << InStatus.fsmonitor << "\n";
    std::cout << "  Multi-pack Index:  " << InStatus.multipackIndex << "\n";
    std::cout << "  Commit Graph:      " << InStatus.commitGraph << "\n";

    std::cout << "\nBackground Maintenance:\n";
    std::cout << "  Enabled:            " << InStatus.maintenanceEnabled << "\n";
    std::cout << "  Prefetch:           " << InStatus.prefetchSchedule << "\n";
    std::cout << "  Commit Graph:       " << InStatus.commitGraphSchedule << "\n";
    std::cout << "  Loose Objects:      " << InStatus.looseObjectsSchedule << "\n";
    std::cout << "  Incremental Repack: " << InStatus.incrementalRepackSchedule << "\n";

    std::cout << "\nRepository Statistics:\n";
    std::cout << "  Objects:           " << InStatus.objectCount << "\n";
    std::cout << "  Packs:             " << InStatus.packCount << "\n";
    std::cout << "  Size:              " << InStatus.sizeMb << " MB\n\n";

    std::cout << "Commands:\n";
    std::cout << "  Run optimization:  ./scalar/optimize.sh\n";
    std::cout << "  Unregister:        ./scalar/unregister.sh\n";
}

} // namespace

namespace kano::git::commands {

void RegisterScalar(CLI::App& InApp) {
    auto* cmd = InApp.add_subcommand("scalar", "Git Scalar mono-repo performance tools");

    auto* reg = cmd->add_subcommand("register", "Register repository with Scalar");
    reg->allow_extras();
    auto* registerNative = new bool{false};
    auto* registerShell = new bool{false};
    auto* registerDryRun = new bool{false};
    reg->add_flag("--native", *registerNative, "Use native C++ scalar register implementation (default)");
    reg->add_flag("--shell", *registerShell, "Deprecated compatibility flag (shell path removed)");
    reg->add_flag("--dry-run", *registerDryRun, "Preview mode");
    reg->callback([=]() {
        if (*registerShell && *registerNative) {
            std::cerr << "Error: --shell cannot be combined with --native\n";
            std::exit(1);
        }
        if (*registerShell) {
            std::cerr << "Error: --shell is no longer supported; scalar register is fully native now\n";
            std::exit(2);
        }

        auto extras = reg->remaining();
        if (!extras.empty()) {
            std::cerr << "Error: Unexpected argument: " << extras.front() << "\n";
            std::exit(1);
        }
        std::exit(RunNativeScalarRegister(*registerDryRun));
    });

    auto* status = cmd->add_subcommand("status", "Show Scalar status");
    status->allow_extras();
    auto* statusNative = new bool{false};
    auto* statusShell = new bool{false};
    auto* statusFormat = new std::string{"text"};
    status->add_flag("--native", *statusNative, "Use native C++ scalar status implementation (default)");
    status->add_flag("--shell", *statusShell, "Deprecated compatibility flag (shell path removed)");
    status->add_option("--format", *statusFormat, "Output format: text|json");
    status->callback([=]() {
        if (*statusShell) {
            std::cerr << "Error: --shell is no longer supported; scalar status is fully native now\n";
            std::exit(2);
        }
        if (*statusFormat != "text" && *statusFormat != "json") {
            std::cerr << "Error: Invalid format: " << *statusFormat << " (must be text or json)\n";
            std::exit(1);
        }
        if (!EnsureGitRepository()) {
            std::exit(1);
        }
        if (!ScalarAvailable()) {
            if (*statusFormat == "json") {
                std::cout << "{\"error\": \"Git Scalar not available\", \"scalar_available\": false}\n";
            } else {
                std::cerr << "Error: Git Scalar is not available\n";
                std::cerr << "Git Scalar requires Git 2.38 or higher.\n";
            }
            std::exit(1);
        }

        const auto native = CollectScalarStatus();
        if (*statusFormat == "json") {
            PrintScalarJson(native);
        } else {
            PrintScalarText(native);
        }
        std::exit(0);
    });

    auto* optimize = cmd->add_subcommand("optimize", "Optimize repository");
    optimize->allow_extras();
    auto* optimizeNative = new bool{false};
    auto* optimizeShell = new bool{false};
    auto* optimizeDryRun = new bool{false};
    optimize->add_flag("--native", *optimizeNative, "Use native C++ scalar optimize implementation (default)");
    optimize->add_flag("--shell", *optimizeShell, "Deprecated compatibility flag (shell path removed)");
    optimize->add_flag("--dry-run", *optimizeDryRun, "Preview mode");
    optimize->callback([=]() {
        if (*optimizeShell && *optimizeNative) {
            std::cerr << "Error: --shell cannot be combined with --native\n";
            std::exit(1);
        }
        if (*optimizeShell) {
            std::cerr << "Error: --shell is no longer supported; scalar optimize is fully native now\n";
            std::exit(2);
        }

        auto extras = optimize->remaining();
        if (!extras.empty()) {
            std::cerr << "Error: Unexpected argument: " << extras.front() << "\n";
            std::exit(1);
        }
        std::exit(RunNativeScalarOptimize(*optimizeDryRun));
    });

    auto* unreg = cmd->add_subcommand("unregister", "Unregister repository from Scalar");
    unreg->allow_extras();
    auto* unregisterNative = new bool{false};
    auto* unregisterShell = new bool{false};
    auto* unregisterDryRun = new bool{false};
    unreg->add_flag("--native", *unregisterNative, "Use native C++ scalar unregister implementation (default)");
    unreg->add_flag("--shell", *unregisterShell, "Deprecated compatibility flag (shell path removed)");
    unreg->add_flag("--dry-run", *unregisterDryRun, "Preview mode");
    unreg->callback([=]() {
        if (*unregisterShell && *unregisterNative) {
            std::cerr << "Error: --shell cannot be combined with --native\n";
            std::exit(1);
        }
        if (*unregisterShell) {
            std::cerr << "Error: --shell is no longer supported; scalar unregister is fully native now\n";
            std::exit(2);
        }

        auto extras = unreg->remaining();
        if (!extras.empty()) {
            std::cerr << "Error: Unexpected argument: " << extras.front() << "\n";
            std::exit(1);
        }
        std::exit(RunNativeScalarUnregister(*unregisterDryRun));
    });
}

} // namespace kano::git::commands
