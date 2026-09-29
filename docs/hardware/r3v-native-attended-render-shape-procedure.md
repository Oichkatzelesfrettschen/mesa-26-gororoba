# R3V native attended render-shape procedure

This document is the procedure for one physically attended session on
the RS485M host that submits the TCL-bypass triangle cell over declared
render shapes: the extent, row pitch, lane order, and fragment constant
a Vulkan render pass places on the qualified cell
(`r300_triangle_render_shape`, `src/amd/r300/common/r300_tcl_bypass_triangle.h`).
Each arm of the session is its own authorization, evidence directory,
digest, and one-shot token; the session earns in one attendance the
silicon evidence the executed render family needs before
`dEQP-VK.api.smoke.triangle` can reach a nonempty IB. Executing it
requires that authorization; reading and rehearsing it does not.

## Boundary this procedure crosses

The qualified cell freezes four target facts. Each is one register
class of the same stream: the extent moves the two scissor-family
payloads the first-draw contract resolves; the pitch moves the
`RB3D_COLORPITCH0` payload; the lane order moves the contract's
`US_OUT_FMT_0` payload; the fragment constant moves the four
`R300_PFS_PARAM_0` payloads in the register's FP24 encoding.
`r300_tcl_bypass_triangle_test` pins that each parameter alone moves
its named payloads and nothing else, and that the reference shape emits
byte-identical to the reference cell, so the qualified digest anchors
the family. What the host cannot prove is silicon behavior under each
moved payload: that the color backend honors a 256-pixel pitch, that the
`C*_SEL` exchange places red and blue as predicted, that the FP24
constant converts to the predicted UNORM8 bytes, and that a 256x256
target renders inside its footprint.

The reference cell's executed constant is the byte-order oracle color
(0.125, 0.375, 0.625, 0.875), interior dword `0xdf20609f`; the
attended-cell procedure's prediction text names `0xff00ff00`, which
describes an earlier fragment block, and the retained oracle verdicts
name the emitter's constant. The constant arm below is the first
silicon witness of a constant other than the oracle color.

## Arms

Every arm is the same executable over a different shape. The reference
arm is the control: its digest equals the qualified cell's, so a
deviation there names the session, not a parameter.

| arm | `--shape` tokens | moved payloads | predicted interior |
|-----|------------------|----------------|--------------------|
| reference | `64 64 64 bgra 0x3e000000 0x3ec00000 0x3f200000 0x3f600000` | none | `0xdf20609f` |
| lanes | `64 64 64 rgba 0x3e000000 0x3ec00000 0x3f200000 0x3f600000` | `US_OUT_FMT_0` | `0xdf9f6020` |
| pitch | `64 64 256 bgra 0x3e000000 0x3ec00000 0x3f200000 0x3f600000` | `RB3D_COLORPITCH0` | `0xdf20609f` at pitch 256 |
| constant | `64 64 64 bgra 0x3f800000 0x0 0x3f800000 0x3f800000` | four `PFS_PARAM_0` | `0xffff00ff` |
| extent | `256 256 256 bgra 0x3e000000 0x3ec00000 0x3f200000 0x3f600000` | scissor pair, `RB3D_COLORPITCH0` | `0xdf20609f` over 256x256 |
| composed | `256 256 256 rgba 0x3f800000 0x0 0x3f800000 0x3f800000` | all four classes | `0xffff00ff` |
| composed-asym | `256 256 256 rgba 0x3f800000 0x0 0x0 0x3f800000` | all four classes | `0xff0000ff` |
| offset | `64 64 64 bgra 0x3e000000 0x3ec00000 0x3f200000 0x3f600000 --offset 4096` | `RB3D_COLOROFFSET0` | `0xdf20609f` at byte 4096, the first 4096 bytes at the sentinel |

The composed arm is the `dEQP-VK.api.smoke.triangle` target shape,
whose magenta constant carries red equal to blue, so it witnesses
extent, pitch, and constant together without separating a lane-order
effect from that equality. The composed-asym arm carries red without
blue, so it is the four-class interaction witness: a lane-order defect
that the composed arm's symmetric constant cannot expose. The arms run
in table order; a falsifier on a single-parameter arm stops the
session before the composed arms, so a composed deviation never has to
be decomposed after the fact.

The offset arm renders the reference shape with row 0 at byte 4096 of
the color allocation, the target a Vulkan attachment bound at a
nonzero `memoryOffset` carries. The payload travels as the byte offset
the kernel biases by the relocation base (`r300_packet0_check` writes
`ib[idx] = idx_value + reloc->gpu_offset`), and
`r100_cs_track_check` validates `offset + pitch * cpp * maxy` against
the buffer size, so an admitted footprint passes that check with the
canary row to spare.

## Preconditions

- The runner is the Meson target `r3v_native_attended_render_shape`,
  invoked as `r3v_native_attended_render_shape --shape <tokens>
  <evidence-dir>`; it statically links the native implementation. The
  drm-shim rehearsal is `r3v-native-triangle-cell-shape`, which records
  the composed shape through the same recorder, arms it under its own
  digest and cell kind (`triangle_render_shape`), submits through the
  shim, and proves the retained `ib.bin` equals the emitter's stream.
- The arming runner names the arm's digest:
  `r3v_native_arming_runner --shape <tokens> <evidence-dir>` reports the
  shape, its predicted interior dword, its color footprint, the digest,
  and every arming factor; `--shape <tokens> --emit-ib <path>` writes
  the stream for the offline replay. `r3v-native-arming-runner-refuses-undeclared`
  calibrates the shape report, the odd-pitch refusal, and the refusal
  of a shape under the reference digest.
- Every precondition of the attended-cell procedure holds: the
  authorized chip `1002:5974`, the declared kernel release and radeon
  module srcversion (the deployed `radeon-rs482-policy` package, per
  the kernel deployment reconciliation in the program status), off-box
  kernel logging, a fresh boot, and one fresh evidence directory per
  arm.

## Arming

Per arm, in a fresh evidence directory:

1. `r3v_native_arming_runner --shape <tokens> "$dir"` with the gate
   closed; retain the report and its digest.
2. Declare `R3V_NATIVE_SUBMIT_HAZARD_ACCEPTED=1`,
   `R3V_NATIVE_AUTHORIZED_IB_BLAKE3=<digest>`,
   `R3V_NATIVE_AUTHORIZED_KERNEL_RELEASE`,
   `R3V_NATIVE_AUTHORIZED_MODULE_SRCVERSION`, and
   `R3V_NATIVE_MANIFEST_DIR="$dir"`.
3. Rerun the arming runner and require `verdict: armed`; retain the
   report.
4. `r3v_native_attended_render_shape --shape <tokens> "$dir"`; the
   `[shape]` line restates the tokens and the predicted interior dword
   before the instance is created.

The one-shot token disarms the directory after the ioctl; the next arm
takes a new directory and a new digest.

## Predictions

Recorded per arm before the run; the observation stands as made.

- `DRM_RADEON_CS` returns 0 and the completion wait retires inside its
  bound.
- The `[oracle]` line reports `executed=1 interior=1 exterior=1
  canary=1` with `interior_samples=4`; the centroid sample equals the
  predicted dword of the arm's row, the `(0,0)` sample and the canary
  row carry `0xa5a5a5a5`.
- The `[coverage]` line reports `judged=1 exact=1 canary=1` with
  `mismatch_pixels=0`, `ambiguous_pixels=0`, and `interior_pixels`
  equal to `analytic_pixels`: 1152 at extent 64, 18432 at extent 256.
  The coverage verdict classifies every pixel center of the extent
  against the analytic triangle (`r300_tcl_bypass_triangle_coverage_oracle`)
  and joins the runner's pass verdict beside the sampled one; re-judged
  over the seven retained color targets of
  `r3v-render-shape-family-seven-arm-delivery-rs482`, it reads exact on
  every arm.
- `dmesg` gains no radeon CS validation error, reset, or lockup line.

## Falsifiers

- The reference arm deviates: the session itself is the finding
  (deployment, boot, or route drift), and no parameter arm runs.
- The lanes arm's interior is `0xdf20609f`: `US_OUT_FMT_0` C*_SEL does
  not place channels as the register model predicts; the R8G8B8A8
  admission stays closed.
- The pitch arm fails `canary_pass`: the color backend wrote outside
  the 64-pixel columns of a 256-pixel row, or past the extent; the
  unfrozen pitch stays closed.
- The constant arm's interior is a UNORM8 rounding of the constant
  other than `0xffff00ff`: the FP24-to-UNORM8 conversion model is
  wrong, and the constant admission stays closed.
- The extent arm fails `interior_pass` with `exterior_pass` true: the
  256x256 raster or its scissor pair differs from the 64x64 model.
- The composed arm deviates after every single arm passed: the
  parameters interact, and the interaction is the finding.
- The composed-asym arm's interior is `0xffff0000`: the lane order did
  not compose with the other three parameters.
- The offset arm renders at byte 0 rather than byte 4096, or leaves
  writes in the first 4096 bytes: `RB3D_COLOROFFSET0` reaches the
  color backend biased differently than the payload declares, and the
  bind-offset admission stays closed.
- Any `dmesg` CS validation error, reset, lockup, or host hang ends the
  session.

## Rollback

The attended-cell procedure's rollback applies unchanged: no
resubmission after a falsifier, off-box `dmesg` capture, clean reboot
for a wedged GPU, power-cycle for a wedged host, and ICD manifest
removal to restore the pre-run driver configuration.

## Retained record

Per arm, the evidence directory and its mirror in the r300 evidence
repository keep the attended-cell record plus `color_target.bin` at the
shape's footprint (`offset + pitch * (height + 1) * 4` bytes), the arming report
with the shape line, and the runner's console with the `[shape]` and
`[oracle]` lines. A session bundle relates the arms to the
program-status row they close.

## Extent ladder

The ladder carries the reference triangle past the receipt ceiling
(`R300_TRIANGLE_RENDER_RECEIPT_MAX_EXTENT`, 256, the widest retained
arm) to the family's emit ceiling (`R300_TRIANGLE_RENDER_EMIT_MAX_EXTENT`,
the RS485M render span `R300_RS4XX_RENDER_SPAN_MAX`, 2560). Every rung
is square at pitch equal to the extent, B8G8R8A8 lanes, and the
reference constant, so each rung moves the two scissor-family payloads
and `RB3D_COLORPITCH0` against the reference cell and nothing else
(`test_render_shape_extent_ladder` in `r300_tcl_bypass_triangle_test`
pins the three dwords at every rung). The vertices scale with the
extent through the viewport transform, as the 256 arm's did:
`(e/8, e/8)`, `(7e/8, e/8)`, `(e/2, 7e/8)`.

A rung past the receipt ceiling records only under
`R3V_NATIVE_RENDER_EXTENT_PROBE=1`, declared beside the arming
variables in step 2; unset, empty, `0`, or any other value leaves the
recorder at the receipt ceiling, and the attended runner refuses the
shape before creating an instance. The arming report prints an
`extent probe gate` line for such a shape, `match` only at the exact
value. The gate opens the declared-shape recorders alone: the advertised
framebuffer and viewport limits, render-target creation, and the public
draw route stay at 256 (`r3v-native-triangle-cell-shape-extent` pins
both gate states).

| rung | `--shape` tokens | predicted interior | color bytes | `[oracle]` samples | `[coverage]` interior / exterior pixels | centroid sample |
|------|------------------|--------------------|-------------|--------------------|------------------------------------------|-----------------|
| 512 | `512 512 512 bgra 0x3e000000 0x3ec00000 0x3f200000 0x3f600000` | `0xdf20609f` | 1050624 | 4 interior, 12 exterior | 73728 / 188416 | `(256,192)` |
| 1024 | `1024 1024 1024 bgra 0x3e000000 0x3ec00000 0x3f200000 0x3f600000` | `0xdf20609f` | 4198400 | 4 interior, 12 exterior | 294912 / 753664 | `(512,384)` |
| 2048 | `2048 2048 2048 bgra 0x3e000000 0x3ec00000 0x3f200000 0x3f600000` | `0xdf20609f` | 16785408 | 4 interior, 12 exterior | 1179648 / 3014656 | `(1024,768)` |
| 2560 | `2560 2560 2560 bgra 0x3e000000 0x3ec00000 0x3f200000 0x3f600000` | `0xdf20609f` | 26224640 | 4 interior, 12 exterior | 1843200 / 4710400 | `(1280,960)` |

The interior count is 9/32 of the extent's area, the NDC triangle's
0.75-by-0.75 half-rectangle, and no pixel center lies on an edge at any
rung, so `ambiguous_pixels` predicts 0. The color footprint is
`pitch * (extent + 1) * 4` bytes, the canary row included.

Order and authorization:

- The session's reference arm runs first as the control, then the
  receipted 256 extent arm, then the rungs in table order.
- Each rung is its own authorization: its own evidence directory, the
  digest its own arming report names, and its own one-shot token.
- The ladder stops at the first falsifier; no later rung runs in that
  session.

Falsifiers, each ending the ladder at its rung:

- Any mismatched pixel: `[coverage]` `exact=0`, `mismatch_pixels`
  nonzero, or `interior_pixels` below the rung's `analytic_pixels`; or
  `[oracle]` `interior=0` or `exterior=0`.
- A canary change: `canary=0` on either line, a write in the canary row
  or the pitch padding.
- A CS refusal: `vkQueueSubmit` returns an error or `DRM_RADEON_CS`
  rejects the stream (`r100_cs_track_check` bounds the color buffer
  by `offset + pitch * cpp * maxy` against the allocation).
- A nonempty radeon `dmesg` delta: a CS validation line, reset, or
  lockup.

The ladder owes a CS-track replay before its first attended rung: the
`r300-*-cs-track-replay` tests replay the fixed cell's 64-pixel bundle
and carry no render-shape member, so each rung's
`r3v_native_arming_runner --shape <tokens> --emit-ib <path>` stream
replays through `replay_r300_cs_track` with a bundle sized to the
rung's color footprint. A passing rung's retained bundle is the receipt
that raises `R300_TRIANGLE_RENDER_RECEIPT_MAX_EXTENT`,
`R3V_MAX_RENDER_EXTENT`, and the advertised framebuffer and viewport
limits to that rung's extent.
