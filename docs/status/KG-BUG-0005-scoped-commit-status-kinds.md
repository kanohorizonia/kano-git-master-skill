# KG-BUG-0005 Scoped Commit Status Kinds

`kog converge` scoped commit planning must preserve Git status semantics when it
builds plan includes. Add, delete, and rename entries are not equivalent to a
single current path:

- Deleted paths may no longer exist in the working tree but are still valid
  tracked pathspecs for `git add -A`.
- Staged renames must include both the old and new paths because plan execution
  resets the index before staging the scoped commit.
- Source, test, and documentation paths may share one ticket intent when the
  dirty set exposes a single work item id.

Regression coverage:

- `NormalizeCommitPlanRepoPaths accepts staged rename old pathspecs tracked in HEAD`
- `converge agent mode scopes add delete and rename source paths to one ticket intent`
