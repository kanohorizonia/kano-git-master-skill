#include "native_workspace.hpp"

#include "scheduler.hpp"

#include <format>
#include <string>
#include <algorithm>

namespace kano::git::workspace {

namespace {

auto EscapeJson(std::string InValue) -> std::string {
    std::string out;
    out.reserve(InValue.size() + 8);
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

} // namespace

auto BuildPlanOperations(
    const std::vector<RepoRecord>& InRepos,
    const std::vector<std::vector<std::size_t>>& InWaves,
    std::string InAction,
    std::string InCommand,
    const std::vector<std::string>& InIncludeTypes) -> std::vector<PlanOperation> {
    const auto typeAllowed = [&InIncludeTypes](const std::string& InType) {
        if (InIncludeTypes.empty()) {
            return true;
        }
        return std::find(InIncludeTypes.begin(), InIncludeTypes.end(), InType) != InIncludeTypes.end();
    };

    std::vector<PlanOperation> ops;
    std::size_t order = 0;
    for (std::size_t waveIndex = 0; waveIndex < InWaves.size(); ++waveIndex) {
        for (const auto repoIndex : InWaves[waveIndex]) {
            if (repoIndex >= InRepos.size()) {
                continue;
            }
            const auto& repo = InRepos[repoIndex];
            if (!typeAllowed(repo.type)) {
                continue;
            }
            ops.push_back(PlanOperation{
                .order = order,
                .waveIndex = waveIndex,
                .path = repo.path,
                .type = repo.type,
                .action = InAction,
                .command = InCommand,
            });
            order += 1;
        }
    }
    return ops;
}

auto BuildPlanJson(
    std::string InPlanner,
    const std::vector<PlanOperation>& InOperations,
    std::string InWavesJson,
    std::string InShellScript,
    std::string InManifestJson,
    std::string InCommand) -> std::string {
    std::string json;
    json += "{";
    json += "\"planner\":\"" + EscapeJson(InPlanner) + "\",";
    json += "\"operations\":[";
    for (std::size_t i = 0; i < InOperations.size(); ++i) {
        if (i > 0) {
            json += ",";
        }
        const auto& op = InOperations[i];
        json += "{";
        json += std::format("\"order\":{},", op.order);
        json += std::format("\"wave\":{},", op.waveIndex);
        json += "\"path\":\"" + EscapeJson(op.path.lexically_normal().generic_string()) + "\",";
        json += "\"type\":\"" + EscapeJson(op.type) + "\",";
        json += "\"action\":\"" + EscapeJson(op.action) + "\"";
        if (!op.command.empty()) {
            json += ",\"command\":\"" + EscapeJson(op.command) + "\"";
        }
        json += "}";
    }
    json += "],";
    json += "\"waves\":";
    json += InWavesJson;

    json += ",\"shell_adapter\":{";
    json += "\"script\":\"" + EscapeJson(InShellScript) + "\"";
    if (!InManifestJson.empty()) {
        json += ",\"manifest\":";
        json += InManifestJson;
    }
    if (!InCommand.empty()) {
        json += ",\"command\":\"" + EscapeJson(InCommand) + "\"";
    }
    json += "}";
    json += "}";
    return json;
}

auto BuildNativeWorkspaceOutput(const DiscoverOptions& InOptions, const std::filesystem::path& InWorkspaceRoot) -> NativeWorkspaceOutput {
    NativeWorkspaceOutput out;
    out.discovery = DiscoverRepos(InOptions);

    const auto plan = BuildExecutionWaves(out.discovery.repos);
    out.waves = plan.waves;
    out.hasCycle = plan.hasCycle;
    out.orderedRepos = FlattenByWaves(out.discovery.repos, plan);

    out.reposJson = ReposToJson(out.discovery.repos);
    out.wavesJson = WavesToJson(out.discovery.repos, plan);
    out.orderedManifestJson = ManifestToJson(InWorkspaceRoot, out.orderedRepos);
    out.updateOperations = BuildPlanOperations(out.discovery.repos, out.waves, "update-repo");
    out.updatePlanJson = BuildPlanJson(
        "native-submodule-update",
        out.updateOperations,
        out.wavesJson,
        "workspace/update-workspace-repos.sh",
        out.orderedManifestJson);

    return out;
}

} // namespace kano::git::workspace
