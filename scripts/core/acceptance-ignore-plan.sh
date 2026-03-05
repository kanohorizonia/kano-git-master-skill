#!/usr/bin/env bash
#
# acceptance-ignore-plan.sh
# Deterministic acceptance flow for ignore-plan:
# - baseline ignore gate should fail with artifact-like untracked files
# - plan ignore-init + apply should reduce findings and make gate pass

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
TIMESTAMP_UTC="$(date -u +%Y%m%dT%H%M%SZ)"
TMP_ROOT="${TMPDIR:-/tmp}"
CASE_DIR="${TMP_ROOT}/kog-ignore-plan-acceptance-${TIMESTAMP_UTC}-$$"
PLAN_FILE="${CASE_DIR}/.kano/cache/git/plans/acceptance-plan.json"

resolve_kog_bin() {
  if [[ -n "${KOG_BIN:-}" ]]; then
    printf '%s\n' "$KOG_BIN"
    return 0
  fi
  if [[ -n "${KANO_GIT_BIN:-}" ]]; then
    printf '%s\n' "$KANO_GIT_BIN"
    return 0
  fi
  if command -v kano-git >/dev/null 2>&1; then
    command -v kano-git
    return 0
  fi
  if [[ -x "${ROOT_DIR}/kog" ]]; then
    printf '%s\n' "${ROOT_DIR}/kog"
    return 0
  fi
  return 1
}

run_check_ignore_gate() {
  local bin="$1"
  local root="$2"
  local out_file="$3"
  local err_file="$4"
  set +e
  "$bin" plan verify ignore --workspace-root "$root" --context plan >"$out_file" 2>"$err_file"
  local status=$?
  set -e
  return "$status"
}

run_in_case() {
  local bin="$1"
  shift
  (
    cd "$CASE_DIR"
    "$bin" "$@"
  )
}

count_candidates() {
  local err_file="$1"
  if [[ ! -f "$err_file" ]]; then
    printf '0\n'
    return 0
  fi
  awk '/^  - /{c+=1} END {print c+0}' "$err_file"
}

main() {
  local kog_bin before_exit after_exit before_count after_count
  local before_out before_err after_out after_err

  if ! kog_bin="$(resolve_kog_bin)"; then
    echo "Error: cannot resolve kano-git binary/wrapper. Set KOG_BIN or KANO_GIT_BIN." >&2
    exit 2
  fi

  mkdir -p "${CASE_DIR}/.kano/cache/git/plans"
  git -C "${CASE_DIR}" init -q

  # Create deterministic artifact-like untracked files.
  mkdir -p "${CASE_DIR}/dist" "${CASE_DIR}/node_modules/pkg" "${CASE_DIR}/.cache"
  printf 'log-line\n' > "${CASE_DIR}/dist/build.log"
  printf 'module.exports = 1;\n' > "${CASE_DIR}/node_modules/pkg/index.js"
  printf 'cache\n' > "${CASE_DIR}/.cache/data.tmp"

  before_out="${CASE_DIR}/before.out"
  before_err="${CASE_DIR}/before.err"
  if run_check_ignore_gate "$kog_bin" "$CASE_DIR" "$before_out" "$before_err"; then
    before_exit=0
  else
    before_exit=$?
  fi
  before_count="$(count_candidates "$before_err")"

  run_in_case "$kog_bin" plan new --output "$PLAN_FILE" --force >/dev/null
  run_in_case "$kog_bin" plan ignore-init --plan-file "$PLAN_FILE" --max-per-repo 200 >/dev/null
  run_in_case "$kog_bin" plan verify pre-apply --stage ignore --plan-file "$PLAN_FILE" >/dev/null
  run_in_case "$kog_bin" plan apply --stage ignore --plan-file "$PLAN_FILE" >/dev/null

  after_out="${CASE_DIR}/after.out"
  after_err="${CASE_DIR}/after.err"
  if run_check_ignore_gate "$kog_bin" "$CASE_DIR" "$after_out" "$after_err"; then
    after_exit=0
  else
    after_exit=$?
  fi
  after_count="$(count_candidates "$after_err")"

  echo "ignore-plan acceptance summary"
  echo "case_dir=${CASE_DIR}"
  echo "plan_file=${PLAN_FILE}"
  echo "before_exit=${before_exit}"
  echo "before_candidates=${before_count}"
  echo "after_exit=${after_exit}"
  echo "after_candidates=${after_count}"

  if [[ "$before_exit" -eq 0 ]]; then
    echo "FAIL: baseline ignore gate unexpectedly passed (expected fail)." >&2
    exit 1
  fi
  if [[ "$before_count" -le 0 ]]; then
    echo "FAIL: baseline ignore gate did not report candidates." >&2
    exit 1
  fi
  if [[ "$after_exit" -ne 0 ]]; then
    echo "FAIL: ignore gate still fails after ignore-plan apply." >&2
    exit 1
  fi
  if [[ "$after_count" -ne 0 ]]; then
    echo "FAIL: candidate count after apply is not zero." >&2
    exit 1
  fi

  echo "PASS: ignore-plan apply reduced artifact-like findings and gate now passes."
}

main "$@"
