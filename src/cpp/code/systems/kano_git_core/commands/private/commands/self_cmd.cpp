// self command - launcher lifecycle helpers (update checks)

#include "command_registry.hpp"
#include "shell_executor.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <format>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <io.h>
#else
#include <unistd.h>
#endif

namespace kano::git::commands {
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

auto IsInteractiveTerminal() -> bool {
#if defined(_WIN32)
    return _isatty(_fileno(stdin)) != 0 && _isatty(_fileno(stdout)) != 0;
#else
    return isatty(fileno(stdin)) != 0 && isatty(fileno(stdout)) != 0;
#endif
}

auto PromptYesNo(const std::string& InPrompt) -> bool {
    std::cout << InPrompt << " [y/N]: " << std::flush;
    std::string answer;
    if (!std::getline(std::cin, answer)) {
        return false;
    }
    answer = Trim(answer);
    std::transform(answer.begin(), answer.end(), answer.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return answer == "y" || answer == "yes";
}

auto ParsePositiveInt(const std::string& InValue, int InFallback) -> int {
    try {
        const auto parsed = std::stoi(Trim(InValue));
        return parsed > 0 ? parsed : InFallback;
    } catch (...) {
        return InFallback;
    }
}

auto CurrentEpoch() -> std::int64_t {
    const auto now = std::chrono::system_clock::now();
    return std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();
}

auto FileMtimeEpoch(const std::filesystem::path& InPath) -> std::int64_t {
    std::error_code ec;
    const auto ft = std::filesystem::last_write_time(InPath, ec);
    if (ec) {
        return 0;
    }
    const auto sysNow = std::chrono::system_clock::now();
    const auto fileNow = std::chrono::file_clock::now();
    const auto adjusted = sysNow + (ft - fileNow);
    return std::chrono::duration_cast<std::chrono::seconds>(adjusted.time_since_epoch()).count();
}

auto ShouldRunIntervalCheck(const std::filesystem::path& InStamp, int InIntervalSeconds) -> bool {
    if (InIntervalSeconds <= 0) {
        return true;
    }
    const auto last = FileMtimeEpoch(InStamp);
    if (last <= 0) {
        return true;
    }
    const auto now = CurrentEpoch();
    return (now - last) >= static_cast<std::int64_t>(InIntervalSeconds);
}

void TouchStampFile(const std::filesystem::path& InStamp) {
    std::error_code ec;
    std::filesystem::create_directories(InStamp.parent_path(), ec);
    std::ofstream out(InStamp, std::ios::out | std::ios::trunc);
    out << "";
}

auto ResolveBinaryCommand() -> std::string {
    if (const char* binaryPath = std::getenv("KANO_GIT_BINARY_PATH"); binaryPath != nullptr) {
        const std::filesystem::path p(binaryPath);
        if (std::filesystem::exists(p)) {
            return p.generic_string();
        }
    }
#if defined(_WIN32)
    return "kano-git.exe";
#else
    return "kano-git";
#endif
}

auto ResolveInstallMarkerPath(const std::filesystem::path& InRepoRoot) -> std::filesystem::path {
    if (const char* marker = std::getenv("KANO_GIT_INSTALL_MARKER"); marker != nullptr && std::string(marker).size() > 0) {
        return std::filesystem::path(marker).lexically_normal();
    }
    return (InRepoRoot / ".kano-installed-marker").lexically_normal();
}

auto IsPackagedInstall(const std::filesystem::path& InRepoRoot) -> bool {
    return std::filesystem::exists(ResolveInstallMarkerPath(InRepoRoot));
}

auto ReadTextFileTrimmed(const std::filesystem::path& InPath) -> std::string {
    std::ifstream in(InPath, std::ios::in | std::ios::binary);
    if (!in) {
        return {};
    }
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return Trim(text);
}

auto ExecuteBashLc(const std::string& InCommand, shell::ExecMode InMode, const std::filesystem::path& InWorkdir) -> shell::ExecResult {
    return shell::ExecuteCommand("bash", {"-lc", InCommand}, InMode, InWorkdir);
}

auto RunDevUpdateCheck(const std::filesystem::path& InRepoRoot,
                       bool InNonInteractive,
                       bool InAutoUpdate) -> int {
    std::vector<std::string> args{"sync", "launcher-update-check", "--repo", InRepoRoot.generic_string()};
    if (InNonInteractive) {
        args.push_back("--non-interactive");
    }
    if (InAutoUpdate) {
        args.push_back("--auto-sync");
    }
    const auto run = shell::ExecuteCommand(ResolveBinaryCommand(), args, shell::ExecMode::PassThrough, InRepoRoot);
    return run.exitCode;
}

auto RunPackageUpdateCheck(const std::filesystem::path& InRepoRoot,
                           bool InNonInteractive,
                           bool InAutoUpdate,
                           bool InForce,
                           const std::string& InCheckCmd,
                           const std::string& InUpdateCmd,
                           int InIntervalSeconds) -> int {
    const auto stampFile = (InRepoRoot / ".kano" / "launcher" / "package-update-check.stamp").lexically_normal();
    if (!InForce && !ShouldRunIntervalCheck(stampFile, InIntervalSeconds)) {
        return 0;
    }
    TouchStampFile(stampFile);

    const auto checkCmd = Trim(InCheckCmd);
    if (checkCmd.empty()) {
        return 0;
    }

    const auto check = ExecuteBashLc(checkCmd, shell::ExecMode::Capture, InRepoRoot);
    if (check.exitCode != 0) {
        return 0;
    }
    const auto latestVersion = Trim(check.stdoutStr);
    if (latestVersion.empty()) {
        return 0;
    }

    const auto currentVersion = ReadTextFileTrimmed((InRepoRoot / "VERSION").lexically_normal());
    if (!currentVersion.empty() && latestVersion == currentVersion) {
        return 0;
    }

    bool shouldUpdate = InAutoUpdate;
    if (!shouldUpdate && !InNonInteractive && IsInteractiveTerminal()) {
        shouldUpdate = PromptYesNo(
            std::format("[launcher] New package version available ({} -> {}). Update now?",
                        currentVersion.empty() ? "unknown" : currentVersion,
                        latestVersion));
    }
    if (!shouldUpdate) {
        return 0;
    }

    const auto updateCmd = Trim(InUpdateCmd);
    if (updateCmd.empty()) {
        std::cerr << "[launcher] Set KANO_GIT_PACKAGE_UPDATE_CMD to enable one-click package updates.\n";
        return 0;
    }

    const auto update = ExecuteBashLc(updateCmd, shell::ExecMode::PassThrough, InRepoRoot);
    return update.exitCode;
}

auto IsUpdateSkipCommand(const std::string& InCommand) -> bool {
    static const std::vector<std::string> kSkip{
        "",
        "help",
        "-h",
        "--help",
        "-v",
        "--version",
        "version",
        "completion",
        "complete",
        "__complete",
        "sync",
        "tui",
        "discover",
        "remote",
        "slog",
        "log",
        "uplog",
        "plan",
        "ignore"};
    return std::find(kSkip.begin(), kSkip.end(), InCommand) != kSkip.end();
}

auto BuildInfoField(const std::string& InInfo, const std::string& InField) -> std::string {
    std::istringstream iss(InInfo);
    std::string token;
    while (iss >> token) {
        const auto pos = token.find('=');
        if (pos == std::string::npos) {
            continue;
        }
        if (token.substr(0, pos) == InField) {
            return token.substr(pos + 1);
        }
    }
    return {};
}

} // namespace

void RegisterSelf(CLI::App& InApp) {
    auto* cmd = InApp.add_subcommand("self", "Launcher self-management commands");
    auto* markerPath = cmd->add_subcommand("marker-path", "Print packaged-install marker path");
    auto* markerRepo = new std::string{"."};
    markerPath->add_option("--repo", *markerRepo, "Launcher project root path");
    markerPath->callback([=]() {
        const auto repoRoot = std::filesystem::weakly_canonical(std::filesystem::path(*markerRepo));
        std::cout << ResolveInstallMarkerPath(repoRoot).generic_string() << "\n";
    });

    auto* isPackaged = cmd->add_subcommand("is-packaged", "Exit 0 when packaged install marker is present");
    auto* packagedRepo = new std::string{"."};
    isPackaged->add_option("--repo", *packagedRepo, "Launcher project root path");
    isPackaged->callback([=]() {
        const auto repoRoot = std::filesystem::weakly_canonical(std::filesystem::path(*packagedRepo));
        std::exit(IsPackagedInstall(repoRoot) ? 0 : 1);
    });

    auto* updateCheck = cmd->add_subcommand("update-check", "Run launcher update checks (dev/package)");
    auto* maybeAutoUpdate = cmd->add_subcommand("maybe-auto-update", "Run auto update flow when launcher policy allows");
    auto* staleCheck = cmd->add_subcommand("stale-check", "Check whether binary should be rebuilt");

    auto* repo = new std::string{"."};
    auto* mode = new std::string{"auto"};
    auto* nonInteractive = new bool{false};
    auto* autoUpdate = new bool{false};
    auto* force = new bool{false};
    auto* intervalSeconds = new int{21600};
    auto* packageCheckCmd = new std::string{};
    auto* packageUpdateCmd = new std::string{};
    auto* staleRepo = new std::string{"."};
    auto* staleBinary = new std::string{};

    updateCheck->add_option("--repo", *repo, "Launcher project root path");
    updateCheck->add_option("--mode", *mode, "Mode: auto|dev|package")->default_str("auto");
    updateCheck->add_flag("--non-interactive", *nonInteractive, "Disable interactive prompts");
    updateCheck->add_flag("--auto-update", *autoUpdate, "Auto-apply update action when available");
    updateCheck->add_flag("--force", *force, "Ignore interval throttle for package mode");
    updateCheck->add_option("--interval-seconds", *intervalSeconds, "Package mode check interval seconds")->default_val(21600);
    updateCheck->add_option("--package-check-cmd", *packageCheckCmd, "Override package version check command");
    updateCheck->add_option("--package-update-cmd", *packageUpdateCmd, "Override package update command");
    staleCheck->add_option("--repo", *staleRepo, "Launcher project root path");
    staleCheck->add_option("--binary", *staleBinary, "Binary path to validate");

    updateCheck->callback([=]() {
        const auto repoRoot = std::filesystem::weakly_canonical(std::filesystem::path(*repo));
        auto modeValue = Trim(*mode);
        std::transform(modeValue.begin(), modeValue.end(), modeValue.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });

        if (modeValue == "auto") {
            modeValue = IsPackagedInstall(repoRoot) ? "package" : "dev";
        }

        if (modeValue == "dev") {
            std::exit(RunDevUpdateCheck(repoRoot, *nonInteractive, *autoUpdate));
        }
        if (modeValue == "package") {
            std::string checkCmd = *packageCheckCmd;
            if (checkCmd.empty()) {
                if (const char* env = std::getenv("KANO_GIT_PACKAGE_VERSION_CHECK_CMD"); env != nullptr) {
                    checkCmd = env;
                }
            }
            std::string updateCmd = *packageUpdateCmd;
            if (updateCmd.empty()) {
                if (const char* env = std::getenv("KANO_GIT_PACKAGE_UPDATE_CMD"); env != nullptr) {
                    updateCmd = env;
                }
            }
            int interval = *intervalSeconds;
            if (const char* env = std::getenv("KANO_GIT_PACKAGE_UPDATE_CHECK_INTERVAL_SECONDS"); env != nullptr) {
                interval = ParsePositiveInt(env, interval);
            }
            std::exit(RunPackageUpdateCheck(repoRoot, *nonInteractive, *autoUpdate, *force, checkCmd, updateCmd, interval));
        }

        std::cerr << "Error: invalid --mode value: " << modeValue << " (expected auto|dev|package)\n";
        std::exit(2);
    });

    auto* maybeRepo = new std::string{"."};
    auto* maybeCommand = new std::string{};
    maybeAutoUpdate->add_option("--repo", *maybeRepo, "Launcher project root path");
    maybeAutoUpdate->add_option("--command", *maybeCommand, "Current user command (for skip policy)")->default_str("");
    maybeAutoUpdate->callback([=]() {
        const auto autoCheck = std::string(std::getenv("KANO_GIT_AUTO_UPDATE_CHECK") != nullptr ? std::getenv("KANO_GIT_AUTO_UPDATE_CHECK") : "1");
        if (autoCheck != "1") {
            std::exit(0);
        }
        const auto agentMode = std::string(std::getenv("KANO_AGENT_MODE") != nullptr ? std::getenv("KANO_AGENT_MODE") : "0");
        if (agentMode == "1") {
            std::exit(0);
        }
        const auto command = Trim(*maybeCommand);
        if (IsUpdateSkipCommand(command)) {
            std::exit(0);
        }

        const auto repoRoot = std::filesystem::weakly_canonical(std::filesystem::path(*maybeRepo));
        if (IsPackagedInstall(repoRoot)) {
            std::string checkCmd;
            if (const char* env = std::getenv("KANO_GIT_PACKAGE_VERSION_CHECK_CMD"); env != nullptr) {
                checkCmd = env;
            }
            std::string updateCmd;
            if (const char* env = std::getenv("KANO_GIT_PACKAGE_UPDATE_CMD"); env != nullptr) {
                updateCmd = env;
            }
            int interval = 21600;
            if (const char* env = std::getenv("KANO_GIT_PACKAGE_UPDATE_CHECK_INTERVAL_SECONDS"); env != nullptr) {
                interval = ParsePositiveInt(env, interval);
            }
            std::exit(RunPackageUpdateCheck(repoRoot, false, false, false, checkCmd, updateCmd, interval));
        }

        if (!IsInteractiveTerminal()) {
            std::exit(0);
        }
        const auto gitCheck = shell::ExecuteCommand("git", {"rev-parse", "--is-inside-work-tree"}, shell::ExecMode::Capture, repoRoot);
        if (gitCheck.exitCode != 0) {
            std::exit(0);
        }
        std::exit(RunDevUpdateCheck(repoRoot, false, false));
    });

    staleCheck->callback([=]() {
        const auto repoRoot = std::filesystem::weakly_canonical(std::filesystem::path(*staleRepo));
        const auto binaryPath = std::filesystem::path(*staleBinary).lexically_normal();
        if (staleBinary->empty() || !std::filesystem::exists(binaryPath)) {
            std::cout << "direction=unknown reason=binary missing\n";
            std::exit(0);
        }

        const auto skipWhenDirty =
            std::string(std::getenv("KOG_SKIP_AUTO_REBUILD_WHEN_DIRTY") != nullptr ? std::getenv("KOG_SKIP_AUTO_REBUILD_WHEN_DIRTY") : "1");
        if (skipWhenDirty == "1") {
            const auto dirty = shell::ExecuteCommand("git", {"status", "--porcelain"}, shell::ExecMode::Capture, repoRoot);
            if (dirty.exitCode == 0 && !Trim(dirty.stdoutStr).empty()) {
                std::cout << "direction=skip reason=dirty worktree\n";
                std::exit(1);
            }
        }

        const auto buildInfo = shell::ExecuteCommand(binaryPath.generic_string(), {"version"}, shell::ExecMode::Capture, repoRoot);
        const auto gitHash = Trim(shell::ExecuteCommand("git", {"rev-parse", "--short", "HEAD"}, shell::ExecMode::Capture, repoRoot).stdoutStr);
        const auto gitBranch = Trim(shell::ExecuteCommand("git", {"rev-parse", "--abbrev-ref", "HEAD"}, shell::ExecMode::Capture, repoRoot).stdoutStr);
        const auto gitRevision =
            Trim(shell::ExecuteCommand("git", {"rev-list", "--count", "--first-parent", "HEAD"}, shell::ExecMode::Capture, repoRoot).stdoutStr);

        const auto buildHash = BuildInfoField(buildInfo.stdoutStr, "hash_short");
        const auto buildBranch = BuildInfoField(buildInfo.stdoutStr, "branch");
        const auto buildRevision = BuildInfoField(buildInfo.stdoutStr, "rev");

        std::string direction = "unknown";
        try {
            if (!gitRevision.empty() && !buildRevision.empty()) {
                const auto gr = std::stoi(gitRevision);
                const auto br = std::stoi(buildRevision);
                if (gr > br) {
                    direction = "upgrade";
                } else if (gr < br) {
                    direction = "downgrade";
                } else {
                    direction = "same-revision";
                }
            }
        } catch (...) {
        }

        if (!gitHash.empty() && !buildHash.empty() && buildHash != "unknown" && gitHash != buildHash) {
            std::cout << std::format(
                "direction={} reason=hash mismatch: binary(branch={}, rev={}, hash={}) vs git(branch={}, rev={}, hash={})\n",
                direction,
                buildBranch.empty() ? "unknown" : buildBranch,
                buildRevision.empty() ? "unknown" : buildRevision,
                buildHash.empty() ? "unknown" : buildHash,
                gitBranch.empty() ? "unknown" : gitBranch,
                gitRevision.empty() ? "unknown" : gitRevision,
                gitHash.empty() ? "unknown" : gitHash);
            std::exit(0);
        }

        const auto versionFile = (repoRoot / "VERSION").lexically_normal();
        const auto cmakeFile = (repoRoot / "src" / "cpp" / "CMakeLists.txt").lexically_normal();
        const auto presetFile = (repoRoot / "src" / "cpp" / "CMakePresets.json").lexically_normal();
        std::error_code ec;
        const auto binaryTime = std::filesystem::last_write_time(binaryPath, ec);
        if (ec) {
            std::cout << "direction=unknown reason=binary timestamp unreadable\n";
            std::exit(0);
        }
        const auto newer = [&](const std::filesystem::path& p) -> bool {
            std::error_code iec;
            if (!std::filesystem::exists(p, iec) || iec) {
                return false;
            }
            std::error_code tec;
            const auto t = std::filesystem::last_write_time(p, tec);
            if (tec) {
                return false;
            }
            return t > binaryTime;
        };
        if (newer(versionFile)) {
            std::cout << "direction=unknown reason=VERSION file is newer than binary\n";
            std::exit(0);
        }
        if (newer(cmakeFile)) {
            std::cout << "direction=unknown reason=CMakeLists changed after binary build\n";
            std::exit(0);
        }
        if (newer(presetFile)) {
            std::cout << "direction=unknown reason=CMakePresets changed after binary build\n";
            std::exit(0);
        }

        std::cout << "direction=same-revision reason=up-to-date\n";
        std::exit(1);
    });
}

} // namespace kano::git::commands
