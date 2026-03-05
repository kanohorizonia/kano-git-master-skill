// kano-git — Kano Git Master CLI
// AI-powered Git CLI tools
//
// SPDX-License-Identifier: MIT

#include <CLI/CLI.hpp>
#include "command_registry.hpp"
#include <cctype>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#if defined(KOG_USE_MODULES)
import kano.git.version;
#else
#include "version.hpp"
#endif

namespace {

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

std::string DefaultPlanPath() {
    if (const char* explicitPlan = std::getenv("KOG_PLAN_FILE"); explicitPlan != nullptr && *explicitPlan != '\0') {
        return std::string{explicitPlan};
    }
    std::filesystem::path root = std::filesystem::current_path();
    if (const char* rootEnv = std::getenv("KANO_GIT_MASTER_ROOT"); rootEnv != nullptr && *rootEnv != '\0') {
        root = std::filesystem::path{rootEnv};
    }
    return (root / ".kano" / "cache" / "git" / "plans" / "default-plan.json").lexically_normal().generic_string();
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

    std::vector<std::string> normalized;
    normalized.reserve(InOutArgs.size() + 4);
    normalized.push_back(InOutArgs[0]);
    normalized.push_back("workspace");
    normalized.push_back("discover");

    bool hasMaxDepth = false;
    for (std::size_t i = 2; i < InOutArgs.size(); ++i) {
        const std::string& arg = InOutArgs[i];
        if (arg == "--native-max-depth") {
            normalized.push_back("--max-depth");
            hasMaxDepth = true;
            continue;
        }
        if (arg.rfind("--native-max-depth=", 0) == 0) {
            normalized.push_back("--max-depth=" + arg.substr(std::string{"--native-max-depth="}.size()));
            hasMaxDepth = true;
            continue;
        }
        if (arg == "--max-depth" || arg.rfind("--max-depth=", 0) == 0) {
            hasMaxDepth = true;
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

    if (!hasMaxDepth) {
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
        rewritten.reserve(InOutArgs.size() + 2);
        rewritten.push_back(InOutArgs[0]);
        rewritten.push_back("commit-push");
        rewritten.push_back("--ai-auto");
        for (std::size_t i = 2; i < InOutArgs.size(); ++i) {
            rewritten.push_back(InOutArgs[i]);
        }
        InOutArgs = std::move(rewritten);
    }
}

void RewritePlanLifecycleAliases(std::vector<std::string>& InOutArgs) {
    if (InOutArgs.size() <= 2 || InOutArgs[1] != "plan") {
        return;
    }

    if (InOutArgs[2] == "runbook" && InOutArgs.size() > 3 && InOutArgs[3] == "commit") {
        InOutArgs[2] = "runbook-commit";
        InOutArgs.erase(InOutArgs.begin() + 3);
        return;
    }
    if (InOutArgs[2] == "runbook" && InOutArgs.size() > 3 && InOutArgs[3] == "ignore") {
        InOutArgs[2] = "runbook-ignore";
        InOutArgs.erase(InOutArgs.begin() + 3);
        return;
    }
    if (InOutArgs[2] == "runbook" && InOutArgs.size() > 3 && InOutArgs[3] == "full") {
        InOutArgs[2] = "runbook-full";
        InOutArgs.erase(InOutArgs.begin() + 3);
        return;
    }

    if (InOutArgs[2] != "verify") {
        return;
    }
    if (InOutArgs.size() <= 3) {
        return;
    }

    std::string mode = InOutArgs[3];
    if (mode == "pre-apply") {
        InOutArgs[2] = "schema-verify";
        InOutArgs.erase(InOutArgs.begin() + 3);
        return;
    }
    if (mode == "post-apply") {
        InOutArgs[2] = "result-verify";
        InOutArgs.erase(InOutArgs.begin() + 3);
        return;
    }
    if (mode == "ignore") {
        InOutArgs[2] = "ignore-gate";
        InOutArgs.erase(InOutArgs.begin() + 3);
        return;
    }
    if (mode == "secret") {
        InOutArgs[2] = "secret-gate";
        InOutArgs.erase(InOutArgs.begin() + 3);
    }
}

void RewriteCommitAiFlagsAndPlan(std::vector<std::string>& InOutArgs) {
    if (InOutArgs.size() <= 1) {
        return;
    }
    const std::string& cmd = InOutArgs[1];
    if (cmd != "commit" && cmd != "amend" && cmd != "commit-push") {
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
            rewritten.push_back("--ai-auto");
            aiRequested = true;
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

    if (!noPlanAuto && aiRequested && !hasPlanFile && (cmd == "commit" || cmd == "commit-push")) {
        rewritten.push_back("--plan-file");
        rewritten.push_back(DefaultPlanPath());
    }
    InOutArgs = std::move(rewritten);
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

    app.set_version_flag("--version,-V", std::string{kano::git::GetBuildVersion()});
    app.require_subcommand(0);  // Allow running with no subcommand (shows help)
    app.fallthrough();

    try {
        // Register all commands
        kano::git::commands::RegisterAll(app);

        if (InArgc <= 1) {
            std::cout << app.help() << std::endl;
            return 0;
        }

        char** utf8Argv = app.ensure_utf8(InArgv);
        auto normalizedArgs = NormalizeLegacyArgs(InArgc, utf8Argv);
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
