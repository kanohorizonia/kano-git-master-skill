# kano-git - C++ Test Targets

This directory contains the product-facing C++ test executables for kano-git.

## Layout

```text
tests/
|- CMakeLists.txt
|- kano_git_cli_tests/
|  |- CMakeLists.txt
|  \- functional/
|- kano_git_tui_tests/
|  |- CMakeLists.txt
|  |- unit/
|  |- property/
|  \- integration/
|- kano_git_commit_plan_tests/
|  |- CMakeLists.txt
|  |- unit/
|  \- property/
|- kano_git_gui_tests/
|  \- README.md
\- e2e/
   |- CMakeLists.txt
   \- plan_commit_regression/
```

## Target Mapping

- `kano_git_cli_tests.exe`
  - source root: `code/tests/kano_git_cli_tests/`
  - current focus: black-box CLI workflow regressions

- `kano_git_tui_tests.exe`
  - source root: `code/tests/kano_git_tui_tests/`
  - current focus: TUI-facing unit/property/integration tests

- `kano_git_commit_plan_tests.exe`
  - source root: `code/tests/kano_git_commit_plan_tests/`
  - current focus: commit-plan schema, AI fill, pathspec, and freshness regressions

- `kano_git_gui_tests`
  - reserved only
  - no executable is produced yet

## Shared Test Code

Shared test infrastructure is no longer stored under `code/tests/`.

It now lives in the `kano_git_test_core` subsystem:

- `code/systems/kano_git_test_core/generators/*`
- `code/systems/kano_git_test_core/support/*`

That subsystem provides:

- randomized TUI test data generators
- functional sandbox/workspace helpers
- shared command execution helpers for black-box tests

## Build

```bash
cd src/cpp
cmake --preset <your-preset>
cmake --build --preset <your-preset> --target kano_git_cli_tests kano_git_tui_tests kano_git_commit_plan_tests
```

## Run

```bash
./out/bin/<preset>/release/kano_git_cli_tests
./out/bin/<preset>/release/kano_git_tui_tests
./out/bin/<preset>/release/kano_git_commit_plan_tests
```

For the default Windows MSVC preset, use `out/bin/windows-ninja-msvc/release/*.exe`.

Focused examples:

```powershell
.\out\bin\windows-ninja-msvc\release\kano_git_commit_plan_tests.exe "[Unit][CommitPlan][Normalize]"
.\out\bin\windows-ninja-msvc\release\kano_git_cli_tests.exe "[functional][plan][freshness]"
```

## Commit-Plan Regression Fixtures

Prefer table-driven native fixtures in `kano_git_commit_plan_tests` when the
case must prove real git pathspec behavior against a temporary repository.
Name each row after the dogfood failure mode, then assert either the normalized
pathspec list or the stable `INVALID_PLAN_*` error prefix.

Use file-backed JSON/golden fixtures only when the raw payload shape itself is
the behavior under test and the case does not need per-run temp paths.

## TDD/BDD Tags

Use focused tags for tests that participate in feature-first reporting:

- TDD/unit checks: `[tdd][unit][feature:<feature>]`
- BDD functional scenarios: `[bdd][functional][feature:<feature>][scenario:<id>][featured]`

Do not retag the whole legacy suite just to satisfy a report lane. The current
initial feature map is limited to high-signal tests for `ai-provider-bootstrap`,
`ai-model-resolution`, `converge-state`, `status-policy`, `discovery`,
`dirty-kind`, and `repo-operation-scheduler`. Scenario Markdown, Mermaid
diagrams, feature-highlight source, and TDD/BDD summaries are generated derived
artifacts under `.kano/tmp/`; regenerate them instead of editing them manually.

## Test Lanes

- `run_kano_git_tests`
  - fast lane
  - runs CLI + TUI + commit-plan tests

- `run_kano_git_all_tests`
  - full lane
  - runs CLI + TUI + commit-plan + E2E

## E2E Scripts

```powershell
cmake --build --preset <preset> --target run_kano_git_e2e
```

```bash
cmake --build --preset <preset> --target run_kano_git_e2e
```
