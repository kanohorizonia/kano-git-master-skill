# root wrapper templates

These are recommended root-level `smart-*.sh` wrappers for projects that use
`kano-git-master-skill` as a submodule at:

`skills/kano/kano-git-master-skill`

## included wrappers

- `smart-clone.sh`
- `smart-commit.sh`
- `smart-commit-push.sh`
- `smart-push.sh`
- `smart-status.sh`
- `smart-sync.sh`
- `smart-sync-upstream-stable-dev.sh`

## copy to project root

From project root:

```bash
cp skills/kano/kano-git-master-skill/assets/root-wrapper-templates/smart-*.sh .
```

Optional executable bit:

```bash
chmod +x smart-*.sh
```

## design notes

- wrappers are thin entrypoints only (no business logic)
- wrappers always export `KANO_GIT_MASTER_ROOT="$ROOT"`
- implementation logic stays in `skills/kano/kano-git-master-skill/scripts/...`
