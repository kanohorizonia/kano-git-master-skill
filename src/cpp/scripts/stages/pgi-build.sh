#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
. "$SCRIPT_DIR/../common/pgo_workflow.sh"

exec bash "$SCRIPT_DIR/../common/pgo_workflow.sh" collect "$@"
