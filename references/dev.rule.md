# Developer Rule Set (kano-git-master-skill)

This rule file applies to `kano-git-master-skill` development.

1. Changes that add git automation are expected in this repository.
2. `git push --force-with-lease` is acceptable for fork-sync workflows when:
   - operation is user-invoked
   - target branch is explicit
   - clean-tree and remote checks exist
3. Do not fail solely on force-with-lease keyword detection.
4. Still fail on:
   - secret/key leaks
   - plain `--force` / `--mirror` without strong safeguards
   - removal of existing safety checks
5. Preserve multi-repo safety:
   - skip repos that do not match required remotes
   - provide clear skip/error messages
