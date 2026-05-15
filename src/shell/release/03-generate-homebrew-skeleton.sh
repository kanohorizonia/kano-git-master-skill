#!/bin/bash
set -euo pipefail

if [ $# -ne 5 ]; then
  echo "Usage: $0 <repo-root> <tag-name> <repo-slug> <artifact-dir> <output-dir>" >&2
  exit 1
fi

REPO_ROOT="$1"
TAG_NAME="$2"
REPO_SLUG="$3"
ARTIFACT_DIR="$4"
OUTPUT_DIR="$5"

VERSION_TEXT="$(cat "$REPO_ROOT/VERSION")"
mkdir -p "$OUTPUT_DIR"

ARM64_PATH="$(find "$ARTIFACT_DIR" -type f -name '*macos-arm64*.tar.gz' | sort | head -n 1 || true)"
X64_PATH="$(find "$ARTIFACT_DIR" -type f -name '*macos-x64*.tar.gz' | sort | head -n 1 || true)"

calc_sha() {
  python - <<'PY' "$1"
from pathlib import Path
import hashlib, sys
path = Path(sys.argv[1])
print(hashlib.sha256(path.read_bytes()).hexdigest())
PY
}

ARM64_SHA="REPLACE_WITH_ARM64_SHA256"
ARM64_URL="REPLACE_WITH_ARM64_TARBALL_URL"
X64_SHA="REPLACE_WITH_X64_SHA256"
X64_URL="REPLACE_WITH_X64_TARBALL_URL"

if [ -z "$ARM64_PATH" ]; then
  echo "ERROR: required macOS arm64 release archive not found under $ARTIFACT_DIR" >&2
  exit 1
fi

if [ -z "$X64_PATH" ]; then
  echo "ERROR: required macOS x64 release archive not found under $ARTIFACT_DIR" >&2
  exit 1
fi

ARM64_FILE="$(basename "$ARM64_PATH")"
ARM64_URL="https://github.com/${REPO_SLUG}/releases/download/${TAG_NAME}/${ARM64_FILE}"
ARM64_SHA="$(calc_sha "$ARM64_PATH")"

X64_FILE="$(basename "$X64_PATH")"
X64_URL="https://github.com/${REPO_SLUG}/releases/download/${TAG_NAME}/${X64_FILE}"
X64_SHA="$(calc_sha "$X64_PATH")"

cat > "$OUTPUT_DIR/kano-git-master-skill.rb" <<EOF
class KanoGitMasterSkill < Formula
  desc "Kano Git Master Skill release preview"
  homepage "https://github.com/kanohorizonia/kano-git-master-skill"
  version "${VERSION_TEXT}"

  on_macos do
    if Hardware::CPU.arm?
      url "${ARM64_URL}"
      sha256 "${ARM64_SHA}"
    else
      url "${X64_URL}"
      sha256 "${X64_SHA}"
    end
  end

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

echo "$OUTPUT_DIR"
