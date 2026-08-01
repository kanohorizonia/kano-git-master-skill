#include "tui_keymap.hpp"

#include <array>

namespace kano::git::commands {
namespace {

constexpr std::array<TuiKeyGuidance, 3> kGuidance{{
    {
        TuiKeyContext::Normal,
        "normal",
        "Up/Down or j/k select | Enter history | r refresh selected repo | : audit | ? help | q quit",
    },
    {
        TuiKeyContext::History,
        "history",
        "Up/Down or j/k select | Left/Right page | Enter detail | [ previous repo | ] next repo | ? help | Esc/q back",
    },
    {
        TuiKeyContext::Detail,
        "detail",
        "Up/Down or j/k change | Left/Right page | m summary/patch | ? help | Esc/q back",
    },
}};

} // namespace

auto GetTuiKeyGuidance(const TuiKeyContext InContext) noexcept
    -> const TuiKeyGuidance& {
    switch (InContext) {
        case TuiKeyContext::Normal:
            return kGuidance[0];
        case TuiKeyContext::History:
            return kGuidance[1];
        case TuiKeyContext::Detail:
            return kGuidance[2];
    }
    return kGuidance[0];
}

auto GetAllTuiKeyGuidance() noexcept
    -> std::span<const TuiKeyGuidance> {
    return kGuidance;
}

} // namespace kano::git::commands
