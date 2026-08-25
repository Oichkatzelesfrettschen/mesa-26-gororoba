# R3V native attended render-shape procedure

This document is the procedure for one physically attended session on
the RS482 host that submits the TCL-bypass triangle cell over declared
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

The composed arm is the `dEQP-VK.api.smoke.triangle` target shape,
whose magenta constant carries red equal to blue, so it witnesses
extent, pitch, and constant together without separating a lane-order
effect from that equality. The composed-asym arm carries red without
blue, so it is the four-class interaction witness: a lane-order defect
that the composed arm's symmetric constant cannot expose. The arms run
in table order; a falsifier on a single-parameter arm stops the
session before the composed arms, so a composed deviation never has to
be decomposed after the fact.

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
shape's footprint (`pitch * (height + 1) * 4` bytes), the arming report
with the shape line, and the runner's console with the `[shape]` and
`[oracle]` lines. A session bundle relates the six arms to the
program-status row they close.
