# R3V current program status

This document is the one program-status authority for the R3V native
Vulkan ICD on RS482. It states the revision and evidence boundaries, the
target deployment, the active conformance partition, the latest target
receipt, the DSO and queue-claim modes, the open tasks, and the documents
it supersedes. Other documents point here; the status table lives here
alone and is updated in place when a receipt or task changes.

## Deployment epoch

One epoch table carries every identity a claim in this document binds to.
Each row is one field with one authority; a row moves only when its
authority moves, so a receipt's source, the installed binary, and the
repository head are never collapsed into one "current" revision. The rows
fall in three classes and move on three different events. A repository
head answers `git rev-parse HEAD` and moves on every merge. A running
identity -- the deployed kernel checkpoint, the package version, the
loaded module srcversion, the installed ICD -- answers a command on the
target and moves on a deployment. A receipt identity names what one
retained bundle was captured against and never moves at all; where a
running identity has advanced past a receipt's, the row carries both and
names what carries the receipt forward.

| Field | Value | Authority |
|---|---|---|
| Document revision | `git log -1 -- docs/hardware/r3v-current-program-status.md` | the commit that carries this table |
| Repository head: mesa-26-gororoba | the commit carrying this table (`git log -1 -- docs/hardware/r3v-current-program-status.md`); the receipt's own source is the row below | `git rev-parse HEAD` in the live checkout |
| Repository head: steinmarder-r300 | `377fab045` (live head); the Flat-beside-NoPerspective receipt entered at `915c000fe` and every later commit is additive to it | `git rev-parse HEAD` |
| Repository head: linux-radeon-gororoba | `2be21eaa892723f1c9cd826b7331c7d234e2c1ce` (offline replay authority: `r300_tcl_bypass_vtx_check` and the CS-track controls replay from this head and from the deployed checkpoint) | `git rev-parse HEAD` |
| Repository head: radeon-custom | `9a52df357` (live head; carries the `0.8.13-1` recipe the target now runs) | `git rev-parse HEAD` |
| Repository head: vostro1000-re | `1d621a0f51a7c7fb343f0702f9481390b8240b94` | `git rev-parse HEAD` |
| Route-admission source | the commit carrying this table: Flat location 0 beside NoPerspective location 1 on the mixed reciprocal carrier opens on the interface alone (the probe gate that quarantined it at `3aa3ad02c690` is removed by the receipt's promotion) | `r3v_interpolation_lowering.c`, `docs/hardware/rs482-post-vap-interpolation-pipeline.md` |
| Receipt source | `6089c5b5bb9a9cc5d197c43c386dc38e5a53bb14` (Flat beside NoPerspective on the mixed carrier through host replication; the route under `R3V_NATIVE_FLAT_MIXED_CARRIER_PROBE=1` at that commit) | retained bundle `r3v-native-noperspective-flat-mixed-carrier-receipt-vostro1000_rs485m_5974/identity.txt` and its `flat-mixed-carrier-receipt/mesa_head.txt` |
| Installed ICD source and build-id | profile 4 release: `690bd77e1eb`, `mesa-gororoba 2:26.2.0-21` at prefix `/usr`, build-id `b03d48e40fdd31379f946ac37c010e9fb656777f`, package sha256 `ae9dd914f26e675e820c19de8ecf1ef2fdf4344d2835b25b99bc3607848627aa`, ICD manifest sha256 `73f71a02ee887ce0861ecd527e7285933a7ddfc31e4d78ea870676ddd1e3e6fb` (package and installed DSO identical; the GL renderer string reports `Mesa 26.2.0-devel (git-690bd77e1e)`). This binary opens the Flat-beside-NoPerspective route on the interface alone, the promotion the receipt below authorized. The receipt's own installed binary stays `6089c5b5bb9`, `mesa-gororoba 2:26.2.0-20`, build-id `c3ba996b9fc75c04cfad2d2a3638c79cdfbb5092`; the statically linked runners come from the reproducible profile-4 qualification builddir at that commit, build-id `68dc7a2cf42d76a5bda1a0233c0ac21d7c8b9f76` | `pacman -Q`, `readelf -n` build-id across package and installed DSO; the receipt's `icd-pkgrel20/` leg |
| Attended-runner source and digest | `6089c5b5bb9`; `r3v_native_attended_rs_tex_adj_probe` sha256 `71e664ed6221eb0805e1f5ba19caeab67f4e4292a817dc039035afe3726f6047`, `r3v_native_arming_runner` sha256 `d41751f350e647565d2537b1778a2af2a4fd716c3a34c48163dd84ee65c8f075` | the receipt's `flat-mixed-carrier-receipt/runner_sha256.txt` |
| Deployed kernel source checkpoint | `2be21eaa892723f1c9cd826b7331c7d234e2c1ce`; the interpolation receipt cycle (Latest target receipt and its predecessors through the Flat two-draw cell) was taken against `0104ede3f1964cc844f9f1839cb6953e2639c4e6` (linux-radeon-gororoba branch `radeon/r300_tcl_bypass_color0_width`, COLOR0 width admission) and holds across the move by the admission identity stated under the table; each earlier receipt names its own checkpoint where it is recorded | `radeon-custom` PKGBUILD `_source_commit`; on-target `source-identity.toml` |
| Package version | `radeon-unified-dkms 0.8.13-1` (radeon-custom `9a52df357` PKGBUILD), built for `7.1.8-1-cachyos` and `6.18.42-1-cachyos-lts` with `lockup_timeout=0` from the package-owned board policy; the interpolation receipt cycle was taken under `0.8.12-1`, and the conformance-plan and render-shape sessions under `radeon-rs482-policy 0.8.11-1`; policy package as `pacman -Q radeon-rs482-policy` reports on the box | `pacman -Q` on the target |
| Loaded module srcversion | `46C05689F2C98A526C314F4`; the interpolation receipt cycle was taken under `729892A3F3530EB12B8D842`, the conformance-plan and render-shape sessions under `727CE89E79FB2D14663C381`, and the exact identity-carrier route record binds to `088E045518D972727C1DD1C` | `/sys/module/radeon/srcversion` on the target |
| Retained-evidence commit | `steinmarder-r300` `915c000fe` (the checkout in which `sha256sum -c bundle_hashes.sha256` passes for `r3v-native-noperspective-flat-mixed-carrier-receipt-vostro1000_rs485m_5974`) | evidence verification checkout |
| dEQP source | `43c65c132` (`deqp-vk` `26d43d452e64`, release `opengl-cts-4.6.8.0-414-g43c65c132`) | installed `deqp-vk` release identity and corpus pin |

Evidence binds to profile 4: the installed ICD, the attended runner, and
every receipt come from the `4_r300_full_release` build through
`make -C build-infra`; profile 3 is the asserts-live diagnostic build and
profile 5 (GCC) the wider-warning diagnostic when common R300 code changes.

The receipts captured under `0104ede3f196` survive the move to
`2be21eaa8927` by source identity rather than by replay. Across the two
pins `r100.c`, `r300.c`, `radeon_cs.c`, `rs400.c`,
`r300_tcl_bypass_vtx_check.h`, `replay_r300_cs_track.c`, and
`replay_r300_tcl_bypass_ib.c` carry equal git blob hashes, so every
command-stream admission predicate a cell was judged by is
byte-identical, the synthesized-lane vertex predicate in the header
included. Three commits separate the pins: one docs,
one CS-track control script, and `8cc1692` "radeon: make GTT compaction
reservation-safe", which touches `radeon_object.c` alone;
`radeon_gtt_compact` has one caller, the `-ENOMEM` GTT retry in
`radeon_gem_object_create`, so a single-cell submission on an unexhausted
512M aperture never reaches the changed code. Source equality transfers
an admission conclusion; it does not produce a current-epoch silicon
observation, and a claim that needs one names its own run.

## Target deployment

| Field | Value | Authority |
|---|---|---|
| Host | `cachyos-vostro1000` (Dell Vostro 1000, AMD K8) | `docs/hardware/vostro1000-kernel-modules.md` |
| GPU | PCI `1002:5974` (RS482/RS485 die id), subsystem `1028:022a`, DMI `Vostro 1000`: the Radeon Xpress 1150 / RS485M product; `CHIP_RS480`, renderer `ATI RS480`; retained evidence sealed under the historical alias `rs482` | `r300_platform_identity_lookup`, `include/pci_ids/r300_pci_ids.h` |
| Kernel | `7.1.8-1-cachyos`, module and package per the epoch table | epoch table |
| dEQP | per the epoch table; bundle on the box at `deqp-vk-bundle` | receipt `deqp` |
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
`r3v_conformance_nonpass_ledger.tsv` (row count and every numeric claim
audited by `r3v_conformance_ledger_audit.py`); an unclassified row
marks the result invalid for qualification. The machine-readable frontier
over the 21 slices -- counts, hazard, evidence status, blocking non-pass classes
joined to their ledger rows, and the next admitting mechanism -- is
`steinmarder-r300/src/re/r300/corpora/rs482_r3v_conformance_frontier_v1/frontier.jsonl`,
regenerated from this partition, the ledger, and the retained bundles by
`build_rs482_r3v_conformance_frontier.py`; the counts in this document
cite that corpus rather than restating it. The ten slices after the eleventh (`api-unclassified`,
`api-query-surface`, `feature-extensions`, `memory-unclassified`,
`memory-query-surface`, `pipeline-monolithic`, `pipeline-variants`,
`robustness-extended`, `shader-execution`, `wsi-presentation`) carry no
target run.

Under closed submission gates no case in slices 6-10 reaches an IB: the
per-case status of every silicon shard equals its drm-shim host-model
counterpart (416,370 of 416,370 for slices 7-10), so those runs are
qualification claims about the closed-gate surface, and a slice's
first real submission waits on a planning pass that lands transcripts,
`r3v_native_plan_tool compose`, per-case plans, and the human gate.

## Latest target receipt

| Field | Value |
|---|---|
| Bundle | `steinmarder-r300/src/re/r300/results/r3v-native-noperspective-flat-mixed-carrier-receipt-vostro1000_rs485m_5974` |
| Evidence class | silicon; attended semantic cell of the public Flat-beside-NoPerspective interface on the mixed reciprocal carrier, the route quarantined behind `R3V_NATIVE_FLAT_MIXED_CARRIER_PROBE=1` at the receipt source and public at the promotion commit; this receipt makes no CTS qualification claim |
| Cell | two-draw over the unclipped probe triangle: Smooth vec4 control varying cell, then the candidate on `R3V_INTERPOLATION_ROUTE_MIXED_RECIPROCAL_CARRIER` with `flat_mask` `0x1` -- TC0 carries vertex 0's `(s, t, r, q)` replicated to every record by the post-VS stage, TC1 carries each vertex's own `(s, t, r, q) * c`, TC2 carries `(c, 0, 0, 1)` with `c = w / max w = (1, 0.25, 0.5)`; 494 IB dwords, byte-identical to rung D; pass 1 differs from the control in 67 dwords, first at index 399 register `0x2150` `VAP_PROG_STREAM_CNTL_0` (`0x26030003` -> `0x06030003`); `VAP_VTX_SIZE` 8 on draw 0 and 16 on draw 1 |
| Verdict | control target perspective 882/882 judged interior pixels (max deviation 1); candidate target affine 882/882 (max deviation 1), perspective 0, unchanged 0; per-channel separation over the judged set 0 / 0 / 501 / 882, red and green constant by construction because the provoking record's Flat value reaches every fragment; carrier witness exact on both passes |
| Source | Mesa `6089c5b5bb9a9cc5d197c43c386dc38e5a53bb14`, one commit for the attended runner, the arming runner, and the installed ICD; `r3v_native_attended_rs_tex_adj_probe` sha256 `71e664ed6221eb08...6047`, `r3v_native_arming_runner` sha256 `d41751f350e64756...f075` |
| Submission | cell BLAKE3 `522066cd89ba60ea...7c2a`, recorded and emitted digests equal; `vkQueueSubmit` 0; one guarded `DRM_IOCTL_RADEON_CS` through fence completion (90 us) inside an armed SP5100 TCO bracket, disarm verified |
| Runtime | installed `mesa-gororoba 2:26.2.0-20` at prefix `/usr`, build-id `c3ba996b9fc75c04cfad2d2a3638c79cdfbb5092`; kernel `7.1.8-1-cachyos`; radeon srcversion `729892A3F3530EB12B8D842`; `radeon-unified-dkms 0.8.12-1` unchanged, checker checkpoint `0104ede3f196` replayed in preflight beside the linux-radeon head; dmesg delta 0; boot id unchanged; Xorg concurrent on tty7 is the recorded deviation |
| Oracle | the probe census (`r300_rs_tex_adj_probe.h`): 882 judged interior pixels, tolerance 2, model separation 5 UNORM8 quanta per channel; the predictions self-classify ahead of the ioctl, and rung D's interpolated image is the decisive falsifier because a pure-affine image aliases the prediction under replicated Flat inputs; six known-bads refuse with exit 2 in preflight, the absent probe gate among them |
| Integrity | retained by `steinmarder-r300` `915c000fe`; every entry in `bundle_hashes.sha256` verifies there |

The receipt proves Flat at location 0 beside NoPerspective at location 1
on RS485M through host replication: the post-VS stage rewrites the
provoking record's varying onto every record ahead of the clipper and
the reciprocal packing, so a clipped edge between two equal records
yields the same record and the US divides the carrier back out.
Replication composes with the texture-path routes for that reason,
while `GB_SELECT.W_SELECT` and `GA_COLOR_CONTROL` are whole-draw and
per-primitive words a second varying kind cannot share
(`docs/hardware/rs482-post-vap-interpolation-pipeline.md`).  The
promotion commit removes the probe gate; the four-vector RS budget
boundary and wider mixed interfaces carry no claim until their own
receipts land.

The preceding target receipt is the public partial-clip fallback:

| Field | Value |
|---|---|
| Bundle | `steinmarder-r300/src/re/r300/results/r3v-native-noperspective-public-partial-clip-fallback-receipt-rs482` |
| Evidence class | silicon; attended semantic cell of the public full-vec4 NoPerspective pipeline on the adaptive route with every probe gate, the force gate, and the R2VB gates unset; this receipt makes no CTS qualification claim |
| Cell | two-draw over the probe triangle crossing `x = -w`: Smooth vec4 control varying cell, then the NoPerspective pipeline created on `R3V_INTERPOLATION_ROUTE_W_SELECT_OR_RECIPROCAL_CARRIER` with the clipping class deferred; `r3v_native_cmd_buffer_select_deferred_routes` judged the draw PARTIAL at `vkQueueSubmit` and spliced the TC1 reciprocal-carrier cell over the direct cell's span ahead of the arming digest (486 IB dwords, byte-identical to the forced rung B cell; VAP_VTX_SIZE 8 then 12; pass 1 differs from the control in 65 dwords, VAP_PROG_STREAM_CNTL_0 first) |
| Verdict | control target perspective 1296/1296 judged pixels of the clipped fan; candidate target affine 1296/1296 (max deviation 1), perspective 0, unchanged 0; witness fans live 6, exact 6 in both passes with `c = (1, 0.25, 0.5)`; the preflight record-only of the same production pipeline over the unclipped triangle selected the direct GB W_SELECT cell (BLAKE3 `32d547e9`, 472 dwords, the rung A public cell) |
| Source | Mesa `3384c3d1aad2ac5983193913e7734f2eaa41404e`; statically linked runner sha256 `4720bbb6e2f5...b3c1` |
| Submission | cell BLAKE3 `6b6026ea718e...421c`; `[route] selected=reciprocal-carrier`; `vkQueueSubmit` 0; one guarded `DRM_IOCTL_RADEON_CS` through fence completion (112 us) |
| Runtime | kernel `7.1.8-1-cachyos`; radeon srcversion `729892A3F3530EB12B8D842`; `radeon-unified-dkms 0.8.12-1` unchanged (PASS at VAP_VTX_SIZE 8 and 12, 8 REJECTs the carrier draw); dmesg delta 0; boot id unchanged |
| Oracle | the probe census (`r300_rs_tex_adj_probe.h`): 1296 judged interior pixels, tolerance 2, model separation 5 quanta; the predictions self-classify ahead of the ioctl; four known-bads refuse with exit 2 in preflight (force gate, W_SELECT probe gate, R2VB gate, `--production` on the unclipped carrier candidate) |
| Integrity | retained by `steinmarder-r300` `834d87d14`; every entry in `bundle_hashes.sha256` verifies there |

The receipt proves the public partial-clip fallback on RS482: the
NoPerspective pipeline retains both qualified cells and the clipping
class is decided at submission after the CPU vertex execution, so a
triangle the clipper accepts whole runs the direct GB W_SELECT cell and
a triangle a clip plane cuts runs the TC1 reciprocal carrier, each
byte-identical to its rung's retained stream
(`docs/hardware/r3v-noperspective-reciprocal-carrier-design.md`).  The
mixed receipt (`r3v-native-noperspective-mixed-carrier-receipt-rs482`,
Mesa `39956c9e60e`, cell BLAKE3 `522066cd`, red/green perspective and
blue/alpha affine 882/882) and the q-lane receipt
(`r3v-native-noperspective-q-lane-carrier-receipt-rs482`) stand beside
it; Flat beside Smooth and NoPerspective, the four-vector RS budget
boundary, and wider mixed interfaces carry no claim until their own
receipts land.

The preceding target receipts are
`r3v-native-noperspective-partial-clip-carrier-receipt-rs482` (Mesa
`b1eb5caf80695371176af8b65f37c72c58a12ebd`, cell BLAKE3 `6b6026ea718e...421c`,
the forced TC1 carrier across the clipper, affine 1296/1296) and
`r3v-native-noperspective-production-route-receipt-rs482` (Mesa
`ac20ebc8bdba2c21f548ba3d9f1e286ba06ce961`, cell BLAKE3 `32d547e9fb06...4afb`,
472 IB dwords): the public NoPerspective pipeline on the direct GB
W_SELECT route, affine 882/882 against a perspective 882/882 control,
retained by `steinmarder-r300` `7467cce8c`.

The preceding target receipt is
`r3v-native-public-flat-color0-two-draw-first-delivery-rs482` (Mesa
`42ff2b207c8dedb0a789639bd1c4cd6159b07690`, cell BLAKE3 `3646c222b6c5...605c`,
452 IB dwords): end-to-end Vulkan `Flat` RGB and alpha through the RS482
GA color-0 provoking-vertex selection, both targets byte-equal to their
provoking-vertex images, later replayed byte-equal under
`radeon-unified-dkms 0.8.12-1`
(`r300-tcl-bypass-vtx-check-color0-width-transition-rs482`).

The latest CTS qualification receipt remains
`r3v-native-smoke-triangle-plan-replay-first-silicon-pass-rs482`: Mesa
`133f7703713910fed6b3f3c545dd1bf08a60395c`, one 231-dword triangle IB,
receipt seal `c68d24e2957a...`, valid `Pass`, and dmesg delta 0. The
prediction for `dEQP-VK.api.smoke.triangle` was Fail on image comparison
because the noop-shim host-planning pass rasterizes nothing; RS482
produced Pass. That deviation refutes the `driver_defect_open` ledger
instance for `dEQP-VK.api.smoke.triangle` alone. The other
`driver_defect_open` rows remain open. The six plan mutations (order,
runtime, digest, relocation, source, nonce) each refuse before any
`DRM_IOCTL_RADEON_CS`, and the offset and module-constant render-shape
receipts close in the same bundle.

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
| Verdict | `classified_nonpass`; qualification invalid; `evidence_class` host-planning |
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
| Verdict | `classified_nonpass` (`image_outside_executed_envelope`); qualification invalid; `evidence_class` host-planning; seal `6e3ed951359d` |
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
the case passes on RS485M (bundle
`r3v-native-smoke-triangle-plan-replay-first-silicon-pass-rs482`).

## Draw, synchronization, and transfer host-planning pass

| Field | Value |
|---|---|
| Cases | 144,837 over eight shards: draw.0000-0001, synchronization.0000-0003, transfer.0000-0001 |
| Binding | `shard` on all eight; declared and observed case counts agree per shard, 0 duplicates, 0 `not_run`, 0 unresolved |
| Driver source | `41e03d256e4b`, profile 4 release under `REPRODUCIBLE_RUN=1`, DSO BLAKE3 `d7e7f38e58d2aeef...`, SHA-256 `c6d972310d49dcf4...` |
| Runner source | `53be67210832`, whose `reap_finished_children` is what lets a long shard finish |
| Verdict | `unclassified_nonpass` on all eight; the runner exits 0 only for `pass` or `pass_with_accepted_nonpass`, so exit 1 is the non-pass convention |
| Results | NotSupported 143,898, Fail 693, Pass 246 |
| Outcomes | `no_nonempty_ib` 144,836, `transcript` 1 |
| CS witness | 0 `DRM_IOCTL_RADEON_CS` in every one of 144,837 per-case straces |

One case in the corpus reaches a nonempty submission:
`dEQP-VK.synchronization.smoke.fences`, one submission of 231 dwords, cell
kind `triangle`, emitter `r3v`, three relocations (vertex read 4096,
color write 263168, completion write 4). Its plan composes, checks, and
replays on the target under the radeon noop drm-shim with every
live-submission gate closed: the case passes, the session records the
submission admitted, and the retained IB is 924 bytes whose BLAKE3
recomputes to the plan's submission digest. Hardware authorization stays
open.

A plan binds one running identity, so a plan replay needs a host carrying
a loaded radeon module: `r3v_native_plan_replay_bind` reads the module
srcversion through the arming provider, and a host with no radeon module
refuses at `R3V_NATIVE_PLAN_BIND_MODULE` before any submission.

Four plan fields are declarations rather than bindings. The bind takes
`deqp_sha256`, `deqp_release`, `partition_sha256`, and `caselist_sha256`
from the plan itself, since the driver cannot observe them, and a
mutation ladder confirms a plan carrying any of the four wrong still
binds and passes. The receipt verifies those four outside the driver.

### Ranked first refusals

The census keys a refusal on the `file:line` the CTS names, and on the
predicate itself when the CTS names none. The families below are the
sited addresses, each one predicate:

| Cases | Family |
|---|---|
| 60,034 | `VK_ANDROID_external_format_resolve`, an extension this platform does not carry |
| 31,306 | external semaphore and memory capabilities, which radeon backs with no `DRIVER_SYNCOBJ` |
| 24,873 | format coverage over ten addresses, blit source and destination and image formats |
| 5,888 | queue-family count |
| 5,500 | timeline semaphore |
| 4,643 | `shaderFloat16` |
| 2,452 | `geometryShader` |

Beside them, 4,490 cases refuse on a vertex-input format across eighty
distinct `VkFormat` values; those predicates name no `file:line`, so they
rank by predicate rather than by address.

The first two families are parked outside the implementation frontier:
one names an Android-only extension, the other a kernel capability the
radeon UAPI does not expose. Format coverage is the largest family a
Mesa change moves.

### The vertex-input row is gated by pipeline admission

The vertex path executes on the host, so a normalized or half-precision
vertex format costs a decode rather than a hardware capability, and
r300_vertex_format_semantics carries sixteen such formats through the
gather. That decode is not the gate. Granting those formats
VK_FORMAT_FEATURE_2_VERTEX_BUFFER_BIT and running the 341 cases whose
recorded first refusal names one of them measures the difference:

| Driver | Pass | Fail | NotSupported |
|---|---|---|---|
| granting the sixteen | 0 | 213 | 128 |
| withholding them | 0 | 0 | 341 |

Every one of the 213 refuses at vkCreateGraphicsPipelines. Pipeline
admission separately weighs the shader stages, the vertex-input shape,
the fixed state, the descriptor layout, and the render-pass cell, so a
format the gather decodes still refuses there, and advertising ahead of
that gate turns a withheld feature into a blocking failure. The grant
therefore covers the four F32 formats a draw executes, and the row's
binding constraint is the pipeline admission gate rather than the format
table. The integer members carry a second constraint: an integer vertex
input variable reaches no value kind in r3v_vertex_spirv.c, whose
OpConvertSToF admits a loaded system value alone.

### R8G8B8A8_UNORM

`r3v_native_render_lane_order` grants both render lane orders, so source
admission is closed. Of the 11,902 cases in this pass whose names carry
`r8g8b8a8`, none refuses at `vkCreateImage`: every one stops at an
earlier CTS predicate, 4,680 at Android external format resolve, 3,520
at external semaphore capabilities, 715 at a blit source format, 640 at
a queue-family count, 546 at image-format support, and 300 at a timeline
semaphore. Widening the image format set moves none of them, and a
separately authorized public-loader receipt at a receipt-pinned extent is
a different objective from moving these dEQP cases, which never reach
that route.

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
  class `host-model`, and is never valid for qualification;
- host-planning: a planning pass on a submission-hazard slice under the
  radeon drm-shim with every gate closed, one process apiece, and a
  per-process strace witnessing zero kernel-entering CS ioctls; the
  runner admits it as evidence class `host-planning`, never as qualification
  evidence, and seals each case's outcome (`transcript` with digests, or
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

Delivered prerequisites:

- the first dEQP transcript is delivered: `dEQP-VK.api.smoke.triangle`,
  the one-case bridge from dEQP semantics to the exact-plan hardware gate,
  produces a valid qualification pass on RS482 through the plan replay (see the
  latest target receipt above); this prerequisite stays closed, and the open
  target-run blockers start under P0 below. It reached a nonempty IB after the
  five render-family elements landed.  Those elements decompose into three
  evidence
  classes rather than five silicon receipts: the lane order, the extent
  with its pitch, and the fragment constant are each one register class
  of the qualified triangle cell (`r300_triangle_render_shape`,
  `common/r300_tcl_bypass_triangle.h`), so one attended session with one
  arm per parameter plus the composed smoke shape earns all three; the
  render-family transfer readback is a host route over the linear color
  BO with no executed change; OPTIMAL admission is a pure admission fact.
  The attended render-shape session ran on RS485M
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
  qualification-valid `Pass`: the plan binds this box's source, DSO, kernel,
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

- the compute recognizer index-from-UBO shape (111 robustness cases; the
  host-planning pass proves no robustness case reaches an IB before it);
- `pipeline_barrier_executing_route_gap`: the secondary replay, the
  nearest scaling blit, and the sampled-image admission moved the family
  4 -> 28 Pass under the shim and all three movements hold on RS485M
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

## Expansion dependency graph

Conformance expands by executed mechanism. The numbered rows provide stable
navigation identities rather than a total order, and each row opens when its
named dependencies hold silicon evidence. The one-case plan/replay chain above
is rung zero.

The source locators bind each status claim to the inspected tree. Each command
records the exact `(rg --fixed-strings)` discovery technique required to
reproduce the locator.

| Symbol | Definition | Discovery command |
|---|---|---|
| `r3v_CmdExecuteCommands` | `src/amd/r300/vulkan/r3v_native_recording.c` | `rg -n --fixed-strings 'r3v_CmdExecuteCommands' src/amd/r300/vulkan/r3v_native_recording.c` |
| `r3v_CmdBlitImage` | `src/amd/r300/vulkan/r3v_native_recording.c` | `rg -n --fixed-strings 'r3v_CmdBlitImage' src/amd/r300/vulkan/r3v_native_recording.c` |
| `r3v_native_view_type_executes` | `src/amd/r300/vulkan/r3v_native.h` | `rg -n --fixed-strings 'r3v_native_view_type_executes' src/amd/r300/vulkan/r3v_native.h` |
| `r3v_native_memory_type_policy` | `src/amd/r300/vulkan/r3v_native_memory.c` | `rg -n --fixed-strings 'r3v_native_memory_type_policy' src/amd/r300/vulkan/r3v_native_memory.c` |
| `r300_texture_initial_domain` | `src/gallium/drivers/r300/r300_texture.c` | `rg -n --fixed-strings 'r300_texture_initial_domain' src/gallium/drivers/r300/r300_texture.c` |
| `r300_emit_fb_state` | `src/gallium/drivers/r300/r300_emit.c` | `rg -n --fixed-strings 'r300_emit_fb_state' src/gallium/drivers/r300/r300_emit.c` |
| `r300_tcl_bypass_triangle_msaa_resolve_emit` | `src/amd/r300/common/r300_tcl_bypass_triangle.c` | `rg -n --fixed-strings 'r300_tcl_bypass_triangle_msaa_resolve_emit' src/amd/r300/common/r300_tcl_bypass_triangle.c` |
| `r300_tcl_bypass_triangle_composed_render_sample_emit` | `src/amd/r300/common/r300_tcl_bypass_triangle.c` | `rg -n --fixed-strings 'r300_tcl_bypass_triangle_composed_render_sample_emit' src/amd/r300/common/r300_tcl_bypass_triangle.c` |
| `r300_tcl_bypass_triangle_bind_reloc_indices` | `src/amd/r300/common/r300_tcl_bypass_triangle.c` | `rg -n --fixed-strings 'r300_tcl_bypass_triangle_bind_reloc_indices' src/amd/r300/common/r300_tcl_bypass_triangle.c` |
| `r300_tcl_bypass_triangle_multi_pass_emit` | `src/amd/r300/common/r300_tcl_bypass_triangle.c` | `rg -n --fixed-strings 'r300_tcl_bypass_triangle_multi_pass_emit' src/amd/r300/common/r300_tcl_bypass_triangle.c` |
| `r3v_native_record_msaa_resolve` | `src/amd/r300/vulkan/r3v_native_cell.c` | `rg -n --fixed-strings 'r3v_native_record_msaa_resolve' src/amd/r300/vulkan/r3v_native_cell.c` |
| `r3v_native_record_composed_render_sample` | `src/amd/r300/vulkan/r3v_native_cell.c` | `rg -n --fixed-strings 'r3v_native_record_composed_render_sample' src/amd/r300/vulkan/r3v_native_cell.c` |
| `r3v_native_cmd_buffer_append_ib` | `src/amd/r300/vulkan/r3v_native_cmd.c` | `rg -n --fixed-strings 'r3v_native_cmd_buffer_append_ib' src/amd/r300/vulkan/r3v_native_cmd.c` |

1. secondary command buffer execution -- depends on rung zero and is closed:
   `r3v_CmdExecuteCommands`
   replays a secondary's recorded host-executed ops (deferred copies,
   event ops, query ops) into the primary in recorded order, and the
   moved `pipeline_barrier` subgroups meet the RS485M silicon qualification
   requirements (8/8 Pass, dmesg delta 0, gates closed, seal `13b4b86a37c2b2`,
   bundle steinmarder-r300
   `r3v-native-secondary-replay-pipeline-barrier-first-silicon-pass-rs482`);
   the replay also closed an update-buffer aliasing double free by
   taking an owned copy of the replayed bytes;
2. the bounded image-blit route -- depends on rung zero and is closed:
   `r3v_CmdBlitImage` lowers unequal-extent rectangles onto a nearest
   resample executor. The Vulkan 1.0 `vkCmdBlitImage`
   [coordinate-transformation and filtering rule](https://registry.khronos.org/vulkan/specs/1.0-extensions/html/vkspec.html#vkCmdBlitImage)
   derives each source coordinate from the destination texel center, the
   source-to-destination extent ratio, and both region offsets, then samples
   with the supplied filter. For positive, zero-origin rectangles, the source
   coordinate reduces to `(d + 0.5) * srcExtent / dstExtent`; the executing
   route supplies `VK_FILTER_NEAREST`, uses distinct images, and refuses flips.
   The twelve moved subgroups produce valid qualification passes on RS485M silicon
   (12/12 Pass, dmesg delta 0, gates closed, seal `765a0687a232c2`,
   bundle steinmarder-r300
   `r3v-native-scaling-blit-pipeline-barrier-silicon-pass-rs482`);
3. fragment sampling through a real descriptor-set binding -- depends on rung
   zero and is closed:
   the sampled cell binds a uniform R8G8B8A8 texel through a
   combined-image-sampler descriptor set and a TCL-bypass triangle
   samples it on TX unit 0; the attended run on RS485M rendered the
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
4. sampled-image shapes -- depend on the executing routes in rows 2 and 3.
   The first shape is closed: the B8G8R8A8_UNORM sampled lane
   order rides the swapped TX_FORMAT1 select set and rendered the
   predicted centroid on RS482 with the byte-X falsifier absent (cell
   blake3 `640c1336`, bundle steinmarder-r300
   `r3v-native-sampled-bgra-lane-order-silicon-pass-rs482`), and a
   split-row texture separates an addressed fetch from a constant one:
   two oracle pixels read the texels at texel rows 6 and 11 as predicted
   on RS482. This receipt proves varying-derived T-axis row addressing
   alone (bundle steinmarder-r300
   `r3v-native-sampled-split-row-addressing-silicon-pass-rs482`).
   S-axis column-dependent addressing remains open. Its discriminator uses
   distinct columns within one row, predicts the exact fetched dword for each
   column, and names a constant-column fetch as the falsifier. Nearest filtering
   and clamp-to-edge remain separate sampler-state mechanisms;
   the rung's conformance movement is zero: the `pipeline_barrier`
   family stands at 28 Pass before and after both shapes, so they widen
   the executed envelope and carry no case.  The `object_management`
   sampled population stays at 90 `vkCreateImage` refusals because its
   `img2D` shape requests `arraySize = 12` together with
   `SAMPLED_BIT | COLOR_ATTACHMENT_BIT`, so multi-layer arrays and the
   sampled-plus-color usage union each leave the other refusing; texture
   extents past the reference geometry stay open;
5. image types, arrays, cube, depth, and larger render extents depend on the
   executing sampled-descriptor route in row 3. Each is a
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
   ledger row `layered_view_type_route_absent` carried until creation
   admitted the array view types and the population measured Pass in this
   row, retiring the ledger entry.  The layer count the render
   family admits answers to the cell's `RB3D_COLOROFFSET0` ceiling,
   which the creation admission and the format query both name, while
   the sampling family reaches the reported device limit because
   `TX_OFFSET_0` carries the full span.  The sampling family holds the
   silicon receipt: the attended arms `layer`, `row1`, and `wide`
   (`r3v_native_sampled_arms.h`, digests `4afc72c0`, `83063087`, and
   `575c6747`) each submitted one live `DRM_RADEON_CS` on RS485M and read
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
   attachment arm of `r3v-native-public-surface` pin.  Measured
   movement on RS485M (steinmarder-r300
   `r3v-object-management-view-rows-measured-rs482`): `image_3d`,
   `image_view_3d`, `image_view_1d_arr`, and `image_view_2d_arr` each
   read 6 passed, 0 failed, 4 not supported at mesa `f743d0f9bec`;
   `image_view_cube` read 6 failed at `vk.createImage` there, since the
   CTS builds that image with `SAMPLED | COLOR_ATTACHMENT` usage and the
   attachment family refused every create flag, and reads 6 passed, 0
   failed, 4 not supported at `fcf09c5b472` with the cube flag admitted
   in that family.  The four not-supported cases per population are the
   CTS's own queue-family and privateData gates.  Open inside the rung: the
   render family's own layer ceiling answers to the creation gate rather
   than to a run, and the `framebuffer` sub-population keeps its refusal,
   needing an attachment route no cell executes;
6. the native 2x and 4x MSAA path depends on rung zero's qualified render
   cell and precedes any sample-count limit increase. The row decomposes into
   three mechanisms, and the register and kernel contracts are read. The
   multisample color surface uses a sample-expanded allocation
   (`layer_size *= nr_samples`, `r300_texture_desc.c`), while the render
   family's load-op clear is a host fill of a CPU-mapped allocation. The MSAA
   target therefore uses the strict device-local recorder path and a
   command-stream clear. `r100_cs_track_check` sizes the color buffer
   as `pitch * cpp * maxy` with no sample term, so the kernel validates
   nothing about the sample expansion and the footprint proof lives
   entirely in the driver.  That bounds what the rung can claim: while
   the parser footprint carries no sample multiplier, the multisample
   path stays an internal attended cell rather than an advertised
   Vulkan capability, and it rises to a public capability once the
   parser incorporates the multiplier or the driver-side bound is
   established independently. [R3V Vulkan memory model over Radeon
   DRM](r3v-vulkan-memory-model-over-radeon-drm.md) owns the Vulkan memory-type
   policy and the kernel's realized fallback-placement rules. This status row
   retains the stricter recorder fact alone:
   `r3v_native_record_msaa_resolve` requests
   `RADEON_GEM_DOMAIN_VRAM` without a fallback domain under
   `RADEON_GEM_NO_CPU_ACCESS`, and the host never maps the multisample surface.
   A successful create satisfies that recorder's strict placement precondition.
   The resolve is `RB3D_AARESOLVE_OFFSET`,
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
   rather than a defect. The strict recorder keeps the multisample surface
   host-unmapped while the resolve destination stays host-visible for readback.
   The sample count multiplies the layer size and leaves the stride alone
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
   runner, and both land. `r3v_native_record_msaa_resolve` uses the strict
   allocation above, and the recorder's five slots reach four buffer
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
   there.  The two-sample arm (cell blake3 `84038889`, six dwords from
   the 4x cell, mesa main `f5643f6898e`) reproduced the verdict over its
   1128-pixel denominator with a 129 us guarded interval, so both sample
   counts the hardware exposes are measured on this shape.  The cleared
   arms (a cover draw under the subsample set leading the stream, cells
   `a0b1c429` at 4x and `9413361a` at 2x, mesa main `f743d0f9bec`) judge
   the exterior too: 2896 and 2920 fully exterior pixels read the clear
   color exactly, the fragment constant reaches zero pixels, and the
   uncleared arms' exterior contents are settled as inherited allocation
   bytes the resolve read through.  Open behind it: while
   `r100_cs_track_check` sizes the color buffer with no sample term the
   path stays an internal attended cell rather than an advertised Vulkan
   capability;
7. composed render and sampling surfaces before any core image or
   framebuffer limit rises. The row depends on the usage union in row 5 and
   bypasses row 6, so it runs when its own mechanisms
   land and does not wait behind the MSAA entry condition.  Two of its
   three mechanisms already execute: the cell's own prologue and
   epilogue carry the coherency edge, since the sampled cell opens with
   `TX_INVALTAGS` before `TX_ENABLE` and every cell closes with
   `RB3D_DSTCACHE_CTLSTAT` flush-dirty plus free-3D-tags, so a render
   cell followed by a sampling cell in one indirect buffer publishes
   the color writes before the texture fetch reads them; and the
   role-based composer (`r300_pm4_compose.h`) already binds several
   independently emitted fragments into one submission on RS485M
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
   receipt: one attended submission on RS485M under the authorization the
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
   coherency edge this rung's receipt holds on silicon.
   `r300_tcl_bypass_triangle_multi_pass_emit` reproduces the
   concatenation offline: two render-shape cells, the second bound to
   the merged indices the winsys first-add rule assigns (a shared page
   or target keeps index 0 or 1, an own one takes the next unused
   index, vertex before color), with every role alias and skipped index
   refused at the binding.  The public two-draw command buffer
   reproduces that stream dword for dword in `r3v-native-public-surface`
   once the emitter is told the pipeline's fragment constant, so the
   `triangle_multi_pass` kind reports its geometry frozen -- two to four
   merged references, each a vertex page read alone or a color target
   written alone, every deferred draw executed -- and the digest decides
   at the gate.  `r3v_native_record_multi_pass` records the cell through
   the install-then-append primitives the public route takes;
   `r3v-native-multi-pass-cell-{bound,shared,alias,mutated-flush,
   mutated-second-state}` pin the recording contract, the armed
   admission, and the refusals of an aliased role and of digests naming
   a stream the recorder never installs, and
   `r300-multi-pass-cs-track-replay` walks the stream through the kernel
   parser over a four-entry relocation list.  The attended runner
   `r3v_native_attended_multi_pass` and its procedure
   (`docs/hardware/r3v-native-attended-multi-pass-procedure.md`) carry
   the one two-draw submission, delivered on RS482 at mesa `64fa102e611`
   (steinmarder-r300
   `r3v-native-two-pass-concatenation-first-delivery-rs482`): both
   targets exact under their own constants over 1152 pixels and zero
   under the other's, dmesg delta empty, a 99 us guarded interval, so
   the concatenation carries no state across the boundary and the
   command processor reaches the second cell.  The public-API two-draw
   command buffer holds its own receipt at mesa `e84ef39eb3b`
   (`r3v_native_attended_public_two_draw`,
   `docs/hardware/r3v-native-attended-public-two-draw-procedure.md`,
   steinmarder-r300
   `r3v-native-public-two-draw-first-delivery-rs482`): two render
   passes with a draw each through images, one render pass, two
   framebuffers, and one pipeline over each admitted fragment module
   (the reference module's green and `r3v_reference_fragment_blue_spirv`),
   the recorded stream digested against the emitter ahead of the ioctl
   (blake3 `44959464`), both targets exact under their own constants
   and zero under the other's, a 100 us guarded interval.  Two refused
   attempts preceded it, each ahead of the ioctl with the attempt
   unspent: the multi-pass predicate had frozen only the recorder
   form, so it now freezes the public form too (both deferred draws
   pending over in-family extents) and `r3v-native-public-surface`
   armed-submits the public two-draw on the shim; and the runner had
   fed the clip-space CPU route a window-space payload, so it now
   writes the NDC reference and refuses ahead of the device any payload
   that misses the reference window positions.  The shared bindings
   are drm-shim-held and silicon-unrun.  Open behind that: the GPU
   producer route, which composes one consumer stream over one carrier
   and judges one read-back, so a second pass beside it refuses by
   name.
8. sampler-state expansion depends on the executing sampled-descriptor route
   in row 3 and opens independently of the image-shape rows. The retained
   sampled cells cover `VK_FILTER_NEAREST` with clamp-to-edge addressing.
   Filters beyond nearest and address modes beyond clamp-to-edge remain open.
   Each filter and address-mode mechanism requires its own attended silicon
   receipt, an exact texel oracle that distinguishes neighbor selection,
   interpolation, and out-of-range addressing, and a clean dmesg delta.

Ledger row `mandatory_format_feature_absent` owns the source-visible
conformance defect. The Vulkan 1.0 Required Format Support table requires
`VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT`,
`VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT`, and the mandatory integer-format
requirements exercised by `dEQP-VK.api.info.format_properties.*`, including
`dEQP-VK.api.info.format_properties.r8_uint`. The inspected source contains no
executing storage-image, storage-texel-buffer, or integer-format route, so the
driver keeps those feature bits outside its advertised surface. Exact RS482
structural silicon absence remains unproven until a named hardware authority or
discriminating silicon receipt establishes it.

Expansion follows one delivery graph and one ownership boundary. [R3V
implementation boundaries](r3v-implementation-boundaries.md) owns source
ownership and completion criteria; [RS482 native delivery route admission and
oracle model](rs482-native-delivery-route-admission-and-oracle-model.md) owns
route topology, admission, coherency, and oracle structure. This status
document retains the expansion-order consequence: each route runs from source
through its selected executor, canonical carrier, publication, VAP re-ingest,
and raster tail, and a selected route runs to completion after an ioctl or
device-visible side effect.

WSI waits for a stable headless submission ladder. The active Xorg runners
first migrate to source locks and stack-manifest v2, tracked by
[steinmarder-r300 issue 237](https://github.com/Oichkatzelesfrettschen/steinmarder-r300/issues/237).
The remaining preconditions pin the Xorg server, DDX, package, kernel, and
runtime identities and deliver the native swapchain image route; acquire,
render, present, release, and reuse proven; the DRM/DRI3 route separated
from the opt-in software XCB route; WSI slices executing under
display-class evidence only.

Diagnostic sidecars stay independent of the run they describe.
`radeontool-gororoba` takes declared pre- and post-run safe snapshots;
`radeontop-gororoba` runs only inside an explicit performance or
engine-discrimination campaign; either launches during a conformance run
only when its sampling and access effects are part of the precommitted
experiment. The radeontool packaging route is
`radeontool-gororoba/_staging_upstream/PKGBUILD` alone (the pld-linux RPM spec,
which packaged pristine upstream 1.6.3, is retired).

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
