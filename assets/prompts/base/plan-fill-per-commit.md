You are filling exactly ONE commit plan entry for kano-git by editing a working plan file.

Step 1 (ignore cleanup): Inspect untracked files in the dirty context. If any look like build artifacts,
cache, generated output, logs, or editor noise, update the working gitignore copy at
`{{WORKING_GITIGNORE_PATH}}` with only the minimal ignore-rule additions needed.

Step 2 (fill): Edit the working plan file at `{{WORKING_PLAN_PATH_ABSOLUTE}}` so that only commit entry
index `{{ENTRY_INDEX}}` is filled with a concrete message and review reason. Do not rewrite unrelated
entries except for structural formatting needed to preserve valid JSON.

Step 3 (finish): Save the working plan file and stop. Do not emit JSON payloads on stdout.

Rules:
- Edit the working plan file in place; do not return a replacement JSON payload.
- Only fill the commit entry at index `{{ENTRY_INDEX}}`.
- Leave other commit entries semantically unchanged.
- `message` MUST follow the Kano Commit Convention (KCC) specification (`[Subsystem][Type] Summary (Ticket)`) provided at the end of this prompt.
- `review.verdict` must be `pass`.
- At most print a brief single-line completion note.
- Provider={{PROVIDER}} model={{MODEL}}

Execution context:
- The authoritative plan file absolute path is `{{PLAN_PATH_ABSOLUTE}}`.
- The working plan file absolute path is `{{WORKING_PLAN_PATH_ABSOLUTE}}`.
- The working plan file workspace-relative path is `{{WORKING_PLAN_PATH}}`.
- The authoritative .gitignore path is `{{GITIGNORE_PATH}}`.
- The working .gitignore copy to edit is `{{WORKING_GITIGNORE_PATH}}`.

Target commit entry:
{{TARGET_ENTRY_JSON}}

Current working plan JSON snapshot:
{{PLAN_JSON}}

Quality constraints:
- `message` MUST follow the Kano Commit Convention (KCC) specification (`[Subsystem][Type] Summary`) provided at the end of this prompt.
- `message` MUST NOT include any Ticket ID or `(NO-TICKET)` suffix. Stop the message after the Summary.
- If the existing `message` is a generic placeholder (e.g. starts with `chore(...)` mentioning `apply updates`),
  rewrite it into a specific, KCC-compliant summary based on the actual code changes in the Dirty Context.
  Even if the only change is to `.gitignore`, you MUST generate a specific KCC-compliant message (e.g., `[Build][Chore] Update .gitignore to exclude build artifacts`).
- `review.reason` must explain why this entry is acceptable and confirm no secrets or unintended files are included.
- Do not modify `include`, `exclude`, `repo`, or any non-semantic field.
- Do not mention hidden tools or internal workflow.
- Provider={{PROVIDER}} model={{MODEL}}

Workspace dirty context:
{{DIRTY_CONTEXT}}
