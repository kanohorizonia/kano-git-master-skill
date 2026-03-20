# Documentation Deployment Scripts

This directory contains the GitHub Pages build pipeline for `kano-git-master-skill`.

## Overview

The pipeline keeps the source of truth inside this repository:

- `docs/` provides the markdown source content
- `src/shell/docs/` prepares and builds the static site
- `.github/workflows/docs-deploy.yml` publishes the built site to GitHub Pages

## Local usage

```bash
./src/shell/docs/build-and-deploy.sh
```

This creates a local workspace under `_ws/` and writes the built site to `_ws/build/public`.

## Workspace layout

```text
_ws/
├── src/
│   ├── raw/        # primary documentation source checkout
│   ├── raw_xxx/    # optional additional raw sources
│   └── quartz/     # Quartz engine checkout
├── build/
│   ├── content_quartz/
│   ├── content_api/
│   └── public/
└── deploy/
    └── gh-pages/   # optional local branch checkout for manual publish flows
```

- Use `raw` for the primary source repository.
- Use `raw_<name>` when multiple source repositories feed one documentation site.
- Keep build-stage content separated by generator, such as `content_quartz` and `content_api`.

## API docs

- `src/shell/docs/04-build-api-docs.sh` generates C++ API docs with Doxygen into `_ws/build/content_api/doxygen-html`.
- `src/shell/docs/05-stage-api-docs.sh` stages that output into `_ws/build/public/api-docs`.
- If `doxygen` is unavailable locally, API docs generation is skipped with a warning so narrative docs can still be built.

## CI usage

```bash
./src/shell/docs/build-and-deploy.sh --ci "$(pwd)"
```

In CI mode, the workflow pre-checks out Quartz and the current repository, then uploads `_ws/build/public` as the Pages artifact.
