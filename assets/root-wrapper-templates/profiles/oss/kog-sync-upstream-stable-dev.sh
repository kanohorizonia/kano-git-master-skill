#!/usr/bin/env bash
#
# kog-sync-upstream-stable-dev.sh - OSS profile wrapper for stable-dev workspace sync

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

exec bash "$ROOT/kog" sync stable-dev --workspace "$@"
