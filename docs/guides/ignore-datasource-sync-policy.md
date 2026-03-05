# Ignore Datasource Sync Policy

This guide defines how to refresh the upstream ignore template datasource (`github/gitignore`) in a deterministic and auditable way.
It is the reference dataset used by ignore-plan generation, not a commit/push action itself.

## Command

```bash
# Preview current pinned metadata only
./kog plan datasource-sync --dry-run

# Sync submodule to latest upstream tracking revision
./kog plan datasource-sync
```

Custom datasource path in plan metadata:

```bash
./kog plan new \
  --ignore-datasource-root /path/to/my-ignore-sources \
  --ignore-datasource-manifest /path/to/my-ignore-sources/local/datasource.manifest.json
```

## Manifest `sources[].path` Resolution

`assets/ignore-sources/local/datasource.manifest.json` supports both:
- relative path: resolved from the manifest file directory
- absolute path: used as-is

Examples:
- `./custom.gitignore`
- `../upstream/github-gitignore`
- `/opt/company/gitignore-corpus`
- `D:/shared/gitignore-corpus`

## Output Metadata

`plan datasource-sync` prints:
- `gitlink_before` / `gitlink_after`: submodule gitlink SHA pinned by the skill repo `HEAD`
- `submodule_head_before` / `submodule_head_after`: checked-out submodule `HEAD` SHA
- `tracking_branch`: branch configured in `.gitmodules` (if set)
- `changed=true|false`: whether submodule checkout SHA changed during sync
- `datasource_sources`: parsed source count from manifest
- per source: `path_raw` / `path_resolved` / `exists`

Use these fields as audit evidence in release notes or backlog artifacts.

## Pin Update Policy

1. Run `./kog plan datasource-sync --dry-run` and record current metadata.
2. Run `./kog plan datasource-sync` to fetch and update submodule checkout.
3. Review the skill repo submodule pointer diff.
4. Run ignore-plan acceptance checks before publishing the pin update.
5. Commit only the submodule pointer change (plus docs/tests if needed) with a clear changelog entry.

## Cadence

- Default cadence: on-demand when ignore suggestions are stale or when upstream templates add required ecosystems.
- Optional cadence: periodic refresh (for example weekly) if the team needs frequent template updates.

## Rollback

If an upstream update causes regressions:
1. Checkout the previous known-good submodule pointer in the skill repo.
2. Re-run acceptance checks.
3. Commit the rollback pointer with incident context.
