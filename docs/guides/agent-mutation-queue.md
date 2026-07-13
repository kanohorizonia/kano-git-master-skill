# Agent Mutation Queue

Use the native agent mutation queue for low-conflict work in a shared checkout.
Use an isolated worktree for broad refactors, generated rewrites, long-running
validation, or ownership that cannot be expressed as exact files or chunks.

## Admission contract

Every item declares:

- repository and base HEAD
- work item and coding-agent/session id
- exact file intent
- optional inclusive `path:start-end` chunk ranges
- optional compatible `path=value` postconditions
- validation command intent

```bash
kog agent-queue admit \
  --id kg-123-codex-1 \
  --work-item KG-123 \
  --agent codex-1 \
  --file src/a.cpp \
  --chunk src/a.cpp:10-30 \
  --validate "pixi run quick-test"
```

Admission and drain state is stored in `<git-common-dir>/kano-agent-queue/state.json`.
All worktrees therefore share one per-repository queue. A KOG-owned directory
lock serializes state mutations. Existing locks are reported, never deleted.

## Drain policy

`kog agent-queue drain` is preview-only. Add `--confirm` to atomically move all
currently compatible pending items into one active batch.

Two intents are compatible when every shared file has either:

- the same non-empty declared postcondition, or
- explicit chunk ranges that do not overlap.

Different files are compatible. Ambiguous same-file intent, overlapping chunks,
an active batch, or a stale admission HEAD blocks drain and preserves all pending
items. Re-admit after replanning, or use a dedicated worktree.

## Exact-path commit

Commit an active batch through an isolated index:

```bash
kog commit \
  --exact-path src/a.cpp \
  --queue-batch <batch-id> \
  --expected-head <sha> \
  -m "[Git][BugFix] Update A (KG-123)"
```

Repeat `--exact-path` for every file. Include both old and new paths for a rename.
The command:

1. rejects repository escapes, directories, overlapping selectors, stale HEAD,
   queue scope mismatch, and existing `index.lock`;
2. builds a temporary index from HEAD and stages only selected paths;
3. reports selected and excluded changes with `--dry-run`;
4. runs Git commit hooks with the temporary `GIT_INDEX_FILE`;
5. updates only selected entries in the normal index after commit; and
6. verifies that unrelated index entries retain their object, mode, stage, and
   index flag state.

The porcelain commit step has a bounded 120-second default because large shared
indexes can legitimately exceed the generic 20-second probe budget. Set
`KOG_GIT_COMMIT_TIMEOUT_MS` for a narrower local bound; the global shell and
capture timeout overrides retain precedence.

KOG does not force commit or push, delete locks, or stage paths outside the
declared scope.

Close the batch after validation:

```bash
kog agent-queue complete --batch <batch-id> --status succeeded
```

Use `failed` or `cancelled` when appropriate. Completion retains a bounded
diagnostic receipt in queue state.
