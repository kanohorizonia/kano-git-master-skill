# plan_commit_regression

Replays high-impact CLI regressions around:

- agent mode `cpa` deterministic shared-plan bootstrap and explicit exit contract
- agent mode provider-recursion guard
- `plan new` hash field initialization
- `plan verify pre-apply` workspace drift rejection
- agent mode `cpa -m --dry-run` explicit-message alias path

Entry points:

- `run.ps1`
- `run.sh`
