#!/usr/bin/env bash
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
cmake --build "$APP_BIN_DIR" --target kano_git_cli kano_git_tui kog_runtime_artifact
echo "[build-tui-pr-gate] build complete: $APP_BIN_DIR"