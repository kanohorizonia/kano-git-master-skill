#pragma once

#include <functional>
#include <memory>
#include <string>

namespace CLI {
class App;
}

namespace kano::git::commands {

struct SyncCommandOptions {
    struct PreCommit {
        std::string repo{"."};
        std::string remote{"origin"};
        bool dryRun{false};
        int maxDepth{0};
        bool noCache{false};
        bool refreshCache{false};
        bool noRecursive{false};
        std::string branchMode{"default"};
        bool profile{false};
    } preCommit;

    struct OriginLatest {
        bool shell{false};
        std::string repo{"."};
        std::string remote{"origin"};
        bool dryRun{false};
        int maxDepth{0};
        bool noCache{false};
        bool refreshCache{false};
        bool noRecursive{false};
        bool noAutoStash{false};
        bool noAuthPreflight{false};
        bool cleanupStaleLocks{false};
        int jobs{1};
        std::string executionPolicy{"parallel"};
        bool profile{false};
    } originLatest;

    struct UpstreamForcePush {
        std::string repo{"."};
        bool dryRun{false};
        bool profile{false};
    } upstreamForcePush;

    struct StableDev {
        bool workspace{false};
        std::string reportFormat{"compact"};
        std::string repo{"."};
        bool dryRun{false};
        bool profile{false};
    } stableDev;

    struct Dev {
        std::string repo{"."};
        bool dryRun{false};
        int maxDepth{0};
        bool noCache{false};
        bool refreshCache{false};
        bool noRecursive{false};
        bool noAuthPreflight{false};
        bool cleanupStaleLocks{false};
        int jobs{1};
        std::string executionPolicy{"parallel"};
        bool profile{false};
    } dev;

    struct LauncherUpdateCheck {
        std::string repo{"."};
        std::string remote{"upstream"};
        bool autoSync{false};
        bool nonInteractive{false};
    } launcherUpdateCheck;

    struct Default {
        bool noRecursive{false};
        bool noAuthPreflight{false};
        bool cleanupStaleLocks{false};
        int jobs{1};
        std::string executionPolicy{"parallel"};
        bool profile{false};
    } defaultSync;
};

auto MakeSyncCommandOptions() -> std::shared_ptr<SyncCommandOptions>;

auto MakeSyncPreCommitCommandCallback(CLI::App& InCommand,
                                      const std::shared_ptr<SyncCommandOptions>& InOptions)
    -> std::function<void()>;
auto MakeSyncOriginLatestCommandCallback(CLI::App& InCommand,
                                         const std::shared_ptr<SyncCommandOptions>& InOptions)
    -> std::function<void()>;
auto MakeSyncUpstreamForcePushCommandCallback(CLI::App& InCommand,
                                              const std::shared_ptr<SyncCommandOptions>& InOptions)
    -> std::function<void()>;
auto MakeSyncStableDevCommandCallback(CLI::App& InCommand,
                                      const std::shared_ptr<SyncCommandOptions>& InOptions)
    -> std::function<void()>;
auto MakeSyncDevCommandCallback(CLI::App& InCommand,
                                const std::shared_ptr<SyncCommandOptions>& InOptions)
    -> std::function<void()>;
auto MakeSyncLauncherUpdateCheckCommandCallback(CLI::App& InCommand,
                                                const std::shared_ptr<SyncCommandOptions>& InOptions)
    -> std::function<void()>;
auto MakeDefaultSyncCommandCallback(CLI::App& InCommand,
                                    const std::shared_ptr<SyncCommandOptions>& InOptions)
    -> std::function<void()>;

} // namespace kano::git::commands
