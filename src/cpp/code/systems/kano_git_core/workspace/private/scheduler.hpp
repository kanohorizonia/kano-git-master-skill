#pragma once

#include "discovery.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace kano::git::workspace {

struct WavePlan {
    std::vector<std::vector<std::size_t>> waves;
    std::vector<std::size_t> cycleNodes;
    bool hasCycle = false;
};

auto BuildExecutionWaves(const std::vector<RepoRecord>& InRepos) -> WavePlan;
auto FlattenByWaves(const std::vector<RepoRecord>& InRepos, const WavePlan& InPlan) -> std::vector<RepoRecord>;
auto WavesToJson(const std::vector<RepoRecord>& InRepos, const WavePlan& InPlan) -> std::string;

} // namespace kano::git::workspace
