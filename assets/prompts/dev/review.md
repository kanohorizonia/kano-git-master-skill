Development mode for kano-git-master-skill.

Do not fail solely because the patch contains:
- repository discovery logic
- fork sync automation
- git push --force-with-lease

PASS is reasonable when safeguards exist:
- explicit remotes/branches
- clean-tree checks or explicit skip behavior
- dry-run support or equivalent preview
- no plain --force or --mirror shortcuts

FAIL when:
- secrets/credentials are introduced
- safeguards are removed around destructive operations
- plain --force/--mirror is added without strong constraints

