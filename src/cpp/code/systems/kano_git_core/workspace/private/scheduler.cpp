#include "scheduler.hpp"

#include <algorithm>
#include <format>
#include <set>
#include <string>
#include <unordered_map>

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

auto RepoPathKey(const RepoRecord& InRepo) -> std::string {
    return InRepo.path.lexically_normal().generic_string();
}

} // namespace

auto BuildExecutionWaves(const std::vector<RepoRecord>& InRepos) -> WavePlan {
    WavePlan plan;
    if (InRepos.empty()) {
        return plan;
    }

    std::unordered_map<std::string, std::size_t> byPath;
    byPath.reserve(InRepos.size());
    for (std::size_t idx = 0; idx < InRepos.size(); ++idx) {
        byPath.emplace(RepoPathKey(InRepos[idx]), idx);
    }

    std::vector<std::vector<std::size_t>> outgoing(InRepos.size());
    std::vector<std::size_t> indegree(InRepos.size(), 0);

    for (std::size_t idx = 0; idx < InRepos.size(); ++idx) {
        std::set<std::size_t> uniqueDeps;
        for (const auto& dep : InRepos[idx].dependencies) {
            const auto it = byPath.find(dep.lexically_normal().generic_string());
            if (it == byPath.end()) {
                continue;
            }
            const std::size_t depIdx = it->second;
            if (depIdx == idx) {
                continue;
            }
            if (uniqueDeps.insert(depIdx).second) {
                outgoing[depIdx].push_back(idx);
                indegree[idx] += 1;
            }
        }
    }

    std::vector<std::size_t> ready;
    ready.reserve(InRepos.size());
    for (std::size_t idx = 0; idx < InRepos.size(); ++idx) {
        if (indegree[idx] == 0) {
            ready.push_back(idx);
        }
    }

    auto byRepoPath = [&InRepos](const std::size_t A, const std::size_t B) {
        return RepoPathKey(InRepos[A]) < RepoPathKey(InRepos[B]);
    };
    std::sort(ready.begin(), ready.end(), byRepoPath);

    std::size_t processedCount = 0;
    while (!ready.empty()) {
        std::vector<std::size_t> wave = ready;
        plan.waves.push_back(wave);
        processedCount += wave.size();

        std::vector<std::size_t> next;
        for (const auto nodeIdx : wave) {
            for (const auto outIdx : outgoing[nodeIdx]) {
                if (indegree[outIdx] == 0) {
                    continue;
                }
                indegree[outIdx] -= 1;
                if (indegree[outIdx] == 0) {
                    next.push_back(outIdx);
                }
            }
        }

        std::sort(next.begin(), next.end(), byRepoPath);
        next.erase(std::unique(next.begin(), next.end()), next.end());
        ready = std::move(next);
    }

    if (processedCount != InRepos.size()) {
        plan.hasCycle = true;
        for (std::size_t idx = 0; idx < InRepos.size(); ++idx) {
            if (indegree[idx] > 0) {
                plan.cycleNodes.push_back(idx);
            }
        }
        std::sort(plan.cycleNodes.begin(), plan.cycleNodes.end(), byRepoPath);
    }

    return plan;
}

auto FlattenByWaves(const std::vector<RepoRecord>& InRepos, const WavePlan& InPlan) -> std::vector<RepoRecord> {
    std::vector<RepoRecord> ordered;
    for (const auto& wave : InPlan.waves) {
        for (const auto idx : wave) {
            if (idx < InRepos.size()) {
                ordered.push_back(InRepos[idx]);
            }
        }
    }
    return ordered;
}

auto WavesToJson(const std::vector<RepoRecord>& InRepos, const WavePlan& InPlan) -> std::string {
    std::string json;
    json += "{";
    json += std::format("\"has_cycle\":{},", InPlan.hasCycle ? "true" : "false");
    json += "\"cycle_nodes\":[";
    for (std::size_t i = 0; i < InPlan.cycleNodes.size(); ++i) {
        if (i > 0) {
            json += ",";
        }
        const auto idx = InPlan.cycleNodes[i];
        const auto value = idx < InRepos.size() ? RepoPathKey(InRepos[idx]) : std::format("invalid-index-{}", idx);
        json += std::format("\"{}\"", EscapeJson(value));
    }
    json += "],";
    json += "\"waves\":[";
    for (std::size_t waveIdx = 0; waveIdx < InPlan.waves.size(); ++waveIdx) {
        if (waveIdx > 0) {
            json += ",";
        }
        json += "[";
        const auto& wave = InPlan.waves[waveIdx];
        for (std::size_t i = 0; i < wave.size(); ++i) {
            if (i > 0) {
                json += ",";
            }
            const auto idx = wave[i];
            const auto value = idx < InRepos.size() ? RepoPathKey(InRepos[idx]) : std::format("invalid-index-{}", idx);
            json += std::format("\"{}\"", EscapeJson(value));
        }
        json += "]";
    }
    json += "]}";
    return json;
}

} // namespace kano::git::workspace
