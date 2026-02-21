# root wrapper templates

These are recommended root-level `smart-*.sh` wrappers for projects that use
`kano-git-master-skill` as a submodule at:

`.agents/kano/kano-git-master-skill`

## profiles

- `common/`: shared wrappers used by all profiles
- `profiles/standalone/`: profile-specific wrappers for no-upstream repos (`smart-sync.sh` defaults to `origin-latest`)
- `profiles/oss/`: profile-specific wrappers for open source contributor workflows
- `profiles/repo-passive-mode/`: passive submodule wrappers for multi-device repositories across PC/Mac/mobile (only acts on already cloned submodules)

## profile behavior quick compare

- `standalone`
	- `smart-sync.sh` only routes to `origin-latest`
	- advanced sync modes are intentionally blocked in wrapper
	- auto-bootstrap skill submodule when missing
- `oss`
	- `smart-sync.sh` supports `upstream-stable-dev|stable-dev|dev|upstream-force-push|origin-latest`
	- no-arg default is `upstream-stable-dev`
	- includes extra wrapper `smart-sync-upstream-stable-dev.sh`
	- auto-bootstrap skill submodule when missing
- `repo-passive-mode`
	- `smart-sync.sh` only routes to `origin-latest`
	- loops through locally cloned repos only (`collect_cloned_repos_csv`)
	- never runs submodule init/sync/update for missing repos
	- intended for multi-device environments where clone layout may differ per device

## generate wrappers (recommended)

Use generator script:

```bash
./.agents/kano/kano-git-master-skill/scripts/core/gen-root-wrappers.sh --profile standalone --target .
```

Open source contributor profile:

```bash
./.agents/kano/kano-git-master-skill/scripts/core/gen-root-wrappers.sh --profile oss --target .
```

Repository passive mode profile (passive submodule mode):

```bash
./.agents/kano/kano-git-master-skill/scripts/core/gen-root-wrappers.sh --profile repo-passive-mode --target .
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
- wrappers pause with `Press Enter to continue...` only for interactive human runs
- wrappers do not pause for agent/CI/non-interactive runs (including `--agent` modes except `--agent manual`)

## repo-passive-mode notes

Use this profile when one repository is shared across devices, but not every device has all submodules cloned.

Expected behavior:

- sync only `.` and submodules that already have a local `.git`
- skip missing submodules silently (no auto clone/init)
- keep each device's local clone topology unchanged

Typical command:

```bash
./smart-sync.sh
```

Equivalent explicit command:

```bash
./smart-sync.sh origin-latest
```
