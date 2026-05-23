#include "kog_config.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <regex>
#include <set>
#include <sstream>

#include "shell_executor.hpp"
#include <toml++/toml.h>

namespace kano::git::commands::kog_config {
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

auto ExpandHomePath(const std::filesystem::path& InPath) -> std::filesystem::path {
    const auto raw = InPath.generic_string();
    if (raw == "~") {
        return HomeDirectory();
    }
    if (raw.rfind("~/", 0) == 0) {
        return (HomeDirectory() / raw.substr(2)).lexically_normal();
    }
    return InPath.lexically_normal();
}

auto ParseBoolValue(const std::string& InValue, bool* OutValue) -> bool {
    if (OutValue == nullptr) {
        return false;
    }
    const auto lowered = ToLower(Trim(InValue));
    if (lowered == "true" || lowered == "1" || lowered == "yes" || lowered == "on") {
        *OutValue = true;
        return true;
    }
    if (lowered == "false" || lowered == "0" || lowered == "no" || lowered == "off") {
        *OutValue = false;
        return true;
    }
    return false;
}

auto ParseStringArray(const toml::node& InNode) -> std::vector<std::string> {
    std::vector<std::string> out;
    const auto* array = InNode.as_array();
    if (array == nullptr) {
        return out;
    }
    out.reserve(array->size());
    for (const auto& item : *array) {
        if (const auto value = item.value<std::string>(); value.has_value()) {
            const auto trimmed = Trim(*value);
            if (!trimmed.empty()) {
                out.push_back(trimmed);
            }
        }
    }
    return out;
}

auto SplitLines(const std::string& InContent) -> std::vector<std::string> {
    std::vector<std::string> out;
    std::string line;
    for (char c : InContent) {
        if (c == '\n') {
            out.push_back(line);
            line.clear();
            continue;
        }
        if (c != '\r') {
            line.push_back(c);
        }
    }
    if (!line.empty()) {
        out.push_back(line);
    }
    return out;
}

auto LooksLikeModelToken(const std::string& InToken) -> bool {
    if (InToken.empty()) {
        return false;
    }
    for (char c : InToken) {
        const bool ok = (c >= 'a' && c <= 'z') ||
                        (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') ||
                        c == '-' || c == '_' || c == '.';
        if (!ok) {
            return false;
        }
    }
    return true;
}

auto IsLikelyAiModelName(const std::string& InToken) -> bool {
    if (!LooksLikeModelToken(InToken)) {
        return false;
    }

    const auto lower = ToLower(InToken);
    const bool knownPrefix =
        lower.find("gpt-") == 0 ||
        lower.find("claude-") == 0 ||
        lower.find("gemini-") == 0 ||
        lower.find("grok-") == 0 ||
        lower.find("o1") == 0 ||
        lower.find("o3") == 0;

    if (!knownPrefix) {
        return false;
    }

    return std::any_of(lower.begin(), lower.end(), [](char c) {
        return c >= '0' && c <= '9';
    });
}

auto ExtractModelsFromHelp(const std::string& InText) -> std::vector<std::string> {
    std::set<std::string> out;
    const std::regex modelPattern(R"(([A-Za-z0-9]+(?:[._-][A-Za-z0-9]+)+))");

    for (const auto& lineRaw : SplitLines(InText)) {
        auto line = Trim(lineRaw);
        if (line.empty()) continue;

        for (std::sregex_iterator it(line.begin(), line.end(), modelPattern), end; it != end; ++it) {
            const auto token = Trim((*it)[1].str());
            if (IsLikelyAiModelName(token)) {
                out.insert(token);
            }
        }
    }

    return std::vector<std::string>(out.begin(), out.end());
}

} // namespace

auto HomeDirectory() -> std::filesystem::path {
    if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0') {
        return std::filesystem::path(home).lexically_normal();
    }
#if defined(_WIN32)
    if (const char* userProfile = std::getenv("USERPROFILE"); userProfile != nullptr && *userProfile != '\0') {
        return std::filesystem::path(userProfile).lexically_normal();
    }
    const char* homeDrive = std::getenv("HOMEDRIVE");
    const char* homePath = std::getenv("HOMEPATH");
    if (homeDrive != nullptr && *homeDrive != '\0' && homePath != nullptr && *homePath != '\0') {
        return (std::filesystem::path(homeDrive) / homePath).lexically_normal();
    }
#endif
    return {};
}

auto ResolveConfigSearchPaths(const std::filesystem::path& InWorkspaceRoot,
                              const std::filesystem::path& InSkillRoot) -> std::vector<std::filesystem::path> {
    std::vector<std::filesystem::path> out;
    std::set<std::string> seen;
    auto tryAdd = [&](const std::filesystem::path& InPath) {
        if (InPath.empty()) {
            return;
        }
        const auto normalized = InPath.lexically_normal();
        const auto key = normalized.generic_string();
        if (key.empty() || seen.contains(key)) {
            return;
        }
        seen.insert(key);
        out.push_back(normalized);
    };

    if (!InSkillRoot.empty()) {
        tryAdd(InSkillRoot / ".kano" / "kog_config.toml");
    }
    const auto home = HomeDirectory();
    if (!home.empty()) {
        tryAdd(home / ".kano" / "kog_config.toml");
    }
    if (!InWorkspaceRoot.empty()) {
        tryAdd(InWorkspaceRoot / ".kano" / "kog_config.toml");
    }
    return out;
}

auto NormalizeAiModelSelection(const std::string& InValue) -> std::string {
    const auto lowered = ToLower(Trim(InValue));
    if (lowered == "default" || lowered == "provider-default") {
        return "provider-default";
    }
    if (lowered == "provider-auto") {
        return "provider-auto";
    }
    if (lowered == "kog-auto") {
        return "kog-auto";
    }
    if (lowered == "auto") {
        return "auto";
    }
    return Trim(InValue);
}

auto ProviderSupportsNativeAuto(const std::string& InProvider) -> bool {
    const auto provider = ToLower(Trim(InProvider));
    return provider == "copilot";
}

auto ResolveDefaultAiModelSelection(const std::string& InProvider,
                                    const std::filesystem::path& InWorkspaceRoot,
                                    const std::filesystem::path& InSkillRoot,
                                    const std::string& InFallback) -> std::string {
    auto resolved = NormalizeAiModelSelection(InFallback);
    if (resolved.empty()) {
        resolved = "auto";
    }

    const auto provider = ToLower(Trim(InProvider));
    for (const auto& configPath : ResolveConfigSearchPaths(InWorkspaceRoot, InSkillRoot)) {
        std::error_code ec;
        if (!std::filesystem::exists(configPath, ec) || ec) {
            continue;
        }

        try {
            const auto parsed = toml::parse_file(configPath.string());
            const toml::table* section = nullptr;
            if (const auto* ai = parsed["ai"].as_table(); ai != nullptr) {
                if (const auto* model = (*ai)["model"].as_table(); model != nullptr) {
                    section = model;
                }
            } else if (const auto* aiModel = parsed["ai_model"].as_table(); aiModel != nullptr) {
                section = aiModel;
            }
            if (section == nullptr) {
                continue;
            }

            for (const char* key : {"selection", "default_selection", "directive", "default_model"}) {
                if (const auto value = (*section)[key].value<std::string>(); value.has_value()) {
                    auto candidate = NormalizeAiModelSelection(*value);
                    if (candidate.empty()) {
                        continue;
                    }
                    const auto slash = candidate.find('/');
                    if (slash != std::string::npos) {
                        const auto refProvider = ToLower(Trim(candidate.substr(0, slash)));
                        auto refModel = Trim(candidate.substr(slash + 1));
                        if (!refProvider.empty() && !refModel.empty() && refProvider == provider) {
                            resolved = refModel;
                        }
                    } else {
                        resolved = candidate;
                    }
                }
            }
        } catch (const toml::parse_error&) {
        } catch (const std::exception&) {
        }
    }

    return resolved;
}

auto NormalizePlanCommitGenerationMode(const std::string& InValue) -> std::string {
    auto mode = ToLower(Trim(InValue));
    if (mode == "single") {
        return "single";
    }
    if (mode == "per-commit" || mode == "percommit" || mode == "per_commit") {
        return "per-commit";
    }
    if (mode == "adaptive" || mode == "adative" || mode == "hybrid") {
        return "adaptive";
    }
    return {};
}

auto GetKnownModelsForProvider(const std::string& InProvider) -> std::vector<std::string> {
    const auto provider = ToLower(Trim(InProvider));
    if (provider != "copilot") {
        return {}; // Non-copilot providers are experimental/hidden for now
    }

    // Try live fetching from copilot --help
    const auto cwd = std::filesystem::current_path();
    auto help = shell::ExecuteCommand("copilot", {"--help"}, shell::ExecMode::Capture, cwd);
    if (help.exitCode != 0) {
        help = shell::ExecuteCommand("gh", {"copilot", "--help"}, shell::ExecMode::Capture, cwd);
    }

    if (help.exitCode == 0) {
        auto liveModels = ExtractModelsFromHelp(help.stdoutStr + "\n" + help.stderrStr);
        if (!liveModels.empty()) {
            return liveModels;
        }
    }

    // Fallback static list
    return {
        "claude-haiku-4.5", "claude-opus-4.5", "claude-opus-4.6", "claude-opus-4.6-fast",
        "claude-sonnet-4", "claude-sonnet-4.5", "claude-sonnet-4.6", "gemini-3-pro-preview",
        "gpt-4.1", "gpt-5-mini", "gpt-5.1", "gpt-5.1-codex", "gpt-5.1-codex-max",
        "gpt-5.1-codex-mini", "gpt-5.2", "gpt-5.2-codex", "gpt-5.3-codex", "gpt-5.4"
    };
}

auto ResolvePlanCommitGenerationMode(const std::filesystem::path& InWorkspaceRoot,
                                     const std::filesystem::path& InSkillRoot,
                                     const std::string& InFallback) -> std::string {
    auto resolved = NormalizePlanCommitGenerationMode(InFallback);
    if (resolved.empty()) {
        resolved = "adaptive";
    }

    for (const auto& configPath : ResolveConfigSearchPaths(InWorkspaceRoot, InSkillRoot)) {
        std::error_code ec;
        if (!std::filesystem::exists(configPath, ec) || ec) {
            continue;
        }

        try {
            const auto parsed = toml::parse_file(configPath.string());
            const toml::table* section = nullptr;
            if (const auto* planAi = parsed["plan_ai"].as_table(); planAi != nullptr) {
                section = planAi;
            } else if (const auto* plan = parsed["plan"].as_table(); plan != nullptr) {
                if (const auto* ai = (*plan)["ai"].as_table(); ai != nullptr) {
                    section = ai;
                }
            }
            if (section == nullptr) {
                continue;
            }
            for (const char* key : {"commit_generation_mode", "generation_mode", "fill_mode"}) {
                if (const auto value = (*section)[key].value<std::string>(); value.has_value()) {
                    if (const auto normalized = NormalizePlanCommitGenerationMode(*value); !normalized.empty()) {
                        resolved = normalized;
                    }
                }
            }
        } catch (const toml::parse_error&) {
        } catch (const std::exception&) {
        }
    }

    return resolved;
}

// ---------------------------------------------------------------------------
// Layer path helpers
// ---------------------------------------------------------------------------

auto SystemConfigPath(const std::filesystem::path& InSkillRoot) -> std::filesystem::path {
    if (InSkillRoot.empty()) {
        return {};
    }
    return (InSkillRoot / ".kano" / "kog_config.toml").lexically_normal();
}

auto GlobalConfigPath() -> std::filesystem::path {
    const auto home = HomeDirectory();
    if (home.empty()) {
        return {};
    }
    return (home / ".kano" / "kog_config.toml").lexically_normal();
}

auto LocalConfigPath(const std::filesystem::path& InWorkspaceRoot) -> std::filesystem::path {
    if (InWorkspaceRoot.empty()) {
        return {};
    }
    return (InWorkspaceRoot / ".kano" / "kog_config.toml").lexically_normal();
}

// ---------------------------------------------------------------------------
// Generic TOML key operations
// ---------------------------------------------------------------------------

namespace {

auto SplitDottedKey(const std::string& InKey) -> std::vector<std::string> {
    std::vector<std::string> parts;
    std::string current;
    for (const char c : InKey) {
        if (c == '.') {
            if (!current.empty()) {
                parts.push_back(std::move(current));
                current.clear();
            }
        } else {
            current.push_back(c);
        }
    }
    if (!current.empty()) {
        parts.push_back(std::move(current));
    }
    return parts;
}

auto TomlNodeToString(const toml::node& InNode) -> std::string {
    if (const auto* str = InNode.as_string(); str != nullptr) {
        return std::string(str->get());
    }
    if (const auto* integer = InNode.as_integer(); integer != nullptr) {
        return std::to_string(integer->get());
    }
    if (const auto* fp = InNode.as_floating_point(); fp != nullptr) {
        std::ostringstream oss;
        oss << fp->get();
        return oss.str();
    }
    if (const auto* b = InNode.as_boolean(); b != nullptr) {
        return b->get() ? "true" : "false";
    }
    // For arrays/tables, use the default formatter
    std::ostringstream oss;
    oss << toml::default_formatter(InNode);
    return oss.str();
}

auto FlattenTable(const toml::table& InTable, const std::string& InPrefix,
                  std::vector<std::pair<std::string, std::string>>& OutEntries) -> void {
    for (const auto& [key, node] : InTable) {
        const auto fullKey = InPrefix.empty() ? std::string(key.str()) : InPrefix + "." + std::string(key.str());
        if (const auto* sub = node.as_table(); sub != nullptr) {
            FlattenTable(*sub, fullKey, OutEntries);
        } else {
            OutEntries.emplace_back(fullKey, TomlNodeToString(node));
        }
    }
}

} // namespace

auto ReadTomlValue(const std::filesystem::path& InConfigPath,
                   const std::string& InDottedKey) -> std::string {
    if (InConfigPath.empty() || InDottedKey.empty()) {
        return {};
    }
    std::error_code ec;
    if (!std::filesystem::exists(InConfigPath, ec) || ec) {
        return {};
    }
    try {
        const auto parsed = toml::parse_file(InConfigPath.string());
        const auto parts = SplitDottedKey(InDottedKey);
        if (parts.empty()) {
            return {};
        }
        const toml::table* current = &parsed;
        for (std::size_t i = 0; i + 1 < parts.size(); ++i) {
            const auto* next = (*current)[parts[i]].as_table();
            if (next == nullptr) {
                return {};
            }
            current = next;
        }
        const auto* leaf = current->get(parts.back());
        if (leaf == nullptr) {
            return {};
        }
        return TomlNodeToString(*leaf);
    } catch (const toml::parse_error&) {
    } catch (const std::exception&) {
    }
    return {};
}

auto ReadEffectiveValue(const std::filesystem::path& InWorkspaceRoot,
                        const std::filesystem::path& InSkillRoot,
                        const std::string& InDottedKey) -> std::string {
    std::string result;
    for (const auto& configPath : ResolveConfigSearchPaths(InWorkspaceRoot, InSkillRoot)) {
        const auto value = ReadTomlValue(configPath, InDottedKey);
        if (!value.empty()) {
            result = value;
        }
    }
    return result;
}

auto ReadEffectiveBool(const std::filesystem::path& InWorkspaceRoot,
                       const std::filesystem::path& InSkillRoot,
                       const std::string& InDottedKey,
                       bool InFallback) -> bool {
    auto result = InFallback;
    for (const auto& configPath : ResolveConfigSearchPaths(InWorkspaceRoot, InSkillRoot)) {
        const auto value = ReadTomlValue(configPath, InDottedKey);
        bool parsed = false;
        if (ParseBoolValue(value, &parsed)) {
            result = parsed;
        }
    }
    return result;
}

auto ResolveWorkspaceExternalRoots(const std::filesystem::path& InWorkspaceRoot,
                                   const std::filesystem::path& InSkillRoot) -> std::vector<std::filesystem::path> {
    std::vector<std::filesystem::path> mergedRoots;

    for (const auto& configPath : ResolveConfigSearchPaths(InWorkspaceRoot, InSkillRoot)) {
        std::error_code ec;
        if (!std::filesystem::exists(configPath, ec) || ec) {
            continue;
        }

        try {
            const auto parsed = toml::parse_file(configPath.string());
            const auto* workspace = parsed["workspace"].as_table();
            if (workspace == nullptr) {
                continue;
            }
            const auto* external = (*workspace)["external"].as_table();
            if (external == nullptr) {
                continue;
            }

            bool inherit = true;
            if (const auto inheritNode = (*external).get("inherit"); inheritNode != nullptr) {
                if (const auto inheritValue = inheritNode->value<bool>(); inheritValue.has_value()) {
                    inherit = *inheritValue;
                } else if (const auto inheritString = inheritNode->value<std::string>(); inheritString.has_value()) {
                    bool parsedInherit = true;
                    if (ParseBoolValue(*inheritString, &parsedInherit)) {
                        inherit = parsedInherit;
                    }
                }
            }

            if (!inherit) {
                mergedRoots.clear();
            }

            if (const auto* rootsNode = (*external).get("roots"); rootsNode != nullptr) {
                for (const auto& root : ParseStringArray(*rootsNode)) {
                    const auto expanded = ExpandHomePath(std::filesystem::path(root));
                    if (!expanded.empty()) {
                        mergedRoots.push_back(expanded.lexically_normal());
                    }
                }
            }
        } catch (const toml::parse_error&) {
        } catch (const std::exception&) {
        }
    }

    std::vector<std::filesystem::path> deduped;
    std::set<std::string> seen;
    deduped.reserve(mergedRoots.size());
    for (const auto& root : mergedRoots) {
        const auto normalized = ExpandHomePath(root).lexically_normal();
        const auto key = normalized.generic_string();
        if (key.empty() || seen.contains(key)) {
            continue;
        }
        seen.insert(key);
        deduped.push_back(normalized);
    }
    return deduped;
}

auto WriteTomlValue(const std::filesystem::path& InConfigPath,
                    const std::string& InDottedKey,
                    const std::string& InValue) -> bool {
    if (InConfigPath.empty() || InDottedKey.empty()) {
        return false;
    }
    try {
        // Create parent directories on demand
        if (InConfigPath.has_parent_path()) {
            std::error_code ec;
            std::filesystem::create_directories(InConfigPath.parent_path(), ec);
            if (ec) {
                return false;
            }
        }

        // Parse existing file or start with empty table
        toml::table root;
        {
            std::error_code ec;
            if (std::filesystem::exists(InConfigPath, ec) && !ec) {
                try {
                    root = toml::parse_file(InConfigPath.string());
                } catch (const toml::parse_error&) {
                    // If parsing fails, start fresh
                    root = toml::table{};
                }
            }
        }

        // Navigate/create nested tables for dotted key
        const auto parts = SplitDottedKey(InDottedKey);
        if (parts.empty()) {
            return false;
        }

        toml::table* current = &root;
        for (std::size_t i = 0; i + 1 < parts.size(); ++i) {
            auto* existing = (*current)[parts[i]].as_table();
            if (existing == nullptr) {
                current->insert_or_assign(parts[i], toml::table{});
                existing = (*current)[parts[i]].as_table();
            }
            current = existing;
        }

        // Set the leaf value as a string
        current->insert_or_assign(parts.back(), InValue);

        // Serialize back
        std::ofstream out(InConfigPath, std::ios::out | std::ios::trunc);
        if (!out) {
            return false;
        }
        out << root;
        out << "\n";
        return static_cast<bool>(out);
    } catch (const std::exception&) {
    }
    return false;
}

auto UnsetTomlKey(const std::filesystem::path& InConfigPath,
                  const std::string& InDottedKey) -> bool {
    if (InConfigPath.empty() || InDottedKey.empty()) {
        return false;
    }
    std::error_code ec;
    if (!std::filesystem::exists(InConfigPath, ec) || ec) {
        return true; // Nothing to unset
    }
    try {
        auto root = toml::parse_file(InConfigPath.string());
        const auto parts = SplitDottedKey(InDottedKey);
        if (parts.empty()) {
            return false;
        }

        toml::table* current = &root;
        for (std::size_t i = 0; i + 1 < parts.size(); ++i) {
            auto* next = (*current)[parts[i]].as_table();
            if (next == nullptr) {
                return true; // Key doesn't exist, nothing to unset
            }
            current = next;
        }

        current->erase(parts.back());

        // Serialize back
        std::ofstream out(InConfigPath, std::ios::out | std::ios::trunc);
        if (!out) {
            return false;
        }
        out << root;
        out << "\n";
        return static_cast<bool>(out);
    } catch (const toml::parse_error&) {
    } catch (const std::exception&) {
    }
    return false;
}

auto ListTomlKeys(const std::filesystem::path& InConfigPath)
    -> std::vector<std::pair<std::string, std::string>> {
    std::vector<std::pair<std::string, std::string>> entries;
    if (InConfigPath.empty()) {
        return entries;
    }
    std::error_code ec;
    if (!std::filesystem::exists(InConfigPath, ec) || ec) {
        return entries;
    }
    try {
        const auto parsed = toml::parse_file(InConfigPath.string());
        FlattenTable(parsed, "", entries);
    } catch (const toml::parse_error&) {
    } catch (const std::exception&) {
    }
    return entries;
}

} // namespace kano::git::commands::kog_config
