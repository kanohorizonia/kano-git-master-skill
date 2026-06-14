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
  local path="$1"
  if command -v sha256sum >/dev/null 2>&1; then
    sha256sum "$path" | awk '{print $1}'
    return 0
  fi
  if command -v shasum >/dev/null 2>&1; then
    shasum -a 256 "$path" | awk '{print $1}'
    return 0
  fi
  if command -v openssl >/dev/null 2>&1; then
    openssl dgst -sha256 "$path" | awk '{print $NF}'
    return 0
  fi
  local ps path_arg
  path_arg="$path"
  if command -v cygpath >/dev/null 2>&1; then
    path_arg="$(cygpath -w "$path")"
  fi
  for ps in pwsh powershell powershell.exe; do
    if command -v "$ps" >/dev/null 2>&1; then
      "$ps" -NoProfile -Command 'param([string]$Path) (Get-FileHash -Algorithm SHA256 -LiteralPath $Path).Hash.ToLowerInvariant()' "$path_arg" | tr -d '\r'
      return 0
    fi
  done
  echo "ERROR: sha256sum, shasum, openssl, or PowerShell is required to hash $path" >&2
  exit 1
}

find_first() {
  local root pattern found seen_key
  local -a roots=("$ARTIFACT_DIR" artifacts artifacts/packages)
  local -a seen=()
  for root in "${roots[@]}"; do
    [ -d "$root" ] || continue
    seen_key="$root"
    if command -v cygpath >/dev/null 2>&1; then
      seen_key="$(cygpath -a "$root")"
    fi
    case " ${seen[*]-} " in
      *" $seen_key "*) continue ;;
    esac
    seen+=("$seen_key")
    for pattern in "$@"; do
      found="$(find "$root" -type f -iname "$pattern" | sort | head -n 1 || true)"
      if [ -n "$found" ]; then
        printf '%s\n' "$found"
        return 0
      fi
    done
  done
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
  echo "ERROR: required macOS release archive not found under $ARTIFACT_DIR, artifacts, or artifacts/packages" >&2
  exit 1
fi
write_install_block

echo "$OUTPUT_DIR"
