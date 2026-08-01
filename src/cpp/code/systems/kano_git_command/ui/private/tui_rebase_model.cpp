#include "tui_rebase_model.hpp"

#include <sstream>
#include <utility>

namespace kano::git::commands {

auto BuildRebasePlanPreview(
    const RebasePlannerState& InPlanner) -> std::string {
    std::ostringstream out;
    out << "Rebase Plan Preview\n";
    out << "repo: "
        << InPlanner.repo.lexically_normal().generic_string()
        << "\n";
    out << "base: " << InPlanner.baseRef << "\n\n";
    out << "# interactive todo style\n";
    for (const auto& item : InPlanner.items) {
        out << item.action << " " << item.sha << " "
            << item.title << "\n";
    }
    if (InPlanner.items.empty()) {
        out << "(no plan items)\n";
    }
    return out.str();
}

auto BuildRebasePlanner(
    const std::filesystem::path& InRepo,
    const RebasePreflightState& InPreflight)
    -> RebasePlannerState {
    RebasePlannerState planner;
    planner.active = true;
    planner.repo = InRepo;
    planner.baseRef =
        InPreflight.upstream != "(none)"
        ? InPreflight.upstream
        : (InPreflight.branch == "main" ? "main" : "master");
    planner.selectedIndex = 0;

    for (const auto& line : InPreflight.candidates) {
        const auto space = line.find(' ');
        if (space == std::string::npos) {
            continue;
        }
        RebasePlanItem item;
        item.sha = line.substr(0, space);
        item.title = line.substr(space + 1);
        item.action = "pick";
        planner.items.push_back(std::move(item));
    }

    planner.preview = BuildRebasePlanPreview(planner);
    return planner;
}

}  // namespace kano::git::commands
