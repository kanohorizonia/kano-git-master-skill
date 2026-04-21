You are filling exactly ONE commit plan entry for kano-git.

Step 1 (ignore cleanup): Inspect untracked files in the dirty context. If any look like build artifacts,
cache, generated output, logs, or editor noise, append them as patterns to `{{GITIGNORE_PATH}}`
(one pattern per line, grouped under a `# kog-auto` comment). Use `git rm --cached <path>` to
untrack any already-tracked artifacts. Only proceed to step 2 after ignore cleanup is complete.

Step 2 (fill): Fill the commit entry below.

Return STRICT JSON ONLY between markers:
BEGIN_KOG_PLAN_FILL_OPS
<json>
END_KOG_PLAN_FILL_OPS

Rules:
- Output exactly one commits item for index {{ENTRY_INDEX}}.
- Do not output any other index.
- Return one JSON object with a top-level `commits` array.
- Each commits item must include `index`, `message`, and nested `review.verdict` + `review.reason` fields.
- `message` MUST follow the Kano Commit Convention (KCC) specification (`[Subsystem][Type] Summary (Ticket)`) provided at the end of this prompt.
- `review.verdict` must be `pass`.
- No prose, no markdown fences, no commentary.
- Provider={{PROVIDER}} model={{MODEL}}

Required JSON schema:
{
  "commits": [
    {
      "index": {{ENTRY_INDEX}},
      "message": "feat(scope): concise commit message",
      "review": {
        "verdict": "pass",
        "reason": "Specific review rationale for this commit."
      }
    }
  ]
}

Target commit entry:
{{TARGET_ENTRY_JSON}}

Quality constraints:
- Produce one concrete, commit-ready message.
- If the `message` in the `Target commit entry` below is a generic placeholder (e.g. starting with `chore(...)` and mentioning `apply updates` or `combine commits`), you MUST rewrite it into a specific, KCC-compliant summary based on the actual code changes visible in the Dirty Context.
- Keep the message scoped to this entry only.
- `review.reason` must explain why this single entry is acceptable and confirm no secrets or
  unintended files are included. This serves as the combined safety review — no separate review
  pass will be run after this.
- Do not mention hidden tools or internal workflow.

Workspace dirty context:
{{DIRTY_CONTEXT}}
