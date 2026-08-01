#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace kano::git::commands {

enum class TuiThemeMode {
    Auto,
    Dark,
    Light,
    Mono,
};

enum class TuiBackgroundHint {
    Unknown,
    Dark,
    Light,
};

// Terminal-palette colors only. The runner maps these tokens to FTXUI's
// corresponding named ANSI colors; no RGB values are embedded in the theme.
enum class TuiColor {
    TerminalDefault,
    Black,
    Red,
    Green,
    Yellow,
    Blue,
    Magenta,
    Cyan,
    White,
    BrightBlack,
    BrightRed,
    BrightGreen,
    BrightYellow,
    BrightBlue,
    BrightMagenta,
    BrightCyan,
    BrightWhite,
};

struct TuiTextStyle {
    TuiColor foreground = TuiColor::TerminalDefault;
    TuiColor background = TuiColor::TerminalDefault;
    bool bold = false;
    bool dim = false;
    bool inverted = false;
};

struct TuiThemePalette {
    TuiTextStyle primary;
    TuiTextStyle title;
    TuiTextStyle sectionTitle;
    TuiTextStyle info;
    TuiTextStyle secondary;
    TuiTextStyle muted;
    TuiTextStyle success;
    TuiTextStyle warning;
    TuiTextStyle error;
    TuiTextStyle running;
    TuiTextStyle selected;
    TuiTextStyle highlight;
};

struct TuiThemeEnvironment {
    std::string colorFgBg;
    bool noColor = false;
    bool kogNoColor = false;
};

struct ResolvedTuiTheme {
    TuiThemeMode requestedMode = TuiThemeMode::Auto;
    TuiThemeMode effectiveMode = TuiThemeMode::Auto;
    TuiBackgroundHint backgroundHint = TuiBackgroundHint::Unknown;
    bool usesAnsiColors = true;
    TuiThemePalette palette;
};

[[nodiscard]] auto ParseTuiThemeMode(std::string_view InValue) -> std::optional<TuiThemeMode>;
[[nodiscard]] auto TuiThemeModeName(TuiThemeMode InMode) -> std::string_view;
[[nodiscard]] auto ParseColorFgBgHint(std::string_view InValue) -> TuiBackgroundHint;

// Explicit Dark, Light, and Mono requests are authoritative. NO_COLOR and
// KOG_NO_COLOR force Mono only while the requested mode is Auto.
[[nodiscard]] auto ResolveTuiTheme(TuiThemeMode InRequestedMode,
                                   const TuiThemeEnvironment& InEnvironment) -> ResolvedTuiTheme;
[[nodiscard]] auto ResolveTuiThemeFromEnvironment(TuiThemeMode InRequestedMode) -> ResolvedTuiTheme;

}
