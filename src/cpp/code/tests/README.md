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
