# Workspace Native Planner Contract

This document defines the shared JSON contract emitted by native C++ workspace planners and consumed by shell adapters.

## Scope

Applies to:

- `kano-git update --native-plan-only`
- `kano-git foreach --native-plan-only --command "..."`

And to adapter execution modes:

- `kano-git update --native-plan ...`
- `kano-git foreach --native-plan --command "..." ...`

## Top-level Schema

```json
{
  "planner": "native-submodule-update | native-foreach",
  "operations": [
    {
      "order": 0,
      "wave": 0,
      "path": "D:/path/to/repo",
      "type": "root | registered | unregistered",
      "action": "update-repo | foreach",
      "command": "optional, present for foreach"
    }
  ],
  "waves": {
    "has_cycle": false,
    "cycle_nodes": [],
    "waves": [["D:/path/to/repo"]]
  },
  "shell_adapter": {
    "script": "workspace/update-workspace-repos.sh | workspace/foreach-repo.sh",
    "manifest": { "optional": "present for update planner" },
    "command": "optional, present for foreach planner"
  }
}
```

## Field Rules

- `planner`
  - Required string.
  - Current values: `native-submodule-update`, `native-foreach`.
- `operations`
  - Required array.
  - Ordered by deterministic execution order (`order` ascending).
- `operations[].order`
  - Required integer.
  - Global sequence index across all waves.
- `operations[].wave`
  - Required integer.
  - Wave index matching `waves.waves`.
- `operations[].path`
  - Required string.
  - Repo path in normalized generic format.
- `operations[].type`
  - Required string.
  - One of `root`, `registered`, `unregistered`.
- `operations[].action`
  - Required string.
  - `update-repo` for update planner, `foreach` for foreach planner.
- `operations[].command`
  - Optional string.
  - Present when `action=foreach`.
- `waves`
  - Required object from native dependency scheduler.
  - `has_cycle=true` means execution should abort in native execution modes.
- `shell_adapter.script`
  - Required string.
  - Current values: `workspace/update-workspace-repos.sh`, `workspace/foreach-repo.sh`.
- `shell_adapter.manifest`
  - Optional object.
  - Present for update planner compatibility path.
- `shell_adapter.command`
  - Optional string.
  - Present for foreach planner.

## Compatibility Notes

- Additive fields are allowed if existing required fields remain stable.
- Consumers should ignore unknown fields.
- `operations` must remain deterministic for identical repo graph input.

## Consumer Responsibilities

- Shell adapters should execute `operations` in listed order.
- `--include-types` filtering is allowed at adapter layer and may reduce operation count.
- On empty operations after filtering, adapters should exit success with a clear info/warn message.
