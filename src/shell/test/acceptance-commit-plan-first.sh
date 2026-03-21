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
  git -C "$work_dir" add README.md
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
  local case_dir="$1"
  local message="$2"
  local plan_file="$3"
  python - <<'PY' "$case_dir" "$message" "$plan_file"
import json, pathlib, subprocess, sys

case_dir = pathlib.Path(sys.argv[1])
message = sys.argv[2]
plan_file = pathlib.Path(sys.argv[3])
work = case_dir / "work"
env = {**dict(), **__import__('os').environ}
env["HOME"] = str(case_dir / "home")
env["USERPROFILE"] = str(case_dir / "home")
env["GIT_CONFIG_GLOBAL"] = str(case_dir / "gitconfig")

def run(*args):
    return subprocess.run(args, cwd=str(work), env=env, capture_output=True, text=True, check=True).stdout.strip()

def fnv1a64_hex(text: str) -> str:
    h = 1469598103934665603
    for b in text.encode("utf-8"):
        h ^= b
        h = (h * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return f"{h:016x}"

head = run("git", "rev-parse", "HEAD")
base_head_sha = "ws-head-v2-" + fnv1a64_hex(f".\t{head}\n")
status = run("git", "status", "--porcelain=v2", "--branch", "--untracked-files=normal", "--ignore-submodules=none")
filtered = []
for raw in status.splitlines():
    t = raw.strip()
    if not t:
        continue
    filtered.append(t)
normalized = "\n".join(filtered).strip()
branch_oid = "no-head"
for line in normalized.splitlines():
    if line.startswith("# branch.oid "):
        value = line[len("# branch.oid "):].strip()
        if value and value != "(initial)":
            branch_oid = value
        break
status_fp = "clean" if not normalized else fnv1a64_hex(normalized)
dirty = "ws-dirty-v2-" + fnv1a64_hex(f".|{branch_oid}|{status_fp}\n")

payload = {
    "meta": {
        "schema_version": "1",
        "plan_id": "explicit-" + fnv1a64_hex(base_head_sha + "\n" + dirty + "\n" + message),
        "generated_at_utc": "2026-03-20T00:00:00Z",
        "base_head_sha": base_head_sha,
        "dirty_fingerprint_pre_ignore": dirty,
        "dirty_fingerprint": dirty,
        "planner": {
            "provider": "native",
            "ai-model": "manual-plan",
            "request_id": "manual"
        },
        "review": {
            "verdict": "pass",
            "reason": "manual explicit plan for acceptance"
        }
    },
    "stages": {
        "commit": [
            {
                "repo": ".",
                "commits": [
                    {
                        "message": message,
                        "review": {
                            "verdict": "pass",
                            "reason": "manual explicit plan for acceptance"
                        }
                    }
                ]
            }
        ],
        "post_sync": []
    }
}

plan_file.parent.mkdir(parents=True, exist_ok=True)
plan_file.write_text(json.dumps(payload, indent=2), encoding="utf-8")
PY
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
  write_explicit_plan "$case_dir" "chore(test): explicit plan path" "$plan_file"
  out="$(run_kog_in_case "$kog_cmd" "$case_dir" commit --plan-file "$plan_file" 2>&1)"
  actual="$(latest_subject "$case_dir")"
  [[ "$actual" == "chore(test): explicit plan path" ]] || return 1
  [[ "$out" == *"plan meta: provider=native ai-model=manual-plan"* ]]
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
  write_explicit_plan "$case_dir" "chore(test): invalid combo plan" "$plan_file"
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
