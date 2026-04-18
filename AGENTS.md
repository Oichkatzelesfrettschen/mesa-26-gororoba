---
description: Mesa 26.1-devel fork with terakan Vulkan driver for Radeon HD 6310 (Evergreen/Sumo, TeraScale-2)
last_verified: 2026-04-18
---

# mesa-26-gororoba -- agent + developer reference

Fork of Mesa 26.1-devel tracking upstream `main` closely.  Carries
the terakan Vulkan driver at `src/amd/terascale/vulkan/` plus r600
SFN improvements targeting Radeon HD 6310 (CHIP_SUMO, TeraScale-2
VLIW5, 2-SIMD Cayman family).

Target host: x130e (Bobcat + HD 6310 APU).  Primary peer repo:
[steinmarder](https://github.com/Oichkatzelesfrettschen/steinmarder)
-- the RE-toolkit + evidence registry that drives the patches here.

## First-time host setup

A fresh x130e-class builder needs apt packages, rustup stable +
components, `rust-toolchain.toml` awareness, ccache/distcc/sccache,
and distcc workers reachable by HOSTNAME (not raw IP).  Full
reproducible recipe in
[steinmarder/docs/workspace/host-setup.md](https://github.com/Oichkatzelesfrettschen/steinmarder/blob/main/docs/workspace/host-setup.md).

Do NOT skip that doc.  Known pitfalls (`rust-toolchain.toml`
nightly override, clang-22 TLS emutls, meson-rust ignoring
`RUSTC_WRAPPER`) are all called out there.

## Canonical build entry

All builds go through `build-infra/` (replaces the 10 legacy
`build-*/` dirs that predated 2026-04-18 synthesis).

```sh
cd build-infra
. env/btver1.env                   # CCACHE_PREFIX=distcc, -fno-emulated-tls, etc.
./scripts/setup-distcc.sh          # tool-health precheck
make rebuild-terakan-distcc        # daily iteration lane
sudo make install PROFILE=terakan-distcc
```

Install prefix: `/usr/local/mesa-terakan-distcc/` by default.
Isolated -- does NOT overwrite system Mesa at
`/usr/lib/x86_64-linux-gnu/`.  Multiple profiles coexist as
separate prefixes.

## Profiles and envs

Profiles live in `build-infra/configs/`:

- `terakan-full.meson`    r600+zink+soft+llvm, rusticl+HUD+VA (full)
- `terakan-distcc.meson`  r600-only, daily distcc iteration
- `terakan-minimal.meson` r600-only, no HUD, NIR scratchpad
- `base-debug.meson`      stock Mesa reference (no terakan)

Each profile's `[binaries]` section pins the compiler chain:

```ini
c    = ['/usr/bin/ccache',  '/usr/bin/clang-22']
cpp  = ['/usr/bin/ccache',  '/usr/bin/clang++-22']
rust = ['/usr/bin/sccache', '/home/eirikr/.rustup/toolchains/stable-x86_64-unknown-linux-gnu/bin/rustc']
```

The absolute-path stable rustc intentionally bypasses rustup's
shim so the in-repo `rust-toolchain.toml` nightly override does
NOT apply.  See
[steinmarder/docs/workspace/ccache-sccache-wiring.md](https://github.com/Oichkatzelesfrettschen/steinmarder/blob/main/docs/workspace/ccache-sccache-wiring.md)
for the full RCA of why this wiring is the canonical one.

Toolchain host-envs in `build-infra/env/`:

- `btver1.env`  x130e Bobcat (CCACHE_PREFIX=distcc, -march=btver1,
                `-fno-emulated-tls`, `-mtls-dialect=gnu2`,
                CCACHE_DIR + SCCACHE_DIR)
- `sapphire.env` Apple Silicon placeholder
- `zen4.env`     Ryzen placeholder

## Compiler cache verification

After a full rebuild, expected healthy stats:

```sh
ccache --show-stats --verbose | head
# Cacheable calls:  ~1400 / ~1400  (>98%)
# Misses on first rebuild; later rebuilds should hit >90%

sccache -s
# Compile requests > 0 for the rusticl Rust TUs
```

If `Uncacheable calls` dominates, the wiring regressed to the
deprecated `ccache distcc compiler` anti-pattern; see the RCA doc
linked above.

## Access patterns

- SSH one-shot for quick commands:
  `ssh x130e 'git log --oneline -5'`
- Tmux for durable builds (survives disconnects):
  `ssh x130e 'tmux new-session -d -s mesa-build'`
  then `tmux send-keys` to feed commands.  Active session: `mesa-build`.
- NFS read-only mirror on Mac:
  `/Volumes/x130e/workspaces/mesa/mesa-26-gororoba/`.
- Hostname-only, never raw IPs; see
  [steinmarder/docs/workspace/hostname-policy.md](https://github.com/Oichkatzelesfrettschen/steinmarder/blob/main/docs/workspace/hostname-policy.md).

## RE + evidence

Every debugging session references steinmarder:

- `src/re/r600/findings/CLAIMS.md` -- live claims tracker
- `src/re/r600/findings/active/`   -- open RCAs
- `src/re/r600/results/`           -- capture bundles + index.csv
- `src/re/r600/docs/rca/`          -- cross-cutting RCAs

Do not invent driver theories here without first consulting CLAIMS.

## Key subsystems in this tree

- `src/amd/terascale/vulkan/`      terakan Vulkan driver
- `src/gallium/drivers/r600/`      Gallium r600 driver (SFN + VLIW5)
- `src/gallium/frontends/rusticl/` rusticl OpenCL (Rust)
- `build-infra/`                   canonical build entry (above)
- `rust-toolchain.toml`            upstream Mesa file: `channel = "nightly"`.
                                   Honored by rustup shim; our
                                   build-infra bypasses via absolute path.

## Upstream discipline

- `origin` = our fork (Oichkatzelesfrettschen/mesa-26-gororoba).
- `upstream` = fdo mesa/mesa.  Rebase r600/terakan work against
  upstream `main` regularly; never force-push to our `main`.
- Fork-specific branches that may merge to our main:
  `synthesis-*`, `x130e-wip-*` (squash-merge or ff only).

## Forbidden without explicit user sign-off

- `git push --force` to `main`.
- Deleting `mesa-debug/` install prefix until a new green build
  has been verified via `build-infra` (last-known-good binary).
- `sudo rm -rf` on shared workspace paths.
- Introducing `10.0.0.*` raw IPs in scripts/configs.
- Chaining `ccache distcc compiler` (documented ccache anti-pattern).
- Using `RUSTC_WRAPPER` env for meson-rust (it's cargo-only; noop here).

## Related docs

Canonical cross-cutting references live in steinmarder's
`docs/workspace/`:

| Link | Purpose |
|------|---------|
| [host-setup.md](https://github.com/Oichkatzelesfrettschen/steinmarder/blob/main/docs/workspace/host-setup.md) | First-time setup recipe (this doc) |
| [ccache-sccache-wiring.md](https://github.com/Oichkatzelesfrettschen/steinmarder/blob/main/docs/workspace/ccache-sccache-wiring.md) | Compiler-cache RCA + canonical wiring |
| [mesa-fork-synthesis.md](https://github.com/Oichkatzelesfrettschen/steinmarder/blob/main/docs/workspace/mesa-fork-synthesis.md) | Why 9 build variants collapsed to 4 profiles |
| [hostname-policy.md](https://github.com/Oichkatzelesfrettschen/steinmarder/blob/main/docs/workspace/hostname-policy.md) | NFS/SSH hostname-only rule |
| [mesa-26-debug-and-mesa-debug.md](https://github.com/Oichkatzelesfrettschen/steinmarder/blob/main/docs/workspace/mesa-26-debug-and-mesa-debug.md) | What the legacy dirs were |

`CLAUDE.md` and `GEMINI.md` at this repo root are symlinks to
THIS file -- proprietary agents see the same canonical content.
