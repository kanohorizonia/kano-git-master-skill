# Ignore Datasource Root

This directory is the default datasource root for ignore-plan generation.

Contents:
- `local/`: Kano-maintained deterministic ignore rules and mappings.
- `upstream/github-gitignore/`: upstream template corpus via git submodule.

Policy files under `local/`:
- `custom.gitignore`: deterministic local ignore rules merged into ignore-plan suggestions.
- `ignore-gate-allowlist.txt`: explicit path allowlist for ignore-gate false-positive exceptions.

Ignore-plan generation should read this folder as a whole datasource set.

Path resolution rules (`local/datasource.manifest.json`):
- `path` supports absolute path and relative path.
- Relative `path` is resolved against the manifest file directory (`assets/ignore-sources/local/`).
- On Windows, POSIX-style inputs from Git Bash/Cygwin/WSL forms are normalized before resolve (for example `/d/...`, `/mnt/d/...`, `/cygdrive/d/...`).

Examples:
- `./custom.gitignore` -> `assets/ignore-sources/local/custom.gitignore`
- `../upstream/github-gitignore` -> `assets/ignore-sources/upstream/github-gitignore`

Sync upstream source:

```bash
./kog plan datasource-sync --dry-run
./kog plan datasource-sync
```
