# Ignore Assets

This is the canonical root for KOG ignore planning and ignore-gate policy.
Responsibilities are deliberately separated:

- `datasource/`: the datasource manifest and upstream template corpora used to
  generate ignore-plan candidates.
- `local-rules/`: deterministic Kano-maintained ignore rules consumed by the
  datasource manifest.
- `policy/`: operator policy used by safety gates. Policy entries do not become
  generated `.gitignore` rules.

## Default paths

- Datasource root: `assets/ignore/datasource/`
- Datasource manifest: `assets/ignore/datasource/manifest.json`
- Kano local rules: `assets/ignore/local-rules/kano.gitignore`
- Ignore-gate policy: `assets/ignore/policy/ignore-gate-allowlist.txt`
- Upstream corpus: `assets/ignore/datasource/upstream/github-gitignore/`

Manifest source paths are relative to the manifest directory. Absolute paths
and explicit command-line datasource overrides remain supported.

Older packaged installs that still contain `assets/ignore-sources/` are read
through the runtime layout compatibility resolver. New source and release
artifacts must use this canonical structure.

Sync the upstream source with:

```bash
./scripts/kog plan datasource-sync --dry-run
./scripts/kog plan datasource-sync
```
