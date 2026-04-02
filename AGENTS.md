# AGENTS.md

## Temporary test artifacts

- Put all smoke-test outputs, local verification outputs, scratch build outputs, and ad-hoc debug artifacts under `.kano/tmp/`.
- Do not create repo-root directories like `_local-ci-smoke/`, `_local-msvc-debug/`, `_local-msvc-smoke/`, or similar one-off test folders.
- If a test needs multiple subdirectories, nest them under `.kano/tmp/<purpose>/`.
- Prefer names that make cleanup obvious, for example:
  - `.kano/tmp/local-ci-smoke/`
  - `.kano/tmp/msvc-debug/`
  - `.kano/tmp/opencpp-smoke/`
- Before finishing work, clean up temporary test artifacts that are no longer needed.

## Ignore-gate expectations

- Temporary verification artifacts must not be staged for commit.
- If a local test needs files that should survive briefly between runs, keep them in `.kano/tmp/` so ignore rules and safety checks can treat them as workspace-local temporary state.
