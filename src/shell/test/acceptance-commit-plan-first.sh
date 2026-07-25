#!/usr/bin/env bash
#
# acceptance-commit-plan-first.sh
# Deterministic acceptance flow for native plan-first commit semantics:
# - commit -m synthesizes a transient plan and commits successfully
# - commit --plan-file applies an explicit plan successfully
# - commit --agent -m uses the same synthesized plan path
# - commit rejects --plan-file combined with -m

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
TIMESTAMP_UTC="$(date -u +%Y%m%dT%H%M%SZ)"
TMP_ROOT="${KOG_ACCEPTANCE_TMP_ROOT:-${ROOT_DIR}/.kano/tmp/git/acceptance}"
CASE_ROOT="${TMP_ROOT}/kog-commit-plan-first-${TIMESTAMP_UTC}-$$"

PASS_COUNT=0
FAIL_COUNT=0

resolve_native_kog_cmd() {
  if [[ -n "${KOG_BIN:-}" ]]; then
    printf '%s\n' "$KOG_BIN"
    return 0
  fi
  local os_name=""
  os_name="$(uname -s 2>/dev/null || true)"
  if [[ "$os_name" =~ ^(MINGW|MSYS|CYGWIN) || "${OS:-}" == "Windows_NT" ]]; then
    local candidate="${ROOT_DIR}/src/cpp/build/bin/windows-ninja-msvc/release/kano-git.exe"
    if [[ -f "$candidate" ]]; then
      printf '%s\n' "$candidate"
      return 0
    fi
  fi
  if [[ "$os_name" == "Linux" ]]; then
    local candidate="${ROOT_DIR}/src/cpp/build/bin/linux-ninja-gcc/release/kano-git"
    if [[ -x "$candidate" ]]; then
      printf '%s\n' "$candidate"
      return 0
    fi
  fi
  if [[ "$os_name" == "Darwin" ]]; then
    local candidate="${ROOT_DIR}/src/cpp/build/bin/macos-ninja-clang/release/kano-git"
    if [[ -x "$candidate" ]]; then
      printf '%s\n' "$candidate"
      return 0
    fi
  fi
  return 1
}

log_case_result() {
  local name="$1"
  local status="$2"
  local detail="$3"
  if [[ "$status" == "PASS" ]]; then
    PASS_COUNT=$((PASS_COUNT + 1))
  else
    FAIL_COUNT=$((FAIL_COUNT + 1))
  fi
  printf '%s: %s - %s\n' "$status" "$name" "$detail"
}

setup_repo_case() {
  local name="$1"
  local case_dir="${CASE_ROOT}/${name}"
  local work_dir="${case_dir}/work"
  local home_dir="${case_dir}/home"

  mkdir -p "$work_dir" "$home_dir/.kano"
  : > "${home_dir}/.kano/kog_config.toml"
  cat > "${case_dir}/gitconfig" <<EOF
[safe]
	directory = ${work_dir}
EOF

  git -C "$work_dir" init -q
  git -C "$work_dir" config user.name "kano-acceptance"
  git -C "$work_dir" config user.email "kano-acceptance@example.com"

  printf 'seed\n' > "${work_dir}/README.md"
  printf '.kano/\n' > "${work_dir}/.gitignore"
  git -C "$work_dir" add .gitignore README.md
  git -C "$work_dir" commit -q -m "chore(test): seed"
  git -C "$work_dir" branch -M main >/dev/null 2>&1 || true

  printf '%s\n' "$case_dir"
}

run_kog_in_case() {
  local kog_cmd="$1"
  local case_dir="$2"
  shift 2
  (
    cd "${case_dir}/work"
    HOME="${case_dir}/home" \
    USERPROFILE="${case_dir}/home" \
    GIT_CONFIG_GLOBAL="${case_dir}/gitconfig" \
    KANO_GIT_MASTER_ROOT="$ROOT_DIR" \
    KANO_GIT_BINARY_PATH="$kog_cmd" \
    KOG_DISABLE_SECRET_GATE=1 \
    "$kog_cmd" "$@"
  )
}

latest_subject() {
  local case_dir="$1"
  git -C "${case_dir}/work" log -1 --pretty=%s
}

write_explicit_plan() {
  local kog_cmd="$1"
  local case_dir="$2"
  local message="$3"
  local plan_file="$4"

  run_kog_in_case "$kog_cmd" "$case_dir" \
    plan new --force --output "$plan_file" >/dev/null
  run_kog_in_case "$kog_cmd" "$case_dir" \
    plan prepare add-commit-entry \
    --plan-file "$plan_file" \
    --repo "." \
    --commit-message "$message" \
    --commit-include "README.md" \
    --commit-review-verdict "pass" \
    --commit-review-reason "manual explicit plan for acceptance" >/dev/null
}

scenario_commit_message_plan_first() {
  local kog_cmd="$1"
  local case_dir out actual
  case_dir="$(setup_repo_case "scenario-commit-message")"
  printf 'commit-message\n' >> "${case_dir}/work/README.md"
  out="$(run_kog_in_case "$kog_cmd" "$case_dir" commit -m "chore(test): commit message plan-first" 2>&1)"
  actual="$(latest_subject "$case_dir")"
  [[ "$actual" == "chore(test): commit message plan-first" ]] || return 1
  [[ "$out" == *"synthesized plan file:"* ]]
}

scenario_commit_plan_file() {
  local kog_cmd="$1"
  local case_dir plan_file out actual
  case_dir="$(setup_repo_case "scenario-commit-plan-file")"
  printf 'commit-plan-file\n' >> "${case_dir}/work/README.md"
  run_kog_in_case "$kog_cmd" "$case_dir" status --format json >/dev/null 2>&1
  plan_file="${case_dir}/explicit-plan.json"
  write_explicit_plan "$kog_cmd" "$case_dir" "chore(test): explicit plan path" "$plan_file"
  out="$(run_kog_in_case "$kog_cmd" "$case_dir" commit --plan-file "$plan_file" 2>&1)"
  actual="$(latest_subject "$case_dir")"
  [[ "$actual" == "chore(test): explicit plan path" ]] || return 1
  [[ "$out" == *"plan meta: provider=native ai-model=deterministic"* ]]
}

scenario_agent_commit_message_plan_first() {
  local kog_cmd="$1"
  local case_dir out actual
  case_dir="$(setup_repo_case "scenario-agent-commit-message")"
  printf 'agent-commit-message\n' >> "${case_dir}/work/README.md"
  out="$(run_kog_in_case "$kog_cmd" "$case_dir" commit --agent codex -m "chore(test): agent message path" 2>&1)"
  actual="$(latest_subject "$case_dir")"
  [[ "$actual" == "chore(test): agent message path" ]] || return 1
  [[ "$out" == *"agent proxy mode: agent=codex review=off"* ]] || return 1
  [[ "$out" == *"synthesized plan file:"* ]]
}

scenario_invalid_plan_file_plus_message() {
  local kog_cmd="$1"
  local case_dir plan_file out status
  case_dir="$(setup_repo_case "scenario-invalid-combo")"
  printf 'invalid-combo\n' >> "${case_dir}/work/README.md"
  plan_file="${case_dir}/explicit-plan.json"
  write_explicit_plan "$kog_cmd" "$case_dir" "chore(test): invalid combo plan" "$plan_file"
  set +e
  out="$(run_kog_in_case "$kog_cmd" "$case_dir" commit --plan-file "$plan_file" -m "should fail" 2>&1)"
  status=$?
  set -e
  [[ "$status" -eq 2 ]] || return 1
  [[ "$out" == *"--plan-file cannot be combined with --message/-m"* ]]
}

run_case() {
  local name="$1"
  shift
  if "$@"; then
    log_case_result "$name" "PASS" "ok"
  else
    log_case_result "$name" "FAIL" "assertion failed"
  fi
}

main() {
  local kog_cmd
  if ! kog_cmd="$(resolve_native_kog_cmd)"; then
    echo "Error: cannot resolve native kano-git command for current shell/platform." >&2
    exit 2
  fi

  mkdir -p "$CASE_ROOT"
  echo "commit plan-first acceptance root=${CASE_ROOT}"
  echo "native_kog_cmd=${kog_cmd}"

  run_case "commit -m synthesizes plan" scenario_commit_message_plan_first "$kog_cmd"
  run_case "commit --plan-file explicit plan" scenario_commit_plan_file "$kog_cmd"
  run_case "commit --agent -m uses synthesized plan" scenario_agent_commit_message_plan_first "$kog_cmd"
  run_case "commit rejects --plan-file plus -m" scenario_invalid_plan_file_plus_message "$kog_cmd"

  echo "summary: pass=${PASS_COUNT} fail=${FAIL_COUNT}"
  if [[ "$FAIL_COUNT" -ne 0 ]]; then
    exit 1
  fi
}

main "$@"
