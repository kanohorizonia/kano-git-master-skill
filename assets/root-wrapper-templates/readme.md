# root wrapper templates

These are recommended root-level wrappers for projects that use
`kano-git-master-skill` as a submodule at:

`.agents/kano/kano-git-master-skill`

## naming policy

- Prefer `kog` wrappers (`kog`, `kog-*.sh`) for new setups.
- Keep `smart-*.sh` wrappers only for compatibility.
- Commit wrappers default to `--no-ai-review`.
- AI review is opt-in via dedicated `*with-ai-review.sh` wrappers.

## profiles

- `common/`: shared wrappers used by all profiles
- `profiles/standalone/`: no overrides (inherits wrappers from `common/`; directory kept with marker)
- `profiles/oss/`: profile-specific wrappers for open source contributor workflows (`*-sync-upstream-stable-dev.sh`)
- `profiles/repo-passive-mode/`: passive submodule wrappers for multi-device repositories across PC/Mac/mobile (only acts on already cloned submodules)
- `profiles/repo-passive-mode-with-ai/`: extends `repo-passive-mode` and adds `*with-ai-review` wrappers

## profile behavior quick compare

- `standalone`
	- uses shared `common/*-sync.sh` behavior (only `origin-latest`)
	- advanced sync modes are intentionally blocked in wrapper
	- auto-bootstrap skill submodule when missing
- `oss`
	- uses shared `common/*-sync.sh` behavior (only `origin-latest`)
	- adds only one profile wrapper: `*-sync-upstream-stable-dev.sh`
	- auto-bootstrap skill submodule when missing
- `repo-passive-mode`
	- `*-sync.sh` only routes to `origin-latest`
	- loops through locally cloned repos only (`collect_cloned_repos_csv`)
	- never runs submodule init/sync/update for missing repos
	- intended for multi-device environments where clone layout may differ per device
- `repo-passive-mode-with-ai`
	- inherits all behavior from `repo-passive-mode`
	- adds `*-commit-with-ai-review.sh` and `*-commit-push-with-ai-review.sh`

## generate wrappers (recommended)

Use generator script:

```bash
./.agents/kano/kano-git-master-skill/src/shell/core/gen-root-wrappers.sh --profile standalone --target .
```

Open source contributor profile:

```bash
./.agents/kano/kano-git-master-skill/src/shell/core/gen-root-wrappers.sh --profile oss --target .
```

Repository passive mode profile (passive submodule mode):

```bash
./.agents/kano/kano-git-master-skill/src/shell/core/gen-root-wrappers.sh --profile repo-passive-mode --target .
```

Repository passive mode with extra with-ai-review wrappers:

```bash
./.agents/kano/kano-git-master-skill/src/shell/core/gen-root-wrappers.sh --profile repo-passive-mode-with-ai --target .
```

Options:

- `--force`: overwrite existing wrapper files
- `--dry-run`: preview actions without writing files

## manual copy (legacy)

```bash
cp .agents/kano/kano-git-master-skill/assets/root-wrapper-templates/common/kog* .
chmod +x kog kog-*.sh
```

## design notes

- wrappers are thin entrypoints only (no core business logic)
- wrappers export `KANO_GIT_MASTER_ROOT="$ROOT"` when needed
- implementation logic stays in `.agents/kano/kano-git-master-skill/src/shell/...`
- generator composes wrappers from `common/` and then applies profile overrides
- wrappers pause with `Press Enter to continue...` only for interactive human runs
- wrappers do not pause for agent/CI/non-interactive runs (including `--agent` modes except `--agent manual`)
- commit wrappers are split into two entry points:
	- `kog-commit.sh` (default `--no-ai-review`)
	- `kog-commit-with-ai-review.sh` (default `--ai-review`)
	- `kog-commit-push.sh` (default `--no-ai-review`)
	- `kog-commit-push-with-ai-review.sh` (default `--ai-review`)

## repo-passive-mode notes

Use this profile when one repository is shared across devices, but not every device has all submodules cloned.

Expected behavior:

- sync only `.` and submodules that already have a local `.git`
- skip missing submodules silently (no auto clone/init)
- keep each device's local clone topology unchanged

Typical command:

```bash
./kog-sync.sh
```

Equivalent explicit command:

```bash
./kog-sync.sh origin-latest
```
