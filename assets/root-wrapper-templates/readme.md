# root wrapper templates

These are recommended root-level `smart-*.sh` wrappers for projects that use
`kano-git-master-skill` as a submodule at:

`.agents/kano/kano-git-master-skill`

## profiles

- `common/`: shared wrappers used by all profiles
- `profiles/standalone/`: profile-specific wrappers for no-upstream repos (`smart-sync.sh` defaults to `origin-latest`)
- `profiles/oss/`: profile-specific wrappers for open source contributor workflows

## generate wrappers (recommended)

Use generator script:

```bash
./.agents/kano/kano-git-master-skill/scripts/core/gen-root-wrappers.sh --profile standalone --target .
```

Open source contributor profile:

```bash
./.agents/kano/kano-git-master-skill/scripts/core/gen-root-wrappers.sh --profile oss --target .
```

Options:

- `--force`: overwrite existing `smart-*.sh`
- `--dry-run`: preview actions without writing files

## manual copy (legacy)

```bash
cp .agents/kano/kano-git-master-skill/assets/root-wrapper-templates/profiles/standalone/smart-*.sh .
chmod +x smart-*.sh
```

## design notes

- wrappers are thin entrypoints only (no core business logic)
- wrappers export `KANO_GIT_MASTER_ROOT="$ROOT"` when needed
- implementation logic stays in `.agents/kano/kano-git-master-skill/scripts/...`
- generator composes wrappers from `common/` and then applies profile overrides
