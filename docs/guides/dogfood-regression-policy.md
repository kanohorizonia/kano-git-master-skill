# Dogfood-to-Regression Policy

Every reproducible production or dogfood defect must leave a traceable,
same-scenario regression artifact. A source fix without a linked case is an
open coverage gap, even when an adjacent test happens to exercise similar code.

## Required workflow

1. Record or update the backlog item with the observed failure and date.
2. Reproduce the failure in an isolated fixture before weakening any safety
   gate.
3. Fix the verified root cause.
4. Add a regression case that asserts the original outcome and final state.
5. Add the incident and exact test mapping to
   `assets/regression/incidents.json`.
6. Run `kog regression coverage --fail-on-gap`, the focused test, and the
   normal quick/full lanes appropriate to the change.
7. Replace a work-item change reference with the published commit SHA when the
   source is published.

The minimum incident metadata is:

- `incident_id`
- `incident_date` in `YYYY-MM-DD`
- a verified `root_cause`
- `backlog_ref`
- a concrete `change_ref`
- at least one `workflow_contracts` marker
- one or more stable regression `case_id` values when covered

Each linked case records its exact `test_name`, repo-relative `test_file`,
`test_kind`, and `mapping_state: source-linked`. Source linkage does not claim
that a test was executed or passed. Test execution evidence belongs in the
normal validation report or backlog worklog.

## Fixtures and names

Use `ITEM-ID/stable-scenario-slug` for `case_id`. Keep the exact Catch2 test
name stable after it is registered. Put durable fixtures below the owning test
suite, preferably `fixtures/ITEM-ID/scenario-slug/`; cases that can generate an
isolated temporary Git repository should do so instead of checking in mutable
repository state.

Use the closest native suite:

- `kano_git_cli_tests` for black-box command and Git-state behavior
- `kano_git_commit_plan_tests` for plan/pathspec unit and property behavior
- `kano_git_regression_tests` for manifest, command, and registry drift checks
- `kano_git_integration_tests` for bounded cross-process behavior
- `e2e/` only when the contract cannot be proven in a bounded native fixture

Long-running E2E execution is non-blocking initially. Its source mapping may be
registered, but it does not become a merge gate merely by appearing in the
manifest. Fast same-scenario native coverage remains preferred.

Canonical workflow markers include:

- `converges-or-fails`
- `detached-head`
- `post-sync-gitlink`
- `ahead-clean-push`
- `nested-repo-child-first`
- `agent-mode-plan`
- `unrelated-stage-preservation`
- `plan-scope`

Add a new marker only when it describes a reusable workflow contract.

## Template and validation

Copy `assets/regression/case-template.json` as a drafting aid, then replace
every sentinel before merging its entry into `incidents.json`. The parser
rejects unresolved values such as `pending:*`, `TODO`, `TBD`, `YYYY-MM-DD`,
angle-bracket placeholders, sentinel IDs, and template paths. Placeholder
matching is field-aware: verified root-cause prose may describe a pending queue
or template defect, while an exact placeholder or placeholder prefix is
rejected. Required strings and workflow markers must be non-blank printable
text; incident dates must be calendar-valid and test files must be normalized,
repo-relative, host-independent paths.

```bash
./scripts/kog regression coverage
./scripts/kog regression coverage --format json
./scripts/kog regression coverage --fail-on-gap
./scripts/kog regression coverage --manifest path/to/incidents.json
```

Exit codes are stable:

- `0`: valid manifest; gaps are informational unless the gate is enabled
- `2`: invalid format, unreadable JSON, invalid schema, or unresolved metadata
- `3`: valid manifest with one or more gaps under `--fail-on-gap`

The default registry is intentionally evidence-backed. Its test files and exact
case names are checked by `kano_git_regression_tests` so stale mappings fail
instead of silently reporting coverage.
