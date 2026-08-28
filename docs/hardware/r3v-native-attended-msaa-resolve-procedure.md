# R3V native attended multisample resolve cell procedure

The multisample resolve cell is the expansion ladder's rung-6 mechanism
on silicon: one indirect buffer whose render half draws the analytic
triangle into a sample-expanded color surface with `GB_AA_CONFIG`'s
subsample set live, and whose resolve half covers the whole extent again
under `RB3D_AARESOLVE_CTL.AARESOLVE_MODE_RESOLVE` so the downsampled
samples reach `RB3D_AARESOLVE_OFFSET`.  The resolve half carries a
fragment constant no multisample sample holds, so the destination
separates the readings of the resolve semantics: one submission
classifies them instead of confirming one.

`docs/hardware/r3v-native-attended-cell-procedure.md` carries the
boundary statement, the host preconditions, the substrate admission
table, the identity freeze, the arming conjunction, the rollback rules,
and the retained-record layout; this document adds only what the resolve
cell changes.

## Cell identity

- Runner: `r3v_native_attended_msaa_resolve <dir> [--samples 2|4]`
  (native build), taking the evidence directory as its first argument.
- Arming digest source: `r3v_native_arming_runner --msaa 4 <dir>` emits
  the resolve cell, binds its relocation payloads to the merged indices,
  and reports `471 IB dwords, blake3 be78db1c...`.  The attended runner
  prints the same digest on its `[shape]` line before it creates the
  instance.
- Cell kind: `R3V_NATIVE_CELL_KIND_TRIANGLE_MSAA_RESOLVE`.  The arming
  gate's geometry predicate for this kind requires four references for
  the cell's five slots, the multisample surface carrying a VRAM write
  alone since both halves render into it.
- Recording: `r3v_native_record_msaa_resolve` builds that reference
  array merged in one pass and binds the payloads to its own positions,
  so the queue's own merge is idempotent.  The record-time contract runs
  on the drm-shim as `r3v-native-msaa-cell-reloc-bound`, with
  `r3v-native-msaa-cell-reloc-unbound` proving the emitted digest
  refuses the recorded cell.
- Shapes: both halves take the reference render shape, 64x64 at pitch 64
  in `B8G8R8A8` lane order, and the resolve destination sits at offset
  zero of its own allocation.
- Vertex payloads: the runner owns both.  The render half fetches the
  analytic triangle (`r300_tcl_bypass_triangle_render_shape_vertices`),
  the resolve half the cover triangle at `(0, 0)`, `(2w, 0)`, `(0, 2h)`
  whose interior holds every pixel center in the extent
  (`r300_tcl_bypass_triangle_cover_vertices`), since a resolve emits
  only for the pixels a fragment covers.
- Seed: the resolve destination carries `R300_TRIANGLE_COLOR_SENTINEL`
  (`0xa5a5a5a5`) before the submission, so a resolve that wrote nothing
  and one that wrote correctly are distinct results.

## Placement

The multisample surface is the recording's own allocation in
`RADEON_GEM_DOMAIN_VRAM` with no fallback domain, under
`RADEON_GEM_NO_CPU_ACCESS`, and the host never maps it.  The
memory-type policy's type 1 gives `RADEON_GEM_DOMAIN_VRAM |
RADEON_GEM_DOMAIN_GTT`, under which a host-unmapped allocation proves
nothing about residency, so the cell takes the strict path instead: a
successful create is the placement, and the runner's `[placement]` line
reports it.  Its size carries the sample expansion the stride does not
(`r300_texture_desc.c` multiplies the layer size and leaves the pitch
alone).

The record-only pass allocates that surface without reaching any
submission, so it is also the placement preflight: a VRAM-alone create
that the kernel refuses fails there rather than inside the attended
run's attempt budget.

## Footprint bounds

`r100_cs_track_check` sizes the color buffer as `pitch * cpp * maxy`
with no sample term, so the kernel validates nothing about the sample
expansion and the multisample footprint proof lives entirely in the
driver.  The resolve destination is bounded there: `aa.pitch *
cb[0].cpp * maxy + aa.offset` against the buffer size, so an undersized
destination refuses at the validator.  While the parser footprint
carries no sample multiplier, the multisample path stays an internal
attended cell rather than an advertised Vulkan capability.

## Declarations

Every value below is set for the run; one missing value refuses at the
gate and consumes the attempt.

- `R3V_NATIVE_SUBMIT_HAZARD_ACCEPTED=1`
- `R3V_NATIVE_AUTHORIZED_IB_BLAKE3=<digest r3v_native_arming_runner
  --msaa 4 reports>`
- `R3V_NATIVE_AUTHORIZED_KERNEL_RELEASE=<uname -r>`
- `R3V_NATIVE_AUTHORIZED_MODULE_SRCVERSION=<loaded radeon srcversion>`
- `R3V_NATIVE_MANIFEST_DIR=<fresh evidence directory>`, the same
  directory the runner takes as its first argument

## Arming

In a fresh evidence directory:

1. `r3v_native_arming_runner --msaa 4 "$dir"` with the gate declarations
   unset, and retain the closed report.
2. Export the four declarations above from that report.
3. Rerun the arming runner and require `verdict: armed`; retain the
   armed report.
4. Record the dmesg window, then run
   `r3v_native_attended_msaa_resolve "$dir" --samples 4`.

The one-shot token disarms the directory after the ioctl.

## Predictions

The verdict denominator is the subsample set, not the pixel center:
`GB_MSPOS0` and `GB_MSPOS1` place the subsamples off-center, so a pixel
whose center sits inside the triangle while a subsample sits outside
resolves to a blend of the draw color and whatever the multisample
buffer held.  `r300_tcl_bypass_triangle_sample_set_oracle` judges a
pixel only when every subsample clears the analytic edges by
`R300_TRIANGLE_SAMPLE_MARGIN`, which holds the judged counts across
margins from 1/64 to 1/16 pixel, so the verdict rides neither the fill
rule nor the float representation.  At the reference geometry the judged
footprint is 1128 pixels at two samples and 1104 at four, against the
center oracle's 1152.

Four passes share that denominator, each admitting a single dword:

- `downsample`: `0xdf20609f`, the render half's draw dword.
  `AARESOLVE_MODE_RESOLVE` redirects the color backend to the resolve
  offset and emits the downsampled samples while the fragment supplies
  coverage alone.
- `fragment`: `0xffff00ff`, the resolve half's own constant through the
  destination's lane order.  The fragment write reaches the destination.
- `seed`: `0xa5a5a5a5`.  The resolve wrote nothing.
- `either`: both writes admitted.  A destination exact here and under
  neither single-dword pass holds a mixture the two writers reached
  order-dependently.

If the cell is correct, exactly one pass reads `interior_exact=1` with
`judged=1` on all four, and the dmesg delta is zero.  The run classifies
which; each of the four is a recorded outcome rather than a deviation.

One attempt spends one sample count.  The arm runs at four samples, the
1104-pixel denominator, since the 4x grid is where the subsample set
meets the slope -2 edge exactly and the margin rule earns its keep; the
two-sample arm at 1128 pixels is the follow-on and a 4x result settles
nothing about it.

The exterior is a named scope cut.  The multisample surface takes no
clear, so the resolve carries whatever that surface held outside the
analytic triangle into the destination; the subsample-set oracle leaves
those pixels unjudged, which surrenders the sentinel corner that proves
the device wrote inside the extent.

## Falsifiers

- No pass reads `interior_exact=1` while the `[census]` line reports a
  nonzero count of a predicted dword across the footprint: the
  destination holds the predicted bytes outside their linear positions,
  so the destination's byte order is the finding.  Per the AMD R3xx 3D
  register document the resolve destination carries a pitch and an
  offset and no tiling field, while `r300_is_simple_msaa_resolve` takes
  its fast path only for a tiled destination, so a linear destination
  receiving a tiled swizzle is the live alternative.
- No pass reads `interior_exact=1` and the census reports neither
  predicted dword: the resolve's addressing or the cover half's coverage
  is the finding, which the retained `resolve_destination.bin` names.
- `judged=0` on any pass: the shape or the retained footprint left the
  producer's domain, so the zero counters carry no claim.
- A nonzero dmesg delta or a lockup ends the boot under the shared
  procedure's rollback rules.

## First attempt: the subsample set never reached either draw

The first attended submission on RS482 (boot
`e5fc857e-4aa3-42e7-b3e5-7f31e2250f53`, cell blake3 `e78da1dc`, 477 IB
dwords) returned `vkQueueSubmit` 0 with an empty dmesg delta, a 104 us
guarded interval, and the destination holding `0xa5a5a5a5` in all 4096
footprint pixels: the `seed` pass read `interior_exact=1` over 1104
judged pixels and the census found neither predicted dword anywhere.
Falsification criterion 2 fired.

The retained `ib.bin` names the mechanism.  `GB_AA_CONFIG`,
`GB_MSPOS0`, `GB_MSPOS1`, and `RB3D_AARESOLVE_CTL` are all first-draw
contract entries (`r300_first_draw_state.c`), written at their
single-sample values as `EXPLICIT_DISABLE` and `REQUIRED_INVARIANT`.
The cell programmed them in a prologue and an interlude, ahead of each
half's contract, so every one was written back before the draw it was
meant for: `GB_AA_CONFIG` armed at dword 0 and disabled at dword 15,
`GB_MSPOS0` reset to the center sample at dword 157, and
`RB3D_AARESOLVE_CTL` armed at dword 236 and returned to NORMAL at dword
256, before the cover draw.  The silicon executed what the stream said:
two ordinary single-sample draws into the multisample surface and no
resolve, so the destination correctly kept its seed.

The subsample set now travels through each half's own contract, which
`r300_first_draw_params` carries as a declaration, and the interlude
holds only the destination's base and pitch -- the two words no contract
entry names.  The emitted stream arms `GB_AA_CONFIG` and the subsample
positions inside both contracts, `AARESOLVE_CTL` NORMAL for the render
half and RESOLVE for the resolve half, and closes both at the epilogue.

## Result

The cell holds its silicon receipt from the second attended arm on
RS482, boot `e5fc857e-4aa3-42e7-b3e5-7f31e2250f53`, under an arming
report matching all five declarations against cell blake3 `be78db1c`.
The hypothesis held:

```text
[oracle] downsample judged=1 interior_exact=1 interior=1104 analytic=1104 unjudged=96
[oracle] fragment   judged=1 interior_exact=0 interior=0    analytic=1104 unjudged=96
[oracle] seed       judged=1 interior_exact=0 interior=0    analytic=1104 unjudged=96
[oracle] either     judged=1 interior_exact=1 interior=1104 analytic=1104 unjudged=96
[census] footprint 4096 pixels holds downsample 1104 fragment 668 seed 0
[oracle] destination centroid (32,24)=0xdf20609f corner (0,0)=0xffff00ff
```

`vkQueueSubmit` returned 0, the dmesg delta was empty, the boot was
unchanged, and the SB600 counter was armed across `DRM_IOCTL_RADEON_CS`
through fence completion for 155 us against the 1.7 s operational grace,
then disarmed.

Every judged pixel carries the render half's draw dword, so
`RB3D_AARESOLVE_CTL.AARESOLVE_MODE_RESOLVE` redirects the color backend
to `RB3D_AARESOLVE_OFFSET` and emits the downsampled samples while the
resolve half's fragment supplies coverage alone.  The `fragment` and
`seed` passes both read `interior_exact=0`, so neither a fragment write
nor an absent write explains the destination.  The reading
`r300_simple_msaa_resolve` implies -- a fragment color written into the
surface being resolved would destroy the samples the resolve reads -- is
the reading the silicon takes.

The destination byte order settles with it: the judged pixels read their
predicted dword at their linear positions, so a linear resolve
destination receives linearly ordered bytes at the 64x64 pitch-64
`B8G8R8A8` shape the arm ran, and the tiled-swizzle reading is refuted
there.  A wider or differently pitched destination is untested, as is
the two-sample arm at the 1128-pixel denominator.

The exterior stays open under the arm's named scope cut.  The
multisample surface takes no clear, so the resolve carries whatever it
held outside the analytic triangle into the destination: 668 unjudged
pixels hold the resolve half's fragment constant, the corner among them,
and 2324 hold neither predicted dword.  A destination receiving the
fragment constant where the surface held no rendered samples is a
mixture the judged footprint says nothing about; a command-stream clear
of the multisample surface is what would close it.

Bundle: steinmarder-r300
`src/re/r300/results/r3v-native-msaa-resolve-downsample-semantics-rs482`,
which retains both arms.

## Retained record

The shared procedure's record plus `resolve_destination.bin`, the
destination's full footprint.  The multisample surface is device-local
and never CPU-read, so it leaves no retained bytes; the `[placement]`
line is its record.
