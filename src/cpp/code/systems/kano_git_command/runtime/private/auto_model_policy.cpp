#include "auto_model_policy.hpp"
#include "kog_config.hpp"

#include <algorithm>
#include <cctype>

#include <toml++/toml.h>

namespace kano::git::commands::auto_model_policy {
namespace {

auto Trim(std::string InValue) -> std::string {
    while (!InValue.empty() &&
           (InValue.back() == '\n' || InValue.back() == '\r' || InValue.back() == ' ' || InValue.back() == '\t')) {
        InValue.pop_back();
    }
    std::size_t start = 0;
    while (start < InValue.size() && (InValue[start] == ' ' || InValue[start] == '\t')) {
        start += 1;
    }
    return InValue.substr(start);
}

auto ToLower(std::string InValue) -> std::string {
    std::transform(InValue.begin(), InValue.end(), InValue.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return InValue;
}

void NormalizePolicy(AutoModelPolicy* OutPolicy) {
    if (OutPolicy == nullptr) {
        return;
    }

    if (OutPolicy->models.empty()) {
        OutPolicy->models = {"gpt-5-mini", "claude-haiku-4.5", "gpt-5.4"};
    }
    if (OutPolicy->changeThresholds.empty()) {
        if (OutPolicy->models.size() > 1) {
            OutPolicy->changeThresholds = {5, 10};
        }
        return;
    }

    for (auto& value : OutPolicy->changeThresholds) {
        value = std::max(0, value);
    }
    std::sort(OutPolicy->changeThresholds.begin(), OutPolicy->changeThresholds.end());

    while (OutPolicy->models.size() < OutPolicy->changeThresholds.size() + 1) {
        OutPolicy->models.push_back(OutPolicy->models.back());
    }
    if (OutPolicy->models.size() > OutPolicy->changeThresholds.size() + 1) {
        OutPolicy->models.resize(OutPolicy->changeThresholds.size() + 1);
    }
}

void ApplyAutoModelPolicyConfig(const std::filesystem::path& InConfigPath,
                                const std::string& InProvider,
                                AutoModelPolicy* OutPolicy) {
    if (OutPolicy == nullptr || InConfigPath.empty()) {
        return;
    }

    std::error_code ec;
    if (!std::filesystem::exists(InConfigPath, ec)) {
        return;
    }

    try {
        auto parsed = toml::parse_file(InConfigPath.string());

        const toml::table* section = &parsed;
        if (const auto* aiModelPolicy = parsed["ai_model_policy"].as_table(); aiModelPolicy != nullptr) {
            section = aiModelPolicy;
        } else if (const auto* ai = parsed["ai"].as_table(); ai != nullptr) {
            if (const auto* model = (*ai)["model"].as_table(); model != nullptr) {
                if (const auto* autoTable = (*model)["auto"].as_table(); autoTable != nullptr) {
                    section = autoTable;
                }
            }
        }

        auto readInt = [&](const char* InKey, int* OutValue) {
            if (OutValue == nullptr) {
                return;
            }
            if (const auto value = (*section)[InKey].value<int64_t>(); value.has_value()) {
                *OutValue = std::max(0, static_cast<int>(*value));
            }
        };

        auto readString = [&](const char* InKey, std::string* OutValue) {
            if (OutValue == nullptr) {
                return;
            }
            if (const auto value = (*section)[InKey].value<std::string>(); value.has_value()) {
                const auto trimmed = Trim(*value);
                if (!trimmed.empty()) {
                    *OutValue = trimmed;
                }
            }
        };

        auto readIntArray = [&](const char* InKey) -> std::vector<int> {
            std::vector<int> out;
            const auto* arrayNode = (*section)[InKey].as_array();
            if (arrayNode == nullptr) {
                return out;
            }
            out.reserve(arrayNode->size());
            for (const auto& node : *arrayNode) {
                if (const auto value = node.value<int64_t>(); value.has_value()) {
                    out.push_back(std::max(0, static_cast<int>(*value)));
                }
            }
            return out;
        };

        auto readStringArray = [&](const char* InKey) -> std::vector<std::string> {
            std::vector<std::string> out;
            const auto* arrayNode = (*section)[InKey].as_array();
            if (arrayNode == nullptr) {
                return out;
            }
            out.reserve(arrayNode->size());
            for (const auto& node : *arrayNode) {
                if (const auto value = node.value<std::string>(); value.has_value()) {
                    const auto trimmed = Trim(*value);
                    if (!trimmed.empty()) {
                        out.push_back(trimmed);
                    }
                }
            }
            return out;
        };

        const auto thresholds = readIntArray("change_thresholds");
        if (!thresholds.empty()) {
            OutPolicy->changeThresholds = thresholds;
        }

        const auto providerLower = ToLower(Trim(InProvider));
        const auto models = readStringArray("models");
        if (!models.empty()) {
            std::vector<std::string> filtered;
            filtered.reserve(models.size());
            for (const auto& rawModelRef : models) {
                const auto pos = rawModelRef.find('/');
                if (pos == std::string::npos) {
                    continue;
                }

                const auto refProvider = ToLower(Trim(rawModelRef.substr(0, pos)));
                auto refModel = Trim(rawModelRef.substr(pos + 1));
                if (refProvider.empty() || refModel.empty()) {
                    continue;
                }
                if (!providerLower.empty() && refProvider == providerLower) {
                    filtered.push_back(std::move(refModel));
                }
            }

            if (!filtered.empty()) {
                OutPolicy->models = std::move(filtered);
            }
        }

        if (OutPolicy->changeThresholds.size() < 2) {
            int firstThreshold = OutPolicy->changeThresholds.empty() ? 5 : OutPolicy->changeThresholds.front();
            int secondThreshold = std::max(firstThreshold, 10);
            readInt("mini_max_changes", &firstThreshold);
            readInt("small_max_changes", &firstThreshold);
            readInt("haiku_max_changes", &secondThreshold);
            readInt("medium_max_changes", &secondThreshold);
            OutPolicy->changeThresholds = {firstThreshold, std::max(firstThreshold, secondThreshold)};
        }

        while (OutPolicy->models.size() < 3) {
            OutPolicy->models.push_back(OutPolicy->models.empty() ? std::string("gpt-5-mini") : OutPolicy->models.back());
        }

        readString("mini_model", &OutPolicy->models[0]);
        readString("small_model", &OutPolicy->models[0]);
        readString("haiku_model", &OutPolicy->models[1]);
        readString("medium_model", &OutPolicy->models[1]);
        readString("large_model", &OutPolicy->models[2]);

        NormalizePolicy(OutPolicy);
    } catch (const toml::parse_error&) {
    } catch (const std::exception&) {
    }
}

} // namespace

auto ResolveAutoModelPolicy(const std::string& InProvider,
                            const std::filesystem::path& InWorkspaceRoot,
                            const std::filesystem::path& InSkillRoot) -> AutoModelPolicy {
    AutoModelPolicy policy{};

    if (ToLower(Trim(InProvider)) != "copilot") {
        policy.changeThresholds = {5, 10};
        policy.models = {"gpt-5-mini", "gpt-5-mini", "gpt-5-mini"};
        return policy;
    }

    for (const auto& configPath : kog_config::ResolveConfigSearchPaths(InWorkspaceRoot, InSkillRoot)) {
        ApplyAutoModelPolicyConfig(configPath, InProvider, &policy);
    }

    NormalizePolicy(&policy);

    return policy;
}

auto ResolveModelForChangeCount(const AutoModelPolicy& InPolicy, int InChangeCount) -> std::string {
    const int changeCount = std::max(0, InChangeCount);
    if (InPolicy.models.empty()) {
        return {};
    }
    for (std::size_t i = 0; i < InPolicy.changeThresholds.size() && i < InPolicy.models.size(); ++i) {
        if (changeCount <= InPolicy.changeThresholds[i]) {
            return InPolicy.models[i];
        }
    }
    return InPolicy.models.back();
}

} // namespace kano::git::commands::auto_model_policy
