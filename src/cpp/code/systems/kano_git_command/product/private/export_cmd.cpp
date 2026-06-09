// export_cmd.cpp — kog export command implementation
//
// Packages a multi-repo workspace into a set of archive files, one per
// repository. Follows the anonymous-namespace helper pattern from status_cmd.cpp.

#include "export_helpers.hpp"

#include <CLI/CLI.hpp>
#include "discovery.hpp"
#include "shell_executor.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <format>
#include <functional>
#include <fstream>
#include <iomanip>
#include <map>
#include <iostream>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
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
    std::map<std::string, std::filesystem::path> sourcePathByArchivePath;

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
                    const std::string archivePath = prefix + line;
                    allFiles.push_back(archivePath);
                    sourcePathByArchivePath[archivePath] = repoPath / std::filesystem::path(line);
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

    std::set<std::string> allFileSet(allFiles.begin(), allFiles.end());
    std::map<std::string, std::string> symlinkTargetByPath;

    auto normalizeArchivePath = [](const std::filesystem::path& InPath) {
        std::string out = InPath.lexically_normal().generic_string();
        while (out.starts_with("./")) {
            out.erase(0, 2);
        }
        return out;
    };

    for (const auto& archivePath : allFiles) {
        const auto sourceIt = sourcePathByArchivePath.find(archivePath);
        if (sourceIt == sourcePathByArchivePath.end()) {
            continue;
        }
        std::error_code ec;
        if (!std::filesystem::is_symlink(sourceIt->second, ec) || ec) {
            continue;
        }
        const auto target = std::filesystem::read_symlink(sourceIt->second, ec);
        if (ec || target.empty() || target.is_absolute()) {
            continue;
        }

        const auto targetArchivePath = normalizeArchivePath(
            std::filesystem::path(archivePath).parent_path() / target);
        if (!targetArchivePath.empty() && allFileSet.contains(targetArchivePath)) {
            symlinkTargetByPath[archivePath] = targetArchivePath;
        }
    }

    // Keep natural archive order except where Windows/GNU tar needs a symlink's
    // in-archive target to be emitted first.
    std::vector<std::string> orderedFiles;
    orderedFiles.reserve(allFiles.size());
    std::set<std::string> emitted;
    std::set<std::string> visiting;
    std::function<void(const std::string&)> emit = [&](const std::string& archivePath) {
        if (emitted.contains(archivePath)) {
            return;
        }
        if (visiting.contains(archivePath)) {
            return;
        }
        visiting.insert(archivePath);
        const auto targetIt = symlinkTargetByPath.find(archivePath);
        if (targetIt != symlinkTargetByPath.end()) {
            emit(targetIt->second);
        }
        visiting.erase(archivePath);
        if (!emitted.contains(archivePath)) {
            emitted.insert(archivePath);
            orderedFiles.push_back(archivePath);
        }
    };

    for (const auto& archivePath : allFiles) {
        emit(archivePath);
    }

    return orderedFiles;
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

namespace {

auto ToLowerExportUpload(std::string InValue) -> std::string {
    std::transform(InValue.begin(), InValue.end(), InValue.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return InValue;
}

auto EndsWithCaseInsensitive(const std::string& InValue, const std::string& InSuffix) -> bool {
    if (InValue.size() < InSuffix.size()) {
        return false;
    }
    return ToLowerExportUpload(InValue.substr(InValue.size() - InSuffix.size())) == ToLowerExportUpload(InSuffix);
}

auto NormalizeRcloneRemote(std::string InRemote) -> std::string {
    InRemote = TrimExport(std::move(InRemote));
    if (!InRemote.empty() && InRemote.back() == ':') {
        InRemote.pop_back();
    }
    return InRemote;
}

auto TrimSlashes(std::string InValue) -> std::string {
    InValue = TrimExport(std::move(InValue));
    while (!InValue.empty() && (InValue.front() == '/' || InValue.front() == '\\')) {
        InValue.erase(InValue.begin());
    }
    while (!InValue.empty() && (InValue.back() == '/' || InValue.back() == '\\')) {
        InValue.pop_back();
    }
    std::replace(InValue.begin(), InValue.end(), '\\', '/');
    return InValue;
}

auto NormalizeUploadLayout(std::string InValue) -> std::string {
    InValue = TrimSlashes(std::move(InValue));
    while (InValue.rfind("./", 0) == 0) {
        InValue.erase(0, 2);
    }
    return InValue;
}

auto IsSafeRelativeUploadLayout(const std::string& InLayout) -> bool {
    if (InLayout.empty()) {
        return true;
    }
    const bool hasControlCharacter = std::any_of(InLayout.begin(), InLayout.end(), [](unsigned char ch) {
        return ch < 0x20U || ch == 0x7fU;
    });
    if (hasControlCharacter || InLayout.find(':') != std::string::npos) {
        return false;
    }
    const auto path = std::filesystem::path(InLayout);
    if (path.is_absolute()) {
        return false;
    }
    for (const auto& part : path) {
        const auto text = part.generic_string();
        if (text.empty() || text == ".") {
            continue;
        }
        if (text == "..") {
            return false;
        }
    }
    return true;
}

auto ResolveLocalUploadTargetPath(const ExportUploadConfig& InConfig) -> std::filesystem::path {
    const std::string layout = NormalizeUploadLayout(InConfig.layout);
    if (layout.empty()) {
        return InConfig.localSyncFolder;
    }
    return InConfig.localSyncFolder / std::filesystem::path(layout);
}

auto BuildRcloneObjectPath(const ExportUploadConfig& InConfig, const std::string& InFilename) -> std::string {
    const std::string remote = NormalizeRcloneRemote(InConfig.rcloneRemote);
    std::string destination = TrimSlashes(InConfig.rcloneDestination);
    const std::string layout = NormalizeUploadLayout(InConfig.layout);
    if (!layout.empty()) {
        destination = destination.empty() ? layout : (destination + "/" + layout);
    }
    if (destination.empty()) {
        return remote + ":" + InFilename;
    }
    return remote + ":" + destination + "/" + InFilename;
}

auto ContainsControlCharacter(const std::string& InValue) -> bool {
    return std::any_of(InValue.begin(), InValue.end(), [](unsigned char ch) {
        return ch < 0x20U || ch == 0x7fU;
    });
}

auto ContainsCredentialMarker(const std::string& InValue) -> bool {
    const auto lower = ToLowerExportUpload(InValue);
    static constexpr std::array<std::string_view, 8> markers{
        "token", "password", "passwd", "secret", "api_key", "apikey", "authorization", "bearer"
    };
    for (const auto marker : markers) {
        if (lower.find(marker) != std::string::npos) {
            return true;
        }
    }
    return false;
}

auto IsSafeRcloneRemoteName(const std::string& InRemote) -> bool {
    const std::string remote = NormalizeRcloneRemote(InRemote);
    if (remote.empty() || remote.front() == '-' || ContainsControlCharacter(remote) || ContainsCredentialMarker(remote)) {
        return false;
    }
    for (const unsigned char ch : remote) {
        if (!(std::isalnum(ch) != 0 || ch == '_' || ch == '-' || ch == '.')) {
            return false;
        }
    }
    return true;
}

auto IsSafeRcloneDestination(const std::string& InDestination) -> bool {
    if (ContainsControlCharacter(InDestination) || ContainsCredentialMarker(InDestination)) {
        return false;
    }
    return InDestination.find(':') == std::string::npos;
}

auto ValidateRcloneUploadConfig(const ExportUploadConfig& InConfig, std::string& OutError) -> bool {
    if (!IsSafeRcloneRemoteName(InConfig.rcloneRemote)) {
        OutError = "rclone remote must be a configured remote name without inline credentials";
        return false;
    }
    if (!IsSafeRcloneDestination(InConfig.rcloneDestination)) {
        OutError = "rclone destination must not contain credentials, control characters, or remote separators";
        return false;
    }
    if (!IsSafeRelativeUploadLayout(NormalizeUploadLayout(InConfig.layout))) {
        OutError = "upload layout must be a safe relative path without '..', drive names, or control characters";
        return false;
    }
    return true;
}

auto IsPathWithinDirectory(const std::filesystem::path& InPath,
                           const std::filesystem::path& InDirectory) -> bool {
    std::error_code ec;
    const auto base = std::filesystem::weakly_canonical(InDirectory, ec);
    if (ec || base.empty()) {
        return false;
    }
    const auto path = std::filesystem::weakly_canonical(InPath, ec);
    if (ec || path.empty()) {
        return false;
    }
    const auto relative = path.lexically_relative(base);
    const auto relativeText = relative.generic_string();
    return !relativeText.empty() && relativeText != ".." && relativeText.rfind("../", 0) != 0;
}

auto RotateRight32(std::uint32_t InValue, int InBits) -> std::uint32_t {
    return (InValue >> InBits) | (InValue << (32 - InBits));
}

auto FormatSha256State(const std::array<std::uint32_t, 8>& InState) -> std::string {
    std::ostringstream out;
    out << std::hex << std::setfill('0');
    for (const auto part : InState) {
        out << std::setw(8) << part;
    }
    return out.str();
}

void ProcessSha256Block(const unsigned char* InBlock,
                        std::array<std::uint32_t, 8>& InOutState) {
    static constexpr std::array<std::uint32_t, 64> kConstants{
        0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
        0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U, 0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
        0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
        0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U, 0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
        0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
        0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U, 0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
        0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
        0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U, 0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U
    };

    std::array<std::uint32_t, 64> words{};
    for (std::size_t i = 0; i < 16; ++i) {
        const std::size_t p = i * 4;
        words[i] = (static_cast<std::uint32_t>(InBlock[p]) << 24) |
                   (static_cast<std::uint32_t>(InBlock[p + 1]) << 16) |
                   (static_cast<std::uint32_t>(InBlock[p + 2]) << 8) |
                   static_cast<std::uint32_t>(InBlock[p + 3]);
    }
    for (std::size_t i = 16; i < 64; ++i) {
        const std::uint32_t s0 = RotateRight32(words[i - 15], 7) ^ RotateRight32(words[i - 15], 18) ^ (words[i - 15] >> 3);
        const std::uint32_t s1 = RotateRight32(words[i - 2], 17) ^ RotateRight32(words[i - 2], 19) ^ (words[i - 2] >> 10);
        words[i] = words[i - 16] + s0 + words[i - 7] + s1;
    }

    std::uint32_t a = InOutState[0];
    std::uint32_t b = InOutState[1];
    std::uint32_t c = InOutState[2];
    std::uint32_t d = InOutState[3];
    std::uint32_t e = InOutState[4];
    std::uint32_t f = InOutState[5];
    std::uint32_t g = InOutState[6];
    std::uint32_t h = InOutState[7];

    for (std::size_t i = 0; i < 64; ++i) {
        const std::uint32_t s1 = RotateRight32(e, 6) ^ RotateRight32(e, 11) ^ RotateRight32(e, 25);
        const std::uint32_t ch = (e & f) ^ ((~e) & g);
        const std::uint32_t temp1 = h + s1 + ch + kConstants[i] + words[i];
        const std::uint32_t s0 = RotateRight32(a, 2) ^ RotateRight32(a, 13) ^ RotateRight32(a, 22);
        const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        const std::uint32_t temp2 = s0 + maj;
        h = g;
        g = f;
        f = e;
        e = d + temp1;
        d = c;
        c = b;
        b = a;
        a = temp1 + temp2;
    }

    InOutState[0] += a;
    InOutState[1] += b;
    InOutState[2] += c;
    InOutState[3] += d;
    InOutState[4] += e;
    InOutState[5] += f;
    InOutState[6] += g;
    InOutState[7] += h;
}

class Sha256Stream {
public:
    void Update(const unsigned char* InData, std::size_t InSize) {
        totalBytes_ += static_cast<std::uint64_t>(InSize);
        std::size_t offset = 0;
        if (bufferSize_ > 0) {
            const std::size_t toCopy = std::min(InSize, buffer_.size() - bufferSize_);
            std::copy_n(InData, toCopy, buffer_.begin() + static_cast<std::ptrdiff_t>(bufferSize_));
            bufferSize_ += toCopy;
            offset += toCopy;
            if (bufferSize_ == buffer_.size()) {
                ProcessSha256Block(buffer_.data(), state_);
                bufferSize_ = 0;
            }
        }

        while (offset + buffer_.size() <= InSize) {
            ProcessSha256Block(InData + offset, state_);
            offset += buffer_.size();
        }

        if (offset < InSize) {
            bufferSize_ = InSize - offset;
            std::copy_n(InData + offset, bufferSize_, buffer_.begin());
        }
    }

    auto Finalize() -> std::string {
        const std::uint64_t bitLength = totalBytes_ * 8ULL;
        buffer_[bufferSize_++] = 0x80U;
        if (bufferSize_ > 56U) {
            std::fill(buffer_.begin() + static_cast<std::ptrdiff_t>(bufferSize_), buffer_.end(), 0U);
            ProcessSha256Block(buffer_.data(), state_);
            bufferSize_ = 0;
        }
        std::fill(buffer_.begin() + static_cast<std::ptrdiff_t>(bufferSize_), buffer_.begin() + 56, 0U);
        for (std::size_t i = 0; i < 8; ++i) {
            buffer_[56 + i] = static_cast<unsigned char>((bitLength >> ((7 - i) * 8)) & 0xffU);
        }
        ProcessSha256Block(buffer_.data(), state_);
        return FormatSha256State(state_);
    }

private:
    std::array<std::uint32_t, 8> state_{0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
                                        0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U};
    std::array<unsigned char, 64> buffer_{};
    std::size_t bufferSize_ = 0;
    std::uint64_t totalBytes_ = 0;
};

auto ComputeFileSha256(const std::filesystem::path& InPath) -> std::string {
    std::ifstream in(InPath, std::ios::binary);
    if (!in) {
        return {};
    }
    Sha256Stream stream;
    std::array<char, 65536> buffer{};
    while (in.read(buffer.data(), static_cast<std::streamsize>(buffer.size())) || in.gcount() > 0) {
        const auto count = in.gcount();
        stream.Update(reinterpret_cast<const unsigned char*>(buffer.data()), static_cast<std::size_t>(count));
    }
    return stream.Finalize();
}

auto WriteUploadSha256Sidecar(const std::filesystem::path& InFilePath,
                              const std::filesystem::path& InSidecarPath) -> std::string {
    const std::string sha = ComputeFileSha256(InFilePath);
    if (sha.empty()) {
        return {};
    }
    std::ofstream out(InSidecarPath, std::ios::binary | std::ios::trunc);
    if (!out) {
        return {};
    }
    out << sha << "  " << InFilePath.filename().generic_string() << "\n";
    return sha;
}

auto CopyFileNoOverwrite(const std::filesystem::path& InSource,
                         const std::filesystem::path& InDestination,
                         std::string& OutError) -> bool {
    std::error_code ec;
    if (std::filesystem::exists(InDestination, ec)) {
        OutError = "destination already exists: " + InDestination.generic_string();
        return false;
    }
    if (!std::filesystem::copy_file(InSource, InDestination, std::filesystem::copy_options::none, ec)) {
        OutError = "failed to copy " + InSource.generic_string() + " to " + InDestination.generic_string();
        if (ec) {
            OutError += ": " + ec.message();
        }
        return false;
    }
    return true;
}

auto VerifyCopiedFile(const std::filesystem::path& InSource,
                      const std::filesystem::path& InDestination,
                      const std::string& InExpectedSha,
                      std::string& OutError) -> bool {
    std::error_code ec;
    const auto sourceSize = std::filesystem::file_size(InSource, ec);
    if (ec) {
        OutError = "failed to read source archive size: " + ec.message();
        return false;
    }
    const auto destinationSize = std::filesystem::file_size(InDestination, ec);
    if (ec) {
        OutError = "failed to read copied archive size: " + ec.message();
        return false;
    }
    if (sourceSize != destinationSize) {
        OutError = "copied archive size mismatch";
        return false;
    }
    const std::string copiedSha = ComputeFileSha256(InDestination);
    if (copiedSha.empty() || copiedSha != InExpectedSha) {
        OutError = "copied archive sha256 mismatch";
        return false;
    }
    return true;
}

auto WriteUploadManifestJson(const ExportUploadRequest& InRequest,
                             const ExportUploadResult& InResult,
                             const std::filesystem::path& InOutputPath,
                             const std::string& InSha256) -> bool {
    std::ofstream out(InOutputPath, std::ios::binary | std::ios::trunc);
    if (!out) {
        return false;
    }
    std::error_code ec;
    const auto sourceSize = std::filesystem::file_size(InRequest.archivePath, ec);
    out << "{\n";
    out << "  \"schemaVersion\": 1,\n";
    out << "  \"kind\": \"kog-export-upload\",\n";
    out << "  \"generator\": \"kog export upload\",\n";
    out << "  \"createdAt\": \"" << JsonEscape(CurrentUtcIso8601Export()) << "\",\n";
    out << "  \"uploadedAt\": \"" << JsonEscape(CurrentUtcIso8601Export()) << "\",\n";
    out << "  \"target\": \"" << JsonEscape(InRequest.config.target) << "\",\n";
    out << "  \"backend\": \"" << JsonEscape(!InResult.backend.empty() ? InResult.backend : InRequest.config.target) << "\",\n";
    out << "  \"connectorHint\": \"" << JsonEscape(!InResult.connectorHint.empty() ? InResult.connectorHint : InRequest.config.connectorHint) << "\",\n";
    out << "  \"layout\": \"" << JsonEscape(NormalizeUploadLayout(InRequest.config.layout)) << "\",\n";
    out << "  \"copyManifest\": " << (InRequest.config.copyManifest ? "true" : "false") << ",\n";
    out << "  \"copySha256\": " << (InRequest.config.copySha256 ? "true" : "false") << ",\n";
    out << "  \"returnUrl\": " << (InRequest.config.returnUrl ? "true" : "false") << ",\n";
    out << "  \"linkMode\": \"" << JsonEscape(InRequest.config.linkMode) << "\",\n";
    out << "  \"archiveFile\": \"" << JsonEscape(InRequest.archivePath.generic_string()) << "\",\n";
    out << "  \"sourceArchive\": \"" << JsonEscape(InRequest.archivePath.generic_string()) << "\",\n";
    out << "  \"archiveName\": \"" << JsonEscape(InRequest.archiveName) << "\",\n";
    out << "  \"remoteArchiveName\": \"" << JsonEscape(InResult.remoteArchiveName) << "\",\n";
    out << "  \"sourceSizeBytes\": " << (ec ? 0 : sourceSize) << ",\n";
    out << "  \"remotePath\": \"" << JsonEscape(InResult.remotePath) << "\",\n";
    out << "  \"localTargetPath\": \"" << JsonEscape(InResult.localTargetPath.generic_string()) << "\",\n";
    out << "  \"syncFolderPath\": \"" << JsonEscape(InResult.syncFolderPath.generic_string()) << "\",\n";
    out << "  \"copiedArchivePath\": \"" << JsonEscape(InResult.copiedArchivePath.generic_string()) << "\",\n";
    out << "  \"copiedManifestPath\": \"" << JsonEscape(InResult.copiedManifestPath.generic_string()) << "\",\n";
    out << "  \"sha256SidecarPath\": \"" << JsonEscape(InResult.sha256SidecarPath.generic_string()) << "\",\n";
    out << "  \"sha256\": \"" << JsonEscape(InSha256) << "\",\n";
    out << "  \"sourceSha256\": \"" << JsonEscape(!InResult.sourceSha256.empty() ? InResult.sourceSha256 : InSha256) << "\",\n";
    out << "  \"visibility\": \"" << JsonEscape(InResult.visibility) << "\",\n";
    out << "  \"permissionChanged\": " << (InResult.permissionChanged ? "true" : "false") << ",\n";
    out << "  \"fileId\": ";
    if (InResult.fileId.has_value()) {
        out << "\"" << JsonEscape(*InResult.fileId) << "\"";
    } else {
        out << "null";
    }
    out << ",\n";
    out << "  \"webUrl\": ";
    if (InResult.webUrl.has_value()) {
        out << "\"" << JsonEscape(*InResult.webUrl) << "\"";
    } else {
        out << "null";
    }
    out << ",\n";
    out << "  \"urlStatus\": ";
    if (!InResult.urlStatus.empty()) {
        out << "\"" << JsonEscape(InResult.urlStatus) << "\"\n";
    } else {
        out << "null\n";
    }
    out << "}\n";
    return true;
}

auto ExtractDriveIdFromLsJson(const std::string& InText) -> std::optional<std::string> {
    const std::regex idRegex("\\\"ID\\\"\\s*:\\s*\\\"([^\\\"]+)\\\"");
    std::smatch match;
    if (std::regex_search(InText, match, idRegex) && match.size() > 1) {
        return match[1].str();
    }
    return std::nullopt;
}

auto ExtractRcloneRemoteType(const std::string& InText) -> std::string {
    std::istringstream input(InText);
    std::string line;
    while (std::getline(input, line)) {
        const auto equalsPos = line.find('=');
        if (equalsPos == std::string::npos) {
            continue;
        }
        const std::string key = ToLowerExportUpload(TrimExport(line.substr(0, equalsPos)));
        if (key == "type") {
            return TrimExport(line.substr(equalsPos + 1));
        }
    }
    return {};
}

auto FindNewestExportManifest(const std::filesystem::path& InRoot) -> std::filesystem::path {
    const auto tmpRoot = InRoot / ".kano" / "tmp";
    std::error_code ec;
    if (!std::filesystem::exists(tmpRoot, ec)) {
        return {};
    }
    std::filesystem::path newest;
    std::filesystem::file_time_type newestTime{};
    for (const auto& entry : std::filesystem::recursive_directory_iterator(tmpRoot, std::filesystem::directory_options::skip_permission_denied, ec)) {
        if (ec) {
            break;
        }
        if (!entry.is_regular_file(ec)) {
            continue;
        }
        const auto path = entry.path();
        if (!EndsWithCaseInsensitive(path.filename().generic_string(), ".export-manifest.json")) {
            continue;
        }
        const auto modified = std::filesystem::last_write_time(path, ec);
        if (ec) {
            continue;
        }
        if (newest.empty() || modified > newestTime) {
            newest = path;
            newestTime = modified;
        }
    }
    return newest;
}

auto ReadTextFileForUpload(const std::filesystem::path& InPath) -> std::string {
    std::ifstream in(InPath, std::ios::binary);
    if (!in) {
        return {};
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

auto ExtractJsonStringField(const std::string& InJson, const std::string& InField) -> std::string {
    const std::regex fieldRegex("\\\"" + InField + "\\\"\\s*:\\s*\\\"([^\\\"]+)\\\"");
    std::smatch match;
    if (std::regex_search(InJson, match, fieldRegex) && match.size() > 1) {
        std::string value = match[1].str();
        std::replace(value.begin(), value.end(), '\\', '/');
        return value;
    }
    return {};
}

auto ResolveArchiveFromExportManifest(const std::filesystem::path& InManifestPath,
                                      const std::filesystem::path& InCwd) -> std::filesystem::path {
    const auto archiveText = ExtractJsonStringField(ReadTextFileForUpload(InManifestPath), "archiveFile");
    if (archiveText.empty()) {
        return {};
    }
    std::filesystem::path archivePath = archiveText;
    if (archivePath.is_relative()) {
        archivePath = (InManifestPath.parent_path() / archivePath).lexically_normal();
        std::error_code ec;
        if (!std::filesystem::exists(archivePath, ec)) {
            archivePath = (InCwd / archiveText).lexically_normal();
        }
    }
    return archivePath;
}

auto GuessManifestForArchive(const std::filesystem::path& InArchivePath,
                             const std::filesystem::path& InCwd) -> std::filesystem::path {
    const auto archiveBase = InArchivePath.filename().generic_string().substr(
        0, InArchivePath.filename().generic_string().rfind('.'));
    const std::vector<std::filesystem::path> candidates{
        InArchivePath.parent_path() / (archiveBase + ".export-manifest.json"),
        InArchivePath.parent_path() / "metadata" / ComputeManifestName(archiveBase),
        InCwd / ".kano" / "tmp" / (archiveBase + ".export-manifest.json")
    };
    std::error_code ec;
    for (const auto& candidate : candidates) {
        if (std::filesystem::exists(candidate, ec) && std::filesystem::is_regular_file(candidate, ec)) {
            return candidate;
        }
    }
    return {};
}

auto ParseBoolConfigValue(const std::string& InValue) -> std::optional<bool> {
    const auto lower = ToLowerExportUpload(TrimExport(InValue));
    if (lower == "true" || lower == "1" || lower == "yes" || lower == "on") {
        return true;
    }
    if (lower == "false" || lower == "0" || lower == "no" || lower == "off") {
        return false;
    }
    return std::nullopt;
}

auto StripConfigQuotes(std::string InValue) -> std::string {
    InValue = TrimExport(std::move(InValue));
    if (InValue.size() >= 2 && ((InValue.front() == '"' && InValue.back() == '"') ||
                                (InValue.front() == '\'' && InValue.back() == '\''))) {
        return InValue.substr(1, InValue.size() - 2);
    }
    return InValue;
}

auto IsDirectUploadBackendName(const std::string& InTargetName) -> bool {
    return InTargetName == "local-sync-folder" || InTargetName == "rclone" || InTargetName == "gdrive-api";
}

auto NormalizeUploadTargetType(std::string InValue) -> std::string {
    InValue = ToLowerExportUpload(TrimExport(std::move(InValue)));
    std::replace(InValue.begin(), InValue.end(), '_', '-');
    if (InValue == "local-sync-folder" || InValue == "local-sync" || InValue == "local") {
        return "local-sync-folder";
    }
    if (InValue == "rclone") {
        return "rclone";
    }
    if (InValue == "gdrive-api" || InValue == "google-drive-api") {
        return "gdrive-api";
    }
    return InValue;
}

auto UploadLayerHasAnyValue(const ExportUploadConfigLayer& InLayer) -> bool {
    return InLayer.target.has_value() || InLayer.localSyncFolder.has_value() || InLayer.rcloneRemote.has_value() ||
           InLayer.rcloneDestination.has_value() || InLayer.layout.has_value() || InLayer.copyManifest.has_value() ||
           InLayer.copySha256.has_value() || InLayer.returnUrl.has_value() || InLayer.linkMode.has_value() ||
           InLayer.backend.has_value() || InLayer.connectorHint.has_value() || InLayer.publicLink.has_value() ||
           InLayer.yes.has_value();
}

auto HomeDirectoryForExportUpload() -> std::filesystem::path {
#if defined(_WIN32)
    if (const char* userProfile = std::getenv("USERPROFILE"); userProfile != nullptr && *userProfile != '\0') {
        return std::filesystem::path(userProfile);
    }
    const char* homeDrive = std::getenv("HOMEDRIVE");
    const char* homePath = std::getenv("HOMEPATH");
    if (homeDrive != nullptr && homePath != nullptr && *homeDrive != '\0' && *homePath != '\0') {
        return std::filesystem::path(std::string(homeDrive) + std::string(homePath));
    }
#endif
    if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0') {
        return std::filesystem::path(home);
    }
    return {};
}

auto UserKogConfigPathForExportUpload() -> std::filesystem::path {
    const auto home = HomeDirectoryForExportUpload();
    if (home.empty()) {
        return {};
    }
    return home / ".kano" / "kog_config.toml";
}

auto RepoKogConfigPathForExportUpload(const std::filesystem::path& InCwd) -> std::filesystem::path {
    return InCwd / ".kano" / "kog_config.toml";
}

auto ExpandLeadingHomeForExportUpload(std::string InValue) -> std::filesystem::path {
    InValue = TrimExport(std::move(InValue));
    if (InValue == "~" || InValue.rfind("~/", 0) == 0 || InValue.rfind("~\\", 0) == 0) {
        const auto home = HomeDirectoryForExportUpload();
        if (!home.empty()) {
            if (InValue.size() == 1) {
                return home;
            }
            return home / InValue.substr(2);
        }
    }
    return std::filesystem::path(InValue);
}

auto StripInlineTomlComment(std::string InLine) -> std::string {
    bool inSingle = false;
    bool inDouble = false;
    for (std::size_t i = 0; i < InLine.size(); ++i) {
        const char ch = InLine[i];
        if (ch == '"' && !inSingle) {
            inDouble = !inDouble;
            continue;
        }
        if (ch == '\'' && !inDouble) {
            inSingle = !inSingle;
            continue;
        }
        if (ch == '#' && !inSingle && !inDouble) {
            return InLine.substr(0, i);
        }
    }
    return InLine;
}

auto ApplyUploadTargetConfigField(ExportUploadConfigLayer& OutLayer,
                                  const std::string& InKey,
                                  const std::string& InValue) -> void {
    const std::string key = TrimExport(InKey);
    const std::string value = StripConfigQuotes(InValue);
    if (key == "type" || key == "target") {
        OutLayer.target = NormalizeUploadTargetType(value);
    } else if (key == "path" || key == "localSyncFolder" || key == "local-sync-folder" || key == "local_sync_folder") {
        OutLayer.localSyncFolder = ExpandLeadingHomeForExportUpload(value);
    } else if (key == "remote" || key == "rcloneRemote" || key == "rclone-remote" || key == "rclone_remote") {
        OutLayer.rcloneRemote = value;
    } else if (key == "folder" || key == "destination" || key == "rcloneDestination" || key == "rclone-destination" || key == "rclone_destination") {
        OutLayer.rcloneDestination = value;
    } else if (key == "layout") {
        OutLayer.layout = NormalizeUploadLayout(value);
    } else if (key == "copy_manifest" || key == "copyManifest" || key == "copy-manifest") {
        OutLayer.copyManifest = ParseBoolConfigValue(value);
    } else if (key == "copy_sha256" || key == "copySha256" || key == "copy-sha256") {
        OutLayer.copySha256 = ParseBoolConfigValue(value);
    } else if (key == "return_url" || key == "returnUrl" || key == "return-url") {
        OutLayer.returnUrl = ParseBoolConfigValue(value);
    } else if (key == "link_mode" || key == "linkMode" || key == "link-mode") {
        OutLayer.linkMode = ToLowerExportUpload(TrimExport(value));
    } else if (key == "backend") {
        OutLayer.backend = value;
    } else if (key == "connector_hint" || key == "connectorHint" || key == "connector-hint") {
        OutLayer.connectorHint = value;
    } else if (key == "public_link" || key == "publicLink" || key == "public-link") {
        OutLayer.publicLink = ParseBoolConfigValue(value);
    } else if (key == "yes") {
        OutLayer.yes = ParseBoolConfigValue(value);
    }
}

auto ReadUploadDefaultTargetFromFile(const std::filesystem::path& InPath) -> std::optional<std::string> {
    std::ifstream in(InPath, std::ios::binary);
    if (!in) {
        return std::nullopt;
    }

    bool inUploadSection = false;
    std::string line;
    while (std::getline(in, line)) {
        line = TrimExport(StripInlineTomlComment(std::move(line)));
        if (line.empty()) {
            continue;
        }
        if (line.front() == '[' && line.back() == ']') {
            inUploadSection = (line.substr(1, line.size() - 2) == "export.upload");
            continue;
        }
        const auto equalsPos = line.find('=');
        if (equalsPos == std::string::npos) {
            continue;
        }
        const std::string key = TrimExport(line.substr(0, equalsPos));
        if (key == "export.upload.default_target" || (inUploadSection && key == "default_target")) {
            const std::string value = StripConfigQuotes(line.substr(equalsPos + 1));
            if (!value.empty()) {
                return value;
            }
        }
    }
    return std::nullopt;
}

auto LoadUploadTargetLayerFromFile(const std::filesystem::path& InPath,
                                   const std::string& InTargetName) -> ExportUploadConfigLayer {
    ExportUploadConfigLayer layer;
    std::ifstream in(InPath, std::ios::binary);
    if (!in) {
        return layer;
    }

    const std::string targetSection = "export.upload.targets." + InTargetName;
    bool inTargetSection = false;
    bool enabled = true;
    std::string line;
    while (std::getline(in, line)) {
        line = TrimExport(StripInlineTomlComment(std::move(line)));
        if (line.empty()) {
            continue;
        }
        if (line.front() == '[' && line.back() == ']') {
            inTargetSection = (line.substr(1, line.size() - 2) == targetSection);
            continue;
        }
        if (!inTargetSection) {
            continue;
        }

        const auto equalsPos = line.find('=');
        if (equalsPos == std::string::npos) {
            continue;
        }
        const std::string key = TrimExport(line.substr(0, equalsPos));
        const std::string value = StripConfigQuotes(line.substr(equalsPos + 1));
        const std::string dottedPrefix = targetSection + ".";
        if (key == "enabled") {
            enabled = ParseBoolConfigValue(value).value_or(enabled);
        } else if (inTargetSection) {
            ApplyUploadTargetConfigField(layer, key, value);
        } else if (key.rfind(dottedPrefix, 0) == 0) {
            const std::string fieldKey = key.substr(dottedPrefix.size());
            if (fieldKey == "enabled") {
                enabled = ParseBoolConfigValue(value).value_or(enabled);
            } else {
                ApplyUploadTargetConfigField(layer, fieldKey, value);
            }
        }
    }
    return enabled ? layer : ExportUploadConfigLayer{};
}

auto DirectBackendLayerFromTargetName(const std::string& InTargetName) -> ExportUploadConfigLayer {
    ExportUploadConfigLayer layer;
    if (IsDirectUploadBackendName(InTargetName)) {
        layer.target = InTargetName;
    }
    return layer;
}

auto ResolveUploadTargetNameForCli(const std::filesystem::path& InUserConfigPath,
                                   const std::filesystem::path& InRepoConfigPath,
                                   const std::string& InCliTargetName) -> std::string {
    const std::string cliTarget = TrimExport(InCliTargetName);
    if (!cliTarget.empty()) {
        return cliTarget;
    }
    const auto repoDefault = ReadUploadDefaultTargetFromFile(InRepoConfigPath);
    if (repoDefault.has_value()) {
        return *repoDefault;
    }
    const auto userDefault = ReadUploadDefaultTargetFromFile(InUserConfigPath);
    if (userDefault.has_value()) {
        return *userDefault;
    }
    return {};
}

auto ResolveUploadConfigForCli(const std::filesystem::path& InCwd,
                                const std::string& InTargetName,
                                const CLI::Option* InLayoutOpt,
                                const std::string& InLayout,
                                const CLI::Option* InCopyManifestOpt,
                                bool InCopyManifest,
                                const CLI::Option* InCopySha256Opt,
                                bool InCopySha256,
                                const CLI::Option* InNoReturnUrlOpt,
                                bool InNoReturnUrl,
                                const CLI::Option* InPublicLinkOpt,
                                bool InPublicLink,
                                const CLI::Option* InYesOpt,
                                bool InYes) -> ExportUploadConfig {
    ExportUploadConfigLayer userLayer;
    ExportUploadConfigLayer repoLayer;
    ExportUploadConfigLayer cliLayer;

    const auto userConfigPath = UserKogConfigPathForExportUpload();
    const auto repoConfigPath = RepoKogConfigPathForExportUpload(InCwd);
    const std::string selectedTarget = ResolveUploadTargetNameForCli(userConfigPath, repoConfigPath, InTargetName);

    if (!selectedTarget.empty()) {
        if (IsDirectUploadBackendName(selectedTarget)) {
            cliLayer = DirectBackendLayerFromTargetName(selectedTarget);
        } else {
            userLayer = LoadUploadTargetLayerFromFile(userConfigPath, selectedTarget);
            repoLayer = LoadUploadTargetLayerFromFile(repoConfigPath, selectedTarget);
            if (!UploadLayerHasAnyValue(userLayer) && !UploadLayerHasAnyValue(repoLayer)) {
                cliLayer.target = selectedTarget;
            }
        }
    }
    if (InPublicLinkOpt != nullptr && InPublicLinkOpt->count() > 0) {
        cliLayer.publicLink = InPublicLink;
    }
    if (InLayoutOpt != nullptr && InLayoutOpt->count() > 0) {
        cliLayer.layout = NormalizeUploadLayout(InLayout);
    }
    if (InCopyManifestOpt != nullptr && InCopyManifestOpt->count() > 0) {
        cliLayer.copyManifest = InCopyManifest;
    }
    if (InCopySha256Opt != nullptr && InCopySha256Opt->count() > 0) {
        cliLayer.copySha256 = InCopySha256;
    }
    if (InNoReturnUrlOpt != nullptr && InNoReturnUrlOpt->count() > 0) {
        cliLayer.returnUrl = !InNoReturnUrl;
    }
    if (InYesOpt != nullptr && InYesOpt->count() > 0) {
        cliLayer.yes = InYes;
    }
    return ResolveExportUploadConfig(userLayer, repoLayer, cliLayer);
}

} // anonymous namespace

auto ResolveExportUploadConfig(const ExportUploadConfigLayer& InUserConfig,
                               const ExportUploadConfigLayer& InRepoConfig,
                               const ExportUploadConfigLayer& InCliConfig) -> ExportUploadConfig {
    ExportUploadConfig out;

    auto applyLayer = [&](const ExportUploadConfigLayer& InLayer) {
        if (InLayer.target.has_value()) {
            out.target = *InLayer.target;
        }
        if (InLayer.localSyncFolder.has_value()) {
            out.localSyncFolder = *InLayer.localSyncFolder;
        }
        if (InLayer.rcloneRemote.has_value()) {
            out.rcloneRemote = *InLayer.rcloneRemote;
        }
        if (InLayer.rcloneDestination.has_value()) {
            out.rcloneDestination = *InLayer.rcloneDestination;
        }
        if (InLayer.layout.has_value()) {
            out.layout = NormalizeUploadLayout(*InLayer.layout);
        }
        if (InLayer.copyManifest.has_value()) {
            out.copyManifest = *InLayer.copyManifest;
        }
        if (InLayer.copySha256.has_value()) {
            out.copySha256 = *InLayer.copySha256;
        }
        if (InLayer.returnUrl.has_value()) {
            out.returnUrl = *InLayer.returnUrl;
        }
        if (InLayer.linkMode.has_value()) {
            out.linkMode = ToLowerExportUpload(TrimExport(*InLayer.linkMode));
        }
        if (InLayer.backend.has_value()) {
            out.backend = *InLayer.backend;
        }
        if (InLayer.connectorHint.has_value()) {
            out.connectorHint = *InLayer.connectorHint;
        }
    };

    applyLayer(InUserConfig);
    applyLayer(InRepoConfig);
    applyLayer(InCliConfig);

    out.publicLink = InCliConfig.publicLink.value_or(false);
    out.yes = InCliConfig.yes.value_or(false);
    if (out.publicLink) {
        out.linkMode = "public-link";
    } else if (out.linkMode == "public-link") {
        out.linkMode = "private";
    } else if (out.linkMode.empty()) {
        out.linkMode = "private";
    }

    return out;
}

auto DoctorExportUploadWithExecutor(const ExportUploadConfig& InConfig,
                                    const ShellExecutor& InExec) -> ExportUploadDoctorResult {
    ExportUploadDoctorResult result;
    result.status = "NOT_CONFIGURED";
    result.backendLabel = "export upload";
    result.guidance = "Configure export.upload.targets.<name> before running kog export upload.";

    if (InConfig.target.empty()) {
        return result;
    }

    if (InConfig.target == "gdrive-api") {
        result.status = "FUTURE_BACKEND";
        result.backendLabel = "Google Drive API (future backend)";
        result.guidance = "Google Drive API upload is FUTURE_BACKEND guidance only in this release; doctor does not start OAuth, store tokens, or configure Google credentials. Use a local Google Drive sync folder or an existing rclone remote instead.";
        return result;
    }

    if (InConfig.target == "local-sync-folder") {
        result.backendLabel = "local-sync-folder";
        std::error_code ec;
        if (InConfig.localSyncFolder.empty() ||
            !std::filesystem::exists(InConfig.localSyncFolder, ec) ||
            !std::filesystem::is_directory(InConfig.localSyncFolder, ec)) {
            result.status = "MISSING_PATH";
            result.guidance = "Configure local-sync-folder as an existing sync root before running upload; kog creates only the safe relative layout below that root and cloud sync is handled externally.";
            return result;
        }
        if (!IsSafeRelativeUploadLayout(NormalizeUploadLayout(InConfig.layout))) {
            result.status = "INVALID_CONFIG";
            result.guidance = "local-sync-folder layout must be a safe relative path without '..', drive names, or control characters.";
            return result;
        }
        result.ok = true;
        result.status = "OK";
        result.guidance = "local-sync-folder root is present; upload copies into the configured layout and external Google Drive/Desktop sync handles cloud propagation.";
        const auto targetPath = ResolveLocalUploadTargetPath(InConfig);
        result.output = "local-sync-folder OK\nSync root: " + InConfig.localSyncFolder.generic_string() +
            "\nLocal target path: " + targetPath.generic_string() +
            "\ncopy_manifest: " + (InConfig.copyManifest ? std::string("true") : std::string("false")) +
            "\ncopy_sha256: " + (InConfig.copySha256 ? std::string("true") : std::string("false"));
        return result;
    }

    if (InConfig.target == "rclone") {
        result.backendLabel = "rclone third-party backend";
        result.thirdParty = true;
        std::string configError;
        if (!ValidateRcloneUploadConfig(InConfig, configError)) {
            result.status = "INVALID_CONFIG";
            result.guidance = configError;
            return result;
        }

        const auto version = InExec("rclone", {"version"}, shell::ExecMode::Capture, std::nullopt);
        if (version.exitCode != 0) {
            result.status = "RCLONE_NOT_FOUND";
            result.guidance = "install rclone and ensure it is on PATH before using the rclone export upload backend.";
            result.output = RedactExportUploadText(version.stdoutStr + version.stderrStr);
            return result;
        }

        const std::string remote = NormalizeRcloneRemote(InConfig.rcloneRemote);
        const auto remotes = InExec("rclone", {"listremotes"}, shell::ExecMode::Capture, std::nullopt);
        if (remotes.exitCode != 0 || remote.empty() || remotes.stdoutStr.find(remote + ":") == std::string::npos) {
            result.status = "RCLONE_REMOTE_MISSING";
            result.guidance = "Run rclone config to create the configured remote: " + remote;
            result.output = RedactExportUploadText(remotes.stdoutStr + remotes.stderrStr);
            return result;
        }

        std::string remoteType;
        const auto remoteConfig = InExec("rclone", {"config", "show", remote + ":"}, shell::ExecMode::Capture, std::nullopt);
        if (remoteConfig.exitCode == 0) {
            remoteType = ExtractRcloneRemoteType(remoteConfig.stdoutStr);
        }

        result.ok = true;
        result.status = "OK";
        result.guidance = "rclone is installed and the configured remote exists. Uploads remain private/default; private Google Drive URLs are derived only when lsjson exposes a Drive file ID. Use --public-link --yes only when you explicitly accept permission mutation.";
        result.output = "rclone version probe:\n" + RedactExportUploadText(version.stdoutStr + version.stderrStr) +
            "Configured remote: " + remote + ":\n" +
            "Remote type: " + (remoteType.empty() ? std::string("unknown") : remoteType) + "\n" +
            "Destination: " + TrimSlashes(InConfig.rcloneDestination) + "\n" +
            "Layout: " + NormalizeUploadLayout(InConfig.layout) + "\n" +
            "URL support: private Drive URL only when rclone lsjson --stat -M returns an ID; otherwise URL_UNAVAILABLE.";
        return result;
    }

    result.status = "NOT_CONFIGURED";
    result.guidance = "Unsupported export upload target: " + InConfig.target;
    return result;
}

auto UploadExportArtifactsWithExecutor(const ExportUploadRequest& InRequest,
                                       const ShellExecutor& InExec) -> ExportUploadResult {
    ExportUploadResult result;
    result.visibility = "unknown-private";
    result.permissionChanged = false;

    std::error_code ec;
    if (InRequest.archivePath.empty() ||
        !std::filesystem::exists(InRequest.archivePath, ec) ||
        !std::filesystem::is_regular_file(InRequest.archivePath, ec)) {
        result.errorMessage = "source archive does not exist: " + InRequest.archivePath.generic_string();
        return result;
    }

    const std::string archiveName = !InRequest.archiveName.empty()
        ? InRequest.archiveName
        : InRequest.archivePath.filename().generic_string();
    const std::string sourceSha = ComputeFileSha256(InRequest.archivePath);
    if (sourceSha.empty()) {
        result.errorMessage = "failed to compute source archive sha256";
        return result;
    }
    result.sourceArchive = InRequest.archivePath.generic_string();
    result.sourceSha256 = sourceSha;
    result.remoteArchiveName = archiveName;
    result.backend = !InRequest.config.backend.empty() ? InRequest.config.backend : InRequest.config.target;
    result.connectorHint = InRequest.config.connectorHint;

    if (InRequest.config.target == "local-sync-folder") {
        result.visibility = "private";
        result.syncFolderPath = InRequest.config.localSyncFolder;
        const auto syncRoot = InRequest.config.localSyncFolder;
        if (syncRoot.empty()) {
            result.errorMessage = "local-sync-folder target is not configured";
            return result;
        }
        if (!IsSafeRelativeUploadLayout(NormalizeUploadLayout(InRequest.config.layout))) {
            result.errorMessage = "local-sync-folder layout must be a safe relative path without '..', drive names, or control characters";
            return result;
        }
        if (!std::filesystem::exists(syncRoot, ec) || !std::filesystem::is_directory(syncRoot, ec)) {
            result.errorMessage = "local-sync-folder sync root path is missing: " + syncRoot.generic_string();
            return result;
        }
        const auto target = ResolveLocalUploadTargetPath(InRequest.config);
        result.localTargetPath = target;
        std::filesystem::create_directories(target, ec);
        if (ec) {
            result.errorMessage = "failed to create local-sync-folder layout directory: " + ec.message();
            return result;
        }
        if (!std::filesystem::is_directory(target, ec)) {
            result.errorMessage = "local-sync-folder target is not a directory: " + target.generic_string();
            return result;
        }

        result.copiedArchivePath = target / archiveName;
        result.uploadManifestPath = target / (archiveName + ".upload-manifest.json");
        if (InRequest.config.copyManifest && !InRequest.manifestPath.empty()) {
            result.copiedManifestPath = target / InRequest.manifestPath.filename();
        }
        if (InRequest.config.copySha256) {
            result.sha256SidecarPath = target / ComputeChecksumFilename(archiveName);
        }

        std::string copyError;
        if (!CopyFileNoOverwrite(InRequest.archivePath, result.copiedArchivePath, copyError)) {
            result.errorMessage = copyError;
            return result;
        }
        if (InRequest.config.copyManifest && !InRequest.manifestPath.empty() && std::filesystem::exists(InRequest.manifestPath, ec)) {
            if (!CopyFileNoOverwrite(InRequest.manifestPath, result.copiedManifestPath, copyError)) {
                result.errorMessage = copyError;
                return result;
            }
        }

        if (InRequest.config.copySha256) {
            const std::string sidecarSha = WriteUploadSha256Sidecar(InRequest.archivePath, result.sha256SidecarPath);
            if (sidecarSha.empty()) {
                result.errorMessage = "failed to write archive sha256 sidecar";
                return result;
            }
        }
        if (!VerifyCopiedFile(InRequest.archivePath, result.copiedArchivePath, sourceSha, result.errorMessage)) {
            return result;
        }

        result.urlStatus = InRequest.config.returnUrl ? "URL_UNAVAILABLE" : "";
        if (!WriteUploadManifestJson(InRequest, result, result.uploadManifestPath, sourceSha)) {
            result.errorMessage = "failed to write upload manifest";
            return result;
        }
        result.success = true;
        result.output = "Copied export archive to local-sync-folder layout. Cloud sync is handled externally and is not claimed complete by kog.";
        return result;
    }

    if (InRequest.config.target == "rclone") {
        std::string configError;
        if (!ValidateRcloneUploadConfig(InRequest.config, configError)) {
            result.errorMessage = configError;
            return result;
        }

        result.visibility = "private";
        result.remotePath = BuildRcloneObjectPath(InRequest.config, archiveName);
        result.uploadManifestPath = InRequest.archivePath.parent_path() / (archiveName + ".upload-manifest.json");
        if (InRequest.config.copySha256) {
            result.sha256SidecarPath = InRequest.archivePath.parent_path() / ComputeChecksumFilename(archiveName);
            const std::string sidecarSha = WriteUploadSha256Sidecar(InRequest.archivePath, result.sha256SidecarPath);
            if (sidecarSha.empty()) {
                result.errorMessage = "failed to write archive sha256 sidecar";
                return result;
            }
        }

        const auto copyArchive = InExec(
            "rclone",
            {"copyto", InRequest.archivePath.generic_string(), result.remotePath},
            shell::ExecMode::Capture,
            std::nullopt);
        if (copyArchive.exitCode != 0) {
            result.errorMessage = "rclone copyto failed: " + RedactExportUploadText(copyArchive.stderrStr);
            return result;
        }

        if (InRequest.config.copySha256) {
            const auto sidecarRemotePath = BuildRcloneObjectPath(InRequest.config, ComputeChecksumFilename(archiveName));
            const auto copySidecar = InExec(
                "rclone",
                {"copyto", result.sha256SidecarPath.generic_string(), sidecarRemotePath},
                shell::ExecMode::Capture,
                std::nullopt);
            if (copySidecar.exitCode != 0) {
                result.errorMessage = "rclone sha256 sidecar copy failed: " + RedactExportUploadText(copySidecar.stderrStr);
                return result;
            }
        }

        if (InRequest.config.copyManifest && !InRequest.manifestPath.empty() && std::filesystem::exists(InRequest.manifestPath, ec)) {
            result.copiedManifestPath = InRequest.manifestPath;
            const auto manifestRemotePath = BuildRcloneObjectPath(InRequest.config, InRequest.manifestPath.filename().generic_string());
            const auto copyOriginalManifest = InExec(
                "rclone",
                {"copyto", InRequest.manifestPath.generic_string(), manifestRemotePath},
                shell::ExecMode::Capture,
                std::nullopt);
            if (copyOriginalManifest.exitCode != 0) {
                result.errorMessage = "rclone export manifest copy failed: " + RedactExportUploadText(copyOriginalManifest.stderrStr);
                return result;
            }
        }

        if (InRequest.config.returnUrl) {
            const auto metadata = InExec(
                "rclone",
                {"lsjson", "--stat", "-M", result.remotePath},
                shell::ExecMode::Capture,
                std::nullopt);
            if (metadata.exitCode != 0) {
                result.errorMessage = "rclone lsjson verification failed: " + RedactExportUploadText(metadata.stderrStr);
                return result;
            }

            result.fileId = ExtractDriveIdFromLsJson(metadata.stdoutStr);
            if (result.fileId.has_value()) {
                result.webUrl = "https://drive.google.com/file/d/" + *result.fileId + "/view";
            } else {
                result.urlStatus = "URL_UNAVAILABLE";
            }
        }

        if (InRequest.config.publicLink && InRequest.config.yes) {
            result.output += "Warning: public link requested; invoking rclone link may mutate sharing permissions.\n";
            const auto link = InExec("rclone", {"link", result.remotePath}, shell::ExecMode::Capture, std::nullopt);
            const auto linkUrl = TrimExport(link.stdoutStr);
            if (link.exitCode != 0) {
                result.errorMessage = "rclone public link failed: " + RedactExportUploadText(link.stderrStr);
                return result;
            }
            if (linkUrl.empty()) {
                result.errorMessage = "rclone public link did not return a URL";
                return result;
            }
            result.visibility = "public-link";
            result.permissionChanged = true;
            result.webUrl = linkUrl;
            result.urlStatus.clear();
        } else if (InRequest.config.publicLink && !InRequest.config.yes) {
            result.output += "Warning: --public-link was requested without --yes; keeping upload private and not calling rclone link.\n";
        }

        if (!WriteUploadManifestJson(InRequest, result, result.uploadManifestPath, sourceSha)) {
            result.errorMessage = "failed to write upload manifest";
            return result;
        }
        const auto manifestRemotePath = BuildRcloneObjectPath(InRequest.config, archiveName + ".upload-manifest.json");
        const auto copyUploadManifest = InExec(
            "rclone",
            {"copyto", result.uploadManifestPath.generic_string(), manifestRemotePath},
            shell::ExecMode::Capture,
            std::nullopt);
        if (copyUploadManifest.exitCode != 0) {
            result.errorMessage = "rclone upload manifest copy failed: " + RedactExportUploadText(copyUploadManifest.stderrStr);
            return result;
        }

        result.success = true;
        if (result.output.empty()) {
            result.output = "Uploaded export archive with rclone copyto.";
        }
        result.output = RedactExportUploadText(result.output);
        return result;
    }

    if (InRequest.config.target == "gdrive-api") {
        result.errorMessage = "Google Drive API backend is not configured; OAuth is intentionally not implemented.";
        return result;
    }

    result.errorMessage = "export upload target is not configured";
    return result;
}

auto RedactExportUploadText(const std::string& InText) -> std::string {
    std::string out = InText;
    out = std::regex_replace(out, std::regex(R"((https?://)[^\s/@:]+:[^\s/@]+@)", std::regex_constants::icase), "$1<redacted>@");
    out = std::regex_replace(
        out,
        std::regex(R"(((?:--[^\s]*(?:token|password|passwd|secret|api[-_]?key|authorization|bearer)[^\s]*)\s+)([^\s]+))", std::regex_constants::icase),
        "$1<redacted>");
    out = std::regex_replace(
        out,
        std::regex(R"(((?:[A-Za-z0-9_.-]*(?:token|password|passwd|secret|api[-_]?key|authorization|bearer)[A-Za-z0-9_.-]*)\s*=\s*)([^\s]+))", std::regex_constants::icase),
        "$1<redacted>");
    out = std::regex_replace(out, std::regex(R"((Bearer\s+)([^\s]+))", std::regex_constants::icase), "$1<redacted>");
    return out;
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
                "import io,os,pathlib,sys,tarfile,zipfile; "
                "mf=pathlib.Path(sys.argv[1]); out=pathlib.Path(sys.argv[2]); fmt=sys.argv[3]; "
                "out.parent.mkdir(parents=True,exist_ok=True); "
                "entries=[]\n"
                "def maybe_norm(data,norm):\n"
                "  if not norm: return data\n"
                "  if b'\\0' in data: return data\n"
                "  return data.replace(b'\\r\\n',b'\\n').replace(b'\\r',b'\\n')\n"
                "def apply_mode(info,mode):\n"
                "  perm=(mode & 0o777) or (info.mode & 0o777) or 0o777\n"
                "  info.mode=perm\n"
                "def add_tar_entry(t,mode,sp,ap,norm):\n"
                "  info=t.gettarinfo(str(sp),arcname=ap)\n"
                "  if info is None: return\n"
                "  apply_mode(info,mode)\n"
                "  if info.issym():\n"
                "    info.type=tarfile.SYMTYPE; info.linkname=os.readlink(sp); info.size=0; t.addfile(info); return\n"
                "  if info.islnk():\n"
                "    info.size=0; t.addfile(info); return\n"
                "  if info.isdir():\n"
                "    info.size=0; t.addfile(info); return\n"
                "  if not info.isfile(): return\n"
                "  data=maybe_norm(sp.read_bytes(),norm)\n"
                "  info.size=len(data)\n"
                "  t.addfile(info,io.BytesIO(data))\n"
                "def add_zip_entry(z,mode,sp,ap,norm):\n"
                "  if sp.is_symlink():\n"
                "    zi=zipfile.ZipInfo(ap); zi.create_system=3; perm=(mode & 0o777) or 0o777; zi.external_attr=((0o120000 | perm) & 0xffff) << 16; z.writestr(zi, os.readlink(sp)); return\n"
                "  if not sp.is_file(): return\n"
                "  data=maybe_norm(sp.read_bytes(),norm)\n"
                "  zi=zipfile.ZipInfo(ap); zi.external_attr=(mode & 0xffff) << 16\n"
                "  z.writestr(zi, data)\n"
                "for line in mf.read_text(encoding='utf-8').splitlines():\n"
                "  if not line.strip(): continue\n"
                "  parts=line.split('\\t');\n"
                "  if len(parts) < 3: continue\n"
                "  m,s,a=parts[0],parts[1],parts[2]; norm=(len(parts) > 3 and parts[3] == '1'); mode=int(m); sp=pathlib.Path(s); ap=a.replace('\\\\','/');\n"
                "  if not sp.exists() and not sp.is_symlink(): continue\n"
                "  entries.append((mode,sp,ap,norm))\n"
                "if fmt == 'tar':\n"
                "  with tarfile.open(out,'w') as t:\n"
                "    for mode,sp,ap,norm in entries:\n"
                "      add_tar_entry(t,mode,sp,ap,norm)\n"
                "elif fmt == 'zip':\n"
                "  with zipfile.ZipFile(out,'w',compression=zipfile.ZIP_DEFLATED) as z:\n"
                "    for mode,sp,ap,norm in entries:\n"
                "      add_zip_entry(z,mode,sp,ap,norm)\n"
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
            "import io,os,pathlib,sys,tarfile,zipfile; "
            "mf=pathlib.Path(sys.argv[1]); out=pathlib.Path(sys.argv[2]); fmt=sys.argv[3]; "
            "out.parent.mkdir(parents=True,exist_ok=True); "
            "entries=[]\n"
            "def maybe_norm(data,norm):\n"
            "  if not norm: return data\n"
            "  if b'\\0' in data: return data\n"
            "  return data.replace(b'\\r\\n',b'\\n').replace(b'\\r',b'\\n')\n"
            "def apply_mode(info,mode):\n"
            "  perm=(mode & 0o777) or (info.mode & 0o777) or 0o777\n"
            "  info.mode=perm\n"
            "def add_tar_entry(t,mode,sp,ap,norm):\n"
            "  info=t.gettarinfo(str(sp),arcname=ap)\n"
            "  if info is None: return\n"
            "  apply_mode(info,mode)\n"
            "  if info.issym():\n"
            "    info.type=tarfile.SYMTYPE; info.linkname=os.readlink(sp); info.size=0; t.addfile(info); return\n"
            "  if info.islnk():\n"
            "    info.size=0; t.addfile(info); return\n"
            "  if info.isdir():\n"
            "    info.size=0; t.addfile(info); return\n"
            "  if not info.isfile(): return\n"
            "  data=maybe_norm(sp.read_bytes(),norm)\n"
            "  info.size=len(data)\n"
            "  t.addfile(info,io.BytesIO(data))\n"
            "def add_zip_entry(z,mode,sp,ap,norm):\n"
            "  if sp.is_symlink():\n"
            "    zi=zipfile.ZipInfo(ap); zi.create_system=3; perm=(mode & 0o777) or 0o777; zi.external_attr=((0o120000 | perm) & 0xffff) << 16; z.writestr(zi, os.readlink(sp)); return\n"
            "  if not sp.is_file(): return\n"
            "  data=maybe_norm(sp.read_bytes(),norm)\n"
            "  zi=zipfile.ZipInfo(ap); zi.external_attr=(mode & 0xffff) << 16\n"
            "  z.writestr(zi, data)\n"
            "for line in mf.read_text(encoding='utf-8').splitlines():\n"
            "  if not line.strip(): continue\n"
            "  parts=line.split('\\t');\n"
            "  if len(parts) < 3: continue\n"
            "  m,s,a=parts[0],parts[1],parts[2]; norm=(len(parts) > 3 and parts[3] == '1'); mode=int(m); sp=pathlib.Path(s); ap=a.replace('\\\\','/');\n"
            "  if not sp.exists() and not sp.is_symlink(): continue\n"
            "  entries.append((mode,sp,ap,norm))\n"
            "if fmt == 'tar':\n"
            "  with tarfile.open(out,'w') as t:\n"
            "    for mode,sp,ap,norm in entries:\n"
            "      add_tar_entry(t,mode,sp,ap,norm)\n"
            "elif fmt == 'zip':\n"
            "  with zipfile.ZipFile(out,'w',compression=zipfile.ZIP_DEFLATED) as z:\n"
            "    for mode,sp,ap,norm in entries:\n"
            "      add_zip_entry(z,mode,sp,ap,norm)\n"
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
    auto* splitSubrepoDepth = new int{0};
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

    auto* upload = cmd->add_subcommand("upload",
        "Upload or copy an existing kog export archive to a configured target");
    auto* uploadLast = new bool{false};
    auto* uploadArchive = new std::string{};
    auto* uploadTarget = new std::string{};
    auto* uploadLayout = new std::string{};
    auto* uploadCopyManifest = new bool{false};
    auto* uploadCopySha256 = new bool{false};
    auto* uploadNoReturnUrl = new bool{false};
    auto* uploadPublicLink = new bool{false};
    auto* uploadYes = new bool{false};
    auto* uploadLastOpt = upload->add_flag("--last", *uploadLast,
        "Upload the newest canonical .kano/tmp/*.export-manifest.json archive");
    auto* uploadArchiveOpt = upload->add_option("--archive", *uploadArchive,
        "Path to an existing export archive to upload");
    upload->add_option("--target", *uploadTarget,
        "Upload target name or backend: local-sync-folder|rclone|gdrive-api");
    auto* uploadLayoutOpt = upload->add_option("--layout", *uploadLayout,
        "Safe relative layout under the sync root or rclone destination (for example ChatGPT_Export/kog)");
    auto* uploadCopyManifestOpt = upload->add_flag("--copy-manifest", *uploadCopyManifest,
        "Copy the original export manifest alongside the uploaded archive");
    auto* uploadCopySha256Opt = upload->add_flag("--copy-sha256", *uploadCopySha256,
        "Write/copy an archive .sha256 sidecar alongside the uploaded archive");
    auto* uploadNoReturnUrlOpt = upload->add_flag("--no-return-url", *uploadNoReturnUrl,
        "Do not attempt to resolve a cloud URL; upload manifest records no URL state");
    auto* uploadPublicLinkOpt = upload->add_flag("--public-link", *uploadPublicLink,
        "Explicitly request a public link when supported; requires --yes");
    auto* uploadYesOpt = upload->add_flag("--yes", *uploadYes,
        "Confirm permission-mutating upload actions such as public-link creation");

    auto* uploadDoctor = upload->add_subcommand("doctor",
        "Diagnose export upload target setup without uploading or mutating permissions");
    auto* doctorTarget = new std::string{};
    auto* doctorLayout = new std::string{};
    auto* doctorCopyManifest = new bool{false};
    auto* doctorCopySha256 = new bool{false};
    auto* doctorNoReturnUrl = new bool{false};
    auto* doctorPublicLink = new bool{false};
    auto* doctorYes = new bool{false};
    uploadDoctor->add_option("--target", *doctorTarget,
        "Upload target name or backend: local-sync-folder|rclone|gdrive-api");
    auto* doctorLayoutOpt = uploadDoctor->add_option("--layout", *doctorLayout,
        "Preview a safe relative upload layout under the configured target");
    auto* doctorCopyManifestOpt = uploadDoctor->add_flag("--copy-manifest", *doctorCopyManifest,
        "Preview original export manifest copy behavior");
    auto* doctorCopySha256Opt = uploadDoctor->add_flag("--copy-sha256", *doctorCopySha256,
        "Preview sha256 sidecar copy behavior");
    auto* doctorNoReturnUrlOpt = uploadDoctor->add_flag("--no-return-url", *doctorNoReturnUrl,
        "Preview upload behavior without URL lookup");
    auto* doctorPublicLinkOpt = uploadDoctor->add_flag("--public-link", *doctorPublicLink,
        "Include public-link opt-in in the effective config preview");
    auto* doctorYesOpt = uploadDoctor->add_flag("--yes", *doctorYes,
        "Include yes confirmation in the effective config preview");

    ShellExecutor realUploadExec = [](const std::string& cmdName,
                                      const std::vector<std::string>& args,
                                      shell::ExecMode mode,
                                      std::optional<std::filesystem::path> workDir) {
        return shell::ExecuteCommand(cmdName, args, mode, workDir);
    };

    uploadDoctor->callback([=]() {
        const auto cwd = std::filesystem::current_path().lexically_normal();
        const auto config = ResolveUploadConfigForCli(
            cwd,
            *doctorTarget,
            doctorLayoutOpt,
            *doctorLayout,
            doctorCopyManifestOpt,
            *doctorCopyManifest,
            doctorCopySha256Opt,
            *doctorCopySha256,
            doctorNoReturnUrlOpt,
            *doctorNoReturnUrl,
            doctorPublicLinkOpt,
            *doctorPublicLink,
            doctorYesOpt,
            *doctorYes);
        const auto lastManifest = FindNewestExportManifest(cwd);
        std::cout << "Effective upload target: " << (config.target.empty() ? std::string("<not configured>") : config.target) << "\n";
        if (!lastManifest.empty()) {
            std::cout << "Last export manifest: " << lastManifest.generic_string() << "\n";
        } else {
            std::cout << "Last export manifest: <none under .kano/tmp>\n";
        }
        const auto result = DoctorExportUploadWithExecutor(config, realUploadExec);
        std::cout << "export upload doctor: " << result.status << "\n";
        if (!result.backendLabel.empty()) {
            std::cout << "Backend: " << result.backendLabel << "\n";
        }
        if (!result.guidance.empty()) {
            std::cout << result.guidance << "\n";
        }
        if (!result.output.empty()) {
            std::cout << RedactExportUploadText(result.output) << "\n";
        }
        if (!result.ok) {
            throw CLI::RuntimeError(1);
        }
    });

    upload->callback([=]() {
        if (uploadDoctor->parsed()) {
            return;
        }

        const auto cwd = std::filesystem::current_path().lexically_normal();
        const auto config = ResolveUploadConfigForCli(
            cwd,
            *uploadTarget,
            uploadLayoutOpt,
            *uploadLayout,
            uploadCopyManifestOpt,
            *uploadCopyManifest,
            uploadCopySha256Opt,
            *uploadCopySha256,
            uploadNoReturnUrlOpt,
            *uploadNoReturnUrl,
            uploadPublicLinkOpt,
            *uploadPublicLink,
            uploadYesOpt,
            *uploadYes);

        std::filesystem::path manifestPath;
        std::filesystem::path archivePath;
        if (uploadLastOpt->count() > 0 && uploadArchiveOpt->count() > 0) {
            std::cerr << "kog export upload: --last cannot be combined with --archive\n";
            throw CLI::RuntimeError(1);
        }
        if (uploadLastOpt->count() > 0) {
            manifestPath = FindNewestExportManifest(cwd);
            if (manifestPath.empty()) {
                std::cerr << "kog export upload: no .kano/tmp/*.export-manifest.json file found for --last\n";
                throw CLI::RuntimeError(1);
            }
            archivePath = ResolveArchiveFromExportManifest(manifestPath, cwd);
            if (!archivePath.empty() && !IsPathWithinDirectory(archivePath, cwd / ".kano" / "tmp")) {
                std::cerr << "kog export upload: --last archive path must stay under .kano/tmp\n";
                throw CLI::RuntimeError(1);
            }
        } else if (uploadArchiveOpt->count() > 0) {
            archivePath = std::filesystem::path(*uploadArchive);
            if (archivePath.is_relative()) {
                archivePath = (cwd / archivePath).lexically_normal();
            }
            manifestPath = GuessManifestForArchive(archivePath, cwd);
        } else {
            std::cerr << "kog export upload: provide --last or --archive <path>\n";
            throw CLI::RuntimeError(1);
        }

        std::error_code ec;
        if (archivePath.empty() || !std::filesystem::exists(archivePath, ec) || !std::filesystem::is_regular_file(archivePath, ec)) {
            std::cerr << "kog export upload: archive not found: " << archivePath.generic_string() << "\n";
            throw CLI::RuntimeError(1);
        }

        ExportUploadRequest request;
        request.archivePath = archivePath;
        request.manifestPath = manifestPath;
        request.archiveName = archivePath.filename().generic_string();
        request.config = config;

        const auto result = UploadExportArtifactsWithExecutor(request, realUploadExec);
        if (!result.output.empty()) {
            std::cout << RedactExportUploadText(result.output) << "\n";
        }
        if (result.success) {
            std::cout << "Upload manifest: " << result.uploadManifestPath.generic_string() << "\n";
            if (result.webUrl.has_value()) {
                std::cout << "URL: " << *result.webUrl << "\n";
            } else if (!result.urlStatus.empty()) {
                std::cout << "URL status: " << result.urlStatus << "\n";
            }
            return;
        }

        std::cerr << "kog export upload: " << RedactExportUploadText(result.errorMessage) << "\n";
        throw CLI::RuntimeError(1);
    });

    cmd->callback([=]() {
        if (upload->parsed()) {
            return;
        }
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

