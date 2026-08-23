# R3V native attended fetched GPU-producer cell procedure

The fetched GPU-producer cell is the hardware-ladder step that proves the
device reads the application's vertex buffer object itself. The
application records three `R32G32B32A32_SFLOAT` positions into a Vulkan
buffer, binds them through `vkCmdBindVertexBuffers`, and issues one
`vkCmdDraw`; at submission the driver composes the fetched R2VB producer
ahead of the recorded consumer cell in one indirect buffer. The producer
fetches two arrays through the two-array `LOAD_VBPNTR` + `DRAW_VBUF_2`
body -- a driver-owned slot-position BO and the application's vertex BO
at its bound offset and stride -- rasterizes one point per record into
the carrier through the color backend, and the consumer fetches that same
carrier as its vertex array. Both stages ride one `DRM_RADEON_CS`, and
the producer's publication tail orders the write before the fetch.

`docs/hardware/r3v-native-attended-cell-procedure.md` carries the
boundary statement, the host preconditions, the identity freeze, the
arming conjunction, the rollback rules, and the retained-record layout;
`docs/hardware/r3v-native-attended-public-gpu-producer-procedure.md`
carries the immediate producer cell this one descends from; this
document adds only what the fetched composition changes.

## Cell identity

- Runner: `r3v_native_attended_public_gpu_producer <dir>
  --fetched[=f32_4|f32_3|f32_2]` (native build), the immediate cell's
  runner in its fetched mode: the same recording and the same three
  positions, the fetched composition at submission. Each source width is
  its own cell: `--fetched` and `--fetched=f32_4` bind 16-byte
  `R32G32B32A32_SFLOAT` records; `--fetched=f32_3` binds 12-byte
  `R32G32B32_SFLOAT` records and `--fetched=f32_2` 8-byte
  `R32G32_SFLOAT` records holding the leading components, with the fetch
  swizzle restoring `z = 0` and `w = 1`, so the three widths share the
  target oracle and differ in the stream digest alone.
- Arming digest source:
  `r3v_native_fetched_gpu_producer_arming_runner <dir> [f32_4|f32_3|f32_2]`
  composes the route through `r300_r2vb_fetched_route_reference_compose`
  for the named width (`F32_4` unnamed) without submitting and reports
  the `ib_blake3` an authorization declares, the `source_format`, the
  stream length, and the dword the producer half ends at. The runner
  evaluates the full conjunction for this cell kind and reports all three
  delivery gates, so its `armed` verdict plus its
  `route: gpu-producer-fetched` line replace the shared procedure's
  arming step. The pinned identities are
  `common/tests/r300_fetched_route_digests.h` (547 dwords, split 316 for
  every width; one digest per width).
- Digest authority: the offline composition and the submit-time
  composition are two constructions of one stream. The submit-order
  harness arm `gpu-fetched-composed` declares the offline digest, drives
  the driver's own admission, and proves through the arming gate and the
  retained `ib.bin` that the two are byte-identical, so the declared
  digest names the bytes the ioctl carries.
- Cell kind: `R3V_NATIVE_CELL_KIND_R2VB_GPU_PRODUCER_FETCHED`. The
  arming gate's geometry predicate for this kind requires the consumer's
  maximum public extent and four references: the carrier read-write, the
  color target written, the slot array (the admission's own page) and the
  source array read.
- Payload scope: the records are fetched from the bound buffer object, so
  the stream names the fetch geometry -- bound offset, stride, count --
  and the record values stay outside the digest; one arming still
  authorizes one composition. The reference records are
  `r300_tcl_bypass_triangle_vertices`, the pretransformed screen
  positions (8, 8), (56, 8), (32, 56), bound at offset zero with the
  width's record size as stride in a one-page allocation, which is the
  reference composition's geometry for that width.
- Admission domain: the delivery identity admits FP24 fixed points alone
  and the fetched route fetches in-bounds records alone; a record outside
  either refuses the submit by name before any write.
- Clear value: the 0xa5a5a5a5 sentinel, as the immediate cell.
- Recording calibration: `r3v_native_attended_public_gpu_producer <dir>
  --record-only --fetched=<width>` builds every object and records the
  command buffer on the drm-shim fixture, then stops at the recording
  boundary; the suite runs it as
  `r3v-native-fetched-gpu-producer-record-{f32_4,f32_3,f32_2}`, and the
  submit-order arms `gpu-fetched-composed{,-f32_3,-f32_2}` prove each
  width's submit-time composition equals its pinned digest.
- Allocations: the carrier is driver-owned and poisoned with
  `R300_R2VB_PRODUCER_POISON_DWORD` across its sixteen dwords before the
  ioctl; the slot BO is driver-owned, one page, holding the three
  `(v + 0.5, 0.5, 0, 1)` records; the source is the application's
  allocation; the color target is the cell's 64x64 B8G8R8A8 surface with
  the canary row past the render extent.

## Declarations

Every value below is set for the run; one missing value refuses at the
gate and consumes the attempt.

- `R3V_NATIVE_SUBMIT_HAZARD_ACCEPTED=1`
- `R3V_NATIVE_R2VB_DELIVERY_EXPERIMENTAL=1`
- `R3V_NATIVE_R2VB_GPU_DELIVERY_EXPERIMENTAL=1`
- `R3V_NATIVE_R2VB_FETCHED_PRODUCER_EXPERIMENTAL=1`
- `R3V_NATIVE_AUTHORIZED_IB_BLAKE3=<digest the fetched arming runner
  reports for the run's width>`
- `R3V_NATIVE_AUTHORIZED_KERNEL_RELEASE=<uname -r>`
- `R3V_NATIVE_AUTHORIZED_MODULE_SRCVERSION=<loaded radeon srcversion>`
- `R3V_NATIVE_MANIFEST_DIR=<fresh evidence directory>`, the same
  directory the runner takes as its argument

A producer gate closed selects the CPU route and the fetched gate closed
selects the immediate route; either submits a stream the authorization
does not name, so the runner refuses before the ioctl rather than
spending the one-shot token.

## Predictions

Recorded before the run; deviation is the finding.

1. The kernel CS parser accepts the composed stream. The offline replay
   `r300-r2vb-fetched-route-replay-<width>` reports `replay dwords=547
   relocs=4 draws=2 passed=2 verdict=ACCEPT`, and every known-bad arm --
   truncated final packet, carrier below the producer color-buffer
   bound, color target below the consumer's, slot or source array below
   the parser's offset-blind vertex-array bound, source relocation past
   the chunk table -- rejects. The fence retires and dmesg carries no
   radeon validation delta.
2. The carrier holds the twelve record dwords the delivery identity
   predicts (the CPU gather agrees), with the odd-count pad slot's four
   dwords still poisoned. The driver decides this and returns
   `VK_SUCCESS`; the read-back bytes and the expectation land beside the
   manifest as `gpu_carrier_observed.bin` and `gpu_carrier_expected.bin`.
3. The color target matches `r300_tcl_bypass_triangle_extent_oracle` at
   the maximum extent: interior covered, exterior clear, canary row
   intact -- the same bytes the immediate cell and the CPU-route cell
   deliver.

## Falsifiers

- Carrier poison across the record extent: no producer color write
  reached the carrier. With the immediate cell delivered on the same
  silicon, the first suspect is the fetched body itself -- the VAP never
  consumed the two arrays (PSC tuple, `VAP_VTX_SIZE`, `LOAD_VBPNTR`
  pointers or strides) -- and the second is the relocation binding of the
  slot or source array. The driver quarantines the capability and reports
  device loss; the run is not repeated on the same boot.
- Carrier written to values other than the expectation: the fetch landed
  but the record reached the US reordered or truncated -- the source
  element's `PROG_STREAM_CNTL_EXT` swizzle is the one register the fetched
  pass changes against the immediate pass, so a lane permutation names
  it; a value divergence beyond permutation names the fetch stride or
  offset. The retained observed bytes against the expected bytes localize
  the stage.
- Carrier correct and target still holding the load-op clear: the
  consumer did not fetch what the producer wrote; the publication tail's
  ordering between the color-backend write and the vertex fetch of the
  same buffer object is the first suspect.
- Target differing from the analytic triangle with the carrier correct:
  the consumer fetch read the carrier at the wrong address, stride, or
  format.
- A disturbed canary row: the draw wrote past the render extent, and the
  cell stops until the overrun is explained.
- A dmesg validation delta or an unretired fence: kernel-boundary
  finding; the wedged state is preserved for inspection and the host is
  cold-power-cycled rather than resubmitted.

## Verdict

Two oracles decide the run and they are recorded separately, as in the
immediate cell: the driver's carrier read-back verdict
(`r3v_native_deferred_draw_verify_gpu_producer`) and the runner's target
verdict (`r300_tcl_bypass_triangle_extent_oracle`). `TARGET_DELIVERED`
requires the completion status, both oracles, and the canary together.

## Retained record

`fetched_route_outcome.json` carries the verdict, the route name, the
submit result, the queue status, the stream digest, and every oracle
field. `color.bin`, `gpu_carrier_observed.bin`, and
`gpu_carrier_expected.bin` carry the target and the carrier read-back with
its expectation. The `steinmarder-r300` bundle seals these beside the
run's dmesg delta, the loaded-module identity, and the arming report;
the retained digest becomes that width's fetched-route silicon identity.
The F32_4 cell is retained as
`r3v-native-fetched-gpu-producer-route-first-delivery-rs482`; the F32_3
and F32_2 cells run the same procedure with their width named on the
runner, the arming runner, and the declared digest, one evidence directory
and one arming per width.
