#include <CLI/CLI.hpp>

#include <filesystem>
#include <iostream>

#include "ai_utils.hpp"
#include "shell_executor.hpp"

namespace kano::git::commands {

namespace {

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
