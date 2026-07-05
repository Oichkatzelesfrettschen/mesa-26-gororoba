# build-infra lane consolidation

Historical record of the lane consolidation from multiple per-host/-march build
directories to the current six canonical numbered profiles.

## Completed consolidation

All prior ad-hoc configs are removed.  The canonical set is:

| Profile | Target | Type |
|---|---|---|
| `4_r300_full_release_x86_64v1-clang22-distcc-cache` | vostro (r300) | release |
| `3_r300_full_debug_optimized_x86_64v1-clang22-distcc-cache` | vostro (r300) | debug |
| `3_terakan_full_release_x86_64v1-clang22-distcc-cache` | x130e (r600) | release + Rusticl |
| `4_terakan_full_debug_x86_64v1-clang22-distcc-cache` | x130e (r600) | debug + Rusticl |
| `5_terakan_norusticl_release_x86_64v1-clang22-distcc-cache` | x130e (r600) | release, no Rusticl |
| `6_terakan_norusticl_debug_x86_64v1-clang22-distcc-cache` | x130e (r600) | debug, no Rusticl |

All six use `HOSTENV=vostro1000-x86-64-v1-clang22-ccache-distcc`.

## Removed configs

The following configs were deleted as part of this consolidation:

- `gororoba-terakan.meson` + `generic-x86-64-os.env` generic lane (superseded by numbered profiles)
- `terakan-distcc-no-rusticl.meson` warm daily lane (folded into 5_/6_)
- `terakan-distcc-no-rusticl-debug.meson` debug artifact (folded into 4_/6_)
- `terakan-distcc-no-rusticl-pump.meson` distcc-pump lane (distcc-pump removed upstream)
- `terakan-distcc.meson` Rusticl recovery lane (folded into 3_/4_)
- `terakan-full.meson` reference stack (folded into 3_/4_)
- `terakan-minimal.meson` NIR scratchpad (no longer needed as a separate profile)
- `base-debug.meson` upstream regression reference (no longer needed as a separate profile)
- `r300-canonical-vostro-x86-64-v1-clang22-release.meson` (superseded by 1_/2_)
- `terakan-cachyos-cross-btver1.meson` `-march=btver1` cross-build (dropped; generic x86-64 runs on Bobcat)
- `terakan-cachyos-cross-rusticl-x86-64-host-glibc-baseline.meson` cross Rusticl (dropped)

## r300 lane history

The r300 lane was consolidated to the two `[12]_r300_full_*` profiles (maximal
r300 GL/GLES + ati_r300 ICD).  All prior r300/vostro configs removed:
`r300-canonical-vostro-k8`, `r300-egl-gbm-trace-vostro-k8`, `r300-trace-vostro-k8`,
`r300-vostro-k8-{debug,release}`, `r3v-vostro-k8-{debug,release}`,
`r3v-vostro-x86-64-v1-clang22-{debug,release}`, and the orphaned
`vostro1000-k8-*` envs.
