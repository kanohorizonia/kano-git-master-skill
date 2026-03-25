# Documentation Deployment Scripts

This directory contains the GitHub Pages build pipeline for `kano-git-master-skill`.

## Overview

The pipeline keeps the source of truth inside this repository:

- `docs/` provides the markdown source content
- `src/shell/docs/` prepares and builds the static site
- `.github/workflows/cd-docs.yml` publishes the built site to GitHub Pages

## Local usage

```bash
./src/shell/docs/build-and-deploy.sh
```

This creates a local workspace under `_site/` and writes the built site to `_site/build/public`.

## Workspace layout

```text
_site/
├── src/
│   ├── raw_skillrepo/        # primary documentation source checkout
│   ├── raw_test/             # test reports from CI artifacts (CI mode)
│   ├── raw_coverage/         # coverage reports from CI artifacts (CI mode)
│   └── quartz/               # Quartz engine checkout
├── build/
│   ├── content_quartz/
│   ├── content_api/
│   └── public/               # Quartz output (deploy to GitHub Pages)
└── deploy/
    └── gh-pages/   # optional local branch checkout for manual publish flows
```

- Use `raw` for the primary source repository.
- Use `raw_<name>` when multiple source repositories feed one documentation site.
- Keep build-stage content separated by generator, such as `content_quartz` and `content_api`.

## API docs

- `src/shell/docs/04-build-api-docs.sh` generates C++ API docs with Doxygen into `_site/build/content_api/doxygen-html`.
- `src/shell/docs/05-stage-api-docs.sh` stages that output into `_site/build/public/api-docs`.
- If `doxygen` is unavailable locally, API docs generation is skipped with a warning so narrative docs can still be built.

## CI usage

```bash
./src/shell/docs/build-and-deploy.sh --ci "$(pwd)"
```

In CI mode, the workflow pre-checks out Quartz and the current repository, then uploads `_site/build/public` as the Pages artifact.
