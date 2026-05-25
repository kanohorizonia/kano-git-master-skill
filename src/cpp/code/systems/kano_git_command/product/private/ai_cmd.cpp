#include <CLI/CLI.hpp>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <string>

#include "ai_utils.hpp"
#include "shell_executor.hpp"

namespace kano::git::commands {

namespace {

auto LowerTrim(std::string InValue) -> std::string {
    const auto first = InValue.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }
    const auto last = InValue.find_last_not_of(" \t\r\n");
    auto out = InValue.substr(first, last - first + 1);
    std::transform(out.begin(), out.end(), out.begin(), [](const unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return out;
}

auto IsKogTestMode() -> bool {
    const auto* raw = std::getenv("KOG_TEST_MODE");
    if (raw == nullptr) {
        return false;
    }
    const auto value = LowerTrim(raw);
    return value == "1" || value == "true" || value == "yes" || value == "on";
}

auto CommandProbeKey(std::string InCommand) -> std::string {
    auto normalized = LowerTrim(std::move(InCommand));
    const auto slash = normalized.find_last_of("/\\");
    if (slash != std::string::npos) {
        normalized = normalized.substr(slash + 1);
    }
    for (const std::string suffix : {".exe", ".cmd", ".bat"}) {
        if (normalized.size() > suffix.size() && normalized.ends_with(suffix)) {
            normalized.resize(normalized.size() - suffix.size());
            break;
        }
    }
    return normalized;
}

auto TestModeCommandAvailable(const std::string& InCommand) -> bool {
    if (!IsKogTestMode()) {
        return false;
    }
    const auto target = CommandProbeKey(InCommand);
    const auto* raw = std::getenv("KOG_TEST_AVAILABLE_COMMANDS");
    if (raw == nullptr || raw[0] == '\0') {
        return false;
    }

    std::string token;
    std::istringstream tokens(raw);
    while (std::getline(tokens, token, ',')) {
        std::istringstream semiTokens(token);
        std::string semiToken;
        while (std::getline(semiTokens, semiToken, ';')) {
            if (CommandProbeKey(semiToken) == target) {
                return true;
            }
        }
    }
    return false;
}

auto TryHandleTestModeCopilotDryRun() -> bool {
#if defined(_WIN32)
    if (!IsKogTestMode()) {
        return false;
    }
    if (!TestModeCommandAvailable("winget")) {
        std::cout << "Copilot CLI is not installed and WinGet is unavailable.\n";
        std::cout << "WinGet is provided by Windows Package Manager / App Installer.\n";
        std::cout << "Install or repair App Installer / WinGet, then run:\n";
        std::cout << "  winget install GitHub.Copilot\n";
        std::exit(1);
    }

    std::cout << "Copilot CLI is not installed.\n\n";
    std::cout << "Recommended Windows install:\n";
    std::cout << "  winget install GitHub.Copilot\n\n";
    std::cout << "[dry-run] winget install GitHub.Copilot --accept-source-agreements --accept-package-agreements\n";
    std::cout << "Copilot bootstrap dry-run completed. No install was executed and copilot.exe is not required for dry-run.\n";
    return true;
#else
    return false;
#endif
}

auto HasPowerShell() -> std::string {
    const auto pwsh = shell::ExecuteCommand("pwsh", {"-NoProfile", "-Command", "$PSVersionTable.PSVersion.ToString()"}, shell::ExecMode::Capture, std::filesystem::current_path());
    if (pwsh.exitCode == 0) {
        return "pwsh";
    }
    const auto powershell = shell::ExecuteCommand("powershell.exe", {"-NoProfile", "-Command", "$PSVersionTable.PSVersion.ToString()"}, shell::ExecMode::Capture, std::filesystem::current_path());
    if (powershell.exitCode == 0) {
        return "powershell.exe";
    }
    return {};
}

} // namespace

void RegisterAi(CLI::App& InApp) {
    auto* ai = InApp.add_subcommand("ai", "AI provider tooling and bootstrap helpers");
    auto* bootstrap = ai->add_subcommand("bootstrap", "Bootstrap AI provider tooling");
    auto* copilot = bootstrap->add_subcommand("copilot", "Bootstrap GitHub Copilot CLI (Windows/WinGet only)");

    auto* dryRun = new bool{false};
    copilot->add_flag("--dry-run", *dryRun, "Print and validate bootstrap steps without installing packages");

    copilot->callback([=]() {
#if !defined(_WIN32)
        std::cerr << "Copilot CLI bootstrap is currently supported only on Windows/WinGet.\n";
        std::cerr << "Install Copilot CLI manually for this platform, then re-run the command.\n";
        std::exit(2);
#else
        if (*dryRun && TryHandleTestModeCopilotDryRun()) {
            return;
        }

        const auto workspaceRoot = std::filesystem::current_path().lexically_normal();
        const auto skillRoot = ResolveSkillRoot(workspaceRoot);
        auto scriptPath = (skillRoot / "src" / "shell" / "bootstrap" / "windows" / "ensure-copilot-cli.ps1").lexically_normal();
        std::error_code ec;
        if (!std::filesystem::exists(scriptPath, ec) || ec) {
            scriptPath = (workspaceRoot / "src" / "shell" / "bootstrap" / "windows" / "ensure-copilot-cli.ps1").lexically_normal();
        }
        if (!std::filesystem::exists(scriptPath, ec) || ec) {
            std::cerr << "Missing bootstrap script: " << scriptPath.generic_string() << "\n";
            std::exit(1);
        }

        const auto shellName = HasPowerShell();
        if (shellName.empty()) {
            std::cerr << "Neither pwsh nor powershell.exe is available.\n";
            std::exit(1);
        }

        std::vector<std::string> args{
            "-NoProfile",
            "-ExecutionPolicy", "Bypass",
            "-File", scriptPath.generic_string(),
            "-Install"
        };
        if (*dryRun) {
            args.push_back("-DryRun");
        }
        const auto result = shell::ExecuteCommand(shellName, args, shell::ExecMode::PassThrough, workspaceRoot);
        if (result.exitCode != 0) {
            std::exit(result.exitCode == 0 ? 1 : result.exitCode);
        }

        std::cout << "Copilot bootstrap completed.\n";
        std::cout << "Recommended verification commands:\n";
        std::cout << "  copilot --version\n";
        std::cout << "  copilot -s --model auto --no-color --stream off --no-ask-user -p \"Reply exactly: hello world\"\n";
#endif
    });
}

} // namespace kano::git::commands
