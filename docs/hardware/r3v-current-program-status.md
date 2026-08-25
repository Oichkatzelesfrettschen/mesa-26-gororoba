# R3V current program status

This document is the one program-status authority for the R3V native
Vulkan ICD on RS482. It states the repository heads, the target
deployment, the active conformance partition, the latest target receipt,
the DSO and queue-claim modes, the open tasks, and the documents it
supersedes. Other documents point here; the status table lives here alone
and is updated in place when a head, receipt, or task changes.
`git log -1 -- docs/hardware/r3v-current-program-status.md` names the
commit whose state a revision of this table describes.

## Repository heads

| Repository | Head | Subject |
|---|---|---|
| `mesa-26-gororoba` | `59b77649a80` | r3v: capture plan transcripts per device with a process ordinal |
| `steinmarder-r300` | `d12f7bac` | r3v: retain the closed-gate run of submission slices 7-10 on RS482 |
| `steinmarder` (workspace index, cross-repo orchestration) | `77869e866` | tools/workspace-index-marker-and-trailer-retention |
| `linux-radeon-gororoba` | `01aab9a` | rs4xx: GTT size-segregated placement (PR #123) |
| `radeon-custom` (DKMS + package source pin) | `54acd22` | docs/rewrap-version-axis-paragraph (PR #171) |
| `vostro1000-re` | `30e6c02e64` | docs/rank-content-description-precondition (PR #664) |
| `deqp-vk-fork` | `43c65c132` | r3v: retire the pre-rename driver spelling from the fork |

The Mesa head is the source SHA the latest target receipts declare
(`00e3c5dd25d` at run time, advanced by `59b77649a80` without a rebuild
on the target).

## Target deployment

| Field | Value | Authority |
|---|---|---|
| Host | `cachyos-vostro1000` (Dell Vostro 1000, AMD K8) | `docs/hardware/vostro1000-kernel-modules.md` |
| GPU | RS482, PCI `1002:5974`, `CHIP_RS480`, renderer `ATI RS480` | `include/pci_ids/r300_pci_ids.h` |
| Kernel | `7.1.8-1-cachyos`, radeon module srcversion `727CE89E79FB2D14663C381` | receipt `runtime_event` |
| Policy package | `radeon-rs482-policy 0.8.11-1` | `radeon-custom` |
| Installed ICD | `libvulkan_r3v.so`, built from Mesa `00e3c5dd25d`, sha256 `c0348c6341de4f74f679e8ccdec887096e4b876448633ec2b223a86d8beb6314` | receipt `icd.dso_sha256` |
| dEQP | `deqp-vk` `26d43d452e64` (release `opengl-cts-4.6.8.0-414-g43c65c132`), bundle on the box at `deqp-vk-bundle` | receipt `deqp` |
| Corpus | pinned by `src/amd/r300/vulkan/tests/r3v_conformance_corpus.pin`; the bundle's own mustpass directory is the pinned corpus | runner `wrong_caselist` refusal |

The workspace layout is identical on the workstation and the box; a
merge to `main` is followed by `git pull --ff-only` on the box before any
box-side run.

## Active conformance partition

`src/amd/r300/vulkan/tests/r3v_conformance_partition.tsv` is the
exact-cover partition of the pinned Vulkan 1.0 mustpass corpus: 21 slices,
3,251,483 cases, 3,251,483 executable. Every group's CTS source now
resolves to a hazard: `submission` when the group's dEQP-VK module
reaches `vk::queueSubmit`, `vk::queueSubmit2`, or `submitCommandsAndWait`
on any path (a group entirely gated by an unadvertised extension still
takes `submission` when its file carries a reachable submit site,
fail-closed conservative), `none` with `host-model` evidence when the
group only queries properties, features, or object-management state and
never submits. `r3v_conformance_slices.tsv` orders the first eleven by
hazard:

| Order | Slice | Hazard | Evidence class | Target state |
|---|---|---|---|---|
| 1 | info | none | host-model | silicon-delivered, 17/3/1 |
| 2 | api-version-init | none | host-model | silicon-delivered, 224/13/0 (case ceiling 1800 s) |
| 3 | api-objects | none | host-model | silicon-delivered, 1312/2864/119 |
| 4 | api-info-surface | none | host-model | silicon-delivered, 1414/3354/56 |
| 5 | memory | none | host-model | silicon-delivered, 2461/2522/2 |
| 6 | command | submission | silicon | silicon-delivered, 90/607/154, 0 CS of 16,450 ioctls |
| 7 | transfer | submission | silicon | shut-gate silicon run, 589/309,726/6 |
| 8 | draw | submission | silicon | shut-gate silicon run, 0/39,595/371 |
| 9 | synchronization | submission | silicon | shut-gate silicon run, 211/64,382/278 |
| 10 | robustness | submission | silicon | shut-gate silicon run, 0/1,101/111; host-planning pass: 1,212/1,212 `no_nonempty_ib`, nothing to replay |
| 11 | wsi | display | silicon | open |

Counts read Pass/NotSupported/Fail. Every Fail is classified against
`r3v_conformance_nonpass_ledger.tsv` (19 rows); an unclassified row
refuses decision grade. The machine-readable frontier over the 19
slices -- counts, hazard, evidence status, blocking non-pass classes
joined to their ledger rows, and the next admitting mechanism -- is
`steinmarder-r300/src/re/r300/corpora/rs482_r3v_conformance_frontier_v1/frontier.jsonl`,
regenerated from this partition, the ledger, and the retained bundles;
the frontier's 19-slice shape predates the `api-query-surface` and
`memory-query-surface` split and regenerates to 21 rows on its next
run. The ten slices after the eleventh (`api-unclassified`,
`api-query-surface`, `feature-extensions`, `memory-unclassified`,
`memory-query-surface`, `pipeline-monolithic`, `pipeline-variants`,
`robustness-extended`, `shader-execution`, `wsi-presentation`) carry no
target run.

Under closed submission gates no case in slices 6-10 reaches an IB: the
per-case status of every silicon shard equals its drm-shim host-model
counterpart (416,370 of 416,370 for slices 7-10), so those runs are
decision-grade statements about the closed-gate surface, and a slice's
first real submission waits on a planning pass that lands transcripts,
`r3v_native_plan_tool compose`, per-case plans, and the human gate.

## Latest target receipt

| Field | Value |
|---|---|
| Bundle | `steinmarder-r300/src/re/r300/results/r3v-submission-slices-7-10-closed-gate-target-run-rs482` |
| Finding | `src/re/r300/findings/active/2026-08-24-r3v-submission-slices-7-10-shut-gate-target-run.md` |
| Verdict | `classified_nonpass`, `decision_grade` true, `evidence_class` silicon |
| Source | Mesa `00e3c5dd25deb8545a531a6725b0d76c50156e1c`, clean tree |
| DSO | sha256 `c0348c6341de4f...6314` |
| Partition manifest | sha256 `5886a3de1a95f0a562e47411a30b01a41e9fc3ee91583cd72e20ed9c3c5d410e` |
| Shards | 23, one dEQP process per shard, ~45 s per 20k-case shard |
| CS ioctls | 0 `DRM_IOCTL_RADEON_CS` of 24,206 witnessed; dmesg delta 0 |
| Queue claim | `experimental_compute_subset`, gate declared, `compute_claim_eligible` false |

The preceding receipts are `r3v-command-slice-first-target-run-rs482`
(seal `faeb93f267cc`, same source and DSO) and
`r3v-hazard-free-conformance-slices-first-target-run-rs482` (seal
`638e57a44286`, Mesa `d78bf73c847`, DSO `60e1c7d28216...a263a`).

## Latest host-planning receipt

| Field | Value |
|---|---|
| Bundle | `steinmarder-r300/src/re/r300/results/r3v-robustness-slice-host-planning-pass-workstation` |
| Finding | `src/re/r300/findings/active/2026-08-25-r3v-robustness-slice-host-planning-pass.md` |
| Verdict | `classified_nonpass`, `decision_grade` false, `evidence_class` host-planning |
| Source | Mesa `fb709b391ec5`, clean tree, build-tree DSO `8e5492954a6c...` |
| Outcomes | `no_nonempty_ib` 1,212 of 1,212 (138 cases create a second device), 0 transcripts |
| CS witness | 0 `DRM_IOCTL_RADEON_CS` at the syscall boundary in 1,212 per-case straces |

The robustness slice therefore has no plan to compose or replay; its next
mechanism is the compute recognizer shape below.

## DSO and queue-claim modes

The DSO mode names which binary answered and how a run bound to it:

- installed: the box-built `libvulkan_r3v.so` reached through the Vulkan
  loader with a pinned `VK_DRIVER_FILES`; the runner receipt records
  `icd.dso_sha256` and refuses a run whose digest differs from the
  expected one (`wrong_icd`);
- host-model: the same DSO under the Radeon drm-shim preload, evidence
  class `host-model`, never decision grade;
- host-planning: a planning pass on a submission-hazard slice under the
  radeon drm-shim with every gate closed, one process apiece, and a
  per-process strace witnessing zero kernel-entering CS ioctls; the
  runner admits it as evidence class `host-planning`, never decision
  grade, and seals each case's outcome (`transcript` with digests, or
  `no_nonempty_ib`); the slice's silicon requirement stands;
- plan-bound: a submission-hazard silicon run replays a composed plan
  that binds at the first submission to the DSO BLAKE3, the built source
  SHA prefix, the kernel and module identity, the RS482 PCI identity, and
  the nonce (`r3v_native_plan.c`).

The queue-claim mode is the receipt's `queue_claim.mode`, produced by
`r3v_native_queue_claim_report` through the loader:

- `default_graphics_only`: the ICD advertises graphics and transfer, and
  the compute bit is absent;
- `experimental_compute_subset`: the exact
  `R3V_NATIVE_COMPUTE_QUEUE_EXPERIMENTAL=1` opt-in adds the compute bit
  over the delivered CPU route and the GPU identity carrier; every
  retained target receipt to date ran in this mode;
- `conformant`: reached when every verb ledger row executes on both
  routes; the only mode that makes a receipt `compute_claim_eligible`.

`R3V_CONFORMANCE_STATUS` in `r3v_private.h` stays
`experimental_nonconformant_graphics_without_compute` and
`conformanceVersion` 0.0.0.0 in every mode; the receipt is the authority
for the mode a run advertised, and the driver refuses when the advertised
bit, the ledger claim, and the gate state disagree.

## Current tasks

P0 (blocks the next target run):

- draw slice: a planning pass for `draw`, `synchronization`, and
  `transfer.0000-0001` that lands transcripts (rerun pending); a
  transcript-bearing shard needs compose, per-case plans, and the human
  gate before its first submission. Transcript roots stay under
  `R3V_NATIVE_PLAN_PATH_MAX` (255), so the box root is short;
- the R8G8B8A8_UNORM color-target admission with a receipt-pinned extent,
  the mechanism that moves the draw slice's `vertex_input` and
  `image_outside_executed_envelope` rows.

P1 (host-model rungs that move classified rows without a new gate):

- the compute recognizer index-from-UBO shape (111 robustness cases; the host-planning pass proves no robustness case reaches an IB before it);
- `pipeline_barrier_executing_route_gap` (100 cases) needs secondary
  command buffers, image blit, or sampled/storage-image admission, each a
  larger mechanism than an image cell;
- `driver_defect_open` (17 command-slice cases) per its ledger rows;
- T10.8, the full-corpus target run, waits on the slices above; the
  eight unrun slices then follow partition order;
- P11 WSI: the surface denominator stands
  (`docs/hardware/r3v-wsi-denominator.md`); swapchain allocation,
  acquire, and present are open.

A refuted rung stays refuted: the image usage family plus OPTIMAL-tiling
color feature moved zero cases and its branch is discarded.

## Superseded documents

This table supersedes the status paragraphs of:

- `docs/hardware/r3v-implementation-boundaries.md` `Status` and `Current
  state`, which keep the completion criteria and the source-authority
  queries and defer to this document for heads, receipts, and tasks;
- `src/amd/r300/vulkan/README.md` `Native ICD status` and `Conformance
  ladder`, which describe mechanism and runner contract and defer here
  for the run state;
- `docs/hardware/r3v-host-model-fail-decomposition.md` Table 3 (proposed
  PR sequence), whose executed rows are recorded above;
- the P10/P11 rows of the external implementation ledger, whose
  remaining tasks are the P0/P1 lists above;
- the `steinmarder-r300` conformance ladder corpus README, which scopes
  leaves and points here for status.
