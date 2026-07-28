#include "tui_history_patch.hpp"

#include "shell_executor.hpp"

#include <string>
#include <vector>

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

auto GitCapture(const std::filesystem::path& InRepo,
                const std::vector<std::string>& InArgs) -> shell::ExecResult {
    return shell::ExecuteCommand("git", InArgs, shell::ExecMode::Capture, InRepo);
}

}

auto FetchCommitDetail(const std::filesystem::path& InRepo,
                       const std::string& InSha,
                       const int InMode) -> std::string {
    if (InSha.empty()) {
        return "(invalid commit sha)";
    }

    std::vector<std::string> args;
    if (InMode == 1) {
        args = {"show", "--no-color", "--date=iso", "-M", "-C", "--name-status", "--pretty=fuller", "-n", "1", InSha};
    } else if (InMode == 2) {
        args = {"show", "--no-color", "--date=iso", "-M", "-C", "--pretty=fuller", "-n", "1", InSha};
    } else {
        args = {"show", "--no-color", "--date=iso", "-M", "-C", "--stat", "--name-status", "--pretty=fuller", "-n", "1", InSha};
    }

    const auto out = GitCapture(InRepo, args);
    if (out.exitCode != 0) {
        return "(failed to load commit detail)\n" + out.stderrStr;
    }
    auto body = out.stdoutStr;
    constexpr std::size_t kMaxChars = 20000;
    if (body.size() > kMaxChars) {
        body = body.substr(0, kMaxChars) + "\n... (truncated)";
    }
    return body;
}

auto FetchCommitFilePatch(const std::filesystem::path& InRepo,
                          const std::string& InSha,
                          const std::string& InPatchPath,
                          const std::string& InPatchPathAlt) -> std::string {
    if (InSha.empty()) {
        return "(invalid commit sha)";
    }
    if (InPatchPath.empty()) {
        return FetchCommitDetail(InRepo, InSha, 2);
    }

    std::vector<std::string> args = {
        "show", "--no-color", "--date=iso", "-M", "-C", "--pretty=fuller", "-n", "1", InSha, "--", InPatchPath,
    };
    if (!InPatchPathAlt.empty() && InPatchPathAlt != InPatchPath) {
        args.push_back(InPatchPathAlt);
    }
    const auto out = GitCapture(InRepo, args);
    if (out.exitCode != 0) {
        return "(failed to load file patch)\n" + out.stderrStr;
    }
    auto body = out.stdoutStr;

    // Merge commits: git show <sha> -- <path> may produce empty combined diff.
    // Fallback: compare against first parent (sha~1) to show the actual change.
    if (Trim(body).empty() || body.find("diff ") == std::string::npos) {
        std::vector<std::string> fallbackArgs = {
            "diff", "--no-color", InSha + "~1", InSha, "--", InPatchPath,
        };
        if (!InPatchPathAlt.empty() && InPatchPathAlt != InPatchPath) {
            fallbackArgs.push_back(InPatchPathAlt);
        }
        const auto fb = GitCapture(InRepo, fallbackArgs);
        if (fb.exitCode == 0 && !Trim(fb.stdoutStr).empty()) {
            body = fb.stdoutStr;
        }
    }

    constexpr std::size_t kMaxChars = 20000;
    if (body.size() > kMaxChars) {
        body = body.substr(0, kMaxChars) + "\n... (truncated)";
    }
    return body.empty() ? "(no patch for selected file)" : body;
}

auto FetchWorkingTreeFilePatch(const std::filesystem::path& InRepo,
                               const std::string& InPatchPath,
                               const std::string& InPatchPathAlt) -> std::string {
    if (InPatchPath.empty()) {
        return FetchWorkingTreeDetail(InRepo, 2);
    }

    auto buildArgs = [&](std::vector<std::string> base) -> std::vector<std::string> {
        base.push_back("--");
        base.push_back(InPatchPath);
        if (!InPatchPathAlt.empty() && InPatchPathAlt != InPatchPath) {
            base.push_back(InPatchPathAlt);
        }
        return base;
    };

    const auto unstaged = GitCapture(InRepo, buildArgs({"diff", "--no-color"}));
    const auto staged = GitCapture(InRepo, buildArgs({"diff", "--no-color", "--cached"}));
    const auto fallback = GitCapture(InRepo, buildArgs({"diff", "--no-color", "HEAD"}));

    if (unstaged.exitCode != 0 && staged.exitCode != 0 && fallback.exitCode != 0) {
        return "(failed to load file patch)\n" + fallback.stderrStr;
    }

    std::string body;
    const auto unstagedBody = Trim(unstaged.stdoutStr);
    const auto stagedBody = Trim(staged.stdoutStr);
    if (!unstagedBody.empty()) {
        body += "# unstaged\n" + unstaged.stdoutStr;
    }
    if (!stagedBody.empty()) {
        if (!body.empty()) {
            body += "\n\n";
        }
        body += "# staged\n" + staged.stdoutStr;
    }
    if (body.empty() && fallback.exitCode == 0) {
        body = fallback.stdoutStr;
    }

    // Fallback for untracked files: git diff returns nothing for files not in the index.
    // git diff --no-index -- /dev/null <path> shows new file content as a diff.
    // Exit code 1 is expected (means differences found).
    if (Trim(body).empty()) {
        const auto noIndex = GitCapture(InRepo, {"diff", "--no-color", "--no-index", "--", "/dev/null", InPatchPath});
        if ((noIndex.exitCode == 0 || noIndex.exitCode == 1) && !Trim(noIndex.stdoutStr).empty()) {
            body = "# untracked (new file)\n" + noIndex.stdoutStr;
        }
    }

    constexpr std::size_t kMaxChars = 20000;
    if (body.size() > kMaxChars) {
        body = body.substr(0, kMaxChars) + "\n... (truncated)";
    }
    return Trim(body).empty() ? "(no patch for selected file)" : body;
}

auto FetchWorkingTreeDetail(const std::filesystem::path& InRepo, const int InMode) -> std::string {
    std::vector<std::string> args;
    if (InMode == 1) {
        args = {"status", "--short", "--branch"};
    } else if (InMode == 2) {
        args = {"diff", "--no-color", "HEAD"};
    } else {
        args = {"diff", "--no-color", "--stat", "HEAD"};
    }

    auto out = GitCapture(InRepo, args);
    if (out.exitCode != 0 && (InMode == 0 || InMode == 2)) {
        args = InMode == 2
            ? std::vector<std::string>{"diff", "--no-color"}
            : std::vector<std::string>{"diff", "--no-color", "--stat"};
        out = GitCapture(InRepo, args);
    }
    if (out.exitCode != 0) {
        return "(failed to load working tree detail)\n" + out.stderrStr;
    }
    auto body = out.stdoutStr;
    if (Trim(body).empty()) {
        body = "working tree is clean";
    }
    constexpr std::size_t kMaxChars = 20000;
    if (body.size() > kMaxChars) {
        body = body.substr(0, kMaxChars) + "\n... (truncated)";
    }
    return body;
}

}
