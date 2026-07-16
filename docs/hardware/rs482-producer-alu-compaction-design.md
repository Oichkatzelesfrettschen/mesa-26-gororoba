# RS482 R2VB producer ALU compaction (HBTCL-04f.4 design)

An R2VB fragment-ALU vertex producer whose derived position-pass FS exceeds the
R300 64-slot ALU ceiling declines to gallivm. The split transport (HBTCL-04f.2 /
04f.3) carries an intermediate across a single-block cut when a producer does not
fit in one pass. This document designs the complementary lane: an algebraic
compaction pass that refactors an over-budget producer into a
semantics-preserving form that fits under the ceiling in one pass. It records
which reduction techniques win on this silicon, which lose and why, the pass
architecture and contract, and how it composes with the split and MRT lanes.

Compaction is an implementation lane and 04f.4 is open. This document is its
design and its validation obligation, not a merge-ready specification: the
proof and workload prerequisites in "Prerequisites before implementation" gate
any code. It also carries two corrections to an earlier draft -- the FP24
exactness bound and the affine-recurrence contract -- that are load-bearing for
correctness and are called out where they apply.

## The budget target is emitted co-issued slots, not source op count

The R300 fragment ALU is a VLIW dot-machine. `R300_PFS_MAX_ALU_INST = 64`
(enforced in `r300_fragprog_emit.c`) counts co-issued slots, each holding a
vector op (`OUTC`, the RGB pipe) and a scalar op (`OUTA`, the alpha pipe)
(`radeon_code.h` `inst[].rgb_inst` + `alpha_inst`). A `DP4` is a four-wide
multiply-accumulate in one slot. The admission gate
(`r300_fs_measure_nir_admission`, read by `r300_r2vb_split_admitted` in
`r300_r2vb.c`) measures the emitted `alu.length` -- the slot count after the
radeon compiler has already run its own reductions:

- common-subexpression elimination, constant folding, copy propagation,
  algebraic optimization, DCE, and vectorization (`radeon_optimize.c` and the
  NIR opt pipeline, including `nir_opt_vectorize`);
- presubtract source folding (`RC_PRESUB_ADD` / `SUB` / `BIAS` / `INV`,
  `radeon_program_constants.h`) -- an `a +/- b` read folded into a source;
- output-modifier folding (`RC_OMOD_MUL_2` / `4` / `8`, `DIV_2` / `4`) -- a
  multiply or divide by a power of two folded into the result;
- vector plus scalar co-issue pairing (`radeon_pair_schedule.c`).

A producer that measures over 64 is over after all of that. The backend compile
is the authoritative admission mechanism, and it distinguishes an escapable ALU
ceiling failure from a temporary-file, constant-file, or structural rejection.
Compaction restructures the NIR expression DAG so a reduction the low-level
peephole cannot reach becomes visible; it runs on NIR before the restage-to-RC
hand-off, then re-measures the real backend to decide whether the rewrite helped.

## Three over-budget mechanisms, three candidate compactions

The emitted-slot ceiling is exceeded by three distinct DAG shapes. Each is a
hypothesis about a possible rewrite, not a confirmed win; the mine
(Prerequisites) decides which shapes actually occur and which rewrites actually
reduce slots.

| Class | Representative | Why it exceeds 64 | Candidate compaction |
| --- | --- | --- | --- |
| Long dependent chain | `recur90` (90x `a = 2a - u`) | A dependent chain does not co-issue: each step needs the previous result, so N steps occupy N serial slots | Shorten the chain algebraically, but only within a proven exact domain (see the affine correction below); `recur90` itself stays a split-transport stress case |
| Wide independent kernel | `wide5` (140 MADs) | Genuine op count; the ops co-issue but there are too many | Share bilinear partial sums across outputs and repack scalar MAD groups into native DP4, if a concrete specimen shows the existing pipeline misses the pattern |
| Irreducibly large | multi-attribute / MRT export | Real work exceeds one pass | Transport, not compaction: the typed split (04f.2 / 04f.3) or MRT carry (04f.5). Out of scope here |

## Winning reductions on a DP4 machine

Each rule is a semantics-preserving NIR rewrite whose success is measured by the
full backend resource vector, with a concrete FP24 exactness predicate. The DP4
regime is the accepted core of this design: the machine's native op is a
four-wide multiply-accumulate in one slot, so a technique earns admission only by
reducing the real emitted cost, never by reducing a source-level multiply count.

### FP24 exactness envelope (corrected)

The RS482 fragment ALU is FP24 in the `s1e7m16` layout: one sign bit, a 7-bit
exponent, and a 16-bit stored mantissa, giving **17 significand bits** and an
exact-integer window of **`|n| <= 2^17 = 131072`**, round-toward-zero on the
compute side. This is the driver's own numeric-domain model
(`r300_numeric_domain.c`, `R300_NUM_DOMAIN_FP24_RTZ`: `exact_int_bound = 131072`,
`significand_bits = 17`, theorem "|n| <= 2^17 exactly representable in FP24").

An eight-term dot with operands bounded by B satisfies `|acc| <= 8*B^2`. The
mathematical worst-case exactness guarantee is `8*B^2 <= 2^17`, hence
**`B <= 128`**: at `B = 128` the worst case is `8*128^2 = 131072 = 2^17`, which is
itself exactly representable, and `8*129^2 = 133128` leaves the window. The
initial driver policy retains the strict interior `8*B^2 < 2^17`, hence
**`B <= 127`** (`8*127^2 = 129032 < 131072`), a conservative admission rule that
declines the exact-but-boundary `B = 128` rather than asserting it inexact. Those
are two distinct facts -- the inclusive exactness threshold and the strict policy
-- and stay named separately, matching the inclusive typed-carry value gate
(`|n| <= 2^17`) the classifier ships. The driver catalog's DP4 rows confirm the
scale -- `4*127^2 = 64516 < 2^17`, `5*127^2 = 80645 < 2^17`.

Correction: an earlier draft cited `8*B^2 < 2^24` giving `B <= 1448`, imported
from `IDCT8DP4ExactBound.v`. That bound is FP32's 24-bit-mantissa envelope and
does not hold on RS482 FP24, whose mantissa is 16 bits (17 significand). It would
admit arithmetic outside the exact FP24 integer window the driver's own catalog
defines. The generic inequality `|dp8| <= 8*B^2` is valid; the RS482 refinement
is `2^17`, not `2^24`. The `2^17` correction is tracked as PROOF-FP24-01
(fixing the proof and its mirror) and gates every FP24-exact rule here. The
exactness obligation is the stepwise accumulation staying in the `2^17`
exact-integer interval, proved with the accumulation order and the inclusive
boundary explicit, not inferred from the final sum alone.

### Affine-recurrence shortening (contract-restricted, corrected)

An unrolled straight-line chain `x_{i+1} = c*x_i + k` repeated N times has the
real-number closed form

```text
x_N = c^N * x_0 + k * (c^N - 1) / (c - 1)    (c != 1)
x_N = x_0 + N*k                              (c == 1)
```

The real-number identity is correct. It is not generally equal to executing
`x := c*x + k` N times in FP24, because the original rounds after each step while
the closed form changes operation order, intermediate magnitude, cancellation,
and rounding points.

`recur90` is a direct counterexample, not the flagship win. Its closed form
contains `2^90`; RS482 FP24's normal range tops out near `2^65`. The original
recurrence stays bounded -- at `u = 1` the value sits at the fixed point `a = 1`
-- while the closed-form terms overflow before they cancel. The transformation
would convert a bounded original execution into an overflowing compact one. So
`recur90` stays a split-transport stress case and is not the first compaction
success case.

A recurrence rule is production-safe only under an explicit semantic contract:

- Exact-domain: prove every original step and every compacted intermediate is
  exactly representable, no schedule underflows, overflows, saturates, or
  flushes, so the real identity lifts to identical FP24 values. Plausible for
  tightly bounded integer or dyadic domains; not for a generic float recurrence.
- Fast-math: an explicit NIR reassociation permission with an accepted numerical
  contract. A different semantic class; it does not enter the automatic HBTCL
  route.
- Bounded-error: an error bound admitted only where the program permits it.
  Diagnostic initially, not production-default.

The first production recurrence rule is restricted to coefficient classes whose
entire stepwise and compacted evaluations are proven exact -- the degenerate or
periodic cases `c in {-1, 0, 1}`, exact integer or dyadic operands, and
statically bounded intermediates. Whether those cases produce meaningful slot
wins is a question for the mine, not an assumption. General float
affine-recurrence compaction stays research-only until a concrete FP24
refinement theorem exists.

### Shared partial sums and DP4 repack (specimen-gated)

For a bilinear form carrying reflection symmetry `C[k][N-1-n] = (-1)^k *
C[k][n]` (the DCT/IDCT cosine parity), outputs `n` and `N-1-n` share the same
even and odd four-term partial sums and differ only in the sign of the odd part.
Computing the shared partials once reduces the scalar multiply count -- the
8-point IDCT goes from 64 multiplies and 56 adds to 32 multiplies, 24
partial-sum adds, and 8 final adds, proved over the reals in the open_gororoba
proof tree (`proofs/theories/IDCT8EvenOdd.v`, `idct8_butterfly_eq_dense`).

This is a scalar-arithmetic reduction over the reals. It is not yet an emitted-
slot result on R300: four products may already collapse into one `DP4`, vector
adds may or may not combine across outputs, and register and constant pressure
change the emitted count. The final slot count is whatever the radeon backend
emits, which only a compiled specimen establishes. It is also a scope question:
an IDCT is a natural video or compute workload, not automatically a vertex
producer. Unless R2VB telemetry finds an IDCT-shaped VS, this rewrite belongs in
a generic compiler or VL-specific lane (IDCT-COMPACT-01), not on the
HBTCL-04f.4 critical path.

Likewise, repacking scalar MAD groups into a native `DP4` is a hypothesis, not a
requirement. The r300 NIR pipeline already runs `nir_opt_vectorize` and the
standard optimizations, and the backend already translates dot operations and
folds source and output modifiers. A custom DP4 pass is justified only by a
concrete specimen: the optimized NIR, the RC instruction stream, the paired
emitted program, the slot count, the specific four-term scalar pattern that
survived existing vectorization, and a post-rewrite RC stream proving a lower
slot count. Without that specimen a custom pass risks doing nothing, recreating
a standard optimization, changing float association, or degrading pairing.

### Presubtract and output-modifier exposure

Restructuring an `a - b` so the radeon presubtract detector fires, or a
multiply/divide by a power of two so the output modifier fires, moves an
arithmetic op off the slot count. RC does this locally; a NIR canonicalization
would only help if an RC dump proves a currently missed local fold, in which
case the fix may belong in the RC optimizer rather than a NIR pass.

## Rejected reductions: the multiply-minimization family, with narrowed claims

The classic operation-count-reduction family -- Walsh-Hadamard / butterfly
multiply reduction, Karatsuba, Cariow, Strassen, Winograd short convolution, and
per-vertex Cayley-Dickson multiply -- trades multiplies for adds. On a machine
whose native op is a four-wide multiply-accumulate in one slot, a multiply and
an add cost the same slot, so the trade converts one-slot MACs into add slots
and loses for the small dense per-vertex producer. Both source archives reach
this regime conclusion independently. The universal policy is narrow: a
transform family is admitted only when a compiled candidate reduces the complete
R300 resource vector and satisfies its semantic contract, never because it
reduces a multiply count.

| Rejected technique | Op-count claim | Narrowed rejection | Source |
| --- | --- | --- | --- |
| Cariow sedenion multiply (16-point WHT + sparse remainder) | d=16: 256 -> 122 multiplies, 240 -> 298 adds (+58 add) | Loses on a MAC ALU: the +58 adds are extra slots and the 106-entry sparse remainder is irregular multiply-accumulate that does not vectorize; wins only where a hardware multiplier is the scarce resource (VLSI / fixed-point) | `cariow_2013_fast_sedenion_multiplication.pdf` sec 3; `cd_kernel/src/cayley_dickson/cariow_factorization.rs`; proof `proofs/verified/C1636_Cariow2013SedenionSchedule.v` (`16 + 106 = 122`) |
| Karatsuba 3-for-4 on the CD doubling product | complex 4 -> 3 multiplies | The commutative cross-term trick does not apply to the CD doubling `(a,b)(c,d) = (ac - conj(d)b, da + b*conj(c))` because operand order and conjugation matter. This does not prove every bilinear-rank reduction is impossible for a noncommutative algebra; the policy is that no Karatsuba-derived rule is admitted without a dimension-specific semantic proof and a measured emitted-slot win | `cd_kernel/src/cayley_dickson/fast_associator.rs` header; `proofs/theories/HurwitzTheorem.v` (composition bound `n in {1,2,4,8}`) |
| Strassen | 7-multiply 2x2 block matrix-matrix | Irrelevant to the current shape: a 4x4 matrix-vector vertex transform is not a matrix-matrix product. Classified out of scope, not rejected by a multiply-versus-add argument | matrix-algorithm literature |
| Fast Walsh-Hadamard rotation | `O(d^2) -> O(d log d)`, d=128: 16384 -> 896 MADs | The stated `d >= 64` crossover is a CPU / high-dimension operation-count crossover, contextual evidence only. The R300 decision uses emitted RC slots, dependencies, constants, and pairing; a CPU crossover is not a production gate | `crates/turboquant/src/rotation.rs` L32 |

The archive's own empirical falsifier is instructive: the project's
`mul_optimized` path did not realize its claimed dim-32 reduction and stayed at
1024 multiplies, not the 498 target. A claimed reduction is a realized one only
when it is measured. Cayley-Dickson structure keeps its legitimate home in the
once-per-frame matrix build, and the WHT butterfly is a candidate only for a
genuinely high-dimension workload the vertex transform is not. A stale dim-16
Cariow multiplier count of 84 in `fast_associator.rs` is wrong; the proved and
implemented count is 122.

## Pass architecture: a certified-rewrite pipeline with two proof layers

The compaction pass borrows its shape from the open_gororoba certification
records -- an alternate schedule bundled with a proof it equals the reference and
the algebraic laws that license it. Those records are evidence architecture, and
by themselves they certify only real-ring identities; the driver needs a second,
finite-precision proof layer.

- `CDFusedBilinearSurface` (`proofs/theories/CDFusedBilinear.v`) packages an
  alternate `fused_mul` with proofs that it equals the reference `mul`, is
  bilinear, and is scale-homogeneous. This is the conceptual template for a
  rule: an alternate schedule plus algebraic equality plus the linear-algebra
  laws that justify further rewriting.
- `Cariow2013SedenionScheduleSurface` (`proofs/verified/C1636_Cariow2013Sedenion
  Schedule.v`) pairs a reduced-count spec with a semantic-equality theorem.
- `FLOAT_OPS` (`proofs/theories/FloatAxioms.v`) is a field signature: it assumes
  associative and commutative add and multiply, distributivity, and additive
  inverses. Those axioms are true for an exact ring and false for finite
  floating-point. Mapping the abstract ops to OCaml float, Rust f64, or RS482
  FP24 does not discharge the axioms. `FLOAT_OPS` therefore certifies the
  algebraic layer, not the finite-precision layer.

A production rule needs two independent obligations:

- A, algebraic: the alternate exact-ring schedule equals the reference exact-ring
  schedule (the `CDFusedBilinearSurface` shape).
- B, numeric refinement: under a domain predicate P, executing both schedules in
  the concrete FP24 semantics yields identical values (bit-exact), or an
  explicit bounded-error theorem when the rule is not bit-exact.

Discharging B requires either a formal `s1e7m16` RTZ / FTZ / saturating semantics
model, or an exact-domain reduction proving every operation lies in a subset
where FP24 equals integer or rational arithmetic. The open_gororoba tree today
has the generic bound and the field identities but no complete FP24 operational
semantics; building that model (the exact-domain path is the near-term option) is
a prerequisite, and it strengthens the broader compute-as-raster program.

A rule is therefore an executable object with a runtime domain predicate, not a
prose obligation:

```text
r300_compaction_rule {
    id
    semantics : EXACT_BITWISE | EXACT_FP24_DOMAIN | FAST_MATH | BOUNDED_ERROR
    recognize(nir)        -> match         // structural recognizer
    check_domain(nir, m)  -> witness       // concrete FP24 domain predicate P
    rewrite(nir, m)       -> nir'          // semantics-preserving transform
}
```

Automatic HBTCL admission allows only `EXACT_BITWISE` and `EXACT_FP24_DOMAIN`
initially. An immutable evidence manifest maps each rule id to its proof
repository SHA, proof file hash, theorem name, numeric-domain theorem, host
oracle, and silicon result, so a floating proof reference cannot change silently
under Mesa.

### Cost is a resource vector, not a slot delta

A candidate is measured by the full backend admission, not by ALU slots alone.
The existing admission already separates the recoverable ALU ceiling from
temporary-file, constant-file, and structural rejection.

```text
r300_compaction_cost {
    admission        // r300_fs_admission verdict
    alu_slots
    tex_slots
    max_temp
    const_slots
    paired_slots
    unpaired_rgb
    unpaired_alpha
    code_words
}
```

A rewrite is an improvement only when it lowers `alu_slots` without exceeding the
constant or temporary file, introducing unsupported texture or control flow,
reducing pairing, or increasing code or compile cost pathologically. The first
implementation need not expose every field, but the mine collects them all.

### Evaluation is transactional, not greedy

A fixed-order pipeline that keeps a rewrite only on an immediate slot reduction
rejects a useful enabling canonicalization -- rule A exposes a shape for rule B
without itself reducing slots. The evaluator instead runs a bounded candidate
search over fresh clones of the optimized baseline:

```text
baseline (optimized restaged FS)
  - recurrence candidate
  - shared-partial candidate
  - dot-repack candidate
  - modifier-exposure candidate
  - a few explicitly allowed compositions
```

Each candidate is recognized, domain-checked, rewritten, run through a bounded
cleanup, compiled through the real backend, and measured. The lowest-cost valid
exact candidate is selected; the application VS and the baseline producer are
never mutated. Bounding the beam and the compositions keeps compile latency and
evidence attribution controlled.

## Contract and composition through a shared producer plan

Compaction and split must operate on one prepared producer. If compaction
reduces a program from 100 to 72 slots without making it fit in one pass, the
split runs on the compacted 72-slot candidate, not on a freshly rebuilt 100-slot
original.

```text
r300_r2vb_producer_plan {
    action : SINGLE | COMPACTED | SPLIT | REJECT
    primary_reason : the deterministic decline or accept class (r300_r2vb_plan_reason)
    observed_reason_mask : every failure class seen across ranked candidates
    space  : clip | window
    rule   : compaction rule id
    before : r300_compaction_cost
    after  : r300_compaction_cost
    carry  : selected transport type per cut component (f / i / u / b)
    has_typed_source : the producer contains f2i/f2u/integer/Boolean semantics
    typed_source_class : the recognized typed-source shape (bool / sint / uint / none)
    typed_source : witness binding each typed op to the recognized chain
    input_semantics : R300_FS_INPUT_R2VB_FLAT_VERTEX for every producer compile
    candidate : owned canonical NIR from which one pass or the split halves build
}

r300_r2vb_plan_reason {
    OK
    CONTROL_FLOW | IO_SHAPE | INTRINSIC              -- structural reject
    TYPED_SOURCE_SHAPE | TYPED_SOURCE_NOT_CARRIED    -- typed-source shape reject
    TYPED_SOURCE_DOMAIN_UNPROVEN                     -- production source-conversion gate
    TYPED_SINGLE_PASS_UNPROVEN                       -- under-budget typed, held back
    TYPED_GATE_DISABLED                              -- R300_R2VB_TYPED_SPLIT off (policy)
    CARRY_WIDTH | SIGNED_RANGE | UNSIGNED_RANGE | MIXED_SIGNEDNESS
    NO_EXACT_CUT | OVER_ALU_NO_SPLIT
    PASS_A | PASS_B | BACKEND                        -- half build or compile failure
    OUT_OF_MEMORY | INTERNAL_INCONSISTENCY           -- infrastructure, not workload
}
```

Cut selection is multi-candidate: `r300_mp_find_vec4_cut` returns the first
candidate whose transport contract fits, and `r300_r2vb_split_admitted` then
compiles only that one. The production planner instead walks every ranked
candidate within a bounded limit -- carry width, typed source and carry
semantics, signed/unsigned range, pass-A and pass-B build, both compiles -- and
selects the best success. When none succeed it retains every observed failure in
`observed_reason_mask` and picks a deterministic `primary_reason` by precedence:
`OUT_OF_MEMORY`/`INTERNAL_INCONSISTENCY`, then structural reject, then
`TYPED_SOURCE_DOMAIN_UNPROVEN`, then `MIXED_SIGNEDNESS`, then range, then
`CARRY_WIDTH`, then `PASS_A`/`PASS_B`/`BACKEND`, then `NO_EXACT_CUT`. This avoids
reporting `CARRY_WIDTH` merely because the first candidate was wide when a later
candidate failed for the actual typed-domain reason.

Typed-source detection is independent of carry type. A shader can compute a typed
value entirely in pass A, convert it back to float before the cut, and carry only
a float: the carry type reads `f` while the source program still holds
`f2i`/`f2u`/integer/Boolean semantics. So `has_typed_source` scans the whole
program, not the selected cut, and the diagnostic route requires every typed op
to belong to the recognized chain (`typed_source`) and the chain's typed value to
be the value crossing the cut, declining `TYPED_SOURCE_SHAPE` or
`TYPED_SOURCE_NOT_CARRIED` otherwise. A policy gate being off (`TYPED_GATE_DISABLED`)
and an allocation failure (`OUT_OF_MEMORY`) are distinct from a semantic reject,
so telemetry does not count them as workload rejections. This makes the
production planner the authority for the typed-frontier classification the host
mirror (F3-CLASSIFIER-01) predicts.

The plan is cached per vertex shader, computed-varying mode, clip/window
position space, and the viewport-dependent window key where it applies. The
admission oracle is keyed by position space because the window producer includes
the divide and viewport operations and can choose a different cut from the clip
producer; the existing four-state admission memo does not retain a selected
compacted program, its rule identity, its proof class, or its cost record, so the
plan replaces it for this lane.

The pass is a pure NIR-to-NIR transform with a fail-closed guarantee:

- Consumes: the restaged position-pass FS NIR
  (`r300_r2vb_build_restaged_fs_nir`) plus the FP24 exact-integer envelope.
- Guarantees: the selected candidate's resource vector is no worse than the
  baseline on every axis it is admitted against; the result is
  semantics-preserving and checkable by the R2VB differential oracle; every
  applied rule discharged its concrete FP24 domain predicate.
- Declines: fail-closed to gallivm when no candidate fits and the compacted
  candidate does not split, or when a rule's domain predicate fails. Gallivm
  computes the reference, so declining is safe.

The delivery ladder, cheapest first: a producer that fits after compaction
delivers in a single pass directly (04f.4); otherwise the typed split transports
one FP32 `vec4` typed carry across a single-block cut of the compacted candidate
(04f.2 / 04f.3); otherwise the MRT carry transports several (04f.5); otherwise
gallivm runs the software reference. Compaction is tried first because a producer
that fits delivers in one pass with the application vertex data unmutated.

### Admission runs on the restaged FS, not the pre-lowering VS

The plan consumes the restaged position-pass FS NIR
(`r300_r2vb_build_restaged_fs_nir`), and the production route admission gate runs
there too. The live route reaches the plan through `r300_r2vb_route_mvp ->
r300_vs_is_fragment_aluable -> r300_vs_nir_is_fragment_aluable`, which scans the
original application VS (constant-folded and DCE'd) against the float-only
`r300_nir_op_is_fragment_aluable` whitelist. That whitelist holds float
arithmetic, transcendentals, float-domain compare/select, and moves; it excludes
the integer and Boolean source ops (`f2i32`/`f2u32`, `i2f32`/`u2f32`,
`imin`/`imax`/`umin`, `flt`, `b2f32`) a typed carry uses, so a typed producer
declines the route before restaging. The typed T0-T9 corpus rendered through
gallivm with `aluable=0` and no split token on silicon (F3-R0, stein PR#110),
confirming the typed split primitive is unreachable this way.

The fragment backend lowers those ops before RC emission
(`r300_nir_lower_bitwise_to_arith`, `nir_lower_int_to_float`, bool-to-float,
compare lowering), which is why the host pass-A/pass-B builders compile. So the
admission split follows the plan's own consumer boundary: a pre-lowering scan
keeps only the structural facts that survive lowering -- single-block control
flow, plain I/O and uniform/UBO intrinsics, a `gl_Position` output, and a bounded
set of position-feeding inputs (up to `R300_R2VB_MAX_PRODUCER_INPUTS`) mapped in
the producer's `VARYING_SLOT_VAR0 + location-rank` order, each representable by
the producer input contract. The T0-T9 corpus folds its typed carry into the
position computation alongside `inPos`, so a leading-input-only rule would reject
it again; the multi-input position path already feeds those inputs in
location-rank order. ALU-lowering capability becomes the backend verdict on the
restaged FS (`BACKEND` reject for unsupported, `OVER_ALU_BUDGET` for
split-eligible). A whitelist expansion alone is the wrong fix: it would mark an
under-budget typed producer `SINGLE` and run it without passing
`r300_mp_select_r2vb_transport`, so an unbounded, mixed-signedness, or
out-of-range carry admits whenever the program fits below 64 slots. The plan
declines under-budget typed producers (`TYPED_SINGLE_PASS_UNPROVEN`) until that
single-pass domain is proven.

The restaged FS also carries the input-conversion semantics, but skipping the
interpolation epsilon fixes one discrepancy and does not by itself establish
Draw-equivalent conversion. The fragment compile applies
`r300_nir_lower_f2i_epsilon` (`x * (1 + 2^-15)`) before `f2i32`/`f2u32` to
compensate interpolated-varying error; an R2VB producer's generated point
attributes are flat, so the nudge can cross a truncation boundary relative to
gallivm and the direct Draw VS path, which lowers integers without it. An
`r300_fs_input_semantics` distinction (`INTERPOLATED` vs `R2VB_FLAT_VERTEX`) skips
the epsilon for flat R2VB producer conversions (04f.3c). That removes the
interpolation nudge, but the FP24 producer still quantizes a runtime float before
`f2i32`/`f2u32` differently from the software VS, so a value near an integer
boundary can convert to a different integer even with the epsilon disabled. The
exact-carry proof shows the resulting integer within `+-2^17` transports exactly;
it does not show the conversion produced the same integer as the reference.
Production admission therefore needs a source-conversion-equivalence predicate
(04f.3e), and until one exists `R300_R2VB_TYPED_SPLIT` is a diagnostic contract
for a controlled corpus, not a production-safe route.

The `r300_fs_input_semantics` value is not local to the throwaway admission
compile: the same value reaches the emitted-slot measurement, the pass-A and
pass-B compiles, the actual producer FS CSO creation, and every producer variant
or cache key, so the oracle never measures a no-epsilon program the CSO then
compiles with the interpolated-fragment epsilon. The distinction stays private to
the driver -- internal `r300_create_fs_state_internal` and
`r300_fs_measure_nir_admission_internal` helpers take the semantics enum and the
public `pipe_shader_state` callback wraps `INTERPOLATED` -- rather than extending
Gallium public shader state. The canonical plan NIR stays immutable; every compile
or state creation clones it, so ownership does not depend on whether a backend
helper mutates or consumes its input.

### The source-domain predicate composes the two NIR range engines (04f.3e)

The predicate runs where the transport selection already runs:
`r300_mp_select_r2vb_transport` on the restaged position NIR, per carried
scalar. That NIR is still integer-typed at this point -- `r300_optimize_nir`
runs `r300_nir_lower_bool_to_float` and leaves integer ALU for the backend's
own lowering -- so integer conversions (`i2f32`, `u2f32`) and integer bound
chains are intact for analysis, and pass B constructs the lowered float form
itself when it builds the carry re-entry.

Admission proves two obligations jointly on each carried integer or boolean
scalar, and any scalar that fails either one declines the whole partition
(the existing decline returns already give the gate this fail-closed shape):

1. Exactly integral: `nir_analyze_fp_class` returns a class mask with
   `FP_CLASS_NON_INTEGRAL`, `FP_CLASS_NAN`, and the infinity bits clear.
   `i2f`/`u2f` results, `b2f` results (exactly {0, 1}), integral constants,
   and the rounding ops (`ffloor`, `fceil`, `ftrunc`, `fround_even`) prove
   this; a `b2i` bit-reinterpret keeps `NON_INTEGRAL` set and declines.
   An exactly integral value has no fractional part for FP24 to quantize,
   so the FP24 producer and the software vertex path hold the identical
   integer and the quantization-before-truncation divergence vanishes.
2. Within the exact window: `nir_unsigned_upper_bound` (unsigned) or the
   signed interval in `r300_mp_signed_range` (signed), applied to the
   pre-conversion integer operand, bounds the magnitude at or under `2^17`.

Both engines are conservative and total -- an unmodeled producer returns
`FP_CLASS_UNKNOWN` or an unbounded interval, and the gate reads either as a
decline -- so soundness holds by construction and the predicate can only
under-admit. The truncation semantics agree across the two references: the
NIR `f2i32`/`f2u32` definition and gallivm's conversion both round toward
zero, and inside the `2^17` window neither saturates, so the saturating
versus non-saturating split between gallivm's NIR and TGSI paths carries no
weight.

Source classes the engines prove today: boolean compares (the
bool-to-float lowering emits `b2f32`, provable and inside {0, 1} for free)
and float-encoded integer system values (the Draw-path encoding emits
`nir_i2f32` at the definition). Generic integer vertex attributes and
integer uniform, UBO, or push-constant loads reach the analysis as unknown
values and decline: `load_input` and `load_ubo` carry no class or bound
facts, and the only range-contract intrinsic in the tree
(`nir_intrinsic_arg_upper_bound_u32_amd`) is AMD-compute-specific. Lifting
those classes requires a driver-side contract that seeds the range table --
the vertex-element format (`pure_integer` plus component width) for
attributes, and a declared range for uniforms -- which is a design decision
for the implementation, not existing infrastructure. Until such a seed
exists the predicate admits the boolean and encoded-sysval classes only,
which is the correct fail-closed floor for leaving diagnostic-only status.

## Prerequisites before implementation

Compaction implementation is gated on correcting the proof contract and mining a
real workload. Optimizing `recur90` because it is the available synthetic case
would produce a rule with no measured demand and an unproven FP24 contract.

1. PROOF-FP24-01: correct `IDCT8DP4ExactBound.v` and its mirror -- remove the
   `2^24` / `B <= 1448` hardware claim, prove the RS482 stepwise accumulation
   against `2^17`. DOC-COMPACT-01 (this correction) cites the corrected theorem.
2. HBTCL-08a: telemetry-only standing-route classification of real draws --
   single-pass fits, typed-split needs, over-budget one-vec4 unsplittable
   producers, and the exact over-budget NIR shapes that recur. This supplies an
   empirical workload.
3. COMP-MINE-01: a host-only emitted-slot and resource census over a real
   producer corpus (synthetic stress cases, fitting controls, real r3v and
   GL/GLES SWTCL shaders, and the telemetry-retained over-budget shapes),
   recording which shapes occur, which are over-ALU rather than temp or const
   bound, and which patterns survive the existing NIR and RC optimizers.
4. COMP-PLAN-01: the cached transactional producer-plan framework above.
5. The first production rule is selected from the mine, not from conceptual
   elegance. The current ranking is: an exact shared-partial or dot rule if the
   mine shows a scalar pattern the existing vectorizer misses; an exact
   integer/dyadic affine subset (not the general closed form); the IDCT even/odd
   rule only in a separate generic/video lane if a real VL shader benefits; and
   presubtract/output-modifier exposure only if an RC dump proves a currently
   missed local fold.

## Validation obligations

Framebuffer equality is necessary but not sufficient; it proves output parity,
not that the intended reduced schedule ran.

Host validation per rule: recognizer positive and negative cases, a
rule-engagement token, before-and-after optimized NIR and RC schedules, the full
backend resource vector, a semantic reference comparison, exhaustive boundary-
domain tests where finite, randomized property tests inside the admitted domain,
negative tests just outside every bound, and deterministic repeated compilation.
Exact integer rules compare against arbitrary-precision integer or rational
evaluation; floating rules compare against a software model of RS482 `s1e7m16`
RTZ / FTZ / saturation, which is currently missing and is a prerequisite.

Route-chain host oracle (04f.3R): F3-R0 was itself a representation-boundary
failure -- the downstream producer tests were green while the application VS route
was unreachable -- so the oracle runs two tiers. Tier A is a direct-NIR unit:
hand-built application VS NIR through shape validation, restage, plan, and pass
compile, giving small sharply diagnosed cases. Tier B is a frontend integration:
the actual T0-T9 SPIR-V through the r3v SPIR-V/NIR preparation to the bound
application VS NIR, then the same production planner, so it exercises the exact
representation the route consumes. Tier B is the authoritative pre-silicon
classifier calibration; Tier A localizes a failure once Tier B flags it. Both run
clip and window plans, returning the plan action and reason without a draw submit.
The required rows are `T0 SPLIT {f,b}`, `T1/T2 SPLIT {f,i}`, `T3 SPLIT {f,u}`,
`T4/T5 REJECT SIGNED_RANGE`, `T6/T7 REJECT UNSIGNED_RANGE`, `T8/T9 REJECT
MIXED_SIGNEDNESS`, plus an under-budget typed producer returning
`TYPED_SINGLE_PASS_UNPROVEN`, a fractional `f2i` case just below and above an
integer boundary, an unsupported integer op, an unbounded typed chain, and the
existing float `recur90` and wide-frontier controls. This is the route-chain
calibration the pre-draw host classifier (F3-CLASSIFIER-01) only predicts and the
frozen-build silicon run (F3-R0) could not reach, so it precedes the attended
silicon transport rows.

Silicon validation per rule (attended, under the standing safety protocol: 60s
idle, `timeout 120`, kmsg guard, no PVS-port reads): the rule id and proof
manifest, pre- and post-rewrite emitted slots, clip-space and window-space
producer BO values, the carry or plan action, the R2VB wiring invariant, a raw
framebuffer comparison, boot id, the privileged kmsg delta, and RBBM status on
any timeout. Both clip and window producer variants are required: a rule that
fits in clip space but pushes the window variant over budget is not a complete
route win. A rewrite that reduces slots but cannot prove semantic equality, or
whose frame diverges on silicon, is a rule defect and is withdrawn.

## Staged implementation of the typed route

The typed route lands in four separable stages, each buildable and testable
before the next, so a source-only change never turns typed shaders production-safe
ahead of its equivalence proof.

- PR A, flat-producer semantics plumbing, changes no route admission: the
  `r300_fs_input_semantics` enum, the private FS state-creation and measurement
  helpers, the epsilon present for `INTERPOLATED` and absent for
  `R2VB_FLAT_VERTEX`, and identical semantics in measurement and CSO
  construction. Host tests inspect the transformed NIR for epsilon
  presence/absence, compile both variants, cover values just below and above
  positive and negative integer boundaries, and prove the current float-only R2VB
  producers stay byte-identical at the compiled-program level. It adds neither
  `R300_R2VB_TYPED_SPLIT` nor typed routing.
- PR B, the shadow producer planner, changes no rendering behavior: the
  `r300_r2vb_producer_plan`, the primary reason and reason mask, canonical
  restaged-NIR ownership, the multi-candidate split search, the full cost and
  result records, typed-source detection distinct from carry type, and plan
  caching by VS, computed-varying mode, clip/window space, and window key. It runs
  in shadow beside the existing float route and asserts every Family-2 float cell
  receives the same effective decision, and it adds the Tier-A and Tier-B
  route-chain host oracles.
- PR C, diagnostic typed split reachability, adds the exact-value
  `R300_R2VB_TYPED_SPLIT=1` gate and permits only an over-ALU-budget, single-block,
  known T0-T3 typed source shape whose typed value crosses a one-`vec4` cut with an
  exact carry range, matching producer/consumer logical type, both halves compiling
  under budget, and flat-input semantics selected. Under-budget typed producers and
  any typed source outside the narrow diagnostic contract decline. The T0-T9 corpus
  is corrected first (safe nonzero-`w` fold, `PT_ATTR`).
- PR D, production source-domain admission, is separate from route reachability:
  typed split becomes eligible for automatic HBTCL-08 selection only after a
  statically defensible source-domain predicate (04f.3e) exists. Until then the
  diagnostic gate proves the mechanism without constituting production closure.

## Sources

Driver: `r300_fragprog_emit.c` (`R300_PFS_MAX_ALU_INST`), `radeon_optimize.c`,
`radeon_program_pair.c` / `radeon_pair_schedule.c`,
`radeon_program_constants.h` (`RC_PRESUB_*`, `RC_OMOD_*`),
`r300_numeric_domain.c` (`R300_NUM_DOMAIN_FP24_RTZ`, `exact_int_bound = 131072`,
`significand_bits = 17` -- the FP24 `2^17` exact-integer window), `r300_r2vb.c`
(`r300_fs_measure_nir_admission`, `r300_r2vb_split_admitted`,
`r300_r2vb_build_restaged_fs_nir`), `r300_nir_ssa_cut.c` (the 04f.3 typed cut).
External proof and reduction evidence (open_gororoba, cited by name; the proofs
live there, this document carries the citation):
`proofs/theories/IDCT8EvenOdd.v` (`idct8_butterfly_eq_dense`, real-algebra 64 ->
32 multiplies), `proofs/theories/IDCT8DP4ExactBound.v` (generic `|dp8| <= 8*B^2`;
its `2^24` / `B <= 1448` hardware label is corrected to `2^17` by PROOF-FP24-01),
`proofs/theories/CDFusedBilinear.v` (`CDFusedBilinearSurface`),
`proofs/theories/FloatAxioms.v` (`FLOAT_OPS`, a field signature, algebraic layer
only), `proofs/theories/HurwitzTheorem.v` (composition bound `n in {1,2,4,8}`),
`proofs/verified/C1636_Cariow2013SedenionSchedule.v` (16 + 106 = 122),
`cd_kernel/src/cayley_dickson/{cariow_factorization.rs,fast_associator.rs}`,
`crates/turboquant/src/rotation.rs`. The Cayley-Dickson research notes
(`cayley_infinity.md` sec "Cariow &amp; Cariowa 2013") reach the same DP4-regime
verdict: the multiply-for-add trade wins on VLSI / fixed-point and loses on a
MAC-native ALU.
