# R3V native attended composed render-then-sample cell procedure

The composed render-then-sample cell is the expansion ladder's rung-7
mechanism on silicon: one indirect buffer whose render half draws the
analytic triangle into the first target, and whose sample half samples
that same target as its texture and draws into the second. The two
halves meet at the cell's own coherency edge -- the render half's
`RB3D_DSTCACHE_CTLSTAT` flush-dirty plus free-3D-tags stands ahead of
the sample half's `TX_INVALTAGS` -- so a submission that reproduces the
first target's coverage in the second proves the color writes published
before the texture fetch read them.

`docs/hardware/r3v-native-attended-cell-procedure.md` carries the
boundary statement, the host preconditions, the substrate admission
table, the identity freeze, the arming conjunction, the rollback rules,
and the retained-record layout; this document adds only what the
composed cell changes.  The composed cell's one-attempt budget rests on
that substrate check: a development-profile boot refuses before arming,
so the attempt stays unspent.

## Cell identity

- Runner: `r3v_native_attended_composed <dir>` (native build), taking
  the evidence directory as its one argument.
- Arming digest source: `r3v_native_arming_runner --composed 0 <dir>`
  emits the composed cell, binds its relocation payloads to the merged
  indices, and reports `485 IB dwords, blake3 247949a2...`. The attended
  runner prints the same digest on its `[shape]` line before it creates
  the instance, so the operator compares the authorization against the
  cell this binary records before any ioctl runs.
- Cell kind: `R3V_NATIVE_CELL_KIND_TRIANGLE_COMPOSED_RENDER_SAMPLE`.
  The arming gate's geometry predicate for this kind requires four
  references for the cell's five slots, the shared first target carrying
  the write domain the render half needs beside the read domain the
  kernel's texture check needs.
- Recording: `r3v_native_record_composed_render_sample` builds that
  reference array merged in one pass and binds the payloads to its own
  positions, so the queue's own merge is idempotent and the indices the
  digest covers are the indices the kernel reads. The record-time
  contract runs on the drm-shim as
  `r3v-native-composed-cell-reloc-bound`, with
  `r3v-native-composed-cell-reloc-unbound` proving the emitted digest
  refuses the recorded cell.
- Shapes: both halves take the reference render shape, 64x64 at pitch
  64 in `B8G8R8A8` lane order, and the sample half's target sits at
  offset zero of its own allocation.
- Vertex payloads: the runner owns both. The render half fetches
  four-dword position records
  (`r300_tcl_bypass_triangle_render_shape_vertices`), the sample half
  eight-dword position-plus-TEX0 records
  (`r300_tcl_bypass_triangle_varying_shape_vertices`).
- Seed: both targets carry `R300_TRIANGLE_COLOR_SENTINEL`
  (`0xa5a5a5a5`) before the submission, so every dword either target
  holds afterward names its writer.

## Declarations

Every value below is set for the run; one missing value refuses at the
gate and consumes the attempt.

- `R3V_NATIVE_SUBMIT_HAZARD_ACCEPTED=1`
- `R3V_NATIVE_AUTHORIZED_IB_BLAKE3=<digest r3v_native_arming_runner
  --composed 0 reports>`
- `R3V_NATIVE_AUTHORIZED_KERNEL_RELEASE=<uname -r>`
- `R3V_NATIVE_AUTHORIZED_MODULE_SRCVERSION=<loaded radeon srcversion>`
- `R3V_NATIVE_MANIFEST_DIR=<fresh evidence directory>`, the same
  directory the runner takes as its argument

## Arming

In a fresh evidence directory:

1. `r3v_native_arming_runner --composed 0 "$dir"` with the gate
   declarations unset, and retain the closed report.
2. Export the four declarations above from that report.
3. Rerun the arming runner and require `verdict: armed`; retain the
   armed report.
4. Record the dmesg window, then run
   `r3v_native_attended_composed "$dir"`.

The one-shot token disarms the directory after the ioctl.

## Predictions

TEX0 at each vertex is that vertex's window position over the render
extent, so the interpolated coordinate at pixel center `(x + 0.5,
y + 0.5)` scaled by the texture extent lands on texel `(x, y)`, a half
texel from either boundary
(`test_varying_tex0_is_the_window_position_fraction`). The sample half
therefore reproduces its texture's coverage pixel for pixel, and one
predicted interior dword covers both targets.

If the cell is correct, both `[oracle]` lines report
`coverage_exact=1 canary=1` with `interior=1152 analytic=1152
exterior=2944 mismatch=0`, the sample centroid reads the render half's
draw dword, and the dmesg delta is zero.

## Falsifiers

- The sample target's interior reads `0xa5a5a5a5` across the analytic
  triangle (`[classify] ... the render half's writes reached no texture
  fetch`): the texture fetch ran ahead of the render half's
  publication, and the cell's coherency edge is the finding.
- The sample target's interior reads other bytes: the fetch's
  addressing or the sample half's coverage, which the mismatch count
  and the retained `sample_target.bin` name.
- The render target deviates while the sample target follows it: the
  render half alone, which the render-shape arms already cover, so the
  session's own deployment or route drift is the finding.
- `canary_pass=0` on either target: the color backend wrote outside the
  render extent, into the pitch padding or the canary row.
- A nonzero dmesg delta or a lockup ends the boot under the shared
  procedure's rollback rules.

## Retained record

The shared procedure's record plus `render_target.bin` and
`sample_target.bin`, each the shape's full footprint including the
canary row.

## Result

The cell holds its silicon receipt from one attended submission on
RS485M, boot `e5fc857e-4aa3-42e7-b3e5-7f31e2250f53`, under an arming
report matching all five declarations against cell blake3 `247949a2`.
Every predicted value held:

```text
[oracle] render judged=1 coverage_exact=1 canary=1 interior=1152 analytic=1152 exterior=2944 ambiguous=0 mismatch=0
[oracle] sample judged=1 coverage_exact=1 canary=1 interior=1152 analytic=1152 exterior=2944 ambiguous=0 mismatch=0
[oracle] sample centroid (32,24)=0xdf20609f predicted 0xdf20609f corner (0,0)=0xa5a5a5a5
```

`vkQueueSubmit` returned 0, the dmesg delta was empty, and the boot id
was unchanged.  The two retained targets are byte-identical across the
full 16640-byte footprint, so the sample half reproduced the render half
pixel for pixel rather than at the centroid alone.

The first falsifier is the one that carries the mechanism: a sample
interior reading `0xa5a5a5a5` would have placed the texture fetch ahead
of the render half's publication.  The sample interior carries the
render half's color, so the render half's `RB3D_DSTCACHE_CTLSTAT`
flush-dirty plus free-3D-tags publishes the color writes before the
sample half's `TX_INVALTAGS` and the fetch that follows it, inside one
indirect buffer on this silicon.

The submission ran inside a hardware-armed SB600 counter over
`DRM_IOCTL_RADEON_CS` through fence completion: `armed verified 65535
65534 65368`, `disarmed verified 65362`, a 134 us guarded interval
against the 1.7 s operational grace, and the counter back to `inactive`.
No recovery waiver was in scope.

The record-only calibration the shared procedure requires runs as
`r3v_native_attended_composed <dir> --record-only`, the directory ahead
of the flag; `r3v_native_attended_composed.c` gates
`r3v_native_watchdog_guard_open` on `!record_only`, so that pass
calibrates the recording rather than the guard, and the guard takes its
calibration from a qualified control submission.
