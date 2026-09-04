---
last_verified: 2026-09-03
evidence_class: program-state
---

# R3V RB2D fill and RS485M identity program ledger

The tranche's scope in one place: every item carried by the audited
direction and the review rounds, with the status that makes it checkable.
This file is the durable home for that state, so a session that loses its
working context recovers the whole program from here rather than from a
transcript.

A row moves when its evidence moves. DONE means the artifact is on main and
a gate proves it; AGENT means a named branch owns it; QUEUED means the
dependency is satisfied and nothing has started; BLOCKED names the
dependency. STOP rows are decisions, not omissions, and each carries the
condition that would reopen it.

Status vocabulary: DONE (landed on main), PUSHED (on a branch, not merged),
AGENT (a running agent owns it), QUEUED (ready, unassigned), BLOCKED (named
dependency), STOP (deliberately not done).

## Epoch

    mesa main            9b3b84fb945   (#2116 merged)
    mesa #2118           2009b159c34, open, rebased on main, profiles 3/4/5
    mesa #2117           superseded, being replaced by two PRs
    stein main           f3627d422
    linux-radeon         2be21eaa8927
    radeon-custom        radeon-unified-dkms 0.8.13-1
    box kernel           7.1.8-1-cachyos, srcversion 46C05689F2C98A526C314F4
    silicon              no token requested, no attempt spent

## Chip identity, established at rank 1

The Dell Vostro 1000 GPU option ROM string table carries "RS485/M BR#26605"
at 0x82 and " Xpress 1150" at 0x1a3. Live vBIOS shadow sha256
70868602cd6aafd7439532504e7dfabfa6966ae4b161b113e2aabad46a9ce807, 53248 bytes;
pristine Phoenix OEM module sha256
149277cafc8ce6762ba68c6ee566b6820843c7ba69411165b9c5834c4eb74d13 carries the
same strings at 0x86 and 0x1a7.

    firmware_chip_name   RS485/M      verbatim
    die_name             RS485        register files, bitfield tables, ISA rules
    part_name            RS485M       captures, receipts, evidence bundles
    product_name         Radeon Xpress 1150
    historical_alias     rs482        spelling sealed evidence carries
    pci                  1002:5974    SHARED with desktop RS482 (Xpress 1100)
    subsystem            1028:022a
    identity basis       pci + subsystem + DMI + firmware, jointly

1002:5974 resolves no single part. 1002:5975 is RS482M.

## Lane A -- mesa identity (#2118)

    DONE   firmware identity finding, die/part split, platform tuple
    DONE   naming ratchet: attribution not proximity, corrective negation,
           bare "vostro" shorthand, receipt-shaped artifact forms
    DONE   prefilter derived from the rules (0 -> 430 files scanned)
    DONE   exact historical-artifact ledger, 592 bundles by path and name
    DONE   P1: arming gates on resolved platform, PLATFORM_UNRESOLVED /
           PLATFORM_MISMATCH, resolver calibrated on 8 arms
    DONE   runtime_match_basis vs identity_evidence split
    DONE   observation-language rule: a measurement names the board it ran
           on; board nouns; the authorized-part form
    DONE   six dangling r300_rs480_die_facts doc locators repointed by scope
    DONE   README PCI id inversion corrected
    DONE   128 repository-scan findings driven to zero; naming-policy-test
           passes repository-wide
    DONE   defect 1: at_rest values live only in r300_specimen_facts; the
           family record keeps the *_reg addresses and none of the values
    DONE   defect 2: negation binds to a window around the attribution verb
           rather than the whole clause
    DONE   rebase onto 9b3b84fb945; profiles 3 (Ok 492), 4 (Ok 490),
           5 gcc (Ok 492, zero warnings), each Fail 0. Profile 4 runs two
           fewer tests because it does not build zink; the difference is
           mesa:zink alone and touches no r300 or r3v path.
    DONE   P1 found in review: plan capture and replay declared the arming
           verdict ARMED outright, and those paths reach DRM_RADEON_CS, so
           a plan file on any device sharing 1002:5974 submitted without
           the board ever being compared. r3v_native_arming_platform_
           verdict() carries the identity half alone and both paths ask it.
           Board identity now populates outside the hazard-gate branch,
           where the facts struct had been left zeroed.
    DONE   each drm-shim harness declares its board at its own device-
           creation seam; an undeclared shim device resolves to no board
    DONE   PR body reconciled: joint resolution over pci + subsystem + DMI
           + firmware, no register-header scope claim, ledger-exact
           exemption, all three profiles
    DONE   ten review threads replied to and resolved
    AGENT  ratchet false positive (accurate shared-id prose rejected) and
           false negative (receipt-phrased specimen claims undetected);
           vostro1000-kernel-modules.md line 17; AGENTS.md:326 directive
    QUEUED dev-host + Vostro no-submit platform resolution through the real
           libdrm and DMI path, not synthetic tuples
    QUEUED merge once the ratchet arms land

## Lane B -- mesa build infrastructure

    AGENT  review-thread-frontier repair (PRRT_kwDOR3YK5M6Qaw1U, evidence
           owner mesa-gororoba-debug-optimized/PKGBUILD past 217378421cb).
           Pre-existing on main; blocks profile-4 evidence under
           REPRODUCIBLE_RUN=1. Rebase and merge AFTER #2118.
    AGENT  build-infra naming census disposition: profile display names,
           build-dir names, meson options, env files, make targets, package
           recipes and comments, runner names, receipt identifiers,
           evidence roots. Migrate project-owned specimen names; preserve
           owner-spelled interfaces behind compatibility aliases.

## Lane C -- mesa registers

    PUSHED radeon_legacy_2d_reg.h, PR 2120: twelve registers and twelve
           field codes out of r300_rb2d_fill.c; both goldens byte-identical,
           all twelve addresses agree with the kernel, six mutation
           categories caught, profiles 3/4/5 at Ok 387 Fail 0. B11 IB byte-identical, span-plan IB
           byte-identical, BLAKE3 unchanged, kernel safe-list address
           parity, one known-bad address mutation per register family.
           Rebase and re-prove after #2118 merges.
    QUEUED r300_register_space.h: typed direct-MMIO vs MC/PLL/PCIe index
           identities; audit refusing an index into a PM4 PACKET0 emitter
    QUEUED rs4xx_reg.h: RS4XX_NB_MC_INDEX_PORT 0x0168, RS4XX_NB_MC_DATA_PORT
           0x016c, owner-spelled RS480_* aliases retained
    QUEUED rs4xx_mc_reg.h: the ~22 MC index rows (of 63 indirect atoms:
           22 MC, 10 PCIe, 31 PLL). Indices, never BAR0 offsets.
    STOP   rs485_reg.h -- absent until comparative evidence shows a register
           present or behaviorally distinct on RS485, not shared with
           RS480/RS482, and used by current code or a checked contract
    STOP   live MC accessor in Mesa userspace -- kernel owns the index lock,
           transition gates, parked containment, sequencing
    STOP   bulk import of the 1606 unnamed registry rows into r300_reg.h

    Registry vs header: 2147 decomposed MMIO atoms, 551 named in r300_reg.h,
    1606 unnamed, 270 of those validated-safe, 7077 fields. Safe-read is NOT
    the generator filter; code-generation disposition is a separate predicate.

## Lane D -- mesa RB2D route

    AGENT  #2117 four repairs as cherry-pickable commits, NOT a merge
           candidate: rebase (3 span call sites broke), one 256-byte
           segment, dispatch hole, missing arming case, provenance before
           install
    QUEUED r3v/transactional-submit-route-preparation -- no GPU execution
           change. prepare -> validate -> commit; r3v_recorded_work_census;
           submit-wide preflight counting route candidates; GPU_ONLY over
           the whole submit; cached operation_route_gates; malformed policy
           refuses vkCreateDevice; single-bit use validation before the
           precommitted loop; VK_ERROR_UNKNOWN at the boundary; phased
           provenance (prepared/committed/ioctl_entered/ioctl_accepted/
           completion_retired/result_verified); explicit HOST_TRANSFER_
           CONST_FILL route row
    QUEUED r3v/native-rb2d-fill-route -- narrow: one submit, one command
           buffer, one fill, one BO, one 256-byte segment, <=3 rectangles,
           one relocation entry, one completion
    QUEUED fill-specific arming authority hashing the whole semantic and
           submit identity, not just the IB digest
    QUEUED memory contract: buffer bound, TRANSFER_DST, alignment, range
           inside VkBuffer and VkDeviceMemory, inside the 32-bit envelope,
           host-visible GTT type for the first route
    QUEUED loader-only application + symbol audit refusing r3v_native_*,
           r300_rb2d_*, radeon_drm_vk_*
    QUEUED runtime host-exclusion: mprotect leg + known-bad host leg,
           provider counters, zero route-local host semantic writes
    QUEUED route_state x automatic_selection separation. After receipt:
           EXECUTING + BENCHMARK_PENDING. AUTO keeps host until crossover
           measured over 4 B, 64 B, 256 B, 4 KiB, 64 KiB, 512 KiB, ~2 MiB
           including planning, IB build, ioctl, completion, invalidation

## Lane E -- steinmarder

    AGENT  #576 PyYAML workflow dependency, time-invariant comment, local
           gate transcript as merge authority (no runners since 08-22)
    AGENT  RS485M identity migration: 7 prose claims, 92 silicon_target
           rows classified, self-contradicting COMBIOS finding front
           matter, 1002:5974 seed -> ambiguous row, regenerate
    QUEUED rs4xx_pci_identity.tsv, rs4xx_platform_identity.tsv,
           rs4xx_firmware_identity.tsv, rs4xx_source_name_assertions.tsv,
           rs4xx_identity_aliases.tsv
    QUEUED sealed-artifact identity overlay
           (historical_artifact_specimen_identity.tsv) -- must consume the
           migration census AFTER it is stable, never concurrently
    QUEUED rs485m_identity_migration.tsv census, unclassified count = 0
    BLOCKED #577 recapture -- needs the box under tmux AND the final RS485M
           platform tuple from the identity migration. Fail-fast POSIX sh,
           fresh directory, module-to-source join through embedded module
           metadata and DKMS output identity
    QUEUED operation-evidence ledger v2, contract_schema dispatch over
           r300-r2vb-identity-carrier-contract/v1 and
           r300-rb2d-fill-contract/v1. Merge before silicon. No RB2D exact
           route record before the silicon result exists.
    QUEUED #575 rework LAST in this group: split into 7 findings, correct 6
           overclaims (dynclks can clear force bits; legacy engine-clock
           setter programs SCLK independently of REDUCED_SPEED_SCLK_EN;
           above-bit-15 unreachability is COMBIOS-populated states only;
           SCLK_CNTL2 unproven on this part; offline ROM has no dmesg
           window; a table ending at the image boundary must decode).
           Regenerate from the post-identity, post-seal tree.

## Lane F -- vostro1000-re

    AGENT  identity migration, ROM filename preserved, canonical metadata
           added. Independent of the mesa and stein ordering.

## Lane G -- low priority, read-only until the principal migration lands

    QUEUED radeontool-gororoba, radeontop-gororoba, umr-gororoba census.
           Each occurrence classified as platform claim / family mechanism /
           owner-spelled / shared PCI / historical artifact. A raw rs482
           token count is not a migration plan.

## Qualification ladder -- after the two replacement PRs merge

    BLOCKED profiles 3, 4, 5 with distcc DISABLED for the evidence build
    BLOCKED profile-4 ICD sha256, build id, exports, DT_NEEDED, Gallium
            separation, compiler and linker versions
    BLOCKED r300 / r3v / radeon-drm-vk / drm-shim suites, registration proof
    BLOCKED parser and CS-track replay tools built from kernel 2be21eaa8927
    BLOCKED mutation refusals: wrong pitch, wrong base offset, wrong
            rectangle extent, wrong fill value, wrong relocation site,
            wrong BO role, wrong write domain, truncated IB, one extra
            segment, 32-bit address wrap
    BLOCKED loader-only application under drm-shim
    BLOCKED exact prepared submit plan captured, every bound field mutated
    BLOCKED kernel-entering DRM_RADEON_CS count = 0 under the shim
    BLOCKED route-local host semantic writes = 0
    BLOCKED non-submitting arming runner
    BLOCKED sealed prediction

## Silicon -- requires an explicit token from the operator

    Attended cell: 64 KiB buffer, offset 12, size 4992, value 0x11223344,
    256-byte pitch, ONE segment, 3 rectangles (3,0,61,1) (0,1,64,18)
    (0,19,35,1), 20 rows, 5120-byte footprint.
    Preconditions: fresh boot, 0.8.13 epoch, srcversion 46C05689F2C98A526C314F4,
    fresh evidence dir, off-box log, no unrelated DRM holder, one submit,
    one completion, one token, no retry, and the arming gate resolving
    DELL_VOSTRO1000_RS485M.
    Separate cells, each its own token: 64-byte pitch probe, multi-segment,
    scissor 0x2000 inclusivity, VRAM destination, mixed transfer buffer.

## Findings that outlive their lane

    A gate that forces a verdict for a path has removed that path from
    every predicate the verdict carries, not only the one the exemption
    was written for. The plan capture and replay exemption was written to
    skip the attended-run ceremony -- bundle digest, kernel and module
    pin, evidence directory, one-shot token -- and it silently also
    skipped the board comparison, on paths that reach DRM_RADEON_CS. Two
    classes of check sat in one function and one verdict spoke for both.
    Split the predicate rather than the caller: identity is a fact about
    the silicon and holds however the run is dressed.

    A zero-initialized fact struct populated inside a conditional reads
    as a well-formed refusal from outside it. The board identity fields
    were filled only inside the hazard-gate branch, so the first version
    of the fix compared zeros and refused everything that used the very
    path it was meant to protect. The refusal looked like the gate
    working.

    A harness that substitutes a fact provider must declare the board it
    stands in for at the seam where it creates the device, not per arm.
    The replay harness declared it for its replay device and not for the
    capture device it builds its own plan with, so half its arms carried
    an undeclared board.

## Standing stop lines

    No recursive rs482 -> RS485M rename. Four populations: platform claims
    (wrong, fix), sealed artifacts (never), owner-spelled (never),
    shared-id and family scope (correct as written).
    No sealed byte rewritten. No global normalization of 1002:5974 to
    either RS482 or RS485M. No watchdog dependency in R3V. No GPU thermal
    governor from a CPU proxy. No SCLK write to preserve a forced state.
    No CP-ME execution. No advertisement before an executing route.
    No merge, silicon decision, or token request delegated to an agent.

## GAPS found by sweeping the directives against the above

Seven items appeared in the audited direction or a reviewer message and were
in no lane until this sweep.

    GAP-1  Thermal proxy falsifier, read-only, BEFORE any thermal claim.
           Hold CPU work and frequency stable; record every EC, ACPI, and
           hwmon temperature; record fan state; run a GPU-heavy CPU-light
           workload; measure amplitude, onset lag, peak lag, decay; run a
           CPU-only control; compare signatures. Requires MAGNITUDE AND LAG,
           not mere movement. If it fails, declare the platform has no
           usable GPU thermal feedback and stop. Currently only the stop
           line was recorded, not the experiment that justifies it.

    GAP-2  COMBIOS ASIC-init register contract. The ROM decoder records 39
           direct MMIO operations over 37 distinct addresses. Normalize at
           the TRANSACTION level: resolve MM_INDEX/MM_DATA pairs to their
           logical target (e.g. R300_VAP_CNTL 0x2080) and CLOCK_CNTL_INDEX/
           DATA to the selected PLL register. Never publish MM_DATA or
           CLOCK_CNTL_INDEX as ordinary safe registers -- they are indexed
           aperture ports that race driver state.
           Produce combios_asic_init_mmio_resolved.tsv with name-confidence
           classes EXACT_KERNEL_NAME / EXACT_XORG_NAME / FAMILY_RRG_INFERRED
           / LOGICAL_APERTURE_TARGET / UNRESOLVED. A family-inferred RS690
           name never becomes an exact RS485M fact.
           Then, AFTER the first RB2D receipt: rs48x_platform_boot_contract.json,
           rs48x_kernel_ownership_contract.tsv, rs48x_r3v_assumptions.tsv,
           carrying only facts R3V depends on. Qualification-only cross-repo
           check that firmware POST VAP, live retained VAP readback, Gallium
           default, and the R3V first-draw contract all decode
           VF_MAX_VTX_NUM = 5.

    GAP-3  Public register-contract export chain. Stein is private, Mesa is
           public, so ordinary Mesa generation must not depend on a sibling
           private checkout.
             stein registry -> export_rs4xx_register_contract.py ->
             sanitized public TSV/JSON -> checked into mesa ->
             gen_r300_register_headers.py -> checked-in generated headers
           Four exports: radeon_legacy_2d, r300_direct, rs4xx_direct,
           rs4xx_mc. Generation predicates: unambiguous address space,
           approved symbolic identity, applicable chip scope, accepted
           source authority, NAMED CURRENT CONSUMER, explicit
           code-generation disposition. NOT safe_read == true.
           Qualification-only cross-repo digest comparison, explicitly
           skipped in public CI when the private repo is absent.

    GAP-4  Field representation. The 7077-field corpus becomes generated
           DESCRIPTOR DATA (struct r300_register_field_desc), never bulk
           preprocessor macros. C masks and checked packers only for active
           consumers, with overflow REFUSAL rather than the historical
           shift-and-silently-truncate convention. Compile-time assertions
           bind packers to the generated metadata.

    GAP-5  Route dependency order after the RB2D fill promotes. Nine routes,
           each requiring semantic contract, numeric domain, common plan,
           deterministic PM4, independent parity, kernel replay, drm-shim,
           host-write exclusion, silicon result, public Vulkan adapter,
           targeted dEQP movement, performance policy, promotion, and
           current-epoch evidence:
             1 RB2D copy          2 RB3D clear       3 TX->RB3D nearest copy
             4 TX->RB3D blit      5 R2VB crossover   6 ROP Boolean routes
             7 FP24 exact-domain  8 stencil + ZPASS  9 multipass jobs
           No execution-unit enum value lands before its first route row.

    GAP-6  Conformance queues, kept in parallel with the route work.
           Product-correctness queue: every valid dEQP Fail is an open
           defect. Regenerate exact decompositions from actual QPA
           artifacts; do NOT reuse the unsourced 338/188/71/96 split.
           Priority: unclassified driver failures, graphics-pipeline
           admission, compute-pipeline admission, image creation,
           descriptor execution, sampler state, barrier and sync routes.
           Hardware-migration queue: move host-executed passing operations
           onto GPU routes. A migration adds no Vulkan capability when the
           host already supplied the command correctly.
           VK_KHR_portability_subset only after a NORMATIVE matrix of which
           deviations it actually permits. Never a blanket waiver.

    GAP-7  Doc and artifact rename citation map, generated BEFORE any
           rename: old_path, new_path, old_scope, new_scope,
           inbound_references, sealed_reference_count,
           redirect_or_alias_required. All mutable inbound references
           updated in the same PR.
           Scope rule: a Vostro measurement renames to rs485m-*; a genuine
           family-architecture document renames to rs48x-*, NOT rs485m-*,
           because a specimen prefix on family content is the opposite
           error. The census named nine such family-scope documents.

## Open questions for the operator

    Q-1  radeon-rs482-policy is a LIVE DEPLOYED package name, cited in
         attended procedures via `pacman -Q`. Renaming it has real-world
         cost beyond any source tree. Rename, alias, or leave?
    Q-2  Nine UNCLASSIFIED identity occurrences from the census, including
         external sibling repo names (xf86-video-ati-rs482, embedded as a
         schema enum constant) and a git branch citation
         (r300/rs482-swtcl-draw-jit-release).
    Q-3  Six MIXED documents whose remainders the census could not separate
         without a manual pass: rs482-gpu-compute-substrate-atlas.md,
         rs482-hybrid-vertex-tcl-design.md, r3v-current-program-status.md,
         r3v-implementation-boundaries.md, rs482-source-authority.md,
         rs482-native-delivery-route-admission-and-oracle-model.md.
         Roughly 200 lines, genuinely open, NOT sweep-cleared.
    Q-4  SHADOW_COMPARE execution policy: deliberately NOT added until a
         comparator and an ownership model exist. Confirm that deferral.
