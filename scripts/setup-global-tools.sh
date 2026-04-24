#!/usr/bin/env bash
# =============================================================================
# Kano Global Tools Bootstrap
# =============================================================================
# Installs the shared toolchain defined in tools/pixi-global.toml into
# the Pixi global environment (~/.pixi).
#
# Usage:
#   bash scripts/setup-global-tools.sh           # install all platforms
#   bash scripts/setup-global-tools.sh --dry-run  # preview only
#   bash scripts/setup-global-tools.sh --current  # install current platform only
#
# This script is idempotent — safe to run multiple times.
# =============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
SKILL_ROOT="$(cd -- "$SCRIPT_DIR/.." && pwd)"
GLOBAL_MANIFEST="$SKILL_ROOT/src/cpp/shared/infra/pixi-global-tool.toml"

# Detect current platform (matches pixi.toml platform naming)
detect_platform() {
  local os_name arch
  os_name="$(uname -s 2>/dev/null || true)"
  case "$os_name" in
    MINGW*|MSYS*|CYGWIN*) printf '%s\n' win-64 ;;
    Darwin)
      arch="$(uname -m 2>/dev/null || true)"
      case "$arch" in
        aarch64|arm64) printf '%s\n' osx-arm64 ;;
        *) printf '%s\n' osx-64 ;;
      esac
      ;;
    *) printf '%s\n' linux-64 ;;
  esac
}

# Check if pixi is available
check_pixi() {
  if ! command -v pixi >/dev/null 2>&1; then
    echo "ERROR: pixi not found in PATH" >&2
    echo "Install from: https://pixi.sh/install/" >&2
    return 1
  fi
  echo "[setup-global-tools] pixi found: $(pixi --version)"
}

# Install global tools for current platform only
install_current_platform() {
  local platform
  platform="$(detect_platform)"
  echo "[setup-global-tools] Installing tools for platform: $platform"

  if ! pixi global install \
    --manifest-path "$GLOBAL_MANIFEST" \
    --platform "$platform" \
    --expose; then
    echo "ERROR: pixi global install failed" >&2
    return 1
  fi
  echo "[setup-global-tools] Installed successfully for $platform"
}

# Install global tools for all platforms
install_all_platforms() {
  echo "[setup-global-tools] Installing tools for all platforms..."

  if ! pixi global install \
    --manifest-path "$GLOBAL_MANIFEST" \
    --platform win-64 \
    --platform linux-64 \
    --platform osx-64 \
    --platform osx-arm64 \
    --expose; then
    echo "ERROR: pixi global install failed" >&2
    return 1
  fi
  echo "[setup-global-tools] Installed successfully for all platforms"
}

# Main
main() {
  local dry_run=false
  local current_only=false

  for arg in "$@"; do
    case "$arg" in
      --dry-run) dry_run=true ;;
      --current) current_only=true ;;
      --help)
        echo "Usage: $0 [--dry-run] [--current] [--help]"
        echo "  --dry-run  Preview commands without executing"
        echo "  --current  Install only current platform (default: all)"
        exit 0
        ;;
    esac
  done

  if [[ ! -f "$GLOBAL_MANIFEST" ]]; then
    echo "ERROR: Global manifest not found: $GLOBAL_MANIFEST" >&2
    exit 1
  fi

  check_pixi || exit 1

  local cmd
  if [[ "$current_only" == true ]]; then
    cmd="install_current_platform"
  else
    cmd="install_all_platforms"
  fi

  if [[ "$dry_run" == true ]]; then
    echo "[setup-global-tools] DRY RUN — would run: $cmd"
    echo "  manifest: $GLOBAL_MANIFEST"
    echo "  platform: $(detect_platform)"
    return 0
  fi

  $cmd
}

main "$@"