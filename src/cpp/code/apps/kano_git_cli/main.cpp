// kano-git — Kano Git Master CLI
// AI-powered Git CLI tools
//
// SPDX-License-Identifier: MIT

#include <CLI/CLI.hpp>
#include "command_registry.hpp"
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iostream>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#endif

#include "build_info.hpp"
#include <kano_timing.h>

using namespace kano::git;

namespace {

auto ToLower(std::string InValue) -> std::string {
    std::transform(InValue.begin(), InValue.end(), InValue.begin(), [](unsigned char InChar) {
        return static_cast<char>(std::tolower(InChar));
    });
    return InValue;
}

auto LooksSensitiveCommandKey(const std::string& InValue) -> bool {
    const auto value = ToLower(InValue);
    return value.find("token") != std::string::npos ||
           value.find("secret") != std::string::npos ||
           value.find("password") != std::string::npos ||
           value.find("authorization") != std::string::npos ||
           value.find("bearer") != std::string::npos ||
           value.find("api-key") != std::string::npos ||
           value.find("apikey") != std::string::npos ||
           value.find("access_token") != std::string::npos ||
           value.find("refresh_token") != std::string::npos;
}

auto RedactUrlCredentialsForLog(std::string InValue) -> std::string {
    const auto lowered = ToLower(InValue);
    const auto schemePos = lowered.find("://");
    if (schemePos == std::string::npos) {
        return InValue;
    }
    const auto authorityStart = schemePos + 3;
    const auto pathPos = InValue.find('/', authorityStart);
    const auto authorityEnd = pathPos == std::string::npos ? InValue.size() : pathPos;
    const auto atPos = InValue.find('@', authorityStart);
    if (atPos == std::string::npos || atPos >= authorityEnd) {
        return InValue;
    }
    InValue.replace(authorityStart, atPos - authorityStart, "<redacted>");
    return InValue;
}

auto RedactCommandArgForLog(const std::string& InArg, bool& InOutRedactNext, bool& InOutRedactUrlNext) -> std::string {
    if (InOutRedactNext) {
        InOutRedactNext = false;
        return "<redacted>";
    }
    if (InOutRedactUrlNext) {
        InOutRedactUrlNext = false;
        return RedactUrlCredentialsForLog(InArg);
    }

    const auto lower = ToLower(InArg);
    const auto eqPos = InArg.find('=');
    if (eqPos != std::string::npos) {
        const auto key = InArg.substr(0, eqPos);
        const auto value = InArg.substr(eqPos + 1);
        if (ToLower(key) == "--url") {
            return key + "=" + RedactUrlCredentialsForLog(value);
        }
        if (LooksSensitiveCommandKey(key)) {
            return key + "=<redacted>";
        }
        return RedactUrlCredentialsForLog(InArg);
    }

    if (lower == "--url") {
        InOutRedactUrlNext = true;
        return InArg;
    }
    if (lower == "--token" ||
        lower == "--api-key" ||
        lower == "--password" ||
        lower == "--authorization" ||
        (!InArg.empty() && InArg[0] == '-' && LooksSensitiveCommandKey(InArg))) {
        InOutRedactNext = true;
        return InArg;
    }
    return RedactUrlCredentialsForLog(InArg);
}

auto CollectTopLevelCommandNames(const CLI::App& InApp) -> std::vector<std::string> {
    const auto subcommands = InApp.get_subcommands([](const CLI::App*) {
        return true;
    });

    std::vector<std::string> out;
    out.reserve(subcommands.size());
    static const std::vector<std::string> kHiddenInternal = {
        "complete",
    };

    for (const auto* subcommand : subcommands) {
        const auto name = subcommand->get_name();
        if (name.empty()) {
            continue;
        }
        if (std::find(kHiddenInternal.begin(), kHiddenInternal.end(), name) != kHiddenInternal.end()) {
            continue;
        }
        out.push_back(name);
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

auto IsKnownTopLevelCommand(const std::string& InCommand,
                            const std::vector<std::string>& InTopLevelCommands) -> bool {
    return std::find(InTopLevelCommands.begin(), InTopLevelCommands.end(), InCommand) != InTopLevelCommands.end();
}

auto ComputeEditDistance(const std::string& InLhs, const std::string& InRhs) -> int {
    const int lhsLength = static_cast<int>(InLhs.size());
    const int rhsLength = static_cast<int>(InRhs.size());
    std::vector<std::vector<int>> dp(lhsLength + 1, std::vector<int>(rhsLength + 1, 0));

    for (int i = 0; i <= lhsLength; ++i) {
        dp[i][0] = i;
    }
    for (int j = 0; j <= rhsLength; ++j) {
        dp[0][j] = j;
    }

    for (int i = 1; i <= lhsLength; ++i) {
        for (int j = 1; j <= rhsLength; ++j) {
            const int cost = (InLhs[i - 1] == InRhs[j - 1]) ? 0 : 1;
            dp[i][j] = std::min({
                dp[i - 1][j] + 1,
                dp[i][j - 1] + 1,
                dp[i - 1][j - 1] + cost,
            });
            if (i > 1 && j > 1 && InLhs[i - 1] == InRhs[j - 2] && InLhs[i - 2] == InRhs[j - 1]) {
                dp[i][j] = std::min(dp[i][j], dp[i - 2][j - 2] + cost);
            }
        }
    }

    return dp[lhsLength][rhsLength];
}

auto SuggestSimilarCommands(const std::string& InInput,
                            const std::vector<std::string>& InTopLevelCommands) -> std::vector<std::string> {
    struct Candidate {
        std::string canonical;
        int distance = 0;
    };

    static const std::vector<std::pair<std::string, std::string>> kLegacyAliases = {
        {"pi", "plan"},
        {"pia", "plan"},
        {"pv", "plan"},
        {"cp", "commit-push"},
        {"cpa", "commit-push"},
    };

    const auto inputLower = ToLower(InInput);

    std::vector<std::pair<std::string, std::string>> matchCandidates;
    matchCandidates.reserve(InTopLevelCommands.size() + kLegacyAliases.size());
    for (const auto& command : InTopLevelCommands) {
        matchCandidates.emplace_back(command, command);
    }
    for (const auto& [alias, canonical] : kLegacyAliases) {
        if (IsKnownTopLevelCommand(canonical, InTopLevelCommands)) {
            matchCandidates.emplace_back(alias, canonical);
        }
    }

    std::map<std::string, int> bestByCanonical;
    for (const auto& [spelling, canonical] : matchCandidates) {
        const auto spellingLower = ToLower(spelling);
        const int distance = ComputeEditDistance(inputLower, spellingLower);
        const int maxLen = std::max(static_cast<int>(inputLower.size()), static_cast<int>(spellingLower.size()));
        const int threshold = std::min(3, std::max(1, maxLen / 3));
        if (distance > threshold) {
            continue;
        }
        const auto found = bestByCanonical.find(canonical);
        if (found == bestByCanonical.end() || distance < found->second) {
            bestByCanonical[canonical] = distance;
        }
    }

    std::vector<Candidate> ranked;
    ranked.reserve(bestByCanonical.size());
    for (const auto& [canonical, distance] : bestByCanonical) {
        ranked.push_back(Candidate{canonical, distance});
    }

    std::sort(ranked.begin(), ranked.end(), [](const Candidate& InLhs, const Candidate& InRhs) {
        if (InLhs.distance != InRhs.distance) {
            return InLhs.distance < InRhs.distance;
        }
        return InLhs.canonical < InRhs.canonical;
    });

    std::vector<std::string> out;
    for (const auto& candidate : ranked) {
        out.push_back(candidate.canonical);
        if (out.size() >= 3) {
            break;
        }
    }
    return out;
}

void PrintUnknownCommandError(const std::string& InCommand,
                              const std::vector<std::string>& InSuggestions) {
    std::cerr << "kog: '" << InCommand << "' is not a kog command. See 'kog --help'.\n";

    if (InSuggestions.empty()) {
        return;
    }

    if (InSuggestions.size() == 1) {
        std::cerr << "\nThe most similar command is\n\n";
    } else {
        std::cerr << "\nThe most similar commands are\n\n";
    }

    for (const auto& suggestion : InSuggestions) {
        std::cerr << "        " << suggestion << "\n";
    }
}

int DiscoverDefaultMaxDepth() {
    const char* envValue = std::getenv("KOG_DISCOVER_MAX_DEPTH");
    if (envValue == nullptr) {
        return 8;
    }
    try {
        const int parsed = std::stoi(std::string{envValue});
        return parsed > 0 ? parsed : 8;
    } catch (...) {
        return 8;
    }
}

bool IsPositiveInt32(const std::string& InValue) {
    constexpr const char* kInt32Max = "2147483647";
    if (InValue.empty()) {
        return false;
    }
    for (const char ch : InValue) {
        if (!std::isdigit(static_cast<unsigned char>(ch))) {
            return false;
        }
    }

    std::string normalized = InValue;
    while (normalized.size() > 1 && normalized[0] == '0') {
        normalized.erase(0, 1);
    }
    if (normalized == "0") {
        return false;
    }
    if (normalized.size() < std::char_traits<char>::length(kInt32Max)) {
        return true;
    }
    if (normalized.size() > std::char_traits<char>::length(kInt32Max)) {
        return false;
    }
    return normalized <= kInt32Max;
}

bool IsTruthyEnv(const char* InName) {
    if (InName == nullptr || *InName == '\0') {
        return false;
    }
    const char* value = std::getenv(InName);
    if (value == nullptr) {
        return false;
    }
    const std::string raw{value};
    std::string normalized;
    normalized.reserve(raw.size());
    for (const char ch : raw) {
        normalized.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    }
    return normalized == "1" || normalized == "true" || normalized == "yes" || normalized == "on";
}

void SetAllowIgnoreGateEnv(bool InEnabled) {
#if defined(_WIN32)
    if (InEnabled) {
        _putenv_s("KOG_ALLOW_IGNORE_GATE", "1");
    }
#else
    if (InEnabled) {
        setenv("KOG_ALLOW_IGNORE_GATE", "1", 1);
    }
#endif
}

void SetSelfBinaryPathEnv(char* InArgv0) {
    std::filesystem::path binaryPath;
#if defined(_WIN32)
    std::wstring moduleBuffer(static_cast<std::size_t>(MAX_PATH), L'\0');
    DWORD copied = 0;
    for (;;) {
        copied = GetModuleFileNameW(nullptr, moduleBuffer.data(), static_cast<DWORD>(moduleBuffer.size()));
        if (copied == 0) {
            break;
        }
        if (copied < moduleBuffer.size() - 1) {
            moduleBuffer.resize(copied);
            binaryPath = std::filesystem::path(moduleBuffer);
            break;
        }
        moduleBuffer.resize(moduleBuffer.size() * 2);
    }
#else
    if (InArgv0 != nullptr && *InArgv0 != '\0') {
        std::error_code ec;
        const auto raw = std::filesystem::path{InArgv0};
        if (raw.is_absolute()) {
            binaryPath = std::filesystem::weakly_canonical(raw, ec);
            if (ec) {
                binaryPath = raw;
            }
        } else {
            const auto candidate = std::filesystem::current_path(ec) / raw;
            if (!ec) {
                binaryPath = std::filesystem::weakly_canonical(candidate, ec);
                if (ec) {
                    binaryPath = candidate;
                }
            }
        }
    }
#endif

    if (binaryPath.empty()) {
        return;
    }

    const auto normalized = binaryPath.lexically_normal().string();
    if (normalized.empty()) {
        return;
    }

#if defined(_WIN32)
    _putenv_s("KANO_GIT_BINARY_PATH", normalized.c_str());
#else
    setenv("KANO_GIT_BINARY_PATH", normalized.c_str(), 1);
#endif
}

void SetSkillRootEnvFromBinaryPath() {
    if (const char* existing = std::getenv("KANO_GIT_SKILL_ROOT"); existing != nullptr && *existing != '\0') {
        return;
    }
    const char* binaryRaw = std::getenv("KANO_GIT_BINARY_PATH");
    if (binaryRaw == nullptr || *binaryRaw == '\0') {
        return;
    }

    std::error_code ec;
    auto binaryPath = std::filesystem::weakly_canonical(std::filesystem::path(binaryRaw), ec);
    if (ec) {
        binaryPath = std::filesystem::path(binaryRaw).lexically_normal();
    }
    if (binaryPath.empty()) {
        return;
    }

    auto current = binaryPath.parent_path();
    for (int i = 0; i < 8 && !current.empty(); ++i) {
        const auto skillMarker = (current / "SKILL.md").lexically_normal();
        if (std::filesystem::exists(skillMarker, ec) && !ec && std::filesystem::is_regular_file(skillMarker, ec)) {
#if defined(_WIN32)
            _putenv_s("KANO_GIT_SKILL_ROOT", current.lexically_normal().string().c_str());
#else
            setenv("KANO_GIT_SKILL_ROOT", current.lexically_normal().string().c_str(), 1);
#endif
            return;
        }
        ec.clear();
        current = current.parent_path();
    }
}

std::string DefaultPlanPath() {
    if (const char* explicitPlan = std::getenv("KOG_PLAN_FILE"); explicitPlan != nullptr && *explicitPlan != '\0') {
        return std::string{explicitPlan};
    }
    const std::filesystem::path root = std::filesystem::current_path();
    return (root / ".kano" / "tmp" / "git" / "plans" / "default-plan.json").lexically_normal().generic_string();
}

bool RewriteSlogShorthand(std::vector<std::string>& InOutArgs, std::string& OutError) {
    if (InOutArgs.size() <= 1) {
        return true;
    }
    const std::string& first = InOutArgs[1];
    if (first == "slog") {
        return true;
    }
    if (first.rfind("slog", 0) != 0 || first.size() <= 4) {
        return true;
    }

    const std::string countPart = first.substr(4);
    if (!IsPositiveInt32(countPart)) {
        OutError = "Error: slog count must be a positive integer <= 2147483647";
        return false;
    }

    std::vector<std::string> rewritten;
    rewritten.reserve(InOutArgs.size() + 1);
    rewritten.push_back(InOutArgs[0]);
    rewritten.push_back("slog");
    rewritten.push_back(countPart);
    for (std::size_t i = 2; i < InOutArgs.size(); ++i) {
        rewritten.push_back(InOutArgs[i]);
    }
    InOutArgs = std::move(rewritten);
    return true;
}

void RewriteDiscoverAlias(std::vector<std::string>& InOutArgs) {
    if (InOutArgs.size() <= 1 || InOutArgs[1] != "discover") {
        return;
    }

    const bool isFullDiscover = InOutArgs.size() > 2 && InOutArgs[2] == "full";
    std::vector<std::string> normalized;
    normalized.reserve(InOutArgs.size() + 2);
    normalized.push_back(InOutArgs[0]);
    normalized.push_back("discover");
    if (isFullDiscover) {
        normalized.push_back("full");
    }

    bool hasMaxDepth = false;
    for (std::size_t i = isFullDiscover ? 3 : 2; i < InOutArgs.size(); ++i) {
        const std::string& arg = InOutArgs[i];
        if (arg == "--native-max-depth") {
            if (isFullDiscover) {
                normalized.push_back("--max-depth");
                hasMaxDepth = true;
            }
            continue;
        }
        if (arg.rfind("--native-max-depth=", 0) == 0) {
            if (isFullDiscover) {
                normalized.push_back("--max-depth=" + arg.substr(std::string{"--native-max-depth="}.size()));
                hasMaxDepth = true;
            }
            continue;
        }
        if (arg == "--max-depth" || arg.rfind("--max-depth=", 0) == 0) {
            if (isFullDiscover) {
                hasMaxDepth = true;
            }
        }
        if (arg == "--native-no-cache" || arg == "--no-cache") {
            normalized.push_back("--no-cache");
            continue;
        }
        if (arg == "--native-refresh-cache") {
            continue;
        }
        normalized.push_back(arg);
    }

    if (isFullDiscover && !hasMaxDepth) {
        normalized.push_back("--max-depth");
        normalized.push_back(std::to_string(DiscoverDefaultMaxDepth()));
    }
    InOutArgs = std::move(normalized);
}

void RewriteCommandAliases(std::vector<std::string>& InOutArgs) {
    if (InOutArgs.size() <= 1) {
        return;
    }
    const std::string& cmd = InOutArgs[1];
    if (cmd == "pi") {
        std::vector<std::string> rewritten;
        rewritten.reserve(InOutArgs.size() + 1);
        rewritten.push_back(InOutArgs[0]);
        rewritten.push_back("plan");
        rewritten.push_back("new");
        for (std::size_t i = 2; i < InOutArgs.size(); ++i) {
            rewritten.push_back(InOutArgs[i]);
        }
        InOutArgs = std::move(rewritten);
        return;
    }
    if (cmd == "pia") {
        std::vector<std::string> rewritten;
        rewritten.reserve(InOutArgs.size() + 2);
        rewritten.push_back(InOutArgs[0]);
        rewritten.push_back("plan");
        rewritten.push_back("new");
        rewritten.push_back("--ai-auto");
        for (std::size_t i = 2; i < InOutArgs.size(); ++i) {
            rewritten.push_back(InOutArgs[i]);
        }
        InOutArgs = std::move(rewritten);
        return;
    }
    if (cmd == "pv") {
        std::vector<std::string> rewritten;
        rewritten.reserve(InOutArgs.size() + 2);
        rewritten.push_back(InOutArgs[0]);
        rewritten.push_back("plan");
        rewritten.push_back("verify");
        rewritten.push_back("pre-apply");
        for (std::size_t i = 2; i < InOutArgs.size(); ++i) {
            rewritten.push_back(InOutArgs[i]);
        }
        InOutArgs = std::move(rewritten);
        return;
    }
    if (cmd == "cp") {
        std::vector<std::string> rewritten;
        rewritten.reserve(InOutArgs.size() + 1);
        rewritten.push_back(InOutArgs[0]);
        rewritten.push_back("commit-push");
        for (std::size_t i = 2; i < InOutArgs.size(); ++i) {
            rewritten.push_back(InOutArgs[i]);
        }
        InOutArgs = std::move(rewritten);
        return;
    }
    if (cmd == "cpa") {
        std::vector<std::string> rewritten;
        rewritten.reserve(InOutArgs.size() + 3);
        rewritten.push_back(InOutArgs[0]);
        rewritten.push_back("commit-push");
        const auto hasExplicitAgentInput = std::any_of(InOutArgs.begin() + 2, InOutArgs.end(), [](const std::string& InArg) {
            return InArg == "-m" ||
                   InArg == "--message" ||
                   InArg.starts_with("--message=") ||
                   InArg == "--plan-file" ||
                   InArg.starts_with("--plan-file=");
        });
        if (IsTruthyEnv("KANO_AGENT_MODE") && !hasExplicitAgentInput) {
            rewritten.push_back("--plan-file");
            rewritten.push_back(DefaultPlanPath());
        } else if (!IsTruthyEnv("KANO_AGENT_MODE")) {
            rewritten.push_back("--ai-auto");
        }
        for (std::size_t i = 2; i < InOutArgs.size(); ++i) {
            rewritten.push_back(InOutArgs[i]);
        }
        InOutArgs = std::move(rewritten);
    }
}

void RewritePlanLifecycleAliases(std::vector<std::string>& InOutArgs) {
    (void)InOutArgs;
    // No-op. Plan lifecycle routing is now handled by native nested subcommands:
    // - plan runbook <commit|ignore|full>
    // - plan verify <pre-apply|post-apply|ignore|secret>
}

void RewriteCommitAiFlagsAndPlan(std::vector<std::string>& InOutArgs) {
    if (InOutArgs.size() <= 1) {
        return;
    }
    const std::string& cmd = InOutArgs[1];
    if (cmd != "commit" && cmd != "amend" && cmd != "commit-push" && cmd != "cherry-pick" && cmd != "converge") {
        return;
    }

    bool aiRequested = false;
    bool hasPlanFile = false;
    bool noPlanAuto = false;
    bool allowIgnoreGate = false;

    std::vector<std::string> rewritten;
    rewritten.reserve(InOutArgs.size() + 2);
    rewritten.push_back(InOutArgs[0]);
    rewritten.push_back(InOutArgs[1]);

    for (std::size_t i = 2; i < InOutArgs.size(); ++i) {
        const std::string& arg = InOutArgs[i];
        if (arg == "-ai") {
            if (cmd == "converge") {
                rewritten.push_back("--ai");
            } else {
                rewritten.push_back("--ai-auto");
                aiRequested = true;
            }
            continue;
        }
        if (arg == "--allow-ignore-gate") {
            allowIgnoreGate = true;
            continue;
        }
        if (arg == "--no-plan-auto") {
            noPlanAuto = true;
            continue;
        }
        if (arg == "--ai-auto" || arg == "--ai-provider" || arg == "--ai-model" || arg == "--provider" || arg == "--model") {
            aiRequested = true;
        } else if (arg.rfind("--ai-provider=", 0) == 0 || arg.rfind("--ai-model=", 0) == 0 ||
                   arg.rfind("--provider=", 0) == 0 || arg.rfind("--model=", 0) == 0) {
            aiRequested = true;
        } else if (arg == "--plan-file" || arg.rfind("--plan-file=", 0) == 0) {
            hasPlanFile = true;
        }
        rewritten.push_back(arg);
    }

    SetAllowIgnoreGateEnv(allowIgnoreGate);

    if (!noPlanAuto && aiRequested && !hasPlanFile && cmd == "commit") {
        rewritten.push_back("--plan-file");
        rewritten.push_back(DefaultPlanPath());
    }
    InOutArgs = std::move(rewritten);
}

bool ShouldSuppressMainTimingForMachineJson(const std::vector<std::string>& InArgs) {
    if (InArgs.size() < 4 || InArgs[1] != "converge" || InArgs[2] != "branches") {
        return false;
    }
    const std::string subcommand = InArgs[3];
    if (subcommand != "plan" &&
        subcommand != "inventory" &&
        subcommand != "status" &&
        subcommand != "apply" &&
        subcommand != "retire") {
        return false;
    }
    if (IsTruthyEnv("KANO_AGENT_MODE") || IsTruthyEnv("AGENT_MODE")) {
        return true;
    }
    return std::find(InArgs.begin() + 4, InArgs.end(), "--json") != InArgs.end();
}

std::vector<std::string> NormalizeLegacyArgs(int InArgc, char* InArgv[]) {
    std::vector<std::string> out;
    out.reserve(static_cast<std::size_t>(InArgc));
    if (InArgc <= 0 || InArgv == nullptr) {
        return out;
    }

    for (int i = 0; i < InArgc; ++i) {
        out.emplace_back(InArgv[i] == nullptr ? "" : std::string{InArgv[i]});
    }
    std::string normalizeError;
    if (!RewriteSlogShorthand(out, normalizeError)) {
        throw std::runtime_error(normalizeError);
    }
    RewriteCommandAliases(out);
    RewritePlanLifecycleAliases(out);
    RewriteDiscoverAlias(out);
    RewriteCommitAiFlagsAndPlan(out);
    return out;
}

} // namespace

int main(int InArgc, char* InArgv[]) {
    CLI::App app{
        "Kano Git Master — AI-powered Git CLI tools\n"
        "Standalone: kano-git <command> or kog <command>\n"
        "Unified:    kano git <command> (future)",
        "kano-git"
    };

    app.set_version_flag("--version,-V", std::string{::kano::git::GetBuildVersion()});
    app.require_subcommand(0);  // Allow running with no subcommand (shows help)
    app.fallthrough();

    std::string cmdLabel = "kog";
    bool redactNext = false;
    bool redactUrlNext = false;
    for (int i = 1; i < InArgc; ++i) {
        cmdLabel += " ";
        cmdLabel += RedactCommandArgForLog(InArgv[i] == nullptr ? "" : std::string{InArgv[i]}, redactNext, redactUrlNext);
    }
    std::vector<std::string> rawArgsForTiming;
    rawArgsForTiming.reserve(static_cast<std::size_t>(std::max(InArgc, 0)));
    for (int i = 0; i < InArgc; ++i) {
        rawArgsForTiming.emplace_back(InArgv[i] == nullptr ? "" : std::string{InArgv[i]});
    }
    std::unique_ptr<kano::infra::timing::ScopedTimingLog> timingLog;
    if (IsTruthyEnv("KOG_DEBUG") && !ShouldSuppressMainTimingForMachineJson(rawArgsForTiming)) {
        timingLog = std::make_unique<kano::infra::timing::ScopedTimingLog>(cmdLabel);
    }

    try {
        SetSelfBinaryPathEnv(InArgc > 0 ? InArgv[0] : nullptr);
        SetSkillRootEnvFromBinaryPath();

        // Register all commands
        kano::git::commands::RegisterAll(app);

        if (InArgc <= 1) {
            std::cout << app.help() << std::endl;
            return 0;
        }

        char** utf8Argv = app.ensure_utf8(InArgv);
        auto normalizedArgs = NormalizeLegacyArgs(InArgc, utf8Argv);
        
        if (normalizedArgs.size() > 1) {
            const std::string& inputCommand = normalizedArgs[1];
            if (!inputCommand.empty() && inputCommand[0] != '-') {
                const auto topLevelCommands = CollectTopLevelCommandNames(app);
                if (!IsKnownTopLevelCommand(inputCommand, topLevelCommands)) {
                    const auto suggestions = SuggestSimilarCommands(inputCommand, topLevelCommands);
                    PrintUnknownCommandError(inputCommand, suggestions);
                    return 1;
                }
            }
        }

        std::vector<char*> parseArgv;
        parseArgv.reserve(normalizedArgs.size());
        for (auto& arg : normalizedArgs) {
            parseArgv.push_back(arg.data());
        }
        app.parse(static_cast<int>(parseArgv.size()), parseArgv.data());
    } catch (const CLI::ParseError& e) {
        return app.exit(e);
    } catch (const std::exception& e) {
        std::cerr << "Fatal error: " << e.what() << "\n";
        return 1;
    } catch (...) {
        std::cerr << "Fatal error: unknown exception\n";
        return 1;
    }

    return 0;
}
