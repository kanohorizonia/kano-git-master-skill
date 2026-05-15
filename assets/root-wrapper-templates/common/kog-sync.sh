#!/usr/bin/env bash
#
# kog-sync.sh - Sync wrapper (defaults to origin-latest when no mode is given)

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

exec bash "$ROOT/kog" sync "$@"
