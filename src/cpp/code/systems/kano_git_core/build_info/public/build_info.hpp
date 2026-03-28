#pragma once

#include <kano_build_info.hpp>

#include <string>
#include <string_view>

namespace kano::git {

namespace detail {

using InfraBuildInfoSnapshot = kano::infra::build_info::Snapshot;

inline const InfraBuildInfoSnapshot& GetInfraBuildInfoSnapshot() {
    static const InfraBuildInfoSnapshot snapshot = [] {
        return kano::infra::build_info::discover_snapshot({
#ifdef KOG_BUILD_VERSION
            .version = KOG_BUILD_VERSION,
#elif defined(KOG_VERSION)
            .version = KOG_VERSION,
#endif
#ifdef KOG_BUILD_VCS
            .vcs = KOG_BUILD_VCS,
#endif
#ifdef KOG_BUILD_BRANCH
            .branch = KOG_BUILD_BRANCH,
#endif
#ifdef KOG_BUILD_REVISION
            .revision = KOG_BUILD_REVISION,
#endif
#ifdef KOG_BUILD_REVISION_HASH_SHORT
            .revisionHashShort = KOG_BUILD_REVISION_HASH_SHORT,
#endif
#ifdef KOG_BUILD_REVISION_HASH
            .revisionHash = KOG_BUILD_REVISION_HASH,
#endif
#ifdef KOG_BUILD_DIRTY
            .dirty = KOG_BUILD_DIRTY,
#endif
#ifdef KOG_BUILD_HOST_NAME
            .host = KOG_BUILD_HOST_NAME,
#endif
#ifdef KOG_BUILD_CI
            .ci = KOG_BUILD_CI,
#endif
#ifdef KOG_BUILD_CONTEXT
            .context = KOG_BUILD_CONTEXT,
#endif
#ifdef KOG_BUILD_PIPELINE_ID
            .pipeline = KOG_BUILD_PIPELINE_ID,
#endif
#ifdef KOG_BUILD_TOOLCHAIN
            .toolchain = KOG_BUILD_TOOLCHAIN,
#endif
#ifdef KOG_BUILD_GENERATOR
            .generator = KOG_BUILD_GENERATOR,
#endif
#ifdef KOG_BUILD_PRESET
            .preset = KOG_BUILD_PRESET,
#endif
#ifdef KOG_BUILD_CONFIGURATION
            .configuration = KOG_BUILD_CONFIGURATION,
#endif
#ifdef KOG_BUILD_PLATFORM
            .platform = KOG_BUILD_PLATFORM,
#endif
        });
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
