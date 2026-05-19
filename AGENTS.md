---
description: Mesa 26.1-devel fork with terakan Vulkan driver for Radeon HD 6310 (Evergreen/Sumo, TeraScale-2)
last_verified: 2026-04-19
---

# mesa-26-gororoba -- agent + developer reference

Fork of Mesa 26.1-devel tracking upstream `main` closely.  Carries
the terakan Vulkan driver at `src/amd/terascale/vulkan/` plus r600
SFN improvements targeting Radeon HD 6310 (CHIP_SUMO, TeraScale-2
VLIW5, 2-SIMD Cayman family).

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

## Standalone build (this repo only)

Minimum toolchain to build locally:

```sh
sudo apt install meson ninja-build clang-21 pkg-config \
    ccache distcc sccache bindgen rustup \
    libdrm-dev libxcb-dri3-dev libxcb-present-dev libxshmfence-dev \
    libx11-xcb-dev libxrandr-dev libxcb-randr0-dev libxcb-sync-dev \
    libxcb-xfixes0-dev libwayland-dev wayland-protocols \
    python3-mako python3-pip python3-ply \
    zlib1g-dev libzstd-dev libexpat1-dev libsensors-dev \
    llvm-21-dev libclang-21-dev libspirv-tools-dev \
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

The x130e canonical build split is:

| Use case | C/C++ chain | Rust chain | Command |
| --- | --- | --- | --- |
| Warm incremental | `ccache -> distcc -> clang-21`, no pump | `sccache -> rustc` | `make rebuild-terakan-distcc-no-rusticl-ccache-no-pump` |
| Cold clean / max remote preprocessing | `distcc-pump -> distcc -> clang-21`, no ccache | `sccache -> rustc` | `make rebuild-terakan-distcc-no-rusticl-pump` |

Do not put `ccache` or `sccache` in front of C/C++ distcc-pump.  Pump
needs distcc to see the original source and compiler command.  The Rust
sccache lane is separate and remains valid because it wraps rustc, not
the C/C++ include-server path.

Warm/no-pump profiles pin:

```ini
c    = ['ccache', 'clang-21']
cpp  = ['ccache', 'clang++-21']
rust = ['sccache', 'rustc']
llvm-config = 'llvm-config-21'
```

Cold/pump profiles pin:

```ini
c    = ['distcc', 'clang-21']
cpp  = ['distcc', 'clang++-21']
rust = ['sccache', 'rustc']
llvm-config = 'llvm-config-21'
```

Native files MUST use PATH-resolved compiler names, not user-specific
toolchain paths.  `rustc` is selected by the active rustup/toolchain
policy for the checkout; `sccache` is the optional cache wrapper in
front of it.  If a host needs a specific Rust channel, set that through
the repo toolchain file or the host env, not by hard-coding a
`~/.rustup/toolchains/.../bin/rustc` path in a Meson native file.
Version-coupled tools such as `llvm-config-21` stay versioned but
PATH-resolved so the compiler and LLVM dependency agree without
encoding a local filesystem path.

Host-envs in `build-infra/env/` (`btver1-ccache-no-pump.env`,
`btver1-distcc-pump.env`, `sapphire.env`, `zen4.env`) set the
lane-specific distcc/cache policy, host-specific CFLAGS,
`-fno-emulated-tls` (required for clang-21 on linux x86_64 to
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
| `per Evergreen_ISA.txt:17572` | `per AMD Evergreen-Family ISA, §10.x.x (MEM_RD_SCATTER)` |
| `see phase5_isa_pdf_audit_20260418T182628Z/...` | `per AMD Radeon HD 6000-Series ISA (Cayman), §X.Y` |
| -- | `per AMD 3D Engine Programming Guide for Evergreen, §M (CB_COLOR0_VIEW)` |
| -- | `per Direct3D 11.3 Functional Specification, §4.4.6 Element Alignment` |

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
- MUST NOT use `Copyright © YYYY steinmarder project` (legacy
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

- MUST use heading depth ≤ 3 levels.
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

## Key subsystems

- `src/amd/terascale/vulkan/`      terakan Vulkan driver
- `src/gallium/drivers/r600/`      Gallium r600 driver (SFN + VLIW5)
- `src/gallium/frontends/rusticl/` rusticl OpenCL (Rust)
- `build-infra/`                   canonical build entry
- `rust-toolchain.toml`            upstream Mesa file (`channel = "nightly"`)
                                   -- bypassed by absolute-path in configs/*.meson

`CLAUDE.md` and `GEMINI.md` at this repo root are symlinks to
THIS file -- proprietary agents see the same canonical content.
