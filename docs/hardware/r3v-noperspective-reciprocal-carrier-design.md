# R3V NoPerspective reciprocal carrier design

The direct GB W_SELECT route serves one NoPerspective interface shape on
RS482: one full float vec4 at location 0 with no other varying, because
`GB_SELECT.W_SELECT` is one word for the whole draw. Every other
NoPerspective interface is created `R3V_INTERPOLATION_ROUTE_UNSUPPORTED`
and refuses at record time (`r3v_interpolation_lowering.h`). This document
fixes the mechanism that serves those shapes: a per-varying affine value
recovered in the fragment program from perspective-interpolated carriers.

## Mechanism

The RS interpolates a varying lane a with the position's reciprocal clip W
(`VTX_W0_FMT` clear, `r3v_native_cell.c` `project_clip_vertex`):

```text
interp(a) = sum(l_i a_i W_i) / sum(l_i W_i),   W_i = 1 / w_i
```

Storing the premultiplied lane a_i' = a_i w_i and one extra lane c_i = w_i
gives

```text
interp(a') = sum(l_i a_i) / sum(l_i W_i)
interp(c)  = sum(l_i)     / sum(l_i W_i)
affine(a)  = interp(a') / interp(c) = interp(a') * rcp(interp(c))
```

so the fragment program computes one `RCP` of the carrier lane and one
`MUL` per NoPerspective lane, while Smooth lanes in the same draw stay on
their perspective interpolation and `GB_SELECT.W_SELECT` stays clear. The
route is per varying, so it admits mixed Smooth, Flat, and NoPerspective
interfaces and any float width.

## Cell changes

- Record: `record_dwords` grows by the carrier lane set; the CPU delivery
  route (`r300_cpu_vertex_job.c` `STORE_VARYING`, `project_clip_vertex`)
  stores a_i w_i for NoPerspective lanes and w_i in the carrier lane using
  the clip W ahead of the reciprocal, with the same overflow rounding the
  clipper's weights use.
- RS: a second interpolator (`R300_RS_IP_1`, `R300_RS_INST_1`,
  `R300_RS_COUNT`, `R300_VAP_PROG_STREAM_CNTL_*`, `R300_VAP_VTX_SIZE`,
  `R300_VAP_VSM_VTX_ASSM` with `TC1`) carries the w lane when the vec4 is
  full; a vec3 or narrower varying carries w in its own q lane.
- US: a baked program from `r300_tcl_bypass_fs_tool` (NIR:
  `out = in0 * rcp(in1.x)`, or `rcp(in0.w)` for the q-lane carrier), a new
  header beside `r300_r2vb_producer_fs_block.h` with its `--check` meson
  test; `US_CODE_OFFSET` and `US_CODE_ADDR_*` hold 64 ALU slots, so RCP plus
  four MUL fits the existing first-draw contract's register set.
- Kernel: `r300_tcl_bypass_vtx_check` must admit the widened
  `VAP_VTX_SIZE` and the TC1 assembly word; the width transition is a
  separate kernel change with its own replay evidence, as the COLOR0
  admission was.

## Numeric domain

The US ALU is FP24 (s1e7m16). `RCP` on the interpolated w carrier and the
following `MUL` round twice at 2^-16 relative precision; the census
tolerance of two UNORM8 quanta absorbs that for the probe payload, and the
`r300_numeric_domain.c` model states the bound for arbitrary payloads. The
carrier w lane must stay finite and positive on every emitted vertex,
which the clip class ACCEPT guarantees and the clipper's weight rounding
preserves for the partial class, so this route serves partially clipped
triangles as well.

## Validation ladder

1. Host model: `r300_rs_tex_adj_probe` census over the new cell's predicted
   images; the affine model must classify the reciprocal-carrier output on
   every judged pixel with the perspective control unchanged.
2. Shim record tests for each admitted interface shape and each refusal.
3. Kernel replay of the widened cell through the pinned width check.
4. One attended two-pass submission per new register class (TC1 carrier,
   q-lane carrier), judged by the same census, retained in steinmarder-r300.

Until step 4 lands for a shape, that shape stays UNSUPPORTED.
