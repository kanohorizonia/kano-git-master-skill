# C++ Stage Refactor Backlog

Status: draft local issue set — 6/6 complete ✅
Scope: `src/cpp/shared/infra` + `src/cpp/scripts`

## Why this exists

GitHub issue creation is blocked in the current environment because `gh auth status` is not authenticated.
This file records the planned tickets so they can be opened upstream later without losing scope.

---

## CPP-INFRA-001 — Define the canonical stage contract and matrix axes

### Goal
Make the native pipeline architecture explicit before more script moves happen.

### Problem
The repo is moving toward `self/`, `stages/`, `workflows/`, and profiling matrices, but the contract is still partly implicit.
Some orchestration remains embedded in shell scripts instead of being driven by a clean graph/config model.

### Scope
- Define canonical atomic stages:
  - `build`
  - `build_pgi`
  - `gather_pgo_profile`
  - `build_pgo`
  - `run_tests`
  - `collect_coverage`
  - `render_test_report`
  - `render_coverage_report`
  - `package_reports`
- Define orchestration-only flows:
  - `self_build`
  - `self_rebuild`
  - `coverage_pipeline`
  - `ci_build_matrix`
  - `release_pipeline`
- Define matrix axes and allowed combinations:
  - platform
  - toolchain
  - config
  - optimization mode
  - test scope
  - coverage mode
  - report mode
  - artifact intent

### Acceptance criteria
- A design doc exists under `docs/design/` for the stage contract. ✅ Done
- Matrix axes and allowed rows are defined in config, not only in shell code. ✅ Done (`config/matrix.yml`)
- Every workflow script references the documented stage contract. ✅ Done (coverage-all.sh, pgo-rebuild.sh, self/build.sh, self/rebuild.sh)

---

## CPP-INFRA-002 — Complete the atomic stage surface in mainline

### Goal
Make the stage layer complete enough that orchestration is only composition, not hidden execution logic.

### Problem
`src/cpp/shared/infra/scripts/stages/` exists, but the implementation is still uneven.
Coverage/profile stages are clearer than the rest, and some flow ownership still lives in common workflow scripts.

### Scope
- Ensure mainline stage entrypoints exist and are stable for:
  - build
  - test
  - test-report
  - coverage-build
  - coverage-gather
  - coverage-report
  - pgi-build
  - pgo-gather
  - pgo-build
  - package-reports
- Audit each stage for:
  - independent execution
  - explicit input artifact contract
  - explicit output artifact contract
  - orchestrator-safe exit behavior

### Acceptance criteria
- Each stage can run independently.
- Each stage documents expected input/output artifacts.
- Workflows call stages instead of re-implementing stage logic.

---

## CPP-INFRA-003 — Split execution, rendering, and packaging responsibilities cleanly

### Goal
Separate raw execution/collection from rendering/packaging.

### Problem
Coverage and reporting responsibilities are still partially mixed.
Scripts such as coverage workflow helpers still own too much flow and policy.

### Scope
- Separate execution layer responsibilities:
  - build test binaries
  - run tests
  - collect raw coverage
  - gather PGO profile inputs
- Separate rendering/packaging responsibilities:
  - render test reports
  - render coverage reports
  - package artifacts
- Reduce `coverage_workflow.sh` / `coverage_report.sh` to orchestration-only or replace them with narrower stage invocations.

### Acceptance criteria
- No single script owns collect + render + package as a hidden monolith.
- Test execution and coverage collection are independently invocable.
- Report generation consumes explicit artifacts from prior stages.

---

## CPP-INFRA-004 — Introduce an explicit report adapter layer

### Goal
Stop leaking external skill/report-renderer details across multiple scripts.

### Problem
External report integration exists today, but the dependency boundary is still distributed across workflow/common scripts.

### Scope
- Add a single adapter surface for external reporting skill integration.
- Centralize:
  - skill root discovery
  - renderer resolution
  - invocation wrappers
  - fallback / failure policy
- Migrate existing report/package callers to use the adapter.

### Acceptance criteria
- External skill paths are not hard-coded in multiple unrelated scripts.
- Report scripts call a shared adapter contract.
- Swapping renderer/provider requires changing one adapter layer, not several workflows.

---

## CPP-INFRA-005 — Consolidate `kano-cpp-infra-refactor` back into mainline and retire the side repo

### Goal
Return to a single source of truth under `kano-git-master-skill/src/cpp/shared/infra`.

### Problem
The side repo no longer buys much isolation because mainline already carries the same structural direction.
Keeping both increases migration cost and risks duplicated cleanup.

### Resolution (2026-04-10)
- Committed structural fix to `kano-cpp-infra.git` (infra submodule): `matrix.sh` updated from flat to nested path formula (`../../../../..` + `src/cpp/scripts`) ✅
- Subdir names normalized: `win64`→`windows`, `mac`→`macos` ✅
- `config/matrix.yml` added with all 8 axes defined in YAML ✅
- Pushed to `origin/main` (commit `950adba`) ✅
- Side repo `kano-cpp-infra-refactor` deleted ✅
- No external consumers found — migration was simply committing the fix and deleting the unused parallel checkout ✅

### Final state
- `src/cpp/shared/infra` (git submodule of `kano-cpp-infra.git`) is the single source of truth ✅
- Side repo removed ✅
- CI/self-build paths continue to work ✅

---

## CPP-INFRA-006 — Audit and clean dead or legacy script references

### Goal
Remove hidden coupling and incomplete matrix targets before further refactor expansion.

### Problem
The architecture shape is ahead of the actual script inventory in some areas.
This creates confusion about what is canonical versus compatibility-only.

### Scope
- Audit `matrix.sh` and related dispatchers for targets that do not exist.
- Audit platform directories for stale `ninja-*` legacy wrappers.
- Mark each script as one of:
  - canonical atomic stage
  - canonical workflow
  - adapter
  - compatibility shim
  - dead code

### Acceptance criteria
- No dispatcher points to missing scripts.
- Legacy wrappers are documented and minimized.
- Canonical entrypoints are obvious from layout and names.

---

## Recommended execution order

1. `CPP-INFRA-001` — define contract first ✅ **DONE**
2. `CPP-INFRA-006` — remove dead references / classify existing scripts ✅ **DONE**
3. `CPP-INFRA-002` — finish atomic stage surface ✅ **DONE**
4. `CPP-INFRA-003` — split execution vs rendering vs packaging ✅ **DONE**
5. `CPP-INFRA-004` — collapse external report integration behind adapter ✅ **DONE**
6. `CPP-INFRA-005` — committed nested-path fix, pushed, side repo deleted ✅ **DONE**

## Status Summary (2026-04-10)

| Ticket | Status | Key Changes |
|---|---|---|
| CPP-INFRA-001 | ✅ Done | Design doc + matrix config + workflow citations complete |
| CPP-INFRA-002 | ✅ Done | 12/12 canonical stages exist; `package-reports.sh` created |
| CPP-INFRA-003 | ✅ Done | `coverage-all.sh` rewritten to compose stages instead of calling monolith |
| CPP-INFRA-004 | ✅ Done | `report_skill_adapter.sh` created; `package-reports-with-skill.sh` updated |
| CPP-INFRA-005 | ✅ Done | Committed nested-path `matrix.sh` + `config/matrix.yml` to `kano-cpp-infra.git` (`950adba`); pushed; side repo deleted |
| CPP-INFRA-006 | ✅ Done | `matrix.sh` base path and subdir names fixed; all 23 paths verified |
