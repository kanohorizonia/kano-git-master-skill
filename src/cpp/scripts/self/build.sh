#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
. "$SCRIPT_DIR/../../shared/infra/build/base/script/orchestration/matrix.sh"

build_script="$(kog_matrix_default_release_script)"
exec bash "$build_script" "$@"
