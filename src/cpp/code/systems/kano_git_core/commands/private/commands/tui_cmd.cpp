// tui command — FTXUI-first interactive dashboard

#include "command_registry.hpp"

#include <ftxui/component/component.hpp>
#include <ftxui/component/component_options.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>

#include <iostream>
#include <string>
#include <vector>

namespace kano::git::commands {
namespace {

auto RunFtxuiDashboard() -> int {
    using namespace ftxui;

    std::vector<std::string> menu = {
        "Workspace status",
        "Workspace guide",
        "Workspace update (native)",
        "Quit",
    };
    int selected = 0;
    std::string footer = "Use arrows + Enter. Esc exits.";

    auto screen = ScreenInteractive::FitComponent();
    auto list = Menu(&menu, &selected);

    auto run = Button("Run", [&] {
        if (selected == 0) {
            footer = "Run: kano-git status";
        } else if (selected == 1) {
            footer = "Run: kano-git guide --flow workspace --checklist";
        } else if (selected == 2) {
            footer = "Run: kano-git workspace update --native --parallel 2";
        } else {
            screen.ExitLoopClosure()();
        }
    });

    auto container = Container::Vertical({list, run});
    auto renderer = Renderer(container, [&] {
        return vbox({
                   text("KOG FTXUI Dashboard") | bold,
                   separator(),
                   text("First full-TUI milestone for cross-repo workflows."),
                   separator(),
                   list->Render() | border,
                   separator(),
                   run->Render(),
                   separator(),
                   text(footer) | dim,
               }) |
               border;
    });

    screen.Loop(renderer);
    return 0;
}

auto PrintDemo() -> void {
    std::cout << "KOG TUI demo mode\n";
    std::cout << "- FTXUI dashboard enabled\n";
    std::cout << "- menu-driven workflow entry points\n";
    std::cout << "Run `kano-git tui` to start the FTXUI UI.\n";
}

} // namespace

void RegisterTui(CLI::App& InApp) {
    auto* cmd = InApp.add_subcommand("tui", "Launch interactive KOG terminal dashboard");

    auto* demo = new bool{false};
    cmd->add_flag("--demo", *demo, "Print demo summary and exit (non-interactive)");

    cmd->callback([demo]() {
        if (*demo) {
            PrintDemo();
            return;
        }
        std::exit(RunFtxuiDashboard());
    });
}

} // namespace kano::git::commands
