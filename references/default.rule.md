# Default Review/Commit Rules

Use conservative safety posture for non-tooling repositories.

1. Block obvious secrets, credentials, private keys, and generated binary blobs.
2. Prefer minimal, focused commits; avoid unrelated broad refactors.
3. For git push automation:
   - `--force-with-lease` is allowed only with explicit target branch and clear safeguards.
   - Plain `--force` and `--mirror` should be treated as unsafe by default.
4. If automation rewrites history, require checks for:
   - clean working tree
   - explicit remote/branch target
   - dry-run support when practical
5. Keep commit messages concise and conventional when possible.
