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

```
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
    |-- btver1.env                 # x130e (Bobcat) clang-21 + distcc
    |-- btver1-ccache-no-pump.env  # x130e ccache-first distcc, no pump
    |-- btver1-distcc-pump.env     # x130e direct distcc-pump, no ccache
    |-- sapphire.env               # Apple Silicon (placeholder)
    +-- zen4.env                   # AMD Ryzen (placeholder)
```

Build outputs land OUTSIDE the source tree, at
`../../build/mesa-<profile>/`, so `git clean -xdf` in gororoba
does not nuke ongoing builds.

The active install prefix defaults to `/usr/local/mesa-26-gororoba`.
Build variants live in separate build directories, but the installed ICD is a
single canonical Terakan copy unless `PREFIX=...` is passed explicitly.

Before a long build, run the host audit:

```
make audit PROFILE=terakan-distcc-no-rusticl HOSTENV=btver1-ccache-no-pump
```

## Common flows

Daily Terakan Vulkan iteration on x130e:
```
make audit PROFILE=terakan-distcc-no-rusticl HOSTENV=btver1-ccache-no-pump
make rebuild-terakan-distcc-no-rusticl-ccache-no-pump
make install PROFILE=terakan-distcc-no-rusticl \
  BUILDDIR=/home/eirikr/workspaces/mesa/build/mesa-terakan-distcc-no-rusticl-ccache-no-pump
make artifact-check
```

Rusticl-enabled recovery lane:
```
make rebuild-terakan-distcc
```

This profile is the Rusticl recovery lane and requires `bindgen`, `rustfmt`,
and `llvm-config-21` availability on the host.

No-Rusticl x130e warm/incremental rebuild that preserves ccache and
does not use distcc-pump:
```
make rebuild-terakan-distcc-no-rusticl-ccache-no-pump
```

This target uses `configs/terakan-distcc-no-rusticl.meson` with
`env/btver1-ccache-no-pump.env`, removes only
`/home/eirikr/workspaces/mesa/build/mesa-terakan-distcc-no-rusticl-ccache-no-pump`,
strips the pump-only `,cpp` option from `~/.distcc/hosts`, and leaves
`~/.cache/ccache` plus `~/.cache/sccache` intact. It is separate from
`/home/eirikr/workspaces/mesa/build/mesa-terakan-distcc-no-rusticl` so a
future rebuild does not collide with that active build lane.

No-Rusticl x130e cold clean rebuild with maximum remote preprocessing:
```
make rebuild-terakan-distcc-no-rusticl-pump
```

This target uses `configs/terakan-distcc-no-rusticl-pump.meson` with
`env/btver1-distcc-pump.env` and build directory
`/home/eirikr/workspaces/mesa/build/mesa-terakan-distcc-no-rusticl-pump`.
It configures normally, prebuilds generated Meson/Ninja targets outside
the include-server lifetime, then runs the heavy compile under
`distcc-pump`.

Canonical split:

| Use case | C/C++ chain | Rust chain | Notes |
| --- | --- | --- | --- |
| Warm incremental | `ccache -> distcc -> clang-21` | `sccache -> rustc` | no pump |
| Cold clean | `distcc-pump -> distcc -> clang-21` | `sccache -> rustc` | default pump host is the verified x570 mDNS worker |

Do not put `ccache` or `sccache` in front of C/C++ distcc-pump.  Pump
needs distcc to see the original source and compiler command; wrappers
that preprocess or cache before distcc defeat the include-server path.
The DESKTOP/WSL worker remains in the classic no-pump mesh until pump
object parity is proven; opt in with `TERAKAN_PUMP_ALLOW_DESKTOP=1`
only for parity probes.

Fresh-from-clean full build (longer; zink+llvmpipe+softpipe):
```
make rebuild-terakan-full
```

NIR pass experiment:
```
make rebuild-terakan-minimal
```

Stock Mesa reference (no terakan) for regression comparison:
```
make rebuild-base-debug
```

Full reset of a profile (removes builddir and archives install prefix aside):
```
make distclean PROFILE=terakan-distcc
```

Install the already-converged build without letting root rebuild targets:

```
make install PROFILE=terakan-distcc-no-rusticl \
  BUILDDIR=/home/eirikr/workspaces/mesa/build/mesa-terakan-distcc-no-rusticl-ccache-no-pump
make artifact-check
```

Runtime smoke test:

```
export LD_LIBRARY_PATH=/usr/local/mesa-26-gororoba/lib/x86_64-linux-gnu:$LD_LIBRARY_PATH
export VK_DRIVER_FILES=/usr/local/mesa-26-gororoba/share/vulkan/icd.d/terascale_icd.x86_64.json
vulkaninfo --summary
```

Delivery policy:

- Current delivery is `/usr/local/mesa-26-gororoba` staging, not a PKGBUILD.
- Rollback means moving the prefix aside with `make distclean`, not deleting it.
- A PKGBUILD is a future packaging task once the Terakan-only install manifest
  and stale-Rusticl cleanup contract are stable.

Show available profiles + hostenvs:
```
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
