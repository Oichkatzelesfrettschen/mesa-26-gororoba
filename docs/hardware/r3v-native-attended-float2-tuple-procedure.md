# R3V native attended FLOAT_4 + FLOAT_2 tuple cell procedure

The fetched tuple cell isolates PSC synthesized-lane expansion on silicon:
the vertex data arrives through a two-array `3D_LOAD_VBPNTR` -- a FLOAT_4
slot-position array and a FLOAT_2 model array, six fetched dwords per vertex
-- and the model element's XY01 selector asks the hardware to expand each
two-dword record to the logical input (x, y, 0, 1) before the RS routes it to
the US and the color backend stores it into the poisoned carrier.  The
producer cell establishes the carrier write from an embedded draw body, and
the re-ingest cell establishes the fetch of GPU-written FLOAT_4 data.  The
executed run below decides the exact F32 `FLOAT_2 + XY01` expansion question
for its recorded RS482 identity; other format and route questions retain
separate frontiers.

The offline kernel-parser replay accepts the stream under the synthesized-lane
width validator (`VAP_VTX_SIZE` equals the summed fetch widths, six dwords).
The retained attended run reports submit success, an empty dmesg delta, a
retired fence, and the exact carrier bytes; Linux Radeon source owns the
deployed parser's register semantics.

`docs/hardware/r3v-native-attended-cell-procedure.md` carries the
boundary statement, the host preconditions, the identity freeze, the
arming conjunction, the rollback rules, and the retained-record layout;
this document adds only what the tuple cell changes.

## Cell identity

- Runner: `r3v_native_attended_float2_tuple` (native backend build).
- Recorder: `r3v_native_record_r2vb_float2_tuple`; the installed IB is
  byte-identical to `r300_r2vb_float2_tuple_reference_emit`, and the
  arming digest binds to that stream.
- Arming digest source: `r3v_native_float2_tuple_arming_runner` builds
  the stream without submitting, reports the `ib_blake3` an
  authorization declares, and evaluates the full conjunction for the
  tuple cell kind.
- Cell kind: `R3V_NATIVE_CELL_KIND_R2VB_FLOAT2_TUPLE`.  The arming
  gate's geometry predicate requires two references: the carrier equal
  to the producer reference layout footprint with the GTT domain in
  both directions, and the vertex BO device-read alone, sized to the
  reference records' two fetch arrays (three slot positions at sixteen
  bytes plus three model records at eight).
- Buffers: the carrier at relocation entry 0, prefilled with
  `R300_R2VB_PRODUCER_POISON_DWORD`; the vertex stream at entry 1,
  host-written by the recorder through
  `r300_r2vb_float2_tuple_vertex_stream`; both cache-published before
  submission.

## Predictions

Recorded before the run; deviation is the finding.

1. The kernel CS parser accepts the stream (the offline replay reports
   `relocs=2 draws=1 verdict=ACCEPT`, and the width-predicate census
   reports `draws=1 pass=1 reject=0 decline=0`), the fence retires, and
   dmesg carries no radeon validation delta.
2. The carrier's expected extent holds the XY01 expansion of each model
   record byte-exact -- slot v reads (x_v, y_v, 0x00000000, 0x3f800000)
   -- and every dword past it keeps poison.  Slot 0 is
   `41000000 3f400000 00000000 3f800000` (8.0, 0.75, +0.0, 1.0).
3. The vertex allocation reads back byte-identical to the serialized
   stream: the fetch source takes no device write.

## Falsifiers

- Carrier poison across the expected extent: the fetched pass did not
  execute or the fetch declaration kept the draw from running
  (`CARRIER_UNWRITTEN`); the retained submit object and dmesg pair
  decide which boundary stopped it.
- Carrier delivered with wrong lane content (`CARRIER_MISMATCH`): the
  XY01 expansion itself is the finding -- z not +0.0 or w not 1.0
  names the synthesized lanes, x or y wrong names the FLOAT_2 fetch or
  the stride walk, and a slot holding another record's values names
  the per-vertex addressing.  This is the class the cell exists to
  decide, and the retained carrier bytes carry the whole signature.
- Poison disturbed past the carrier extent or any changed vertex byte:
  containment failure; the run stops until the overrun is explained.
- A dmesg validation delta or an unretired fence: kernel-boundary
  finding; the wedged state is preserved and the host is
  cold-power-cycled rather than resubmitted.

## Verdict

`r300_r2vb_producer_carrier_check` judges the carrier against the XY01
delivery identity, and the byte comparison against the re-serialized
vertex stream judges containment on the fetch source; only
`CARRIER_DELIVERED` -- expected extent exact, tail poison intact,
vertex intact, fence completed -- exits zero.  The retained record adds
`vertex.bin` and `float2_tuple_outcome.json` beside the producer
record's artifacts.

## Executed run

The cell ran on RS482 on 2026-08-15 from main `cbe9d2597cd` (IB blake3
`320b2a819e6f46c5de824c4f4e09829a0861d36787a2799fec9bde6c540694a7`,
298 dwords, digest identical across the dev host and the box) and
returned `CARRIER_DELIVERED`: all three slots hold the XY01 expansion
byte-exact -- (8.0, 0.75, 0.0, 1.0), (56.0, 1.0, 0.0, 1.0),
(999.0, 2.0, 0.0, 1.0) -- with `expected_pass=1 tail_poison_pass=1
vertex_intact=1 mismatched=0`, empty dmesg delta, fence retired.  The
PSC synthesized-lane expansion of one fetched F32 `FLOAT_2` element
under the `XY01` selector holds on PCI `1002:5974` RS482 for this exact
packet and vertex extent.  Other widths, data types, selectors, and the
public integrated delivery route remain separate evidence frontiers.  The
release-build preflight for this run surfaced the
producer-cell host test's NDEBUG assert-erasure segfault, fixed in
`cbe9d2597cd` before arming.  The retained record lives in the
`steinmarder-r300` bundle
`results/r3v-native-float2-tuple-xy01-delivery-rs482/`.

## Index-pair requalification (delivered)

The tree programs the VAP_VF_MAX_VTX_INDX/VAP_VF_MIN_VTX_INDX pair in one
PACKET0 run, so the first-draw contract and the bare prefix each grew and
the cell's IB digest changed: the emission is 301 dwords with IB blake3
`0ff78b5ebceca983184d845e2014387778d51f6722755d08c6e40ba121ab0258`.

An attended RS482 (1002:5974) run delivered this digest CARRIER_DELIVERED
byte-exact -- twelve carrier dwords exact, poison-preserved padding,
intact vertex source, vkQueueSubmit COMPLETED, empty dmesg delta -- under
the loaded radeon-unified-dkms 0.8.3 XY01-aware validator (module
srcversion 95D23C4E23D42D8E205F8F5). The three added dwords come from two
emission sites: `r300_first_draw_state_emit()` emits each contract entry
as a separate PACKET0 header/value pair, so the new
`VAP_VF_MIN_VTX_INDX` entry contributes two dwords;
`r300_pm4_emit_vertex_index_range()` replaces the bare two-dword
`VAP_VF_MAX_VTX_INDX` write with a three-dword MAX/MIN run, contributing
the remaining dword. The delivery matches the 298-dword predecessor, so
the index-pair change is neutral to the carrier result on silicon. The
run is retained as steinmarder-r300
`results/r3v-native-float2-tuple-index-pair-loaded-validator-rs482/`.
