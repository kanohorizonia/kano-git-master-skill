# Changelog

All notable changes to the Git Master Skill project.

## [Unreleased]

### Validation Gates

- Public release candidates must pass `git diff --check`, the public docs script-reference audit, the pre-commit quality gate, and a native version/help smoke check before tagging.
- Public docs and examples must avoid machine-local paths, personal account defaults, private endpoints, and committed credentials.

## [0.0.1] - 2026-06-22

### Status

**Alpha Release** - Public open-source baseline for the native `kog` / `kano-git` command surface.

### Added

- Native-first `kog` and `kano-git` launchers for repository discovery, status, fetch, log, commit planning, commit/push automation, repo hygiene, export, config, auth checks, completion, and release helpers.
- Release archive export and offline smoke validation through `kog export --single`.
- Upload handoff support for local sync folders and existing rclone remotes.
- Public docs audit for stale root-shell script references.
- Root MIT license for public distribution.

### Changed

- Current public documentation now treats the native CLI as the source of truth.
- Historical root-shell workflow examples are design/archive material unless explicitly listed in current command docs.
- Public examples use placeholder paths and organizations instead of workstation-local or person-specific values.

### Known Limitations

- This is an alpha baseline. Interfaces may still change before a stable release.
- Online developer builds may fetch C++ dependencies through CMake FetchContent.
- Google Drive API upload remains guidance-only in this release; use local sync folders or existing rclone remotes.
- Some archived design notes still describe retired root-shell workflows for historical context.

## Pre-Alpha Development

Earlier development phases delivered the shell prototype, worktree and submodule helpers, Git Scalar exploration, VCS bridge experiments, native CLI migration, C++ build/test infrastructure, release export, and documentation cleanup. Those phases are historical context, not the current public command contract.

## Version Format

This project follows [Semantic Versioning](https://semver.org/):

- MAJOR version for incompatible API changes.
- MINOR version for new functionality that is backward compatible.
- PATCH version for bug fixes that are backward compatible.

Pre-release suffixes:

- `-alpha`: Early public development.
- `-beta`: Feature complete, broader testing phase.
- `-rc`: Release candidate, final testing.

**Current Status**: 0.0.1 alpha

## Categories

- **Added**: New features.
- **Changed**: Changes to existing functionality.
- **Deprecated**: Soon-to-be removed features.
- **Removed**: Removed features.
- **Fixed**: Bug fixes.
- **Security**: Security fixes.
- **Performance**: Performance improvements.

---

**Last Updated**: 2026-06-22
**Current Version**: 0.0.1
