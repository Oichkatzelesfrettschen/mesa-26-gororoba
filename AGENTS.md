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

## Key subsystems

- `src/amd/terascale/vulkan/`      terakan Vulkan driver
- `src/gallium/drivers/r600/`      Gallium r600 driver (SFN + VLIW5)
- `src/gallium/frontends/rusticl/` rusticl OpenCL (Rust)
- `build-infra/`                   canonical build entry
- `rust-toolchain.toml`            upstream Mesa file (`channel = "nightly"`)
                                   -- bypassed by absolute-path in configs/*.meson

`CLAUDE.md` and `GEMINI.md` at this repo root are symlinks to
THIS file -- proprietary agents see the same canonical content.
