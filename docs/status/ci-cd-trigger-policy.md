# CI/CD Trigger Policy

Date: 2026-06-23
Backlog items: KG-TSK-0093, KG-TSK-0094, KG-TSK-0097

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
