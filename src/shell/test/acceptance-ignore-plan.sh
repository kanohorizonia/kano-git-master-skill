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
CASE_DIR="${TMP_ROOT%/}/kog-ignore-plan-acceptance-${TIMESTAMP_UTC}-$$"
PLAN_FILE="${CASE_DIR}/.kano/tmp/git/plans/acceptance-plan.json"

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
  KANO_GIT_SKILL_ROOT="$ROOT_DIR" \
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
    KANO_GIT_SKILL_ROOT="$ROOT_DIR" "$bin" "$@"
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
  local allowlisted_exit near_miss_exit legacy_plan legacy_workspace legacy_gate_exit
  local before_out before_err after_out after_err

  if ! kog_bin="$(resolve_kog_bin)"; then
    echo "Error: cannot resolve kano-git binary/wrapper. Set KOG_BIN or KANO_GIT_BIN." >&2
    exit 2
  fi

  mkdir -p "${CASE_DIR}/.kano/tmp/git/plans"
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
  if ! grep -Fq "${ROOT_DIR}/assets/ignore/datasource" "$PLAN_FILE"; then
    echo "FAIL: plan metadata did not use the canonical ignore datasource root." >&2
    exit 1
  fi
  if ! grep -Fq "${ROOT_DIR}/assets/ignore/datasource/manifest.json" "$PLAN_FILE"; then
    echo "FAIL: plan metadata did not use the canonical ignore datasource manifest." >&2
    exit 1
  fi
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

  # The policy's `src/cpp/scripts/**` entry is a legitimate source exception.
  # It must span path segments, while a near-miss path must remain blocked.
  mkdir -p "${CASE_DIR}/src/cpp/scripts/build" "${CASE_DIR}/src/cpp/script/build"
  printf 'source fixture\n' > "${CASE_DIR}/src/cpp/scripts/build/tool.exe"
  if run_check_ignore_gate "$kog_bin" "$CASE_DIR" "${CASE_DIR}/allowlisted.out" "${CASE_DIR}/allowlisted.err"; then
    allowlisted_exit=0
  else
    allowlisted_exit=$?
  fi

  printf 'artifact fixture\n' > "${CASE_DIR}/src/cpp/script/build/tool.exe"
  if run_check_ignore_gate "$kog_bin" "$CASE_DIR" "${CASE_DIR}/near-miss.out" "${CASE_DIR}/near-miss.err"; then
    near_miss_exit=0
  else
    near_miss_exit=$?
  fi

  # Older packaged assets remain readable when the canonical root is absent.
  mkdir -p "${CASE_DIR}/legacy-skill/assets/ignore-sources/local"
  cat > "${CASE_DIR}/legacy-skill/assets/ignore-sources/local/datasource.manifest.json" <<'JSON'
{
  "version": 1,
  "sources": [
    {
      "id": "kano-local-rules",
      "kind": "gitignore",
      "path": "./custom.gitignore",
      "enabled": true
    }
  ]
}
JSON
  printf 'legacy-generated/\n' > "${CASE_DIR}/legacy-skill/assets/ignore-sources/local/custom.gitignore"
  printf 'legacy-source/**\n' > "${CASE_DIR}/legacy-skill/assets/ignore-sources/local/ignore-gate-allowlist.txt"
  legacy_workspace="${CASE_DIR}/legacy-workspace"
  mkdir -p "${legacy_workspace}/legacy-source"
  git -C "$legacy_workspace" init -q
  printf 'legacy source fixture\n' > "${legacy_workspace}/legacy-source/tool.exe"
  legacy_plan="${CASE_DIR}/legacy-plan.json"
  (
    cd "$legacy_workspace"
    KANO_GIT_SKILL_ROOT="${CASE_DIR}/legacy-skill" \
      "$kog_bin" plan new --output "$legacy_plan" --force >/dev/null
  )
  set +e
  KANO_GIT_SKILL_ROOT="${CASE_DIR}/legacy-skill" \
    "$kog_bin" plan verify ignore \
      --workspace-root "$legacy_workspace" \
      --context plan \
      >"${CASE_DIR}/legacy-gate.out" 2>"${CASE_DIR}/legacy-gate.err"
  legacy_gate_exit=$?
  set -e

  echo "ignore-plan acceptance summary"
  echo "case_dir=${CASE_DIR}"
  echo "plan_file=${PLAN_FILE}"
  echo "before_exit=${before_exit}"
  echo "before_candidates=${before_count}"
  echo "after_exit=${after_exit}"
  echo "after_candidates=${after_count}"
  echo "allowlisted_exit=${allowlisted_exit}"
  echo "near_miss_exit=${near_miss_exit}"
  echo "legacy_gate_exit=${legacy_gate_exit}"

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
  if [[ "$allowlisted_exit" -ne 0 ]]; then
    echo "FAIL: canonical ignore-gate glob did not allow a legitimate source path." >&2
    exit 1
  fi
  if [[ "$near_miss_exit" -eq 0 ]]; then
    echo "FAIL: ignore-gate glob matched across the wrong path segment." >&2
    exit 1
  fi
  if ! grep -Fq "${CASE_DIR}/legacy-skill/assets/ignore-sources" "$legacy_plan"; then
    echo "FAIL: legacy packaged ignore datasource root was not preserved." >&2
    exit 1
  fi
  if ! grep -Fq "${CASE_DIR}/legacy-skill/assets/ignore-sources/local/datasource.manifest.json" "$legacy_plan"; then
    echo "FAIL: legacy packaged ignore datasource manifest was not preserved." >&2
    exit 1
  fi
  if [[ "$legacy_gate_exit" -ne 0 ]]; then
    echo "FAIL: legacy packaged ignore-gate policy was not read by the gate." >&2
    cat "${CASE_DIR}/legacy-gate.err" >&2
    exit 1
  fi

  echo "PASS: ignore planning, canonical/legacy assets, and policy glob semantics are valid."
}

main "$@"
