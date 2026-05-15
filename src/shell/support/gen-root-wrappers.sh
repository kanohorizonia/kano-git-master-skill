#!/usr/bin/env bash
#
# gen-root-wrappers.sh - Generate root wrapper scripts from template profiles.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SKILL_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
TEMPLATE_ROOT="$SKILL_ROOT/assets/root-wrapper-templates"
COMMON_DIR="$TEMPLATE_ROOT/common"
PROFILES_DIR="$TEMPLATE_ROOT/profiles"

PROFILE=""
TARGET="."
FORCE=0
DRY_RUN=0

usage() {
  cat <<'EOF'
Usage: gen-root-wrappers.sh --profile <name> [--target <dir>] [--force] [--dry-run]

Options:
  --profile <name>   Wrapper profile name
                     (standalone|oss|repo-passive-mode|repo-passive-mode-with-ai)
  --target <dir>     Target directory for generated wrappers (default: .)
  --force            Overwrite existing files
  --dry-run          Print actions without writing files
  -h, --help         Show help
EOF
}

trim_line() {
  local line="$1"
  line="${line#"${line%%[![:space:]]*}"}"
  line="${line%"${line##*[![:space:]]}"}"
  printf '%s' "$line"
}

is_supported_profile() {
  local profile="$1"
  case "$profile" in
    standalone|oss|repo-passive-mode|repo-passive-mode-with-ai)
      return 0
      ;;
    *)
      return 1
      ;;
  esac
}

ensure_dirs() {
  [[ -d "$COMMON_DIR" ]] || { echo "ERROR: missing common templates: $COMMON_DIR" >&2; exit 1; }
  [[ -d "$PROFILES_DIR" ]] || { echo "ERROR: missing profiles templates: $PROFILES_DIR" >&2; exit 1; }
  [[ -d "$TARGET" ]] || { echo "ERROR: target directory does not exist: $TARGET" >&2; exit 1; }
}

resolve_profile_chain() {
  local profile="$1"
  local -a chain=()
  local -A seen=()
  local current="$profile"
  local base_file base

  while [[ -n "$current" ]]; do
    if [[ -n "${seen["$current"]:-}" ]]; then
      echo "ERROR: profile inheritance cycle detected at '$current'" >&2
      return 1
    fi
    seen["$current"]=1
    chain+=("$current")

    base_file="$PROFILES_DIR/$current/.base-profile"
    if [[ ! -f "$base_file" ]]; then
      break
    fi
    base="$(head -n 1 "$base_file" | tr -d '\r\n')"
    base="$(trim_line "$base")"
    if [[ -z "$base" ]]; then
      break
    fi
    if [[ ! -d "$PROFILES_DIR/$base" ]]; then
      echo "ERROR: base profile '$base' not found for '$current'" >&2
      return 1
    fi
    current="$base"
  done

  local i
  for ((i = ${#chain[@]} - 1; i >= 0; i--)); do
    printf '%s\n' "${chain[$i]}"
  done
}

build_excludes() {
  local -a profiles=("$@")
  local profile exclude_file line
  for profile in "${profiles[@]}"; do
    exclude_file="$PROFILES_DIR/$profile/.exclude-templates"
    [[ -f "$exclude_file" ]] || continue
    while IFS= read -r line || [[ -n "$line" ]]; do
      line="$(trim_line "$line")"
      [[ -n "$line" ]] || continue
      [[ "$line" == \#* ]] && continue
      printf '%s\n' "$line"
    done <"$exclude_file"
  done | sort -u
}

is_excluded_file() {
  local name="$1"
  local -a excludes=("${@:2}")
  local item
  for item in "${excludes[@]}"; do
    if [[ "$name" == "$item" ]]; then
      return 0
    fi
  done
  return 1
}

emit_action() {
  local action="$1"
  local file="$2"
  printf '[%s] %s\n' "$action" "$file"
}

copy_template_dir() {
  local src_dir="$1"
  shift
  local -a excludes=("$@")
  local src_path name dst_path

  shopt -s nullglob
  for src_path in "$src_dir"/*; do
    [[ -f "$src_path" ]] || continue
    name="$(basename "$src_path")"
    [[ "$name" == .* ]] && continue
    if is_excluded_file "$name" "${excludes[@]}"; then
      emit_action "skip-excluded" "$name"
      continue
    fi
    dst_path="$TARGET/$name"
    if [[ -e "$dst_path" && "$FORCE" -ne 1 ]]; then
      emit_action "skip-exists" "$name"
      continue
    fi
    if [[ "$DRY_RUN" -eq 1 ]]; then
      emit_action "copy" "$name"
      continue
    fi
    cp "$src_path" "$dst_path"
    chmod +x "$dst_path" 2>/dev/null || true
    emit_action "copy" "$name"
  done
  shopt -u nullglob
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --profile)
      [[ $# -ge 2 ]] || { echo "ERROR: --profile requires a value" >&2; exit 1; }
      PROFILE="$2"
      shift 2
      ;;
    --target)
      [[ $# -ge 2 ]] || { echo "ERROR: --target requires a value" >&2; exit 1; }
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
      exit 1
      ;;
  esac
done

if [[ -z "$PROFILE" ]]; then
  echo "ERROR: --profile is required" >&2
  usage >&2
  exit 1
fi
if ! is_supported_profile "$PROFILE"; then
  echo "ERROR: unsupported profile: $PROFILE" >&2
  exit 1
fi

ensure_dirs
TARGET="$(cd "$TARGET" && pwd -P)"

mapfile -t PROFILE_CHAIN < <(resolve_profile_chain "$PROFILE")
mapfile -t EXCLUDES < <(build_excludes "$PROFILE")

echo "[gen-root-wrappers] profile=$PROFILE target=$TARGET force=$FORCE dry_run=$DRY_RUN"
echo "[gen-root-wrappers] profile-chain=${PROFILE_CHAIN[*]}"
if [[ "${#EXCLUDES[@]}" -gt 0 ]]; then
  echo "[gen-root-wrappers] excludes=${EXCLUDES[*]}"
fi

copy_template_dir "$COMMON_DIR" "${EXCLUDES[@]}"
for chain_profile in "${PROFILE_CHAIN[@]}"; do
  copy_template_dir "$PROFILES_DIR/$chain_profile" "${EXCLUDES[@]}"
done
