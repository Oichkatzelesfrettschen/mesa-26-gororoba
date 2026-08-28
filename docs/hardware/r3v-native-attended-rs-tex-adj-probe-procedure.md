# R3V native attended rasterizer interpolation probe procedure

The rasterizer interpolation probe classifies one rasterizer control word on RS482 silicon. Two
render passes record the varying TEX0 cell over one carrier whose three vertices hold strongly
unequal reciprocal clip W; the control pass carries the varying cell's exact bytes, and the
candidate pass carries the same bytes with one control word changed. The AMD R3xx 3D Registers
document defines `RS_INST.TEX_ADJ` (bit 22) as the choice between real and adjusted pixel centers
for texture-coordinate sampling, and `GB_SELECT.W_SELECT` (bit 4, value 1 selecting 1.0) as the
source of the outgoing 1/W "used to disable perspective correct colors/textures"; neither carries a
retained silicon classification on RS482, so the probe treats each as an unidentified control word
until a full pixel census against the registered interpolation models names it
(`r300_rs_tex_adj_probe.h`). The mechanism is named for the bit under test, not for NoPerspective:
only a census that lands on the affine model promotes a word into a direct NoPerspective contract.

`docs/hardware/r3v-native-attended-cell-procedure.md` carries the boundary statement, host
preconditions, substrate admission table, identity freeze, arming conjunction, rollback rules, and
retained-record layout; `docs/hardware/r3v-native-attended-public-flat-color0-two-draw-procedure.md`
carries the two-pass public form this runner mirrors. This document adds only what the probe
changes.

## Cell identity

- Runner: `r3v_native_attended_rs_tex_adj_probe <dir> [--record-only] [--waiver <path>]
  [--candidate tex-adj|w-select]` (native build); the evidence directory is the required argument
  and `tex-adj` the default candidate. `--record-only` records the command buffer and stops at the
  recording boundary after the digest, route, and state checks; the recording contract is
  calibrated on the drm-shim under `r3v-native-rs-tex-adj-probe-record` and
  `r3v-native-rs-w-select-probe-record`.
- Arming digest source: `r3v_native_arming_runner --multi-pass-rs-tex-adj-probe <dir>` (or
  `--multi-pass-rs-w-select-probe`) emits `r3v_native_multi_pass_public_rs_tex_adj_probe_reference`
  through `r300_tcl_bypass_triangle_clip_space_multi_pass_emit`: pass 0 the control varying cell,
  pass 1 the candidate, 472 IB dwords for either candidate; cell blake3 `b8eb7960` for `TEX_ADJ`
  (dword 424, `RS_INST_0` `0x00000008` to `0x00400008`) and `32d547e9` for `W_SELECT` (dword
  245, `GB_SELECT` `0x00000000` to `0x00000010`). The runner prints the same digest on its
  `[shape]` line before it creates the instance, and its `[record]` line names the one differing
  dword and the register whose packet carries it.
- Route: the control pipeline links the Smooth fragment fixture, the candidate pipeline the
  `noperspective` fixture, over one vertex module that passes position through. Under exactly one
  open probe gate -- `R3V_NATIVE_RS_TEX_ADJ_PROBE=1` or `R3V_NATIVE_RS_W_SELECT_PROBE=1` -- the
  NoPerspective interface takes the named candidate (`r3v_rs_probe_candidate_select`), and every
  other interface, gate state, or route takes the control. The runner refuses ahead of any object
  creation unless the gate for its selected candidate is open alone, and refuses under
  `R3V_NATIVE_FLAT_REPLICATION_PINNED` or any `R3V_NATIVE_R2VB_*_EXPERIMENTAL` gate.
- Mechanism held constant across the passes: CPU delivery, TEX0 as source and destination
  (`RS_IP_0` texture pointer 0, `RS_INST_0` `TEX_CN_WRITE` into US input 0), the pass-through
  fragment binary, render format, viewport, scissor, raster state, a clip-accepted triangle, the
  carrier bytes, and the relocation geometry. The candidate differs from the control in exactly one
  dword: `RS_INST_0` gains `TEX_ADJ` (bit 22), or `GB_SELECT` in the first-draw contract gains
  `W_SELECT` (bit 4). The `[state]` line proves it: the control plan's per-draw stream check admits
  pass 0 and refuses pass 1, the candidate plan's admits pass 1 and refuses pass 0, and the two
  pass streams differ at one index whose packet names the candidate register.
- Carrier: window position at the reference extent with z = 0, the reciprocal clip W lanes 1, 1/4,
  and 1/2 (the R300 software-transformed vertex convention, `VTX_W0_FMT` clear), and the TEX0
  payload (s, t, r, q) = (0.25, 0.75, 0.5, 1), (0.4, 0.1, 0.3, 0.5), (0.1, 0.2, 0.05, 0.25): s, t,
  r <= q at every vertex keeps a projective s/q reading inside the UNORM8 range, and q varies per
  vertex so a projective adjustment cannot masquerade as affine interpolation. The public runner
  feeds the clip-space form (x w, y w, 0, w) and the driver's CPU projection lands these records;
  the `[witness]` step reads each pass's carrier back and requires the TEX0 payload verbatim and
  the reciprocal W lanes pairwise distinct in the 1 : 1/4 : 1/2 ratio.

## Registered models

Each model is a function of the record triple at a pixel center, evaluated in binary64 and
converted to UNORM8 (`r300_rs_tex_adj_probe_model_value`):

| Model | Value at a pixel center | Names |
| --- | --- | --- |
| perspective | sum(l_i a_i W_i) / sum(l_i W_i), W the reciprocal clip W | perspective-correct interpolation |
| affine | sum(l_i a_i) | framebuffer-linear interpolation, the Vulkan NoPerspective value |
| projective-q | perspective (s, t, r, q) with s, t, r divided by q | a projective texture-coordinate adjustment |
| shifted-center | perspective at a center shifted by a half pixel in x, y, or both | the documented adjusted-pixel-center reading |
| unchanged | the control image's dword | no observable effect |

A pixel is judged when it lies at least two pixels inside the triangle and the perspective and
affine predictions differ by at least five UNORM8 quanta in some channel, so a tolerance of two
quanta per channel admits at most one of them; the census counts, per model, the judged pixels
within tolerance, and records the largest deviation. At the reference extent 882 pixels are
judged. The shifted-center row counts the three positive half-pixel shifts alone, so it is
independent of the perspective row; a true half-pixel shift separates from perspective by up to 5
(x), 4 (y), and 8 (both) quanta over the judged pixels against the tolerance of 2, and the
calibration test names it `shifted-center`. A shift the tolerance absorbs lands in
`perspective-perturbed`; a negative shift lands in `unclassified`. The classification names a model only when it
alone matches every judged pixel: `perspective` (no control supplied), `unchanged` (every judged
dword equals the control's), `perspective-perturbed` (within tolerance of perspective while some
dword differs from the control), `affine`, `projective-q`, `shifted-center`, or `unclassified`
with the counts as the finding.

## Predictions

Recorded before the submission, in the runner's `[predict]` and `[models]` lines and the three
expected images (`expected_perspective.bin`, `expected_affine.bin`, `expected_projective_q.bin`):

1. The control pass classifies as `perspective`: the premise. A control that fails it ends the
   attempt as a finding about the varying cell, and the candidate census carries no classification.
   The first hypothesis for such a failure is `RS_COUNT.HIRES_EN`, documented as high-resolution
   texture-coordinate output when q equals 1, while the carrier drives q to 1, 1/2, and 1/4; the
   bit is identical in both passes, so it cannot explain a control-to-candidate delta.
2. The candidate pass classifies as exactly one of `affine`, `projective-q`, `shifted-center`,
   `perspective-perturbed`, `unchanged`, or `unclassified`. The document-derived expectation for
   `TEX_ADJ` is `shifted-center` (a positive half-pixel shift), with `perspective-perturbed` for a
   sub-tolerance shift and `unclassified` for a negative one; for `W_SELECT` it is `affine`. A census that lands elsewhere is the finding.
3. Both streams `PASS` the kernel's pinned TCL-bypass width check and `ACCEPT` under CS tracking,
   holding every negative control (`r300-rs-tex-adj-probe-kernel-replay`,
   `r300-rs-tex-adj-probe-cs-track-replay`).
4. `vkQueueSubmit` returns 0, the dmesg delta is empty, and the watchdog counter returns to
   `inactive` after the guarded interval.

Only an `affine` classification promotes the word into a direct NoPerspective contract; any other
result sends R3V's NoPerspective lowering to the (a w, w) carrier with a reciprocal-multiply in the
fragment program.

## Calibrated mutations

Each mutation refuses ahead of silicon (`r300-rs-tex-adj-probe`, `r3v-interpolation-lowering`,
and the record tests):

- Bit written after the draw: the candidate check fails the pass whose `RS_INST_0` ahead of its
  draw holds the control word.
- Bit set on the wrong RS instruction: `rs_instruction = 1` refuses validation; the plan form
  writes `RS_INST_1`, an instruction `RS_INST_COUNT` 0 never runs, and the candidate check fails.
- Candidate pass inheriting the control state, and control pass inheriting the candidate state:
  each fails at its own draw under its own plan.
- RS source changed from TEX0: `R300_RS_TEX_ADJ_PROBE_SOURCE_COLOR0` refuses validation and the
  candidate check fails.
- Carrier W values equalized: the discriminator collapses to zero judged pixels.
- Q made constant: the projective model coincides with perspective and the census names neither.
- Interface changed from NoPerspective to Smooth while retaining candidate state: the Smooth
  interface selects the control under the open gate, so candidate state cannot outlive the
  interface; both gates open select the control as well.

## Retained record

The shared procedure's record plus `control_target.bin`, `candidate_target.bin`, both carrier
readbacks, the three expected images, and `run.txt` carrying every printed line: `[shape]`,
`[route]`, `[record]`, `[state]`, `[predict]`, `[models]`, `[witness]`, both `[census]` lines with
every count, and `[classification] <register>=<name>`. The classification line is a statement
about the bit, not a feature claim.
