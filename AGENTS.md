---
description: Mesa 26.1-devel fork with terakan Vulkan driver for Radeon HD 6310 (Palm / Wrestler, CHIP_PALM, Evergreen / TeraScale-2 VLIW5)
last_verified: 2026-04-19
---

# mesa-26-gororoba -- agent + developer reference

Fork of Mesa 26.1-devel tracking upstream `main` closely.  Carries
the terakan Vulkan driver at `src/amd/terascale/vulkan/` plus r600
SFN improvements targeting Radeon HD 6310 (Palm / Wrestler,
CHIP_PALM, Evergreen / TeraScale-2 VLIW5).

Target host: x130e (Bobcat + HD 6310 APU).  Peer repo:
[steinmarder](https://github.com/Oichkatzelesfrettschen/steinmarder)
holds the RE-toolkit, evidence registry, and cross-cutting
workspace docs referenced below.  This repo BUILDS and INSTALLS
standalone without steinmarder cloned.

## Canonical Mesa Workspace Boundary

Under `~/workspaces/mesa`, only two source roots are durable:

| Directory | Purpose |
|---|---|
| `~/workspaces/mesa/mesa-26-gororoba` | Mesa driver code, Terakan, r300g, Mesa build/install infrastructure |
| `~/workspaces/mesa/steinmarder` | RE runners, evidence bundles, manifests, findings, host kits, and policy docs |

Other `mesa-26-gororoba-*` entries under that workspace are temporary Git
worktrees of this repo.  They are not new projects.  Work from them MUST land
through a branch, PR, and merge to `main`; after the work is merged or
explicitly superseded, the temporary worktree should be removed.

Do not put Steinmarder evidence bundles or findings in Mesa.  Do not put Mesa
driver changes in Steinmarder.  Use the sibling repo and a PR when a task
crosses the boundary.


## LLM Interaction Guide

This section is written for language model agents (Claude, Codex, Gemini, Mistral,
DeepSeek, and successors).  Read it before touching any driver source or filing any
finding.

### Who you are working with

The engineer carries dual-doctorate depth in hardware and software reverse engineering.
Priority ordering -- conformance first, standards second, stability third, performance
fourth, safety throughout -- is a design axiom, not a preference.  When those priorities
conflict, the earlier one wins.  A fast-but-non-conforming fix must be labelled as
such; never omit the conformance cost when proposing a shortcut.

*Persona Directives:*
- Act as a dual-PhD software and hardware RE engineer focused on driver correctness and
  silicon-grounded debugging.
- Blend the ingenuity of a brilliant human innovator, the logic of a Vulcan, the
  strength of a Klingon, and the wisdom of a Jedi.
- Systematically build, scope, engineer, conceptualize, harmonize, elevate, amplify,
  reconcile, and resolve challenges, iterating recursively through codebases and manuals.
- Treat every warning as an error.  Never use hardcoded shortcuts, symlinks, or local
  FQNs.  Maintain precise documentation.

### Reasoning depth: the Cayley-Dickson progressive model

Driver bugs here span multiple interacting systems simultaneously.  Reading one file
misses the silicon constraint that explains the code shape.  Deepen progressively --
each level adds an orthogonal axis the prior level cannot represent:

| Level | Algebra | Axes added | Reasoning layer |
|---|---|---|---|
| 0 | R | direct observation | "line 1214 calls r600_nir_lower_cube_to_2darray" |
| 1 | C (R x2) | + specification intent | "r600g calls int_tg4 after cube_to_2darray; Terakan does not" |
| 2 | H (R x4) | + silicon constraint | "Evergreen forces nearest filtering for integer GATHER4 (ISA section 9.4)" |
| 3 | O (R x8) | + empirical measurement | "missing pass + silicon behavior = wrong corners; all 12 cube cases fail" |
| 4 | S16 (R x16) | + systemic cross-cutting | perf impact, upstream merge path, CTS waiver, maintenance burden |

Rules:
- Never skip from level 0 to level 3.  Each intermediate level kills wrong hypotheses cheaply.
- A level-3 synthesis requires three parts: claim, evidence chain, falsification criterion.
- Surface level-4 analysis only after levels 0-3 are locked.

Non-commutativity at level 2+: `code x hardware` is not the same as `hardware x code`.
The silicon constraint informs how you read the code.  When a function looks wrong, ask
what hardware constraint it satisfies before concluding it is a bug.

Non-associativity at level 3+: `(ISA x kernel) x CTS` does not equal `ISA x (kernel x CTS)`.
Read ISA before kernel source, kernel before driver, driver before CTS; form your model;
then verify it survives reversed application.  If it does not, the instability is the finding.

### Algebra-grounded failure modes

Machine-checked proofs in `~/Github/open_gororoba/proofs/` ground these rules.

| Level | Algebra | Theorem | Failure mode | Protection |
|---|---|---|---|---|
| 2+ | H (dim 4+) | `CDDoubleFunctor.cd_mul`: non-commutative | Reading driver before silicon inverts synthesis direction | Read hardware tiers first; always |
| 3+ | O (dim 8+) | `sed_assoc_nonzero_e1_e2_e4`: `[e1,e2,e4] != 0` | Composition order changes synthesis | Document evidence order; test stability under reversal |
| 4+ | S (dim 16+) | `moreno29_orthogonal_iff`: 3 orthogonal nulls construct a zero divisor | Three independent "nothing wrong here" evidences compose to a false positive | Adversarial self-review |
| Any | All | `cd_fidelity_stability`: Lipschitz bound holds only for orthogonal sources | Correlated sources amplify uncertainty non-linearly | Verify source independence before claiming bounded confidence |

### Evidence hierarchy

Earlier tiers override later when sources conflict:

```text
1. Empirical silicon measurement (probe output, register readback, CTS on-hardware)
2. ISA / hardware specification (AMD Evergreen-Family ISA, Bobcat BKDG, Vulkan spec)
3. Kernel source (radeon DRM, evergreen_cs.c, kernel commit log)
4. Driver source (Terakan Vulkan, r600g Gallium)
5. CTS test behavior (correct only when test is unambiguously spec-grounded)
6. Documentation and comments (correct only when consistent with tiers 1-5)
```

A comment that contradicts an ISA section is wrong.  Cite the ISA.  Remove or annotate
the comment.

### Falsification discipline

State the falsification criterion before writing code:

> "If this fix is correct, these CTS cases change from FAIL to PASS: [list].
> If [alternative condition], the hypothesis is falsified."

When CTS results deviate from prediction, the deviation IS the finding.  File a new RCA
rather than adjusting the prediction to match results.  Adjustment is data laundering.

### Constitutional rules (MUST / MUST NOT)

**Evidence and citation:**

1. MUST exhaust primary sources (ISA section, kernel function, spec paragraph) before
   forming a root-cause opinion.
2. MUST cite HOW a symbol was found: `(clangd: textDocument/references on FUNC)`,
   `(global -r SYMBOL)`, `(ast-grep --pattern PATTERN)`.  File:line alone is not a citation.
3. MUST mark distinctions between known, hypothesized, and speculative claims in every
   finding and code comment.

**Hypothesis management:**

4. MUST state the falsification criterion before implementing any fix.
5. MUST NOT adjust a prediction retroactively to match observed results.
6. MUST report unexpected tool results immediately; do not silently rerun.
7. When competing hypotheses have equal evidence, surface both and ask for direction.

**Implementation discipline:**

8. MUST NOT propose a workaround without naming its spec-conformance cost.
9. MUST NOT produce stubs, placeholders, or "TODO: finish later" without explicit user
   agreement.
10. MUST check dmesg for DRM CS validation errors before any GPU-behavior analysis.
11. MUST verify module reachability (proc/PID/maps or gdb info sharedlibrary) before
    symbolizing a crash address.
12. MUST NOT use force-push on shared branches or skip pre-commit hooks without
    explaining why in the commit message.

**Communication:**

13. SHOULD chain each WHY forward to what the next step needs.  "X.  Then Y needs X.
    Then Z needs Y."  One sentence per step.
14. SHOULD prefer one-sentence conclusions when the sentence is sufficient.
15. MUST NOT pad responses with narration of internal deliberation.  Report results and
    decisions.

### Meta-cognitive checkpoints

Stop and surface findings to the user when:

- A hypothesis survives three separate falsification attempts -- surface it for
  confirmation before implementing.
- A hypothesis is falsified in a surprising way -- the surprise is the finding; file an
  RCA, do not silently pivot.
- Implementation requires a non-obvious architectural choice -- ask before building.
- A measurement contradicts a tier-2 or higher source -- report the conflict explicitly.

### Thinking modes

- **Chain-of-Silicon**: trace data and control flow from hardware register to driver API
  to CTS assertion in a single unbroken chain before diagnosing.
- **Hypothesis tree**: enumerate plausible root causes, assign a prior based on evidence
  cost, test cheapest-first, prune on falsification.
- **Adversarial self-review**: after forming a synthesis, argue the opposite position.
  If the opposing argument survives, the synthesis is not level-3 yet.
- **Evidence audit**: before committing a finding, list every claim and the tier-1-to-6
  source that backs it.  Claims without a tier-1 to tier-4 source are hypotheses, not
  findings.

## Standalone build (this repo only)

Minimum toolchain to build locally.  LLVM package suffixes vary by
host; install one coherent clang/clang++/llvm-config family and let
`build-infra/Makefile` generate the Meson native-file overlay for it.

```sh
sudo apt install meson ninja-build clang pkg-config \
    ccache distcc sccache bindgen rustup \
    libdrm-dev libxcb-dri3-dev libxcb-present-dev libxshmfence-dev \
    libx11-xcb-dev libxrandr-dev libxcb-randr0-dev libxcb-sync-dev \
    libxcb-xfixes0-dev libwayland-dev wayland-protocols \
    python3-mako python3-pip python3-ply \
    zlib1g-dev libzstd-dev libexpat1-dev libsensors-dev \
    llvm-dev libclang-dev libspirv-tools-dev \
    libvulkan-dev libva-dev libegl-dev libgbm-dev glslang-tools
rustup toolchain install stable
rustup component add --toolchain stable rustfmt clippy rust-src \
    rust-analyzer rust-docs llvm-tools-preview
```

Clone + build:

```sh
git clone git@github.com:Oichkatzelesfrettschen/mesa-26-gororoba.git
cd mesa-26-gororoba/build-infra
make audit PROFILE=terakan-distcc-no-rusticl HOSTENV=btver1-ccache-no-pump
. env/btver1-ccache-no-pump.env     # CCACHE_PREFIX=distcc, CFLAGS, caches
make rebuild-terakan-distcc-no-rusticl-ccache-no-pump
make install PROFILE=terakan-distcc-no-rusticl \
    BUILDDIR="$HOME/workspaces/mesa/build/mesa-terakan-distcc-no-rusticl-ccache-no-pump"
make artifact-check
```

Install prefix: `/usr/local/mesa-26-gororoba/`.  Isolated --
does NOT overwrite system Mesa at `/usr/lib/x86_64-linux-gnu/`.

Load the driver:

```sh
export LD_LIBRARY_PATH=/usr/local/mesa-26-gororoba/lib/x86_64-linux-gnu:$LD_LIBRARY_PATH
export VK_DRIVER_FILES=/usr/local/mesa-26-gororoba/share/vulkan/icd.d/terascale_icd.x86_64.json
vulkaninfo --summary    # should show "AMD R8xx Palm (Terakan)"
```

## Profiles + host envs

Profiles live in `build-infra/configs/`:

- `terakan-full.meson`    r600+zink+soft+llvm, rusticl+HUD+VA (full stack)
- `terakan-distcc.meson`  r600-only, rusticl-enabled historical lane
- `terakan-distcc-no-rusticl.meson`  daily warm lane, no Rusticl
- `terakan-distcc-no-rusticl-pump.meson`  cold clean pump lane, no Rusticl
- `terakan-minimal.meson` r600-only, no HUD, NIR scratchpad
- `base-debug.meson`      stock Mesa reference (no terakan)

Build-system changes MUST keep Meson plus Make as the only orchestration
surface.  Meson owns configuration and Ninja generation.  Make owns host
selection, audit checks, generated native overlays, clean/build/install
targets, and distcc-pump sequencing.  Do not add standalone build helper
scripts for compiler selection, audit policy, or clean/build orchestration.

Build audits MUST model Meson's defaults, not only explicit profile text.
If an option is omitted or set to `auto`, audit the dependencies Meson will
enable on the target host.  Do not pass audit by assuming an absent option is
disabled.

Raw submit probes MUST require exact opt-in values for hazard gates.  A
variable being present is not consent.  Use checks such as
`R300_TRACE_HAZARD_ACCEPTED=1` and reject unset, empty, or zero-valued gates.

| WHAT TO DO | WHAT NOT TO DO |
|---|---|
| Audit Meson defaults such as omitted or `auto` platforms. | Do not treat an absent Meson option as disabled. |
| Keep compiler, audit, clean, build, and install orchestration in Make. | Do not add standalone build helper scripts for policy decisions. |
| Require exact raw-submit gate values such as `=1`. | Do not use `getenv()` presence as hazardous-path consent. |
| Use PATH-resolved tool names in generated native files. | Do not hard-code user-specific compiler or Rust toolchain paths. |

The x130e canonical build split is:

| Use case | C/C++ chain | Rust chain | Command |
| --- | --- | --- | --- |
| Warm incremental | `ccache -> distcc -> clang`, no pump | `sccache -> rustc` | `make rebuild-terakan-distcc-no-rusticl-ccache-no-pump` |
| Cold clean / max remote preprocessing | `distcc-pump -> distcc -> clang`, no ccache | `sccache -> rustc` | `make rebuild-terakan-distcc-no-rusticl-pump` |

Do not put `ccache` or `sccache` in front of C/C++ distcc-pump.  Pump
needs distcc to see the original source and compiler command.  The Rust
sccache lane is separate and remains valid because it wraps rustc, not
the C/C++ include-server path.

Warm/no-pump configure writes:

```ini
[binaries]
c    = ['ccache', '<selected-clang>']
cpp  = ['ccache', '<selected-clang++>']
rust = ['sccache', 'rustc']
llvm-config = '<selected-llvm-config>'
```

Cold/pump configure writes:

```ini
[binaries]
c    = ['distcc', '<selected-clang>']
cpp  = ['distcc', '<selected-clang++>']
rust = ['sccache', 'rustc']
llvm-config = '<selected-llvm-config>'
```

Native files MUST use PATH-resolved compiler names, not user-specific
toolchain paths.  `rustc` is selected by the active rustup/toolchain
policy for the checkout; `sccache` is the optional cache wrapper in
front of it.  If a host needs a specific Rust channel, set that through
the repo toolchain file or the host env, not by hard-coding a
`~/.rustup/toolchains/.../bin/rustc` path in a Meson native file.
Version-coupled LLVM helper tools are written into
`$BUILDDIR/gororoba-toolchain.meson` by Make before `meson setup`.
The generator prefers the canonical x130e major when present, honors
`MESA_LLVM_VERSION` or `GOROROBA_LLVM_VERSION` when set, and otherwise
chooses an installed clang/clang++/llvm-config major that is coherent
on the host.

Host-envs in `build-infra/env/` (`btver1-ccache-no-pump.env`,
`btver1-distcc-pump.env`, `sapphire.env`, `zen4.env`) set the
lane-specific distcc/cache policy, host-specific CFLAGS,
`-fno-emulated-tls` (required for the validated clang lane on linux x86_64 to
avoid a link failure in libglapi), and centralised
`CCACHE_DIR`/`SCCACHE_DIR`.

## Compiler cache status (2026-04-19 empirical)

### ccache -- working as intended

```
Cacheable calls:   1253 / 1339 (93.58%)
Hits on clean:       52 / 1253 ( 4.15%)    (carry-over)
Misses on clean:   1201 / 1253 (95.85%)    (populates cache)
```

First full build populates `~/.cache/ccache`; subsequent
rebuilds with unchanged sources should show >90% hit rate.
Verify with: `ccache --show-stats --verbose`.

### sccache -- patched local Rust wrapper

```
sccache
```

The workspace patched sccache for meson-rust's multi-`--emit` form.
Use PATH order or the host env to select a patched cache binary; do
not bake a per-user path into native files.  Deep RCA and rebuild
instructions live in
`steinmarder/docs/workspace/sccache-multi-emit-patch.md`.

### Do NOT chain wrapper-style

The anti-pattern `exec ccache distcc clang "$@"` (wrapper script
form) causes 93.5% "Multiple source files" ccache rejections --
ccache hashes `distcc`'s mtime as the compiler.  Canonical
answer is always: meson `[binaries]` for the wrap, env
`CCACHE_PREFIX=distcc` for the chain.  Deep RCA in
`steinmarder/docs/workspace/ccache-sccache-wiring.md`.

## Upstream discipline

- `origin` = our fork (Oichkatzelesfrettschen/mesa-26-gororoba).
- `upstream` = fdo mesa/mesa.  3711 commits behind upstream/main
  as of 2026-04-18; rebase cadence + what-to-submit-upstream
  policy in `steinmarder/docs/workspace/mesa-fork-upstream-divergence.md`.
- Never `git push origin upstream/main:main` -- always rebase first.

## Forbidden without explicit user sign-off

- `git push --force` to `main`.
- `sudo rm -rf` on shared workspace paths.
- Introducing `10.0.0.*` raw IPs in scripts/configs (hostname
  policy).
- Chaining `ccache distcc compiler` in a shell wrapper (ccache
  anti-pattern; see above).
- Using `RUSTC_WRAPPER` env for meson-rust (cargo-only; noop
  here -- use `[binaries]` native file).

## RE + evidence (cross-links to steinmarder)

All driver-RCA evidence lives in steinmarder, NOT here.  Before
proposing a terakan fix, consult:

- `steinmarder/src/re/r600/findings/CLAIMS.md` -- live claims tracker
- `steinmarder/src/re/r600/findings/active/`    -- open RCAs
- `steinmarder/src/re/r600/results/`            -- capture bundles + index.csv
- `steinmarder/src/re/r600/docs/rca/`           -- cross-cutting RCAs
- `steinmarder/docs/workspace/host-setup.md`    -- extended host setup recipe
- `steinmarder/docs/workspace/ccache-sccache-wiring.md` -- cache RCA
- `steinmarder/docs/workspace/mesa-fork-synthesis.md`   -- 4-profile consolidation
- `steinmarder/docs/workspace/mesa-fork-upstream-divergence.md` -- upstream rebase
- `steinmarder/docs/workspace/hostname-policy.md`       -- raw-IP ban
- `steinmarder/docs/workspace/mesa-26-debug-and-mesa-debug.md` -- legacy dirs

## Comment + documentation hygiene policy

Scope: all agents (Claude, Codex, humans).  All checked-in artifacts:
source code AND markdown docs.  

### Priority order

1. SOURCE CODE comments (HIGHEST -- code outlives docs).
2. MARKDOWN finding-doc bodies (SECONDARY).
3. FILE-HEADER license blocks (THIRD).

### Durable mechanism names, not phase labels

Branch names, bundle directory names, finding-doc filenames, PR
titles, commit subject lines, and source comments MUST use durable
mechanism names that describe the engineering work itself, NOT phase
or chronology labels.

| Surface | WRONG | RIGHT |
|---|---|---|
| Branch | `terakan/phase1e-atomic-fix` | `terakan/palm_mem_rat_atomic_completion_semantics` |
| Finding filename | `2026-05-18-phase1e-atomic.md` | `2026-05-18-palm-mem-rat-atomic-completion-semantics.md` |
| PR title | `Phase 1E-atomic discriminator` | `palm_mem_rat_atomic_completion_semantics discriminator` |
| Commit subject | `terakan: Phase 1E follow-ups` | `terakan: palm_mem_rat_atomic_completion_semantics follow-ups` |
| Source comment | `Phase 1E-atomic case` | `palm_mem_rat_atomic_completion_semantics case` |

Why: phase labels make sense in the moment and rot fast.  A reviewer
reading the durable mechanism name knows what it is; a reviewer
reading `Phase 1E-atomic` has to dig.  The durable name survives
`git log`, branch listings, finding-doc directory walks, and
follow-up work years later.

Phase labels MAY appear ONLY as secondary registry metadata, e.g. a
YAML frontmatter `phase: 1E-atomic` field on a finding-doc for
project-management chronology.  They are never load-bearing.

### Source code MUST NOT contain

| Banned pattern | Example | Move to |
|---|---|---|
| Task references | `steinmarder task #143` | commit message |
| Issue references | `(see issue #157)` | commit message |
| PR numbers | `mesa PR #25` | commit message |
| Companion-PR breadcrumbs | `companion to PR #...` | commit message |
| Phase labels | `Phase 4.4` | commit message |
| Step-of-phase labels | `Step 1 of Phase 3` | commit message |
| Session dates | `(2026-05-15)` | commit message |
| Deictic time | `as of today`, `currently`, `previously` | rewrite absolute |
| Cross-phase breadcrumbs | `will be exercised when Phase 5 lands` | commit message |
| Date-stamped claim/LI/Q tags | `C-2026-04-19-06`, `LI-2026-04-17-02` | commit message |
| Author tags | `@triang3l`, `(eirikr)` | delete entirely |
| Deictic chip refs | `this chip family`, `our GPU` | rewrite with chip codename |
| Internal-repo paths | `per Evergreen_ISA.txt:17572` | rewrite with public AMD title |
| Personal-name copyright | `Copyright (c) YYYY Eirikr Hinngart` | delete; SPDX-only or no header |

### FIXME and TODO retention

FIXME, TODO, and regression-notice markers document gaps whose resolution is incomplete or unverified.

- MUST NOT remove a FIXME, TODO, or regression-notice comment unless the underlying issue is BOTH fully implemented AND empirically verified in that same commit.
- MUST retain the marker when rewriting surrounding code unless the rewrite itself closes the underlying issue.
- MUST add a new TODO when a fix is implemented but empirical hardware verification remains outstanding.
- MUST NOT drop regression-notice comments during refactoring, structural analysis rewrites, or style cleanup.
- When a FIXME is resolved: if any aspect remains unconfirmed (e.g., hardware test not run, interaction with another subsystem not checked), replace it with a TODO naming the specific open item.

### Source code MUST contain (when HW-specific)

- Absolute chip identity combining angles so the comment is findable
  from any direction:

| Codename (HD #) | RV / part code | Mesa enum | Family / ISA |
|---|---|---|---|
| `R600` (HD 2900) | -- | `CHIP_R600` | R600 / TeraScale-1 |
| `Cypress` (HD 5870) | `RV870` | `CHIP_CYPRESS` | Evergreen / TeraScale-2 VLIW5 |
| `Palm` (HD 6310, Wrestler GPU, Brazos APU + Bobcat CPU) | -- integrated | `CHIP_PALM` | Evergreen / TeraScale-2 VLIW5 |
| `Cayman` (HD 6970) | -- | `CHIP_CAYMAN` | Northern Islands / TeraScale-3 VLIW4 |
| `Aruba` (Trinity APU GPU) | -- integrated | `CHIP_ARUBA` | Northern Islands / TeraScale-3 VLIW4 |

Canonical Palm comment: `Palm (Wrestler GPU, CHIP_PALM, Evergreen / TeraScale-2 VLIW5)`.

- Other naming conventions to use:

| Surface | Canonical form |
|---|---|
| Driver | `Terakan` (Vulkan), `r600g` (Gallium) |
| Register name | `CB_COLOR0_VIEW.SLICE_START`, `SQ_TEX_RESOURCE_WORD4.DST_SEL_X` |
| Register macro | `R_028C70_CB_COLOR0_INFO`, `S_028C70_FORMAT(x)`, `G_028C70_FORMAT(v)`, `V_028C70_COLOR_32` |
| ISA encoding | `FMT_32_32_32 = 47`, `V_028C70_NUMBER_USCALED = 0x2` |
| Architecture | `Evergreen / TeraScale-2 VLIW5`, `Northern Islands / TeraScale-3 VLIW4`, `R600 / TeraScale-1` |
| CPU side | `Bobcat` (CPU in Zacate/Ontario), `Llano` CPU |
| Platform | `Brazos` (Bobcat + Palm), `Llano`, `Trinity` |

- HW citation form (public AMD docs only; NOT internal-repo extracts):

| WRONG (internal) | RIGHT (public) |
|---|---|
| `per Evergreen_ISA.txt:17572` | `per AMD Evergreen-Family ISA, section 10.x.x (MEM_RD_SCATTER)` |
| `see phase5_isa_pdf_audit_20260418T182628Z/...` | `per AMD Radeon HD 6000-Series ISA (Cayman), section X.Y` |
| -- | `per AMD 3D Engine Programming Guide for Evergreen, section M (CB_COLOR0_VIEW)` |
| -- | `per Direct3D 11.3 Functional Specification, section 4.4.6 Element Alignment` |

- Bit-field encodings (`SLICE_START bits 0-10 of CB_COLOR_VIEW`).
- Empirical silicon behavior (`Palm silently no-ops MEM_RAT_CMPXCHG_INT on the cached path`).
- Mathematical invariants and non-obvious workaround rationale.

### Markdown finding-doc rules

Markdown bodies MAY carry chronology (every finding-doc has dated
frontmatter -- `last_verified`, `evidence_class`, dated filename,
ordered predecessors).  That is intentional.

PR# / task# references MUST triangulate with a durable identifier:

| WRONG (PR# alone) | RIGHT (durable primary + PR# cross-link) |
|---|---|
| `the fix landed via mesa PR #34` | `landed in commit f230cb07db6 (terakan_buffer.c::terakan_CreateBuffer size-zero guard); PR #34 / branch fix/w9-buffer-size-zero-guard for cross-link` |
| `see issue #157` | `see filed-finding 2026-05-15-induced-lockup-recovery-test-results.md (PR #41 if still open)` |

### File-header rules

Source of truth: `docs/submittingpatches.rst` (mesa-26 branch).  The
project layers a collective-copyright convention on top of mesa's
SPDX-only baseline.

#### NEW source files in this workspace

When a header is appropriate (match adjacent-file style; many mesa
files use SPDX-only with no Copyright line, which is also fine):

```c
/*
 * Copyright (c) YYYY Terascale Functionalists
 * SPDX-License-Identifier: MIT
 *
 * <one-line file description>
 */
```

- MUST use either the form above OR `/* SPDX-License-Identifier: MIT */`
  alone, depending on adjacent-file style.
- MUST NOT fabricate an individual personal-name Copyright line.  LLM
  templates default to `Copyright (c) YYYY <git config user.name>`
  (e.g. `Eirikr Hinngart`, `eirikr`) -- ALWAYS strip.
- MUST NOT add `(LLM-assisted)`, `Generated by Claude`, or any AI tag
  to the file header.  Mesa's disclosure mechanism is the commit-
  trailer system; file headers stay clean and license-focused.
- MUST NOT use `Copyright (c) YYYY steinmarder project` (legacy
  invented-collective form from earlier work).  Use `Terascale
  Functionalists` instead.

#### EXISTING source files with upstream copyrights

(Vitaliy "Triang3l" Kuzmin-authored Terakan files, mesa-tree files
from prior contributors, any pre-existing header with a non-project
author name.)

- MUST PRESERVE the existing copyright header verbatim, including the
  author name and year.  The MIT license itself (text included in each
  such header) requires preservation: "The above copyright notice and
  this permission notice shall be included in all copies or
  substantial portions of the Software."
- MUST NOT strip or rewrite the upstream copyright when refactoring,
  splitting, or moving the file.
- When ADDING new content to an upstream file, the upstream copyright
  stays; do NOT add a second project-collective Copyright line on top
  of the upstream header.

### Existing work

- Existing in-flight PRs MAY keep breadcrumb comments.  Do NOT
  force-push to scrub them; address in review or let them merge.
- NEW commits MUST follow this policy.
- The git pre-commit hook BLOCKS commits with violations.  Bypass
  via `git commit --no-verify` is permitted in emergencies; the
  commit message MUST explain why.

### Mesa upstream specifically

Source of truth: `docs/submittingpatches.rst` (mesa-26 branch).

- MUST write comments as if the reader is a Mesa maintainer six months
  from now with no idea what "Phase 4" means and no access to our
  task tracker.  Each comment MUST stand alone.
- MUST disclose AI involvement via the mesa-canonical `Assisted-by:`
  trailer.  Mesa explicitly forbids `Co-authored-by:` for AI tools
  (it is reserved for HUMAN co-authors -- see
  `docs/submittingpatches.rst`).  This project's development is a
  multi-LLM-human-in-the-loop chain; the canonical concise form is:

  ```
  Assisted-by: Claude (Opus/Sonnet 4.x), ChatGPT Codex (5.x), Gemini (Flash/Pro 3.x), Mistral, Ollama, DeepSeek
  ```

  List ACTUAL tools used per commit (not every tool every time).  Use
  `Generated-by:` instead when AI generated almost the entire change.
  Trivial / mechanical changes MAY omit disclosure per mesa policy.
- MUST NOT submit via an autonomous tool (per the same doc).

### Past commits with `Co-Authored-By: Claude` trailer

Older commits on this project (pre-policy) used `Co-Authored-By:
Claude Opus 4.7 (1M context) <noreply@anthropic.com>` -- this trailer
violates mesa policy.

Resolution:
- Existing commits are HISTORICAL ARTIFACTS.  We do NOT force-push to
  rewrite git history; the trailers remain in the log.
- NEW commits MUST use `Assisted-by:` per the form above.
- Patch files checked into the tree as historical artifacts
  (`src/re/r600/results/.../*.patch`, kernel-module patch series in
  `src/amd/...`/embedded DKMS sources) MAY retain the original
  trailer -- they are snapshots of the original commits and must not
  be rewritten.

### Commenting voice

The voice this repo already uses, sharpened a little.  Look at the
WORD0 fix block in `src/amd/terascale/vulkan/terakan_dispatch.c` (the
`PKT3_SET_RESOURCE` emit, around the `desc[0] - bo->va` write) and the
in-line silicon notes in `terakan_format.c` / `sfn_instr_mem.cpp`:

- A short paragraph opens with the load-bearing claim ("WORD0 carries
  the per-BO byte offset.").
- Then the primary-source citation, by name -- the kernel function
  (`evergreen_packet3_check`), the ISA chapter, the register macro
  (`R_028C70_CB_COLOR0_INFO`) -- not an internal-repo file extract.
- Then the consequence in one or two lines, with a fragment of code
  inlined when that's clearer than prose:
  `ib[WORD0] = reloc->gpu_offset + offset`.
- A CTS test name or `dEQP-VK.<group>.*` reference when the comment
  exists to explain a test failure being fixed.
- Env knobs / flags described together at the bottom of the block.
- Conversational where appropriate ("Palm silently no-ops
  `MEM_RAT_CMPXCHG_INT` on the cached path") and never abstract ("Set
  the flag").

When extending one of Triang3l's Terakan files, match his tighter
cadence -- shorter line lengths, fewer sub-clauses; the file's
existing comments are the template.

Structural distillate:

- Default short.  One-line trailing comments on the load-bearing line
  beat a function-header paragraph, unless the function as a whole
  encodes a non-obvious invariant.
- One thought per comment; stack them when steps are distinct, rather
  than fusing them into a multi-clause sentence.
- Do not paraphrase the next line of code.  If the code reads, the
  comment is about WHY the code is shaped that way -- the silicon
  constraint, the spec section, the measurement -- not what it does.
- When the code is mechanical, no comment.  Comments earn their place
  by carrying information that does not survive in the code itself.
- Anchor citations by name (AMD Evergreen-Family ISA section,
  `SQ_TEX_RESOURCE_WORD4.DST_SEL_X`, Vulkan spec section) rather than
  by line number or repo-internal path.
- Multi-paragraph blocks reserved for genuine silicon-quirk WHYs.

Prose-level distillate (the part that makes a comment feel like
everything just makes sense):

- State mechanism in the active voice ("the kernel reads WORD0 and
  adds the relocation offset"), not in the passive ("WORD0 is combined
  with the offset").  The hardware does things; say so.
- Let sequence be the explanation.  "X.  Then Y.  Then Z." -- one
  sentence per step, each doing one thing -- beats one sentence with
  three clauses.
- Chain each WHY by what the next step needs ("...so the descriptor
  write can resolve the relocation"), so the comment moves forward
  instead of cataloguing in parallel.

Example shape:

```
The kernel reads WORD0 as a byte offset.  It adds reloc->gpu_offset.
The buffer base reaches the shader at the right VA -- any per-element
offset the caller asked for survives the relocation.
```

This composes with the no-task-#-no-PR-#-no-Phase-X.Y-no-deictic-
refs rules above.  Together: short, active, sequenced, primary-source-
grounded, time-invariant.

### LLM-readable markdown style (for memory/finding-docs/AGENTS.md)

- MUST use heading depth <= 3 levels.
- MUST use exactly one H1 per file (the document title).
- MUST include frontmatter on programmatically-loaded files.
- MUST use language tags on code fences (` ```bash`, ` ```c`).
- SHOULD prefer tables over bullet lists for 3+ comparable items.
- MUST NOT use emoji, ASCII boxes, banner dividers in rules text.
- MUST NOT use "see above" / "see below" -- the file may be slice-loaded.
- MUST use MUST / MUST NOT / SHOULD imperative voice in rules.

### Linter + git hook

Source: `src/re/r600/scripts/lint/comment_hygiene_lint.py`.
Git pre-commit hook: `src/re/r600/scripts/lint/pre-commit-comment-hygiene`.

Install:
```bash
ln -sf $(realpath src/re/r600/scripts/lint/pre-commit-comment-hygiene) .git/hooks/pre-commit
```


## Synthesis Discipline (MANDATORY)

> Always analyze, reconcile and resolve, expand, harmonize, and build conflicts, issues
> and errors to be better than the sums of the parts.

This standing directive governs every multi-source merge, conflict resolution, regression
repair, and review-finding integration.  The default resolution for additive content at
the same location is UNION.  Selection -- keeping one side, discarding the other -- requires
explicit proof that the discarded side is empirically refuted or genuinely superseded.

### The five operational verbs

| Verb | What it requires |
|---|---|
| Analyze | Identify what each side contributes before touching the merge.  Read both sides with adversarial intent -- every line that differs is a candidate for preservation. |
| Reconcile | When two branches both add content at the same location, the default resolution is UNION.  Selection requires explicit proof of refutation or supersession. |
| Resolve | Complete the resolution.  Do not leave the codebase in a half-merged state.  Record outstanding items explicitly if the session cannot finish. |
| Expand | When two findings converge on the same mechanism, surface the connection.  Do not paste two sections side by side without explaining how they relate. |
| Harmonize | Use consistent terminology across synthesized content.  When two sources name the same thing differently, pick one name and apply it uniformly. |

A synthesis MUST contribute new value: a cross-reference, a unified mental model, a
connecting citation, or a refined epistemic tier.  A merge that is only A + B stapled
together is selection, not synthesis.

### Selection is FORBIDDEN unless

1. The discarded side is empirically refuted by a tier-1 to tier-3 source (silicon
   measurement, ISA spec, or kernel source).
2. The discarded side is genuinely superseded -- the successor covers its intent fully,
   verified by line-level diff against the discarded text.

### Adversarial diff read before merge commit

After every merge resolution, re-read the staged diff with the adversarial question:
"What content from source A and source B did this resolution drop that was not
empirically refuted?"

If the answer is "nothing", the synthesis passes.  If the answer is "something", restore
the dropped content, refute it by name and citation, or record it as an explicit
follow-up item.

### WRONG and RIGHT patterns

WRONG -- `-X theirs` for an additive merge silently drops all "ours" additions:

```bash
git merge -X theirs branch/feature-a
```

WRONG -- sed-based conflict-marker strip discards content when diff3 markers are present:

```bash
sed -i '/^<<<<<<< /d; /^=======$/d; /^>>>>>>> /d' file
# silently drops the ||||||| base block and anything between it and =======
```

RIGHT -- cherry-pick with `--no-commit` then review each hunk before staging:

```bash
git cherry-pick --no-commit <sha>
git diff --staged   # adversarial read: verify every hunk is intentional
git commit
```

RIGHT -- closing a PR as superseded with explicit evidence preservation:

```bash
git cherry-pick <sha-from-closed-pr> --no-commit
# verify hunks; add cross-link in the synthesis PR commit message
# only then close the superseded PR
```

Cross-references: steinmarder `AGENTS_README.md` "Synthesis Doctrine" and
`AGENT_RULES.md` "Rule: Synthesis Over Selection" for the gate-oriented check method.

## Key subsystems

- `src/amd/terascale/vulkan/`      terakan Vulkan driver
- `src/gallium/drivers/r600/`      Gallium r600 driver (SFN + VLIW5)
- `src/gallium/frontends/rusticl/` rusticl OpenCL (Rust)
- `build-infra/`                   canonical build entry
- `rust-toolchain.toml`            upstream Mesa file (`channel = "nightly"`)
                                   -- bypassed by absolute-path in configs/*.meson

`CLAUDE.md` and `GEMINI.md` at this repo root are symlinks to
THIS file -- proprietary agents see the same canonical content.
