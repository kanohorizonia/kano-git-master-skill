#include "tui_keymap.hpp"

#include <array>

namespace kano::git::commands {
namespace {

constexpr std::array<TuiKeyGuidance, 7> kGuidance{{
    {
        TuiKeyContext::Normal,
        "normal",
        "Up/Down or j/k select | Enter history | r refresh selected repo | : audit | ? help | q quit",
        "? help | q quit",
    },
    {
        TuiKeyContext::History,
        "history",
        "Up/Down or j/k select | Left/Right page | / search | n next | o page order | Enter detail | [ previous repo | ] next repo | ? help | Esc/q back",
        "Enter detail | Esc/q back",
    },
    {
        TuiKeyContext::Detail,
        "detail",
        "Up/Down or j/k change | Left/Right page | m summary/patch | ? help | Esc/q back",
        "? help | Esc/q back",
    },
    { TuiKeyContext::Discover, "discover", "[ or PgDown prev page | ] or PgUp next page | Esc/q close", "[ prev | ] next | Esc/q close" },
    { TuiKeyContext::Preview, "preview", "Esc/q close", "Esc/q close" },
    { TuiKeyContext::Command,
      "command",
      "g scope | Tab complete | Up/Down candidates | Enter inspect | Esc cancel",
      "Enter inspect | Esc cancel" },
    { TuiKeyContext::Palette,
      "palette",
      "Up/Down select | Enter inspect | Esc close",
      "Enter inspect | Esc close" },
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
        case TuiKeyContext::Discover:
            return kGuidance[3];
        case TuiKeyContext::Preview:
            return kGuidance[4];
        case TuiKeyContext::Command:
            return kGuidance[5];
        case TuiKeyContext::Palette:
            return kGuidance[6];
    }
    return kGuidance[0];
}

auto GetAllTuiKeyGuidance() noexcept
    -> std::span<const TuiKeyGuidance> {
    return kGuidance;
}

} // namespace kano::git::commands
