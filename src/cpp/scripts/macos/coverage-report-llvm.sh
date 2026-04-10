#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
. "$SCRIPT_DIR/../common/kano_cpp_test_skill.sh"

kog_export_report_context
KANO_CPP_TEST_SKILL_ROOT="$(kog_require_cpp_test_skill_root)"
export KANO_CPP_TEST_SKILL_ROOT

: "${KANO_REPORT_SLUG:=macos-x64-coverage}"
export KANO_REPORT_SLUG

if [[ -x "/Applications/Xcode.app/Contents/Developer/Toolchains/XcodeDefault.xctoolchain/usr/bin/llvm-cov" ]]; then
  export KANO_LLVM_COV="/Applications/Xcode.app/Contents/Developer/Toolchains/XcodeDefault.xctoolchain/usr/bin/llvm-cov"
  export KANO_LLVM_PROFDATA="/Applications/Xcode.app/Contents/Developer/Toolchains/XcodeDefault.xctoolchain/usr/bin/llvm-profdata"
else
  export KANO_LLVM_COV="${KANO_LLVM_COV:-llvm-cov}"
  export KANO_LLVM_PROFDATA="${KANO_LLVM_PROFDATA:-llvm-profdata}"
fi

export KANO_TEST_COMMAND='./out/bin/macos-ninja-clang-coverage/debug/kano_git_tui_tests --reporter junit --out "$KANO_TEST_XML"'
export KANO_TEST_BINARY='./out/bin/macos-ninja-clang-coverage/debug/kano_git_tui_tests'
export KANO_LLVM_IGNORE_FILENAME_REGEX='_deps|catch2|ftxui|thirdparty|build|\.vcpkg'
export KANO_LLVM_EXPORT_COMMAND='"$KANO_LLVM_COV" export "$KANO_TEST_BINARY" -instr-profile="$KANO_PROFILE_DATA" > "$KANO_COVERAGE_REPORT_DIR/llvm-export.json" && python "$KOG_CPP_ROOT/shared/infra/build/base/script/common/llvm_json_to_cobertura.py" "$KANO_COVERAGE_REPORT_DIR/llvm-export.json" "$KOG_REPO_ROOT" "$KANO_COVERAGE_XML"'

cd "$KOG_CPP_ROOT"
bash "$KANO_CPP_TEST_SKILL_ROOT/src/shell/reports/macos/llvm-report.sh"
