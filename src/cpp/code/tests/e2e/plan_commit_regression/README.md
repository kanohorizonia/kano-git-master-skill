# plan_commit_regression

Replays high-impact CLI regressions around:

- agent mode `cpa` guard behavior
- `plan new` hash field initialization
- `plan verify pre-apply` workspace drift rejection
- agent mode `cpa -m --dry-run` smoke path

Entry points:

- `run.ps1`
- `run.sh`
