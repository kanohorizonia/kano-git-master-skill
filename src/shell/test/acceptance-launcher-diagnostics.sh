#!/usr/bin/env bash

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
CASE_ROOT="${ROOT_DIR}/.kano/tmp/launcher-diagnostics-$$"

cleanup() {
  rm -rf "$CASE_ROOT"
}
trap cleanup EXIT

mkdir -p \
  "$CASE_ROOT/scripts" \
  "$CASE_ROOT/fake-bin" \
  "$CASE_ROOT/src/cpp/out/release" \
  "$CASE_ROOT/src/cpp/shared/infra/scripts/lib"

cp "$ROOT_DIR/scripts/kog" "$CASE_ROOT/scripts/kog"
cp "$ROOT_DIR/scripts/kano-git" "$CASE_ROOT/scripts/kano-git"

cat > "$CASE_ROOT/fake-bin/pixi" <<'EOF'
#!/usr/bin/env bash
exit 0
EOF

cat > "$CASE_ROOT/fake-bin/uname" <<'EOF'
#!/usr/bin/env bash
if [[ -n "${KOG_TEST_UNAME:-}" ]]; then
  printf '%s\n' "$KOG_TEST_UNAME"
else
  command -p uname "$@"
fi
EOF
chmod +x "$CASE_ROOT/fake-bin/pixi" "$CASE_ROOT/fake-bin/uname"

cat > "$CASE_ROOT/src/cpp/shared/infra/scripts/lib/pixi_bootstrap.sh" <<'EOF'
kano_pixi_bootstrap_expose_global_tools() {
  return 0
}

kano_pixi_bootstrap_activate() {
  echo "[pixi-bootstrap] fixture activated" >&2
  if [[ "${KOG_TEST_BOOTSTRAP_FAIL:-0}" == "1" ]]; then
    return 41
  fi
  return 0
}
EOF

cat > "$CASE_ROOT/src/cpp/out/release/kano-git" <<'EOF'
#!/usr/bin/env bash
printf 'native:%s\n' "$*"
EOF
cp "$CASE_ROOT/src/cpp/out/release/kano-git" "$CASE_ROOT/src/cpp/out/release/kano-git.exe"
chmod +x \
  "$CASE_ROOT/src/cpp/out/release/kano-git" \
  "$CASE_ROOT/src/cpp/out/release/kano-git.exe"

run_launcher() {
  PATH="$CASE_ROOT/fake-bin:$PATH" \
    KANO_GIT_BINARY_PATH="" \
    KANO_GIT_BIN="" \
    bash "$CASE_ROOT/scripts/kog" --help
}

KOG_DEBUG=0 run_launcher >"$CASE_ROOT/quiet.out" 2>"$CASE_ROOT/quiet.err"
grep -q '^native:--help$' "$CASE_ROOT/quiet.out"
! grep -q '\[pixi-bootstrap\]' "$CASE_ROOT/quiet.err"
! grep -q 'Checking:' "$CASE_ROOT/quiet.err"

KOG_DEBUG=1 run_launcher >"$CASE_ROOT/debug.out" 2>"$CASE_ROOT/debug.err"
grep -q '^native:--help$' "$CASE_ROOT/debug.out"
grep -q '\[pixi-bootstrap\] fixture activated' "$CASE_ROOT/debug.err"
grep -q 'Checking:' "$CASE_ROOT/debug.err"

KOG_TEST_UNAME=MINGW64_NT-10.0 KOG_DEBUG=0 run_launcher \
  >"$CASE_ROOT/windows-quiet.out" 2>"$CASE_ROOT/windows-quiet.err"
grep -q '^native:--help$' "$CASE_ROOT/windows-quiet.out"
! grep -q 'Checking:' "$CASE_ROOT/windows-quiet.err"

KOG_TEST_UNAME=MINGW64_NT-10.0 KOG_DEBUG=1 run_launcher \
  >"$CASE_ROOT/windows-debug.out" 2>"$CASE_ROOT/windows-debug.err"
grep -q '^native:--help$' "$CASE_ROOT/windows-debug.out"
grep -q 'Checking:.*kano-git.exe' "$CASE_ROOT/windows-debug.err"

set +e
KOG_DEBUG=0 KOG_TEST_BOOTSTRAP_FAIL=1 run_launcher \
  >"$CASE_ROOT/failure.out" 2>"$CASE_ROOT/failure.err"
failure_status=$?
set -e
[[ "$failure_status" -ne 0 ]]
grep -q '\[pixi-bootstrap\] fixture activated' "$CASE_ROOT/failure.err"

echo "PASS: launcher diagnostics are opt-in and bootstrap failures remain visible"
