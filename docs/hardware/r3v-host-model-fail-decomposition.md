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
host-model`, `decision_grade: False` (no `--expect-source-sha` or
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
| `r3v_native_image.c:65` (`tiling`, main guard, universal) and `:75` (`format`, `VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT` branch only, lines 73-78) | up to 100 (`pipeline_barrier`); 0 of the 90 `object_management` cases close on this alone | `findMaxRGBA8ImageSize` always requests `VK_IMAGE_TILING_OPTIMAL` and `VK_FORMAT_R8G8B8A8_UNORM` against a `VK_IMAGE_TYPE_2D`, one-mip, one-layer, `flags=0` image; the admitted cells require `VK_IMAGE_TILING_LINEAR` universally (line 65), plus `R3V_NATIVE_TARGET_FORMAT` (`VK_FORMAT_B8G8R8A8_UNORM`, line 75) only inside the color-attachment-usage branch--the transfer-usage branch (lines 81-88) never re-checks format, and `r3v_native_transfer_texel_bytes` already accepts `R8G8B8A8_UNORM`, so a `TRANSFER_SRC\|DST`-usage probe blocks on `tiling` alone | Admit a second, `OPTIMAL`-tiled cell at the same fixed extent and usage vocabulary the `LINEAR` cells already admit (`COLOR_ATTACHMENT_BIT` alone, or `TRANSFER_SRC\|DST` alone, the latter already `R8G8B8A8_UNORM`-capable); confirming this closes all 100 first needs the exact `usage` `findMaxRGBA8ImageSize`'s `vertex_buffer_stride` callers pass (not traced here) | Host-provable: pure `VkImageCreateInfo` field comparison, no submission |
| `r3v_native_image.c:59-67` `r3v_CreateImage`, `imageType`/`flags`/`usage`/`extent` fields | 90 (`object_management`), across five disjoint sub-populations (see above) | 1D/3D `imageType`, `CUBE_COMPATIBLE_BIT` `flags`, `SAMPLED_BIT` combined usage with `arrayLayers>1`, a 256x256 extent past the 64x64 admitted cell, and depth-usage images--five distinct unadmitted shapes, detailed above | Each sub-population needs its own new admitted shape (1D support, 3D support, cube support, multi-layer sampled support, a larger or second extent cell, depth-format support); none is a single small PR | Host-provable, but not one mechanism--see the per-population breakdown above |
| `r3v_native_object.c:43-50` `r3v_CreateBufferView` | 18 (object_management only) | Every call: the function is `return vk_error(device, R3V_NATIVE_REFUSAL_RESULT);` unconditionally | Implement buffer-view admission over the existing transfer-family buffer route: bind `VkBufferViewCreateInfo.format` to a texel size the transfer path already knows (`r3v_native_transfer_texel_bytes`), range and offset bounded by the parent buffer's `r3v_native_transfer_footprint_bytes` | Host-provable: object bookkeeping, no submission |
| `r300_compute_spirv.c:744-752`, `default:` arm of the instruction-opcode walk | 12 (10 in object_management, 2 in descriptor_set) | The CTS `ComputePipeline` shader (`vktApiObjectManagementTests.cpp:1357-1364`) is `dataOut[i] = ~dataIn[i]`; SPIR-V `OpNot` (opcode 200) is outside the walk's admitted opcode set (`OpVariable`/`OpLoad`/`OpStore`/`OpAccessChain`/`OpLabel`/`OpFunction*`/`OpReturn`, plus arithmetic opcodes 126-141) and outside the 126-141 arithmetic range the `default:` arm special-cases, so it falls to `"opcode outside the identity-map subset"`; `r300_compute_job_from_spirv` (`r3v_native_compute.c:500-505`) returns this reason and `create_pipeline` maps it straight to `R3V_NATIVE_REFUSAL_RESULT` before the verb ledger (`r300_compute_verb.c:153-161`) is ever consulted, since `r300_compute_verb_for_job` only recognizes `R300_COMPUTE_JOB_OP_IDENTITY` and returns `NULL` for every other job op | Extend the front-end grammar to admit one bitwise-NOT instruction between the load and the store (`OpNot`, opcode 200), classify the resulting job as a new `R300_COMPUTE_JOB_OP_BITWISE_NOT`, and give `r300_compute_verb_for_job` a case for it pointing at a new or the existing `BITWISE_LOGICOP_MAP` row with `cpu_route` flipped to `EXECUTING` (`r300_compute_verb.c:57-61` already carries that row with `cpu_route=ABSENT`; whether it is the right row for a unary NOT or a new row is needed is itself a design question, not resolved here) | Host-provable: the refusal itself is host-checkable; the front-end and verb-table change needs a positive/negative pair on the buffer contents, still no submission |
| `r3v_native_compute.c:59-79` `r3v_CreateDescriptorSetLayout`, `pImmutableSamplers != NULL` branch (line 78) | 10 (object_management `graphics_pipeline` cases) | `GraphicsPipeline::Resources` (`vktApiObjectManagementTests.cpp:1905-1906`) builds its one binding as `single(0u, COMBINED_IMAGE_SAMPLER, 1u, FRAGMENT_BIT, true)`, where `true` requests an immutable sampler | Admit one immutable-sampler binding: at layout build time, resolve `pImmutableSamplers[0]` to the existing `r3v_native_sampler` object--already creatable for the admitted shape `r3v_CreateSampler` takes (`flags==0` and `anisotropyEnable==VK_FALSE`; every other shape refuses at `r3v_native_object.c:68-70`)--and record it in the binding instead of refusing on sight | Host-provable: layout construction, no submission |
| `r3v_native_pipeline.c:378-386` `create_pipeline`, `layout->set_count != 0` | 1 (`descriptor_set.descriptor_set_layout_lifetime.graphics`) | `VkPipelineLayoutCreateInfo` carries one non-empty descriptor set layout; graphics-pipeline creation refuses any layout with `set_count != 0` | Extend `create_pipeline`'s admitted layout shape to accept exactly the one COMBINED_IMAGE_SAMPLER set the fixed cell's fragment stage can bind (mirrors the sampler-in-descriptor-set-layout fix above one layer up) | Host-provable: layout field comparison, no submission |
| Format-support gate (physical-device format query, not a `create*` refusal) | 1 (`descriptor_set.descriptor_set_layout_binding.layout_binding_order`, Amber script) | Hypothesis, not traced to the format-properties query source: the Amber test's requested color-attachment format is outside the single format (`R3V_NATIVE_TARGET_FORMAT`) the query advertises as attachment-capable | Same fixed-format-table constraint as the `CreateImage` rows; no separate fix if the hypothesis holds | Host-provable |

## Table 2: cases closed per mechanism (ranked)

| Rank | Mechanism | Cases closed | dEQP groups touched |
|---|---|---|---|
| 1 | `OPTIMAL`-tiled `R8G8B8A8_UNORM` image cell in `r3v_CreateImage` | up to 100, pending the unconfirmed `usage` field | `dEQP-VK.memory.pipeline_barrier.*` |
| 2 | Buffer-view admission in `r3v_CreateBufferView` | 18 | `dEQP-VK.api.object_management.*` |
| 3 | `OpNot` admission in the compute SPIR-V front-end plus a CPU route for the resulting job | 12 | `dEQP-VK.api.object_management.*` (10), `dEQP-VK.api.descriptor_set.*` (2) |
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
| 1 | `r3v: admit an OPTIMAL-tiled R8G8B8A8_UNORM image cell in CreateImage` | Second fixed `VkImageCreateInfo` cell (Table 1, row 1); first traces the exact `usage` `findMaxRGBA8ImageSize`'s `vertex_buffer_stride` callers pass, so the cell's admitted usage set is evidence-grounded rather than guessed | `dEQP-VK.memory.pipeline_barrier.all*` and `.all_device*` `vertex_buffer_stride` cases | up to 100 | An image request combining the new cell's usage bits with a third, unadmitted format still refuses |
| 2 | `r3v: admit buffer views over the existing transfer buffer route` | `r3v_CreateBufferView` (Table 1, row 3) | `dEQP-VK.api.object_management.*` (`buffer_view_storage_*`, `buffer_view_uniform_*`) | 18 | A `VkBufferViewCreateInfo` whose `range` exceeds the parent buffer's transfer footprint still refuses |
| 3 | `r300: admit OpNot and land the CPU route it needs` | `r300_compute_spirv.c` opcode walk plus `r300_compute_verb.c` row plus a `r300_cpu_compute_job.c` kernel (Table 1, row 4) | `dEQP-VK.api.object_management.*compute_pipeline*`, `dEQP-VK.api.descriptor_set.descriptor_set_layout_binding.update_subsequent_binding`, `.descriptor_set_layout_lifetime.compute` | 12 | A second unary opcode (e.g. `OpSNegate`, already in the 126-141 arithmetic band but still refused) stays outside the admitted grammar and still refuses at the front end |
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

## Residual

The `numExtensions == properties.size()` race (1 case, outside every
mechanism above) and the general non-pass ledger having no rows yet for
any of these `Fail` texts (`tests/r3v_conformance_nonpass_ledger.tsv`)
are both out of scope for this decomposition; each PR above adds its own
ledger rows as part of landing, per `check-ledgers`' requirement that
every row carry a witness case.
