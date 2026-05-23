---
last_verified: 2026-05-23
scope: Claude Code entrypoint for mesa-26-gororoba; imports canonical AGENTS.md
---

@AGENTS.md

# Claude loader for mesa-26-gororoba

This file is a thin Claude loader.  `AGENTS.md` is canonical; do not duplicate it
and do not replace this with a symlink.  Mesa policy governs Mesa edits even when
Claude is launched from steinmarder or a parent workspace.

Extended doctrine -- the LLM interaction guide, reasoning depth (Cayley-Dickson
ladder), source-comment voice, tool discipline, synthesis rule, and
regression-on-fix discipline -- lives in `AGENTS.md`.  Do not duplicate those
sections here; update the canonical file and keep this wrapper as the
Claude-specific loader.

Claude-specific rules:

- Use `/memory` to verify that this file and `AGENTS.md` are loaded before a
  sensitive change.
- Do not commit `CLAUDE.local.md`; local preferences belong in ignored local
  files or user memory, not repo policy.
- Do not add steinmarder-only evidence into Mesa.  Link or cite sibling evidence
  by durable path/name only when relevant.
- Use plan mode for risky driver edits, but do not substitute planning for
  builds, CTS/Piglit/deqp, or staged lint.
- Keep any `.claude/rules/` additions narrow, path-scoped, and mechanism-named.
