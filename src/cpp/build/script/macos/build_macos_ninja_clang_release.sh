#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
export KOG_CPP_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
export KOG_BUILD_USE_LLVM=1

source "$SCRIPT_DIR/../common/unix_preset_build.sh"

kog_run_unix_preset "macos-ninja-clang" "macos-ninja-clang-release"
