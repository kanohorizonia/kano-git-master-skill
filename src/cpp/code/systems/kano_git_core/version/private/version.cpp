#include "version.hpp"

namespace kano::git {

const char* KanoGitVersionAnchor() {
    return GetBuildVersion().data();
}

}
