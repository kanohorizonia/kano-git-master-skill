#!/usr/bin/env bash
# =============================================================================
# External Report Skill Adapter
# =============================================================================
# Single unified adapter for kano-cpp-test-skill integration.
# Provides discovery, context setup, and skill invocation.
#
# Usage:
#   source report_skill_adapter.sh
#   report_skill_load          # Discover skill and export context
#   report_skill_package       # Run report packaging via skill
# =============================================================================
set -euo pipefail

# ─────────────────────────────────────────────────────────────────────────────
# Internal: find skill root
# ─────────────────────────────────────────────────────────────────────────────
_report_skill_find_root() {
  local repo_root="${1:-}"
  local parent_root candidate

  if [[ -z "$repo_root" ]]; then
    repo_root="$(git -C "${KOG_REPO_ROOT:-$PWD}" rev-parse --show-toplevel 2>/dev/null || pwd)"
  fi
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

# ─────────────────────────────────────────────────────────────────────────────
# report_skill_load — Discover skill and export context env vars
# ─────────────────────────────────────────────────────────────────────────────
# Sets: KOG_REPO_ROOT, KOG_CPP_ROOT, KANO_WORKSPACE_ROOT, KANO_REPORT_ROOT,
#       KANO_CPP_TEST_SKILL_ROOT
# Exits with error if skill not found.
# ─────────────────────────────────────────────────────────────────────────────
report_skill_load() {
  local skill_root
  local repo_root

  repo_root="$(git -C "${KOG_REPO_ROOT:-$PWD}" rev-parse --show-toplevel 2>/dev/null || pwd)"

  export KOG_REPO_ROOT="$repo_root"
  export KOG_CPP_ROOT="$repo_root/src/cpp"
  export KANO_WORKSPACE_ROOT="$KOG_REPO_ROOT"
  export KOG_REPORT_ROOT="${KOG_REPORT_ROOT:-$repo_root/.kano/tmp/reports}"

  if ! skill_root="$(_report_skill_find_root "$repo_root")"; then
    echo "[ERROR] kano-cpp-test-skill not found." >&2
    echo "[ERROR] Set KANO_CPP_TEST_SKILL_ROOT or KOG_CPP_TEST_SKILL_ROOT," >&2
    echo "[ERROR] or checkout the skill into _tools/kano-cpp-test-skill." >&2
    return 1
  fi

  export KANO_CPP_TEST_SKILL_ROOT="$skill_root"
}

# ─────────────────────────────────────────────────────────────────────────────
# report_skill_package — Run report packaging via external skill
# ─────────────────────────────────────────────────────────────────────────────
# Requires: report_skill_load called first
# Input: KANO_REPORT_SLUG (default: package-all)
# ─────────────────────────────────────────────────────────────────────────────
report_skill_package() {
  if [[ -z "${KANO_CPP_TEST_SKILL_ROOT:-}" ]]; then
    echo "[ERROR] report_skill_load must be called before report_skill_package" >&2
    return 1
  fi
  : "${KANO_REPORT_SLUG:=package-all}"
  export KANO_REPORT_SLUG

  bash "$KANO_CPP_TEST_SKILL_ROOT/src/shell/reports/common/package-reports.sh" "$@"
}
