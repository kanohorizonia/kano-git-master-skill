#include "tui_history_patch.hpp"

#include "shell_executor.hpp"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace kano::git::commands {
namespace {

constexpr std::string_view kTruncatedSuffix = "\n... (truncated)";

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
                const std::vector<std::string>& InArgs,
                const std::size_t InMaxBytes = kTuiFilePatchMaxBytes)
    -> shell::ExecResult {
    constexpr unsigned int kInteractiveReadTimeoutMs = 5000;
    return shell::ExecuteCommand(
        "git",
        InArgs,
        shell::ExecMode::Capture,
        InRepo,
        shell::ProgressCallback{},
        kInteractiveReadTimeoutMs,
        shell::CaptureLimits{InMaxBytes, InMaxBytes});
}

auto IsCancelled(const TuiGitProbeControl& InControl) -> bool {
    return InControl.isCancelled && InControl.isCancelled();
}

auto ControlledGitCapture(const std::filesystem::path& InRepo,
                          const std::vector<std::string>& InArgs,
                          const TuiGitProbeControl& InControl,
                          const std::size_t InMaxBytes = kTuiFilePatchMaxBytes)
    -> std::optional<shell::ExecResult> {
    if (!TryBeginTuiGitProbe(InControl, InArgs)) {
        return std::nullopt;
    }
    return GitCapture(InRepo, InArgs, InMaxBytes);
}

auto TruncateFilePatch(std::string InBody,
                       const bool InCaptureTruncated = false) -> std::string {
    if (!InCaptureTruncated && InBody.size() <= kTuiFilePatchMaxBytes) {
        return InBody;
    }
    static_assert(kTruncatedSuffix.size() < kTuiFilePatchMaxBytes);
    if (InBody.size() > kTuiFilePatchMaxBytes - kTruncatedSuffix.size()) {
        InBody.resize(kTuiFilePatchMaxBytes - kTruncatedSuffix.size());
    }
    InBody.append(kTruncatedSuffix);
    return InBody;
}

}

auto TryBeginTuiGitProbe(
    const TuiGitProbeControl& InControl,
    const std::vector<std::string>& InArguments) -> bool {
    if (IsCancelled(InControl)) {
        return false;
    }
    if (InControl.onLaunch) {
        InControl.onLaunch(InArguments);
    }
    return true;
}

auto ParseTuiPorcelainV1Z(
    const std::string_view InRaw,
    const TuiPorcelainParseLimits InLimits) -> TuiPorcelainParseResult {
    TuiPorcelainParseResult result;
    const auto boundedSize = std::min(InRaw.size(), InLimits.maxBytes);
    const auto bounded = InRaw.substr(0, boundedSize);
    result.truncated = boundedSize < InRaw.size();

    std::size_t pos = 0;
    auto readField = [&]() -> std::optional<std::string_view> {
        if (pos >= bounded.size()) {
            return std::nullopt;
        }
        const auto nulPos = bounded.find('\0', pos);
        if (nulPos == std::string_view::npos) {
            result.truncated = result.truncated || boundedSize < InRaw.size();
            result.malformed = !result.truncated;
            pos = bounded.size();
            return std::nullopt;
        }
        const auto field = bounded.substr(pos, nulPos - pos);
        pos = nulPos + 1;
        return field;
    };

    while (pos < bounded.size()) {
        if (result.entries.size() >= InLimits.maxEntries) {
            result.truncated = true;
            break;
        }

        const auto record = readField();
        if (!record.has_value()) {
            break;
        }
        if (record->size() < 3 || (*record)[2] != ' ') {
            result.malformed = true;
            break;
        }

        TuiPorcelainPath entry{
            .indexStatus = (*record)[0],
            .worktreeStatus = (*record)[1],
            .path = std::string(record->substr(3)),
        };
        if (entry.path.empty()) {
            result.malformed = true;
            break;
        }

        const bool bRenameOrCopy =
            entry.indexStatus == 'R' || entry.worktreeStatus == 'R' ||
            entry.indexStatus == 'C' || entry.worktreeStatus == 'C';
        if (bRenameOrCopy) {
            const auto previousPath = readField();
            if (!previousPath.has_value() || previousPath->empty()) {
                result.malformed = true;
                break;
            }
            entry.previousPath = std::string(*previousPath);
        }
        result.entries.push_back(std::move(entry));
    }

    return result;
}

auto EscapeTuiPathForDisplay(const std::string_view InPath) -> std::string {
    constexpr char kHex[] = "0123456789ABCDEF";
    std::string display;
    display.reserve(InPath.size());
    auto appendHexByte = [&](const unsigned char InByte) {
        display += "\\x";
        display.push_back(kHex[(InByte >> 4U) & 0x0FU]);
        display.push_back(kHex[InByte & 0x0FU]);
    };

    std::size_t index = 0;
    while (index < InPath.size()) {
        const auto ch = static_cast<unsigned char>(InPath[index]);
        if (ch >= 0x80U) {
            std::size_t length = 0;
            std::uint32_t codePoint = 0;
            std::uint32_t minimum = 0;
            if (ch >= 0xC2U && ch <= 0xDFU) {
                length = 2;
                codePoint = ch & 0x1FU;
                minimum = 0x80U;
            } else if (ch >= 0xE0U && ch <= 0xEFU) {
                length = 3;
                codePoint = ch & 0x0FU;
                minimum = 0x800U;
            } else if (ch >= 0xF0U && ch <= 0xF4U) {
                length = 4;
                codePoint = ch & 0x07U;
                minimum = 0x10000U;
            }

            bool valid = length > 0 && index + length <= InPath.size();
            for (std::size_t offset = 1; valid && offset < length; ++offset) {
                const auto continuation =
                    static_cast<unsigned char>(InPath[index + offset]);
                valid = (continuation & 0xC0U) == 0x80U;
                if (valid) {
                    codePoint = (codePoint << 6U) |
                        (continuation & 0x3FU);
                }
            }
            valid = valid && codePoint >= minimum &&
                codePoint <= 0x10FFFFU &&
                !(codePoint >= 0xD800U && codePoint <= 0xDFFFU);
            if (!valid) {
                appendHexByte(ch);
                ++index;
                continue;
            }
            if (codePoint >= 0x80U && codePoint <= 0x9FU) {
                display += "\\u00";
                display.push_back(kHex[(codePoint >> 4U) & 0x0FU]);
                display.push_back(kHex[codePoint & 0x0FU]);
            } else {
                display.append(InPath.substr(index, length));
            }
            index += length;
            continue;
        }

        switch (ch) {
            case '\\':
                display += "\\\\";
                break;
            case '\n':
                display += "\\n";
                break;
            case '\r':
                display += "\\r";
                break;
            case '\t':
                display += "\\t";
                break;
            default:
                if (ch < 0x20U || ch == 0x7FU) {
                    appendHexByte(ch);
                } else {
                    display.push_back(static_cast<char>(ch));
                }
                break;
        }
        ++index;
    }
    return display;
}

auto TryStartTuiDetailPatchFetch(
    TuiDetailPatchBudget& InOutBudget) noexcept -> bool {
    if (InOutBudget.startedFetches >= kTuiDetailPatchFetchLimit) {
        return false;
    }
    InOutBudget.startedFetches += 1;
    return true;
}

auto ParseHistoryNameStatusZ(
    const std::string_view InRaw,
    const TuiPorcelainParseLimits InLimits)
    -> HistoryNameStatusParseResult {
    HistoryNameStatusParseResult result;
    const auto boundedSize = std::min(InRaw.size(), InLimits.maxBytes);
    const auto bounded = InRaw.substr(0, boundedSize);
    result.truncated = boundedSize < InRaw.size();
    std::size_t pos = 0;
    auto readField = [&]() -> std::optional<std::string_view> {
        if (pos >= bounded.size()) {
            return std::nullopt;
        }
        const auto nulPos = bounded.find('\0', pos);
        if (nulPos == std::string_view::npos) {
            result.malformed = !result.truncated;
            result.truncated = result.truncated || boundedSize < InRaw.size();
            pos = bounded.size();
            return std::nullopt;
        }
        const auto field = bounded.substr(pos, nulPos - pos);
        pos = nulPos + 1;
        return field;
    };

    while (pos < bounded.size()) {
        if (result.entries.size() >= InLimits.maxEntries) {
            result.truncated = true;
            break;
        }
        auto status = readField();
        if (!status.has_value()) {
            break;
        }
        while (!status->empty() &&
               (status->front() == '\n' || status->front() == '\r')) {
            status = status->substr(1);
        }
        if (status->empty()) {
            continue;
        }

        const auto firstPath = readField();
        if (!firstPath.has_value()) {
            result.malformed = !result.truncated;
            break;
        }

        HistoryChangedPath path{
            .status = std::string(*status),
            .path = std::string(*firstPath),
        };
        const char statusCode = path.status.front();
        if (statusCode == 'R' || statusCode == 'C') {
            const auto secondPath = readField();
            if (!secondPath.has_value()) {
                result.malformed = !result.truncated;
                break;
            }
            path.previousPath = std::move(path.path);
            path.path = std::string(*secondPath);
        }
        if (path.path.empty()) {
            result.malformed = true;
            break;
        }
        result.entries.push_back(std::move(path));
    }
    return result;
}

auto FetchCommitDetail(const std::filesystem::path& InRepo,
                       const std::string& InSha,
                       const int InMode,
                       const TuiGitProbeControl& InControl) -> std::string {
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

    const auto out = ControlledGitCapture(
        InRepo, args, InControl, kTuiFilePatchMaxBytes);
    if (!out.has_value()) {
        return "(detail load cancelled)";
    }
    if (out->exitCode != 0) {
        return "(failed to load commit detail)\n" + out->stderrStr;
    }
    return TruncateFilePatch(out->stdoutStr, out->stdoutTruncated);
}

auto FetchCommitFilePatch(const std::filesystem::path& InRepo,
                          const std::string& InSha,
                          const std::string& InPatchPath,
                          const std::string& InPatchPathAlt,
                          const TuiGitProbeControl& InControl) -> std::string {
    if (InSha.empty()) {
        return "(invalid commit sha)";
    }
    if (InPatchPath.empty()) {
        return TruncateFilePatch(
            FetchCommitDetail(InRepo, InSha, 2, InControl));
    }

    std::vector<std::string> args = {
        "show", "--no-color", "--date=iso", "-M", "-C", "--pretty=fuller", "-n", "1", InSha, "--", InPatchPath,
    };
    if (!InPatchPathAlt.empty() && InPatchPathAlt != InPatchPath) {
        args.push_back(InPatchPathAlt);
    }
    const auto out = ControlledGitCapture(InRepo, args, InControl);
    if (!out.has_value()) {
        return "(detail load cancelled)";
    }
    if (out->exitCode != 0) {
        return TruncateFilePatch(
            "(failed to load file patch)\n" + out->stderrStr);
    }
    auto body = out->stdoutStr;
    bool bCaptureTruncated = out->stdoutTruncated;

    // Merge commits: git show <sha> -- <path> may produce empty combined diff.
    // Fallback: compare against first parent (sha~1) to show the actual change.
    if (Trim(body).empty() || body.find("diff ") == std::string::npos) {
        std::vector<std::string> fallbackArgs = {
            "diff", "--no-color", InSha + "~1", InSha, "--", InPatchPath,
        };
        if (!InPatchPathAlt.empty() && InPatchPathAlt != InPatchPath) {
            fallbackArgs.push_back(InPatchPathAlt);
        }
        const auto fb = ControlledGitCapture(InRepo, fallbackArgs, InControl);
        if (!fb.has_value()) {
            return "(detail load cancelled)";
        }
        if (fb->exitCode == 0 && !Trim(fb->stdoutStr).empty()) {
            body = fb->stdoutStr;
            bCaptureTruncated = fb->stdoutTruncated;
        }
    }

    body = TruncateFilePatch(std::move(body), bCaptureTruncated);
    return body.empty() ? "(no patch for selected file)" : body;
}

auto FetchWorkingTreeFilePatch(const std::filesystem::path& InRepo,
                               const std::string& InPatchPath,
                               const std::string& InPatchPathAlt,
                               const TuiGitProbeControl& InControl) -> std::string {
    if (InPatchPath.empty()) {
        return TruncateFilePatch(FetchWorkingTreeDetail(InRepo, 2, InControl));
    }

    auto buildArgs = [&](std::vector<std::string> base) -> std::vector<std::string> {
        base.push_back("--");
        base.push_back(InPatchPath);
        if (!InPatchPathAlt.empty() && InPatchPathAlt != InPatchPath) {
            base.push_back(InPatchPathAlt);
        }
        return base;
    };

    const auto unstaged = ControlledGitCapture(
        InRepo, buildArgs({"diff", "--no-color"}), InControl);
    if (!unstaged.has_value()) {
        return "(detail load cancelled)";
    }
    const auto staged = ControlledGitCapture(
        InRepo, buildArgs({"diff", "--no-color", "--cached"}), InControl);
    if (!staged.has_value()) {
        return "(detail load cancelled)";
    }

    std::string body;
    bool bCaptureTruncated =
        unstaged->stdoutTruncated || staged->stdoutTruncated;
    const auto unstagedBody = Trim(unstaged->stdoutStr);
    const auto stagedBody = Trim(staged->stdoutStr);
    if (!unstagedBody.empty()) {
        body += "# unstaged\n" + unstaged->stdoutStr;
    }
    if (!stagedBody.empty()) {
        if (!body.empty()) {
            body += "\n\n";
        }
        body += "# staged\n" + staged->stdoutStr;
    }

    std::optional<shell::ExecResult> fallback;
    if (body.empty()) {
        fallback = ControlledGitCapture(
            InRepo,
            buildArgs({"diff", "--no-color", "HEAD"}),
            InControl);
        if (!fallback.has_value()) {
            return "(detail load cancelled)";
        }
        if (fallback->exitCode == 0) {
            body = fallback->stdoutStr;
            bCaptureTruncated = fallback->stdoutTruncated;
        }
    }
    if (unstaged->exitCode != 0 && staged->exitCode != 0 &&
        fallback.has_value() && fallback->exitCode != 0) {
        return TruncateFilePatch(
            "(failed to load file patch)\n" + fallback->stderrStr);
    }

    // Fallback for untracked files: git diff returns nothing for files not in the index.
    // git diff --no-index -- /dev/null <path> shows new file content as a diff.
    // Exit code 1 is expected (means differences found).
    if (Trim(body).empty()) {
        const auto noIndex = ControlledGitCapture(
            InRepo,
            {"diff", "--no-color", "--no-index", "--", "/dev/null", InPatchPath},
            InControl);
        if (!noIndex.has_value()) {
            return "(detail load cancelled)";
        }
        if ((noIndex->exitCode == 0 || noIndex->exitCode == 1) &&
            !Trim(noIndex->stdoutStr).empty()) {
            body = "# untracked (new file)\n" + noIndex->stdoutStr;
            bCaptureTruncated = noIndex->stdoutTruncated;
        }
    }

    body = TruncateFilePatch(std::move(body), bCaptureTruncated);
    return Trim(body).empty() ? "(no patch for selected file)" : body;
}

auto FetchWorkingTreeDetail(const std::filesystem::path& InRepo,
                            const int InMode,
                            const TuiGitProbeControl& InControl) -> std::string {
    std::vector<std::string> args;
    if (InMode == 1) {
        args = {"status", "--short", "--branch"};
    } else if (InMode == 2) {
        args = {"diff", "--no-color", "HEAD"};
    } else {
        args = {"diff", "--no-color", "--stat", "HEAD"};
    }

    auto out = ControlledGitCapture(InRepo, args, InControl);
    if (!out.has_value()) {
        return "(detail load cancelled)";
    }
    if (out->exitCode != 0 && (InMode == 0 || InMode == 2)) {
        args = InMode == 2
            ? std::vector<std::string>{"diff", "--no-color"}
            : std::vector<std::string>{"diff", "--no-color", "--stat"};
        out = ControlledGitCapture(InRepo, args, InControl);
        if (!out.has_value()) {
            return "(detail load cancelled)";
        }
    }
    if (out->exitCode != 0) {
        return "(failed to load working tree detail)\n" + out->stderrStr;
    }
    auto body = out->stdoutStr;
    if (Trim(body).empty()) {
        body = "working tree is clean";
    }
    return TruncateFilePatch(std::move(body), out->stdoutTruncated);
}

}
