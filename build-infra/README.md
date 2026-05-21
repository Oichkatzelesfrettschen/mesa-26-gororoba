# mesa-26-gororoba/build-infra

Canonical build infrastructure for the gororoba Mesa fork.
Replaces the nine pre-synthesis `build-*/` directories.

## Why this exists

Pre-synthesis the tree had `build-debug/`, `build-terakan-debug/`,
`build-terakan-distcc/`, `build-terakan-distcc-clean/`,
`build-terakan-distcc-gororoba/`, `build-terakan-nirbit/`,
`build-terakan-plain/`, `build-terakan-spiwatch/`,
`build-terakan-vtxfetch/`, `build-terakan-vtxfetch-cachefix/` --
each a separate `meson setup` with options implicit in the dir's
name.  Option diffing collapsed them to four canonical sets
(`terakan-full`, `terakan-distcc`, `terakan-minimal`, `base-debug`)
plus forward-looking `release`/`profile` variants.  See
`../../steinmarder/docs/workspace/mesa-fork-synthesis.md`.

## Layout

```text
build-infra/
|-- Makefile                       # entry point
|-- README.md                      # this file
|-- configs/
|   |-- terakan-full.meson         # r600+zink+soft+llvm, rusticl+HUD+VA
|   |-- terakan-distcc.meson       # r600 only, rusticl recovery lane
|   |-- terakan-distcc-no-rusticl.meson
|   |                                  # r600+terakan, no Rusticl
|   |-- terakan-distcc-no-rusticl-pump.meson
|   |                                  # clean-build pump lane, no Rusticl
|   |-- terakan-minimal.meson      # r600 only, no HUD, NIR scratchpad
|   +-- base-debug.meson           # upstream Mesa reference, no terakan
+-- env/
    |-- btver1.env                 # x130e (Bobcat) LLVM-family + distcc
    |-- btver1-ccache-no-pump.env  # x130e ccache-first distcc, no pump
    |-- btver1-distcc-pump.env     # x130e direct distcc-pump, no ccache
    |-- sapphire.env               # Apple Silicon (placeholder)
    +-- zen4.env                   # AMD Ryzen (placeholder)
```

Build outputs land OUTSIDE the source tree, at
`../../build/mesa-<profile>/`, so `git clean -xdf` in gororoba
does not nuke ongoing builds.

Legacy in-tree `build-terakan-*` directories are not build infrastructure.
They are superseded by this Makefile and ignored at the source root.  Terakan
evidence bundles belong in steinmarder under `src/re/r600/results/`, not in
Mesa build directories.

`r300vk` is the RS480/R300 Vulkan research lane in steinmarder
`src/re/r300/`.  Until a Mesa-side R300 Vulkan ICD exists, `r300vk` probes and
evidence stay in steinmarder; this repository remains the Terakan code and
Mesa build-infra checkout.

The install prefix defaults to `/usr/local/mesa-<profile>`, derived from
`INSTALL_NAMESPACE` and `PROFILE`.  This keeps profile-specific artifacts
isolated because `meson install` does not remove files from an earlier
profile.  Pass `PREFIX=...` explicitly when intentionally installing into a
shared active tree.

Before a long build, run the host audit:

```bash
make audit PROFILE=terakan-distcc-no-rusticl HOSTENV=btver1-ccache-no-pump
```

## Common flows

Build-system policy:

- Meson native files carry Mesa options.
- Make is the only build orchestration layer above Meson.
- Host-specific LLVM command names are generated into
  `$BUILDDIR/gororoba-toolchain.meson` during `make configure`.
- New build-system behavior belongs in `build-infra/Makefile` or Meson
  files.  Do not add standalone helper scripts for compiler selection,
  audit policy, or clean/build orchestration.

Daily Terakan Vulkan iteration on x130e:
```bash
make audit PROFILE=terakan-distcc-no-rusticl HOSTENV=btver1-ccache-no-pump
make rebuild-terakan-distcc-no-rusticl-ccache-no-pump
make install PROFILE=terakan-distcc-no-rusticl \
  BUILDDIR=~/workspaces/mesa/build/mesa-terakan-distcc-no-rusticl-ccache-no-pump
make artifact-check
```

Rusticl-enabled recovery lane:
```bash
make rebuild-terakan-distcc
```

This profile is the Rusticl recovery lane and requires `bindgen`, `rustfmt`,
and a coherent clang/clang++/llvm-config major on the host.  The Makefile
writes `$BUILDDIR/gororoba-toolchain.meson` before `meson setup`, so the
committed Meson profiles describe Mesa options while the generated overlay
captures the host's installed LLVM naming scheme.

No-Rusticl x130e warm/incremental rebuild that preserves ccache and
does not use distcc-pump:
```bash
make rebuild-terakan-distcc-no-rusticl-ccache-no-pump
```

This target uses `configs/terakan-distcc-no-rusticl.meson` with
`env/btver1-ccache-no-pump.env`, removes only
`~/workspaces/mesa/build/mesa-terakan-distcc-no-rusticl-ccache-no-pump`,
strips the pump-only `,cpp` option from `~/.distcc/hosts`, and leaves
`~/.cache/ccache` plus `~/.cache/sccache` intact. It is separate from
`~/workspaces/mesa/build/mesa-terakan-distcc-no-rusticl` so a
future rebuild does not collide with that active build lane.

No-Rusticl x130e cold clean rebuild with maximum remote preprocessing:
```bash
make rebuild-terakan-distcc-no-rusticl-pump
```

This target uses `configs/terakan-distcc-no-rusticl-pump.meson` with
`env/btver1-distcc-pump.env` and build directory
`~/workspaces/mesa/build/mesa-terakan-distcc-no-rusticl-pump`.
It configures normally, prebuilds generated Meson/Ninja targets outside
the include-server lifetime, then runs the heavy compile under
`distcc-pump`.

Canonical split:

| Use case | C/C++ chain | Rust chain | Notes |
| --- | --- | --- | --- |
| Warm incremental | `ccache -> distcc -> clang` | `sccache -> rustc` | no pump |
| Cold clean | `distcc-pump -> distcc -> clang` | `sccache -> rustc` | default pump host is the verified x570 mDNS worker |

Do not put `ccache` or `sccache` in front of C/C++ distcc-pump.  Pump
needs distcc to see the original source and compiler command; wrappers
that preprocess or cache before distcc defeat the include-server path.
Meson setup remains local: configure-time compiler probes and generated
target discovery run without distcc or pump variables, then Ninja performs
the distributed compile phase.
The DESKTOP/WSL worker remains in the classic no-pump mesh until pump
object parity is proven; opt in with `TERAKAN_PUMP_ALLOW_DESKTOP=1`
only for parity probes.

Fresh-from-clean full build (longer; zink+llvmpipe+softpipe):
```bash
make rebuild-terakan-full
```

NIR pass experiment:
```bash
make rebuild-terakan-minimal
```

Stock Mesa reference (no terakan) for regression comparison:
```bash
make rebuild-base-debug
```

Full reset of a profile (removes builddir and archives install prefix aside):
```bash
make distclean PROFILE=terakan-distcc
```

Install the already-converged build without letting root rebuild targets:

```bash
make install PROFILE=terakan-distcc-no-rusticl \
  BUILDDIR=~/workspaces/mesa/build/mesa-terakan-distcc-no-rusticl-ccache-no-pump
make artifact-check
```

Runtime smoke test:

```bash
export PREFIX=/usr/local/mesa-terakan-distcc-no-rusticl
export LD_LIBRARY_PATH=$PREFIX/lib/x86_64-linux-gnu:$LD_LIBRARY_PATH
export VK_DRIVER_FILES=$PREFIX/share/vulkan/icd.d/terascale_icd.x86_64.json
vulkaninfo --summary
```

Delivery policy:

- Current delivery is `/usr/local/mesa-<profile>` staging, not a PKGBUILD.
- Rollback means moving the prefix aside with `make distclean`, not deleting it.
- A PKGBUILD is a future packaging task once the Terakan-only install manifest
  and stale-Rusticl cleanup contract are stable.

Show available profiles + hostenvs:
```bash
make list
```

## Adding a profile

1. Create `configs/<new-profile>.meson` with the distinguishing
   `[project options]`.
2. Add `rebuild-<new-profile>:` target in the Makefile.
3. Document the choice in
   `../../steinmarder/docs/workspace/mesa-fork-synthesis.md`.

## Adding a hostenv

1. Create `env/<new-host>.env` with `CC`, `CXX`, `CFLAGS`, etc.
2. Invoke with `HOSTENV=<new-host>`, e.g. `make configure
   PROFILE=terakan-full HOSTENV=zen4`.
