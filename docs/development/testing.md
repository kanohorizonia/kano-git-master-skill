# Testing Guide

This repo currently has two distinct test layers:

- native test targets and native E2E harnesses under `src/cpp/code/tests/`
- shell acceptance/workflow regressions under `src/shell/test/`

The important rule is to group tests by what they support, not by file extension.
Shell or PowerShell files that exist only to drive native C++ test lanes stay under
`src/cpp/code/tests/`. Product/workflow shell acceptance lives under
`src/shell/test/`.

## Current Test Topology

### Native test lanes

- `src/cpp/code/tests/run_tests.sh`
- `src/cpp/code/tests/run_tests.ps1`
- `src/cpp/code/tests/e2e/plan_commit_regression/`

These drive the native test targets defined in `src/cpp/code/tests/CMakeLists.txt`:

- `run_kano_git_tests`
  - fast lane
  - CLI + TUI tests
- `run_kano_git_all_tests`
  - full lane
  - CLI + TUI + E2E

### Shell acceptance coverage

- `src/shell/test/acceptance-commit-plan-first.sh`
- `src/shell/test/acceptance-quickstart-commit-push.sh`
- `src/shell/test/acceptance-ignore-plan.sh`

These validate workflow behavior against built `kog` binaries and disposable repos.

### Shell support helpers

- `src/shell/support/gen-root-wrappers.sh`

This is scaffolding/support, not a product runtime entrypoint and not a test.

## Recommended Entry Points

If `pixi` is available, use it as the default repo entrypoint.

```bash
pixi install
pixi run build-native
pixi run quick-test
pixi run full-test
pixi run acceptance-commit-plan-first
pixi run acceptance-quickstart
```

## Direct Invocation

### Native test lanes

```bash
# Bash runner
bash src/cpp/code/tests/run_tests.sh <preset>

# PowerShell runner
pwsh -File src/cpp/code/tests/run_tests.ps1 -Preset <preset>

# CMake targets
cmake --build --preset <preset> --target run_kano_git_tests
cmake --build --preset <preset> --target run_kano_git_all_tests
```

### Shell acceptance

```bash
bash src/shell/test/acceptance-commit-plan-first.sh
bash src/shell/test/acceptance-quickstart-commit-push.sh
bash src/shell/test/acceptance-ignore-plan.sh
```

## Where New Tests Should Go

### Put tests under `src/cpp/code/tests/` when

- they are native C++ test targets
- they are shell/PowerShell runners for native test lanes
- they are E2E harnesses owned by native test execution

### Put tests under `src/shell/test/` when

- they validate shell/native workflow behavior end to end
- they create disposable repos and drive built `kog` commands directly
- they represent product acceptance/regression scenarios

### Put helpers under `src/shell/support/` when

- they generate assets or wrappers
- they are scaffolding/maintenance helpers
- they are not runtime command flows and not tests

## Current Coverage Snapshot

| Area | Location | Status |
|---|---|---|
| Native CLI/TUI lanes | `src/cpp/code/tests/` | Active |
| Native E2E plan regression | `src/cpp/code/tests/e2e/plan_commit_regression/` | Active |
| Commit plan-first acceptance | `src/shell/test/acceptance-commit-plan-first.sh` | Active |
| Commit/commit-push quickstart acceptance | `src/shell/test/acceptance-quickstart-commit-push.sh` | Active |
| Ignore-plan acceptance | `src/shell/test/acceptance-ignore-plan.sh` | Active |

## Notes

- Older references to `scripts/test/` are stale in this checkout and should not be
  used as the source of truth.
- On Windows, prefer the native Windows build script before running acceptance:

```bash
bash src/cpp/scripts/windows/build_windows_ninja_msvc_release.sh
```

- Acceptance scripts may emit Git CRLF warnings in disposable repos; those are not
  failures by themselves.

## See Also

- `src/cpp/code/tests/README.md`
- `TESTING.md`
- `docs/guides/cpa-commit-plan-workflow.md`
