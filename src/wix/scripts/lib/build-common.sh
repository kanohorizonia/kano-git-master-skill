#!/usr/bin/env bash
set -euo pipefail

KANO_WIX_LIB_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
KANO_WIX_SCRIPT_DIR="$(cd -- "${KANO_WIX_LIB_DIR}/.." && pwd)"
KANO_WIX_ROOT_DIR="$(cd -- "${KANO_WIX_SCRIPT_DIR}/.." && pwd)"
KANO_WIX_REPO_ROOT="$(cd -- "${KANO_WIX_ROOT_DIR}/../.." && pwd)"

kano_wix_trim() {
  local value="$1"
  value="${value#"${value%%[![:space:]]*}"}"
  value="${value%"${value##*[![:space:]]}"}"
  printf '%s' "$value"
}

kano_wix_convert_to_msi_product_version() {
  local canonical
  canonical="$(kano_wix_trim "$1")"
  if [[ ! "$canonical" =~ ^([0-9]+)\.([0-9]+)\.([0-9]+)(-[0-9A-Za-z.-]+)?(\+[0-9A-Za-z.-]+)?$ ]]; then
    echo "Canonical VERSION must be semver-like '<major>.<minor>.<patch>[-prerelease][+build]': $canonical" >&2
    exit 1
  fi

  local major="${BASH_REMATCH[1]}"
  local minor="${BASH_REMATCH[2]}"
  local patch="${BASH_REMATCH[3]}"

  if (( major > 255 )); then
    echo "MSI ProductVersion major field must be <= 255: $canonical" >&2
    exit 1
  fi
  if (( minor > 255 )); then
    echo "MSI ProductVersion minor field must be <= 255: $canonical" >&2
    exit 1
  fi
  if (( patch > 65535 )); then
    echo "MSI ProductVersion build field must be <= 65535: $canonical" >&2
    exit 1
  fi

  printf '%s.%s.%s' "$major" "$minor" "$patch"
}

kano_wix_to_windows_path() {
  cygpath -aw "$1"
}

kano_wix_remove_directory_if_exists() {
  local path="$1"
  if [[ -e "$path" ]]; then
    local path_win ps_script
    path_win="$(kano_wix_to_windows_path "$path")"
    ps_script="if (Test-Path -LiteralPath '$path_win') { cmd /c attrib -R /S /D \"$path_win\\*\" | Out-Null; try { Remove-Item -LiteralPath '$path_win' -Recurse -Force -ErrorAction Stop } catch [System.IO.DirectoryNotFoundException] { } catch [System.Management.Automation.ItemNotFoundException] { } }"
    powershell -NoProfile -NonInteractive -Command "$ps_script"
  fi
}

kano_wix_make_unique_temp_subdir() {
  local parent_dir="$1"
  mkdir -p "$parent_dir"
  local unique_id
  unique_id="$(date +%s)-$$-$RANDOM"
  local temp_dir="${parent_dir}/${unique_id}"
  mkdir -p "$temp_dir"
  printf '%s' "$temp_dir"
}

kano_wix_resolve_wix_exe() {
  local wix_exe_override="${WIX_EXE:-}"
  if [[ -n "$wix_exe_override" && -f "$wix_exe_override" ]]; then
    printf '%s' "$wix_exe_override"
    return 0
  fi

  if [[ -n "${WIX:-}" ]]; then
    if [[ -f "$WIX" ]]; then
      printf '%s' "$WIX"
      return 0
    fi
    if [[ -f "$WIX/wix.exe" ]]; then
      printf '%s' "$WIX/wix.exe"
      return 0
    fi
    if [[ -f "$WIX/bin/wix.exe" ]]; then
      printf '%s' "$WIX/bin/wix.exe"
      return 0
    fi
  fi

  if command -v wix >/dev/null 2>&1; then
    command -v wix
    return 0
  fi

  local common_paths=(
    "/c/Program Files/WiX Toolset v6.0/bin/wix.exe"
    "/c/Program Files/WiX Toolset v5.0/bin/wix.exe"
    "${KANO_WIX_ROOT_DIR}/.tools/wix.exe"
  )
  local common_path
  for common_path in "${common_paths[@]}"; do
    if [[ -f "$common_path" ]]; then
      printf '%s' "$common_path"
      return 0
    fi
  done

  if command -v dotnet >/dev/null 2>&1; then
    mkdir -p "${KANO_WIX_ROOT_DIR}/.tools"
    dotnet tool install --tool-path "${KANO_WIX_ROOT_DIR}/.tools" wix --version 6.0.2 >/dev/null
    if [[ -f "${KANO_WIX_ROOT_DIR}/.tools/wix.exe" ]]; then
      printf '%s' "${KANO_WIX_ROOT_DIR}/.tools/wix.exe"
      return 0
    fi
  fi

  echo "WiX v6 CLI not found. Install WiX Toolset v6 so 'wix.exe' is available." >&2
  exit 1
}

kano_wix_ensure_extension() {
  local wix_exe="$1"
  local extension_id="$2"
  local extension_ref="${extension_id}/6.0.2"
  if "$wix_exe" extension list | grep -Fqi "$extension_id"; then
    return 0
  fi
  "$wix_exe" extension add -g "$extension_ref" >/dev/null
}

kano_wix_new_cmd_wrapper() {
  local wrapper_path="$1"
  local target_script_name="$2"
  cat >"$wrapper_path" <<EOF
@echo off
setlocal
set "SCRIPT_DIR=%~dp0"
if defined GIT_BASH_PATH if exist "%GIT_BASH_PATH%" goto run
if exist "%ProgramFiles%\Git\bin\bash.exe" set "GIT_BASH_PATH=%ProgramFiles%\Git\bin\bash.exe"
if not defined GIT_BASH_PATH if exist "%ProgramFiles(x86)%\Git\bin\bash.exe" set "GIT_BASH_PATH=%ProgramFiles(x86)%\Git\bin\bash.exe"
if not defined GIT_BASH_PATH (
  echo Error: Git for Windows bash.exe was not found.>&2
  exit /b 1
)
:run
"%GIT_BASH_PATH%" "%SCRIPT_DIR%${target_script_name}" %*
EOF
}

kano_wix_write_packaged_shell_scripts() {
  local scripts_dir="$1"
  local canonical_version="$2"
  local packaged_source_dir="${KANO_WIX_ROOT_DIR}/payload/scripts"

  kano_wix_remove_directory_if_exists "$scripts_dir"
  mkdir -p "$scripts_dir"

  if [[ ! -d "$packaged_source_dir" ]]; then
    echo "Missing packaged shell script source directory: $packaged_source_dir" >&2
    exit 1
  fi

  kano_wix_copy_dir_if_present "$packaged_source_dir" "$scripts_dir"
  printf '%s\n' "$canonical_version" > "${scripts_dir}/package-version.txt"

  chmod +x "${scripts_dir}/kano-git" "${scripts_dir}/kog"
  kano_wix_new_cmd_wrapper "${scripts_dir}/kano-git.cmd" "kano-git"
  kano_wix_new_cmd_wrapper "${scripts_dir}/kog.cmd" "kog"
}

kano_wix_copy_dir_if_present() {
  local source_dir="$1"
  local dest_dir="$2"
  if [[ -d "$source_dir" ]]; then
    local source_win dest_win ps_script
    source_win="$(kano_wix_to_windows_path "$source_dir")"
    dest_win="$(kano_wix_to_windows_path "$dest_dir")"
    mkdir -p "$dest_dir"
    ps_script="robocopy '$source_win' '$dest_win' /E /NFL /NDL /NJH /NJS /NP > \$null; if (\$LASTEXITCODE -ge 8) { exit \$LASTEXITCODE }; exit 0"
    powershell -NoProfile -NonInteractive -Command "$ps_script"
  fi
}

kano_wix_directory_has_files() {
  local dir="$1"
  if [[ ! -d "$dir" ]]; then
    return 1
  fi
  find "$dir" -type f -print -quit | grep -q .
}

kano_wix_resolve_binary_source() {
  local repo_root="$1"
  local preset="${KANO_WIX_CPP_PRESET:-windows-ninja-msvc}"
  local config="${KANO_WIX_CPP_CONFIG:-release}"
  local override="${KANO_WIX_BINARY_SOURCE:-}"

  if [[ -n "$override" ]]; then
    if [[ -f "$override" ]]; then
      printf '%s' "$override"
      return 0
    fi
    echo "KANO_WIX_BINARY_SOURCE does not exist: $override" >&2
    return 1
  fi

  local preferred="${repo_root}/src/cpp/out/bin/${preset}/${config}/kano-git.exe"
  if [[ -f "$preferred" ]]; then
    printf '%s' "$preferred"
    return 0
  fi

  local candidate=""
  while IFS= read -r candidate; do
    if [[ -n "$candidate" ]]; then
      printf '%s' "$candidate"
      return 0
    fi
  done < <(find "${repo_root}/src/cpp/out/bin" -type f -name 'kano-git.exe' 2>/dev/null | sort)

  echo "Required native binary is missing. Expected default: $preferred" >&2
  echo "Hint: build the Windows CLI first, or set KANO_WIX_BINARY_SOURCE to an explicit kano-git.exe path." >&2
  echo "Hint: available out/bin tree currently contains:" >&2
  find "${repo_root}/src/cpp/out/bin" -maxdepth 4 -type f 2>/dev/null >&2 || true
  return 1
}

kano_wix_stage_payload() {
  local payload_root="$1"
  local architecture="$2"

  kano_wix_remove_directory_if_exists "$payload_root"
  mkdir -p "$payload_root"

  local file_name source_path
  for file_name in VERSION README.md SKILL.md; do
    source_path="${KANO_WIX_REPO_ROOT}/${file_name}"
    if [[ ! -f "$source_path" ]]; then
      echo "Required payload file missing: $source_path" >&2
      exit 1
    fi
    cp "$source_path" "$payload_root/$file_name"
  done

  kano_wix_copy_dir_if_present "${KANO_WIX_REPO_ROOT}/assets" "${payload_root}/assets"
  if [[ "$architecture" != "x64" ]]; then
    echo "Unsupported MSI architecture for payload staging: $architecture" >&2
    exit 1
  fi

  local binary_source
  if ! binary_source="$(kano_wix_resolve_binary_source "$KANO_WIX_REPO_ROOT")"; then
    exit 1
  fi

  mkdir -p "${payload_root}/bin"
  cp "$binary_source" "${payload_root}/bin/kano-git.exe"

  kano_wix_write_packaged_shell_scripts "${payload_root}/scripts" "$(<"${KANO_WIX_REPO_ROOT}/VERSION")"
}

kano_wix_prepare_context() {
  local clean_output="$1"

  KANO_WIX_UPGRADE_CODE="11111111-1111-1111-1111-111111111111"
  KANO_WIX_ARCHITECTURE="x64"
  KANO_WIX_OUTPUT_DIR="${KANO_WIX_ROOT_DIR}/out"
  KANO_WIX_OUTPUT_NAME="kano-git-master-skill.msi"
  KANO_WIX_PRODUCT_FILE="${KANO_WIX_ROOT_DIR}/code/Product.wxs"
  KANO_WIX_VERSION_FILE="${KANO_WIX_REPO_ROOT}/VERSION"

  if [[ ! -f "$KANO_WIX_PRODUCT_FILE" ]]; then
    echo "WiX entrypoint not found: $KANO_WIX_PRODUCT_FILE" >&2
    exit 1
  fi
  if [[ ! -f "$KANO_WIX_VERSION_FILE" ]]; then
    echo "Canonical VERSION file not found: $KANO_WIX_VERSION_FILE" >&2
    exit 1
  fi

  if [[ "$clean_output" == "1" ]]; then
    kano_wix_remove_directory_if_exists "$KANO_WIX_OUTPUT_DIR"
  fi

  KANO_WIX_OUTPUT_DIR="$(mkdir -p "$KANO_WIX_OUTPUT_DIR" && cd "$KANO_WIX_OUTPUT_DIR" && pwd)"
  KANO_WIX_PAYLOAD_ROOT="${KANO_WIX_OUTPUT_DIR}/payload"
  KANO_WIX_MSI_OUTPUT="${KANO_WIX_OUTPUT_DIR}/${KANO_WIX_OUTPUT_NAME}"
  KANO_WIX_PDB_OUTPUT="${KANO_WIX_OUTPUT_DIR}/${KANO_WIX_OUTPUT_NAME%.msi}.wixpdb"
  KANO_WIX_INTERMEDIATE_DIR="$(kano_wix_make_unique_temp_subdir "${KANO_WIX_OUTPUT_DIR}/_wix")"

  KANO_WIX_CANONICAL_VERSION="$(kano_wix_trim "$(<"$KANO_WIX_VERSION_FILE")")"
  if [[ -z "$KANO_WIX_CANONICAL_VERSION" ]]; then
    echo "Canonical VERSION file is empty: $KANO_WIX_VERSION_FILE" >&2
    exit 1
  fi

  KANO_WIX_PRODUCT_VERSION="$(kano_wix_convert_to_msi_product_version "$KANO_WIX_CANONICAL_VERSION")"
  kano_wix_stage_payload "$KANO_WIX_PAYLOAD_ROOT" "$KANO_WIX_ARCHITECTURE"

  KANO_WIX_HAS_ASSETS=0
  if kano_wix_directory_has_files "${KANO_WIX_PAYLOAD_ROOT}/assets"; then
    KANO_WIX_HAS_ASSETS=1
  fi
}

kano_wix_print_context() {
  echo "WiX v6 build pipeline"
  echo "  Entrypoint : $KANO_WIX_PRODUCT_FILE"
  echo "  Output     : $KANO_WIX_MSI_OUTPUT"
  echo "  Payload    : $KANO_WIX_PAYLOAD_ROOT"
  echo "  VERSION    : $KANO_WIX_CANONICAL_VERSION -> MSI $KANO_WIX_PRODUCT_VERSION"
  echo "  Rule       : MSI ProductVersion uses only the first three numeric fields"
  echo "  Scope      : per-user only"
  echo "  UpgradeCode: $KANO_WIX_UPGRADE_CODE"
  echo "  HasAssets  : $KANO_WIX_HAS_ASSETS"
}

kano_wix_run_build() {
  local wix_exe_path
  wix_exe_path="$(kano_wix_resolve_wix_exe)"
  kano_wix_ensure_extension "$wix_exe_path" "WixToolset.Util.wixext"

  local payload_root_win product_file_win msi_output_win pdb_output_win intermediate_dir_win
  payload_root_win="$(kano_wix_to_windows_path "$KANO_WIX_PAYLOAD_ROOT")"
  product_file_win="$(kano_wix_to_windows_path "$KANO_WIX_PRODUCT_FILE")"
  msi_output_win="$(kano_wix_to_windows_path "$KANO_WIX_MSI_OUTPUT")"
  pdb_output_win="$(kano_wix_to_windows_path "$KANO_WIX_PDB_OUTPUT")"
  intermediate_dir_win="$(kano_wix_to_windows_path "$KANO_WIX_INTERMEDIATE_DIR")"

  set -x
  "$wix_exe_path" build "$product_file_win" \
    -ext WixToolset.Util.wixext \
    -arch "$KANO_WIX_ARCHITECTURE" \
    -intermediatefolder "$intermediate_dir_win" \
    -o "$msi_output_win" \
    -pdb "$pdb_output_win" \
    -d "ProductVersion=$KANO_WIX_PRODUCT_VERSION" \
    -d "UpgradeCode=$KANO_WIX_UPGRADE_CODE" \
    -d "HasAssets=$KANO_WIX_HAS_ASSETS" \
    -d "PayloadRoot=$payload_root_win"
  set +x

  echo "MSI created: $KANO_WIX_MSI_OUTPUT"
}
