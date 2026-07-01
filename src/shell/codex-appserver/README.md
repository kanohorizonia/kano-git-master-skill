# Codex App-Server Support Scripts

These scripts are support utilities for local Codex app-server diagnostics and
thread inspection. They are not primary Git Master workflows; keep public Git
workflows on the `scripts/kog` / `scripts/kano-git` native launcher surface.

## Scripts

- `codex-appserver-dump-thread.ps1`
  - Reads Codex session index/transcript evidence and prints a bounded thread
    transcript for local debugging.
- `codex-appserver-set-name.ps1`
  - Uses Codex app-server protocol to set a user-facing thread name without
    requiring a transcript rollout to exist before the first turn.
- `codex-appserver-dump-sessions.ps1`
  - Lists deduplicated non-archived Codex sessions from the local Codex session
    index, with bounded default output and optional JSON.
- `codex-session-index.ps1`
  - Lists non-archived Codex sessions from the local Codex session index.

## Ownership

These utilities are shared local developer helpers and belong with Git Master's
script convention under `src/shell/`. KOA/Ark-owned runner and provider scripts
stay in `kano-agent-ark-skill` because they depend on Ark work-order and
local-task contracts.

The Ark compatibility wrappers in
`kano-agent-ark-skill/scripts/codex-appserver-scripts/` delegate here when
`kano-git-master-skill` is present next to `kano-agent-ark-skill`, or when
`KANO_GIT_MASTER_SKILL_ROOT` points at this repository.

Ark-owned runner/provider helpers, especially `codex-appserver-send.ps1`, remain
in `kano-agent-ark-skill` until KOA-TSK-0223 productionizes that path.
