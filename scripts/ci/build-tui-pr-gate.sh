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
#   * re-entrant: does NOT source the watchdog/bootstrap helpers
#   * bounded: returns the build's own exit code verbatim
#
# Usage:
#   bash scripts/ci/build-tui-pr-gate.sh <configure_preset> <build_preset>
#
# Environment (set by the workflow before invocation):
#   KANO_CPP_ROOT  : path to src/cpp (workspace-relative)
#   PATH           : must include cmake (added by pixi install step 7)
#
# P1 history (this round):
#   r1: direct `cmake --preset` + `cmake --build` → 1-second errors on
#       macos/windows because the workflow step had been sourcing the
#       infra's watchdog/bootstrap helpers.  Fixed by removing the
#       `source` line and using direct cmake invocation.
#   r2: switched to cmake `--preset` (wrong flag `-C`) → fixed to `--preset`.
#   r3: fixed `cd CPP_ROOT` so the preset file is found at CWD=PWD.
#       CMakeCache.txt was being written to a wrong location.
#   r4: switched to letting the preset determine binaryDir (no `-B`)
#       so the FetchContent cache at out/obj/<suffix>/_deps/ resolves.
#   r5: hard-coded BUILD_DIR mapping was wrong for windows-ninja-msvc
#       (preset binaryDir is out/obj/win-ninja-msvc, lowercase).  Tried
#       `cmake --preset ... --print-value-of=CMAKE_BINARY_DIR` but that
#       option is unavailable in the --preset form.
#   r6: parsed binaryDir from CMakePresets.json with awk → all three
#       platforms' configure succeeded but the test binary went to
#       out/obj/<preset>/ instead of out/bin/<platform>/release/, so
#       run_tui_pr_focus.py exited in ~1 second with no JUnit produced.
#   r7: switched to `exec` of the matrix-resolved platform-specific
#       build script (native-build.sh / ninja-msvc-release.sh) but those
#       scripts themselves call pixi_bootstrap.sh which has a path bug
#       (tries to `cd scripts/shared/infra` from inside the infra tree,
#       which doesn't exist).  This is an infra bug, not a KOG-BUG-0146
#       issue — the user explicitly said "Windows 與 macOS 的問題要記
#       在 workflow/infra scope，不要繼續污染 KOG-BUG-0146."
#   r8 (this version): revert to direct cmake invocation with
#       explicit binaryDir override matching the test runner's expected
#       application_dir.  Build the kog_runtime_artifact target that
#       produces the test binary at out/bin/<platform>/release/.  This
#       is the same approach as the working TUI PR Gates run 37403590260
#       that produced 1440/1440 passing tests on Linux.

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

# Map configure preset to the binaryDir the test runner expects
# (out/bin/<platform>/release).  This is where run_tui_pr_focus.py
# looks for the test binary.  The FetchContent cache restore step
# (workflow step 6) does NOT need to match this because we use a
# separate binaryDir from the FetchContent-derived one.
case "$(uname -s 2>/dev/null || true)" in
    MINGW*|MSYS*|CYGWIN*) APP_BIN_DIR="out/bin/windows-ninja-msvc/release" ;;
    Darwin)                APP_BIN_DIR="out/bin/macos-ninja-clang-arm64/release" ;;
    *)                      APP_BIN_DIR="out/bin/linux-ninja-clang/release" ;;
esac

echo "[build-tui-pr-gate] configure preset=$CONFIGURE_PRESET"
echo "[build-tui-pr-gate] build preset=$BUILD_PRESET"
echo "[build-tui-pr-gate] CPP_ROOT=$CPP_ROOT"
echo "[build-tui-pr-gate] APP_BIN_DIR=$APP_BIN_DIR"

# cmake --preset reads CMakePresets.json from the current working directory,
# not from -S.  cd into CPP_ROOT so the preset is found at ./CMakePresets.json.
cd "$CPP_ROOT" || {
    echo "failed to cd to CPP_ROOT=$CPP_ROOT" >&2
    exit 2
}

# Configure WITH -B set to the test runner's application_dir, so the
# test binary lands where run_tui_pr_focus.py expects it.
# This avoids the FetchContent cache lookup because the preset's
# binaryDir (out/obj/<preset>) is independent of -B; -B only redirects
# where the build output goes.  The FetchContent cache lookup uses
# CMAKE_BINARY_DIR, not -B.
cmake -S . -B "$APP_BIN_DIR" --preset "$CONFIGURE_PRESET"

# Build.  The artifact target matches what run_tui_pr_focus.py expects.
cmake --build "$APP_BIN_DIR" --preset "$BUILD_PRESET" \
    --target kog_runtime_artifact

echo "[build-tui-pr-gate] build complete: $APP_BIN_DIR"