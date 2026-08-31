#include "tui_display_cells.hpp"

#include <ftxui/screen/string.hpp>

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace kano::git::commands {
namespace {

struct Utf8CodePoint {
    bool valid = false;
    std::size_t length = 1U;
    std::uint32_t value = 0U;
};

auto DecodeUtf8(const std::string_view InText,
                const std::size_t InOffset) -> Utf8CodePoint {
    const auto lead = static_cast<unsigned char>(InText[InOffset]);
    if (lead <= 0x7FU) {
        return {.valid = true, .length = 1U, .value = lead};
    }

    std::size_t length = 0U;
    std::uint32_t value = 0U;
    std::uint32_t minimum = 0U;
    if (lead >= 0xC2U && lead <= 0xDFU) {
        length = 2U;
        value = lead & 0x1FU;
        minimum = 0x80U;
    } else if (lead >= 0xE0U && lead <= 0xEFU) {
        length = 3U;
        value = lead & 0x0FU;
        minimum = 0x800U;
    } else if (lead >= 0xF0U && lead <= 0xF4U) {
        length = 4U;
        value = lead & 0x07U;
        minimum = 0x10000U;
    } else {
        return {};
    }

    if (InOffset + length > InText.size()) {
        return {};
    }
    for (std::size_t index = 1U; index < length; ++index) {
        const auto byte = static_cast<unsigned char>(InText[InOffset + index]);
        if ((byte & 0xC0U) != 0x80U) {
            return {};
        }
        value = (value << 6U) | (byte & 0x3FU);
    }
    if (value < minimum || value > 0x10FFFFU ||
        (value >= 0xD800U && value <= 0xDFFFU)) {
        return {};
    }
    return {.valid = true, .length = length, .value = value};
}

auto AppendHexEscape(std::string& OutText, const unsigned char InByte) -> void {
    constexpr std::string_view kHexDigits = "0123456789ABCDEF";
    OutText += "\\x";
    OutText.push_back(kHexDigits[(InByte >> 4U) & 0x0FU]);
    OutText.push_back(kHexDigits[InByte & 0x0FU]);
}

auto AppendEscapedBytes(std::string& OutText,
                        const std::string_view InText,
                        const std::size_t InOffset,
                        const std::size_t InLength) -> void {
    for (std::size_t index = 0U; index < InLength; ++index) {
        AppendHexEscape(
            OutText,
            static_cast<unsigned char>(InText[InOffset + index]));
    }
}

auto IsEmojiModifier(const std::string_view InGlyph) -> bool {
    if (InGlyph.empty()) {
        return false;
    }
    const auto codePoint = DecodeUtf8(InGlyph, 0U);
    return codePoint.valid && codePoint.value >= 0x1F3FBU &&
        codePoint.value <= 0x1F3FFU;
}

auto DisplayGlyphs(const std::string_view InText) -> std::vector<std::string> {
    std::vector<std::string> glyphs;
    for (auto glyph : ftxui::Utf8ToGlyphs(TuiDisplaySanitize(InText))) {
        if (glyph.empty() || ftxui::string_width(glyph) == 0 ||
            IsEmojiModifier(glyph)) {
            if (!glyphs.empty()) {
                glyphs.back() += glyph;
            } else if (!glyph.empty()) {
                glyphs.push_back(std::move(glyph));
            }
            continue;
        }
        glyphs.push_back(std::move(glyph));
    }
    return glyphs;
}

auto JoinGlyphRange(const std::vector<std::string>& InGlyphs,
                    const std::size_t InBegin,
                    const std::size_t InEnd) -> std::string {
    std::string text;
    for (std::size_t index = InBegin; index < InEnd; ++index) {
        text += InGlyphs[index];
    }
    return text;
}

auto PrefixWithinCells(const std::vector<std::string>& InGlyphs,
                       const int InMaximumCells) -> std::string {
    if (InMaximumCells <= 0) {
        return {};
    }
    int usedCells = 0;
    std::size_t end = 0U;
    while (end < InGlyphs.size()) {
        const int glyphWidth = ftxui::string_width(InGlyphs[end]);
        if (usedCells + glyphWidth > InMaximumCells) {
            break;
        }
        usedCells += glyphWidth;
        ++end;
    }
    return JoinGlyphRange(InGlyphs, 0U, end);
}

auto SuffixWithinCells(const std::vector<std::string>& InGlyphs,
                       const int InMaximumCells) -> std::string {
    if (InMaximumCells <= 0) {
        return {};
    }
    int usedCells = 0;
    std::size_t begin = InGlyphs.size();
    while (begin > 0U) {
        const int glyphWidth = ftxui::string_width(InGlyphs[begin - 1U]);
        if (usedCells + glyphWidth > InMaximumCells) {
            break;
        }
        usedCells += glyphWidth;
        --begin;
    }
    return JoinGlyphRange(InGlyphs, begin, InGlyphs.size());
}

auto MarkerWithinCells(const std::string_view InMarker,
                       const int InMaximumCells) -> std::string {
    return PrefixWithinCells(DisplayGlyphs(InMarker), InMaximumCells);
}

} // namespace

auto TuiDisplaySanitize(const std::string_view InText) -> std::string {
    std::string sanitized;
    sanitized.reserve(InText.size());
    for (std::size_t offset = 0U; offset < InText.size();) {
        const auto byte = static_cast<unsigned char>(InText[offset]);
        if (byte <= 0x7FU) {
            switch (byte) {
                case '\n':
                    sanitized += "\\n";
                    break;
                case '\r':
                    sanitized += "\\r";
                    break;
                case '\t':
                    sanitized += "\\t";
                    break;
                default:
                    if (byte < 0x20U || byte == 0x7FU) {
                        AppendHexEscape(sanitized, byte);
                    } else {
                        sanitized.push_back(static_cast<char>(byte));
                    }
                    break;
            }
            ++offset;
            continue;
        }

        const auto codePoint = DecodeUtf8(InText, offset);
        if (!codePoint.valid) {
            AppendHexEscape(sanitized, byte);
            ++offset;
            continue;
        }
        if (codePoint.value >= 0x80U && codePoint.value <= 0x9FU) {
            AppendEscapedBytes(sanitized, InText, offset, codePoint.length);
        } else {
            sanitized.append(InText.substr(offset, codePoint.length));
        }
        offset += codePoint.length;
    }
    return sanitized;
}

auto TuiDisplayWidth(const std::string_view InText) -> int {
    return ftxui::string_width(TuiDisplaySanitize(InText));
}

auto TuiDisplayEllipsis(const int InMaximumCells) -> std::string {
    return std::string(static_cast<std::size_t>(std::clamp(InMaximumCells, 0, 3)), '.');
}

auto TuiDisplayTruncateEnd(const std::string_view InText,
                            const int InMaximumCells) -> std::string {
    if (InMaximumCells <= 0) {
        return {};
    }
    const auto sanitized = TuiDisplaySanitize(InText);
    if (ftxui::string_width(sanitized) <= InMaximumCells) {
        return sanitized;
    }
    const auto ellipsis = TuiDisplayEllipsis(InMaximumCells);
    const int contentCells = InMaximumCells - ftxui::string_width(ellipsis);
    return PrefixWithinCells(DisplayGlyphs(sanitized), contentCells) + ellipsis;
}

auto TuiDisplayTruncateEndWithPrefix(const std::string_view InPrefix,
                                     const std::string_view InValue,
                                     const int InMaximumCells) -> std::string {
    const auto prefix = TuiDisplaySanitize(InPrefix);
    const int prefixCells = ftxui::string_width(prefix);
    if (prefixCells >= InMaximumCells) {
        return TuiDisplayTruncateEnd(prefix, InMaximumCells);
    }
    return prefix + TuiDisplayTruncateEnd(InValue, InMaximumCells - prefixCells);
}

auto TuiDisplayTruncateFront(const std::string_view InText,
                              const int InMaximumCells) -> std::string {
    if (InMaximumCells <= 0) {
        return {};
    }
    const auto sanitized = TuiDisplaySanitize(InText);
    if (ftxui::string_width(sanitized) <= InMaximumCells) {
        return sanitized;
    }
    const auto ellipsis = TuiDisplayEllipsis(InMaximumCells);
    const int contentCells = InMaximumCells - ftxui::string_width(ellipsis);
    return ellipsis + SuffixWithinCells(DisplayGlyphs(sanitized), contentCells);
}

auto TuiDisplayTruncateMiddle(const std::string_view InText,
                              const int InMaximumCells,
                              const std::string_view InEllipsis) -> std::string {
    if (InMaximumCells <= 0) {
        return {};
    }
    const auto sanitized = TuiDisplaySanitize(InText);
    if (ftxui::string_width(sanitized) <= InMaximumCells) {
        return sanitized;
    }
    const auto marker = MarkerWithinCells(InEllipsis, InMaximumCells);
    const int markerWidth = ftxui::string_width(marker);
    if (markerWidth >= InMaximumCells) {
        return marker;
    }

    const int contentCells = InMaximumCells - markerWidth;
    const int prefixCells = contentCells / 2 + 1;
    const int suffixCells = std::max(0, contentCells - prefixCells);
    const auto glyphs = DisplayGlyphs(sanitized);
    return PrefixWithinCells(glyphs, prefixCells) + marker +
        SuffixWithinCells(glyphs, suffixCells);
}

auto TuiDisplayPadRight(const std::string_view InText, const int InWidth)
    -> std::string {
    const auto bounded = TuiDisplayTruncateEnd(InText, InWidth);
    const int padding = std::max(0, InWidth - ftxui::string_width(bounded));
    return bounded + std::string(static_cast<std::size_t>(padding), ' ');
}

auto TuiDisplayPadLeft(const std::string_view InText, const int InWidth)
    -> std::string {
    const auto bounded = TuiDisplayTruncateEnd(InText, InWidth);
    const int padding = std::max(0, InWidth - ftxui::string_width(bounded));
    return std::string(static_cast<std::size_t>(padding), ' ') + bounded;
}

} // namespace kano::git::commands
