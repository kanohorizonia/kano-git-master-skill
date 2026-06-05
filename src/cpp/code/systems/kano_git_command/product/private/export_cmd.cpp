// export_cmd.cpp — kog export command implementation
//
// Packages a multi-repo workspace into a set of archive files, one per
// repository. Follows the anonymous-namespace helper pattern from status_cmd.cpp.

#include "export_helpers.hpp"

#include <CLI/CLI.hpp>
#include "discovery.hpp"
#include "shell_executor.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <format>
#include <fstream>
#include <iomanip>
#include <map>
#include <iostream>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace kano::git::commands {

// ---------------------------------------------------------------------------
// Pure helper function implementations
// ---------------------------------------------------------------------------

namespace {

auto NormalizeFilterPath(std::string InValue) -> std::string {
    std::replace(InValue.begin(), InValue.end(), '\\', '/');
    while (InValue.starts_with("./")) {
        InValue.erase(0, 2);
    }
    while (!InValue.empty() && InValue.front() == '/') {
        InValue.erase(InValue.begin());
    }
    while (InValue.size() > 1 && InValue.ends_with('/')) {
        InValue.pop_back();
    }
    return InValue;
}

auto ContainsGlobMagic(const std::string& InValue) -> bool {
    return InValue.find('*') != std::string::npos || InValue.find('?') != std::string::npos;
}

auto GlobToRegex(const std::string& InPattern) -> std::regex {
    std::string regexText = "^";
    for (std::size_t i = 0; i < InPattern.size(); ++i) {
        const char ch = InPattern[i];
        if (ch == '*') {
            const bool isDoubleStar = (i + 1 < InPattern.size() && InPattern[i + 1] == '*');
            if (isDoubleStar) {
                regexText += ".*";
                ++i;
            } else {
                regexText += "[^/]*";
            }
            continue;
        }
        if (ch == '?') {
            regexText += "[^/]";
            continue;
        }
        if (ch == '.' || ch == '^' || ch == '$' || ch == '|' || ch == '(' || ch == ')' ||
            ch == '[' || ch == ']' || ch == '{' || ch == '}' || ch == '+' || ch == '\\') {
            regexText.push_back('\\');
        }
        regexText.push_back(ch);
    }
    regexText += "$";
    return std::regex(regexText, std::regex::ECMAScript);
}

auto PathMatchesFilter(std::string InPath, std::string InFilter) -> bool {
    InPath = NormalizeFilterPath(std::move(InPath));
    InFilter = NormalizeFilterPath(std::move(InFilter));
    if (InPath.empty() || InFilter.empty()) {
        return false;
    }

    if (!ContainsGlobMagic(InFilter)) {
        return InPath == InFilter || InPath.starts_with(InFilter + "/");
    }

    return std::regex_match(InPath, GlobToRegex(InFilter));
}

auto HasPathFilters(const ExportOptions& InOpts) -> bool {
    return !InOpts.includePaths.empty() || !InOpts.excludePaths.empty();
}

auto ShouldIncludePathByFilters(const std::string& InRepoRelativePath,
                                const ExportOptions& InOpts) -> bool {
    const std::string normalizedPath = NormalizeFilterPath(InRepoRelativePath);
    bool includeMatched = InOpts.includePaths.empty();
    for (const auto& include : InOpts.includePaths) {
        if (PathMatchesFilter(normalizedPath, include)) {
            includeMatched = true;
            break;
        }
    }
    if (!includeMatched) {
        return false;
    }

    for (const auto& exclude : InOpts.excludePaths) {
        if (PathMatchesFilter(normalizedPath, exclude)) {
            for (const auto& include : InOpts.includePaths) {
                if (PathMatchesFilter(normalizedPath, include)) {
                    return true;
                }
            }
            return false;
        }
    }

    return true;
}

} // anonymous namespace

auto ValidateOptions(const ExportOptions& InOpts) -> bool {
    bool ok = true;
    if (InOpts.format != "tar" && InOpts.format != "zip") {
        std::cerr << "kog export: invalid --format value '" << InOpts.format
                  << "'; must be 'tar' or 'zip'\n";
        ok = false;
    }
    if (InOpts.source != "head" && InOpts.source != "working-tree") {
        std::cerr << "kog export: invalid --source value '" << InOpts.source
                  << "'; must be 'head' or 'working-tree'\n";
        ok = false;
    }
    if (InOpts.revPad <= 0) {
        std::cerr << "kog export: invalid --rev-pad value " << InOpts.revPad
                  << "; must be a positive integer\n";
        ok = false;
    }
    if (InOpts.splitSubrepoDepth < 0) {
        std::cerr << "kog export: invalid --split-subrepo-depth value " << InOpts.splitSubrepoDepth
                  << "; must be a non-negative integer\n";
        ok = false;
    }
    if (!InOpts.validateReleaseArchive && InOpts.forceValidateReleaseArchive) {
        std::cerr << "kog export: --validate-release-archive conflicts with "
                  << "--no-validate-release-archive\n";
        ok = false;
    }
    if (!InOpts.subtreePath.empty()) {
        if (InOpts.single) {
            std::cerr << "kog export: --subtree cannot be combined with --single\n";
            ok = false;
        }
        if (InOpts.includeSubmoduleStubs) {
            std::cerr << "kog export: --subtree cannot be combined with --include-submodule-stubs\n";
            ok = false;
        }
        if (InOpts.forceValidateReleaseArchive) {
            std::cerr << "kog export: --validate-release-archive is not supported with --subtree\n";
            ok = false;
        }
        if (InOpts.splitSubrepoDepth > 0) {
            std::cerr << "kog export: --split-subrepo-depth cannot be combined with --subtree\n";
            ok = false;
        }
    }
    if (InOpts.includeSubrepos && !InOpts.single) {
        std::cerr << "kog export: --include-subrepos requires --single\n";
        ok = false;
    }
    if (InOpts.allowMissingSubrepos && !InOpts.includeSubrepos) {
        std::cerr << "kog export: --allow-missing-subrepos requires --include-subrepos\n";
        ok = false;
    }
    return ok;
}

auto ResolveOutputDir(const std::filesystem::path& InCwd,
                      const std::string& InExplicit) -> std::filesystem::path {
    if (!InExplicit.empty()) {
        return std::filesystem::path(InExplicit);
    }
    return InCwd / ".kano" / "tmp" / "git" / "export";
}

auto FormatRevision(int InRevision, int InPad) -> std::string {
    std::ostringstream oss;
    oss << std::setw(InPad) << std::setfill('0') << InRevision;
    return oss.str();
}

auto ComputeArchiveName(const std::string& InRepoName,
                        const std::string& InRevision,
                        const std::string& InFormat) -> std::string {
    return InRepoName + "_rev" + InRevision + "." + InFormat;
}

auto ComputePrefix(const std::string& InRepoName,
                   const std::string& InExplicitPrefix) -> std::string {
    if (!InExplicitPrefix.empty()) {
        return NormalizeArchivePrefix(InExplicitPrefix);
    }
    return NormalizeArchivePrefix(InRepoName);
}

auto NormalizeArchivePrefix(const std::string& InPrefix) -> std::string {
    if (InPrefix.empty()) {
        return InPrefix;
    }
    std::string out = InPrefix;
    if (out.back() != '/') {
        out.push_back('/');
    }
    return out;
}

auto BuildGitArchiveArgs(const std::string& InFormat,
                         const std::string& InPrefix,
                         const std::filesystem::path& InOutputPath) -> std::vector<std::string> {
    return {
        "archive",
        "HEAD",
        "--format=" + InFormat,
        "--prefix=" + InPrefix,
        "--output=" + InOutputPath.generic_string()
    };
}

auto BuildGitArchiveArgsForSubtree(const std::string& InFormat,
                                   const std::string& InPrefix,
                                   const std::filesystem::path& InOutputPath,
                                   const std::string& InRepoRelativeSubtree,
                                   bool InKeepSubtreePath) -> std::vector<std::string> {
    if (InKeepSubtreePath) {
        return {
            "archive",
            "HEAD",
            "--format=" + InFormat,
            "--prefix=" + InPrefix,
            "--output=" + InOutputPath.generic_string(),
            InRepoRelativeSubtree
        };
    }

    return {
        "archive",
        "HEAD:" + InRepoRelativeSubtree,
        "--format=" + InFormat,
        "--prefix=" + InPrefix,
        "--output=" + InOutputPath.generic_string()
    };
}

auto CollectWorkingTreeFilesForSubtree(const std::filesystem::path& InRepoPath,
                                       const std::filesystem::path& InRepoRelativeSubtree,
                                       bool InKeepSubtreePath,
                                       const ShellExecutor& InExec) -> std::vector<std::string> {
    const std::string subtreeRel = InRepoRelativeSubtree.generic_string();
    const auto lsRes = InExec("git", {"ls-files", "--cached", "--others", "--exclude-standard", "--", subtreeRel}, shell::ExecMode::Capture, InRepoPath);
    if (lsRes.exitCode != 0) {
        return {};
    }

    std::vector<std::string> out;
    std::istringstream iss(lsRes.stdoutStr);
    std::string relPath;
    const std::string stripPrefix = subtreeRel + "/";
    while (std::getline(iss, relPath)) {
        if (!relPath.empty() && relPath.back() == '\r') {
            relPath.pop_back();
        }
        if (relPath.empty()) {
            continue;
        }

        std::string arcRel = relPath;
        if (!InKeepSubtreePath && arcRel.rfind(stripPrefix, 0) == 0) {
            arcRel = arcRel.substr(stripPrefix.size());
        }
        if (arcRel.empty()) {
            continue;
        }
        out.push_back(arcRel);
    }

    return out;
}


auto CollectWorkingTreeFiles(const ExportRecord& InRecord, const std::string& InPrefix, bool InSingle, const ShellExecutor& InExec) -> std::vector<std::string> {
    std::vector<std::string> allFiles;

    auto getFiles = [&](const std::filesystem::path& repoPath, const std::string& prefix, const std::vector<std::string>& skipList = {}) {
        const auto res = InExec("git", {"ls-files", "--cached", "--others", "--exclude-standard"}, shell::ExecMode::Capture, repoPath);
        if (res.exitCode == 0) {
            std::istringstream iss(res.stdoutStr);
            std::string line;
            while (std::getline(iss, line)) {
                if (!line.empty() && line.back() == '\r') {
                    line.pop_back();
                }
                if (!line.empty()) {
                    // Skip git metadata
                    if (line == ".git" || line.starts_with(".git/") || line.starts_with(".git\\") ||
                        line.find("/.git/") != std::string::npos || line.find("\\.git\\") != std::string::npos) {
                        continue;
                    }
                    // Skip kog workspace metadata/cache
                    if (line == ".kano" || line.starts_with(".kano/") || line.starts_with(".kano\\") ||
                        line.find("/.kano/") != std::string::npos || line.find("\\.kano\\") != std::string::npos) {
                        continue;
                    }
                    // Skip submodules if we are in single mode (we will add their contents explicitly)
                    if (InSingle && std::find(skipList.begin(), skipList.end(), line) != skipList.end()) {
                        continue;
                    }
                    allFiles.push_back(prefix + line);
                }
            }
        }
    };

    // Prepare skip list for root repo
    std::vector<std::string> rootSkips;
    if (InSingle) {
        for (const auto& p : InRecord.submodulePaths) {
            rootSkips.push_back(p.generic_string());
        }
    }

    // Root repo files
    getFiles(InRecord.repoPath, InPrefix, rootSkips);

    // If single mode, recursively add submodule files
    if (InSingle) {
        for (const auto& subRelPath : InRecord.submodulePaths) {
            const std::string subPrefix = InPrefix + subRelPath.generic_string() + "/";
            getFiles(InRecord.repoPath / subRelPath, subPrefix);
        }
    }

    // Final deduplication
    std::sort(allFiles.begin(), allFiles.end());
    allFiles.erase(std::unique(allFiles.begin(), allFiles.end()), allFiles.end());

    // Move symlinks to the end so tar creates their targets before the links.
    // This prevents "Cannot create symlink: No such file or directory" when
    // the symlink target sorts alphabetically after the symlink itself.
    std::stable_partition(allFiles.begin(), allFiles.end(),
        [&](const std::string& InPrefixedPath) {
            // Strip the archive prefix to get the repo-relative path
            const std::string relPath = (InPrefixedPath.size() > InPrefix.size())
                ? InPrefixedPath.substr(InPrefix.size())
                : InPrefixedPath;
            // Check against root repo path; submodule files use their own prefix
            // but we only need to detect symlinks — false negatives are safe here.
            std::error_code ec;
            const auto fullPath = InRecord.repoPath / relPath;
            return !std::filesystem::is_symlink(fullPath, ec);
        });

    return allFiles;
}

auto CollectGitIndexEntriesWithPrefix(const std::filesystem::path& InRepoPath,
                                      const std::string& InPathPrefix,
                                      const ShellExecutor& InExec,
                                      std::vector<std::string>& OutCacheInfoLines) -> bool {
    const auto res = InExec("git", {"ls-files", "-s"}, shell::ExecMode::Capture, InRepoPath);
    if (res.exitCode != 0) {
        return false;
    }

    std::istringstream iss(res.stdoutStr);
    std::string line;
    while (std::getline(iss, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty()) {
            continue;
        }

        const auto modeEnd = line.find(' ');
        const auto space2 = (modeEnd == std::string::npos) ? std::string::npos : line.find(' ', modeEnd + 1);
        const auto tabPos = line.find('\t');
        if (modeEnd == std::string::npos || space2 == std::string::npos || tabPos == std::string::npos || tabPos + 1 >= line.size()) {
            continue;
        }

        const std::string mode = line.substr(0, modeEnd);
        const std::string oid = line.substr(modeEnd + 1, space2 - (modeEnd + 1));
        const std::string stage = line.substr(space2 + 1, tabPos - (space2 + 1));
        const std::string relPath = line.substr(tabPos + 1);
        if (mode.empty() || oid.empty() || stage.empty() || relPath.empty()) {
            continue;
        }

        OutCacheInfoLines.push_back(mode + " " + oid + " " + stage + "\t" + InPathPrefix + relPath);
    }
    return true;
}

auto CollectExecutablePathsWithPrefix(const std::filesystem::path& InRepoPath,
                                      const std::string& InPathPrefix,
                                      const ShellExecutor& InExec,
                                      std::map<std::string, int>& OutModeByPath) -> bool {
    const auto res = InExec("git", {"ls-files", "-s"}, shell::ExecMode::Capture, InRepoPath);
    if (res.exitCode != 0) {
        return false;
    }

    std::istringstream iss(res.stdoutStr);
    std::string line;
    while (std::getline(iss, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty()) {
            continue;
        }
        const auto modeEnd = line.find(' ');
        const auto tabPos = line.find('\t');
        if (modeEnd == std::string::npos || tabPos == std::string::npos || tabPos + 1 >= line.size()) {
            continue;
        }
        const std::string mode = line.substr(0, modeEnd);
        const std::string relPath = line.substr(tabPos + 1);
        if (relPath.empty()) {
            continue;
        }
        int parsedMode = 0644;
        if (mode == "100755") {
            parsedMode = 0755;
        } else if (mode == "120000") {
            parsedMode = 0120000;
        }
        OutModeByPath[InPathPrefix + relPath] = parsedMode;
    }
    return true;
}

auto ShouldNormalizePathToLfByAttributes(const std::filesystem::path& InRepoPath,
                                         const std::string& InRepoRelativePath,
                                         const ShellExecutor& InExec) -> bool {
    if (InRepoRelativePath.empty()) {
        return false;
    }

    const auto res = InExec(
        "git",
        {"check-attr", "text", "eol", "--", InRepoRelativePath},
        shell::ExecMode::Capture,
        InRepoPath);
    if (res.exitCode != 0) {
        return false;
    }

    bool hasEolLf = false;
    bool textUnset = false;

    std::istringstream iss(res.stdoutStr);
    std::string line;
    while (std::getline(iss, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty()) {
            continue;
        }

        // git check-attr text eol -- <path> returns lines like:
        //   <path>: text: set
        //   <path>: eol: lf
        const auto firstSep = line.find(": ");
        if (firstSep == std::string::npos) {
            continue;
        }
        const auto secondSep = line.find(": ", firstSep + 2);
        if (secondSep == std::string::npos) {
            continue;
        }

        const std::string attr = line.substr(firstSep + 2, secondSep - (firstSep + 2));
        const std::string value = line.substr(secondSep + 2);

        if (attr == "eol" && value == "lf") {
            hasEolLf = true;
        }
        if (attr == "text" && value == "unset") {
            textUnset = true;
        }
    }

    return hasEolLf && !textUnset;
}

auto CollectLfNormalizationByAttributes(const std::filesystem::path& InRepoPath,
                                        const std::vector<std::string>& InRepoRelativePaths,
                                        const ShellExecutor& InExec) -> std::unordered_map<std::string, bool> {
    std::unordered_map<std::string, bool> normalizedByPath;
    if (InRepoRelativePaths.empty()) {
        return normalizedByPath;
    }

    std::vector<std::string> paths;
    paths.reserve(InRepoRelativePaths.size());
    for (const auto& path : InRepoRelativePaths) {
        if (!path.empty()) {
            paths.push_back(path);
        }
    }
    if (paths.empty()) {
        return normalizedByPath;
    }

    std::sort(paths.begin(), paths.end());
    paths.erase(std::unique(paths.begin(), paths.end()), paths.end());

    struct AttrState {
        bool hasEolLf = false;
        bool textUnset = false;
    };
    std::unordered_map<std::string, AttrState> states;
    states.reserve(paths.size());

    constexpr std::size_t kMaxBatchCount = 256;
    constexpr std::size_t kMaxBatchChars = 12000;
    std::size_t index = 0;
    while (index < paths.size()) {
        std::vector<std::string> args{"check-attr", "text", "eol", "--"};
        std::size_t batchChars = 0;
        std::size_t batchCount = 0;

        while (index < paths.size() && batchCount < kMaxBatchCount) {
            const auto& path = paths[index];
            if (batchCount > 0 && batchChars + path.size() > kMaxBatchChars) {
                break;
            }
            args.push_back(path);
            batchChars += path.size();
            batchCount += 1;
            index += 1;
        }

        if (batchCount == 0) {
            args.push_back(paths[index]);
            index += 1;
        }

        const auto res = InExec("git", args, shell::ExecMode::Capture, InRepoPath);
        if (res.exitCode != 0) {
            continue;
        }

        std::istringstream iss(res.stdoutStr);
        std::string line;
        while (std::getline(iss, line)) {
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            if (line.empty()) {
                continue;
            }

            const auto firstSep = line.find(": ");
            if (firstSep == std::string::npos) {
                continue;
            }
            const auto secondSep = line.find(": ", firstSep + 2);
            if (secondSep == std::string::npos) {
                continue;
            }

            const std::string path = line.substr(0, firstSep);
            const std::string attr = line.substr(firstSep + 2, secondSep - (firstSep + 2));
            const std::string value = line.substr(secondSep + 2);

            auto& state = states[path];
            if (attr == "eol" && value == "lf") {
                state.hasEolLf = true;
            }
            if (attr == "text" && value == "unset") {
                state.textUnset = true;
            }
        }
    }

    normalizedByPath.reserve(states.size());
    for (const auto& [path, state] : states) {
        normalizedByPath[path] = state.hasEolLf && !state.textUnset;
    }

    return normalizedByPath;
}

auto CollectSubmodulePathsFromGitStatus(const std::filesystem::path& InRepoPath,
                                        const ShellExecutor& InExec) -> std::vector<std::filesystem::path> {
    std::vector<std::filesystem::path> out;
    const auto res = InExec("git", {"submodule", "status", "--recursive"}, shell::ExecMode::Capture, InRepoPath);
    if (res.exitCode != 0) {
        return out;
    }
    std::istringstream iss(res.stdoutStr);
    std::string line;
    while (std::getline(iss, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.size() < 3) {
            continue;
        }
        // git submodule status format starts with one status char, then sha, then path.
        std::size_t cursor = 1;
        while (cursor < line.size() && line[cursor] == ' ') {
            ++cursor;
        }
        const auto shaEnd = line.find(' ', cursor);
        if (shaEnd == std::string::npos) {
            continue;
        }
        cursor = shaEnd + 1;
        while (cursor < line.size() && line[cursor] == ' ') {
            ++cursor;
        }
        if (cursor >= line.size()) {
            continue;
        }
        const auto pathEnd = line.find(' ', cursor);
        const std::string pathToken = (pathEnd == std::string::npos)
                                          ? line.substr(cursor)
                                          : line.substr(cursor, pathEnd - cursor);
        if (!pathToken.empty()) {
            out.emplace_back(std::filesystem::path(pathToken));
        }
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

auto ComputeManifestName(const std::string& InArchiveBaseName) -> std::string {
    return InArchiveBaseName + "_manifest.txt";
}

auto ComputeChecksumFilename(const std::string& InFilename) -> std::string {
    return InFilename + ".sha256";
}

// ---------------------------------------------------------------------------
// Export manifest JSON helpers
// ---------------------------------------------------------------------------

auto NormalizePlatform(const std::string& InRawPlatform) -> std::string {
    // Normalize to: windows / linux / mac
    std::string lower = InRawPlatform;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (lower == "windows" || lower == "win64" || lower == "hostwin64" ||
        lower == "win32" || lower == "win") {
        return "windows";
    }
    if (lower == "mac" || lower == "macos" || lower == "darwin" ||
        lower == "osx" || lower == "macosx") {
        return "mac";
    }
    if (lower == "linux") {
        return "linux";
    }
    return lower; // pass through unknown values unchanged
}

auto DetectHostPlatform() -> std::string {
#if defined(_WIN32)
    return "windows";
#elif defined(__APPLE__)
    return "mac";
#else
    return "linux";
#endif
}

auto ReadSha256FromSidecar(const std::filesystem::path& InSidecarPath) -> std::string {
    std::ifstream f(InSidecarPath);
    if (!f) {
        return {};
    }
    std::string line;
    if (!std::getline(f, line)) {
        return {};
    }
    // sha256sum format: "<hex>  <filename>" or just "<hex>"
    // PowerShell Get-FileHash format: just "<HEX>" (uppercase)
    const auto spacePos = line.find(' ');
    std::string hex = (spacePos != std::string::npos) ? line.substr(0, spacePos) : line;
    // Trim trailing whitespace
    while (!hex.empty() && (hex.back() == '\r' || hex.back() == '\n' || hex.back() == ' ')) {
        hex.pop_back();
    }
    // Normalize to lowercase
    std::transform(hex.begin(), hex.end(), hex.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return hex;
}

static auto JsonEscape(const std::string& InStr) -> std::string {
    std::string out;
    out.reserve(InStr.size() + 4);
    for (const char c : InStr) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:   out += c;      break;
        }
    }
    return out;
}

auto FormatExportManifestJson(const ExportManifestData& InData) -> std::string {
    std::ostringstream j;
    j << "{\n";
    j << "  \"schemaVersion\": " << InData.schemaVersion << ",\n";
    j << "  \"generator\": \"" << JsonEscape(InData.generator) << "\",\n";
    j << "  \"projectName\": \"" << JsonEscape(InData.projectName) << "\",\n";
    j << "  \"repository\": \"" << JsonEscape(InData.repository) << "\",\n";
    j << "  \"exportMode\": \"" << JsonEscape(InData.exportMode) << "\",\n";
    j << "  \"singleArchive\": " << (InData.singleArchive ? "true" : "false") << ",\n";
    j << "  \"createdAt\": \"" << JsonEscape(InData.createdAt) << "\",\n";
    j << "  \"platform\": \"" << JsonEscape(InData.platform) << "\",\n";
    j << "  \"rootCommit\": \"" << JsonEscape(InData.rootCommit) << "\",\n";
    j << "  \"rootBranch\": \"" << JsonEscape(InData.rootBranch) << "\",\n";
    j << "  \"archiveFile\": \"" << JsonEscape(InData.archiveFile) << "\",\n";
    j << "  \"path\": \"" << JsonEscape(InData.archiveFile) << "\",\n";
    j << "  \"format\": \"" << JsonEscape(InData.format) << "\",\n";
    j << "  \"sizeBytes\": " << InData.sizeBytes << ",\n";
    j << "  \"sha256\": \"" << JsonEscape(InData.sha256) << "\",\n";
    j << "  \"archives\": [\n";
    j << "    {\n";
    j << "      \"kind\": \"release-archive\",\n";
    j << "      \"platform\": \"" << JsonEscape(InData.platform) << "\",\n";
    j << "      \"path\": \"" << JsonEscape(InData.archiveFile) << "\",\n";
    j << "      \"archiveFile\": \"" << JsonEscape(InData.archiveFile) << "\",\n";
    j << "      \"format\": \"" << JsonEscape(InData.format) << "\",\n";
    j << "      \"sizeBytes\": " << InData.sizeBytes << ",\n";
    j << "      \"sha256\": \"" << JsonEscape(InData.sha256) << "\"\n";
    j << "    }\n";
    j << "  ]";
    if (!InData.submodules.empty()) {
        j << ",\n  \"submodules\": [\n";
        for (std::size_t i = 0; i < InData.submodules.size(); ++i) {
            j << "    {\n";
            j << "      \"path\": \"" << JsonEscape(InData.submodules[i].first) << "\",\n";
            j << "      \"commit\": \"" << JsonEscape(InData.submodules[i].second) << "\"\n";
            j << "    }";
            if (i + 1 < InData.submodules.size()) {
                j << ",";
            }
            j << "\n";
        }
        j << "  ]";
    }
    if (!InData.subrepoEntries.empty()) {
        j << ",\n  \"subrepoEntries\": [\n";
        for (std::size_t i = 0; i < InData.subrepoEntries.size(); ++i) {
            const auto& e = InData.subrepoEntries[i];
            j << "    {\n";
            j << "      \"path\": \"" << JsonEscape(e.path) << "\",\n";
            j << "      \"commit\": \"" << JsonEscape(e.commit) << "\",\n";
            j << "      \"mode\": \"" << JsonEscape(e.mode) << "\",\n";
            j << "      \"contentIncluded\": " << (e.contentIncluded ? "true" : "false") << ",\n";
            j << "      \"remote\": " << (e.remote.empty() ? "null" : ("\"" + JsonEscape(e.remote) + "\"")) << ",\n";
            j << "      \"status\": \"" << JsonEscape(e.status) << "\"";
            if (!e.error.empty()) {
                j << ",\n      \"error\": \"" << JsonEscape(e.error) << "\"\n";
            } else {
                j << "\n";
            }
            j << "    }";
            if (i + 1 < InData.subrepoEntries.size()) {
                j << ",";
            }
            j << "\n";
        }
        j << "  ]";
    }
    if (!InData.smokeTestResult.empty()) {
        j << ",\n  \"smokeTestResult\": \"" << JsonEscape(InData.smokeTestResult) << "\"";
    }
    if (InData.hasSubtree) {
        j << ",\n  \"subtree\": {\n";
        j << "    \"name\": \"" << JsonEscape(InData.subtreeName) << "\",\n";
        j << "    \"sourcePath\": \"" << JsonEscape(InData.subtreeSourcePath) << "\",\n";
        j << "    \"repositoryPath\": \"" << JsonEscape(InData.subtreeRepositoryPath) << "\",\n";
        j << "    \"repoRelativePath\": \"" << JsonEscape(InData.subtreeRepoRelativePath) << "\",\n";
        j << "    \"stripSubtreePath\": " << (InData.subtreeStripPath ? "true" : "false") << "\n";
        j << "  }";
    }
    j << "\n}\n";
    return j.str();
}


auto CurrentUtcIso8601Export() -> std::string {
    const auto now = std::chrono::system_clock::now();
    const auto tt = std::chrono::system_clock::to_time_t(now);
    std::tm utcTm{};
#if defined(_WIN32)
    gmtime_s(&utcTm, &tt);
#else
    gmtime_r(&tt, &utcTm);
#endif
    std::ostringstream oss;
    oss << std::put_time(&utcTm, "%Y-%m-%dT%H:%M:%SZ");
    return oss.str();
}

auto TrimExport(std::string InValue) -> std::string {
    while (!InValue.empty() &&
           (InValue.back() == '\n' || InValue.back() == '\r' ||
            InValue.back() == ' ' || InValue.back() == '\t')) {
        InValue.pop_back();
    }
    std::size_t start = 0;
    while (start < InValue.size() &&
           (InValue[start] == ' ' || InValue[start] == '\t')) {
        start += 1;
    }
    return InValue.substr(start);
}

auto RelativeDisplayPathForExport(const std::filesystem::path& InRoot, const std::filesystem::path& InPath) -> std::filesystem::path {
    const auto normalizedRoot = InRoot.lexically_normal();
    const auto normalizedPath = InPath.lexically_normal();
    const auto relative = normalizedPath.lexically_relative(normalizedRoot);
    if (!relative.empty()) {
        return relative;
    }
    return normalizedPath;
}

auto GroupFromRelativePath(const std::filesystem::path& InRelativePath) -> std::string {
    const auto parent = InRelativePath.parent_path().generic_string();
    if (parent.empty() || parent == ".") {
        return ".";
    }
    return parent;
}

auto FirstNLines(const std::string& InText, int InN) -> std::string {
    std::istringstream iss(InText);
    std::string line;
    std::ostringstream out;
    int count = 0;
    while (count < InN && std::getline(iss, line)) {
        out << line << "\n";
        count += 1;
    }
    return out.str();
}

auto FormatManifest(const ExportRecord& InRecord,
                    const ExportResult& InResult,
                    const ExportOptions& InOpts,
                    const std::string& InStatusOut,
                    const std::string& InLsFilesOut) -> std::string {
    const std::string archiveBase = InResult.archiveName.substr(
        0, InResult.archiveName.rfind('.'));
    const std::string prefix = ComputePrefix(InRecord.repoName, InOpts.prefix);

    std::ostringstream oss;
    oss << "Package: " << archiveBase << "\n";
    oss << "Repo: " << InRecord.repoPath.filename().generic_string() << "\n";
    oss << "RepoRoot: " << InRecord.repoPath.generic_string() << "\n";
    oss << "ArchiveRoot: " << prefix << "\n";
    oss << "RevisionFirstParentCount: " << "\n";
    oss << "GitShortSHA: " << "\n";
    oss << "Format: " << InOpts.format << "\n";
    oss << "CreatedAtUTC: " << CurrentUtcIso8601Export() << "\n";
    oss << "\n";
    oss << "GitStatusShort:\n";
    oss << TrimExport(InStatusOut) << "\n";
    oss << "\n";
    oss << "TrackedFilesSample:\n";
    oss << FirstNLines(InLsFilesOut, 200);
    return oss.str();
}

auto FormatMarkdownMetadata(const ExportRecord& InRecord,
                           const ExportResult& InResult,
                           const ExportOptions& InOpts,
                           const std::string& InRootLogOut,
                           const std::vector<std::pair<std::string, std::string>>& InSubLogs) -> std::string {
    const std::string archiveBase = InResult.archiveName.substr(
        0, InResult.archiveName.rfind('.'));

    std::ostringstream oss;
    oss << "# Export Metadata: " << archiveBase << "\n\n";
    oss << "## General Info\n";
    oss << "- **Repository**: `" << InRecord.repoName << "`\n";
    oss << "- **Source Path**: `" << InRecord.repoPath.generic_string() << "`\n";
    oss << "- **Exported At**: " << CurrentUtcIso8601Export() << "\n";
    oss << "- **Format**: " << InOpts.format << "\n";
    oss << "- **Archive File**: `" << InResult.archiveName << "`\n\n";

    if (InRecord.isSubtree) {
        oss << "## Subtree Export\n";
        oss << "- **Export Mode**: subtree\n";
        oss << "- **Export Name**: `" << InRecord.repoName << "`\n";
        oss << "- **Git Repository Path**: `" << InRecord.repoPath.generic_string() << "`\n";
        oss << "- **Subtree Absolute Path**: `" << InRecord.subtreeAbsPath.generic_string() << "`\n";
        oss << "- **Subtree Repo-Relative Path**: `" << InRecord.subtreeRepoRelativePath.generic_string() << "`\n";
        oss << "- **Strip Subtree Path**: " << (!InOpts.keepSubtreePath ? "true" : "false") << "\n\n";
    }

    oss << "## Project History (last " << InOpts.logCount << " entries)\n";
    oss << "### Root Repository: " << InRecord.repoName << "\n";
    oss << "```text\n";
    if (InRootLogOut.empty()) {
        oss << "(No history available)\n";
    } else {
        oss << InRootLogOut;
    }
    oss << "\n```\n\n";

    for (const auto& [subPath, subLog] : InSubLogs) {
        oss << "### Submodule: " << subPath << "\n";
        oss << "```text\n";
        if (subLog.empty()) {
            oss << "(No history available)\n";
        } else {
            oss << subLog;
        }
        oss << "\n```\n\n";
    }

    return oss.str();
}

auto FormatProgressLine(const std::string& InDisplayName,
                        const std::filesystem::path& InArchivePath) -> std::string {
    return "  Exported " + InDisplayName + " -> " + InArchivePath.generic_string();
}

auto FormatSummary(const std::filesystem::path& InOutputDir,
                   const std::filesystem::path& InMetadataDir,
                   const std::vector<ExportResult>& InResults) -> std::string {
    std::ostringstream oss;
    oss << "Export complete\n";
    oss << "OutputDir: " << InOutputDir.generic_string() << "\n";
    oss << "MetadataDir: " << InMetadataDir.generic_string() << "\n";
    oss << "Archives:\n";
    for (const auto& result : InResults) {
        if (result.success) {
            oss << "  " << result.archiveName << "\n";
        }
    }
    return oss.str();
}

auto FormatDryRunPlan(const std::vector<ExportRecord>& InRecords,
                       const ExportOptions& InOpts) -> std::string {
    std::ostringstream oss;
    oss << "Dry-run export plan\n";
    oss << "OutputDir: " << InOpts.outputDir.generic_string() << "\n";
    oss << "Repos to export:\n";
    std::string currentGroup;
    for (const auto& record : InRecords) {
        const auto relativePath = std::filesystem::path(record.relativeRepoPath);
        const auto group = GroupFromRelativePath(relativePath);
        if (group != currentGroup) {
            currentGroup = group;
            oss << "\nGROUP: " << currentGroup << "\n";
        }
        // Use a placeholder revision for dry-run display
        const std::string archiveName = ComputeArchiveName(
            record.repoName, FormatRevision(0, InOpts.revPad), InOpts.format);
        oss << "  " << record.repoName << " (" << record.relativeRepoPath << ") -> " << archiveName << "\n";
    }
    return oss.str();
}

auto IsPathIgnoredByExport(const std::filesystem::path& InRepoPath,
                           const std::filesystem::path& InTargetPath,
                           const ShellExecutor& InExec) -> bool {
    const auto relativePath = std::filesystem::relative(InTargetPath, InRepoPath);
    if (relativePath.empty()) {
        return false;
    }
    const auto result = InExec("git", {"check-attr", "export-ignore", relativePath.generic_string()},
                               shell::ExecMode::Capture, InRepoPath);
    if (result.exitCode != 0) {
        return false;
    }
    // Expected output: <path>: export-ignore: set
    return result.stdoutStr.find("export-ignore: set") != std::string::npos;
}

auto BuildExportList(const std::filesystem::path& InRoot,
                       const std::vector<workspace::RepoRecord>& InDiscovered,
                       bool InNoRecursive,
                       bool InSingle,
                        int InSplitSubrepoDepth,
                        const ShellExecutor& InExec) -> std::vector<ExportRecord> {
    std::vector<ExportRecord> result;
    std::set<std::string> skippedByPolicy;

    auto normalizePolicyToken = [](std::string InValue) {
        InValue = TrimExport(InValue);
        std::transform(InValue.begin(), InValue.end(), InValue.begin(), [](unsigned char ch) {
            return static_cast<char>(std::tolower(ch));
        });
        return InValue;
    };

    auto isExportDisabledByPolicy = [&](const std::filesystem::path& InParentRepo,
                                        const std::filesystem::path& InSubmoduleRelPath) {
        const auto value = normalizePolicyToken(
            workspace::GetSubmoduleConfig(InParentRepo, InSubmoduleRelPath, "kog-export"));
        return value == "false" || value == "0" || value == "off" || value == "no" || value == "skip";
    };

    auto isRepoExportEnabled = [&](const workspace::RepoRecord& InRepo) {
        const auto normalizedPath = InRepo.path.lexically_normal();
        const auto parent = InRepo.registrationRelativeTo.lexically_normal();
        if (parent.empty()) {
            return true;
        }
        std::error_code ec;
        if (std::filesystem::exists(parent, ec) && std::filesystem::exists(normalizedPath, ec) &&
            std::filesystem::equivalent(parent, normalizedPath, ec)) {
            return true;
        }
        if (parent == normalizedPath) {
            return true;
        }
        const auto relToParent = normalizedPath.lexically_relative(parent);
        if (relToParent.empty() || relToParent.generic_string().rfind("..", 0) == 0) {
            return true;
        }
        if (isExportDisabledByPolicy(parent, relToParent)) {
            skippedByPolicy.insert(RelativeDisplayPathForExport(InRoot, normalizedPath).generic_string());
            return false;
        }
        return true;
    };

    auto isRootSubmoduleExportEnabled = [&](const std::filesystem::path& InSubmoduleRelPath) {
        if (isExportDisabledByPolicy(InRoot.lexically_normal(), InSubmoduleRelPath)) {
            skippedByPolicy.insert(InSubmoduleRelPath.generic_string());
            return false;
        }
        return true;
    };

    auto emitPolicySkipHints = [&]() {
        for (const auto& rel : skippedByPolicy) {
            std::cout << "Skip export for " << rel
                      << ": .gitmodules policy kog-export=false\n";
        }
    };

    // Always prepend the workspace root as the first entry
    ExportRecord rootRecord;
    rootRecord.repoPath = InRoot.lexically_normal();
    rootRecord.repoName = rootRecord.repoPath.filename().generic_string();
    rootRecord.relativeRepoPath = ".";
    if (rootRecord.repoName.empty()) {
        rootRecord.repoName = InRoot.lexically_normal().filename().generic_string();
    }
    const std::string rootName = rootRecord.repoName;

    rootRecord.isRoot = true;
    const auto normalizedRoot = rootRecord.repoPath;

    // Collect submodule paths for exclusion (multi-repo) or history (single-repo)
    for (const auto& repo : InDiscovered) {
        if (!isRepoExportEnabled(repo)) {
            continue;
        }
        const auto normalizedPath = repo.path.lexically_normal();
        bool isRoot = false;
        std::error_code ec;
        if (std::filesystem::exists(normalizedPath, ec) && std::filesystem::exists(normalizedRoot, ec)) {
            if (std::filesystem::equivalent(normalizedPath, normalizedRoot, ec)) {
                isRoot = true;
            }
        }
        if (!isRoot && normalizedPath == normalizedRoot) {
            isRoot = true;
        }

        if (!isRoot) {
            rootRecord.submodulePaths.push_back(RelativeDisplayPathForExport(normalizedRoot, normalizedPath));
        }
    }
    const auto gitSubmodules = CollectSubmodulePathsFromGitStatus(normalizedRoot, InExec);
    for (const auto& relPath : gitSubmodules) {
        if (!isRootSubmoduleExportEnabled(relPath)) {
            continue;
        }
        rootRecord.submodulePaths.push_back(relPath);
    }
    std::sort(rootRecord.submodulePaths.begin(), rootRecord.submodulePaths.end());
    rootRecord.submodulePaths.erase(std::unique(rootRecord.submodulePaths.begin(), rootRecord.submodulePaths.end()), rootRecord.submodulePaths.end());
    rootRecord.submodulePaths.erase(
        std::remove_if(rootRecord.submodulePaths.begin(), rootRecord.submodulePaths.end(),
                       [&](const std::filesystem::path& InSubmoduleRelPath) {
                           return !isRootSubmoduleExportEnabled(InSubmoduleRelPath);
                       }),
        rootRecord.submodulePaths.end());

    result.push_back(std::move(rootRecord));

    auto isDescendantRepoPath = [](const std::filesystem::path& InAncestor,
                                   const std::filesystem::path& InCandidate) -> bool {
        const auto relative = InCandidate.lexically_relative(InAncestor);
        if (relative.empty()) {
            return false;
        }
        const auto relStr = relative.generic_string();
        return relStr != "." && relStr.rfind("..", 0) != 0;
    };

    std::vector<std::filesystem::path> discoveredPaths;
    discoveredPaths.reserve(InDiscovered.size());
    for (const auto& repo : InDiscovered) {
        if (!isRepoExportEnabled(repo)) {
            continue;
        }
        const auto p = repo.path.lexically_normal();
        std::error_code ec;
        if (std::filesystem::exists(p, ec) && std::filesystem::exists(normalizedRoot, ec) &&
            std::filesystem::equivalent(p, normalizedRoot, ec)) {
            continue;
        }
        if (p == normalizedRoot) {
            continue;
        }
        discoveredPaths.push_back(p);
    }

    std::sort(discoveredPaths.begin(), discoveredPaths.end(), [](const auto& A, const auto& B) {
        return A.generic_string().size() < B.generic_string().size();
    });
    discoveredPaths.erase(std::unique(discoveredPaths.begin(), discoveredPaths.end()), discoveredPaths.end());

    std::map<std::string, int> repoDepthByPath;
    repoDepthByPath[normalizedRoot.generic_string()] = 0;
    for (const auto& path : discoveredPaths) {
        int depth = isDescendantRepoPath(normalizedRoot, path) ? 1 : 0;
        for (const auto& [ancestorPath, ancestorDepth] : repoDepthByPath) {
            const auto ancestor = std::filesystem::path(ancestorPath);
            if (isDescendantRepoPath(ancestor, path)) {
                depth = std::max(depth, ancestorDepth + 1);
            }
        }
        repoDepthByPath[path.generic_string()] = depth;
    }

    auto depthFromRoot = [&](const std::filesystem::path& InRepoPath) -> int {
        const auto key = InRepoPath.lexically_normal().generic_string();
        if (repoDepthByPath.contains(key)) {
            return repoDepthByPath[key];
        }
        return 0;
    };

    if (InNoRecursive || InSingle) {
        emitPolicySkipHints();
        return result;
    }

    if (InSplitSubrepoDepth > 0) {
        auto collectDescendantSubmodulePaths = [&](const std::filesystem::path& InRepoPath) -> std::vector<std::filesystem::path> {
            std::vector<std::filesystem::path> submodulePaths;
            const auto normalizedRepoPath = InRepoPath.lexically_normal();

            for (const auto& repo : InDiscovered) {
                if (!isRepoExportEnabled(repo)) {
                    continue;
                }
                const auto candidatePath = repo.path.lexically_normal();
                std::error_code ec;
                if (std::filesystem::exists(candidatePath, ec) && std::filesystem::exists(normalizedRepoPath, ec) &&
                    std::filesystem::equivalent(candidatePath, normalizedRepoPath, ec)) {
                    continue;
                }
                if (candidatePath == normalizedRepoPath) {
                    continue;
                }

                const auto relative = candidatePath.lexically_relative(normalizedRepoPath);
                if (relative.empty()) {
                    continue;
                }
                const auto relStr = relative.generic_string();
                if (relStr == "." || relStr.rfind("..", 0) == 0) {
                    continue;
                }
                submodulePaths.push_back(relative);
            }

            const auto gitSubmodules = CollectSubmodulePathsFromGitStatus(normalizedRepoPath, InExec);
            for (const auto& relPath : gitSubmodules) {
                if (isExportDisabledByPolicy(normalizedRepoPath, relPath)) {
                    continue;
                }
                submodulePaths.push_back(relPath);
            }
            std::sort(submodulePaths.begin(), submodulePaths.end());
            submodulePaths.erase(std::unique(submodulePaths.begin(), submodulePaths.end()), submodulePaths.end());
            return submodulePaths;
        };

        for (const auto& repo : InDiscovered) {
            if (!isRepoExportEnabled(repo)) {
                continue;
            }
            const auto normalizedPath = repo.path.lexically_normal();
            std::error_code ec;
            bool isRoot = false;
            if (std::filesystem::exists(normalizedPath, ec) && std::filesystem::exists(normalizedRoot, ec)) {
                if (std::filesystem::equivalent(normalizedPath, normalizedRoot, ec)) {
                    isRoot = true;
                }
            }
            if (!isRoot && normalizedPath == normalizedRoot) {
                isRoot = true;
            }
            if (isRoot) {
                continue;
            }

            if (depthFromRoot(repo.path) > InSplitSubrepoDepth) {
                continue;
            }

            if (IsPathIgnoredByExport(normalizedRoot, normalizedPath, InExec)) {
                continue;
            }

            ExportRecord rec;
            rec.repoPath = repo.path;
            rec.relativeRepoPath = RelativeDisplayPathForExport(InRoot, repo.path).generic_string();
            rec.exportAsSingle = (depthFromRoot(repo.path) == InSplitSubrepoDepth);
            std::string subName = rec.relativeRepoPath;
            std::replace(subName.begin(), subName.end(), '/', '_');
            std::replace(subName.begin(), subName.end(), '\\', '_');
            rec.repoName = rootName + "_" + subName;
            rec.isRoot = false;
            if (rec.exportAsSingle) {
                rec.submodulePaths = collectDescendantSubmodulePaths(rec.repoPath);
            }
            result.push_back(std::move(rec));
        }
        std::sort(result.begin(), result.end(), [](const ExportRecord& A, const ExportRecord& B) {
            if (A.isRoot != B.isRoot) {
                return A.isRoot;
            }
            return A.relativeRepoPath < B.relativeRepoPath;
        });
        emitPolicySkipHints();
        return result;
    }

    // Append discovered subrepos (skip any that match the root path)
    for (const auto& repo : InDiscovered) {
        if (!isRepoExportEnabled(repo)) {
            continue;
        }
        const auto normalizedPath = repo.path.lexically_normal();
        std::error_code ec;
        bool isRoot = false;
        if (std::filesystem::exists(normalizedPath, ec) && std::filesystem::exists(normalizedRoot, ec)) {
            if (std::filesystem::equivalent(normalizedPath, normalizedRoot, ec)) {
                isRoot = true;
            }
        }
        if (!isRoot && normalizedPath == normalizedRoot) {
            isRoot = true;
        }

        if (isRoot) {
            continue;
        }

        // Check if the repo path is ignored by the root repo
        if (IsPathIgnoredByExport(normalizedRoot, normalizedPath, InExec)) {
            continue;
        }

        ExportRecord rec;
        rec.repoPath = repo.path;
        rec.relativeRepoPath = RelativeDisplayPathForExport(InRoot, repo.path).generic_string();

        // Include root repo name in subrepo names for clarity
        std::string subName = rec.relativeRepoPath;
        std::replace(subName.begin(), subName.end(), '/', '_');
        std::replace(subName.begin(), subName.end(), '\\', '_');
        rec.repoName = rootName + "_" + subName;

        rec.isRoot = false;
        result.push_back(std::move(rec));
    }

    // Sort by relative path to ensure consistent grouping
    std::sort(result.begin(), result.end(), [](const ExportRecord& A, const ExportRecord& B) {
        return A.relativeRepoPath < B.relativeRepoPath;
    });

    emitPolicySkipHints();

    return result;
}

// ---------------------------------------------------------------------------
// Shell-calling helpers (implemented in task 4)
// ---------------------------------------------------------------------------

namespace {

auto GitCapture(const std::filesystem::path& InRepo,
                const std::vector<std::string>& InArgs,
                const ShellExecutor& InExec) -> shell::ExecResult {
    return InExec("git", InArgs, shell::ExecMode::Capture, InRepo);
}

auto ComputeRevision(const std::filesystem::path& InRepoPath, int InRevPad,
                     const ShellExecutor& InExec) -> std::string {
    const auto result = GitCapture(InRepoPath, {"rev-list", "--count", "--first-parent", "HEAD"}, InExec);
    if (result.exitCode != 0 || result.stdoutStr.empty()) {
        return FormatRevision(0, InRevPad);
    }
    try {
        const int rev = std::stoi(result.stdoutStr);
        return FormatRevision(rev, InRevPad);
    } catch (const std::exception&) {
        return FormatRevision(0, InRevPad);
    }
}

auto WriteChecksumFile(const std::filesystem::path& InTargetFile,
                       const std::filesystem::path& InMetadataDir,
                       const ShellExecutor& InExec) -> bool {
    const auto filename = InTargetFile.filename().generic_string();
    const auto checksumPath = InMetadataDir / ComputeChecksumFilename(filename);

    // Try sha256sum (Linux, Git Bash)
    {
        const auto r = InExec(
            "sha256sum", {InTargetFile.generic_string()},
            shell::ExecMode::Capture, InMetadataDir);
        if (r.exitCode == 0 && !r.stdoutStr.empty()) {
            std::ofstream f(checksumPath);
            if (f) {
                f << r.stdoutStr;
                return true;
            }
        }
    }

    // Try shasum -a 256 (macOS)
    {
        const auto r = InExec(
            "shasum", {"-a", "256", InTargetFile.generic_string()},
            shell::ExecMode::Capture, InMetadataDir);
        if (r.exitCode == 0 && !r.stdoutStr.empty()) {
            std::ofstream f(checksumPath);
            if (f) {
                f << r.stdoutStr;
                return true;
            }
        }
    }

    // Try PowerShell Get-FileHash (Windows fallback)
    {
        const auto r = InExec(
            "powershell",
            {"-NoProfile", "-Command",
             "Get-FileHash -Algorithm SHA256 '" + InTargetFile.generic_string() + "' | Select-Object -ExpandProperty Hash"},
            shell::ExecMode::Capture, InMetadataDir);
        if (r.exitCode == 0 && !r.stdoutStr.empty()) {
            std::ofstream f(checksumPath);
            if (f) {
                f << r.stdoutStr;
                return true;
            }
        }
    }

    std::cerr << "kog export: warning: no sha256 tool found; skipping checksum for "
              << InTargetFile.filename().generic_string() << "\n";
    return false;
}

} // anonymous namespace

namespace {

auto ResolveReleaseArchiveSmokeScript(const std::filesystem::path& InWorkspaceRoot) -> std::filesystem::path {
    const auto scriptPath = InWorkspaceRoot / "src" / "shell" / "test" / "smoke-release-archive.sh";
    std::error_code ec;
    if (std::filesystem::exists(scriptPath, ec) && std::filesystem::is_regular_file(scriptPath, ec)) {
        return scriptPath;
    }
    return {};
}

auto HasBash(const std::filesystem::path& InWorkspaceRoot,
             const ShellExecutor& InExec,
             const std::string& InBashCommand) -> bool {
    const auto result = InExec(InBashCommand, {"-lc", "true"}, shell::ExecMode::Capture, InWorkspaceRoot);
    return result.exitCode == 0;
}

auto HasPython(const std::filesystem::path& InWorkspaceRoot,
               const ShellExecutor& InExec,
               const std::string& InPythonCommand) -> bool {
    const auto result = InExec(InPythonCommand, {"-c", "import sys"}, shell::ExecMode::Capture, InWorkspaceRoot);
    return result.exitCode == 0;
}

auto SplitNonEmptyLines(const std::string& InText) -> std::vector<std::string> {
    std::vector<std::string> lines;
    std::istringstream iss(InText);
    std::string line;
    while (std::getline(iss, line)) {
        line = TrimExport(line);
        if (!line.empty()) {
            lines.push_back(line);
        }
    }
    return lines;
}

auto ResolvePreferredBash(const std::filesystem::path& InWorkspaceRoot,
                          const ShellExecutor& InExec) -> std::string {
#ifdef KOG_PLATFORM_WINDOWS
    auto isGitBashPath = [](const std::string& InPath) -> bool {
        std::string lower = InPath;
        std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char ch) {
            return static_cast<char>(std::tolower(ch));
        });
        return lower.find("\\git\\usr\\bin\\bash.exe") != std::string::npos ||
               lower.find("\\git\\bin\\bash.exe") != std::string::npos;
    };

    const auto whereBash = InExec("where.exe", {"bash"}, shell::ExecMode::Capture, InWorkspaceRoot);
    const auto bashCandidates = SplitNonEmptyLines(whereBash.stdoutStr);
    for (const auto& candidate : bashCandidates) {
        if (isGitBashPath(candidate) && std::filesystem::exists(candidate)) {
            return candidate;
        }
    }

    const auto whereGit = InExec("where.exe", {"git"}, shell::ExecMode::Capture, InWorkspaceRoot);
    const auto gitCandidates = SplitNonEmptyLines(whereGit.stdoutStr);
    for (const auto& gitCandidate : gitCandidates) {
        auto dir = std::filesystem::path(gitCandidate).parent_path();
        for (int depth = 0; depth < 3 && !dir.empty(); ++depth) {
            const auto gitUsrBash = dir / "usr" / "bin" / "bash.exe";
            if (std::filesystem::exists(gitUsrBash)) {
                return gitUsrBash.string();
            }
            const auto gitBinBash = dir / "bin" / "bash.exe";
            if (std::filesystem::exists(gitBinBash)) {
                return gitBinBash.string();
            }
            dir = dir.parent_path();
        }
    }

    for (const auto& candidate : bashCandidates) {
        if (std::filesystem::exists(candidate)) {
            return candidate;
        }
    }
#endif
    return "bash";
}

auto ResolvePreferredPython(const std::filesystem::path& InWorkspaceRoot,
                            const ShellExecutor& InExec) -> std::string {
#ifdef KOG_PLATFORM_WINDOWS
    const char* candidates[] = {"python", "python3"};
#else
    const char* candidates[] = {"python3", "python"};
#endif
    for (const char* candidate : candidates) {
        if (HasPython(InWorkspaceRoot, InExec, candidate)) {
            return candidate;
        }
    }
    return {};
}

auto AppendCapturedOutput(std::ostream& InStream,
                          const std::string& InLabel,
                          const std::string& InText) -> void {
    const auto trimmed = TrimExport(InText);
    if (!trimmed.empty()) {
        InStream << InLabel << ":\n" << trimmed << "\n";
    }
}

auto RunReleaseArchiveValidation(const ExportRecord& InRecord,
                                 const ExportResult& InResult,
                                 const ExportOptions& InOpts,
                                 const std::filesystem::path& InWorkspaceRoot,
                                 const ShellExecutor& InExec) -> int {
    if (!InOpts.validateReleaseArchive) {
        return 0;
    }

    if (InRecord.isSubtree) {
        return 0;
    }

    if (!InRecord.isRoot) {
        return 0;
    }

    auto skipOrFail = [&](const std::string& InReason) -> int {
        if (InOpts.forceValidateReleaseArchive) {
            std::cerr << "kog export: release archive validation required but unavailable: "
                      << InReason << "\n";
            return 1;
        }
        std::cout << "  Skipped release archive validation: " << InReason << "\n";
        return 0;
    };

    if (!InResult.success) {
        return 0;
    }

    if (InOpts.format != "tar" || InResult.archivePath.extension() != ".tar") {
        return skipOrFail("only .tar archives are supported by smoke-release-archive.sh");
    }

    if (!InOpts.single) {
        return skipOrFail("requires --single so submodule working-tree contents are included");
    }
    if (!InOpts.includeSubrepos) {
        return skipOrFail("requires --include-subrepos to include subrepo contents in root archive");
    }

    const auto smokeScript = ResolveReleaseArchiveSmokeScript(InWorkspaceRoot);
    if (smokeScript.empty()) {
        return skipOrFail("src/shell/test/smoke-release-archive.sh was not found");
    }

    const auto bashCommand = ResolvePreferredBash(InWorkspaceRoot, InExec);
    if (!HasBash(InWorkspaceRoot, InExec, bashCommand)) {
        return skipOrFail("bash is not available on PATH");
    }

    std::cout << "  Validating release archive -> " << InResult.archivePath.generic_string() << "\n";
    // Build relative paths manually: strip the workspace root prefix so
    // MSYS/Git Bash (cwd = InWorkspaceRoot) can resolve them without
    // Windows absolute path issues.
    auto toRelative = [](const std::filesystem::path& InFull,
                         const std::filesystem::path& InBase) -> std::string {
        // Normalize both to generic (forward-slash) strings for comparison
        std::string fullStr = InFull.lexically_normal().generic_string();
        std::string baseStr = InBase.lexically_normal().generic_string();
        // Ensure base has no trailing slash
        while (!baseStr.empty() && baseStr.back() == '/') {
            baseStr.pop_back();
        }
        if (fullStr.size() > baseStr.size() &&
            fullStr.substr(0, baseStr.size()) == baseStr &&
            fullStr[baseStr.size()] == '/') {
            return fullStr.substr(baseStr.size() + 1);
        }
        return fullStr; // last resort: absolute path
    };
    const auto relScript  = toRelative(smokeScript,            InWorkspaceRoot);
    const auto relArchive = toRelative(InResult.archivePath,   InWorkspaceRoot);

    auto shellQuote = [](const std::string& InText) -> std::string {
        std::string out{"'"};
        for (const char ch : InText) {
            if (ch == '\'') {
                out += "'\\''";
            } else {
                out.push_back(ch);
            }
        }
        out.push_back('\'');
        return out;
    };

    const std::string commandLine = shellQuote(relScript) + " " + shellQuote(relArchive);
    const auto validation = InExec(
        bashCommand,
        {"-lc", commandLine},
        shell::ExecMode::Capture,
        InWorkspaceRoot);

    if (validation.exitCode != 0) {
        std::cerr << "kog export: release archive validation failed for "
                  << InResult.archivePath.generic_string() << "\n";
        AppendCapturedOutput(std::cerr, "stdout", validation.stdoutStr);
        AppendCapturedOutput(std::cerr, "stderr", validation.stderrStr);
        return validation.exitCode == 0 ? 1 : validation.exitCode;
    }

    AppendCapturedOutput(std::cout, "stdout", validation.stdoutStr);

    return 0;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Public shell-calling helpers (dependency-injected, exposed for unit tests)
// ---------------------------------------------------------------------------

auto ExportOneRepo(const ExportRecord& InRecord,
                   const ExportOptions& InOpts,
                   const std::filesystem::path& InOutputDir,
                   const std::filesystem::path& InMetadataDir,
                   const ShellExecutor& InExec) -> ExportResult {
    ExportResult result;
    result.success = false;

    const std::string revision = ComputeRevision(InRecord.repoPath, InOpts.revPad, InExec);
    result.archiveName = ComputeArchiveName(InRecord.repoName, revision, InOpts.format);
    result.archivePath = InOutputDir / result.archiveName;

    // Run archive
    shell::ExecResult archiveResult;
    const std::string prefix = ComputePrefix(InRecord.repoName, InOpts.prefix);

    const bool singleForRecord = InOpts.single || InRecord.exportAsSingle;
    const bool includeSubreposForRecord = InOpts.includeSubrepos || InRecord.exportAsSingle;
    const bool useWorkingTree = (InOpts.source == "working-tree") || (singleForRecord && includeSubreposForRecord) || HasPathFilters(InOpts);
    const std::string pythonCommand = useWorkingTree ? ResolvePreferredPython(InRecord.repoPath, InExec) : "";

    if (!useWorkingTree) {
        std::vector<std::string> args;
        if (InRecord.isSubtree) {
            args = BuildGitArchiveArgsForSubtree(
                InOpts.format,
                prefix,
                result.archivePath,
                InRecord.subtreeRepoRelativePath.generic_string(),
                InOpts.keepSubtreePath);
        } else {
            args = BuildGitArchiveArgs(InOpts.format, prefix, result.archivePath);
        }
        archiveResult = InExec("git", args, shell::ExecMode::Capture, InRecord.repoPath);
    } else {
        if (InRecord.isSubtree) {
            const auto localTmpDir = InRecord.repoPath / ".kano" / "tmp";
            std::error_code mkEc;
            std::filesystem::create_directories(localTmpDir, mkEc);
            const auto manifestPath = localTmpDir / (InRecord.repoName + "_subtree_working_tree_manifest.tsv");

            std::vector<std::string> relFiles = CollectWorkingTreeFilesForSubtree(
                InRecord.repoPath,
                InRecord.subtreeRepoRelativePath,
                InOpts.keepSubtreePath,
                InExec);
            relFiles.erase(
                std::remove_if(
                    relFiles.begin(),
                    relFiles.end(),
                    [&](const std::string& rel) {
                        const std::string repoRelativePath = InOpts.keepSubtreePath
                            ? rel
                            : (InRecord.subtreeRepoRelativePath.generic_string() + "/" + rel);
                        return !ShouldIncludePathByFilters(repoRelativePath, InOpts);
                    }),
                relFiles.end());
            if (relFiles.empty()) {
                result.errorMessage = "no files found in subtree working tree export";
                return result;
            }

            std::map<std::string, int> modeByPath;
            if (!CollectExecutablePathsWithPrefix(InRecord.repoPath, "", InExec, modeByPath)) {
                result.errorMessage = "failed to collect subtree file modes";
                return result;
            }

            {
                std::ofstream mf(manifestPath, std::ios::binary);
                for (const auto& rel : relFiles) {
                    const std::string fullRel = InOpts.keepSubtreePath
                                                    ? rel
                                                    : (InRecord.subtreeRepoRelativePath.generic_string() + "/" + rel);
                    const auto full = (InRecord.repoPath / fullRel).lexically_normal();
                    const int mode = modeByPath.contains(fullRel) ? modeByPath[fullRel] : 0644;
                    const bool normalizeLf = ShouldNormalizePathToLfByAttributes(InRecord.repoPath, fullRel, InExec);
                    mf << mode << "\t" << full.generic_string() << "\t" << (prefix + rel) << "\t"
                       << (normalizeLf ? "1" : "0") << "\n";
                }
            }

            const std::string py =
                "import io,pathlib,sys,tarfile,zipfile; "
                "mf=pathlib.Path(sys.argv[1]); out=pathlib.Path(sys.argv[2]); fmt=sys.argv[3]; "
                "out.parent.mkdir(parents=True,exist_ok=True); "
                "entries=[]\n"
                "def maybe_norm(data,norm):\n"
                "  if not norm: return data\n"
                "  if b'\\0' in data: return data\n"
                "  return data.replace(b'\\r\\n',b'\\n').replace(b'\\r',b'\\n')\n"
                "for line in mf.read_text(encoding='utf-8').splitlines():\n"
                "  if not line.strip(): continue\n"
                "  parts=line.split('\\t');\n"
                "  if len(parts) < 3: continue\n"
                "  m,s,a=parts[0],parts[1],parts[2]; norm=(len(parts) > 3 and parts[3] == '1'); mode=int(m); sp=pathlib.Path(s); ap=a.replace('\\\\','/');\n"
                "  if not sp.exists() or not sp.is_file(): continue\n"
                "  entries.append((mode,sp,ap,norm))\n"
                "if fmt == 'tar':\n"
                "  with tarfile.open(out,'w') as t:\n"
                "    for mode,sp,ap,norm in entries:\n"
                "      data=maybe_norm(sp.read_bytes(),norm)\n"
                "      info=t.gettarinfo(str(sp),arcname=ap); info.mode=mode; info.size=len(data)\n"
                "      t.addfile(info,io.BytesIO(data))\n"
                "elif fmt == 'zip':\n"
                "  with zipfile.ZipFile(out,'w',compression=zipfile.ZIP_DEFLATED) as z:\n"
                "    for mode,sp,ap,norm in entries:\n"
                "      data=maybe_norm(sp.read_bytes(),norm)\n"
                "      zi=zipfile.ZipInfo(ap); zi.external_attr=(mode & 0xffff) << 16\n"
                "      z.writestr(zi, data)\n"
                "else:\n"
                "  raise SystemExit('unsupported archive format: ' + fmt)";

            if (pythonCommand.empty()) {
                result.errorMessage = "working-tree export requires python3 or python";
                return result;
            }
            archiveResult = InExec(pythonCommand, {"-c", py, manifestPath.generic_string(), result.archivePath.generic_string(), InOpts.format}, shell::ExecMode::Capture, InRecord.repoPath);
            std::error_code ec;
            std::filesystem::remove(manifestPath, ec);
        } else {
        const auto localTmpDir = InRecord.repoPath / ".kano" / "tmp";
        std::error_code mkEc;
        std::filesystem::create_directories(localTmpDir, mkEc);
        const auto manifestPath = localTmpDir / (InRecord.repoName + "_working_tree_manifest.tsv");

        if (singleForRecord && includeSubreposForRecord) {
            for (const auto& subRelPath : InRecord.submodulePaths) {
                const auto subRepoPath = InRecord.repoPath / subRelPath;
                std::error_code checkEc;
                if (!std::filesystem::exists(subRepoPath, checkEc) || !std::filesystem::is_directory(subRepoPath, checkEc)) {
                    std::cerr << "kog export: warning: skipping missing subrepo '"
                              << subRelPath.generic_string()
                              << "' (not initialized or unavailable)\n";
                }
            }
        }

        std::vector<std::string> relFiles = CollectWorkingTreeFiles(InRecord, "", singleForRecord && includeSubreposForRecord, InExec);
        relFiles.erase(
            std::remove_if(
                relFiles.begin(),
                relFiles.end(),
                [&](const std::string& rel) {
                    return !ShouldIncludePathByFilters(rel, InOpts);
                }),
            relFiles.end());
        if (relFiles.empty()) {
            result.errorMessage = "no files found in working tree export";
            return result;
        }

        std::map<std::string, int> modeByPath;
        if (!CollectExecutablePathsWithPrefix(InRecord.repoPath, "", InExec, modeByPath)) {
            result.errorMessage = "failed to collect root file modes";
            return result;
        }
        if (singleForRecord && includeSubreposForRecord) {
            for (const auto& subRelPath : InRecord.submodulePaths) {
                const auto subRepoPath = InRecord.repoPath / subRelPath;
                const std::string subPrefix = subRelPath.generic_string() + "/";
                if (!CollectExecutablePathsWithPrefix(subRepoPath, subPrefix, InExec, modeByPath)) {
                    std::cerr << "kog export: warning: unable to collect submodule file modes for '"
                              << subRelPath.generic_string()
                              << "'; falling back to default mode 0644 for affected files\n";
                }
            }
        }

        {
            std::ofstream mf(manifestPath, std::ios::binary);
            std::vector<std::pair<std::string, std::filesystem::path>> submodulePrefixes;
            submodulePrefixes.reserve(InRecord.submodulePaths.size());
            if (singleForRecord && includeSubreposForRecord) {
                for (const auto& subRelPath : InRecord.submodulePaths) {
                    submodulePrefixes.push_back({subRelPath.generic_string() + "/", subRelPath});
                }
                std::sort(submodulePrefixes.begin(), submodulePrefixes.end(), [](const auto& A, const auto& B) {
                    return A.first.size() > B.first.size();
                });
            }

            std::vector<std::string> rootPathsForAttr;
            rootPathsForAttr.reserve(relFiles.size());
            std::unordered_map<std::string, std::vector<std::string>> submodulePathsForAttr;
            std::unordered_map<std::string, std::pair<std::string, std::string>> submodulePathByRel;
            if (singleForRecord && includeSubreposForRecord) {
                for (const auto& rel : relFiles) {
                    bool matchedSubmodule = false;
                    for (const auto& [subPrefix, subRelPath] : submodulePrefixes) {
                        if (rel.rfind(subPrefix, 0) != 0) {
                            continue;
                        }
                        const std::string subRepoRel = rel.substr(subPrefix.size());
                        const std::string submoduleKey = subRelPath.generic_string();
                        submodulePathsForAttr[submoduleKey].push_back(subRepoRel);
                        submodulePathByRel[rel] = {submoduleKey, subRepoRel};
                        matchedSubmodule = true;
                        break;
                    }
                    if (!matchedSubmodule) {
                        rootPathsForAttr.push_back(rel);
                    }
                }
            } else {
                rootPathsForAttr = relFiles;
            }

            const auto rootNormalizeByPath = CollectLfNormalizationByAttributes(InRecord.repoPath, rootPathsForAttr, InExec);
            std::unordered_map<std::string, std::unordered_map<std::string, bool>> submoduleNormalizeByPath;
            for (const auto& [submoduleKey, subPaths] : submodulePathsForAttr) {
                const auto subRepoPath = InRecord.repoPath / std::filesystem::path(submoduleKey);
                submoduleNormalizeByPath[submoduleKey] = CollectLfNormalizationByAttributes(subRepoPath, subPaths, InExec);
            }

            for (const auto& rel : relFiles) {
                const auto full = (InRecord.repoPath / rel).lexically_normal();
                const int mode = modeByPath.contains(rel) ? modeByPath[rel] : 0644;
                bool normalizeLf = false;
                const auto subLookup = submodulePathByRel.find(rel);
                if (subLookup != submodulePathByRel.end()) {
                    const auto& [submoduleKey, subRepoRel] = subLookup->second;
                    const auto normalizeMapIt = submoduleNormalizeByPath.find(submoduleKey);
                    if (normalizeMapIt != submoduleNormalizeByPath.end()) {
                        const auto pathNormalizeIt = normalizeMapIt->second.find(subRepoRel);
                        if (pathNormalizeIt != normalizeMapIt->second.end()) {
                            normalizeLf = pathNormalizeIt->second;
                        }
                    }
                } else {
                    const auto rootNormalizeIt = rootNormalizeByPath.find(rel);
                    if (rootNormalizeIt != rootNormalizeByPath.end()) {
                        normalizeLf = rootNormalizeIt->second;
                    }
                }
                mf << mode << "\t" << full.generic_string() << "\t" << (prefix + rel) << "\t"
                   << (normalizeLf ? "1" : "0") << "\n";
            }
        }

        const std::string py =
            "import io,pathlib,sys,tarfile,zipfile; "
            "mf=pathlib.Path(sys.argv[1]); out=pathlib.Path(sys.argv[2]); fmt=sys.argv[3]; "
            "out.parent.mkdir(parents=True,exist_ok=True); "
            "entries=[]\n"
            "def maybe_norm(data,norm):\n"
            "  if not norm: return data\n"
            "  if b'\\0' in data: return data\n"
            "  return data.replace(b'\\r\\n',b'\\n').replace(b'\\r',b'\\n')\n"
            "for line in mf.read_text(encoding='utf-8').splitlines():\n"
            "  if not line.strip(): continue\n"
            "  parts=line.split('\\t');\n"
            "  if len(parts) < 3: continue\n"
            "  m,s,a=parts[0],parts[1],parts[2]; norm=(len(parts) > 3 and parts[3] == '1'); mode=int(m); sp=pathlib.Path(s); ap=a.replace('\\\\','/');\n"
            "  if not sp.exists() or not sp.is_file(): continue\n"
            "  entries.append((mode,sp,ap,norm))\n"
            "if fmt == 'tar':\n"
            "  with tarfile.open(out,'w') as t:\n"
            "    for mode,sp,ap,norm in entries:\n"
            "      data=maybe_norm(sp.read_bytes(),norm)\n"
            "      info=t.gettarinfo(str(sp),arcname=ap); info.mode=mode; info.size=len(data)\n"
            "      t.addfile(info,io.BytesIO(data))\n"
            "elif fmt == 'zip':\n"
            "  with zipfile.ZipFile(out,'w',compression=zipfile.ZIP_DEFLATED) as z:\n"
            "    for mode,sp,ap,norm in entries:\n"
            "      data=maybe_norm(sp.read_bytes(),norm)\n"
            "      zi=zipfile.ZipInfo(ap); zi.external_attr=(mode & 0xffff) << 16\n"
            "      z.writestr(zi, data)\n"
            "else:\n"
            "  raise SystemExit('unsupported archive format: ' + fmt)";

        if (pythonCommand.empty()) {
            result.errorMessage = "working-tree export requires python3 or python";
            return result;
        }
        archiveResult = InExec(
            pythonCommand,
            {"-c", py, manifestPath.generic_string(), result.archivePath.generic_string(), InOpts.format},
            shell::ExecMode::Capture,
            InRecord.repoPath);

        std::error_code ec;
        std::filesystem::remove(manifestPath, ec);
        }
    }

    if (archiveResult.exitCode != 0) {
        result.errorMessage = "archive failed: " + archiveResult.stderrStr;
        return result;
    }

    result.success = true;

    // Populate result fields used by export-manifest.json generation
    {
        std::error_code ec;
        result.sizeBytes = std::filesystem::file_size(result.archivePath, ec);
        if (ec) {
            result.sizeBytes = 0;
        }
    }
    {
        const auto shortSha = GitCapture(InRecord.repoPath, {"rev-parse", "--short", "HEAD"}, InExec);
        if (shortSha.exitCode == 0) {
            result.rootCommit = TrimExport(shortSha.stdoutStr);
        }
    }
    {
        const auto branch = GitCapture(InRecord.repoPath, {"rev-parse", "--abbrev-ref", "HEAD"}, InExec);
        if (branch.exitCode == 0) {
            result.rootBranch = TrimExport(branch.stdoutStr);
        }
    }

    if (InOpts.noMetadata) {
        return result;
    }

    // Write manifest
    const auto statusOut = GitCapture(InRecord.repoPath, {"status", "--short"}, InExec);
    const auto lsFilesOut = GitCapture(InRecord.repoPath, {"ls-files"}, InExec);
    const std::string manifestText = FormatManifest(
        InRecord, result, InOpts,
        statusOut.stdoutStr, lsFilesOut.stdoutStr);

    const std::string archiveBase = result.archiveName.substr(
        0, result.archiveName.rfind('.'));
    const auto manifestName = ComputeManifestName(archiveBase);
    const auto manifestPath = InMetadataDir / manifestName;

    std::ofstream mf(manifestPath);
    if (mf) {
        mf << manifestText;
    }

    // Write checksums
    WriteChecksumFile(result.archivePath, InMetadataDir, InExec);
    WriteChecksumFile(manifestPath, InMetadataDir, InExec);

    // Read back sha256 for manifest JSON
    const auto sidecarPath = InMetadataDir / ComputeChecksumFilename(result.archiveName);
    result.sha256 = ReadSha256FromSidecar(sidecarPath);

    const auto logArgs = std::vector<std::string>{"log", "-n", std::to_string(InOpts.logCount), "--oneline", "--decorate"};
    const auto rootLogOut = GitCapture(InRecord.repoPath, logArgs, InExec);

    std::vector<std::pair<std::string, std::string>> subLogs;
    if (singleForRecord) {
        for (const auto& subRelPath : InRecord.submodulePaths) {
            const auto subLog = GitCapture(InRecord.repoPath / subRelPath, logArgs, InExec);
            subLogs.push_back({subRelPath.generic_string(), subLog.stdoutStr});
        }
    }

    const std::string mdText = FormatMarkdownMetadata(InRecord, result, InOpts, rootLogOut.stdoutStr, subLogs);
    const auto mdPath = InOutputDir / (archiveBase + ".md");
    std::ofstream mdf(mdPath);
    if (mdf) {
        mdf << mdText;
    }

    return result;
}

auto RunExportWithExecutor(const ExportOptions& InOpts,
                           const std::vector<ExportRecord>& InExportList,
                           const std::filesystem::path& InOutputDir,
                           const std::filesystem::path& InMetadataDir,
                           const ShellExecutor& InExec) -> int {
    std::vector<ExportResult> results;
    bool anyArchiveFailed = false;
    bool anyValidationFailed = false;
    bool rootSmokePassed = true;

    std::string currentGroup;
    for (const auto& record : InExportList) {
        const auto relativePath = std::filesystem::path(record.relativeRepoPath);
        const auto group = GroupFromRelativePath(relativePath);
        if (group != currentGroup) {
            currentGroup = group;
            std::cout << "\nGROUP: " << currentGroup << "\n";
        }

        const auto result = ExportOneRepo(record, InOpts, InOutputDir, InMetadataDir, InExec);
        if (!result.success) {
            std::cerr << "kog export: failed to export '" << record.repoName
                      << "' (" << record.relativeRepoPath << "): " << result.errorMessage << "\n";
            anyArchiveFailed = true;
        } else {
            const std::string displayName = record.isRoot ? record.repoName : record.relativeRepoPath;
            std::cout << FormatProgressLine(displayName, result.archivePath) << "\n";
            const int validationExit = RunReleaseArchiveValidation(record, result, InOpts, InExportList.front().repoPath, InExec);
            if (validationExit != 0) {
                anyValidationFailed = true;
                if (record.isRoot) {
                    rootSmokePassed = false;
                }
            }
        }
        results.push_back(result);
    }

    std::cout << "\n" << FormatSummary(InOutputDir, InMetadataDir, results);

    // Write export-manifest.json for --single mode and --subtree mode.
    // Conditioned on archive success only — validation failure does not block manifest.
    if ((InOpts.single || !InOpts.subtreePath.empty()) && !anyArchiveFailed) {
        // Find the root result (first successful result from the root record)
        const ExportResult* rootResult = nullptr;
        const ExportRecord* rootRecord = nullptr;
        for (std::size_t i = 0; i < results.size() && i < InExportList.size(); ++i) {
            if (InExportList[i].isRoot && results[i].success) {
                rootResult = &results[i];
                rootRecord = &InExportList[i];
                break;
            }
        }

        if (rootResult != nullptr && rootRecord != nullptr) {
            ExportManifestData manifest;
            manifest.projectName = rootRecord->repoName;
            manifest.repository  = rootRecord->isSubtree ? rootRecord->repoPath.filename().generic_string() : rootRecord->repoName;
            manifest.exportMode  = rootRecord->isSubtree ? "subtree" : "single";
            manifest.singleArchive = true;
            manifest.createdAt   = CurrentUtcIso8601Export();
            manifest.platform    = DetectHostPlatform();
            manifest.rootCommit  = rootResult->rootCommit;
            manifest.rootBranch  = rootResult->rootBranch;
            manifest.archiveFile = rootResult->archivePath.generic_string();
            manifest.format      = InOpts.format;
            manifest.sizeBytes   = rootResult->sizeBytes;
            manifest.sha256      = rootResult->sha256;

            // Collect submodule commits for non-subtree single exports
            if (!rootRecord->isSubtree) {
                for (const auto& subRelPath : rootRecord->submodulePaths) {
                    const auto subRepoPath = rootRecord->repoPath / subRelPath;
                    const auto subSha = InExec("git", {"rev-parse", "--short", "HEAD"},
                                               shell::ExecMode::Capture, subRepoPath);
                    const auto subRemote = InExec("git", {"remote", "get-url", "origin"},
                                                  shell::ExecMode::Capture, subRepoPath);
                    std::string subCommit;
                    if (subSha.exitCode == 0) {
                        subCommit = TrimExport(subSha.stdoutStr);
                    }
                    manifest.submodules.push_back({subRelPath.generic_string(), subCommit});

                    ExportSubrepoManifestEntry entry;
                    entry.path = subRelPath.generic_string();
                    entry.commit = subCommit;
                    entry.mode = InOpts.includeSubrepos ? "expanded" : "pointer-only";
                    entry.contentIncluded = InOpts.includeSubrepos && !subCommit.empty();
                    entry.remote = (subRemote.exitCode == 0) ? TrimExport(subRemote.stdoutStr) : "";
                    entry.status = (!InOpts.includeSubrepos || !subCommit.empty()) ? "ok" : "failed";
                    if (entry.status == "failed") {
                        entry.error = "unable to resolve subrepo commit";
                    }
                    manifest.subrepoEntries.push_back(std::move(entry));
                }
            } else {
                manifest.hasSubtree = true;
                manifest.subtreeName = rootRecord->repoName;
                manifest.subtreeSourcePath = rootRecord->subtreeAbsPath.generic_string();
                manifest.subtreeRepositoryPath = rootRecord->repoPath.generic_string();
                manifest.subtreeRepoRelativePath = rootRecord->subtreeRepoRelativePath.generic_string();
                manifest.subtreeStripPath = !InOpts.keepSubtreePath;
            }
            manifest.smokeTestResult = rootSmokePassed ? "passed" : "failed";

            const std::string manifestJson = FormatExportManifestJson(manifest);

            // Derive the base name from the archive: <name>_revNNN.export-manifest.json
            const std::string archiveBase = rootResult->archiveName.substr(
                0, rootResult->archiveName.rfind('.'));
            const std::string manifestFilename = archiveBase + ".export-manifest.json";

            // Canonical copy: <cwd>/.kano/tmp/<name>_revNNN.export-manifest.json
            const auto canonicalDir = InExportList.front().repoPath / ".kano" / "tmp";
            std::error_code ec;
            std::filesystem::create_directories(canonicalDir, ec);
            if (!ec) {
                std::ofstream cf(canonicalDir / manifestFilename);
                if (cf) {
                    cf << manifestJson;
                }
            }

            // Sibling copy: alongside the archive in the output directory
            std::ofstream sf(InOutputDir / manifestFilename);
            if (sf) {
                sf << manifestJson;
            }
        }
    }

    return (anyArchiveFailed || anyValidationFailed) ? 1 : 0;
}

namespace {

auto ResolveSubtreeExportRecord(const ExportOptions& InOpts,
                                const std::filesystem::path& InCwd,
                                const ShellExecutor& InExec,
                                ExportRecord& OutRecord,
                                std::string& OutError) -> bool {
    std::error_code ec;
    auto subtreeAbs = InOpts.subtreePath;
    if (subtreeAbs.is_relative()) {
        subtreeAbs = (InCwd / subtreeAbs).lexically_normal();
    }
    if (!std::filesystem::exists(subtreeAbs, ec) || !std::filesystem::is_directory(subtreeAbs, ec)) {
        OutError = "kog export: --subtree must point to an existing directory";
        return false;
    }

    const auto top = InExec("git", {"-C", subtreeAbs.generic_string(), "rev-parse", "--show-toplevel"}, shell::ExecMode::Capture, InCwd);
    if (top.exitCode != 0) {
        OutError = "kog export: failed to resolve containing git repository for --subtree";
        return false;
    }
    auto repoRoot = std::filesystem::path(TrimExport(top.stdoutStr)).lexically_normal();
    if (repoRoot.empty()) {
        OutError = "kog export: failed to resolve repository root for --subtree";
        return false;
    }
    const auto rel = std::filesystem::relative(subtreeAbs, repoRoot, ec);
    if (ec || rel.empty() || rel.generic_string().rfind("..", 0) == 0) {
        OutError = "kog export: --subtree must be inside its containing git repository";
        return false;
    }

    OutRecord.repoPath = repoRoot;
    OutRecord.relativeRepoPath = ".";
    OutRecord.isRoot = true;
    OutRecord.isSubtree = true;
    OutRecord.subtreeAbsPath = subtreeAbs;
    OutRecord.subtreeRepoRelativePath = rel.lexically_normal();
    OutRecord.subtreeDisplayPath = rel.generic_string();
    OutRecord.repoName = !InOpts.exportName.empty() ? InOpts.exportName : subtreeAbs.filename().generic_string();
    return true;
}

auto RunExport(const ExportOptions& InOpts) -> int {
    ExportOptions opts = InOpts;
    if (!opts.single && opts.splitSubrepoDepth == 0 && opts.subtreePath.empty()) {
        // Default behavior: split at root and export next layer as integrated single archives.
        opts.splitSubrepoDepth = 1;
    }
    if (opts.single) {
        opts.includeSubrepos = true;
    }

    if (!ValidateOptions(opts)) {
        return 1;
    }

    const auto cwd = std::filesystem::current_path().lexically_normal();

    // Real shell executor wraps shell::ExecuteCommand
    ShellExecutor realExec = [](const std::string& cmd,
                                const std::vector<std::string>& args,
                                shell::ExecMode mode,
                                std::optional<std::filesystem::path> workDir) {
        return shell::ExecuteCommand(cmd, args, mode, workDir);
    };

    if (opts.subtreePath.empty()) {
        // Validate workspace root is a git repo
        const auto gitCheck = realExec(
            "git", {"rev-parse", "--git-dir"},
            shell::ExecMode::Capture, cwd);
        if (gitCheck.exitCode != 0) {
            std::cerr << "kog export: current directory is not a git repository\n";
            return 1;
        }
    }

    // Discover subrepos
    workspace::DiscoveryResult discovery;
    if (opts.subtreePath.empty()) {
        workspace::DiscoverOptions discoverOpts;
        discoverOpts.rootDir = cwd;
        discoverOpts.scope = workspace::DiscoverScope::RegisteredOnly;
        discovery = workspace::DiscoverRepos(discoverOpts);
    }

    const auto outputDir = opts.outputDir.empty()
        ? ResolveOutputDir(cwd, "")
        : opts.outputDir;
    const auto metadataDir = outputDir / "metadata";

    std::vector<ExportRecord> exportList;
    if (!opts.subtreePath.empty()) {
        ExportRecord subtreeRecord;
        std::string err;
        if (!ResolveSubtreeExportRecord(opts, cwd, realExec, subtreeRecord, err)) {
            std::cerr << err << "\n";
            return 1;
        }
        exportList.push_back(std::move(subtreeRecord));
    } else {
        exportList = BuildExportList(cwd, discovery.repos, opts.noRecursive, opts.single, opts.splitSubrepoDepth, realExec);
    }

    if (opts.dryRun) {
        ExportOptions optsWithDir = opts;
        optsWithDir.outputDir = outputDir;
        if (!optsWithDir.subtreePath.empty()) {
            const auto& rec = exportList.front();
            std::cout << "Dry-run export plan\n";
            std::cout << "ExportMode: subtree\n";
            std::cout << "GitRepoRoot: " << rec.repoPath.generic_string() << "\n";
            std::cout << "SubtreeAbsolutePath: " << rec.subtreeAbsPath.generic_string() << "\n";
            std::cout << "SubtreeRepoRelativePath: " << rec.subtreeRepoRelativePath.generic_string() << "\n";
            std::cout << "StripSubtreePath: " << (!optsWithDir.keepSubtreePath ? "true" : "false") << "\n";
            std::cout << "OutputDir: " << outputDir.generic_string() << "\n";
            const auto fakeRev = FormatRevision(0, optsWithDir.revPad);
            std::cout << "Archive: " << ComputeArchiveName(rec.repoName, fakeRev, optsWithDir.format) << "\n";
            return 0;
        }
        if (optsWithDir.splitSubrepoDepth > 0) {
            std::cout << "Dry-run export plan\n";
            std::cout << "ExportMode: split-subrepos\n";
            std::cout << "SplitSubrepoDepth: " << optsWithDir.splitSubrepoDepth << "\n";
            std::cout << "RootRepoIncluded: true\n";
            std::cout << "SplitModeAtDepth<=" << (optsWithDir.splitSubrepoDepth - 1)
                      << ", SingleModeAtDepth=" << optsWithDir.splitSubrepoDepth << "\n";
            std::cout << "OutputDir: " << outputDir.generic_string() << "\n";
            if (exportList.empty()) {
                std::cout << "No matching child subrepos found for split export.\n";
                return 0;
            }
        }
        std::cout << FormatDryRunPlan(exportList, optsWithDir);
        return 0;
    }

    if (opts.splitSubrepoDepth > 0 && exportList.empty()) {
        std::cout << "kog export: warning: no matching child subrepos found for --split-subrepo-depth="
                  << opts.splitSubrepoDepth << "\n";
        return 0;
    }

    // Create output directories
    std::error_code ec;
    std::filesystem::create_directories(outputDir, ec);
    if (ec) {
        std::cerr << "kog export: failed to create output directory: " << ec.message() << "\n";
        return 1;
    }
    std::filesystem::create_directories(metadataDir, ec);
    if (ec) {
        std::cerr << "kog export: failed to create metadata directory: " << ec.message() << "\n";
        return 1;
    }

    return RunExportWithExecutor(opts, exportList, outputDir, metadataDir, realExec);
}

} // anonymous namespace

} // namespace kano::git::commands

// ---------------------------------------------------------------------------
// Public registration function (task 5)
// ---------------------------------------------------------------------------

namespace kano::git::commands {

void RegisterExport(CLI::App& InApp) {
    auto* cmd = InApp.add_subcommand("export",
        "Package a multi-repo workspace into archive files, one per repository");

    auto* outputDir = new std::string{};
    auto* format = new std::string{"tar"};
    auto* prefix = new std::string{};
    auto* includePaths = new std::vector<std::string>{};
    auto* excludePaths = new std::vector<std::string>{};
    auto* includeSubmoduleStubs = new bool{false};
    auto* noRecursive = new bool{false};
    auto* dryRun = new bool{false};
    auto* noMetadata = new bool{false};
    auto* revPad = new int{3};
    auto* source = new std::string{"head"};
    auto* single = new bool{false};
    auto* logCount = new int{10};
    auto* noValidateReleaseArchive = new bool{false};
    auto* forceValidateReleaseArchive = new bool{false};
    auto* subtree = new std::string{};
    auto* exportName = new std::string{};
    auto* keepSubtreePath = new bool{false};
    auto* splitSubrepoDepth = new int{1};
    auto* includeSubrepos = new bool{false};
    auto* allowMissingSubrepos = new bool{false};

    cmd->add_option("--output", *outputDir,
        "Output directory for archives (default: <cwd>/.kano/tmp/git/export/)");
    cmd->add_option("--format", *format,
        "Archive format: tar|zip (default: tar)");
    cmd->add_option("--prefix", *prefix,
        "Override archive root prefix (default: <repo-name>/)");
    cmd->add_option("--include-path", *includePaths,
        "Include only repo-relative paths matching this prefix or glob; repeatable");
    cmd->add_option("--exclude-path", *excludePaths,
        "Exclude repo-relative paths matching this prefix or glob; repeatable");
    cmd->add_flag("--include-submodule-stubs", *includeSubmoduleStubs,
        "Include empty submodule placeholder dirs in root archive (working-tree only)");
    cmd->add_flag("--no-recursive", *noRecursive,
        "Export only the workspace root repo, skip subrepos");
    cmd->add_flag("--dry-run", *dryRun,
        "Print what would be exported without creating files");
    cmd->add_flag("--no-metadata", *noMetadata,
        "Skip manifest and SHA-256 generation");
    cmd->add_option("--rev-pad", *revPad,
        "Revision zero-padding width (default: 3)");
    cmd->add_option("--source", *source,
        "Archive source: head|working-tree (default: head)");
    cmd->add_flag("--single", *single,
        "Export the entire workspace into a single archive with subrepo/submodule contents included (implies --no-recursive)");
    cmd->add_option("--log-count", *logCount,
        "Number of history log entries to include in metadata (default: 10)");
    cmd->add_flag("--no-validate-release-archive", *noValidateReleaseArchive,
        "Skip automatic smoke validation for single-file release archives");
    cmd->add_flag("--validate-release-archive", *forceValidateReleaseArchive,
        "Require smoke validation for the root release archive; fail if it cannot run");
    cmd->add_option("--subtree", *subtree,
        "Path to a directory to export as a standalone archive root");
    cmd->add_option("--name", *exportName,
        "Override export/archive name for --subtree mode (default: subtree basename)");
    cmd->add_flag("--keep-subtree-path", *keepSubtreePath,
        "Preserve repo-relative subtree path inside archive (default: strip subtree path)");
    cmd->add_option("--split-subrepo-depth", *splitSubrepoDepth,
        "Split export by depth: repos above N stay split archives, repos at depth N switch to integrated single-export behavior. Set 0 for full recursive split mode (default: 1)");
    cmd->add_flag("--include-subrepos", *includeSubrepos,
        "Explicitly request expanded --single mode; retained for compatibility because --single now includes subrepo/submodule working-tree contents by default");
    cmd->add_flag("--allow-missing-subrepos", *allowMissingSubrepos,
        "When used with --include-subrepos, continue export even if some subrepos are unavailable");

    cmd->callback([=]() {
        ExportOptions opts;
        opts.outputDir = *outputDir;
        opts.format = *format;
        opts.prefix = *prefix;
        opts.includePaths = *includePaths;
        opts.excludePaths = *excludePaths;
        opts.includeSubmoduleStubs = *includeSubmoduleStubs;
        opts.noRecursive = *noRecursive;
        opts.dryRun = *dryRun;
        opts.noMetadata = *noMetadata;
        opts.revPad = *revPad;
        opts.source = *source;
        opts.single = *single;
        opts.logCount = *logCount;
        opts.validateReleaseArchive = !*noValidateReleaseArchive;
        opts.forceValidateReleaseArchive = *forceValidateReleaseArchive;
        opts.subtreePath = *subtree;
        opts.exportName = *exportName;
        opts.keepSubtreePath = *keepSubtreePath;
        opts.splitSubrepoDepth = *splitSubrepoDepth;
        opts.includeSubrepos = *includeSubrepos;
        opts.allowMissingSubrepos = *allowMissingSubrepos;

        // Keep --source default as "head" for deterministic archive metadata
        // and Git-index executable mode preservation across platforms.

        const int exitCode = RunExport(opts);
        if (exitCode != 0) {
            throw CLI::RuntimeError(exitCode);
        }
    });
}

} // namespace kano::git::commands

