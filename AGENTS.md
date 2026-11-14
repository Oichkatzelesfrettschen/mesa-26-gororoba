# Mesa Agent and Developer Guide

`AGENTS.md` carries the Mesa rules. `CLAUDE.md` is its tracked symbolic link;
`GEMINI.md` imports `@AGENTS.md` with that exact case. This checkout supplies
its normal build, install, test, lint, and publication workflows independently
of sibling evidence repositories. Paths in this guide resolve from the Git root.
The [engineering reference](docs/agent-reference.md) carries the complete rules,
worked examples, hardware identities, command tables, and detailed mechanisms.
The task reads its applicable sections through the directions below before acting.

## Engineering spirit

`AD AETHERA UNIVERSI PER MATHEMATICAM ET SCIENTIAM ET ARTEM TECHNICAM`

Direct all repository engineering through mathematics, scientific inquiry, and disciplined technical rigor. Welcome bold creative insight, and ground every hypothesis in concrete mechanisms, empirical evidence, robust implementations, and verifiable test outcomes.

Architect and implement with bold curiosity and rigorous craftsmanship: explore silicon pathways, weave individual observations into unified physical mechanisms, and build cohesive bridges across the compiler, driver, and kernel layers so the whole system stands far more resilient, capable, and illuminating than its individual parts.

Practice disciplined imagination: formulate bold hypotheses, and test them against source code, specifications, silicon behavior, build diagnostics, test suites, and adversarial review. Express ideas through functional code, rigorous documentation, calibrated probes, validation datasets, and clear architecture models.

Execute priorities in strict sequence: conformance, standards, stability, performance. Anchor all operations in safety; let earlier priorities firmly govern later ones.

Treat unexpected behavior and anomalies as valuable empirical findings: preserve the observation, identify the mechanism, and refine the system model accordingly.

Leave every touched file, comment, test, and finding more accurate, reproducible, navigable, and deeply grounded in primary source authority than before.

The register is direct, declarative, indicative, descriptive, and present tense.
The artifact carries the finding, mechanism, and observable consequence. Confidence
comes from evidence; curiosity, humor, and calm attention sustain the investigation.
A hypothesis receives its cheapest safe decisive test in the same turn. Each
established result enters the project record; an unresolved hypothesis keeps its
own open entry. A technical explanation stands alone for a reader who arrives cold.

Spark the mind; verify the result; generate novel insights and engineer with unyielding discipline.

## Ownership and source identity

Mesa owns Terakan Vulkan in `src/amd/terascale/vulkan/`, R3V Vulkan in
`src/amd/r300/vulkan/`, r300g and r600g in `src/gallium/drivers/`, shared
NIR/compiler paths, and `build-infra/`. The steinmarder repositories retain
their lane-specific runners and evidence. Driver edits and evidence changes
land in their native repositories through distinct PRs. A normal Mesa workflow
has no sibling runtime dependency. The [workspace and evidence reference](docs/agent-reference.md#workspace-roots)
and [evidence boundary](docs/agent-reference.md#evidence-boundary) give their homes.

Claims name the exact PCI ID, ASIC, execution path, specification, and test oracle.
The attended RS485M part, its shared die ID, and the RS480 family token carry
different scopes. PALM/Wrestler, discrete R300, and later generations supply
comparative evidence only when their scope is stated. The
[chip-identity rules](docs/agent-reference.md#chip-identity-and-comment-names)
govern hardware claims; a chip nickname alone establishes no capability.

## Required reads by task

| Task | Reference sections and owning documents |
|---|---|
| Driver, compiler, lowering, descriptor, packet, or synchronization change | [Agent operating rules](docs/agent-reference.md#agent-operating-rules), [Driver code and patch style](docs/agent-reference.md#driver-code-and-patch-style), the relevant R300/R3V or PALM/Terakan evidence lane, and [Engineering foundations](docs/agent-reference.md#engineering-foundations) |
| Hardware probe, conformance run, crash, or capability claim | [Cayley-Dickson audit ladder](docs/agent-reference.md#cayley-dickson-audit-ladder), [Evidence boundary](docs/agent-reference.md#evidence-boundary), [Validation expectations](docs/agent-reference.md#validation-expectations), [Security and hardware stop-line](docs/agent-reference.md#security-and-hardware-stop-line), and the host's documented gate |
| Meson, native file, cache, host environment, packaging, install, or clean change | [Build and cache doctrine](docs/agent-reference.md#build-and-cache-doctrine), `build-infra/CLAUDE.md`, `build-infra/README.md`, and [Build commands](docs/agent-reference.md#build-test-and-lint-commands) |
| Headless GL or loader diagnosis | [Headless hardware GL](docs/agent-reference.md#headless-hardware-gl), the selected build prefix, and runtime provider verification |
| Comments, Markdown, findings, or naming | [Comments, commits, and Markdown](docs/agent-reference.md#comments-commits-and-markdown), [Durable names](docs/agent-reference.md#durable-names), and [GPU vocabulary](docs/agent-reference.md#gpu-driver-and-reverse-engineering-vocabulary) |
| Commit, PR, conflict resolution, upstream intake, or publication | [Owned-origin publication](docs/agent-reference.md#owned-origin-publication-and-upstream-intake), [Synthesis over selection](docs/agent-reference.md#synthesis-over-selection), and [Regression-on-fix discipline](docs/agent-reference.md#regression-on-fix-discipline) |
| Cleanup, branch pruning, or worktree retirement | [Strict clean and deletion readiness](docs/agent-reference.md#strict-clean-and-deletion-readiness) and `docs/strict-clean-definition.md` |
| Instruction consolidation or guide change | The entire engineering reference and [instruction conservation record](docs/instruction-conservation.md) |

## Evidence and complete mechanisms

A behavior change records the direct observation, governing constraint,
implementation hypothesis, falsifier, validation command or bundle, and predicted
CTS/Piglit/deqp movement before changing code. Predictions stay immutable; a
deviation becomes a finding. Build, runtime, conformance, silicon, formal proof,
finite checks, and inference remain distinct. A negative bounds its conditions.
Architecture claims name their primary authority; contradictions receive
investigation rather than a universal ranking of sources.

The audit sequence connects observation, rule, mechanism, provenance, interactions,
and integration. The Cayley-Dickson names preserve its mnemonic structure.
Theorems establish their actual declarations; an engineering or statistical
interpretation requires a separate argument. The full interaction matrix and
verified source boundaries remain in the reference.

The implementation exposes data, control, ownership, lifetime, ABI, command grammar,
cache, synchronization, error, generated-state, and test-oracle relationships.
Finite mappings use tables; cleanup names expose ownership. Each patch implements
the smallest complete mechanism and preserves valid behavior. A scope cut states
its conformance cost, containment, and removal condition. A contrary model stays
open while its decisive falsifier remains unrun. Unexpected contradictions and
unsettled architecture decisions receive a stop report before behavior changes.

NIR is the compiler direction. New code adds no TGSI entrypoint, intermediate,
or lowering dependency. Existing NIR patterns precede custom abstractions;
TGSI migration follows ownership and call boundaries with one mechanism per patch.

## Builds, tools, and validation

Meson owns configuration and Ninja generation; Make and build-infra own profile
and host selection, audits, overlays, clean, build, and install orchestration.
The active C/C++ chain is `ccache -> distcc -> clang` with
`CCACHE_PREFIX=distcc`; distcc-pump stays retired from the active lane.
Rust uses its Meson-selected toolchain; Cargo's `RUSTC_WRAPPER` selects no Meson compiler.
Each build mode has its own root and prefix. Release builds supply conformance
and silicon evidence, with matching loader libraries and ICD JSON for each run.
Pacman owns system replacement; `install` and `distclean` reject `/usr`.

C and C++ translation units retain their native language with C11/C++11 floors
and the configured newer standard. Python tools support CPython 3.12 through
3.14; commands resolve `PYTHON=${PYTHON:-python3}`, export it, and invoke
`$PYTHON`. Shell automation uses POSIX `sh`; Bash extensions declare Bash.
Warnings and unexpected diagnostics receive an explanation and correction.

Every Make entrypoint requires the declared interpreter: the shell resolves
`PYTHON=${PYTHON:-python3}` and exports `PYTHON` first. The normal entrypoint
is `make -C build-infra list`, followed by
the documented profile's audit, configure, build, install, and test targets.
`make -C build-infra comment-hygiene-check` runs the calibrated documentation
gate. The reference retains the full command and architecture tables.
Generated files come from their generator. A qualification result names a clean
tree and exact commit. An unexecuted check reports `not run` and its reason.
No passing build establishes conformance, runtime reachability, or silicon behavior.
Performance claims use target-workload profiles and comparable before/after runs.

## Safety, voice, and integration

Hardware access follows exact opt-in gates, the correct target identity, and the
documented attended-host policy. GPU diagnosis captures DRM CS validation logs
before interpretation and verifies loaded-module reachability before symbolizing
addresses. Secrets, injection, unchecked writes, traversal, missing authorization,
sensitive logging, and ungated raw access interrupt feature work for containment.
Live hardware actions receive their own authorization; this guide supplies none.

Comments state the mechanism, its named authority, consequence, and guard at the
scope that needs them. Chronology and reviewer breadcrumbs live in history and
finding records. Markdown has one title, headings to level three, and tagged
fences. Checked-in text is emoji-free; mathematical symbols and upstream spelling
remain. Technical names receive durable meanings. A finding filename names its
subject while dated verification metadata lives in its record.

A merge preserves every non-refuted contribution and records each justified
supersession. The synthesis adds a stronger model, terminology map, validation
matrix, invariant, calibrated harness, or verified refactor. Diff review accounts
for removed lines and catches the failure class. Voice, examples, read triggers,
and operational rules receive separate conservation entries during consolidation.

Session worktrees live under `~/worktrees/mesa-26-gororoba/<branch>`. Origin
publication follows branch, push, PR, merge, canonical fast-forward, and cleanup;
upstream remains fetch-only and external submissions require the owner's request.
A rewritten branch receives a new name. Commits and PRs carry no AI attribution
trailer; another project's submission policy receives its own owner decision.
Upstream headers stay intact; new attribution is never invented. Worktree removal
waits for contained, preserved history and a clean working tree.

The [conservation record](docs/instruction-conservation.md) accounts for the full
reference, the moved root sections, the corrected claims, and the read triggers.
The small root improves startup reachability while the reference retains the
mechanisms and examples that give the guide its depth.
