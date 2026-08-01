#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace kano::git::commands {

struct RebasePreflightState {
    bool active = false;
    std::filesystem::path repo;
    std::string branch;
    std::string upstream;
    std::string tracking;
    std::string mergeBase;
    std::vector<std::string> candidates;
    std::string risk;
    std::string note;
};

struct RebasePlanItem {
    std::string sha;
    std::string title;
    std::string action = "pick";  // pick|squash|fixup|drop
};

struct RebasePlannerState {
    bool active = false;
    std::filesystem::path repo;
    std::string baseRef;
    std::vector<RebasePlanItem> items;
    int selectedIndex = 0;
    std::string preview;
};

struct RebaseRunnerState {
    bool active = false;
    std::filesystem::path repo;
    std::vector<RebasePlanItem> queue;
    int index = 0;
    bool waitingConflictResolution = false;
    std::string lastOutput;
};

[[nodiscard]] auto BuildRebasePlanPreview(
    const RebasePlannerState& InPlanner) -> std::string;

[[nodiscard]] auto BuildRebasePlanner(
    const std::filesystem::path& InRepo,
    const RebasePreflightState& InPreflight) -> RebasePlannerState;

}  // namespace kano::git::commands
