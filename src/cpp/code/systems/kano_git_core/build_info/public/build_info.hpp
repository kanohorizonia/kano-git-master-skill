#pragma once
#include <kano_build_info.h>
#include <string>
#include <string_view>

namespace kano::git {

namespace detail {

struct InfraBuildInfoSnapshot {
    std::string version;
    std::string vcs;
    std::string branch;
    std::string revision;
    std::string revision_hash_short;
    std::string revision_hash;
    std::string dirty;
    std::string host;
    std::string ci;
    std::string context;
    std::string pipeline;
    std::string toolchain;
    std::string generator;
    std::string preset;
    std::string configuration;
    std::string platform;
};

inline std::string CopyOrFallback(const char* value, std::string_view fallback) {
    if (value != nullptr && value[0] != '\0') {
        return value;
    }
    return std::string(fallback);
}

inline const InfraBuildInfoSnapshot& GetInfraBuildInfoSnapshot() {
    static const InfraBuildInfoSnapshot snapshot = [] {
        KanoBuildInfo info = kano_build_info_discover();

        InfraBuildInfoSnapshot out{
            CopyOrFallback(kano_build_info_get_version(info),
#ifdef KOG_BUILD_VERSION
                KOG_BUILD_VERSION
#elif defined(KOG_VERSION)
                KOG_VERSION
#else
                "unknown"
#endif
            ),
            CopyOrFallback(kano_build_info_get_vcs_status(info),
#ifdef KOG_BUILD_VCS
                KOG_BUILD_VCS
#else
                "unknown"
#endif
            ),
            CopyOrFallback(kano_build_info_get_vcs_branch(info),
#ifdef KOG_BUILD_BRANCH
                KOG_BUILD_BRANCH
#else
                "unknown"
#endif
            ),
            CopyOrFallback(kano_build_info_get_vcs_revision(info),
#ifdef KOG_BUILD_REVISION
                KOG_BUILD_REVISION
#else
                "unknown"
#endif
            ),
#ifdef KOG_BUILD_REVISION_HASH_SHORT
            std::string(KOG_BUILD_REVISION_HASH_SHORT),
#else
            std::string("unknown"),
#endif
#ifdef KOG_BUILD_REVISION_HASH
            std::string(KOG_BUILD_REVISION_HASH),
#else
            std::string("unknown"),
#endif
            CopyOrFallback(kano_build_info_get_vcs_status(info),
#ifdef KOG_BUILD_DIRTY
                KOG_BUILD_DIRTY
#else
                "unknown"
#endif
            ),
#ifdef KOG_BUILD_HOST_NAME
            std::string(KOG_BUILD_HOST_NAME),
#else
            std::string("unknown"),
#endif
#ifdef KOG_BUILD_CI
            std::string(KOG_BUILD_CI),
#else
            std::string("false"),
#endif
#ifdef KOG_BUILD_CONTEXT
            std::string(KOG_BUILD_CONTEXT),
#else
            std::string("local-manual"),
#endif
#ifdef KOG_BUILD_PIPELINE_ID
            std::string(KOG_BUILD_PIPELINE_ID),
#else
            std::string("unknown"),
#endif
            CopyOrFallback(kano_build_info_get_compiler(info),
#ifdef KOG_BUILD_TOOLCHAIN
                KOG_BUILD_TOOLCHAIN
#else
                "unknown"
#endif
            ),
#ifdef KOG_BUILD_GENERATOR
            std::string(KOG_BUILD_GENERATOR),
#else
            std::string("unknown"),
#endif
#ifdef KOG_BUILD_PRESET
            std::string(KOG_BUILD_PRESET),
#else
            std::string("unknown-preset"),
#endif
            CopyOrFallback(kano_build_info_get_build_type(info),
#ifdef KOG_BUILD_CONFIGURATION
                KOG_BUILD_CONFIGURATION
#else
                "unknown"
#endif
            ),
#ifdef KOG_BUILD_PLATFORM
            std::string(KOG_BUILD_PLATFORM),
#else
            std::string("unknown"),
#endif
        };

        kano_build_info_free(info);
        return out;
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
    return detail::GetInfraBuildInfoSnapshot().revision_hash_short;
}

inline std::string_view GetBuildRevisionHash() {
    return detail::GetInfraBuildInfoSnapshot().revision_hash;
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
