#!/usr/bin/env bash
# coverage-all.sh — Full coverage pipeline via stage composition
#
# Design doc: ../../../../shared/infra/docs/design/cpp-stage-contract.md
# Implements: coverage_pipeline combination (matrix.yml)
# Stage contract: ../../stages/coverage-build.sh → coverage-gather.sh → coverage-report.sh
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"

bash "$SCRIPT_DIR/../stages/coverage-build.sh" "$@"
bash "$SCRIPT_DIR/../stages/coverage-gather.sh" "$@"
exec bash "$SCRIPT_DIR/../stages/coverage-report.sh" "$@"
