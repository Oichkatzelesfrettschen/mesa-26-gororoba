---
last_verified: 2026-05-23
scope: Claude Code entrypoint for mesa-26-gororoba; imports canonical AGENTS.md
---

@AGENTS.md

# Claude loader for mesa-26-gororoba

This file is a thin Claude Code loader.  `AGENTS.md` is canonical.  Do not
duplicate the canonical doctrine here and do not replace this file with a
symlink.  Mesa policy governs Mesa edits even when Claude is launched from
steinmarder or a parent workspace.

Extended doctrine -- source-comment voice, reasoning depth, tool discipline,
synthesis rules, regression-on-fix discipline, Mesa submission policy, and
hardware-evidence boundaries -- lives in `AGENTS.md`.  Update the canonical file
rather than copying those sections into this wrapper.

Claude-specific rules:

- Use `/memory` to verify that this file, `AGENTS.md`, and any applicable
  `.claude/rules/*.md` files are loaded before starting sensitive driver,
  build-system, install-prefix, CTS/Piglit/deqp, or hardware-probe work.
- Claude memory is context, not enforcement.  Hooks, tests, lints, build gates,
  CTS/Piglit/deqp runs, and staged diff review are the enforcement layer.
- Do not commit `CLAUDE.local.md`; local preferences belong in ignored local
  files or user memory, not repo policy.
- Do not add steinmarder-only evidence, retained bundles, host kits, or local
  orchestration into Mesa.  Cite sibling evidence by durable path/name only when
  it is relevant to a Mesa change.
- Use plan mode for risky driver edits, generated-file changes, install changes,
  and hardware-probe-adjacent work, but never substitute a plan for builds,
  conformance tests, staged lints, or adversarial diff review.
- Keep any `.claude/rules/` additions narrow, path-scoped, and mechanism-named.
  Link them from `AGENTS.md` only when they become cross-lane doctrine.
- For self-review, use review agents to gather findings; do the interpretation
  here.  Verify or falsify each finding against the actual code before acting.
  A gathered finding can mis-state the mechanism, and acting on a wrong one is
  its own regression.
