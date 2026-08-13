#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd -- "$script_dir/../../.." && pwd)"
repo_script="src/shell/test/ci-linux-audit-runtime.sh"

# shellcheck disable=SC1091
source "$repo_root/src/cpp/shared/infra/scripts/lib/linux_ci_runner.sh"

if ! kano_cpp_linux_ci_is_linux_host; then
  kano_cpp_linux_ci_exec_via_docker "$repo_script" "$@"
  exit $?
fi

configure_preset="$(kano_cpp_linux_ci_release_configure_preset)"
build_preset="$(kano_cpp_linux_ci_release_build_preset)"
build_root="$repo_root/src/cpp/out/obj/$configure_preset"

if [[ ! -f "$build_root/CMakeCache.txt" ]]; then
  kano_cpp_linux_ci_run_release_build
fi

cmake --build \
  --preset "$build_preset" \
  --target kog_runtime_artifact
cmake \
  "-DKOG_AUDIT_RUNTIME_ARTIFACT_DIR=$build_root/runtime-artifact" \
  "-DKOG_AUDIT_SCHEMA_ROOT=$repo_root/assets/audit/schemas" \
  -P "$repo_root/src/cpp/code/apps/kano_git_cli/verify-audit-runtime.cmake"
