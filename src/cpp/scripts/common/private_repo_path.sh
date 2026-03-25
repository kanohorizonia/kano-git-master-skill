#!/usr/bin/env bash
# =============================================================================
# Private Repository Path Helper
# =============================================================================
# Resolves the path to the private skill repository.
# The private repo contains sensitive infrastructure (host addresses, credentials).
#
# Convention: private repo lives at src/shell/private/ inside the skill repo.
# This script should be sourced from scripts in src/cpp/scripts/<platform>/
# =============================================================================
set -euo pipefail

# Compute skill root (two levels up from scripts/common/)
_kog_private_skill_root() {
    local script_dir
    script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
    # src/cpp/scripts/common → skill root
    echo "$(cd "$script_dir/../../.." && pwd)"
}

# Path to private scripts directory
kog_private_scripts_dir() {
    echo "$(_kog_private_skill_root)/src/shell/private/scripts"
}

# Path to a specific private script
kog_private_script() {
    local name="${1:-}"
    echo "$(kog_private_scripts_dir)/$name"
}
