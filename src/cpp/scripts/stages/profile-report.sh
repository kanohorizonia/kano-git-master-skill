#!/usr/bin/env bash
# =============================================================================
# Profile Report Stage — delegates to kano-cpp-infra profiling framework
# =============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
INFRA_PROFILING_ROOT="$(cd -- "$SCRIPT_DIR/../../shared/infra/scripts/profiling" && pwd)"
export KOG_PROFILING_ROOT="$INFRA_PROFILING_ROOT"
export KOG_PROFILE_SCRIPT_ROOT="$INFRA_PROFILING_ROOT"

exec bash "$INFRA_PROFILING_ROOT/report.sh" "$@"
