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
| `mesa-26-gororoba` | `9f65f5f61e5` | r3v: reconcile the deployed kernel module with the contract pins before a plan replay |
| `steinmarder-r300` | `61449545c` | r3v: retain the smoke-triangle one-case host-planning receipt and the kernel deployment reconciliation |
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

## Latest one-case host-planning receipt

| Field | Value |
|---|---|
| Bundle | `steinmarder-r300/src/re/r300/results/r3v-smoke-triangle-host-planning-pass-workstation` |
| Finding | `src/re/r300/findings/active/2026-08-25-r3v-smoke-triangle-host-planning-receipt-and-kernel-deployment-reconciliation.md` |
| Case | `dEQP-VK.api.smoke.triangle`, `binding: shard_subset` of `command.0000` (851 cases), subset of 1 |
| Verdict | `classified_nonpass` (`image_outside_executed_envelope`), `decision_grade` false, `evidence_class` host-planning, seal `6e3ed951359d` |
| Source | Mesa `fe55d59c955e`, clean tree (tree `699807657a94`, the tree of main commit `1f91536e639`; the rebase merge renamed the commit and kept its content), build-tree DSO `de0fcee09883...` |
| Outcome | `no_nonempty_ib`; refusal at `vkCreateImage` (`r3v_native_image.c`, usage classification); 0 CS ioctls |

The runner binds a caselist that is a proper subset of one verified
shard as that shard's subset (`r3v_conformance_partition.bind_caselist`),
so a one-case planning or replay run keeps its slice, hazard, and
evidence identity. The test's shape crosses the executed render family in
five independent elements, each its own mechanism with its own silicon
evidence: R8G8B8A8_UNORM lane order (B8G8R8A8_UNORM executed), a 256x256
extent (64x64 and the frozen 64-pixel `RB3D_COLORPITCH0` executed),
`TRANSFER_SRC` readback of a render-family image (the transfer and
render families are disjoint in `r3v_CreateImage`), a magenta fragment
constant (the qualified fragment binary is the solid-green write), and
OPTIMAL tiling on the render family (an admission truth under the
opaque-layout contract, the one element that adds no executed word).
Every submitting graphics case of the command slice refuses at
`vk.createImage` or `vk.endCommandBuffer`, so the first dEQP transcript
waits on those elements; the one-case compose, replay, mutation, and
silicon steps are open.

## Kernel deployment reconciliation

| Field | Value |
|---|---|
| Tool | `src/amd/r300/vulkan/tests/r3v_kernel_deployment_reconcile.py` with `r3v_kernel_deployment_delta_classification.tsv` |
| Artifact | `kernel/kernel_deployment_reconciliation.json` in the bundle above, seal `02a89d5bfae8` |
| Kernel head | `01aab9a` (size-segregated GTT placement) |
| Deployed | `radeon-rs482-policy 0.8.11-1`, source checkpoint `3c5ccb3`, installed and running srcversion `727CE89E79FB2D14663C381` |
| Delta | 9 files: `optimization_only` 3 (`radeon_object.c`, `radeon_object.h`, contract row `RADEON_BO_DOMAIN_PLACEMENT_ORDER`), `unrelated` 6 |
| Pins | 14 of 14 hold at the head; the one placement-order row reads its pre-delta text at the checkpoint |
| Verdict | `retain_deployed`: the first conformance-plan silicon replay binds to 0.8.11-1 and the optimization drift is recorded |

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

- the first dEQP transcript: `dEQP-VK.api.smoke.triangle` is the
  selected one-case bridge from dEQP semantics to the exact-plan hardware
  gate, and it reaches a nonempty IB only after the five render-family
  elements above land.  Those elements decompose into three evidence
  classes rather than five silicon receipts: the lane order, the extent
  with its pitch, and the fragment constant are each one register class
  of the qualified triangle cell (`r300_triangle_render_shape`,
  `common/r300_tcl_bypass_triangle.h`), so one attended session with one
  arm per parameter plus the composed smoke shape earns all three; the
  render-family transfer readback is a host route over the linear color
  BO with no executed change; OPTIMAL admission is a pure admission fact.
  The attended render-shape session ran on RS482
  (`docs/hardware/r3v-native-attended-render-shape-procedure.md`): seven
  arms in table order (reference control, lanes, pitch, constant,
  extent, composed smoke shape, composed-asym), each armed under its own
  digest and `triangle_render_shape` cell kind, `vkQueueSubmit` 0, every
  oracle pass true with the centroid at the predicted dword (reference
  `0xdf20609f`, lanes `0xdf9f6020`, pitch-256 `0xdf20609f`, magenta
  `0xffff00ff`, 256x256 `0xdf20609f`, composed `0xffff00ff`,
  composed-asym `0xff0000ff`), dmesg delta zero, retained `ib.bin`
  byte-identical to the emitter, on Mesa `300a7555716`, kernel
  7.1.8-1-cachyos with `radeon-rs482-policy 0.8.11-1` (srcversion
  `727CE89E79FB2D14663C381`), preflight 354 ok / 40 expected fail / 0
  fail; deviations: no fresh boot (uptime 10 h) and a concurrent Xorg
  session on the GPU; bundle steinmarder-r300
  `r3v-render-shape-family-seven-arm-delivery-rs482`.  The admission
  widening landed on that evidence: the render family spans both 32-bpp
  lane orders, extents to 256, LINEAR or OPTIMAL tiling, the
  eight-pixel-aligned row pitch, transfer usage beside the attachment
  bit, and any FP24-lattice fragment constant, with a non-reference
  target lowered through `r300_tcl_bypass_triangle_render_shape_emit`
  and a copy out of a render target invalidating the unsnooped GTT
  lines first.  Every constant-color single-triangle draw now lowers
  through `r300_tcl_bypass_triangle_render_shape_emit`, the reference
  64x64 B8G8R8A8 target included, so the executed fragment constant is
  the bound module's; the cell family keeps the varying record shape and
  the host-expanded instance count at the reference target.  The
  render shape also carries a target byte offset in its
  `RB3D_COLOROFFSET0` payload, `r3v_BindImageMemory` admits the render
  family at any page-aligned offset whose footprint fits, and the
  load-op clear and the in-pass attachment clears realize any
  `VkClearColorValue` through the target's lane order and the UNORM8
  conversion.  Two receipts stay open: the reference shape under the
  module's constant, and the offset arm of the attended render-shape
  procedure.  The smoke.triangle host-planning rerun reached the first
  transcript (`smoke-triangle.run7`, receipt seal `1be4dc1b5db1`, plan
  seal `777c6a36a5df...`): one submission of 231 IB dwords with three
  relocations (vertex read, color write, completion write), after the
  cell family gained a color-target byte offset, the pipeline stopped
  reading `pTessellationState` on a vertex-plus-fragment pipeline (the
  rule reads it only with both tessellation stages present), load-op
  and attachment clears admit any color, and copies execute in record
  order around the deferred draw (pre-draw copies, then the clear, the
  IB, and the bounded completion wait, then post-draw copies with the
  render target invalidated after completion).  The dEQP verdict under
  the noop drm-shim stays Fail on image comparison, since the shim
  rasterizes nothing.  Open silicon receipts: the offset arm and the
  reference shape under the module's constant.  The transcript
  then needs compose, an independent plan check, drm-shim replay, the
  six mutations (order, digest, relocation, source identity, runtime
  ceiling, nonce) each refusing before the transport, and the
  one-attempt silicon run on 0.8.11-1;
- the fragment-constant identity: the executed cell's fragment block
  writes the byte-order oracle constant (0.125, 0.375, 0.625, 0.875),
  interior dword `0xdf20609f`; pipeline admission
  (`r3v_native_pipeline.c`) accepts the module writing `vec4(0, 1, 0, 1)`
  and `r3v_native_reference_spirv.h` describes the route as solid green
  (`0xff00ff00`).  The public draw now carries the admitted module's
  constant into the four `R300_PFS_PARAM_0` payloads at every target,
  and `R300_MODULE_CONSTANT_CPU_ROUTE_IB_BLAKE3` pins the stream that
  produces; the retained CPU-route digest keeps naming the oracle-color
  reference cell.  The row closes on a silicon receipt for the reference
  shape under a non-oracle constant;
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

## Expansion ladder

Conformance expands by executed mechanism, and each rung opens only after
the rung before it holds silicon evidence. The one-case plan/replay chain
above is rung zero; the ladder after it runs in this order:

1. secondary command buffer execution, implemented or removed by name from
   the first selected family; it is the largest identified
   `pipeline_barrier_executing_route_gap` subpopulation;
2. the bounded image-blit route;
3. fragment sampling through a real descriptor-set binding;
4. sampled-image shapes, admitted only as the executing routes in rungs 2
   and 3 complete;
5. image types, arrays, cube, depth, and larger render extents, each a
   separate mechanism with its own receipt;
6. the native 2x and 4x MSAA path before any sample-count limit rises;
7. composed render and sampling surfaces before any core image or
   framebuffer limit rises.

A mandatory format feature the RS482 pixel pipe lacks stays classified as
structural nonconformance; a software claim for it is fabricated
capability and stays out.

Direct-source ownership moves after the first conformance replay is
frozen: direct SPIR-V readers and the delivery-route selector move to
`src/amd/r300/vulkan` (the selector as policy), while the neutral job IR,
numeric-domain rules, R2VB producer plans, PM4 emitters, carrier
contracts, and compiler admission stay in `common/`. The carrier-policy
registry wires into the production selector only where a real adapter and
route exist, with liveness rows and known-bad selector mutations beside
it. A mechanism already expressed in common code keeps that one
implementation; the Vulkan layer binds to it.

Every vertex route is one execution graph: source, then the selected
executor (direct VAP, CPU, R2VB single, R2VB split), then the canonical
carrier, publication, common VAP re-ingest, and the common raster tail.
The CPU route is the semantic oracle and the small-draw default. R2VB
executes only when the producer is admitted, the numeric domain holds, the
source is GPU-visible, the measured total route cost wins, and every
required BO exists before submission; after an ioctl or any device-visible
side effect the route runs to completion, since a fallback there splits
the graph.

WSI waits for a stable headless submission ladder. Its preconditions:
steinmarder issue #237 closed; Xorg server, DDX, package, kernel, and
runtime identities pinned; the native swapchain image route; acquire,
render, present, release, and reuse proven; the DRM/DRI3 route separated
from the opt-in software XCB route; WSI slices executing under
display-class evidence only.

Diagnostic sidecars stay independent of the run they describe.
`radeontool-gororoba` takes declared pre- and post-run safe snapshots;
`radeontop-gororoba` runs only inside an explicit performance or
engine-discrimination campaign; either launches during a conformance run
only when its sampling and access effects are part of the precommitted
experiment. The radeontool packaging route is `_staging_upstream/PKGBUILD`
alone (the pld-linux RPM spec, which packaged pristine upstream 1.6.3, is
retired).

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
