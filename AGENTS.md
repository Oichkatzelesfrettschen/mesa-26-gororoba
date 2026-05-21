---
description: Mesa 26.x fork with Terakan Vulkan driver for Radeon HD 6310 PALM/Wrestler
last_verified: 2026-05-21
scope: canonical multi-CLI agent and developer guidance for mesa-26-gororoba
---

# mesa-26-gororoba -- Agent and Developer Reference

## Load-bearing rule

This repository builds and installs standalone.  It may consume evidence from the sibling steinmarder repo, but it must not require steinmarder to be cloned for normal Mesa build, install, or test flows.

Codex reads this `AGENTS.md` directly.  Claude Code and Gemini CLI load wrapper files that import this file.  Do not replace those wrappers with symlinks.  Do not duplicate this full body into multiple CLI files unless a tool loses Markdown import support.

## Overview

Fork of Mesa 26.x tracking upstream `main` closely.  Carries the Terakan Vulkan driver at `src/amd/terascale/vulkan/` plus r600 SFN and related work targeting Radeon HD 6310 PALM/Wrestler, `CHIP_PALM`, Evergreen / TeraScale-2 / VLIW5.

Target host class: x130e / Bobcat / HD 6310 APU.  Peer repo: `steinmarder/`, which holds RE tooling, evidence bundles, findings, host kits, and cross-cutting workspace docs.

## Canonical Mesa workspace boundary

Under the Mesa workspace root, only two source roots are durable:

| Directory | Purpose |
|---|---|
| `mesa-26-gororoba/` | Mesa driver code, Terakan, r300g, Mesa build/install infrastructure |
| `steinmarder/` | RE runners, evidence bundles, manifests, findings, host kits, policy docs |

Other `mesa-26-gororoba-*` entries are temporary Git worktrees of this repo, not new projects.  Work from them must land through a branch, PR, and merge to `main`; after merge or explicit supersession, remove the temporary worktree.

Do not put steinmarder evidence bundles or findings in Mesa.  Do not put Mesa driver changes in steinmarder.  Use the sibling repo and a PR when a task crosses the boundary.

## Priority order

Conformance first, standards second, stability third, performance fourth, safety throughout.  A fast-but-non-conforming patch must be labelled as such.  Never hide the conformance cost of a workaround.

## Naming discipline

Use durable mechanism names, not waves, phases, agents, worktrees, or chronology labels.

| Surface | Wrong | Right |
|---|---|---|
| Branch | `terakan/wave8a-fix` | `terakan/palm_mem_rat_atomic_completion_semantics` |
| Commit subject | `Wave 8A follow-ups` | `terakan: validate PALM MEM_RAT atomic completion semantics` |
| PR title | `Phase 1E discriminator` | `palm_mem_rat_atomic_completion_semantics discriminator` |
| Comment | `Wave 8A path` | `cached MEM_RAT atomic path` |
| Test artifact | `phase1e-results` | `palm_dual_mem_rat_atomic_silicon_cut` |

The first commit subject matters.  A single-commit squash merge may use it even if the PR title was later corrected.  Set branch, first commit subject, and PR title canonically before first push.

## Hard prohibitions

Do not:

- force-push `main`.
- run destructive commands on shared workspace paths.
- introduce raw IPv4 addresses in scripts/configs.
- encode local absolute paths or private host FQNs.
- symlink instruction files.
- chain `ccache distcc compiler` through a shell wrapper.
- use `RUSTC_WRAPPER` for Meson Rust and assume it affects Meson; use Meson `[binaries]` native files.
- remove build targets, tests, or validation checks as collateral for a narrow fix.
- add stubs, placeholders, or TODO prose without explicit tracked rationale.
- change generated files without running the generator or explaining why it is unavailable.

## Standalone build

Mesa must build from this repository alone.  Use native files and environment variables that can be reproduced by a clean checkout.

Canonical shape:

```bash
meson setup builddir \
  --prefix="$PWD/install" \
  -Dbuildtype=debugoptimized \
  -Dgallium-drivers=r300,r600,softpipe \
  -Dvulkan-drivers=amd_terascale \
  -Dllvm=enabled
ninja -C builddir
ninja -C builddir install
```

Adapt options to the actual checkout and Meson state.  Do not hardcode local absolute paths.  Discover repository root when scripting:

```bash
repo_root=$(git rev-parse --show-toplevel)
```

## Compiler cache discipline

Use ccache/distcc through the supported compiler chain, not wrapper-style shell scripts.

Wrong:

```bash
exec ccache distcc clang "$@"
```

Right:

```bash
export CCACHE_PREFIX=distcc
# native file [binaries] points CC/CXX to ccache or compiler as documented by the local build recipe.
```

If changing cache wiring, consult the steinmarder workspace cache RCA and update the reproducible recipe.  Do not patch around a cache miss by bypassing the cache silently.

## Upstream discipline

- `origin` is the fork.
- `upstream` is freedesktop Mesa.
- Never push `upstream/main` directly to fork `main`.
- Rebase intentionally and record what diverged.
- Separate upstreamable fixes from local evidence/bring-up scaffolding.
- Keep Terakan changes reviewable by mechanism, not by batch size.

## RE and evidence boundary

All driver RCA evidence lives in steinmarder, not here.  Before proposing a Terakan fix, consult the sibling evidence when available:

- `steinmarder/src/re/r600/findings/CLAIMS.md`
- `steinmarder/src/re/r600/findings/active/`
- `steinmarder/src/re/r600/results/`
- `steinmarder/src/re/r600/docs/rca/`
- `steinmarder/GPU_ARCHITECTURE_BASELINES.md`
- `steinmarder/AGENTS_README.md`
- `steinmarder/AGENT_RULES.md`

Mesa code changes must cite public specs, kernel/Mesa source paths, and exact evidence names in comments when those comments are needed.  Do not cite private PR chronology or wave labels in code.

## Source comment voice

Comments should be short, active, sequenced, and primary-source-grounded.  Explain why the code is shaped that way, not what the next line already says.

Good:

```text
PALM routes this RAT path through the cached memory clause.  The silicon
probe shows compare-exchange completion is not observable there, so the
lowering keeps the operation on the validated uncached path.
```

Bad:

```text
Wave 8A workaround for atomics.
```

When extending Triang3l-authored Terakan files, match the file's cadence: shorter line lengths, fewer subclauses, and comments only where they carry silicon/spec/test information.

## Evidence and falsification

Before editing code for a hardware RCA, state:

- direct observation,
- source/spec constraint,
- implementation hypothesis,
- falsification criterion,
- validation command or bundle path.

Do not claim a CTS issue is fixed from a build-only result.  Build success, runtime success, CTS success, and silicon evidence are different evidence classes.

## Security stop-line

Critical vulnerabilities interrupt normal work:

- hardcoded secrets,
- SQL injection,
- command injection,
- path traversal,
- sensitive logs,
- missing authentication,
- missing authorization,
- insecure deserialization,
- SSRF.

Never pass untrusted input into shell commands or filesystem paths without allow-listing, normalization, and containment checks.  Do not log secrets or raw tokens.  If a critical issue is found, contain and report it before normal feature work resumes.

## Validation expectations

| Changed surface | Minimum validation |
|---|---|
| Terakan source | targeted build + relevant runtime/CTS when available |
| r300/r600 source | targeted build + relevant Piglit/CTS/deqp or documented hardware gate |
| NIR/lowering/compiler | build + shader/compiler tests + affected conformance tests |
| Meson/build files | clean setup or reconfigure + ninja target |
| generated headers/schemas | run generator or explain unavailability |
| scripts | shellcheck if applicable + known-good/known-bad path |
| docs/comments | symbol verification + comment hygiene rules |

Always record what was run.  If hardware is unavailable or safety policy blocks a run, say that explicitly.

## Synthesis rule

When merging changes from parallel branches or review findings, preserve all non-refuted content.  Do not use wave/phase chronology to decide which branch wins.  Mechanism and evidence decide.

Default additive merge policy: union, then harmonize terminology.

Before merge commit:

```bash
git diff --staged
```

Ask what content this resolution dropped from each source branch that was not empirically refuted.  Restore, refute, or explicitly track anything missing.

## Multi-CLI wrappers

- `CLAUDE.md` imports this file with `@AGENTS.md` and carries Claude-specific loading notes.
- `GEMINI.md` imports this file with `@AGENTS.md` and carries Gemini-specific loading notes.
- Do not symlink either file.
- Do not maintain divergent doctrine across wrappers.

## Key subsystems

| Subsystem | Typical paths |
|---|---|
| Terakan Vulkan | `src/amd/terascale/vulkan/` |
| r600 Gallium/SFN | `src/gallium/drivers/r600/` |
| r300 Gallium | `src/gallium/drivers/r300/` |
| winsys/drm | `src/gallium/winsys/`, `src/drm-shim/` where applicable |
| NIR/compiler plumbing | `src/compiler/`, driver lowering paths |
| build/install | `meson.build`, `meson_options.txt`, native files, install scripts |

Keep subsystem-specific rules close to the subsystem when they are too narrow for this root file.
