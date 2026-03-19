#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
export KOG_CPP_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"

source "$SCRIPT_DIR/../common/windows_preset_build.sh"

export KOG_FASTBUILD_ENABLED=1
kog_apply_fastbuild_env
kog_run_windows_preset "windows-fastbuild-msvc" "windows-fastbuild-msvc-release" "x64"
