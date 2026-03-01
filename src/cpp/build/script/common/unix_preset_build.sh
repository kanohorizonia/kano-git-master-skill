#!/usr/bin/env bash
set -euo pipefail

if [[ -z "${KOG_CPP_ROOT:-}" ]]; then
  echo "KOG_CPP_ROOT is not set." >&2
  exit 1
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/build_metadata.sh"

kog_run_unix_preset() {
  local InConfigurePreset="$1"
  local InBuildPreset="$2"

  (
    cd "$KOG_CPP_ROOT"
    kog_collect_build_metadata
    cmake --preset "$InConfigurePreset"
    cmake --build --preset "$InBuildPreset"
  )
}
