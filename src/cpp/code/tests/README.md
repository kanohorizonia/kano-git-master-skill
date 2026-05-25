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
cmake --build --preset <your-preset> --target kano_git_cli_tests kano_git_tui_tests
```

## Run

```bash
./build/bin/<preset>/kano_git_cli_tests
./build/bin/<preset>/kano_git_tui_tests
```

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
  - runs CLI + TUI tests only

- `run_kano_git_all_tests`
  - full lane
  - runs CLI + TUI + E2E

## E2E Scripts

```powershell
cmake --build --preset <preset> --target run_kano_git_e2e
```

```bash
cmake --build --preset <preset> --target run_kano_git_e2e
```
