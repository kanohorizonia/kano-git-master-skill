#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"

bash "$SCRIPT_DIR/../stages/pgi-build.sh" "$@"
bash "$SCRIPT_DIR/../stages/pgo-gather.sh"
bash "$SCRIPT_DIR/../common/pgo_workflow.sh" merge
exec bash "$SCRIPT_DIR/../stages/pgo-build.sh" "$@"
