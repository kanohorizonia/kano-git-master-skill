#pragma once

#include <string>
#include <string_view>

namespace kano::git::commands {

// Converts terminal-hostile or malformed input into printable valid UTF-8.
// This is deliberately a display-only projection; callers must retain source
// identity and protocol data separately.
[[nodiscard]] auto TuiDisplaySanitize(std::string_view InText) -> std::string;

// Measures the sanitized display projection with FTXUI's cell model.
[[nodiscard]] auto TuiDisplayWidth(std::string_view InText) -> int;

[[nodiscard]] auto TuiDisplayEllipsis(int InMaximumCells) -> std::string;
[[nodiscard]] auto TuiDisplayTruncateEnd(std::string_view InText,
                                          int InMaximumCells) -> std::string;
[[nodiscard]] auto TuiDisplayTruncateEndWithPrefix(
    std::string_view InPrefix,
    std::string_view InValue,
    int InMaximumCells) -> std::string;
[[nodiscard]] auto TuiDisplayTruncateFront(std::string_view InText,
                                            int InMaximumCells) -> std::string;
[[nodiscard]] auto TuiDisplayTruncateMiddle(std::string_view InText,
                                            int InMaximumCells,
                                            std::string_view InEllipsis = "...")
    -> std::string;
[[nodiscard]] auto TuiDisplayPadRight(std::string_view InText,
                                      int InWidth) -> std::string;
[[nodiscard]] auto TuiDisplayPadLeft(std::string_view InText,
                                     int InWidth) -> std::string;

} // namespace kano::git::commands
