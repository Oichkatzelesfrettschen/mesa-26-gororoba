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
|   |-- terakan-distcc.meson       # r600 only, rusticl r600, daily lane
|   |-- terakan-minimal.meson      # r600 only, no HUD, NIR scratchpad
|   +-- base-debug.meson           # upstream Mesa reference, no terakan
+-- env/
    |-- btver1.env                 # x130e (Bobcat) clang-22 + distcc
    |-- sapphire.env               # Apple Silicon (placeholder)
    +-- zen4.env                   # AMD Ryzen (placeholder)
```

Build outputs land OUTSIDE the source tree, at
`../../build/mesa-<profile>/`, so `git clean -xdf` in gororoba
does not nuke ongoing builds.

Install prefixes default to `/usr/local/mesa-<profile>/` so
multiple profiles can coexist on a test host.

## Common flows

Daily terakan iteration on x130e:
```
make rebuild-terakan-distcc
sudo make install PROFILE=terakan-distcc
```

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

Full reset of a profile (nukes builddir AND install prefix):
```
make distclean PROFILE=terakan-distcc
```

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
