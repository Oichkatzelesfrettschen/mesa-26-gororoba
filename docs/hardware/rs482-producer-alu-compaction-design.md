# RS482 R2VB producer ALU compaction (HBTCL-04f.4 design)

An R2VB fragment-ALU vertex producer whose derived position-pass FS exceeds the
R300 64-slot ALU ceiling declines to gallivm. The split transport (HBTCL-04f.2 /
04f.3) carries an intermediate across a single-block cut when a producer cannot
fit in one pass. This document designs the complementary lane: an algebraic
compaction pass that refactors an over-budget producer into a
semantics-preserving form that *fits* under the ceiling in one pass, so no
transport is needed. It records which reduction techniques win on this silicon,
which lose and why, the pass architecture and contract, and how it composes with
the split and MRT lanes.

## The budget target is emitted co-issued slots, not source op count

The R300 fragment ALU is a VLIW dot-machine. `R300_PFS_MAX_ALU_INST = 64`
(enforced in `r300_fragprog_emit.c`) counts co-issued slots, each holding a
vector op (`OUTC`, the RGB pipe) and a scalar op (`OUTA`, the alpha pipe)
(`radeon_code.h` `inst[].rgb_inst` + `alpha_inst`). A `DP4` is a four-wide
multiply-accumulate in one slot. The admission gate
(`r300_fs_measure_nir_admission`, read by `r300_r2vb_split_admitted` in
`r300_r2vb.c`) measures the *emitted* `alu.length` -- the slot count after the
radeon compiler has already run its own reductions:

- common-subexpression elimination, constant folding, copy propagation
  (`radeon_optimize.c`);
- presubtract source folding (`RC_PRESUB_ADD` / `SUB` / `BIAS` / `INV`,
  `radeon_program_constants.h`) -- an `a +/- b` read folded into a source, no
  slot;
- output-modifier folding (`RC_OMOD_MUL_2` / `4` / `8`, `DIV_2` / `4`) -- a
  multiply or divide by a power of two folded into the result, no slot;
- vector plus scalar co-issue pairing (`radeon_pair_schedule.c`).

A producer that measures over 64 is over *after* all of that. Compaction
therefore must change the NIR expression DAG so a reduction the low-level
peephole cannot reach becomes visible; it runs on NIR before the restage-to-RC
hand-off, then lets the existing RC reductions finish.

## Three over-budget mechanisms, three different compactions

The emitted-slot ceiling is exceeded by three distinct DAG shapes, and each
wants a different rewrite. Conflating them mis-targets the pass.

| Class | Representative | Why it exceeds 64 | Compaction |
| --- | --- | --- | --- |
| Long dependent chain | `recur90` (90x `a = 2a - u`) | A dependent chain cannot co-issue: each step needs the previous result, so N steps occupy N serial slots | Shorten the chain algebraically: an affine recurrence has a closed form, collapsing O(N) dependent slots to O(1) |
| Wide independent kernel | `wide5` (140 MADs) | Genuine op count; the ops co-issue but there are too many | Repack scalar MAD groups into native DP4 (four slots to one) and share bilinear partial sums across outputs |
| Irreducibly large | multi-attribute / MRT export | Real work exceeds one pass | Transport, not compaction: the typed split (04f.2 / 04f.3) or MRT carry (04f.5). Out of scope here |

## Winning reductions on a DP4 machine

Each rule is a semantics-preserving NIR rewrite whose success is measured by a
drop in emitted `alu.length`, with an FP24-exactness obligation.

### Affine-recurrence to closed form (the dependent-chain win)

An unrolled straight-line chain `x_{i+1} = c*x_i + k` repeated N times (the
recur-class producer, and the shape of `a = 2a - u` with `c = 2`, `k = -u`) has
the geometric closed form

```text
x_N = c^N * x_0 + k * (c^N - 1) / (c - 1)    (c != 1)
x_N = x_0 + N*k                              (c == 1)
```

The R300 fragment program is straight-line (the FS has no loops), so the
recurrence arrives fully unrolled and the recognizer is a peephole over the
unrolled chain: N identical `a = c*a + k` steps collapse to one evaluation of
the closed form. O(N) dependent slots become O(1). This is the largest single
win for the recur-class and is the direct answer to a chain that cannot
co-issue.

Exactness envelope: the closed form must stay inside the FP24 exact-integer
window. For `c = 2` the term `c^N = 2^N` is FP24-exact only while `2^N < 2^24`
(N <= 23); a longer chain, or a non-dyadic `c`, discharges the exactness
obligation only under its own bound or declines. The fixed-point identities the
recur-class relies on (`a = 1` fixed point of `2a - u` at `u = 1`) are integer
identities and reduce cleanly.

### Even/odd butterfly with shared partial sums (the IDCT win)

For a producer whose bilinear form carries reflection symmetry
`C[k][N-1-n] = (-1)^k * C[k][n]` -- the DCT/IDCT cosine parity -- the outputs
`n` and `N-1-n` share the same even and odd four-term partial sums and differ
only in the sign of the odd part. Computing the shared partials once halves the
multiplies and adds a handful of sign-controlled adds. For the 8-point IDCT
this is 64 multiplies to 32 plus 8 add/sub, proved exact in the open_gororoba
proof tree (`proofs/theories/IDCT8EvenOdd.v`, theorem
`idct8_butterfly_eq_dense`). This is a genuine total-op cut, not a
multiply-for-add trade, because the shared partial sums remove real work rather
than moving it between pipes.

Exactness envelope: the DP4 lane is arithmetically exact for bounded integer
operands. The eight-term dot with operands bounded by B satisfies
`|acc| <= 8*B^2`, integer-exact in the 24-bit FP24 mantissa iff `8*B^2 < 2^24`,
i.e. `B <= 1448` (`proofs/theories/IDCT8DP4ExactBound.v`, `dp8_abs_bound`,
`fp24_ceiling_positive`). The UINT7 lane (`B = 127`) gives `8*127^2 = 129032 <
2^17`, strictly inside the mantissa (`dp8_int7_within_2pow17`). Producers whose
operands stay in that window reduce exactly; wider operands fall back to the
FP24 rounding floor and decline if the caller requires bit-exactness.

### DP4 repack and shared-partial CSE (the wide-kernel win)

A dense dot product expressed as a scalar MAD chain occupies one slot per term;
the same dot as a native `DP4` is one slot for four terms. Repacking
MAD groups toward the four-`DP4` floor, and sharing identical bilinear partials
across multiple outputs (the mechanism underneath the IDCT butterfly,
generalized), removes slots the local peephole cannot recover because the
sharing requires reassociation across the expression tree. A dense 4x4 vertex
transform is already at its four-`DP4` floor; an affine row (constant `w_clip`)
folds to three `DP4` plus a move.

### Presubtract and output-modifier exposure

Restructuring an `a - b` so the radeon presubtract detector fires, or a
multiply/divide by a power of two so the output modifier fires, moves an
arithmetic op off the slot count into a source or result modifier. RC does this
locally; the compaction pass exposes the opportunity by canonicalizing the DAG
into the shape those detectors match.

## Rejected reductions: the multiply-minimization family loses on DP4

The classic operation-count-reduction family -- Walsh-Hadamard / butterfly
multiply reduction, Karatsuba, Strassen, Winograd short convolution, and
per-vertex Cayley-Dickson multiply -- trades multiplies for adds. On a machine
whose native op is a four-wide multiply-accumulate in one slot, a multiply and
an add cost the same slot, so the trade converts one-slot MACs into several
add slots and *loses*. Both source archives reach this conclusion
independently, and the rejection is recorded here so a future contributor does
not re-derive it.

| Rejected technique | Op-count claim | Why it loses on DP4 | Source |
| --- | --- | --- | --- |
| Cariow sedenion multiply (16-point WHT + sparse remainder) | d=16: 256 -> 122 multiplies but 240 -> 298 adds (+58 add) | The +58 adds are extra slots on a MAC machine, and the 106-entry sparse remainder is irregular multiply-accumulate that does not vectorize; wins only where a hardware multiplier is the scarce resource (VLSI / fixed-point) | `cariow_2013_fast_sedenion_multiplication.pdf` sec 3; `cd_kernel/src/cayley_dickson/cariow_factorization.rs`; proof `proofs/verified/C1636_Cariow2013SedenionSchedule.v` (`16 + 106 = 122`) |
| Karatsuba 3-for-4 on the CD doubling product | complex 4 -> 3 multiplies, recursion `~d^1.585` | The doubling product `(a,b)(c,d) = (ac - conj(d)b, da + b*conj(c))` mixes conjugation and non-commutative operand order, so the cross-term trick `(a+b)(c+d)` needs `a*d = d*a`, which fails at quaternions and beyond; grounded in the Hurwitz composition bound `n in {1,2,4,8}` (`proofs/theories/HurwitzTheorem.v`) | `cd_kernel/src/cayley_dickson/fast_associator.rs` header; `proofs/theories/HurwitzTheorem.v` |
| Fast Walsh-Hadamard rotation | `O(d^2) -> O(d log d)`, d=128: 16384 -> 896 MADs | The crossover is `d >= 64`; below that the dense matmul is already cheaper. The per-vertex transform is `d = 4` -- far below crossover, so the butterfly only adds loop and cross-lane-add overhead | `crates/turboquant/src/rotation.rs` L32; `docs/.../turboquant_cd_optimization_analysis.md` |

The archive's own empirical falsifier is instructive: the project's
`mul_optimized` path did not realize its claimed dim-32 reduction and stayed at
1024 multiplies, not the 498 target. A claimed reduction is not a realized one
until it is measured. Cayley-Dickson structure keeps its legitimate home in the
once-per-frame matrix build, never the per-vertex hot loop; the WHT butterfly is
available only for a `d >= 64` producer, which the vertex transform is not.

## Pass architecture: a certified-rewrite pipeline

The compaction pass borrows its shape from the open_gororoba certification
records, which model exactly a "reducible operation": an alternate op schedule
bundled with proofs that it equals the reference and that the algebraic laws
justifying it hold.

- `FLOAT_OPS` (`proofs/theories/FloatAxioms.v`) separates the algebraic identity
  from the numeric backend: a rewrite is proved once over an abstract field
  signature, then discharged against the FP24 domain. The compaction rules carry
  their exactness obligation in that separated form -- the algebra is exact over
  the ring, and a per-rule FP24 bound decides whether the concrete backend
  preserves it.
- `CDFusedBilinearSurface` (`proofs/theories/CDFusedBilinear.v`) is the contract
  shape: a `fused_mul` bundled with proofs that it (1) equals the reference
  `mul`, (2) is bilinear, and (3) is scale-homogeneous. A compaction rule is the
  same object -- an alternate schedule plus a proof it equals the reference plus
  the linear-algebra law that licenses further rewriting.
- `Cariow2013SedenionScheduleSurface` (`proofs/verified/C1636_Cariow2013Sedenion
  Schedule.v`) is the count-plus-equality template: a reduced-count spec paired
  with a semantic-equality theorem. Each compaction rule reports a slot-count
  delta and carries the equality obligation.
- The evidentiary tags the archive attaches to each count (theorem-proved versus
  target-claimed) map onto the cost model: a rule contributes a measured slot
  delta, not a claimed one.

The pass is a list of rules applied in cost-ranked order:

```text
CompactionRule {
    precondition(nir)  -> bool          // structural recognizer
    rewrite(nir)       -> nir'          // semantics-preserving transform
    equals_reference   : obligation     // nir' computes the same result
    fp24_exact         : obligation      // within the FP24 exact envelope, or bounded
    cost_delta(nir,nir'): int           // emitted alu.length after - before
}
```

The driver applies each rule whose precondition matches, re-measures
`r300_fs_measure_nir_admission` after each, keeps the rewrite only when
`cost_delta < 0` and both obligations discharge, and stops as soon as the
producer fits under 64. The initial rule set, cost-ranked, is:

1. affine-recurrence to closed form (largest dependent-chain win);
2. even/odd butterfly with shared partials (IDCT-shaped producers);
3. DP4 repack toward the four-`DP4` floor;
4. reassociate-then-CSE for shared bilinear partials;
5. presubtract / output-modifier exposure.

The multiply-minimization rules are absent from the set for the per-vertex
`d = 4` producer and are gated behind a `d >= 64` precondition that the vertex
transform never satisfies.

## Contract and composition

The pass is a pure NIR-to-NIR transform with a fail-closed guarantee.

- Consumes: the restaged position-pass FS NIR
  (`r300_r2vb_build_restaged_fs_nir`) plus the FP24 exact-integer envelope.
- Guarantees: the emitted `alu.length` is reduced or unchanged, never increased;
  the result is semantics-preserving and checkable by the R2VB differential
  oracle (route-on frame equals gallivm frame); every applied rule discharged
  its FP24-exactness obligation or was skipped.
- Declines: fail-closed to gallivm when the producer still measures over 64
  after all rules, or when a rule's exactness obligation cannot be discharged in
  the operand window. Declining is safe -- gallivm computes the reference.

Compaction composes with the transport lanes as an ordered ladder, cheapest
first:

1. compaction (04f.4): if the producer fits under 64 after rewriting, deliver in
   one pass, no transport;
2. typed split (04f.2 / 04f.3): if it still does not fit, cut at a single-block
   SSA frontier and transport one FP32 `vec4` typed carry across the two halves;
3. MRT carry (04f.5): if one carry is insufficient, transport multiple carries
   through render targets;
4. gallivm: if none apply, the software reference path.

Compaction is tried first because a producer that fits needs no carry BO, no
re-ingest, and no second pass, so it is strictly cheaper than transport when it
succeeds.

## Validation obligations

Compaction is an implementation lane (04f.4 is open); this document is its
design. When implemented, each rule earns trust the same way the split did:

- a host oracle per rule, mirroring `r300_r2vb_producer_split_test.c`: build a
  producer that triggers the rule, assert the rewrite fires, assert the emitted
  `alu.length` drops below 64, and assert semantic equality against the dense
  form;
- an RS482 differential-frame cell per rule in the HW-03.10 corpus: route-on
  frame byte-identical to the gallivm frame, under the standing safety protocol
  (60s idle, `timeout 120`, kmsg guard, no PVS-port reads);
- the exactness obligation validated against the operand window the rule
  advertises, with the FP24 bound (`8*B^2 < 2^24`, `B <= 1448`; UINT7 inside
  `2^17`) as the acceptance envelope.

A rewrite that reduces slots but cannot prove semantic equality, or whose frame
diverges on silicon, is a rule defect, not a producer defect, and the rule is
withdrawn.

## Sources

Driver: `r300_fragprog_emit.c` (`R300_PFS_MAX_ALU_INST`), `radeon_optimize.c`,
`radeon_program_pair.c` / `radeon_pair_schedule.c`,
`radeon_program_constants.h` (`RC_PRESUB_*`, `RC_OMOD_*`), `r300_r2vb.c`
(`r300_fs_measure_nir_admission`, `r300_r2vb_split_admitted`,
`r300_r2vb_build_restaged_fs_nir`), `r300_nir_ssa_cut.c` (the 04f.3 typed cut).
External proof and reduction evidence (open_gororoba, cited by name; the proofs
live there, this document carries the citation):
`proofs/theories/IDCT8EvenOdd.v` (`idct8_butterfly_eq_dense`, 64 -> 32),
`proofs/theories/IDCT8DP4ExactBound.v` (`dp8_abs_bound`,
`dp8_int7_within_2pow17`, `fp24_ceiling_positive`; `B <= 1448`, UINT7 in
`2^17`), `proofs/theories/CDFusedBilinear.v` (`CDFusedBilinearSurface`),
`proofs/theories/FloatAxioms.v` (`FLOAT_OPS`),
`proofs/theories/HurwitzTheorem.v` (composition bound `n in {1,2,4,8}`),
`proofs/verified/C1636_Cariow2013SedenionSchedule.v` (16 + 106 = 122),
`cd_kernel/src/cayley_dickson/{cariow_factorization.rs,fast_associator.rs}`,
`crates/turboquant/src/rotation.rs` (`d >= 64` WHT crossover). The Cayley-Dickson
research notes (`cayley_infinity.md` sec "Cariow &amp; Cariowa 2013") reach the
same DP4-regime verdict: the multiply-for-add trade wins on VLSI / fixed-point
and loses on a MAC-native ALU. A dim-16 Cariow multiplier count of 84 in
`fast_associator.rs` is stale; the proved and implemented count is 122.
