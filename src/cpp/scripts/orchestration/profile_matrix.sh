#!/usr/bin/env bash
# profile_matrix.sh — entry point for running profiling matrices.
# Delegates to kano-cpp-infra profiling framework.
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# INFRA_PROFILING_ROOT: ../../ -> src/, then shared/infra/build/base/script/profiling
INFRA_PROFILING_ROOT="$(cd -- "$SCRIPT_DIR/../../shared/infra/build/base/script/profiling" && pwd)"
export KOG_PROFILING_ROOT="$INFRA_PROFILING_ROOT"
export KOG_PROFILE_SCRIPT_ROOT="$INFRA_PROFILING_ROOT"
# Point to the actual project repo root (kano-git-master-skill), not the infra submodule dir
# profile_matrix.sh lives in src/cpp/scripts/orchestration/, so ../../../.. reaches skill root
export KOG_PROFILE_REPO_ROOT="$(cd -- "$SCRIPT_DIR/../../../.." && pwd)"
export KOG_PROFILE_CPP_ROOT="$(cd -- "$SCRIPT_DIR/../../" && pwd)"

exec bash "$INFRA_PROFILING_ROOT/run.sh" "$@"
