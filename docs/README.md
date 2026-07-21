# Git Master Skill Documentation

**Version**: 0.0.1
**Status**: Alpha Release
**Current command contract updated**: 2026-06-22

This documentation set contains both current native-CLI docs and older historical
notes. Use the current command contract first. Older shell workflow examples are
not current unless they are explicitly reintroduced and covered by smoke tests.

## Current Entry Points

- [Current Command Surface](./guides/current-command-surface.md)
- [KCC Commit Message Policy](./guides/kcc-commit-message-policy.md)
- [Public Artifact Contract](./guides/public-artifact-contract.md)
- [CI/CD Trigger Policy](./status/ci-cd-trigger-policy.md)
- [CPA Commit Plan Workflow](./guides/cpa-commit-plan-workflow.md)
- [Repo Hygiene](./repo-hygiene.md)
- [Testing Guide](./development/testing.md)
- [Pixi Development Environment](./development/pixi.md)
- [Kano C++ Dev Convention](./development/kano-cpp-dev-convention.md)

Canonical commands:

```bash
./scripts/kog --help
./scripts/kog self build
./scripts/kog repo-hygiene check
./scripts/kog export --help
./scripts/kog export --single
```

## Release and Audit Tests

```bash
src/shell/test/smoke-release-archive.sh <archive.tar>
src/shell/test/smoke-release-online-build.sh <archive.tar>
src/shell/test/audit-public-doc-script-refs.sh
```

The offline release smoke test does not require network access. `kog export
--single` automatically runs it for the root release archive when the smoke
script is present. The online build smoke test may fetch C++ dependencies from
GitHub through CMake FetchContent.

## Current Architecture Docs

- [Command Library Dependency Graph](./design/command-library-dependency-graph.md)
- [Workspace Native Planner Contract](./design/workspace-native-planner-contract.md)
- [C++ Stage Contract](./design/cpp-stage-contract.md)
- [C++ Coverage and PGO Provider Model](./cpp-profile-coverage-pgo-model.md)
- [CI/CD Trigger Policy](./status/ci-cd-trigger-policy.md)
- [PGO and Coverage Shared-Infra Tracking](./status/pgo-coverage-shared-infra-tracking.md)
- [Last-Known-Good Baseline and Regression Bisect Design](./design/last-known-good-regression-bisect.md)
- [Worktree & Scalar Design](./design/worktree-scalar.md)
- [Orphan Branch Design](./design/orphan-branch.md)

## Historical / Legacy Reference Archive

The following areas may contain older shell-oriented examples. Keep them as design
or migration references, not as the current product surface:

- `docs/examples/`
- `docs/comparisons/`
- `docs/migrations/`
- older feature guides that predate the native `kog` command surface

When updating these files, either migrate examples to native `kog` commands or
mark the section as historical. Do not add old root-shell workflow examples to
current docs.

## Documentation Conventions

- Use lowercase kebab-case filenames.
- Put current user-facing docs in `docs/guides/` or the root `docs/` directory.
- Put design-only material in `docs/design/`.
- Prefer `./scripts/kog ...` examples for current workflows.
- Use release smoke tests to validate archive and launcher behavior.

## Getting Help

Start with the native help surface:

```bash
./scripts/kog --help
./scripts/kog <command> --help
```

For script-specific checks, prefer the smoke tests listed above rather than old
root-shell workflow examples.
