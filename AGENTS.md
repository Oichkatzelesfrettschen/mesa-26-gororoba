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

## LLM interaction guide

This section is for language model agents working in Mesa code.  Read it before
editing Terakan, r300, r600, NIR, Meson, tests, or comments.  The sibling
steinmarder repo carries broader evidence and runner doctrine, but this file
must stand alone for Mesa build, install, and review flows.

### Discipline, rigor, and depth

Work inside the real system.  Read code, docs, logs, tests, histories, and
prior evidence before forming a conclusion.  Build a precise model of the
interacting systems, name unknowns explicitly, and turn every implementation
claim into something checkable.

Use an ordered task tree.  Research before editing; model before designing;
verify before claiming completion.  Each tool invocation must answer a concrete
question or move implementation forward.

Research from primary sources where possible: Mesa source, Linux kernel source,
Khronos/Vulkan/OpenGL specs, AMD ISA/register manuals, CTS/Piglit/deqp tests,
and retained steinmarder evidence when available.  Treat memory and generated
summaries as leads, not authority.

Implement complete, robust solutions.  No stubs, placeholders, dead code, or
"TODO later" prose without explicit tracked rationale.  When blocked, trace the
root cause through all interacting layers instead of choosing a shortcut.

### Reasoning depth

Mesa/Terakan bugs frequently span specification, lowering, command emission,
kernel validation, cache/coherency behavior, and exact PALM/Wrestler silicon.
Do not jump from a single code observation to a silicon conclusion.

| Level | Axis added | Example |
|---:|---|---|
| 0 | direct observation | A lowering pass emits a specific intrinsic. |
| 1 | spec intent | The Vulkan or GLSL rule requires a result class. |
| 2 | driver/kernel mechanism | The descriptor, packet, or CS validator path carries the state. |
| 3 | silicon evidence | PALM/Wrestler probes or CTS show the hardware behavior. |
| 4 | systemic cost | Upstreamability, maintainability, performance, and waiver impact. |

Level-3 conclusions require claim, evidence chain, and falsification criterion.
Level-4 analysis is secondary until levels 0-3 are locked.  Read hardware and
spec constraints before judging code shape; code that looks odd may be
satisfying a silicon or kernel constraint.

### Evidence and falsification

Before editing code for a hardware RCA, state:

- direct observation,
- source/spec constraint,
- implementation hypothesis,
- falsification criterion,
- validation command or bundle path.

Do not claim a CTS issue is fixed from a build-only result.  Build success,
runtime success, CTS success, and silicon evidence are different evidence
classes.  Unexpected CTS or tool output is the finding; do not silently rerun
until it fits the prediction.

### Tool discipline for RCA and audit work

Use the strongest tool that matches the claim.

Tier S, not optional when available and relevant:

- `clangd` or LSP for definitions, references, call hierarchy, and symbol
  reachability.
- GNU Global, `cscope`, or equivalent indexes for large C trees.
- `ast-grep` or Semgrep for structural patterns and class-of-bug audits.
- `rg`, `git grep`, and `fd` for text, comments, strings, generated path
  discovery, and fallback search.
- `git log -S`, `git log -G`, and `git blame` for evolution.

Tier A, flow-sensitive:

- Compiler diagnostics, warnings-as-errors, and static analysis where viable.
- Coccinelle for cross-file consistency checks.
- Mesa, kernel, and CTS/Piglit/deqp runner logs.
- Shader disassembly, packet decode, and NIR dumps.

Tier C, empirical:

- CTS/Piglit/deqp, dmesg, hardware counters, retained bundles, and minimal
  reproducers when hardware policy permits.

When an audit reports a code claim, cite how the symbol/path was found, not
only file:line.  For example: `(clangd: references on FUNC)`, `(global -r
SYMBOL)`, or `(rg --fixed-strings SYMBOL src/)`.

Every new probe, lint, or verdict-producing script must be calibrated against
known-good and known-bad inputs before its verdict is trusted.

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
- introduce raw IPv4 or IPv6 literals in scripts/configs; use hostnames.
- encode local absolute paths or private host FQDNs.
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

Mesa code changes must cite public specs, kernel/Mesa source paths, freedesktop.org issue numbers, and exact evidence names in comments when those comments are needed.  Freedesktop.org bug and issue references (e.g. `https://gitlab.freedesktop.org/...` URLs and GitLab issue numbers on freedesktop.org) are required and allowed.  Do not cite internal GitHub/GitLab issue numbers from this fork or from steinmarder.  Do not cite private PR chronology or wave labels in code.

## Source comment voice

Comments should be short, active, sequenced, and primary-source-grounded.  Explain why the code is shaped that way, not what the next line already says.

Mesa-like comments are not necessarily one-liners.  Use short labels for
obvious local sections, and compact multi-sentence blocks when the code depends
on hardware behavior, API rules, kernel validation, empirical evidence, or
non-local invariants.  The common rule is mechanism over decoration.

Do not frame source comments with decorative delimiter lines, banner boxes,
ASCII art, or long punctuation runs.  Start with the first useful sentence and
end after the last useful sentence; do not wrap it in `/* ----- */`,
`// =====`, `/* --- label --- */`, or similar borders.

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

Also bad:

```text
/* ---------------------------------------------------------------------------
 * Single-slot ramp texture cache for R300-class gradient draws.
 * -------------------------------------------------------------------------- */
```

When extending Triang3l-authored Terakan files, match the file's cadence: shorter line lengths, fewer subclauses, and comments only where they carry silicon/spec/test information.

### TODO bodies stay mechanism-only

Legitimate TODO/FIXME/XXX/HACK/PLACEHOLDER tags must name three things in mechanism terms:

- the missing work, in terms of the function, register, ISA section, kernel symbol, or spec chapter that needs the change;
- the reason for deferral, in terms of the silicon, ABI, or evidence constraint that blocks completing it now;
- the tracking artifact, in terms of a durable name (function, register, `gitlab.freedesktop.org` issue URL, spec chapter, silicon-constraint name).

The same comment must not embed any of the following:

- reviewer breadcrumbs (`reviewer P1 badge`, `Sourcery flagged`),
- PR-thread references (`PR thread that introduced this`, `the install-prefix work that surfaced this`),
- internal phase, wave, or mission labels (`Wave 5C`, `Phase 1E-atomic`, `mission-r300-breakthrough`),
- AGENTS.md rule-number citations (`Per AGENTS.md rule 9a, ...`),
- time-relative or deictic references (`currently`, `previously`, `this driver`, `our GPU`), per the `## Source comment voice` rule already established in this file.

The chronology prohibition above and the deictic prohibition just listed bind TODO bodies as strictly as ordinary comments.  Reviewer feedback, PR chronology, and phase labels belong in the commit message and the PR description; the source comment carries mechanism only.

Wrong shape (project chronology smuggled into the source):

```text
/* TODO: ...  Reason for deferral: outside this PR's scope.
 *       Tracking: reviewer P1 badge on the consolidated style PR.
 */
```

Right shape (all three mechanism elements named: missing work, reason, tracking artifact):

```text
/* TODO: missing work --
 *           PALM does not honour ALU_PUSH_BEFORE when stack depth
 *           is a non-zero multiple of 4; SFN must emit an explicit
 *           PUSH CF before the ALU clause in that case.
 *       reason --
 *           the explicit PUSH consumes 1 subentry, which forces
 *           STACK_SIZE in SQ_PGM_RESOURCES[15:8] to grow, so the
 *           emission cannot land until terakan_cf_stack_tracker
 *           accounts the extra subentry on the mod-4 boundary.
 *       tracking --
 *           Evergreen ISA section 4 "Control Flow / ALU-PUSH hazard"
 *           and r600_nir_lower_cube_to_2darray.
 */
```

A complex case still stays mechanism-only:

```text
/* TODO: missing work --
 *           r600_nir_lower_int_tg4 must run before
 *           r600_nir_lower_cube_to_2darray for integer GATHER4 on
 *           cube textures.
 *       reason --
 *           inserting the pass earlier regresses textureGather on
 *           int_2d_array; the swap is gated on first fixing the
 *           2d_array residual under TERAKAN_EXPERIMENTAL_CUBE_GATHER_DIM_2D_ARRAY=1.
 *       tracking --
 *           sfn_nir.cpp pass-order block in terakan_shader.c and the
 *           dEQP-VK.glsl.texture_gather.basic_cube.int.* test family.
 */
```

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

When merging changes from parallel branches or review findings, preserve all
non-refuted content.  Do not use wave/phase chronology to decide which branch
wins.  Mechanism and evidence decide.

Default additive merge policy: union, then harmonize terminology, then preserve
the strongest resulting rule or change.  The result must carry the evidence,
examples, and enforcement hooks forward in repo-native language.

| Verb | Required action |
|---|---|
| Analyze | Identify what each side contributes before editing. |
| Reconcile | Preserve all non-refuted content; selection needs proof. |
| Resolve | Finish the merge; leave no half-merged state or hidden follow-up. |
| Expand | Surface links between findings, code paths, and tests. |
| Harmonize | Use one durable mechanism name for the same thing. |
| Infuse | Add the check, citation, or rule that prevents the same failure class. |

Before merge commit:

```bash
git diff --staged
```

Ask what content this resolution dropped from each source branch that was not empirically refuted.  Restore, refute, or explicitly track anything missing.

Forbidden shortcuts for additive content:

```bash
git merge -X theirs branch
git checkout --theirs file
sed -i '/^<<</d; /^===/d; /^>>>/d' file
```

Those commands select; they do not synthesize.  Use `git cherry-pick
--no-commit`, manual conflict editing, and adversarial diff review instead.

## Regression-on-fix discipline

A targeted fix for issue A must not regress unrelated behavior B.  After any
change to a script, runner, build file, lowering pass, or descriptor path:

- re-read `git diff --staged` with adversarial intent;
- verify every removed line was intentional, duplicated elsewhere, or refuted;
- compare test labels with the commands they run;
- verify every symbol named in comments/docs against source;
- enumerate every override mechanism before documenting one;
- check optional tool availability before configuring for it;
- calibrate every new verdict-producing probe, lint, or runner on known-good
  and known-bad inputs.

If a reviewer finds a defect, fix the class, not just the instance.  Add the
rule, lint, test, or documented check that would have caught it.

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
