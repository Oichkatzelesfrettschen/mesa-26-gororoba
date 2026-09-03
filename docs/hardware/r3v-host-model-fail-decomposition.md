# R3V host-model fail decomposition: object_management, pipeline_barrier, descriptor_set

## Scope and method

This decomposition covers three dEQP-VK families inside the hazard-free
slices of `tests/r3v_conformance_partition.tsv`: `dEQP-VK.api.object_management.*`
(the `api-objects` slice, hazard `none`), `dEQP-VK.memory.pipeline_barrier.*`
and `dEQP-VK.api.descriptor_set.*` (both in the `command` slice, hazard
`submission`, which the Radeon drm-shim host model absorbs as a no-op
before any hardware boundary is reached). It reads out the per-case
refusal from the qpa result text and binds it to the exact driver line
in `src/amd/r300/vulkan/` (rank-4 source per `AGENTS.md` evidence rank).
No driver code changed; this is read-only classification plus a proposed
PR sequence.

Build: own Meson build directory (not the shared
`mesa-3_r300_full_debug_optimized_x86_64v1-clang22-distcc-cache` tree),
mirroring its options (`-Dbuildtype=debugoptimized -Dwerror=true
-Db_ndebug=false`, `ccache clang-22`/`clang++-22` native file,
`-Dvulkan-drivers=ati_r300 -Dgallium-drivers=r300,zink
-Dtools=nir,glsl,dlclose-skip,drm-shim`), built clean at source SHA
`4653e691f1692ad7c7e37835316e6cf7d82fc7bc`. `libvulkan_r3v.so`,
`libradeon_noop_drm_shim.so`, and `r3v_devenv_icd.x86_64.json` all built
without warnings under the configured flags.

This branch merged `origin/main` after the measurement build and runs
completed (a merge, not a rebase, to keep the already-published commit
reachable); the counts and qpa evidence in this document still describe
the `4653e691f1692ad7c7e37835316e6cf7d82fc7bc` build, and every source
citation below was re-verified against the merged HEAD
(`e68f40792d4bbdcc1961cae439a8b0264c25c9ce`) since
`r3v_native.h` and `r3v_native_image.c` both changed in the four
intervening commits (`e68f40792d4` admits `VK_IMAGE_CREATE_ALIAS_BIT` on
the linear transfer image family) and shifted the line numbers a citation
against the measurement SHA would have named. The guard logic and every
conclusion in this document are unaffected by that gap: `ALIAS_BIT` only
widens the admitted `flags` value from exactly `0` to `0` or
`VK_IMAGE_CREATE_ALIAS_BIT`, which changes none of the refusals this
document traces.

Caselists: `r3v_conformance_partition.py generate --kind exhaustive`
against the pinned mustpass corpus (3,251,483 cases, digest
`9cbcaff30025`, manifest `fe304d86e150`) produced 19 slices; `api-objects`
(4,299 cases, hazard `none`) and `command` (851 cases, hazard
`submission`) hold the three families. `api.object_management` is 457
cases in `api-objects`; `memory.pipeline_barrier` plus `api.descriptor_set`
together are 110 cases in `command`.

Runs: `r3v_conformance_runner.py run` against the deqp-vk binary named in
the task, release
`opengl-cts-4.6.8.0-410-ga2482928076da50046e78b57d86e1ba0ad23b35b`,
ICD `r3v_devenv_icd.x86_64.json`, `LD_PRELOAD` of the noop drm-shim,
`RADEON_GPU_ID=0x5974`, `R3V_NATIVE_COMPUTE_QUEUE_EXPERIMENTAL=1`,
`--dmesg-command ""`, `--timeout 1800`. Both runs are `evidence_class:
host-model`, with qualification invalid (no `--expect-source-sha` or
`--source-root`/`--build-root` pinned; this is an exploratory
classification run, not a qualification run per `Validation expectations`).

- `api-objects` filtered to `dEQP-VK.api.object_management.*`: 457 run,
  counts `{Pass: 231, Fail: 129, QualityWarning: 4, NotSupported: 93}`,
  seal `d36f06d4cdd5`.
- `command` filtered to `dEQP-VK.memory.pipeline_barrier.*` and
  `dEQP-VK.api.descriptor_set.*`: 110 run, counts `{Pass: 5,
  NotSupported: 1, Fail: 104}`, seal `632861fa7b4b`.

Every non-pass classifies against `tests/r3v_conformance_nonpass_ledger.tsv`
as `unclassified`; the ledger has no row for these `Fail` texts yet, which
is expected for a family the ladder has not admitted a rung for. Case
counts below come from parsing `run.qpa` `<Result StatusCode="Fail">`
blocks and matching the `vk.create*` call the CTS reference-object
wrapper (`vkRefUtil*.cpp/.inl`) names in its exception text.

The `object_management` Fail count is 129; the per-mechanism breakdown
below sums to 129 (90 `createImage` + 18 `createBufferView` + 10
`createDescriptorSetLayout` immutable-sampler + 10 `OpNot`/compute-pipeline
+ 1 unrelated race), not the 128 an earlier pass of this document reported.
The tenth `OpNot` case is `object_management.alloc_callback_fail_multiple.
compute_pipeline`: its CTS wrapper reports `"Got invalid error code:
VK_ERROR_UNKNOWN"` (the `allocCallbackFailMultipleObjectsTest` harness's own
message) rather than the `vkRefUtil` exception text the `vk.create*` regex
matched on, so an initial pass counted it under "other" instead of folding
it into the `ComputePipeline`/`OpNot` population it actually belongs to.

All `vk.createImage` populations trace to the same one refusal site
(`r3v_native_image.c:59-67`, a single `if` over eleven ORed field
comparisons), but the sub-populations do not fail on the same fields.
Reading the CTS shapes named in the case text confirms two fields that
every `vk.createImage` fail shares--`tiling` and `format`--and several
fields specific to a subset:

- `vktMemoryPipelineBarrierTests.cpp:894-914` (`findMaxRGBA8ImageSize`,
  the memory-probe helper behind every `.all*_vertex_buffer_stride_*`
  case): `VK_IMAGE_TYPE_2D`, `VK_FORMAT_R8G8B8A8_UNORM`,
  `VK_IMAGE_TILING_OPTIMAL`, `mipLevels=1`, `arrayLayers=1`, `flags=0`,
  and a caller-supplied `usage` this decomposition did not trace to its
  exact call site for `vertex_buffer_stride`--known: `tiling` and
  `format` both mismatch; hypothesis: `usage` also mismatches for some
  callers, unconfirmed.
- `vktApiObjectManagementTests.cpp:3605-3618` (`img1D`/`img2D`/`img3D`/
  `imgCube`, feeding `image_1d`/`image_2d`/`image_3d`/`image_view_*`/
  `framebuffer`): all four use `VK_FORMAT_R8G8B8A8_UNORM` and
  `VK_IMAGE_TILING_OPTIMAL`; `img1D` additionally sets `imageType=1D`
  (never admitted, guard line 60) with `arraySize=4`; `img3D` sets
  `imageType=3D` (never admitted) with `extent.depth=4`; `imgCube` sets
  `flags=CUBE_COMPATIBLE_BIT` (never admitted, guard line 59, since the
  guard admits only `flags==0` or `flags==VK_IMAGE_CREATE_ALIAS_BIT`);
  `img2D` sets `usage=SAMPLED_BIT|COLOR_ATTACHMENT_BIT` (never
  admitted--the admitted color-attachment arm requires usage to equal
  `COLOR_ATTACHMENT_BIT` alone, line 73) and `arraySize=12`.
- `vktApiObjectManagementTests.cpp:2379-2390` (`Framebuffer::Resources`):
  the color attachment alone matches the admitted usage
  (`COLOR_ATTACHMENT_BIT`), but its `extent` is 256x256 against the
  admitted cell's `R3V_NATIVE_TARGET_WIDTH`/`HEIGHT` of 64x64
  (`r3v_native.h:787-788`); the depth attachment carries
  `usage=DEPTH_STENCIL_ATTACHMENT_BIT`, which the guard's `else` arm
  (`r3v_native_image.c:89-90`) refuses outright since no depth usage is
  in the admitted set at all.

None of the 90 `object_management` cases fall inside the shape reachable
by relaxing `tiling` and `format` alone: `image_1d`/`image_view_1d`/
`image_view_1d_arr` (27 cases) need `imageType=1D` admission;
`image_3d`/`image_view_3d` (18 cases) need `imageType=3D` admission;
`image_view_cube` (9 cases) needs `flags=CUBE_COMPATIBLE_BIT` admission;
`image_2d`/`image_view_2d`/`image_view_2d_arr` (27 cases) need
`arrayLayers>1` and `usage=SAMPLED_BIT|COLOR_ATTACHMENT_BIT` admission;
`framebuffer` (9 cases) needs a 256x256 extent and, separately, depth
usage admission. Each is a materially larger driver capability than a
second disjoint `VkImageCreateInfo` cell, and the five populations do
not share a single closing mechanism the way the `pipeline_barrier`
population plausibly does.

## Table 1: refusal sites, by cases closed

| Refusal site (file:line) | Cases | Trigger shape | Admitting mechanism | Hazard |
|---|---|---|---|---|
| `r3v_native_image.c:65` (`tiling`, main guard, universal) and `:75` (`format`, `VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT` branch only) | up to 100 (`pipeline_barrier`); 0 of the 90 `object_management` cases close on this alone | `findMaxRGBA8ImageSize` always requests `VK_IMAGE_TILING_OPTIMAL` and `VK_FORMAT_R8G8B8A8_UNORM` against a `VK_IMAGE_TYPE_2D`, one-mip, one-layer, `flags=0` image; the admitted cells require `VK_IMAGE_TILING_LINEAR` universally (line 65), plus `R3V_NATIVE_TARGET_FORMAT` (`VK_FORMAT_B8G8R8A8_UNORM`, line 75) only inside the color-attachment-usage branch--the transfer-usage branch (lines 81-88) never re-checks format, and `r3v_native_transfer_texel_bytes` already accepts `R8G8B8A8_UNORM`, so a `TRANSFER_SRC\|DST`-usage probe blocks on `tiling` alone | Admit a second, `OPTIMAL`-tiled cell at the same fixed extent and usage vocabulary the `LINEAR` cells already admit (`COLOR_ATTACHMENT_BIT` alone, or `TRANSFER_SRC\|DST` alone, the latter already `R8G8B8A8_UNORM`-capable); landed in commit `1d0ecc2e493` -- necessary but not sufficient, see the "Rung 1 correction" section | Host-provable: pure `VkImageCreateInfo` field comparison, no submission |
| `r3v_native_image.c:59-67` `r3v_CreateImage`, `imageType`/`flags`/`usage`/`extent` fields | 90 (`object_management`), across five disjoint sub-populations enumerated in this row's refusal column | 1D/3D `imageType`, `CUBE_COMPATIBLE_BIT` `flags`, `SAMPLED_BIT` combined usage with `arrayLayers>1`, a 256x256 extent past the 64x64 admitted cell, and depth-usage images--five distinct unadmitted shapes, detailed above | Each sub-population needs its own new admitted shape (1D support, 3D support, cube support, multi-layer sampled support, a larger or second extent cell, depth-format support); none is a single small PR | Host-provable, but not one mechanism--see the per-population breakdown above |
| `r3v_native_object.c:43-50` `r3v_CreateBufferView` | 18 (object_management only) | Every call: the function is `return vk_error(device, R3V_NATIVE_REFUSAL_RESULT);` unconditionally | Implement buffer-view admission over the existing transfer-family buffer route: bind `VkBufferViewCreateInfo.format` to a texel size the transfer path already knows (`r3v_native_transfer_texel_bytes`), range and offset bounded by the parent buffer's `r3v_native_transfer_footprint_bytes` | Host-provable: object bookkeeping, no submission |
| `r3v_compute_spirv.c:744-752`, `default:` arm of the instruction-opcode walk | 10 (object_management only) | The CTS `ComputePipeline` shader (`vktApiObjectManagementTests.cpp:1357-1364`) is `dataOut[i] = ~dataIn[i]`; SPIR-V `OpNot` (opcode 200) is outside the walk's admitted opcode set (`OpVariable`/`OpLoad`/`OpStore`/`OpAccessChain`/`OpLabel`/`OpFunction*`/`OpReturn`, plus arithmetic opcodes 126-141) and outside the 126-141 arithmetic range the `default:` arm special-cases, so it falls to `"opcode outside the identity-map subset"`; `r3v_compute_job_from_spirv` (`r3v_native_compute.c:500-505`) returns this reason and `create_pipeline` maps it straight to `R3V_NATIVE_REFUSAL_RESULT` before the verb ledger (`r300_compute_verb.c:153-161`) is ever consulted, since `r300_compute_verb_for_job` only recognizes `R300_COMPUTE_JOB_OP_IDENTITY` and returns `NULL` for every other job op.  The two `descriptor_set` cases an earlier pass of this table counted here refuse on other grammar and now hold rows of their own below | Extend the front-end grammar to admit one bitwise-NOT instruction between the load and the store (`OpNot`, opcode 200), classify the resulting job as a new `R300_COMPUTE_JOB_OP_BITWISE_NOT`, and give `r300_compute_verb_for_job` a case for it pointing at a verb whose operation owns an executing host route in `r300_operation_route.c` (`BITWISE_NOT_MAP` already does, through `R300_OPERATION_ROUTE_HOST_BITWISE_NOT`; whether that verb or a new one is the right home for a unary NOT is itself a design question, not resolved here) | Host-provable: the refusal itself is host-checkable; the front-end and verb-table change needs a positive/negative pair on the buffer contents, still no submission |
| `r3v_compute_spirv.c` final checks, `kernel lacks the load/store pair` | 1 (`descriptor_set.descriptor_set_layout_lifetime.compute`) | The CTS module is an empty kernel -- `OpFunction`, `OpLabel`, `OpReturn`, `OpFunctionEnd`, no storage variable -- so the admitter's closing check finds neither a load nor a store; `create_compute_pipeline` would also fail its binding resolution, which needs both job bindings present in set 0 | Admit a kernel that names no storage buffer as a job that writes nothing, and give `create_compute_pipeline` a layout path for a pipeline whose job carries no binding | Host-provable: admission alone, no submission |
| `r3v_compute_spirv.c` `OpMemberDecorate` arm, `member layout outside the single-member word array` | 1 (`descriptor_set.descriptor_set_layout_binding.update_subsequent_binding`) | The CTS module declares three storage buffers -- a three-member struct at binding 2, an array of two blocks at binding 0, a single block at binding 1 -- and three load/store pairs; `OpMemberDecorate %7 1 Offset 4` refuses before the body is read | Extend the storage-layout model past one member of one runtime array, and the body model past one load and one store, so a multi-binding elementwise kernel lowers | Host-provable: admission alone, no submission |
| `r3v_native_compute.c:59-79` `r3v_CreateDescriptorSetLayout`, `pImmutableSamplers != NULL` branch (line 78) | 10 (object_management `graphics_pipeline` cases) | `GraphicsPipeline::Resources` (`vktApiObjectManagementTests.cpp:1905-1906`) builds its one binding as `single(0u, COMBINED_IMAGE_SAMPLER, 1u, FRAGMENT_BIT, true)`, where `true` requests an immutable sampler | Admit one immutable-sampler binding: at layout build time, resolve `pImmutableSamplers[0]` to the existing `r3v_native_sampler` object--already creatable for the admitted shape `r3v_CreateSampler` takes (`flags==0` and `anisotropyEnable==VK_FALSE`; every other shape refuses at `r3v_native_object.c:68-70`)--and record it in the binding instead of refusing on sight | Host-provable: layout construction, no submission |
| `r3v_native_pipeline.c:378-386` `create_pipeline`, `layout->set_count != 0` | 1 (`descriptor_set.descriptor_set_layout_lifetime.graphics`) | `VkPipelineLayoutCreateInfo` carries one non-empty descriptor set layout; graphics-pipeline creation refuses any layout with `set_count != 0` | Extend `create_pipeline`'s admitted layout shape to accept exactly the one COMBINED_IMAGE_SAMPLER set the fixed cell's fragment stage can bind (mirrors the sampler-in-descriptor-set-layout fix above one layer up) | Host-provable: layout field comparison, no submission |
| Format-support gate (physical-device format query, not a `create*` refusal) | 1 (`descriptor_set.descriptor_set_layout_binding.layout_binding_order`, Amber script) | Hypothesis, not traced to the format-properties query source: the Amber test's requested color-attachment format is outside the single format (`R3V_NATIVE_TARGET_FORMAT`) the query advertises as attachment-capable | Same fixed-format-table constraint as the `CreateImage` rows; no separate fix if the hypothesis holds | Host-provable |

## Table 2: cases closed per mechanism (ranked)

| Rank | Mechanism | Cases closed | dEQP groups touched |
|---|---|---|---|
| 1 | `OPTIMAL`-tiled `R8G8B8A8_UNORM` image cell in `r3v_CreateImage` | 0 measured (landed in commit `1d0ecc2e493`; necessary, not sufficient -- see the "Rung 1 correction" section) | `dEQP-VK.memory.pipeline_barrier.*` |
| 2 | Buffer-view admission in `r3v_CreateBufferView` | 18 | `dEQP-VK.api.object_management.*` |
| 3 | `OpNot` admission in the compute SPIR-V front-end plus a CPU route for the resulting job | 10 | `dEQP-VK.api.object_management.*` |
| 4 | Immutable-sampler binding in `r3v_CreateDescriptorSetLayout` | 10 | `dEQP-VK.api.object_management.*` |
| 5 | Non-empty layout in `r3v_CreateGraphicsPipelines` | 1 | `dEQP-VK.api.descriptor_set.*` |
| unranked | (hypothesis, same format-table root cause as rank 1) | 1 | `dEQP-VK.api.descriptor_set.*` (Amber format query) |

The 90 `object_management` `vk.createImage` cases are not one closable
rung. They split into five sub-populations (1D, 3D, cube, multi-layer
sampled 2D, oversized/depth framebuffer), each needing its own new
admitted shape; none ranks above rank 5 in cases-per-mechanism, and the
largest sub-population (multi-layer sampled 2D, 27 cases across
`image_2d`/`image_view_2d`/`image_view_2d_arr`) still trails rank 2.
They are listed as a residual below rank 5 rather than ranked, since
closing any one of them does not close a clean rung by itself.

One case (`object_management.multithreaded_per_thread_resources.instance`,
`Runtime check failed: (size_t)numExtensions == properties.size()` at
`vkQueryUtil.cpp:515`) is a concurrent `vkEnumerateInstanceExtensionProperties`
call-count race in the CTS harness against the loader/ICD's extension
enumeration, not a resource-create refusal; it sits outside every
mechanism in Table 1 and needs its own rung (instance-level enumeration
under concurrent callers) before it closes.

## Table 3: proposed PR sequence

Each PR lands one mechanism, targets the exact dEQP group the mechanism
closes, and carries a negative fixture (a shape still outside the
admitted cell, expected to keep refusing) beside the positive one, per
`Falsification record` in `AGENTS.md`.

| Order | PR | Mechanism | Target dEQP group | Cases closed | Negative fixture |
|---|---|---|---|---|---|
| 1 | `r3v: admit an OPTIMAL-tiled R8G8B8A8_UNORM image cell in CreateImage` (landed in commit `1d0ecc2e493`) | Second fixed `VkImageCreateInfo` cell (Table 1, row 1) | `dEQP-VK.memory.pipeline_barrier.*` (104-case filtered shard) | 0 of 104 measured -- see "Rung 1 correction" below | An image request combining the new cell's usage bits with a third, unadmitted format still refuses |
| 2 | `r3v: admit buffer views over the existing transfer buffer route` | `r3v_CreateBufferView` (Table 1, row 3) | `dEQP-VK.api.object_management.*` (`buffer_view_storage_*`, `buffer_view_uniform_*`) | 18 | A `VkBufferViewCreateInfo` whose `range` exceeds the parent buffer's transfer footprint still refuses |
| 3 | `r300: admit OpNot and land the CPU route it needs` | `r3v_compute_spirv.c` opcode walk plus `r300_compute_verb.c` row plus a `r300_cpu_compute_job.c` kernel (Table 1, row 4) | `dEQP-VK.api.object_management.*compute_pipeline*` | 10 | A second unary opcode (e.g. `OpSNegate`, already in the 126-141 arithmetic band but still refused) stays outside the admitted grammar and still refuses at the front end |
| 4 | `r3v: admit one immutable-sampler binding in descriptor set layouts` | `r3v_CreateDescriptorSetLayout` (Table 1, row 5) | `dEQP-VK.api.object_management.*graphics_pipeline*` | 10 | Two bindings each carrying `pImmutableSamplers` still refuse (the cell binds exactly one sampler slot) |
| 5 | `r3v: admit the one-set fragment-sampler layout in CreateGraphicsPipelines` | `create_pipeline` `layout->set_count` gate (Table 1, row 6), depends on PR 4 for the sampler object to bind | `dEQP-VK.api.descriptor_set.descriptor_set_layout_lifetime.graphics` | 1 | `set_count == 2` or a vertex-stage binding in the one set still refuses |

PR 5 depends on PR 4's sampler-binding plumbing; every other PR is
independent and reorderable by whichever closes more cases sooner. The
five `object_management` `vk.createImage` sub-populations (1D, 3D, cube,
multi-layer sampled 2D, oversized/depth framebuffer) are deliberately
left out of this sequence: each needs its own scoping pass (a new
admitted `imageType`, `flags`, or `usage`/`extent` shape) before it is a
PR-sized rung, and bundling any of them into PR 1 would put a
transfer-image fix and an unrelated image-capability expansion in one
review.

## Rung 1 correction: OPTIMAL tiling is necessary, not sufficient

Commit `1d0ecc2e493` (`r3v: admit VK_IMAGE_TILING_OPTIMAL on the linear transfer
image family`) landed Table 1 row 1's mechanism and measured its
`dEQP-VK.memory.pipeline_barrier.*` movement directly, correcting this
document's original estimate ("up to 100... pending the unconfirmed
`usage` field").

Own Meson build directory mirroring
`mesa-3_r300_full_debug_optimized_x86_64v1-clang22-distcc-cache`
(`-Dbuildtype=debugoptimized -Db_ndebug=false -Dwerror=true
-Dvulkan-drivers=ati_r300 -Dgallium-drivers=r300,zink`), full tree
clean, zero warnings. `r3v_conformance_partition.py generate --kind
exhaustive` against the same pinned corpus (digest `9cbcaff30025`)
reproduces the `command` slice's 851-case shard; filtering it to
`dEQP-VK.memory.pipeline_barrier.*` yields the same 104 cases this
document's `command` filtered run covers. `r3v_conformance_runner.py
run` against that 104-case caselist, same ICD/drm-shim/env vocabulary
as this document's original runs: before the OPTIMAL-tiling commit,
`Fail:100 Pass:4` (seal `9a4f99414d8e`); after, `Fail:100 Pass:4` (seal
`2e00e7ddbb93`). Zero of 104 cases move Fail to Pass.

A single-case probe
(`dEQP-VK.memory.pipeline_barrier.host_read_transfer_dst.1024`)
confirms the mechanism itself is correct and reaches the CTS: before
the commit, `vkCreateImage` throws `VK_ERROR_UNKNOWN` during the
CTS harness's memory-type probe phase (`findMaxRGBA8ImageSize`,
`vktMemoryPipelineBarrierTests.cpp:873`); after, that call succeeds,
and the same case instead fails later, at `vkEndCommandBuffer`, on a
poisoned command buffer. A `backtrace()`/`dladdr` census (built with
`#pragma clang optimize off` on `r3v_native_recording.c` to defeat
tail-call elimination that otherwise collapses the calling frame, kept
out of the landed patch) over every `r3v_native_cmd_poison` call
across a full run of the 104-case caselist attributes:

- 56 poison events to `r3v_CmdExecuteCommands`: `createHostCommand`'s
  op-selection logic
  (`vktMemoryPipelineBarrierTests.cpp:8266`,
  `ops.push_back(OP_SECONDARY_COMMAND_BUFFER_BEGIN)`) makes a
  secondary command buffer an available random choice on nearly every
  op-selection round of the randomized command sequence these test
  cases record, and R3V has no secondary-command-buffer route --
  `r3v_CmdExecuteCommands` is one of the many core-1.0 entrypoints
  `r3v_native_recording.c` poisons unconditionally.
- 4 poison events to `r3v_CmdBlitImage`: unimplemented image blit,
  independently selectable by the same op-generation logic.
- 16 of the 104 cases (the `all` and `all_device` usage subgroups)
  still refuse at `vkCreateImage` itself: `usageToImageUsageFlags`
  (`vktMemoryPipelineBarrierTests.cpp:385`) resolves those groups'
  usage to `TRANSFER_SRC|TRANSFER_DST|SAMPLED|STORAGE`, and no
  admitted cell grants `SAMPLED_BIT` or `STORAGE_BIT`.
- The remaining cases (`transfer_dst_vertex_buffer`,
  `transfer_src_transfer_dst`, `host_write_transfer_src`,
  `host_read_transfer_dst`, and the five `transfer_dst_*_buffer`
  groups) resolve to `imageUsage` values within the newly admitted
  transfer cell (a subset of `TRANSFER_SRC|TRANSFER_DST`), so
  `vkCreateImage` now succeeds for every one of them; every failure
  among them traces to the secondary-command-buffer or blit gate
  above, not to image tiling.

Table 1 row 1, Table 2 rank 1, and Table 3 PR 1 above are corrected in
place to point here rather than restate the original prediction.
Closing any case in this family needs secondary-command-buffer
support, image blit, or sampled/storage-image admission as a separate,
materially larger mechanism than an image-creation cell; none of those
three is scoped by this decomposition, and each needs its own rung
before `dEQP-VK.memory.pipeline_barrier.*` moves off `Fail:100`.

## Residual

The `numExtensions == properties.size()` race (1 case, outside every
mechanism above) and the general non-pass ledger having no rows yet for
any of these `Fail` texts (`tests/r3v_conformance_nonpass_ledger.tsv`)
were both out of scope for the original decomposition pass; the four
ledger rows and the corrected model below close that gap for every
residual class named above.

## Ledger classification: the residue is four mechanisms, not five

`tests/r3v_conformance_nonpass_ledger.tsv` carries four new rows, each
inserted ahead of the broad `image_outside_executed_envelope`,
`descriptor_outside_executed_subset`, and `driver_defect_open` catch-alls
so the specific mechanism wins under `classify()`'s first-match-wins
order. Every row's witness is a literal `dEQP-VK.` case from
`external/vulkancts/mustpass/main/vk-default/{api,memory}.txt`, and each
carries the exact refusal text observed on this build
(`4653e691f1692ad7c7e37835316e6cf7d82fc7bc`-derived worktree,
own Meson build directory mirroring
`mesa-3_r300_full_debug_optimized_x86_64v1-clang22-distcc-cache`, run
against the drm-shim host model, `RADEON_GPU_ID=0x5974`,
`R3V_NATIVE_COMPUTE_QUEUE_EXPERIMENTAL=1`).

**The five `object_management` image sub-populations are one root
cause, not five.** Table 1 originally listed 1D, 3D, cube, multi-layer
sampled 2D, and oversized/depth framebuffer as five materially
different `VkImageCreateInfo` shapes needing five separate admission
cells. They are, but every one of them requests
`VK_IMAGE_USAGE_SAMPLED_BIT` (directly, or through the combined
`SAMPLED_BIT|COLOR_ATTACHMENT_BIT` usage `image_2d`/`image_view_2d*`
carry) against `VK_FORMAT_R8G8B8A8_UNORM`, and `r3v_get_format_properties`
withholds `VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT` for that format
unconditionally (the `mandatory_format_feature_absent` ledger row,
`dEQP-VK.api.info.format_properties.r8_uint` and siblings): no admitted
`r3v_native_image.c` cell samples an image at all, on any format. Widening
`imageType`, `flags`, or `extent` alone therefore closes nothing; each of
the five sub-populations still refuses at the same `usage` check even
after its own shape-specific field is admitted, because the driver has
no executed sampled-image route to bind. This reclassifies the residue
from "five creation cells" to one mechanism (a sampled-image route) that
gates all five, plus the five cells themselves as a second, independent
mechanism layered on top -- closing the sampled-image route does not by
itself admit `imageType=1D`, and admitting `imageType=1D` does not by
itself close the sampled-image gate. `sampled_image_format_feature_withheld`
(90 cases, host-model run seal `959264ee4799`) carries this reading.

The ladder's sampled rungs landed after that seal, so the class name
`sampled_image_format_feature_withheld` now outlives the posture it was
named for: `r3v_CreateImage` admits sampled usage on the linear
transfer family for both 32-bpp lane orders, and both
R8G8B8A8_UNORM and B8G8R8A8_UNORM carry
`VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_BIT`.  The 90 cases stand where they
were: the host-model run at seal `7a7ea4232c80` measures the same 90
`vkCreateImage` refusals, now because `img2D` requests `arraySize = 12`
together with `SAMPLED_BIT | COLOR_ATTACHMENT_BIT` rather than because
the sampled bit is withheld.  This row records the state at
seal `959264ee4799`, and the class name is the key sealed runs cite.

**`pipeline_barrier` is an executing-route family, not a creation-cell
family.** The Rung 1 correction above already traced this: 56 cases
poison on `r3v_CmdExecuteCommands` (no secondary-command-buffer route),
4 poison on `r3v_CmdBlitImage` (no blit route), 16 refuse at
`vkCreateImage` on the same withheld sampled/storage-image usage as the
row above, and the rest resolve inside the admitted transfer cell and
still poison downstream. `pipeline_barrier_executing_route_gap` (100 of
104 cases classified Fail, host-model run seal `cc497bd8e6e5`) covers
the family as one class since every sub-mechanism needs a materially
larger executing route -- secondary command buffers, blit, or a sampled/
storage image -- before any case in the group can pass; none of the
three is a creation-cell fix.

**Immutable samplers and the one-set descriptor layout are one missing
route, not two.** `GraphicsPipeline::Resources`
(`vktApiObjectManagementTests.cpp:1905-1906`) fails at
`vkCreateDescriptorSetLayout` because `r3v_CreateDescriptorSetLayout`
(`r3v_native_compute.c:59-79`) refuses any `pImmutableSamplers` binding
outright (10 `object_management.*.graphics_pipeline` cases across the
ten object-management groups). `descriptor_set.descriptor_set_layout_
lifetime.graphics` fails one layer up, at `vkCreateGraphicsPipelines`,
because `create_pipeline` (`r3v_native_pipeline.c:378-386`) refuses any
non-empty descriptor set layout regardless of what it contains. Both
refusals gate the same missing mechanism: a fragment shader sampling a
bound descriptor. `sampling_fragment_route_absent` (10+1 cases, host-
model run seals `959264ee4799` and `cc497bd8e6e5`) carries this reading.

**The `numExtensions == properties.size()` case is a loader-level race,
not an r3v defect.** `multithreadedCreatePerThreadResourcesTest<Instance>`
runs `Instance::create` from several concurrent threads, each of which
calls the CTS two-call enumeration idiom
(`enumerateInstanceExtensionProperties`, `vkQueryUtil.cpp:507-515`)
against the pre-instance, loader-resolved
`vkEnumerateInstanceExtensionProperties`. `r3v_EnumerateInstanceExtensionProperties`
(`r3v_instance.c:64-72`) forwards to
`vk_enumerate_instance_extension_properties` (`vk_instance.c:273-295`),
which iterates the file-scope `static const`
`r3v_instance_extensions_supported` table with no mutable state; two
calls into the driver in the same process return the identical count by
construction. Five direct host-model reruns of the single case
(`--deqp-case`, no partition filtering) split 2 `Runtime check failed:
'(size_t)numExtensions == properties.size()'` at `vkQueryUtil.cpp:515`,
2 `VK_INCOMPLETE` at `vkQueryUtil.cpp:514`, and 1 `Pass` -- the count a
thread's own fill call sees differs from the count its own preceding
size call saw, which only the layer above the ICD trampoline (the
Khronos Vulkan-Loader's manifest scan and instance-extension merge) can
introduce. This host carries `MangoHud`, `vkBasalt`,
`VkLayer_FROG_gamescope_wsi`, `VkLayer_MESA_anti_lag`,
`VkLayer_MESA_device_select`, and the Steam overlay/fossilize implicit
layers under `/usr/share/vulkan/implicit_layer.d`; the loader merges
every enabled implicit layer's extension list into the enumerated
result on each call, and re-evaluates layer enablement on each scan.
`loader_instance_extension_enumeration_race` (1 case, host-model run
seal `959264ee4799`) records this as a loader-and-host-configuration
race outside `src/amd/r300/vulkan/`, not an open r3v driver defect; the
Vulkan-Loader source is not vendored in this repository, so the exact
loader code path is a hypothesis pinned by exclusion (the driver side is
provably deterministic) and the reproduced count divergence, not by a
loader source citation.

Measured receipts (own Meson build directory, `--dmesg-command ""`,
`R3V_NATIVE_COMPUTE_QUEUE_EXPERIMENTAL=1`, evidence class host-model,
qualification invalid): the `api.object_management` family (457
cases, caselist drawn directly from
`external/vulkancts/mustpass/main/vk-default/api.txt` rather than the
partition tool's `generate --mustpass-dir` path: the CTS checkout's
mustpass directory carries `vk-fraction-mandatory-tests.txt`, which
relists `dEQP-VK.info.build` beside `info.txt`, and the exact-cover
tool refuses a corpus with a duplicate case, so it takes the pinned
bundle's file list alone)
classifies as `{Pass: 240, Fail: 120, QualityWarning: 4, NotSupported:
93}` with zero `unclassified`, seal `959264ee4799`; the `Fail` split is
`sampled_image_format_feature_withheld: 90`,
`sampling_fragment_route_absent: 10`,
`loader_instance_extension_enumeration_race: 1`, and
`driver_defect_open: 19` (the buffer-view and `OpNot`/compute-pipeline
populations, still generic pending their own PRs). The `memory.
pipeline_barrier` plus `api.descriptor_set` family (110 cases, same
direct-mustpass caselist method) classifies as `{Pass: 5, NotSupported:
1, Fail: 104}` with zero `unclassified`, seal `cc497bd8e6e5`; the `Fail`
split is `pipeline_barrier_executing_route_gap: 100`,
`sampling_fragment_route_absent: 1`, and `driver_defect_open: 3` (two
`OpNot`-gated `descriptor_set` cases plus one more). `r3v_conformance_
runner.py check-ledgers` (against `r3v_conformance_slices.tsv`, the
file the `r3v-conformance-ledgers` Meson test actually names) and
`selftest` both pass, and `meson test --suite r3v` is green (219 pass,
37 expected-fail, 0 fail, 6 skip) under `env -u R3V_NATIVE_MANIFEST_DIR`.

The `OpNot` compute-instruction population (12 cases total, 10 in
`object_management` and 2 in `descriptor_set`) is not given its own
ledger row here: it falls into the generic `driver_defect_open` bucket
above, and a concurrent PR is landing `OpNot` admission in
`r3v_compute_spirv.c`/`r300_compute_verb.c`/`r300_cpu_compute_job.c`
directly, which will need its own row (or the `driver_defect_open`
default continues to cover it correctly until it lands, since the
population is a real open defect either way).

## Draw and robustness slices: 482 fails, five mechanisms

The closed-gate host-model runs of the `draw` (371 Fail) and
`robustness` (111 Fail) partition slices decompose by first refusal,
found by reading each case's log against the driver
(`rg --fixed-strings` on the refusal text; the two CTS-authored texts
have no driver hit because the driver's own refusals are bare
`vk_error(device, R3V_NATIVE_REFUSAL_RESULT)` calls, `r3v_native.h`):

- 309 `dEQP-VK.draw.renderpass.*`: `r3v_CreateImage`
  (`r3v_native_image.c`) refuses the base-class color attachment,
  `VK_FORMAT_R8G8B8A8_UNORM` at the test's extent, against the executed
  target `VK_FORMAT_B8G8R8A8_UNORM` at 64x64.  Class
  `image_outside_executed_envelope`.
- 30 `dEQP-VK.pipeline.monolithic.vertex_input.*`: the same function,
  the same `R8G8B8A8_UNORM` format; the usage the test also carries
  (`COLOR_ATTACHMENT|TRANSFER_SRC`) is the later check.  Class
  `image_outside_executed_envelope`.
- 111 `dEQP-VK.robustness.buffer_access.*_copy.*`:
  `r3v_compute_job_from_spirv` (`vulkan/r3v_compute_spirv.c`) refuses
  the module at `storage access outside member 0 of the word array`:
  the shaders load the storage index from a two-int uniform block
  through a one-index `OpAccessChain`, and the recognizer admits only
  the flattened GlobalInvocationID shape.  Class `driver_defect_open`.
- 25 `output_location.array.{b10g11r11,r16g16,r32,r32g32b32a32,r8g8-uint}-*`,
  `shuffle.inputs-outputs-mod`, `flat_b_sat_error`,
  `depth_bias_triangle_list_fill`: the CTS pre-flight color-attachment
  format check fails because `r3v_get_format_properties` grants those
  formats no `COLOR_ATTACHMENT_BIT` under any tiling; a render target in
  those formats is a silicon route the color backend has not executed.
  Class `driver_defect_open`.
- 7 `output_location.array.b8g8r8a8-unorm-*`, `shuffle.inputs-outputs`:
  the same pre-flight check under `VK_IMAGE_TILING_OPTIMAL`, where the
  executed format carries the bit on linear tiling alone; the fragment
  shader then writes three color locations, which
  `r3v_native_render_pass_matches_cell`'s one-attachment cell refuses.
  Class `driver_defect_open`.

A rung admitting the color-plus-transfer usage family and the
optimal-tiling color-attachment bit on `B8G8R8A8_UNORM` was built and
measured: 0 of the 42 targeted cases moved and 0 of 39,966 draw cases
changed status, because each family stops at the format or the
attachment count before usage or tiling is consulted, so the rung was
not merged; a surface widening earns its place by a case it moves.  The
host-provable rungs that remain are the `R8G8B8A8_UNORM` target
(a channel order the CPU route writes as readily as `B8G8R8A8_UNORM`,
while the extent stays receipt-pinned at 64x64) and the recognizer's
uniform-indexed storage access; the 25 unlisted-format cases and the
multi-attachment cases are silicon routes.  `driver_defect_open` is
the ledger's last row (`dEQP-VK\..*`, no detail pattern), so it
names the residue, not a mechanism: its 143 cases here are three
unrelated refusals.
