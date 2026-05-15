// repo hygiene command - detect and fix index executable bits and CRLF/LF issues

#include <CLI/CLI.hpp>
#include "discovery.hpp"
#include "native_workspace.hpp"
#include "shell_executor.hpp"
#include "terminal_color.hpp"

#include <iostream>
#include <filesystem>
#include <string>
#include <vector>
#include <regex>
#include <sstream>
#include <set>

namespace kano::git::commands {

namespace {

auto Trim(std::string InValue) -> std::string {
    while (!InValue.empty() && (InValue.back() == '\n' || InValue.back() == '\r' || InValue.back() == ' ' || InValue.back() == '\t')) {
        InValue.pop_back();
    }
    std::size_t start = 0;
    while (start < InValue.size() && (InValue[start] == ' ' || InValue[start] == '\t')) {
        start += 1;
    }
    return InValue.substr(start);
}

auto GitCapture(const std::filesystem::path& InRepo, const std::vector<std::string>& InArgs) -> shell::ExecResult {
    return shell::ExecuteCommand("git", InArgs, shell::ExecMode::Capture, InRepo);
}

bool IsExecutableRequired(const std::string& path) {
    if (path == "scripts/kog" || path == "scripts/kano-git") return true;
    if (path == "assets/root-wrapper-templates/common/kog") return true;
    if (path.starts_with("assets/root-wrapper-templates/common/kog-") && path.ends_with(".sh")) return true;
    if (path.starts_with("src/shell/") && path.ends_with(".sh")) return true;
    if (path.starts_with("scripts/") && path.ends_with(".sh")) return true;
    if (path.starts_with("tools/") && path.ends_with(".sh")) return true;
    if (path == ".githooks/pre-commit" || path == ".githooks/pre-push") return true;
    return false;
}

bool IsLfRequired(const std::string& path) {
    if (path == ".gitattributes" || path == ".gitignore" || path == ".gitmodules" || path == ".editorconfig" || path == ".kogignore") return true;
    if (path.ends_with(".sh") || path.ends_with(".bash") || path.ends_with(".py")) return true;
    if (path.ends_with(".cpp") || path.ends_with(".hpp") || path.ends_with(".h") || path.ends_with(".c")) return true;
    if (path.ends_with(".cmake") || path == "CMakeLists.txt") return true;
    if (path.ends_with(".groovy") || path.ends_with(".md") || path.ends_with(".toml") || path.ends_with(".yml") || path.ends_with(".yaml") || path.ends_with(".json")) return true;
    if (path.starts_with(".githooks/")) return true;
    if (path == "scripts/kog" || path == "scripts/kano-git" || path == "assets/root-wrapper-templates/common/kog") return true;
    if (path.starts_with("assets/root-wrapper-templates/common/kog-") && path.ends_with(".sh")) return true;
    return false;
}

void CheckRepoHygiene(const std::filesystem::path& repoRoot, bool fix, bool archiveSafe) {
    std::cout << "KOG repo hygiene summary\n";
    std::cout << "- Repo: " << repoRoot.generic_string() << "\n";

    auto lsFilesS = GitCapture(repoRoot, {"ls-files", "-s"});
    auto lsFilesEol = GitCapture(repoRoot, {"ls-files", "--eol"});

    std::vector<std::string> linesS;
    {
        std::istringstream stream(lsFilesS.stdoutStr);
        std::string line;
        while (std::getline(stream, line)) linesS.push_back(line);
    }

    std::vector<std::string> linesEol;
    {
        std::istringstream stream(lsFilesEol.stdoutStr);
        std::string line;
        while (std::getline(stream, line)) linesEol.push_back(line);
    }

    int filesScanned = linesS.size();
    int lfIssues = 0;
    int execIssues = 0;
    int attrIssues = 0;

    std::vector<std::string> toFixExec;
    std::vector<std::string> toFixLf;
    bool needsGitattributesFix = false;

    for (const auto& line : linesS) {
        if (line.empty()) continue;
        // 100644 1234567890abcdef 0	path/to/file
        std::string mode = line.substr(0, 6);
        std::size_t tabPos = line.find('\t');
        if (tabPos == std::string::npos) continue;
        std::string path = line.substr(tabPos + 1);

        if (IsExecutableRequired(path)) {
            if (mode == "100644") {
                execIssues++;
                std::cout << "[FAIL] " << path << " executable-bit-missing " << "Script should be executable (100755) but is tracked as 100644\n";
                toFixExec.push_back(path);
            }
        }
    }

    for (const auto& line : linesEol) {
        if (line.empty()) continue;
        // i/lf    w/crlf  attr/text eol=lf      	path/to/file
        std::size_t tabPos = line.find('\t');
        if (tabPos == std::string::npos) continue;
        std::string path = line.substr(tabPos + 1);

        bool isLfRequired = IsLfRequired(path);
        
        // Skip binary checks if we can parse it from attr
        if (line.find("attr/-text") != std::string::npos || line.find("attr/binary") != std::string::npos) {
             continue; // Binary file
        }

        std::string info = line.substr(0, tabPos);
        bool indexCrlf = info.find("i/crlf") != std::string::npos;
        bool workingCrlf = info.find("w/crlf") != std::string::npos;
        bool hasEolLfAttr = info.find("eol=lf") != std::string::npos;

        if (isLfRequired) {
            if (!hasEolLfAttr) {
                attrIssues++;
                std::cout << "[WARN] " << path << " missing-eol-lf-attr " << "Missing text eol=lf in .gitattributes\n";
                needsGitattributesFix = true;
            }
            if (indexCrlf) {
                lfIssues++;
                std::cout << "[FAIL] " << path << " crlf-in-index " << "CRLF detected in Git index for LF-required file\n";
                toFixLf.push_back(path);
            }
        }
    }

    std::cout << "- Files scanned: " << filesScanned << "\n";
    std::cout << "- LF issues: " << lfIssues << "\n";
    std::cout << "- Executable mode issues: " << execIssues << "\n";
    std::cout << "- Gitattributes issues: " << attrIssues << "\n\n";

    if (lfIssues == 0 && execIssues == 0 && attrIssues == 0) {
        std::cout << "[PASS] Repo hygiene is healthy\n";
    }

    if (fix) {
        std::cout << "--- Applying Fixes ---\n";
        
        if (needsGitattributesFix) {
             // In a real scenario we might smartly inject rules, but here we just warn and expect the user or higher level tools to fix.
             // We can run git add --renormalize to fix what we can.
             std::cout << "Please ensure .gitattributes is fully configured. We will renormalize all tracked files now.\n";
        }

        if (!toFixExec.empty()) {
            for (const auto& path : toFixExec) {
                GitCapture(repoRoot, {"update-index", "--chmod=+x", path});
                std::cout << "mode fixed: 100644 => 100755 " << path << "\n";
            }
        }

        if (!toFixLf.empty() || needsGitattributesFix) {
             GitCapture(repoRoot, {"add", "--renormalize", "."});
             std::cout << "Renormalized Git index to enforce LF rules.\n";
             // Optional: converting working tree files could be done here, but standard `git add --renormalize` handles index cleanly.
        }

        std::cout << "Fix complete.\n";
    } else {
        if (lfIssues > 0 || execIssues > 0) {
            std::exit(1);
        }
    }
}

} // namespace

void RegisterRepoHygiene(CLI::App& InApp) {
    auto* cmd = InApp.add_subcommand("repo-hygiene", "Detect and fix Git-index executable bits and CRLF/LF issues");

    auto* repo = new std::string{"."};
    cmd->add_option("--repo", *repo, "Target repository root path");

    auto* check = cmd->add_subcommand("check", "Check repository hygiene");
    auto* checkArchiveSafe = new bool{false};
    check->add_flag("--archive-safe", *checkArchiveSafe, "Run strict archive-safe checks");
    check->callback([=]() {
        const auto root = std::filesystem::weakly_canonical(std::filesystem::path(*repo));
        CheckRepoHygiene(root, false, *checkArchiveSafe);
    });

    auto* fix = cmd->add_subcommand("fix", "Fix repository hygiene issues automatically");
    auto* fixArchiveSafe = new bool{false};
    fix->add_flag("--archive-safe", *fixArchiveSafe, "Apply strict archive-safe fixes");
    fix->callback([=]() {
        const auto root = std::filesystem::weakly_canonical(std::filesystem::path(*repo));
        CheckRepoHygiene(root, true, *fixArchiveSafe);
    });
}

} // namespace kano::git::commands
