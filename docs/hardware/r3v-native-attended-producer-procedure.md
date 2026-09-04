# R3V native attended producer-cell procedure

The producer-only cell is the hardware-ladder step that proves the R2VB
producer pass writes the carrier, with the consumer left out: the cell
poisons a GTT carrier, executes one producer pass (first-draw state
contract, carrier retarget to C4_32_FP, RS routing, the compiled
varying-passthrough US program, and the embedded POINTS draw), waits for
retirement, maps the carrier on the CPU, and compares. Re-ingest and the
visible draw stay out of this cell; their correctness is a separate
ladder step with its own evidence class.

`docs/hardware/r3v-native-attended-cell-procedure.md` carries the
boundary statement, the host preconditions, the identity freeze, the
arming conjunction, the rollback rules, and the retained-record layout;
the producer procedure adds the producer cell's distinct carrier and oracle
contract.

## Cell identity

- Runner: `r3v_native_attended_producer` (native backend build).
- Recorder: `r3v_native_record_r2vb_producer`; the installed IB is
  byte-identical to `r300_r2vb_producer_reference_emit`, and the arming
  digest binds to it.
- Arming digest source: `r3v_native_producer_arming_runner` builds the
  reference pass without submitting and reports the `ib_blake3` an
  authorization declares; the same runner evaluates the full conjunction
  for the producer cell kind, so its `armed` verdict replaces the
  triangle runner's in the shared procedure's arming step.
- Cell kind: `R3V_NATIVE_CELL_KIND_R2VB_PRODUCER`. The arming gate's
  geometry predicate for this kind requires the carrier allocation to
  equal the reference layout footprint exactly and the one relocation
  to carry the GTT domain for both read and write; the 64x64 extent
  freeze of the render-target cells does not apply.
- Carrier: one GTT buffer object at relocation slot 0, prefilled with
  `R300_R2VB_PRODUCER_POISON_DWORD` over the whole allocation and
  cache-published before submission.

## Predictions

Recorded before the run; deviation is the finding.

1. The kernel CS parser accepts the producer stream (the offline replay
   reports `verdict=ACCEPT relocs=1 draws=1`). Parser acceptance is a
   command-stream admissibility result. The attended run separately proves
   live transport through a successful `DRM_RADEON_CS`, a retired fence, and
   an empty dmesg validation delta.
2. The expected extent of the carrier holds the dwords
   `r300_r2vb_producer_reference_expected` computes -- the delivery
   identity over the reference records -- byte-exact.
3. Every dword past the expected extent still holds the poison.

## Exact submit-object parser replay

`src/amd/r300/vulkan/tests/run_producer_submit_object_replay.sh` drives
`r3v_native_producer_cell_harness open` through the shim and replays the
retained submit object with `R3V_CS_TRACK_REPLAY_TOOL`. The retained final
relocation list contains exactly two native entries: the 64-byte producer
carrier at index 0 and the four-byte completion BO at index 1. The checker
binds both entries' handles, domains, sizes, roles, chunk descriptors, and
BLAKE3 digests to `submit_manifest.json` before the parser consumes
`ib.bin`.

The known-good replay requires one accepted draw with two relocations. The
same IB rejects an undersized `VAP_VTX_SIZE`, a 32-byte carrier, and a
truncated final packet. When `R3V_KERNEL_REPLAY_TOOL` is available, the
narrow TCL-bypass model separately reports the immediate draw as declined;
that predicate does not replace CS-track acceptance. Offline parser
acceptance proves command-stream admissibility only. A successful ioctl,
retired completion, and empty dmesg validation delta remain separate live
transport observations for the attended procedure.

## Falsifiers

- Poison across the whole expected extent: the producer executed no
  color write that reached the carrier; the US program, RS routing, or
  the color-backend retarget failed as a unit. The retained IB and
  registers decide which; the run is not repeated on the same boot.
- Poison in only part of the expected extent: the producer wrote some
  slots but left other slots unwritten. The runner records
  `CARRIER_PARTIAL_UNWRITTEN`; the poisoned dwords remain direct evidence
  of missing production, and the non-poison mismatches remain a separate
  value question.
- Wrong bytes with no poison in the expected extent: the write landed but
  the shaded value is wrong -- interpolator routing, US program semantics,
  format conversion in the C4_32_FP path, or FP24 narrowing. The byte
  pattern against the expected dwords localizes the stage and the runner
  records `CARRIER_MISMATCH`.
- Poison disturbed in the rounded tail past the expected extent: the
  three-record layout rounds to a four-pixel row, so the disturbed dwords
  are in-row padding rather than bytes outside the allocation. The result
  names a slot-addressing or coverage failure; it does not establish an
  out-of-row write.
- A dmesg validation delta or an unretired fence: kernel-boundary
  finding; the wedged state is preserved for inspection and the host is
  cold-power-cycled rather than resubmitted.

## Verdict

`r300_r2vb_producer_carrier_check` computes the verdict from the mapped
carrier: expected-extent compare, tail poison retention, and refusal of
a poison value colliding with any expected dword, since that pairing
leaves the unwritten case undecidable. `poison_dwords == expected_dwords`
means `CARRIER_UNWRITTEN`; a nonzero smaller poison count means
`CARRIER_PARTIAL_UNWRITTEN`; zero poison with mismatches means
`CARRIER_MISMATCH`. The runner keeps these carrier classes after the
transport checks. Offline parser acceptance proves command-stream
admissibility, while a successful ioctl and retired fence prove live
transport; neither class stands in for the other, and only the carrier
bytes prove production.

## FP24 boundary-sweep stream

The sweep is the producer cell re-armed with
`r300_r2vb_producer_fp24_sweep_records` in place of the reference
triangle's records: twelve components on the edges of the
delivery-admission lattice (+0, the minimum normal magnitude and its
first step, the 1.0 neighborhood's mantissa extremes and exponent-carry
neighbor, a mid-range multi-bit mantissa, and the maximum-exponent
magnitudes up to the largest value admitted by the R2VB identity route).
The generic FP24 storage model has a wider upper exponent bin; the count equals the
reference count, so the cell kind, carrier geometry, poison contract,
outcome classes, and every arming factor except the digest carry over.
Both runners take the selector `fp24-sweep` as a second argument --
`r3v_native_producer_arming_runner <dir> fp24-sweep` reports the sweep
digest an authorization declares, and
`r3v_native_attended_producer <dir> fp24-sweep` submits it -- and each
stream's digest authorizes only its own bytes. The predictions
specialize: a mismatch confined to specific lanes falsifies the
admission window at that edge (interpolator or US narrowing off the
modeled lattice) rather than the transport, and the mismatching lane's
byte pattern against its expected dword localizes the stage.

## FP24 upper-ceiling bisection stream

The executed sweep left the identity-delivery ceiling bracketed between
999.0 (delivered exact) and 2^65 (delivered with the exponent
decremented). The `fp24-bisect` selector re-arms the cell with
`r300_r2vb_producer_fp24_bisect_records`: 2^32, 2^48, 2^56, then every
exponent from 2^58 through 2^64, with the maximum mantissa at 2^63
(`0x5f7fff80`) and 2^64 (`0x5fffff80`). Prediction, recorded before the
run: lanes at or below the silicon ceiling deliver byte-exact and lanes
above it deliver with the exponent field decremented, so one run brackets
the ceiling to within a lane pair; twelve exact lanes name `0x5fffff80`
as the smallest viable ceiling and move the bracket above it, and a
deviation of any other shape (a wrong mantissa, a non-monotone
exact/wrong pattern) falsifies the exponent-window hypothesis itself.
The verdict for a partial delivery with any poisoned expected dword is
`CARRIER_PARTIAL_UNWRITTEN`; the retained carrier bytes carry the per-lane
result. A partial delivery whose expected extent contains no poison remains
`CARRIER_MISMATCH` because every mismatching dword has a produced value.

## Executed bisection run

The `fp24-bisect` stream ran on RS485M on 2026-08-14 from main
`3bd7c9fc002` (IB blake3
`6df52da2e124dcf709d1eca12034087857f0ada3810f0616f00d2c0211126040`,
313 dwords) and returned `CARRIER_DELIVERED`: all twelve lanes through
`0x5fffff80` byte-exact, tail poison intact, empty dmesg delta, fence
retired. Combined with the sweep's halved `0x60000000` lane, the
identity-delivery ceiling is `0x5fffff80` exactly: the RS482 R2VB identity
route resolves as s1e7m16 with bias 62 and the top exponent field reserved,
so `R300_R2VB_FP24_IDENTITY_MAX_F32_BITS` carries `0x5FFFFF80` while the
generic `R300_FP24_MAX_FINITE_F32_BITS` remains `0x607FFF80` for stored and
temporary FP24 values. The sweep table's maximum-exponent lanes sit at the
route-specific lattice edge. The
retained record lives in the `steinmarder-r300` bundle
`results/r3v-native-fp24-bisect-ceiling-rs482/`.

## Executed reference run

The reference cell ran on RS485M on 2026-08-14 from main `cb3d078ed41` (IB blake3
`680dfd6f73fe336a87669cfe4da601e0e5f29b25f78b600de64f91f2f35612dc`,
313 dwords) and returned `CARRIER_DELIVERED`: all three predictions
held (`expected_pass=1 tail_poison_pass=1 mismatched=0
tail_disturbed=0`, slot 0 byte-exact to the delivery identity, empty
dmesg delta, fence retired). The retained record lives in the
`steinmarder-r300` bundle
`results/r3v-native-producer-carrier-delivery-rs482/`.

## Executed sweep run

The `fp24-sweep` stream ran on RS485M on 2026-08-14 from main
`cd28064499a` (IB blake3
`5e1cf1dc5a8fc5750783f56583ae36766afac63101b67609fee2871f89eca0bf`,
313 dwords) and returned a partial delivery: the fence retired, the
tail kept poison, the zero/min-normal/1.0-neighborhood/2.0/999.0 lanes
delivered byte-exact, and the three top-exponent lanes each delivered
with the exponent field one below the expectation
(`0x60000000 -> 0x5f800000`, `0x607fff00 -> 0x5fffff00`,
`0x607fff80 -> 0x5fffff80`, mantissa preserved). Known: values in the
generic upper exponent bin remain representable in storage but values in
that bin are outside `r300_r2vb_fp24_identity_admits` and deliver halved,
so the generic `R300_FP24_MAX_FINITE_F32_BITS` must not stand in for the
route-specific identity ceiling.
Hypothesized: the US source-read exponent window ends one bin below
the modeled bound; a bisection sweep between `0x4479c000` (999.0,
delivered exact) and `0x60000000` locates the true ceiling. The run
predated the delivered-but-wrong outcome class, so the retained
verdict string reads `CARRIER_UNWRITTEN`; the retained bytes in the
`steinmarder-r300` bundle
`results/r3v-native-fp24-sweep-top-bin-halving-rs482/` carry the
mismatch evidence. Later runs name a mixed-poison carrier
`CARRIER_PARTIAL_UNWRITTEN`; `CARRIER_MISMATCH` remains the class for
mismatches whose expected extent contains no poison.

## Pending requalification

The tree now programs the VAP_VF_MAX_VTX_INDX/VAP_VF_MIN_VTX_INDX pair
in one PACKET0 run, so the first-draw contract and the bare prefix each
grew and the cell's IB digest changed: the current emission is
316 dwords with IB blake3
`2c6ad55886a648ccd0bbf98939503c6674e2a72837d9913e854021c9921f915a`.
The executed-run record above remains the retained evidence for its own
byte stream; the paired-index emission is requalified by a fresh
attended cell armed on the new digest.
