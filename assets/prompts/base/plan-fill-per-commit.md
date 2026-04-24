You are filling exactly ONE commit plan entry for kano-git.

Step 1 (ignore cleanup): Inspect untracked files in the dirty context. If any look like build artifacts,
cache, generated output, logs, or editor noise, you must update `{{GITIGNORE_PATH}}`. Do not just blindly
append to it. Instead, overview the entire current `.gitignore` content, deduplicate existing patterns,
and organize it into a readable, well-structured, and maintainable file. Add any newly discovered artifact
patterns logically within the file. Use `git rm --cached <path>` to untrack any already-tracked artifacts.
Only proceed to step 2 after ignore cleanup is complete.

Step 2 (fill): Directly update entry index {{ENTRY_INDEX}} in the authoritative plan file.
Follow this SOP exactly:
1. Copy `{{PLAN_PATH_ABSOLUTE}}` to `{{PLAN_PATH_ABSOLUTE}}.tmp`.
2. In the `.tmp` file, find the commit entry at flat index {{ENTRY_INDEX}} under `stages.commit[*].commits[*]`
   and fill its `message` field with a KCC-compliant commit message, and set `review.verdict` to `pass`
   with a specific review reason.
3. Self-validate the `.tmp` file:
   a. Confirm the file is valid JSON (can be parsed without error).
   b. Confirm the `message` matches KCC format: `[Subsystem][Type] Summary`.
   c. Confirm the message is not a placeholder.
4. If validation fails, correct the `.tmp` file and retry (up to 3 times).
5. Once validation passes, overwrite `{{PLAN_PATH_ABSOLUTE}}` with the `.tmp` file contents, then delete `.tmp`.
6. Do NOT print any JSON output. Do NOT output BEGIN_KOG_PLAN_FILL_OPS markers.

Quality constraints:
- `message` MUST follow the Kano Commit Convention (KCC) specification (`[Subsystem][Type] Summary`) provided at the end of this prompt.
- If the existing `message` is a generic placeholder (e.g. starts with `chore(...)` mentioning `apply updates`),
  rewrite it into a specific, KCC-compliant summary based on the actual code changes in the Dirty Context.
- `review.reason` must explain why this entry is acceptable and confirm no secrets or unintended files are included.
- Do not modify `include`, `exclude`, `repo`, or any non-semantic field.
- Do not mention hidden tools or internal workflow.
- Provider={{PROVIDER}} model={{MODEL}}

Workspace dirty context:
{{DIRTY_CONTEXT}}
