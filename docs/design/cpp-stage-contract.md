# C++ Native Pipeline — Stage Contract & Architecture

## Status

Draft — `CPP-INFRA-001`

## Overview

This document defines the canonical stage contract for the native C++ build/test/coverage/report/packaging pipeline in `kano-git-master-skill`. It is the single source of truth for what constitutes a stage, how stages compose into workflows, and how the matrix/orchestration layer routes to platform-specific implementations.

---

## Layer Model

The pipeline is organized into 4 layers, from most generic to most specific:

| Layer | Path | Role |
|---|---|---|
| **Shared infra** | `src/cpp/shared/infra/scripts/` | Generic primitives, platform detection, environment utilities, orchestration helpers |
| **Project stages** | `src/cpp/shared/infra/scripts/stages/` | Canonical atomic stage entrypoints — each must be independently executable |
| **Project workflows** | `src/cpp/shared/infra/scripts/workflows/` | Composition of stages into named pipelines |
| **Platform adapters** | `src/cpp/shared/infra/scripts/{windows,macos,linux}/` | Platform-specific leaf implementations selected by the matrix |

---

## Stage Contract

Every atomic stage MUST satisfy all of the following:

1. **Independently executable** — Can be run without any other stage running first
2. **Explicit input artifact** — Accepts as arguments or environment the exact artifact(s) it consumes
3. **Explicit output artifact** — Produces a well-defined artifact (file path, directory, or data structure) as its sole output
4. **Single responsibility** — Does exactly one logical thing (build, test, gather, report, package)
5. **Orchestrator-safe** — Returns a well-defined exit code; never prompts for user input; logs its actions

---

## Canonical Atomic Stages

### `build` — Native project build

**Path:** `src/cpp/shared/infra/scripts/stages/build.sh`

**Delegates to:** `self/build.sh` → matrix → platform script

**Input:** Build preset (debug, release, relwithdebinfo, pgi) via positional args

**Output:** Built artifacts under `out/bin/<preset>/`

**Note:** This is currently a pass-through to `self/build.sh`. The long-term contract is that this is the canonical build stage.

---

### `test` — Run tests

**Path:** `src/cpp/shared/infra/scripts/stages/test.sh`

**Input:**
- `KOG_TEST_COMMAND` env var (takes precedence)
- Default: `code/tests/run_tests.sh`

**Output:** Test results; exit code reflects test pass/fail

---

### `test-report` — Render test report

**Path:** `src/cpp/shared/infra/scripts/stages/test-report.sh`

**Delegates to:** matrix → `test-report.sh` on target platform

**Input:** `KANO_TEST_XML` (JUnit XML path), `KANO_REPORT_SLUG`, `KANO_REPORT_ROOT`

**Output:** HTML/JSON test report under `$KANO_REPORT_ROOT/`

**Current issue:** Matrix points to `$KOG_MATRIX_BASE/{platform}/test-report.sh` which does not exist on most platforms.

---

### `coverage-build` — Build with coverage instrumentation

**Path:** `src/cpp/shared/infra/scripts/stages/coverage-build.sh`

**Delegates to:** matrix → platform script

**Input:** Build preset, coverage tool selection

**Output:** Coverage-instrumented binaries under `out/bin/<preset>-coverage/`

---

### `coverage-gather` — Run tests and collect raw coverage data

**Path:** `src/cpp/shared/infra/scripts/stages/coverage-gather.sh`

**Delegates to:** matrix → platform script

**Input:** `KANO_OPENCPPCOVERAGE_SOURCES`, test command

**Output:** Raw coverage data files (profraw, .coverage, etc.)

---

### `coverage-report` — Render coverage report from raw data

**Path:** `src/cpp/shared/infra/scripts/stages/coverage-report.sh`

**Delegates to:** matrix → platform script (coverage-report-microsoft.sh, coverage-report-opencppcoverage.sh, coverage-report-llvm.sh)

**Input:** `KANO_REPORT_ROOT`, backend (opencppcoverage, microsoft, llvm)

**Output:** `cobertura.xml`, `html/` directory under `$KANO_REPORT_ROOT/`

---

### `pgi-build` — Phase 1 PGO: Instrumented build

**Path:** `src/cpp/shared/infra/scripts/stages/pgi-build.sh`

**Input:** Build preset

**Output:** PGI-instrumented binaries

---

### `pgo-gather` — Phase 2 PGO: Gather profile data

**Path:** `src/cpp/shared/infra/scripts/stages/pgo-gather.sh`

**Input:** `KOG_PGO_GATHER_COMMAND` or test runner

**Output:** `.profraw` files merged into `merged.profdata`

---

### `pgo-build` — Phase 3 PGO: Optimized build with profile

**Path:** `src/cpp/shared/infra/scripts/stages/pgo-build.sh`

**Input:** `merged.profdata`

**Output:** PGO-optimized binaries

---

### `profile` — Run profiling matrix

**Path:** `src/cpp/shared/infra/scripts/stages/profile.sh`

**Input:** Matrix name (e.g., `default`, `pgo`, `launchers`)

**Output:** Per-case raw outputs under `.kano/tmp/profiling/<matrix>/<case>/`

---

### `profile-report` — Render profiling report

**Path:** `src/cpp/shared/infra/scripts/stages/profile-report.sh`

**Input:** Matrix name

**Output:** `profile.json`, `summary.md`, `index.html` under `docs/profiling/<slug>/`

---

### `package-reports` — Package all reports into distribution artifact

**Path:** `src/cpp/shared/infra/scripts/stages/package-reports.sh` (does not yet exist as a dedicated stage)

**Delegates to:** `scripts/common/package-reports-with-skill.sh`

**Input:** Report artifacts from prior stages

**Output:** `coverage-reports.tar.gz` (or equivalent distribution bundle)

---

## Orchestration-Only Flows

These are **NOT** atomic stages. They are explicit composition of stages.

### `self-build` — Default one-shot build entry

**Path:** `src/cpp/shared/infra/scripts/self/build.sh`

**Composition:** `matrix → platform/release script`

**Use:** Developer one-shot build without coverage or testing

---

### `self-rebuild` — Clean + one-shot build

**Path:** `src/cpp/shared/infra/scripts/self/rebuild.sh`

**Composition:** `rm -rf out/ + self-build`

**Use:** Developer full rebuild

---

### `coverage-all` — Full coverage pipeline

**Path:** `src/cpp/shared/infra/scripts/workflows/coverage-all.sh`

**Delegates to:** `coverage_report.sh all` in shared/infra

**Note:** This currently calls `coverage_report.sh` which is a shared/infra monolith. The long-term contract is for this to call `stages/coverage-build → stages/coverage-gather → stages/coverage-report` directly, with shared/infra only providing utilities.

---

### `pgo-rebuild` — Full PGO pipeline

**Path:** `src/cpp/shared/infra/scripts/workflows/pgo-rebuild.sh`

**Composition:** `pgi-build → pgo-gather → pgo_workflow.sh merge → pgo-build`

**Status:** This is already the cleanest orchestration flow. Reference implementation.

---

### `ci-build-matrix` — CI-driven build matrix

**Path:** Not yet implemented as a first-class orchestration script.

**Target model:** Reads matrix config (JSON/YAML), dispatches stage invocations per cell.

---

## Matrix / Orchestration Layer

**Path:** `src/cpp/shared/infra/scripts/orchestration/matrix.sh`

**Config:** `src/cpp/shared/infra/config/matrix.yml`

### Matrix axes (defined in config, not shell code)

| Axis | Allowed values | Defined in |
|---|---|---|
| `platform` | `windows`, `macos`, `linux` | `matrix.yml:axes.platform` |
| `arch` | `x64`, `arm64` | `matrix.yml:axes.arch` |
| `toolchain` | `msvc`, `clang`, `gcc` | `matrix.yml:axes.toolchain` |
| `build_preset` | `debug`, `release`, `relwithdebinfo`, `pgi` | `matrix.yml:axes.build_preset` |
| `coverage_mode` | `none`, `llvm`, `microsoft`, `opencppcoverage` | `matrix.yml:axes.coverage_mode` |
| `test_scope` | `unit`, `integration`, `all`, `smoke` | `matrix.yml:axes.test_scope` |
| `report_mode` | `junit`, `html`, `cobertura`, `all` | `matrix.yml:axes.report_mode` |
| `artifact_intent` | `developer`, `ci`, `release`, `analysis` | `matrix.yml:axes.artifact_intent` |

Axis combinations are also defined in `matrix.yml:combinations`:
- `release_build` — standard release build
- `coverage_pipeline` — coverage build + gather + report
- `pgo_pipeline` — 3-stage PGO (pgi-build → pgo-gather → pgo-build)
- `profiling` — profiling run + report
- `test_report` — test run + report rendering
- `package_reports` — package all reports

### Current functions

| Function | Purpose |
|---|---|
| `kog_matrix_host_os()` | Detect MINGW/MSYS/CYGWIN → win64, Darwin → mac, else → linux |
| `kog_matrix_arch()` | Detect arm64/aarch64 → arm64, else x64 |
| `kog_matrix_default_release_script()` | Return path to release build script |
| `kog_matrix_default_test_report_script()` | Return path to test-report script |
| `kog_matrix_default_coverage_build_script()` | Return path to coverage-build script |
| `kog_matrix_default_coverage_gather_script()` | Return path to coverage-gather script |
| `kog_matrix_default_coverage_report_script()` | Return path to coverage-report script |

### Current problems with matrix

1. **Points to non-existent scripts** — Several matrix return paths point to scripts that don't exist:
   - `win64/ninja-msvc-release.sh` → MISSING
   - `win64/test-report.sh` → MISSING
   - `win64/ninja-msvc-coverage-run.sh` → MISSING
   - `mac/test-report.sh` → MISSING
   - `mac/test-report-arm64.sh` → MISSING
   - `mac/ninja-clang-coverage-run.sh` → MISSING
   - `linux/test-report.sh` → MISSING
   - `linux/ninja-gcc-release.sh` → MISSING
   - `linux/ninja-clang-coverage-run.sh` → MISSING

2. **Naming mismatch** — The matrix returns paths like `ninja-msvc-release.sh` but the actual platform scripts are named `native-build.sh`, `coverage-report-*.sh`, etc.

### Matrix redesign principle

The matrix should only return paths to **scripts that exist and are maintained**. Stale return paths must be removed or filled in before this architecture can be considered healthy.

---

## Platform Adapter Layer

**Path:** `src/cpp/shared/infra/scripts/{windows,macos,linux}/`

These are the leaf implementations selected by the matrix. Each adapter:

- Accepts canonical stage input contracts
- Delegates to shared/infra utilities for tool detection and env setup
- Produces canonical stage output artifacts
- Is platform-specific by nature (MSVC vs clang vs gcc)

### Current state

| Platform | Directory | Status |
|---|---|---|
| Windows | `src/cpp/shared/infra/scripts/windows/` | Partial — has coverage scripts but missing release, test-report |
| macOS | `src/cpp/shared/infra/scripts/macos/` | Minimal — only `native-build.sh` |
| Linux | `src/cpp/shared/infra/scripts/linux/` | Minimal — `docker-build.sh`, `native-build.sh` |

---

## Shared Infra Boundaries

**What belongs in `shared/infra/scripts/common/`:**

- Environment detection utilities (platform, compiler, tool paths)
- Coverage data merge utilities (llvm-profdata, etc.)
- Report rendering adapters (llvm_json_to_cobertura.py, render_junit_test_report.py)
- Preset build helpers (windows_preset_build.sh, unix_preset_build.sh)
- Metadata generation (build_metadata.sh)

**What does NOT belong in shared/infra:**

- Workflow orchestration logic (coverage_build + coverage_gather + coverage_merge + coverage_report + coverage_all all in one file) — this should be decomposed into stages
- Preset/runner selection policy — this belongs in the matrix/config layer, not in shared logic
- Platform-specific command construction — this belongs in platform adapters

---

## Report Kind Separation

Two explicit report kinds, kept separate:

| Kind | Stage | Renderer |
|---|---|---|
| `test` | `test-report` | JUnit XML → HTML via `render_junit_test_report.py` |
| `coverage` | `coverage-report` | Raw coverage data → cobertura.xml + HTML via platform tool |

---

## Implementation Notes

### Critical gap: matrix return paths must match real files

Every `kog_matrix_default_*_script()` function must return a path that exists. If the target script doesn't exist, the function must either:
- Be removed (if that combination is not supported)
- Be filled in with a stub or implementation

There must be no dead dispatcher targets.

### Shared infra vs project stages boundary

The stages under `src/cpp/shared/infra/scripts/stages/` are the **canonical project-level entrypoints**. They may delegate to shared/infra utilities, but they must not re-implement stage logic in shared/infra. Shared/infra provides **primitives**; stages provide **contracts**.

### External report skill adapter

External report/renderer integration (e.g., `kano-cpp-test-skill`) must be behind a single adapter layer. Currently it leaks across `kano_cpp_test_skill.sh`, `package-reports-with-skill.sh`, and `coverage_report.sh`. This needs to collapse into `common/report_skill_adapter.sh` (or equivalent).

---

## Next Steps (from this design)

1. Audit all matrix return paths and verify each target script exists ✅ (see `matrix.yml:known_gaps`)
2. Fill in missing platform adapters or remove unsupported matrix rows ⚠️ 8 gaps documented
3. Decompose `coverage_report.sh` into individual stage compositions ✅ Done
4. Create the missing `package-reports` stage ✅ Done
5. Establish the external report skill adapter boundary ✅ Done (`report_skill_adapter.sh`)
6. Verify each stage has explicit input/output contracts documented ✅ Done (this doc)

---

## Appendix: Current File Inventory

### `src/cpp/shared/infra/scripts/stages/` (canonical stage entrypoints)

```
build.sh          — delegates to self/build.sh
test.sh           — KOG_TEST_COMMAND or code/tests/run_tests.sh
test-report.sh    — matrix → platform test-report
coverage-build.sh — matrix → platform coverage-build
coverage-gather.sh — matrix → platform coverage-gather
coverage-report.sh — matrix → platform coverage-report
pgi-build.sh      — PGO Phase 1
pgo-gather.sh     — PGO Phase 2
pgo-build.sh      — PGO Phase 3
profile.sh        — profiling matrix runner
profile-report.sh — profiling report renderer
```

### `src/cpp/shared/infra/scripts/workflows/` (orchestration)

```
coverage-all.sh   — full coverage pipeline
pgo-rebuild.sh    — full PGO pipeline (reference clean composition)
```

### `src/cpp/shared/infra/scripts/self/` (one-shot entrypoints)

```
build.sh          — matrix → platform release build
rebuild.sh        — rm out/ + build
```

### `src/cpp/shared/infra/scripts/orchestration/matrix.sh` (routing)

Returns platform-specific script paths for: release build, test-report, coverage-build, coverage-gather, coverage-report.
