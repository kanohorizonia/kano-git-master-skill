# Release Automation

This directory contains canonical shell scripts for release automation scaffolding.

- `00-verify-release-version.sh` ensures the pushed/manual tag matches `VERSION`.
- `01-generate-release-notes.sh` builds a release-notes markdown file from downloaded artifacts.
- `02-generate-winget-skeleton.sh` writes preview winget manifests with resolved GitHub Release URLs/checksums, without publishing them.
- `03-generate-homebrew-skeleton.sh` writes a preview Homebrew formula with resolved GitHub Release tarball URLs/checksums, without publishing it.

These scripts are intended to be called from GitHub Actions workflows under `.github/workflows/`.
