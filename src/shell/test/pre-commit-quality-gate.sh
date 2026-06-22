#!/usr/bin/env bash
#
# pre-commit-quality-gate.sh
# Local, shell-only quality gate for checks that should run before commit and
# before release archive generation. This deliberately avoids requiring the
# native kog binary so fresh clones and Windows Git Bash environments can run it.

set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
cd "$repo_root"

bash -n scripts/kog
bash -n scripts/kano-git

if [[ -f src/cpp/shared/infra/scripts/platform/linux/native-build.sh ]]; then
  bash -n src/cpp/shared/infra/scripts/platform/linux/native-build.sh
fi
if [[ -f src/cpp/shared/infra/scripts/platform/mac/native-build.sh ]]; then
  bash -n src/cpp/shared/infra/scripts/platform/mac/native-build.sh
fi
if [[ -f src/cpp/shared/infra/scripts/lib/unix_preset_build.sh ]]; then
  bash -n src/cpp/shared/infra/scripts/lib/unix_preset_build.sh
fi

src/shell/test/audit-git-index-hygiene.sh
src/shell/test/audit-public-doc-script-refs.sh
if [[ "${KOG_ENABLE_KCC_AUDIT:-0}" == "1" ]]; then
  kcc_args=(--max-count "${KOG_KCC_AUDIT_MAX_COUNT:-50}")
  if [[ "${KOG_KCC_AUDIT_STRICT:-0}" == "1" ]]; then
    kcc_args+=(--strict)
  fi
  src/shell/test/audit-kcc-commit-messages.sh "${kcc_args[@]}"
fi
src/shell/test/smoke-root-wrapper-generator.sh

echo "PASS: pre-commit quality gate"
