#!/usr/bin/env bash
#
# acceptance-quickstart-commit-push.sh
# Deterministic acceptance flow for quickstart commit/commit-push scenarios:
# - commit -m (non-AI, plan-first commit-only shorthand)
# - cp -m (non-AI, commit+push)
# - cpa single-change shortcut (auto route to cp -m)
# - commit --plan-file --plan-stage commit (manual plan-driven)
# - commit --agent -m (agent proxy mode + synthesized plan)
# - invalid --plan-file + -m combination must fail

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
TIMESTAMP_UTC="$(date -u +%Y%m%dT%H%M%SZ)"
TMP_ROOT="${KOG_ACCEPTANCE_TMP_ROOT:-${ROOT_DIR}/.kano/tmp/git/acceptance}"
CASE_ROOT="${TMP_ROOT}/kog-quickstart-acceptance-${TIMESTAMP_UTC}-$$"

PASS_COUNT=0
FAIL_COUNT=0
SKIP_COUNT=0

resolve_native_kog_cmd() {
  if [[ -n "${KOG_BIN:-}" ]]; then
    printf '%s\n' "$KOG_BIN"
    return 0
  fi
  local os_name=""
  os_name="$(uname -s 2>/dev/null || true)"
  if [[ "$os_name" == "Linux" ]]; then
    local linux_candidate=""
    linux_candidate="${ROOT_DIR}/src/cpp/build/bin/linux-ninja-gcc/release/kano-git"
    if [[ -x "$linux_candidate" ]]; then
      printf '%s\n' "$linux_candidate"
      return 0
    fi
  fi
  if [[ "$os_name" == "Darwin" ]]; then
    local mac_candidate=""
    mac_candidate="${ROOT_DIR}/src/cpp/build/bin/macos-ninja-clang/release/kano-git"
    if [[ -x "$mac_candidate" ]]; then
      printf '%s\n' "$mac_candidate"
      return 0
    fi
  fi
  if [[ "$os_name" =~ ^(MINGW|MSYS|CYGWIN) || "${OS:-}" == "Windows_NT" ]]; then
    local candidate=""
    candidate="${ROOT_DIR}/src/cpp/build/bin/windows-ninja-msvc/release/kano-git.exe"
    if [[ -f "$candidate" ]]; then
      printf '%s\n' "$candidate"
      return 0
    fi
  fi
  if command -v kano-git >/dev/null 2>&1; then
    command -v kano-git
    return 0
  fi
  if command -v kog >/dev/null 2>&1; then
    command -v kog
    return 0
  fi
  return 1
}

resolve_wrapper_cmd() {
  if [[ -x "${ROOT_DIR}/scripts/kog" ]]; then
    printf '%s\n' "${ROOT_DIR}/scripts/kog"
    return 0
  fi
  return 1
}

wrapper_is_responsive() {
  local wrapper_cmd="$1"
  if ! command -v timeout >/dev/null 2>&1; then
    return 1
  fi
  timeout 20s "$wrapper_cmd" --version >/dev/null 2>&1
}

log_case_result() {
  local name="$1"
  local status="$2"
  local detail="$3"
  if [[ "$status" == "PASS" ]]; then
    PASS_COUNT=$((PASS_COUNT + 1))
  elif [[ "$status" == "SKIP" ]]; then
    SKIP_COUNT=$((SKIP_COUNT + 1))
  else
    FAIL_COUNT=$((FAIL_COUNT + 1))
  fi
  printf '%s: %s - %s\n' "$status" "$name" "$detail"
}

setup_repo_case() {
  local name="$1"
  local case_dir="${CASE_ROOT}/${name}"
  local remote_dir="${case_dir}/remote.git"
  local work_dir="${case_dir}/work"

  mkdir -p "$work_dir" "$remote_dir"
  git -C "$remote_dir" init --bare -q
  git -C "$work_dir" init -q
  git -C "$work_dir" config user.name "kano-acceptance"
  git -C "$work_dir" config user.email "kano-acceptance@example.com"

  printf 'seed\n' > "${work_dir}/README.md"
  git -C "$work_dir" add README.md
  git -C "$work_dir" commit -q -m "chore(test): seed"

  git -C "$work_dir" branch -M main
  git -C "$work_dir" remote add origin "$remote_dir"
  git -C "$work_dir" push -q -u origin main

  # Native secret gate resolves rules from workspace-local skill path.
  mkdir -p "${work_dir}/.agents/skills/kano/kano-git-master-skill/assets/security"
  cat > "${work_dir}/.agents/skills/kano/kano-git-master-skill/assets/security/secret-blacklist.rules" <<EOF
# acceptance minimal rules file (intentionally empty)
EOF

  printf '%s\n' "$work_dir"
}

run_kog_in_repo() {
  local kog_cmd="$1"
  local repo_dir="$2"
  shift 2
  (
    cd "$repo_dir"
    KANO_GIT_MASTER_ROOT="$ROOT_DIR" KANO_GIT_BINARY_PATH="$kog_cmd" KOG_DISABLE_SECRET_GATE=1 "$kog_cmd" "$@"
  )
}

latest_subject() {
  local repo_dir="$1"
  git -C "$repo_dir" log -1 --pretty=%s
}

remote_subject() {
  local remote_dir="$1"
  git --git-dir="$remote_dir" log -1 --pretty=%s refs/heads/main
}

scenario_commit_m() {
  local kog_cmd="$1"
  local repo_dir msg actual plan_count out
  repo_dir="$(setup_repo_case "scenario-commit-m")"
  msg="chore(test): commit-only manual message"

  printf 'case-commit\n' >> "${repo_dir}/README.md"
  out="$(run_kog_in_repo "$kog_cmd" "$repo_dir" commit -m "$msg" 2>&1)"
  actual="$(latest_subject "$repo_dir")"
  plan_count="$(find "${repo_dir}/.kano/cache/git/plans" -maxdepth 1 -type f -name 'message-plan-*.json' 2>/dev/null | wc -l | tr -d ' ')"
  [[ "$actual" == "$msg" ]] || return 1
  [[ "$plan_count" -ge 1 ]] || return 1
  [[ "$out" == *"synthesized plan file:"* ]]
}

scenario_commit_push_m() {
  local kog_cmd="$1"
  local repo_dir msg local_head
  repo_dir="$(setup_repo_case "scenario-cp-m")"
  msg="chore(test): commit-push manual message"

  printf 'case-cp\n' >> "${repo_dir}/README.md"
  run_kog_in_repo "$kog_cmd" "$repo_dir" commit-push -m "$msg" >/dev/null 2>&1

  local_head="$(latest_subject "$repo_dir")"
  [[ "$local_head" == "$msg" ]]
}

scenario_cpa_shortcut() {
  local wrapper_cmd="$1"
  local repo_dir remote_dir msg local_head remote_head
  repo_dir="$(setup_repo_case "scenario-cpa-shortcut")"
  remote_dir="$(cd "${repo_dir}/.." && pwd)/remote.git"
  msg="chore(test): cpa shortcut message"

  printf 'case-cpa\n' >> "${repo_dir}/README.md"
  (
    cd "$repo_dir"
    KANO_GIT_MASTER_ROOT="$ROOT_DIR" \
      KOG_DISABLE_SECRET_GATE=1 \
      KOG_CPA_SHORTCUT_MESSAGE="$msg" \
      KOG_CPA_DISABLE_SINGLE_CHANGE_SHORTCUT=0 \
      "$wrapper_cmd" cpa >/dev/null 2>&1
  )

  local_head="$(latest_subject "$repo_dir")"
  remote_head="$(remote_subject "$remote_dir")"
  [[ "$local_head" == "$msg" && "$remote_head" == "$msg" ]]
}

scenario_manual_plan_commit() {
  local kog_cmd="$1"
  local repo_dir plan_dir plan_file msg actual
  repo_dir="$(setup_repo_case "scenario-manual-plan")"
  msg="chore(test): plan-driven commit message"

  printf 'case-plan\n' >> "${repo_dir}/README.md"
  plan_dir="${repo_dir}/.kano/tmp/git/plans"
  plan_file="${plan_dir}/manual-plan.json"
  mkdir -p "$plan_dir"
  cat > "$plan_file" <<EOF
{
  "meta": {
    "schema_version": "3",
    "plan_id": "plan-manual-001",
    "generated_at_utc": "2026-03-05T00:00:00Z",
    "executed_at_utc": "",
    "base_head_sha": "manual-base-sha",
    "dirty_fingerprint": "manual-dirty-fingerprint",
    "planner": {
      "provider": "manual",
      "ai-model": "manual"
    },
    "review": {
      "verdict": "pass",
      "reason": "manual plan reviewed"
    }
  },
  "stages": {
    "ignore": [],
    "commit": [
      {
        "repo": ".",
        "commits": [
          {
            "message": "${msg}",
            "review": {
              "verdict": "pass",
              "reason": "manual review pass"
            }
          }
        ]
      }
    ],
    "post_sync": []
  }
}
EOF

  run_kog_in_repo "$kog_cmd" "$repo_dir" commit --plan-file "$plan_file" --plan-stage commit >/dev/null 2>&1
  actual="$(latest_subject "$repo_dir")"
  [[ "$actual" == "$msg" ]]
}

scenario_agent_commit_m() {
  local kog_cmd="$1"
  local repo_dir msg actual out
  repo_dir="$(setup_repo_case "scenario-agent-commit-m")"
  msg="chore(test): agent plan-first commit"

  printf 'case-agent-commit\n' >> "${repo_dir}/README.md"
  out="$(
    cd "$repo_dir"
    KANO_GIT_MASTER_ROOT="$ROOT_DIR" \
      KANO_GIT_BINARY_PATH="$kog_cmd" \
      KOG_DISABLE_SECRET_GATE=1 \
      "$kog_cmd" commit --agent codex -m "$msg" 2>&1
  )"
  actual="$(latest_subject "$repo_dir")"
  [[ "$actual" == "$msg" ]] || return 1
  [[ "$out" == *"agent proxy mode: agent=codex review=off"* ]] || return 1
  [[ "$out" == *"synthesized plan file:"* ]]
}

scenario_invalid_plan_file_and_message() {
  local kog_cmd="$1"
  local repo_dir plan_dir plan_file out status
  repo_dir="$(setup_repo_case "scenario-invalid-plan-plus-message")"
  printf 'case-invalid-combo\n' >> "${repo_dir}/README.md"

  plan_dir="${repo_dir}/.kano/tmp/git/plans"
  plan_file="${plan_dir}/manual-plan.json"
  mkdir -p "$plan_dir"
  cat > "$plan_file" <<EOF
{
  "meta": {
    "schema_version": "1",
    "plan_id": "plan-invalid-combo-001",
    "generated_at_utc": "2026-03-05T00:00:00Z",
    "base_head_sha": "manual-base-sha",
    "dirty_fingerprint_pre_ignore": "manual-dirty-fingerprint",
    "dirty_fingerprint": "manual-dirty-fingerprint",
    "planner": {
      "provider": "manual",
      "ai-model": "manual"
    },
    "review": {
      "verdict": "pass",
      "reason": "manual plan reviewed"
    }
  },
  "stages": {
    "commit": [],
    "post_sync": []
  }
}
EOF

  set +e
  out="$(run_kog_in_repo "$kog_cmd" "$repo_dir" commit --plan-file "$plan_file" -m "should fail" 2>&1)"
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

run_optional_case() {
  local name="$1"
  local enable="$2"
  shift 2
  if [[ "$enable" != "1" ]]; then
    log_case_result "$name" "SKIP" "wrapper unavailable"
    return 0
  fi
  run_case "$name" "$@"
}

main() {
  local kog_cmd wrapper_cmd wrapper_enabled
  if ! kog_cmd="$(resolve_native_kog_cmd)"; then
    echo "Error: cannot resolve native kano-git command for current shell/platform." >&2
    echo "Hint: WSL/Linux shells should use Linux build; Windows Git Bash/Cygwin should use .exe build." >&2
    echo "Hint: set KOG_BIN explicitly if you want to override." >&2
    exit 2
  fi
  wrapper_enabled=0
  if wrapper_cmd="$(resolve_wrapper_cmd)"; then
    if wrapper_is_responsive "$wrapper_cmd"; then
      wrapper_enabled=1
    fi
  fi

  mkdir -p "$CASE_ROOT"
  echo "quickstart acceptance root=${CASE_ROOT}"
  echo "native_kog_cmd=${kog_cmd}"
  echo "wrapper_enabled=${wrapper_enabled}"

  run_case "commit -m non-AI commit-only" scenario_commit_m "$kog_cmd"
  run_case "commit-push -m non-AI commit-push" scenario_commit_push_m "$kog_cmd"
  run_optional_case "cpa single-change shortcut" "$wrapper_enabled" scenario_cpa_shortcut "$wrapper_cmd"
  run_case "manual plan-driven commit --plan-file" scenario_manual_plan_commit "$kog_cmd"
  run_case "commit --agent -m agent proxy plan-first" scenario_agent_commit_m "$kog_cmd"
  run_case "commit rejects --plan-file plus -m" scenario_invalid_plan_file_and_message "$kog_cmd"

  echo "summary: pass=${PASS_COUNT} skip=${SKIP_COUNT} fail=${FAIL_COUNT}"
  if [[ "$FAIL_COUNT" -ne 0 ]]; then
    exit 1
  fi
}

main "$@"
