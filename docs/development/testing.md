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
- `src/shell/test/acceptance-remote-mac-build-policy.sh`

These validate workflow behavior against built `kog` binaries and disposable repos.

### Shell support helpers

- `src/shell/support/gen-root-wrappers.sh`

This is scaffolding/support, not a product runtime entrypoint and not a test.

## Recommended Entry Points

If `pixi` is available, use the shared infra manifest as the default entrypoint for native flows.

```bash
pixi install --manifest-path src/cpp/shared/infra/pixi.toml
pixi run --manifest-path src/cpp/shared/infra/pixi.toml build
pixi run --manifest-path src/cpp/shared/infra/pixi.toml quick-test
pixi run --manifest-path src/cpp/shared/infra/pixi.toml full-test

# Repo-specific acceptance tasks stay at repo root
pixi run acceptance-commit-plan-first
pixi run acceptance-quickstart
pixi run acceptance-pixi-bootstrap-parity
```

Notes:

- `pixi run --manifest-path src/cpp/shared/infra/pixi.toml build` resolves to the current host-default native build task
- the native test runners also know how to trigger `scripts/kog self build` when needed

## PGO Test Suite (Coverage-Guided)

Use the PGO pipeline entrypoint:

```bash
./scripts/kog self build --pgo
```

The gather phase now runs a representative default suite mapped to major
product areas:

- `kano_git_cli_tests` with functional coverage
- `kano_git_commit_plan_tests` with unit/property coverage
- `kano_git_export_tests` with unit/integration coverage
- `kano_git_tui_tests` with unit/property coverage

Gather artifacts are emitted to:

- `.kano/tmp/pgo/gather-reports/junit/` (per-binary JUnit XML)
- `.kano/tmp/pgo/gather-reports/logs/` (per-binary logs)

This is intended to keep PGO profile data aligned with real test workloads
instead of smoke-only checks.

### Windows Coverage Tool Policy for `kog self build --pgo`

On Windows, `kog self build --pgo` now supports two distinct coverage behaviors:

- `KANO_CPP_INFRA_COVERAGE_TOOL=opencppcoverage`
  - single-pass gather in PGO collect mode
  - one test pass produces both coverage artifacts and `.pgd` profile data

- `KANO_CPP_INFRA_COVERAGE_TOOL=microsoft`
  - split pipeline
  - phase A: run a dedicated coverage pass on `windows-ninja-msvc-coverage` (Debug, `/PROFILE`) with Microsoft static instrumentation + collect
  - phase B: run normal PGO collect/use flow (`windows-ninja-msvc-pgo-collect` -> merge -> `windows-ninja-msvc-pgo-use`)

Rationale:

- Microsoft static C++ coverage expects the documented static instrumentation flow for `/PROFILE` binaries.
- OpenCppCoverage can collect coverage directly while running PGO collect binaries.

If you need to override the suite for a targeted run:

```bash
KOG_PGO_GATHER_COMMAND='ctest --test-dir src/cpp/out/obj/windows-ninja-msvc-pgo-collect -C Debug --output-on-failure' \
  ./scripts/kog self build --pgo
```

For suite tuning, use coverage output as input:

1. Generate coverage reports (`coverage-all` / `report-coverage`) on your target platform.
2. Identify under-covered command areas.
3. Add or adjust PGO gather workloads so those areas are exercised during collect.

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
bash src/shell/test/acceptance-remote-mac-build-policy.sh
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

- they are scaffolding/maintenance helpers
- they are not runtime command flows and not tests

## TDD/BDD Taxonomy and Feature Mapping

KOG uses focused Catch2 tags to feed the shared Kano report renderer without
requiring every legacy test to be retagged in one pass. New or touched
low-level checks should use this TDD shape:

```text
[tdd][unit][feature:<feature>]
```

BDD scenario tests should use this shape, with the scenario id matching the
`ScenarioRecorder` sidecar metadata:

```text
[bdd][functional][feature:<feature>][scenario:<id>][featured]
```

Additional domain tags such as `[converge]`, `[planner]`, `[status]`, or
`[inventory]` may follow the required taxonomy tags. The current initial feature
map intentionally covers only high-signal tests touched during the TDD/BDD
reporting pass instead of retagging the whole suite:

| Feature | Initial TDD sources | BDD scenarios |
|---|---|---|
| `ai-provider-bootstrap` | AI working-copy/bootstrap unit flow tests | `KOG-BDD-AI-001`, `KOG-BDD-AI-002`, `KOG-BDD-AI-003` |
| `ai-model-resolution` | model resolution, model selection, and model arg unit tests | - |
| `converge-state` | converge planner/state functional regression checks | `KOG-BDD-CONVERGE-002` through feature `converge` |
| `status-policy` | recursive status policy regression checks | `KOG-BDD-STATUS-001` |
| `discovery` | discover inventory regression checks | `KOG-BDD-DISCOVERY-001` |
| `dirty-kind` | converge/status checks that assert dirty-kind decisions | - |
| `repo-operation-scheduler` | scheduler lock, ordering, and partial-failure checks | - |

The shared renderer consumes these tags and BDD metadata to produce
`tdd-bdd-summary.json` and `tdd-bdd-summary.md`, mapping TDD tests and BDD
scenarios by feature. Generated scenario Markdown, Mermaid diagrams,
`feature-highlights-source.*`, and TDD/BDD summary files are derived report
artifacts under `.kano/tmp/` and must not be manually edited or checked in.

## User Story Coverage Mapping

Each user story maps to CLI commands and their test coverage.

### Command Inventory

| Command | Description | Test Coverage |
|---------|-------------|--------------|
| `discover` | Discover repositories in workspace | CLI: 13 tagged discovery tests (inventory, gitignore, kogignore, cache, external) |
| `init` | Initialize a new workspace | Not yet tested |
| `plan` | Plan repository updates | CLI: 2 tests (freshness verification) |
| `commit` | AI-powered commit message generation | CLI: 10+ tests (commit-push workflow, secret gate, detached HEAD) |
| `push` | Multi-remote push workflow | CLI: 3 tests (policy enforcement) |
| `sync` | Repository synchronization | CLI: 5 tests (post-sync, locks, Windows, self-build) |
| `pr` | GitHub PR workflow | Not yet tested |
| `status` | Repository status display | CLI: 1 test (table width adaptation) |
| `clone` | Clone with upstream support | Not yet tested |
| `branch` | Branch operations | Not yet tested |
| `cache` | Cache management | Not yet tested |
| `config` | Configuration management | Not yet tested |
| `log` | Git log with AI enhancement | Not yet tested |
| `resolve` | AI-powered conflict resolution | Not yet tested |
| `doctor` | Environment health checks | Not yet tested |
| `worktree` | Git worktree management | Not yet tested |
| `submodule` | Enhanced submodule management | CLI: 2 tests (add, passthrough) |
| `subtree` | Git subtree operations | Not yet tested |
| `scalar` | Git Scalar mono-repo | Not yet tested |
| `p4` | Git-Perforce bridge | Not yet tested |
| `svn` | Git-Subversion bridge | Not yet tested |

### User Story to Test Mapping

| US-ID | User Story | CLI Command(s) | Test Location | Status |
|-------|------------|----------------|---------------|--------|
| US-001 | As a developer, I want to discover all repositories in my workspace | `discover` | `functional_test_main.cpp: workspace_discover_*`; `discover_inventory_test.cpp` | ✅ Covered |
| US-002 | As a developer, I want to initialize a new workspace | `init` | — | ❌ Missing |
| US-003 | As a developer, I want to plan repository updates before applying | `plan` | `functional_test_main.cpp: plan_*` | ✅ Covered |
| US-004 | As a developer, I want to commit with AI-generated messages | `commit` | `functional_test_main.cpp: commit_push_*` | ✅ Covered |
| US-005 | As a developer, I want to push to multiple remotes | `push` | `functional_test_main.cpp: push_*` | ✅ Covered |
| US-006 | As a developer, I want to sync repositories with upstream | `sync` | `functional_test_main.cpp: sync_*` | ✅ Covered |
| US-007 | As a developer, I want to create GitHub PRs | `pr` | — | ❌ Missing |
| US-008 | As a developer, I want to see repository status | `status` | `functional_test_main.cpp: repo_status_*` | ✅ Covered |
| US-009 | As a developer, I want to clone repos with upstream setup | `clone` | — | ❌ Missing |
| US-010 | As a developer, I want to manage branches | `branch` | — | ❌ Missing |
| US-011 | As a developer, I want to resolve merge conflicts with AI | `resolve` | — | ❌ Missing |
| US-012 | As a developer, I want to manage git worktrees | `worktree` | — | ❌ Missing |
| US-013 | As a developer, I want to manage submodules | `submodule` | `functional_test_main.cpp: submodule_*` | ✅ Covered |
| US-014 | As a developer, I want to check environment health | `doctor` | — | ❌ Missing |
| US-015 | As a developer, I want to protect secrets in commits | `commit` (secret-gate) | `functional_test_main.cpp: secret_gate_*` | ✅ Covered |

### TUI Component Coverage

TUI tests cover the terminal UI components, not individual commands:

| Component | Test Location | Coverage |
|-----------|---------------|----------|
| AutocompleteEngine | `test_autocomplete_engine.cpp` | Command/option completion |
| CommandExecutor | `test_command_executor.cpp` | Command parsing and execution |
| CommandModeState | `test_command_mode_state.cpp` | Input buffer, candidate selection |
| MetadataCache | `test_metadata_cache.cpp` | CLI metadata extraction |
| TuiState | `test_tui_state.cpp` | Mode transitions, state management |

### Coverage Targets

- **Current baseline**: 86.48% line coverage (TUI tests only)
- **Target**: 80% coverage across all user stories
- **Priority gaps**: US-002 (init), US-007 (pr), US-009 (clone), US-011 (resolve)

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
bash src/cpp/shared/infra/scripts/windows/build_windows_ninja_msvc_release.sh
bash src/cpp/shared/infra/scripts/windows/ninja-msvc-release.sh
```

- For repo-local tool orchestration, `pixi run --manifest-path src/cpp/shared/infra/pixi.toml build`,
  `pixi run --manifest-path src/cpp/shared/infra/pixi.toml quick-test`, and
  `pixi run --manifest-path src/cpp/shared/infra/pixi.toml full-test` are the preferred
  entrypoints over stale repo-root examples.

- Acceptance scripts may emit Git CRLF warnings in disposable repos; those are not
  failures by themselves.

## See Also

- `src/cpp/code/tests/README.md`
- `TESTING.md`
- `docs/guides/cpa-commit-plan-workflow.md`
