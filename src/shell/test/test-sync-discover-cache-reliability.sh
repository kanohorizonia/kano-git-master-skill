#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SKILL_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
SYNC_SCRIPT="$SKILL_ROOT/commit-tools/sync/smart-sync-origin-latest.sh"

TESTS_RUN=0
TESTS_PASSED=0
TESTS_FAILED=0

report_pass() {
  local message="$1"
  TESTS_RUN=$((TESTS_RUN + 1))
  TESTS_PASSED=$((TESTS_PASSED + 1))
  echo "✓ PASS: $message"
}

report_fail() {
  local message="$1"
  TESTS_RUN=$((TESTS_RUN + 1))
  TESTS_FAILED=$((TESTS_FAILED + 1))
  echo "✗ FAIL: $message"
}

assert_contains() {
  local haystack="$1"
  local needle="$2"
  local message="$3"
  if grep -Fq "$needle" <<<"$haystack"; then
    report_pass "$message"
  else
    report_fail "$message (missing: $needle)"
  fi
}

assert_not_contains() {
  local haystack="$1"
  local needle="$2"
  local message="$3"
  if grep -Fq "$needle" <<<"$haystack"; then
    report_fail "$message (unexpected: $needle)"
  else
    report_pass "$message"
  fi
}

seed_discover_cache() {
  local root="$1"
  local cache_file="$2"
  local marker="$3"
  local generated_epoch="$4"
  local repos_json="$5"

  local gm_mtime
  gm_mtime="$(stat -c %Y "$root/.gitmodules" 2>/dev/null || stat -f %m "$root/.gitmodules" 2>/dev/null || echo 0)"
  mkdir -p "$(dirname "$cache_file")"
  printf '{"version":1,"generated_epoch":%s,"gitmodules_mtime":%s,"marker":"%s","repos":%s}\n' "$generated_epoch" "$gm_mtime" "$marker" "$repos_json" > "$cache_file"
}

resolve_discover_cache_file() {
  local root="$1"
  local stats_file=""
  local discovered_cache_file=""

  stats_file="$(mktemp)"
  bash -lc "source '$SKILL_ROOT/lib/git-helpers.sh'; GITH_DISCOVER_STATS_FILE='$stats_file' GITH_DISCOVER_QUIET=1 GITH_DISCOVER_CACHE=1 GITH_DISCOVER_INCREMENTAL=1 GITH_DISCOVER_METADATA_LEVEL=minimal gith_discover_repos '$root' '6' >/dev/null" >/dev/null 2>&1 || true
  discovered_cache_file="$(sed -n 's/^cache_file=//p' "$stats_file" | head -n1)"
  rm -f "$stats_file"
  printf '%s' "$discovered_cache_file"
}

read_cache_marker() {
  local cache_file="$1"
  sed -n 's/^.*"marker":"\([^"]*\)".*$/\1/p' "$cache_file" | head -n1
}

echo "Testing sync discover cache reliability..."

workspace="$(mktemp -d)"
cleanup() {
  rm -rf "$workspace"
}
trap cleanup EXIT

root_repo="$workspace/root"
mkdir -p "$root_repo"
git init "$root_repo" >/dev/null
git -C "$root_repo" config user.name "Test User"
git -C "$root_repo" config user.email "test@example.com"
git -C "$root_repo" checkout -b feature/work >/dev/null

remote_root="$workspace/remote-root.git"
git init --bare "$remote_root" >/dev/null
git -C "$root_repo" remote add origin "$remote_root"

printf "root\n" > "$root_repo/README.md"
git -C "$root_repo" add README.md
git -C "$root_repo" commit -m "init root" >/dev/null
git -C "$root_repo" push -u origin feature/work >/dev/null

registered_repo="$root_repo/deps/registered"
unregistered_repo="$root_repo/sandbox/unregistered"
mkdir -p "$registered_repo" "$unregistered_repo"

git init "$registered_repo" >/dev/null
git -C "$registered_repo" config user.name "Test User"
git -C "$registered_repo" config user.email "test@example.com"
git -C "$registered_repo" checkout -b branch_v1 >/dev/null
printf "registered\n" > "$registered_repo/file.txt"
git -C "$registered_repo" add file.txt
git -C "$registered_repo" commit -m "init registered" >/dev/null
git -C "$registered_repo" remote add origin "$workspace/remote-registered.git"
git init --bare "$workspace/remote-registered.git" >/dev/null
git -C "$registered_repo" push -u origin branch_v1 >/dev/null

git init "$unregistered_repo" >/dev/null
git -C "$unregistered_repo" config user.name "Test User"
git -C "$unregistered_repo" config user.email "test@example.com"
git -C "$unregistered_repo" checkout -b feature/local >/dev/null
printf "unregistered\n" > "$unregistered_repo/file.txt"
git -C "$unregistered_repo" add file.txt
git -C "$unregistered_repo" commit -m "init unregistered" >/dev/null
git -C "$unregistered_repo" remote add origin "$workspace/remote-unregistered.git"
git init --bare "$workspace/remote-unregistered.git" >/dev/null
git -C "$unregistered_repo" push -u origin feature/local >/dev/null

cat > "$root_repo/.gitmodules" <<'EOF'
[submodule "deps/registered"]
	path = deps/registered
	url = ../remote-registered.git
	branch = branch_v1
EOF

git -C "$root_repo" add .gitmodules
git -C "$root_repo" commit -m "add gitmodules" >/dev/null

cache_file="$(resolve_discover_cache_file "$root_repo")"
cache_dir="$(dirname "$cache_file")"

if [[ -z "$cache_file" ]]; then
  echo "✗ FAIL: failed to resolve discover cache file path"
  exit 1
fi

marker="$(read_cache_marker "$cache_file")"
if [[ -z "$marker" ]]; then
  marker="$(bash -lc "source '$SKILL_ROOT/lib/git-helpers.sh'; gith_compute_discover_marker '$root_repo' 6 node_modules .cache build dist .venv venv __pycache__")"
fi
now_epoch="$(date +%s)"

repos_json_valid="[{\"path\":\"$root_repo\",\"type\":\"root\"},{\"path\":\"$registered_repo\",\"type\":\"registered\"},{\"path\":\"$unregistered_repo\",\"type\":\"unregistered\"}]"
seed_discover_cache "$root_repo" "$cache_file" "$marker" "$now_epoch" "$repos_json_valid"

output_cache_hit="$(GITH_DISCOVER_CACHE_DIR="$cache_dir" GITH_DISCOVER_CACHE=1 GITH_DISCOVER_CACHE_TTL_SECONDS=3600 GITH_DISCOVER_INCREMENTAL=1 bash "$SYNC_SCRIPT" --repo "$root_repo" --target branch --dry-run 2>&1 || true)"
assert_contains "$output_cache_hit" "Discover mode:" "cache hit reports discover mode"
assert_not_contains "$output_cache_hit" "Discover mode: unknown" "cache hit discover mode should not be unknown"
assert_contains "$output_cache_hit" "Repo: deps/registered" "cache hit includes registered repo"
assert_contains "$output_cache_hit" "Repo: sandbox/unregistered" "cache hit includes unregistered repo"
assert_not_contains "$output_cache_hit" "tmp/native-submodule-remove-test" "cache hit avoids unrelated full-scan paths"

seed_discover_cache "$root_repo" "$cache_file" "$marker" "$now_epoch" "{invalid-json"
output_corrupt_cache="$(GITH_DISCOVER_CACHE_DIR="$cache_dir" GITH_DISCOVER_CACHE=1 GITH_DISCOVER_CACHE_TTL_SECONDS=3600 GITH_DISCOVER_INCREMENTAL=1 bash "$SYNC_SCRIPT" --repo "$root_repo" --target branch --dry-run 2>&1 || true)"
assert_contains "$output_corrupt_cache" "Discover mode:" "corrupt cache reports discover mode"
assert_not_contains "$output_corrupt_cache" "Discover mode: unknown" "corrupt cache discover mode should not be unknown"
assert_contains "$output_corrupt_cache" "Repo: deps/registered" "corrupt cache falls back to full scan and still finds registered repo"
assert_contains "$output_corrupt_cache" "Repo: sandbox/unregistered" "corrupt cache falls back to full scan and still finds unregistered repo"

stale_epoch="$((now_epoch - 7200))"
seed_discover_cache "$root_repo" "$cache_file" "stale-marker-mismatch" "$stale_epoch" "$repos_json_valid"
output_stale_cache="$(GITH_DISCOVER_CACHE_DIR="$cache_dir" GITH_DISCOVER_CACHE=1 GITH_DISCOVER_CACHE_TTL_SECONDS=60 GITH_DISCOVER_INCREMENTAL=1 GITH_DISCOVER_MAX_STALE_SECONDS=120 bash "$SYNC_SCRIPT" --repo "$root_repo" --target branch --dry-run 2>&1 || true)"
assert_contains "$output_stale_cache" "Discover mode:" "stale cache invalidation reports discover mode"
assert_not_contains "$output_stale_cache" "Discover mode: unknown" "stale cache discover mode should not be unknown"
assert_contains "$output_stale_cache" "Repo: deps/registered" "stale cache invalidation still discovers registered repo"
assert_contains "$output_stale_cache" "Repo: sandbox/unregistered" "stale cache invalidation still discovers unregistered repo"

# Reclassification case 1: cache labels a registered repo as unregistered.
repos_json_misclassified_reg="[{\"path\":\"$root_repo\",\"type\":\"root\"},{\"path\":\"$registered_repo\",\"type\":\"unregistered\"},{\"path\":\"$unregistered_repo\",\"type\":\"unregistered\"}]"
seed_discover_cache "$root_repo" "$cache_file" "$marker" "$now_epoch" "$repos_json_misclassified_reg"
output_reclass_registered="$(GITH_DISCOVER_CACHE_DIR="$cache_dir" GITH_DISCOVER_CACHE=1 GITH_DISCOVER_CACHE_TTL_SECONDS=3600 GITH_DISCOVER_INCREMENTAL=1 bash "$SYNC_SCRIPT" --repo "$root_repo" --target branch --dry-run 2>&1 || true)"
assert_contains "$output_reclass_registered" "Discover mode:" "misclassified registered repo reports discover mode"
assert_not_contains "$output_reclass_registered" "Discover mode: unknown" "misclassified registered repo discover mode should not be unknown"
assert_contains "$output_reclass_registered" "Branch source: registered .gitmodules branch" "misclassified registered repo is corrected to registered branch source"
assert_contains "$output_reclass_registered" "Repo: deps/registered" "registered repo remains discoverable after reclassification refresh"

# Reclassification case 2: cache labels an unregistered repo as registered.
repos_json_misclassified_unreg="[{\"path\":\"$root_repo\",\"type\":\"root\"},{\"path\":\"$registered_repo\",\"type\":\"registered\"},{\"path\":\"$unregistered_repo\",\"type\":\"registered\"}]"
seed_discover_cache "$root_repo" "$cache_file" "$marker" "$now_epoch" "$repos_json_misclassified_unreg"
output_reclass_unregistered="$(GITH_DISCOVER_CACHE_DIR="$cache_dir" GITH_DISCOVER_CACHE=1 GITH_DISCOVER_CACHE_TTL_SECONDS=3600 GITH_DISCOVER_INCREMENTAL=1 bash "$SYNC_SCRIPT" --repo "$root_repo" --target branch --dry-run 2>&1 || true)"
assert_contains "$output_reclass_unregistered" "Discover mode:" "misclassified unregistered repo reports discover mode"
assert_not_contains "$output_reclass_unregistered" "Discover mode: unknown" "misclassified unregistered repo discover mode should not be unknown"
assert_contains "$output_reclass_unregistered" "Branch source: unregistered current branch" "misclassified unregistered repo is corrected to unregistered branch source"
assert_contains "$output_reclass_unregistered" "Repo: sandbox/unregistered" "unregistered repo remains discoverable after reclassification refresh"

echo "========================================="
echo "Test Summary:"
echo "  Total:  $TESTS_RUN"
echo "  Passed: $TESTS_PASSED"
echo "  Failed: $TESTS_FAILED"
echo "========================================="

if [[ "$TESTS_FAILED" -eq 0 ]]; then
  echo "✓ All tests passed!"
  exit 0
fi

echo "✗ Some tests failed"
exit 1
