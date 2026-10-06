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

BUILD_DIR="$REPO_ROOT/src/cpp/out/ci-tui-pr-gate"
mkdir -p "$BUILD_DIR"

echo "[build-tui-pr-gate] configure preset=$CONFIGURE_PRESET"
echo "[build-tui-pr-gate] build preset=$BUILD_PRESET"
echo "[build-tui-pr-gate] CPP_ROOT=$CPP_ROOT"
echo "[build-tui-pr-gate] BUILD_DIR=$BUILD_DIR"

# Configure.  The pixi env from the previous "Install locked shared-infra
# environment" step already added cmake and ninja to PATH.
cmake -S "$CPP_ROOT" -B "$BUILD_DIR" -C "$CONFIGURE_PRESET"

# Build.  The artifact target matches what run_tui_pr_focus.py expects.
cmake --build "$BUILD_DIR" --preset "$BUILD_PRESET" \
    --target kog_runtime_artifact

echo "[build-tui-pr-gate] build complete: $BUILD_DIR"
