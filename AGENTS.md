# mesa-26-gororoba Agent and Developer Reference

## Instruction source

`AGENTS.md` is the root instruction file for `mesa-26-gororoba`.

All Mesa agents and contributors follow `AGENTS.md` for repository rules. Other root agent files may exist only to load `AGENTS.md` for tools that require a tool-specific filename.

`CLAUDE.md` loads `@AGENTS.md`. Claude-specific loading notes may follow the load line. `GEMINI.md` follows the same rule when Gemini CLI is part of the workflow. Do not keep `GEMINI.md` only to match other agent filenames.

Root agent files MUST be regular tracked files. Do not use symlinks. Do not copy the `AGENTS.md` body into `CLAUDE.md`, `GEMINI.md`, or any other loader. Copied text will drift and create conflicting instructions.

Codex and compatible agents read `AGENTS.md` directly. Human contributors read `AGENTS.md` directly.

## Hard rules

The following rules are enforceable. Later sections may explain mechanism or rationale, but no later section may weaken or contradict these rules.

### Boundary, paths, and instruction files

- MUST keep Mesa normal flows independent of `steinmarder`: build, install, test, source-comment, upstream-sync, and submission. `steinmarder` may supply evidence only; its bundles and findings stay out of Mesa, and Mesa driver changes stay out of `steinmarder`.
- MUST use repository-relative paths, generated native files, PATH-resolved tools, or explicit user roots. Discover the repository root with `repo_root=$(git rev-parse --show-toplevel)`.
- MUST NOT put local absolute paths, private host FQDNs, per-user toolchains, raw IP literals, or worktree names in checked-in files.
- MUST keep instruction files as real tracked files: no symlinks, copied bodies, or divergent loader instructions.

### Root cause, evidence, and conformance

- MUST identify the exact driver path, chip, test, spec rule, and kernel or Mesa mechanism before changing behavior.
- MUST prefer PCI IDs, ISA/register sources, and measurements over PALM/Wrestler family nicknames.
- MUST establish root cause from primary sources before opinion: ISA section, kernel function, spec paragraph, and test oracle.
- MUST mark known, hypothesized, and speculative claims distinctly in findings and code comments.
- MUST cite the symbol-discovery method, not only `file:line`: `(clangd: textDocument/references on FUNC)`, `(global -r SYMBOL)`, `(ast-grep --pattern PATTERN)`, or `(rg --fixed-strings SYMBOL src/)`.
- MUST record the observation, source or spec constraint, implementation hypothesis, falsifier, validation command or retained bundle path, and expected CTS, Piglit, or deqp movement before any hardware-RCA or conformance fix.
- MUST check `dmesg` for DRM CS validation errors before GPU-behavior analysis.
- MUST verify module reachability with `/proc/PID/maps` or `gdb info sharedlibrary` before symbolizing a crash address.
- MUST keep evidence classes separate: build, runtime, conformance, and silicon.
- MUST NOT claim a CTS, Piglit, or deqp fix from build-only evidence.
- MUST NOT revise predictions after observation, silently rerun tests until output matches a hypothesis, or propose a workaround without naming its spec-conformance cost.

### Builds, tests, and verdicts

- MUST treat warnings and unexpected tool, CTS, Piglit, or deqp output as defects until explained.
- MUST report unexpected results immediately.
- MUST build touched code cleanly under configured warning flags and add no new warnings.
- MUST record what was built, tested, skipped, blocked, or unavailable.
- MUST say `not run` and give the reason when a test was not run.
- MUST calibrate every new probe, lint, or verdict-producing script against known-good and known-bad inputs before trusting its verdict.
- MUST NOT remove build targets, tests, or validation checks as collateral for a narrow fix.
- MUST NOT change generated files without running the generator or documenting why it is unavailable.

### Languages and scripts

- MUST keep each translation unit in its existing language and configured standard.
- MUST keep C translation units at C11 or newer, never pre-C11.
- MUST keep C++ backends at C++11 or newer, never below the configured standard.
- MUST treat C11 and C++11 as floors, not ceilings.
- MUST NOT mix C and C++ in one translation unit.
- MUST write Python tooling for CPython 3.12 through 3.14 inclusive. Avoid APIs deprecated for removal after 3.14 where practical.
- MUST write shell scripts as POSIX `sh` unless a script explicitly requires and declares `bash`.

### Build orchestration

- MUST preserve Meson plus Make. Meson owns configuration and Ninja generation.
  Make and build-infra own host selection, audit checks, generated native
  overlays, clean, build, and install.
- MUST model Meson defaults in build audits. When an option is omitted or set to `auto`, audit the dependencies Meson will enable on the target host.
- MUST NOT treat absent or `auto` Meson options as disabled.
- MUST require exact opt-in values for hazard gates, such as `R300_TRACE_HAZARD_ACCEPTED=1`.
- MUST reject unset, empty, and zero-valued hazard gates.
- MUST NOT use `getenv()` presence as hazardous-path consent.
- MUST use Meson `[binaries]` plus `CCACHE_PREFIX=distcc` for distcc/ccache integration.
- MUST NOT chain `ccache distcc compiler` through a shell wrapper.
- MUST NOT revive a C or C++ distcc-pump lane that puts `ccache` or
  `sccache` before distcc-pump.
- MUST NOT assume `RUSTC_WRAPPER` affects Meson Rust.
- MUST NOT hardcode `~/.rustup/toolchains/.../bin/rustc` in a Meson native file.
- MUST NOT add standalone build helper scripts for compiler selection, audit policy, clean, build, install, or hazard consent.

### Git, merge, and submission

- MUST use durable mechanism names in branches, commit subjects, PR titles, source comments, finding filenames, and bundle directories.
- MUST set the branch name, first commit subject, and PR title before first push.
- MUST NOT use wave, phase, mission, session, PR, reviewer, agent, or worktree labels as load-bearing identity.
- MUST preserve all non-refuted content during merges. Default additive resolution is union plus synthesis.
- MUST NOT use `git merge -X theirs`, `git checkout --theirs`, blanket conflict-marker stripping such as `sed -i '/^<<</d; ...'`, or unreviewed deletion as synthesis.
- MUST force-push `main` or shared branches only with explicit user sign-off and a commit message explaining why.
- MUST skip pre-commit hooks only in emergencies and only with the reason in the commit message.
- MUST NOT push `upstream/main` directly to fork `main` with `git push origin upstream/main:main`; integrate through an intentional rebase.
- MUST NOT submit Mesa patches through an autonomous tool. The submitter must understand and own the change.

### AI disclosure, authorship, and copyright

- MUST disclose AI involvement with the Mesa-required `Assisted-by:` trailer, or `Generated-by:` when AI generated almost the entire change, when policy requires disclosure.
- MUST use `Co-authored-by:` only for human co-authors.
- MUST NOT add `(LLM-assisted)`, `Generated by Claude`, or any AI tag to source headers.
- MUST NOT force-push to scrub historical pre-policy `Co-Authored-By: Claude` trailers.
- MUST preserve upstream copyright headers verbatim, including author name and year.
- MUST NOT fabricate personal-name copyright lines such as `Copyright (c) YYYY Eirikr Hinngart` or `Copyright (c) YYYY <git config user.name>`.
- MUST NOT keep LLM-default copyright names.
- MUST NOT use `Copyright (c) YYYY steinmarder project`.
- MUST NOT strip upstream copyrights, rewrite them during file movement, or add a second project-collective line above them.

### Comments, prose, and safety

- MUST write source comments so a Mesa maintainer six months later can understand them without this project's task tracker.
- MUST NOT cite internal GitHub or GitLab fork issue numbers, private PR chronology, wave labels, task numbers, author tags, local paths, private hosts, or deictic time in source comments.
- MUST use American (United States) English spelling in new or modified source comments, commit messages, and documentation authored by this project's contributors.
- MUST NOT mass-reformat code, churn upstream comment spelling when the patch is about behavior, produce stubs, placeholders, dead code, or `TODO: finish later` prose without explicit tracked rationale and user agreement.
- MUST report results and decisions directly: state what a thing is and does, drop contrast framing (`X, not Y`) and side commentary, and add no narration of internal deliberation.
- MUST stop normal feature work for a critical security or unsafe hardware-access defect, then contain and report before resuming.
- MUST NOT run destructive commands such as `sudo rm -rf` on shared workspace paths.

## Workspace roots

The parent workspace has two durable source roots.

- `mesa-26-gororoba/` owns Mesa source: Terakan, r300g, r600g, NIR/compiler code, and Meson/build/install infrastructure.
- `steinmarder/` owns reverse-engineering runners, retained evidence, findings, manifests, host kits, safety policy, and cross-repo orchestration.

`mesa-26-gororoba-*` directories are temporary Git worktrees of `mesa-26-gororoba/`, not separate projects. Worktree changes land through a branch, review, and merge to `main`. Remove a temporary worktree after its branch is merged or superseded.

Keep repo contents separated. Do not store steinmarder evidence bundles or findings in Mesa. Do not store Mesa driver changes in `steinmarder/`. A Mesa fix may use steinmarder evidence, but the code lands in Mesa. Cross-repo work uses sibling checkouts and PRs, not file moves.

## Project scope and priorities

`mesa-26-gororoba` is a Mesa 26.x fork that tracks upstream `main`. Local work includes Terakan Vulkan under `src/amd/terascale/vulkan/`, r600 SFN work, and related r300, r600, NIR, compiler, build, and install changes for Radeon HD 6310 PALM/Wrestler (`CHIP_PALM`, Evergreen, TeraScale-2, VLIW5). The primary host class is x130e / Bobcat / HD 6310 APU. The peer repository is `steinmarder/`.

Priority order is fixed: conformance, standards, stability, performance. Safety applies throughout. Earlier priorities override later priorities.

A fast workaround is not a fix when it breaks conformance. A non-conforming workaround may be considered only when its conformance cost, containment, and removal path are recorded.

Investigate before editing. Read source, specs, tests, logs, generated files, kernel paths, CTS/Piglit/deqp behavior, commit history, and retained evidence. Work in this order: scope the task, identify the component, split claims, collect primary evidence, model the mechanism, design the change, implement, verify, and record the result.

Leave code, comments, tests, and findings more accurate, reproducible, navigable, and source-grounded than the starting state.

## Agent operating rules

Language-model agents working in Mesa MUST apply these rules before editing Terakan, r300, r600, NIR, Meson, tests, or comments. Mesa instructions must stand alone for Mesa build, install, and review. `steinmarder/` may provide evidence and runner context, but it does not replace Mesa rules.

### Operating stance

Work at hardware/software reverse-engineering depth: exact chips, exact paths, exact specs, exact tests, exact evidence. Do not trade conformance for speed or patch size without recording the cost.

Use the priority order from `Project scope and priorities` as a hard constraint. Label any non-conforming workaround with its conformance cost, containment, and removal path.

Treat warnings as defects. Do not use hardcoded shortcuts, symlinks, or local FQDNs. Keep documentation precise enough for later maintainers to verify.

### Work sequence

Work inside the real repository. Read code, docs, logs, tests, history, generated files, and prior evidence before reaching a conclusion. Treat memory and generated summaries as leads, not authority.

Use an ordered task tree: research before editing, model before designing, verify before claiming completion. Each tool invocation must answer a named question or move implementation forward.

Research from primary sources when possible: Mesa source, Linux kernel source, Khronos Vulkan/OpenGL specs, AMD ISA/register manuals, CTS/Piglit/deqp tests, and retained steinmarder evidence.

Implement complete, robust changes. Do not add stubs, placeholders, dead code, or `TODO later` prose without explicit tracked rationale. When blocked, trace the root cause through the interacting layers instead of choosing a shortcut.

### Evidence rank

When sources conflict, higher rank controls.

1. Silicon evidence: probe output, register readback, CTS on hardware.
2. ISA, register, hardware, and API specs: AMD Evergreen ISA, Bobcat BKDG, Vulkan/OpenGL specs.
3. Kernel source: radeon DRM, `evergreen_cs.c`, kernel commit log.
4. Driver source: Terakan Vulkan, r600g Gallium, r300g Gallium, R300VK.
5. CTS/Piglit/deqp behavior, only when the test oracle is clearly spec-grounded.
6. Documentation and comments, only when consistent with ranks 1 through 5.

Implementation-affecting architecture claims require a rank 1 through 4 source by name. Claims without that backing are hypotheses. If a comment conflicts with a higher-ranked source, cite the higher-ranked source and remove or annotate the comment.

### Falsification record

Before code changes for hardware RCA, record:

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

Report the evidence chain, alternatives, tradeoffs, and next evidence needed. Treat surprise as a finding; do not silently pivot.

### Reasoning checks

Use these checks during RCA, conformance, and hardware-facing changes.

- Silicon-to-test chain: trace data and control flow from hardware register, bitfield, field definition, driver API, and command emission to the CTS/Piglit/deqp assertion before diagnosing.
- Hypothesis tree: list plausible root causes, rank them by evidence cost and likelihood, test the cheapest decisive case first, and prune only on falsification.
- Opposition review: after forming a synthesis, argue the strongest contrary case. If the contrary case survives, keep the finding unresolved and name the next evidence needed.
- Claim audit: before committing a finding, list each implementation claim and the rank-1 through rank-6 source that backs it.

## Source comments and driver style

The code is the primary text. Comments explain mechanisms that are not obvious from the next line of code. A useful comment records a silicon constraint, spec rule, kernel validation rule, command-stream invariant, measured quirk, ABI boundary, synchronization rule, or the reason a workaround preserves conformance.

Comment shape: mechanism first, authority second, ordered constraint third, testable consequence last. Remove ceremonial prose. Add the missing invariant when terse prose hides the mechanism. Move phase names, sessions, PR chronology, reviewers, agents, local paths, and private artifacts to commit messages or findings, not source comments. Mark unfalsifiable claims as conjecture or remove them. Prefer third-person present tense (`the kernel reads WORD0`, `the TX unit ignores NEAREST for integer formats`) over first person. Impersonal `we` for the code path (`we do not have to wait at the end of an IB`) matches Mesa upstream and is fine; deictic `we`/`our` for the project or team (`our driver`, `our approach`) is not. For a silicon bug or workaround, name the affected chip or register, state the observable failure in one sentence, cite a bug URL or ISA section when public, and mark empirical-only knowledge with `seems to`/`appears to` versus the plain indicative for known silicon behavior.

State what a thing is and does, in positive declarative form. Name the mechanism and let the binding constraint stand as fact: `the vbuf stage maps the vertex buffer after every allocate_vertices, so the draw is dropped at submission`. The comment carries the mechanism and the constraint that makes it hold; correctness follows and needs no assertion. Drop contrast framing (`X, not Y`) and correctness claims (`this is correct`, `is required`). The reason an alternative was rejected belongs in the commit message. `correct-or-reject` and similar named postures stay as terms.

Leave each final artifact more accurate, reproducible, navigable, and source-grounded than its inputs.

### Driver code

Driver code should expose the domain directly: API object, chip generation, register field, command packet, memory lifetime, synchronization edge, format table, and error path. Names, structs, data tables, assertions, and cleanup labels carry most of the explanation. Comments carry only constraints that would otherwise be lost.

Use direct, concrete names. Public entry points carry the subsystem prefix. Local helpers use mechanism verbs such as `emit`, `lower`, `fill`, `find`, `validate`, `lock`, `unlock`, `init`, `finish`, and `destroy`. Constants, registers, packet fields, and enum translations keep domain names. State objects, key structs, and descriptor words are typed driver state, not loose parameter groups.

Use the shape required by the mechanism. Prefer a helper when the operation has a name. Prefer a data table when cases form a finite map. Do not split a hardware/API matrix only to reduce line count when the split hides the invariant. A long function is acceptable only when it remains locally auditable: feature bits, packet words, ioctl validation, resource lifetime, ordered emission, and unwind edges stay visible, and every exit path can be checked.

Treat error paths as ownership topology. Labels such as `fail`, `unsupported`, `err_*`, `out`, `put`, `free`, and retry labels show which object is live, which invariant failed, and which cleanup edge runs next. Follow the subsystem ABI: `VkResult` and `vk_error` in Vulkan paths; negative errno and WARN/assert fences in kernel-shaped paths. Assertions guard impossible internal states. External input is validated and rejected.

Use data tables for finite maps: formats, register fields, enum translations, chip gates, descriptor words, packet layouts, and workaround selectors. Preserve distinctions when cases differ materially. Abstraction is valid only while it preserves the chip, ABI, spec, and evidence boundary that made the case exist.

Uncertainty in a comment must name the mechanism and the guarded, disabled, or falsifiable path. It must not depend on review chronology, private context, or session state.

Patch quality is proof density: the smallest mechanism that preserves exact domains, visible state transitions, reviewable cleanup, and a falsifiable consequence. Commit messages and PR text follow the same order: component prefix, mechanism, invariant-based rationale, evidence by command or bundle, and tests stated plainly.

## Durable names

Use names from mechanism or content, not chronology, actors, work sessions, or review process. This applies to branch names, bundle directories, finding filenames, PR titles, commit subjects, source comments, and checked-in identifiers.

Forbidden load-bearing identity includes waves, phases, missions, agents, worktrees, sessions, reviewers, PR numbers, task numbers, and dates that do not describe content.

Examples:

- Branch: avoid `terakan/wave8a-fix` and `terakan/phase1e-atomic-fix`; use `terakan/palm_mem_rat_atomic_completion_semantics`.
- Commit subject: avoid `Wave 8A follow-ups` and `terakan: Phase 1E follow-ups`; use `terakan: validate PALM MEM_RAT atomic completion semantics`.
- PR title: avoid `Phase 1E discriminator`; use `palm_mem_rat_atomic_completion_semantics discriminator`.
- Comment: avoid `Wave 8A path` and `Phase 1E-atomic case`; use `cached MEM_RAT atomic path`.
- Finding filename: avoid `2026-05-18-phase1e-atomic.md`; use `2026-05-18-palm-mem-rat-atomic-completion-semantics.md`.
- Test or bundle artifact: avoid `phase1e-results`; use `palm_dual_mem_rat_atomic_silicon_cut`.

The first commit subject matters because a single-commit squash merge may reuse it even when the PR title was corrected later. Set branch name, first commit subject, and PR title before first push.

Phase, wave, and chronology terms may appear only as secondary registry metadata, such as `phase: 1E-atomic` in finding YAML. They must not be load-bearing and must not appear in source comments or primary artifact names.

A name describes what is inside: content, target, or mechanism. It does not describe the act of collecting, grouping, staging, or sequencing.

`tranche` is forbidden in branches, filenames, identifiers, and comments. It is aggregation jargon and does not name content.

`set`, `batch`, and `group` are forbidden only when they are ordinal containers, such as `set5`, `batch_2`, or `group3`. Descriptive domain compounds are allowed when they name content: `group_map`, `subgroup`, `workgroup`, `batch_size`, and a Vulkan descriptor `set`.

To derive a durable name: read the artifact, state in one line what it does or contains, isolate the mechanism and object, then name those. Examples:

- `builds the imageStore reference IB` -> `capture_reference_ib`
- `byte capture through one uprobe on libdrm drmIoctl` -> `DRMIOCTL_UPROBE_SCRIPT`
- `next-15 blocker frontier manifest` -> `BLOCKER_FRONTIER_MANIFEST`

Names such as `VARIANT_2C` and `phase2_kamikaze` encode chronology or staging, not content. Re-read the artifact and derive the name again.

## Cayley-Dickson audit ladder

Use Cayley-Dickson names as evidence-depth labels, not ornament. The ladder defines which evidence planes must be checked before a claim advances.

Driver bugs here can cross specification, lowering, command emission, kernel validation, cache and coherency behavior, and exact PALM/Wrestler silicon. One source file rarely contains the whole constraint. Each level adds an axis that can expose a composition failure.

- `R` / 1D / observation: exact file, symbol, packet, register, test, log line, or artifact. Example: `line 1214 calls r600_nir_lower_cube_to_2darray`.
- `C` / 2D / rule: specification, ABI, API contract, ISA text, test oracle, or commit contract. Example: `r600g calls int_tg4 after cube_to_2darray; Terakan does not`.
- `H` / 4D / mechanism: code path, kernel validation, lowering, descriptor, allocator, cache, runtime, or hardware constraint. Example: `Evergreen forces nearest filtering for integer GATHER4`, ISA section 9.4.
- `O` / 8D / provenance: probe, dmesg, counter, trace, disassembly, CTS/Piglit/deqp, benchmark, or retained bundle. Example: `missing pass plus silicon behavior gives wrong corners; all 12 cube cases fail`.
- `S16` / 16D / interaction lattice: build/runtime, generated/hand-written, host/target, silicon/family, source/finding, and code/evidence interactions.
- `T32` or `CD32` / 32D / integration envelope: upstreamability, ABI/install impact, safety, security, CI, rollback, maintenance, documentation, and future research path.

Use `T32` or `CD32` only for systemic decisions. Its audit axes are: observation, specification, code path, mechanism, hardware, kernel or ABI, test, provenance, version, configuration, generated state, resource lifetime, memory/cache, ordering, privilege, security, falsifier, performance, portability, upstream surface, maintenance, deployment, rollback, CI, documentation, operator impact, evidence independence, contradiction, alternative model, validation matrix, final synthesis, residual uncertainty, and future constraint.

Rules:

- MUST NOT jump from 1D observation to 8D evidence. The 2D and 4D levels eliminate cheap wrong hypotheses.
- An 8D finding MUST include claim, evidence chain, and falsification criterion.
- MUST surface 16D or 32D analysis only after lower levels are stable, or after naming the remaining uncertainty.
- MUST NOT publish a 16D synthesis from separately passing facts. Test the interactions.
- MUST NOT make a 32D architecture decision until lower levels are stable or the unresolved uncertainty is explicit.
- Direction matters: `code -> hardware` is not `hardware -> code`. Read the constraint before judging the implementation.
- Order matters: `(ISA x kernel) x CTS` may differ from `ISA x (kernel x CTS)`. Read ISA before kernel, kernel before driver, and driver before CTS; then verify that the model survives the reverse order. If it does not, the instability is the finding.
- Independent-looking local passes can compose into a false global conclusion. Verify source independence before claiming bounded confidence.

### Proof-backed failure modes

When `open_gororoba/proofs/` is available, use proof names as review checks.

- `CDDoubleFunctor.cd_mul`: multiplication is non-commutative in `H` and above. Failure mode: reading driver before silicon can invert the synthesis direction. Protection: read hardware tiers first.
- `sed_assoc_nonzero_e1_e2_e4`: `[e1,e2,e4] != 0` in `O` and above. Failure mode: evidence order changes the synthesis. Protection: document evidence order and test stability under reversal.
- `moreno29_orthogonal_iff`: three orthogonal nulls construct a zero divisor in `S16` and above. Failure mode: three independent `nothing failed here` results can compose to a false positive. Protection: adversarial self-review.
- `cd_fidelity_stability`: the Lipschitz bound holds only for orthogonal sources. Failure mode: correlated sources amplify uncertainty non-linearly. Protection: verify source independence before claiming bounded confidence.

## PALM/Terakan evidence lane

Current target: x130e / AMD E-300 / Radeon HD 6310, PCI `1002:9802`, `CHIP_PALM`, PALM/Wrestler, Evergreen, TeraScale-2, VLIW5. PALM/Wrestler is not SUMO. SUMO and SUMO2 are adjacent Llano contexts, not the exact target.

PALM/Terakan RCA uses this evidence order:

1. Exact PALM measurement: probe, dmesg, BAR/debugfs-safe path, counters, CTS/Piglit/deqp on x130e.
2. Exact source: Terakan, r600g/SFN, Mesa/NIR, Linux radeon validation, and DRM UAPI.
3. Evergreen, TeraScale-2, and VLIW5 ISA and register programming guides.
4. R600, R700, and Cayman contrast only when labeled as inherited or adjacent and checked against PALM evidence.
5. Generated summaries, comments, and memory as leads only.

Before hardware RCA edits, record direct observation, source or spec constraint, implementation hypothesis, falsifier, validation command or retained bundle, and expected CTS/Piglit/deqp movement. Check `dmesg` for DRM CS validation errors before GPU-behavior analysis. Verify module reachability before symbolizing a crash address.

### Chip identity and comment names

Use chip identity strings that are searchable by codename, product, Mesa enum, platform, and ISA. Palm source-comment form:

`Palm (Wrestler GPU, CHIP_PALM, Evergreen / TeraScale-2 VLIW5)`

Chip identities:

- `R600` (HD 2900): `CHIP_R600`; R600 / TeraScale-1.
- `Cypress` (HD 5870, `RV870`): `CHIP_CYPRESS`; Evergreen / TeraScale-2 VLIW5.
- `Palm` (HD 6310, Wrestler GPU, Brazos platform + Bobcat CPU, integrated): `CHIP_PALM`; Evergreen / TeraScale-2 VLIW5.
- `Cayman` (HD 6970): `CHIP_CAYMAN`; Northern Islands / TeraScale-3 VLIW4.
- `Aruba` (Trinity APU GPU, integrated): `CHIP_ARUBA`; Northern Islands / TeraScale-3 VLIW4.

Source-comment forms:

- Driver: `Terakan` for Vulkan; `r600g` for Gallium.
- Register name: `CB_COLOR0_VIEW.SLICE_START`; `SQ_TEX_RESOURCE_WORD4.DST_SEL_X`.
- Register macro: `R_028C70_CB_COLOR0_INFO`; `S_028C70_FORMAT(x)`; `G_028C70_FORMAT(v)`; `V_028C70_COLOR_32`.
- ISA encoding: `FMT_32_32_32 = 47`; `V_028C70_NUMBER_USCALED = 0x2`.
- Architecture: `Evergreen / TeraScale-2 VLIW5`; `Northern Islands / TeraScale-3 VLIW4`; `R600 / TeraScale-1`.
- CPU side: `Bobcat` for Zacate/Ontario; `Llano` CPU.
- Platform: `Brazos` for Bobcat + Palm; `Llano`; `Trinity`.

Hardware and API citations MUST name public documents and sections, not internal extracts, retained bundle paths, or audit artifacts.

- Wrong: `per Evergreen_ISA.txt:17572`. Right: `per AMD Evergreen-Family ISA, section 10.x.x (MEM_RD_SCATTER)`.
- Wrong: `see phase5_isa_pdf_audit_20260418T182628Z/...`. Right: `per AMD Radeon HD 6000-Series ISA (Cayman), section X.Y`.
- Right: `per AMD 3D Engine Programming Guide for Evergreen, section M (CB_COLOR0_VIEW)`.
- Right: `per Direct3D 11.3 Functional Specification, section 4.4.6 Element Alignment`.

Hardware-specific source comments MUST include load-bearing hardware facts: bitfield encoding such as `SLICE_START bits 0-10 of CB_COLOR_VIEW`, empirical behavior such as `Palm silently no-ops MEM_RAT_CMPXCHG_INT on the cached path`, and any mathematical invariant or non-obvious workaround rationale needed to understand the code.

## GPU driver and reverse-engineering vocabulary

Use mechanism terms before summary terms. A Mesa claim MUST name the affected path: compiler lowering, descriptor construction, packet emission, kernel validation, resource lifetime, memory/cache behavior, runtime loader state, or conformance result.

Use these terms by path:

- Silicon identity: PCI ID, ASIC family, Mesa chip enum, IP block, generation, stepping, feature bit, engine, ring, aperture.
- Compiler path: NIR, TGSI, SPIR-V input, lowering pass, legalization, instruction selection, register allocation, scheduling, backend emission, disassembly oracle.
- Descriptor/resource path: BO, descriptor word, reloc-adjusted VA, pitch, tiling, swizzle, cache policy, coherency domain, map/unmap boundary, lifetime rule.
- Command stream: PM4 packet, indirect buffer, packet grammar, register write, draw or dispatch boundary, relocation, CS validator, fence, sequence number.
- Kernel interface: DRM UAPI, ioctl path, GEM, TTM, radeon object, CS parse path, fence wait, reset path, debugfs path, KMS interaction, dmesg validation error.
- Runtime path: ICD or DRI loader choice, dispatch table, winsys, screen/context/resource object, Gallium pipe state, Vulkan object lifetime, debug/release contamination.
- Evidence: CTS/Piglit/deqp result, dmesg delta, shader disassembly, packet decode, retained bundle, calibrated probe, golden trace, known-good/known-bad oracle.
- Upstream surface: minimal patch surface, bisectability, conformance delta, reviewer burden, ABI/install impact, backport risk, maintenance owner.

Do not write broad claims such as `the GPU supports X` or `the driver supports X` unless the evidence class is named. Use bounded claims:

```text
Known: PALM accepts this packet sequence through the radeon CS validator, and the retained CTS run observes the expected output.

Hypothesis: this is a descriptor-word construction bug, not a silicon-capability claim.

Speculative: adjacent Evergreen behavior suggests the same cache-domain rule, but exact PALM evidence is not yet decision-grade.
```

Use `breakthrough` only for a discontinuous result that changes the evidence graph. For normal Mesa work, use the mechanism: `driver enablement`, `conformance improvement`, `lowering-path correction`, `descriptor-path repair`, `packet grammar recovery`, `hazard-model refinement`, `silicon-behavior characterization`, `validation-methodology improvement`, or `source-grounded architecture model`.

## Standalone build

Mesa MUST build from this repository alone. Use reproducible native files and
environment variables. Meson owns configuration and Ninja generation. Make and
build-infra own host selection, audit checks, generated native overlays, clean,
build, and install. Change that split only with explicit approval for a
build-system architecture change.

Baseline standalone build:

```bash
meson setup builddir \
  --prefix="/opt/local/mesa-26-gororoba-debug" \
  -Dbuildtype=debugoptimized \
  -Dgallium-drivers=r300,r600,softpipe \
  -Dvulkan-drivers=amd_terascale \
  -Dllvm=enabled
ninja -C builddir
ninja -C builddir install
```

Adapt options to the checkout and current Meson option set. Use `meson configure` and repo-local options instead of guessing. Do not hardcode local absolute paths. Discover the repository root in scripts:

```bash
repo_root=$(git rev-parse --show-toplevel)
```

Build audits MUST model Meson defaults. When an option is omitted or set to `auto`, audit the dependencies Meson will enable on the target host. Do not treat an absent option as disabled.

Raw-submit and hazardous probes require exact opt-in values, such as `R300_TRACE_HAZARD_ACCEPTED=1`. Reject unset, empty, and zero-valued gates. Variable presence is not consent.

### Release, debug, and measurement contamination

Keep release and debug builds in separate build directories and separate install prefixes. They MUST NOT share object files, build directories, or install paths. Run `meson setup`, `ninja -C <builddir>`, and `ninja -C <builddir> install` completely for one build before starting the other.

Silicon evidence and conformance work use `buildtype=release`. `debugoptimized` and `debug` builds change timing, allocator behavior, and GL error paths. Do not collect silicon evidence from a debug-class build.

Driver RCA and shader disassembly use a separate `buildtype=debug` build at its own prefix.

Run probes against one build by pointing the loader at that build prefix and
Meson-configured library directory:

```bash
mesa_prefix=$(meson introspect <builddir> --buildoptions | jq -r '.[] | select(.name == "prefix").value')
mesa_libdir=$(meson introspect <builddir> --buildoptions | jq -r '.[] | select(.name == "libdir").value')
LIBGL_DRIVERS_PATH="$mesa_prefix/$mesa_libdir/dri" \
LD_LIBRARY_PATH="$mesa_prefix/$mesa_libdir${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
  ./probe_binary
```

For Vulkan probes, point `VK_ICD_FILENAMES` at the exact ICD JSON under the selected prefix.

Do not inherit `LIBGL_DRIVERS_PATH`, `LD_LIBRARY_PATH`, or `VK_ICD_FILENAMES` from the shell during release evidence runs. Unset them or set them explicitly. A debug DRI driver or stale Vulkan ICD loaded into a release probe invalidates silicon evidence.

### Build profiles and host envs

The default build profile lives at the top of `build-infra/configs/`; the other
profiles live in `build-infra/configs/alternates/`.  The Makefile resolves a
bare `PROFILE=` name against both directories, so `make` invocations name a
profile by basename regardless of which directory holds it.

- `2_r300_full_debug_x86_64v1-clang22-distcc-cache.meson` (DEFAULT, in `configs/`): maximal r300 plus `amd_r300` ICD; debug; vostro.
- `1_r300_full_release_x86_64v1-clang22-distcc-cache.meson` (`configs/alternates/`): maximal r300 plus `amd_r300` ICD; release; vostro. The conformance-baseline profile: GL/GLES/Piglit and silicon-evidence runs use this, because an asserts-live debug build can abort a case release would pass.
- `3_terakan_full_release_x86_64v1-clang22-distcc-cache.meson` (`configs/alternates/`): r600, zink, softpipe, LLVM, `amd_terascale`, and Rusticl; release; x130e.
- `4_terakan_full_debug_x86_64v1-clang22-distcc-cache.meson` (`configs/alternates/`): r600, zink, softpipe, LLVM, `amd_terascale`, and Rusticl; debug; x130e.
- `5_terakan_norusticl_release_x86_64v1-clang22-distcc-cache.meson` (`configs/alternates/`): same as profile 3 without Rusticl; release; x130e fallback.
- `6_terakan_norusticl_debug_x86_64v1-clang22-distcc-cache.meson` (`configs/alternates/`): same as profile 4 without Rusticl; debug; x130e fallback.

Active host envs live in `build-infra/env/`:
`vostro1000-x86-64-v1-clang22-ccache-distcc.env` for the numbered profiles,
and `generic-x86-64-os.env` for ad hoc portable builds. Historical btver1,
sapphire, zen4, and distcc-pump envs live under `build-infra/env/Archive/` and
are not active Make `HOSTENV` values. Active envs set lane-specific
distcc/cache policy, host CFLAGS, `-fno-emulated-tls`, and centralized
`CCACHE_DIR`/`SCCACHE_DIR`. The validated clang lane on Linux x86_64 requires
`-fno-emulated-tls` to avoid a libglapi link failure.

### Build directories and install prefixes

Each build directory maps to one install prefix. Do not share object files or
install paths between directories.

The Makefile derives the canonical build directory from the profile:
`build/mesa-<profile>/`. A plain `make install PROFILE=<profile>` installs to
the isolated default prefix `/opt/local/mesa-<profile>`, which keeps profile
artifacts separate for review, bisect, and evidence work.

The shared active prefixes are only for intentional operator-selected installs:

- release active tree: `/opt/local/mesa-26-gororoba`;
- debug active tree: `/opt/local/mesa-26-gororoba-debug`.

Use the `install-<profile>` targets, or pass `PREFIX=` explicitly, only when the
goal is to replace one of those active trees. Do not install unrelated profile
builds into the same active prefix during evidence collection. Neither prefix is
inside the repository tree. Do not use in-repo `install/` directories or
suffixed variants such as `install-gallium`; they pollute the worktree and
require separate `LIBGL_DRIVERS_PATH` or `VK_ICD_FILENAMES` overrides. Project
builds MUST NOT disturb system Mesa under `/usr/lib/`.

### Clean and reconfigure

Incremental clean removes compiled objects and keeps Meson configuration:

```bash
ninja -C <builddir> clean
```

Full wipe and reconfigure is required when Meson options change or after a Meson upgrade:

```bash
meson setup --wipe <builddir> [options...]
```

`--wipe` gives a fresh directory setup while preserving download caches. Do not edit `meson-private/cmd_line.txt` by hand. After `--wipe`, run `ninja -C <builddir>` and `ninja -C <builddir> install` in full before collecting evidence.

## Build-system and cache discipline

Native files use PATH-resolved compiler names or generated local overlays. Checked-in files MUST NOT contain private compiler paths. Rust is selected by active Meson/toolchain policy, not by a checked-in absolute path.

Make writes version-coupled LLVM helper tools into `$BUILDDIR/gororoba-toolchain.meson` before `meson setup`. The generator prefers the x130e LLVM major when present, honors `MESA_LLVM_VERSION` or `GOROROBA_LLVM_VERSION` when set, and otherwise selects an installed coherent `clang`/`clang++`/`llvm-config` major on the host.

C/C++ cache lanes:

- Warm incremental: `ccache -> distcc -> clang`, no pump. Rust:
  `sccache -> rustc`. Use `CCACHE_PREFIX=distcc`; expect hits after a
  populated build.
- Historical distcc-pump envs are archived and have no active Make target. Do
  not revive pump without a new build-system design review because upstream
  removed the supported pump lane during profile consolidation.

Active configure writes:

```ini
[binaries]
c    = ['ccache', '<selected-clang>']
cpp  = ['ccache', '<selected-clang++>']
rust = ['sccache', 'rustc']
llvm-config = '<selected-llvm-config>'
```

Historical pump configure wrote:

```ini
[binaries]
c    = ['distcc', '<selected-clang>']
cpp  = ['distcc', '<selected-clang++>']
rust = ['sccache', 'rustc']
llvm-config = '<selected-llvm-config>'
```

Do not wrap pump builds with ccache or sccache. Do not use wrapper scripts such as `exec ccache distcc clang "$@"`; that form causes about 93.5% `Multiple source files` ccache rejections because ccache hashes `distcc`'s mtime as the compiler. Use Meson `[binaries]` for the wrapper boundary and `CCACHE_PREFIX=distcc` for the cache chain.

`RUSTC_WRAPPER` is cargo-only here. Do not assume it changes Meson C/C++ behavior or Meson Rust selection. The Rust sccache lane remains separate because it wraps `rustc`, not the C/C++ include-server path.

The workspace patched sccache for meson-rust's multi-`--emit` form. Select the patched binary through PATH order or host env, not a baked per-user path.

When cache wiring changes, consult `steinmarder/docs/workspace/ccache-sccache-wiring.md` and `steinmarder/docs/workspace/sccache-multi-emit-patch.md`, then update the reproducible recipe. Do not bypass cache misses silently.

Check ccache state with:

```bash
ccache --show-stats --verbose
```

Expected status: a first full build populates `~/.cache/ccache` and shows about 95% misses while filling the cache. Later rebuilds with unchanged sources should show more than 90% hits.

## Submission and upstream

`origin` names this fork. `upstream` names freedesktop.org Mesa (`mesa/mesa`).

Do not push `upstream/main` directly to fork `main` with `git push origin upstream/main:main`. Sync through an intentional rebase and record any divergence.

Separate upstreamable fixes from local evidence and bring-up scaffolding. Keep Terakan and R300VK changes reviewable by mechanism, not batch size.

Use `docs/submittingpatches.rst` on the mesa-26 branch for Mesa submission rules. Apply those rules unless a fork-local task explicitly narrows scope. Patches must not mix behavior with formatting churn, should affect one component when possible, must not break builds, should remain bisectable, must be tested prudently, and must be presented without fixup commits for review.

Commit subjects use a component prefix and a concise mechanism. Commit bodies name the invariant or bug, describe the change at maintainer-review depth, cite evidence, and state tests without turning the message into a template.

Use `Closes:` for GitLab issue URLs. Use `Fixes:` only for the earlier commit that introduced the defect. Use `Backport-to:` or the current Mesa stable marker only when appropriate.

Do not submit Mesa patches through an autonomous tool. The submitter must understand and own the change, and must add AI disclosure trailers when Mesa policy requires them.

### AI-assistance trailers

Mesa reserves `Co-authored-by:` for human co-authors. Do not use it for AI tools.

List only tools used for the commit:

```text
Assisted-by: Claude (Opus/Sonnet 4.x), ChatGPT Codex (5.x), Gemini (Flash/Pro 3.x), Mistral, Ollama, DeepSeek
```

Use `Assisted-by:` for mixed human/AI work. Use `Generated-by:` when AI generated almost the entire change. Trivial or mechanical changes may omit disclosure when Mesa policy allows omission. Source headers never carry AI labels; disclosure belongs in commit trailers only.

Historical pre-policy commits with `Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>` remain historical artifacts. Do not force-push to rewrite them. New commits use `Assisted-by:` or `Generated-by:`.

Checked-in patch snapshots may retain original trailers: `src/re/r600/results/.../*.patch`, kernel-module patch series under `src/amd/...`, and embedded DKMS sources. They are snapshots and must not be rewritten only for trailer policy.

### File headers, copyright, and SPDX

Use `docs/submittingpatches.rst` on the mesa-26 branch for Mesa header rules. Per-file headers are optional; many Mesa files use SPDX-only headers with no copyright line.

For new source files, match adjacent-file style. When a copyright header is appropriate, use:

```c
/*
 * Copyright (c) YYYY Terascale Functionalists
 * SPDX-License-Identifier: MIT
 *
 * <one-line file description>
 */
```

When adjacent files use SPDX-only style, use:

```c
/* SPDX-License-Identifier: MIT */
```

Do not fabricate individual personal-name copyright lines such as `Copyright (c) YYYY <git config user.name>`, `Copyright (c) YYYY Eirikr Hinngart`, or `Copyright (c) YYYY eirikr`. Strip LLM-template defaults.

Do not add `(LLM-assisted)`, `Generated by Claude`, or any AI label to a file header. Do not use `Copyright (c) YYYY steinmarder project`; use `Terascale Functionalists` when a project collective line is appropriate.

Preserve existing upstream headers verbatim, including author name and year. This applies to Vitaliy "Triang3l" Kuzmin-authored Terakan files, Mesa-tree files from prior contributors, and any pre-existing header with a non-project author. The MIT license requires preserving copyright and permission notices in copied or substantial portions.

Do not strip or rewrite an upstream header when refactoring, splitting, or moving a file. When adding content to an upstream file, keep the existing upstream header and do not add a second project-collective copyright line above it.

## Comments, commits, and Markdown

Follow local style first: `.editorconfig`, `.clang-format`, adjacent code, and repo-local build rules override generic taste. Mesa-shaped C/C++ uses 3-space indentation, no tabs, 78-column code where practical, short Doxygen-shaped function comments when useful, and comments that name the spec or source rule when that rule controls the code. Do not mass-reformat code when the patch is about behavior.

Use American (United States) English spelling in new or modified source comments, commit messages, and project-authored documentation: `honor`, `behavior`, `initialize`. This rule applies only to new or modified text. Do not edit upstream comments solely for spelling; churn-only spelling edits create unnecessary merge conflicts. Quoted spec text, kernel symbol names, diagnostics, and hardware identifiers stay verbatim. When a substantive edit already changes a comment line, the touched line may be aligned to American English; otherwise leave existing spelling alone.

Source comments MUST NOT contain unstable or private references: task numbers, private issue numbers, PR numbers, companion-PR breadcrumbs, wave/phase/mission/session labels, worktree names, agent names, author tags, local absolute paths, private host FQDNs, raw private IPs, deictic time, dated claim/LI/Q tags, deictic chip names, internal-repo source paths, or internal evidence paths as authority.

Examples of forbidden source-comment authority include `companion to PR #...`, `Phase 4.4`, `Step 1 of Phase 3`, `@triang3l`, `(eirikr)`, `as of today`, `currently`, `will be exercised when Phase 5 lands`, `C-2026-04-19-06`, `LI-2026-04-17-02`, `this chip family`, `our GPU`, and `per Evergreen_ISA.txt:17572`.

Source comments name durable mechanisms: exact chip, ISA/register rule, API/spec rule, kernel validator, test class, or measured behavior.

Commit messages and finding documents may carry chronology when useful. Source comments do not.

Preferred shape:

```text
The kernel treats WORD0 as the per-BO byte offset. It adds the relocation base
before validation, so the shader sees the caller's intended buffer address.
```

Bad shape:

```text
Phase 8 workaround from the agent branch.
```

Use short labels for obvious local sections. Use compact multi-sentence blocks only when the code depends on hardware behavior, API rules, kernel validation, empirical evidence, or non-local invariants. Mechanism controls comment length. Default to the shortest form that preserves the load-bearing constraint: a single sentence trailing the code, a two-to-four sentence block for a constraint with interacting parts, and a five-line-or-longer paragraph only for a verbatim spec quote, a multi-step silicon-bug explanation, or workaround rationale that will not compress. If removing a sentence leaves the constraint unchanged, remove it; a block that could be halved without losing a load-bearing fact should be halved. Mesa-upstream blocks for a single constraint (see `si_buffer.c`, `evergreen_state.c`) are typically four to five lines -- our r300vk blocks that run past eight lines for one constraint are over-written and should be trimmed.

Do not frame comments with decorative delimiter lines, banner boxes, ASCII art, or long punctuation runs. Avoid wrappers such as `/* ----- */`, `// =====`, and `/* --- label --- */`. Start with the first useful sentence and stop after the last useful sentence.

When extending Triang3l-authored Terakan files, match the file cadence: shorter line lengths, fewer subclauses, and comments only when they carry silicon, spec, or test information.

Commit messages and PR titles are mechanism-named and component-prefixed when project style expects it. The body makes the invariant, change, and evidence reviewable in one to five sentences: name the root cause or constraint, name the fix, cite the spec section, register macro, or kernel function when load-bearing, and state any test movement plainly. `Fixes:`, backport notes, AI disclosure, and review trailers stay where Mesa expects them.

Keep the commit body to mechanism, matching the Mesa-upstream norm (e.g. `radeonsi: fix conformance window emission in the SPS` -- two sentences of cause and one example). Do NOT paste build invocations, profile names, tool output, host names, or a `Validation:` checklist into the body; that process evidence lives in the PR description, never in `git log`. Do NOT add `Note:` paragraphs about pre-existing CI noise the patch did not cause. Do NOT embed a PR or MR number in the subject (no `(#NNN)` suffix); use a `Part-of:` trailer. A body that reads like a worklog -- nested `*` bullets from a coarse squash, several sub-components -- means the commits were not granular enough: split them or compress to the aggregate mechanism.

Do not mix formatting churn with logic changes. Each commit should be buildable, reviewable, and bisectable unless a stated migration plan says otherwise.

### Source comment shape

Use these examples as style anchors: the WORD0 fix block in `src/amd/terascale/vulkan/terakan_dispatch.c` near `PKT3_SET_RESOURCE` and `desc[0] - bo->va`, and the inline silicon notes in `terakan_format.c` and `sfn_instr_mem.cpp`.

A mechanism comment has this order:

1. Load-bearing claim: `WORD0 carries the per-BO byte offset.`
2. Public or source authority by name: `evergreen_packet3_check`, AMD ISA chapter, register macro, or spec section.
3. Consequence, with an inline code fragment when clearer than prose: `ib[WORD0] = reloc->gpu_offset + offset`.
4. Test reference when the comment explains a fixed failure: CTS case or `dEQP-VK.<group>.*`.
5. Env knobs or flags, grouped at the end of the block when relevant.

Default to short comments. A one-line trailing comment on the load-bearing line is better than a function-header paragraph unless the whole function encodes a non-obvious invariant.

Use one thought per comment. Stack separate comments when steps are distinct. Do not fuse separate steps into one multi-clause sentence.

Do not paraphrase the next line of code. If the code already says what happens, the comment explains why the code has that shape: silicon constraint, spec section, kernel validator, measurement, or non-local invariant.

Do not comment mechanical code. Comments earn space by carrying information that does not survive in code alone.

Name citations by authority, not by internal line number or private path: AMD Evergreen-Family ISA section, `SQ_TEX_RESOURCE_WORD4.DST_SEL_X`, Vulkan spec section, or kernel function.

Use active voice and sequence. `The kernel reads WORD0. Then it adds reloc->gpu_offset. Then the shader sees the intended VA.` is better than one passive sentence with three clauses.

Example:

```text
The kernel reads WORD0 as a byte offset. It adds reloc->gpu_offset.
The buffer base reaches the shader at the right VA; any per-element
offset requested by the caller survives relocation.
```

Multi-paragraph comment blocks are reserved for genuine silicon-quirk reasoning.

### TODO comments

Rule files are not TODO trackers. Deferred work belongs in the source file at the affected mechanism, using `TODO:`, `FIXME:`, `XXX:`, `HACK:`, or `PLACEHOLDER:` at the start of the comment. Do not add real future-work TODOs to rule files. Examples in this section describe comment shape only.

A TODO-family comment MUST name three mechanism elements:

- missing work: function, register, ISA section, kernel symbol, or spec chapter that needs the change;
- deferral reason: silicon, ABI, or evidence constraint blocking completion now;
- tracking artifact: durable function name, register name, `gitlab.freedesktop.org` issue URL, spec chapter, or silicon-constraint name.

A TODO-family comment MUST NOT contain reviewer breadcrumbs, PR-thread references, phase/wave/mission labels, AGENTS.md rule numbers, or deictic references such as `currently`, `previously`, `this driver`, or `our GPU`.

Wrong shape:

```text
/* TODO: ...  Reason for deferral: outside this PR's scope.
 *       Tracking: reviewer P1 badge on the consolidated style PR.
 */
```

Right shape:

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

Reviewer feedback, PR chronology, and phase labels belong in the commit message or PR description. Source comments carry mechanism only.

### Finding documents and agent-loaded Markdown

Finding documents may carry chronology: dated frontmatter, `last_verified`, `evidence_class`, dated filenames, and ordered predecessors. PR or task references must be paired with a durable identifier.

Examples:

- Wrong: `the fix landed via mesa PR #34`
- Right: `landed in commit f230cb07db6 (terakan_buffer.c::terakan_CreateBuffer size-zero guard); PR #34 / branch fix/w9-buffer-size-zero-guard for cross-link`
- Wrong: `see issue #157`
- Right: `see filed-finding 2026-05-15-induced-lockup-recovery-test-results.md (PR #41 if still open)`

Markdown loaded by agents MUST use exactly one H1, heading depth no deeper than `###`, frontmatter on programmatically loaded files, language tags on code fences, exact cross-references, and rule text in `MUST`, `MUST NOT`, or `SHOULD` form.

Use tables only when columns carry independent comparison value. Prefer bullets for simple ownership, lookup, and rule lists.

Do not use emoji, ASCII boxes, banner dividers, `see above`, `see below`, or vague future promises in rule files. Slice-loaded text must stand without nearby context.

### Comment-hygiene linter and Git hook

Comment-hygiene enforcement MUST NOT make a clean Mesa checkout depend on `steinmarder/`.

If the linter is mirrored or vendored into Mesa, wire it through the local pre-commit framework and treat it as a Mesa-side gate. If the only implementation is in `steinmarder/`, treat it as advisory until the Mesa-side mirror exists.

Advisory sibling invocation, when the checkout layout provides it:

```bash
../steinmarder/src/re/r600/scripts/lint/comment_hygiene_lint.py --staged
```

Do not install a blocking Git hook in Mesa that points at a missing sibling path. A hook that depends on an external checkout violates Mesa independence and turns comment hygiene into an environment accident.

New commits must follow this policy even when no linter runs. The linter enforces part of the policy; it does not define it.

Existing in-flight PRs may keep breadcrumb comments. Do not force-push only to scrub them. New commits must follow this policy.

## Evidence boundary

Driver-RCA evidence lives in `steinmarder/`, not in Mesa. Before proposing a Mesa driver fix, consult sibling evidence when the checkout is available. This applies to Terakan, r300g, r600g, R300VK, shared NIR/compiler paths, build infrastructure, and cross-driver conformance work.

Primary sibling references are grouped by lane.

Shared workspace references:

- `steinmarder/GPU_ARCHITECTURE_BASELINES.md`
- `steinmarder/AGENTS_README.md`
- `steinmarder/AGENT_RULES.md`
- `steinmarder/docs/workspace/host-setup.md`
- `steinmarder/docs/workspace/ccache-sccache-wiring.md`
- `steinmarder/docs/workspace/sccache-multi-emit-patch.md`
- `steinmarder/docs/workspace/mesa-fork-synthesis.md`
- `steinmarder/docs/workspace/mesa-fork-upstream-divergence.md`
- `steinmarder/docs/workspace/hostname-policy.md`

r600 and Terakan references:

- `steinmarder/src/re/r600/`
- `steinmarder/src/re/r600/findings/CLAIMS.md`
- `steinmarder/src/re/r600/findings/active/`
- `steinmarder/src/re/r600/results/`
- `steinmarder/src/re/r600/docs/rca/`

r300 and R300VK references:

- `steinmarder/src/re/300/`
- `steinmarder/src/re/300/findings/CLAIMS.md`
- `steinmarder/src/re/300/findings/active/`
- `steinmarder/src/re/300/results/`
- `steinmarder/src/re/300/docs/rca/`

Use the repository's exact path spelling. Do not invent an alias such as `steinmarder/src/re/r300/` unless that path exists. If both `src/re/300/` and another r300-named path exist, cite the exact path used by the evidence.

Steinmarder evidence may be cited by durable bundle path, finding filename, public spec name, exact hardware identity, or commit SHA. Do not copy bundles, findings, host kits, or local orchestration files into Mesa.

Mesa source comments prefer public specs, source symbols, DRM/Khronos names, freedesktop.org references, and exact hardware identity. Freedesktop.org GitLab issue URLs and issue numbers are allowed. Internal GitHub or GitLab issue numbers from this fork or from `steinmarder/` are not source-comment authority.

Empirical fork evidence belongs in findings or commit messages unless the source code needs the mechanism to be intelligible.

## Tooling for RCA and audits

Use the strongest available tool that matches the claim. Do not use a weaker text search when a structural, indexed, flow-sensitive, binary, or empirical tool is required.

### Tool availability

When a relevant tool is missing, do not treat absence as evidence. Determine the package or install path, update the installation requirements document for the affected module, and record the validation command. If the tool cannot be installed in the current environment, record `not run` and why.

Tool requirement updates MUST name:

- tool command or package name;
- reason the tool is needed;
- install source or package manager when known;
- expected version or version floor when required;
- validation command, such as `<tool> --version`;
- module, script, or audit that depends on it.

### Tool classes

Source navigation and reachability:

- `clangd`, LSP, `read-tags`, `ctags`, GNU Global/`gtags`, `cscope`, `rg`, `ripgrep`, `git grep`, `git-grep`, `fd`, `git log -S`, `git log -G`, `git blame`.

Structural search and source transformation:

- `ast-grep`, Semgrep, Coccinelle/`spatch`, `weggli`, `comby`, Tree-sitter CLI.

Static analysis, complexity, and source metrics:

- compiler diagnostics, warnings-as-errors, `clang-tidy`, `scan-build`, `cppcheck`, `sparse`, `smatch`, Infer, CodeQL, `lizard`, `scc`, `cflow`.

Mesa, shader, and generated-state evidence:

- Mesa build logs, CTS/Piglit/deqp logs, NIR dumps, shader disassembly, packet decode, generated-file checks, known-good/known-bad test inputs.

Binary, reverse-engineering, and symbolization:

- `gdb`, `addr2line`, `objdump`, `nm`, `readelf`, LIEF, `binwalk`, radare2/`r2`/`radiff2`, Rizin, Ghidra.

Tracing, profiling, and runtime inspection:

- `strace`, `ltrace`, `perfetto`, `trace-cmd`, LTTng, `lttng`, `lttng-tools-generic-kernel`, SystemTap/`stap`, `bpftrace`, `bcc-tools`, Sysprof, Valgrind, Heaptrack, Hotspot, `python-ptrace`, Frida, `frida-tools`, `python-frida`, `python-frida-tools`.

Fuzzing and input mutation:

- honggfuzz, AFL++, Radamsa.

C unit and integration support:

- Check, shellcheck where applicable, project-native test runners, calibrated probes.

Probe and harness storage: calibrated probes, one-off harnesses, and experiment
artifacts live in the `steinmarder` reverse-engineering tree (`../steinmarder`), never in
this Mesa source repo. Keep this repo to driver code, build infrastructure, and committed
tests; a probe that must persist goes to `steinmarder/src/re/r300` with its evidence
bundle.

Local invocation notes (cachyos/Arch): `spatch` is `/usr/bin/spatch` (coccinelle-bin);
BCC ships as the wrapped tools `/usr/bin/opensnoop` and `/usr/bin/execsnoop`; `ast-grep`,
`lizard`, and `weggli` are under `~/.local/bin`. MSAN is not usable for Mesa: MemorySanitizer
needs the entire dependency closure (libc++, LLVM, libdrm) instrumented or it reports
false uninitialized reads on every uninstrumented frame, so use ASan+UBSan for memory
errors and Valgrind/memcheck or DRD for uninitialized-read and race detection instead.

Heavy code intelligence and graph tooling:

- Joern, CodeQL databases, Ghidra projects, radare2/Rizin projects.

### Tool tiers

Tier S tools are required when available and relevant:

- `clangd` or LSP for definitions, references, call hierarchy, and symbol reachability.
- GNU Global, `cscope`, or equivalent indexes for large C trees.
- `ast-grep` or Semgrep for structural patterns and class-of-bug audits.
- `rg`, `git grep`, and `fd` for text, comments, strings, generated paths, and fallback search.
- `git log -S`, `git log -G`, and `git blame` for evolution.

Tier A tools are flow-sensitive or build-sensitive:

- compiler diagnostics, warnings-as-errors, and static analysis where viable;
- Coccinelle for cross-file consistency checks;
- Mesa, kernel, CTS, Piglit, and deqp runner logs;
- shader disassembly, packet decode, and NIR dumps;
- generated-file checks.

Tier C tools are empirical:

- CTS/Piglit/deqp, `dmesg`, hardware counters, retained bundles, perf/ftrace/bpftrace, and minimal reproducers when hardware policy permits.

When an audit reports a code claim, cite how the symbol or path was found, not only `file:line`. Examples: `(clangd: references on FUNC)`, `(global -r SYMBOL)`, `(rg --fixed-strings SYMBOL src/)`.

Every new probe, lint, or verdict-producing script MUST be calibrated against known-good and known-bad inputs before its verdict is trusted.

When using Coccinelle, locate `spatch` with `command -v spatch`, record the installed package or path, and cite the semantic patch used. Do not cite `spatch` output without the semantic patch and target path set.

### Agent coordination

Use at most three concurrent subagents. Assign each subagent a bounded read-only task, input scope, expected output, and citation requirement. The parent agent owns synthesis, conflict resolution, implementation choices, and final claims.

Use small or local models for search, file location, summarization, and citation fan-out. Escalate to a larger model only for deep synthesis, hazardous decisions, hard-to-reverse changes, or cross-file mechanism reasoning. Record the escalation reason when it affects the work record.

Do not let subagents make load-bearing implementation decisions, push commits, delete files, alter build configuration, or suppress warnings. Subagents collect evidence; the parent evaluates it.

### Retained tools and probes

Retained analysis tools, probes, linters, and verdict-producing scripts MUST be real programs integrated into the repository's native build or validation flow when they become required. They MUST NOT be hardcoded demonstrations.

A retained tool MUST have:

- clear inputs and outputs;
- deterministic behavior where practical;
- known-good and known-bad calibration cases;
- documented dependencies;
- documented safety gates for hazardous hardware access;
- a validation command;
- a maintainer-readable failure mode.

Do not add a retained script that only proves one handpicked case unless it is explicitly documented as a temporary reproducer.

## Validation expectations

Minimum validation depends on the changed surface.

- Terakan source: targeted build plus relevant runtime or CTS when available.
- r300/r600 source: targeted build plus Piglit, CTS, deqp, or documented hardware gate.
- NIR, lowering, and compiler code: build plus shader/compiler tests and affected conformance tests.
- Meson and build files: clean setup or reconfigure plus Ninja target and artifact/install check.
- Generated files: run the generator or document why unavailable; verify the generated diff.
- Source comments and docs: comment hygiene, Markdown structure, and source-reference audit.
- Scripts: shellcheck when applicable plus known-good and known-bad paths.

Do not claim a test passed if it was not run. Say `not run` and why. If a test is blocked by hardware safety, name the required gate. If CTS/Piglit/deqp movement differs from prediction, treat the deviation as evidence.

## Security and hardware stop-line

Critical security defects and unsafe hardware-access defects stop normal feature work. Contain and report before continuing if a change exposes:

- secrets, tokens, credentials, or private request bodies;
- SQL injection or unsanitized query construction;
- command injection through shell wrappers, generated scripts, hooks, or test runners;
- path traversal or unchecked filesystem writes;
- sensitive data in logs, manifests, bundles, test output, or generated artifacts;
- missing authentication or authorization on sensitive paths;
- insecure deserialization or SSRF;
- unsafe MMIO, BAR, `/dev/mem`, raw-submit, reset, or privileged debugfs access outside the lane's explicit gate.

Never pass untrusted input to `sh -c`, `bash -c`, `eval`, generated shell fragments, or unchecked path concatenation. Use allow-lists, normalization, containment checks, and exact opt-in gates. Do not log secrets or raw tokens.

## Synthesis over selection

When merging parallel branches or review findings, preserve all non-refuted content. Mechanism and evidence decide; wave, phase, chronology, branch age, and author do not.

Default additive resolution is union plus synthesis. Selection is allowed only when the discarded side is empirically refuted by tier-1 through tier-3 evidence or genuinely superseded by a verified line-level diff and recorded rationale.

Merge actions:

- Analyze: identify what each side contributes before editing. Every differing line is a candidate for preservation.
- Reconcile: preserve non-refuted content. Selection requires proof of refutation or supersession.
- Resolve: finish the merge; leave no half-merged state. Record outstanding items explicitly when a session cannot finish.
- Expand: connect findings, code paths, and tests. Do not paste two sections side by side without explaining their relationship.
- Harmonize: use one durable mechanism name for the same thing across the synthesized artifact.
- Infuse: add the check, citation, rule, lint, or test that prevents the same failure class.

A synthesis MUST add value: unified model, terminology map, cross-reference, stronger rule, validation matrix, refined evidence tier, retained test, or clearer upstream path. A paste of A next to B is not synthesis.

After every merge resolution, read the staged diff adversarially:

```bash
git diff --staged
```

Ask what non-refuted content from each source was dropped. If anything was dropped, restore it, refute it by name and citation, or record it as explicit follow-up.

Forbidden additive-content shortcuts:

```bash
git merge -X theirs branch/feature-a
git checkout --theirs file
sed -i '/^<<<<<<< /d; /^=======$/d; /^>>>>>>> /d' file
```

Those forms select instead of synthesizing. The `sed` form can silently drop the `|||||||` base block and any content between it and `=======`.

Preferred pattern:

```bash
git cherry-pick --no-commit <sha>
git diff --staged
git commit
```

For a superseded PR, cherry-pick the relevant SHA with `--no-commit`, verify hunks, add the cross-link in the synthesis PR commit message, and only then close the superseded PR.

For additive Markdown synthesis, compare each source against the merged result after hand merge:

```bash
comm -23 <(sort -u source_a.txt) <(sort -u merged.txt) > only_in_a.txt
comm -23 <(sort -u source_b.txt) <(sort -u merged.txt) > only_in_b.txt
```

`only_in_a.txt` and `only_in_b.txt` should be empty unless missing lines are explicitly refuted or tracked. `comm` detects dropped lines only; it does not preserve narrative order. Read the merged section by hand.

Use steinmarder `AGENTS_README.md` "Synthesis Doctrine" and `AGENT_RULES.md` "Rule: Synthesis Over Selection" for the gate-oriented merge check when available.

## Regression-on-fix discipline

A targeted fix for issue A MUST NOT regress unrelated behavior B. After changes to scripts, runners, build files, lowering passes, descriptor paths, or comments:

- read `git diff --staged` adversarially;
- verify each removed line was intentional, duplicated elsewhere, or refuted;
- compare test labels with the commands they run;
- verify every symbol named in comments or docs against source;
- enumerate every override mechanism before documenting one;
- check optional tool availability before configuring for it;
- calibrate each new verdict-producing probe, lint, or runner on known-good and known-bad inputs.

When a reviewer finds a defect, fix the class, not only the instance. Add the rule, lint, test, or documented check that would have caught it.

## Strict clean and deletion readiness

When asked to clean, clean-and-merge, prune, or assess a repository for
deletion, apply `docs/strict-clean-definition.md` before deleting anything. That
file is the authoritative checklist; this section is only the pointer to it.

## Multi-CLI loaders

`AGENTS.md` owns Mesa rules. Tool-specific root files only load it and add tool-specific loading notes.

- `CLAUDE.md` loads `@AGENTS.md` and may contain Claude-specific loading notes.
- `GEMINI.md` loads `@AGENTS.md` and may contain Gemini-specific loading notes when Gemini CLI is used.
- Do not symlink loader files.
- Do not duplicate this body into loader files.
- Do not maintain different doctrine across loader files.

## Key subsystems

Use these root paths for first-pass scope:

- Terakan Vulkan: `src/amd/terascale/vulkan/`
- Gallium r600, SFN, and VLIW5: `src/gallium/drivers/r600/`
- Gallium r300: `src/gallium/drivers/r300/`
- Rusticl/OpenCL when enabled: `src/gallium/frontends/rusticl/`
- winsys/drm: `src/gallium/winsys/`, `src/drm-shim/` where applicable
- NIR and compiler plumbing: `src/compiler/`, `src/gallium/auxiliary/`, affected lowering paths
- Build entry: `build-infra/`, `meson.build`, `meson.options`, `meson_options.txt`, native files, install scripts

`rust-toolchain.toml` is an upstream Mesa file with `channel = "nightly"`. Active builds select Rust through Meson and toolchain policy, not checked-in absolute paths.

Keep subsystem-specific rules near the subsystem when they are too narrow for this root file. Do not turn `AGENTS.md` into a dump of lane-local rules. Move narrow rules into a lane README or path-scoped agent doc and leave a pointer here.

## Engineering foundations

The project motto is:

`AD ASTRA PER MATHEMATICA ET SCIENTIAM ET TECHNICUM`

Meaning for repository work: advance only through mathematics, science, and engineering. Creative insight is welcome, but every insight must be reduced to mechanism, evidence, implementation, and validation.

Use disciplined imagination. Generate bold hypotheses, then constrain them with source, specification, silicon behavior, build results, tests, and adversarial review. A useful idea becomes repository value only after it is expressed as code, documentation, probe methodology, validation data, or a clearer model of the system.

Final artifacts MUST be more accurate, reproducible, navigable, testable, and source-grounded than their inputs.

### Engineering posture

Investigate before asserting. Trace behavior to source, specification, test oracle, log, generated artifact, kernel path, command stream, retained evidence, or silicon measurement.

Treat warnings and unexpected output as defects until explained. Configure builds and tests to expose the full failure surface when practical; do not hide downstream failures by stopping at the first convenient success.

Avoid hardcoded shortcuts, local users, symlinks, local-only paths, machine-specific FQDNs, raw private IPs, and unreproducible environment assumptions.

Implement robust solutions, not demos, decorative abstractions, or plausibility sketches. A change is not complete until its mechanism, dependencies, validation path, and remaining uncertainty are recorded.

Notice anomalies while working. A surprising deviation is evidence, not noise. Preserve it, name it, and decide whether it changes the model.

### First-principles workflow

Start from the task, not from a preferred patch shape.

1. Define scope.
2. Identify assumptions.
3. Derive constraints.
4. Inspect primary sources.
5. Decompose claims into testable units.
6. Build a mechanism model.
7. Test the cheapest decisive hypothesis first.
8. Implement the smallest complete mechanism.
9. Validate against known-good, known-bad, and target cases.
10. Record what changed, what was tested, what was not run, and what uncertainty remains.

Use online research and community archaeology only as supporting evidence. Public specifications, source code, tests, hardware measurements, and retained bundles carry more weight than generated summaries or memory.

### System model

When explaining or changing a subsystem, describe the interconnected mechanism:

- call paths;
- data flow;
- control flow;
- state ownership;
- ABI and UAPI boundaries;
- register paths;
- command streams;
- descriptor and resource lifetime;
- synchronization points;
- cache and coherency domains;
- error paths;
- generated files;
- build graph;
- test oracle;
- known deficiencies.

Do not describe a subsystem as a loose collection of files. Describe what state moves, who owns it, which invariant constrains it, and how failure becomes observable.

### Synthesis requirement

Where content overlaps, unify it. Where arguments diverge, reconcile them or name the evidence that distinguishes them. Where ideas repeat, collapse redundancy without losing depth. Where parameters are absent, surface them. Where models are implicit, extract and test them.

A synthesis MUST improve the material. It must add at least one of:

- a stronger mechanism model;
- a terminology map;
- a cross-reference;
- a validation matrix;
- a sharper evidence tier;
- a source-grounded invariant;
- a reusable probe;
- a semantic-preserving refactor;
- a clearer upstream path.

Merging text without improving the model is not synthesis.

### TODO and defect pressure

`TODO`, `FIXME`, `XXX`, `HACK`, and `PLACEHOLDER` comments are evidence-bearing artifacts. Before changing or removing one, analyze its local context, historical reason, architecture pressure, and testability.

A deferred-work marker MUST identify the missing mechanism, the reason it remains deferred, and the durable tracking artifact. It MUST NOT carry PR chronology, phase labels, reviewer breadcrumbs, agent identity, or session history.

Every unresolved gap should be scoped when discovered: missed parameter sweep, symbolic mutation, undeveloped theory link, unvalidated hardware assumption, register ambiguity, command-stream uncertainty, ABI hazard, build-system fault, test gap, probe limitation, or undocumented dependency.

### Creative rigor

Use creativity to discover hidden invariants, not to bypass validation. Useful outcomes include:

- hardware-grounded formulations;
- command-stream decompositions;
- probe-backed discoveries;
- validation harnesses;
- methodology improvements;
- algorithmic refinements;
- semantic-preserving refactors;
- conformance improvements;
- production-relevant cleanup;
- clearer architecture models.

Confidence is earned by evidence. Novelty is valuable only when it improves prediction, validation, implementation, or maintainability.

Spark the mind; verify the result.
