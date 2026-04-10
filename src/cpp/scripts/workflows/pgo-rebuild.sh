#!/usr/bin/env bash
#
# pgo-rebuild.sh — Full PGO pipeline (3-stage)
#
# Design doc: ../../../../shared/infra/docs/design/cpp-stage-contract.md
# Implements: pgo_pipeline combination (matrix.yml)
# Stage contract: pgi-build.sh → pgo-gather.sh → pgo-build.sh
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
INFRA_COMMON_ROOT="$(cd -- "$SCRIPT_DIR/../../shared/infra/scripts/common" && pwd)"

bash "$SCRIPT_DIR/../stages/pgi-build.sh" "$@"
bash "$SCRIPT_DIR/../stages/pgo-gather.sh"
bash "$INFRA_COMMON_ROOT/pgo_workflow.sh" merge
exec bash "$SCRIPT_DIR/../stages/pgo-build.sh" "$@"
