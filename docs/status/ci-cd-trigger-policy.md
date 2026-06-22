# CI/CD Trigger Policy

Date: 2026-06-23
Backlog item: KG-TSK-0093

## Policy

Jenkins is the canonical CI/CD runner for `kano-git-master-skill`. GitHub
Actions remains available for public-safe checks, GitHub Pages publishing, and
explicitly reviewed/manual release preparation. A normal README-only or docs-only
push must not run the full native build, coverage, installer, or release
publication lanes.

## Incident Root Cause

GitHub Actions run `27968013586` ran workflow `CI Build` from
`.github/workflows/ci-build.yml` on event `push` for branch `main`.

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
| `kano-cloud-build.yml` | No | No | Yes; Jenkins/manual cloud build inputs | No direct publication |
| `ci-build.yml` | Public-safe `quality-gate` only | Public-safe `quality-gate` only | Full native build, coverage, and MSI lanes | No |
| `cd-docs.yml` | GitHub Pages build/deploy for docs/source-site paths | No | GitHub Pages build/deploy | No |
| `cd-release.yml` | No | No | Yes; requires `release_reviewed=true` and defaults to draft release | No tag-push auto publish |

## KOB Reference Alignment

`kano-agent-backlog-skill` keeps its full cloud build on
`agent-skill-cloud-build.yml` as `workflow_dispatch`, with Jenkins-style inputs.
It allows a limited public release gate on push and pull request, and publishes
Pages from docs/README changes. Its release channel docs require public release
assets to match Jenkins `Build_CI` source/version evidence; dry-run publish
output is only review evidence.

KOG follows the same boundary:

- GitHub Actions full build/release work is explicit/manual, not implicit on
  README/docs push.
- Jenkins remains the source of release evidence and package-manager preparation.
- GitHub Release publication requires an explicit manual run after evidence
  review, and drafts are the default.
