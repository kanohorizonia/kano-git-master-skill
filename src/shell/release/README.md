# Release Automation

This directory contains canonical shell scripts for release automation scaffolding.

- `00-verify-release-version.sh` ensures the pushed/manual tag matches `VERSION`.
- `01-generate-release-notes.sh` builds a release-notes markdown file from downloaded artifacts.
- `02-generate-winget-skeleton.sh` writes winget manifests with resolved release asset URLs/checksums.
- `02-generate-winget-from-artifacts.sh` finds the staged MSI artifact and calls `kog release winget generate`.
- `03-generate-homebrew-skeleton.sh` writes a Homebrew formula with resolved release tarball URLs/checksums.
- `04-publish-winget-internal.sh` publishes generated winget manifests into the configured internal manifest repository.
- `05-publish-homebrew-internal.sh` publishes the generated formula into the configured internal Homebrew tap.

These scripts can be called from Jenkins via the agent-skill pipeline. Publish scripts fail closed unless the internal repository/tap URL is provided through the Jenkins environment or pipeline config.
