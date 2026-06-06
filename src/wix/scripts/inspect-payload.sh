#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
source "${SCRIPT_DIR}/lib/build-common.sh"

kano_wix_prepare_context 0
kano_wix_print_context

echo "Payload inspection"
echo "  Root : $KANO_WIX_PAYLOAD_ROOT"
echo "  Files: $(find "$KANO_WIX_PAYLOAD_ROOT" -type f | wc -l | tr -d ' ')"
echo "  Directories: $(find "$KANO_WIX_PAYLOAD_ROOT" -type d | wc -l | tr -d ' ')"
echo
echo "Top-level entries"
find "$KANO_WIX_PAYLOAD_ROOT" -mindepth 1 -maxdepth 1 | sort
echo
echo "Sample payload tree (depth <= 3)"
find "$KANO_WIX_PAYLOAD_ROOT" -mindepth 1 -maxdepth 3 | sort
