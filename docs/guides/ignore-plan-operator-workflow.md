# Ignore Plan Operator Workflow

This guide is for day-to-day operators who run the ignore stage in plan pipelines.

It focuses on:
- deterministic command sequence (`init -> ignore-init -> verify -> apply`)
- fast troubleshooting from error text to next action
- evidence checklist for bug reports and audit trails

## Scope

This guide covers only `stages.ignore` operations:
- `kog plan new`
- `kog plan ignore-init`
- `kog plan verify pre-apply --stage ignore`
- `kog plan apply --stage ignore`
- `kog plan verify ignore --context plan`

## Standard Flow

Use one plan file consistently in the whole run:

```bash
# 1) Create or refresh plan template
./kog plan new --plan-file .kano/cache/git/plans/default-plan.json --force

# 2) Populate ignore candidates from current working tree
./kog plan ignore-init --plan-file .kano/cache/git/plans/default-plan.json --max-per-repo 200

# 3) Validate ignore-stage schema and required shape
./kog plan verify pre-apply --stage ignore --plan-file .kano/cache/git/plans/default-plan.json

# 4) Apply merged/deduplicated rules to targets
./kog plan apply --stage ignore --plan-file .kano/cache/git/plans/default-plan.json

# 5) Confirm gate status
./kog plan verify ignore --context plan
```

## Troubleshooting Matrix

### Case: `plan file not found/readable`

Typical error:
- `Error: plan file not found/readable: <path>`

Action:
1. Run `kog plan new --plan-file "<path>" --force`
2. Re-run the original command with the same `--plan-file`.

### Case: `invalid --stage value`

Typical error:
- `Error: invalid --stage value: <value> (expected ignore|commit|all)`

Action:
1. Use one of `ignore`, `commit`, `all`.
2. For ignore flow, always use `--stage ignore`.

### Case: `stages.ignore missing` or schema invalid

Typical errors:
- `Error: plan schema invalid: stages.ignore missing`
- `Error: plan schema invalid: cannot locate stages.ignore array`

Action:
1. Regenerate plan template:
   - `kog plan new --plan-file "<path>" --force`
2. Repopulate ignore stage:
   - `kog plan ignore-init --plan-file "<path>"`
3. Re-verify:
   - `kog plan verify pre-apply --stage ignore --plan-file "<path>"`

### Case: `no ignore plan entries found in stages.ignore`

Typical error:
- `Error: no ignore plan entries found in stages.ignore.`

Action:
1. Re-run ignore-init on the same plan file:
   - `kog plan ignore-init --plan-file "<path>"`
2. Confirm working tree actually has artifact-like untracked files.
3. Run verify/apply again.

### Case: ignore gate still fails after apply

Typical signal:
- `Error: ignore gate failed (plan); unresolved untracked artifact-like files detected.`

Action:
1. Inspect listed files and classify:
   - must-ignore
   - must-track
2. Update plan/target `.gitignore` decisions and re-apply.
3. Recheck:
   - `kog plan verify ignore --context plan`

## Evidence Checklist

When opening a bug or attaching acceptance evidence, include:

1. Command transcript
- exact commands, full flags, and command order
- include `--plan-file` path used in every step

2. Plan artifact
- plan JSON used in the run
- especially `stages.ignore` content before and after apply

3. Gate outputs
- before-apply `ignore-gate` output and exit code
- after-apply `ignore-gate` output and exit code

4. Environment snapshot
- OS/shell
- `kano-git --version`
- workspace root path

5. File evidence
- relevant `.gitignore` target file diff
- list of unresolved files if gate still fails

## Minimal Acceptance Script

A repeatable script exists:

```bash
bash .agents/skills/kano/kano-git-master-skill/scripts/core/acceptance-ignore-plan.sh
```

Expected summary:
- `before_exit=3` with positive candidate count
- `after_exit=0` with zero candidate count
- final `PASS` line

