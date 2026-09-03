# mesa-26-gororoba Agent and Developer Reference

## Instruction source

`AGENTS.md` is the root instruction file for `mesa-26-gororoba` and owns the Mesa rules.

All Mesa agents and contributors follow `AGENTS.md`. Codex, compatible agents, and human contributors read it directly. Other root agent files exist only to load it for tools that require a tool-specific filename.

`CLAUDE.md` is a tracked repository-relative symbolic link to `AGENTS.md`, so Claude Code and other readers receive the canonical rules directly. `GEMINI.md` loads `@AGENTS.md`, spelled in that exact case because imports resolve literally on a case-sensitive filesystem, and exists only while Gemini CLI is part of the workflow.

Root instruction paths are tracked repository entries. The `AGENTS.md` body lives in one place; `CLAUDE.md` resolves to that body, and loader files reference it without copying doctrine that can drift.

## Hard rules

These rules are enforceable. Later sections explain mechanism or rationale and never weaken or contradict them.

### Generating principles

Seven principles generate the rules in this file. A case no rule names resolves by the nearest principle.

1. Assign durable mechanism identity: name every durable artifact--identifier, comment, claim, citation--by its mechanism or content. Confine chronology, actors, and workflow state to commit messages, finding records, and registry metadata.
2. Use mechanism-centered voice: state what the system does, the state transitions it executes, and the observable outcome. Express boundaries through positive restrictions, designated homes, or governing mechanisms (`release builds only`; `chronology lives in the commit message`; `CP DMA prefetches run fire-and-forget, and the wait consolidates at end-of-IB`). Reserve negation for safety and security stop-lines where absence itself is the fact.
3. Ground authority by name and rank: bind every technical claim to a specific named source at the highest available evidence rank. Place provenance details in commit messages and finding documents.
4. Separate evidence classes: label known, hypothesized, and speculative claims distinctly. Maintain separate classes for build, runtime, conformance, and silicon evidence. Record predictions before observation, and treat deviations as new findings.
5. Maintain a single canonical home per fact: store each fact in one primary location and link to it from secondary sites. Limit duplication to a single hard-rule summary line and its corresponding expansion section.
6. Implement the smallest complete mechanism: scope every patch, comment, and tool to its distinct operational facts--bounded, complete, and singular in purpose.
7. Enforce fail-closed gates: open hazardous execution paths on exact opt-in values exclusively (`KEY=1`), keeping unset, empty, or zero states locked. Calibrate verdict-producing tools against known-good and known-bad inputs before deployment.

### Boundary, paths, and instruction files

- Execute all standard Mesa workflows--building, installing, testing, source-commenting, upstream intake, and owned-origin publication--entirely within this repository. Confine driver code changes to `mesa-26-gororoba` and evidence bundles to `steinmarder-r300`. Coordinate cross-repository changes through sibling checkouts and distinct pull requests, keeping each file in its native repository.
- Use repository-relative paths, generated native configuration files, PATH-resolved binaries, or explicit user root variables for checked-in assets. Discover the workspace root dynamically via `repo_root=$(git rev-parse --show-toplevel)`.
- Restrict local absolute paths, private host FQDNs, user-specific toolchains, raw IP literals, and worktree names to external local environment state.

### Root cause, evidence, and conformance

- Establish the exact driver path, target silicon, test case, specification clause, and kernel or Mesa mechanism before proposing or applying a behavior change.
- Identify silicon through precise PCI IDs, register definitions, ISA mappings, and physical measurements; reserve family nicknames (such as PALM/Wrestler) for descriptive prose.
- Derive root cause directly from primary authorities: cited ISA sections, kernel dispatch functions, specification paragraphs, and test oracles.
- Classify and mark claims across findings and source comments into explicit tiers: known facts, working hypotheses, and exploratory conjectures.
- Record the exact symbol-discovery technique alongside every code locator: `(clangd: textDocument/references on FUNC)`, `(global -r SYMBOL)`, `(ast-grep --pattern PATTERN)`, `(rg --fixed-strings SYMBOL src/)`.
- Formulate and record a complete falsification harness before changing driver code: state the direct observation, governing spec constraint, implementation hypothesis, falsification criterion, validation command or retained bundle path, and predicted test movement across CTS, Piglit, or deqp suites.
- Begin GPU fault analysis with a decisive `dmesg` capture to check for DRM CS validation rejections.
- Target the RS480(RS482/485) / K8 / SB600 Vostro 1000 platform through its registered out-of-tree kernel modules (radeon GPU-reset + hazard mitigation, SB600 watchdog, EC thermal) as documented in `docs/hardware/vostro1000-kernel-modules.md`.
- Symbolize crashes by verifying active module reachability in `/proc/PID/maps` or `gdb info sharedlibrary`.
- Segregate build, runtime, conformance, and silicon findings into distinct, unmixed evidence classes.
- Ground conformance claims in executed test runs; use build success strictly to prove compilation.
- Preserve recorded predictions immutably across observations; honor collected probe data as durable evidence; account for the exact specification conformance cost of any temporary workaround.
- Explore boldly and synthesize fearlessly: combine disparate clues into unified physical models, architecting cross-subsystem bridges that reveal hidden invariants and leave the whole system far stronger and more intelligible than the sum of its parts.

### Builds, tests, and verdicts

- Treat compiler warnings and unexpected tool or test outputs as actionable defects until fully explained.
- Surface unexpected diagnostic outputs immediately in the active log or report.
- Compile modified code cleanly and warning-free under all configured compiler flags.
- Itemize all built components, executed tests, skipped suites, blocked steps, and unavailable prerequisites in the run report.
- Designate unexecuted suites as `not run` accompanied by the exact operational reason.
- Authoring calibration: calibrate newly authored probes, linter rules, and verdict scripts against verified positive and negative baselines before relying on their output.
- Preserve existing build targets, test suites, and validation passes intact when implementing narrow fixes.
- Execute the authoritative generator tool when updating generated files, or record the exact rationale if the toolchain is unavailable.
- Generate authoritative validation results from clean working trees at the declared SHA: confirm `git status --porcelain=v2`, `git diff HEAD`, and `git diff --cached HEAD` are empty, with `HEAD` matching the declared SHA in an isolated qualification worktree.

These rules expand in `build-infra/CLAUDE.md` and `Validation expectations`.

### Languages and scripts

- Maintain each translation unit in its established native language and configured language standard.
- Author and compile C translation units against C11 or newer baselines.
- Implement C++ backends against C++11 or newer standards, meeting or exceeding configured compiler flags.
- Treat C11 and C++11 as foundational baseline floors, adopting newer standard features where configured.
- Isolate languages strictly per translation unit, keeping C and C++ source boundaries distinct and cleanly linked.
- Target Python tooling to CPython 3.12 through 3.14 inclusive, using supported standard library APIs across that entire span.
- Write portable shell automation targeting POSIX `sh`; declare `#!/bin/bash` explicitly on scripts that invoke bash-specific extensions.
- Craft every script and translation unit as an elegant, purposeful mechanism: align toolchains, harmonize interfaces, and build cohesive bridges across languages so each component elevates the collective architecture.

### Build orchestration

- Consult and follow `build-infra/CLAUDE.md` for build and cache doctrine prior to altering any Meson option, native file, host environment variable, install prefix, build directory, or cache lane.
- Maintain a strict division of responsibilities: assign build configuration and Ninja generation exclusively to Meson; govern host selection, audit checks, generated native overlays, cleaning, building, and installation via Make and build-infra.
- Audit dependencies against target host capabilities when evaluating Meson defaults, matching host-detected behaviors for omitted or `auto` options.
- Gate hazardous execution paths behind explicit opt-in matches (`R300_TRACE_HAZARD_ACCEPTED=1`), holding unset, empty, or zero states firmly closed until granted.
- Wire distcc/ccache integrations through Meson `[binaries]` using `CCACHE_PREFIX=distcc`; configure compiler wrappers directly in Meson native files and position `distcc-pump` ahead of caching layers.
- Bind the Rust toolchain to a stable system path in Meson native files, allowing Meson Rust to resolve compilers independently of `RUSTC_WRAPPER`.
- Confine compiler selection, audit policies, clean routines, build runs, installation paths, and hazard consents strictly to Make and build-infra tooling.
- Architect the build matrix as a deterministic, resilient pipeline: streamline build steps, eliminate ambient variance, and orchestrate compiler, caching, and native layers into a harmonious, reproducible engine.

### Git, merge, and owned-origin publication

- Assign durable, mechanism-centered names to branches, commit subjects, PR titles, source comments, finding filenames, and bundle directories; establish the branch name, initial commit subject, and PR title prior to the first push, confining transient metadata (waves, phases, missions, sessions, PR numbers, reviewers, agents, and worktree tags) strictly to registry fields.
- Preserve all non-refuted content across branches through active union and synthesis; restrict selective hunk elimination strictly to changes empirically refuted by evidence or superseded by verified rationale.
- Require explicit user authorization and a detailed rationale in the commit message before applying force-pushes to `main` or shared branches.
- Treat pre-commit hook bypasses as emergency procedures requiring explicit justification recorded in the commit message.
- Integrate `upstream/main` into fork `main` exclusively through intentional rebase, recording upstream divergence explicitly.
- Publish changes directly to `origin/main` as the owned target: empower Codex and compatible agents to drive branch creation, implementation, local validation, commits, pushes, PR review resolution, merges, main synchronization, and worktree cleanup.
- Treat external remotes as intake sources; execute submissions to freedesktop.org or third-party repositories only under separate, explicit user authorization.
- Synthesize git history into an enduring record of architectural progress: reconcile divergent ideas, fuse parallel discoveries into cohesive commits, and build an unbroken lineage of verified engineering.

### AI disclosure, authorship, and copyright

- Add the Mesa `Assisted-by:` trailer to commit messages when policy requires disclosure, reserving `Generated-by:` for changes almost entirely produced by AI.
- Reserve `Co-authored-by:` trailers exclusively for human co-authors.
- Confine source file headers strictly to copyright and SPDX license grants; place all AI disclosures (such as `(LLM-assisted)` or `Generated by Claude`) in commit trailers.
- Preserve historical pre-policy `Co-Authored-By: Claude` trailers intact as immutable historical records.
- Preserve upstream copyright headers verbatim across file movements, keeping author names and attribution years intact under a unified attribution block.
- Equip new source files with their SPDX license grant exclusively; retain existing copyright lines only when they cite a verified legal holder, stripping template defaults, personal placeholders, and project collectives during intake.

### Comments, prose, and safety

- Author source comments to stand self-contained for future Mesa maintainers, grounding every statement in public, durable authority. Confine provenance trails, internal trackers, PR chronologies, wave labels, task numbers, author signatures, local paths, private host names, deictic timestamps, and retained-experiment names strictly to commit messages and finding records. State the verified physical or logical mechanism directly on the exact chip and execution path.
- Equip comments with dense functional content: construct compact semantic tables and state diagrams to map descriptor words, packet layouts, bit fields, and state transitions directly. Maintain functional text formatting; position structural models that span a file at file or type scope, linking local call sites back to them.
- Apply American (United States) English spelling across new and modified source comments, commit messages, and project documentation. Preserve original spelling in untouched upstream comments to protect history from formatting churn.
- Deliver complete, functional code with verified rationale in every patch, executing intentional behavioral and structural improvements.
- Report engineering outcomes and decisions directly: state the result, the chosen course, the supporting evidence, and the residual uncertainty, ensuring every sentence delivers a usable, actionable fact. Name the positive mechanism that governs the system boundary.
- Prioritize critical security and unsafe hardware-access defects above all feature work: isolate the condition, lock hazardous paths, and report findings immediately before resuming normal tasks.
- Protect shared workspace paths and maintain their persistent integrity across sessions.

These rules expand in `Comments, commits, and Markdown`, `Driver code and patch style`, and `Security and hardware stop-line`.

## Workspace roots

Maintain two durable source roots in the parent workspace:

- `mesa-26-gororoba/` houses Mesa source: Terakan, r300g, r600g, NIR/compiler code, and Meson/build/install infrastructure.
- `steinmarder-r300/` houses reverse-engineering runners, retained evidence, findings, manifests, host kits, safety policies, and cross-repo orchestration.

Manage temporary Git worktrees named `mesa-26-gororoba-*` linked to `mesa-26-gororoba/`. Land worktree changes onto `main` through branches, rigorous reviews, and additive merges. Retire temporary worktrees cleanly upon branch merge or supersession through this disciplined sequence:

- Verify that the worktree working tree is clean and its `HEAD` is reachable from a pushed ref. Preserve unpushed or dirty state onto a mechanism-named branch, or retain it within the `steinmarder-r300` preservation corpus.
- Execute `git worktree remove <path>` from the parent repository and verify its complete removal via `git worktree list --porcelain`.
- Remove the out-of-tree Meson build directory associated with the checkout, temporary patch snapshots, and scratch logs, while retaining durable evidence under the `steinmarder-r300` retention contract.

Execute repository-wide maintenance with `git worktree prune`: run `git worktree prune --dry-run --verbose`, classify each reported administrative entry, and prune only confirmed stale pointers. Retire external worktrees via their respective parent repositories; run `git worktree repair` after parent moves to reconcile absolute worktree pointers.

Preserve native repository boundaries: store steinmarder-r300 evidence bundles and findings in `steinmarder-r300/`, and commit driver source changes to `mesa-26-gororoba/`. Coordinate cross-repository integration across sibling checkouts and distinct pull requests.

## Project scope and priorities

Track upstream Mesa 26.x `main` within `mesa-26-gororoba`. Focus active development on the Terakan (Vulkan Evergreen/NI), R3V (Vulkan R300-R500), r600g, and r300g Gallium driver subsystems and associated compiler/lowering pipelines.

Execute priorities in strict sequence: conformance, standards, stability, performance. Anchor all operations in safety; let earlier priorities firmly govern later ones.

Deliver full specification conformance with every valid fix. Document the exact conformance cost, containment boundary, and scheduled removal path for any temporary workaround.

Precede all source modification with disciplined investigation: review source code, specifications, tests, execution logs, generated artifacts, kernel execution paths, CTS/Piglit/deqp behavior, commit history, and retained evidence. Execute work through a deliberate pipeline: scope the task, isolate the component, partition claims, collect primary evidence, model the mechanism, design the change, implement the smallest complete mechanism, verify outcomes against oracles, and document the verified result.

Leave every touched file, comment, test, and finding more accurate, reproducible, navigable, and deeply grounded in primary source authority than before.

## Agent operating rules

Let Mesa documentation govern build, install, and review workflows, while drawing empirical evidence and runner automation from `steinmarder-r300/`.

Architect and implement with bold curiosity and rigorous craftsmanship: explore silicon pathways, weave individual observations into unified physical mechanisms, and build cohesive bridges across the compiler, driver, and kernel layers so the whole system stands far more resilient, capable, and illuminating than its individual parts.

### Operating stance

Operate at reverse-engineering depth: target exact silicon, exact execution paths, exact specifications, exact test suites, and empirical evidence. Conformance adjustments balance against performance or patch size only with documented cost accounting.

The priority hierarchy in `Project scope and priorities` serves as an operational invariant. Workarounds document their conformance delta, containment boundary, and removal path.

Treat build and test warnings as actionable defects. Toolchains, paths, and dependencies resolve through explicit repository configuration and standard environment variables. The root `CLAUDE.md` file links to `AGENTS.md` to ensure identical policy enforcement. Documentation provides exact references for independent verification.

### Work sequence

Execute operations within the active repository. Read source files, documentation, execution logs, test suites, revision history, generated files, and prior evidence before formulating conclusions. Treat persistent memory and generated summaries as research leads requiring primary verification.

Follow an ordered execution tree: research before editing, model before designing, verify before declaring completion. Each tool call resolves a specific inquiry or advances an implementation step.

Derive models from primary sources: Mesa source, Linux kernel source, Khronos Vulkan/OpenGL specifications, AMD ISA/register manuals, CTS/Piglit/deqp test suites, and retained steinmarder-r300 evidence.

Deliver complete, robust implementations. Every code modification fulfills a tracked functional role. Trace blockers through interacting subsystem layers to resolve the root mechanism directly.

### Evidence rank

When sources conflict, higher rank controls for example:

1. Silicon evidence: probe capture, register readback, hardware CTS execution.
2. ISA, register, hardware, and API specifications: AMD Evergreen ISA, Bobcat BKDG, Vulkan/OpenGL specifications.
3. Kernel source: radeon DRM, `evergreen_cs.c`, kernel commit history.
4. Driver source: Terakan Vulkan, r600g Gallium, r300g Gallium, r3v.
5. CTS/Piglit/deqp behavior with spec-grounded test oracles.
6. Documentation and source comments consistent with ranks 1 through 5.

Architecture-affecting claims require explicit citation of a rank 1 through 4 source. Unsupported assertions remain labeled as hypotheses. When comments diverge from higher-ranked sources, cite the higher-ranked authority and align or annotate the comment.

### Falsification record

Before applying code changes during hardware RCA, record:

- direct observation;
- source or spec constraint;
- implementation hypothesis;
- falsification criterion;
- validation command or retained bundle path;
- expected CTS/Piglit/deqp movement.

Prediction form:

- If this fix is correct, these CTS/Piglit/deqp cases change from FAIL to PASS: `[list]`.
- If `[alternative condition]`, the hypothesis is falsified.

When CTS, Piglit, deqp, or tool results deviate from prediction, the deviation is the finding. Open a new RCA instead of changing the prediction after observation. Build success, runtime success, conformance success, and silicon evidence are separate evidence classes.

### Stop points

Stop implementation and report when:

- a hypothesis survives three independent falsification attempts;
- a hypothesis fails in an unexpected way;
- the fix requires a non-obvious architecture choice;
- a measurement contradicts a rank-1 or rank-2 source.

Report the evidence chain, alternatives, tradeoffs, and next evidence needed. Treat surprise as a finding; a silent pivot buries it.

### Reasoning checks

Use these checks during RCA, conformance, and hardware-facing changes.

- Silicon-to-test chain: trace data and control flow from hardware register, bitfield, field definition, driver API, and command emission to the CTS/Piglit/deqp assertion before diagnosing.
- Hypothesis tree: list plausible root causes, rank them by evidence cost and likelihood, test the cheapest decisive case first, and prune only on falsification.
- Opposition review: after forming a synthesis, argue the strongest contrary case. If the contrary case survives, keep the finding unresolved and name the next evidence needed.
- Claim audit: before committing a finding, list each implementation claim and the rank-1 through rank-6 source that backs it.

## Driver code and patch style

Expose domain mechanics directly through driver code: express API objects, chip generations, register fields, command packets, memory lifetimes, synchronization edges, format tables, and error paths as explicit structures. Let names, structs, data tables, assertions, and cleanup labels carry primary explanatory weight, reserving comments strictly for constraints that source constructs cannot directly declare.

Apply direct, concrete naming across every subsystem:
- Prefix public entry points with their subsystem name.
- Name local helpers with precise mechanism verbs: `emit`, `lower`, `fill`, `find`, `validate`, `lock`, `unlock`, `init`, `finish`, and `destroy`.
- Preserve domain terminology across constants, registers, packet fields, and enum translations.
- Model state objects, key structs, and descriptor words as strongly typed driver state.

Structure code according to the governing mechanism:
- Route named operations into concise helper functions.
- Encode finite mappings directly into structured data tables.
- Maintain hardware and API matrices in layouts that keep their operational invariants immediately visible.
- Scope long functions strictly to locally auditable flows where feature bits, packet words, ioctl validation, resource lifetimes, ordered emissions, and unwind edges stay visible across all exit paths.

Model error handling as an explicit ownership topology:
- Use intentional cleanup labels (`fail`, `unsupported`, `err_*`, `out`, `put`, `free`) to show active object lifetimes, violated invariants, and unwinding sequences.
- Align status codes with subsystem ABI conventions: return `VkResult` and emit `vk_error` in Vulkan paths; return negative errno and assert fences in kernel-facing layers.
- Assert impossible internal states, and validate all external inputs at system boundaries.

Map finite domains through structured data tables:
- Tabulate formats, register fields, enum translations, chip gates, descriptor words, packet layouts, and workaround selectors in direct lookup tables.
- Preserve material distinctions between chip families, ABI boundaries, and specification requirements.

Maximize proof density across every patch: craft the smallest complete mechanism that preserves exact domains, visible state transitions, reviewable cleanup edges, and falsifiable outcomes. In commit messages and PR descriptions, state the component prefix, mechanism, invariant-backed rationale, validation evidence, and test movements plainly.

## Durable names

Assign durable mechanism and content names to every durable artifact: apply mechanism-centered naming to branch names, bundle directories, finding filenames, PR titles, commit subjects, source comments, and checked-in identifiers.

Confine transient workflow identity--such as waves, phases, missions, agents, worktrees, sessions, reviewers, PR numbers, task numbers, and process chronologies--strictly to registry metadata fields.

Adopt mechanism-centered name patterns:
- Branch: use `terakan/palm_mem_rat_atomic_completion_semantics` (rather than phase or wave labels).
- Commit subject: use `terakan: validate PALM MEM_RAT atomic completion semantics`.
- PR title: use `palm_mem_rat_atomic_completion_semantics discriminator`.
- Source comment: use `cached MEM_RAT atomic path`.
- Finding filename: use `2026-05-18-palm-mem-rat-atomic-completion-semantics.md`.
- Test or bundle artifact: use `palm_dual_mem_rat_atomic_silicon_cut`.

Establish the branch name, first commit subject, and PR title prior to the first push, ensuring that squash-merge subjects faithfully preserve mechanism identity.

Name artifacts by what they contain and execute:
- Describe the mechanism, target, and payload directly.
- Replace container and aggregation jargon (`tranche`, or ordinal containers like `set5`, `batch_2`, `group3`) with specific descriptive domain compounds (`group_map`, `subgroup`, `workgroup`, `batch_size`, or Vulkan descriptor `set`).

Derive durable names by isolating the active mechanism and object:
- `builds the imageStore reference IB` -> `capture_reference_ib`
- `byte capture through one uprobe on libdrm drmIoctl` -> `DRMIOCTL_UPROBE_SCRIPT`
- `next-15 blocker frontier manifest` -> `BLOCKER_FRONTIER_MANIFEST`

Re-read artifacts and re-derive names whenever a label relies on chronology or staging.

## Cayley-Dickson audit ladder

Apply Cayley-Dickson depth levels as functional audit gates across evidence planes, systematically checking each dimension before advancing a claim:

- `R` / 1D / observation: pinpoint the exact file, symbol, packet, register, test, log line, or artifact (e.g. `line 1214 calls r600_nir_lower_cube_to_2darray`).
- `C` / 2D / rule: cite the governing specification, ABI, API contract, ISA text, test oracle, or commit contract (e.g. `r600g calls int_tg4 after cube_to_2darray; Terakan does not`).
- `H` / 4D / mechanism: map the complete code path, kernel validation rule, lowering pass, descriptor format, allocator constraint, cache policy, runtime behavior, or hardware constraint (e.g. `Evergreen forces nearest filtering for integer GATHER4`, ISA section 9.4).
- `O` / 8D / provenance: verify through live probes, dmesg logs, hardware counters, traces, disassemblies, CTS/Piglit/deqp runs, benchmarks, or retained evidence bundles (e.g. `missing pass plus silicon behavior gives wrong corners; all 12 cube cases fail`).
- `S16` / 16D / interaction lattice: evaluate the multi-axis interaction matrix across build/runtime, generated/hand-written, host/target, silicon/family, source/finding, and code/evidence planes.
- `T32` or `CD32` / 32D / integration envelope: resolve systemic architectural decisions across all thirty-two audit dimensions (observation, specification, code path, mechanism, hardware, kernel/ABI, test, provenance, version, configuration, generated state, resource lifetime, memory/cache, ordering, privilege, security, falsifier, performance, portability, upstream surface, maintenance, deployment, rollback, CI, documentation, operator impact, evidence independence, contradiction, alternative model, validation matrix, final synthesis, residual uncertainty, and future constraints).

Execute audits along the Cayley-Dickson ladder with strict rigor:
- Climb systematically through 2D and 4D rule-and-mechanism checks to eliminate invalid hypotheses before investing in 8D captures.
- Back every 8D finding with its explicit claim, evidence chain, and falsification criterion.
- Synthesize 16D lattices and 32D envelopes from verified, stable lower levels, explicitly registering any residual uncertainty.
- Test interaction lattices actively: verify that individual passing components compose into a harmonious, robust whole.
- Respect evidence direction and order: read hardware and ISA specifications before kernel code, kernel code before driver logic, and driver logic before CTS oracles; confirm that models remain invariant when tested in reverse order.
- Verify source independence across evidence streams before asserting bounded confidence, protecting against correlated false positives.

### Proof-backed failure modes

When `open_gororoba/proofs/` is available, use proof names as review checks.

- `CDDoubleFunctor.cd_mul`: multiplication is non-commutative in `H` and above. Failure mode: reading driver before silicon can invert the synthesis direction. Protection: read hardware tiers first.
- `sed_assoc_nonzero_e1_e2_e4`: `[e1,e2,e4] != 0` in `O` and above. Failure mode: evidence order changes the synthesis. Protection: document evidence order and test stability under reversal.
- `moreno29_orthogonal_iff`: three orthogonal nulls construct a zero divisor in `S16` and above. Failure mode: three independent `nothing failed here` results can compose to a false positive. Protection: adversarial self-review.
- `cd_fidelity_stability`: the Lipschitz bound holds only for orthogonal sources. Failure mode: correlated sources amplify uncertainty non-linearly. Protection: verify source independence before claiming bounded confidence.

## R300/R3V and RS480/RS482/RS485 evidence lane

Target hardware context: Vostro 1000 / AMD Athlon 64 / K8 + Radeon Xpress 200M/1100/1150 (RS480 `1002:5954`, RS482/RS485 `1002:5974`, RS482M `1002:5975`; the Vostro 1000 carries `1002:5974` with subsystem `1028:022a`, the Radeon Xpress 1150 / RS485M product) + SB600. Contrast with discrete R300 (`CHIP_R300`), R350, RV350/RV380, and R420/R500 ASICs.

Distinguish RS480-family IGP architecture from discrete R300:
- Geometry execution: RS480/RS482/RS485 lacks hardware vertex processing / TCL engines (`num_vert_fpus = 0`). Geometry routes through host SW-TCL or Render-to-Vertex-Buffer (R2VB) carrier textures re-ingested into VAP (`R300_VAP_CNTL`).
- Memory controller & aperture: UMA host system memory accessed through the RS480 Northbridge indirect register path (`RS480_NB_MC_INDEX` / `RS480_NB_MC_DATA` in `rs400.c` / `rs400d.h`).
- Fragment shader ALU: Fixed VLIW FP24 (s1e7m16) Ultra Shader pipeline (`R300_US_CONFIG`, `R300_US_ALU_RGB_ADDR_0`, `R300_US_ALU_ALPHA_ADDR_0`).

Structure R300/R3V and RS480/RS482/RS485 reverse-engineering and RCA along this evidence hierarchy:

1. Exact physical measurement: physical RS482/RS485 probe captures, `dmesg` validation logs, BAR/debugfs snapshots, and hardware test executions retained under `steinmarder-r300/results/`.
2. Exact driver and kernel source: R3V Vulkan (`src/amd/r300/vulkan/`), r300g Gallium (`src/gallium/drivers/r300/`), R300 common core (`src/amd/r300/common/`), Linux kernel DRM radeon (`drivers/gpu/drm/radeon/rs400.c`, `r300.c`, `r300d.h`, `rs400d.h`), and registered out-of-tree Vostro 1000 kernel modules (`docs/hardware/vostro1000-kernel-modules.md`).
3. Authoritative hardware manuals: AMD `R3xx_3D_Registers.pdf` / `.txt`, AMD K8 Family 0Fh BKDG, and AMD IGP BIOS Developer Guides.
4. Comparative later-generation manuals: RS690 RRG (`43372_rs690_rrg_3.00o.pdf`), R5xx Acceleration Architecture Guides (`R5xx_Acceleration_v1.1` to `v1.5.pdf`), RV630/M76 RRGs, explicitly labeled as comparative and requiring empirical corroboration.
5. Supporting leads: community driver notes, compiler tree summaries, and historical DRI documentation.

### Chip identity and comment names

Formulate chip identity strings so they remain directly searchable across codenames, product designations, Mesa enum identifiers, platforms, and ISA families. Use the standard RS482 source-comment format, with the Palm format beneath it for Evergreen-era work:

`RS485M (Radeon Xpress 1150, CHIP_RS480, R300-class US/PFS fixed VLIW)`

`Palm (Wrestler GPU, CHIP_PALM, Evergreen / TeraScale-2 VLIW5)`

Standard chip identities:

- `R300` (Radeon 9700): `CHIP_R300`; discrete R300 with hardware TCL.
- `RS480` / `RS482` / `RS485` (Radeon Xpress 200/1100/1150): `CHIP_RS480`; UMA integrated IGP, SW-TCL only (`num_vert_fpus = 0`), R2VB capable. The die is the register-file and ISA scope; the part cut from it is the measurement scope, and the Vostro 1000 carries the mobile `RS485M` (Radeon Xpress 1150), which its video BIOS names `RS485/M`.
- `R420` (Radeon X800): `CHIP_R420`; discrete R400.
- `RV515` / `RV530` / `R580` (Radeon X1000 series): `CHIP_RV515` / `CHIP_R580`; discrete R500 with US500 dynamic branching.
- `R600` (HD 2900): `CHIP_R600`; R600 / TeraScale-1.
- `Cypress` (HD 5870, `RV870`): `CHIP_CYPRESS`; Evergreen / TeraScale-2 VLIW5.
- `Palm` (HD 6310, Wrestler GPU, Brazos platform + Bobcat CPU, integrated): `CHIP_PALM`; Evergreen / TeraScale-2 VLIW5.
- `Cayman` (HD 6970): `CHIP_CAYMAN`; Northern Islands / TeraScale-3 VLIW4.
- `Aruba` (Trinity APU GPU, integrated): `CHIP_ARUBA`; Northern Islands / TeraScale-3 VLIW4.

Standard source-comment nomenclature:

- Driver: `R3V` for Vulkan R300-R500; `r300g` for R300 Gallium; `Terakan` for Vulkan Evergreen/NI; `r600g` for R600 Gallium.
- Register name: `R300_VAP_CNTL`; `R300_GA_COLOR_CONTROL`; `R300_US_CONFIG`; `CB_COLOR0_VIEW.SLICE_START`; `SQ_TEX_RESOURCE_WORD4.DST_SEL_X`.
- Register macro: `R_000148_MC_FB_LOCATION`; `R_0007C0_CP_STAT`; `R_028C70_CB_COLOR0_INFO`; `S_028C70_FORMAT(x)`; `G_028C70_FORMAT(v)`; `V_028C70_COLOR_32`.
- ISA encoding: `FMT_32_32_32 = 47`; `V_028C70_NUMBER_USCALED = 0x2`.
- Architecture: `R300-class US/PFS fixed VLIW`; `Evergreen / TeraScale-2 VLIW5`; `Northern Islands / TeraScale-3 VLIW4`; `R600 / TeraScale-1`.
- CPU side: `AMD K8 (Family 0Fh)` for Vostro 1000; `Bobcat` for Zacate/Ontario; `Llano` CPU.
- Platform: `Dell Vostro 1000` (AMD K8 + RS485M + SB600); `Brazos` for Bobcat + Palm; `Llano`; `Trinity`.

Ground hardware and API citations in public primary documents and exact section numbers. Confine internal extracts, bundle paths, and audit records to the evidence layer, deriving citation authority from public standards:

- Cite public ISA manuals: `per AMD Evergreen-Family ISA, section 10.x.x (MEM_RD_SCATTER)` (rather than internal text line offsets).
- Cite family architecture references: `per AMD Radeon HD 6000-Series ISA (Cayman), section X.Y` (rather than phase audit scratch directories).
- Cite official programming guides: `per AMD 3D Engine Programming Guide for Evergreen, section M (CB_COLOR0_VIEW)`.
- Cite authoritative API specifications: `per Direct3D 11.3 Functional Specification, section 4.4.6 Element Alignment`.

Embed governing hardware facts directly in hardware-specific source comments: declare bitfield structures such as `SLICE_START bits 0-10 of CB_COLOR_VIEW`, empirical behavior such as `Palm silently no-ops MEM_RAT_CMPXCHG_INT on the cached path`, and every mathematical invariant or workaround mechanism required to render the code fully intelligible.

## PALM/Terakan evidence lane

Target hardware context: x130e / AMD E-300 / Radeon HD 6310, PCI `1002:9802`, `CHIP_PALM`, PALM/Wrestler, Evergreen, TeraScale-2, VLIW5. Distinguish PALM/Wrestler from SUMO and SUMO2 (Llano platform contexts).

Structure PALM/Terakan reverse-engineering and RCA along this evidence hierarchy:

1. Exact PALM physical measurement: hardware probes, `dmesg` logs, BAR/debugfs-safe paths, hardware performance counters, CTS/Piglit/deqp execution on physical x130e silicon.
2. Exact driver source: Terakan Vulkan, r600g/SFN Gallium, Mesa/NIR passes, Linux kernel radeon CS validation, and DRM UAPI.
3. Architecture manuals: Evergreen, TeraScale-2, and VLIW5 ISA references and 3D engine register programming guides.
4. Adjacent hardware families: R600, R700, and Cayman contrast cases, explicitly labeled as inherited or adjacent and validated against PALM evidence.
5. Supporting leads: generated summaries, source comments, and historical notes, treated strictly as exploratory leads requiring primary verification.

Ground all PALM hardware-RCA modifications in the `Falsification record` and the hard-rule `dmesg` DRM CS and module-reachability checks.

## GPU driver and reverse-engineering vocabulary

Lead with precise mechanism terms before summary abstractions. State the exact path affected by every Mesa claim: compiler lowering, descriptor construction, packet emission, kernel validation, resource lifetime, memory/cache behavior, runtime loader state, or conformance outcome.

Apply mechanism-centered terminology across each path:

- Silicon identity: PCI ID, ASIC family, Mesa chip enum, IP block, generation, stepping, feature bit, engine, ring, aperture.
- Compiler path: NIR, TGSI, SPIR-V input, lowering pass, legalization, instruction selection, register allocation, scheduling, backend emission, disassembly oracle.
- Descriptor/resource path: BO, descriptor word, reloc-adjusted VA, pitch, tiling, swizzle, cache policy, coherency domain, map/unmap boundary, lifetime rule.
- Command stream: PM4 packet, indirect buffer, packet grammar, register write, draw or dispatch boundary, relocation, CS validator, fence, sequence number.
- Kernel interface: DRM UAPI, ioctl path, GEM, TTM, radeon object, CS parse path, fence wait, reset path, debugfs path, KMS interaction, dmesg validation error.
- Runtime path: ICD or DRI loader choice, dispatch table, winsys, screen/context/resource object, Gallium pipe state, Vulkan object lifetime, debug/release contamination.
- Evidence: CTS/Piglit/deqp result, dmesg delta, shader disassembly, packet decode, retained bundle, calibrated probe, golden trace, known-good/known-bad oracle.
- Upstream surface: minimal patch surface, bisectability, conformance delta, reviewer burden, ABI/install impact, backport risk, maintenance owner.

Anchor broad capability assertions in their verified evidence class, using explicitly bounded claim formulations. Examples:

```text
Known: PALM accepts this packet sequence through the radeon CS validator, and the retained CTS run observes the expected output.

Hypothesis: this is a descriptor-word construction bug, not a silicon-capability claim.

Speculative: adjacent Evergreen behavior suggests the same cache-domain rule, but exact PALM evidence is not yet conclusive.
```

Reserve `breakthrough` strictly for discontinuous discoveries that alter the evidence topology. For standard Mesa engineering, describe the governing mechanism directly: `driver enablement`, `conformance improvement`, `lowering-path correction`, `descriptor-path repair`, `packet grammar recovery`, `hazard-model refinement`, `silicon-behavior characterization`, `validation-methodology improvement`, or `source-grounded architecture model`.

## Headless hardware GL

Follow `headless-hardware-gl-runner.md` to access the hardware GL provider on a
display-manager host: bind render-node EGL/GBM execution to the target PCI
device, route every X-window-system test through the target-scoped glamor X
path, and verify the server and direct-client providers independently. A
`DRISWRAST` AIGLX provider classifies indirect GLX as software rendering; an
`llvmpipe` or `swrast` client renderer classifies that direct-client run as
software rendering.

## Build and cache doctrine

Mesa builds from this repository alone with reproducible native files and
environment variables. Meson owns configuration and Ninja generation. Make
and build-infra own host selection, audit checks, generated native overlays,
cleaning, building, and installation. A build-system change requires explicit
authorization before changing that division.

Use `meson configure` and repository-local options before choosing a build
configuration. Scripts discover their checkout through
`repo_root=$(git rev-parse --show-toplevel)` and use repository-relative
paths, PATH-resolved tools, or declared user roots. An omitted or `auto`
Meson option receives an audit of the target-host dependencies that Meson
enables. Hazardous submit paths require exact opt-ins such as
`R300_TRACE_HAZARD_ACCEPTED=1`.

Release, debugoptimized, and debug builds use separate build directories and
install prefixes. Release builds supply conformance and silicon evidence;
debugoptimized and debug builds change timing, allocation, and error behavior.
Run complete configure, build, and install cycles for one build before using
another. A probe declares one prefix through its Meson `prefix` and `libdir`,
sets matching `LIBGL_DRIVERS_PATH`, `LD_LIBRARY_PATH`, and, for Vulkan, the
matching installed ICD JSON. Mixed release/debug loader paths invalidate an
evidence claim.

The active profiles live in `build-infra/configs/` and
`build-infra/configs/alternates/`; the Makefile resolves `PROFILE=` by
basename. `4_r300_full_release_*` provides the R300 conformance baseline.
The numbered clang profiles use
`vostro1000-x86-64-v1-clang22-ccache-distcc.env`; the GCC diagnostic profile
uses `vostro1000-x86-64-v1-gcc-ccache-distcc.env` with
`COMPILER_FAMILY=gnu`. Historical pump environments remain under
`build-infra/env/Archive/` and supply no active Make target.

Each profile maps to `build/mesa-<profile>/` and an isolated default prefix
`/opt/local/mesa-<profile>`. The only operator-selected shared prefixes are
`/opt/local/mesa-26-gororoba` for release and
`/opt/mesa-gororoba-debug-optimized` for debugoptimized, with
`/opt/local/mesa-gororoba-debug-optimized` as its compatibility alias. An
in-repository install tree contaminates the worktree and supplies no valid
evidence surface.

`ninja -C <builddir> clean` preserves Meson configuration. A Meson-option or
Meson-version change uses `meson setup --wipe <builddir>`, followed by full
build and install cycles. Only Meson changes generated
`meson-private/cmd_line.txt`.

Native files name PATH-resolved compilers or generated local overlays. Make
writes version-coupled LLVM helpers to `$BUILDDIR/mesa-toolchain.meson` before
configuration and selects a coherent installed clang/clang++/llvm-config set,
honoring `MESA_LLVM_VERSION` when declared. The active C/C++ cache chain is
`ccache -> distcc -> clang` through Meson `[binaries]` plus
`CCACHE_PREFIX=distcc`; Rust uses `sccache -> rustc`. Wrapper scripts that
invoke `ccache distcc clang` remain retired because ccache identifies distcc
as the compiler and rejects multi-source calls. `RUSTC_WRAPPER` controls Cargo
only and does not select Meson Rust. Cache wiring changes update the
reproducible recipe after consulting the workspace ccache and sccache notes.
`ccache --show-stats --verbose` records the cache state; a warmed unchanged
build normally exceeds 90 percent hits.

## Owned-origin publication and upstream intake

Direct all repository publications to `origin/main` as the owned fork target, treating `upstream` (`mesa/mesa` on freedesktop.org) strictly as a fetch-only reference. Publish code changes exclusively to `origin`. Fetch and integrate upstream improvements through deliberate intake procedures, and reserve external upstream publication strictly for separately authorized workflows.

Integrate `upstream/main` into fork `main` exclusively through intentional rebase, recording upstream divergence explicitly. Prevent automated or direct push routes from `upstream/main` directly into `origin/main`. Reconcile and resolve conflicts across the r300, r600, Terakan, and R3V domains by synthesizing non-refuted invariants to build an architecture that is better than the sum of its parts.

Drive full lifecycle development through an unbroken sequence of verified operations: create mechanism-named branches, perform local validation, author focused commits, push to `origin`, open pull requests targeting `origin/main`, resolve active review findings, execute additive merges, synchronize `origin/main`, and cleanly retire associated worktrees. Empower Codex and compatible autonomous agents to complete this workflow end-to-end.

Confine external upstream publication to explicit user authorizations that specify the external destination, scope, credentials, and review boundary prior to submission.

Adopt `docs/submittingpatches.rst` on the mesa-26 branch as the foundational patch-construction and review standard while directing publication to `origin/main`. Isolate functional logic from formatting churn across separate commits, scope patches to single components wherever feasible, maintain warning-free builds, preserve bisectability across every commit, validate changes rigorously, and deliver review-ready changes free of temporary fixup commits.

Scope fork-local Terakan, R3V, r300, and r600 changes by explicit mechanism rather than aggregation batches.

Format commit messages with structured precision:
- Prefix commit subjects with their component name and a concise mechanism summary.
- Author commit bodies that state the invariant, explain the defect or mechanism at maintainer depth, cite primary evidence, and report test outcomes plainly.
- Reference GitLab issues using `Closes:`.
- Cite introducing commits using `Fixes:` strictly when referencing the specific commit that introduced the defect.
- Apply `Backport-to:` or the active Mesa stable marker only when required.

### AI-assistance trailers

Reserve `Co-authored-by:` trailers exclusively for human co-authors.

Disclose tools used for a commit through explicit trailers:

```text
Assisted-by: Claude (Opus/Sonnet 4.x), ChatGPT Codex (5.x), Gemini (Flash/Pro 3.x), Mistral, Ollama, DeepSeek
```

Apply `Assisted-by:` to collaborative human/AI contributions, and reserve `Generated-by:` for changes produced almost entirely by AI. Omit trailers on trivial or purely mechanical refactors when Mesa policy permits. Place all disclosures exclusively in commit trailers.

Preserve historical pre-policy commits containing `Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>` intact as permanent historical records. Use `Assisted-by:` or `Generated-by:` for all new commits.

Preserve original trailers in checked-in patch snapshots (such as `src/re/r600/results/.../*.patch`, kernel-module patch series under `src/amd/...`, and embedded DKMS sources) verbatim as immutable historical artifacts.

### File headers, copyright, and SPDX

Apply Mesa header standards from `docs/submittingpatches.rst` on the mesa-26 branch: use concise SPDX-only headers across new source files unless explicit copyright lines are required.

Equip newly authored source files with their SPDX license identifier:

```c
/* SPDX-License-Identifier: MIT */
```

Format headers with descriptions using standard block comments:

```c
/*
 * SPDX-License-Identifier: MIT
 *
 * <one-line file description>
 */
```

Include copyright lines strictly when identifying verified legal copyright holders. Strip invented project collectives, local git configuration usernames, and default LLM template attribution (`Copyright (c) YYYY <git config user.name>`), ensuring that unadorned SPDX grants represent the verified state.

Confine source headers strictly to licensing terms, routing AI disclosures (`Assisted-by:`, `Generated-by:`) exclusively to commit trailers.

Preserve existing upstream copyright headers verbatim across file movements, splits, and refactorings, retaining original author names and attribution years (including Vitaliy "Triang3l" Kuzmin-authored Terakan files and historical contributor lines). Integrate new contributions to existing upstream files under their established headers without prepending additional project-collective lines.

## Comments, commits, and Markdown

Treat source code as the primary authority. Reserve source comments for explaining mechanisms that lie beyond direct inline syntax: record silicon constraints, specification rules, kernel validation paths, command-stream invariants, hardware quirks, ABI boundaries, synchronization edges, and the conformance-preservation rationale behind workarounds.

Adhere to local formatting and repository conventions:
- Follow `.editorconfig`, `.clang-format`, adjacent file conventions, and repository-specific build rules.
- Format Mesa C/C++ using 3-space indentation, no tabs, 78-column limits where practical, and focused Doxygen function summaries.
- Apply American (United States) English spelling across all newly authored or modified source comments, commit messages, and project documentation (`honor`, `behavior`, `initialize`).
- Preserve original spelling in untouched upstream comments to protect history from formatting churn.
- Maintain quoted specification text, kernel symbols, hardware identifiers, and diagnostic strings verbatim.

Ground source comments in durable, public authority:
- Name the exact governing mechanism, silicon target, ISA/register rule, API clause, kernel validator, test class, or measured behavior.
- Confine transient workflow metadata--task IDs, PR references, issue numbers, companion PR tags, wave/phase/mission labels, worktree names, agent identifiers, author tags, absolute paths, private FQDNs, raw IPs, deictic time anchors, dated claim tags, internal repo paths, and campaign-specific experiment terms--strictly to commit messages, findings, and registries.
- Translate experimental findings into direct physical mechanisms: state the property demonstrated by an experiment as the mechanism itself on the exact chip and execution path where it holds.
- Cite named rules directly so source comments remain fully intelligible without requiring open specification documents; append stable specification section numbers as supplemental disambiguation where useful.

Size comments proportionally to their operative mechanisms:
- Use concise one-line labels for self-evident local sections.
- Construct compact, connected blocks when dependencies cross API boundaries, lowering passes, allocation limits, kernel validation rules, or hardware behaviors.
- Group multiple facts into a single comment when they form a unified causal, temporal, conditional, or ownership chain.
- Move persistent structural and architecture models to file or type scope (such as descriptor layout tables), keeping call-site comments scoped to local linkages.
- Encode complex descriptor layouts, packet fields, bit allocations, and state transitions into compact semantic tables and state diagrams, keeping them free of decorative ASCII borders and separator boxes.
- Match the established rhythm and cadence when editing Triang3l-authored Terakan files: maintain compact line lengths, focused clauses, and comments centered on silicon, specification, or test mechanics.

Format commit messages and PR metadata for maintainer review:
- Prefix commit subjects with their subsystem component and a concise mechanism summary.
- Craft commit bodies that state the root invariant, describe the change at maintainer depth, cite primary evidence, and report test outcomes in one to five sentences.
- Place build invocations, test logs, validation checklists, and environment details in PR descriptions rather than commit bodies.
- Link PRs via `Part-of:` trailers rather than appending PR numbers to subject lines.
- Isolate logic changes from formatting churn across distinct commits, keeping every commit buildable, reviewable, and bisectable.

### Comment class and placement

Generate comments through four explicit architectural decisions: semantic role, evidence class, language form, and placement. Structure each comment to fit its specific role--contracts, cross-layer invariants, single-line register facts, descriptor tables, silicon workarounds, and unsafe-code proofs require distinct information layouts:

- Semantic role: express API contracts (behavior, ownership, lifetimes, error boundaries), translations (API-to-register or API-to-packet layouts), local invariants (register bitfields, formats, equations), hardware quirks and workarounds, structural representations (descriptor words, packet layouts, state machines), safety proofs, or file-scope navigation maps.
- Evidence class: express documented behavior in the direct indicative; identify observed-but-undocumented behavior by its target generation and execution path; label working hypotheses explicitly (`hypothesis:`, `speculative:`) or refine them prior to check-in.
- Language form: select standard Mesa C/C++ line or block comments, Doxygen comments where the surrounding file adopts them, field-level annotations, compact semantic tables, or structured Rust attributes.
- Placement: establish persistent architecture models at file or type scope; scope call-site comments strictly to local linkages (skip conditions, bound states, offset rules); position branch comments directly at the branch to state its discriminating invariant.

Document Rust implementations according to rusticl standards: apply `//!` for module contracts, `///` for public items and struct fields, explicit `# Safety` / `# Panics` / `# Errors` sections for API obligations, and `// SAFETY:` immediately preceding every unsafe block. Formulate each `// SAFETY:` comment as a rigorous proof encompassing all operative preconditions--lifetime bounds, ownership transfers, aliasing exclusivity, and synchronization edges:

```rust
// SAFETY: `self.pipe` remains valid for the lifetime of `self`, and the raw
// `pipe_context` retains the `screen` pointer until `PipeContext::drop`.
```

Confine AI disclosures (`Assisted-by:`, `Generated-by:`) exclusively to commit trailers.

### Stating mechanism as fact

State what a component is and does in positive, declarative form. Declare the governing mechanism directly and let its operational constraints stand as fact: `the vbuf stage maps the vertex buffer after every allocate_vertices, so the draw is dropped at submission`. State the concrete mechanism and the constraint that enforces it; correctness follows naturally from the mechanism, rendering defensive correctness claims and contrastive framing unnecessary.

Formulate system boundaries in their positive duals: specify the explicit restriction (`release builds only`), declare the designated home (`chronology lives in the commit message`), or state the governing mechanism directly (`CP DMA prefetches run fire-and-forget, and the wait consolidates at end-of-IB`; `this descriptor shape resolves only through the texture path`; `the caller retains the allocation`; `radeon's sync is implicit dma_resv only`). Collapse multiple absences into the shared positive fact: `a producer that fits delivers in one pass`. Preserve explicit prohibitions strictly for hard-stop safety, security, and hardware hazards where absence is the whole fact.

Confine historical design debates and rejected alternatives to commit messages; mention rejected alternatives in source comments strictly when rejection is itself the active lifetime, safety, or conformance invariant enforced by the code (`aliasing these descriptor shapes reads past the bound range, so the pipeline rejects them`). Maintain established posture terms (`correct-or-reject`).

Write in third-person present tense: `the kernel reads WORD0`, `the TX unit ignores NEAREST for integer formats`. Reserve the impersonal `we` exclusively for the code execution path (`we wait at the end of the IB`), routing project and team references to commits and findings. Let ceremonial prose fall away; wherever terse phrasing obscures the underlying invariant, deliver the explicit mechanism.

Structure comments for silicon bugs and hardware workarounds around their core facts: name the affected chip or register, state the observable outcome in a single sentence, and cite the public bug URL or ISA section. Deconstruct workaround explanations into distinct, unmixed layers: state the empirical observation, name the affected generation and execution path, declare the rule enforced by the code, and label any remaining hypothetical aspects.

Formulate uncertainty around the active mechanism, naming the guarded, disabled, or falsifiable code path, free of workflow chronology, private workspace context, and session state.

### Source comment shape

Anchor comment styling to established repository examples: the WORD0 fix block in `src/amd/terascale/vulkan/terakan_dispatch.c` near `PKT3_SET_RESOURCE` and `desc[0] - bo->va`, and the inline silicon notes in `terakan_format.c` and `sfn_instr_mem.cpp`.

Order the components of a comprehensive mechanism comment systematically:

1. Governing invariant: state the primary operational invariant (`WORD0 carries the per-BO byte offset.`).
2. Public or source authority: cite the governing rule by name (`evergreen_packet3_check`, AMD ISA chapter, register macro, or specification clause), appending stable section numbers as supplemental disambiguation.
3. Consequence: show the direct result, using inline code snippets when clearer than prose (`ib[WORD0] = reloc->gpu_offset + offset`).
4. Test reference: cite the specific test case when explaining a fixed defect (`CTS case or dEQP-VK.<group>.*`).
5. Environment knobs: group configuration flags and environment overrides at the end of the comment block.

Apply the smallest complete subset of this order to match the scope of the change. Maintain this sequence as a logical dependency flow--claim, governing authority, observable consequence, and execution guard--binding related facts into a single unified movement so the consequence is visibly derived from the constraint. Express single local facts in a concise sentence, and connect multi-stage mechanisms into short, coherent blocks.

### Mechanism movement

Compose comments around complete mechanism movements rather than isolated sentences. State the governing object or operation, trace it through the constraint shaping the code, and conclude with the consequence or guard that renders the constraint useful. Use dependent clauses, participial phrases, appositives, and causal connectives to preserve exact relationships between core technical claims. Advance the established subject through coordinate clauses during continuous mechanisms, opening a new movement when ownership or operational scope changes.

Derive comments directly from the mechanism model: identify the subject, initial state, active constraint, state transition, observable consequence, and execution scope, linearizing this dependency graph into a single coherent movement. When connecting documented behavior to empirical outcomes, classify each evidence source explicitly.

Favor concise, high-density comments: place a focused one-line trailing comment on the target line, reserving multi-line blocks for functions that embody complex, non-obvious invariants.

Center each comment block on a single mechanism movement. Combine multiple sentences within a block when they advance a shared state machine, ownership transition, or causal chain. Start a new comment when the mechanism, ownership domain, phase, or evidence tier shifts.

Let the code declare what happens, and let comments explain why the code has that structure: cite silicon constraints, specification rules, kernel validators, empirical measurements, or cross-subsystem invariants.

Cite authorities by concrete identifier: `SQ_TEX_RESOURCE_WORD4.DST_SEL_X`, named AMD Evergreen-Family ISA rules, Vulkan specification clauses, or kernel validation functions. Confine internal line numbers and private paths to commit messages and findings.

Employ active voice and causal linking when one invariant necessitates another:

```text
CP DMA prefetches into L2 run fire-and-forget, so the command stream keeps
emitting after issuing the prefetch; the one wait lands at end-of-IB before
the stream retires.
```

Express execution sequences through causal and temporal relationships, linking distinct phases into a coherent progression:

```text
The kernel reads WORD0 as a byte offset, adds reloc->gpu_offset, and presents
the buffer base to the shader at the intended VA; any per-element offset
requested by the caller survives relocation.
```

Reserve multi-paragraph comment blocks strictly for intricate silicon quirks and complex cross-layer hardware hazards.

### TODO comments

Maintain source-level TODOs directly at the affected mechanism, treating rule documents as policy guides rather than issue trackers. Open deferred-work comments with standard markers (`TODO:`, `FIXME:`, `XXX:`, or `HACK:`); deliver complete implementations for new work without placeholders, treating pre-existing `PLACEHOLDER:` markers as evidence-bearing historical artifacts.

Include three mandatory mechanism elements in every TODO-family comment:
- Missing work: identify the specific function, register, ISA section, kernel symbol, or specification chapter requiring enhancement.
- Deferral reason: name the exact silicon quirk, ABI requirement, or evidence gap currently blocking completion.
- Tracking artifact: cite a durable function name, register identifier, `gitlab.freedesktop.org` issue URL, specification chapter, or silicon-constraint identifier.

Confine reviewer notes, PR references, phase/wave/mission tags, rule numbers, and deictic markers (`currently`, `previously`, `this driver`) strictly to commit messages and PR descriptions.

Structure TODO comments according to this standard shape:

```text
/* TODO: missing work --
 *           <function, register, ISA section, kernel symbol, or spec
 *           chapter that needs the change>.
 *       reason --
 *           <silicon, ABI, or evidence constraint blocking completion>.
 *       tracking-artifact --
 *           <durable function, register, gitlab.freedesktop.org issue
 *           URL, spec chapter, or silicon-constraint name>.
 */
```

Treat `TODO`, `FIXME`, `XXX`, `HACK`, and existing `PLACEHOLDER` comments as evidence-bearing artifacts. Evaluate local context, historical rationale, architectural impact, and testability before modifying or resolving a marker. Systematically scope every unresolved gap upon discovery: record parameter sweeps, symbolic mutations, theoretical foundations, hardware assumptions, register ambiguities, command-stream hazards, ABI risks, build discrepancies, test gaps, probe limits, and undocumented dependencies.

### Finding documents and agent-loaded Markdown

Format finding documents with structured chronological metadata in frontmatter: include `last_verified`, `evidence_class`, dated filenames, and ordered predecessor links. Pair all PR and task references with durable mechanism identifiers.

Reference durable identifiers first, accompanied by tracker links:

```text
landed in commit f230cb07db6 (terakan_buffer.c::terakan_CreateBuffer size-zero guard); PR #34 / branch fix/w9-buffer-size-zero-guard for cross-link
see filed-finding 2026-05-15-induced-lockup-recovery-test-results.md (PR #41 if still open)
```

Structure agent-loaded Markdown documents with clean, predictable formatting:
- Use exactly one top-level `#` title per document.
- Limit heading hierarchy to `###` depth.
- Include structured YAML frontmatter on programmatically processed files.
- Declare language tags on all code fences.
- Formulate rules as direct, positive declarative statements with exact cross-references.
- Construct tables when columns provide independent comparative data, using structured bullet lists for simple lookup tables, ownership mappings, and rule catalogs.
- Author rule documents in present-tense declarative prose with explicit cross-references so that isolated text extracts remain fully self-contained.

Maintain clean, ASCII-compatible text across all checked-in documentation:
- Keep documentation free of emoji.
- Use standard typography: choose straight quotes over curly quotes, `--` over em dashes, and `...` over ellipsis glyphs.
- Enforce closed double-hyphen delimiters: join `--` tightly to adjacent words (`word--word`), keeping punctuation fully closed.
- Retain meaningful scientific and mathematical symbols: mathematical operators, Greek letters in ISA and equation text, arrows in state transition maps, box-drawing characters in descriptor and packet diagrams, degree symbols, and micro signs.
- Preserve author and copyright names in their original spellings, including accented characters and Unicode copyright symbols in upstream attribution lines (`Copyright (c) 2024 Vitaliy Triang3l Kuzmin`).

### Comment-hygiene linter and Git hook

The comment-hygiene linter lives in this repository at `build-infra/scripts/lint/comment_hygiene_lint.py`, mirrored from the steinmarder lint tree, so a Mesa checkout enforces the rule without a sibling `steinmarder-r300/` tree. The pre-commit hook `build-infra/scripts/lint/pre-commit-comment-hygiene` runs `--staged --strict` and refuses the commit when the linter is missing or not executable; install it from the repository root:

```bash
ln -sf ../../build-infra/scripts/lint/pre-commit-comment-hygiene .git/hooks/pre-commit
```

`make -C build-infra comment-hygiene-check` is the repository-quality gate inside `make -C build-infra audit`: it fails when the linter or hook is absent, runs the linter's `--self-test`, and lints every file changed against `COMMENT_HYGIENE_BASE` (default `origin/main`) plus the staged set. `comment-hygiene-check-test` calibrates the hook's refusal on an absent and on a non-executable linter. A commit whose validation record says the lint was `not run` keeps that record; the gate applies from the commit that carries it onward.

Apply comment-hygiene rules across all newly authored commits regardless of automated linter presence. Preserve breadcrumb comments in existing in-flight PRs to avoid unnecessary force-pushes, enforcing hygiene strictly across all new commits.

## Evidence boundary

Maintain reverse-engineering evidence bundles, probe captures, and finding records within `steinmarder-r300/`, housing driver source code, build machinery, and upstream-facing tests within `mesa-26-gororoba/`. Consult sibling evidence in `steinmarder-r300/` prior to proposing driver modifications across Terakan, r300g, r600g, r3v, shared NIR/compiler passes, and build infrastructure.

Consult primary evidence references across designated repository lanes:

Shared workspace references:
- `steinmarder-r300/GPU_ARCHITECTURE_BASELINES.md`
- `steinmarder-r300/AGENTS_README.md`
- `steinmarder-r300/AGENT_RULES.md`
- `steinmarder-r300/docs/workspace/host-setup.md`
- `steinmarder-r300/docs/workspace/ccache-sccache-wiring.md`
- `steinmarder-r300/docs/workspace/sccache-multi-emit-patch.md`
- `steinmarder-r300/docs/workspace/mesa-fork-synthesis.md`
- `steinmarder-r300/docs/workspace/mesa-fork-upstream-divergence.md`
- `steinmarder-r300/docs/workspace/hostname-policy.md`

r600 and Terakan references:
- `steinmarder-r300/src/re/r600/`
- `steinmarder-r300/src/re/r600/findings/CLAIMS.md`
- `steinmarder-r300/src/re/r600/findings/active/`
- `steinmarder-r300/src/re/r600/results/`
- `steinmarder-r300/src/re/r600/docs/rca/`

r300 and r3v references:
- `steinmarder-r300/src/re/r300/`
- `steinmarder-r300/src/re/r300/findings/CLAIMS.md`
- `steinmarder-r300/src/re/r300/findings/active/`
- `steinmarder-r300/src/re/r300/results/`
- `steinmarder-r300/src/re/r300/docs/rca/`

Cite exact path spellings on disk (`src/re/r300/`). Reference evidence bundles by their durable directory paths, finding filenames, public specification titles, hardware identifiers, or commit SHAs.

Prioritize public specifications, driver symbols, DRM/Khronos naming, freedesktop.org GitLab issue URLs, and physical hardware identifiers in Mesa source comments. Confine fork-specific issue trackers and internal bundle paths to findings and commit messages unless required to make the driver logic intelligible.

## Tooling for RCA and audits

Deploy the most rigorous, specialized tool matching the technical claim: prioritize structural, indexed, flow-sensitive, binary, and empirical inspection tools over basic lexical searches.

### Tool availability

A missing local tool represents an environment constraint rather than code correctness. Determine the package or installation route, update module prerequisites, and execute the validation command. When a tool cannot be installed, record `not run` along with the specific technical blocker.

Document tool requirement updates with complete metadata:
- Name the package or executable command.
- State the technical rationale for requiring the tool.
- Identify the upstream repository, package manager, or build source.
- Specify required versions and minimum version floors.
- Provide the verification command (e.g. `<tool> --version`).
- Identify all dependent modules, audit passes, and scripts.

### Tool classes

Deploy tools systematically across their domain specializations:

Source navigation and symbol reachability:
- `clangd`, LSP, `read-tags`, `ctags`, GNU Global/`gtags`, `cscope`, `rg`, `ripgrep`, `git grep`, `git-grep`, `fd`, `git log -S`, `git log -G`, `git blame`.

Structural analysis and AST transformation:
- `ast-grep`, Semgrep, Coccinelle/`spatch`, `weggli`, `comby`, Tree-sitter CLI.

Static analysis, complexity, and linting:
- Compiler diagnostics, warnings-as-errors (`-Werror`), `clang-tidy`, `scan-build`, `cppcheck`, `sparse`, `smatch`, Infer, CodeQL, `lizard`, `scc`, `cflow`.

Mesa, shader, and generated-state validation:
- Mesa build logs, CTS/Piglit/deqp test runners, NIR instruction dumps, shader disassembly tools, packet decoders, generator validation scripts, known-good and known-bad test baselines.

Binary inspection, reverse engineering, and symbolization:
- `gdb`, `addr2line`, `objdump`, `nm`, `readelf`, LIEF, `binwalk`, radare2/`r2`/`radiff2`, Rizin, Ghidra.

Tracing, profiling, and runtime introspection:
- `strace`, `ltrace`, `perfetto`, `trace-cmd`, LTTng, `lttng`, `lttng-tools-generic-kernel`, SystemTap/`stap`, `bpftrace`, `bcc-tools`, Sysprof, Valgrind, Heaptrack, Hotspot, `python-ptrace`, Frida, `frida-tools`, `python-frida`, `python-frida-tools`.

Fuzzing and generational mutation:
- honggfuzz, AFL++, Radamsa.

C unit and integration testing:
- Check, shellcheck, project test harnesses, calibrated hardware probes.

Probe and harness storage: house calibrated probes, standalone test harnesses, and experimental artifacts in the `steinmarder-r300` tree (`../steinmarder-r300`). Maintain driver code, build infrastructure, and committed regression tests within `mesa-26-gororoba/`, transferring durable probes and evidence bundles to `steinmarder-r300/src/re/r300`.

Environment execution notes (CachyOS/Arch): install `spatch` from `coccinelle-bin`; use `opensnoop` and `execsnoop` from BCC; run `ast-grep`, `lizard`, and `weggli` directly from `PATH`. Note that MSAN is incompatible with Mesa due to uninstrumented dependency closures (libc++, LLVM, libdrm); deploy ASan+UBSan for memory errors and Valgrind/memcheck or DRD for uninitialized reads and thread races.

Code property graphs and deep analysis:
- Joern, CodeQL databases, Ghidra projects, radare2/Rizin workspaces.

### Tool tiers

Require Tier S tools whenever available and relevant:
- `clangd` or LSP for definitions, references, call hierarchies, and symbol reachability.
- GNU Global, `cscope`, or indexed tags for large C trees.
- `ast-grep` or Semgrep for structural patterns and class-wide audits.
- `rg`, `git grep`, and `fd` for text, comments, string literals, and path searches.
- `git log -S`, `git log -G`, and `git blame` for historical evolution.

Deploy Tier A tools for flow-sensitive and build-sensitive validation:
- Compiler diagnostics, warnings-as-errors, and static analyzers.
- Coccinelle for multi-file semantic consistency.
- Mesa, kernel, CTS, Piglit, and deqp runner output.
- Shader disassemblies, packet traces, and NIR dumps.
- Generator verification diffs.

Deploy Tier C tools for empirical validation:
- CTS/Piglit/deqp test suites, `dmesg` logs, hardware performance counters, retained evidence bundles, perf/ftrace/bpftrace captures, and minimal reproducer harnesses under explicit safety gates.

Record the discovery mechanism alongside every symbol or path claim in audit reports: cite `(clangd: references on FUNC)`, `(global -r SYMBOL)`, `(ast-grep --pattern PATTERN)`, `(rg --fixed-strings SYMBOL src/)`.

Pre-deployment verification: calibrate every newly authored probe, linter rule, or verdict script against verified positive and negative baselines prior to deployment.

When invoking Coccinelle, locate `spatch` via `command -v spatch`, record the package provenance, and cite the exact semantic patch file applied.

### Agent coordination

Coordinate at most three concurrent subagents. Scope subagents as read-only evidence collectors unless explicitly granted implementation authority: provide each subagent with a bounded task, well-defined input scope, target output schema, and mandatory citation criteria. The parent agent retains sole authority over synthesis, conflict resolution, implementation decisions, and final claims.

Deploy compact or local models for search, file location, text summarization, and citation fan-out. Escalate to high-capacity models for deep architectural synthesis, hazardous hardware operations, irreversible changes, or complex cross-file invariant modeling, recording the technical rationale for escalation in the work log.

Keep all foundational engineering choices--code modifications, commit pushes, file deletions, build configuration changes, and warning treatments--firmly with the parent agent.

### Retained tools and probes

Develop retained analysis tools, probes, linters, and verdict scripts as production-grade software within the repository's native build and validation pipelines.

Equip every retained tool with:
- explicit, documented input and output interfaces;
- deterministic, reproducible execution;
- known-good and known-bad calibration test suites;
- documented runtime and toolchain dependencies;
- fail-closed safety gates for hazardous hardware operations;
- a clear validation command;
- maintainer-readable diagnostic output on failure.

Limit temporary single-case scripts strictly to explicitly documented, transient reproducer branches.

## Validation expectations

Scale validation directly to the changed subsystem surface:

- Terakan/R3V driver paths: execute targeted builds paired with active runtime suites or CTS tests.
- r300/r600 Gallium paths: execute targeted builds paired with Piglit, CTS, deqp, or documented hardware safety gates.
- NIR, lowering, and compiler paths: execute compilation builds paired with compiler unit tests, shader disassembly checks, and affected API conformance suites.
- Meson and build infrastructure: execute clean setup and reconfigure cycles paired with Ninja targets and explicit artifact installation audits.
- Generated files: run the authoritative generator tool directly and verify the resulting diff; record the exact operational reason when generator toolchains are unavailable.
- Source comments and documentation: audit comment hygiene, verify Markdown structural conformance, and validate source symbol references.
- Automation scripts: run shellcheck on shell assets and execute positive and negative calibration paths.

Ground pass verdicts exclusively in executed runs, designating unexecuted suites as `not run` accompanied by their specific operational blockers. Specify the required safety gate for any test blocked by hardware policy. Treat CTS, Piglit, or deqp movements that deviate from recorded predictions as new empirical findings.

Bind qualification, conformance-baseline, and merge-gating build and test verdicts to a single immutable committed state. Set `REPRODUCIBLE_RUN=1` on every build-infra invocation that contributes to those verdicts. This mode rejects the control checkout as `TOPSRC`, requires distinct detached control and source worktrees, verifies physical tracked bytes, staged and untracked state, and ignored populated `subprojects/` in both worktrees, and records the mode in the build identity. Configure compiles an archive-derived source view pinned to the declared source commit; later build, test, install, and artifact checks must retain `REPRODUCIBLE_RUN=1` or fail identity verification. Prior to every reproducible run, confirm that `HEAD` in both worktrees matches the declared control and source SHAs. Treat the detached worktrees and `BUILD_ROOT` as single-owner qualification resources: other worktree paths cannot change their inputs, and source-worktree edits after archive selection cannot enter the compiled source view. A writer with access to either detached worktree or `BUILD_ROOT` violates the qualification boundary and invalidates the verdict.

## Security and hardware stop-line

Halt standard feature development upon discovering critical security defects or unsafe hardware-access paths. Isolate the execution path, engage fail-closed containment, and report findings immediately when encountering:

- exposed secrets, access tokens, credentials, or private request bodies;
- SQL injection vectors or unescaped query assembly;
- command injection via shell wrappers, generated scripts, hooks, or test harnesses;
- path traversal vectors or unchecked filesystem writes;
- sensitive data leaks into logs, manifests, evidence bundles, test captures, or build artifacts;
- missing authentication or authorization checks across sensitive boundaries;
- insecure deserialization vulnerabilities or server-side request forgery (SSRF);
- un-gated MMIO, BAR, `/dev/mem`, raw command submission, ASIC reset, or privileged debugfs access outside explicit lane gates.

Enforce allow-lists, path normalization, and input containment checks across all untrusted inputs prior to shell execution or path resolution. Pass vetted inputs exclusively to `sh -c`, `bash -c`, `eval`, generated shell fragments, and path concatenation routines. Unlock hazardous execution paths strictly through exact opt-in environment gates. Redact credentials, secrets, and raw tokens from all log streams and artifacts.

## Synthesis over selection

Preserve all non-refuted content when merging parallel branches or reconciling review findings. Derive merge resolutions from verified mechanisms and empirical evidence, independent of branch age, phase identifiers, or author attribution.

Resolve overlapping contributions through additive union and deep synthesis. Permit selective omission exclusively when the discarded content is empirically refuted by Tier 1 through Tier 3 evidence or superseded by a verified line-level diff accompanied by recorded technical rationale.

Execute merges through structured synthesis actions:

- Analyze: audit the distinct technical contribution of each branch before editing, treating every differing line as a candidate for preservation.
- Reconcile: preserve all non-refuted invariants, requiring empirical refutation or proven supersession before omitting any claim.
- Resolve: drive merge operations to completion; record outstanding action items explicitly in the work record when a session boundary intervenes.
- Expand: articulate the unifying mechanisms connecting findings, execution paths, and test suites, weaving parallel discoveries into a cohesive architecture.
- Harmonize: unify terminology across the synthesized artifact, establishing single durable mechanism identifiers for shared concepts.
- Infuse: equip the synthesized artifact with the rules, static assertions, linters, or regression tests that eliminate the underlying failure class.

Ensure every synthesis adds tangible architectural value: deliver a unified system model, terminology cross-reference, strengthened operational rule, validation matrix, sharpened evidence classification, retained test harness, or streamlined upstream integration path.

Audit the staged diff adversarially following every merge resolution:

```bash
git diff --staged
```

Verify that all non-refuted content from each source branch remains fully represented. Restore any omitted content, record an explicit citation-backed refutation, or document it as a tracked follow-up item.

Avoid selective shortcuts that discard parallel engineering:

```bash
git merge -X theirs branch/feature-a
git checkout --theirs file
sed -i '/^<<<<<<< /d; /^=======$/d; /^>>>>>>> /d' file
```

Synthesize parallel contributions through intentional cherry-picks:

```bash
git cherry-pick --no-commit <sha>
git diff --staged
git commit
```

When superseding a pull request, cherry-pick the relevant commit SHA with `--no-commit`, audit individual hunks, record the cross-repository link in the synthesis commit body, and retire the superseded pull request.

Validate additive Markdown synthesis by evaluating line-level differences across inputs and outputs:

```bash
comm -23 <(sort -u source_a.txt) <(sort -u merged.txt) > only_in_a.txt
comm -23 <(sort -u source_b.txt) <(sort -u merged.txt) > only_in_b.txt
```

Verify that `only_in_a.txt` and `only_in_b.txt` remain empty, or confirm that any omitted lines carry explicit refutation records or tracking entries. Review the final narrative flow by hand to ensure cohesive structural progression.

Consult `steinmarder-r300/AGENTS_README.md` ("Synthesis Doctrine") and `steinmarder-r300/AGENT_RULES.md` ("Rule: Synthesis Over Selection") for lane-specific merge gates.

## Regression-on-fix discipline

Preserve all existing valid behaviors when implementing targeted defect repairs. Following modifications to scripts, runners, build configurations, lowering passes, descriptor pipelines, or comments:

- read `git diff --staged` adversarially;
- verify that every removed line represents an intentional correction, consolidated duplicate, or refuted hypothesis;
- align test labels with their actual executed commands;
- verify every symbol named in comments or documentation against active source definitions;
- enumerate all available override mechanisms prior to documenting any single knob;
- verify optional tool availability prior to configuring execution pipelines;
- regression calibration: re-calibrate verdict runners, linters, and probes against known-good and known-bad inputs to confirm defect isolation and prevent false verdicts.

When reviewing a reported defect, eliminate the entire failure class across the codebase by adding the invariant check, linter rule, regression test, or documented verification step that prevents recurrence.

## Strict clean and deletion readiness

Consult `docs/strict-clean-definition.md` before executing repository cleanup, branch pruning, or tree retirement. Treat that checklist as the authoritative standard for deletion readiness.

## Key subsystems

Navigate Gallium drivers and shared compiler components via standard Mesa layout conventions under `src/gallium/` and `src/compiler/`. Access the two fork-specific out-of-Gallium Vulkan drivers under `src/amd/`:
- Terakan: `src/amd/terascale/vulkan/`
- [[R3V: `src/amd/r300/vulkan/`]]

Manage build entry points across repository-root configuration files (`meson.build`, `meson.options`, `meson_options.txt`), `build-infra/`, native configuration files, and install scripts.

Select the Rust toolchain through Meson configuration and environment policy in accordance with upstream `rust-toolchain.toml` (`channel = "nightly"`).

House repository-wide doctrine in this root document; place subsystem-specific rules in lane READMEs or path-scoped documentation, linking them back to this guide.

## Engineering foundations

The project motto is:

`AD ASTRA PER MATHEMATICA ET SCIENTIAM ET TECHNICUM`

Direct all repository engineering through mathematics, scientific inquiry, and disciplined technical rigor. Welcome bold creative insight, and ground every hypothesis in concrete mechanisms, empirical evidence, robust implementations, and verifiable test outcomes.

Practice disciplined imagination: formulate bold hypotheses, and test them against source code, specifications, silicon behavior, build diagnostics, test suites, and adversarial review. Express ideas through functional code, rigorous documentation, calibrated probes, validation datasets, and clear architecture models.

Elevate every touched component so that final artifacts stand more accurate, reproducible, navigable, testable, and source-grounded than their starting state.

### Engineering posture

Investigate physical and logical mechanics before asserting conclusions. Trace behavior to primary sources: source code, specifications, test oracles, execution logs, generated files, kernel paths, command streams, retained evidence, or silicon measurements.

Treat compiler warnings, test deviations, and anomalous tool outputs as actionable defects until fully explained. Configure builds and validation suites to expose the complete failure surface, surfacing underlying faults early.

Build all artifacts to reproduce cleanly on bare hosts via PATH-resolved tools, tracked regular files, documented dependencies, and public environment definitions.

Deliver complete, production-grade solutions. Record the underlying mechanism, dependencies, validation path, and remaining uncertainty for every change.

Treat unexpected behavior and anomalies as valuable empirical findings: preserve the observation, identify the mechanism, and refine the system model accordingly.

### First-principles workflow

Derive implementations directly from foundational requirements:

1. Define operational scope.
2. Identify and state assumptions.
3. Derive governing constraints.
4. Inspect primary authority sources.
5. Decompose claims into falsifiable units.
6. Construct a precise mechanism model.
7. Test the cheapest decisive hypothesis first.
8. Implement the smallest complete mechanism.
9. Validate against known-good, known-bad, and target test cases.
10. Record code changes, executed tests, unrun suites, and residual uncertainty.

Treat external commentary and community archaeology as supporting research leads, anchoring authoritative claims in public specifications, verified source code, execution suites, hardware measurements, and retained evidence bundles.

### System model

Express subsystem designs and modifications through their complete operational topology:

- call paths and execution flows;
- data flow and transformations;
- control flow and state branching;
- state ownership and lifetime boundaries;
- ABI and UAPI interfaces;
- hardware register paths and bitfield mappings;
- command stream grammar and packet sequences;
- descriptor layouts and resource lifetimes;
- synchronization barriers and memory fences;
- cache hierarchies and coherency domains;
- error handling and unwinding paths;
- generated artifacts and generator tools;
- build dependency graphs;
- test oracles and verification matrices;
- known silicon quirks and constraints.

Model each subsystem as a living mechanism: state which data moves, identify who owns it, declare the governing invariant, and define how failure becomes observable.

### Synthesis requirement

Unify overlapping content, reconcile divergent arguments through decisive evidence, collapse redundant prose while preserving depth, surface implicit parameters, and extract and validate underlying models. While `Synthesis over selection` governs the structural mechanics of branch and finding integration, this requirement governs the conceptual and theoretical elevation of the system model.

Elevate the architecture through every synthesis by delivering:

- a stronger, more predictive mechanism model;
- an unambiguous terminology map;
- authoritative cross-references;
- a comprehensive validation matrix;
- a refined evidence classification;
- a source-grounded operational invariant;
- a reusable, calibrated probe harness;
- a semantics-preserving architectural refactor;
- a streamlined upstream integration path.

Transform the whole system to be far more intelligible, resilient, and capable than the sum of its individual parts. Consult `Synthesis over selection` for the governing branch integration and diff-audit workflow.

### Creative rigor

Deploy creativity to discover hidden invariants, expose underlying symmetries, and strengthen verification. Channel creative exploration and elucidations into high-leverage outcomes:

- hardware-grounded system formulations;
- command-stream decompositions;
- probe-backed empirical discoveries;
- robust validation harnesses;
- refined testing methodologies;
- algorithmic optimizations;
- semantics-preserving structural refactors;
- specification conformance improvements;
- maintainable codebase cleanups;
- clear, enduring architecture models.

Anchor confidence in verified empirical evidence, deploying clever and novel approaches to enhance prediction accuracy, validation rigor, implementation simplicity, and long-term maintainability.

Spark the mind; verify the result; generate novel insights and engineer with unyielding discipline. 
