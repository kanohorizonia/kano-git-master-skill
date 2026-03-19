module;

#include "version.hpp"

export module kano.git.version;

export namespace kano::git {
using ::kano::git::GetBuildVersion;
using ::kano::git::GetBuildVCS;
using ::kano::git::GetBuildBranch;
using ::kano::git::GetBuildRevision;
using ::kano::git::GetBuildRevisionHashShort;
using ::kano::git::GetBuildRevisionHash;
using ::kano::git::GetBuildDirty;
using ::kano::git::BuildHostName;
using ::kano::git::GetBuildCI;
using ::kano::git::GetBuildContext;
using ::kano::git::GetBuildPipelineId;
using ::kano::git::GetBuildToolchain;
using ::kano::git::GetBuildGenerator;
using ::kano::git::GetBuildPreset;
using ::kano::git::GetBuildConfiguration;
using ::kano::git::BuildHostPlatform;
using ::kano::git::GetBuildInfo;
using ::kano::git::GetVersion;

}
