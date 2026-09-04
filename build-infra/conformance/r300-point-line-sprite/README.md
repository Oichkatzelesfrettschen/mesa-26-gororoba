# r300 point/line/sprite conformance regression harness

A deqp-GLES2 regression gate for the RS485M point, line, and sprite work.
These fixes (per-vertex point size through the gallium draw wide-point stage, the
aliased-line-width clamp, gl_PointCoord, the point-size cap) each pass on real
silicon but ride the shared draw module and the GA rasterizer setup, so they are
easy to regress from an unrelated change. This harness pins the validated
per-case verdicts and fails CI on any `Pass -> not Pass` regression.

## Run

The harness runs on the host that has the RS485M GPU and the surfaceless
`deqp-gles2` build (the Vostro). It exercises whatever driver the system loader
resolves (`/usr/lib/dri/r300_dri.so`); install the build under test first.

```sh
# from the repo, on the GPU host:
build-infra/conformance/r300-point-line-sprite/run.sh --check    # gate (exit 1 on regression)
build-infra/conformance/r300-point-line-sprite/run.sh --record   # re-baseline after an intended change
```

Overrides: `DEQP=/path/to/deqp-gles2`, `OUT=/var/tmp/dir`,
`BASELINE=/path/to/baseline.tsv`. To test a scoped `/opt` prefix instead of the
system driver, export `LIBGL_DRIVERS_PATH`/`LD_LIBRARY_PATH` before calling
(the harness clears them otherwise so the system driver is the default subject).

The deqp surface config is pinned to `pbuffer` + `rgba8888d24s8`; the
auto-selected config and the `fbo` surface type both produce 100% false fails on
r300 (RGB565 alpha mismatch / GLES3 depth-stencil enum), so do not change them.

## What it gates

`caselist.txt` is the domain: rasterization point/line/triangle (incl. wide and
`limits.points`), point + line clipping, point/line/random draws, the single
GLES2 `gl_PointCoord` case, the blitter HW point-sprite
(`texture.mipmap.2d.generate`, which drives `glGenerateMipmap` through the
`r300_draw_rectangle` `UTIL_BLITTER_ATTRIB_TEXCOORD_XY` point-sprite path), and two
regression sentinels (`vertex_arrays.single_attribute.first`,
`shaders.matrix.mul.dynamic`) that share the draw/VS path and must stay clean.

The blitter mipmap group gates the other half of the `gl_PointCoord` work: the
draw-varying sprite path sets `point_sprite_via_draw`, and `r300_draw_rectangle`
clears that flag before its derived-state rebuild so the blitter keeps emitting its
own HW point sprite -- a leak of the flag into the blitter surfaces as a mipmap
regression here rather than only as mass clear-path collateral.

`baseline.tsv` is `STATUS<TAB>case`, sorted by case. `--check` diffs the live run
against it and classifies:

- **REGRESSION** -- baseline `Pass`, now not `Pass` (fail, crash, or not run). Exit 1.
- **PROGRESSION** -- baseline not `Pass`, now `Pass`. Reported (update the baseline).
- **NEW / GONE** -- case present in only one set (deqp version drift).

A crash aborts a whole deqp group; the missing cases then read as `MISSING` in
the diff, so a crash surfaces as a batch of regressions rather than passing
silently.

## Baseline provenance

The committed baseline is recorded on the validated build with the conformant
point/line rasterization limit (`max_point_size` = 64, `max_line_width` = 4) and
the `gl_PointSize` clamp (`nir_lower_point_size`): point size correct and clamped,
no point crashes, `clipping.point` and the point/random draws clean. There are no
`Fail` entries -- `limits.points` and `shaders.builtin_variable.pointcoord` now
`Pass`, and the `*_wide` line cases (interpolation, primitives, clipping) report
`NotSupported` because the driver advertises only the width-4 aliased line range
the GA quad-expansion covers conformantly rather than a wide range it cannot. The
32 `texture.mipmap.2d.generate` blitter cases all `Pass` (260 cases total: 243
`Pass`, 17 `NotSupported`, no `Fail`).

Recorded against the validated current-main build in the meson builddir via the
`LIBGL_DRIVERS_PATH` override, not the installed system driver -- the system
driver on a host can lag current main (e.g. before the `max_line_width` clamp the
`*_wide` cases `Pass`/`Fail` instead of `NotSupported`, and `limits.points`
regresses), which would record a stale baseline. Re-`--record` against the build
under test (override or freshly installed), and re-`--record` only when a fix
intentionally moves a case; note which changed.
