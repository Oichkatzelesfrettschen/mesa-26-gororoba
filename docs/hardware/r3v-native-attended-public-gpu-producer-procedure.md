# R3V native attended public GPU-producer cell procedure

The public GPU-producer cell is the hardware-ladder step that proves the
device produces the vertex data it consumes. The application records
three `R32G32B32A32_SFLOAT` positions into a Vulkan buffer, binds them
through `vkCmdBindVertexBuffers`, and issues one `vkCmdDraw`; at
submission the driver composes the R2VB producer pass over those records
ahead of the recorded consumer cell in one indirect buffer, so the
producer rasterizes each record into the carrier through the color
backend and the consumer fetches that same buffer object as its vertex
array. The immediate producer accepts clip/NDC positions and applies the
Vulkan viewport before emission. The fetched producer has no viewport
stage in its VAP fetch path and therefore accepts explicitly
pretransformed window-space positions. Both stages ride one
`DRM_RADEON_CS`, and the producer's publication tail is what orders the
write before the fetch.

`docs/hardware/r3v-native-attended-cell-procedure.md` carries the
boundary statement, the host preconditions, the identity freeze, the
arming conjunction, the rollback rules, and the retained-record layout;
this document adds only what the public route changes.

## Cell identity

- Runner: `r3v_native_attended_public_gpu_producer` (native backend
  build), taking the evidence directory as its one argument.
- Arming digest source:
  `r3v_native_public_gpu_producer_arming_runner` composes the route
  through `r300_r2vb_public_route_reference_compose` without submitting
  and reports the `ib_blake3` an authorization declares, the stream
  length, and the dword the producer half ends at. The same runner
  evaluates the full conjunction for this cell kind and reports both
  delivery gates, so its `armed` verdict plus its `route: gpu-producer`
  line replace the shared procedure's arming step.
- Digest authority: the offline composition and the submit-time
  composition are two constructions of one stream, and the
  `r3v-native-public-surface` harness compares them dword for dword
  under the driver's own admission, so the declared digest names the
  bytes the ioctl carries.
- Cell kind: `R3V_NATIVE_CELL_KIND_R2VB_GPU_PRODUCER_PUBLIC`. The arming
  gate's geometry predicate for this kind requires the consumer's
  maximum public extent, two references, and the carrier relocation
  carrying the GTT domain for both read and write.
- Payload scope: the immediate records travel as literal `DRAW_IMMD_2`
  body dwords after the viewport projection, so one arming authorizes one
  payload. The reference NDC records are (-0.75, -0.75), (0.75, -0.75),
  and (0, 0.75), each with z = 0 and w = 1; the 64x64 viewport projects
  them to the pretransformed screen positions (8, 8), (56, 8), and
  (32, 56). The fetched reference records use those window-space values
  directly. Both forms therefore place the producer half byte-identical
  to `r300_r2vb_producer_reference_emit` and the consumer half
  byte-identical to the qualified cell.
- Clear value: the recording admits one load-op clear, the 0xa5a5a5a5
  sentinel the target oracle reads as its exterior and canary value, so
  the run passes `(float)0xa5 / 255.0f` in all four channels and any
  other color refuses at `vkCmdBeginRenderPass`.
- Recording calibration: `r3v_native_attended_public_gpu_producer
  <dir> --record-only` builds every object and records the command
  buffer on the drm-shim fixture, then stops at the recording boundary.
  The suite runs it as `r3v-native-public-gpu-producer-record`, so a
  sequence the driver's recording contract refuses fails there rather
  than on an authorized hardware attempt.  The recording mode reaches no
  `DRM_RADEON_CS`, and is the one mode that runs under an interposer. Vulkan
  allocation and mapping can still issue GEM create or map ioctls, and a
  recording validation failure emits its refusal verdict.
- Allocations: the carrier is driver-owned and poisoned with
  `R300_R2VB_PRODUCER_POISON_DWORD` across its sixteen dwords before the
  ioctl; the color target is the cell's 64x64 B8G8R8A8 surface with the
  canary row past the render extent.

## Declarations

Every value below is set for the run; one missing value refuses at the
gate and consumes the attempt.

- `R3V_NATIVE_SUBMIT_HAZARD_ACCEPTED=1`
- `R3V_NATIVE_R2VB_DELIVERY_EXPERIMENTAL=1`
- `R3V_NATIVE_R2VB_GPU_DELIVERY_EXPERIMENTAL=1`
- `R3V_NATIVE_AUTHORIZED_IB_BLAKE3=<digest the arming runner reports>`
- `R3V_NATIVE_AUTHORIZED_KERNEL_RELEASE=<uname -r>`
- `R3V_NATIVE_AUTHORIZED_MODULE_SRCVERSION=<loaded radeon srcversion>`
- `R3V_NATIVE_MANIFEST_DIR=<fresh evidence directory>`, the same
  directory the runner takes as its argument

Either delivery gate closed selects the CPU route, which submits the
consumer alone under an authorization naming the composed stream, so the
runner refuses before the ioctl rather than spending the one-shot token.

## Predictions

Recorded before the run; deviation is the finding.

1. The kernel CS parser accepts the composed stream. The offline replay
   already reports `replay dwords=547 relocs=2 draws=2 passed=2
   verdict=ACCEPT`, and every known-bad arm -- truncated final packet,
   carrier below the producer's color-buffer bound, color target below
   the consumer's -- rejects. The fence retires and dmesg carries no
   radeon validation delta.
2. The carrier holds the twelve transformed window-space record dwords
   the CPU gather and viewport projection predict, with the odd-count pad
   slot's four dwords still poisoned. The driver decides this and returns
   `VK_SUCCESS`; the read-back bytes and the expectation land beside the
   manifest as `gpu_carrier_observed.bin` and `gpu_carrier_expected.bin`.
3. The color target matches `r300_tcl_bypass_triangle_extent_oracle` at
   the maximum extent: interior covered, exterior clear, canary row
   intact -- the same bytes the CPU-route triangle cell delivers.

## Falsifiers

- Carrier poison across the record extent: the producer executed no
  color write that reached the carrier. The driver quarantines the
  capability and reports device loss; the run is not repeated on the
  same boot.
- Carrier written to values other than the expectation: the write landed
  but the shaded value is wrong -- interpolator routing, US program
  semantics, C4_32_FP conversion, or FP24 narrowing. The retained
  observed bytes against the expected bytes localize the stage.
- Carrier correct and target still holding the load-op clear: the
  consumer did not fetch what the producer wrote. The publication tail's
  ordering between the color-backend write and the vertex fetch of the
  same buffer object is the first suspect, and this is the failure class
  the route exists to test.
- Target differing from the analytic triangle with the carrier correct:
  the fetch read the carrier at the wrong address, stride, or format;
  the relocation payload and `LOAD_VBPNTR` state decide which.
- A disturbed canary row: the draw wrote past the render extent, and the
  cell stops until the overrun is explained.
- A dmesg validation delta or an unretired fence: kernel-boundary
  finding; the wedged state is preserved for inspection and the host is
  cold-power-cycled rather than resubmitted.

## Verdict

Two oracles decide the run and they are recorded separately. The
carrier verdict is the driver's: `r3v_native_deferred_draw_verify_gpu_producer`
compares the read-back against the CPU gather, retains both byte
strings, and on divergence quarantines the capability on the device and
returns `VK_ERROR_DEVICE_LOST`, which the runner classifies
`CARRIER_DIVERGED`. The target verdict is the runner's:
`r300_tcl_bypass_triangle_extent_oracle` over the retained `color.bin`.
`TARGET_DELIVERED` requires the completion status, both oracles, and the
canary together. A parser acceptance or a retired fence alone proves
transport; only the two byte strings prove the route.

## Retained record

`public_route_outcome.json` carries the verdict, the submit result, the
queue status, the stream digest, and every oracle field.
`color.bin` carries the full 64x65-pixel target footprint.
`gpu_carrier_observed.bin` and `gpu_carrier_expected.bin` carry the
carrier read-back and its expectation, sixteen dwords each. The
`steinmarder-r300` bundle seals these beside the run's dmesg delta, the
loaded-module identity, and the arming report.
