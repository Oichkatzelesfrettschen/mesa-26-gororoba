---
last_verified: 2026-05-21
scope: Claude Code entrypoint for mesa-26-gororoba; imports canonical AGENTS.md
---

@AGENTS.md

# Claude Code notes for mesa-26-gororoba

This file intentionally imports `AGENTS.md` rather than duplicating it or symlinking it.

Extended doctrine, including the LLM interaction guide, reasoning depth,
source-comment style, tool discipline, synthesis rule, and regression-on-fix
discipline lives in `AGENTS.md`.  Do not duplicate those sections here; update
the canonical file and keep this wrapper as the Claude-specific loader.

Claude-specific rules:

- Use `/memory` to verify that this file and `AGENTS.md` are loaded.
- Do not commit `CLAUDE.local.md`.
- Do not add steinmarder-only evidence into Mesa.  Link or cite sibling evidence by durable path/name only when relevant.
- Use plan mode for risky driver edits, but do not substitute planning for builds, CTS/Piglit/deqp, or staged lint.
- Keep any `.claude/rules/` additions path-scoped and mechanism-named.
