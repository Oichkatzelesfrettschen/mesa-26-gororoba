# Strict clean definition for repo deletion candidates

Canonical checklist for "clean", "clean and merge", "prune", or "assess for
deletion" requests.  `AGENTS.md` links here; apply every applicable condition
before any deletion, and never treat a repo as disposable until they hold.

A repository is deletion-ready only when all applicable conditions are true:

1. The working tree has no tracked modifications and no untracked, unignored
   files.
2. The current branch is the canonical primary branch, normally `main` or
   `master`, unless the repo documents a different primary branch.
3. The primary branch is synced with its configured remote primary: no commits
   ahead and none behind.
4. Local non-primary branches have been reviewed, reconciled, PR'd or merged
   where appropriate, and deleted only after their content is represented on the
   primary branch or explicitly deemed discardable.
5. Remote non-upstream branches the user owns have been reviewed, PR'd or merged
   where appropriate, and deleted only after their content is represented on the
   primary branch or explicitly deemed discardable.
6. Open PRs the user owns have been reviewed, comments addressed, and merged or
   explicitly closed as obsolete.
7. Linked worktrees, hidden worktrees, nested `.git` directories, and `.git`
   file worktrees have been inventoried; any unique work in them has been
   reconciled before deletion.
8. Build or validation gates meaningful for that repo have passed, with warnings
   treated as errors where the project supports that discipline.
9. For repos without a meaningful build gate, the absence of a gate is recorded
   rather than silently treated as success.
10. The repo is deleted only by reversible trash movement unless permanent
    deletion is explicitly requested.
