#!/usr/bin/env bash
#
# self/rebuild.sh — Clean + one-shot release build
#
# Design doc: ../../../shared/infra/docs/design/cpp-stage-contract.md
# Implements: self-rebuild orchestration-only flow (rm out/ + self-build)
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
CPP_ROOT="$(cd -- "$SCRIPT_DIR/../.." && pwd)"

rm -rf "$CPP_ROOT/out" "$CPP_ROOT/build"
. "$CPP_ROOT/shared/infra/scripts/orchestration/matrix.sh"
build_script="$(kog_matrix_default_release_script)"
exec bash "$build_script" "$@"
