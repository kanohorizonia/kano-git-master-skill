#!/usr/bin/env bash
#
# gen-root-wrappers.sh - Generate project root wrapper scripts from profile templates

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
SKILL_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd -P)"
TEMPLATES_ROOT="$SKILL_ROOT/assets/root-wrapper-templates"

PROFILE="standalone"
TARGET_DIR="$(pwd -P)"
FORCE=0
DRY_RUN=0

usage() {
  cat <<'USAGE'
Usage: gen-root-wrappers.sh [options]

Options:
  --profile <standalone|oss|repo-passive-mode|repo-passive-mode-with-ai>  Template profile (default: standalone)
  --target <dir>              Target project root directory (default: current directory)
  --force                     Overwrite existing smart-*.sh files
  --dry-run                   Print planned actions without writing files
  -h, --help                  Show this help

Examples:
  ./.agents/kano/kano-git-master-skill/scripts/core/gen-root-wrappers.sh
  ./.agents/kano/kano-git-master-skill/scripts/core/gen-root-wrappers.sh --profile oss --target /path/to/repo
  ./.agents/kano/kano-git-master-skill/scripts/core/gen-root-wrappers.sh --profile standalone --force
  ./.agents/kano/kano-git-master-skill/scripts/core/gen-root-wrappers.sh --profile repo-passive-mode --target .
  ./.agents/kano/kano-git-master-skill/scripts/core/gen-root-wrappers.sh --profile repo-passive-mode-with-ai --target .
USAGE
}

log() {
  echo "[gen-root-wrappers] $*"
}

die() {
  echo "ERROR: $*" >&2
  exit 1
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --profile)
      [[ $# -ge 2 ]] || die "--profile requires a value"
      PROFILE="$2"
      shift 2
      ;;
    --target)
      [[ $# -ge 2 ]] || die "--target requires a value"
      TARGET_DIR="$2"
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
      die "Unknown argument: $1"
      ;;
  esac
done

case "$PROFILE" in
  standalone|oss|repo-passive-mode|repo-passive-mode-with-ai)
    ;;
  *)
    die "Unsupported profile: $PROFILE (expected: standalone, oss, repo-passive-mode, or repo-passive-mode-with-ai)"
    ;;
esac

TARGET_DIR="$(cd "$TARGET_DIR" && pwd -P)"
[[ -d "$TARGET_DIR" ]] || die "Target directory does not exist: $TARGET_DIR"

COMMON_DIR="$TEMPLATES_ROOT/common"
PROFILE_DIR="$TEMPLATES_ROOT/profiles/$PROFILE"
[[ -d "$COMMON_DIR" ]] || die "Common template directory not found: $COMMON_DIR"
[[ -d "$PROFILE_DIR" ]] || die "Profile template directory not found: $PROFILE_DIR"

BASE_PROFILE_DIR=""
BASE_PROFILE_FILE="$PROFILE_DIR/.base-profile"
if [[ -f "$BASE_PROFILE_FILE" ]]; then
  BASE_PROFILE="$(sed -n '1p' "$BASE_PROFILE_FILE" | tr -d '[:space:]')"
  [[ -n "$BASE_PROFILE" ]] || die "Base profile file is empty: $BASE_PROFILE_FILE"
  BASE_PROFILE_DIR="$TEMPLATES_ROOT/profiles/$BASE_PROFILE"
  [[ -d "$BASE_PROFILE_DIR" ]] || die "Base profile directory not found: $BASE_PROFILE_DIR"
fi

mapfile -t template_names < <(
  {
    find "$COMMON_DIR" -maxdepth 1 -type f -name 'smart-*.sh' -exec basename {} \;
    if [[ -n "$BASE_PROFILE_DIR" ]]; then
      find "$BASE_PROFILE_DIR" -maxdepth 1 -type f -name 'smart-*.sh' -exec basename {} \;
    fi
    find "$PROFILE_DIR" -maxdepth 1 -type f -name 'smart-*.sh' -exec basename {} \;
  } | sort -u
)

EXCLUDE_FILE="$PROFILE_DIR/.exclude-templates"
if [[ -f "$EXCLUDE_FILE" ]]; then
  mapfile -t exclude_names < <(sed -e 's/#.*$//' -e 's/^\s*//' -e 's/\s*$//' "$EXCLUDE_FILE" | awk 'NF')
  if [[ ${#exclude_names[@]} -gt 0 ]]; then
    filtered=()
    for name in "${template_names[@]}"; do
      skip=0
      for ex in "${exclude_names[@]}"; do
        if [[ "$name" == "$ex" ]]; then
          skip=1
          break
        fi
      done
      if [[ "$skip" -eq 0 ]]; then
        filtered+=("$name")
      fi
    done
    template_names=("${filtered[@]}")
  fi
fi
[[ ${#template_names[@]} -gt 0 ]] || die "No templates found for profile: $PROFILE"

log "profile=$PROFILE"
log "target=$TARGET_DIR"

copied=0
skipped=0
for name in "${template_names[@]}"; do
  src="$COMMON_DIR/$name"
  if [[ -n "$BASE_PROFILE_DIR" && -f "$BASE_PROFILE_DIR/$name" ]]; then
    src="$BASE_PROFILE_DIR/$name"
  fi
  if [[ -f "$PROFILE_DIR/$name" ]]; then
    src="$PROFILE_DIR/$name"
  fi
  dst="$TARGET_DIR/$name"

  if [[ -e "$dst" && "$FORCE" -eq 0 ]]; then
    log "skip  $name (already exists; use --force to overwrite)"
    skipped=$((skipped + 1))
    continue
  fi

  log "write $name"
  if [[ "$DRY_RUN" -eq 0 ]]; then
    cp "$src" "$dst"
    chmod +x "$dst"
  fi
  copied=$((copied + 1))
done

log "done copied=$copied skipped=$skipped dry_run=$DRY_RUN"
log "note: commit wrappers include smart-commit.sh and smart-commit-with-ai-review.sh when available"
