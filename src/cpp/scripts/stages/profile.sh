#!/usr/bin/env bash
# =============================================================================
# Profile Stage — delegates to kano-cpp-infra profiling framework
# =============================================================================
# KOG_PROFILING_ROOT must point to the kano-cpp-infra scripts/profiling/
# directory. It is automatically derived from the submodule path.
# =============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# Derive the kano-cpp-infra submodule path: ../../ -> src/cpp/ -> scripts/ -> infra/build/base/script/profiling
INFRA_PROFILING_ROOT="$(cd -- "$SCRIPT_DIR/../../shared/infra/build/base/script/profiling" && pwd)"
INFRA_SCRIPTS_ROOT="$(cd -- "$SCRIPT_DIR/../../shared/infra/build/base/script" && pwd)"
export KOG_PROFILING_ROOT="$INFRA_PROFILING_ROOT"
export KOG_PROFILE_SCRIPT_ROOT="$INFRA_PROFILING_ROOT"
# Point to the actual project repo root (kano-git-master-skill), not the infra submodule dir
export KOG_PROFILE_REPO_ROOT="$(cd -- "$SCRIPT_DIR/../../../.." && pwd)"
export KOG_PROFILE_CPP_ROOT="$(cd -- "$SCRIPT_DIR/../../" && pwd)"
export KOG_BASELINE_SCRIPT="$KOG_PROFILE_CPP_ROOT/scripts/common/measure_iteration_baseline.sh"
export KOG_PGO_REBUILD_SCRIPT="$KOG_PROFILE_CPP_ROOT/scripts/workflows/pgo-rebuild.sh"

exec bash "$INFRA_PROFILING_ROOT/run.sh" "$@"
