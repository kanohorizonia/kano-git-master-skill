#pragma once

#include <CLI/CLI.hpp>

namespace kano::git::commands {

auto RunTuiDashboard(CLI::App& InApp) -> int;
auto PrintTuiDemoSummary() -> void;

} // namespace kano::git::commands
