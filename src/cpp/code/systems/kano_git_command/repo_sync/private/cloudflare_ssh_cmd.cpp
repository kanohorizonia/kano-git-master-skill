#include <CLI/CLI.hpp>

#include "auth_cmd.hpp"
#include "shell_executor.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

namespace kano::git::commands {
namespace {

struct CloudflareSshOptions {
    std::string hostname;
    std::string user{"git"};
    std::string cloudflaredPath;
    bool install{false};
    bool dryRun{false};
    bool confirmHostWrite{false};
};

auto Trim(std::string InValue) -> std::string {
    const auto first = InValue.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }
    const auto last = InValue.find_last_not_of(" \t\r\n");
    return InValue.substr(first, last - first + 1);
}

auto FirstLine(const std::string& InValue) -> std::string {
    std::istringstream stream(InValue);
    std::string line;
    std::getline(stream, line);
    return Trim(line);
}

auto IsSafeHostToken(const std::string& InValue, const bool InAllowDot) -> bool {
    if (InValue.empty()) {
        return false;
    }
    return std::all_of(InValue.begin(), InValue.end(), [=](const unsigned char ch) {
        return std::isalnum(ch) != 0 || ch == '-' || ch == '_' || (InAllowDot && ch == '.');
    });
}

auto ResolveHomeDirectory() -> std::optional<std::filesystem::path> {
#if defined(_WIN32)
    const char* raw = std::getenv("USERPROFILE");
    if (raw == nullptr || raw[0] == '\0') {
        raw = std::getenv("HOME");
    }
#else
    const char* raw = std::getenv("HOME");
#endif
    if (raw == nullptr || raw[0] == '\0') {
        return std::nullopt;
    }
    return std::filesystem::path(raw).lexically_normal();
}

auto ReadTextFile(const std::filesystem::path& InPath) -> std::string {
    std::ifstream input(InPath, std::ios::binary);
    if (!input.good()) {
        return {};
    }
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

auto WritePrivateTextFile(const std::filesystem::path& InPath,
                          const std::string& InContent,
                          std::string& OutError) -> bool {
    std::error_code ec;
    std::filesystem::create_directories(InPath.parent_path(), ec);
    if (ec) {
        OutError = "failed to create directory " + InPath.parent_path().generic_string() + ": " + ec.message();
        return false;
    }
#if !defined(_WIN32)
    std::filesystem::permissions(
        InPath.parent_path(),
        std::filesystem::perms::owner_all,
        std::filesystem::perm_options::replace,
        ec);
    ec.clear();
#endif

    std::ofstream output(InPath, std::ios::binary | std::ios::trunc);
    if (!output.good()) {
        OutError = "failed to open " + InPath.generic_string() + " for writing";
        return false;
    }
    output << InContent;
    output.close();
    if (!output.good()) {
        OutError = "failed to write " + InPath.generic_string();
        return false;
    }
#if !defined(_WIN32)
    std::filesystem::permissions(
        InPath,
        std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
        std::filesystem::perm_options::replace,
        ec);
    if (ec) {
        OutError = "failed to set private permissions on " + InPath.generic_string() + ": " + ec.message();
        return false;
    }
#endif
    return true;
}

auto NormalizeForAppend(std::string InText) -> std::string {
    while (!InText.empty() && (InText.back() == '\n' || InText.back() == '\r')) {
        InText.pop_back();
    }
    if (!InText.empty()) {
        InText += "\n\n";
    }
    return InText;
}

auto RemoveManagedBlock(const std::string& InText,
                        const std::string& InBegin,
                        const std::string& InEnd,
                        std::string& OutError) -> std::string {
    const auto begin = InText.find(InBegin);
    if (begin == std::string::npos) {
        return InText;
    }
    const auto end = InText.find(InEnd, begin + InBegin.size());
    if (end == std::string::npos) {
        OutError = "managed SSH block has a begin marker without a matching end marker";
        return {};
    }
    auto after = end + InEnd.size();
    while (after < InText.size() && (InText[after] == '\n' || InText[after] == '\r')) {
        ++after;
    }
    auto out = InText.substr(0, begin);
    out += InText.substr(after);
    return out;
}

auto ResolveCloudflaredPath(const std::string& InExplicitPath) -> std::optional<std::filesystem::path> {
    std::error_code ec;
    if (!InExplicitPath.empty()) {
        const auto explicitPath = std::filesystem::absolute(InExplicitPath, ec).lexically_normal();
        if (!ec && std::filesystem::is_regular_file(explicitPath, ec) && !ec) {
            return explicitPath;
        }
        return std::nullopt;
    }

    const std::vector<std::filesystem::path> candidates{
#if defined(_WIN32)
        std::filesystem::path{"C:/Program Files/cloudflared/cloudflared.exe"},
        std::filesystem::path{"C:/Program Files (x86)/cloudflared/cloudflared.exe"},
#elif defined(__APPLE__)
        std::filesystem::path{"/opt/homebrew/bin/cloudflared"},
        std::filesystem::path{"/usr/local/bin/cloudflared"},
#else
        std::filesystem::path{"/usr/local/bin/cloudflared"},
        std::filesystem::path{"/usr/bin/cloudflared"},
#endif
    };
    for (const auto& candidate : candidates) {
        if (std::filesystem::is_regular_file(candidate, ec) && !ec) {
            return candidate.lexically_normal();
        }
        ec.clear();
    }

#if defined(_WIN32)
    const auto probe = shell::ExecuteCommand(
        "where.exe", {"cloudflared.exe"}, shell::ExecMode::Capture, std::filesystem::current_path());
#else
    const auto probe = shell::ExecuteCommand(
        "which", {"cloudflared"}, shell::ExecMode::Capture, std::filesystem::current_path());
#endif
    if (probe.exitCode != 0) {
        return std::nullopt;
    }
    const auto first = FirstLine(probe.stdoutStr);
    if (first.empty()) {
        return std::nullopt;
    }
    const auto resolved = std::filesystem::path(first).lexically_normal();
    if (!std::filesystem::is_regular_file(resolved, ec) || ec) {
        return std::nullopt;
    }
    return resolved;
}

auto InstallCloudflared(const bool InDryRun) -> int {
#if defined(__APPLE__)
    std::cout << (InDryRun ? "[dry-run] " : "") << "brew install cloudflared\n";
    if (InDryRun) {
        return 0;
    }
    return shell::ExecuteCommand(
               "brew", {"install", "cloudflared"}, shell::ExecMode::PassThrough, std::filesystem::current_path())
        .exitCode;
#elif defined(_WIN32)
    const std::vector<std::string> args{
        "install",
        "--id",
        "Cloudflare.cloudflared",
        "--exact",
        "--accept-source-agreements",
        "--accept-package-agreements",
    };
    std::cout << (InDryRun ? "[dry-run] " : "")
              << "winget install --id Cloudflare.cloudflared --exact "
                 "--accept-source-agreements --accept-package-agreements\n";
    if (InDryRun) {
        return 0;
    }
    return shell::ExecuteCommand(
               "winget", args, shell::ExecMode::PassThrough, std::filesystem::current_path())
        .exitCode;
#else
    std::cerr << "Automatic cloudflared installation is currently supported on macOS/Homebrew "
                 "and Windows/WinGet.\n";
    std::cerr << "Install cloudflared with the platform package documented by Cloudflare, then retry.\n";
    return 2;
#endif
}

auto ValidateOptions(const CloudflareSshOptions& InOptions) -> bool {
    if (!IsSafeHostToken(InOptions.hostname, true) || InOptions.hostname.find('.') == std::string::npos) {
        std::cerr << "Invalid --hostname. Use a fully qualified DNS hostname.\n";
        return false;
    }
    if (!IsSafeHostToken(InOptions.user, false)) {
        std::cerr << "Invalid --user. Use an SSH user without whitespace or shell metacharacters.\n";
        return false;
    }
    return true;
}

auto MainConfigHasGlobalInclude(const std::string& InConfig) -> bool {
    std::istringstream lines(InConfig);
    std::string line;
    while (std::getline(lines, line)) {
        const auto trimmed = Trim(line);
        if (trimmed.rfind("Host ", 0) == 0 || trimmed.rfind("Match ", 0) == 0) {
            return false;
        }
        if (trimmed == "Include ~/.ssh/config.d/*" ||
            trimmed == "Include \"~/.ssh/config.d/*\"") {
            return true;
        }
    }
    return false;
}

auto ConfigureSsh(const CloudflareSshOptions& InOptions,
                  const std::filesystem::path& InCloudflaredPath) -> int {
    const auto home = ResolveHomeDirectory();
    if (!home.has_value()) {
        std::cerr << "Unable to resolve the current user home directory.\n";
        return 2;
    }

    const auto sshDirectory = *home / ".ssh";
    const auto mainConfigPath = sshDirectory / "config";
    const auto fragmentPath = sshDirectory / "config.d" / "kog-cloudflare-access.conf";
    const auto backupPath = sshDirectory / "config.kog.bak";
    const std::string includeBegin = "# BEGIN KOG CLOUDFLARE ACCESS INCLUDE";
    const std::string includeEnd = "# END KOG CLOUDFLARE ACCESS INCLUDE";
    const std::string hostBegin = "# BEGIN KOG CLOUDFLARE ACCESS " + InOptions.hostname;
    const std::string hostEnd = "# END KOG CLOUDFLARE ACCESS " + InOptions.hostname;

    auto mainConfig = ReadTextFile(mainConfigPath);
    const auto mainConfigBefore = mainConfig;
    std::string includeError;
    mainConfig = RemoveManagedBlock(mainConfig, includeBegin, includeEnd, includeError);
    if (!includeError.empty()) {
        std::cerr << includeError << ": " << mainConfigPath.generic_string() << "\n";
        return 2;
    }
    if (!MainConfigHasGlobalInclude(mainConfig)) {
        std::string withGlobalInclude;
        withGlobalInclude += includeBegin + "\n";
        withGlobalInclude += "Include ~/.ssh/config.d/*\n";
        withGlobalInclude += includeEnd + "\n\n";
        withGlobalInclude += mainConfig;
        mainConfig = std::move(withGlobalInclude);
    }

    auto fragment = ReadTextFile(fragmentPath);
    std::string blockError;
    fragment = RemoveManagedBlock(fragment, hostBegin, hostEnd, blockError);
    if (!blockError.empty()) {
        std::cerr << blockError << ": " << fragmentPath.generic_string() << "\n";
        return 2;
    }
    fragment = NormalizeForAppend(std::move(fragment));
    fragment += hostBegin + "\n";
    fragment += "Host " + InOptions.hostname + "\n";
    fragment += "  HostName " + InOptions.hostname + "\n";
    fragment += "  User " + InOptions.user + "\n";
    fragment += "  ProxyCommand \"" + InCloudflaredPath.generic_string() + "\" access ssh --hostname %h\n";
    fragment += hostEnd + "\n";

    std::cout << (InOptions.dryRun ? "[dry-run] " : "")
              << "SSH include: " << mainConfigPath.generic_string() << "\n";
    std::cout << (InOptions.dryRun ? "[dry-run] " : "")
              << "Cloudflare Access host: " << fragmentPath.generic_string() << "\n";
    if (InOptions.dryRun) {
        std::cout << fragment;
        return 0;
    }
    if (!InOptions.confirmHostWrite) {
        std::cerr << "Refusing to modify SSH configuration without --confirm-host-write.\n";
        return 2;
    }

    std::error_code ec;
    if (mainConfig != mainConfigBefore && std::filesystem::is_regular_file(mainConfigPath, ec) && !ec) {
        std::filesystem::copy_file(
            mainConfigPath, backupPath, std::filesystem::copy_options::overwrite_existing, ec);
        if (ec) {
            std::cerr << "Failed to back up " << mainConfigPath.generic_string()
                      << ": " << ec.message() << "\n";
            return 1;
        }
    }

    std::string writeError;
    if (!WritePrivateTextFile(mainConfigPath, mainConfig, writeError)) {
        std::cerr << writeError << "\n";
        return 1;
    }
    if (!WritePrivateTextFile(fragmentPath, fragment, writeError)) {
        std::cerr << writeError << "\n";
        return 1;
    }

    std::cout << "Cloudflare Access SSH configuration completed for " << InOptions.hostname << ".\n";
    std::cout << "Verify with:\n";
    std::cout << "  ssh -T " << InOptions.user << "@" << InOptions.hostname << "\n";
    return 0;
}

auto RunSetup(const std::shared_ptr<CloudflareSshOptions>& InOptions) -> void {
    if (!ValidateOptions(*InOptions)) {
        std::exit(2);
    }

    auto cloudflared = ResolveCloudflaredPath(InOptions->cloudflaredPath);
    if (!cloudflared.has_value()) {
        if (!InOptions->install) {
            std::cerr << "cloudflared is not installed or was not found.\n";
            std::cerr << "Retry with --install, or pass --cloudflared-path <path>.\n";
            std::exit(2);
        }
        const auto installResult = InstallCloudflared(InOptions->dryRun);
        if (installResult != 0) {
            std::exit(installResult);
        }
        if (InOptions->dryRun) {
#if defined(_WIN32)
            cloudflared = std::filesystem::path{"C:/Program Files/cloudflared/cloudflared.exe"};
#elif defined(__APPLE__)
            cloudflared = std::filesystem::path{"/usr/local/bin/cloudflared"};
#endif
        } else {
            cloudflared = ResolveCloudflaredPath(InOptions->cloudflaredPath);
        }
    }
    if (!cloudflared.has_value()) {
        std::cerr << "cloudflared installation completed but the executable was not found.\n";
        std::exit(1);
    }

    const auto result = ConfigureSsh(*InOptions, *cloudflared);
    if (result != 0) {
        std::exit(result);
    }
}

auto RunDoctor(const std::shared_ptr<CloudflareSshOptions>& InOptions) -> void {
    if (!ValidateOptions(*InOptions)) {
        std::exit(2);
    }
    const auto home = ResolveHomeDirectory();
    const auto cloudflared = ResolveCloudflaredPath(InOptions->cloudflaredPath);
    if (!home.has_value()) {
        std::cerr << "status=not-ready reason=home-unresolved\n";
        std::exit(1);
    }

    const auto mainConfigPath = *home / ".ssh" / "config";
    const auto fragmentPath = *home / ".ssh" / "config.d" / "kog-cloudflare-access.conf";
    const auto mainConfig = ReadTextFile(mainConfigPath);
    const auto fragment = ReadTextFile(fragmentPath);
    const auto hostBegin = "# BEGIN KOG CLOUDFLARE ACCESS " + InOptions->hostname;
    const auto ready = cloudflared.has_value() &&
                       MainConfigHasGlobalInclude(mainConfig) &&
                       fragment.find(hostBegin) != std::string::npos;

    std::cout << "hostname=" << InOptions->hostname << "\n";
    std::cout << "cloudflared=" << (cloudflared.has_value() ? cloudflared->generic_string() : "missing") << "\n";
    std::cout << "ssh_include=" << (MainConfigHasGlobalInclude(mainConfig) ? "configured" : "missing") << "\n";
    std::cout << "host_block=" << (fragment.find(hostBegin) != std::string::npos ? "configured" : "missing") << "\n";
    std::cout << "status=" << (ready ? "ready" : "not-ready") << "\n";
    if (!ready) {
        std::exit(1);
    }
}

void AddCommonOptions(CLI::App& InCommand,
                      const std::shared_ptr<CloudflareSshOptions>& InOptions) {
    InCommand.add_option("--hostname", InOptions->hostname, "Cloudflare Access SSH hostname")
        ->required();
    InCommand.add_option("--user", InOptions->user, "SSH user written to the managed host block");
    InCommand.add_option(
        "--cloudflared-path",
        InOptions->cloudflaredPath,
        "Explicit cloudflared executable path when PATH discovery is insufficient");
}

} // namespace

void RegisterCloudflareSsh(CLI::App& InAuth) {
    auto* command = InAuth.add_subcommand(
        "cloudflare-ssh",
        "Install cloudflared and manage an SSH ProxyCommand host for Cloudflare Access");

    const auto setupOptions = std::make_shared<CloudflareSshOptions>();
    auto* setup = command->add_subcommand(
        "setup",
        "Install cloudflared when requested and idempotently configure the current user SSH client");
    AddCommonOptions(*setup, setupOptions);
    setup->add_flag("--install", setupOptions->install, "Install cloudflared with Homebrew or WinGet when missing");
    setup->add_flag("--dry-run", setupOptions->dryRun, "Preview install and SSH configuration without host writes");
    setup->add_flag(
        "--confirm-host-write",
        setupOptions->confirmHostWrite,
        "Confirm writes under the current user .ssh directory");
    setup->callback([setupOptions]() { RunSetup(setupOptions); });

    const auto doctorOptions = std::make_shared<CloudflareSshOptions>();
    auto* doctor = command->add_subcommand(
        "doctor",
        "Check cloudflared discovery and the managed SSH include/host block");
    AddCommonOptions(*doctor, doctorOptions);
    doctor->callback([doctorOptions]() { RunDoctor(doctorOptions); });
}

} // namespace kano::git::commands
