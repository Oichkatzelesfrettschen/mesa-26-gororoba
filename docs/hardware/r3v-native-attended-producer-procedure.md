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
this document adds only what the producer cell changes.

## Cell identity

- Runner: `r3v_native_attended_producer` (native backend build).
- Recorder: `r3v_native_record_r2vb_producer`; the installed IB is
  byte-identical to `r300_r2vb_producer_reference_emit`, and the arming
  digest binds to it.
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

1. The kernel CS parser accepts the stream (the offline replay already
   reports `verdict=ACCEPT relocs=1 draws=1`), the fence retires, and
   dmesg carries no radeon validation delta.
2. The expected extent of the carrier holds the dwords
   `r300_r2vb_producer_reference_expected` computes -- the delivery
   identity over the reference records -- byte-exact.
3. Every dword past the expected extent still holds the poison.

## Falsifiers

- Poison across the whole expected extent: the producer executed no
  color write that reached the carrier; the US program, RS routing, or
  the color-backend retarget failed as a unit. The retained IB and
  registers decide which; the run is not repeated on the same boot.
- Wrong bytes in the expected extent: the write landed but the shaded
  value is wrong -- interpolator routing, US program semantics, format
  conversion in the C4_32_FP path, or FP24 narrowing. The byte pattern
  against the expected dwords localizes the stage.
- Poison disturbed past the expected extent: the producer wrote outside
  its row; pitch, height, or the draw's slot addressing is wrong, and
  the cell stops until the overrun is explained.
- A dmesg validation delta or an unretired fence: kernel-boundary
  finding; the wedged state is preserved for inspection and the host is
  cold-power-cycled rather than resubmitted.

## Verdict

`r300_r2vb_producer_carrier_check` computes the verdict from the mapped
carrier: expected-extent compare, tail poison retention, and refusal of
a poison value colliding with any expected dword, since that pairing
leaves the unwritten case undecidable. A parser acceptance or a retired
fence alone proves transport; only the carrier bytes prove production.
