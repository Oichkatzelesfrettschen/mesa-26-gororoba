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
class the direct W_SELECT route refuses. R3V packs ahead of the clipper,
and the clipper blends the premultiplied payload and the carrier lane with
one clip-space edge parameter t, so a generated vertex's payload / carrier
is `((1 - t) w_a a + t w_b b) / ((1 - t) w_a + t w_b)`, the value the
Vulkan specification assigns a clipped NoPerspective output (Clipping
Shader Outputs: t' = t w_b / ((1 - t) w_a + t w_b)). A clipped carrier
vertex is a convex combination of source records, so its carrier lane
stays inside (0, 1] and its payload inside the envelope;
`r300_noperspective_reciprocal_validate_expanded` asserts that over the
published fan and skips the clipper's padding records. The host proof is
the public-surface harness's unequal-w one-plane triangle (w 1 past
x = -w, w 2 inside; both crossings at t = 1/2 exactly): the fan of two
triangles carries three source and three generated records, carrier lane
3/4 at each generated vertex, and the FP24 recovery model lands within one
UNORM8 quantum of the clipped-edge value. The PM4 stream is byte-identical
to rung A's cell, so the kernel replay of rung A stands for rung B; the
silicon receipt (`r3v_native_attended_rs_tex_adj_probe --candidate
reciprocal-carrier-partial`, vertex 0 at NDC x = -1.5, census 2 px inside
the target border) is
`steinmarder-r300/src/re/r300/results/r3v-native-noperspective-partial-clip-carrier-receipt-rs482`:
affine 1296/1296, control perspective 1296/1296, both fan witnesses live 6
exact 6.

Rung C, q-lane carrier: a float, vec2, or vec3 NoPerspective varying at
location 0 with a contiguous mask from x leaves TEX0.w free, so the
post-VS stage packs `a.xyz * c` into the leading lanes, 0 into the lanes
past the width, and `c = w / max(w)` into w, and the US recovers
`xyz * rcp(w)` with alpha 1.0 (`r300_noperspective_q_lane_plan.h`,
`r300_noperspective_q_lane_fs_block.h`). The record and every register
word are the varying cell's, so the cell differs from the control in its
US program alone and the kernel width check judges VAP_VTX_SIZE 8 on both
draws (PASS at 8, REJECT at 4; CS tracking ACCEPT with every control). The
route `R3V_INTERPOLATION_ROUTE_RECIPROCAL_Q_LANE` opens with no gate when
the fragment module is the narrow pass-through of the same width -- the
varying's lanes zero-filled with alpha 1, the program that binary
executes -- on CPU delivery over a triangle list with the RS destination
consumed; a component offset, a width mismatch, a vec4 under the narrow
program, and the narrow program on a Smooth or Flat interface are
UNSUPPORTED. The packing precedes the clipper, so every clipping class is
admitted under rung B's argument and
`r300_noperspective_q_lane_validate_expanded` asserts the fan. The host
proof is the public-surface harness over the three widths: the recorded
stream byte-equal to the q-lane family cell, an unequal-w ACCEPT triangle
packed to c = 0.25, 0.5, 1 with zero-filled lanes, and the one-plane
unequal-w crossing clipped into a six-record fan at the clipped-edge
values. The silicon discriminator is the vec3 case
(`r3v_native_attended_rs_tex_adj_probe --candidate reciprocal-q-lane`):
the probe attribute's s, t, r ride the varying, the census judges the
candidate against the logical records (s, t, r, 1), and the oracle
requires affine within one quantum on every judged pixel, each of the
three channels separating the models on its own, perspective and
unchanged at zero, and alpha exactly 255. Vec1 and vec2 share that
receipt: their PM4 and US program are byte-identical to the vec3 cell's
(the plan's words are width-independent) and they differ only in the
lanes the packer zero-fills, which the harness proves per width.

Rung D, mixed Smooth/NoPerspective carrier: the first admitted mixed
shape is a Smooth float vec4 at location 0 beside a NoPerspective float
vec4 at location 1 with no Flat location. The vertex job stores both
locations (`R300_VERTEX_JOB_OP_STORE_VARYING` takes the location in
`dst`, so the record is twelve dwords), and the post-VS stage packs each
triangle into the sixteen-dword mixed shape ahead of the clipper: TC0 the
Smooth vector verbatim, TC1 the NoPerspective vector premultiplied by
`c = w / max(w)`, TC2 `(c, 0, 0, 1)`
(`r300_noperspective_mixed_carrier_plan.h`). The register contract is
the reciprocal plan at two payload vectors plus the carrier -- VAP_VTX_SIZE
16, RS_IP/RS_INST 0..2, RS_COUNT twelve components -- with
`GB_SELECT.W_SELECT` 0 for the whole draw, and the US block stores
`(TC0.x, TC0.y, r.x, r.y)` with `r = TC1 * rcp(TC2.x)` (four ALU
instructions, three temporaries;
`r300_noperspective_mixed_carrier_fs_block.h`), so one target exposes two
Smooth lanes that stay perspective beside two NoPerspective lanes that
become affine. The plan refuses more than four RS vectors including the
carrier, a carrier aliasing a payload vector, a premultiplied set other
than vector 1, and a US program past the R300 budget of 64 ALU
instructions or 32 temporaries; the stream check names VTX_SIZE 12,
W_SELECT 1, and a wrong RS_IP or RS_INST of any vector. The kernel width
check judges the widened record without a kernel change: control PASS at
8, mixed cell PASS at 16, VTX_SIZE 12 REJECTs the mixed draw, CS tracking
ACCEPT with every control. The route
`R3V_INTERPOLATION_ROUTE_MIXED_RECIPROCAL_CARRIER` opens with no gate on
exactly that interface under the mixed lane program `(loc0.xy, loc1.xy)`
on CPU delivery over a triangle list with the RS destinations consumed;
reordered locations, both locations Smooth or both NoPerspective, Flat
mixed in, a third location, a width mismatch, a component offset, an
integer varying, and the mixed interface under the pass-through program
are UNSUPPORTED, and an open R2VB delivery gate withholds the CPU post-VS
mechanism. The published record width is route-derived
(`r3v_interpolation_published_record_dwords`), so the carrier
allocation, the staging capacity, and the clipper -- widened to sixteen
dwords, admitting 4, 8, 12, and 16 -- take the route's width. The host
proof is the public-surface harness: the recorded stream byte-equal to the
mixed family cell, the unequal-w ACCEPT triangle packed to c = 0.25, 0.5,
1 with TC0 verbatim and TC1 premultiplied, and the one-plane crossing
clipped into three source and three generated records whose TC0 is the
clip-space blend and whose TC1 / c is the Vulkan clipped NoPerspective
value. The silicon discriminator
(`r3v_native_attended_rs_tex_adj_probe --candidate mixed-reciprocal-carrier`)
stores the probe attribute to both locations, so red and green carry
(s, t) perspective and blue and alpha carry (s, t) affine over the same
source values; the census judges the candidate against the logical
records (s, t, s, t) one channel at a time, and the oracle requires red
and green perspective-exact within one quantum with affine matching none
of their separated pixels, blue and alpha affine-exact with perspective
matching none, every channel separated (501, 882, 501, 882 of 882 in the
prediction), and no unchanged or sentinel pixel; the oracle is calibrated
ahead of the ioctl against the mixed prediction (holds) and the pure
perspective and affine predictions (fail). Flat beside the mixed pair
stays a later shape.

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
