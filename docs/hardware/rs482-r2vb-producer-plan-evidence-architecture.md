# RS482 R2VB producer-plan evidence architecture

This document is the research-data architecture of the R2VB producer-plan
campaign: the silicon substrate fully decomposed, the mechanism stack as it
stands on main, the derivation of every load-bearing numeric coefficient, the
evidence contract that makes the retained data citable, the calibration state
of each measuring instrument, and the open-limitations ledger.  It is the
source map for external write-ups: every claim below names the artifact that
carries its proof, and every constant traces to a first-principles derivation
or a named silicon measurement.

The companion mechanism-design document is
`rs482-producer-alu-compaction-design.md` (budget escape, typed transport,
source-domain predicate).  The overall hybrid vertex architecture lives in
`rs482-hybrid-vertex-tcl-design.md`.  This document does not restate their
designs; it binds the campaign's data model together.

## Silicon substrate

The target is the RS480-class integrated GPU (PCI `1002:5974`, marketed as
RS480/RS482/RS485, Radeon Xpress 200), an R300-derived 3D core behind an AMD
K8 HyperTransport northbridge in a Dell Vostro 1000.  The properties below
are the constraints every mechanism in this campaign answers to.

- Memory topology: the chip has no dedicated VRAM.  A 128 MB carve-out of
  system DRAM (NB_TOM) plus roughly 1 GB of unsnooped GART pages form the
  GPU-visible address space.  Every buffer object round trip is a system
  memory transaction.
- Vertex path: the vertex fixed-function transform units are absent from
  this IGP variant.  Mesa drives it as SW-TCL -- the draw module executes
  vertex shaders on the CPU -- and the R2VB campaign re-hosts vertex
  transforms on the fragment ALU instead.
- Fragment ALU (US, the unified shader block of R300-class hardware): 64
  paired color/alpha instruction slots (OUTC plus OUTA per slot), FP24
  arithmetic (`s1e7m16`), no integer datapath, no control flow, 32 vec4
  float constants, 8 rasterizer texcoord units.  The US has no set/compare
  opcode family; comparisons lower to CMP/CND substrate forms.
- Command path: PM4 packets through the radeon DRM CS checker
  (`r300_cs.c` era validation).  `VAP_VTX_SIZE` is validated against the
  declared output vertex format; an under-fed size stalls the GA front end
  (the 04f.2R root cause).
- Known silicon/kernel wedge classes, each with a retained reproduction
  bundle in steinmarder-r300: the stale-VAP full-frame fetch (a recent
  submit renders the predecessor's fullscreen quad, byte-identical IBs,
  heals with idle -- the 30-second idle protocol between corpus cases
  exists because of it), the GA-rooted 3D wedge (RBBM soft reset clears VAP
  only), and the non-posted HyperTransport stall on wedged MMIO reads.

## Mechanism stack on main

The R2VB route turns the fragment ALU into a vertex transform engine: a
producer fragment shader draws into a buffer, and the draw re-ingests that
buffer as vertex input.  The production chain is closed silicon-green
through clip classification, edge generation, topology gather, per-output
re-ingest, and the float split; the campaign now hardens its decision layer.

Three decision components share one program identity:

- The admission memo (`r300_r2vb.c`): a per-vertex-shader cache of one byte
  per (computed-varying mode, position space) cell, written by the classify
  path and by the clip route's direct budget call.  The memo is the route
  authority: rendering decisions follow it alone.
- The producer plan (`r300_r2vb_plan.c`): a cached classification record per
  cell -- action, primary reason, observed-reason mask, typed-source class,
  measured resource vectors, the selected split partition, and the owned
  canonical candidate NIR.  The plan is the future authority; today it runs
  in shadow.
- The shadow check (`r300_r2vb_plan_shadow_check`): at every memo decision
  point the plan recomputes the cell and compares its effective action to
  the memo byte.  Agreement is silent.  Divergence increments a
  process-wide counter and, under `R300_R2VB_PLAN_DEBUG=1` exactly, prints
  one diagnostic line.  Rendering never changes: a divergence is a planner
  defect finding, not an application fault.

The dual-authority overlap is the falsification engine of the campaign.  The
RS482 shadow-parity corpus caught the plan's cv=0 cell rejecting a
computed-varying producer that the memo admitted; the root cause was a cell
semantics mismatch (the plan applied whole-shader varying discipline to a
cell that predicts the position pass alone), fixed by scoping the varying
discipline to the cv=1 cell and moving the typed-source scan onto the
restaged position candidate.  The divergence, its wrong first diagnosis, and
the corrected diagnosis are three separate immutable bundles -- the record
keeps its own failed hypothesis.

Two measurement instruments feed the compaction research from this stack:

- Standing-route telemetry (`r300_r2vb_telemetry.c`): counters at the memo
  decision point (per action, per primary reason, per typed class),
  printing under `R300_R2VB_TELEMETRY=1`, and retention of engaged
  producers -- SPLIT plans and non-structural rejects -- as
  `nir_serialize` blobs named by their full BLAKE3 content hash under
  `R300_R2VB_TELEMETRY_RETAIN=<dir>`.  The exact value `0` disables retention
  and keeps the observation gate closed.  Publication is atomic (same-directory
  temp file, `O_EXCL`, rename; unlink on failure), an existing file
  verifies byte-for-byte before deduplicating, and the summary prints once
  per context epoch through atomic counter loads.
- Producer-resource census (`r300_r2vb_producer_census.c`): every corpus
  specimen runs the plan chain and records post-compiler statistics under
  named phases (`walk` for the diagnostic candidate walk;
  `selected-baseline`, `pass-a`, `pass-b` for explicit recompiles of the
  plan-selected programs), with fail-closed capture (overflow and
  truncation are recorded conditions, not silent losses) and deterministic
  transcripts.

The identity test (`r300_r2vb_admission_cso_identity_test.c`, on the
admission-cso-identity branch until the closure bundle banks) closes the
remaining trust gap: the program measured for admission and the program the
producer CSO compiles are memcmp-identical `r300_fragment_program_code`
emissions with value-identical constant lists, on the baseline and both
split halves, in both position spaces.  The whole-struct comparison is sound
because the struct is pointer-free and both storage paths start zeroed;
`code_offset` packs instruction counts, not addresses.

## Coefficient derivations

Every numeric constant in the campaign decomposes to bit-level structure or
a named measurement.  The derivations, in dependency order:

- `s1e7m16` (FP24): 1 sign + 7 exponent + 16 mantissa bits = 24.  The
  significand with its implicit leading bit spans 17 bits.
- `2^17 = 131072` (the exact-integer window): a binary format with a 17-bit
  significand represents every integer of magnitude up to `2^17`
  exactly, and `2^17` itself is exact as `1.0 x 2^17`.  Above it, integer
  spacing exceeds 1 and exactness fails.  Proven mechanically: the Rocq/Flocq
  FLX(17) refinement in open_gororoba is `FP24Representable.v`
  `fp24_int_exact_inclusive`, and the integer-window half is
  `IDCT8DP4ExactBound.v` `dp8_exact_threshold`; the transform application is
  `R2VBTransformDP4.v` `mvp4_rows_exact`, extracted to C through CertiRocq.
- `8 * B^2 <= 2^17` (the DP4-chain exactness bound): an 8-term dot-product
  row (the IDCT8 shape, and the general 8-wide MAC accumulation) sums 8
  products each bounded by `B^2`; the accumulator peak is `8 * B^2`, and
  exactness requires the peak inside the window.  Decomposed:
  `B^2 <= 2^14`, so `B <= 2^7 = 128`.
- `B <= 128` inclusive versus `B <= 127` strict: the inclusive exactness
  threshold is 128, because `8 * 128^2 = 2^17` is itself exactly
  representable.  The production gate holds at 127 as a fail-closed
  engineering choice -- excluding the boundary costs one value and removes
  the boundary-analysis obligation -- and the two thresholds are documented
  as distinct facts, because conflating them misreports a proof as a
  policy.
- 64 (the producer ALU ceiling): the US instruction store holds 64 paired
  color/alpha slots.  The emitted-slot count of the compiled producer, not
  a pre-compile estimate, is the admission oracle; multi-pass splitting is
  the only mechanism that clears workloads above it.
- 8 (`R300_R2VB_MAX_PRODUCER_INPUTS`): the split pass-B draw feeds every
  model attribute plus the carry, and the producer's input binding rides
  the rasterizer texcoord units, of which the RS has 8.  The CD-4 sedenion
  product (8 FP32x4 vertex elements) is silicon-confirmed at exactly this
  width.
- 1 vec4 (the carry width): the split transports one crossing set through
  the carry buffer; a crossing set wider than four scalars declines
  (`R300_R2VB_PLAN_CARRY_WIDTH`).  This is a chosen frontier, not a
  silicon limit: it keeps the re-ingest format one attribute wide.
- 32 vec4 (FS constants): the second resource ceiling; class-2 producer
  failures on the corpus are constant-budget, not ALU-budget.
- Diagnostic token vocabulary: `checksum=7131e1c5, covered=128` is the
  pristine reference-frame signature of the corpus FBO; `covered=1024` is
  the stale-VAP full-frame fetch signature (the wedge, not a result);
  `diffbytes=0` states byte-identity of route versus control frames.

## Evidence contract

Research-quality here means an external reader can verify every claim from
retained artifacts without trusting the narrative.  The contract has eight
properties, each with its implementing mechanism:

1. Immutable add-only bundles: every silicon run lands as a new results
   directory in steinmarder-r300; correction lands as a new bundle plus a
   superseding finding, never as an edit to a banked one.  The planner
   campaign holds three: the divergence observation, the corrected-cell
   rerun, and the closure run.
2. Hash-bound provenance: bundles carry `provenance.txt` and
   `provenance.hashes.txt` -- installed Mesa SHA, package identity, DSO
   SHA-256, runner hash -- so the executing artifact is bound to the source
   tree by content, not by claim.  The r3v ICD statically links gallium,
   which makes `libvulkan_r3v.so` the executing artifact for Vulkan-path
   evidence; DSO mtime and hash discriminate staleness.
3. Boot-and-kernel binding: runs record the boot ID before and after, and
   privileged kmsg snapshots with deltas; a clean delta and a stable boot
   ID are closure criteria, because the known wedge classes are visible in
   exactly that channel (DRM CS rejections, GPU resets, lockups).
4. Prediction before observation: hardware-RCA changes record expected
   corpus movement first; a deviation is the finding and opens a new RCA
   rather than amending the prediction.  The planner campaign's wrong
   first diagnosis stands banked because of this rule.
5. Calibrated instruments: every verdict-producing test proves itself on
   known-good and known-bad inputs before its verdicts count.  The
   telemetry calibration damages a retained file in place and requires the
   atomic republish; the census requires deterministic transcripts twice;
   the plan oracle carries admit and decline rows for every reason class.
6. Machine-readable verdict first: a run's sweep writes the structured
   verdict (counts, tokens, criteria) before prose interpretation, so the
   interpretation cannot silently substitute for the data.
7. Engagement-proven negatives: a zero-divergence count is meaningful only
   with planner engagement demonstrated in the same log (plan evaluations,
   admission tokens, `R300_R2VB_PLAN_DEBUG=1` in the environment
   manifest).  A silent log proves absence of evidence, not parity.
8. Population versus synthetic separation: corpus results characterize the
   corpus; claims about real workloads wait for telemetry-retained
   producer populations.  The census packing datum (float chains
   presubtract-dominant, typed carry halves output-modifier-dominant) is
   backend characterization, not yet compaction-rule demand.

## Instrument calibration state

| Instrument | Checks | Calibration classes |
|---|---|---|
| Plan oracle (`r300_r2vb_plan_oracle_test`) | 87 | admit/decline per reason; both spaces; cv=0/cv=1 cell semantics; typed rows in both directions |
| Census (`r300_r2vb_producer_census`) | 264 | per-specimen expectations mirror the route-chain oracle; determinism run twice; fail-closed capture; row table complete against the corpus manifest, missing/extra member calibrated |
| Telemetry (`r300_r2vb_telemetry_test`) | 18 | closed/open gate; dedup; full-hash name; damaged-file republish; structural-reject non-retention |
| Identity (`r300_r2vb_admission_cso_identity_test`) | 36 | program + constant identity, baseline and halves, both spaces |
| Shadow counter | corpus-level | zero on the closure corpus with engagement proven |

All instrument runs hold under clang and gcc `-Werror` trees and under
ASan+UBSan with zero reports; the sanitizer gate on the identity branch
surfaced and removed a pre-existing production leak (the classic front-end
path leaked one NIR clone per compile and per retry round, fixed by
releasing the clone when `nir_to_rc` does not consume it).

## Open-limitations ledger

Each entry names the mechanism, the risk class, and the correction gate.

- `R2VB-PLAN-TARGET-SLICE-01`: `plan_scan_structure` walks the folded whole
  application VS, so a texture op, unsupported intrinsic, or control flow
  feeding only a non-position varying rejects the cv=0 cell.  The typed
  scan already runs on the restaged position candidate; the structural
  scan does not.  Fail-safe today (classify's whole-program float
  whitelist rejects such shaders before any memo write, so no divergence
  is reachable), and it under-admits, leaving gallivm authoritative.
  Correction gates on the planner becoming broad automatic route
  authority: every cell scan moves onto the exact restaged producer the
  cell represents.
- Shadow-divergence counter concurrency: `plan_shadow_divergences`
  increments unsynchronized while the telemetry counters are atomic.  Same
  multi-context exposure class the telemetry hardening closed; single
  process, single context in every current corpus.  Fold into the next
  planner behavior change rather than a freeze-breaking single-line PR.
- Window-cell replan cost: a viewport change re-keys and replans the
  window-space cells, which re-runs measured compiles.  Bounded (two cells
  per VS, once per viewport change), invisible on the corpus; measure
  before optimizing when real-workload telemetry lands.
- Cross-process serialization canonicality: retention deduplicates
  byte-identical serialized NIR blobs by content hash.  Equivalent
  programs serializing identically across processes is unproven; the
  contract is phrased narrowly (byte-identity deduplicates) until a
  subprocess determinism test exists.
- Source-domain predicate: designed (path identity over source witnesses,
  executable vocabulary in the compaction design document), not
  implemented.  The production typed route stays blocked on it; the
  diagnostic route rides the exact `R300_R2VB_TYPED_SPLIT=1` gate.
- Structural comparison durability: the identity test's whole-struct
  memcmp is valid while `r300_fragment_program_code` stays pointer-free
  with zeroed storage paths; a structural change to that struct converts
  the comparison to named-field normalization.

## Repository topology

Five repositories carry disjoint roles; every file keeps its home.

- `mesa-26-gororoba`: driver source, build infrastructure, committed
  calibration tests.  All mechanisms above live here.
- `steinmarder-r300`: retained evidence -- immutable results bundles,
  findings with falsification records, probes, runners.  The three planner
  bundles and the corpus runners live here.
- `radeon-custom`: the out-of-tree radeon DKMS fork (GPU-reset and hazard
  containment; the RAD-06 CS-checker cross-check draft).  Kernel-lane
  changes never ride a Mesa closure boot.
- `open_gororoba`: mechanized proofs (Rocq/Flocq FP24 window, CertiRocq
  extraction) that anchor the coefficient derivations.
- `xorg-server` packaging: the glamor host stack whose gradient closure
  (609/609 on the banked package image) is the desktop-facing consumer of
  the same driver.
