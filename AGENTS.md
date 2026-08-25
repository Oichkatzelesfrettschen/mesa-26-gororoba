# mesa-26-gororoba Agent and Developer Reference

## Instruction source

`AGENTS.md` is the root instruction file for `mesa-26-gororoba` and owns the Mesa rules.

All Mesa agents and contributors follow `AGENTS.md`. Codex, compatible agents, and human contributors read it directly. Other root agent files exist only to load it for tools that require a tool-specific filename.

`CLAUDE.md` is a tracked repository-relative symbolic link to `AGENTS.md`, so
Claude Code and other readers receive the canonical rules directly. `GEMINI.md`
loads `@AGENTS.md`, spelled in that exact case because imports resolve literally
on a case-sensitive filesystem, and exists only while Gemini CLI is part of the
workflow.

Root instruction paths are tracked repository entries. The `AGENTS.md` body
lives in one place; `CLAUDE.md` resolves to that body, and loader files reference
it without copying doctrine that can drift.

## Hard rules

These rules are enforceable. Later sections explain mechanism or rationale and never weaken or contradict them.

### Generating principles

Seven principles generate the rules in this file. A case no rule names resolves by the nearest principle.

1. Durable mechanism identity: every durable artifact -- name, comment, claim, citation -- carries mechanism or content identity; chronology, actors, and process ride in commits, findings, and registry metadata.
2. Indicative voice: rules, comments, and reports state what an artifact is and does, present tense, artifact as subject. A boundary takes its positive dual: the restriction (`release builds only`), the named home (`chronology lives in the commit message`), or the mechanism itself (`CP DMA prefetches run fire-and-forget, and the wait consolidates at end-of-IB`; `CPU-only`; `this descriptor shape resolves only through the texture path`). The positive form entails the absence a negation would state, so the absence stays off the page. A style rule models this: it names the antipattern by the shape of its construct -- binary contrast, stacked absence, apparent negation -- and shows the positive target, holding the dispreferred specimen out of its own body. A hard-stop safety or security boundary keeps its prohibition, where that is the whole content.
3. Authority by name and rank: a load-bearing claim binds to a named source at the highest available evidence rank; provenance detail rides in the commit message and the finding.
4. Evidence-class separation: known, hypothesized, and speculative stay marked; build, runtime, conformance, and silicon stay distinct; prediction precedes observation, and deviation is the finding.
5. Single home per fact: each fact keeps one canonical location, and other sites point to it; a hard-rule digest line plus one expansion section are the two permitted homes.
6. Smallest complete mechanism: a change, comment, or tool carries exactly its distinct load-bearing facts -- complete, and free of stubs, decoration, and repetition.
7. Fail-closed gates: hazardous paths open on exact opt-in values only; unset, empty, and zero stay closed; a verdict-producer earns trust by calibration on known-good and known-bad inputs first.

### Boundary, paths, and instruction files

- Mesa normal flows -- build, install, test, source-comment, upstream intake, and owned-origin publication -- stand complete inside this repository. `steinmarder` supplies evidence only; its bundles and findings live in `steinmarder`, and Mesa driver changes live in Mesa.
- Paths in checked-in work are repository-relative, generated native files, PATH-resolved tools, or explicit user roots; discover the repository root with `repo_root=$(git rev-parse --show-toplevel)`.
- Local absolute paths, private host FQDNs, per-user toolchains, raw IP literals, and worktree names are workspace-local facts and live outside the tree.
- Instruction paths are tracked repository entries; `CLAUDE.md` is the
  repository-relative symbolic link to `AGENTS.md`, loaders reference the
  canonical body, and doctrine lives in `AGENTS.md` alone.

### Root cause, evidence, and conformance

- A behavior change names the exact driver path, chip, test, spec rule, and kernel or Mesa mechanism first.
- PCI IDs, ISA and register sources, and measurements identify silicon; PALM/Wrestler family nicknames serve prose only.
- Root cause comes from primary sources before opinion: ISA section, kernel function, spec paragraph, test oracle.
- Findings and code comments mark known, hypothesized, and speculative claims distinctly.
- A code claim carries its symbol-discovery method with the location: `(clangd: textDocument/references on FUNC)`, `(global -r SYMBOL)`, `(ast-grep --pattern PATTERN)`, `(rg --fixed-strings SYMBOL src/)`.
- A hardware-RCA or conformance fix records the observation, source or spec constraint, implementation hypothesis, falsifier, validation command or retained bundle path, and expected CTS/Piglit/deqp movement before the change.
- GPU-behavior analysis starts from a `dmesg` check for DRM CS validation errors.
- The RS482 / K8 / SB600 Vostro 1000 target needs out-of-tree kernel modules (radeon GPU-reset + hazard mitigation, SB600 watchdog, EC thermal); `docs/hardware/vostro1000-kernel-modules.md` is the registry of what each does and why the hardware needs it.
- Crash symbolization starts from module reachability via `/proc/PID/maps` or `gdb info sharedlibrary`.
- Build, runtime, conformance, and silicon stay separate evidence classes.
- A CTS/Piglit/deqp fix claim rests on test evidence; build-only evidence proves a build.
- A recorded prediction stands after observation; a probe runs once and its result stands; a workaround names its spec-conformance cost.

### Builds, tests, and verdicts

- Warnings and unexpected tool, CTS, Piglit, or deqp output are defects until explained.
- Unexpected results surface immediately.
- Touched code builds cleanly under the configured warning flags and adds no warnings.
- The report records what was built, tested, skipped, blocked, or unavailable.
- An unrun test reads `not run` with its reason.
- A new probe, lint, or verdict-producing script earns trust by calibration against known-good and known-bad inputs first.
- Build targets, tests, and validation checks survive a narrow fix.
- A generated-file change runs the generator, or documents why the generator is unavailable.
- A decision-grade result comes from a clean tree at the declared SHA: `git status --porcelain=v2` empty, `git diff HEAD` and `git diff --cached HEAD` empty, and `HEAD` equal to the declared source SHA, in an isolated worktree for qualification runs.

These rules expand in `build-infra/CLAUDE.md` and `Validation expectations`.

### Languages and scripts

- Each translation unit keeps its existing language and configured standard.
- C translation units stay at C11 or newer.
- C++ backends stay at C++11 or newer, at or above the configured standard.
- C11 and C++11 are floors; later standards are open.
- One translation unit holds one language; C and C++ stay separate.
- Python tooling targets CPython 3.12 through 3.14 inclusive, and avoids APIs deprecated for removal after 3.14 where practical.
- Shell scripts are POSIX `sh`; a script that requires `bash` declares it.

### Build orchestration

- `build-infra/CLAUDE.md` carries the build and cache doctrine, and a change to a Meson option, native file, host env, install prefix, build directory, or cache lane reads it first.
- Meson plus Make both stand. Meson owns configuration and Ninja generation. Make and build-infra own host selection, audit checks, generated native overlays, clean, build, and install.
- Build audits model Meson defaults: for an omitted or `auto` option, audit the dependencies Meson enables on the target host. An absent or `auto` option resolves to what Meson will do, not to disabled.
- Hazard gates take an exact opt-in value such as `R300_TRACE_HAZARD_ACCEPTED=1`. Unset, empty, and zero-valued gates stay closed, and `getenv()` presence alone is not consent.
- distcc/ccache integration uses Meson `[binaries]` plus `CCACHE_PREFIX=distcc`. A shell wrapper chaining `ccache distcc compiler` stays retired, as does any C/C++ distcc-pump lane that puts `ccache` or `sccache` before distcc-pump.
- Meson Rust ignores `RUSTC_WRAPPER`; a Meson native file names the Rust toolchain by a stable path, never a hardcoded `~/.rustup/toolchains/.../bin/rustc`.
- Compiler selection, audit policy, clean, build, install, and hazard consent live in Make and build-infra; standalone helper scripts for them stay out.

### Git, merge, and owned-origin publication

- Branches, commit subjects, PR titles, source comments, finding filenames, and bundle directories carry durable mechanism names. The branch name, first commit subject, and PR title are set before first push. Load-bearing identity comes from mechanism and content; wave, phase, mission, session, PR, reviewer, agent, and worktree labels live in registry metadata at most.
- Merges preserve all non-refuted content; the default resolution is union plus synthesis. `git merge -X theirs`, `git checkout --theirs`, blanket conflict-marker stripping such as `sed -i '/^<<</d; ...'`, and unreviewed deletion are not synthesis.
- A force-push to `main` or a shared branch carries explicit user sign-off and a commit message explaining why.
- A skipped pre-commit hook is an emergency move, with the reason in the commit message.
- `upstream/main` integrates into fork `main` through an intentional rebase; `git push origin upstream/main:main` stays out.
- `origin/main` is the owned publication target. Codex and compatible agents execute branch creation, implementation, validation, commit, push, PR review, merge, main synchronization, and scoped branch or worktree cleanup for authorized work.
- External remotes are intake-only. A push or submission to freedesktop.org or another external project requires separate explicit user authority.

### AI disclosure, authorship, and copyright

- When policy requires disclosure, the commit carries the Mesa `Assisted-by:` trailer, or `Generated-by:` when AI generated almost the entire change.
- `Co-authored-by:` names human co-authors only.
- Source headers carry copyright and license content only; AI disclosure such as `(LLM-assisted)` or `Generated by Claude` lives in commit trailers.
- Historical pre-policy `Co-Authored-By: Claude` trailers stand; a force-push to scrub them stays out.
- Upstream copyright headers stay verbatim through file movement, author name and year intact, with no second project-collective line above them.
- A new source file header carries its SPDX license grant and no copyright
  line. An existing copyright line remains only when it names a real holder;
  fabricated personal names, LLM-default names, and invented project
  collectives are template output and get stripped.

### Comments, prose, and safety

- A source comment stands on its own for a Mesa maintainer six months later, without this project's task tracker or RCA campaign. It cites no internal GitHub/GitLab fork issue number, private PR chronology, wave label, task number, author tag, local path, private host, deictic time, or retained-experiment name, and it states a demonstrated property as the mechanism rather than by analogy to the experiment that demonstrated it.
- A comment carries content: a compact semantic table or diagram belongs where it maps a descriptor word, packet layout, bit field, or state transition more precisely than prose, and delimiter lines, banner boxes, and ASCII art are decoration. Structure that persists across a file lives at file or type scope, and the point of use keeps the local link.
- New or modified source comments, commit messages, and documentation by this project's contributors use American (United States) English spelling. A behavior patch leaves upstream comment spelling alone.
- A patch changes behavior or structure with intent: no mass reformat, no stub, placeholder, dead code, or `TODO: finish later` prose absent explicit tracked rationale and user agreement.
- Reports state results and decisions directly: the outcome, the decision, the evidence, and the residual uncertainty, each sentence carrying a result the reader uses. A claim states its positive term and names the mechanism, and the mechanism entails what the artifact lacks.
- A critical security or unsafe hardware-access defect stops normal feature work; contain and report it, then resume.
- Shared workspace paths stay intact; a destructive command such as `sudo rm -rf` on them stays out.

These rules expand in `Comments, commits, and Markdown`, `Driver code and patch style`, and `Security and hardware stop-line`.

## Workspace roots

The parent workspace has two durable source roots.

- `mesa-26-gororoba/` owns Mesa source: Terakan, r300g, r600g, NIR/compiler code, and Meson/build/install infrastructure.
- `steinmarder/` owns reverse-engineering runners, retained evidence, findings, manifests, host kits, safety policy, and cross-repo orchestration.

`mesa-26-gororoba-*` directories are temporary Git worktrees of
`mesa-26-gororoba/`, not separate projects. Worktree changes land through a
branch, review, and merge to `main`. A temporary worktree leaves service after
its branch is merged or superseded. Teardown follows this order:

- Verify that the checkout is clean and its HEAD is reachable from a pushed
  ref. Commit dirty or unreachable state to a branch named for its mechanism,
  or retain it as evidence in the `steinmarder` preservation corpus.
- Run `git worktree remove <path>` from the worktree's parent repository and
  confirm that the named checkout leaves `git worktree list --porcelain`.
- Remove the out-of-tree Meson build directory named after the checkout,
  transient patch snapshots or recovery copies, and scratch logs. Durable
  evidence in the `steinmarder` preservation corpus remains under its
  retention contract.

`git worktree prune` is repository-wide maintenance. Start with
`git worktree prune --dry-run --verbose`, classify every candidate, and prune
only after every reported administrative entry is approved for removal. A
worktree of another repository leaves service through its own parent
repository. A parent repository move orphans absolute worktree pointers, so
`git worktree repair` runs after the move.

Repo contents keep their home roots: steinmarder evidence bundles and findings live in `steinmarder/`, and Mesa driver changes live in Mesa. A Mesa fix may use steinmarder evidence, but the code lands in Mesa. Cross-repo work travels through sibling checkouts and PRs; every file keeps its home repository.

## Project scope and priorities

`mesa-26-gororoba` is a Mesa 26.x fork that tracks upstream `main`. Local work includes Terakan Vulkan under `src/amd/terascale/vulkan/`, r600 SFN work, and related r300, r600, NIR, compiler, build, and install changes for Radeon HD 6310 PALM/Wrestler (`CHIP_PALM`, Evergreen, TeraScale-2, VLIW5). The primary host class is x130e / Bobcat / HD 6310 APU. The peer repository is `steinmarder/`.

Priority order is fixed: conformance, standards, stability, performance. Safety applies throughout. Earlier priorities override later priorities.

A fast workaround is not a fix when it breaks conformance. A non-conforming workaround may be considered only when its conformance cost, containment, and removal path are recorded.

Investigate before editing. Read source, specs, tests, logs, generated files, kernel paths, CTS/Piglit/deqp behavior, commit history, and retained evidence. Work in this order: scope the task, identify the component, split claims, collect primary evidence, model the mechanism, design the change, implement, verify, and record the result.

Leave code, comments, tests, and findings more accurate, reproducible, navigable, and source-grounded than the starting state.

## Agent operating rules

Language-model agents apply these rules before editing Terakan, r300, r600, NIR, Meson, tests, or comments. Mesa instructions stand alone for Mesa build, install, and review. `steinmarder/` provides evidence and runner context and does not replace Mesa rules.

### Operating stance

Work at hardware/software reverse-engineering depth: exact chips, exact paths, exact specs, exact tests, exact evidence. Conformance trades against speed or patch size only with the cost recorded.

Use the priority order from `Project scope and priorities` as a hard constraint. Label any non-conforming workaround with its conformance cost, containment, and removal path.

Treat warnings as defects. Hardcoded shortcuts, unrelated symlinks, and local
FQDNs stay out. The root `CLAUDE.md` instruction alias is the explicit symlink
that keeps Claude Code policy identical to `AGENTS.md`. Keep documentation
precise enough for later maintainers to verify.

### Work sequence

Work inside the real repository. Read code, docs, logs, tests, history, generated files, and prior evidence before reaching a conclusion. Treat memory and generated summaries as leads, not authority.

Use an ordered task tree: research before editing, model before designing, verify before claiming completion. Each tool invocation must answer a named question or move implementation forward.

Research from primary sources when possible: Mesa source, Linux kernel source, Khronos Vulkan/OpenGL specs, AMD ISA/register manuals, CTS/Piglit/deqp tests, and retained steinmarder evidence.

Implement complete, robust changes. Stubs, placeholders, dead code, and `TODO later` prose enter only with explicit tracked rationale. When blocked, trace the root cause through the interacting layers instead of choosing a shortcut.

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

Report the evidence chain, alternatives, tradeoffs, and next evidence needed. Treat surprise as a finding; a silent pivot buries it.

### Reasoning checks

Use these checks during RCA, conformance, and hardware-facing changes.

- Silicon-to-test chain: trace data and control flow from hardware register, bitfield, field definition, driver API, and command emission to the CTS/Piglit/deqp assertion before diagnosing.
- Hypothesis tree: list plausible root causes, rank them by evidence cost and likelihood, test the cheapest decisive case first, and prune only on falsification.
- Opposition review: after forming a synthesis, argue the strongest contrary case. If the contrary case survives, keep the finding unresolved and name the next evidence needed.
- Claim audit: before committing a finding, list each implementation claim and the rank-1 through rank-6 source that backs it.

## Driver code and patch style

Driver code exposes the domain directly: API object, chip generation, register field, command packet, memory lifetime, synchronization edge, format table, and error path. Names, structs, data tables, assertions, and cleanup labels carry most of the explanation. Comments carry only constraints that would otherwise be lost; comment doctrine lives in `Comments, commits, and Markdown`.

Use direct, concrete names. Public entry points carry the subsystem prefix. Local helpers use mechanism verbs such as `emit`, `lower`, `fill`, `find`, `validate`, `lock`, `unlock`, `init`, `finish`, and `destroy`. Constants, registers, packet fields, and enum translations keep domain names. State objects, key structs, and descriptor words are typed driver state, not loose parameter groups.

Use the shape required by the mechanism. Prefer a helper when the operation has a name. Prefer a data table when cases form a finite map. A hardware/API matrix keeps the shape that keeps its invariant visible; invariant visibility alone decides a split. A long function is acceptable only when it remains locally auditable: feature bits, packet words, ioctl validation, resource lifetime, ordered emission, and unwind edges stay visible, and every exit path can be checked.

Treat error paths as ownership topology. Labels such as `fail`, `unsupported`, `err_*`, `out`, `put`, `free`, and retry labels show which object is live, which invariant failed, and which cleanup edge runs next. Follow the subsystem ABI: `VkResult` and `vk_error` in Vulkan paths; negative errno and WARN/assert fences in kernel-shaped paths. Assertions guard impossible internal states. External input is validated and rejected.

Use data tables for finite maps: formats, register fields, enum translations, chip gates, descriptor words, packet layouts, and workaround selectors. Preserve distinctions when cases differ materially. Abstraction is valid only while it preserves the chip, ABI, spec, and evidence boundary that made the case exist.

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

Phase, wave, and chronology terms may appear only as secondary registry metadata, such as `phase: 1E-atomic` in finding YAML; the metadata field is their one home.

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

- Evidence climbs through the 2D and 4D levels, which eliminate cheap wrong hypotheses, before reaching 8D.
- An 8D finding carries claim, evidence chain, and falsification criterion.
- 16D and 32D analysis surfaces after lower levels are stable, or alongside a named remaining uncertainty.
- A 16D synthesis tests the interactions; separately passing facts do not carry it.
- A 32D architecture decision waits for stable lower levels or an explicit unresolved uncertainty.
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

PALM hardware-RCA edits follow `Falsification record` and the hard-rule `dmesg` DRM CS and module-reachability checks.

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

Hardware and API citations name public documents and sections. Internal extracts, retained bundle paths, and audit artifacts are evidence, not citation authority.

- Wrong: `per Evergreen_ISA.txt:17572`. Right: `per AMD Evergreen-Family ISA, section 10.x.x (MEM_RD_SCATTER)`.
- Wrong: `see phase5_isa_pdf_audit_20260418T182628Z/...`. Right: `per AMD Radeon HD 6000-Series ISA (Cayman), section X.Y`.
- Right: `per AMD 3D Engine Programming Guide for Evergreen, section M (CB_COLOR0_VIEW)`.
- Right: `per Direct3D 11.3 Functional Specification, section 4.4.6 Element Alignment`.

Hardware-specific source comments carry the load-bearing hardware facts: bitfield encoding such as `SLICE_START bits 0-10 of CB_COLOR_VIEW`, empirical behavior such as `Palm silently no-ops MEM_RAT_CMPXCHG_INT on the cached path`, and any mathematical invariant or non-obvious workaround rationale needed to understand the code.

## GPU driver and reverse-engineering vocabulary

Use mechanism terms before summary terms. A Mesa claim names the affected path: compiler lowering, descriptor construction, packet emission, kernel validation, resource lifetime, memory/cache behavior, runtime loader state, or conformance result.

Use these terms by path:

- Silicon identity: PCI ID, ASIC family, Mesa chip enum, IP block, generation, stepping, feature bit, engine, ring, aperture.
- Compiler path: NIR, TGSI, SPIR-V input, lowering pass, legalization, instruction selection, register allocation, scheduling, backend emission, disassembly oracle.
- Descriptor/resource path: BO, descriptor word, reloc-adjusted VA, pitch, tiling, swizzle, cache policy, coherency domain, map/unmap boundary, lifetime rule.
- Command stream: PM4 packet, indirect buffer, packet grammar, register write, draw or dispatch boundary, relocation, CS validator, fence, sequence number.
- Kernel interface: DRM UAPI, ioctl path, GEM, TTM, radeon object, CS parse path, fence wait, reset path, debugfs path, KMS interaction, dmesg validation error.
- Runtime path: ICD or DRI loader choice, dispatch table, winsys, screen/context/resource object, Gallium pipe state, Vulkan object lifetime, debug/release contamination.
- Evidence: CTS/Piglit/deqp result, dmesg delta, shader disassembly, packet decode, retained bundle, calibrated probe, golden trace, known-good/known-bad oracle.
- Upstream surface: minimal patch surface, bisectability, conformance delta, reviewer burden, ABI/install impact, backport risk, maintenance owner.

A broad claim such as `the GPU supports X` or `the driver supports X` stands only with its evidence class named. Use bounded claims:

```text
Known: PALM accepts this packet sequence through the radeon CS validator, and the retained CTS run observes the expected output.

Hypothesis: this is a descriptor-word construction bug, not a silicon-capability claim.

Speculative: adjacent Evergreen behavior suggests the same cache-domain rule, but exact PALM evidence is not yet decision-grade.
```

Use `breakthrough` only for a discontinuous result that changes the evidence graph. For normal Mesa work, use the mechanism: `driver enablement`, `conformance improvement`, `lowering-path correction`, `descriptor-path repair`, `packet grammar recovery`, `hazard-model refinement`, `silicon-behavior characterization`, `validation-methodology improvement`, or `source-grounded architecture model`.

## Headless hardware GL

`headless-hardware-gl-runner.md` carries the procedure for reaching a hardware GL
provider on a display-manager host: the render-node EGL/GBM path that needs no X
server, the headless glamor X path for tests requiring a real default framebuffer,
and the provider verification that separates a hardware result from an llvmpipe
one. A `DRISWRAST` provider or an `llvmpipe` renderer string means the run
measured software, whatever driver was under test.

## Build and cache doctrine

`build-infra/CLAUDE.md` carries the standalone build, release and debug
separation, build profiles and host envs, build directories and install
prefixes, clean and reconfigure, and the distcc/ccache wiring. Meson owns
configuration and Ninja generation; Make and build-infra own host selection,
audit checks, generated native overlays, clean, build, and install. Changing
that split takes explicit approval for a build-system architecture change. Read
`build-infra/CLAUDE.md` before touching a Meson option, a native file, a host
env, an install prefix, or a cache lane.

## Owned-origin publication and upstream intake

`origin` names the owned fork, and `origin/main` is its publication target.
`upstream` names the fetch-only freedesktop.org Mesa source (`mesa/mesa`).
Owned-origin publication writes only to this repository's `origin`. Upstream
intake fetches and deliberately integrates source from freedesktop.org.
External upstream publication writes or submits to a repository outside the
owned origin.

`upstream/main` reaches fork `main` only through an intentional rebase, with any divergence recorded; a direct `git push origin upstream/main:main` stays out.

An authorized change flows through a mechanism-named branch, local validation,
an intentional commit, a push to `origin`, a pull request targeting
`origin/main`, resolution of current review findings, merge,
canonical-main synchronization, and scoped branch or worktree cleanup. Codex
and compatible agents complete that flow end-to-end.

External upstream publication remains outside this project's normal workflow.
A separate explicit user authorization names the external destination, scope,
credentials, and review boundary before any push or submission.

Use `docs/submittingpatches.rst` on the mesa-26 branch as a patch-construction
and review-quality baseline. `origin/main` remains the publication target.
Patches keep behavior separate from formatting churn, affect one component
when possible, keep builds green, remain bisectable, are tested prudently,
and arrive without fixup commits for review.

Keep fork-local Terakan, R3V, r300, and r600 changes reviewable by mechanism,
not batch size.

Commit subjects use a component prefix and a concise mechanism. Commit bodies name the invariant or bug, describe the change at maintainer-review depth, cite evidence, and state tests without turning the message into a template.

Use `Closes:` for GitLab issue URLs. Use `Fixes:` only for the earlier commit that introduced the defect. Use `Backport-to:` or the current Mesa stable marker only when appropriate.

### AI-assistance trailers

Mesa reserves `Co-authored-by:` for human co-authors.

List only tools used for the commit:

```text
Assisted-by: Claude (Opus/Sonnet 4.x), ChatGPT Codex (5.x), Gemini (Flash/Pro 3.x), Mistral, Ollama, DeepSeek
```

Use `Assisted-by:` for mixed human/AI work. Use `Generated-by:` when AI generated almost the entire change. Trivial or mechanical changes may omit disclosure when Mesa policy allows omission. Disclosure belongs in commit trailers only.

Historical pre-policy commits with `Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>` remain historical artifacts; history stands. New commits use `Assisted-by:` or `Generated-by:`.

Checked-in patch snapshots may retain original trailers: `src/re/r600/results/.../*.patch`, kernel-module patch series under `src/amd/...`, and embedded DKMS sources. They are snapshots; trailer policy alone rewrites none of them.

### File headers, copyright, and SPDX

Use `docs/submittingpatches.rst` on the mesa-26 branch for Mesa header rules. Per-file headers are optional; many Mesa files use SPDX-only headers with no copyright line.

New source files carry the SPDX license grant and no copyright line:

```c
/* SPDX-License-Identifier: MIT */
```

With a description, use the block form:

```c
/*
 * SPDX-License-Identifier: MIT
 *
 * <one-line file description>
 */
```

A copyright line asserts authorship, so it appears only when it names a real holder who actually holds it. An invented project collective, a personal name pulled from local git configuration, and the `Copyright (c) YYYY <git config user.name>` an LLM template defaults to all assert authorship that does not exist; strip them. Absent attribution is correct, and it is preferred over invented attribution.

A file header carries license content only; AI disclosure such as `(LLM-assisted)` or `Generated by Claude` lives in commit trailers.

Preserve existing upstream headers verbatim, including author name and year. This applies to Vitaliy "Triang3l" Kuzmin-authored Terakan files, Mesa-tree files from prior contributors, and any pre-existing header with a non-project author. The MIT license requires preserving copyright and permission notices in copied or substantial portions.

Upstream headers survive refactoring, splitting, and file moves verbatim. Added content joins an upstream file under the existing upstream header, with no second project-collective copyright line above it.

## Comments, commits, and Markdown

The code is the primary text. Comments explain mechanisms that are not obvious from the next line of code. A useful comment records a silicon constraint, spec rule, kernel validation rule, command-stream invariant, measured quirk, ABI boundary, synchronization rule, or the reason a workaround preserves conformance.

Follow local style first: `.editorconfig`, `.clang-format`, adjacent code, and repo-local build rules override generic taste. Mesa-shaped C/C++ uses 3-space indentation, no tabs, 78-column code where practical, short Doxygen-shaped function comments when useful, and comments that name the spec or source rule when that rule controls the code. Mass reformatting enters only as an explicitly requested formatting migration.

Use American (United States) English spelling in new or modified source comments, commit messages, and project-authored documentation: `honor`, `behavior`, `initialize`. This rule applies only to new or modified text. Upstream comments keep their spelling absent a substantive edit; churn-only spelling edits create unnecessary merge conflicts. Quoted spec text, kernel symbol names, diagnostics, and hardware identifiers stay verbatim. When a substantive edit already changes a comment line, the touched line may be aligned to American English; otherwise existing spelling stands.

Source comments cite public, durable authority; the unstable and private reference classes live in commits, findings, and trackers: task numbers, private issue numbers, PR numbers, companion-PR breadcrumbs, wave/phase/mission/session labels, worktree names, agent names, author tags, local absolute paths, private host FQDNs, raw private IPs, deictic time, dated claim/LI/Q tags, deictic chip names, internal-repo source paths, internal evidence paths, and retained-experiment vocabulary.

Retained-experiment vocabulary is the RCA campaign's own naming -- mutation arms, calibration arms, probe fixtures, capture cells, bundle identifiers -- and an analogy to such an experiment (`the same vacuity a mutation arm forcing an already-present value has`) is evidence chronology in disguise: the reader needs the campaign to parse it. A comment restates the property the experiment demonstrated as the mechanism itself, on the exact chip and path where it holds (`a seed equal to the value leaves an unwritten register unsatisfied, so a stream passes only when it establishes every value itself`); the experiment that discovered it lives in the commit message and the finding. Test-local API terms the file itself defines (`poison`, a parameter of the checker under test) are the code's own vocabulary and stand.

Source comments name durable mechanisms: exact chip, ISA/register rule, API/spec rule, kernel validator, test class, or measured behavior.

Commit messages and finding documents may carry chronology and the spec section number when useful. A source comment states the mechanism or names the controlling rule in its own terms, so a reader does not need the spec open to parse it. A stable spec section or version may trail the named rule as supplemental disambiguation; the named rule itself carries the authority, and chronology lives in the commit message and the finding, as does section-number provenance. Like the American-spelling rule, this governs new or modified comments only; existing comments keep their section numbers rather than absorb churn.

Mechanism controls comment length: the number of distinct load-bearing facts sets the length, and a line threshold does not. Use short labels for obvious local sections, and compact connected blocks when the code depends on hardware behavior, API rules, kernel validation, empirical evidence, or non-local invariants. A comment may carry several independently checkable facts when they form one causal, temporal, conditional, or ownership chain; independent checkability belongs to the claims, not to sentence boundaries. Split when the actor, ownership, phase, evidence class, or invariant changes, or when added syntax obscures the dependency it is meant to expose. Default to the shortest form that preserves the mechanism: a single sentence for one local fact, a causal sentence for a connected relation, and a short block when the model spans an API rule, a lowering pass, allocation granularity, kernel validation, and an observed failure. Remove repetition and paraphrase at any length. Architecture that persists across the file moves to file or type scope (see the descriptor-layout preamble in `si_descriptors.c`); the point of use keeps only the local link in the chain. Mesa-upstream blocks for a single constraint (see `si_buffer.c`, `evergreen_state.c`) are typically four to five lines; use them to calibrate how much space a single fact deserves.

A comment runs from its first useful sentence to its last. A compact semantic table or diagram that encodes a descriptor word layout, packet fields, a bit layout, or a state transition is content, and it belongs when it carries the mapping more precisely than prose (the image/sampler descriptor table at the top of `si_descriptors.c` is the calibration example); delimiter lines, banner boxes, ASCII art, long punctuation runs, and wrappers such as `/* ----- */`, `// =====`, and `/* --- label --- */` are decoration.

When extending Triang3l-authored Terakan files, match the file cadence: shorter line lengths, fewer subclauses, and comments only when they carry silicon, spec, or test information.

Commit messages and PR titles are mechanism-named and component-prefixed when project style expects it. The body makes the invariant, change, and evidence reviewable in one to five sentences: name the root cause or constraint, name the fix, cite the spec section, register macro, or kernel function when load-bearing, and state any test movement plainly. `Fixes:`, backport notes, AI disclosure, and review trailers stay where Mesa expects them.

Keep the commit body to mechanism, matching the Mesa-upstream norm (e.g. `radeonsi: fix conformance window emission in the SPS` -- two sentences of cause and one example). Build invocations, profile names, tool output, host names, and `Validation:` checklists live in the PR description. A `Note:` about pre-existing CI noise the patch did not cause belongs in the PR description, when it needs a home at all. The subject carries the component prefix and mechanism only; the `Part-of:` trailer carries the PR or MR link (the `(#NNN)` suffix an author or a model appends to a subject moves there). A squash-merge button that appends `(#NNN)` to the merged subject is a forge artifact rather than an author artifact, so it refutes no commit and justifies no history rewrite; passing an explicit merge subject keeps the suffix off the subject line. A body that reads like a worklog -- nested `*` bullets from a coarse squash, several sub-components -- means the commits were not granular enough: split them or compress to the aggregate mechanism. Commit prose is emoji-free mechanism text.

Formatting churn and logic changes ride separate commits. Each commit stays buildable, reviewable, and bisectable unless a stated migration plan says otherwise.

### Comment class and placement

Four decisions generate a comment: semantic role, evidence class, language form, and placement. The role fixes which facts the comment must carry; an API contract, a cross-layer invariant, a one-line register fact, a descriptor table, a silicon workaround, and an unsafe-code proof have different information shapes and do not share one template.

- Semantic role: contract (API behavior, ownership, lifetime, error conditions), translation (API-to-register or API-to-packet mapping), local invariant (register field, format, equation), hardware quirk or workaround, representation (descriptor word, packet layout, state transition), safety proof, or file-scope navigation.
- Evidence class: documented behavior uses the plain indicative; reproduced-but-undocumented behavior names the generation and path where it was observed; conjecture and unfalsifiable claims are marked (`seems to`, `appears to`) or removed.
- Language form: Mesa C/C++ line or block comment, Doxygen only where the file already uses it, field annotation, compact semantic table, or the Rust forms below.
- Placement: architecture that persists across a file lives at file or type scope; the point of use carries only the local link in the chain (skip condition, bound state, offset rule); a branch comment states what distinguishes the branch, at the branch.

Rust code follows the rusticl taxonomy: `//!` for module contracts, `///` for public items and fields, `# Safety` / `# Panics` / `# Errors` sections when those obligations exist, and `// SAFETY:` immediately before each unsafe operation. A `// SAFETY:` comment proves every precondition the operation relies on -- lifetime, ownership, aliasing, and synchronization -- and a bare non-nullness claim (`the pointer is valid`) is an incomplete proof:

```rust
// SAFETY: `self.pipe` is non-null for the lifetime of `self`, and the raw
// `pipe_context` retains the `screen` pointer until `PipeContext::drop`.
```

AI disclosure lives in commit trailers (`Assisted-by:`, `Generated-by:`) alone.

### Stating mechanism as fact

State what a thing is and does, in positive declarative form. Name the mechanism and let the binding constraint stand as fact: `the vbuf stage maps the vertex buffer after every allocate_vertices, so the draw is dropped at submission`. The comment carries the mechanism and the constraint that makes it hold; correctness follows from the mechanism, so the reviewer assumes it and correctness claims and contrast framing fall away. A boundary takes its positive dual: the restriction (`release builds only`), the named home (`chronology lives in the commit message`), or the mechanism itself. A stacked absence collapses to the positive fact its members share -- `a producer that fits delivers in one pass` -- and the reader receives the mechanism instead of reconstructing it from its complement. An apparent negation names a mechanism, so write the mechanism: `CP DMA prefetches run fire-and-forget, and the wait consolidates at end-of-IB`; `this descriptor shape resolves only through the texture path`; `the caller retains the allocation`; `radeon's sync is implicit dma_resv only`. Each positive form entails the absence a negation would state, so the specimen stays off the page. This section names the antipattern by construct-shape and shows the positive target, holding to the voice it defines. A hard-stop safety or security boundary keeps its prohibition, where that is the whole content. Historical design debate about a rejected alternative belongs in the commit message; the rejected alternative enters the source only when rejecting it is itself the conformance, lifetime, or safety invariant the code enforces (`aliasing these descriptor shapes reads past the bound range, so the pipeline rejects them`). `correct-or-reject` and similar named postures stay as terms.

Write third-person present tense: `the kernel reads WORD0`, `the TX unit ignores NEAREST for integer formats`. Impersonal `we` for the code path (`we do not have to wait at the end of an IB`) matches Mesa upstream, and the code path is the only referent `we` takes; the project and team appear in commits and findings. Ceremonial prose falls away; when terse prose hides the mechanism, the missing invariant is the fix.

For a silicon bug or workaround, name the affected chip or register, state the observable failure in one sentence, and cite a bug URL or ISA section when public. A workaround comment separates what was observed, on which generation and path, what the code enforces, and what remains hypothetical; a block that fuses observation, enforcement, and speculation splits along those lines.

Uncertainty in a comment names the mechanism and the guarded, disabled, or falsifiable path, and stands free of review chronology, private context, and session state.

### Source comment shape

Use these examples as style anchors: the WORD0 fix block in `src/amd/terascale/vulkan/terakan_dispatch.c` near `PKT3_SET_RESOURCE` and `desc[0] - bo->va`, and the inline silicon notes in `terakan_format.c` and `sfn_instr_mem.cpp`.

A full mechanism comment orders its facts:

1. Load-bearing claim: `WORD0 carries the per-BO byte offset.`
2. Public or source authority by name: `evergreen_packet3_check`, AMD ISA chapter, register macro, or the named rule. A stable spec section may trail the named rule as supplemental disambiguation; the named rule carries the authority.
3. Consequence, with an inline code fragment when clearer than prose: `ib[WORD0] = reloc->gpu_offset + offset`.
4. Test reference when the comment explains a fixed failure: CTS case or `dEQP-VK.<group>.*`.
5. Env knobs or flags, grouped at the end of the block when relevant.

Use the smallest applicable subset of that order; most comments carry one or two of the elements, and only a comment explaining a fixed conformance failure or a gated path carries all five. The order is a dependency order -- claim, then the cause or authority it rests on, then consequence, then scope or guard -- rather than a sentence template. Bind facts that describe one mechanism into the same movement so the reader sees why the consequence follows from the constraint. A comment carrying one fact may be one sentence; a connected mechanism may require a causal sentence or a short block.

### Mechanism movement

The unit of composition is a mechanism movement, not a sentence. A movement names the governing object or operation, carries it through the constraint that shapes the code, and arrives at the consequence or guard that makes the constraint useful. Dependent clauses, participial phrases, appositives, and causal or temporal connectives preserve those relations while keeping each load-bearing claim exact. Repeated subject-proposition openings mark a new movement when ownership or scope changes; continuous mechanisms carry the established subject through their dependent and coordinated clauses. Paratactic rhythm belongs to finite maps and strict ordered protocols when separateness or order is itself the invariant; related facts use connected prose.

Generate comments from the mechanism model, not from a list of extracted facts: identify the subject, state, constraint, transition, consequence, and scope, then linearize their dependency graph into one movement. A movement can relate documented behavior to a measured consequence, but it names each evidence class where it enters rather than turning the consequence into proof of the mechanism. Hypothesis remains marked as hypothesis.

Default to short comments. A one-line trailing comment on the load-bearing line is better than a function-header paragraph unless the whole function encodes a non-obvious invariant.

Keep each comment block centered on one mechanism movement. A movement may contain several sentences when each advances the same state, ownership relation, or causal model. Start a separate comment when the mechanism or ownership changes, or when a phase or evidence boundary requires independent treatment; use a sentence boundary for a new dependency or for clarity, not merely because another fact is independently checkable.

The code itself says what happens; a comment explains why the code has that shape: silicon constraint, spec rule, kernel validator, measurement, or non-local invariant.

Comments earn space by carrying information beyond what the code alone holds; mechanical code reads bare.

Name citations by concrete authority: `SQ_TEX_RESOURCE_WORD4.DST_SEL_X`, the named AMD Evergreen-Family ISA rule, the named Vulkan spec rule, or the kernel function. Internal line numbers and private paths are evidence locators, and locators live in the commit message and the finding. A stable spec section may follow the named rule as disambiguation.

Use active voice. Prefer a causal connective when one fact forces another:

```text
CP DMA prefetches into L2 run fire-and-forget, so the command stream keeps
emitting after issuing the prefetch; the one wait lands at end-of-IB before
the stream retires.
```

Use explicit sequence when order itself is the mechanism, but carry the sequence through causal or temporal linking; a new sentence marks a distinct phase or decision rather than resetting the subject after every step. Connected sequence and causal prose beat imperative narration (`make sure to wait before returning`).

Example:

```text
The kernel reads WORD0 as a byte offset, adds reloc->gpu_offset, and presents
the buffer base to the shader at the intended VA; any per-element offset
requested by the caller survives relocation.
```

Multi-paragraph comment blocks are reserved for genuine silicon-quirk reasoning.

### TODO comments

Rule files are not TODO trackers; real future-work TODOs live in the source file at the affected mechanism, and the examples in this section describe comment shape only. A deferred-work comment opens with `TODO:`, `FIXME:`, `XXX:`, or `HACK:`, and a new marker comes from that four-item set; the no-placeholder patch rule governs new work, and existing `PLACEHOLDER:` markers are evidence-bearing artifacts on the same footing as the other markers.

A TODO-family comment names three mechanism elements:

- missing work: function, register, ISA section, kernel symbol, or spec chapter that needs the change;
- deferral reason: silicon, ABI, or evidence constraint blocking completion now;
- tracking artifact: durable function name, register name, `gitlab.freedesktop.org` issue URL, spec chapter, or silicon-constraint name. When no external issue exists, the named function, register, or spec chapter itself is the tracking artifact.

A TODO-family comment carries mechanism only; reviewer breadcrumbs, PR-thread references, phase/wave/mission labels, AGENTS.md rule numbers, and deictic references such as `currently`, `previously`, `this driver`, and `our GPU` live in the commit message or PR description.

Required shape:

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

`TODO`, `FIXME`, `XXX`, `HACK`, and existing `PLACEHOLDER` comments are evidence-bearing artifacts. Changing or removing one starts from its local context, historical reason, architecture pressure, and testability. Every unresolved gap gets scoped when discovered: missed parameter sweep, symbolic mutation, undeveloped theory link, unvalidated hardware assumption, register ambiguity, command-stream uncertainty, ABI hazard, build-system fault, test gap, probe limitation, or undocumented dependency.

### Finding documents and agent-loaded Markdown

Finding documents may carry chronology: dated frontmatter, `last_verified`, `evidence_class`, dated filenames, and ordered predecessors. PR or task references pair with a durable identifier.

A reference names the durable identifier first and carries the tracker link
beside it: `landed in commit f230cb07db6 (terakan_buffer.c::terakan_CreateBuffer
size-zero guard); PR #34 / branch fix/w9-buffer-size-zero-guard for
cross-link`, and `see filed-finding
2026-05-15-induced-lockup-recovery-test-results.md (PR #41 if still open)`.

Markdown loaded by agents uses exactly one H1, heading depth no deeper than `###`, frontmatter on programmatically loaded files, language tags on code fences, exact cross-references, and rule text as direct positive-declarative statements.

Use tables only when columns carry independent comparison value. Prefer bullets for simple ownership, lookup, and rule lists.

Rule files carry present-tense declarative text with exact cross-references. Slice-loaded text stands without nearby context.

Checked-in text is emoji-free. An emoji carries no information its word does not, and it breaks greps, widens diffs, and renders as a box or a double-width cell wherever the glyph is missing. Typographic substitutes stay out on the same grounds: straight quotes over curly ones, `--` over an em dash, `...` over an ellipsis glyph. `--` closes up against both words (`word--word`, no surrounding spaces), matching em-dash typography; the American-spelling paragraph's churn rule applies here too, so existing text with spaced dashes keeps its spelling absent a substantive edit. Symbols that carry meaning stay in -- mathematical operators, Greek letters in ISA and equation text, arrows in state transitions, box-drawing in descriptor and packet diagrams, the degree and micro signs. A name keeps the spelling its owner uses, so accented characters in upstream author and copyright lines are preserved verbatim; `Copyright (c) 2024 Vitaliy Triang3l Kuzmin` and its Unicode copyright sign both stand as written.

### Comment-hygiene linter and Git hook

Comment-hygiene enforcement keeps a clean Mesa checkout independent of `steinmarder/`.

If the linter is mirrored or vendored into Mesa, wire it through the local pre-commit framework and treat it as a Mesa-side gate. If the only implementation is in `steinmarder/`, treat it as advisory until the Mesa-side mirror exists.

Advisory sibling invocation, when the checkout layout provides it:

```bash
../steinmarder/src/re/scripts/lint/comment_hygiene_lint.py --staged
```

A blocking Git hook in Mesa points at Mesa-resident tooling only: a hook that depends on an external checkout violates Mesa independence and turns comment hygiene into an environment accident.

New commits follow this policy even when no linter runs. The linter enforces part of the policy; it does not define it.

Existing in-flight PRs may keep breadcrumb comments; breadcrumb scrubbing alone justifies no force-push. New commits follow this policy.

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

Cite the repository's exact path spelling; an alias such as `steinmarder/src/re/r300/` exists only when that path exists on disk. If both `src/re/300/` and another r300-named path exist, cite the exact path used by the evidence.

Steinmarder evidence may be cited by durable bundle path, finding filename, public spec name, exact hardware identity, or commit SHA. Bundles, findings, host kits, and local orchestration files live in `steinmarder/`; Mesa carries the citation.

Mesa source comments prefer public specs, source symbols, DRM/Khronos names, freedesktop.org references, and exact hardware identity. Freedesktop.org GitLab issue URLs and issue numbers are allowed. Internal GitHub or GitLab issue numbers from this fork or from `steinmarder/` are not source-comment authority.

Empirical fork evidence belongs in findings or commit messages unless the source code needs the mechanism to be intelligible.

## Tooling for RCA and audits

Use the strongest available tool that matches the claim; a weaker text search yields when the claim requires a structural, indexed, flow-sensitive, binary, or empirical tool.

### Tool availability

A missing tool proves nothing about the code. Determine the package or install path, update the installation requirements document for the affected module, and record the validation command. When the tool cannot be installed in the current environment, record `not run` and why.

Tool requirement updates name:

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
artifacts live in the `steinmarder` reverse-engineering tree (`../steinmarder`).
This repo holds driver code, build infrastructure, and committed
tests; a probe that must persist goes to `steinmarder/src/re/r300` with its evidence
bundle.

Local invocation notes (CachyOS/Arch): `spatch` comes from coccinelle-bin;
BCC ships the `opensnoop` and `execsnoop` tools; run `ast-grep`, `lizard`, and
`weggli` through `PATH`. MSAN is not usable for Mesa: MemorySanitizer
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

Every new probe, lint, or verdict-producing script earns trust by calibration against known-good and known-bad inputs first.

When using Coccinelle, locate `spatch` with `command -v spatch`, record the installed package or path, and cite the semantic patch used. `spatch` output is citable only with the semantic patch and target path set.

### Agent coordination

Use at most three concurrent subagents. Subagents are read-only evidence collectors unless the user explicitly authorizes a different role; assign each a bounded task, input scope, expected output, and citation requirement. The parent agent owns synthesis, conflict resolution, implementation choices, and final claims.

Use small or local models for search, file location, summarization, and citation fan-out. Escalate to a larger model only for deep synthesis, hazardous decisions, hard-to-reverse changes, or cross-file mechanism reasoning. Record the escalation reason when it affects the work record.

Subagents collect evidence and the parent evaluates it; load-bearing implementation decisions, commit pushes, file deletion, build-configuration changes, and warning suppression stay with the parent.

### Retained tools and probes

Retained analysis tools, probes, linters, and verdict-producing scripts are real programs in the repository's native build or validation flow once required, not hardcoded demonstrations.

A retained tool carries:

- clear inputs and outputs;
- deterministic behavior where practical;
- known-good and known-bad calibration cases;
- documented dependencies;
- documented safety gates for hazardous hardware access;
- a validation command;
- a maintainer-readable failure mode.

A retained script that proves only one handpicked case enters the tree only as an explicitly documented temporary reproducer.

## Validation expectations

Minimum validation depends on the changed surface.

- Terakan source: targeted build plus relevant runtime or CTS when available.
- r300/r600 source: targeted build plus Piglit, CTS, deqp, or documented hardware gate.
- NIR, lowering, and compiler code: build plus shader/compiler tests and affected conformance tests.
- Meson and build files: clean setup or reconfigure plus Ninja target and artifact/install check.
- Generated files: run the generator or document why unavailable; verify the generated diff.
- Source comments and docs: comment hygiene, Markdown structure, and source-reference audit.
- Scripts: shellcheck when applicable plus known-good and known-bad paths.

A pass claim rests on a run; an unrun test reads `not run` with its reason. A test blocked by hardware safety names the required gate. CTS/Piglit/deqp movement that differs from prediction is evidence.

A decision-grade build or test result binds to one committed state. Ninja builds the working tree, so uncommitted edits outside the tested commit's diff enter the binaries and the result stops describing the declared SHA; a shared checkout carrying a peer's in-flight edits fails this silently. Before a qualification, conformance-baseline, or merge-gating run: `git status --porcelain=v2` reports nothing, `git diff HEAD` and `git diff --cached HEAD` are empty, and `HEAD` equals the declared source SHA. Qualification runs use an isolated worktree (`git worktree add --detach`), which satisfies the check by construction and leaves the shared checkout's state out of the evidence.

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

Untrusted input passes allow-lists, normalization, and containment checks before any shell or path use; `sh -c`, `bash -c`, `eval`, generated shell fragments, and path concatenation take vetted values only. Hazardous paths open on exact opt-in gates. Logs carry redactions where secrets and raw tokens would appear.

## Synthesis over selection

When merging parallel branches or review findings, preserve all non-refuted content. Mechanism and evidence decide; wave, phase, chronology, branch age, and author do not.

Default additive resolution is union plus synthesis. Selection is allowed only when the discarded side is empirically refuted by tier-1 through tier-3 evidence or genuinely superseded by a verified line-level diff and recorded rationale.

Merge actions:

- Analyze: identify what each side contributes before editing. Every differing line is a candidate for preservation.
- Reconcile: preserve non-refuted content. Selection requires proof of refutation or supersession.
- Resolve: finish the merge; leave no half-merged state. Record outstanding items explicitly when a session cannot finish.
- Expand: connect findings, code paths, and tests; a side-by-side paste without the relationship explained connects nothing.
- Harmonize: use one durable mechanism name for the same thing across the synthesized artifact.
- Infuse: add the check, citation, rule, lint, or test that prevents the same failure class.

A synthesis adds value: unified model, terminology map, cross-reference, stronger rule, validation matrix, refined evidence tier, retained test, or clearer upstream path. A paste of A next to B is not synthesis.

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

A targeted fix for issue A leaves unrelated behavior B intact. After changes to scripts, runners, build files, lowering passes, descriptor paths, or comments:

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

## Key subsystems

Gallium drivers and the shared compiler code sit where the standard
Mesa layout puts them, under `src/gallium/` and `src/compiler/`; for
those the source tree is the map. The two Vulkan drivers live outside
the Gallium layout, under `src/amd/`, so each one gets named here:
Terakan at `src/amd/terascale/vulkan/`, R3V at
`src/amd/r300/vulkan/`. Build entry spans the repository-root
`meson.build`, `meson.options`, and `meson_options.txt` plus
`build-infra/`, native files, and install scripts.

`rust-toolchain.toml` is an upstream Mesa file with `channel = "nightly"`. Active builds select Rust through Meson and toolchain policy, not checked-in absolute paths.

This root file carries repository-wide doctrine; a rule too narrow for it lives in a lane README or path-scoped agent doc near its subsystem, with a pointer here.

## Engineering foundations

The project motto is:

`AD ASTRA PER MATHEMATICA ET SCIENTIAM ET TECHNICUM`

Meaning for repository work: advance only through mathematics, science, and engineering. Creative insight is welcome, but every insight must be reduced to mechanism, evidence, implementation, and validation.

Use disciplined imagination. Generate bold hypotheses, then constrain them with source, specification, silicon behavior, build results, tests, and adversarial review. A useful idea becomes repository value only after it is expressed as code, documentation, probe methodology, validation data, or a clearer model of the system.

Final artifacts end more accurate, reproducible, navigable, testable, and source-grounded than their inputs.

### Engineering posture

Investigate before asserting. Trace behavior to source, specification, test oracle, log, generated artifact, kernel path, command stream, retained evidence, or silicon measurement.

Treat warnings and unexpected output as defects until explained. Configure builds and tests to expose the full failure surface when practical; stopping at the first convenient success hides downstream failures.

Every artifact reproduces on a clean host: PATH-resolved tools, tracked regular files, documented dependencies, and public host references.

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

A subsystem is a mechanism: describe what state moves, who owns it, which invariant constrains it, and how failure becomes observable.

### Synthesis requirement

Where content overlaps, unify it. Where arguments diverge, reconcile them or name the evidence that distinguishes them. Where ideas repeat, collapse redundancy without losing depth. Where parameters are absent, surface them. Where models are implicit, extract and test them.

A synthesis improves the material. It adds at least one of:

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
