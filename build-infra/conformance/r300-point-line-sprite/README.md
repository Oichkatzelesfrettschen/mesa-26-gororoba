# r300 point/line/sprite conformance regression harness

A deqp-GLES2 regression gate for the RS480/RS482 point, line, and sprite work.
These fixes (per-vertex point size through the gallium draw wide-point stage, the
aliased-line-width clamp, gl_PointCoord, the point-size cap) each pass on real
silicon but ride the shared draw module and the GA rasterizer setup, so they are
easy to regress from an unrelated change. This harness pins the validated
per-case verdicts and fails CI on any `Pass -> not Pass` regression.

## Run

The harness runs on the host that has the RS482 GPU and the surfaceless
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
GLES2 `gl_PointCoord` case, and two regression sentinels
(`vertex_arrays.single_attribute.first`, `shaders.matrix.mul.dynamic`) that share
the draw/VS path and must stay clean.

`baseline.tsv` is `STATUS<TAB>case`, sorted by case. `--check` diffs the live run
against it and classifies:

- **REGRESSION** -- baseline `Pass`, now not `Pass` (fail, crash, or not run). Exit 1.
- **PROGRESSION** -- baseline not `Pass`, now `Pass`. Reported (update the baseline).
- **NEW / GONE** -- case present in only one set (deqp version drift).

A crash aborts a whole deqp group; the missing cases then read as `MISSING` in
the diff, so a crash surfaces as a batch of regressions rather than passing
silently.

## Baseline provenance

The committed baseline is recorded on the validated build (mesa-gororoba-debug
`2:26.2.0-22`, the per-vertex-point-size fix): point size correct, no point
crashes, `clipping.point` and the point/random draws clean. Known-fail entries
in the baseline are the conformance gaps tracked separately: the `*_wide` lines
(addressed opt-in by `r300_clamp_max_line_width`), `limits.points`
(`clamp_max_point_size`, pending), and `shaders.builtin_variable.pointcoord`
(gl_PointCoord draw-module buildout, pending). Re-`--record` only when a fix
intentionally moves one of those, and note which case changed.
