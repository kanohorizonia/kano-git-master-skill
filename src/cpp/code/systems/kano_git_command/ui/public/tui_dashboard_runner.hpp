#pragma once

#include <CLI/CLI.hpp>

#include <string_view>

namespace kano::git::commands {

auto RunTuiDashboard(CLI::App& InApp, std::string_view InTheme = "auto") -> int;
auto PrintTuiDemoSummary() -> void;

} // namespace kano::git::commands
