# clang 22 ccache + distcc r300vk Vostro lane

## Why

r300vk validation on the Vostro needs a repeatable fast lane that uses clang 22,
keeps ccache warm on the slow RS482 host, and sends cache misses to LAN distcc
volunteers.  The ccache and distcc manuals agree on the safe shape:
`ccache clang-22` in Meson, with `CCACHE_PREFIX=distcc` in the environment.
Do not configure Meson as `ccache distcc clang-22`, and do not combine this
incremental lane with distcc-pump.

## What

The canonical incremental release and debug paths are:

```sh
make -C build-infra -j1 configure PROFILE=r300vk-vostro-x86-64-v1-clang22-release HOSTENV=vostro1000-x86-64-v1-clang22-ccache-distcc COMPILER_CHAIN=ccache PREFIX=/usr/local/mesa-26-gororoba
make -C build-infra build PROFILE=r300vk-vostro-x86-64-v1-clang22-release HOSTENV=vostro1000-x86-64-v1-clang22-ccache-distcc COMPILER_CHAIN=ccache PREFIX=/usr/local/mesa-26-gororoba
make -C build-infra install PROFILE=r300vk-vostro-x86-64-v1-clang22-release HOSTENV=vostro1000-x86-64-v1-clang22-ccache-distcc COMPILER_CHAIN=ccache PREFIX=/usr/local/mesa-26-gororoba

make -C build-infra -j1 configure PROFILE=r300vk-vostro-x86-64-v1-clang22-debug HOSTENV=vostro1000-x86-64-v1-clang22-ccache-distcc COMPILER_CHAIN=ccache PREFIX=/usr/local/mesa-26-gororoba-debug
make -C build-infra build PROFILE=r300vk-vostro-x86-64-v1-clang22-debug HOSTENV=vostro1000-x86-64-v1-clang22-ccache-distcc COMPILER_CHAIN=ccache PREFIX=/usr/local/mesa-26-gororoba-debug
make -C build-infra install PROFILE=r300vk-vostro-x86-64-v1-clang22-debug HOSTENV=vostro1000-x86-64-v1-clang22-ccache-distcc COMPILER_CHAIN=ccache PREFIX=/usr/local/mesa-26-gororoba-debug
```

The release prefix is `/usr/local/mesa-26-gororoba`.  The debug prefix is
`/usr/local/mesa-26-gororoba-debug`.  The `rebuild-*` convenience targets first
run `clean`; use them only when the builddir itself needs regeneration.

## How

`configs/r300vk-vostro-x86-64-v1-clang22-*.meson` pins the component set,
x86-64-v1 code generation, `-Os`, section splitting, lld, and relro/now link
hardening.  `env/vostro1000-x86-64-v1-clang22-ccache-distcc.env` pins LLVM 22,
sets `CCACHE_PREFIX=distcc`, probes TCP distccd volunteers, and uses plain
distcc hosts without `,cpp` because `,cpp` belongs to pump mode.

Validated live volunteers on 2026-05-25:

- `ALIENWARE.local/32,lzo`: `/usr/bin/clang` is clang 21.1.8, but
  `/usr/bin/clang-22` and `/usr/bin/clang++-22` are clang 22.1.6; distccd is
  listening on TCP 3632.
- `x570-5600X3D.local/16,lzo`: `/usr/bin/clang` and `/usr/bin/clang-22` are
  clang 22.1.5; ccache and distccd are installed; included only when TCP 3632
  is reachable from the Vostro.

The profile name uses `x86-64-v1` for the psABI baseline.  Clang spells that
baseline as `-march=x86-64`, so the native files use `-march=x86-64` and
`-mtune=generic` instead of a host-native CPU.

Primary references:

- ccache manual: https://ccache.dev/manual/latest.html
- distcc manual, "Using distcc with ccache":
  https://www.distcc.org/man/distcc_1.html
- distcc pump manual: https://www.distcc.org/man/pump_1.html
- clang command guide: https://clang.llvm.org/docs/CommandGuide/clang.html
