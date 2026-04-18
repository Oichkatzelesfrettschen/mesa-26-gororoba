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

## Canonical build entry

All builds go through `build-infra/` (replaces the 10 legacy
`build-*/` dirs that predated 2026-04-18 synthesis).

\`\`\`sh
cd build-infra
make rebuild-terakan-distcc            # daily iteration lane on x130e
sudo make install PROFILE=terakan-distcc
\`\`\`

Profiles live in `build-infra/configs/`:
- `terakan-full.meson`    r600+zink+soft+llvm, rusticl+HUD+VA (release default)
- `terakan-distcc.meson`  r600-only, daily distcc iteration
- `terakan-minimal.meson` r600-only, no HUD, NIR scratchpad
- `base-debug.meson`      stock Mesa reference (no terakan)

Toolchain host-envs in `build-infra/env/`:
- `btver1.env`  x130e Bobcat (clang-22 + distcc + march=btver1)
- `sapphire.env` Apple Silicon placeholder
- `zen4.env`     Ryzen placeholder

Targets: `configure`, `build`, `clean`, `distclean`, `install`,
`test`, `rebuild-<profile>`, `list`.  Build trees land OUTSIDE
the source at `../../build/mesa-<profile>/`; install prefixes
default to `/usr/local/mesa-<profile>/`.

## Access patterns

On x130e:
- SSH one-shot for quick: `ssh x130e 'git log --oneline -5'`
- Tmux for durable builds (survives disconnects):
  `ssh x130e 'tmux new-session -d -s mesa-build'`
  then `tmux send-keys` to feed commands.
- Current active build tmux: session `mesa-build`.

From Mac via NFS mirror (read-only fast path):
`/Volumes/x130e/workspaces/mesa/mesa-26-gororoba/`.
Hostname-only: never raw IPs (`10.0.0.*`) -- see
[steinmarder/docs/workspace/hostname-policy.md](https://github.com/Oichkatzelesfrettschen/steinmarder/blob/main/docs/workspace/hostname-policy.md).

## RE + evidence

Every debugging session here should reference steinmarder:
- `src/re/r600/findings/CLAIMS.md` -- live claims tracker
- `src/re/r600/findings/active/`    -- open RCAs
- `src/re/r600/results/`            -- capture bundles + index.csv
- `src/re/r600/docs/rca/`           -- cross-cutting RCAs

Do not invent new driver theories here without first consulting
the CLAIMS tracker.

## Key subsystems in this tree

- `src/amd/terascale/vulkan/`    terakan Vulkan driver
- `src/gallium/drivers/r600/`    Gallium r600 driver (SFN + VLIW5)
- `src/gallium/frontends/rusticl/` rusticl OpenCL (Rust)
- `build-infra/`                 canonical build entry (above)

## Upstream discipline

- `origin` is our fork (Oichkatzelesfrettschen/mesa-26-gororoba).
- `upstream` is fdo mesa/mesa.  Rebase r600/terakan work against
  upstream `main` regularly; never force-push to our `main`.
- Fork-specific branches that can be merged to our main:
  `synthesis-*`, `x130e-wip-*` (squash-merge or ff only).

## Forbidden without explicit user sign-off

- `git push --force` to `main`.
- Deleting `mesa-debug/` install prefix until a new green build
  has been verified via `build-infra` (last-known-good binary).
- `sudo rm -rf` on shared workspace paths.
- Introducing `10.0.0.*` raw IPs in scripts/configs (hostname
  policy).

## Related docs and pointer files

- `build-infra/README.md` -- build infra details.
- `README.md` -- upstream Mesa top-level (if present; may be bare).
- `CLAUDE.md` and `GEMINI.md` at this repo root are symlinks to
  THIS file -- proprietary agents resolve the same content.
- `steinmarder/docs/workspace/mesa-fork-synthesis.md` -- why the
  9 legacy build-*/ variants collapse to 4 profiles + 3 host envs.
