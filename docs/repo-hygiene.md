# KOG Repo Hygiene

The `kog repo-hygiene` command is designed to detect and fix Git-index executable bits and CRLF/LF normalization issues before committing or exporting archives.

## Why is this needed?

- **Executable bits (`chmod +x`) on Windows**: Git relies on the executable bit (`100755`) stored in its index. On Windows, where POSIX file permissions aren't natively mapped to the filesystem in the same way, `chmod +x` is often not detected by Git (especially when `core.filemode=false`).
- **Archive exports (`git archive`)**: `git archive` preserves the blob content exactly as tracked in the Git index along with its mode. If a shell script is tracked as `100644` (normal file) or stored with `CRLF` in the index due to missing `.gitattributes` rules, it will be exported identically. This leads to broken scripts when the archive is unzipped on a Linux/macOS machine.
- To correctly fix cross-platform executable issues, `git update-index --chmod=+x <file>` must be used.

## Usage

### Check Mode

Run a non-destructive scan of tracked files in the repository:

```bash
kog repo-hygiene check
```

This will output a summary of LF issues (where LF-required files contain CRLF in the Git index), Executable mode issues (where shell scripts are missing the `100755` mode), and Gitattributes issues.

You can also run a strict archive-safe check:

```bash
kog repo-hygiene check --archive-safe
```

Archive-safe mode also runs the shell-only pre-commit quality gate when the
script is available. That gate covers current public docs references, root
wrapper generator smoke tests, launcher shell syntax, and Git-index hygiene.

### Fix Mode

Automatically apply missing executable bits to scripts and renormalize LF line endings:

```bash
kog repo-hygiene fix
```

This command will:
1. Identify all scripts requiring an executable bit and apply `git update-index --chmod=+x` to them.
2. Identify LF-required files and execute `git add --renormalize` to correct the index.

### Shell-only pre-commit quality gate

Run this before committing, even when the native binary has not been built yet:

```bash
src/shell/test/pre-commit-quality-gate.sh
```

Enable the tracked hook in this repository with:

```bash
git config core.hooksPath .githooks
```

The hook can be bypassed for emergency commits with:

```bash
KOG_SKIP_PRECOMMIT_QUALITY_GATE=1 git commit
```

### Verification

You can verify the fixes using standard Git commands:

```bash
# Verify LF normalization
git ls-files --eol -- <path>

# Verify executable mode (should show 100755)
git ls-files -s <path>
```
