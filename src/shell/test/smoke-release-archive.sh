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

tar -xf "$archive_path" -C "$tmp_root"

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

test -x scripts/kog
test -x scripts/kano-git
bash -n scripts/kog
bash -n scripts/kano-git
bash -n src/cpp/shared/infra/scripts/platform/linux/native-build.sh
bash -n src/cpp/shared/infra/scripts/platform/mac/native-build.sh
bash -n src/cpp/shared/infra/scripts/lib/unix_preset_build.sh

if find assets/ignore-sources/upstream/github-gitignore src/cpp/shared/infra -name .git -print | grep -q .; then
  echo "Error: archive contains broken submodule .git pointer files." >&2
  exit 1
fi

./scripts/kog --help >/dev/null 2>&1

if [[ -x src/shell/test/pre-commit-quality-gate.sh ]]; then
  src/shell/test/pre-commit-quality-gate.sh >/dev/null
else
  if [[ -x src/shell/test/smoke-root-wrapper-generator.sh ]]; then
    src/shell/test/smoke-root-wrapper-generator.sh >/dev/null
  fi

  if [[ -x src/shell/test/audit-public-doc-script-refs.sh ]]; then
    src/shell/test/audit-public-doc-script-refs.sh >/dev/null
  fi
fi

if [[ "${KOG_RELEASE_SMOKE_BUILD:-0}" == "1" ]]; then
  ./scripts/kog self build
fi

echo "PASS: release archive smoke test"
