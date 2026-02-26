module;
#include <string_view>

export module kano.git.version;

export namespace kano::git {

constexpr std::string_view GetVersion() {
#ifdef KOG_VERSION
    return KOG_VERSION;
#else
    return "unknown";
#endif
}

}
