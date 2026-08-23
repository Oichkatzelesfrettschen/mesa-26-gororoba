# R3V native attended varying triangle cell procedure

The varying triangle cell is the hardware-ladder step that proves one
vertex-computed varying crosses the RS interpolator into the fragment
program. The application binds the reference NDC triangle as three
`R32G32B32A32_SFLOAT` positions, a vertex module that stores
`tint = fma(position, (0.5, 0.5, 0, 0), (0.5, 0.5, 0.25, 1))` to its
location-0 output beside `gl_Position`, and a fragment module that
writes the location-0 input to color output 0, then issues one
`vkCmdDraw`. The pipeline admits the varying vertex job together with
the pass-through fragment shape, the draw records the varying cell, and
at submission the CPU executor writes eight-dword records -- the
transformed position, then the computed tint -- into the carrier; the
consumer fetches them as two `FLOAT_4` elements, the RS interpolates the
second into US input 0, and the target receives the gradient.

`docs/hardware/r3v-native-attended-cell-procedure.md` carries the
boundary statement, the host preconditions, the identity freeze, the
arming conjunction, the rollback rules, and the retained-record layout;
this document adds only what the varying cell changes.

## Cell identity

- Runner: `r3v_native_attended_varying_triangle`, taking the evidence
  directory as its one argument.
- Arming digest source: `r3v_native_arming_runner --varying` emits the
  varying cell without submitting and reports the `ib_blake3` an
  authorization declares beside every arming factor; the attended runner
  prints the same digest as `cell varying-triangle ib_dwords=236
  ib_blake3=...` before it creates the instance.
  `r3v-native-arming-runner-varying-ib-identity` pins the runner's
  emission byte-identical to `r300_triangle_manifest --varying`, and
  `r3v-native-submit-order-varying-armed` pins the recorded command
  buffer's digest to `R300_VARYING_CELL_IB_BLAKE3`
  (`common/tests/r300_varying_cell_digests.h`), so the declared digest
  names the bytes the ioctl carries.
- Cell kind: `R3V_NATIVE_CELL_KIND_TRIANGLE`, the position-only cell's
  kind; the two cells differ in their streams and digests alone, so the
  arming gate's geometry predicate is the triangle cell's.
- Stream: the position-only consumer's contract and target with
  `VAP_PROG_STREAM_CNTL_0` declaring two `FLOAT_4` elements (vector 0 and
  vector 6, the texture-coordinate-0 vector), `VAP_VTX_SIZE` 8,
  `VAP_OUTPUT_VTX_FMT_1` four components, `VAP_VSM_VTX_ASSM`
  position plus TC0, `RS_COUNT` four interpolated components, `RS_IP_0`
  texture pointer 0 in channel order, `RS_INST_0` writing US input 0, a
  32-byte `3D_LOAD_VBPNTR` record, and the varying-passthrough US block
  (`r300_tcl_bypass_triangle_varying_fs`).
- Kernel-parser replay: `r300-varying-cell-cs-track-replay` replays the
  cell through `replay_r300_cs_track` (`replay dwords=236 relocs=2
  draws=1 passed=1 verdict=ACCEPT`) with every negative control holding;
  the TCL-bypass vertex-output check proves the two-element identity
  list against `VAP_VTX_SIZE` 8.
- Payload: the CPU route transforms the NDC triangle to the reference
  window positions and computes the per-vertex tint
  `r300_tcl_bypass_triangle_varying_colors` -- (0.125, 0.125, 0.25, 1),
  (0.875, 0.125, 0.25, 1), (0.5, 0.875, 0.25, 1) -- so the carrier is
  `r300_tcl_bypass_triangle_varying_vertices` byte for byte.
- Clear value: the 0xa5a5a5a5 sentinel in all four channels, as for the
  position-only cell.
- Recording calibration: `r3v_native_attended_varying_triangle <dir>
  --record-only` runs on the drm-shim fixture as
  `r3v-native-varying-triangle-record`.
- Delivery route: the CPU route, every `R3V_NATIVE_R2VB_*_EXPERIMENTAL`
  gate unset; the runner refuses an open producer gate before the
  ioctl.

## Declarations

Every value below is set for the run; one missing value refuses at the
gate and consumes the attempt.

- `R3V_NATIVE_SUBMIT_HAZARD_ACCEPTED=1`
- `R3V_NATIVE_AUTHORIZED_IB_BLAKE3=<digest r3v_native_arming_runner
  --varying reports>`
- `R3V_NATIVE_AUTHORIZED_KERNEL_RELEASE=<uname -r>`
- `R3V_NATIVE_AUTHORIZED_MODULE_SRCVERSION=<loaded radeon srcversion>`
- `R3V_NATIVE_MANIFEST_DIR=<fresh evidence directory>`, the same
  directory the runner takes as its argument
- `R3V_NATIVE_R2VB_DELIVERY_EXPERIMENTAL`,
  `R3V_NATIVE_R2VB_GPU_DELIVERY_EXPERIMENTAL`, and
  `R3V_NATIVE_R2VB_FETCHED_PRODUCER_EXPERIMENTAL` unset

## Predictions

Recorded before the run; deviation is the finding.

1. The kernel CS parser accepts the stream, the fence retires, and dmesg
   carries no radeon validation delta.
2. The target's interior samples carry the barycentric interpolation of
   the three tint colors within `R300_TRIANGLE_VARYING_ORACLE_TOLERANCE`
   (2) bytes per channel -- blue 0x40 and alpha 0xff at every sample,
   red and green the gradient -- with the exterior and the canary row at
   the sentinel: `r300_tcl_bypass_triangle_varying_extent_oracle` reports
   `interior_pass`, `exterior_pass`, and `canary_pass` with
   `interior_max_deviation` at most 2.

## Falsifiers

- Interior at the sentinel with the fence retired: the draw wrote no
  color; the two-element vertex fetch, the `VAP_VSM_VTX_ASSM` assembly,
  or the RS routing starved the US, the class the position-only cell's
  first-draw contract already bounds.
- Interior at one constant color: the US did not read the interpolated
  varying; the RS_INST_0 destination or the US program's input register
  is the first suspect.
- Interior a gradient outside the tolerance band: the interpolation or
  the record layout is wrong -- swapped vertices read as a rotated
  gradient, a missing swizzle as a permuted channel order, FP24 narrowing
  as a uniform bias; `interior_max_deviation` and the retained
  `color.bin` localize it.
- A disturbed canary row, a dmesg validation delta, or an unretired
  fence: as for the position-only cell; the host is inspected, not
  resubmitted.

## Verdict

One oracle decides the run: `r300_tcl_bypass_triangle_varying_extent_oracle`
over the retained `color.bin`. `TARGET_DELIVERED` requires the
completion status, `executed`, `interior_pass`, `exterior_pass`, and
`canary_pass` together. A parser acceptance or a retired fence alone
proves transport; the gradient proves the varying.

## Retained record

`varying_triangle_outcome.json` carries the verdict, the submit result,
the queue status, the cell's dword count and digest, every oracle field,
`interior_max_deviation`, and the tolerance. `color.bin` carries the full
64x65-pixel target footprint. The `steinmarder-r300` bundle seals these
beside the run's dmesg delta, the loaded-module identity, and the arming
report.
