#!/usr/bin/env bash

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
TIMESTAMP_UTC="$(date -u +%Y%m%dT%H%M%SZ)"
TMP_ROOT="${TMPDIR:-/tmp}"
CASE_DIR="${TMP_ROOT}/kog-remote-mac-build-${TIMESTAMP_UTC}-$$"
FAKE_BIN_DIR="${CASE_DIR}/bin"
SOURCE_DIR="${CASE_DIR}/src/cpp"
LOG_FILE="${CASE_DIR}/trace.log"
ARTIFACT_ROOT="${CASE_DIR}/artifacts"

assert_contains() {
  local needle="$1"
  if ! grep -F "$needle" "$LOG_FILE" >/dev/null 2>&1; then
    echo "FAIL: expected log to contain: $needle" >&2
    echo "--- trace ---" >&2
    cat "$LOG_FILE" >&2
    exit 1
  fi
}

assert_rsync_count() {
  local expected="$1"
  local actual
  actual="$(grep -c '^rsync ' "$LOG_FILE" || true)"
  if [[ "$actual" != "$expected" ]]; then
    echo "FAIL: expected ${expected} rsync phases, got ${actual}." >&2
    echo "--- trace ---" >&2
    cat "$LOG_FILE" >&2
    exit 1
  fi
}

assert_equals() {
  local expected="$1"
  local actual="$2"
  local label="$3"
  if [[ "$expected" != "$actual" ]]; then
    echo "FAIL: ${label}: expected [${expected}] got [${actual}]" >&2
    exit 1
  fi
}

write_fake_tools() {
  mkdir -p "$FAKE_BIN_DIR"

  cat >"${FAKE_BIN_DIR}/kano-remote-host" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
printf 'krh %s\n' "$*" >> "$LOG_FILE"
printf '{"address_with_user":"builder@example.test","route":"auto"}\n'
EOF

  cat >"${FAKE_BIN_DIR}/hostname" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
printf '%s\n' "local-ci"
EOF

  cat >"${FAKE_BIN_DIR}/rsync" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
printf 'rsync %s\n' "$*" >> "$LOG_FILE"
exit 0
EOF

  cat >"${FAKE_BIN_DIR}/ssh" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
printf 'ssh %s\n' "$*" >> "$LOG_FILE"
cmd="${*: -1}"
case "$cmd" in
  *"echo 'SSH OK'"*)
    printf 'SSH OK\n'
    ;;
  *"cmake not found"*)
    printf '/usr/bin/cmake\n'
    ;;
  *"ninja not found"*)
    printf '/usr/bin/ninja\n'
    ;;
  *)
    :
    ;;
esac
EOF

  chmod +x "${FAKE_BIN_DIR}/kano-remote-host" "${FAKE_BIN_DIR}/hostname" "${FAKE_BIN_DIR}/rsync" "${FAKE_BIN_DIR}/ssh"
}

main() {
  mkdir -p "$SOURCE_DIR" "$ARTIFACT_ROOT"
  : > "$LOG_FILE"
  write_fake_tools

  export PATH="${FAKE_BIN_DIR}:$PATH"
  export LOG_FILE
  export INF_CPP_ROOT="$SOURCE_DIR"
  export LOCAL_HOST_NAME="local-ci"
  export KOG_REMOTE_HOST_GROUP="mac-local"
  export KOG_REMOTE_HOST_ROUTE="auto"

  # shellcheck disable=SC1091
  source "$ROOT_DIR/src/cpp/scripts/macos/remote-build.sh"

  mapfile -t build_artifacts < <(kog_remote_artifact_allowlist build "macos-ninja-clang" "Debug")
  mapfile -t coverage_artifacts < <(kog_remote_artifact_allowlist coverage "macos-ninja-clang-coverage" "Debug")

  assert_equals "out/bin/macos-ninja-clang/debug/" "${build_artifacts[0]:-}" "build artifact #1"
  assert_equals "out/lib/macos-ninja-clang/debug/" "${build_artifacts[1]:-}" "build artifact #2"
  assert_equals "out/coverage/" "${coverage_artifacts[0]:-}" "coverage artifact #1"

  kog_remote_build_macos "macos-ninja-clang" "Debug"
  kog_remote_sync_back_macos_policy "$INF_REMOTE_BUILD_LAST_HOST" "$INF_REMOTE_BUILD_LAST_ROOT" "$ARTIFACT_ROOT" build "macos-ninja-clang" "Debug"
  kog_remote_sync_back_macos_policy "$INF_REMOTE_BUILD_LAST_HOST" "$INF_REMOTE_BUILD_LAST_ROOT" "$ARTIFACT_ROOT" coverage "macos-ninja-clang-coverage" "Debug"

  assert_contains "krh pick mac-local --route auto"
  assert_contains "rsync -avz --delete -e ssh -o StrictHostKeyChecking=no -o ConnectTimeout=10 --exclude out/ --exclude build/ --exclude .git/ --exclude node_modules/ --exclude __pycache__/ --exclude .kano/ --exclude .cache/ ${SOURCE_DIR}/ builder@example.test:${INF_REMOTE_BUILD_LAST_ROOT}/"
  assert_contains "ssh -o StrictHostKeyChecking=no -o ConnectTimeout=10 builder@example.test"
  assert_contains "rsync -avz -e ssh -o StrictHostKeyChecking=no -o ConnectTimeout=10 --relative builder@example.test:${INF_REMOTE_BUILD_LAST_ROOT}/./out/bin/macos-ninja-clang/debug/ ${ARTIFACT_ROOT}/"
  assert_contains "rsync -avz -e ssh -o StrictHostKeyChecking=no -o ConnectTimeout=10 --relative builder@example.test:${INF_REMOTE_BUILD_LAST_ROOT}/./out/lib/macos-ninja-clang/debug/ ${ARTIFACT_ROOT}/"
  assert_contains "rsync -avz -e ssh -o StrictHostKeyChecking=no -o ConnectTimeout=10 --relative builder@example.test:${INF_REMOTE_BUILD_LAST_ROOT}/./out/coverage/ ${ARTIFACT_ROOT}/"
  assert_rsync_count 4

  echo "PASS: remote mac build policy uses pick, host-namespaced remote root, and separate sync-back."
}

main "$@"
