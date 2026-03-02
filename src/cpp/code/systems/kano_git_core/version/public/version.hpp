#pragma once
#include <string>
#include <string_view>

namespace kano::git {

constexpr std::string_view GetBuildVersion() {
#ifdef KOG_BUILD_VERSION
    return KOG_BUILD_VERSION;
#elif defined(KOG_VERSION)
    return KOG_VERSION;
#else
    return "unknown";
#endif
}

constexpr std::string_view GetBuildVCS() {
#ifdef KOG_BUILD_VCS
    return KOG_BUILD_VCS;
#else
    return "unknown";
#endif
}

constexpr std::string_view GetBuildBranch() {
#ifdef KOG_BUILD_BRANCH
    return KOG_BUILD_BRANCH;
#else
    return "unknown";
#endif
}

constexpr std::string_view GetBuildRevision() {
#ifdef KOG_BUILD_REVISION
    return KOG_BUILD_REVISION;
#else
    return "unknown";
#endif
}

constexpr std::string_view GetBuildRevisionHashShort() {
#ifdef KOG_BUILD_REVISION_HASH_SHORT
    return KOG_BUILD_REVISION_HASH_SHORT;
#else
    return "unknown";
#endif
}

constexpr std::string_view GetBuildRevisionHash() {
#ifdef KOG_BUILD_REVISION_HASH
    return KOG_BUILD_REVISION_HASH;
#else
    return "unknown";
#endif
}

constexpr std::string_view GetBuildDirty() {
#ifdef KOG_BUILD_DIRTY
    return KOG_BUILD_DIRTY;
#else
    return "unknown";
#endif
}

constexpr std::string_view BuildHostName() {
#ifdef KOG_BUILD_HOST_NAME
    return KOG_BUILD_HOST_NAME;
#else
    return "unknown";
#endif
}

constexpr std::string_view GetBuildCI() {
#ifdef KOG_BUILD_CI
    return KOG_BUILD_CI;
#else
    return "false";
#endif
}

constexpr std::string_view GetBuildPipelineId() {
#ifdef KOG_BUILD_PIPELINE_ID
    return KOG_BUILD_PIPELINE_ID;
#else
    return "unknown";
#endif
}

constexpr std::string_view GetBuildToolchain() {
#ifdef KOG_BUILD_TOOLCHAIN
    return KOG_BUILD_TOOLCHAIN;
#else
    return "unknown";
#endif
}

constexpr std::string_view BuildHostPlatform() {
#ifdef KOG_BUILD_PLATFORM
    return KOG_BUILD_PLATFORM;
#else
    return "unknown";
#endif
}

inline std::string GetBuildInfo() {
    std::string out;
    out.reserve(256);
    out += "version=";
    out += GetBuildVersion();
    out += " vcs=";
    out += GetBuildVCS();
    out += " branch=";
    out += GetBuildBranch();
    out += " rev=";
    out += GetBuildRevision();
    out += " hash_short=";
    out += GetBuildRevisionHashShort();
    out += " hash=";
    out += GetBuildRevisionHash();
    out += " dirty=";
    out += GetBuildDirty();
    out += " host=";
    out += BuildHostName();
    out += " host_platform=";
    out += BuildHostPlatform();
    out += " toolchain=";
    out += GetBuildToolchain();
    out += " ci=";
    out += GetBuildCI();
    out += " pipeline=";
    out += GetBuildPipelineId();
    return out;
}

constexpr std::string_view GetVersion() {
    return GetBuildVersion();
}

}
