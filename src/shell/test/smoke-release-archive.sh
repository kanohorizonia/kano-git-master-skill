#!/usr/bin/env bash
#
# smoke-release-archive.sh
# Verifies that an exported kano-git-master-skill archive has a usable Unix
# launcher surface and does not contain broken submodule .git pointer files.
#
# Usage:
#   src/shell/test/smoke-release-archive.sh <archive.tar>
#
# Optional:
#   KOG_RELEASE_SMOKE_BUILD=1   Also run ./scripts/kog self build. This is an
#                               online build smoke test when CMake FetchContent
#                               source modes are used.

set -euo pipefail

archive_path="${1:-}"

fail() {
  echo "Error: $*" >&2
  exit 1
}

require_file() {
  local path="$1"
  [[ -f "$path" ]] || fail "required file missing from release archive: $path"
}

require_executable() {
  local path="$1"
  if [[ ! -x "$path" ]]; then
    ls -l "$path" >&2 2>/dev/null || true
    fail "required executable missing or not executable in release archive: $path"
  fi
}

require_bash_syntax() {
  local path="$1"
  require_file "$path"
  bash -n "$path" || fail "bash syntax check failed for release archive file: $path"
}

has_pixi_runtime() {
  command -v pixi >/dev/null 2>&1 && return 0
  [[ -n "${PIXI_HOME:-}" && -x "${PIXI_HOME}/bin/pixi" ]] && return 0
  [[ -n "${HOME:-}" && -x "${HOME}/.pixi/bin/pixi" ]] && return 0
  return 1
}

if [[ -z "$archive_path" ]]; then
  echo "Usage: $0 <archive.tar>" >&2
  exit 2
fi
if [[ ! -f "$archive_path" ]]; then
  echo "Error: archive not found: $archive_path" >&2
  exit 2
fi

tmp_root="$(mktemp -d)"
cleanup() {
  rm -rf "$tmp_root" 2>/dev/null || true
}
trap cleanup EXIT

if command -v python3 >/dev/null 2>&1; then
python3 - "$archive_path" "$tmp_root" <<"PY"
import sys
import tarfile
from pathlib import Path
archive_path = Path(sys.argv[1])
extract_root = Path(sys.argv[2])
with tarfile.open(archive_path, "r:*") as tf:
  try:
    tf.extractall(extract_root, filter='data')
  except TypeError:
    tf.extractall(extract_root)
PY
else
  tar -xf "$archive_path" -C "$tmp_root"
fi

repo_root=""
if [[ -d "$tmp_root/kano-git-master-skill" ]]; then
  repo_root="$tmp_root/kano-git-master-skill"
else
  repo_root="$(find "$tmp_root" -mindepth 1 -maxdepth 1 -type d | sed -n '1p')"
fi
if [[ -z "$repo_root" || ! -d "$repo_root" ]]; then
  echo "Error: cannot resolve extracted repo root." >&2
  exit 1
fi

cd "$repo_root"

require_executable scripts/kog
require_executable scripts/kano-git
require_bash_syntax scripts/kog
require_bash_syntax scripts/kano-git
require_bash_syntax src/cpp/shared/infra/scripts/platform/linux/native-build.sh
require_bash_syntax src/cpp/shared/infra/scripts/platform/mac/native-build.sh
require_bash_syntax src/cpp/shared/infra/scripts/lib/unix_preset_build.sh

if find assets/ignore-sources/upstream/github-gitignore src/cpp/shared/infra -name .git -print | grep -q .; then
  echo "Error: archive contains broken submodule .git pointer files." >&2
  exit 1
fi

if [[ "${KOG_RELEASE_SMOKE_REQUIRE_LAUNCHER_HELP:-0}" == "1" ]] || has_pixi_runtime; then
  ./scripts/kog --help >/dev/null 2>&1 || fail "release archive launcher help failed: scripts/kog --help"
else
  echo "Info: skipping scripts/kog --help because pixi is unavailable in this offline smoke environment." >&2
fi

if [[ -x src/shell/test/pre-commit-quality-gate.sh ]]; then
  src/shell/test/pre-commit-quality-gate.sh >/dev/null || fail "release archive pre-commit quality gate failed"
else
  if [[ -x src/shell/test/smoke-root-wrapper-generator.sh ]]; then
    src/shell/test/smoke-root-wrapper-generator.sh >/dev/null || fail "release archive root-wrapper smoke failed"
  fi

  if [[ -x src/shell/test/audit-public-doc-script-refs.sh ]]; then
    src/shell/test/audit-public-doc-script-refs.sh >/dev/null || fail "release archive public-doc script audit failed"
  fi
fi

if [[ "${KOG_RELEASE_SMOKE_BUILD:-0}" == "1" ]]; then
  ./scripts/kog self build
fi

echo "PASS: release archive smoke test"
