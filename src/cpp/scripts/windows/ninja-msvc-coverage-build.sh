#!/usr/bin/env bash
# =============================================================================
# Windows MSVC Coverage Build
# =============================================================================
# Builds with /PROFILE flag for coverage instrumentation.
# Run: ninja-msvc-coverage-run.sh to execute tests with coverage collection
# Run: ninja-msvc-coverage-report.sh to generate HTML report
# =============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
export KOG_CPP_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

source "$SCRIPT_DIR/../../shared/infra/build/base/script/common/windows_preset_build.sh"

# Windows coverage: /PROFILE flag via KOG_ENABLE_COVERAGE=ON
export KOG_ENABLE_COVERAGE=1

# Native MSVC coverage instrumentation is sensitive to path aliasing and cached
# compiler outputs. Build coverage binaries from real paths with no compiler
# launcher so Microsoft.CodeCoverage.Console can instrument them reliably.
export KOG_SUBST_MODE=off
export KOG_COMPILER_LAUNCHER=none

# Detect if running in CI (GitHub Actions) to enable artifact output
if [[ -n "${CI:-}" ]]; then
    echo "[ninja-msvc-coverage-build] CI mode: enabling coverage build"
fi

kog_run_windows_preset "windows-ninja-msvc-coverage" "windows-ninja-msvc-coverage-debug" "x64"
