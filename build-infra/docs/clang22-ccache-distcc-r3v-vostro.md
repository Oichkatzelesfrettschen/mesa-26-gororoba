# clang 22 ccache + distcc r300 Vostro lane

## Why

r300 validation on the Vostro (full GL/GLES plus the ati_r300 ICD) needs a
repeatable fast lane that uses clang 22,
keeps ccache warm on the slow RS482 host, and sends cache misses to LAN distcc
volunteers.  The default lane uses `ccache clang-22` in Meson, with
`CCACHE_PREFIX=distcc` in the environment.  ccache resolves the compiler on the
Vostro before invoking distcc, so volunteers for this lane need the same
versioned compiler path that the Vostro ccache process resolves.  Use
`COMPILER_CHAIN=distcc` when the volunteers must resolve `clang-22` through
their own PATH.  Do not configure Meson as `ccache distcc clang-22`, and do not
combine this incremental lane with distcc-pump.

## What

The canonical incremental release and debug paths are:

```sh
make -C build-infra configure PROFILE=4_r300_full_release_x86_64v1-clang22-distcc-cache HOSTENV=vostro1000-x86-64-v1-clang22-ccache-distcc COMPILER_CHAIN=ccache PREFIX=/opt/local/mesa-26-gororoba
make -C build-infra build PROFILE=4_r300_full_release_x86_64v1-clang22-distcc-cache HOSTENV=vostro1000-x86-64-v1-clang22-ccache-distcc COMPILER_CHAIN=ccache PREFIX=/opt/local/mesa-26-gororoba
make -C build-infra install PROFILE=4_r300_full_release_x86_64v1-clang22-distcc-cache HOSTENV=vostro1000-x86-64-v1-clang22-ccache-distcc COMPILER_CHAIN=ccache PREFIX=/opt/local/mesa-26-gororoba

make -C build-infra configure PROFILE=3_r300_full_debug_optimized_x86_64v1-clang22-distcc-cache HOSTENV=vostro1000-x86-64-v1-clang22-ccache-distcc COMPILER_CHAIN=ccache PREFIX=/opt/local/mesa-gororoba-debug-optimized
make -C build-infra build PROFILE=3_r300_full_debug_optimized_x86_64v1-clang22-distcc-cache HOSTENV=vostro1000-x86-64-v1-clang22-ccache-distcc COMPILER_CHAIN=ccache PREFIX=/opt/local/mesa-gororoba-debug-optimized
make -C build-infra install PROFILE=3_r300_full_debug_optimized_x86_64v1-clang22-distcc-cache HOSTENV=vostro1000-x86-64-v1-clang22-ccache-distcc COMPILER_CHAIN=ccache PREFIX=/opt/local/mesa-gororoba-debug-optimized
```

The build-infra install prefix is `/opt/local/mesa-26-gororoba`.  The debug
prefix is `/opt/local/mesa-gororoba-debug-optimized`.  The package-managed install uses
the FHS-style `/opt/mesa-gororoba` and `/opt/mesa-gororoba-debug-optimized`
prefixes, with `/opt/local/...` compatibility aliases.  The `rebuild-*`
convenience targets first run `clean`; use them only when the builddir itself
needs regeneration.

## How

`configs/alternates/4_r300_full_release_x86_64v1-clang22-distcc-cache.meson` and its `2_`
debug sibling pin the maximal r300 component set, x86-64-v1 code generation,
`-O2` release codegen, `-fno-emulated-tls` for the libglapi clang link, stack
hardening, format-security warnings as errors, and relro link hardening.  The
canonical lane deliberately avoids `-pipe`, LTO, `-fno-plt`, `-march=native`,
and host-specific tuning so artifacts remain portable from K8 through modern
CachyOS and Debian hosts.  `env/vostro1000-x86-64-v1-clang22-ccache-distcc.env` pins LLVM 22,
sets `CCACHE_PREFIX=distcc`, flattens `~/.distcc/hosts` into `DISTCC_HOSTS`,
strips the pump-only `,cpp` flag, normalizes local fallback entries to distcc's
`localhost[/LIMIT]` grammar, and appends `localhost/2` when the host file does
not already name a local fallback.  Direct local builds use `nproc`; the
distcc and ccache-plus-distcc lanes cap Ninja at six aggregate jobs across the
Vostro and its volunteers.  The environment owns `DISTCC_HOSTS` at source
time, so login-shell defaults cannot silently route this lane to stale
workers.  The environment pins `PATH` and `CCACHE_PATH` to `/usr/bin` first so
ccache resolves
the packaged Clang 22 tools on the Vostro.  The generated Meson toolchain
overlay keeps compiler entries as versioned command names for the ccache and
distcc lanes.  In the direct distcc lane, volunteers search their own PATH for
`clang-22` and `clang++-22`.  In the ccache lane, ccache resolves those command
names on the Vostro and prefixes cache misses with distcc, so volunteers must
provide the same resolved compiler path.  The local `direct` lane uses absolute
compiler paths, and the generated LLVM utility entries use resolved local tool
paths.  The Makefile passes the sourced cache/distcc environment explicitly
through `flock` to compiler-bearing Ninja recipes so the compiler search path
used by ccache matches the configured lane.

Mode matrix:

- Default incremental lane: Meson uses `['ccache', 'clang-22']` and
  `['ccache', 'clang++-22']`, with `CCACHE_PREFIX=distcc`.  ccache hashes on the
  Vostro client; cache misses go to the live `DISTCC_HOSTS` mesh with the
  compiler path ccache resolved on the client.
- Direct distcc fallback: Meson uses `['distcc', 'clang-22']` and
  `['distcc', 'clang++-22']`, with `CCACHE_PREFIX` unset.  Volunteers resolve
  the versioned compiler name through their own PATH.  This is for clean builds
  where cache hashing is not the limiting cost or for mixed hosts where
  `/usr/bin/clang-22` is not a stable path.
- Local tertiary fallback: the Vostro env appends `localhost/2` unless the host
  file already names a local fallback; if the host file is absent or empty, the
  env emits only `localhost/2`.
- Pump lane: pump mode is a no-cache lane.  Use the pump-specific envs that
  unset `CCACHE_PREFIX` and invoke `distcc-pump`; do not combine pump with
  ccache.

The profile name uses `x86-64-v1` for the psABI baseline.  Clang spells that
baseline as `-march=x86-64`, so the native files use `-march=x86-64` and
`-mtune=generic` instead of a host-native CPU.

Primary references:

- ccache manual: https://ccache.dev/manual/latest.html
- distcc manual, "Using distcc with ccache":
  https://www.distcc.org/man/distcc_1.html
- distcc pump manual: https://www.distcc.org/man/pump_1.html
- clang command guide: https://clang.llvm.org/docs/CommandGuide/clang.html
