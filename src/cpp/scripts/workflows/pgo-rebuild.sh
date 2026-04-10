#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
INFRA_COMMON_ROOT="$(cd -- "$SCRIPT_DIR/../../shared/infra/build/base/script/common" && pwd)"

bash "$SCRIPT_DIR/../stages/pgi-build.sh" "$@"
bash "$SCRIPT_DIR/../stages/pgo-gather.sh"
bash "$INFRA_COMMON_ROOT/pgo_workflow.sh" merge
exec bash "$SCRIPT_DIR/../stages/pgo-build.sh" "$@"
