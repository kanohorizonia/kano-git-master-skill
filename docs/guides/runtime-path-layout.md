# Runtime Path Layout

KOG resolves stable workspace and skill-asset paths through one native runtime
layout. Command implementations choose filenames and explicit command-line
overrides; they do not reconstruct `.kano`, plan, cache, or ignore-asset paths.

## Workspace layout

| Purpose | Default |
|---|---|
| KOG temporary root | `<workspace>/.kano/tmp/git/` |
| Shared plan | `<workspace>/.kano/tmp/git/plans/default-plan.json` |
| Generated plan files | `<workspace>/.kano/tmp/git/plans/` |
| Provider prompts | `<workspace>/.kano/tmp/git/provider-prompts/` |
| AI responses | `<workspace>/.kano/tmp/git/ai-responses/` |
| Workspace cache | `<workspace>/.kano/cache/git/` |
| Cached plans | `<workspace>/.kano/cache/git/plans/` |
| User cache | `<home>/.kano/cache/git/` |

`KOG_PLAN_FILE` overrides the shared plan path. `kano.cache.global-dir` keeps
its existing Git-config precedence for the user cache. An explicit
`--plan-file`, `--output`, ignore datasource, or policy path remains
command-owned and wins over these defaults.

## Skill-root resolution

The launcher-provided `KANO_GIT_SKILL_ROOT` is authoritative. The existing
`KANO_GIT_MASTER_ROOT` package/developer launcher contract is the next
authority. Direct native development runs then probe the workspace skill layout,
the checked-out skill root, and the user `.agents/skills/kano/` installation.

This preserves both release/package execution and a workspace containing
`.agents/skills/kano/kano-git-master-skill`.

## Ownership boundary

`kano_git_command_runtime` owns command-facing paths for commit, commit-push,
plan, cache, export, converge, TUI diagnostics, and packaged assets. Lower-level
`kano_git_core` cannot depend on the command library without introducing a
dependency cycle. Its repository-discovery cache and shell-executor debug log
therefore retain core-local construction of the same public `.kano/cache/git`
and `.kano/tmp/git` contract. Those are infrastructure bootstrap paths, not
independent command override points.

If core needs additional path families, promote the layout to a new core module
first; do not add another command-local resolver or claim that command runtime
can be imported downward.

## Ignore assets

New source and release layouts use:

```text
assets/ignore/
  datasource/
    manifest.json
    upstream/github-gitignore/
  local-rules/
    kano.gitignore
  policy/
    ignore-gate-allowlist.txt
```

The datasource manifest generates ignore candidates. Local rules are a
Kano-maintained datasource. The policy allowlist is operator safety policy and
never becomes a generated `.gitignore` rule.

For upgrade compatibility, a packaged skill that only contains the former
`assets/ignore-sources/` layout is still resolved. Canonical assets take
precedence whenever both layouts exist.
