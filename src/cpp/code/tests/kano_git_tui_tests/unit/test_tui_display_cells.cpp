#include <catch2/catch_test_macros.hpp>

#include "tui_audit_frame.hpp"
#include "tui_display_cells.hpp"

#include <ftxui/screen/screen.hpp>
#include <ftxui/screen/string.hpp>

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

namespace {

using namespace kano::git::commands;

auto IsValidUtf8(const std::string_view InText) -> bool {
    for (std::size_t index = 0; index < InText.size();) {
        const auto lead = static_cast<unsigned char>(InText[index]);
        if (lead < 0x80U) {
            ++index;
            continue;
        }

        std::size_t length = 0U;
        std::uint32_t codePoint = 0U;
        std::uint32_t minimum = 0U;
        if (lead >= 0xC2U && lead <= 0xDFU) {
            length = 2U;
            codePoint = lead & 0x1FU;
            minimum = 0x80U;
        } else if (lead >= 0xE0U && lead <= 0xEFU) {
            length = 3U;
            codePoint = lead & 0x0FU;
            minimum = 0x800U;
        } else if (lead >= 0xF0U && lead <= 0xF4U) {
            length = 4U;
            codePoint = lead & 0x07U;
            minimum = 0x10000U;
        } else {
            return false;
        }
        if (index + length > InText.size()) {
            return false;
        }
        for (std::size_t offset = 1U; offset < length; ++offset) {
            const auto continuation =
                static_cast<unsigned char>(InText[index + offset]);
            if ((continuation & 0xC0U) != 0x80U) {
                return false;
            }
            codePoint = (codePoint << 6U) | (continuation & 0x3FU);
        }
        if (codePoint < minimum || codePoint > 0x10FFFFU ||
            (codePoint >= 0xD800U && codePoint <= 0xDFFFU)) {
            return false;
        }
        index += length;
    }
    return true;
}

auto RequireSafeDisplay(const std::string_view InText,
                        const int InMaximumCells) -> void {
    REQUIRE(IsValidUtf8(InText));
    REQUIRE(TuiDisplayWidth(InText) <= InMaximumCells);
    for (const unsigned char byte : InText) {
        const bool isPrintable =
            (byte >= 0x20U && byte != 0x7FU) || byte >= 0x80U;
        REQUIRE(isPrintable);
    }
}

#if defined(KOG_FTXUI_WINDOWS_CONPTY_COMBINING_MACRON_FALLBACK)
constexpr int kCombiningMacronWidth = 2;
constexpr const char* kCombiningEndExpected = "a...";
constexpr const char* kCombiningPathExpected = "path: a\xCC\x84/...";
#else
constexpr int kCombiningMacronWidth = 1;
constexpr const char* kCombiningEndExpected = "a\xCC\x84...";
constexpr const char* kCombiningPathExpected = "path: a\xCC\x84/r...";
#endif

} // namespace

TEST_CASE("FTXUI serialization preserves empty screen cells",
          "[unit][tui_display_cells][KOG-BUG-0109]") {
    // Given: a fixed-width screen whose trailing cells were never painted.
    auto screen = ftxui::Screen::Create(
        ftxui::Dimension::Fixed(4), ftxui::Dimension::Fixed(1));
    screen.PixelAt(0, 0).character = "x";
    screen.PixelAt(1, 0).character.clear();

    // Then: serialization retains all terminal cells instead of shifting rows.
    REQUIRE(screen.ToString() == "x   ");
}

TEST_CASE("TUI display cells use FTXUI widths and sanitize unsafe bytes",
          "[unit][tui_display_cells][KOG-BUG-0109]") {
    // Given: ASCII, CJK, combining, emoji, malformed UTF-8, and controls.
    const std::array cases{
        std::pair{std::string("alpha"), 5},
        std::pair{std::string("\xE6\xB8\xAC\xE8\xA9\xA6"), 4},
        std::pair{std::string("a\xCC\x84"), kCombiningMacronWidth},
        std::pair{std::string("\xF0\x9F\xAA\x90"), 2},
    };

    // When: each string is measured through the display helper.
    for (const auto& [value, expectedWidth] : cases) {
        // Then: the helper agrees with FTXUI's glyph/cell model.
        CAPTURE(value, expectedWidth);
        REQUIRE(TuiDisplayWidth(value) == expectedWidth);
        REQUIRE(TuiDisplayWidth(value) == ftxui::string_width(value));
    }

    const std::string malformed{"bad\xF0\x28\x8C\x28", 7U};
    const std::string controls =
        std::string("a") + '\x1b' + "[31m" + '\t' + "b" + '\n';

    // When: unsafe data crosses the display-only boundary.
    const auto sanitizedMalformed = TuiDisplaySanitize(malformed);
    const auto sanitizedControls = TuiDisplaySanitize(controls);

    // Then: it becomes printable valid UTF-8 without retaining terminal controls.
    REQUIRE(sanitizedMalformed == "bad\\xF0(\\x8C(");
    REQUIRE(sanitizedControls == "a\\x1B[31m\\tb\\n");
    RequireSafeDisplay(sanitizedMalformed, 32);
    RequireSafeDisplay(sanitizedControls, 32);
}

TEST_CASE("TUI display cells truncate and pad without splitting FTXUI glyphs",
           "[unit][tui_display_cells][KOG-BUG-0109]") {
    const std::string combining = std::string{"a\xCC\x84"} + "bcde";
    const std::string cjk = "ab\xE6\xB8\xAC\xE8\xA9\xA6";
    const std::string emoji = std::string{"a\xF0\x9F\xAA\x90"} + "bc";

    // Given: a smaller display-cell budget than each source string.
    // When: each form is bounded.
    const auto end = TuiDisplayTruncateEnd(combining, 4);
    const auto front = TuiDisplayTruncateFront(cjk, 5);
    const auto emojiEnd = TuiDisplayTruncateEnd(emoji, 4);
    const auto middle = TuiDisplayTruncateMiddle("run-0123456789", 10, "..");

    // Then: output is contiguous glyph-safe text within the requested cells.
    REQUIRE(end == kCombiningEndExpected);
    REQUIRE(front == "...\xE8\xA9\xA6");
    REQUIRE(emojiEnd == "a...");
    REQUIRE(middle == "run-0..789");
    RequireSafeDisplay(end, 4);
    RequireSafeDisplay(front, 5);
    RequireSafeDisplay(emojiEnd, 4);
    RequireSafeDisplay(middle, 10);

    REQUIRE(TuiDisplayEllipsis(0).empty());
    REQUIRE(TuiDisplayEllipsis(1) == ".");
    REQUIRE(TuiDisplayEllipsis(2) == "..");
    REQUIRE(TuiDisplayEllipsis(3) == "...");
    REQUIRE(TuiDisplayPadRight("\xE6\xB8\xAC", 4) == "\xE6\xB8\xAC  ");
    REQUIRE(TuiDisplayPadLeft("\xE6\xB8\xAC", 4) == "  \xE6\xB8\xAC");

    const std::string composite =
        std::string{"\xE6\xB8\xAC\xE8\xA9\xA6-a\xCC\x84-"} +
        "\xF0\x9F\xAA\x90";
    const auto padded = TuiDisplayPadRight(composite, 20);
    REQUIRE(padded.starts_with(composite));
    REQUIRE(TuiDisplayWidth(padded) == 20);
}

TEST_CASE("TUI repository display lines preserve prefixes and bound paths",
          "[unit][tui_display_cells][KOG-BUG-0109]") {
    constexpr int kBudget = 12;
    const std::array cases{
        std::pair{std::string{"C:/repository/path"}, std::string{"path: C:/..."}},
        std::pair{std::string{"\xE6\xB8\xAC\xE8\xA9\xA6/repository/path"}, std::string{"path: \xE6\xB8\xAC..."}},
        std::pair{std::string{"a\xCC\x84/repository/path"},
                  std::string{kCombiningPathExpected}},
        std::pair{std::string{"\xF0\x9F\xAA\x90/repository/path"}, std::string{"path: \xF0\x9F\xAA\x90/..."}},
    };

    for (const auto& [path, expected] : cases) {
        const auto output = TuiDisplayTruncateEndWithPrefix("path: ", path, kBudget);
        CAPTURE(path, output);
        REQUIRE(TuiDisplayWidth(output) <= kBudget);
        REQUIRE(output.ends_with("..."));
        REQUIRE(IsValidUtf8(output));
        REQUIRE(output == expected);
    }

    const auto shortOutput = TuiDisplayTruncateEndWithPrefix("path: ", "repo", kBudget);
    REQUIRE(shortOutput == "path: repo");
    REQUIRE_FALSE(shortOutput.ends_with("..."));
}

TEST_CASE("TUI display truncation retains zero-width continuation glyphs",
           "[unit][tui_display_cells][KOG-BUG-0109]") {
    // Given: FTXUI separates a rendered base glyph from combining marks and
    // emoji modifiers. A display-cell truncation boundary must retain each
    // continuation with the visible glyph it modifies.
    const std::string cjkCombining = std::string{"a\xE6\xB8\xAC\xCC\x81"} + "bcde";
    const std::string trailingCjkCombining =
        std::string{"abcd\xE6\xB8\xAC\xCC\x81"};
    const std::string emojiModifier =
        std::string{"\xF0\x9F\x91\x8D\xF0\x9F\x8F\xBD"} + "abcd";

    // When: end, front, and middle truncation end on the visible base glyph.
    const auto end = TuiDisplayTruncateEnd(cjkCombining, 6);
    const auto front = TuiDisplayTruncateFront(trailingCjkCombining, 5);
    const auto middle = TuiDisplayTruncateMiddle(cjkCombining, 6, "..");
    const auto emojiEnd = TuiDisplayTruncateEnd(emojiModifier, 5);

    // Then: the trailing combining mark/modifier is never silently discarded.
    REQUIRE(end == std::string{"a\xE6\xB8\xAC\xCC\x81"} + "...");
    REQUIRE(front == std::string{"...\xE6\xB8\xAC\xCC\x81"});
    REQUIRE(middle == std::string{"a\xE6\xB8\xAC\xCC\x81"} + "..e");
    REQUIRE(emojiEnd ==
        std::string{"\xF0\x9F\x91\x8D\xF0\x9F\x8F\xBD"} + "...");
    RequireSafeDisplay(end, 6);
    RequireSafeDisplay(front, 5);
    RequireSafeDisplay(middle, 6);
    RequireSafeDisplay(emojiEnd, 5);
}

TEST_CASE("TUI dashboard pane geometry collapses and restores at required sizes",
          "[unit][tui_display_cells][KOG-BUG-0109]") {
    // When: live terminal geometry moves through every required size and restores.
    const auto wide = ComputeTuiAuditDashboardGeometry(120, 36, false, true);
    const auto compact = ComputeTuiAuditDashboardGeometry(72, 22, false, true);
    const auto collapsed = ComputeTuiAuditDashboardGeometry(40, 12, false, true);
    const auto minimum = ComputeTuiAuditDashboardGeometry(24, 12, false, true);
    const auto restored = ComputeTuiAuditDashboardGeometry(120, 36, false, true);

    // Then: the layout collapses only where required and restores exactly.
    REQUIRE_FALSE(wide.repositoryPaneCollapsed);
    REQUIRE_FALSE(compact.repositoryPaneCollapsed);
    REQUIRE(collapsed.repositoryPaneCollapsed);
    REQUIRE(minimum.repositoryPaneCollapsed);
    REQUIRE(restored == wide);
    REQUIRE(collapsed.frame.height == 10);
    REQUIRE(minimum.frame.width == 22);
    REQUIRE(minimum.frame.height == 10);
    REQUIRE(minimum.rightPanelContentHeight == 0);
}
