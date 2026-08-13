# CI/CD Trigger Policy

Date: 2026-08-14
Backlog items: KG-TSK-0093, KG-TSK-0094, KG-TSK-0097, KG-TSK-0138

## Policy

Jenkins is the canonical CI/CD runner for `kano-git-master-skill`. GitHub
Actions remains available for public-safe checks, GitHub Pages publishing, and
explicitly reviewed/manual release preparation. A normal README-only or docs-only
push must not run the full native build, coverage, installer, or release
publication lanes.

## Incident Root Cause

GitHub Actions run `27968013586` ran the legacy CI workflow on event `push` for
branch `main`.

- Actor: `dorgonman`
- Commit: `baf614c8f54cbd65f9da154be4ab920a136f82c1`
- Commit title: `[KOG][Docs] Polish public command examples (KG-TSK-0092)`
- Trigger cause: the workflow push path filters included `README.md` and
  `docs/**`, and the heavy native build, coverage, and MSI jobs had no
  manual-only event guard.

The result was a docs-only push starting the full GitHub Actions CI matrix.

## Final Trigger Matrix

| Workflow | Push | Pull request | Workflow dispatch | Release/tag publication |
| --- | --- | --- | --- | --- |
| `agent-skill-cloud-build.yml` / `KanoAgentSkills / Cloud Build` | No | No | Yes; Jenkins/manual cloud build inputs | No direct publication |
| `release-gates.yml` / `KanoAgentSkills / Release Gates` | Public-safe `quality-gate` only | Public-safe `quality-gate` only | Full native build, coverage, and MSI lanes | No |
| `tui-pr-gates.yml` / `KanoAgentSkills / TUI PR Gates` | No | macOS 15 arm64, Linux x64, and Windows x64 release-build TUI focus suite only for bounded TUI/build-input paths | Same matrix, for a selected source ref reproduction | No |
| `pages.yml` / `KanoAgentSkills / Publish Pages` | GitHub Pages build and `gh-pages` branch publish for docs/source-site paths | No | GitHub Pages build and `gh-pages` branch publish | No |
| `publish-release.yml` / `KanoAgentSkills / Publish Release` | No | No | Yes; requires `release_reviewed=true` and defaults to draft release | Reviewed/manual GitHub Release only; no tag-push auto publish |
| `code-quality-coverage-upload.yml` / `KanoAgentSkills / Code Quality Coverage Upload` | No | No | Yes; uploads an existing Cobertura XML artifact to GitHub Code Quality | No |
| GitHub-native `CodeQL` | Repository security setting, not checked-in KOG workflow YAML | Repository security setting, not checked-in KOG workflow YAML | Security scanning only if enabled in GitHub | Not a release gate |

## KOB Reference Alignment

`kano-agent-backlog-skill` keeps its full cloud build on
`agent-skill-cloud-build.yml` as `workflow_dispatch`, with Jenkins-style inputs.
It allows a limited public release gate on push and pull request, and publishes
Pages from docs/README changes. Its checked-in workflow inventory uses
`KanoAgentSkills / Cloud Build`, `KanoAgentSkills / Publish Pages`, and
`KanoAgentSkills / Release Gates`.

KOB's visible `CodeQL` entry is treated as GitHub-native code scanning and
security-alert input. KOG does not add a fake CodeQL workflow to source control;
if CodeQL is enabled in repository security settings, it remains advisory and is
not a Jenkins replacement or release gate.

KOG follows the same boundary:

- GitHub Actions full build/release work is explicit/manual, not implicit on
  README/docs push.
- GitHub Code Quality coverage upload is an explicit/manual experiment that
  consumes an existing Cobertura XML artifact; it does not generate coverage,
  publish releases, or replace Jenkins coverage ownership.
- Jenkins remains the source of release evidence and package-manager preparation.
- GitHub Release publication requires an explicit manual run after evidence
  review, and drafts are the default.

## TUI PR Gate Contract (KG-TSK-0138)

`TUI PR Gates` is intentionally a narrow, public-safe pull-request check. It
does not replace Jenkins, create release assets, publish packages, upload
coverage, or change the manual release workflows. It builds release-configured
native binaries on Linux x64 (Clang preset), macOS arm64, and Windows x64,
smoke-checks `kano-git` and `kano-git-tui`, then invokes exactly the Catch2
selectors `[tui_pr_focus]` in `kano_git_tui_tests` and `[audit_pr_focus]` in
`kano_git_commit_plan_tests`. The second selector is reserved for the audit
reader and pinned-attempt lifecycle cases that influence the audit surface. The
runner rejects a missing JUnit report, a selector that matches zero tests, or a
missing member of the checked-in critical-case inventory, so a renamed/removed
focus tag or critical scenario cannot silently produce a green result. The
inventory includes interactive-first-frame, redirected-stream rejection, typed
audit receipt truth, native terminal ownership, and the pinned-audit-reader
lifecycle cases.

The workflow is path-filtered to the real TUI/CLI app entrypoints, command UI,
runtime and commit-plan code, core audit/shell/workspace code, their two test
suites, its focused runner, native build inputs, launchers, Pixi manifests, the
submodule pin, and itself. General docs-only or README-only changes therefore
remain outside this native matrix. Its
`workflow_dispatch` `source_ref` input permits an operator to rerun the same
bounded check for a commit, branch, or tag; leaving it blank uses the ref
selected in the GitHub Actions UI.

Dependency installation uses the recursively checked-out shared-infra lockfile
through `setup-pixi`'s supported `manifest-path` and `run-install` contract.
The action derives its environment cache from that lock. A separate
OS/architecture/CMake-input keyed cache retains only immutable FetchContent
`*-src` directories; it never restores object files, generated build state, or
test results. Every job still runs `pixi install --locked`, configures CMake,
and performs a fresh release build, so a cache hit is not test evidence.

Artifacts are deliberately limited to the available focus-suite JUnit XML files
(each capped at 2 MiB by the runner even if a test fails or times out), a
one-line pass/fail status, and a four-line run manifest containing the actual
checkout revision. An oversized JUnit file is deleted before upload. The upload
step fails if no bounded evidence exists. The workflow does not capture shell
output, environment exports, checkout credentials, or arbitrary workspace files
into artifacts. Contributors should still treat test names and failure messages
as public-safe because JUnit is retained for seven days.

### Required-check and operator boundary

Branch protection must explicitly mark the three matrix checks named `TUI PR
focus (linux-x64)`, `TUI PR focus (macos-arm64)`, and `TUI PR focus
(windows-x64)` as required before this lane can be described as merge-blocking.
Workflow YAML cannot make itself a required check. Until that repository setting
is applied, the jobs are visible evidence only and a maintainer must not claim
they block merges. A skipped workflow caused by its path filter is likewise not
evidence that the TUI gate ran; path-filtered checks are advisory until a stable
always-run aggregator or equivalent branch-protection design is established.
Operators should use `workflow_dispatch` when a focused reproduction is needed.
For a manual run, the manifest records `git rev-parse HEAD`, rather than
assuming the UI-selected ref or `GITHUB_SHA` resolved to the intended commit.
