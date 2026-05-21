@agents.md

# Claude Code Loader for mesa-26-gororoba

## Loading rule

`AGENTS.md` contains the Mesa rules. This file exists only to load `AGENTS.md` and add Claude Code operating notes.

Do not copy the `AGENTS.md` body here. Do not add Mesa policy that applies to every agent here. Shared rules belong in `AGENTS.md`.

When Claude Code starts inside `steinmarder/`, a parent workspace, or a temporary worktree, load `mesa-26-gororoba/AGENTS.md` before editing Mesa paths. Mesa rules govern every edit under Mesa paths regardless of launch directory.

## Claude Code execution notes

Use Claude Code tools to inspect the real repository before editing. Treat memory, prior summaries, and generated explanations as leads, not authority.

Prefer narrow edits. Avoid broad replace operations, mass formatting, or convenience rewrites unless the requested task is explicitly a formatting migration.

After every edit, inspect the diff. Before committing or reporting completion, read the staged or unstaged diff adversarially and verify that no non-refuted content was dropped.

Use the repository root explicitly in shell commands when path ambiguity matters:

```bash
repo_root=$(git rev-parse --show-toplevel)
```

## Subagents and model economy

Use at most three concurrent subagents.

Claude Code subagents are read-only evidence collectors unless the user explicitly authorizes a different role. Assign each subagent a bounded question, input path set, expected output, and citation requirement.

Use the smallest adequate model for file location, search fan-out, summarization, and citation gathering. Escalate only for deep synthesis, hazardous decisions, hard-to-reverse changes, or cross-file mechanism reasoning. The parent Claude session owns synthesis, implementation choices, conflict resolution, and final claims.

Subagents must report how symbols and paths were found: LSP reference query, GNU Global, `rg`, `git grep`, `git log -S`, `ast-grep`, or another named method. File paths alone are not enough for load-bearing code claims.

## Task state

Use Claude Code task tracking as transient working state only. Durable state belongs in code, tests, commit messages, findings, documentation, or retained bundles.

Do not add `TODO` items to rule files to remember session work. Source TODO-family comments must follow the mechanism-only TODO rules in `AGENTS.md`.

## Tool use

Prefer source-aware tools before plain text search when the question is structural:

- LSP or `clangd` for definitions, references, call hierarchy, and reachability.
- GNU Global, `ctags`, or `cscope` for large-tree navigation.
- `ast-grep`, Semgrep, Coccinelle/`spatch`, or `weggli` for structural patterns.
- `rg`, `git grep`, and `fd` for strings, comments, generated paths, and fallback search.
- `git log -S`, `git log -G`, and `git blame` for evolution.

When a required tool is missing, identify the package or path, update the relevant installation requirements document when appropriate, and record `not run` if the tool cannot be used.

## Build and validation reporting

Do not claim a build or test passed unless it was run in this session or the result is cited from retained evidence. When a command was not run, say `not run` and why.

For GPU-behavior work, check `dmesg` for DRM CS validation errors before analysis and verify module reachability before crash symbolization.

Release evidence must not inherit stale loader variables. Set or unset `LIBGL_DRIVERS_PATH`, `LD_LIBRARY_PATH`, and `VK_ICD_FILENAMES` explicitly for the selected build prefix.

## Claude-specific authorship boundaries

Do not add Claude names, model names, or AI labels to source file headers or source comments.

When Mesa policy requires disclosure, use commit trailers from `AGENTS.md`: `Assisted-by:` for mixed human/AI work or `Generated-by:` when AI generated almost the entire change. Do not use `Co-authored-by:` for Claude or any other AI tool in new commits.

## Response shape

Report results, decisions, evidence, and remaining uncertainty. Do not narrate hidden deliberation. Chain reasoning only when it explains the next action or validation requirement.

Prefer concise, mechanism-first summaries:

- changed mechanism;
- evidence used;
- validation run;
- tests not run and why;
- risks or unresolved falsifiers.

Do not use emoji, ASCII banners, decorative separators, or phase/session labels in rule text, source comments, or commit-ready prose.
