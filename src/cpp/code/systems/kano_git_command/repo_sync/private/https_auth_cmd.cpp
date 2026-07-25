#include <CLI/CLI.hpp>

#include "auth_cmd.hpp"
#include "shell_executor.hpp"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#if !defined(_WIN32)
#include <unistd.h>
#endif

namespace kano::git::commands {
namespace {

constexpr const char* kDefaultGcmVersion = "2.9.1";

struct HttpsAuthOptions {
    std::string hostname;
    std::string username;
    std::string authMode{"pat"};
    std::string gcmPath;
    std::string gcmVersion{kDefaultGcmVersion};
    bool install{false};
    bool dryRun{false};
    bool confirmGlobalWrite{false};
};

auto Trim(std::string InValue) -> std::string {
    const auto first = InValue.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }
    const auto last = InValue.find_last_not_of(" \t\r\n");
    return InValue.substr(first, last - first + 1);
}

auto ResolveHomeDirectory() -> std::optional<std::filesystem::path> {
    if (const auto* home = std::getenv("HOME"); home != nullptr && *home != '\0') {
        return std::filesystem::path{home};
    }
    if (const auto* profile = std::getenv("USERPROFILE"); profile != nullptr && *profile != '\0') {
        return std::filesystem::path{profile};
    }
    return std::nullopt;
}

auto IsValidHostname(const std::string& InHostname) -> bool {
    if (InHostname.empty() || InHostname.front() == '.' || InHostname.back() == '.') {
        return false;
    }
    return std::all_of(InHostname.begin(), InHostname.end(), [](const unsigned char character) {
        return std::isalnum(character) != 0 || character == '.' || character == '-';
    });
}

auto IsValidVersion(const std::string& InVersion) -> bool {
    return !InVersion.empty() &&
           std::all_of(InVersion.begin(), InVersion.end(), [](const unsigned char character) {
               return std::isdigit(character) != 0 || character == '.';
           });
}

auto IsRegularFile(const std::filesystem::path& InPath) -> bool {
    std::error_code ec;
    return std::filesystem::is_regular_file(InPath, ec) && !ec;
}

auto IsExecutableFile(const std::filesystem::path& InPath) -> bool {
    if (!IsRegularFile(InPath)) {
        return false;
    }
#if defined(_WIN32)
    return true;
#else
    return ::access(InPath.c_str(), X_OK) == 0;
#endif
}

auto ToLower(std::string InValue) -> std::string {
    std::transform(
        InValue.begin(),
        InValue.end(),
        InValue.begin(),
        [](const unsigned char InCharacter) {
            return static_cast<char>(std::tolower(InCharacter));
        });
    return InValue;
}

auto IsGcmExecutableName(const std::filesystem::path& InPath) -> bool {
    const auto filename = ToLower(InPath.filename().string());
    return filename.find("git-credential-manager") != std::string::npos;
}

auto IsRunnableGcm(const std::filesystem::path& InPath) -> bool {
    if (!InPath.is_absolute() ||
        !IsGcmExecutableName(InPath) ||
        !IsExecutableFile(InPath)) {
        return false;
    }

    const auto result = shell::ExecuteCommand(
        InPath.string(),
        {"--version"},
        shell::ExecMode::Capture,
        std::filesystem::current_path());
    if (result.exitCode != 0) {
        return false;
    }

    const auto versionText = result.stdoutStr + "\n" + result.stderrStr;
    const bool hasDigit = std::any_of(
        versionText.begin(),
        versionText.end(),
        [](const unsigned char InCharacter) {
            return std::isdigit(InCharacter) != 0;
        });
    return hasDigit && versionText.find('.') != std::string::npos;
}

auto GitCapture(const std::vector<std::string>& InArgs) -> shell::ExecResult {
    return shell::ExecuteCommand(
        "git",
        InArgs,
        shell::ExecMode::Capture,
        std::filesystem::current_path());
}

auto GlobalGitConfigValues(const std::string& InKey) -> std::vector<std::string> {
    std::vector<std::string> values;
    const auto result = GitCapture({"config", "--global", "--get-all", InKey});
    if (result.exitCode != 0) {
        return values;
    }

    std::istringstream input(result.stdoutStr);
    std::string line;
    while (std::getline(input, line)) {
        line = Trim(line);
        if (!line.empty()) {
            values.push_back(line);
        }
    }
    return values;
}

auto GlobalGitConfigValue(const std::string& InKey) -> std::optional<std::string> {
    const auto result = GitCapture({"config", "--global", "--get", InKey});
    if (result.exitCode != 0) {
        return std::nullopt;
    }
    const auto value = Trim(result.stdoutStr);
    if (value.empty()) {
        return std::nullopt;
    }
    return value;
}

auto ResolveConfiguredGcmPath() -> std::optional<std::filesystem::path> {
    for (const auto& helper : GlobalGitConfigValues("credential.helper")) {
        const std::filesystem::path candidate{helper};
        if (IsRunnableGcm(candidate)) {
            return candidate;
        }
    }
    return std::nullopt;
}

auto ResolveExecutableOnPath(const std::string& InExecutable)
    -> std::optional<std::filesystem::path> {
#if defined(_WIN32)
    constexpr const char* locator = "where";
#else
    constexpr const char* locator = "which";
#endif
    const auto located = shell::ExecuteCommand(
        locator,
        {InExecutable},
        shell::ExecMode::Capture,
        std::filesystem::current_path());
    if (located.exitCode != 0) {
        return std::nullopt;
    }

    std::istringstream input(located.stdoutStr);
    std::string firstPath;
    if (!std::getline(input, firstPath)) {
        return std::nullopt;
    }
    const std::filesystem::path candidate{Trim(firstPath)};
    if (!candidate.is_absolute() || !IsExecutableFile(candidate)) {
        return std::nullopt;
    }
    return candidate;
}

auto ResolveGcmPath(const std::string& InExplicitPath) -> std::optional<std::filesystem::path> {
    if (!InExplicitPath.empty()) {
        std::error_code ec;
        const auto explicitPath =
            std::filesystem::absolute(std::filesystem::path{InExplicitPath}, ec);
        if (!ec && IsRunnableGcm(explicitPath)) {
            return explicitPath;
        }
        return std::nullopt;
    }

    if (const auto configured = ResolveConfiguredGcmPath(); configured.has_value()) {
        return configured;
    }

    if (const auto directPath = ResolveExecutableOnPath("git-credential-manager");
        directPath.has_value() && IsRunnableGcm(*directPath)) {
        return directPath;
    }
    return std::nullopt;
}

struct GcmAsset {
    std::string name;
    std::string sha256;
};

auto PlatformAsset(const std::string& InVersion) -> std::optional<GcmAsset> {
    if (InVersion != kDefaultGcmVersion) {
        return std::nullopt;
    }

#if defined(__APPLE__)
#if defined(__aarch64__) || defined(_M_ARM64)
    return GcmAsset{
        "gcm-osx-arm64-" + InVersion + ".tar.gz",
        "2ac8f99258d04acb45cf592eb5b06ec0e0760c329bce40a4d18dabb5e0e37f68"};
#elif defined(__x86_64__) || defined(_M_X64)
    return GcmAsset{
        "gcm-osx-x64-" + InVersion + ".tar.gz",
        "326fc3e1c708ae792b010b2f9f701f3dc4dbaebdf40cafd036254ab0f148c034"};
#else
    return std::nullopt;
#endif
#elif defined(__linux__)
#if defined(__aarch64__) || defined(_M_ARM64)
    return GcmAsset{
        "gcm-linux-arm64-" + InVersion + ".tar.gz",
        "cf3806b7528b5a5af16bd4bd0683202fc432d9008dd91d20c4c6744b24a033b5"};
#elif defined(__x86_64__) || defined(_M_X64)
    return GcmAsset{
        "gcm-linux-x64-" + InVersion + ".tar.gz",
        "31fc151c3b111ffe25616a4356bd9a50bdcdbd0922c5e11990fb220c6caf1ce1"};
#else
    return std::nullopt;
#endif
#else
    return std::nullopt;
#endif
}

auto UserInstallPath(const std::filesystem::path& InHome, const std::string& InVersion)
    -> std::filesystem::path {
    return InHome / ".local" / "share" / "kog" / "git-credential-manager" /
           InVersion / "git-credential-manager";
}

auto InstallGcm(const HttpsAuthOptions& InOptions,
                const std::filesystem::path& InHome)
    -> std::optional<std::filesystem::path> {
    const auto asset = PlatformAsset(InOptions.gcmVersion);
    if (!asset.has_value()) {
        if (InOptions.gcmVersion != kDefaultGcmVersion) {
            std::cerr << "Automatic installation only supports the checksum-pinned GCM "
                      << kDefaultGcmVersion
                      << ". Install another version separately and pass --gcm-path.\n";
            return std::nullopt;
        }
#if defined(_WIN32)
        std::cerr << "Automatic user-local GCM installation is not provided on Windows. "
                     "Install current Git for Windows, then rerun setup.\n";
#else
        std::cerr << "Automatic GCM installation is unsupported for this platform or architecture.\n";
#endif
        return std::nullopt;
    }

    const auto installPath = UserInstallPath(InHome, InOptions.gcmVersion);
    if (IsRunnableGcm(installPath)) {
        return installPath;
    }

    const auto installRoot = installPath.parent_path();
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto archivePath = std::filesystem::temp_directory_path() /
                             ("kog-gcm-" + std::to_string(stamp) + ".tar.gz");
    const auto downloadUrl =
        "https://github.com/git-ecosystem/git-credential-manager/releases/download/v" +
        InOptions.gcmVersion + "/" + asset->name;

    if (InOptions.dryRun) {
        std::cout << "[dry-run] download official GCM release: " << downloadUrl << "\n";
        std::cout << "[dry-run] verify SHA-256: " << asset->sha256 << "\n";
        std::cout << "[dry-run] install GCM under: " << installRoot.generic_string() << "\n";
        return installPath;
    }

    std::error_code ec;
    std::filesystem::create_directories(installRoot, ec);
    if (ec) {
        std::cerr << "Failed to create GCM install directory: " << ec.message() << "\n";
        return std::nullopt;
    }

    const auto download = shell::ExecuteCommand(
        "curl",
        {"-fL", downloadUrl, "-o", archivePath.string()},
        shell::ExecMode::PassThrough,
        std::filesystem::current_path());
    if (download.exitCode != 0) {
        std::filesystem::remove(archivePath, ec);
        std::cerr << "Failed to download the official GCM release archive.\n";
        return std::nullopt;
    }

#if defined(__APPLE__)
    const auto checksum = shell::ExecuteCommand(
        "shasum",
        {"-a", "256", archivePath.string()},
        shell::ExecMode::Capture,
        std::filesystem::current_path());
#else
    const auto checksum = shell::ExecuteCommand(
        "sha256sum",
        {archivePath.string()},
        shell::ExecMode::Capture,
        std::filesystem::current_path());
#endif
    std::istringstream checksumOutput(checksum.stdoutStr);
    std::string actualSha256;
    checksumOutput >> actualSha256;
    if (checksum.exitCode != 0 || actualSha256 != asset->sha256) {
        std::filesystem::remove(archivePath, ec);
        std::cerr << "GCM archive checksum verification failed; refusing extraction.\n";
        return std::nullopt;
    }

    const auto extract = shell::ExecuteCommand(
        "tar",
        {"-xzf", archivePath.string(), "-C", installRoot.string()},
        shell::ExecMode::PassThrough,
        std::filesystem::current_path());
    std::filesystem::remove(archivePath, ec);
    if (extract.exitCode != 0 || !IsRegularFile(installPath)) {
        std::cerr << "Failed to extract a usable Git Credential Manager executable.\n";
        return std::nullopt;
    }

    std::filesystem::permissions(
        installPath,
        std::filesystem::perms::owner_exec,
        std::filesystem::perm_options::add,
        ec);
    if (ec || !IsRunnableGcm(installPath)) {
        std::cerr << "Failed to make Git Credential Manager executable: " << ec.message() << "\n";
        return std::nullopt;
    }
    return installPath;
}

auto ValidateOptions(const HttpsAuthOptions& InOptions) -> bool {
    if (!IsValidHostname(InOptions.hostname)) {
        std::cerr << "Invalid --hostname. Pass a hostname without scheme, port, path, or wildcard.\n";
        return false;
    }
    if (InOptions.authMode != "pat" &&
        InOptions.authMode != "browser" &&
        InOptions.authMode != "basic") {
        std::cerr << "Invalid --auth-mode. Supported values: pat, browser, basic.\n";
        return false;
    }
    if (!IsValidVersion(InOptions.gcmVersion)) {
        std::cerr << "Invalid --gcm-version. Expected a numeric dotted release such as 2.9.1.\n";
        return false;
    }
    return true;
}

auto ConfigureGit(const HttpsAuthOptions& InOptions,
                  const std::filesystem::path& InGcmPath) -> int {
    const auto helperValue = InGcmPath.generic_string();
    const auto providerKey = "credential.https://" + InOptions.hostname + ".provider";
    const auto authModeKey =
        "credential.https://" + InOptions.hostname + ".gitLabAuthModes";
    const auto usernameKey = "credential.https://" + InOptions.hostname + ".username";

    std::cout << (InOptions.dryRun ? "[dry-run] " : "")
              << "GCM helper: " << helperValue << "\n";
    std::cout << (InOptions.dryRun ? "[dry-run] " : "")
              << "GitLab HTTPS host: " << InOptions.hostname
              << " auth-mode=" << InOptions.authMode << "\n";
    if (!InOptions.username.empty()) {
        std::cout << (InOptions.dryRun ? "[dry-run] " : "")
                  << "GitLab username: " << InOptions.username << "\n";
    }

    if (InOptions.dryRun) {
        return 0;
    }
    if (!InOptions.confirmGlobalWrite) {
        std::cerr << "Refusing to modify global Git configuration without "
                     "--confirm-global-write.\n";
        return 2;
    }

    const auto helpers = GlobalGitConfigValues("credential.helper");
    if (std::find(helpers.begin(), helpers.end(), helperValue) == helpers.end()) {
        const auto addHelper = GitCapture(
            {"config", "--global", "--add", "credential.helper", helperValue});
        if (addHelper.exitCode != 0) {
            std::cerr << "Failed to add the Git Credential Manager helper.\n";
            return 1;
        }
    }

    const auto setProvider = GitCapture(
        {"config", "--global", providerKey, "gitlab"});
    const auto setAuthMode = GitCapture(
        {"config", "--global", authModeKey, InOptions.authMode});
    if (setProvider.exitCode != 0 || setAuthMode.exitCode != 0) {
        std::cerr << "Failed to configure the GitLab HTTPS credential provider.\n";
        return 1;
    }
    if (!InOptions.username.empty()) {
        const auto setUsername = GitCapture(
            {"config", "--global", usernameKey, InOptions.username});
        if (setUsername.exitCode != 0) {
            std::cerr << "Failed to configure the GitLab HTTPS username.\n";
            return 1;
        }
    }

    std::cout << "GitLab HTTPS credential configuration completed for "
              << InOptions.hostname << ".\n";
    std::cout << "No token was created or printed. Provision a GitLab PAT through your "
                 "secret workflow, then verify with:\n";
    std::cout << "  kog auth test --url https://" << InOptions.hostname
              << "/GROUP/REPO.git\n";
    return 0;
}

auto IsConfiguredHelper(const std::filesystem::path& InGcmPath) -> bool {
    const auto helperValue = InGcmPath.generic_string();
    const auto helpers = GlobalGitConfigValues("credential.helper");
    return std::find(helpers.begin(), helpers.end(), helperValue) != helpers.end();
}

auto RunSetup(const std::shared_ptr<HttpsAuthOptions>& InOptions) -> void {
    if (!ValidateOptions(*InOptions)) {
        std::exit(2);
    }

    const auto home = ResolveHomeDirectory();
    if (!home.has_value()) {
        std::cerr << "Unable to resolve the current user home directory.\n";
        std::exit(1);
    }

    auto gcmPath = ResolveGcmPath(InOptions->gcmPath);
    if (!gcmPath.has_value()) {
        if (!InOptions->install) {
            std::cerr << "Git Credential Manager was not found. Retry with --install "
                         "or pass --gcm-path <path>.\n";
            std::exit(2);
        }
        if (!InOptions->dryRun && !InOptions->confirmGlobalWrite) {
            std::cerr << "Refusing user-local install and global Git configuration "
                         "without --confirm-global-write.\n";
            std::exit(2);
        }
        gcmPath = InstallGcm(*InOptions, *home);
    }
    if (!gcmPath.has_value()) {
        std::exit(1);
    }

    const auto result = ConfigureGit(*InOptions, *gcmPath);
    if (result != 0) {
        std::exit(result);
    }
}

auto RunDoctor(const std::shared_ptr<HttpsAuthOptions>& InOptions) -> void {
    if (!ValidateOptions(*InOptions)) {
        std::exit(2);
    }

    const auto gcmPath = ResolveGcmPath(InOptions->gcmPath);
    const auto providerKey = "credential.https://" + InOptions->hostname + ".provider";
    const auto authModeKey =
        "credential.https://" + InOptions->hostname + ".gitLabAuthModes";
    const auto usernameKey = "credential.https://" + InOptions->hostname + ".username";
    const auto provider = GlobalGitConfigValue(providerKey);
    const auto authMode = GlobalGitConfigValue(authModeKey);
    const auto username = GlobalGitConfigValue(usernameKey);
    const bool helperConfigured = gcmPath.has_value() && IsConfiguredHelper(*gcmPath);
    const bool authModeMatches =
        authMode.has_value() && authMode.value() == InOptions->authMode;
    const bool usernameMatches =
        InOptions->username.empty() ||
        (username.has_value() && username.value() == InOptions->username);
    const bool ready = gcmPath.has_value() &&
                       helperConfigured &&
                       provider.value_or("") == "gitlab" &&
                       authModeMatches &&
                       usernameMatches;

    std::cout << "hostname=" << InOptions->hostname << "\n";
    std::cout << "gcm="
              << (gcmPath.has_value() ? gcmPath->generic_string() : "missing")
              << "\n";
    std::cout << "credential_helper="
              << (helperConfigured ? "configured" : "missing") << "\n";
    std::cout << "provider=" << provider.value_or("missing") << "\n";
    std::cout << "auth_mode=" << authMode.value_or("missing") << "\n";
    std::cout << "auth_mode_expected=" << InOptions->authMode << "\n";
    std::cout << "auth_mode_match=" << (authModeMatches ? "true" : "false") << "\n";
    std::cout << "username=" << username.value_or("unset") << "\n";
    if (!InOptions->username.empty()) {
        std::cout << "username_expected=" << InOptions->username << "\n";
        std::cout << "username_match=" << (usernameMatches ? "true" : "false") << "\n";
    }
    std::cout << "status=" << (ready ? "ready" : "not-ready") << "\n";
    if (!ready) {
        std::exit(1);
    }
}

void AddCommonOptions(CLI::App& InCommand,
                      const std::shared_ptr<HttpsAuthOptions>& InOptions) {
    InCommand.add_option(
        "--hostname",
        InOptions->hostname,
        "GitLab HTTPS hostname without scheme or path")
        ->required();
    InCommand.add_option(
        "--username",
        InOptions->username,
        "Optional non-secret GitLab username stored in scoped Git config");
    InCommand.add_option(
        "--auth-mode",
        InOptions->authMode,
        "GitLab GCM authentication mode: pat, browser, or basic");
    InCommand.add_option(
        "--gcm-path",
        InOptions->gcmPath,
        "Explicit Git Credential Manager executable path");
    InCommand.add_option(
        "--gcm-version",
        InOptions->gcmVersion,
        "Pinned official GCM release used by --install");
}

} // namespace

void RegisterHttpsAuth(CLI::App& InAuth) {
    auto* command = InAuth.add_subcommand(
        "https",
        "Install and configure Git Credential Manager for a GitLab HTTPS host");

    const auto setupOptions = std::make_shared<HttpsAuthOptions>();
    auto* setup = command->add_subcommand(
        "setup",
        "Install GCM user-locally when requested and configure scoped GitLab HTTPS auth");
    AddCommonOptions(*setup, setupOptions);
    setup->add_flag(
        "--install",
        setupOptions->install,
        "Download an official user-local GCM tarball when missing");
    setup->add_flag(
        "--dry-run",
        setupOptions->dryRun,
        "Preview installation and Git configuration without writes");
    setup->add_flag(
        "--confirm-global-write",
        setupOptions->confirmGlobalWrite,
        "Confirm user-local installation and global Git configuration writes");
    setup->callback([setupOptions]() { RunSetup(setupOptions); });

    const auto doctorOptions = std::make_shared<HttpsAuthOptions>();
    auto* doctor = command->add_subcommand(
        "doctor",
        "Check GCM discovery and scoped GitLab HTTPS configuration");
    AddCommonOptions(*doctor, doctorOptions);
    doctor->callback([doctorOptions]() { RunDoctor(doctorOptions); });
}

} // namespace kano::git::commands
