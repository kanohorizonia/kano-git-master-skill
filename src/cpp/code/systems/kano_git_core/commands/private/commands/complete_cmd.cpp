// __complete command - internal completion backend

#include "command_registry.hpp"
#include "shell_executor.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <regex>
#include <set>
#include <string>
#include <vector>

namespace kano::git::commands {
namespace {

bool StartsWith(const std::string& InValue, const std::string& InPrefix) {
    return InValue.rfind(InPrefix, 0) == 0;
}

auto Trim(std::string InValue) -> std::string {
    while (!InValue.empty() && (InValue.back() == '\n' || InValue.back() == '\r' || InValue.back() == ' ' || InValue.back() == '\t')) {
        InValue.pop_back();
    }
    std::size_t start = 0;
    while (start < InValue.size() && (InValue[start] == ' ' || InValue[start] == '\t')) {
        start += 1;
    }
    return InValue.substr(start);
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

auto ToLowerCopy(std::string InValue) -> std::string {
    std::transform(InValue.begin(), InValue.end(), InValue.begin(), [](unsigned char c) {
        if (c >= 'A' && c <= 'Z') {
            return static_cast<char>(c - 'A' + 'a');
        }
        return static_cast<char>(c);
    });
    return InValue;
}

auto IsLikelyAiModelName(const std::string& InToken) -> bool {
    if (!LooksLikeModelToken(InToken)) {
        return false;
    }

    const auto lower = ToLowerCopy(InToken);
    const bool knownPrefix =
        StartsWith(lower, "gpt-") ||
        StartsWith(lower, "claude-") ||
        StartsWith(lower, "gemini-") ||
        StartsWith(lower, "grok-") ||
        StartsWith(lower, "o1") ||
        StartsWith(lower, "o3") ||
        StartsWith(lower, "o4") ||
        StartsWith(lower, "o5");

    if (!knownPrefix) {
        return false;
    }

    return std::any_of(lower.begin(), lower.end(), [](char c) {
        return c >= '0' && c <= '9';
    });
}

auto CommandSucceeds(const std::string& InCommand, const std::vector<std::string>& InArgs = {}) -> bool {
    const auto result = shell::ExecuteCommand(InCommand, InArgs, shell::ExecMode::Capture, std::filesystem::current_path());
    return result.exitCode == 0;
}

auto DetectAvailableProviders() -> std::vector<std::string> {
    std::vector<std::string> providers;
    if (CommandSucceeds("opencode", {"--help"})) {
        providers.emplace_back("opencode");
    }
    if (CommandSucceeds("codex", {"--help"})) {
        providers.emplace_back("codex");
    }
    if (CommandSucceeds("copilot", {"--help"}) || CommandSucceeds("gh", {"copilot", "--version"})) {
        providers.emplace_back("copilot");
    }

    if (providers.empty()) {
        providers = {"opencode", "codex", "copilot"};
    }

    std::sort(providers.begin(), providers.end());
    providers.erase(std::unique(providers.begin(), providers.end()), providers.end());
    return providers;
}

auto ReadCachedModels(const std::string& InProvider) -> std::vector<std::string> {
    std::vector<std::string> out;
    const char* home = std::getenv("HOME");
    if (home == nullptr || std::string(home).empty()) {
        return out;
    }

    const auto cacheFile = std::filesystem::path(home) / ".cache" / "kano-git-master-skill" / "models" / (InProvider + ".txt");
    if (!std::filesystem::exists(cacheFile)) {
        return out;
    }

    std::ifstream in(cacheFile);
    if (!in) {
        return out;
    }

    std::string line;
    while (std::getline(in, line)) {
        line = Trim(line);
        if (!IsLikelyAiModelName(line)) {
            continue;
        }
        out.push_back(line);
    }

    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

auto ExtractModelTokensFromText(const std::string& InText) -> std::vector<std::string> {
    std::set<std::string> out;
    const std::regex modelPattern(R"(([A-Za-z0-9]+(?:[._-][A-Za-z0-9]+)+))");

    for (const auto& lineRaw : SplitLines(InText)) {
        auto line = Trim(lineRaw);
        if (line.empty() || line.rfind("===", 0) == 0) {
            continue;
        }

        const auto firstSpace = line.find(' ');
        if (firstSpace != std::string::npos) {
            const auto firstToken = line.substr(0, firstSpace);
            if (IsLikelyAiModelName(firstToken)) {
                out.insert(firstToken);
            }
        }

        for (std::sregex_iterator it(line.begin(), line.end(), modelPattern), end; it != end; ++it) {
            const auto token = Trim((*it)[1].str());
            if (IsLikelyAiModelName(token)) {
                out.insert(token);
            }
        }
    }

    return std::vector<std::string>(out.begin(), out.end());
}

auto FetchProviderModelsLive(const std::string& InProvider) -> std::vector<std::string> {
    if (InProvider == "opencode") {
        const auto result = shell::ExecuteCommand("opencode", {"models"}, shell::ExecMode::Capture, std::filesystem::current_path());
        if (result.exitCode == 0) {
            return ExtractModelTokensFromText(result.stdoutStr + "\n" + result.stderrStr);
        }
        return {};
    }

    if (InProvider == "codex") {
        const auto result = shell::ExecuteCommand("codex", {"models"}, shell::ExecMode::Capture, std::filesystem::current_path());
        if (result.exitCode == 0) {
            return ExtractModelTokensFromText(result.stdoutStr + "\n" + result.stderrStr);
        }
        return {};
    }

    if (InProvider == "copilot") {
        if (CommandSucceeds("copilot", {"--help"})) {
            const auto help = shell::ExecuteCommand("copilot", {"--help"}, shell::ExecMode::Capture, std::filesystem::current_path());
            if (help.exitCode == 0) {
                auto out = ExtractModelTokensFromText(help.stdoutStr + "\n" + help.stderrStr);
                if (!out.empty()) {
                    return out;
                }
            }
        }
        if (CommandSucceeds("gh", {"copilot", "--help"})) {
            const auto help = shell::ExecuteCommand("gh", {"copilot", "--help"}, shell::ExecMode::Capture, std::filesystem::current_path());
            if (help.exitCode == 0) {
                auto out = ExtractModelTokensFromText(help.stdoutStr + "\n" + help.stderrStr);
                if (!out.empty()) {
                    return out;
                }
            }
        }
        return {
            "gpt-5.3-codex",
            "gpt-5.1-codex",
            "gpt-5-mini",
            "claude-4.5-sonnet",
            "gemini-3-pro"
        };
    }

    return {};
}

auto GetModelsForProvider(const std::string& InProvider) -> std::vector<std::string> {
    if (InProvider.empty() || InProvider == "auto") {
        std::set<std::string> merged{"auto"};
        for (const auto& provider : DetectAvailableProviders()) {
            for (const auto& model : GetModelsForProvider(provider)) {
                merged.insert(model);
            }
        }
        return std::vector<std::string>(merged.begin(), merged.end());
    }

    std::set<std::string> models;
    models.insert("auto");

    for (const auto& model : ReadCachedModels(InProvider)) {
        models.insert(model);
    }
    if (models.size() <= 1) {
        for (const auto& model : FetchProviderModelsLive(InProvider)) {
            models.insert(model);
        }
    }

    return std::vector<std::string>(models.begin(), models.end());
}

auto ExtractOptionValue(const std::vector<std::string>& InTokens, const std::vector<std::string>& InOptionNames) -> std::string {
    std::string value;
    for (std::size_t i = 0; i < InTokens.size(); ++i) {
        const auto& token = InTokens[i];
        for (const auto& optionName : InOptionNames) {
            if (token == optionName) {
                if (i + 1 < InTokens.size() && !InTokens[i + 1].empty() && InTokens[i + 1][0] != '-') {
                    value = InTokens[i + 1];
                }
                continue;
            }

            const auto eqPrefix = optionName + "=";
            if (StartsWith(token, eqPrefix)) {
                value = token.substr(eqPrefix.size());
            }
        }
    }
    return value;
}

auto DetectPendingOptionValue(const std::vector<std::string>& InContextTokens) -> std::optional<std::string> {
    if (InContextTokens.empty()) {
        return std::nullopt;
    }

    const std::string& last = InContextTokens.back();
    if (last == "--ai-provider") {
        return std::string("provider");
    }
    if (last == "--ai-model") {
        return std::string("model");
    }
    return std::nullopt;
}

void AddMatchingWords(const std::vector<std::string>& InWords,
                      const std::string& InPrefix,
                      std::set<std::string>& InOut) {
    for (const auto& word : InWords) {
        if (InPrefix.empty() || StartsWith(word, InPrefix)) {
            InOut.insert(word);
        }
    }
}

const CLI::App* FindExactSubcommand(const CLI::App* InContext, const std::string& InToken) {
    const auto subs = InContext->get_subcommands([](const CLI::App* sub) {
        return sub != nullptr && !sub->get_name().empty();
    });

    for (const CLI::App* sub : subs) {
        if (sub->get_name() == InToken) {
            return sub;
        }
    }
    return nullptr;
}

void AddMatchingSubcommands(const CLI::App* InContext,
                            const std::string& InPrefix,
                            std::set<std::string>& InOut) {
    const auto subs = InContext->get_subcommands([](const CLI::App* sub) {
        return sub != nullptr && !sub->get_name().empty();
    });

    for (const CLI::App* sub : subs) {
        const std::string& name = sub->get_name();
        if (InPrefix.empty() || StartsWith(name, InPrefix)) {
            InOut.insert(name);
        }
    }
}

void AddMatchingOptions(const CLI::App* InContext,
                        const std::string& InPrefix,
                        std::set<std::string>& InOut) {
    const auto opts = InContext->get_options([](const CLI::Option* opt) {
        return opt != nullptr && opt->nonpositional();
    });

    for (const CLI::Option* opt : opts) {
        for (const std::string& lname : opt->get_lnames()) {
            const std::string full = "--" + lname;
            if (InPrefix.empty() || StartsWith(full, InPrefix)) {
                InOut.insert(full);
            }
        }

        for (const std::string& sname : opt->get_snames()) {
            const std::string full = "-" + sname;
            if (InPrefix.empty() || StartsWith(full, InPrefix)) {
                InOut.insert(full);
            }
        }
    }
}

std::vector<std::string> CompleteFromTokens(const CLI::App& InRoot,
                                            const std::vector<std::string>& InTokens) {
    std::vector<std::string> contextTokens;
    std::string partial;

    if (!InTokens.empty()) {
        contextTokens.assign(InTokens.begin(), InTokens.end() - 1);
        partial = InTokens.back();
    }

    const CLI::App* context = &InRoot;
    for (const std::string& token : contextTokens) {
        if (token.empty() || token[0] == '-') {
            break;
        }

        const CLI::App* next = FindExactSubcommand(context, token);
        if (next == nullptr) {
            break;
        }
        context = next;
    }

    std::set<std::string> candidates;

    if (context == &InRoot) {
        if (partial.empty() || partial[0] != '-') {
            AddMatchingWords({"build", "rebuild"}, partial, candidates);
        }
    }

    if (context->get_name() == "commit" || context->get_name() == "amend") {
        const auto pending = DetectPendingOptionValue(contextTokens);
        if (pending.has_value()) {
            if (*pending == "provider") {
                AddMatchingWords(DetectAvailableProviders(), partial, candidates);
                candidates.insert("auto");
                return std::vector<std::string>(candidates.begin(), candidates.end());
            }

            if (*pending == "model") {
                const auto provider = ExtractOptionValue(contextTokens, {"--ai-provider"});
                AddMatchingWords(GetModelsForProvider(provider), partial, candidates);
                return std::vector<std::string>(candidates.begin(), candidates.end());
            }
        }

        if (partial.empty() || partial[0] == '-') {
            AddMatchingWords({"-ai"}, partial, candidates);
        }
    }

    if (!partial.empty() && partial[0] == '-') {
        AddMatchingOptions(context, partial, candidates);
    } else {
        AddMatchingSubcommands(context, partial, candidates);
        AddMatchingOptions(context, partial, candidates);
    }

    return std::vector<std::string>(candidates.begin(), candidates.end());
}

} // namespace

void RegisterComplete(CLI::App& InApp) {
    auto* cmd = InApp.add_subcommand("__complete", "Internal completion backend");
    cmd->group("");

    auto* context = new std::vector<std::string>{};
    auto* current = new std::string{};

    cmd->add_option("--context", *context, "Completion context token (repeatable)");
    cmd->add_option("--current", *current, "Current partial token")->default_str("");

    cmd->callback([&InApp, context, current]() {
        std::vector<std::string> tokens = *context;
        tokens.push_back(*current);
        const auto candidates = CompleteFromTokens(InApp, tokens);

        for (const std::string& candidate : candidates) {
            std::cout << candidate << '\n';
        }
    });
}

} // namespace kano::git::commands
