# Last-known-good baseline and regression bisect design

Status: design decision for `KG-FTR-0020`
Updated: 2026-06-23

## Decision

KOG should add a repo-local last-known-good baseline workflow before adding any
automated regression bisect runner.

The first implementation should be plan-first and dry-run-first:

1. Record a validated baseline with explicit evidence.
2. Plan a regression range from that baseline to the current bad state.
3. Emit deterministic bisect guidance and machine-readable plan JSON.
4. Require explicit operator approval before creating temporary worktrees or
   running any bisect loop.

This design does not implement the commands. It defines the contract future work
must satisfy.

## Non-goals

- No global observability store.
- No cross-repo automatic bisect in one mutation.
- No destructive checkout in the user's current working tree.
- No automatic rollback, reset, stash, or force push.
- No baseline claim without validation evidence.
- No background CI history scraper in the first version.

## Proposed command surface

```bash
kog baseline record --name <name> --reason <text> --validate "pixi run quick-test"
kog baseline list
kog baseline show <baseline-id>
kog regression plan --baseline <baseline-id> --bad HEAD --validate "pixi run quick-test"
kog regression bisect --plan <plan.json> --dry-run
kog regression bisect --plan <plan.json> --worktree .kano/tmp/kog/bisect/<id> --confirm
```

Aliases can be added later only after the long-form commands are stable.

## Baseline storage model

Default storage should be repo-local and reviewable:

```text
.kano/kog/baselines/<baseline-id>.json
```

Temporary or exploratory runs may write to:

```text
.kano/tmp/kog/baselines/<baseline-id>.json
.kano/tmp/kog/regression-plans/<plan-id>.json
```

Persistent baseline writes should require a clean working tree or an explicit
plan-backed commit flow. KOG must not silently add generated baseline files to an
unrelated commit.

## Baseline metadata schema

Schema name: `kog.baseline.v1`

Required fields:

- `schema`
- `baseline_id`
- `created_at`
- `created_by`
- `reason`
- `workspace_root`
- `kog_version`
- `target_branch`
- `remote`
- `root_commit`
- `repos`
- `validation`

Each `repos[]` entry records:

- `path`
- `repo_kind` (`root`, `registered-subrepo`, `unregistered-subrepo`)
- `head`
- `branch`
- `upstream`
- `remote_head`
- `gitlink_commit` when applicable
- `dirty_state` (`clean`, `dirty`, `unknown`)
- `reachable_from_remote`

Each `validation[]` entry records:

- `command_label`
- `command`
- `exit_code`
- `started_at`
- `duration_ms`
- `artifact_paths`
- `summary`

Baselines with `dirty_state != clean` are allowed only as local scratch records
and must be rejected as last-known-good inputs for automated regression planning.

## Regression plan schema

Schema name: `kog.regression_plan.v1`

Required fields:

- `schema`
- `plan_id`
- `baseline_id`
- `bad_ref`
- `created_at`
- `validation_command`
- `candidate_repos`
- `ranges`
- `no_go`
- `recommended_next_step`

Each `ranges[]` entry records:

- `repo_path`
- `good_ref`
- `bad_ref`
- `commit_count`
- `range_status` (`ready`, `empty`, `unreachable`, `dirty`, `multi-repo-ambiguous`)
- `suggested_bisect_command`

The planner should choose `no_go=true` when a deterministic bisect would be
misleading or unsafe.

## No-go criteria

Regression planning must refuse or emit `no_go=true` when:

- The baseline has no successful validation evidence.
- Any candidate repo is dirty or has unresolved index/worktree conflicts.
- Any candidate repo has active rebase, merge, cherry-pick, revert, or bisect
  state.
- The good or bad ref is not reachable locally and cannot be fetched safely.
- More than one repo changed between the baseline and bad state and no single
  suspect repo is selected.
- The validation command is interactive, non-deterministic, or requires secrets
  without a declared non-interactive profile.
- The requested workflow would checkout commits in the user's active worktree.

## Validation strategy

Baseline record validation:

```bash
kog baseline record --name smoke --reason "pre-fix LKG" --validate "pixi run quick-test" --dry-run
kog baseline record --name smoke --reason "pre-fix LKG" --validate "pixi run quick-test" --confirm
```

Regression plan validation:

```bash
kog regression plan --baseline smoke --bad HEAD --validate "pixi run quick-test" --dry-run
```

Bisect validation should start with a generated disposable-worktree dry run. The
first implementation should print the exact worktree path and the exact test
command that would run, then stop.

## Agent usage examples

When a coding agent hits a regression:

1. Read the most recent baseline:
   `kog baseline list --branch <branch> --limit 5`
2. Plan the range:
   `kog regression plan --baseline <id> --bad HEAD --validate "pixi run quick-test" --dry-run`
3. Attach the resulting `kog.regression_plan.v1` JSON to the worklog or handoff.
4. If `no_go=true`, do not run bisect; report the blocker and ask for a narrower
   baseline, suspect repo, or deterministic validation command.
5. If `no_go=false`, request explicit operator approval before running
   `kog regression bisect --confirm` in a disposable worktree.

## Follow-up implementation slices

1. Add `kog baseline record/list/show` with JSON schemas and tests.
2. Add `kog regression plan` as dry-run-only range planner.
3. Add disposable-worktree bisect dry-run planning.
4. Add confirmed bisect execution only after the dry-run planner is stable.
5. Add report packaging so baseline and regression plans can be attached to
   KOG/KOB worklogs.

## Open questions

- Should persistent baselines be committed by default, or should KOG require
  `kog cpa` after writing them?
- Should CI attach a baseline automatically after green `coverage-all`, or
  should this remain an explicit release/operator action?
- Should baseline ids include branch names, ticket ids, or only timestamp plus
  content hash?
