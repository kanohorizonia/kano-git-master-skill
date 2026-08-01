#include "tui_theme.hpp"

#include <array>
#include <charconv>
#include <cctype>
#include <cstdlib>
#include <string>

namespace kano::git::commands {
namespace {

auto Trim(std::string_view InValue) -> std::string_view {
    while (!InValue.empty() && std::isspace(static_cast<unsigned char>(InValue.front())) != 0) {
        InValue.remove_prefix(1);
    }
    while (!InValue.empty() && std::isspace(static_cast<unsigned char>(InValue.back())) != 0) {
        InValue.remove_suffix(1);
    }
    return InValue;
}

auto Lower(std::string_view InValue) -> std::string {
    std::string value(Trim(InValue));
    for (auto& ch : value) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return value;
}

auto Style(const TuiColor InForeground = TuiColor::TerminalDefault,
           const bool InBold = false,
           const bool InDim = false,
           const bool InInverted = false) -> TuiTextStyle {
    TuiTextStyle style;
    style.foreground = InForeground;
    style.bold = InBold;
    style.dim = InDim;
    style.inverted = InInverted;
    return style;
}

auto BuildAutoPalette() -> TuiThemePalette {
    TuiThemePalette palette;
    palette.primary = Style();
    palette.title = Style(TuiColor::TerminalDefault, true);
    palette.sectionTitle = Style(TuiColor::Yellow, true);
    palette.info = Style(TuiColor::Cyan);
    palette.secondary = Style();
    palette.muted = Style(TuiColor::TerminalDefault, false, true);
    palette.success = Style(TuiColor::Green);
    palette.warning = Style(TuiColor::Yellow);
    palette.error = Style(TuiColor::Red);
    palette.running = Style(TuiColor::Cyan);
    palette.selected = Style(TuiColor::TerminalDefault, true, false, true);
    palette.highlight = Style(TuiColor::Cyan, true);
    return palette;
}

auto BuildDarkPalette() -> TuiThemePalette {
    TuiThemePalette palette;
    palette.primary = Style();
    palette.title = Style(TuiColor::BrightWhite, true);
    palette.sectionTitle = Style(TuiColor::BrightYellow, true);
    palette.info = Style(TuiColor::BrightCyan);
    palette.secondary = Style();
    palette.muted = Style(TuiColor::TerminalDefault, false, true);
    palette.success = Style(TuiColor::BrightGreen);
    palette.warning = Style(TuiColor::BrightYellow);
    palette.error = Style(TuiColor::BrightRed);
    palette.running = Style(TuiColor::BrightCyan);
    palette.selected = Style(TuiColor::TerminalDefault, true, false, true);
    palette.highlight = Style(TuiColor::BrightCyan, true);
    return palette;
}

auto BuildLightPalette() -> TuiThemePalette {
    TuiThemePalette palette;
    palette.primary = Style();
    palette.title = Style(TuiColor::Black, true);
    palette.sectionTitle = Style(TuiColor::Magenta, true);
    palette.info = Style(TuiColor::Blue);
    palette.secondary = Style(TuiColor::Black);
    palette.muted = Style(TuiColor::Black, false, true);
    palette.success = Style(TuiColor::Green);
    palette.warning = Style(TuiColor::Magenta);
    palette.error = Style(TuiColor::Red);
    palette.running = Style(TuiColor::Blue);
    palette.selected = Style(TuiColor::TerminalDefault, true, false, true);
    palette.highlight = Style(TuiColor::Blue, true);
    return palette;
}

auto BuildMonoPalette() -> TuiThemePalette {
    TuiThemePalette palette;
    palette.primary = Style();
    palette.title = Style(TuiColor::TerminalDefault, true);
    palette.sectionTitle = Style(TuiColor::TerminalDefault, true);
    palette.info = Style();
    palette.secondary = Style();
    palette.muted = Style(TuiColor::TerminalDefault, false, true);
    palette.success = Style(TuiColor::TerminalDefault, true);
    palette.warning = Style(TuiColor::TerminalDefault, true);
    palette.error = Style(TuiColor::TerminalDefault, true);
    palette.running = Style();
    palette.selected = Style(TuiColor::TerminalDefault, true, false, true);
    palette.highlight = Style(TuiColor::TerminalDefault, true);
    return palette;
}

struct Rgb {
    int red = 0;
    int green = 0;
    int blue = 0;
};

auto XtermColor(const int InIndex) -> std::optional<Rgb> {
    static constexpr std::array<Rgb, 16> kAnsiColors{{
        {0, 0, 0},
        {128, 0, 0},
        {0, 128, 0},
        {128, 128, 0},
        {0, 0, 128},
        {128, 0, 128},
        {0, 128, 128},
        {192, 192, 192},
        {128, 128, 128},
        {255, 0, 0},
        {0, 255, 0},
        {255, 255, 0},
        {0, 0, 255},
        {255, 0, 255},
        {0, 255, 255},
        {255, 255, 255},
    }};
    static constexpr std::array<int, 6> kCubeLevels{{0, 95, 135, 175, 215, 255}};

    if (InIndex < 0 || InIndex > 255) {
        return std::nullopt;
    }
    if (InIndex < static_cast<int>(kAnsiColors.size())) {
        return kAnsiColors[static_cast<std::size_t>(InIndex)];
    }
    if (InIndex <= 231) {
        const int cubeIndex = InIndex - 16;
        return Rgb{
            kCubeLevels[static_cast<std::size_t>(cubeIndex / 36)],
            kCubeLevels[static_cast<std::size_t>((cubeIndex / 6) % 6)],
            kCubeLevels[static_cast<std::size_t>(cubeIndex % 6)],
        };
    }

    const int gray = 8 + ((InIndex - 232) * 10);
    return Rgb{gray, gray, gray};
}

auto ClassifyBackground(const Rgb& InRgb) -> TuiBackgroundHint {
    // Integer approximation of perceived luminance. The threshold keeps the
    // xterm mid-gray slot on the dark side while classifying silver as light.
    const int luminance = ((299 * InRgb.red) + (587 * InRgb.green) + (114 * InRgb.blue)) / 1000;
    return luminance >= 140 ? TuiBackgroundHint::Light : TuiBackgroundHint::Dark;
}

auto EnvHasValue(const char* InValue) -> bool {
    return InValue != nullptr && *InValue != '\0';
}

}

auto ParseTuiThemeMode(const std::string_view InValue) -> std::optional<TuiThemeMode> {
    const auto value = Lower(InValue);
    if (value == "auto") {
        return TuiThemeMode::Auto;
    }
    if (value == "dark") {
        return TuiThemeMode::Dark;
    }
    if (value == "light") {
        return TuiThemeMode::Light;
    }
    if (value == "mono") {
        return TuiThemeMode::Mono;
    }
    return std::nullopt;
}

auto TuiThemeModeName(const TuiThemeMode InMode) -> std::string_view {
    switch (InMode) {
        case TuiThemeMode::Auto:
            return "auto";
        case TuiThemeMode::Dark:
            return "dark";
        case TuiThemeMode::Light:
            return "light";
        case TuiThemeMode::Mono:
            return "mono";
    }
    return "auto";
}

auto ParseColorFgBgHint(const std::string_view InValue) -> TuiBackgroundHint {
    auto background = Trim(InValue);
    if (const auto separator = background.rfind(';'); separator != std::string_view::npos) {
        background = background.substr(separator + 1);
    }
    background = Trim(background);
    if (background.empty()) {
        return TuiBackgroundHint::Unknown;
    }

    int index = -1;
    const auto* begin = background.data();
    const auto* end = begin + background.size();
    const auto result = std::from_chars(begin, end, index);
    if (result.ec != std::errc{} || result.ptr != end) {
        return TuiBackgroundHint::Unknown;
    }

    const auto color = XtermColor(index);
    return color.has_value() ? ClassifyBackground(*color) : TuiBackgroundHint::Unknown;
}

auto ResolveTuiTheme(const TuiThemeMode InRequestedMode,
                     const TuiThemeEnvironment& InEnvironment) -> ResolvedTuiTheme {
    ResolvedTuiTheme theme;
    theme.requestedMode = InRequestedMode;
    theme.backgroundHint = ParseColorFgBgHint(InEnvironment.colorFgBg);
    theme.effectiveMode = InRequestedMode;

    if (InRequestedMode == TuiThemeMode::Auto) {
        if (InEnvironment.noColor || InEnvironment.kogNoColor) {
            theme.effectiveMode = TuiThemeMode::Mono;
        } else if (theme.backgroundHint == TuiBackgroundHint::Dark) {
            theme.effectiveMode = TuiThemeMode::Dark;
        } else if (theme.backgroundHint == TuiBackgroundHint::Light) {
            theme.effectiveMode = TuiThemeMode::Light;
        }
    }

    switch (theme.effectiveMode) {
        case TuiThemeMode::Auto:
            theme.palette = BuildAutoPalette();
            theme.usesAnsiColors = true;
            break;
        case TuiThemeMode::Dark:
            theme.palette = BuildDarkPalette();
            theme.usesAnsiColors = true;
            break;
        case TuiThemeMode::Light:
            theme.palette = BuildLightPalette();
            theme.usesAnsiColors = true;
            break;
        case TuiThemeMode::Mono:
            theme.palette = BuildMonoPalette();
            theme.usesAnsiColors = false;
            break;
    }

    return theme;
}

auto ResolveTuiThemeFromEnvironment(const TuiThemeMode InRequestedMode) -> ResolvedTuiTheme {
    TuiThemeEnvironment environment;
    if (const char* colorFgBg = std::getenv("COLORFGBG"); colorFgBg != nullptr) {
        environment.colorFgBg = colorFgBg;
    }
    environment.noColor = EnvHasValue(std::getenv("NO_COLOR"));
    environment.kogNoColor = EnvHasValue(std::getenv("KOG_NO_COLOR"));
    return ResolveTuiTheme(InRequestedMode, environment);
}

}
