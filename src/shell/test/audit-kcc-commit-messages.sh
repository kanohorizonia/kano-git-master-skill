#!/usr/bin/env bash
#
# audit-kcc-commit-messages.sh
# Classify recent commit subjects against the Kano Commit Convention (KCC).
# Report-only by default; use --strict to fail on non-compliant subjects.

set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"

range=""
max_count="50"
strict=false
read_stdin=false
self_test=false

usage() {
  cat <<'USAGE'
Usage: audit-kcc-commit-messages.sh [OPTIONS]

Options:
  --range <rev-range>    Audit this git rev range instead of recent HEAD commits
  --max-count <n>        Limit git log subjects when --range is omitted or broad (default: 50)
  --stdin                Read commit subjects from stdin, one subject per line
  --strict               Exit non-zero when any subject is non-compliant
  --self-test            Run built-in classifier examples
  -h, --help             Show this help

KCC subject shape:
  [Area][Intent] Concise subject (TICKET-ID)
  [Area][Chore] Maintenance subject (NO-TICKET)
USAGE
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --range)
      range="${2:-}"
      shift 2
      ;;
    --max-count)
      max_count="${2:-}"
      shift 2
      ;;
    --stdin)
      read_stdin=true
      shift
      ;;
    --strict)
      strict=true
      shift
      ;;
    --self-test)
      self_test=true
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "ERROR: unknown option: $1" >&2
      usage >&2
      exit 2
      ;;
  esac
done

ticket_re='[A-Z][A-Z0-9]+-(TSK|BUG|FTR|USR|EPIC|ISSUE)-[0-9]+'
kcc_re='^\[([A-Za-z][A-Za-z0-9 -]*)\]\[([A-Za-z][A-Za-z0-9 -]*)\] .+ \((([A-Z][A-Z0-9]+-(TSK|BUG|FTR|USR|EPIC|ISSUE)-[0-9]+)|NO-TICKET)\)$'
ticket_prefix_re='^[A-Z][A-Z0-9]+-(TSK|BUG|FTR|USR|EPIC|ISSUE)-[0-9]+:'

lower() {
  printf '%s' "${1,,}"
}

classify_subject() {
  local subject="$1"
  if [[ "$subject" =~ $kcc_re ]]; then
    local area="${BASH_REMATCH[1]}"
    local intent="${BASH_REMATCH[2]}"
    local ticket="${BASH_REMATCH[3]}"
    local area_l intent_l
    area_l="$(lower "$area")"
    intent_l="$(lower "$intent")"
    if [[ "$ticket" == "NO-TICKET" ]]; then
      if [[ "$area_l" == "submodule" || "$intent_l" == "chore" || "$intent_l" == "maintenance" ]]; then
        echo "allowed-no-ticket"
      else
        echo "no-ticket-non-exception"
      fi
    else
      echo "kcc-compliant"
    fi
    return
  fi

  if [[ "$subject" =~ $ticket_prefix_re || "$subject" =~ $ticket_re || "$subject" == *"NO-TICKET"* ]]; then
    echo "ticket-present-non-kcc"
    return
  fi

  echo "ticketless"
}

run_self_test() {
  local failures=0
  local subject expected actual
  while IFS=$'\t' read -r subject expected; do
    [[ -z "$subject" ]] && continue
    actual="$(classify_subject "$subject")"
    if [[ "$actual" != "$expected" ]]; then
      echo "FAIL: expected '$expected' but got '$actual': $subject" >&2
      failures=$((failures + 1))
    fi
  done <<'CASES'
[Docs][Feature] Revamp README landing page (KOB-TSK-0042)	kcc-compliant
[KOG][BugFix] Keep plan-file commits intent-scoped (KOA-TSK-0149)	kcc-compliant
[Backlog][Task] Link public artifact contract (KG-FTR-0014)	kcc-compliant
KOB-TSK-0042: revamp README landing page	ticket-present-non-kcc
revamp README landing page	ticketless
[Submodule][Chore] Update shared dependency pointers (NO-TICKET)	allowed-no-ticket
[Build][Maintenance] Refresh generated wrappers (NO-TICKET)	allowed-no-ticket
[Docs][Feature] Refresh current command docs (NO-TICKET)	no-ticket-non-exception
CASES

  if [[ "$failures" -gt 0 ]]; then
    return 1
  fi
  echo "PASS: KCC commit message classifier self-test"
}

if [[ "$self_test" == true ]]; then
  run_self_test
  exit $?
fi

subjects=()
if [[ "$read_stdin" == true ]]; then
  while IFS= read -r line || [[ -n "$line" ]]; do
    subjects+=("$line")
  done
else
  cd "$repo_root"
  git_args=(log "--format=%s")
  if [[ -n "$max_count" ]]; then
    git_args+=("--max-count=$max_count")
  fi
  if [[ -n "$range" ]]; then
    git_args+=("$range")
  fi
  while IFS= read -r line || [[ -n "$line" ]]; do
    subjects+=("$line")
  done < <(git "${git_args[@]}")
fi

total=0
kcc=0
non_kcc_ticket=0
ticketless=0
allowed_no_ticket=0
no_ticket_non_exception=0

for subject in "${subjects[@]}"; do
  [[ -z "$subject" ]] && continue
  total=$((total + 1))
  class="$(classify_subject "$subject")"
  case "$class" in
    kcc-compliant) kcc=$((kcc + 1)) ;;
    ticket-present-non-kcc) non_kcc_ticket=$((non_kcc_ticket + 1)) ;;
    ticketless) ticketless=$((ticketless + 1)) ;;
    allowed-no-ticket) allowed_no_ticket=$((allowed_no_ticket + 1)) ;;
    no-ticket-non-exception) no_ticket_non_exception=$((no_ticket_non_exception + 1)) ;;
  esac
  printf '%s\t%s\n' "$class" "$subject"
done

non_compliant=$((non_kcc_ticket + ticketless + no_ticket_non_exception))

echo
echo "KCC audit summary:"
echo "  total: $total"
echo "  kcc-compliant: $kcc"
echo "  allowed-no-ticket: $allowed_no_ticket"
echo "  ticket-present-non-KCC: $non_kcc_ticket"
echo "  ticketless: $ticketless"
echo "  no-ticket-non-exception: $no_ticket_non_exception"

if [[ "$strict" == true && "$non_compliant" -gt 0 ]]; then
  echo "FAIL: KCC audit found $non_compliant non-compliant commit subject(s)" >&2
  exit 1
fi

echo "PASS: KCC audit completed"
