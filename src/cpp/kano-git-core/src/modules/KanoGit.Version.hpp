#pragma once
#include <string_view>

namespace kano::git {

constexpr std::string_view GetVersion() {
#ifdef KOG_VERSION
    return KOG_VERSION;
#else
    return "unknown";
#endif
}

}
