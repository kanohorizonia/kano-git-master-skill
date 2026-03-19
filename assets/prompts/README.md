# Prompt Assets

This directory stores prompt templates used by native C++ AI flows.

## Structure

- `base/` — default prompt assets wired into the native CLI
- `dev/` — reserved for development/testing variants
- `user/` — reserved for user-local variants or future overlays

## Active prompt files

### Plan prompts

- `base/plan-init.md`
  - Purpose: generate a full initial plan JSON
  - Override env: `KOG_PLAN_PROMPT_TEMPLATE`
  - Placeholders:
    - `{{PROVIDER}}`
    - `{{MODEL}}`
    - `{{TEMPLATE_JSON}}`
    - `{{DIRTY_CONTEXT}}`

- `base/plan-fill-single.md`
  - Purpose: fill all existing `stages.commit` entries in one AI pass
  - Override env: `KOG_PLAN_FILL_SINGLE_PROMPT_TEMPLATE`
  - Placeholders:
    - `{{PROVIDER}}`
    - `{{MODEL}}`
    - `{{PLAN_JSON}}`
    - `{{DIRTY_CONTEXT}}`

- `base/plan-fill-per-commit.md`
  - Purpose: fill exactly one commit entry in `per-commit` / `adaptive` modes
  - Override env: `KOG_PLAN_FILL_PER_COMMIT_PROMPT_TEMPLATE`
  - Placeholders:
    - `{{PROVIDER}}`
    - `{{MODEL}}`
    - `{{ENTRY_INDEX}}`
    - `{{TARGET_ENTRY_JSON}}`
    - `{{DIRTY_CONTEXT}}`

- `base/plan-fill-retry.md`
  - Purpose: retry wrapper appended after validator rejection
  - Override env: `KOG_PLAN_FILL_RETRY_PROMPT_TEMPLATE`
  - Placeholders:
    - `{{BASE_PROMPT}}`
    - `{{FAILURE_CATEGORY_LINE}}`
    - `{{FAILURE_DETAIL_LINE}}`
    - `{{EXPECTED_COMMITS}}`
    - `{{RETURNED_COMMITS_LINE}}`
    - `{{PREVIOUS_RAW_SECTION}}`

### Commit prompts

- `base/commit-message.md`
  - Purpose: generate one commit message from staged diff context
  - Override env: `KOG_COMMIT_MESSAGE_PROMPT_TEMPLATE`
  - Placeholders:
    - `{{REPO_LABEL}}`
    - `{{STAGED_COUNT}}`
    - `{{UNSTAGED_COUNT}}`
    - `{{UNTRACKED_COUNT}}`
    - `{{STAGED_DIFF}}`

- `base/review.md`
  - Purpose: safety review for staged diff + commit message
  - Override env: `KOG_COMMIT_REVIEW_PROMPT_TEMPLATE`
  - Placeholders:
    - `{{MESSAGE}}`
    - `{{STAGED_DIFF}}`

## Editing guidance

- Keep output contracts explicit and machine-parseable.
- Prefer moving workflow enforcement into code/validators rather than overloading prompts.
- `single` prompts should be goal-oriented.
- `per-commit` prompts can be more constrained because the task surface is smaller.
- If a prompt change affects required output shape, update validators before relying on the new prompt.

## Runtime resolution order

For each prompt type, the loader checks in this order:
1. Environment-variable override path
2. Workspace-local asset path
3. Skill asset path

If no asset is found, C++ fallback prompt text is used.

## Provider subprocess environment toggles

These env vars are optional. If unset, native AI flows keep the previous safe/default CLI behavior.

### Copilot

- default behavior for native Copilot subprocesses: `--autopilot --max-autopilot-continues 12`
- default Copilot tool grants for native subprocesses: `--allow-tool shell(git:*) --allow-tool write`
- `KOG_COPILOT_AUTOPILOT` (`1|true|yes|on` to force on, `0|false|no|off` to disable the default)
- `KOG_COPILOT_MAX_AUTOPILOT_CONTINUES` (default: `12`)
- `KOG_COPILOT_CONTINUE` (`1|true|yes|on`)
- `KOG_COPILOT_RESUME` (`1|true|yes|on` for bare `--resume`, or set a session ID)
- `KOG_COPILOT_AGENT`
- `KOG_COPILOT_ADD_DIRS` (semicolon-separated)
- `KOG_COPILOT_ALLOW_TOOLS` (semicolon-separated; when unset, defaults to `shell(git:*)` and `write`)
- `KOG_COPILOT_ALLOW_URLS` (semicolon-separated)
- `KOG_COPILOT_AVAILABLE_TOOLS` (semicolon-separated)
- `KOG_COPILOT_EXCLUDED_TOOLS` (semicolon-separated)
- `KOG_COPILOT_ALLOW_ALL_TOOLS` (`1|true|yes|on`)
- `KOG_COPILOT_ALLOW_ALL_PATHS` (`1|true|yes|on`)
- `KOG_COPILOT_ALLOW_ALL_URLS` (`1|true|yes|on`)
- `KOG_COPILOT_ALLOW_ALL` (`1|true|yes|on`)

### Codex

- `KOG_CODEX_USE_EXEC` (`1|true|yes|on`) — switch from legacy `codex -q` to `codex exec`
- `KOG_CODEX_FULL_AUTO` (`1|true|yes|on`)
- `KOG_CODEX_EPHEMERAL` (`1|true|yes|on`)
- `KOG_CODEX_JSON` (`1|true|yes|on`)
- `KOG_CODEX_SANDBOX`
- `KOG_CODEX_APPROVAL`
- `KOG_CODEX_PROFILE`
- `KOG_CODEX_ADD_DIRS` (semicolon-separated)

### OpenCode

- `KOG_OPENCODE_CONTINUE` (`1|true|yes|on`)
- `KOG_OPENCODE_SESSION`
- `KOG_OPENCODE_FORK` (`1|true|yes|on`)
- `KOG_OPENCODE_AGENT`
- `KOG_OPENCODE_ATTACH`
- `KOG_OPENCODE_VARIANT`
- `KOG_OPENCODE_FORMAT`
- `KOG_OPENCODE_THINKING` (`1|true|yes|on`)

## Layered kog config

`kog_config.toml` is resolved in this order, later files overriding earlier ones:

1. system: `.kano/kog_config.toml`
2. global: `$HOME/.kano/kog_config.toml`
3. local: `.kano/kog_config.toml`

Current config keys:

- `[ai.model.auto]` — auto model thresholds/models
- `[plan.ai]`
  - `commit_generation_mode = "single" | "per-commit" | "adaptive"`
