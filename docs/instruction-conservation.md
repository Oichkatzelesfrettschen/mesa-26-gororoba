# Instruction conservation and read destinations

The root guide carries startup doctrine. `docs/agent-reference.md` retains every
source section as binding detail, with the explicit corrections recorded below.
The root requires the applicable reference before work in that area. The guide
keeps its engineering voice near the beginning rather than behind the loading cap.

## Source identity

The pre-integration root has SHA-256 `af06666e8055d9f1cc5268472fd217bbe45875aebb5c6f0b92dcdcdbcd8acf48`.
The source contains 27 second-level sections. Every section has a destination.
Source bytes remain recoverable from Git history and the local recovery bundle.
The section map establishes reachability; the line accounting below establishes
which wording changes. A semantic-equivalence claim requires its own review.

## Section destinations

| Source section | Detailed home | Required read |
|---|---|---|
| Instruction source | [Reference](agent-reference.md#instruction-source) | The matching task row in root Required reads by task; guide edits read the entire reference |
| Hard rules | [Reference](agent-reference.md#hard-rules) | The matching task row in root Required reads by task; guide edits read the entire reference |
| Workspace roots | [Reference](agent-reference.md#workspace-roots) | The matching task row in root Required reads by task; guide edits read the entire reference |
| Project scope and priorities | [Reference](agent-reference.md#project-scope-and-priorities) | The matching task row in root Required reads by task; guide edits read the entire reference |
| Agent operating rules | [Reference](agent-reference.md#agent-operating-rules) | The matching task row in root Required reads by task; guide edits read the entire reference |
| Driver code and patch style | [Reference](agent-reference.md#driver-code-and-patch-style) | The matching task row in root Required reads by task; guide edits read the entire reference |
| Durable names | [Reference](agent-reference.md#durable-names) | The matching task row in root Required reads by task; guide edits read the entire reference |
| Cayley-Dickson audit ladder | [Reference](agent-reference.md#cayley-dickson-audit-ladder) | The matching task row in root Required reads by task; guide edits read the entire reference |
| R300/R3V and RS480/RS482/RS485 evidence lane | [Reference](agent-reference.md#r300r3v-and-rs480rs482rs485-evidence-lane) | The matching task row in root Required reads by task; guide edits read the entire reference |
| PALM/Terakan evidence lane | [Reference](agent-reference.md#palmterakan-evidence-lane) | The matching task row in root Required reads by task; guide edits read the entire reference |
| GPU driver and reverse-engineering vocabulary | [Reference](agent-reference.md#gpu-driver-and-reverse-engineering-vocabulary) | The matching task row in root Required reads by task; guide edits read the entire reference |
| Headless hardware GL | [Reference](agent-reference.md#headless-hardware-gl) | The matching task row in root Required reads by task; guide edits read the entire reference |
| Build and cache doctrine | [Reference](agent-reference.md#build-and-cache-doctrine) | The matching task row in root Required reads by task; guide edits read the entire reference |
| Owned-origin publication and upstream intake | [Reference](agent-reference.md#owned-origin-publication-and-upstream-intake) | The matching task row in root Required reads by task; guide edits read the entire reference |
| Comments, commits, and Markdown | [Reference](agent-reference.md#comments-commits-and-markdown) | The matching task row in root Required reads by task; guide edits read the entire reference |
| Evidence boundary | [Reference](agent-reference.md#evidence-boundary) | The matching task row in root Required reads by task; guide edits read the entire reference |
| Tooling for RCA and audits | [Reference](agent-reference.md#tooling-for-rca-and-audits) | The matching task row in root Required reads by task; guide edits read the entire reference |
| Validation expectations | [Reference](agent-reference.md#validation-expectations) | The matching task row in root Required reads by task; guide edits read the entire reference |
| Security and hardware stop-line | [Reference](agent-reference.md#security-and-hardware-stop-line) | The matching task row in root Required reads by task; guide edits read the entire reference |
| Synthesis over selection | [Reference](agent-reference.md#synthesis-over-selection) | The matching task row in root Required reads by task; guide edits read the entire reference |
| Regression-on-fix discipline | [Reference](agent-reference.md#regression-on-fix-discipline) | The matching task row in root Required reads by task; guide edits read the entire reference |
| Strict clean and deletion readiness | [Reference](agent-reference.md#strict-clean-and-deletion-readiness) | The matching task row in root Required reads by task; guide edits read the entire reference |
| NIR-first compiler direction | [Reference](agent-reference.md#nir-first-compiler-direction) | The matching task row in root Required reads by task; guide edits read the entire reference |
| Build, test, and lint commands | [Reference](agent-reference.md#build-test-and-lint-commands) | The matching task row in root Required reads by task; guide edits read the entire reference |
| High-level architecture | [Reference](agent-reference.md#high-level-architecture) | The matching task row in root Required reads by task; guide edits read the entire reference |
| Key subsystems | [Reference](agent-reference.md#key-subsystems) | The matching task row in root Required reads by task; guide edits read the entire reference |
| Engineering foundations | [Reference](agent-reference.md#engineering-foundations) | The matching task row in root Required reads by task; guide edits read the entire reference |

## Corrections and surviving invariants

- The global and Mesa startup guides fit together below 32,768 bytes. The
  complete reference remains explicitly required by task, rather than being
  silently cut off after the first part of a long root file.
- R300/R3V, r600/Terakan, shared orchestration, and Mesa source retain distinct
  repository ownership. Normal Mesa workflows remain standalone.
- Evidence class and claim scope replace a universal rank override. The
  evidence search order remains a navigation aid, and contradictions receive
  investigation instead of automatic selection.
- `CDDoubleFunctor.cd_mul`, `sed_assoc_nonzero_e1_e2_e4`,
  `moreno29_orthogonal_iff`, and `cd_fidelity_stability` retain their mathematical
  identity. The corrected text states their actual scope, with no inference
  from a scalar ratio bound to statistical orthogonality.
- The T32 mnemonic remains. Its original integration list enumerates 33 axes,
  so the unsupported assertion of exactly thirty-two dimensions is removed.
- Upstream intake lands through an origin PR on a dedicated branch; rewritten
  intake history receives a new branch name. The rule preserves controlled
  intake and divergence records while following the current publication flow.
- Emoji-free text retains scientific symbols and upstream spelling. The closed
  double-hyphen and substitute-typography mandates are superseded by that rule.
- Findings use durable subject filenames and chronological record metadata.
  The older dated-filename mandate is reconciled with durable identity.
- The CI Python example uses the declared and exported `$PYTHON` interpreter.
  Every other command and architecture table remains in the detailed reference.

## Changed source lines

The following source lines change through the corrections above. Every other
nonempty source line remains exact in the reference; surrounding reference
additions supply the corrected wording. The retained source hash binds this
accounting to a concrete input.

```text
# mesa-26-gororoba Agent and Developer Reference
### Proof-backed failure modes
- Enforce closed double-hyphen delimiters: join `--` tightly to adjacent words (`word--word`), keeping punctuation fully closed.
- Integrate `upstream/main` into fork `main` exclusively through intentional rebase, recording upstream divergence explicitly.
- Publish changes directly to `origin/main` as the owned target: empower Codex and compatible agents to drive branch creation, implementation, local validation, commits, pushes, PR review resolution, merges, main synchronization, and worktree cleanup.
- Use standard typography: choose straight quotes over curly quotes, `--` over em dashes, and `...` over ellipsis glyphs.
- `CDDoubleFunctor.cd_mul`: multiplication is non-commutative in `H` and above. Failure mode: reading driver before silicon can invert the synthesis direction. Protection: read hardware tiers first.
- `T32` or `CD32` / 32D / integration envelope: resolve systemic architectural decisions across all thirty-two audit dimensions (observation, specification, code path, mechanism, hardware, kernel/ABI, test, provenance, version, configuration, generated state, resource lifetime, memory/cache, ordering, privilege, security, falsifier, performance, portability, upstream surface, maintenance, deployment, rollback, CI, documentation, operator impact, evidence independence, contradiction, alternative model, validation matrix, final synthesis, residual uncertainty, and future constraints).
- `cd_fidelity_stability`: the Lipschitz bound holds only for orthogonal sources. Failure mode: correlated sources amplify uncertainty non-linearly. Protection: verify source independence before claiming bounded confidence.
- `moreno29_orthogonal_iff`: three orthogonal nulls construct a zero divisor in `S16` and above. Failure mode: three independent `nothing failed here` results can compose to a false positive. Protection: adversarial self-review.
- `sed_assoc_nonzero_e1_e2_e4`: `[e1,e2,e4] != 0` in `O` and above. Failure mode: evidence order changes the synthesis. Protection: document evidence order and test stability under reversal.
- `steinmarder-r300/` houses reverse-engineering runners, retained evidence, findings, manifests, host kits, safety policies, and cross-repo orchestration.
3. Ground authority by name and rank: bind every technical claim to a specific named source at the highest available evidence rank. Place provenance details in commit messages and finding documents.
Architecture-affecting claims require explicit citation of a rank 1 through 4 source. Unsupported assertions remain labeled as hypotheses. When comments diverge from higher-ranked sources, cite the higher-ranked authority and align or annotate the comment.
Format finding documents with structured chronological metadata in frontmatter: include `last_verified`, `evidence_class`, dated filenames, and ordered predecessor links. Pair all PR and task references with durable mechanism identifiers.
Integrate `upstream/main` into fork `main` exclusively through intentional rebase, recording upstream divergence explicitly. Prevent automated or direct push routes from `upstream/main` directly into `origin/main`. Reconcile and resolve conflicts across the r300, r600, Terakan, and R3V domains by synthesizing non-refuted invariants to build an architecture that is better than the sum of its parts.
Maintain reverse-engineering evidence bundles, probe captures, and finding records within `steinmarder-r300/`, housing driver source code, build machinery, and upstream-facing tests within `mesa-26-gororoba/`. Consult sibling evidence in `steinmarder-r300/` prior to proposing driver modifications across Terakan, r300g, r600g, r3v, shared NIR/compiler passes, and build infrastructure.
Maintain two durable source roots in the parent workspace:
Spark the mind; verify the result; generate novel insights and engineer with unyielding discipline.
Treat build and test warnings as actionable defects. Toolchains, paths, and dependencies resolve through explicit repository configuration and standard environment variables. The root `CLAUDE.md` file links to `AGENTS.md` to ensure identical policy enforcement. Documentation provides exact references for independent verification.
Use the repository's `build-infra` entrypoint for day-to-day work.
When `open_gororoba/proofs/` is available, use proof names as review checks.
`AGENTS.md` is the root instruction file for `mesa-26-gororoba` and owns the Mesa rules.
| CI config lint checks | `python3 bin/toml_lint.py && ./.gitlab-ci/run-yamllint.sh` |
```

## Validation and remaining evidence

The integration checks section conservation, changed-line accounting, Markdown
destinations, original voice passages, exact theorem declarations, whitespace,
and the combined startup budget. The Mesa comment-hygiene gate checks the
changed documentation. A guide-size check proves budget fit, while a complete
client loading trace proves received content. A paid model behavior comparison,
a full prompt trace, and a new formal proof build are not run: no claim about
model personality outcomes, client internals, or proof recompilation depends
on those checks.

The authority for the documented loading budget is the OpenAI guide:
<https://learn.chatgpt.com/docs/agent-configuration/agents-md#how-codex-discovers-guidance>.
The source declarations live in the gororoba mathematical checkout under
`rocq/open/theories/`: `CDFidelity.v:62`, `C1546_MorZDAnticomm.v:218`,
`SedenionAssociator.v:60`, and `CDDoubleFunctor.v:59`. Their types are read
directly; no new proof claim follows from the mnemonic.
