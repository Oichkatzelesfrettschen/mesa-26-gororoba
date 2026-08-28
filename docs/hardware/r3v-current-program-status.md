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
| `mesa-26-gororoba` | `84cf229c0d6` | r3v: arm the composed cell from its bound digest |
| `steinmarder-r300` | `2ae932526` | r3v: read the varying interpolation out of the 256x256 target |
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
`r3v_conformance_nonpass_ledger.tsv` (20 rows); an unclassified row
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
| Bundle | `steinmarder-r300/src/re/r300/results/r3v-native-smoke-triangle-plan-replay-first-silicon-pass-rs482` |
| Finding | `src/re/r300/findings/active/2026-08-26-r3v-smoke-triangle-plan-replay-silicon-pass.md` |
| Case | `dEQP-VK.api.smoke.triangle`, plan replay of one 231-dword triangle IB |
| Verdict | `Pass`, `decision_grade` true, `evidence_class` silicon, receipt seal `c68d24e2957a...` |
| Source | Mesa `133f7703713910fed6b3f3c545dd1bf08a60395c`, clean tree; DSO sha256 `fc37a699222e13...6aa3`, BLAKE3 `9a155d81b9b9...ff94` |
| Submission | digest `389cc2a228a1...51fb1`, retained IB and one `chain.log` entry, session `complete/admitted` |
| Runtime | kernel `7.1.8-1-cachyos`, radeon srcversion `727CE89E79FB2D14663C381`, `radeon-rs482-policy 0.8.11-1`, boot `d217f017-...`, dmesg delta 0 |
| Gate | `R3V_NATIVE_SUBMIT_HAZARD_ACCEPTED` unset; the bound plan is the authorization |

The prediction was Fail on image comparison (reasoned from the noop-shim
host-planning receipt); the observed dEQP status is Pass. The deviation is
the finding: the `driver_defect_open` classification is a host-model
artifact of the shim, and the RS482 render refutes it. The six-mutation
plan ladder (order, runtime, digest, relocation, source, nonce) each
refuses before any `DRM_IOCTL_RADEON_CS`, and the two render-shape
receipts (offset arm, module-constant arm) close in the same bundle.

The preceding submission-slice receipt is
`r3v-submission-slices-7-10-closed-gate-target-run-rs482` (Mesa
`00e3c5dd25de`, seal per shard, 0 `DRM_IOCTL_RADEON_CS` of 24,206). The
earlier receipts are `r3v-command-slice-first-target-run-rs482`
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
waited on those elements; the one-case compose, independent plan check,
drm-shim mutation ladder, and one-attempt silicon replay are done, and
the case passes on RS482 (bundle
`r3v-native-smoke-triangle-plan-replay-first-silicon-pass-rs482`).

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

- the first dEQP transcript is delivered: `dEQP-VK.api.smoke.triangle`,
  the one-case bridge from dEQP semantics to the exact-plan hardware gate,
  passes at decision grade on RS482 through the plan replay (see the
  latest target receipt above); this row is closed and the next target
  run is the expansion ladder rung 1.  It reached a nonempty IB after the
  five render-family elements landed.  Those elements decompose into three
  evidence
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
  conversion.  Both render-shape receipts are closed on RS482 (bundle
  steinmarder-r300
  `r3v-native-smoke-triangle-plan-replay-first-silicon-pass-rs482`): the
  offset arm renders the reference shape based at byte 4096 with the first
  4096 bytes left at the sentinel (oracle centroid `0xdf20609f`), and the
  module-constant arm renders the reference 64x64 shape with the admitted
  pipeline module's own `vec4(0, 1, 0, 1)` (oracle centroid `0xff00ff00`),
  each armed under its own digest, `vkQueueSubmit` 0, dmesg delta 0,
  `ib.bin` byte-identical to the emitter.  The smoke.triangle transcript
  (231 IB dwords, three relocations: vertex read, color write, completion
  write) then composed to a sealed plan and replayed on RS482 to a
  decision-grade `Pass`: the plan binds this box's source, DSO, kernel,
  and module identity at the first submission, the queue opens the CS
  ioctl without `R3V_NATIVE_SUBMIT_HAZARD_ACCEPTED`, dEQP writes reference
  and result images and its image comparison passes, the session reaches
  `complete/admitted` with one retained IB, and the dmesg delta is zero.
  The prediction was Fail (reasoned from the noop-shim host-planning
  receipt, where nothing rasterizes); the Pass is the deviation and the
  finding.  The six plan mutations (order, runtime, digest, relocation,
  source, nonce) each refuse before any `DRM_IOCTL_RADEON_CS`: order and
  runtime at `r3v_native_plan_tool check`, digest and relocation at the
  driver admit, source and nonce at the driver bind;
- the fragment-constant identity: the executed cell's fragment block
  writes the byte-order oracle constant (0.125, 0.375, 0.625, 0.875),
  interior dword `0xdf20609f`; pipeline admission
  (`r3v_native_pipeline.c`) accepts the module writing `vec4(0, 1, 0, 1)`
  and `r3v_native_reference_spirv.h` describes the route as solid green
  (`0xff00ff00`).  The public draw now carries the admitted module's
  constant into the four `R300_PFS_PARAM_0` payloads at every target,
  and `R300_MODULE_CONSTANT_CPU_ROUTE_IB_BLAKE3` pins the stream that
  produces; the retained CPU-route digest keeps naming the oracle-color
  reference cell.  The row closes: the module-constant arm renders the
  reference shape under the admitted module's `vec4(0, 1, 0, 1)` on RS482,
  oracle centroid `0xff00ff00`, in bundle
  `r3v-native-smoke-triangle-plan-replay-first-silicon-pass-rs482`;
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
- `pipeline_barrier_executing_route_gap`: the secondary replay, the
  nearest scaling blit, and the sampled-image admission moved the family
  4 -> 28 Pass under the shim and all three movements hold on RS482
  silicon (rungs 1-3 above); the residual walls are the withheld
  storage/all-usage image shapes at `vkCreateImage` (24 cases) and the
  render pipelines outside the qualified draw subset at
  `vkCreateGraphicsPipelines` (52 cases).  The layered and 1D admission
  (rung 5) left this family at its 28 Pass and moved 36
  `object_management` cases instead;
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

1. secondary command buffer execution -- closed: `r3v_CmdExecuteCommands`
   replays a secondary's recorded host-executed ops (deferred copies,
   event ops, query ops) into the primary in recorded order, and the
   moved `pipeline_barrier` subgroups pass on RS482 silicon at decision
   grade (8/8 Pass, dmesg delta 0, gates closed, seal `13b4b86a37c2b2`,
   bundle steinmarder-r300
   `r3v-native-secondary-replay-pipeline-barrier-first-silicon-pass-rs482`);
   the replay also closed an update-buffer aliasing double free by
   taking an owned copy of the replayed bytes;
2. the bounded image-blit route -- closed: `r3v_CmdBlitImage` lowers
   unequal-extent rectangles onto a nearest resample executor whose
   sample point (d + 0.5) * src/dst matches the spec's nearest filter
   (VK_FILTER_NEAREST, distinct images; flips keep refusing), and the
   twelve moved subgroups pass on RS482 silicon at decision grade
   (12/12 Pass, dmesg delta 0, gates closed, seal `765a0687a232c2`,
   bundle steinmarder-r300
   `r3v-native-scaling-blit-pipeline-barrier-silicon-pass-rs482`);
3. fragment sampling through a real descriptor-set binding -- closed:
   the sampled cell binds a uniform R8G8B8A8 texel through a
   combined-image-sampler descriptor set and a TCL-bypass triangle
   samples it on TX unit 0; the attended run on RS482 rendered the
   predicted centroid `0xe02060a0` with an empty dmesg delta and gates
   armed one-shot (cell blake3 `4e3d252315fb`, bundle steinmarder-r300
   `r3v-native-sampled-descriptor-cell-silicon-pass-rs482`); the
   falsifying first run exposed that `R300_TX_FORMAT1` channel selects
   default to X (finding
   `rs482-tx-format1-channel-selects-default-to-x`), fixed by the
   identity-select composition; the rung moves the two sampled
   `pipeline_barrier` subgroups, 8 cases, taking the family to 28 Pass
   (host-model seal `209685514115`, bundle steinmarder-r300
   `r3v-sampled-rung-conformance-movement-host-model`);
4. sampled-image shapes, admitted only as the executing routes in rungs 2
   and 3 complete -- first shape closed: the B8G8R8A8_UNORM sampled lane
   order rides the swapped TX_FORMAT1 select set and rendered the
   predicted centroid on RS482 with the byte-X falsifier absent (cell
   blake3 `640c1336`, bundle steinmarder-r300
   `r3v-native-sampled-bgra-lane-order-silicon-pass-rs482`), and a
   split-row texture separates an addressed fetch from a constant one:
   two oracle pixels read the texels at texel rows 6 and 11 as predicted
   on RS482, so the TX unit addresses rows from the varying and the T
   axis runs in texture order (bundle steinmarder-r300
   `r3v-native-sampled-split-row-addressing-silicon-pass-rs482`);
   the rung's conformance movement is zero: the `pipeline_barrier`
   family stands at 28 Pass before and after both shapes, so they widen
   the executed envelope and carry no case.  The `object_management`
   sampled population stays at 90 `vkCreateImage` refusals because its
   `img2D` shape requests `arraySize = 12` together with
   `SAMPLED_BIT | COLOR_ATTACHMENT_BIT`, so multi-layer arrays and the
   sampled-plus-color usage union each leave the other refusing; texture
   extents past the reference geometry stay open, and the filter and
   wrap modes outside nearest plus clamp-to-edge are sampler state
   rather than an image shape and take their own rung position;
5. image types, arrays, cube, depth, and larger render extents, each a
   separate mechanism with its own receipt.  The `object_management`
   population that needs this rung is creation-only -- the cases build
   the 12-layer `img2D` and destroy it without sampling a layer -- so
   admitting the shape to move them would advertise a layered sampled
   image the TX block programs no route for, which is the fabricated
   capability the ledger's first row refuses.  The rung opened on the
   executing layered route: a view selects one layer and its stride
   joins the bind offset in the `TX_OFFSET_0` and `RB3D_COLOROFFSET0`
   payloads the sampling and render cells already carry, so creation
   admits `arrayLayers` to the reported `maxImageArrayLayers`,
   `VK_IMAGE_TYPE_1D` as the height-one member of the layout, and the
   sampled bit beside the color-attachment bit over the stricter
   64-byte row pitch both routes read.  The host-model receipt is the
   submit-order arm `sampled-layer-armed`, whose recorded IB for a view
   selecting the last of three texture layers byte-matches the offline
   cell emitted at that layer's stride while the layer-zero arm still
   matches offset zero.  The rung moves 36 `object_management` cases --
   `image_1d`, `image_2d`, `image_view_1d`, `image_view_2d` across nine
   parent groups, the group reaching 286 Pass from 250 -- and the
   `pipeline_barrier` family re-measures unchanged (bundle
   steinmarder-r300
   `r3v-layered-1d-image-conformance-movement-host-model`, seals
   `ef21bc535a06` and `48a5de9734a0`).  Eighteen array-view cases moved
   their refusal from `vkCreateImage` to `vkCreateImageView`, which the
   ledger row `layered_view_type_route_absent` now carries.  The layer count the render
   family admits answers to the cell's `RB3D_COLOROFFSET0` ceiling,
   which the creation admission and the format query both name, while
   the sampling family reaches the reported device limit because
   `TX_OFFSET_0` carries the full span.  The sampling family holds the
   silicon receipt: the attended arms `layer`, `row1`, and `wide`
   (`r3v_native_sampled_arms.h`, digests `4afc72c0`, `83063087`, and
   `575c6747`) each submitted one live `DRM_RADEON_CS` on RS482 and read
   the dword its prediction named, with every dmesg delta empty.  The
   layered arm's two unselected layers hold a decoy texel, so a dropped
   `TX_OFFSET_0` stride reads layer 0 and lands on the named falsifier
   rather than on a value the predicted dword absorbs (bundle
   steinmarder-r300
   `r3v-native-layered-and-height-one-texture-silicon-pass-rs482`).
   Each of those receipts carried the point check alone, because the
   runner filled the render shape's `color_bits` with the texel and
   those values sit off the FP24 s1e7m16 lattice, so the coverage
   producer refused and reported every counter zero.  A cell whose
   fragment color arrives through the TX unit drives no
   `R300_PFS_PARAM_0` constant, so the verdict now admits on geometry
   alone and carries a `judged` flag that separates a refusal from a
   total mismatch.  The refusal was already read out of the retained bytes
   for the `layer`, `row1`, and `wide` arms (steinmarder-r300 #543),
   which recovered their region result offline while the producer kept
   refusing; the admission split is what stops the refusal at its
   source.  Replaying the narrowed verdict over every retained
   `color_target.bin` reproduces that result for those three arms and
   extends it to the ones it did not reach: `sampled`, `bgra`, and,
   under the two-texel model their `[shape]` lines name, `split-rows`
   are each coverage-exact over the full 64x64 footprint too, 1152
   interior pixels against 1152 analytic with a clean canary and no
   mismatch.  The retained first sampled take reproduces its recorded
   deviation, which calibrates the replay against a known-bad input.
   The volume, cube, and array view rows separate by what their cases
   execute.  Every one of them is an `object_management` case
   (`vktApiObjectManagementTests.cpp`).  Its `Image` entry carries an
   empty `Resources` and a `create` that calls `createImage` alone, so
   an image case creates and destroys without binding memory; its
   `ImageView` entry binds memory to the dependency image and creates a
   view over it.  Neither draws, submits, nor samples, so admission
   alone moves the row and no fetch runs behind it.  `r3v_CreateImage`
   admits `VK_IMAGE_TYPE_3D` over the layer stride its depth slices
   already stack at, reporting that stride as `depthPitch`, and
   `VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT` over a square 2D image with
   whole cubes of layers; `r3v_CreateImageView` admits the 3D, cube,
   cube-array, and array view types over the layers the image holds.
   The capability stays honest at the executing boundary rather than at
   creation: the TX program addresses one slice through the
   `TX_OFFSET_0` stride a view resolved at creation and the color
   backend places one slice's base in `RB3D_COLOROFFSET0`, so the draw's
   sampled binding and the render pass both take the one-slice view
   types alone (`r3v_native_view_type_executes`), which
   `r3v-native-submit-order-sampled-array-view-refused` and the
   attachment arm of `r3v-native-public-surface` pin.  Predicted
   movement: the `image_3d`, `image_view_3d`, `image_view_cube`,
   `image_view_1d_arr`, and `image_view_2d_arr` sub-populations reach
   Pass, unmeasured until a conformance run.  Open inside the rung: the
   render family's own layer ceiling answers to the creation gate rather
   than to a run, and the `framebuffer` sub-population keeps its refusal,
   needing an attachment route no cell executes;
6. the native 2x and 4x MSAA path before any sample-count limit rises.
   The rung decomposes into three mechanisms, and the register and
   kernel contracts are read.  The multisample color surface lives in
   VRAM (`r300_texture_initial_domain`, r300_texture.c) over a
   sample-expanded allocation (`layer_size *= nr_samples`,
   r300_texture_desc.c), while the render family's load-op clear is a
   host fill of a CPU-mapped type-0 allocation, so an MSAA target needs
   a device-local render family and a clear the command stream emits
   rather than the host.  `r100_cs_track_check` sizes the color buffer
   as `pitch * cpp * maxy` with no sample term, so the kernel validates
   nothing about the sample expansion and the footprint proof lives
   entirely in the driver.  That bounds what the rung can claim: while
   the parser footprint carries no sample multiplier, the multisample
   path stays an internal attended cell rather than an advertised
   Vulkan capability, and it rises to a public capability once the
   parser incorporates the multiplier or the driver-side bound is
   established independently.  Placement takes the same care.
   `r3v_native_memory_type_policy` (r3v_native_memory.c) gives type 1
   `RADEON_GEM_DOMAIN_VRAM | RADEON_GEM_DOMAIN_GTT` under
   `RADEON_GEM_NO_CPU_ACCESS`, so a host-unmapped allocation is not a
   VRAM-resident allocation: GTT stands as a fallback domain.  The
   recorder therefore either takes a strict internal VRAM path or
   retains placement evidence proving the multisample buffer resides
   where the cell claims.  The resolve is `RB3D_AARESOLVE_OFFSET`,
   which takes a relocation and carries a 32-byte-aligned destination
   offset, `RB3D_AARESOLVE_PITCH`, a pixel pitch in bits 1 through 13
   the kernel masks to `0x3ffe`, and
   `RB3D_AARESOLVE_CTL.AARESOLVE_MODE`, with the destination format
   inherited from color buffer 0.  Per the AMD R3xx 3D register
   document the resolve destination carries a pitch and an offset and
   no tiling field, where `RB3D_COLORPITCH0` carries `COLORTILE` and
   `COLORMICROTILE` beside its pitch; `r300_is_simple_msaa_resolve`
   nonetheless takes its fast path only for a tiled destination, so
   whether a linear resolve destination receives linearly ordered bytes
   is an open silicon question.  Its falsifier: a resolve into a linear
   GTT destination whose oracle reads the destination's interior at the
   predicted dword, refuted by a tiled swizzle of the same bytes.  The
   rung opens on the device-local render family with a command-stream
   clear; a narrower first arm leaves the multisample buffer at
   `VK_ATTACHMENT_LOAD_OP_DONT_CARE` and oracles the resolve
   destination's interior alone, which is a named scope cut -- it
   surrenders the sentinel corner that proves the device wrote inside
   the extent.  An uncleared
   footprint takes `r300_tcl_bypass_triangle_interior_oracle`, which
   classifies the centers the geometry covers and leaves the exterior,
   the pitch padding, and the canary row unjudged, so the garbage
   around the draw reads as unjudged rather than as the refusal the
   full-footprint oracle returns for it; its blank-footprint arm
   reports zero interior pixels against the same denominator, so a
   resolve that wrote nothing and one that wrote correctly are distinct
   results.  Its denominator is the pixel center, which a resolved
   target does not answer to: `GB_MSPOS0` and `GB_MSPOS1` place the
   subsamples off-center, so a pixel whose center sits inside the
   triangle while a subsample sits outside resolves to a blend of the
   draw color and whatever the multisample buffer held, and the
   admitted-set test refuses it.  The MSAA arm therefore takes
   `r300_tcl_bypass_triangle_sample_set_oracle`, which judges a pixel
   only when every subsample clears the analytic edges by
   `R300_TRIANGLE_SAMPLE_MARGIN`; the center denominator serves the
   single-sample uncleared target alone.  The subsample positions are
   r300g's own (`r300_emit_fb_state_pipelined`) on the 1/12 subpixel
   grid `GB_TILE_CONFIG.SUBPIXEL` selects: `(6,6)` at one sample,
   `(3,9)` and `(9,3)` at two, and `(4,4) (8,8) (2,10) (10,2)` at four.
   The judged footprints at the reference geometry are 1152, 1128, and
   1104 pixels against the center oracle's 1152, pinned against an
   independent enumeration in exact rational arithmetic.  The margin
   carries a second mechanism beyond the blend: the 4x grid's thirds
   meet the slope -2 edge from `(56, 8)` to `(32, 56)` at 64 sample
   positions where the edge function is exactly zero, so those samples
   have no defined side without the hardware's fill rule, and float32
   edge evaluation resolves only 13 of the 64 as on-edge.  The margin
   rule holds the judged counts across margins from 1/64 to 1/16 pixel,
   so the verdict rides neither the fill rule nor the float
   representation.
   One question stands open before the emitter, and one is settled.
   The resolve half's draw semantics resolve from r300g's own working
   path: `r300_simple_msaa_resolve` (r300_blit.c) binds the
   multisample surface that holds the content being resolved as the
   render target and draws a full-target rectangle through
   `util_blitter_custom_color`, whose `NULL` custom blend selects the
   full-RGBA-write-mask blend state and whose fragment shader is
   `BLITTER_FS_CLEAR_COL_ONE_CBUF`.  A fragment color written into that
   surface would destroy the samples the resolve reads, leaving every
   resolve destination the constant color, so the surviving reading is
   that `RB3D_AARESOLVE_CTL.AARESOLVE_MODE_RESOLVE` redirects the color
   backend to `RB3D_AARESOLVE_OFFSET` and emits the downsampled
   samples while the fragment supplies coverage alone.
   `AARESOLVE_CTL.AARESOLVE_ALPHA_SAMPLE0` and `AARESOLVE_ALPHA_AVERAGE`
   corroborate it: both derive the resolve output's alpha from the
   samples.  The argument bounds itself.  `r300_emit_fb_state`
   (r300_emit.c) emits `RB3D_COLOROFFSET0` for the bound surface from
   its own atom whether or not the AA atom sits in resolve mode, so the
   color backend holds a bound color offset and a resolve offset at
   once, and the source establishes that the fragment color reaches no
   destination rather than that the mode retargets the write.  A
   destination receiving a mixture stays live under that reading.  The
   first emitter therefore carries a resolve-half fragment color no
   multisample sample holds and a predicted dword for each of the three
   outcomes -- the downsampled samples, the fragment color, and the
   mixture -- so one submit classifies the semantics instead of
   confirming them.  The destination byte order stays the falsifier
   already recorded, and both readings of it -- linear order and the
   tiled swizzle of the same bytes -- take their predicted dwords before
   the run.  A third destination content is named before the arm runs:
   a mixture, if the fragment write and the sample downsample both
   reach the destination order-dependently, which reads as a finding
   rather than a defect.  The multisample surface is never CPU-read and
   `r300_texture_initial_domain` places an `nr_samples > 1` resource in
   `RADEON_DOMAIN_VRAM` alone, so it takes a device-local allocation
   while the resolve destination stays host-visible for readback.  The
   sample count multiplies the layer size and leaves the stride alone
   (`r300_texture_desc.c`: `layer_size *= base->nr_samples`), so a 2x or
   4x surface at the reference extent is that multiple of the
   single-sample layer with its pitch unchanged.
   The offline cell is emitted:
   `r300_tcl_bypass_triangle_msaa_resolve_emit` opens with `GB_AA_CONFIG`
   and the `GB_MSPOS0`/`GB_MSPOS1` pair, renders the reference triangle
   into the multisample surface, writes the resolve register run --
   `RB3D_AARESOLVE_OFFSET`, the destination's pitch masked to
   `0x3ffe`, and `AARESOLVE_MODE_RESOLVE | ALPHA_AVERAGE` in one
   `PACKET0` with the destination's relocation behind it, the order
   `r300_emit_aa_state` writes -- then covers the whole extent a second
   time into the same surface with a fragment constant no multisample
   sample holds, and closes both the resolve mode and the subsample set.
   `RB3D_AARESOLVE_PITCH` takes a raw pixel pitch in bits 1 through 13:
   r300g masks `r300_surface::pitch`, which is the full
   `RB3D_COLORPITCH0` register word carrying format, tile, microtile,
   and endian bits beside the stride (`r300_texture.c`), so the mask
   extracts the stride and the resolve register carries no format field.
   A resolve emits only for the pixels a fragment covers, which is why
   `r300_simple_msaa_resolve` draws a full-target rectangle; the cell
   reaches the same coverage with one triangle at `(0, 0)`, `(2w, 0)`,
   `(0, 2h)`, whose interior holds every pixel center in the extent, so
   the three-vertex writer serves the resolve half unchanged.  The cell
   binds five relocation sites over four buffer objects: the render
   half's vertices and the multisample surface, the resolve
   destination, then the multisample surface a second time on the
   texture slot and the cover geometry on the composed vertex slot.
   Both the composed and the multisample cell carry five sites, so site
   count no longer selects a single expected slot sequence and the
   validator admits either, with a five-site sequence matching neither
   still refused.  What remains is the recorder and the attended
   runner, and both land.  `r3v_native_record_msaa_resolve` allocates the
   multisample surface itself in `RADEON_GEM_DOMAIN_VRAM` with no
   fallback domain under `RADEON_GEM_NO_CPU_ACCESS`, which the host
   never maps, so a successful create is the placement rather than a
   claim about it, and the recorder's five slots reach four buffer
   objects with the surface's merged entry carrying a VRAM write alone.
   `r3v_native_attended_msaa_resolve` reads the destination through four
   passes of `r300_tcl_bypass_triangle_sample_set_oracle` over one
   denominator -- the downsampled samples, the resolve half's fragment
   constant, the pre-submission seed, and the union -- with a footprint
   census that separates a permuted destination order from an absent
   write without a tiling model.
   The rung holds its silicon receipt in two arms on RS482
   (`docs/hardware/r3v-native-attended-msaa-resolve-procedure.md`;
   bundle steinmarder-r300
   `r3v-native-msaa-resolve-downsample-semantics-rs482`).  The first arm
   refuted the cell: `GB_AA_CONFIG`, both `GB_MSPOS` words, and
   `RB3D_AARESOLVE_CTL` are first-draw contract entries, so a subsample
   set programmed ahead of the contract is written back before the draw
   it was meant for, and the retained stream carried two single-sample
   draws and no resolve while the destination correctly held its
   `0xa5a5a5a5` seed in all 4096 footprint pixels.  The subsample set now
   travels through each half's own contract, which
   `r300_first_draw_params` carries as a declaration.  The second arm
   delivers: `downsample judged=1 interior_exact=1` over 1104 judged
   pixels against 1104 analytic with the `fragment` and `seed` passes
   both zero, `vkQueueSubmit` 0, an empty dmesg delta over an unchanged
   boot, and a 155 us guarded interval against the 1.7 s grace.  So
   `AARESOLVE_MODE_RESOLVE` redirects the color backend to
   `RB3D_AARESOLVE_OFFSET` and emits the downsampled samples while the
   fragment supplies coverage alone -- the reading
   `r300_simple_msaa_resolve` implies, now measured -- and a linear
   resolve destination receives linearly ordered bytes at the 64x64
   pitch-64 `B8G8R8A8` shape, which refutes the tiled-swizzle reading
   there.  Open behind it: the multisample surface takes no clear, so 668
   unjudged exterior pixels carry the resolve half's fragment constant
   and a command-stream clear of that surface is what would judge them;
   the two-sample arm at the 1128-pixel denominator is unrun; and while
   `r100_cs_track_check` sizes the color buffer with no sample term the
   path stays an internal attended cell rather than an advertised Vulkan
   capability;
7. composed render and sampling surfaces before any core image or
   framebuffer limit rises.  The rung depends on the usage union in
   rung 5 rather than on rung 6, so it runs when its own mechanisms
   land and does not wait behind the MSAA entry condition.  Two of its
   three mechanisms already execute: the cell's own prologue and
   epilogue carry the coherency edge, since the sampled cell opens with
   `TX_INVALTAGS` before `TX_ENABLE` and every cell closes with
   `RB3D_DSTCACHE_CTLSTAT` flush-dirty plus free-3D-tags, so a render
   cell followed by a sampling cell in one indirect buffer publishes
   the color writes before the texture fetch reads them; and the
   role-based composer (`r300_pm4_compose.h`) already binds several
   independently emitted fragments into one submission on RS482
   silicon through the fetched producer route.  The composed cell itself
   lands: `r300_tcl_bypass_triangle_composed_render_sample_emit` emits
   the render half, then the sample half whose texture geometry is the
   render half's target -- extent, row pitch, lane order, and byte
   offset -- so the halves cannot disagree about the bytes between them,
   and the concatenation puts the render half's destination-cache flush
   ahead of the sample half's texture-tag invalidate.  The arming runner
   emits it under `--composed`, 485 IB dwords, blake3 `7e1eeb21`, which
   describes the cell in its emitted form.  A submission of that form
   reaches the wrong buffers: the emitter writes each relocation payload
   as its slot number, which holds while every slot names a distinct
   buffer object, and the composed cell's first target is both the
   render half's color slot and the sample half's texture slot, so
   `radeon_drm_vk_reloc_list_add` merges the two into one relocation
   entry as the kernel does and three of the five payloads then name a
   buffer they were not emitted for.
   `r300_tcl_bypass_triangle_bind_reloc_indices` binds the payloads to
   the merged indices, and its test derives the merged map by the winsys
   rule and pins the disagreement before binding.
   `r3v_native_record_composed_render_sample` records the cell over that
   contract: it builds the reference array merged in one pass, the first
   target's entry carrying the write domain the render half needs beside
   the read domain the kernel's texture check needs, and binds the
   payloads to that array's own positions, so the queue's own merge is
   idempotent and the indices the arming digest covers are the indices
   the kernel reads.  Its harness runs on the drm-shim and is calibrated
   against a recorder that skips the binding and one that leaves the
   array unmerged.  The gate reaches the cell: the arming runner binds
   before digesting, so `--composed` reports `247949a2`, the bound
   cell's digest and the one the recorder installs; the queue's geometry
   predicate reads the merged binding rather than falling to the
   unfrozen default; and the harness arms from the offline cell ahead of
   device creation, the order the attended procedure runs in, then
   submits on the shim, with an unbound arm proving the emitted digest
   refuses.  The recorder leaves both vertex payloads to the caller, and
   the two arrays take different layouts -- four-dword position records
   for the render half, eight-dword position-plus-TEX0 records for the
   sample half.  The attended runner lands:
   `r3v_native_attended_composed` seeds both vertex arrays, fills both
   targets with `R300_TRIANGLE_COLOR_SENTINEL`, records through the
   composed recorder, and takes a coverage verdict on each target, with
   `docs/hardware/r3v-native-attended-composed-render-sample-procedure.md`
   carrying the arming, predictions, and falsifiers.  Its predicted
   interior covers both targets because TEX0 at each vertex is that
   vertex's window position over the render extent, so a nearest fetch
   at pixel center reads texel `(x, y)` a half texel from either
   boundary and the sample half reproduces its texture's coverage pixel
   for pixel.  The sentinel is what separates the failure modes: a
   sample interior reading it names a texture fetch ahead of the render
   half's publication, so the coherency edge, rather than the coverage,
   carries a deviation there.  The cell holds the silicon
   receipt: one attended submission on RS482 under the authorization the
   arming report matched on all five declarations, `vkQueueSubmit`
   returning 0, both coverage verdicts reading `judged=1
   coverage_exact=1 canary=1` with 1152 interior pixels against 1152
   analytic and no mismatch, the sample centroid reading the render
   half's draw dword `0xdf20609f`, and an empty dmesg delta over an
   unchanged boot.  The two retained targets are byte-identical across
   the full footprint, so the sample half reproduced the render half
   pixel for pixel rather than at the centroid alone, and the corner and
   canary row both hold the `0xa5a5a5a5` seed.  The falsifier that names
   the coherency edge -- a sample interior reading the seed, which would
   place the texture fetch ahead of the render half's publication -- did
   not fire, so the render half's `RB3D_DSTCACHE_CTLSTAT` flush-dirty
   plus free-3D-tags publishes the color writes before the sample half's
   `TX_INVALTAGS` on this silicon.  The submission ran inside a
   hardware-armed SB600 counter over `DRM_IOCTL_RADEON_CS` through fence
   completion, a 134 us guarded interval against the 1.7 s operational
   grace.  The recording contract behind it lands: a
   command buffer carries `R3V_NATIVE_DEFERRED_DRAW_MAX` render passes,
   each with its own load-op clear, carrier, and vertex execution, and
   the queue executes them in record order.  Two clear-only passes take
   the zero-IB path and execute under the closed submission gate, both
   targets carrying their own `VkClearColorValue`; a pass past the bound
   has no deferred record to fill and refuses.  A second pass that
   records a draw appends its cell to the installed stream through
   `r3v_native_cmd_buffer_append_ib`, which merges the buffer references
   by handle and binds the appended payloads to the merged indices, the
   queue's own merge over the result staying idempotent.  Each half opens
   with its own first-draw contract and closes with the
   destination-cache flush, so no state crosses the boundary -- the
   coherency edge this rung's receipt holds on silicon.  The
   concatenation is the recording's own stream, so no offline emitter
   reproduces the digest the arming gate compares against: the
   `triangle_multi_pass` kind reports its geometry unfrozen and an armed
   submission refuses before any ioctl, while the plan capture and
   replay routes, whose plan binds the exact recorded stream at capture,
   execute it.  Open behind that: an offline emitter for the
   concatenation, which is what would let a two-cell buffer reach an
   attended arming, and the GPU producer route, which composes one
   consumer stream over one carrier and judges one read-back, so a
   second pass beside it refuses by name.

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
