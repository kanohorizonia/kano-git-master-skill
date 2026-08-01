#pragma once

#include <span>
#include <string_view>

namespace kano::git::commands {

enum class TuiKeyContext {
    Normal,
    History,
    Detail,
};

struct TuiKeyGuidance {
    TuiKeyContext context;
    std::string_view label;
    std::string_view controls;
};

[[nodiscard]] auto GetTuiKeyGuidance(TuiKeyContext InContext) noexcept
    -> const TuiKeyGuidance&;

[[nodiscard]] auto GetAllTuiKeyGuidance() noexcept
    -> std::span<const TuiKeyGuidance>;

} // namespace kano::git::commands
