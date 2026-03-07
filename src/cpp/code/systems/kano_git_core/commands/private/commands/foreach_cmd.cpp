// foreach command — run a command across discovered repos

#include "command_registry.hpp"
#include "shell_executor.hpp"
#include "native_workspace.hpp"
#include "discovery.hpp"

#include <algorithm>
#include <filesystem>
#include <future>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace kano::git::commands {
namespace {

struct RepoExecResult {
    std::string path;
    std::string type;
    int exitCode = 0;
    std::string stdoutStr;
    std::string stderrStr;
};

auto ExecuteRepoCommand(const workspace::RepoRecord& InRepo, const std::string& InCommand) -> RepoExecResult {
    RepoExecResult out;
    out.path = InRepo.path.lexically_normal().generic_string();
    out.type = InRepo.type;
#if defined(_WIN32)
    auto result = shell::ExecuteCommand("cmd.exe", {"/d", "/s", "/c", InCommand}, shell::ExecMode::Capture, InRepo.path);
#else
    auto result = shell::ExecuteCommand("bash", {"-lc", InCommand}, shell::ExecMode::Capture, InRepo.path);
#endif
    out.exitCode = result.exitCode;
    out.stdoutStr = std::move(result.stdoutStr);
    out.stderrStr = std::move(result.stderrStr);
    return out;
}

void PrintRepoExecResult(const RepoExecResult& InResult) {
    std::cout << "\n==> [" << InResult.path << "] (" << InResult.type << ")\n";
    if (!InResult.stdoutStr.empty()) {
        std::cout << InResult.stdoutStr;
        if (InResult.stdoutStr.back() != '\n') {
            std::cout << '\n';
        }
    }
    if (!InResult.stderrStr.empty()) {
        std::cerr << InResult.stderrStr;
        if (InResult.stderrStr.back() != '\n') {
            std::cerr << '\n';
        }
    }
}

} // namespace

void RegisterForeach(CLI::App& InApp) {
    auto* foreach = InApp.add_subcommand("foreach", "Run command on each discovered repo");
    foreach->allow_extras();
    foreach->positionals_at_end(false);
    auto* foreachNative = new bool{false};
    auto* foreachShell = new bool{false};
    auto* foreachNativePlan = new bool{false};
    auto* foreachNativePlanOnly = new bool{false};
    auto* foreachCommand = new std::string{};
    auto* foreachPositionalCommand = new std::string{};
    auto* foreachContinueOnError = new bool{false};
    auto* foreachParallel = new int{1};
    auto* foreachNativeMaxDepth = new int{3};
    auto* foreachNativeNoCache = new bool{false};
    auto* foreachNativeRefreshCache = new bool{false};
    auto* foreachNativeNoIncremental = new bool{false};
    auto* foreachNativeCacheTtl = new int{60};
    auto* foreachNativeMaxStale = new int{900};
    auto* foreachNativeMetadata = new std::string{"full"};
    auto* foreachManifest = new std::string{};
    auto* foreachIncludeTypes = new std::string{"root,registered,unregistered"};
    auto* foreachExclude = new std::vector<std::string>{};
    auto* foreachMaxDepth = new int{3};
    auto* foreachDryRun = new bool{false};

    foreach->add_flag("--native", *foreachNative, "Use native C++ wave executor (default)");
    foreach->add_flag("--shell", *foreachShell, "Deprecated compatibility flag (shell path removed)");
    foreach->add_flag("--native-plan", *foreachNativePlan, "Use native C++ planner + shell foreach adapter execution");
    foreach->add_flag("--native-plan-only", *foreachNativePlanOnly, "Emit native foreach plan JSON only");
    foreach->add_flag("--continue-on-error", *foreachContinueOnError, "Continue if command fails in a repo");
    foreach->add_option("--parallel", *foreachParallel, "Parallel execution per wave (default 1)");
    foreach->add_option("--native-max-depth", *foreachNativeMaxDepth, "Native discovery max depth");
    foreach->add_flag("--native-no-cache", *foreachNativeNoCache, "Disable native discovery cache");
    foreach->add_flag("--native-refresh-cache", *foreachNativeRefreshCache, "Force native cache refresh");
    foreach->add_flag("--native-no-incremental", *foreachNativeNoIncremental, "Disable native incremental cache validation");
    foreach->add_option("--native-cache-ttl", *foreachNativeCacheTtl, "Native cache TTL seconds");
    foreach->add_option("--native-max-stale", *foreachNativeMaxStale, "Native incremental max stale seconds");
    foreach->add_option("--native-metadata-level", *foreachNativeMetadata, "Native metadata level: full|minimal");
    foreach->add_option("--manifest", *foreachManifest, "Manifest file path");
    foreach->add_option("--include-types", *foreachIncludeTypes, "Comma-separated repo types");
    foreach->add_option("--exclude", *foreachExclude, "Temporary scan-scope exclude override for this invocation only (repeatable; prefer .gitignore/.kogignore for shared policy)");
    foreach->add_option("--max-depth", *foreachMaxDepth, "Discovery max depth");
    foreach->add_flag("--dry-run", *foreachDryRun, "Preview mode");
    foreach->add_option("--command", *foreachCommand, "Explicit command string for native foreach");
    foreach->add_option("cmd", *foreachPositionalCommand, "Positional command string");
    foreach->callback([=]() {
        if (*foreachNativePlanOnly) {
            *foreachNativePlan = true;
        }
        if (*foreachShell) {
            std::cerr << "Error: --shell is no longer supported; foreach is fully native now\n";
            std::exit(2);
        }
        if (!*foreachNative && !*foreachNativePlan) {
            *foreachNative = true;
        }
        if (foreachCommand->empty() && !foreachPositionalCommand->empty()) {
            *foreachCommand = *foreachPositionalCommand;
        }
        auto extras = foreach->remaining();
        if (foreachCommand->empty() && !extras.empty()) {
            *foreachCommand = extras.front();
            extras.erase(extras.begin());
        }
        if (!extras.empty()) {
            std::cerr << "Error: unsupported extra arguments in native foreach mode:";
            for (const auto& extra : extras) {
                std::cerr << ' ' << extra;
            }
            std::cerr << "\n";
            std::exit(2);
        }
        if (*foreachNativePlan && !*foreachNativePlanOnly) {
            *foreachNative = true;
            *foreachNativePlan = false;
        }

        if (*foreachNative || *foreachNativePlan) {
            if (foreachCommand->empty()) {
                std::cerr << "Error: command is required for native/native-plan foreach\n";
                std::exit(1);
            }
            if (*foreachParallel <= 0) {
                std::cerr << "Error: --parallel must be >= 1\n";
                std::exit(1);
            }

            workspace::DiscoverOptions options;
            options.rootDir = std::filesystem::current_path();
            options.maxDepth = *foreachNativeMaxDepth;
            options.excludePatterns = *foreachExclude;
            options.useCache = !*foreachNativeNoCache;
            options.cacheTtlSeconds = *foreachNativeCacheTtl;
            options.refreshCache = *foreachNativeRefreshCache;
            options.incremental = !*foreachNativeNoIncremental;
            options.maxStaleSeconds = *foreachNativeMaxStale;
            options.metadataLevel = *foreachNativeMetadata;

            const auto native = workspace::BuildNativeWorkspaceOutput(options, std::filesystem::current_path());
            if (native.hasCycle) {
                std::cerr << "Error: workspace dependency graph has cycle(s); cannot execute native foreach\n";
                std::cerr << native.wavesJson << "\n";
                std::exit(2);
            }

            std::vector<std::string> includeTypes;
            {
                std::stringstream ss(*foreachIncludeTypes);
                std::string item;
                while (std::getline(ss, item, ',')) {
                    if (!item.empty()) {
                        if (item == "submodule") {
                            item = "registered";
                        } else if (item == "standalone") {
                            item = "unregistered";
                        }
                        includeTypes.push_back(item);
                    }
                }
            }
            if (includeTypes.empty()) {
                includeTypes = {"root", "registered", "unregistered"};
            }

            const auto typeAllowed = [&includeTypes](const std::string& InType) {
                return std::find(includeTypes.begin(), includeTypes.end(), InType) != includeTypes.end();
            };

            const auto foreachOperations = workspace::BuildPlanOperations(
                native.discovery.repos,
                native.waves,
                "foreach",
                *foreachCommand,
                includeTypes);

            const auto foreachPlanJson = workspace::BuildPlanJson(
                "native-foreach",
                foreachOperations,
                native.wavesJson,
                "workspace/foreach-repo.sh",
                {},
                *foreachCommand);

            if (*foreachNativePlanOnly) {
                std::cout << foreachPlanJson << "\n";
                std::exit(0);
            }

            if (*foreachNativePlan) {
                std::cerr << "Error: native-plan adapter execution via shell is disabled in strict native-only mode\n";
                std::exit(2);
            }

            int successCount = 0;
            int failureCount = 0;

            for (const auto& wave : native.waves) {
                std::vector<workspace::RepoRecord> repos;
                repos.reserve(wave.size());
                for (const auto idx : wave) {
                    if (idx >= native.discovery.repos.size()) {
                        continue;
                    }
                    const auto& repo = native.discovery.repos[idx];
                    if (typeAllowed(repo.type)) {
                        repos.push_back(repo);
                    }
                }

                if (repos.empty()) {
                    continue;
                }

                std::vector<RepoExecResult> results;
                if (*foreachParallel <= 1 || repos.size() == 1) {
                    for (const auto& repo : repos) {
                        (void)foreachDryRun;
                        results.push_back(ExecuteRepoCommand(repo, *foreachCommand));
                    }
                } else {
                    std::vector<std::future<RepoExecResult>> futures;
                    futures.reserve(repos.size());
                    for (const auto& repo : repos) {
                        futures.push_back(std::async(std::launch::async, [repo, foreachCommand]() {
                            return ExecuteRepoCommand(repo, *foreachCommand);
                        }));
                    }
                    for (auto& fut : futures) {
                        results.push_back(fut.get());
                    }
                }

                for (const auto& result : results) {
                    PrintRepoExecResult(result);
                    if (result.exitCode == 0) {
                        successCount += 1;
                    } else {
                        failureCount += 1;
                        if (!*foreachContinueOnError) {
                            std::cerr << "Error: foreach failed, stopping (use --continue-on-error to continue)\n";
                            std::cout << "\nSummary: " << (successCount + failureCount) << " repos, "
                                      << successCount << " succeeded, " << failureCount << " failed\n";
                            std::exit(1);
                        }
                    }
                }
            }

            std::cout << "\nSummary: " << (successCount + failureCount) << " repos, "
                      << successCount << " succeeded, " << failureCount << " failed\n";
            std::exit(failureCount > 0 ? 1 : 0);
        }

        std::cerr << "Error: foreach must run in native mode\n";
        std::exit(2);
    });
}

} // namespace kano::git::commands
