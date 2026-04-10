#!/usr/bin/env bash
# =============================================================================
# Test Stage — generic entry point for running tests
# =============================================================================
# This is a generic stage script. It runs the test command via:
#   KOG_TEST_COMMAND  — explicit command to run (takes precedence)
#   KOG_CPP_ROOT/code/tests/run_tests.sh  — default if KOG_TEST_COMMAND is not set
#
# Project-specific override: set KOG_TEST_COMMAND in your environment or
# create a project-specific stages/test.sh that sets it before calling this.
# =============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
KOG_CPP_ROOT="${KOG_CPP_ROOT:-$(cd -- "$SCRIPT_DIR/../.." && pwd)}"

if [[ -n "${KOG_TEST_COMMAND:-}" ]]; then
  (
    cd "$KOG_CPP_ROOT"
    eval "$KOG_TEST_COMMAND" "$@"
  )
else
  exec bash "$KOG_CPP_ROOT/code/tests/run_tests.sh" "$@"
fi
