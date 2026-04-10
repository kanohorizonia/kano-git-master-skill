#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
CPP_ROOT="$(cd -- "$SCRIPT_DIR/../.." && pwd)"

rm -rf "$CPP_ROOT/out" "$CPP_ROOT/build"
exec bash "$SCRIPT_DIR/build.sh" "$@"
