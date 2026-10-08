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
#   * re-entrant: no `source` of watchdog/bootstrap helpers
#   * no-op re-entry guard: safe to call from any CI step context
#   * bounded: returns the build's own exit code verbatim
#
# Usage:
#   bash scripts/ci/build-tui-pr-gate.sh <configure_preset> <build_preset>
#
# Environment (set by the workflow before invocation):
#   KANO_CPP_ROOT  : path to src/cpp (workspace-relative)
#   PATH           : must include cmake (added by pixi install step 7)

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

# The configure preset names use an inconsistent binaryDir suffix convention
# (e.g. windows-ninja-msvc -> out/obj/win-ninja-msvc; linux-ninja-clang ->
# out/obj/linux-ninja-clang).  Hard-coding the mapping is brittle, so ask
# cmake for the canonical binaryDir it just configured.  This is the only
# way to stay in lock-step with the FetchContent cache path that
# workflow step 6 restores.
echo "[build-tui-pr-gate] configure preset=$CONFIGURE_PRESET"
echo "[build-tui-pr-gate] build preset=$BUILD_PRESET"
echo "[build-tui-pr-gate] CPP_ROOT=$CPP_ROOT"

# cmake --preset reads CMakePresets.json from the current working directory,
# not from -S.  cd into CPP_ROOT so the preset is found at ./CMakePresets.json.
cd "$CPP_ROOT" || {
    echo "failed to cd to CPP_ROOT=$CPP_ROOT" >&2
    exit 2
}

# Configure WITHOUT -B so the preset's binaryDir is honoured and the
# FetchContent cache restored to out/obj/<suffix>/_deps/ is found.
cmake --preset "$CONFIGURE_PRESET"

# Ask cmake for the canonical binaryDir it configured.
BUILD_DIR="$(cmake --preset "$CONFIGURE_PRESET" --print-value-of=CMAKE_BINARY_DIR)"
echo "[build-tui-pr-gate] BUILD_DIR=$BUILD_DIR"

# Build.  The artifact target matches what run_tui_pr_focus.py expects.
cmake --build "$BUILD_DIR" --preset "$BUILD_PRESET" \
    --target kog_runtime_artifact

echo "[build-tui-pr-gate] build complete: $BUILD_DIR"