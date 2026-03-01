#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SKILL_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"

RUNS=3
REPO_PATH="."
OPERATIONS_CSV="commit,sync,push,commit-push"
OUTPUT_FILE=""
KEEP_LOGS=0
NATIVE_BIN=""
COMMIT_MESSAGE="bench: noop message"

usage() {
  cat <<EOF
Usage: $(basename "$0") [options]

Benchmark shell scripts vs native C++ commands (dry-run safe defaults).

Options:
  --runs <n>             Number of runs per operation/implementation (default: 3)
  --repo <path>          Target repo path (default: .)
  --ops <csv>            Operations: commit,sync,push,commit-push (default: all)
  --output <path>        TSV output file path (default: tmp/benchmarks/auto timestamp)
  --native-bin <path>    Native kano-git binary path (auto-detect if omitted)
  --message <text>       Commit message for dry-run commit ops
  --keep-logs            Keep per-run stdout/stderr logs in tmp/benchmarks/logs
  -h, --help             Show help

Examples:
  bash src/shell/test/benchmark-shell-vs-native.sh
  bash src/shell/test/benchmark-shell-vs-native.sh --runs 5 --ops sync,push
  bash src/shell/test/benchmark-shell-vs-native.sh --repo . --output tmp/bench.tsv
EOF
}

now_ms() {
  local out=""
  if out="$(date +%s%3N 2>/dev/null)" && [[ "$out" =~ ^[0-9]+$ ]]; then
    printf '%s\n' "$out"
    return 0
  fi

  if command -v python3 >/dev/null 2>&1; then
    python3 - <<'PY'
import time
print(int(time.time() * 1000))
PY
    return 0
  fi

  if command -v python >/dev/null 2>&1; then
    python - <<'PY'
import time
print(int(time.time() * 1000))
PY
    return 0
  fi

  printf '%s\n' $(( $(date +%s) * 1000 ))
}

ensure_output_file() {
  if [[ -z "$OUTPUT_FILE" ]]; then
    local ts
    ts="$(date +%Y%m%d-%H%M%S)"
    OUTPUT_FILE="$SKILL_ROOT/tmp/benchmarks/shell-vs-native-$ts.tsv"
  fi
  mkdir -p "$(dirname "$OUTPUT_FILE")"

  cat > "$OUTPUT_FILE" <<'EOF'
operation	implementation	run	exit_code	elapsed_ms
EOF
}

trim() {
  local val="$1"
  val="${val#${val%%[![:space:]]*}}"
  val="${val%${val##*[![:space:]]}}"
  printf '%s' "$val"
}

join_for_display() {
  local out=""
  local arg=""
  for arg in "$@"; do
    if [[ -n "$out" ]]; then
      out+=" "
    fi
    out+="$arg"
  done
  printf '%s\n' "$out"
}

resolve_native_bin() {
  if [[ -n "$NATIVE_BIN" ]]; then
    if [[ ! -f "$NATIVE_BIN" ]]; then
      echo "ERROR: --native-bin file not found: $NATIVE_BIN" >&2
      exit 1
    fi
    return 0
  fi

  local candidates=(
    "$SKILL_ROOT/src/cpp/build/bin/windows-ninja-msvc/release/kano-git.exe"
    "$SKILL_ROOT/src/cpp/build/bin/linux-ninja-clang/release/kano-git"
    "$SKILL_ROOT/src/cpp/build/bin/linux-ninja-gcc/release/kano-git"
  )

  local candidate=""
  for candidate in "${candidates[@]}"; do
    if [[ -f "$candidate" ]]; then
      NATIVE_BIN="$candidate"
      return 0
    fi
  done

  candidate="$(find "$SKILL_ROOT/src/cpp/build/bin" -type f \( -name 'kano-git' -o -name 'kano-git.exe' \) 2>/dev/null | head -n 1 || true)"
  if [[ -n "$candidate" && -f "$candidate" ]]; then
    NATIVE_BIN="$candidate"
    return 0
  fi

  echo "ERROR: Native binary not found." >&2
  echo "Hint: run: bash scripts/kano-git build" >&2
  exit 1
}

validate_operations() {
  local ops_csv="$1"
  local op=""
  IFS=',' read -r -a OPS <<< "$ops_csv"
  for op in "${OPS[@]}"; do
    op="$(trim "$op")"
    case "$op" in
      commit|sync|push|commit-push)
        ;;
      *)
        echo "ERROR: Unsupported operation in --ops: $op" >&2
        echo "Supported: commit,sync,push,commit-push" >&2
        exit 1
        ;;
    esac
  done
}

record_result() {
  local op="$1"
  local impl="$2"
  local run_id="$3"
  local code="$4"
  local elapsed="$5"
  printf '%s\t%s\t%s\t%s\t%s\n' "$op" "$impl" "$run_id" "$code" "$elapsed" >> "$OUTPUT_FILE"
}

run_timed() {
  local op="$1"
  local impl="$2"
  local run_id="$3"
  shift 3

  local log_base="$SKILL_ROOT/tmp/benchmarks/logs/${op}-${impl}-run${run_id}"
  mkdir -p "$(dirname "$log_base")"

  local start end elapsed status
  start="$(now_ms)"
  set +e
  "$@" >"${log_base}.out" 2>"${log_base}.err"
  status=$?
  set -e
  end="$(now_ms)"
  elapsed=$((end - start))

  record_result "$op" "$impl" "$run_id" "$status" "$elapsed"

  if [[ "$status" -ne 0 ]]; then
    echo "[WARN] ${op}/${impl} run=${run_id} failed (exit=${status}, elapsed=${elapsed}ms)" >&2
    tail -n 12 "${log_base}.err" >&2 || true
  fi

  if [[ "$KEEP_LOGS" -ne 1 ]]; then
    rm -f "${log_base}.out" "${log_base}.err"
  fi
}

run_native_op() {
  local op="$1"
  case "$op" in
    commit)
      run_timed "$op" "native" "$2" "$NATIVE_BIN" commit --preflight-only --profile --no-recursive
      ;;
    sync)
      run_timed "$op" "native" "$2" "$NATIVE_BIN" sync origin-latest --repo "$REPO_PATH" --dry-run --no-recursive --profile
      ;;
    push)
      run_timed "$op" "native" "$2" "$NATIVE_BIN" push --fetch-only --dry-run --no-recursive --profile
      ;;
    commit-push)
      run_timed "$op" "native" "$2" "$NATIVE_BIN" commit-push --dry-run --no-recursive --profile -m "$COMMIT_MESSAGE"
      ;;
  esac
}

run_shell_op() {
  local op="$1"
  case "$op" in
    commit)
      run_timed "$op" "shell" "$2" bash "$SKILL_ROOT/src/shell/commit-tools/commit/smart-commit-noai.sh" -m "$COMMIT_MESSAGE" --dry-run --repos "."
      ;;
    sync)
      run_timed "$op" "shell" "$2" bash "$SKILL_ROOT/src/shell/commit-tools/sync/smart-sync-origin-latest.sh" --repo "$REPO_PATH" --dry-run
      ;;
    push)
      run_timed "$op" "shell" "$2" bash "$SKILL_ROOT/src/shell/commit-tools/smart-push.sh" --sync-only --dry-run --repos "."
      ;;
    commit-push)
      run_timed "$op" "shell" "$2" bash "$SKILL_ROOT/src/shell/commit-tools/commit-push/smart-commit-push.sh" --dry-run -m "$COMMIT_MESSAGE" --repos "."
      ;;
  esac
}

print_summary() {
  echo ""
  echo "=== Benchmark Summary (avg elapsed ms) ==="
  awk -F'\t' '
    NR==1 {next}
    {
      key=$1"|"$2
      sum[key]+=$5
      count[key]+=1
      if ($4 != 0) fail[key]+=1
    }
    END {
      printf "%-14s  %-8s  %-8s  %-8s  %s\n", "operation", "impl", "avg_ms", "fails", "runs"
      printf "%-14s  %-8s  %-8s  %-8s  %s\n", "---------", "----", "------", "-----", "----"
      for (k in count) {
        split(k, p, "|")
        avg=int(sum[k]/count[k])
        f=(k in fail ? fail[k] : 0)
        printf "%-14s  %-8s  %-8d  %-8d  %d\n", p[1], p[2], avg, f, count[k]
      }
    }
  ' "$OUTPUT_FILE" | sort

  echo ""
  echo "Raw TSV: $OUTPUT_FILE"
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --runs)
      RUNS="${2:-}"
      shift 2
      ;;
    --repo)
      REPO_PATH="${2:-}"
      shift 2
      ;;
    --ops)
      OPERATIONS_CSV="${2:-}"
      shift 2
      ;;
    --output)
      OUTPUT_FILE="${2:-}"
      shift 2
      ;;
    --native-bin)
      NATIVE_BIN="${2:-}"
      shift 2
      ;;
    --message)
      COMMIT_MESSAGE="${2:-}"
      shift 2
      ;;
    --keep-logs)
      KEEP_LOGS=1
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "ERROR: Unknown argument: $1" >&2
      usage >&2
      exit 1
      ;;
  esac
done

if ! [[ "$RUNS" =~ ^[1-9][0-9]*$ ]]; then
  echo "ERROR: --runs must be a positive integer" >&2
  exit 1
fi

if [[ ! -d "$REPO_PATH" ]]; then
  echo "ERROR: --repo path not found: $REPO_PATH" >&2
  exit 1
fi

validate_operations "$OPERATIONS_CSV"
resolve_native_bin
ensure_output_file

echo "=== Shell vs Native Benchmark ==="
echo "repo: $REPO_PATH"
echo "runs per op: $RUNS"
echo "operations: $OPERATIONS_CSV"
echo "native binary: $NATIVE_BIN"
echo "output: $OUTPUT_FILE"

action_op=""
run_id=0
IFS=',' read -r -a OPS <<< "$OPERATIONS_CSV"
for action_op in "${OPS[@]}"; do
  action_op="$(trim "$action_op")"
  echo ""
  echo "[bench] operation=$action_op"
  for ((run_id=1; run_id<=RUNS; run_id++)); do
    run_native_op "$action_op" "$run_id"
    run_shell_op "$action_op" "$run_id"
    echo "  run=$run_id native+shell complete"
  done
done

print_summary
