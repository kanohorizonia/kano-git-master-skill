#include "build_info.hpp"

namespace kano::git {

auto BuildInfoTranslationUnitAnchor() -> std::string_view {
    return GetVersion();
}

} // namespace kano::git
