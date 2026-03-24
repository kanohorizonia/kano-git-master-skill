#include "version.hpp"

namespace kano::git {

auto VersionTranslationUnitAnchor() -> std::string_view {
    return GetVersion();
}

} // namespace kano::git
