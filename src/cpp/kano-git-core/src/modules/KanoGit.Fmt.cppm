module;
#define FMT_HEADER_ONLY 1
#include <fmt/core.h>
#include <fmt/color.h>
#include <fmt/os.h>
export module KanoGit.Fmt;

export namespace fmt {
    using fmt::v10::format;
    using fmt::v10::print;
    // Exporting namespace contents so other modules can use `fmt::` without `#include`
}
