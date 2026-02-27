#pragma once

#include "discovery.hpp"

#include <filesystem>
#include <cstddef>
#include <string>
#include <vector>

namespace kano::git::workspace {

struct PlanOperation {
    std::size_t order = 0;
    std::size_t waveIndex = 0;
    std::filesystem::path path;
    std::string type;
    std::string action;
    std::string command;
};

struct NativeWorkspaceOutput {
    DiscoveryResult discovery;
    std::vector<RepoRecord> orderedRepos;
    std::vector<std::vector<std::size_t>> waves;
    std::vector<PlanOperation> updateOperations;
    std::string reposJson;
    std::string wavesJson;
    std::string orderedManifestJson;
    std::string updatePlanJson;
    bool hasCycle = false;
};

auto BuildNativeWorkspaceOutput(const DiscoverOptions& InOptions, const std::filesystem::path& InWorkspaceRoot) -> NativeWorkspaceOutput;
auto BuildPlanOperations(
    const std::vector<RepoRecord>& InRepos,
    const std::vector<std::vector<std::size_t>>& InWaves,
    std::string InAction,
    std::string InCommand = {},
    const std::vector<std::string>& InIncludeTypes = {}) -> std::vector<PlanOperation>;
auto BuildPlanJson(
    std::string InPlanner,
    const std::vector<PlanOperation>& InOperations,
    std::string InWavesJson,
    std::string InShellScript,
    std::string InManifestJson = {},
    std::string InCommand = {}) -> std::string;

} // namespace kano::git::workspace
