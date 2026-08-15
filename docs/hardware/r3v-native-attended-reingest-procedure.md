# R3V native attended re-ingest cell procedure

The producer-plus-re-ingest cell isolates GPU-write to vertex-fetch ordering
on one submission: the reference producer pass writes the fixed triangle's
three FLOAT_4 vertices into a poisoned GTT carrier, the publication tail
retires the color write and syncs the engines, and the reference triangle
draw re-binds that carrier through `3D_LOAD_VBPNTR` and renders into a
sentinel-filled 64x64 target. The executed run below establishes this ordering
for its recorded RS482 identity and exact concatenated stream. Public-route
composition remains a separate mechanism.

`docs/hardware/r3v-native-attended-cell-procedure.md` carries the
boundary statement, the host preconditions, the identity freeze, the
arming conjunction, the rollback rules, and the retained-record layout;
this document adds only what the re-ingest cell changes.

## Cell identity

- Runner: `r3v_native_attended_reingest` (native backend build).
- Recorder: `r3v_native_record_r2vb_reingest`; the installed IB is
  byte-identical to `r300_r2vb_reingest_reference_emit` -- the reference
  producer emission concatenated with the reference triangle emission --
  and the arming digest binds to the concatenation.
- Arming digest source: `r3v_native_reingest_arming_runner` builds the
  stream without submitting, reports the `ib_blake3` an authorization
  declares, and evaluates the full conjunction for the re-ingest cell
  kind.
- Cell kind: `R3V_NATIVE_CELL_KIND_R2VB_REINGEST`. The arming gate's
  geometry predicate requires two references: the carrier equal to the
  producer reference layout footprint with the GTT domain in both
  directions, and the color target equal to the triangle family's
  retained footprint (64-pixel pitch, 64 rows plus the canary row),
  written through the color backend alone.
- Buffers: the carrier at relocation entry 0, prefilled with
  `R300_R2VB_PRODUCER_POISON_DWORD`; the color target at entry 1,
  prefilled with `R300_TRIANGLE_COLOR_SENTINEL`; both cache-published
  before submission.

## Predictions

Recorded before the run; deviation is the finding.

1. The kernel CS parser accepts the stream (the offline replay reports
   `relocs=2 draws=2 verdict=ACCEPT`), the fence retires, and dmesg
   carries no radeon validation delta.
2. The carrier's expected extent holds the delivery identity over the
   triangle vertices byte-exact, and every dword past it keeps poison --
   the producer stage repeats its proven verdict.
3. The color target renders the analytic triangle: interior draw color,
   exterior sentinel, canary rows intact -- the same oracle verdict the
   attended triangle cell earned from a CPU-written vertex BO.

## Falsifiers

- Carrier poison across the expected extent: the producer stage failed;
  the cell stops there and the producer-only cell's falsifier table
  applies (`CARRIER_UNWRITTEN`).
- Carrier delivered but the target sentinel undisturbed or wrong: the
  vertex fetch of GPU-written bytes is the failing stage
  (`CARRIER_ONLY`) -- stale vertex-cache content past the engine sync,
  GART read-after-write ordering, or the fetch path itself; the
  retained carrier and target bytes decide which.
- Sentinel disturbed past the render extent or poison disturbed past
  the carrier extent: containment failure; the run stops until the
  overrun is explained.
- A dmesg validation delta or an unretired fence: kernel-boundary
  finding; the wedged state is preserved and the host is
  cold-power-cycled rather than resubmitted.

## Verdict

`r300_r2vb_producer_carrier_check` judges the producer stage and
`r300_tcl_bypass_triangle_oracle` judges the render; only
`REINGEST_RENDERED` -- both verdicts passing under a completed fence --
exits zero. The retained record adds `color.bin` and
`reingest_outcome.json` beside the producer record's artifacts.

## Executed run

The cell ran on RS482 on 2026-08-14 from main `cd28064499a` (IB blake3
`553bb0cedacffeca85bc7e4a8bfabc9d02b0120e074a7f81bd2ccfd780c964df`,
542 dwords) and returned `REINGEST_RENDERED`: carrier
`expected_pass=1 tail_poison_pass=1 mismatched=0 tail_disturbed=0`,
target `executed=1 interior=1 exterior=1 canary=1`, empty dmesg delta,
fence retired. The GPU-write to vertex-fetch ordering holds on one
submission through the publication tail. The retained record lives in the
`steinmarder-r300` bundle `results/r3v-native-reingest-rendered-rs482/`.
