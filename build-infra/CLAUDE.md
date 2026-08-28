# Build and cache doctrine for mesa-26-gororoba

`AGENTS.md` at the repository root owns the Mesa rules and points here for build
configuration, install prefixes, and cache wiring. This file carries the two
chapters that bind under `build-infra/`: the standalone build and the
build-system and cache discipline. Every rule in `AGENTS.md` applies to work in
this directory; this file adds the build-specific mechanism. A session started
inside `build-infra/` loads this file and reaches the root rules through a Read
of `AGENTS.md` at the repository root, because an ancestor loader's imports stay
unresolved.

## Standalone build

Mesa builds from this repository alone, with reproducible native files and
environment variables. Meson owns configuration and Ninja generation. Make and
build-infra own host selection, audit checks, generated native overlays, clean,
build, and install. Change that split only with explicit approval for a
build-system architecture change.

Baseline standalone build:

```bash
meson setup builddir \
  --prefix="/opt/mesa-gororoba-debug-optimized" \
  -Dbuildtype=debugoptimized \
  -Dgallium-drivers=r300,r600,softpipe \
  -Dvulkan-drivers=amd_terascale \
  -Dllvm=enabled
ninja -C builddir
ninja -C builddir install
```

Build-infra accepts `/opt/local/mesa-gororoba-debug-optimized` as the
compatibility alias for the canonical debugoptimized prefix
`/opt/mesa-gororoba-debug-optimized`. The shared-prefix list below names both
paths.

Adapt options to the checkout and current Meson option set. Use `meson configure` and repo-local options rather than guessing. Commands and scripts carry repository-relative paths, PATH-resolved tools, or explicit user roots. Discover the repository root in scripts:

```bash
repo_root=$(git rev-parse --show-toplevel)
```

Build audits model Meson defaults: for an omitted or `auto` option, audit the dependencies Meson enables on the target host. An absent option resolves to what Meson will do, not to disabled.

Raw-submit and hazardous probes require exact opt-in values, such as `R300_TRACE_HAZARD_ACCEPTED=1`. Reject unset, empty, and zero-valued gates. Variable presence is not consent.

### Release, debug, and measurement contamination

Release and debug builds keep separate build directories and separate install prefixes, and share no object files, build directories, or install paths. Run `meson setup`, `ninja -C <builddir>`, and `ninja -C <builddir> install` completely for one build before starting the other.

Silicon evidence and conformance work use `buildtype=release`; `debugoptimized` and `debug` builds change timing, allocator behavior, and GL error paths.

Driver RCA and shader disassembly use a separate `buildtype=debug` build at its own prefix.

Run probes against one build by pointing the loader at that build prefix and
Meson-configured library directory:

```bash
mesa_prefix=$(meson introspect <builddir> --buildoptions | jq -r '.[] | select(.name == "prefix").value')
mesa_libdir=$(meson introspect <builddir> --buildoptions | jq -r '.[] | select(.name == "libdir").value')
LIBGL_DRIVERS_PATH="$mesa_prefix/$mesa_libdir/dri" \
LD_LIBRARY_PATH="$mesa_prefix/$mesa_libdir${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
  ./probe_binary
```

For Vulkan probes, point `VK_ICD_FILENAMES` at the ICD JSON this prefix
installed for the configured `vulkan-drivers` Meson option. `ati_r300`
installs `r3v_icd.<cpu>.json`; `amd_terascale` installs
`terascale_icd.<cpu>.json`. Prefer the matching leaf under the prefix rather
than hard-coding one driver:

```bash
mesa_icd_dir="$mesa_prefix/share/vulkan/icd.d"
# Prefer the ICD for the drivers this prefix was configured with.
mesa_icd=$(ls "$mesa_icd_dir"/r3v_icd.*.json \
               "$mesa_icd_dir"/terascale_icd.*.json 2>/dev/null | head -1)
test -n "$mesa_icd" && test -f "$mesa_icd"
```

A release evidence run defines `LIBGL_DRIVERS_PATH` as
`$mesa_prefix/$mesa_libdir/dri`, `LD_LIBRARY_PATH` as
`$mesa_prefix/$mesa_libdir`, and `VK_ICD_FILENAMES` as `$mesa_icd`, or unsets
each variable. A debug DRI driver or stale Vulkan ICD loaded into a release
probe invalidates silicon evidence.

### Build profiles and host envs

The default build profile lives at the top of `build-infra/configs/`; the other
profiles live in `build-infra/configs/alternates/`.  The Makefile resolves a
bare `PROFILE=` name against both directories, so `make` invocations name a
profile by basename regardless of which directory holds it.

Each profile's `.meson` file declares its own drivers, buildtype, and
options; read the config for the enumeration. The facts the configs do
not state: profile `3_r300_full_debug_optimized_*` is the default
(maximal r300 plus the `ati_r300` ICD, debugoptimized, vostro); profile
`4_r300_full_release_*` is the conformance baseline -- GL/GLES/Piglit
and silicon-evidence runs use it because an assertions-live debug build
can abort a case release would pass; profile
`5_r300_full_release_x86_64v1-gcc-distcc-cache` is the GCC diagnostic
profile and pairs with the GCC Vostro env plus `COMPILER_FAMILY=gnu`. Configure
it with:

```sh
make -C build-infra configure PROFILE=5_r300_full_release_x86_64v1-gcc-distcc-cache \
  HOSTENV=vostro1000-x86-64-v1-gcc-ccache-distcc COMPILER_FAMILY=gnu
```

The `terakan_full` release/debug pair serves x130e with
`terakan_norusticl` variants as fallbacks; and
`r300_h264dec_full_debug_*` is a development surface.

Active host envs live in `build-infra/env/`:
`vostro1000-x86-64-v1-clang22-ccache-distcc.env` for the numbered clang
profiles, `vostro1000-x86-64-v1-gcc-ccache-distcc.env` for the GCC profile,
and `generic-x86-64-os.env` for ad hoc portable builds. The GCC profile uses
`COMPILER_FAMILY=gnu`, `COMPILER_CHAIN=ccache`, and a generated gcc/g++
toolchain overlay; its client and distcc volunteers use one matching GCC
major, or `MESA_GCC_VERSION` pins the major on every endpoint. Historical
btver1, sapphire, zen4, and distcc-pump envs live under
`build-infra/env/Archive/` and are not active Make `HOSTENV` values. Active
envs set lane-specific distcc/cache policy, host CFLAGS, `-fno-emulated-tls`,
and centralized `CCACHE_DIR`/`SCCACHE_DIR`. The validated clang lane on Linux
x86_64 requires `-fno-emulated-tls` to avoid a libglapi link failure.

### Build directories and install prefixes

Each build directory maps to one install prefix; directories share no object
files or install paths.

The Makefile derives the canonical build directory from the profile:
`build/mesa-<profile>/`. A plain `make install PROFILE=<profile>` installs to
the isolated default prefix `/opt/local/mesa-<profile>`, which keeps profile
artifacts separate for review, bisect, and evidence work.

The shared active prefixes are only for intentional operator-selected installs:

- release active tree: `/opt/local/mesa-26-gororoba`;
- debugoptimized active tree: `/opt/mesa-gororoba-debug-optimized`, with
  `/opt/local/mesa-gororoba-debug-optimized` as its compatibility alias.

Use the `install-<profile>` targets, or pass `PREFIX=` explicitly, only when the
goal is to replace one of those active trees. Evidence collection keeps each
profile build in its own prefix. Install trees live outside the repository;
an in-repo `install/` directory or suffixed variant such as `install-gallium`
pollutes the worktree and requires separate `LIBGL_DRIVERS_PATH` or
`VK_ICD_FILENAMES` overrides. Project builds leave system Mesa under
`/usr/lib/` undisturbed.

### Clean and reconfigure

Incremental `ninja -C <builddir> clean` removes compiled objects and keeps Meson configuration. A Meson option change or a Meson upgrade requires `meson setup --wipe <builddir>`, which gives a fresh directory setup while preserving download caches. `meson-private/cmd_line.txt` is generated state and changes only through `meson setup` and `meson configure`. After `--wipe`, run `ninja -C <builddir>` and `ninja -C <builddir> install` in full before collecting evidence.

## Build-system and cache discipline

Native files use PATH-resolved compiler names or generated local overlays, and checked-in files name compilers by those forms only. Rust is selected by active Meson/toolchain policy.

Make writes version-coupled LLVM helper tools into `$BUILDDIR/mesa-toolchain.meson` before `meson setup`. The generator prefers the x130e LLVM major when present, honors `MESA_LLVM_VERSION` when set, and otherwise selects an installed coherent `clang`/`clang++`/`llvm-config` major on the host.

C/C++ cache lanes:

- Warm incremental: `ccache -> distcc -> clang`, no pump. Rust:
  `sccache -> rustc`. Use `CCACHE_PREFIX=distcc`; expect hits after a
  populated build.
- Historical distcc-pump envs are archived and have no active Make target. Do
  not revive pump without a new build-system design review because upstream
  removed the supported pump lane during profile consolidation.

Active configure writes:

```ini
[binaries]
c    = ['ccache', '<selected-clang>']
cpp  = ['ccache', '<selected-clang++>']
rust = ['sccache', 'rustc']
llvm-config = '<selected-llvm-config>'
```

Historical pump configure wrote:

```ini
[binaries]
c    = ['distcc', '<selected-clang>']
cpp  = ['distcc', '<selected-clang++>']
rust = ['sccache', 'rustc']
llvm-config = '<selected-llvm-config>'
```

Pump builds run unwrapped by ccache or sccache. Wrapper scripts such as `exec ccache distcc clang "$@"` stay retired: that form causes about 93.5% `Multiple source files` ccache rejections because ccache hashes `distcc`'s mtime as the compiler. Use Meson `[binaries]` for the wrapper boundary and `CCACHE_PREFIX=distcc` for the cache chain.

`RUSTC_WRAPPER` is cargo-only here; it changes neither Meson C/C++ behavior nor Meson Rust selection. The Rust sccache lane remains separate because it wraps `rustc`, not the C/C++ include-server path.

The workspace patched sccache for meson-rust's multi-`--emit` form. Select the patched binary through PATH order or host env, not a baked per-user path.

When cache wiring changes, consult `steinmarder/docs/workspace/ccache-sccache-wiring.md` and `steinmarder/docs/workspace/sccache-multi-emit-patch.md`, then update the reproducible recipe. A cache-miss regression gets a diagnosis before the recipe changes.

Check ccache state with:

```bash
ccache --show-stats --verbose
```

Expected status: a first full build populates `~/.cache/ccache` and shows about 95% misses while filling the cache. Later rebuilds with unchanged sources should show more than 90% hits.
