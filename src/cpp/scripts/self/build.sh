#!/usr/bin/env bash
#
# self/build.sh — Developer one-shot release build
#
# Design doc: ../../../shared/infra/docs/design/cpp-stage-contract.md
# Implements: self-build orchestration-only flow
# Stage: ../stages/build.sh (delegates to matrix → platform adapter)
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
. "$SCRIPT_DIR/../../shared/infra/scripts/orchestration/matrix.sh"

build_script="$(kog_matrix_default_release_script)"
exec bash "$build_script" "$@"
