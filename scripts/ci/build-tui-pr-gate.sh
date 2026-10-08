#!/usr/bin/env bash
# build-tui-pr-gate.sh — Stable, re-entrant TUI PR Gates build entrypoint
#
# Replaces the legacy workflow step that `source`d the infra's
# scripts/lib/unix_preset_build.sh and scripts/lib/windows_preset_build.sh,
# both of which re-enter through kano_cpp_infra_watchdog_enter and
# kano_pixi_bootstrap_activate. That double-source path caused the
# 1-second step 8 failure on macos-arm64 and windows-x64.
#
# This entrypoint is:
#   * stable: same script on Linux, macos, Windows runners
#   * re-entrant: does NOT source the watchdog/bootstrap helpers;
#     `exec`s the matrix-resolved platform-specific build script instead
#   * no-op re-entry guard: safe to call from any CI step context
#   * bounded: returns the build's own exit code verbatim
#
# Usage:
#   bash scripts/ci/build-tui-pr-gate.sh <configure_preset> <build_preset>
#
# Environment (set by the workflow before invocation):
#   KANO_CPP_ROOT  : path to src/cpp (workspace-relative)
#   PATH           : must include cmake (added by pixi install step 7)
#
# P1 history:
#   r1..r3: direct `cmake --preset` + `cmake --build` from this script
#           produced test binaries at out/obj/<preset>/ — not the
#           out/bin/<preset>/release/ path that run_tui_pr_focus.py
#           expects.  The test step ran the binary search but found
#           nothing and exited within ~1 second (TUI PR Gates run 37750465970).
#   r4:  use the matrix-resolved platform-specific build script
#           (native-build.sh on linux/mac; ninja-msvc-release.sh on windows)
#           via `exec bash`.  These scripts know how to produce the
#           out/bin/<platform>/release layout that the TUI PR focus
#           selector requires.

set -euo pipefail

if [[ $# -ne 2 ]]; then
    echo "usage: build-tui-pr-gate.sh <configure_preset> <build_preset>" >&2
    exit 64
fi

CONFIGURE_PRESET="$1"
BUILD_PRESET="$2"

# Resolve repository root regardless of where the entrypoint is invoked from.
SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]:-$0}")" && pwd)"
REPO_ROOT="$(cd -- "$SCRIPT_DIR/../.." && pwd)"
CPP_ROOT="${KANO_CPP_ROOT:-$REPO_ROOT/src/cpp}"

if [[ ! -d "$CPP_ROOT" ]]; then
    echo "CPP_ROOT does not exist: $CPP_ROOT" >&2
    exit 2
fi

if [[ ! -f "$CPP_ROOT/CMakePresets.json" ]]; then
    echo "CMakePresets.json not found at $CPP_ROOT/CMakePresets.json" >&2
    exit 2
fi

echo "[build-tui-pr-gate] configure preset=$CONFIGURE_PRESET"
echo "[build-tui-pr-gate] build preset=$BUILD_PRESET"
echo "[build-tui-pr-gate] CPP_ROOT=$CPP_ROOT"

# The platform-specific build scripts under src/cpp/shared/infra/scripts/platform/
# are re-entrant executables that don't source the watchdog/bootstrap helpers.
# They know how to lay the binaryDir at out/bin/<platform>/release where
# run_tui_pr_focus.py expects the test binary to live.
case "$(uname -s 2>/dev/null || true)" in
    MINGW*|MSYS*|CYGWIN*) PLATFORM_BUILD_SCRIPT="$CPP_ROOT/shared/infra/scripts/platform/win64/ninja-msvc-release.sh" ;;
    Darwin)                PLATFORM_BUILD_SCRIPT="$CPP_ROOT/shared/infra/scripts/platform/mac/native-build.sh" ;;
    *)                      PLATFORM_BUILD_SCRIPT="$CPP_ROOT/shared/infra/scripts/platform/linux/native-build.sh" ;;
esac

if [[ ! -f "$PLATFORM_BUILD_SCRIPT" ]]; then
    echo "platform build script not found: $PLATFORM_BUILD_SCRIPT" >&2
    exit 2
fi

echo "[build-tui-pr-gate] platform build script=$PLATFORM_BUILD_SCRIPT"

# `exec` so the build script inherits our script's process slots and its
# exit code propagates verbatim.  No `source` of watchdog/bootstrap helpers.
exec bash "$PLATFORM_BUILD_SCRIPT" "$CONFIGURE_PRESET" "$BUILD_PRESET"