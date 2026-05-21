---
last_verified: 2026-05-21
scope: Gemini CLI entrypoint for mesa-26-gororoba; imports canonical AGENTS.md
---

@AGENTS.md

# Gemini CLI notes for mesa-26-gororoba

This file intentionally imports `AGENTS.md` rather than duplicating it or symlinking it.

Extended doctrine, including the LLM interaction guide, reasoning depth,
source-comment style, tool discipline, synthesis rule, and regression-on-fix
discipline lives in `AGENTS.md`.  Do not duplicate those sections here; update
the canonical file and keep this wrapper as the Gemini-specific loader.

Gemini-specific rules:

- Use `/memory show` to inspect loaded context before high-risk work.
- Use `/memory refresh` after editing this file or `AGENTS.md` in-session.
- Do not use `--yolo` or broad auto-approval for Mesa install, hardware probing, destructive commands, or privileged commands without explicit user sign-off.
- If `.gemini/settings.json` is introduced, keep context loading unambiguous: either this wrapper is the context file, or `contextFileName` is set to `AGENTS.md`, not both with divergent doctrine.
