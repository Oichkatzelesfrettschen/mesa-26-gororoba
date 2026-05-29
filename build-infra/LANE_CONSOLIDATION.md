# build-infra lane consolidation

Plan to collapse the per-host/per-march build lanes onto the single generic
`gororoba-terakan.meson` + `generic-x86-64-os.env` lane.  The canonical lane and
its `MODE=stable` thin-LTO option already exist; this document is the durable
execution plan for retiring the superseded lanes, derived from a full survey of
`configs/` and `env/`.  The file edits beyond this lane MUST land with a build
verification (a `meson setup` `-O`-level check and one clean build), so the bulk
retirement is staged separately from the already-applied `generic-x86-64-os.env`
correctness fixes.

## Canonical lane

```sh
# default: -Os, no LTO
make build install PROFILE=gororoba-terakan HOSTENV=generic-x86-64-os
# stable: thin-LTO -O2 release
make ... MODE=stable
```

The optimization LEVEL is owned by `gororoba-terakan.meson` (`optimization='s'`
-> -Os); `generic-x86-64-os.env` carries only orthogonal flags (it no longer
hardcodes `-Os`, which would override `MODE=stable`'s `-Doptimization=2` because
the last `-O` on the clang line wins).  Clean vs incremental needs no config
conditional: `meson setup --reconfigure` + ninja's dependency graph handle it;
`meson install --no-rebuild --only-changed` is the incremental install.

## MODE matrix

| MODE | level source | LTO | use |
|---|---|---|---|
| (default) | `optimization='s'` (config) -> -Os | none | daily builds, probes |
| stable | `-Doptimization=2` (CLI) | thin | long-lived install |
| (debug) | `terakan-distcc-no-rusticl-debug.meson` profile | none | debug artifact (separate prefix) |

## Retire (superseded; delete after the lane is verified)

- `env/btver1.env` (fragile `paste` DISTCC_HOSTS; superseded), `env/btver1-ccache-no-pump.env`, `env/btver1-distcc-ccache-warm.env` (its `CCACHE_COMPILERCHECK`/`CCACHE_DEPEND` are now merged into `generic-x86-64-os.env`).
- `env/btver1-distcc-pump.env` and `configs/terakan-distcc-no-rusticl-pump.meson` (distcc-pump is dead: removed upstream, incompatible with ccache).
- `env/cachyos-znver3-cross-btver1.env` and `configs/terakan-cachyos-cross-btver1.meson` (the `-march=btver1` differentiator is dropped; generic x86-64 runs on Bobcat).
- r300 lane consolidated to the two `[12]_r300_full_*_x86_64v1-clang22-distcc-cache` profiles (maximal r300 GL/GLES + amd_r300 ICD).  All prior r300/vostro configs are removed: `r300-canonical-vostro-k8`, `r300-egl-gbm-trace-vostro-k8`, `r300-trace-vostro-k8`, `r300-vostro-k8-{debug,release}`, `r300vk-vostro-k8-{debug,release}`, `r300vk-vostro-x86-64-v1-clang22-{debug,release}`, and the orphaned `vostro1000-k8-*` envs.

## Keep (distinct purpose)

`terakan-distcc-no-rusticl-debug` (debug artifact), `terakan-distcc` + the
Rusticl cross-build lane (Rusticl recovery), `terakan-full` (reference stack),
`terakan-minimal` (NIR scratchpad), `base-debug` (upstream regression),
the two `[12]_r300_full_*` r300 profiles (canonical r300; see
`docs/r300-full-profile-options.md`), the `vostro1000-x86-64-v1-clang22-ccache-distcc`
env (LLVM-22 pin), and the `zen4`/`sapphire` placeholders.

## Makefile follow-ups

- Reroute the canonical PAIR definitions from `terakan-distcc-no-rusticl` +
  `btver1-ccache-no-pump` to `gororoba-terakan` + `generic-x86-64-os`; keep the
  old pair names as aliases until external callers migrate; add a
  `rebuild-gororoba-terakan` target.
- Replace the hardcoded `btver1-ccache-no-pump` / `btver1-distcc-pump` audit
  name-checks with env-content checks (`grep -q 'CCACHE_PREFIX.*distcc'`, and a
  pump check on the absence of `CCACHE_PREFIX`).
- Update `.PHONY` and the JOBS table is already correct (generic falls through to
  `nproc`).

## External references to migrate

steinmarder docs that name the retired lanes:
`docs/mesa-build-orchestration.md`, `docs/workspace/{host-setup,canonical-build-recipe,build-workflow-options,sccache-multi-emit-patch,mesa-fork-synthesis}.md`,
`mesa-rekit/scripts/capture_terakan_context.sh` (hardcoded builddir default), and
the hand-rolled `mesa-rekit/build/terakan/terakan-{clean,incremental}.sh` (either
deprecate in favour of the Makefile or reroute).  Mesa's own `AGENTS.md` profile
listing and `build-infra/README.md` layout section.  Committed evidence bundles
already carry the `<home>` sentinel and need no change.

## Acceptance gate (run before deleting any file)

1. `meson setup` the canonical lane (default and `MODE=stable`) and confirm the
   resolved level: `ninja -C <builddir> -v | grep -m1 ' -O'` shows `-Os` for the
   default and `-O2` for `MODE=stable` (this is the regression the `-Os`-in-CFLAGS
   removal fixes; verify it, do not assume it).
2. One clean build + install on the canonical lane succeeds.
3. Each KEEP config still configures (`meson setup --reconfigure`).
