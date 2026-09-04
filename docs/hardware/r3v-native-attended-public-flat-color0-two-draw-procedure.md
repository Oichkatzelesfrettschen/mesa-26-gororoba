# R3V native attended public direct GA Flat color-0 two-draw cell procedure

The public direct GA Flat color-0 two-draw cell carries a Vulkan `Flat` RGBA varying from the
application-shaped surface to RS485M silicon through the direct hardware route: the varying rides the
TCL-bypass color 0 vector instead of TEX0, and the GA selects the provoking vertex's color per
primitive (`r3v_interpolation_lowering.h`, `r300_flat_color0_plan.h`).  Two render passes each draw
the reference triangle in a distinct vertex order under one pipeline, so the provoking vertex -- the
first, under the Vulkan core provoking-vertex rule -- differs between the passes, and each pass's
own first-draw contract establishes the GA and RS state ahead of its draw.  The expected targets
come from the CPU replication oracle (`r3v_post_vs_lower_triangles` over the same records, then the
analytic coverage), retained ahead of the submission; the receipt is byte equality of each target
with its expected image, because the direct and the replication command streams necessarily differ
in the varying's carrier lane and the interpolation state they establish.

`docs/hardware/r3v-native-attended-cell-procedure.md` carries the boundary statement, host
preconditions, substrate admission table, identity freeze, arming conjunction, rollback rules, and
retained-record layout; `docs/hardware/r3v-native-attended-public-flat-two-draw-procedure.md`
carries the replication route over TEX0 this cell contrasts with, and its retained receipt (bundle
`r3v-native-public-flat-two-draw-first-delivery-rs482` in `steinmarder-r300`).  This document adds
only what the direct color-0 route changes.

## Cell identity

- Runner: `r3v_native_attended_public_flat_color0_two_draw <dir> [--record-only] [--waiver <path>]`
  (native build); the evidence directory is the required argument. `--record-only` records the
  command buffer and stops at the recording boundary after the digest and interface checks, printing
  `record: ACCEPTED`; the recording contract is calibrated on the drm-shim under the record test
  `r3v-native-public-flat-color0-two-draw-record`.
- Arming digest source: `r3v_native_arming_runner --multi-pass-public-flat-color0 <dir>` emits
  `r3v_native_multi_pass_public_flat_color0_reference` through
  `r300_tcl_bypass_triangle_clip_space_multi_pass_emit`: 452 IB dwords, cell blake3 `3646c222`. The
  replication precedent (`--multi-pass-public-flat`) emits 472 dwords, blake3 `8a0c4f37`, retained
  as bundle `r3v-native-public-flat-two-draw-first-delivery-rs482` in `steinmarder-r300`. The
  direct and the replication streams necessarily differ -- the varying moves from TEX0 into color 0
  and the interpolation state moves into each draw's own contract -- so the oracle judged here is
  target-byte equality against the replication-derived expected images, never stream equality
  between the two routes.
- Interface record: blake3 `096d66a9`, the same link the replication receipt pins --
  `varying_mask=0x1 flat_mask=0x1 post_vs.flat_mask=0x1 provoking=0` (FIRST) -- with
  `interpolation_route` printed as `direct-ga-color0` rather than `replicate`. The runner refuses
  ahead of any ioctl unless the route is exactly `R3V_INTERPOLATION_ROUTE_DIRECT_GA_COLOR0`.
- Mechanism: the varying rides the TCL-bypass color 0 vector (PSC `DST_VEC 2`,
  `VAP_OUTPUT_VTX_FMT_0.COLOR_0_PRESENT`, `VAP_VSM_VTX_ASSM` carrying `POS | COLOR`). Each draw's
  first-draw contract programs `GA_COLOR_CONTROL` `0x0000aaa5` (RGB0 FLAT, ALPHA0 FLAT,
  `PROVOKING_VERTEX_FIRST`, over the contract's base word `0x0003aaaa`), `RS_COUNT` `0x00040080`
  (`IC_COUNT(1) | HIRES_EN`), `RS_IP_0` `0x00000000` (color pointer 0, RGBA), `RS_INST_0`
  `0x00004000` (`COL_CN_WRITE`, `COL_ADDR(0)`), and `RS_INST_COUNT` 0. The pass-through fragment
  binary is unchanged, because color 0 lands in US input 0, the register TEX0 landed in on the
  replication route. The CPU route keeps three distinct carrier values when the triangle's clipping
  class is ACCEPT; a partially clipped triangle keeps replication, because the clipper's fan
  vertices differ from the source's and hardware provoking selection cannot recover the source
  primitive's provoking value. `r3v_interpolation_lowering.h` states the eligibility predicate:

  ```text
    direct GA Flat allowed iff
      delivery route is CPU
      and the primitive is emitted as a triangle list
      and clipping class is ACCEPT
      and the Flat location maps completely to an admitted color lane
      and the required RS destination is available
      and the fragment program consumes that destination
      and provoking FIRST is representable
    otherwise use provoking-value replication
  ```

- Eligibility pin: `R3V_NATIVE_FLAT_REPLICATION_PINNED=1` pins the replication route (the retained
  replication runner sets it); this runner refuses at start if the gate is open, since the pin names
  a stream this authorization does not cover.
- Vertex module `r3v_reference_vertex_flat_rgba`: `tint = (2x, 2y, 0, 0.55 + 0.5333333x)`. Over A
  `(-0.75, -0.75)`, B `(0.75, -0.75)`, C `(0, 0.75)`, the packed UNORM8 dwords are A `0x26000000`
  (alpha `38.25` rounds to `38`), B `0xf2ff0000` (`242.25` to `242`), C `0x8c00ff00` (`140.25` to
  `140`); every predicted alpha sits a quarter step from the nearest UNORM8 boundary, so
  round-to-nearest and truncation agree. Pass 1 draws B, C, A (provoking B, interior `0xf2ff0000`);
  pass 2 draws C, A, B (provoking C, interior `0x8c00ff00`); the exterior sentinel `0xa5a5a5a5`
  seeds both targets before submission.
- Expected targets: generated ahead of submission through the CPU replication oracle
  (`r3v_post_vs_lower_triangles` over the same records, packed, then
  `r300_tcl_bypass_triangle_expected_target`) and retained as `first_expected.bin` /
  `second_expected.bin`, blake3 `c652ab6a` / `7893b7bf`. The receipt is
  `r300_tcl_bypass_triangle_target_compare == 0` on both, plus the coverage oracles
  (`coverage_exact`, `canary`, `interior=1152`, `analytic=1152`, `exterior=2944`) and the
  non-provoking falsifier oracles reading `interior=0`.

## Declarations

- `R3V_NATIVE_SUBMIT_HAZARD_ACCEPTED=1`
- `R3V_NATIVE_AUTHORIZED_IB_BLAKE3=<digest r3v_native_arming_runner --multi-pass-public-flat-color0
  reports>`
- `R3V_NATIVE_AUTHORIZED_KERNEL_RELEASE=<uname -r>`
- `R3V_NATIVE_AUTHORIZED_MODULE_SRCVERSION=<loaded radeon srcversion>`
- `R3V_NATIVE_MANIFEST_DIR=<fresh evidence directory>`

`R3V_NATIVE_FLAT_REPLICATION_PINNED` stays unset: the runner refuses ahead of the token when it is
open, because pinning names the replication route, not this cell's direct route.  Every
`R3V_NATIVE_R2VB_*_EXPERIMENTAL` gate stays unset: the runner refuses ahead of the token when one is
open, because the cell runs on the CPU delivery route alone, with the Flat interpolation moved to
hardware selection over color 0.

## Arming

1. `r3v_native_attended_public_flat_color0_two_draw "$dir" --record-only` on the host build, and
   require `record: ACCEPTED` after a `[record]` line whose recorded and emitted digests agree, an
   `[interface]` line reading `route=direct-ga-color0` with `flat_mask=0x1`, and a `[state]` line
   reading `direct plan registers established ahead of 2 draw(s)`.
2. `r3v_native_arming_runner --multi-pass-public-flat-color0 "$dir"` with the declarations unset;
   retain the closed report.
3. Export the declarations from that report.
4. Rerun the arming runner and require `verdict: armed`.
5. Record the dmesg window, then run `r3v_native_attended_public_flat_color0_two_draw "$dir"`.

## Predictions

If the direct GA route delivers: the `[record]` line reports the recorded and emitted digests equal,
`kind=TRIANGLE_MULTI_PASS`, `references=4`, `deferred_draws=2`, `ib_dwords=452`; the `[state]` line
reports the direct plan established ahead of 2 draws; the first target under `0xf2ff0000` and the
second under `0x8c00ff00` each report `coverage_exact=1 canary=1 interior=1152 analytic=1152
exterior=2944 mismatch=0`; every `under-non-provoking` line reports `interior=0`; each
`-vs-expected` line reports `judged=1 differing=0 alpha_deviates=0`; the second centroid reads
`0x8c00ff00`; `vkQueueSubmit` returns 0; the dmesg delta is zero; the boot id is unchanged; the
watchdog reads `inactive` afterward; and `[classify]` reads the byte-equal verdict naming each
target's own pass's first-vertex RGBA and no other value.

## Kernel parser scope

The kernel's pinned TCL-bypass width check
(`r300_tcl_bypass_vtx_check`, `drivers/gpu/drm/radeon/
r300_tcl_bypass_vtx_check.h`) admits `VAP_OUTPUT_VTX_FMT_0` equal to
`POS_PRESENT` or `POS_PRESENT | COLOR_0_PRESENT`, requiring four dwords
for position, four more for color 0, and the decoded texture-coordinate
widths, so the direct stream's eight-dword record returns `PASS` on both
draws and a `VAP_VTX_SIZE` of seven with color 0 present rejects.  The
first pinned check admitted position alone, declined the direct stream
on both draws (`fmt0_beyond_position`), and left ordinary CS tracking
(`r100_cs_track_check`) as the kernel's whole judgment of the fetch;
the retained first delivery ran under that kernel, and the color-0 leg
of the widened check is anchored by that delivery's target bytes.  The
replay tests `r300-flat-color0-kernel-replay` and
`r300-flat-color0-cs-track-replay` pin the widened scope: both streams
`PASS` the width check, the CS-track replay accepts both, and its
control "VAP_VTX_SIZE below the output width" holds on the replication
stream and no longer rejects on the direct one.

## Falsifiers

- The `[record]` digests differ, the `[interface]` line does not read `route=direct-ga-color0` with
  the one Flat varying, or the `[state]` line reports fewer than 2 draws with the plan established:
  the runner exits 2 unspent, and the recording or state check is the finding.
- An `under-non-provoking` line with `interior` above zero: the GA selected another vertex, or the
  varying interpolated across the three color-0 inputs (`other_vertex_present`).
- An interior pixel whose RGB equals the provoking vertex's RGB under a different alpha byte
  (`alpha_deviates` above zero): alpha interpolated while RGB stayed flat (`alpha_interpolated`);
  the alpha lane is pairwise distinct across all three vertices for exactly this discriminator.
- A target `coverage_exact=1` under the other pass's provoking color: state crossed the pass
  boundary, or the second pass's contract did not reset the first pass's GA or RS words (`crossed`).
- The second target holding the sentinel over the whole extent: the command processor never reached
  the second cell (`second_unreached`).
- `canary_pass=0` on either target: a write outside the render extent.
- Any target byte differing from its retained expected image (`differing[p] != 0`): the receipt's
  byte-equality oracle fails even when the coverage oracle passes.
- A nonzero dmesg delta or a lockup ends the boot under the shared procedure's rollback rules.

The route is classified by whichever falsifier fires, never adjusted toward a pass, on the runner's
`[classify]` line; a deviation opens a finding rather than a changed prediction.

`r300-flat-color0-plan` calibrates the same registers offline, judged against the same emitted
stream the runner records: GA Flat -> Gouraud, provoking FIRST -> LAST, and RGB Flat with alpha
Gouraud each localize to `GA_COLOR_CONTROL` alone; RS source COLOR0 -> TEX0 localizes to `RS_COUNT`,
`RS_IP_0`, `RS_INST_0`, and `VAP_VSM_VTX_ASSM`; an RS destination away from US input 0 localizes to
`RS_INST_0` alone; a wrong COLOR0 carrier value differs from the expected target at every analytic
pixel; a second pass retaining the first pass's GA or RS state names the deviating draw index under
`r300_flat_color0_plan_stream_check`; every mutated plan is refused by
`r300_flat_color0_plan_validate` and fails the canonical per-draw check while passing its own; the
canonical stream against the replication stream also fails the canonical check.
`r3v-interpolation-lowering` refuses the direct route and selects replication for a partially
clipped primitive.

A delivered receipt proves end-to-end Vulkan `Flat` (RGBA) through RS482 GA provoking-vertex
selection over color 0, judged against the replication oracle's expected images, on the reference
triangle and the reference two-pass concatenation alone.  Hypothesis to record: the color 0 lane may
carry lower precision than TEX0, a possibility the quarter-step alphas are built to tolerate without
resolving; a deviation on the alpha discriminator classifies the route rather than adjusting a pass.

## Retained record

The shared procedure's record plus `first_target.bin`, `second_target.bin`, `first_expected.bin`,
and `second_expected.bin`, each the shape's full footprint including the canary row.


## Result

The cell holds its silicon receipt from one attended submission on
RS485M at Mesa `42ff2b207c8`, boot
`e5fc857e-4aa3-42e7-b3e5-7f31e2250f53`, under an arming report matching
all five declarations against cell blake3 `3646c222`, retained as
`steinmarder-r300/src/re/r300/results/r3v-native-public-flat-color0-two-draw-first-delivery-rs482`.
Every predicted value held:

```text
[interface] blake3 096d66a9 varying_mask=0x1 flat_mask=0x1 noperspective_mask=0x0 post_vs.flat_mask=0x1 provoking=0 route=direct-ga-color0
[record] kind=16 references=4 deferred_draws=2 ib_dwords=452 recorded blake3 3646c222 emitted blake3 3646c222
[state] direct plan registers established ahead of 2 draw(s)
[oracle] first-under-own-provoking-0xf2ff0000 judged=1 coverage_exact=1 canary=1 interior=1152 analytic=1152 exterior=2944 ambiguous=0 mismatch=0
[oracle] second-under-own-provoking-0x8c00ff00 judged=1 coverage_exact=1 canary=1 interior=1152 analytic=1152 exterior=2944 ambiguous=0 mismatch=0
[witness] pass 0 carrier records distinct=1 record0_is_provoking=1
[witness] pass 1 carrier records distinct=1 record0_is_provoking=1
[oracle] first-vs-expected judged=1 differing=0 alpha_deviates=0
[oracle] second-vs-expected judged=1 differing=0 alpha_deviates=0
```

All four non-provoking oracles read `interior=0`, `vkQueueSubmit`
returned 0, the dmesg delta was empty, the boot id was unchanged, and
the SB600 counter reported `armed verified 65535 65535 65369`,
`disarmed verified 65362`, a 140 us guarded interval over
`DRM_IOCTL_RADEON_CS` through fence completion, and `inactive` after.

The receipt proves end-to-end Vulkan Flat, RGB and alpha, through RS482
GA provoking-vertex selection over color 0: the device-fetched carriers
still held three distinct records per pass with the provoking first, so
host replication did not run, and each target is byte-equal to the
image the replication oracle predicted.  The alpha lane, distinct at
every vertex, held its provoking byte on every interior pixel, so
`ALPHA0_SHADING_FLAT` selects as `RGB0_SHADING_FLAT` does.  The
quarter-step alphas passed through the color 0 lane unchanged at UNORM8,
which bounds the lane's precision to eight bits or wider on these three
values and leaves the general float-vec4 precision of the lane a
recorded hypothesis.  Partially clipped primitives retain replication
and carry no receipt here.
