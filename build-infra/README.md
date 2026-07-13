# mesa-26-gororoba/build-infra

Canonical build infrastructure for the gororoba Mesa fork.

## Canonical profiles

The default profile sits at the top of `build-infra/configs/`; the other five
live in `build-infra/configs/alternates/` and are selected by passing
`PROFILE=` explicitly.  The Makefile resolves a bare profile name against both
directories, so the per-profile `rebuild-`/`install-` targets need no path
prefix.

| Profile | Target | Surface | Type | Location |
|---|---|---|---|---|
| `3_r300_full_debug_optimized_x86_64v1-clang22-distcc-cache` (default) | vostro (RS482, r300) | maximal r300 + ati_r300 ICD | debug | `configs/` |
| `4_r300_full_release_x86_64v1-clang22-distcc-cache` | vostro (RS482, r300) | maximal r300 + ati_r300 ICD | release (conformance baseline) | `configs/alternates/` |
| `3_terakan_full_release_x86_64v1-clang22-distcc-cache` | x130e (PALM, r600) | r600+zink+soft+llvm+amd_terascale + Rusticl | release | `configs/alternates/` |
| `4_terakan_full_debug_x86_64v1-clang22-distcc-cache` | x130e (PALM, r600) | r600+zink+soft+llvm+amd_terascale + Rusticl | debug | `configs/alternates/` |
| `5_terakan_norusticl_release_x86_64v1-clang22-distcc-cache` | x130e (PALM, r600) | same as 3_ without Rusticl | release | `configs/alternates/` |
| `6_terakan_norusticl_debug_x86_64v1-clang22-distcc-cache` | x130e (PALM, r600) | same as 4_ without Rusticl | debug | `configs/alternates/` |

All six numbered profiles use
`HOSTENV=vostro1000-x86-64-v1-clang22-ccache-distcc` and
`COMPILER_CHAIN=ccache`.  Conformance and silicon-evidence runs use the release
profile
(`4_r300_full_release`, now under `alternates/`) because an asserts-live debug
build can abort a CTS/Piglit case that release would pass.  `make install
PROFILE=...` lands in the isolated per-profile prefix `/opt/local/mesa-<profile>`
by default; the shared active trees `/opt/local/mesa-26-gororoba` (release) and
`/opt/local/mesa-gororoba-debug-optimized` (debug) are used only by the
`install-<profile>` targets or when an explicit `PREFIX=` is passed.

## Layout

```text
build-infra/
|-- Makefile                       # entry point
|-- README.md                      # this file
|-- configs/
|   |-- 3_r300_full_debug_optimized_x86_64v1-clang22-distcc-cache.meson   # default profile
|   +-- alternates/                # non-default profiles; pass PROFILE= explicitly
|       |-- 4_r300_full_release_x86_64v1-clang22-distcc-cache.meson
|       |-- 3_terakan_full_release_x86_64v1-clang22-distcc-cache.meson
|       |-- 4_terakan_full_debug_x86_64v1-clang22-distcc-cache.meson
|       |-- 5_terakan_norusticl_release_x86_64v1-clang22-distcc-cache.meson
|       +-- 6_terakan_norusticl_debug_x86_64v1-clang22-distcc-cache.meson
+-- env/
    |-- vostro1000-x86-64-v1-clang22-ccache-distcc.env  # numbered profiles
    |-- generic-x86-64-os.env       # portable ad hoc lane
    +-- Archive/                    # removed host envs retained for provenance
```

Build outputs land in the gitignored repo-local `build/mesa-<profile>/` tree, so
they never appear in `git status`, and `make clean` removes the active profile's
subdir.  Each worktree gets its own `build/`, so parallel profile builds do not
collide.  (`git clean -xdf` also removes them since they are ignored; prefer
`make clean` to drop one profile without touching the others or the source.)

## Build-system policy

- Meson native files carry Mesa options.
- Make is the only build orchestration layer above Meson.
- Host-specific LLVM command names are generated into
  `$BUILDDIR/gororoba-toolchain.meson` during `make configure`.
- `make configure` and `make install` re-assert every `[project options]`
  entry from the profile as `-D` flags (via
  `scripts/meson_profile_dflags.py`).  Native-file values are defaults only;
  after Meson drops a retired choice (for example the old `amd_r300`
  vulkan-drivers token), coredata resets to the option default
  (`vulkan-drivers=auto`) and the native file alone does not re-apply.  On
  x86_64, `auto` pulls lavapipe while r300 profiles keep `llvm=disabled`, so
  configure aborts.  The CLI `-D` pass heals that drift without a wipe.
- New build-system behavior belongs in `build-infra/Makefile` or Meson files.
  Do not add standalone helper scripts for compiler selection, audit policy,
  or clean/build orchestration.  Make-invoked implementation bodies under
  `scripts/` (profile audit, profile `-D` extraction) stay allowed.

## Common flows

Before a long build, run the host audit:

```bash
make audit PROFILE=5_terakan_norusticl_release_x86_64v1-clang22-distcc-cache \
           HOSTENV=vostro1000-x86-64-v1-clang22-ccache-distcc
```

r300 DEBUG build (vostro, **default install target** -- assertions live,
gallium-xa XA tracker, valgrind/libunwind/perfetto instrumentation):
```bash
make rebuild-3_r300_full_debug_optimized_x86_64v1-clang22-distcc-cache
# Package and install as the system Mesa (replaces stock mesa or release build):
cd build-infra/packaging/mesa-gororoba-debug && makepkg --noconfirm && yes | sudo pacman -U mesa-gororoba-debug-*.pkg.tar.zst
```

r300 RELEASE build (vostro, conformance-baseline -- use only for CTS/Piglit/deqp runs
where assertions-live behavior would contaminate pass/fail):
```bash
make rebuild-4_r300_full_release_x86_64v1-clang22-distcc-cache
make install-4_r300_full_release_x86_64v1-clang22-distcc-cache
make artifact-check PROFILE=4_r300_full_release_x86_64v1-clang22-distcc-cache PREFIX=/opt/local/mesa-26-gororoba
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
The distcc-pump profiles and Make targets were removed with the lane
consolidation; remaining pump notes live only in archived provenance docs.

## Adding a profile

1. Create `configs/<new-profile>.meson` with `[built-in options]` + `[project options]`.
2. Add `rebuild-<new-profile>:` and `install-<new-profile>:` targets in the Makefile.
3. Document the profile purpose in `AGENTS.md` under "Build profiles and host envs".
