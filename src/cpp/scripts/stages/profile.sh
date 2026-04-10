#!/usr/bin/env bash
# =============================================================================
# Profile Stage — delegates to kano-cpp-infra profiling framework
# =============================================================================
# KOG_PROFILING_ROOT must point to the kano-cpp-infra scripts/profiling/
# directory. It is automatically derived from the submodule path.
# =============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# Derive the kano-cpp-infra submodule path: ../../ -> src/cpp/ -> scripts/ -> infra/scripts/profiling
INFRA_PROFILING_ROOT="$(cd -- "$SCRIPT_DIR/../../shared/infra/scripts/profiling" && pwd)"
export KOG_PROFILING_ROOT="$INFRA_PROFILING_ROOT"
export KOG_PROFILE_SCRIPT_ROOT="$INFRA_PROFILING_ROOT"

exec bash "$INFRA_PROFILING_ROOT/run.sh" "$@"
