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

Storing the premultiplied lane b_i = a_i w_i and one shared lane c_i = w_i
gives

```text
interp(b) = sum(l_i a_i) / sum(l_i W_i)
interp(c) = sum(l_i)     / sum(l_i W_i)
affine(a) = interp(b) / interp(c) = interp(b) * rcp(interp(c))
```

so the fragment program computes one `RCP` of the shared carrier lane and
one `MUL` per NoPerspective vector, while Smooth lanes in the same draw stay
on their perspective interpolation and `GB_SELECT.W_SELECT` stays clear for
the whole draw. The route is per varying, so it admits mixed Smooth, Flat,
and NoPerspective interfaces and any float width.

The identity survives clipping: the clipper interpolates records linearly
in clip space, and linear clip-space interpolation of `(a w, w)` yields a
generated vertex whose b/c equals the framebuffer-linear value the Vulkan
NoPerspective rule requires at that vertex.

## Normalization

Every source triangle scales its carrier w by the common factor
s = 1 / max_i(w_i) before packing: c_i = s w_i, b_i = a_i s w_i. The
factor cancels in b/c, pins the largest carrier w of the triangle at one,
and bounds the premultiplied lanes by max|a_i|. Admission bounds, checked
per triangle ahead of any write:

- every varying value finite;
- every clip w positive and finite (clip class ACCEPT, or a partial class
  whose generated vertices keep positive w);
- max(w) / min(w) within the FP24 RCP envelope, so the interpolated
  carrier stays away from the s1e7m16 denormal floor;
- premultiplied lanes within the FP24 carrier envelope;
- RS interpolator count: `payload vectors + shared w vector <= admitted
  RS vector budget` (R300 exposes four `RS_IP` vectors; one shared w
  vector serves every NoPerspective varying in the draw, never one per
  varying);
- US temporary and instruction count within the 64-slot ALU program.

## Ownership

Vulkan interpolation qualifiers belong to R3V. The neutral CPU vertex job
(`r300_cpu_vertex_job.c`) stays API-neutral and unchanged. The stage order
on the CPU delivery route is

```text
neutral CPU vertex output
-> R3V Flat replication            (r3v_post_vs_lowering.c)
-> R3V NoPerspective carrier packing (r3v_post_vs_lowering.c)
-> clip-space polygon clipping
-> viewport projection
-> common R300 carrier emission
```

`r3v_post_vs_lowering` gains the NoPerspective mask and the packing
metadata (carrier vector index or q-lane placement). The common directory
`src/amd/r300/common/` owns the API-neutral hardware plan:
`r300_noperspective_reciprocal_plan.*` (RS/VAP/US register plan and stream
validator), the generated RCP+MUL fragment block, the host numerical
model, and the manifest writer.

## Kernel admission

`r300_tcl_bypass_vtx_check` in `linux-radeon-gororoba` already models a
position vector plus multiple texture-coordinate vectors; its calibrated
shape was position plus two texture vectors at `VAP_VTX_SIZE = 12`, which
is exactly the TC0+TC1 carrier record. The generated cell therefore
replays against the installed 0.8.12 checker first and requires

```text
r300_tcl_bypass_vtx_check: PASS
r100_cs_track_check:       ACCEPT
```

A kernel change follows only from a replay that names a real unsupported
premise; the change extends that named boundary alone and deploys through
`radeon-custom` with the signed-source, production-package, reboot, and
control-replay procedure.

## Implementation rungs

Rung A, full-vec4 TC1 carrier: record of position (4), TC0 = a w (4),
TC1 = normalized w in x with the remaining lanes fixed (4), `VAP_VTX_SIZE`
12, `RS_IP_0`/`RS_IP_1`, `RS_INST_0`/`RS_INST_1`, `RS_INST_COUNT` two, US
`rcp(TC1.x)` then `TC0 * reciprocal`. A temporary force-carrier gate routes
the existing unclipped full-vec4 probe through the carrier instead of the
direct W_SELECT route so the target compares against the retained affine
W_SELECT target; that isolates TC1, the widened record, the second
interpolator, and the US program from clipping.

Rung B, partial clipping: the reciprocal carrier serves the partial clip
class the direct W_SELECT route refuses. Generated carrier vertices stay
finite, each generated (a w, w) pair reconstructs the framebuffer-linear
edge value, the safe-interior affine census is complete with zero
perspective matches, and fan size and carrier writes stay bounded.

Rung C, q-lane carrier: vec1 through vec3 place normalized w in the unused
q lane, `rcp(input0.w)` then `input0.xyz * reciprocal`, keeping the
eight-dword position + TEX0 record and one interpolator.

Rung D, mixed interfaces: Smooth stays ordinary perspective TEX input,
NoPerspective takes premultiplied input plus the shared w vector, Flat
replicates (direct GA stays an independent optimization), and
`GB_SELECT.W_SELECT` is zero for the whole draw. An interface past the RS
or US capacity refuses.

## Cross-repository closure

- `mesa-26-gororoba`: the four rungs, exact emitters, mutation suites,
  parser-replay manifests, the installed ICD package, attended runners.
- `linux-radeon-gororoba`: each manifest replays through the current
  checker first; the checker changes only for an observed decline.
- `radeon-custom`: packages and reboots only when `linux-radeon-gororoba`
  changes behavior.
- `steinmarder-r300`: one receipt per rung -- forced TC1 carrier,
  partial-clip carrier, q-lane carrier, mixed Smooth/NoPerspective
  carrier.

## Validation ladder

1. Host model: `r300_rs_tex_adj_probe` census over the new cell's predicted
   images; the affine model must classify the reciprocal-carrier output on
   every judged pixel with the perspective control unchanged.
2. Shim record tests for each admitted interface shape and each refusal.
3. Kernel replay of the widened cell through the pinned width check.
4. One attended submission per rung, judged by the same census, retained
   in steinmarder-r300.

Until step 4 lands for a shape, that shape stays UNSUPPORTED.
