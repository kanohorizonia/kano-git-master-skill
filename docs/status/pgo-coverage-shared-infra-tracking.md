# PGO and coverage shared-infra tracking

Status: tracking note for `KG-TSK-0088`
Updated: 2026-06-23

## Scope

This note tracks how `kano-git-master-skill` currently consumes the shared native
PGO and coverage infrastructure. It is an integration/status artifact only; it
does not change the shared infra implementation.

## Current integration points

- Repo-root Pixi tasks in `pixi.toml` delegate build, test, coverage, report,
  PGO, and Linux CI lanes into `src/cpp/shared/infra`.
- Shared Pixi tasks in `src/cpp/shared/infra/pixi.toml` expose the canonical
  lanes:
  - `coverage-all`
  - `coverage-report`
  - `gather-reports`
  - `pgo-gather`
  - `pgi-build`
  - `profile-gather`
  - `pgo-build`
  - `pgo-test`
  - `pgo-rebuild`
  - `pgo-gather-with-coverage`
  - `profile-run-manifest`
  - `ci-linux-coverage-all`
  - `ci-linux-pgi-build`
  - `ci-linux-profile-gather`
  - `ci-linux-pgo-build`
  - `ci-linux-pgo-test`
- `src/cpp/shared/infra/scripts/workflows/coverage-all.sh` composes the
  coverage lane from `coverage-build`, `coverage-gather`, and
  `coverage-report`.
- `src/cpp/shared/infra/scripts/workflows/pgo-rebuild.sh` is the release PGO
  orchestration lane. It resolves a profile-run manifest, runs PGI/collect
  build, gathers profile data, then merges and performs the optimized build
  unless gather-only mode is requested.
- `src/cpp/shared/infra/scripts/workflows/pgo-gather-with-coverage.sh` is the
  optional observation lane. It sets `KANO_CXX_PROFILE_RUN_MODE` to
  `pgo-gather-with-coverage`, skips the PGO use stage, and chooses
  OpenCppCoverage on Windows or LLVM coverage elsewhere when no provider is set.
- `src/cpp/shared/infra/scripts/stages/profile-run-manifest.sh` emits the
  provider capability manifest through `kano_cpp_infra_tool
  profile-run-manifest`.
- `src/cpp/shared/infra/scripts/lib/native_tool.sh` currently owns the
  profile-run manifest decision logic and provider guardrails.
- Public docs already point users at lane-level commands and the provider model:
  `README.md`, `docs/cpp-profile-coverage-pgo-model.md`, and
  `docs/design/cpp-stage-contract.md`.

## Guardrails confirmed in current docs/code

- `coverage-all` is the canonical coverage lane and must be treated as
  build -> gather -> report, not just report rendering.
- `pgo-gather` and `pgo-rebuild` are the canonical release PGO lanes.
- `pgo-gather-with-coverage` is optional training-run observation, not the
  default CI reporting lane.
- Microsoft.CodeCoverage.Console output is not MSVC PGO training data.
- MSVC unified PGO plus coverage execution is only allowed through
  OpenCppCoverage; Microsoft coverage remains a split lane.
- Microsoft CodeCoverage server-mode is local/session detached collection, not
  remote telemetry.
- Coverage reports, PGO training artifacts, and detached collector sessions
  remain separate artifact classes.

## Validation lanes

Use these commands when changing KG integration or shared-infra PGO/coverage
surface:

```bash
pixi run quick-test
pixi run profile-run-manifest
pixi run coverage-all
pixi run pgo-gather
pixi run pgo-rebuild
pixi run pgo-gather-with-coverage
pixi run ci-linux-coverage-all
pixi run ci-linux-pgi-build
pixi run ci-linux-profile-gather
pixi run ci-linux-pgo-build
pixi run ci-linux-pgo-test
```

For documentation-only tracking changes, `pixi run profile-run-manifest` plus
`git diff --check` is enough to verify that the manifest entrypoint still
exists and the doc patch is clean. Full `coverage-all` and `pgo-rebuild` remain
the release-quality gates.

## Current gaps and follow-ups

- `docs/cpp-profile-coverage-pgo-model.md` says the capability resolver is
  `scripts/profiling/profile_run_capabilities.py`, but no such file is present
  in the current tree. The active implementation appears to be
  `src/cpp/shared/infra/scripts/lib/native_tool.sh profile-run-manifest`.
  Follow-up should either update the doc or reintroduce a stable resolver file.
- Automated provider-combination coverage for `profile-run-manifest` is not
  visible in the current tree. Add smoke or unit coverage for:
  - MSVC + OpenCppCoverage + MSVC PGO unified lane allowed.
  - MSVC + Microsoft coverage + MSVC PGO unified lane rejected with the guardrail
    message.
  - Microsoft server-mode classified as local-session coverage.
  - LLVM unified profile-data lane allowed.
  - split PGO or split coverage lanes allowed without the opposite provider.
- `docs/design/cpp-stage-contract.md` still records matrix adapter gaps. Treat
  that document as a tracked architecture status until matrix return paths are
  fully aligned with existing platform scripts.
- Public 0.0.1 docs should keep the first screen at lane level. Do not add
  provider internals to the README landing narrative beyond the existing link to
  the C++ coverage and PGO provider model.

## Decision

Keep KG public docs focused on the command lanes:

- `coverage-all` for coverage evidence.
- `pgo-gather` and `pgo-rebuild` for release PGO.
- `pgo-gather-with-coverage` only when a supported unified observation backend
  is explicitly needed.

Provider semantics and artifact-class rules belong in
`docs/cpp-profile-coverage-pgo-model.md`; build-stage ownership belongs in
`docs/design/cpp-stage-contract.md`; this file tracks KG integration state and
follow-up gaps.
