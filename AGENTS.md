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
    BUILDDIR=/home/eirikr/workspaces/mesa/build/mesa-terakan-distcc-no-rusticl-ccache-no-pump
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
c    = ['/usr/bin/ccache',  '/usr/bin/clang-21']
cpp  = ['/usr/bin/ccache',  '/usr/bin/clang++-21']
rust = ['/home/eirikr/.local/bin/sccache', '/home/eirikr/.rustup/toolchains/stable-x86_64-unknown-linux-gnu/bin/rustc']
```

Cold/pump profiles pin:

```ini
c    = ['/usr/bin/distcc',  '/usr/bin/clang-21']
cpp  = ['/usr/bin/distcc',  '/usr/bin/clang++-21']
rust = ['/home/eirikr/.local/bin/sccache', '/home/eirikr/.rustup/toolchains/stable-x86_64-unknown-linux-gnu/bin/rustc']
```

Absolute-path stable rustc bypasses the in-repo
`rust-toolchain.toml` (which specifies `channel = "nightly"`)
because `/usr/bin/rustc` is a Debian rustup shim that honors the
in-repo file otherwise.  Confirmed stable 1.95.0 builds Mesa 26
rusticl cleanly -- nightly is not required.

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
~/.local/bin/sccache
```

The workspace patched sccache for meson-rust's multi-`--emit` form.
Use the absolute path in native files so agents do not accidentally
pick an older `/usr/bin/sccache`.  Deep RCA and rebuild instructions
live in `steinmarder/docs/workspace/sccache-multi-emit-patch.md`.

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

For NEW source files in this workspace:
- MUST use one of: `/* SPDX-License-Identifier: MIT */` or no header.
- MUST NOT add personal-name Copyright lines.  LLM templates default
  to `Copyright (c) YYYY <git config user.name>` -- ALWAYS strip.

Original upstream files (Triang3l-authored Terakan files): leave
existing headers untouched.

### Existing work

- Existing in-flight PRs MAY keep breadcrumb comments.  Do NOT
  force-push to scrub them; address in review or let them merge.
- NEW commits MUST follow this policy.
- The git pre-commit hook BLOCKS commits with violations.  Bypass
  via `git commit --no-verify` is permitted in emergencies; the
  commit message MUST explain why.

### Mesa upstream specifically

- MUST write comments as if the reader is a Mesa maintainer six months
  from now with no idea what "Phase 4" means and no access to our
  task tracker.  Each comment MUST stand alone.
- MUST disclose AI involvement via the `Co-Authored-By: Claude ...`
  commit trailer per mesa's `docs/submittingpatches.rst`.
- MUST NOT submit via an autonomous tool (per the same doc).

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
