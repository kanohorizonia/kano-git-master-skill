# kano-git Test Setup Summary

## Current Test Artifacts

- `kano_git_test_core.lib`
  - shared subsystem for test generators and functional support helpers

- `kano_git_cli_tests.exe`
  - built from `code/tests/kano_git_cli_tests/*`

- `kano_git_tui_tests.exe`
  - built from `code/tests/kano_git_tui_tests/*`

- `kano_git_gui_tests`
  - reserved only
  - no executable target yet

## Current Build Topology

- `code/systems/kano_git_test_core/CMakeLists.txt`
  - owns shared test code

- `code/tests/CMakeLists.txt`
  - owns Catch2 bootstrap and top-level aggregation only

- `code/tests/kano_git_cli_tests/CMakeLists.txt`
  - owns CLI test executable

- `code/tests/kano_git_tui_tests/CMakeLists.txt`
  - owns TUI test executable

- `code/tests/e2e/CMakeLists.txt`
  - owns E2E custom targets

- `run_kano_git_tests`
  - fast lane for CLI + TUI

- `run_kano_git_all_tests`
  - full lane for CLI + TUI + E2E

## Build Example

```bash
cmake --build --preset <preset> --target kano_git_cli_tests kano_git_tui_tests
```
