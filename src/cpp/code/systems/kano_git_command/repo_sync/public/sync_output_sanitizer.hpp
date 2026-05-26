#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace kano::git::commands {

enum class SyncOutputSanitizeMode {
    Human,
    DebugRaw,
};

static inline auto NormalizeSyncCapturedText(std::string_view InText,
                                             SyncOutputSanitizeMode InMode = SyncOutputSanitizeMode::Human) -> std::string {
    if (InText.empty()) {
        return {};
    }

    auto decodeVisibleControlGlyphs = [](std::string_view InRaw) {
        // Some renderer paths can materialize C0 controls as Unicode glyphs.
        // Recover them so human-mode normalization can treat them as true controls.
        constexpr std::string_view kVisibleEsc = "\xE2\x86\x90"; // U+2190 (left arrow) => ESC
        constexpr std::string_view kVisibleCr = "\xE2\x99\xAA";  // U+266A (music note) => CR
        constexpr std::string_view kVisibleLf = "\xE2\x97\x99";  // U+25D9 (inverse circle) => LF

        std::string out;
        out.reserve(InRaw.size());
        for (std::size_t i = 0; i < InRaw.size();) {
            if (i + kVisibleEsc.size() <= InRaw.size() && InRaw.substr(i, kVisibleEsc.size()) == kVisibleEsc) {
                out.push_back('\x1B');
                i += kVisibleEsc.size();
                continue;
            }
            if (i + kVisibleCr.size() <= InRaw.size() && InRaw.substr(i, kVisibleCr.size()) == kVisibleCr) {
                out.push_back('\r');
                i += kVisibleCr.size();
                continue;
            }
            if (i + kVisibleLf.size() <= InRaw.size() && InRaw.substr(i, kVisibleLf.size()) == kVisibleLf) {
                out.push_back('\n');
                i += kVisibleLf.size();
                continue;
            }
            out.push_back(InRaw[i]);
            i += 1;
        }
        return out;
    };

    auto normalizeLineEndingsToLf = [](std::string_view InRaw) {
        std::string out;
        out.reserve(InRaw.size());
        for (std::size_t i = 0; i < InRaw.size(); ++i) {
            const char ch = InRaw[i];
            if (ch == '\r') {
                if ((i + 1) < InRaw.size() && InRaw[i + 1] == '\n') {
                    i += 1;
                }
                out.push_back('\n');
                continue;
            }
            out.push_back(ch);
        }
        return out;
    };

    auto stripAnsiSequences = [](std::string_view InRaw) {
        std::string out;
        out.reserve(InRaw.size());

        for (std::size_t i = 0; i < InRaw.size(); ++i) {
            const unsigned char ch = static_cast<unsigned char>(InRaw[i]);
            if (ch != 0x1B) {
                out.push_back(static_cast<char>(ch));
                continue;
            }

            if ((i + 1) >= InRaw.size()) {
                continue;
            }

            const char next = InRaw[i + 1];
            if (next == '[') {
                i += 2;
                while (i < InRaw.size()) {
                    const unsigned char c = static_cast<unsigned char>(InRaw[i]);
                    if (c >= 0x40 && c <= 0x7E) {
                        break;
                    }
                    i += 1;
                }
                continue;
            }

            if (next == ']') {
                i += 2;
                while (i < InRaw.size()) {
                    if (InRaw[i] == '\a') {
                        break;
                    }
                    if (InRaw[i] == '\x1B' && (i + 1) < InRaw.size() && InRaw[i + 1] == '\\') {
                        i += 1;
                        break;
                    }
                    i += 1;
                }
                continue;
            }
            // Non-CSI/OSC escape; drop ESC only.
        }

        return out;
    };

    auto removeNonHumanControlBytes = [](std::string_view InRaw) {
        std::string out;
        out.reserve(InRaw.size());
        for (const char ch : InRaw) {
            const unsigned char uch = static_cast<unsigned char>(ch);
            if (uch < 0x20) {
                if (ch == '\n' || ch == '\t') {
                    out.push_back(ch);
                }
                continue;
            }
            if (uch == 0x7F) {
                continue;
            }
            out.push_back(ch);
        }
        return out;
    };

    auto ensureSingleTrailingNewline = [](std::string InRaw) {
        while (!InRaw.empty() && InRaw.back() == '\n') {
            InRaw.pop_back();
        }
        if (!InRaw.empty()) {
            InRaw.push_back('\n');
        }
        return InRaw;
    };

    std::string text = decodeVisibleControlGlyphs(InText);
    text = normalizeLineEndingsToLf(text);

    if (InMode == SyncOutputSanitizeMode::Human) {
        text = stripAnsiSequences(text);
        text = removeNonHumanControlBytes(text);
    }

    return ensureSingleTrailingNewline(std::move(text));
}

} // namespace kano::git::commands
