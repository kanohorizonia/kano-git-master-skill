#!/bin/bash
set -euo pipefail

if [ $# -ne 0 ] && [ $# -ne 5 ]; then
  echo "Usage: $0 [<repo-root> <tag-name> <repo-slug> <artifact-dir> <output-dir>]" >&2
  exit 1
fi

REPO_ROOT="${1:-$(pwd)}"
VERSION_TEXT="$(tr -d '[:space:]' < "$REPO_ROOT/VERSION")"
TAG_NAME="${2:-${KANO_RELEASE_TAG:-v${VERSION_TEXT}}}"
REPO_SLUG="${3:-${KANO_GITHUB_REPOSITORY:-kanohorizonia/kano-git-master-skill}}"
ARTIFACT_DIR="${4:-${KANO_PACKAGE_ROOT:-artifacts/packages}}"
OUTPUT_DIR="${5:-${KANO_PACKAGE_MANAGER_RECIPE_ROOT:-Release/package-managers}/homebrew}"
FORMULA_NAME="${KANO_HOMEBREW_FORMULA_NAME:-kano-git-master-skill}"
ASSET_BASE_URL="${KANO_RELEASE_ASSET_BASE_URL:-https://github.com/${REPO_SLUG}/releases/download/${TAG_NAME}}"

calc_sha() {
  python - "$1" <<'PY'
from pathlib import Path
import hashlib
import sys
path = Path(sys.argv[1])
print(hashlib.sha256(path.read_bytes()).hexdigest())
PY
}

find_first() {
  local pattern
  for pattern in "$@"; do
    find "$ARTIFACT_DIR" -type f -name "$pattern" 2>/dev/null | sort | head -n 1
  done | awk 'NF { print; exit }'
}

mkdir -p "$OUTPUT_DIR"

ARM64_PATH="$(find_first '*macos-arm64*.tar.gz' '*mac-arm64*.tar.gz' '*darwin-arm64*.tar.gz')"
X64_PATH="$(find_first '*macos-x64*.tar.gz' '*mac-x64*.tar.gz' '*darwin-x64*.tar.gz' '*macos-amd64*.tar.gz' '*mac-amd64*.tar.gz')"
GENERIC_PATH="$(find_first '*mac*.tar.gz' '*mac*.tar')"

formula_file="$OUTPUT_DIR/${FORMULA_NAME}.rb"
class_name="KanoGitMasterSkill"

write_common_header() {
  cat > "$formula_file" <<EOF
class ${class_name} < Formula
  desc "Kano Git Master Skill"
  homepage "https://github.com/kanohorizonia/kano-git-master-skill"
  version "${VERSION_TEXT}"
EOF
}

write_install_block() {
  cat >> "$formula_file" <<'EOF'

  def install
    libexec.install Dir["*"]
    bin.install_symlink libexec/"bin/kano-git" => "kano-git"
    bin.install_symlink libexec/"bin/kano-git" => "kog"
  end

  test do
    system "#{bin}/kano-git", "version"
  end
end
EOF
}

write_url_block() {
  local path="$1"
  local file sha
  file="$(basename "$path")"
  sha="$(calc_sha "$path")"
  cat >> "$formula_file" <<EOF
  url "${ASSET_BASE_URL%/}/${file}"
  sha256 "${sha}"
EOF
}

write_arch_url_block() {
  local arm_file x64_file arm_sha x64_sha
  arm_file="$(basename "$ARM64_PATH")"
  x64_file="$(basename "$X64_PATH")"
  arm_sha="$(calc_sha "$ARM64_PATH")"
  x64_sha="$(calc_sha "$X64_PATH")"
  cat >> "$formula_file" <<EOF

  on_macos do
    if Hardware::CPU.arm?
      url "${ASSET_BASE_URL%/}/${arm_file}"
      sha256 "${arm_sha}"
    else
      url "${ASSET_BASE_URL%/}/${x64_file}"
      sha256 "${x64_sha}"
    end
  end
EOF
}

write_common_header
if [ -n "$ARM64_PATH" ] && [ -n "$X64_PATH" ]; then
  write_arch_url_block
elif [ -n "$GENERIC_PATH" ]; then
  write_url_block "$GENERIC_PATH"
else
  echo "ERROR: required macOS release archive not found under $ARTIFACT_DIR" >&2
  exit 1
fi
write_install_block

echo "$OUTPUT_DIR"
