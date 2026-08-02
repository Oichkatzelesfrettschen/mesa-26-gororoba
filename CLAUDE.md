@AGENTS.md

# Claude Code Loader for mesa-26-gororoba

## Loading rule

`AGENTS.md` owns the Mesa rules; the `@AGENTS.md` import above loads them. The import path is spelled in that exact case: imports resolve literally, and this filesystem is case-sensitive. This file carries Claude Code operating notes only; shared doctrine lands in `AGENTS.md`.

When Claude Code starts inside `steinmarder/`, a parent workspace, or a temporary worktree, load `mesa-26-gororoba/AGENTS.md` before editing Mesa paths. Mesa rules govern every edit under Mesa paths regardless of launch directory.

## Claude Code operating notes

Inspect the real repository with Claude Code tools before editing; memory, prior summaries, and recalled context are leads, and `AGENTS.md` plus source are authority.

Inspect the diff after every edit; the adversarial staged-diff read from `AGENTS.md` runs before any commit or completion claim.

Claude Code task tracking is transient working state; durable state lands in code, tests, commit messages, findings, documentation, or retained bundles.

Subagent limits, the read-only default, model economy, and citation duties live in `AGENTS.md` under `Agent coordination` and `Tooling for RCA and audits`.

The em-dash substitute convention (`word--word`, closed up, no surrounding spaces) lives in `AGENTS.md` under `Comments, commits, and Markdown`.

## Response shape

Responses report results, decisions, evidence, and remaining uncertainty in mechanism-first form: changed mechanism, evidence used, validation run, tests not run and why, risks or unresolved falsifiers. Chained reasoning appears when it explains the next action or a validation requirement; the rest of the deliberation lives in thoughtspace. Responses are plain ASCII mechanism prose under durable names.
