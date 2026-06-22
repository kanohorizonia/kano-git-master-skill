#!/usr/bin/env bash
# rewrite-author.sh — Normalize git author/committer identity and force-push.
#
# Designed for monorepos with submodules. Walks submodules recursively in
# post-order (deepest first), rewrites each repo's history, then re-rewrites
# the superproject so its tree picks up the new submodule SHAs.
#
# USAGE:
#   rewrite-author.sh [OPTIONS] [PATH...]
#
# If no PATH is given, operates on the current directory.
# If --recursive, also walks into submodules (post-order).
#
# OPTIONS:
#   -n, --target-name=NAME       Target author/committer name (required)
#   -e, --target-email=EMAIL     Target email (required)
#   -b, --branch=BRANCH          Branch to rewrite (default: main)
#   -m, --mode=MODE              target-only (default) | all-to-target
#                                 target-only  : only rewrite commits whose author/committer
#                                                email matches --target-email. Preserves
#                                                commits with other emails (e.g. AI agents).
#                                 all-to-target: rewrite every commit to the target identity.
#   -r, --recursive              Also rewrite submodules (post-order traversal).
#   -d, --dry-run                Show what would happen, do not modify anything.
#       --no-push                Rewrite but don't push to remote.
#       --no-backup              Remove backup branch after successful push.
#       --keep-originals         Keep filter-branch refs/original/* after success.
#       --no-stash               Don't auto-stash uncommitted changes (abort if dirty).
#       --backup-prefix=PREFIX   Backup branch prefix (default: backup/author-rewrite).
#   -h, --help                   Show this help.
#
# EXAMPLES:
#   # Dry run on current repo (superproject) only
#   rewrite-author.sh --target-name="Example Author" --target-email=author@example.com --dry-run
#
#   # Dry run on superproject + all submodules
#   rewrite-author.sh --recursive --dry-run
#
#   # Rewrite a single submodule and push
#   rewrite-author.sh kano-jenkins-skill
#
#   # Rewrite all submodules of a specific repo
#   rewrite-author.sh --recursive kano-git-master-skill
#
#   # Use a different target identity
#   rewrite-author.sh --target-name="New Name" --target-email=new@example.com
#
# EXIT CODES:
#   0  success
#   1  bad arguments
#   2  preflight failure (e.g. branch missing)
#   3  filter-branch failure
#   4  push failure
#   5  stash/pop failure
#
# REQUIRES: git, bash 3.2+ (works in git bash on Windows).
#
# SAFETY:
#   - Always creates a backup branch before rewriting (unless --no-backup after push).
#   - Uses --force-with-lease (refuses to clobber unrelated remote updates).
#   - Skips repos whose history is already correctly normalized.
#   - Auto-stashes uncommitted changes (unless --no-stash).

set -euo pipefail

# ---------------------------------------------------------------------------
# Defaults
# ---------------------------------------------------------------------------
TARGET_NAME=""
TARGET_EMAIL=""
BRANCH="main"
MODE="target-only"
RECURSIVE=false
DRY_RUN=false
NO_PUSH=false
NO_BACKUP=false
KEEP_ORIGINALS=false
NO_STASH=false
BACKUP_PREFIX="backup/author-rewrite"

# Runtime
declare -a TARGETS=()
declare -a PROCESSED=()  # for post-order traversal
declare -i EXIT_CODE=0
ORIG_DIR="$(pwd)"
TS="$(date -u +%Y%m%d-%H%M%S)"

# ---------------------------------------------------------------------------
# Logging
# ---------------------------------------------------------------------------
_ts() { date +%H:%M:%S; }
log() { echo "[$(_ts)] $*"; }
err() { echo "[$(_ts)] ERROR: $*" >&2; }
section() { echo; echo "================================================================"; echo "  $*"; echo "================================================================"; }

# ---------------------------------------------------------------------------
# Help
# ---------------------------------------------------------------------------
usage() {
    sed -n '2,55p' "$0" | sed 's/^# \{0,1\}//'
}

# ---------------------------------------------------------------------------
# Argument parsing
# ---------------------------------------------------------------------------
parse_args() {
    while [ $# -gt 0 ]; do
        case "$1" in
            -h|--help) usage; exit 0 ;;
            -n=*|--target-name=*) TARGET_NAME="${1#*=}" ;;
            -e=*|--target-email=*) TARGET_EMAIL="${1#*=}" ;;
            -b=*|--branch=*) BRANCH="${1#*=}" ;;
            -m=*|--mode=*) MODE="${1#*=}" ;;
            -r|--recursive) RECURSIVE=true ;;
            -d|--dry-run) DRY_RUN=true ;;
            --no-push) NO_PUSH=true ;;
            --no-backup) NO_BACKUP=true ;;
            --keep-originals) KEEP_ORIGINALS=true ;;
            --no-stash) NO_STASH=true ;;
            --backup-prefix=*) BACKUP_PREFIX="${1#*=}" ;;
            -*) err "Unknown option: $1"; usage; exit 1 ;;
            *) TARGETS+=("$1") ;;
        esac
        shift
    done

    if [ "$MODE" != "target-only" ] && [ "$MODE" != "all-to-target" ]; then
        err "Invalid --mode: $MODE (expected: target-only or all-to-target)"
        exit 1
    fi

    if [ -z "$TARGET_NAME" ] || [ -z "$TARGET_EMAIL" ]; then
        err "--target-name and --target-email are required"
        exit 1
    fi

    if [ ${#TARGETS[@]} -eq 0 ]; then
        TARGETS=(".")
    fi
}

# ---------------------------------------------------------------------------
# Pre-flight: check that filter-branch will work for a given repo
# Returns 0 if OK, 2 on hard error.
# ---------------------------------------------------------------------------
preflight() {
    local repo_path="$1"
    if ! git -C "$repo_path" rev-parse --git-dir >/dev/null 2>&1; then
        err "[$repo_path] Not a git repository"
        return 2
    fi
    if ! git -C "$repo_path" rev-parse --verify "$BRANCH" >/dev/null 2>&1; then
        err "[$repo_path] Branch '$BRANCH' does not exist"
        return 2
    fi
    return 0
}

# ---------------------------------------------------------------------------
# Check if a repo's branch already has the correct author normalization.
# Echoes 0 if rewrite is needed, 1 if already correct.
# ---------------------------------------------------------------------------
needs_rewrite() {
    local repo_path="$1"
    local authors
    authors=$(git -C "$repo_path" log "$BRANCH" --format='%an <%ae>' | sort -u)

    if [ "$MODE" = "all-to-target" ]; then
        # Any commit with different identity needs rewrite
        if echo "$authors" | grep -vqF "$TARGET_NAME <$TARGET_EMAIL>"; then
            return 0
        fi
        return 1
    else
        # target-only: any commit with target email but different name needs rewrite
        if echo "$authors" | grep -F "<$TARGET_EMAIL>" | grep -vqF "$TARGET_NAME <$TARGET_EMAIL>"; then
            return 0
        fi
        return 1
    fi
}

# ---------------------------------------------------------------------------
# Build the env-filter payload. Use a here-doc to avoid quoting hell.
# Echoes the filter body (with leading newline).
# target-only  : only rewrites commits whose author/committer email matches
#                --target-email. Preserves commits with other emails.
# all-to-target: unconditionally rewrites every commit.
# ---------------------------------------------------------------------------
build_env_filter() {
    if [ "$MODE" = "all-to-target" ]; then
        cat <<EOF

export GIT_AUTHOR_NAME="$TARGET_NAME"
export GIT_AUTHOR_EMAIL="$TARGET_EMAIL"
export GIT_COMMITTER_NAME="$TARGET_NAME"
export GIT_COMMITTER_EMAIL="$TARGET_EMAIL"
EOF
    else
        cat <<EOF

if [ "\$GIT_AUTHOR_EMAIL" = "$TARGET_EMAIL" ]; then
    export GIT_AUTHOR_NAME="$TARGET_NAME"
fi
if [ "\$GIT_COMMITTER_EMAIL" = "$TARGET_EMAIL" ]; then
    export GIT_COMMITTER_NAME="$TARGET_NAME"
fi
EOF
    fi
}

# ---------------------------------------------------------------------------
# Auto-stash uncommitted changes in a repo.
# Echoes the stash ref (or "none" if nothing to stash).
# Sets STASH_REF global.
# ---------------------------------------------------------------------------
STASH_REF="none"
stash_if_dirty() {
    local repo_path="$1"
    if [ "$NO_STASH" = true ]; then
        if ! git -C "$repo_path" diff --quiet HEAD 2>/dev/null \
           || ! git -C "$repo_path" diff --quiet --cached 2>/dev/null; then
            err "[$repo_path] Working tree dirty and --no-stash set; aborting this repo"
            return 5
        fi
        STASH_REF="none"
        return 0
    fi

    if git -C "$repo_path" diff --quiet HEAD 2>/dev/null \
       && git -C "$repo_path" diff --quiet --cached 2>/dev/null; then
        STASH_REF="none"
        return 0
    fi

    local stash_name="kano-rewrite-author: $repo_path @ $TS"
    if [ "$DRY_RUN" = true ]; then
        log "[$repo_path] [DRY-RUN] Would stash with message: $stash_name"
        STASH_REF="dry-run"
        return 0
    fi

    if git -C "$repo_path" stash push -u -m "$stash_name" >/dev/null 2>&1; then
        STASH_REF="$stash_name"
        log "[$repo_path] Stashed uncommitted changes"
        return 0
    else
        err "[$repo_path] stash push failed"
        return 5
    fi
}

pop_stash() {
    local repo_path="$1"
    if [ "$STASH_REF" = "none" ] || [ "$STASH_REF" = "dry-run" ]; then
        return 0
    fi
    if [ "$DRY_RUN" = true ]; then
        return 0
    fi
    if git -C "$repo_path" stash pop >/dev/null 2>&1; then
        log "[$repo_path] Restored stashed changes"
    else
        err "[$repo_path] stash pop failed; stash is preserved at $STASH_REF"
    fi
}

# ---------------------------------------------------------------------------
# Rewrite a single repo. Returns 0 on success, non-zero on failure.
# ---------------------------------------------------------------------------
rewrite_repo() {
    local repo_path="$1"
    local rel_path="${repo_path#$ORIG_DIR/}"
    [ "$rel_path" = "$repo_path" ] && rel_path="$repo_path"
    local backup_branch="${BACKUP_PREFIX}-${TS}-${rel_path//[\/\\:]/_}"

    section "[$rel_path] rewriting on '$BRANCH'"

    if ! preflight "$repo_path"; then
        return 2
    fi

    local old_head
    old_head=$(git -C "$repo_path" rev-parse "$BRANCH")
    log "Pre-rewrite HEAD : $old_head"
    log "Author identities before:"
    git -C "$repo_path" log "$BRANCH" --format='%an <%ae>' | sort -u | sed 's/^/  /'

    if ! needs_rewrite "$repo_path"; then
        log "Already correctly normalized — skipping"
        return 0
    fi

    if ! stash_if_dirty "$repo_path"; then
        return 5
    fi

    # Create backup branch
    if [ "$DRY_RUN" = true ]; then
        log "[DRY-RUN] Would create backup branch: $backup_branch"
    else
        git -C "$repo_path" branch "$backup_branch" "$BRANCH"
        log "Created backup branch: $backup_branch"
    fi

    # Build and run filter-branch
    local filter
    filter=$(build_env_filter)

    if [ "$DRY_RUN" = true ]; then
        log "[DRY-RUN] Would run: git filter-branch -f --env-filter '<see body>' -- $BRANCH"
        log "[DRY-RUN] Filter body:"
        echo "$filter" | sed 's/^/    /'
    else
        if FILTER_BRANCH_SQUELCH_WARNING=1 \
           git -C "$repo_path" filter-branch -f --env-filter "$filter" -- "$BRANCH" >/dev/null 2>&1; then
            log "filter-branch completed"
        else
            err "filter-branch failed for [$rel_path]"
            pop_stash "$repo_path" || true
            return 3
        fi
    fi

    # Verify
    local new_head
    new_head=$(git -C "$repo_path" rev-parse "$BRANCH")
    log "Post-rewrite HEAD: $new_head"
    log "Author identities after:"
    git -C "$repo_path" log "$BRANCH" --format='%an <%ae>' | sort -u | sed 's/^/  /'

    # Tree-equivalence check
    if [ "$DRY_RUN" = false ]; then
        local old_tree new_tree
        old_tree=$(git -C "$repo_path" rev-parse "$backup_branch^{tree}" 2>/dev/null || echo "missing")
        new_tree=$(git -C "$repo_path" rev-parse "$BRANCH^{tree}")
        if [ "$old_tree" = "$new_tree" ] && [ "$old_tree" != "missing" ]; then
            log "Top-level tree: IDENTICAL (only metadata changed) ✓"
        else
            err "Top-level tree CHANGED — review carefully!"
            err "  old: $old_tree"
            err "  new: $new_tree"
        fi
    fi

    # Clean up filter-branch's refs/original (unless --keep-originals)
    if [ "$KEEP_ORIGINALS" = false ] && [ "$DRY_RUN" = false ]; then
        local orig_refs
        orig_refs=$(git -C "$repo_path" for-each-ref --format='%(refname)' refs/original/ || true)
        if [ -n "$orig_refs" ]; then
            echo "$orig_refs" | while read -r ref; do
                [ -z "$ref" ] && continue
                git -C "$repo_path" update-ref -d "$ref" 2>/dev/null || true
            done
            log "Removed refs/original/* backups"
        fi
    fi

    # Push
    if [ "$NO_PUSH" = false ]; then
        if [ "$DRY_RUN" = true ]; then
            log "[DRY-RUN] Would: git push --force-with-lease origin $BRANCH"
        else
            if git -C "$repo_path" push --force-with-lease origin "$BRANCH" 2>&1 | sed 's/^/  /'; then
                log "Push to origin/$BRANCH succeeded"
            else
                err "Push to origin/$BRANCH FAILED"
                err "Backup preserved at: $backup_branch"
                pop_stash "$repo_path" || true
                return 4
            fi
        fi
    else
        log "Skipped push (--no-push)"
    fi

    # Remove backup if requested
    if [ "$NO_BACKUP" = true ] && [ "$DRY_RUN" = false ]; then
        git -C "$repo_path" branch -D "$backup_branch" 2>/dev/null \
            && log "Removed backup branch: $backup_branch"
    fi

    pop_stash "$repo_path" || true
    return 0
}

# ---------------------------------------------------------------------------
# Walk submodules in post-order (deepest first). For a given superproject,
# recurse into each submodule first, then process the superproject itself.
# ---------------------------------------------------------------------------
collect_submodules_recursive() {
    local base="$1"
    if [ ! -f "$base/.gitmodules" ]; then
        return 0
    fi
    while read -r path; do
        [ -z "$path" ] && continue
        # If submodule is not initialized, skip
        if ! git -C "$base" submodule status "$path" >/dev/null 2>&1; then
            continue
        fi
        local full="$base/$path"
        if [ -d "$full" ] && [ -f "$full/.gitmodules" ]; then
            collect_submodules_recursive "$full"
        fi
        echo "$full"
    done < <(git -C "$base" config --file .gitmodules --get-regexp '^submodule\..*\.path$' | awk '{print $2}')
}

# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------
main() {
    parse_args "$@"

    section "rewrite-author.sh configuration"
    log "Target identity : $TARGET_NAME <$TARGET_EMAIL>"
    log "Branch          : $BRANCH"
    log "Mode            : $MODE"
    log "Recursive       : $RECURSIVE"
    log "Dry run         : $DRY_RUN"
    log "Push            : $([ "$NO_PUSH" = true ] && echo "no" || echo "yes")"
    log "Auto-stash      : $([ "$NO_STASH" = true ] && echo "no" || echo "yes")"
    log "Backup prefix   : $BACKUP_PREFIX"
    log "Targets         : ${TARGETS[*]}"

    local all_repos=()
    for target in "${TARGETS[@]}"; do
        target="${target%/}"
        [ -z "$target" ] && target="."
        # Make absolute if relative
        case "$target" in
            /*|[A-Za-z]:/*|[A-Za-z]:\\*) ;;
            *) target="$ORIG_DIR/$target" ;;
        esac
        if [ "$RECURSIVE" = true ]; then
            # Collect submodules in post-order, then add the superproject last
            while read -r sub; do
                [ -z "$sub" ] && continue
                all_repos+=("$sub")
            done < <(collect_submodules_recursive "$target")
            all_repos+=("$target")
        else
            all_repos+=("$target")
        fi
    done

    if [ ${#all_repos[@]} -eq 0 ]; then
        err "No repositories to process"
        exit 1
    fi

    section "Processing ${#all_repos[@]} repositor(ies) in post-order"
    for r in "${all_repos[@]}"; do
        log " - $r"
    done

    for repo in "${all_repos[@]}"; do
        if rewrite_repo "$repo"; then
            continue
        fi
        local_rc=$?
        EXIT_CODE=$local_rc
        err "Failed on: $repo (exit=$local_rc)"
    done

    section "Summary"
    if [ "$DRY_RUN" = true ]; then
        log "DRY-RUN complete. Re-run without --dry-run to apply changes."
    else
        log "Done."
    fi
    if [ $EXIT_CODE -ne 0 ]; then
        err "One or more repos failed. See above for details."
    fi
    exit $EXIT_CODE
}

main "$@"
