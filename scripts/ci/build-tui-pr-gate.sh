#!/usr/bin/env bash
# Stable, re-entrant TUI PR Gates build entrypoint (KOG-BUG-0146).
#
# This script exists because the GitHub Actions step that previously
# sourced the legacy Unix and Windows preset build helpers from inside
# the ephemeral step context caused a double-source re-entry through
# the kano-cpp-infra watchdog and pixi bootstrap activate functions,
# making the build step fail in 1 second on macOS and Windows.
#
# Contracts enforced by the workflow-contract regression tests
# in src/cpp/code/tests/kano_git_regression_tests/regression_coverage_test.cpp:
#   case_id KOG-BUG-0146/stable-executable-build-entrypoint:
#     * this script is tracked executable (100755) in the git index
#     * it never sources the legacy preset helpers
#     * it never calls the watchdog or bootstrap activate functions
#     * the build command uses --config Release but never --preset
#       (the build preset would re-configure against a different binaryDir
#       and break FetchContent cache lookup)
#   case_id KOG-BUG-0146/q-esc-acceptance-in-workflow:
#     * the TUI PR Gates workflow must require both the q and Escape
#       production standalone TUI acceptance tests for POSIX (linux/macos)
#       and Windows ConPTY (windows-x64) terminal lifecycles — this
#       script's correctness is the precondition for those tests being
#       runnable on windows-x64 once KOG-BUG-0107's
#       export_cmd.cpp:1059 MinGW compile error is fixed.
#
# See assets/regression/incidents.json (KOG-BUG-0146) for the full
# root cause and the matching workflow contracts.
set -euo pipefail
if [[ $# -ne 2 ]]; then
    echo "usage: build-tui-pr-gate.sh <configure_preset> <build_preset>" >&2
    exit 64
fi
CONFIGURE_PRESET="$1"
BUILD_PRESET="$2"
SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]:-$0}")" && pwd)"
REPO_ROOT="$(cd -- "$SCRIPT_DIR/../.." && pwd)"
CPP_ROOT="${KANO_CPP_ROOT:-$REPO_ROOT/src/cpp}"
if [[ ! -d "$CPP_ROOT" ]]; then echo "CPP_ROOT does not exist: $CPP_ROOT" >&2; exit 2; fi
if [[ ! -f "$CPP_ROOT/CMakePresets.json" ]]; then echo "CMakePresets.json not found at $CPP_ROOT/CMakePresets.json" >&2; exit 2; fi
case "$(uname -s 2>/dev/null || true)" in
    MINGW*|MSYS*|CYGWIN*) APP_BIN_DIR="out/bin/windows-ninja-msvc/release" ;;
    Darwin)                APP_BIN_DIR="out/bin/macos-ninja-clang-arm64/release" ;;
    *)                      APP_BIN_DIR="out/bin/linux-ninja-clang/release" ;;
esac
echo "[build-tui-pr-gate] configure preset=$CONFIGURE_PRESET"
echo "[build-tui-pr-gate] build preset=$BUILD_PRESET"
echo "[build-tui-pr-gate] CPP_ROOT=$CPP_ROOT"
echo "[build-tui-pr-gate] APP_BIN_DIR=$APP_BIN_DIR"
cd "$CPP_ROOT" || { echo "failed to cd to CPP_ROOT=$CPP_ROOT" >&2; exit 2; }
cmake -S . -B "$APP_BIN_DIR" --preset "$CONFIGURE_PRESET"
cmake --build "$APP_BIN_DIR" --config Release --target kano_git_cli kano_git_tui kano_git_tui_tests kano_git_commit_plan_tests kog_runtime_artifact
echo "[build-tui-pr-gate] build complete: $APP_BIN_DIR"