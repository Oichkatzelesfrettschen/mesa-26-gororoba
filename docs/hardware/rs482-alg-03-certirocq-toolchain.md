# ALG-03 CertiRocq proof-carrying C toolchain

## Correction

`rocq-certicoq` and `coq-certicoq` are the wrong package names for the Rocq 9 line. The live Rocq-9 package is `rocq-certirocq`, from the CertiRocq project.

CertiRocq 0.9.1+9.1 is the relevant release for Rocq 9.1. It makes the verified Rocq-to-Clight/C lane available for proof-carrying r300 transform kernels. ALG-03 is therefore available now through a dedicated switch, not future-blocked on a hypothetical CertiCoq port.

## Switch topology

Keep the existing proof switch untouched:

- `rocq-9.1.1` / OCaml 5.4: main `open_gororoba` proof baseline.
- `rocq-verified-ext` / OCaml 5.3: `rocq-verified-extraction`, verified MetaRocq erasure to OCaml/Malfunction.
- `rocq-certirocq` / OCaml 4.14: `rocq-certirocq` 0.9.1+9.1, the Gallina-to-Clight/C lane.

The OCaml-4 switch is required because the CertiRocq lane depends on CompCert through `coq-compcert`, and CompCert remains an OCaml-4.x package. Rocq 9.1 itself is compatible with OCaml 4.14, so the same Rocq proofs re-check in that switch without downgrading the logic.

## Evidence tiers

The proof-carrying chain is tiered per kernel:

| Stage | Standing |
| --- | --- |
| Algebraic theorem in `open_gororoba` | proven: Rocq-checked, zero admits |
| FP24 exact-integer window | proven when the kernel stays inside the `IDCT8DP4ExactBound.v` bounds |
| Rocq to OCaml/Malfunction | verified for kernels extracted through `rocq-verified-extraction` with safe options |
| Rocq to Clight/C | verified only for kernels accepted by CertiRocq and recorded with their generated artifact, command, runtime/ABI assumptions, and proof status |
| Hand C translation | demonstrated by differential test only |
| Driver integration and RS485M execution | demonstrated or measured, never proven |

Do not call a driver C kernel proven merely because the algebra theorem is proven. Call it proven-to-C only when the generated C/Clight artifact is produced by the verified CertiRocq lane and the required assumptions are recorded.

## ALG-03 workflow

1. Re-check the source Rocq theory in the main proof switch.
2. Re-check the same theory in the `rocq-certirocq` OCaml-4.14 switch.
3. Generate the Clight/C artifact through CertiRocq.
4. Commit or retain the generated artifact path, the exact command, switch name, package versions, and runtime/ABI assumptions.
5. Differential-test the generated C against the extracted OCaml/Malfunction lane and the Rocq reference evaluator.
6. Integrate into the r300 producer only after the generated artifact and tests are both retained.
7. Label the result precisely: proven algebra, proven generated C/Clight where applicable, demonstrated driver integration, demonstrated silicon execution.

## Non-goals

- Do not downgrade the main OCaml-5.4 proof switch.
- Do not retry `rocq-certicoq` or `coq-certicoq` for Rocq 9 work.
- Do not collapse the FLOAT_OPS-to-FP24 boundary outside the exact-integer window without a separate bound proof.
- Do not describe a hand-translated C kernel as proven.

## Relation to HBTCL

ALG-03 does not block HBTCL-01 or HBTCL-02. The GPU wedge fix proceeds through the no-submit PM4 decode and the bounded clear-quad convergence patch. ALG-03 upgrades later r300/R2VB transform kernels so the mathematical part can travel from Rocq to C through a verified lane instead of a hand translation.

## References

- `docs/hardware/rs482-hybrid-vertex-tcl-design.md`
- `open_gororoba/proofs/theories/IDCT8DP4ExactBound.v`
- `open_gororoba/proofs/theories/Quaternion.v`
- `open_gororoba/proofs/theories/OctonionNorm.v`
- CertiRocq project: `CertiRocq/certirocq`
- MetaRocq verified extraction project: `MetaRocq/rocq-verified-extraction`
