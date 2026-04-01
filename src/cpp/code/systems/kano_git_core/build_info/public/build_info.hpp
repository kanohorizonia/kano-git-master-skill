#pragma once

#include <string>
#include <string_view>

namespace kano::git {

namespace detail {

struct InfraBuildInfoSnapshot {
    std::string_view version;
    std::string_view vcs;
    std::string_view branch;
    std::string_view revision;
    std::string_view revisionHashShort;
    std::string_view revisionHash;
    std::string_view dirty;
    std::string_view host;
    std::string_view ci;
    std::string_view context;
    std::string_view pipeline;
    std::string_view toolchain;
    std::string_view generator;
    std::string_view preset;
    std::string_view configuration;
    std::string_view platform;
};

inline const InfraBuildInfoSnapshot& GetInfraBuildInfoSnapshot() {
    static const InfraBuildInfoSnapshot snapshot = [] {
        return InfraBuildInfoSnapshot{
#ifdef KOG_BUILD_VERSION
            KOG_BUILD_VERSION,
#elif defined(KOG_VERSION)
            KOG_VERSION,
#else
            "",
#endif
#ifdef KOG_BUILD_VCS
            KOG_BUILD_VCS,
#else
            "",
#endif
#ifdef KOG_BUILD_BRANCH
            KOG_BUILD_BRANCH,
#else
            "",
#endif
#ifdef KOG_BUILD_REVISION
            KOG_BUILD_REVISION,
#else
            "",
#endif
#ifdef KOG_BUILD_REVISION_HASH_SHORT
            KOG_BUILD_REVISION_HASH_SHORT,
#else
            "",
#endif
#ifdef KOG_BUILD_REVISION_HASH
            KOG_BUILD_REVISION_HASH,
#else
            "",
#endif
#ifdef KOG_BUILD_DIRTY
            KOG_BUILD_DIRTY,
#else
            "",
#endif
#ifdef KOG_BUILD_HOST_NAME
            KOG_BUILD_HOST_NAME,
#else
            "",
#endif
#ifdef KOG_BUILD_CI
            KOG_BUILD_CI,
#else
            "",
#endif
#ifdef KOG_BUILD_CONTEXT
            KOG_BUILD_CONTEXT,
#else
            "",
#endif
#ifdef KOG_BUILD_PIPELINE_ID
            KOG_BUILD_PIPELINE_ID,
#else
            "",
#endif
#ifdef KOG_BUILD_TOOLCHAIN
            KOG_BUILD_TOOLCHAIN,
#else
            "",
#endif
#ifdef KOG_BUILD_GENERATOR
            KOG_BUILD_GENERATOR,
#else
            "",
#endif
#ifdef KOG_BUILD_PRESET
            KOG_BUILD_PRESET,
#else
            "",
#endif
#ifdef KOG_BUILD_CONFIGURATION
            KOG_BUILD_CONFIGURATION,
#else
            "",
#endif
#ifdef KOG_BUILD_PLATFORM
            KOG_BUILD_PLATFORM,
#else
            "",
#endif
        };
    }();

    return snapshot;
}

} // namespace detail

inline std::string_view GetBuildVersion() {
    return detail::GetInfraBuildInfoSnapshot().version;
}

inline std::string_view GetBuildVCS() {
    return detail::GetInfraBuildInfoSnapshot().vcs;
}

inline std::string_view GetBuildBranch() {
    return detail::GetInfraBuildInfoSnapshot().branch;
}

inline std::string_view GetBuildRevision() {
    return detail::GetInfraBuildInfoSnapshot().revision;
}

inline std::string_view GetBuildRevisionHashShort() {
    return detail::GetInfraBuildInfoSnapshot().revisionHashShort;
}

inline std::string_view GetBuildRevisionHash() {
    return detail::GetInfraBuildInfoSnapshot().revisionHash;
}

inline std::string_view GetBuildDirty() {
    return detail::GetInfraBuildInfoSnapshot().dirty;
}

inline std::string_view BuildHostName() {
    return detail::GetInfraBuildInfoSnapshot().host;
}

inline std::string_view GetBuildCI() {
    return detail::GetInfraBuildInfoSnapshot().ci;
}

inline std::string_view GetBuildContext() {
    return detail::GetInfraBuildInfoSnapshot().context;
}

inline std::string_view GetBuildPipelineId() {
    return detail::GetInfraBuildInfoSnapshot().pipeline;
}

inline std::string_view GetBuildToolchain() {
    return detail::GetInfraBuildInfoSnapshot().toolchain;
}

inline std::string_view GetBuildGenerator() {
    return detail::GetInfraBuildInfoSnapshot().generator;
}

inline std::string_view GetBuildPreset() {
    return detail::GetInfraBuildInfoSnapshot().preset;
}

inline std::string_view GetBuildConfiguration() {
    return detail::GetInfraBuildInfoSnapshot().configuration;
}

inline std::string_view BuildHostPlatform() {
    return detail::GetInfraBuildInfoSnapshot().platform;
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
    out += " generator=";
    out += GetBuildGenerator();
    out += " preset=";
    out += GetBuildPreset();
    out += " config=";
    out += GetBuildConfiguration();
    out += " ci=";
    out += GetBuildCI();
    out += " context=";
    out += GetBuildContext();
    out += " pipeline=";
    out += GetBuildPipelineId();
    return out;
}

inline std::string_view GetVersion() {
    return GetBuildVersion();
}

}
