#!/usr/bin/env bash

set -euo pipefail

kog_repo_root() {
  local script_dir
  script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
  cd -- "$script_dir/../../../.." >/dev/null 2>&1
  pwd
}

kog_cpp_root() {
  local repo_root
  repo_root="$(kog_repo_root)"
  printf '%s\n' "$repo_root/src/cpp"
}

kog_find_cpp_test_skill_root() {
  local repo_root parent_root candidate
  repo_root="$(kog_repo_root)"
  parent_root="$(cd -- "$repo_root/.." >/dev/null 2>&1 && pwd)"

  for candidate in \
    "${KANO_CPP_TEST_SKILL_ROOT:-}" \
    "${KOG_CPP_TEST_SKILL_ROOT:-}" \
    "$repo_root/_tools/kano-cpp-test-skill" \
    "$parent_root/kano-cpp-test-skill"
  do
    [[ -n "$candidate" ]] || continue
    if [[ -f "$candidate/src/shell/reports/common/report-env.sh" ]]; then
      printf '%s\n' "$candidate"
      return 0
    fi
  done

  return 1
}

kog_require_cpp_test_skill_root() {
  local skill_root
  if ! skill_root="$(kog_find_cpp_test_skill_root)"; then
    echo "[ERROR] kano-cpp-test-skill not found." >&2
    echo "[ERROR] Set KANO_CPP_TEST_SKILL_ROOT or KOG_CPP_TEST_SKILL_ROOT, or checkout the skill into _tools/kano-cpp-test-skill." >&2
    return 1
  fi
  printf '%s\n' "$skill_root"
}

kog_default_report_root() {
  local repo_root
  repo_root="$(kog_repo_root)"
  printf '%s\n' "${KOG_REPORT_ROOT:-$repo_root/.kano/tmp/reports}"
}

kog_export_report_context() {
  export KOG_REPO_ROOT="$(kog_repo_root)"
  export KOG_CPP_ROOT="$(kog_cpp_root)"
  export KANO_WORKSPACE_ROOT="$KOG_REPO_ROOT"
  export KANO_REPORT_ROOT="$(kog_default_report_root)"
}
