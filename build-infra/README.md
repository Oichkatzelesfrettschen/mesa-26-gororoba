# mesa-26-gororoba/build-infra

Canonical build infrastructure for the gororoba Mesa fork.

## Canonical profiles

Six numbered profiles in `build-infra/configs/`:

| Profile | Target | Surface | Type |
|---|---|---|---|
| `1_r300_full_release_x86_64v1-clang22-distcc-cache` | vostro (RS482, r300) | maximal r300 + amd_r300 ICD | release |
| `2_r300_full_debug_x86_64v1-clang22-distcc-cache` | vostro (RS482, r300) | maximal r300 + amd_r300 ICD | debug |
| `3_terakan_full_release_x86_64v1-clang22-distcc-cache` | x130e (PALM, r600) | r600+zink+soft+llvm+amd_terascale + Rusticl | release |
| `4_terakan_full_debug_x86_64v1-clang22-distcc-cache` | x130e (PALM, r600) | r600+zink+soft+llvm+amd_terascale + Rusticl | debug |
| `5_terakan_norusticl_release_x86_64v1-clang22-distcc-cache` | x130e (PALM, r600) | same as 3_ without Rusticl | release |
| `6_terakan_norusticl_debug_x86_64v1-clang22-distcc-cache` | x130e (PALM, r600) | same as 4_ without Rusticl | debug |

All six use `HOSTENV=vostro1000-x86-64-v1-clang22-ccache-distcc` and
`COMPILER_CHAIN=ccache`.  `make install PROFILE=...` lands in the isolated
per-profile prefix `/opt/local/mesa-<profile>` by default; the shared active
trees `/opt/local/mesa-26-gororoba` (release) and
`/opt/local/mesa-26-gororoba-debug` (debug) are used only by the
`install-<profile>` targets or when an explicit `PREFIX=` is passed.

## Layout

```text
build-infra/
|-- Makefile                       # entry point
|-- README.md                      # this file
|-- configs/
|   |-- 1_r300_full_release_x86_64v1-clang22-distcc-cache.meson
|   |-- 2_r300_full_debug_x86_64v1-clang22-distcc-cache.meson
|   |-- 3_terakan_full_release_x86_64v1-clang22-distcc-cache.meson
|   |-- 4_terakan_full_debug_x86_64v1-clang22-distcc-cache.meson
|   |-- 5_terakan_norusticl_release_x86_64v1-clang22-distcc-cache.meson
|   +-- 6_terakan_norusticl_debug_x86_64v1-clang22-distcc-cache.meson
+-- env/
    |-- vostro1000-x86-64-v1-clang22-ccache-distcc.env  # primary vostro + x130e lane
    |-- btver1.env                 # x130e (Bobcat) LLVM-family + distcc
    |-- btver1-ccache-no-pump.env  # x130e ccache-first distcc, no pump
    |-- btver1-distcc-pump.env     # x130e direct distcc-pump, no ccache
    +-- ...                        # additional host envs
```

Build outputs land OUTSIDE the source tree at `../../build/mesa-<profile>/`,
so `git clean -xdf` in gororoba does not nuke ongoing builds.

## Build-system policy

- Meson native files carry Mesa options.
- Make is the only build orchestration layer above Meson.
- Host-specific LLVM command names are generated into
  `$BUILDDIR/gororoba-toolchain.meson` during `make configure`.
- New build-system behavior belongs in `build-infra/Makefile` or Meson files.
  Do not add standalone helper scripts for compiler selection, audit policy,
  or clean/build orchestration.

## Common flows

Before a long build, run the host audit:

```bash
make audit PROFILE=5_terakan_norusticl_release_x86_64v1-clang22-distcc-cache \
           HOSTENV=vostro1000-x86-64-v1-clang22-ccache-distcc
```

r300 RELEASE build (vostro, canonical measurement lane):
```bash
make rebuild-1_r300_full_release_x86_64v1-clang22-distcc-cache
make install-1_r300_full_release_x86_64v1-clang22-distcc-cache
make artifact-check PROFILE=1_r300_full_release_x86_64v1-clang22-distcc-cache PREFIX=/opt/local/mesa-26-gororoba
```

r600/terakan RELEASE build (x130e, Rusticl enabled):
```bash
make rebuild-3_terakan_full_release_x86_64v1-clang22-distcc-cache
make install-3_terakan_full_release_x86_64v1-clang22-distcc-cache
```

r600/terakan RELEASE build (x130e, no Rusticl -- use when bindgen breaks):
```bash
make rebuild-5_terakan_norusticl_release_x86_64v1-clang22-distcc-cache
make install-5_terakan_norusticl_release_x86_64v1-clang22-distcc-cache
```

Show available profiles + hostenvs:
```bash
make list
```

Full reset of a profile (removes builddir and archives install prefix aside):
```bash
make distclean PROFILE=5_terakan_norusticl_release_x86_64v1-clang22-distcc-cache
```

Runtime smoke test (terakan):
```bash
export PREFIX=/opt/local/mesa-26-gororoba
export LD_LIBRARY_PATH=$PREFIX/lib/x86_64-linux-gnu:$LD_LIBRARY_PATH
export VK_DRIVER_FILES=$PREFIX/share/vulkan/icd.d/terascale_icd.x86_64.json
vulkaninfo --summary
```

## Cache discipline

Warm incremental: `ccache -> distcc -> clang` (use `COMPILER_CHAIN=ccache`).
Do not put `ccache` or `sccache` in front of C/C++ distcc-pump.  Pump needs
distcc to see the original source and compiler command.

## Adding a profile

1. Create `configs/<new-profile>.meson` with `[built-in options]` + `[project options]`.
2. Add `rebuild-<new-profile>:` and `install-<new-profile>:` targets in the Makefile.
3. Document the profile purpose in `AGENTS.md` under "Build profiles and host envs".
