#include <catch2/catch_test_macros.hpp>

#include "tui_theme.hpp"

#include <array>
#include <string_view>

namespace {

using kano::git::commands::ResolvedTuiTheme;
using kano::git::commands::TuiColor;
using kano::git::commands::TuiTextStyle;

void RequireTerminalDefaultColors(const TuiTextStyle& InStyle) {
    REQUIRE(InStyle.foreground == TuiColor::TerminalDefault);
    REQUIRE(InStyle.background == TuiColor::TerminalDefault);
}

auto PaletteStyles(const ResolvedTuiTheme& InTheme) -> std::array<const TuiTextStyle*, 12> {
    return {
        &InTheme.palette.primary,
        &InTheme.palette.title,
        &InTheme.palette.sectionTitle,
        &InTheme.palette.info,
        &InTheme.palette.secondary,
        &InTheme.palette.muted,
        &InTheme.palette.success,
        &InTheme.palette.warning,
        &InTheme.palette.error,
        &InTheme.palette.running,
        &InTheme.palette.selected,
        &InTheme.palette.highlight,
    };
}

}

TEST_CASE("TUI theme parser accepts only supported explicit overrides",
          "[tdd][unit][bug:tui-theme][KG-BUG-0088]") {
    using namespace kano::git::commands;

    REQUIRE(ParseTuiThemeMode("auto") == TuiThemeMode::Auto);
    REQUIRE(ParseTuiThemeMode(" DARK ") == TuiThemeMode::Dark);
    REQUIRE(ParseTuiThemeMode("Light") == TuiThemeMode::Light);
    REQUIRE(ParseTuiThemeMode("MONO") == TuiThemeMode::Mono);
    REQUIRE_FALSE(ParseTuiThemeMode("").has_value());
    REQUIRE_FALSE(ParseTuiThemeMode("system").has_value());

    REQUIRE(std::string(TuiThemeModeName(TuiThemeMode::Auto)) == "auto");
    REQUIRE(std::string(TuiThemeModeName(TuiThemeMode::Dark)) == "dark");
    REQUIRE(std::string(TuiThemeModeName(TuiThemeMode::Light)) == "light");
    REQUIRE(std::string(TuiThemeModeName(TuiThemeMode::Mono)) == "mono");
}

TEST_CASE("TUI theme parses the COLORFGBG background token as an optional hint",
          "[tdd][unit][bug:tui-theme][KG-BUG-0088]") {
    using namespace kano::git::commands;

    REQUIRE(ParseColorFgBgHint("15;0") == TuiBackgroundHint::Dark);
    REQUIRE(ParseColorFgBgHint(" 0 ; 15 ") == TuiBackgroundHint::Light);
    REQUIRE(ParseColorFgBgHint("232") == TuiBackgroundHint::Dark);
    REQUIRE(ParseColorFgBgHint("0;255") == TuiBackgroundHint::Light);
    REQUIRE(ParseColorFgBgHint("") == TuiBackgroundHint::Unknown);
    REQUIRE(ParseColorFgBgHint("15;") == TuiBackgroundHint::Unknown);
    REQUIRE(ParseColorFgBgHint("15;unknown") == TuiBackgroundHint::Unknown);
    REQUIRE(ParseColorFgBgHint("0;256") == TuiBackgroundHint::Unknown);
}

TEST_CASE("TUI explicit dark and light themes win over no-color and COLORFGBG hints",
          "[tdd][unit][bug:tui-theme][KG-BUG-0088]") {
    using namespace kano::git::commands;

    TuiThemeEnvironment environment;
    environment.colorFgBg = "0;15";
    environment.noColor = true;
    environment.kogNoColor = true;

    const auto dark = ResolveTuiTheme(TuiThemeMode::Dark, environment);
    REQUIRE(dark.requestedMode == TuiThemeMode::Dark);
    REQUIRE(dark.effectiveMode == TuiThemeMode::Dark);
    REQUIRE(dark.backgroundHint == TuiBackgroundHint::Light);
    REQUIRE(dark.usesAnsiColors);
    REQUIRE(dark.palette.info.foreground == TuiColor::BrightCyan);

    environment.colorFgBg = "15;0";
    const auto light = ResolveTuiTheme(TuiThemeMode::Light, environment);
    REQUIRE(light.requestedMode == TuiThemeMode::Light);
    REQUIRE(light.effectiveMode == TuiThemeMode::Light);
    REQUIRE(light.backgroundHint == TuiBackgroundHint::Dark);
    REQUIRE(light.usesAnsiColors);
    REQUIRE(light.palette.info.foreground == TuiColor::Blue);
}

TEST_CASE("TUI auto theme honors no-color before terminal background hints",
          "[tdd][unit][bug:tui-theme][KG-BUG-0088]") {
    using namespace kano::git::commands;

    TuiThemeEnvironment environment;
    environment.colorFgBg = "15;0";
    environment.noColor = true;

    const auto noColor = ResolveTuiTheme(TuiThemeMode::Auto, environment);
    REQUIRE(noColor.effectiveMode == TuiThemeMode::Mono);
    REQUIRE_FALSE(noColor.usesAnsiColors);

    environment.noColor = false;
    environment.kogNoColor = true;
    const auto kogNoColor = ResolveTuiTheme(TuiThemeMode::Auto, environment);
    REQUIRE(kogNoColor.effectiveMode == TuiThemeMode::Mono);
    REQUIRE_FALSE(kogNoColor.usesAnsiColors);
}

TEST_CASE("TUI auto theme uses COLORFGBG when available and stays adaptive when unknown",
          "[tdd][unit][bug:tui-theme][KG-BUG-0088]") {
    using namespace kano::git::commands;

    TuiThemeEnvironment environment;
    environment.colorFgBg = "15;0";
    const auto dark = ResolveTuiTheme(TuiThemeMode::Auto, environment);
    REQUIRE(dark.effectiveMode == TuiThemeMode::Dark);

    environment.colorFgBg = "0;15";
    const auto light = ResolveTuiTheme(TuiThemeMode::Auto, environment);
    REQUIRE(light.effectiveMode == TuiThemeMode::Light);

    environment.colorFgBg = "unknown";
    const auto adaptive = ResolveTuiTheme(TuiThemeMode::Auto, environment);
    REQUIRE(adaptive.effectiveMode == TuiThemeMode::Auto);
    REQUIRE(adaptive.backgroundHint == TuiBackgroundHint::Unknown);
    REQUIRE(adaptive.usesAnsiColors);
    RequireTerminalDefaultColors(adaptive.palette.primary);
    RequireTerminalDefaultColors(adaptive.palette.secondary);
    REQUIRE(adaptive.palette.success.foreground == TuiColor::Green);
    REQUIRE(adaptive.palette.warning.foreground == TuiColor::Yellow);
    REQUIRE(adaptive.palette.error.foreground == TuiColor::Red);
}

TEST_CASE("TUI selected rows use terminal-default inversion in every theme",
          "[tdd][unit][bug:tui-theme][KG-BUG-0088]") {
    using namespace kano::git::commands;

    const std::array modes{
        TuiThemeMode::Auto,
        TuiThemeMode::Dark,
        TuiThemeMode::Light,
        TuiThemeMode::Mono,
    };
    TuiThemeEnvironment environment;

    for (const auto mode : modes) {
        CAPTURE(std::string(TuiThemeModeName(mode)));
        const auto theme = ResolveTuiTheme(mode, environment);
        RequireTerminalDefaultColors(theme.palette.selected);
        REQUIRE(theme.palette.selected.bold);
        REQUIRE(theme.palette.selected.inverted);
    }
}

TEST_CASE("TUI mono theme removes ANSI colors while retaining text emphasis",
          "[tdd][unit][bug:tui-theme][KG-BUG-0088]") {
    using namespace kano::git::commands;

    TuiThemeEnvironment environment;
    environment.colorFgBg = "15;0";
    const auto theme = ResolveTuiTheme(TuiThemeMode::Mono, environment);

    REQUIRE(theme.effectiveMode == TuiThemeMode::Mono);
    REQUIRE_FALSE(theme.usesAnsiColors);
    for (const auto* style : PaletteStyles(theme)) {
        RequireTerminalDefaultColors(*style);
    }
    REQUIRE(theme.palette.title.bold);
    REQUIRE(theme.palette.muted.dim);
    REQUIRE(theme.palette.error.bold);
    REQUIRE(theme.palette.selected.inverted);
}
