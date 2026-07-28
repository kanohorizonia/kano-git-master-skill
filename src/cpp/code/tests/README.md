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
|- kano_git_regression_tests/
|  |- CMakeLists.txt
|  \- regression_coverage_test.cpp
|- kano_git_integration_tests/
|  |- CMakeLists.txt
|  |- process_executor_cases.cpp
|  |- git_transport_cases.cpp
|  \- commit_push_cases.cpp
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

- `kano_git_integration_tests.exe`
  - source root: `code/tests/kano_git_integration_tests/`
  - independently exercises real process capture, local bare-remote Git
    transport, and commit-push recovery behavior
  - uses only disposable local repositories; no public network is required

- `kano_git_regression_tests.exe`
  - source root: `code/tests/kano_git_regression_tests/`
  - validates the dogfood incident schema, exact source mapping drift, stable
    report ordering, command formats, and exit-code gates

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
cmake --build --preset <your-preset> --target kano_git_cli_tests kano_git_tui_tests kano_git_commit_plan_tests kano_git_regression_tests kano_git_integration_tests
```

## Run

```bash
./out/bin/<preset>/release/kano_git_cli_tests
./out/bin/<preset>/release/kano_git_tui_tests
./out/bin/<preset>/release/kano_git_commit_plan_tests
./out/bin/<preset>/release/kano_git_regression_tests
./out/bin/<preset>/release/kano_git_integration_tests
```

For the default Windows MSVC preset, use `out/bin/windows-ninja-msvc/release/*.exe`.

Focused examples:

```powershell
.\out\bin\windows-ninja-msvc\release\kano_git_commit_plan_tests.exe "[Unit][CommitPlan][Normalize]"
.\out\bin\windows-ninja-msvc\release\kano_git_cli_tests.exe "[functional][plan][freshness]"
.\out\bin\windows-ninja-msvc\release\kano_git_regression_tests.exe
```

## Dogfood Regression Registry

The source registry is `assets/regression/incidents.json`; its drafting template
is `assets/regression/case-template.json`. Every mapping names an exact test and
repo-relative source file. The regression target verifies that each file exists
and contains that exact name, preventing stale source-only mappings.

Run the product report with:

```bash
./scripts/kog regression coverage
./scripts/kog regression coverage --format json
./scripts/kog regression coverage --fail-on-gap
```

See `docs/guides/dogfood-regression-policy.md` for required incident metadata,
fixture locations, naming, and the non-blocking long-E2E policy.

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
  - runs the existing CLI + TUI selection and dogfood regression registry tests
  - does not run native integration tests

- `run_kano_git_all_tests`
  - full lane
  - runs CLI + TUI + native integration + E2E

- `run_kano_git_integration_tests`
  - independent native integration lane
  - currently covers eight bounded process, transport, and commit-push scenarios

Preferred Pixi entry points:

```bash
pixi run quick-test
pixi run integration-test
pixi run full-test
pixi run ci-linux-integration-test
```

The Bash runner supports both an integration-only lane and opt-in composition:

```bash
bash src/cpp/code/tests/run_tests.sh <preset> integration
bash src/cpp/code/tests/run_tests.sh <preset> default --with-integration
```

The PowerShell runner composes the same executable with the standard Windows
suite:

```powershell
pwsh -File src/cpp/code/tests/run_tests.ps1 -Preset <preset> -WithIntegration
```

The integration executable normally completes in about 15 seconds on a local
developer machine. Its fixtures force non-interactive Git/editor behavior,
disable repository hooks, use an external KOG binary cache, and create only
disposable local repositories. The lane writes its merged JUnit result to
`src/cpp/.kano/tmp/pgo/integration-test-reports/test-reports/integration/tests.xml`;
per-binary XML and report-packaging diagnostics stay under the same report
root when a failure occurs.

## E2E Scripts

```powershell
cmake --build --preset <preset> --target run_kano_git_e2e
```

```bash
cmake --build --preset <preset> --target run_kano_git_e2e
```
