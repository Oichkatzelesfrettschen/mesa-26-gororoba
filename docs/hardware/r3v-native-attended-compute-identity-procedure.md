# R3V native attended compute identity carrier cell procedure

The compute identity carrier cell is the hardware-ladder step that proves
the identity verb's GPU route on silicon: a Vulkan compute dispatch whose
storage buffers the device reads and writes itself. The application binds
sixty-four words (the small integers 0..63 as binary32, FP24 fixed points)
in one storage buffer and a seeded output in another, creates the
reference identity-map kernel (`shaders/r3v_reference_identity_map.comp`,
`out_words[i] = in_words[i]` over a 64-wide workgroup), binds both through
one set-0 descriptor set, and issues one `vkCmdDispatch(1, 1, 1)`. Under
the compute gate and the identity verb's gate the driver's admission
installs the compute identity carrier pass -- the fetched R2VB producer
alone, the input buffer as its F32_4 source array and the output buffer as
its C4_32_FP slot row, sixteen records -- records the CPU bit copy as the
oracle, submits one `DRM_RADEON_CS` over three relocations, and after the
completion wait reads the output back against the oracle.

`docs/hardware/r3v-native-attended-cell-procedure.md` carries the
boundary statement, the host preconditions, the identity freeze, the
arming conjunction, the rollback rules, and the retained-record layout;
`docs/hardware/r3v-native-attended-fetched-gpu-producer-procedure.md`
carries the fetched producer pass this cell is built from; this document
adds only what the compute cell changes.

## Cell identity

- Runner: `r3v_native_attended_compute_identity <dir>` (native build),
  taking the evidence directory as its one argument.
- Arming digest source: `r3v_native_arming_runner --compute-identity
  <dir>` emits the reference pass without submitting, evaluates the
  arming conjunction for this cell kind, and reports the `ib_blake3` an
  authorization declares; the attended runner prints the same digest as
  `cell compute-identity-carrier ib_dwords=316 ib_blake3=...` before it
  creates the instance. `r3v-native-arming-runner-compute-identity-ib-identity`
  pins the runner's emission byte-identical to
  `r300_compute_identity_carrier_manifest`, and the gpu-route harness arm
  `composed` pins the recorded command buffer's digest to
  `R300_COMPUTE_IDENTITY_CARRIER_IB_BLAKE3`
  (`common/tests/r300_compute_identity_carrier_digests.h`), so the declared
  digest names the bytes the ioctl carries.
- Cell kind: `R3V_NATIVE_CELL_KIND_COMPUTE_IDENTITY_CARRIER`. The arming
  gate's geometry predicate for this kind requires three references: the
  output written (GTT), the slot array (the admission's own page) and the
  input array read (GTT).
- Stream: the fetched producer's target prologue with the output buffer
  as the C4_32_FP color target at offset zero and pitch sixteen, the
  varying-passthrough US block, the two-array fetched body (`LOAD_VBPNTR`
  + `DRAW_VBUF_2`: the slot array at offset zero and the input array at
  offset zero, stride 16, sixteen records), and the publication tail; 316
  dwords.
- Kernel-parser replay: `r300-compute-identity-carrier-cs-track-replay`
  replays the pass through `replay_r300_cs_track` (`replay dwords=316
  relocs=3 draws=1 passed=1 verdict=ACCEPT`) with the truncated-packet,
  output-bound, and slot and input vertex-array-bound known-bad arms
  rejecting.
- Payload: the input words are the integers 0..63 as binary32; the
  driver's oracle is their bit copy, and the FP24 host model agrees with
  it (every word is a fixed point). The output is seeded with
  `0x5c5c5c5c`, which no input word equals.
- Recording calibration: `r3v_native_attended_compute_identity <dir>
  --record-only` runs on the drm-shim fixture as
  `r3v-native-compute-identity-record`.
- Delivery route: the identity verb's GPU route; the runner refuses a
  closed verb gate or an open `R3V_NATIVE_R2VB_*_EXPERIMENTAL` gate before
  the ioctl.

## Declarations

Every value below is set for the run; one missing value refuses at the
gate and consumes the attempt.

- `R3V_NATIVE_SUBMIT_HAZARD_ACCEPTED=1`
- `R3V_NATIVE_COMPUTE_QUEUE_EXPERIMENTAL=1`
- `R3V_NATIVE_COMPUTE_IDENTITY_GPU_EXPERIMENTAL=1`
- `R3V_NATIVE_AUTHORIZED_IB_BLAKE3=<digest r3v_native_arming_runner
  --compute-identity reports>`
- `R3V_NATIVE_AUTHORIZED_KERNEL_RELEASE=<uname -r>`
- `R3V_NATIVE_AUTHORIZED_MODULE_SRCVERSION=<loaded radeon srcversion>`
- `R3V_NATIVE_MANIFEST_DIR=<fresh evidence directory>`, the same
  directory the runner takes as its argument
- `R3V_NATIVE_R2VB_DELIVERY_EXPERIMENTAL`,
  `R3V_NATIVE_R2VB_GPU_DELIVERY_EXPERIMENTAL`, and
  `R3V_NATIVE_R2VB_FETCHED_PRODUCER_EXPERIMENTAL` unset

## Predictions

If the route is correct, the run reports `verdict: TARGET_DELIVERED`: the
submit returns `VK_SUCCESS` with queue status `COMPLETED`, the observed
route is `compute-identity-carrier`, the driver's read-back matches the
oracle (`gpu_compute_observed.bin` equals `gpu_compute_expected.bin`),
and the runner's `compute_output.bin` equals `compute_input.bin` word for
word. The dmesg delta is zero.

## Falsifiers

- `OUTPUT_MISMATCH` with `output_untouched=1`: the pass retired without
  the color backend writing the output -- the carrier retarget, the
  fetch, or the slot positions did not address the output row; the
  retained `gpu_compute_observed.bin` holds the seed.
- `OUTPUT_MISMATCH` with `output_untouched=0`: the device wrote the row
  with other bytes -- a fetch swizzle, pitch, or FP24 narrowing defect
  the host model did not predict; the observed bytes name it.
- `SUBMISSION_REFUSED`: an admission refusal or a closed gate; the
  runner's console names it and no ioctl ran.
- A nonzero dmesg delta or a lockup ends the boot under the shared
  procedure's rollback rules.

## Retained record

The shared procedure's record plus `compute_input.bin`,
`compute_output.bin`, `gpu_compute_observed.bin`,
`gpu_compute_expected.bin`, and `compute_identity_outcome.json`.
