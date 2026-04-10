#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
CPP_ROOT="$(cd -- "$SCRIPT_DIR/../.." && pwd)"

exec bash "$CPP_ROOT/code/tests/run_tests.sh" "$@"
