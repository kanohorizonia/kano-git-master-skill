#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
export KOG_CPP_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

source "$SCRIPT_DIR/../common/windows_preset_build.sh"

OPEN_SOLUTION=0

while [[ $# -gt 0 ]]; do
  case "$1" in
    --open)
      OPEN_SOLUTION=1
      shift
      ;;
    -h|--help)
      cat <<'EOF'
Usage: bash src/cpp/scripts/windows/msbuild-generate-sln.sh [--open]

Generate the Visual Studio solution for the `windows-msbuild` CMake preset.

Options:
  --open      Open the generated .sln in the default Windows handler.
  -h, --help  Show this help message.
EOF
      exit 0
      ;;
    *)
      echo "Unknown argument: $1" >&2
      exit 1
      ;;
  esac
done

SOLUTION_PATH="$(kog_configure_windows_preset "windows-msbuild" "x64")"

if [[ -z "$SOLUTION_PATH" ]]; then
  echo "Visual Studio solution was not found after configure." >&2
  exit 1
fi

echo "Generated solution: $SOLUTION_PATH"

if [[ "$OPEN_SOLUTION" == "1" ]]; then
  cmd.exe /d /c start "" "$SOLUTION_PATH" >/dev/null 2>&1
  echo "Opened solution in Visual Studio shell handler."
fi
