#!/usr/bin/env bash
# =============================================================================
# Windows MSVC Coverage - Generate Report
# =============================================================================
# Converts .coverage binary to XML/Cobertura for CI ingestion.
# Note: Microsoft.CodeCoverage.Console does NOT produce HTML directly.
# The .coverage binary can be opened in Visual Studio for interactive reports,
# or converted to Cobertura XML for CI tools (Codecov, etc.)
#
# Usage:
#   bash ninja-msvc-coverage-report.sh
#
# Output:
#   $KOG_COVERAGE_ROOT/coverage/coverage.xml
#   $KOG_COVERAGE_ROOT/coverage/coverage.cobertura.xml
#   $KOG_COVERAGE_ROOT/coverage/summary.txt (text summary via PowerShell)
# =============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
export KOG_CPP_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
KOG_COVERAGE_ROOT="${KOG_COVERAGE_ROOT:-${KOG_CPP_ROOT}/out/coverage}"

# Detect CodeCoverage.Console
KOG_CODE_COVERAGE_CONSOLE="${KOG_CODE_COVERAGE_CONSOLE:-}"
if [[ -z "$KOG_CODE_COVERAGE_CONSOLE" ]]; then
    for path in \
        "C:/Program Files/Microsoft Visual Studio/2022/Community/Common7/IDE/Extensions/Microsoft/CodeCoverage.Console/Microsoft.CodeCoverage.Console.exe" \
        "C:/Program Files/Microsoft Visual Studio/18/Community/Common7/IDE/Extensions/Microsoft/CodeCoverage.Console/Microsoft.CodeCoverage.Console.exe"
    do
        if [[ -x "$path" ]]; then
            KOG_CODE_COVERAGE_CONSOLE="$path"
            break
        fi
    done
fi

if [[ -z "$KOG_CODE_COVERAGE_CONSOLE" || ! -x "$KOG_CODE_COVERAGE_CONSOLE" ]]; then
    echo "[ERROR] Microsoft.CodeCoverage.Console not found." >&2
    exit 1
fi

COVERAGE_FILE="$KOG_COVERAGE_ROOT/windows.coverage"

if [[ ! -f "$COVERAGE_FILE" ]]; then
    echo "[ERROR] Coverage file not found: $COVERAGE_FILE" >&2
    echo "[ERROR] Run ninja-msvc-coverage-run.sh first." >&2
    exit 1
fi

mkdir -p "$KOG_COVERAGE_ROOT"

# Convert to XML (native format)
echo "[coverage-report] Converting to XML..."
"$KOG_CODE_COVERAGE_CONSOLE" merge "$COVERAGE_FILE" \
    -o "$KOG_COVERAGE_ROOT/coverage.xml" \
    -f xml \
    2>&1 | grep -v "^Logo" || true

# Convert to Cobertura XML (for Codecov/CI tools)
echo "[coverage-report] Converting to Cobertura XML..."
"$KOG_CODE_COVERAGE_CONSOLE" merge "$COVERAGE_FILE" \
    -o "$KOG_COVERAGE_ROOT/coverage.cobertura.xml" \
    -f cobertura \
    2>&1 | grep -v "^Logo" || true

echo "[coverage-report] Reports:"
echo "  XML:        $KOG_COVERAGE_ROOT/coverage.xml"
echo "  Cobertura:  $KOG_COVERAGE_ROOT/coverage.cobertura.xml"

# Also try to generate HTML via VS CodeCoverage API (PowerShell)
# This uses the internal CoverageInfo class
echo "[coverage-report] Generating text summary via PowerShell..."
powershell -NoProfile -ExecutionPolicy Bypass -Command "
\$ErrorActionPreference = 'Stop'
try {
    # Load Coverage info from the .coverage file
    Add-Type -Path \"${KOG_CODE_COVERAGE_CONSOLE%/*}/*.dll\" -ErrorAction SilentlyContinue
    
    # Try to read coverage data via COM
    \$coverage = [Microsoft.VisualStudio.Coverage.Analysis.CoverageInfo]::CreateFromFile('$COVERAGE_FILE')
    \$session = \$coverage.BuildSession()
    
    \$summary = \$session.ExecutedCodeCoverageStatistics
    Write-Output \"Lines covered:   \$(\$summary.LinesCovered)\"
    Write-Output \"Lines not covered: \$(\$summary.LinesNotCovered)\"
    Write-Output \"Line coverage:  \$([math]::Round(\$summary.LineCoverage * 100, 2))%\"
    Write-Output \"Blocks covered: \$(\$summary.BlocksCovered)\"
    Write-Output \"Blocks not covered: \$(\$summary.BlocksNotCovered)\"
    Write-Output \"Block coverage: \$([math]::Round(\$summary.BlockCoverage * 100, 2))%\"
} catch {
    Write-Output \"Could not generate summary via PowerShell (requires VS SDK)\"
    Write-Output \"Open \$COVERAGE_FILE in Visual Studio for interactive report\"
}
" > "$KOG_COVERAGE_ROOT/summary.txt" 2>&1 || true

cat "$KOG_COVERAGE_ROOT/summary.txt"
echo "[coverage-report] Summary: $KOG_COVERAGE_ROOT/summary.txt"
