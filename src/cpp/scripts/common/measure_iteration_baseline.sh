#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CPP_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
REPO_ROOT="$(cd "$CPP_ROOT/../.." && pwd)"
WINDOWS_PRESET_BUILD_SH="$SCRIPT_DIR/../../shared/infra/scripts/common/windows_preset_build.sh"

if [[ "${OSTYPE:-}" == msys* || "${OSTYPE:-}" == cygwin* ]]; then
  if [[ -f "$WINDOWS_PRESET_BUILD_SH" ]]; then
    export KOG_CPP_ROOT="$CPP_ROOT"
    # Reuse existing repo bootstrap logic for vcvars/toolchain setup.
    # shellcheck disable=SC1090
    source "$WINDOWS_PRESET_BUILD_SH"
  fi
fi

usage() {
  cat <<'EOF'
Usage:
  bash src/cpp/scripts/common/measure_iteration_baseline.sh \
    --configure-preset <name> \
    --build-preset <name> \
    --build-dir <path> \
    [--config <Debug|Release|RelWithDebInfo|MinSizeRel>] \
    [--output <csv-path>] \
    [--cpp-file <path>] \
    [--header-file <path>] \
    [--module-file <path>]

Measures iteration build baseline with five cases:
  1) clean-build
  2) no-op-rebuild
  3) cpp-edit-rebuild
  4) header-edit-rebuild
  5) module-edit-rebuild

Notes:
- This script mutates probe files temporarily, then restores original contents.
- Use from repo root for default probe paths.
EOF
}

CONFIGURE_PRESET=""
BUILD_PRESET=""
BUILD_DIR=""
CONFIG=""
OUTPUT_FILE=".kano/tmp/build-iteration/baseline.csv"
CPP_FILE="$CPP_ROOT/code/systems/kano_git_core/commands/private/commands/version_cmd.cpp"
HEADER_FILE="$CPP_ROOT/code/systems/kano_git_core/workspace/public/discovery.hpp"
MODULE_FILE="$CPP_ROOT/code/systems/kano_git_core/commands/public/commands.cppm"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --configure-preset)
      CONFIGURE_PRESET="$2"
      shift 2
      ;;
    --build-preset)
      BUILD_PRESET="$2"
      shift 2
      ;;
    --build-dir)
      BUILD_DIR="$2"
      shift 2
      ;;
    --config)
      CONFIG="$2"
      shift 2
      ;;
    --output)
      OUTPUT_FILE="$2"
      shift 2
      ;;
    --cpp-file)
      CPP_FILE="$2"
      shift 2
      ;;
    --header-file)
      HEADER_FILE="$2"
      shift 2
      ;;
    --module-file)
      MODULE_FILE="$2"
      shift 2
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "Unknown argument: $1" >&2
      usage >&2
      exit 1
      ;;
  esac
done

if [[ -z "$CONFIGURE_PRESET" || -z "$BUILD_PRESET" || -z "$BUILD_DIR" ]]; then
  echo "Missing required args: --configure-preset, --build-preset, --build-dir" >&2
  usage >&2
  exit 1
fi

require_file() {
  local path="$1"
  if [[ ! -f "$path" ]]; then
    echo "Required file not found: $path" >&2
    exit 1
  fi
}

write_csv_header() {
  printf 'case,elapsed_seconds,compile_line_count,dyndep_line_count,scan_line_count\n' > "$OUTPUT_FILE"
}

count_lines() {
  local pattern="$1"
  local log_file="$2"
  (grep -E "$pattern" "$log_file" || true) | wc -l | tr -d ' '
}

resolve_to_repo_root_if_relative() {
  local in_path="$1"
  if [[ "$in_path" = /* || "$in_path" =~ ^[A-Za-z]:[\\/] ]]; then
    printf '%s\n' "$in_path"
    return 0
  fi
  printf '%s\n' "$REPO_ROOT/$in_path"
}

append_probe_marker() {
  local file_path="$1"
  printf '\n// baseline-probe:%s\n' "$(date -u +%Y%m%dT%H%M%SZ)" >> "$file_path"
}

restore_file() {
  local backup="$1"
  local original="$2"
  mv "$backup" "$original"
}

run_configure() {
  echo "[baseline] configure preset=$CONFIGURE_PRESET"
  if [[ "${OSTYPE:-}" == msys* || "${OSTYPE:-}" == cygwin* ]]; then
    echo "[baseline] windows preset flow handles configure during build"
    return 0
  fi
  (
    cd "$CPP_ROOT"
    cmake --preset "$CONFIGURE_PRESET"
  )
}

run_build_case() {
  local case_name="$1"
  local log_file
  log_file="$(mktemp)"

  echo "[baseline] running case=$case_name"
  local start_ts end_ts elapsed
  start_ts="$(date +%s)"

  if [[ -n "$CONFIG" ]]; then
    if [[ "${OSTYPE:-}" == msys* || "${OSTYPE:-}" == cygwin* ]]; then
      kog_run_windows_preset "$CONFIGURE_PRESET" "$BUILD_PRESET" "x64" 2>&1 | tee "$log_file"
    else
      (
        cd "$CPP_ROOT"
        cmake --build --preset "$BUILD_PRESET" --config "$CONFIG"
      ) 2>&1 | tee "$log_file"
    fi
  else
    if [[ "${OSTYPE:-}" == msys* || "${OSTYPE:-}" == cygwin* ]]; then
      kog_run_windows_preset "$CONFIGURE_PRESET" "$BUILD_PRESET" "x64" 2>&1 | tee "$log_file"
    else
      (
        cd "$CPP_ROOT"
        cmake --build --preset "$BUILD_PRESET"
      ) 2>&1 | tee "$log_file"
    fi
  fi

  end_ts="$(date +%s)"
  elapsed=$((end_ts - start_ts))

  local compile_count dyndep_count scan_count
  compile_count="$(count_lines 'Building CXX object|Building C object' "$log_file")"
  dyndep_count="$(count_lines 'Generating CXX dyndep file' "$log_file")"
  scan_count="$(count_lines 'Scanning .* for CXX dependencies' "$log_file")"

  printf '%s,%s,%s,%s,%s\n' "$case_name" "$elapsed" "$compile_count" "$dyndep_count" "$scan_count" >> "$OUTPUT_FILE"
  rm -f "$log_file"
}

run_mutation_case() {
  local case_name="$1"
  local target_file="$2"

  local backup
  backup="$(mktemp)"
  cp "$target_file" "$backup"

  append_probe_marker "$target_file"
  run_build_case "$case_name"

  restore_file "$backup" "$target_file"
}

main() {
  OUTPUT_FILE="$(resolve_to_repo_root_if_relative "$OUTPUT_FILE")"
  BUILD_DIR="$(resolve_to_repo_root_if_relative "$BUILD_DIR")"
  CPP_FILE="$(resolve_to_repo_root_if_relative "$CPP_FILE")"
  HEADER_FILE="$(resolve_to_repo_root_if_relative "$HEADER_FILE")"
  MODULE_FILE="$(resolve_to_repo_root_if_relative "$MODULE_FILE")"

  require_file "$CPP_FILE"
  require_file "$HEADER_FILE"
  require_file "$MODULE_FILE"

  mkdir -p "$(dirname "$OUTPUT_FILE")"

  write_csv_header

  echo "[baseline] cleaning build dir: $BUILD_DIR"
  rm -rf "$BUILD_DIR"

  run_configure
  run_build_case "clean-build"
  run_build_case "no-op-rebuild"
  run_mutation_case "cpp-edit-rebuild" "$CPP_FILE"
  run_mutation_case "header-edit-rebuild" "$HEADER_FILE"
  run_mutation_case "module-edit-rebuild" "$MODULE_FILE"

  echo "[baseline] done. results: $OUTPUT_FILE"
}

main
