// self command - launcher lifecycle helpers (update checks, install marker, sync)

#include <CLI/CLI.hpp>
#include "shell_executor.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <format>
#include <iomanip>
#include <iostream>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "build_info.hpp"

#if defined(_WIN32)
#include <io.h>
#else
#include <unistd.h>
#endif

namespace kano::git::commands {
namespace {

// ============================================================================
// Install State Schema (JSON format)
// ============================================================================
// Path: ~/.kano/git/kog-install-state.json (or $KANO_GIT_INSTALL_MARKER)
// Schema:
// {
//   "version": "0.1.0-beta",
//   "bin_path": "/path/to/kano-git",
//   "repo_path": "/path/to/kano-git-master-skill",
//   "installed_at": "2026-03-17T10:30:00Z",
//   "platform": "windows|linux|macos",
//   "arch": "x64|arm64"
// }
// ============================================================================

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

auto JsonEscape(const std::string_view InValue) -> std::string {
  std::string out;
  out.reserve(InValue.size() + 16);
  for (const char c : InValue) {
    switch (c) {
    case '\\': out += "\\\\"; break;
    case '"': out += "\\\""; break;
    case '\n': out += "\\n"; break;
    case '\r': out += "\\r"; break;
    case '\t': out += "\\t"; break;
    default: out.push_back(c); break;
    }
  }
  return out;
}

auto PlatformName() -> std::string {
#if defined(_WIN32)
  return "windows";
#elif defined(__APPLE__)
  return "macos";
#elif defined(__linux__)
  return "linux";
#else
  return "unknown";
#endif
}

auto ArchName() -> std::string {
#if defined(__aarch64__) || defined(_M_ARM64)
  return "arm64";
#else
  return "x64";
#endif
}

auto GetHomeDirectory() -> std::filesystem::path {
  if (const char* home = std::getenv("HOME"); home != nullptr && std::string(home).size() > 0) {
    return std::filesystem::path(home);
  }
  if (const char* userProfile = std::getenv("USERPROFILE"); userProfile != nullptr && std::string(userProfile).size() > 0) {
    return std::filesystem::path(userProfile);
  }
  // Fallback to current user's home via path expansion
  return std::filesystem::path("~");
}

auto Iso8601Timestamp() -> std::string {
  const auto now = std::chrono::system_clock::now();
  const auto time = std::chrono::system_clock::to_time_t(now);
  std::ostringstream oss;
  oss << std::put_time(std::gmtime(&time), "%Y-%m-%dT%H:%M:%SZ");
  return oss.str();
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

auto ReadTextFileTrimmed(const std::filesystem::path& InPath) -> std::string;

struct GitHubReleaseInfo {
    std::string tagName;
    std::string htmlUrl;
};

struct ParsedVersion {
    int major{0};
    int minor{0};
    int patch{0};
    std::vector<std::string> prereleaseParts;
};

auto ResolveInstallStatePath() -> std::filesystem::path {
    if (const char* state = std::getenv("KANO_GIT_INSTALL_STATE_FILE"); state != nullptr && std::string(state).size() > 0) {
        return std::filesystem::path(state).lexically_normal();
    }
    if (const char* marker = std::getenv("KANO_GIT_INSTALL_MARKER"); marker != nullptr && std::string(marker).size() > 0) {
        return std::filesystem::path(marker).lexically_normal();
    }
    if (const char* home = std::getenv("HOME"); home != nullptr && std::string(home).size() > 0) {
        return (std::filesystem::path(home) / ".kano" / "git" / "kog-install-state.json").lexically_normal();
    }
    return (std::filesystem::path(".") / ".kano" / "git" / "kog-install-state.json").lexically_normal();
}

auto ResolveInstallMarkerDir() -> std::filesystem::path {
  if (const char* markerDir = std::getenv("KANO_GIT_INSTALL_MARKER_DIR"); markerDir != nullptr && std::string(markerDir).size() > 0) {
    return std::filesystem::path(markerDir).lexically_normal();
  }
  return ResolveInstallStatePath().parent_path().lexically_normal();
}

auto ResolveInstallMarkerPath(const std::filesystem::path& InRepoRoot) -> std::filesystem::path {
  (void)InRepoRoot;
  return ResolveInstallStatePath();
}

auto IsPackagedInstall() -> bool {
    return std::filesystem::exists(ResolveInstallStatePath());
}

auto IsPackagedInstall(const std::filesystem::path& InRepoRoot) -> bool {
  return std::filesystem::exists(ResolveInstallMarkerPath(InRepoRoot));
}

auto ReadVersionFile(const std::filesystem::path& InRepoRoot) -> std::string {
  const auto versionPath = InRepoRoot / "VERSION";
  return ReadTextFileTrimmed(versionPath);
}

auto ReadPackagedVersionFile(const std::filesystem::path& InRepoRoot) -> std::string {
    const auto versionPath = (InRepoRoot / "scripts" / "package-version.txt").lexically_normal();
    return ReadTextFileTrimmed(versionPath);
}

auto ParseJsonStringField(const std::string& InJson, const std::string& InField) -> std::string {
    const auto key = std::format("\"{}\"", InField);
    const auto pos = InJson.find(key);
    if (pos == std::string::npos) {
        return {};
    }
    const auto colonPos = InJson.find(':', pos + key.size());
    if (colonPos == std::string::npos) {
        return {};
    }
    const auto firstQuote = InJson.find('"', colonPos + 1);
    if (firstQuote == std::string::npos) {
        return {};
    }

    std::string value;
    value.reserve(64);
    bool escaped = false;
    for (std::size_t index = firstQuote + 1; index < InJson.size(); ++index) {
        const char ch = InJson[index];
        if (escaped) {
            switch (ch) {
            case 'n': value.push_back('\n'); break;
            case 'r': value.push_back('\r'); break;
            case 't': value.push_back('\t'); break;
            case '\\': value.push_back('\\'); break;
            case '"': value.push_back('"'); break;
            default: value.push_back(ch); break;
            }
            escaped = false;
            continue;
        }
        if (ch == '\\') {
            escaped = true;
            continue;
        }
        if (ch == '"') {
            return value;
        }
        value.push_back(ch);
    }
    return {};
}

auto ParseMarkerField(const std::filesystem::path& InMarkerPath, const std::string& InField) -> std::string {
    std::ifstream in(InMarkerPath, std::ios::in | std::ios::binary);
    if (!in) {
        return {};
    }
    std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return ParseJsonStringField(content, InField);
}

auto NormalizeVersionTag(std::string InValue) -> std::string {
    auto value = Trim(std::move(InValue));
    if (!value.empty() && (value.front() == 'v' || value.front() == 'V')) {
        value.erase(value.begin());
    }
    const auto plusPos = value.find('+');
    if (plusPos != std::string::npos) {
        value.erase(plusPos);
    }
    return value;
}

auto SplitString(const std::string& InValue, char InDelimiter) -> std::vector<std::string> {
    std::vector<std::string> parts;
    std::stringstream ss(InValue);
    std::string token;
    while (std::getline(ss, token, InDelimiter)) {
        parts.push_back(token);
    }
    return parts;
}

auto IsAsciiDigits(const std::string& InValue) -> bool {
    return !InValue.empty() && std::all_of(InValue.begin(), InValue.end(), [](unsigned char ch) { return std::isdigit(ch) != 0; });
}

auto TryParseVersion(const std::string& InValue) -> std::optional<ParsedVersion> {
    const auto normalized = NormalizeVersionTag(InValue);
    if (normalized.empty()) {
        return std::nullopt;
    }

    ParsedVersion parsed;
    auto core = normalized;
    const auto dashPos = normalized.find('-');
    if (dashPos != std::string::npos) {
        core = normalized.substr(0, dashPos);
        parsed.prereleaseParts = SplitString(normalized.substr(dashPos + 1), '.');
    }

    const auto coreParts = SplitString(core, '.');
    if (coreParts.size() != 3) {
        return std::nullopt;
    }
    if (!IsAsciiDigits(coreParts[0]) || !IsAsciiDigits(coreParts[1]) || !IsAsciiDigits(coreParts[2])) {
        return std::nullopt;
    }

    try {
        parsed.major = std::stoi(coreParts[0]);
        parsed.minor = std::stoi(coreParts[1]);
        parsed.patch = std::stoi(coreParts[2]);
    } catch (...) {
        return std::nullopt;
    }

    return parsed;
}

auto ComparePrereleaseParts(const std::vector<std::string>& InLeft, const std::vector<std::string>& InRight) -> int {
    if (InLeft.empty() && InRight.empty()) {
        return 0;
    }
    if (InLeft.empty()) {
        return 1;
    }
    if (InRight.empty()) {
        return -1;
    }

    const auto count = std::max(InLeft.size(), InRight.size());
    for (std::size_t index = 0; index < count; ++index) {
        if (index >= InLeft.size()) {
            return -1;
        }
        if (index >= InRight.size()) {
            return 1;
        }
        const auto& left = InLeft[index];
        const auto& right = InRight[index];
        const bool leftNumeric = IsAsciiDigits(left);
        const bool rightNumeric = IsAsciiDigits(right);
        if (leftNumeric && rightNumeric) {
            const auto leftValue = std::stoll(left);
            const auto rightValue = std::stoll(right);
            if (leftValue < rightValue) {
                return -1;
            }
            if (leftValue > rightValue) {
                return 1;
            }
            continue;
        }
        if (leftNumeric != rightNumeric) {
            return leftNumeric ? -1 : 1;
        }
        if (left < right) {
            return -1;
        }
        if (left > right) {
            return 1;
        }
    }
    return 0;
}

auto CompareParsedVersions(const ParsedVersion& InLeft, const ParsedVersion& InRight) -> int {
    if (InLeft.major != InRight.major) {
        return InLeft.major < InRight.major ? -1 : 1;
    }
    if (InLeft.minor != InRight.minor) {
        return InLeft.minor < InRight.minor ? -1 : 1;
    }
    if (InLeft.patch != InRight.patch) {
        return InLeft.patch < InRight.patch ? -1 : 1;
    }
    return ComparePrereleaseParts(InLeft.prereleaseParts, InRight.prereleaseParts);
}

auto CompareVersionStrings(const std::string& InLeft, const std::string& InRight) -> std::optional<int> {
    const auto left = TryParseVersion(InLeft);
    const auto right = TryParseVersion(InRight);
    if (!left.has_value() || !right.has_value()) {
        return std::nullopt;
    }
    return CompareParsedVersions(*left, *right);
}

auto ReadInstalledVersion(const std::filesystem::path& InRepoRoot) -> std::string {
    auto version = ReadPackagedVersionFile(InRepoRoot);
    if (!version.empty()) {
        return version;
    }
    version = ReadVersionFile(InRepoRoot);
    if (!version.empty()) {
        return version;
    }
    return ParseMarkerField(ResolveInstallStatePath(), "version");
}

void PrintPackageUpdateSummary(const std::string& InCurrentVersion,
                               const std::string& InLatestVersion,
                               const std::string& InStatus,
                               const std::string& InGuidance) {
    std::cout << std::format("Current: {}\n", InCurrentVersion.empty() ? "unknown" : InCurrentVersion);
    std::cout << std::format("Latest: {}\n", InLatestVersion.empty() ? "unknown" : InLatestVersion);
    std::cout << std::format("Status: {}\n", InStatus.empty() ? "unknown" : InStatus);
    std::cout << std::format("Guidance: {}\n", InGuidance.empty() ? "Check the package channel for updates." : InGuidance);
}

auto DeterminePackageUpdateStatus(const std::string& InCurrentVersion,
                                  const std::string& InLatestVersion) -> std::pair<std::string, std::string> {
    if (InCurrentVersion.empty() || InLatestVersion.empty()) {
        return {"unable-to-check", "Version metadata is incomplete; inspect the published package channel manually."};
    }

    if (const auto comparison = CompareVersionStrings(InCurrentVersion, InLatestVersion); comparison.has_value()) {
        if (*comparison < 0) {
            return {"update-available", {}};
        }
        if (*comparison == 0) {
            return {"up-to-date", "No action needed."};
        }
        return {"ahead-of-stable", "Installed version is newer than the published stable release."};
    }

    if (InCurrentVersion == InLatestVersion) {
        return {"up-to-date", "No action needed."};
    }

    return {"unable-to-compare", "Version formats differ; inspect the published package channel manually."};
}

auto DefaultPackageReleaseRepo() -> std::string {
    if (const char* env = std::getenv("KANO_GIT_PACKAGE_RELEASE_REPO"); env != nullptr && std::string(env).size() > 0) {
        return Trim(env);
    }
    return "kanohorizonia/kano-git-master-skill";
}

auto BuildGitHubApiCommand(const std::string& InRepo) -> std::string {
    const auto apiUrl = std::format("https://api.github.com/repos/{}/releases/latest", InRepo);
#if defined(_WIN32)
    return std::format(
        "$ProgressPreference='SilentlyContinue'; "
        "$resp=Invoke-RestMethod -Headers @{{'Accept'='application/vnd.github+json';'X-GitHub-Api-Version'='2022-11-28'}} -Uri '{}' -Method Get; "
        "$resp | ConvertTo-Json -Depth 16 -Compress",
        apiUrl);
#else
    return std::format(
        "curl -fsSL -H 'Accept: application/vnd.github+json' -H 'X-GitHub-Api-Version: 2022-11-28' '{}'",
        apiUrl);
#endif
}

// Forward declaration for ExecuteBashLc (used in non-Windows branch)
auto ExecuteBashLc(const std::string& InCommand, shell::ExecMode InMode, const std::filesystem::path& InWorkdir) -> shell::ExecResult;

auto TryFetchLatestGitHubRelease(const std::string& InRepo, const std::filesystem::path& InWorkdir) -> std::optional<GitHubReleaseInfo> {
    if (InRepo.empty()) {
        return std::nullopt;
    }

    shell::ExecResult result;
#if defined(_WIN32)
    result = shell::ExecuteCommand("powershell", {"-NoProfile", "-NonInteractive", "-Command", BuildGitHubApiCommand(InRepo)}, shell::ExecMode::Capture, InWorkdir);
    if (result.exitCode != 0) {
        result = shell::ExecuteCommand("gh", {"api", std::format("repos/{}/releases/latest", InRepo)}, shell::ExecMode::Capture, InWorkdir);
    }
#else
    result = ExecuteBashLc(BuildGitHubApiCommand(InRepo), shell::ExecMode::Capture, InWorkdir);
    if (result.exitCode != 0) {
        result = shell::ExecuteCommand("gh", {"api", std::format("repos/{}/releases/latest", InRepo)}, shell::ExecMode::Capture, InWorkdir);
    }
#endif
    if (result.exitCode != 0) {
        return std::nullopt;
    }

    GitHubReleaseInfo info;
    info.tagName = ParseJsonStringField(result.stdoutStr, "tag_name");
    info.htmlUrl = ParseJsonStringField(result.stdoutStr, "html_url");
    if (info.tagName.empty()) {
        return std::nullopt;
    }
    return info;
}

auto FindKogLocation() -> std::string {
  if (const char* kogPath = std::getenv("KANO_GIT_BINARY_PATH"); kogPath != nullptr) {
    const std::filesystem::path p(kogPath);
    if (std::filesystem::exists(p)) {
      return p.generic_string();
    }
  }

#if defined(_WIN32)
  const auto result = shell::ExecuteCommand("where", {"kog"}, shell::ExecMode::Capture, std::filesystem::current_path());
#else
  const auto result = shell::ExecuteCommand("which", {"kog"}, shell::ExecMode::Capture, std::filesystem::current_path());
#endif

  if (result.exitCode == 0) {
    auto path = Trim(result.stdoutStr);
    const auto newlinePos = path.find('\n');
    if (newlinePos != std::string::npos) {
      path = path.substr(0, newlinePos);
    }
    return Trim(path);
  }

  return {};
}

auto FindKogRepoRoot() -> std::string {
  const auto kogPath = FindKogLocation();
  if (kogPath.empty()) {
    return {};
  }

  std::filesystem::path scriptPath(kogPath);
  if (!std::filesystem::exists(scriptPath)) {
    return {};
  }

  std::error_code ec;
  scriptPath = std::filesystem::canonical(scriptPath, ec);
  if (ec) {
    return {};
  }

  // Traverse up from binary location until we find .git or reach filesystem root
  // Binary is typically at: .../src/cpp/out/bin/[platform]/release/kano-git.exe
  // .git is at the skill root
  auto current = scriptPath.parent_path();
  while (!current.empty() && current != current.parent_path()) {
    const auto gitDir = current / ".git";
    if (std::filesystem::exists(gitDir)) {
      return current.generic_string();
    }
    current = current.parent_path();
  }

  return {};
}

auto GenerateInstallMarkerJson(const std::filesystem::path& InRepoRoot, const std::filesystem::path& InBinaryPath) -> std::string {
  const auto version = ReadVersionFile(InRepoRoot);
  const auto timestamp = Iso8601Timestamp();
  const auto platform = PlatformName();
  const auto arch = ArchName();

  std::ostringstream json;
  json << "{\n";
  json << "  \"version\": \"" << JsonEscape(version) << "\",\n";
  json << "  \"bin_path\": \"" << JsonEscape(InBinaryPath.generic_string()) << "\",\n";
  json << "  \"repo_path\": \"" << JsonEscape(InRepoRoot.generic_string()) << "\",\n";
  json << "  \"installed_at\": \"" << timestamp << "\",\n";
  json << "  \"platform\": \"" << platform << "\",\n";
  json << "  \"arch\": \"" << arch << "\"\n";
  json << "}\n";

  return json.str();
}

auto WriteInstallMarker(const std::filesystem::path& InRepoRoot, const std::filesystem::path& InBinaryPath) -> bool {
  const auto markerPath = ResolveInstallMarkerPath(InRepoRoot);
  const auto markerDir = markerPath.parent_path();

  std::error_code ec;
  std::filesystem::create_directories(markerDir, ec);
  if (ec) {
    std::cerr << "Error: Failed to create install state directory: " << markerDir.generic_string() << "\n";
    return false;
  }

  const auto jsonContent = GenerateInstallMarkerJson(InRepoRoot, InBinaryPath);

  std::ofstream out(markerPath, std::ios::out | std::ios::trunc | std::ios::binary);
  if (!out) {
    std::cerr << "Error: Failed to open install state file for writing: " << markerPath.generic_string() << "\n";
    return false;
  }

  out << jsonContent;

  std::cout << "Install state written to: " << markerPath.generic_string() << "\n";
  std::cout << jsonContent;

  return true;
}

auto ParseMarkerRepoPath(const std::filesystem::path& InMarkerPath) -> std::string {
  std::ifstream in(InMarkerPath, std::ios::in | std::ios::binary);
  if (!in) {
    return {};
  }

  std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

  const auto repoPathKey = "\"repo_path\"";
  const auto pos = content.find(repoPathKey);
  if (pos == std::string::npos) {
    return {};
  }

  const auto colonPos = content.find(':', pos);
  if (colonPos == std::string::npos) {
    return {};
  }

  const auto startQuote = content.find('"', colonPos);
  if (startQuote == std::string::npos) {
    return {};
  }

  std::string value;
  for (std::size_t i = startQuote + 1; i < content.size(); ++i) {
    if (content[i] == '\\' && i + 1 < content.size()) {
      switch (content[i + 1]) {
        case '"': value += '"'; ++i; break;
        case '\\': value += '\\'; ++i; break;
        case 'n': value += '\n'; ++i; break;
        case 'r': value += '\r'; ++i; break;
        case 't': value += '\t'; ++i; break;
        default: value += content[i]; break;
      }
    } else if (content[i] == '"') {
      break;
    } else {
      value += content[i];
    }
  }

  return value;
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

    const auto installedVersion = NormalizeVersionTag(ReadInstalledVersion(InRepoRoot));
    const auto releaseRepo = DefaultPackageReleaseRepo();
    if (const auto release = TryFetchLatestGitHubRelease(releaseRepo, InRepoRoot); release.has_value()) {
        const auto latestVersion = NormalizeVersionTag(release->tagName);
        const auto currentVersion = installedVersion;
        auto [status, comparisonGuidance] = DeterminePackageUpdateStatus(currentVersion, latestVersion);
        std::string guidance = release->htmlUrl.empty()
            ? std::format("Check GitHub Releases for {} manually.", releaseRepo)
            : std::format("Download the latest release from {}", release->htmlUrl);
        if (status == "up-to-date") {
            guidance = "No action needed.";
        } else if (status == "ahead-of-stable") {
            guidance = "Installed version is newer than the published stable release.";
        } else if (!comparisonGuidance.empty()) {
            guidance = comparisonGuidance;
        }

        PrintPackageUpdateSummary(currentVersion, latestVersion, status, guidance);

        if (InAutoUpdate) {
            std::cout << "Note: packaged installs do not auto-update in place; use the published package channel instead.\n";
        }
        return 0;
    }

    const auto checkCmd = Trim(InCheckCmd);
    if (!checkCmd.empty()) {
        const auto check = ExecuteBashLc(checkCmd, shell::ExecMode::Capture, InRepoRoot);
        if (check.exitCode == 0) {
            const auto latestVersion = NormalizeVersionTag(Trim(check.stdoutStr));
            auto [status, guidance] = DeterminePackageUpdateStatus(installedVersion, latestVersion);
            if (status == "update-available") {
                guidance = "A newer package version is available from the configured package channel.";
            } else if (status == "up-to-date") {
                guidance = "No action needed.";
            } else if (status == "ahead-of-stable") {
                guidance = "Installed version is newer than the configured package channel result.";
            }
            PrintPackageUpdateSummary(installedVersion, latestVersion, status, guidance);
            if (InAutoUpdate) {
                std::cout << "Note: packaged installs do not auto-update in place; use the published package channel instead.\n";
            }
            return 0;
        }
    }

    PrintPackageUpdateSummary(
        installedVersion,
        {},
        "unable-to-check",
        std::format("Unable to query GitHub Releases for {}; check the repository releases page manually.", releaseRepo));
    if (InAutoUpdate) {
        std::cout << "Note: packaged installs do not auto-update in place; use the published package channel instead.\n";
    }
    if (!Trim(InUpdateCmd).empty()) {
        std::cout << "Note: package update commands are advisory only in packaged mode; no in-place mutation was performed.\n";
    }
    return 0;
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

auto BuildInfoEntries() -> std::vector<std::pair<std::string, std::string>> {
    return {
        {"version", std::string(::kano::git::GetBuildVersion())},
        {"vcs", std::string(::kano::git::GetBuildVCS())},
        {"branch", std::string(::kano::git::GetBuildBranch())},
        {"rev", std::string(::kano::git::GetBuildRevision())},
        {"hash_short", std::string(::kano::git::GetBuildRevisionHashShort())},
        {"hash", std::string(::kano::git::GetBuildRevisionHash())},
        {"dirty", std::string(::kano::git::GetBuildDirty())},
        {"host", std::string(::kano::git::BuildHostName())},
        {"host_platform", std::string(::kano::git::BuildHostPlatform())},
        {"toolchain", std::string(::kano::git::GetBuildToolchain())},
        {"generator", std::string(::kano::git::GetBuildGenerator())},
        {"preset", std::string(::kano::git::GetBuildPreset())},
        {"config", std::string(::kano::git::GetBuildConfiguration())},
        {"ci", std::string(::kano::git::GetBuildCI())},
        {"context", std::string(::kano::git::GetBuildContext())},
        {"pipeline", std::string(::kano::git::GetBuildPipelineId())},
    };
}

auto BuildInfoJson() -> std::string {
    const auto entries = BuildInfoEntries();
    std::string out = "{";
    for (std::size_t i = 0; i < entries.size(); ++i) {
        if (i > 0) {
            out += ",";
        }
        out += "\"" + JsonEscape(entries[i].first) + "\":\"" + JsonEscape(entries[i].second) + "\"";
    }
    out += "}";
    return out;
}

} // namespace

void RegisterSelf(CLI::App& InApp) {
    auto* cmd = InApp.add_subcommand("self", "Launcher self-management commands");

    auto* selfVersion = cmd->add_subcommand("version", "Show launcher version and build information");
    auto* versionFormat = new std::string{"plain"};
    selfVersion->add_option("--format", *versionFormat, "Output format: plain|json")->default_str("plain");
    selfVersion->callback([=]() {
        if (*versionFormat == "json") {
            std::cout << BuildInfoJson() << "\n";
        } else {
            std::cout << kano::git::GetBuildInfo() << "\n";
        }
    });

    auto* installStatePath = cmd->add_subcommand("install-state-path", "Print packaged-install state file path");
    installStatePath->callback([=]() {
        std::cout << ResolveInstallStatePath().generic_string() << "\n";
    });

    auto* markerPath = cmd->add_subcommand("marker-path", "Print install state file path");
    auto* markerRepo = new std::string{"."};
    markerPath->add_option("--repo", *markerRepo, "Launcher project root path");
    markerPath->callback([=]() {
        const auto repoRoot = std::filesystem::weakly_canonical(std::filesystem::path(*markerRepo));
        std::cout << ResolveInstallMarkerPath(repoRoot).generic_string() << "\n";
    });

    auto* isPackaged = cmd->add_subcommand("is-packaged", "Exit 0 when install state file is present");
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
            modeValue = IsPackagedInstall() ? "package" : "dev";
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
        if (IsPackagedInstall()) {
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

  // ========================================================================
  // self upgrade/finalize-upgrade - Package-manager upgrade seams
  // ========================================================================
  auto* selfUpgrade = cmd->add_subcommand("upgrade", "Render Kano Git package upgrade steps");
  auto* selfUpgradeDryRun = new bool{true};
  auto* selfUpgradeYes = new bool{false};
  selfUpgrade->add_flag("--dry-run,!--no-dry-run", *selfUpgradeDryRun, "Render upgrade plan without invoking package-manager commands");
  selfUpgrade->add_flag("--yes", *selfUpgradeYes, "Required with --no-dry-run");
  selfUpgrade->callback([=]() {
    std::cout << "packageIdentifier=KanoHorizonia.KanoGit\n";
    std::cout << "packageName=Kano Git\n";
    std::cout << "moniker=kog\n";
    std::cout << "planCommand=winget upgrade --id KanoHorizonia.KanoGit -e\n";
    std::cout << "postUpgradeCommand=kog self finalize-upgrade\n";
    if (*selfUpgradeDryRun) {
      std::exit(0);
    }
    if (!*selfUpgradeYes) {
      std::cerr << "Error: SELF_UPGRADE_REQUIRES_YES\n";
      std::exit(1);
    }
    std::cerr << "Error: SELF_UPGRADE_EXECUTION_NOT_ENABLED_IN_THIS_BUILD\n";
    std::exit(1);
  });

  auto* selfFinalizeUpgrade = cmd->add_subcommand("finalize-upgrade", "Render post-upgrade skill finalization steps");
  auto* selfFinalizeDryRun = new bool{true};
  auto* selfFinalizeYes = new bool{false};
  selfFinalizeUpgrade->add_flag("--dry-run,!--no-dry-run", *selfFinalizeDryRun, "Render finalize plan without writing skill state");
  selfFinalizeUpgrade->add_flag("--yes", *selfFinalizeYes, "Required with --no-dry-run");
  selfFinalizeUpgrade->callback([=]() {
    std::cout << "planCommand=kog skill doctor\n";
    std::cout << "planCommand=kog skill install\n";
    if (*selfFinalizeDryRun) {
      std::exit(0);
    }
    if (!*selfFinalizeYes) {
      std::cerr << "Error: SELF_FINALIZE_UPGRADE_REQUIRES_YES\n";
      std::exit(1);
    }
    std::cerr << "Error: SELF_FINALIZE_UPGRADE_EXECUTION_NOT_ENABLED_IN_THIS_BUILD\n";
    std::exit(1);
  });

  // ========================================================================
  // self install - Generate/update install state
  // ========================================================================
  auto* selfInstall = cmd->add_subcommand("install", "Generate or update install state file");
  auto* installRepo = new std::string{"."};
  auto* installBinary = new std::string{};
  auto* installForce = new bool{false};

  selfInstall->add_option("--repo", *installRepo, "Path to kano-git-master-skill repo root");
  selfInstall->add_option("--binary", *installBinary, "Path to kano-git binary (auto-detected if not specified)");
  selfInstall->add_flag("--force", *installForce, "Overwrite existing install state");

  selfInstall->callback([=]() {
    const auto repoRoot = std::filesystem::weakly_canonical(std::filesystem::path(*installRepo));

    // Verify repo is valid
    const auto gitDir = repoRoot / ".git";
    if (!std::filesystem::exists(gitDir)) {
      std::cerr << "Error: Not a git repository: " << repoRoot.generic_string() << "\n";
      std::exit(1);
    }

    const auto versionFile = repoRoot / "VERSION";
    if (!std::filesystem::exists(versionFile)) {
      std::cerr << "Error: VERSION file not found in: " << repoRoot.generic_string() << "\n";
      std::cerr << "Hint: Ensure you're pointing to a valid kano-git-master-skill repo.\n";
      std::exit(1);
    }

    // Resolve binary path
    std::filesystem::path binaryPath;
    if (!installBinary->empty()) {
      binaryPath = std::filesystem::weakly_canonical(std::filesystem::path(*installBinary));
    } else {
      // Try to find binary in build directory
      const auto buildDir = repoRoot / "src" / "cpp" / "build";
      const auto presets = std::vector<std::string>{
        "windows-ninja-msvc", "linux-ninja-gcc", "macos-ninja-clang-arm64", "macos-ninja-clang-x64"
      };

      for (const auto& preset : presets) {
        std::filesystem::path candidate = buildDir / "bin" / preset / "release" /
#if defined(_WIN32)
          "kano-git.exe";
#else
          "kano-git";
#endif
        if (std::filesystem::exists(candidate)) {
          binaryPath = candidate;
          break;
        }
        candidate = buildDir / "bin" / preset / "debug" /
#if defined(_WIN32)
          "kano-git.exe";
#else
          "kano-git";
#endif
        if (std::filesystem::exists(candidate)) {
          binaryPath = candidate;
          break;
        }
      }

      if (binaryPath.empty()) {
        std::cerr << "Error: Could not find kano-git binary in build directory.\n";
        std::cerr << "Hint: Build the project first with 'kog self build'.\n";
        std::cerr << "Hint: Or specify binary path with --binary option.\n";
        std::exit(1);
      }
    }

    // Check for existing install state
    const auto markerPath = ResolveInstallMarkerPath(repoRoot);
    if (std::filesystem::exists(markerPath) && !*installForce) {
      std::cerr << "Error: Install state already exists: " << markerPath.generic_string() << "\n";
      std::cerr << "Hint: Use --force to overwrite.\n";
      std::exit(1);
    }

    // Write install state
    if (!WriteInstallMarker(repoRoot, binaryPath)) {
      std::cerr << "Error: Failed to write install state.\n";
      std::exit(1);
    }

    std::cout << "Install state created successfully.\n";
    std::exit(0);
  });

  // ========================================================================
  // self sync - Sync kog repo (fetch + rebase only, no rebuild)
  // ========================================================================
  auto* selfSync = cmd->add_subcommand("sync", "Sync the kog skill repo (fetch + rebase, equivalent to running kog sync inside kano-git-master-skill)");
  auto* syncRemote = new std::string{"upstream"};
  auto* syncBranch = new std::string{};
  auto* syncAutoRebase = new bool{false};
  auto* syncSkipRebuild = new bool{false};
  auto* syncNonInteractive = new bool{false};

  selfSync->add_option("--remote", *syncRemote, "Remote to sync from (default: upstream, fallback: origin)");
  selfSync->add_option("--branch", *syncBranch, "Branch to sync (default: remote HEAD)");
  selfSync->add_flag("--auto-rebase", *syncAutoRebase, "Automatically rebase without prompting");
  selfSync->add_flag("--skip-rebuild", *syncSkipRebuild, "(Deprecated) No-op; rebuild is no longer automatic. Use 'kog self build' to rebuild.");
  selfSync->add_flag("--non-interactive", *syncNonInteractive, "Disable interactive prompts");

  selfSync->callback([=]() {
    // Find kog repo root
    const auto kogRepoRoot = FindKogRepoRoot();
    if (kogRepoRoot.empty()) {
      std::cerr << "Error: Could not find kog repository.\n";
      std::cerr << "Hint: Ensure 'kog' is in your PATH and points to kano-git-master-skill.\n";
      std::exit(1);
    }

    const auto repoRoot = std::filesystem::path(kogRepoRoot);
    std::cout << "[self sync] Found kog repo: " << repoRoot.generic_string() << "\n";

    // Check for dirty worktree
    const auto dirty = shell::ExecuteCommand("git", {"status", "--porcelain"}, shell::ExecMode::Capture, repoRoot);
    if (dirty.exitCode == 0 && !Trim(dirty.stdoutStr).empty()) {
      std::cerr << "Error: Dirty worktree in " << repoRoot.generic_string() << "\n";
      std::cerr << "Hint: Commit, stash, or discard changes before running 'kog self sync'.\n";
      std::exit(1);
    }

    // Resolve remote
    std::string remote = *syncRemote;
    const auto upstreamCheck = shell::ExecuteCommand("git", {"remote", "get-url", remote}, shell::ExecMode::Capture, repoRoot);
    if (upstreamCheck.exitCode != 0) {
      // Fallback to origin
      remote = "origin";
      const auto originCheck = shell::ExecuteCommand("git", {"remote", "get-url", remote}, shell::ExecMode::Capture, repoRoot);
      if (originCheck.exitCode != 0) {
        std::cerr << "Error: No remote found (tried upstream and origin).\n";
        std::exit(1);
      }
    }

    std::cout << "[self sync] Using remote: " << remote << "\n";

    // Fetch
    std::cout << "[self sync] Fetching from " << remote << "...\n";
    const auto fetchResult = shell::ExecuteCommand("git", {"fetch", remote, "--prune"}, shell::ExecMode::PassThrough, repoRoot);
    if (fetchResult.exitCode != 0) {
      std::cerr << "Error: Failed to fetch from " << remote << "\n";
      std::exit(1);
    }

    // Resolve branch
    std::string branch = *syncBranch;
    if (branch.empty()) {
      // Try to get default branch from remote HEAD
      const auto remoteHead = shell::ExecuteCommand("git", {"symbolic-ref", "--quiet", "--short", "refs/remotes/" + remote + "/HEAD"}, shell::ExecMode::Capture, repoRoot);
      if (remoteHead.exitCode == 0) {
        branch = Trim(remoteHead.stdoutStr);
        // Strip remote prefix
        const auto prefix = remote + "/";
        if (branch.substr(0, prefix.size()) == prefix) {
          branch = branch.substr(prefix.size());
        }
      } else {
        // Fallback to current branch
        const auto currentBranch = shell::ExecuteCommand("git", {"rev-parse", "--abbrev-ref", "HEAD"}, shell::ExecMode::Capture, repoRoot);
        if (currentBranch.exitCode == 0) {
          branch = Trim(currentBranch.stdoutStr);
        } else {
          branch = "main";
        }
      }
    }

    std::cout << "[self sync] Target branch: " << branch << "\n";

    // Check if updates available
    const auto aheadCount = shell::ExecuteCommand("git", {"rev-list", "--count", "HEAD.." + remote + "/" + branch}, shell::ExecMode::Capture, repoRoot);
    int updates = 0;
    if (aheadCount.exitCode == 0) {
      try {
        updates = std::stoi(Trim(aheadCount.stdoutStr));
      } catch (...) {
        updates = 0;
      }
    }

    if (updates <= 0) {
      std::cout << "[self sync] Already up to date: " << remote << "/" << branch << "\n";
      std::exit(0);
    }

    std::cout << "[self sync] Found " << updates << " update(s) available.\n";

    // Confirm rebase (unless auto or non-interactive)
    bool shouldRebase = *syncAutoRebase;
    if (!shouldRebase && !*syncNonInteractive && IsInteractiveTerminal()) {
      shouldRebase = PromptYesNo(std::format("[self sync] Rebase onto {}/{}?", remote, branch));
    }

    if (!shouldRebase) {
      std::cout << "[self sync] Updates available but not applied.\n";
      std::cout << "Hint: Use --auto-rebase to apply updates automatically.\n";
      std::exit(0);
    }

    // Perform rebase
    std::cout << "[self sync] Rebasing onto " << remote << "/" << branch << "...\n";
    const auto rebaseResult = shell::ExecuteCommand("git", {"rebase", remote + "/" + branch}, shell::ExecMode::PassThrough, repoRoot);
    if (rebaseResult.exitCode != 0) {
      std::cerr << "Error: Rebase failed. Please resolve conflicts manually.\n";
      std::cerr << "Hint: After resolving, run 'git rebase --continue' and then 'kog self build'.\n";
      std::exit(1);
    }

    std::cout << "[self sync] Rebase completed successfully.\n";
    std::cout << "[self sync] Sync complete. Run 'kog self build' to rebuild the binary if needed.\n";
    std::exit(0);
  });
}

} // namespace kano::git::commands
