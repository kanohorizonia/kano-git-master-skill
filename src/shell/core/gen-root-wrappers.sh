#!/usr/bin/env bash
#
# gen-root-wrappers.sh
# Copies thin root-level kog wrapper templates into a workspace root.
#
# Usage:
#   src/shell/core/gen-root-wrappers.sh --profile standalone --target .

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SKILL_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
TEMPLATES_ROOT="$SKILL_ROOT/assets/root-wrapper-templates"
COMMON_DIR="$TEMPLATES_ROOT/common"
PROFILES_DIR="$TEMPLATES_ROOT/profiles"

PROFILE="standalone"
TARGET="."
FORCE=0
DRY_RUN=0

usage() {
  cat <<'USAGE'
Usage: gen-root-wrappers.sh [options]

Options:
  --profile <name>      Wrapper profile:
                        - standalone
                        - oss
                        - repo-passive-mode
                        - repo-passive-mode-with-ai
  --target <dir>        Target workspace root (default: current directory)
  --force               Overwrite existing wrapper files
  --dry-run             Preview actions without writing files
  -h, --help            Show help

Examples:
  src/shell/core/gen-root-wrappers.sh --profile standalone --target .
  src/shell/core/gen-root-wrappers.sh --profile oss --target . --force
  src/shell/core/gen-root-wrappers.sh --profile repo-passive-mode --target . --dry-run
USAGE
}

is_valid_profile() {
  case "$1" in
    standalone|oss|repo-passive-mode|repo-passive-mode-with-ai)
      return 0
      ;;
    *)
      return 1
      ;;
  esac
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --profile)
      [[ $# -ge 2 ]] || { echo "ERROR: --profile requires a value" >&2; exit 2; }
      PROFILE="$2"
      shift 2
      ;;
    --target)
      [[ $# -ge 2 ]] || { echo "ERROR: --target requires a value" >&2; exit 2; }
      TARGET="$2"
      shift 2
      ;;
    --force)
      FORCE=1
      shift
      ;;
    --dry-run)
      DRY_RUN=1
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "ERROR: unknown argument: $1" >&2
      usage >&2
      exit 2
      ;;
  esac
done

if ! is_valid_profile "$PROFILE"; then
  echo "ERROR: unsupported profile: $PROFILE" >&2
  usage >&2
  exit 2
fi

if [[ ! -d "$COMMON_DIR" ]]; then
  echo "ERROR: common template directory not found: $COMMON_DIR" >&2
  exit 1
fi

TARGET="$(mkdir -p "$TARGET" && cd "$TARGET" && pwd)"

declare -A EXCLUDE=()
if [[ "$PROFILE" == "repo-passive-mode" ]]; then
  EXCLUDE["kog-commit-with-ai-review.sh"]=1
  EXCLUDE["kog-commit-push-with-ai-review.sh"]=1
  # Historical names kept here so older exclude files do not silently regress.
  EXCLUDE["smart-commit-with-ai-review.sh"]=1
  EXCLUDE["smart-commit-push-with-ai-review.sh"]=1
fi

copy_file() {
  local src="$1"
  local dst_name="$2"
  local dst="$TARGET/$dst_name"

  if [[ -n "${EXCLUDE[$dst_name]:-}" ]]; then
    echo "[gen-root-wrappers] skip excluded: $dst_name"
    return 0
  fi

  if [[ -e "$dst" && "$FORCE" -ne 1 ]]; then
    echo "[gen-root-wrappers] keep existing: $dst_name (use --force to overwrite)"
    return 0
  fi

  if [[ "$DRY_RUN" -eq 1 ]]; then
    echo "[gen-root-wrappers] would copy: $dst_name"
    return 0
  fi

  cp "$src" "$dst"
  chmod +x "$dst"
  echo "[gen-root-wrappers] copied: $dst_name"
}

copy_tree_files() {
  local dir="$1"
  [[ -d "$dir" ]] || return 0

  while IFS= read -r -d '' file; do
    local rel="${file#$dir/}"
    case "$rel" in
      .base-profile|.exclude-templates|.gitkeep)
        continue
        ;;
    esac
    copy_file "$file" "$rel"
  done < <(find "$dir" -maxdepth 1 -type f -print0 | sort -z)
}

copy_tree_files "$COMMON_DIR"

case "$PROFILE" in
  standalone)
    ;;
  oss)
    copy_tree_files "$PROFILES_DIR/oss"
    ;;
  repo-passive-mode)
    copy_tree_files "$PROFILES_DIR/repo-passive-mode"
    ;;
  repo-passive-mode-with-ai)
    copy_tree_files "$PROFILES_DIR/repo-passive-mode-with-ai"
    ;;
esac

if [[ "$DRY_RUN" -ne 1 ]]; then
  printf '%s\n' "$PROFILE" > "$TARGET/.kog-wrapper-profile"
fi

if [[ "$DRY_RUN" -eq 1 ]]; then
  echo "[gen-root-wrappers] dry-run complete: profile=$PROFILE target=$TARGET"
else
  echo "[gen-root-wrappers] complete: profile=$PROFILE target=$TARGET"
fi
