# R3V implementation boundaries and Vulkan completion criteria

## Status

The current-source statements in this document describe
`mesa-26-gororoba` at the commit that carries this document revision;
`git log -1 -- docs/hardware/r3v-implementation-boundaries.md` names it.

The current Gallium-backed implementation is live code. The native R3V ICD
exists as a distinct Gallium-free library with an owned Radeon DRM transport,
GEM-backed memory whose host coherency the driver maintains itself over the
unsnooped GART, queue and command-carrier objects, and one privately
injected fixed-cell PM4 lowering path carrying a compiler-produced fragment
program. Submission sits behind a multi-factor arming conjunction with
one-shot disarm, and both the semantic cell and the exact submit object
retain as digest-bound evidence. Its evidence stands at the host-unit,
build/link, no-submit PM4, offline kernel-parser, drm-shim host-model, and
one-shot silicon classes: three attended armed submissions on RS482 were
accepted and retired clean by the running radeon kernel. The first
submitted the bare inherited-state cell and left the target at its
sentinel fill; the direct-write 2D control then proved the transport
carries device writes byte-exact through the same BO, cache,
relocation, and readback substrate; and the contract-prefixed 234-dword
cell rendered the triangle as predicted -- interior `0xff00ff00`,
exterior and canary at the sentinel -- so the inert first run is closed
as a first-draw state-contract failure. The proven raster path is that
one fixed cell.  The public recording surface now reaches it: a bounded
render-pass/pipeline/draw vocabulary -- the qualified 64x64 linear
color target, a pipeline admitted by byte equality with the reference
SPIR-V pair and the cell's fixed state vector, and a draw that gathers
the bound vertex buffer through the CPU executor into a
command-buffer-owned carrier -- records the byte-identical cell IB
through public `vkCmd*` entry points, at the drm-shim host-model class
under the `r3v-native-public-surface` harness, and every contract
deviation poisons or refuses.  The loader boundary is proven at the
same host-model class: the `r3v-native-loader-application` gate links a
standalone application against the installed Vulkan loader alone,
reaches the ICD only through its manifest, performs the complete
instance-to-submit sequence, and byte-compares the submit-retained IB
against the independently emitted reference cell, while its symbol
audit holds the binary free of every audited driver-symbol prefix in
any binding, the reference SPIR-V data header being the one driver
artifact the application compiles in.
The recorded IB equals the digest the
arming authority qualifies, so the command-stream grammar the silicon
witnessed is what the public route records; the witness's rendered
pixels are bound to the reference vertex payload the attended run
carried, and a public draw with other in-range records or a nonzero
`firstVertex` changes the carrier bytes the same IB fetches, an input
set the silicon has not yet observed.  General vertex routes and the
complete Vulkan semantic/conformance sections remain implementation
contracts.

Two capability claims outrun the one-cell surface, and each is a
recorded deferred conformance gap whose removal path is the native
transfer and compute expansion of the order below.  The graphics queue
bit is required for `vkCmdDraw` validity, and the registry grants
every graphics family the core transfer commands; the recording
surface poisons those commands, so they fail closed with a reported
error rather than misbehave, and the gap closes when native copies
execute.  Vulkan 1.0 requires an implementation that exposes graphics
to expose at least one family supporting both graphics and compute;
the native family carries graphics alone, the compute commands fail
closed, and the gap closes when a native compute route lands.  The
extent gap is closed: `vkCreateImage` accepts every extent inside the
reported 64x64 maximum, and the cell family realizes it -- in TCL
bypass the extent reaches the hardware through the `SC_SCISSORS_BR`
and `SC_CLIPRECT_BR_0` payloads alone, `RB3D_COLORPITCH0` keeps the
64-pixel word because pitch is a memory-layout property, and at the
maximum extent the emission is byte-identical to the silicon-witnessed
reference cell, whose digest anchors the family.  A non-maximum extent
differs from the witnessed IB in those two dwords, an input class the
silicon has not yet observed; that narrowing rides the same
host-model-versus-silicon boundary as the vertex payload note above.

The bounded R300 R2VB `FLOAT_2` source transaction has one home:
`r300-r2vb-float2-source-contract.md`. This document owns the implementation
and conformance boundaries; that document owns the source-format transaction.

## Purpose

R3V has three distinct boundaries:

1. the current Gallium-backed experimental Vulkan ICD;
2. a future native R3V implementation with R3V-owned memory, queues, command
   lowering, PM4, and completion;
3. complete Vulkan command semantics, synchronization, WSI, feature exposure,
   and conformance.

A result at one boundary never closes another. Source architecture, build and
link identity, runtime reachability, silicon execution, API semantics, and
conformance remain separate evidence classes.

The Linux Radeon driver owns memory placement, command validation, relocation,
submission, completion primitives, and hazard containment. Mesa userspace owns
Vulkan objects, R300 state construction, vertex semantics, route selection, and
execution planning. Kernel acceptance proves that a submitted tuple is safe; it
does not supply missing userspace semantics.

PALM and Terakan provide reusable direct-DRM engineering patterns. Their
Evergreen register values, packet semantics, cache rules, shader ISA, and
silicon results are not RS480 authority.

## Current-source authority

Each current-implementation claim binds to a named Mesa source location and a
structural query.

| Claim | Mesa authority | Structural query |
|---|---|---|
| Gallium-backed R3V build and link boundary | `src/meson.build`; `src/amd/r300/vulkan/meson.build` | `rg -n 'r3v-gallium-backend\|driver_r300\|libgalliumvl' src/meson.build src/amd/r300/vulkan/meson.build` |
| `r3v_device` Gallium ownership | `src/amd/r300/vulkan/r3v_device.h`; `r3v_device.c` | `rg -n 'struct r3v_device\|radeon_winsys\|pipe_screen\|pipe_context' src/amd/r300/vulkan/r3v_device.h` |
| Gallium queue replay and fence completion | `src/amd/r300/vulkan/r3v_queue.c` | `rg -n 'pipe->flush\|fence_finish' src/amd/r300/vulkan/r3v_queue.c` |
| Direct-backend consent still falls through to Gallium | `src/amd/r300/vulkan/r3v_queue.c`; `r3v_device.c` | `rg -n 'R3V_CS_DIRECT_BACKEND_HAZARD_ACCEPTED' src/amd/r300/vulkan/` |
| Extracted shader descriptors still borrow Gallium-owned storage | `src/gallium/drivers/r300/r300_public.h`; `src/amd/r300/vulkan/r3v_pipeline.c` | `rg -n 'extract' src/gallium/drivers/r300/r300_public.h src/amd/r300/vulkan/r3v_pipeline.c` |
| Unsupported compute shapes can complete without execution | `src/amd/r300/vulkan/r3v_pipeline.c`; `r3v_cmd_buffer.c` | `rg -n 'R300_COMPUTE_REJECT_UNKNOWN_SHAPE\|no-op' src/amd/r300/vulkan/r3v_pipeline.c` |
| Current R2VB source and delivery domains | `src/gallium/drivers/r300/r300_r2vb.c`; `src/amd/r300/common/r300_r2vb_source_contract.h` | `rg -n 'producer_input_preflight\|delivery_element_preflight' src/gallium/drivers/r300/r300_r2vb.c` |
| Native ICD build identity and separation audit | `meson.options`; `src/amd/r300/vulkan/meson.build` | `rg -n 'r3v-native-backend\|libvulkan_r3v_native\|separation' meson.options src/amd/r300/vulkan/meson.build` |
| Gallium-free Radeon DRM transport | `src/amd/radeon/drm_vk/` | `rg -n 'radeon_drm_vk_cs_build\|radeon_drm_vk_completion' src/amd/radeon/drm_vk/` |
| Native submission arms on a multi-factor conjunction with one-shot disarm | `src/amd/r300/vulkan/r3v_native_arming.c`; `r3v_native_queue.c` | `rg -n 'r3v_native_arming_evaluate\|attempt.token' src/amd/r300/vulkan/` |
| Native submit gate fails closed and retains cell and submit object by digest | `src/amd/r300/vulkan/r3v_native_queue.c` | `rg -n 'R3V_NATIVE_SUBMIT_HAZARD_ACCEPTED\|R3V_NATIVE_MANIFEST_DIR\|submit_manifest' src/amd/r300/vulkan/` |
| Native host coherency over the unsnooped GART | `src/amd/radeon/drm_vk/radeon_drm_vk_bo.c`; `src/amd/r300/vulkan/r3v_physical_device.c` | `rg -n 'radeon_drm_vk_bo_cache_sync\|HOST_CACHED' src/amd/` |
| Triangle-cell fragment program is compiler output | `src/gallium/drivers/r300/compiler/tests/r300_tcl_bypass_fs_tool.c`; `src/amd/r300/common/r300_tcl_bypass_triangle_fs_block.h` | `rg -n 'r300_tcl_bypass_fs_tool\|r300_tcl_bypass_triangle_fs_block' src/` |
| Private fixed-cell recording outside the ICD export surface | `src/amd/r300/vulkan/r3v_native_cell.c`; `r3v_native.h` | `rg -n 'r3v_native_record_tcl_bypass_triangle' src/amd/r300/vulkan/` |
| Public recording surface: image, view, pipeline, and draw subset | `src/amd/r300/vulkan/r3v_native_image.c`; `r3v_native_pipeline.c`; `r3v_native_draw.c` | `rg -n 'r3v_CmdDraw\|r3v_CreateImage\|NATIVE_LIVE_CMDS' src/amd/r300/vulkan/` |
| Deferred draw execution at queue submission | `src/amd/r300/vulkan/r3v_native_cell.c`; `r3v_native_queue.c` | `rg -n 'execute_deferred_draw' src/amd/r300/vulkan/` |
| Native queue GRAPHICS advertisement and format subset | `src/amd/r300/vulkan/r3v_physical_device.c` | `rg -n 'VK_QUEUE_GRAPHICS_BIT\|R3V_NATIVE_BACKEND' src/amd/r300/vulkan/r3v_physical_device.c` |
| Reference SPIR-V admission pair and its generator | `src/amd/r300/vulkan/r3v_native_reference_spirv.h`; `shaders/generate_reference_spirv.py` | `rg -n 'r3v_reference_vertex_spirv\|generate_reference_spirv' src/amd/r300/vulkan/` |
| Loader-boundary application gate and its symbol audit | `src/amd/r300/vulkan/tests/r3v_native_loader_application.c`; `src/amd/r300/vulkan/tests/r3v_native_loader_application_symbol_audit.py`; `src/amd/r300/vulkan/meson.build` | `rg -n 'r3v-native-loader-application|FORBIDDEN_PREFIXES|R3V_EXPECTED_ICD_DSO' src/amd/r300/vulkan/` |

## Current Gallium-backed R3V implementation

### Ownership boundary

The functional R3V ICD is built with Gallium r300 support and links
`driver_r300`, `libgallium`, and `libgalliumvl` into
`libvulkan_r3v.so`.

`struct r3v_device` owns a `radeon_winsys`, `pipe_screen`, and
`pipe_context`. Vulkan buffers, images, and device memory own or borrow
`pipe_resource` objects. Queue submission replays recorded commands through
Gallium, flushes the `pipe_context`, waits for a Gallium fence, and synchronizes
host-shadow resources.

The `R3V_CS_DIRECT_BACKEND_HAZARD_ACCEPTED=1` selector records explicit consent
for direct submission experiments. It does not select an implemented direct
backend; submission still executes the Gallium replay path.

The r300 extraction API exposes precompiled shader descriptors, including the
fragment US/FG PM4 block. Those descriptors still reference Gallium CSO
storage. They are bridge inputs, not R3V-owned native binaries.

### Included Mesa mechanisms

| Surface | Current mechanism |
|---|---|
| OpenGL and GLES | Gallium state trackers over r300g |
| Experimental Vulkan | SPIR-V and Vulkan commands lowered into Gallium CSOs and `pipe_context` replay |
| NIR ingress | r300 NIR lowering, `nir_to_rc`, and the direct Draw NIR executor where admitted |
| NIR compatibility | `nir_to_tgsi` for Draw shapes outside the direct executor |
| CPU vertex execution | Gallium Draw SW TCL, including direct NIR, TGSI, and optional LLVM lanes |
| R300 graphics | RC compilation plus VAP, PSC, RS, US, TX, CB, ZB, ROP, viewport, and raster state |
| R2VB | fragment-ALU producer, CB export to GTT, cache publication, and TCL-bypass re-ingest |
| Graphics-as-compute | explicitly admitted raster kernels and multipass carriers |
| Video | Gallium VL MPEG-1/MPEG-2 shader decode and separately gated experiments |
| Memory and transfers | `pipe_resource`, Gallium winsys BOs, maps, uploads, copies, blits, and clears |
| Queue and completion | synchronous Gallium replay and Gallium fences |
| WSI | common WSI over Gallium-exported resources or a separate software fallback |
| Host modeling | Radeon drm-shim identity, BO-domain, and ioctl models |

The Xserver, glamor packaging, Radeon DDX, KMS policy, installed package
identity, kernel parser, and retained target bundles are qualification
dependencies or evidence authorities. Their source does not become Mesa-owned
by participating in the end-to-end qualification boundary.

### Maintenance criteria

A capability in the Gallium-backed implementation is current only when:

- its tests are present in the normal build graph;
- every admitted command executes its documented semantics;
- unsupported commands fail or decline at a documented boundary;
- source, build, runtime, silicon, conformance, and deployment evidence are
  labeled separately;
- current Mesa, kernel, package, and target identities are retained for
  hardware claims;
- each verdict producer has known-good and known-bad calibration;
- PALM or Terakan silicon observations are not promoted into RS480 facts.

The Gallium-backed ICD may remain intentionally nonconformant. It must still be
semantically honest inside every capability it exposes.

## Native Radeon DRM R3V implementation

### Required ownership

A native R3V ICD owns its Vulkan objects, BOs, memory bindings, command buffers,
execution graph, queues, PM4, synchronization, and completion. Its complete
functional build omits runtime ownership by `driver_r300`, `libgallium`,
`libgalliumvl`, `pipe_context`, `pipe_screen`, `pipe_resource`, and Gallium CSOs.

A loader-only skeleton is not the native implementation. A direct selector that
falls back to Gallium is not the native implementation. Extracted PM4 that
still aliases a Gallium CSO is not native ownership.

### Current native state

`-Dr3v-native-backend=true` builds `libvulkan_r3v_native.so` beside the
Gallium-backed ICD; the separation audit holds its exports to the three
`vk_icd*` symbols and its dependency list free of Gallium runtime libraries.
The landed mechanisms are:

- the Gallium-free transport `src/amd/radeon/drm_vk/` (ioctl vtable seam,
  BO/PRIME refcount, relocation dedupe, three-chunk CS build/submit split,
  finite completion via a write-domain BO plus bounded `GEM_WAIT_IDLE`);
- deep-copied fragment binaries (`r300_fragment_binary`) with content hash
  and structural validator;
- native device, memory (one owned GEM BO per `VkDeviceMemory`), buffer,
  image, image-view, pipeline, queue, and command-carrier objects;
  reporting narrowed to executable routes: the queue family advertises
  `VK_QUEUE_GRAPHICS_BIT` for the recording surface, format properties
  advertise the accepted subset (the linear B8G8R8A8 color target and
  the F32-family vertex formats), and one UMA heap sizes from
  `DRM_RADEON_GEM_INFO`;
- the public graphics recording surface: the qualified 64x64 linear
  image and identity view, the pipeline admitted by byte equality with
  the reference SPIR-V pair and the cell's fixed state vector, and the
  render-pass/bind/draw command subset whose draw lowers through the
  CPU vertex carrier into the fixed cell, with the vertex gather and
  load-op clear executing at queue submission;
- the fixed TCL-bypass triangle lowered into a native command buffer by
  `r3v_native_record_tcl_bypass_triangle`, a private entry linked directly
  by the pre-hardware harness; the recording opens with the neutral
  first-draw state contract (`src/amd/r300/common/r300_first_draw_state.c`),
  emitted in pipeline order and proven self-establishing by the
  poison-model checker; `vkBeginCommandBuffer` and
  `vkEndCommandBuffer` record nothing themselves;
- the exact-value submit gate `R3V_NATIVE_SUBMIT_HAZARD_ACCEPTED=1`; the
  closed gate retains the IB, relocation list, and manifest under
  `R3V_NATIVE_MANIFEST_DIR` and fails closed with `VK_ERROR_DEVICE_LOST`;
- the drm-shim triangle-cell harness driving both gate states, with the
  closed-gate retained IB byte-identical to the direct emitter.

Compute pipelines, descriptors, transfers, WSI, extents and formats
past the fixed cell, and native R2VB remain outside the landed
surface. Live `DRM_RADEON_CS` submission
has three attended witnesses, each kernel-accepted and retired clean:
the bare inherited-state cell left the color target unwritten, the
direct-write 2D control landed its probe bytes exactly, and the
contract-prefixed successor cell rendered the triangle as predicted;
the witness bundle is the frozen private-cell reference.

### Source-layer split

| Layer | Authority |
|---|---|
| `src/amd/radeon/drm_vk/` | Radeon DRM BO, map, PRIME, relocation, submission, and finite completion transport |
| `src/amd/r300/common/` | RS480/R300 device facts, formats, packet fields, state packs, barriers, and validators |
| `src/amd/r300/cpu/` | portable byte-defined vertex execution baseline plus measured per-target tuned paths |
| `src/amd/r300/vulkan/` | Vulkan objects, command lowering, execution graph, queue policy, images, WSI, and entry points |

The shared Radeon DRM layer contains no R300 or Evergreen graphics state.
R300 packet values, registers, tiling, cache operations, shader metadata,
vertex tuples, and R2VB semantics stay in the R300 layers.

### Native object graph

```text
r3v_instance
  -> r3v_physical_device
       -> render-node fd
       -> RS480 device information
       -> one UMA budget model
  -> r3v_device
       -> radeon_drm_vk_device
       -> completion service
       -> native queues and object registries
  -> r3v_device_memory
       -> one owned r3v_bo
  -> r3v_buffer / r3v_image
       -> bound range or layout views of memory BOs
  -> r3v_pipeline
       -> owned shader binaries and immutable R300 state packs
  -> r3v_cmd_buffer
       -> Vulkan-semantic command records
  -> r3v_exec_graph
       -> resource-scoped execution nodes
```

`VkBuffer` and `VkImage` create metadata. Memory binding installs a range or
layout view into an already allocated `VkDeviceMemory` BO. Host mapping maps the
owned BO under an explicit unsnooped-UMA visibility contract.

### Owned pipeline binaries

The existing r300 extraction API is a migration bridge. A native pipeline must
deep-copy every consumed word and subordinate table into R3V-owned storage
before the Gallium CSO can be destroyed.

The first owned fragment binary carries:

- the US/FG PM4 block;
- immutable validation metadata;
- external and state-constant layout;
- a content hash;
- source compiler identity.

A temporary build-time compiler bridge may produce the binary. The runtime
artifact and queue path must remain valid after all Gallium objects are
released.

### Native execution and first hardware witness

A native command buffer lowers to explicit resource-scoped nodes such as
`CPU_VERTEX`, `R2VB_PRODUCER`, `PM4_DRAW`, `COPY`, `CLEAR`, `BARRIER`, and
`PRESENT`. Each node names BO reads and writes, byte ranges, domains,
coordinate space, precision contract, and predecessors.

The first native hardware witness is one pretransformed TCL-bypass triangle,
not PVS and not R2VB. It uses:

- one GTT vertex BO and one color BO;
- one `FLOAT_4` position stream;
- identity `XYZW` PSC selectors;
- `VAP_VTX_SIZE = 4`;
- position-only VAP output;
- no index buffer, instancing, user clip planes, query, texture, or external
  shader constants;
- one owned fragment binary;
- explicit cache/VAP publication;
- one complete relocation list;
- one finite completion object.

The no-submit form first fixes PM4, relocation identity, state coverage, and
command size. Radeon shim results remain host-model evidence. Offline kernel
replay proves parser acceptance and calibrated malformed rejection. Only then
does an attended RS480-family target submit the known-good cell.

### CPU vertex execution and R2VB migration

The first general native vertex route is NIR-driven but independent of Gallium
Draw ownership:

```text
Vulkan vertex and index state
-> byte-defined portable baseline (any host endianness; R300-era hosts
   span x86, x86-64, and PowerPC)
-> per-target tuned path only where a measurement on that target
   justifies it (K8 is the primary measured target; general code speed
   rides the build profile's compiler flags)
-> direct writes into the final mapped GTT carrier
-> TCL-bypass delivery
```

R2VB migration follows the fixed triangle and CPU route:

1. migrate the `FLOAT_4` identity source control;
2. migrate the qualified `FLOAT_3` producer source with `XYZ1` reconstruction;
3. add `FLOAT_2` only after `XY01` userspace and kernel validators agree;
4. migrate qualified count, grid, and topology cells;
5. add computed varyings one measured shape at a time;
6. add hybrid carriers only with an explicit final VAP join.

The native route owns its BOs, PM4, packers, barriers, and completion. Calling
the Gallium R2VB function from a native queue remains Gallium-backed execution.

### Native WSI

Native WSI begins after native BO ownership and export identity are established:

```text
native image-memory BO
-> PRIME dma-buf export
-> identical BO at common WSI
-> same-GPU presentation
-> retirement and reuse
```

X11 and Wayland are independent qualification cells. A Gallium resource export
does not close native presentation.

### Completion criteria

The native implementation is complete only when:

- native and Gallium-backed ICDs have distinct build and runtime identities;
- the complete native functional target links without Gallium runtime
  ownership;
- dependency, symbol, and include audits prove that separation;
- memory owns real BOs and bound objects are views;
- queues submit bounded direct PM4 and complete finitely;
- the first valid PM4 cell retires and a malformed control fails closed;
- CPU vertex and migrated R2VB paths retain exact output oracles;
- every advertised native capability has current kernel and target evidence.

## Complete Vulkan semantics and conformance

Complete Vulkan support begins from a working native implementation and closes
the API contract. It requires:

- authoritative image-format, creation, and memory-binding rules;
- complete aliasing, mapping, flush, invalidate, and external-memory semantics;
- resource-scoped queue ordering, fences, semaphores, events, and barriers;
- native transfers, clears, render passes, dynamic rendering, queries, and
  presentation;
- complete graphics pipeline and vertex-interface semantics;
- compute only when workgroups, descriptors, storage access, atomics, barriers,
  and dispatch semantics exist;
- no successful no-op command;
- each advertised X11 and Wayland WSI surface;
- feature and extension tables generated from implemented behavior;
- CTS, dEQP, Piglit, and workload evidence appropriate to every exposed
  surface;
- default promotion only after image, queue, memory, execution, and WSI
  identities are current and replayable.

Complete Vulkan semantics never follow merely because Gallium emulates an
operation or because the native queue can submit one direct draw.

## R300 extraction boundary

Suitable common value-type mechanisms include:

- neutral vertex format records;
- DATA_TYPE and component-selector packing;
- checked source and destination extents;
- FP24 constant packing;
- shader admission cost records;
- deep-copied fragment binary descriptors;
- immutable blend, DSA, raster, viewport, scissor, and output-format packs;
- VAP, PSC, and RS tuple construction and validation;
- R2VB slot layout and source contracts;
- PM4 packet and relocation writers;
- cache and role-transition barrier packs;
- texture and image layout arithmetic with value-type inputs.

R300-specific mechanisms remain under R300 common or Vulkan code:

- RS480 family capabilities and quirks;
- invariant and VAP-invariant register values;
- R300 shader ISA and RC metadata;
- VAP, PSC, RS, GA, US, TX, CB, ZB, and ROP registers;
- R300 texture layout and tiling rules;
- R300 cache publication and flush sequences;
- R300 draw packet construction;
- R2VB producer and delivery semantics.

Gallium-owned objects remain in the Gallium-backed implementation:
`pipe_context`, `pipe_screen`, `pipe_resource`, dirty atoms, CSO lifetime,
Draw/vbuf ownership, `u_upload`, `u_blitter`, transfer helpers, VL decoder
ownership, and Gallium winsys command buffers and fences.

A helper becomes common code only after its inputs and outputs are independent
value types and its tests run without a Gallium object.

## R2VB source-format transition

The neutral source contract defines:

```text
F32_2 -> 2 physical dwords -> XY01 logical vec4
F32_3 -> 3 physical dwords -> XYZ1 logical vec4
F32_4 -> 4 physical dwords -> XYZW logical vec4
```

The live automatic R2VB producer admits `F32_3` and `F32_4`. Final delivery
admits FP32x4 only. `F32_2` remains outside the live route.

The integration order, with steps 1 through 4 landed at the pinned head:

1. keep the neutral source and Gallium-adapter tests in the normal build
   (landed);
2. route existing `F32_3` and `F32_4` construction through the neutral contract
   with byte-identical PM4 controls (landed; pinned by
   `r300_r2vb_psc_byte_identity_test`);
3. add an exact `F32_2` source gate that never rides `R300_R2VB_STANDING`
   (landed as `R300_R2VB_FLOAT2_SOURCE`);
4. capture the six-dword `FLOAT_4 + FLOAT_2` producer tuple without submit
   (landed; pinned by `r300_r2vb_float2_tuple_test`);
5. extend userspace and kernel validators from identity-only PSC to explicit
   synthesized-lane contracts;
6. run the bounded `FLOAT_2` silicon ladder;
7. decide standing promotion in a separate change.

The native implementation migrates `F32_3` before `F32_2`. Producer support for
a narrow source never implies narrow final-delivery support.

## Repository authority

| Repository | Authority |
|---|---|
| `mesa-26-gororoba` | Gallium-backed and native R3V userspace, compilers, state packs, R2VB, WSI, and tests |
| `steinmarder-r300` | RS480 frontier, probes, falsifiers, findings, manifests, and target result bundles |
| `vostro1000-re` | K8 and platform behavior plus CPU-executor qualification |
| `linux-radeon-gororoba` | Radeon parser, GEM, GART, faults, completion, recovery, and containment |
| `radeon-custom` | source pin, package construction, deployment transition, rollback, and installed runtime identity |
| Xserver and Radeon DDX repositories | X11 source, package, and installed-image authority |
| `steinmarder-r600-terakan` | reusable process patterns and PALM evidence, never RS480 hardware facts |

Mesa behavior changes land in Mesa. Kernel changes land in the kernel source
repository. Package policy lands in the package repository. Target evidence and
findings remain in the evidence repository.

## Ordered development

Steps 2 through 8 are landed at the pinned head; step 8's evidence stands at
the no-submit, drm-shim, and offline kernel-parser classes.

1. Keep the Gallium-backed implementation current and semantically honest.
2. Refactor existing `F32_3` and `F32_4` R2VB construction through the neutral
   source contract (landed).
3. Land the gated `F32_2` no-submit producer transaction and validators
   (landed; the synthesized-lane validator extension remains open).
4. Extract a generic Radeon DRM transport layer with host tests (landed).
5. Deep-copy R300 fragment binaries into R3V-owned storage (landed).
6. Create distinct Gallium-backed and native ICD identities (landed).
7. Build native BO, memory, command, queue, and completion ownership (landed;
   buffer-only memory, private fixed-cell recording).
8. Emit and offline-validate the fixed identity-bypass triangle (landed).
9. Run the attended native triangle cell (landed: the contract-prefixed
   cell rendered as predicted on RS482; procedure, arming, and the
   retained record live in
   `docs/hardware/r3v-native-attended-cell-procedure.md`, and the
   witness bundle is the frozen private-cell reference).
10. Build and qualify the native CPU vertex executor (gather stage and
    carrier delivery landed: `src/amd/r300/cpu/` carries the portable
    byte-defined baseline and the SSE2/SSE3 tuned candidates under the
    `r300-cpu-vertex` oracle at the host-unit class, and the stream-fed
    recorder delivers through `r300_cpu_vertex_gather`; the
    `r300_cpu_vertex_bench` three-way measurement -- baseline versus
    SSE2 versus SSE3 against the memcpy copy ceiling, on the K8 target
    under the `k8-sse3` profile flags -- remains open and decides which
    candidate the auto dispatch keeps).
11. Migrate native R2VB `F32_3`, then `F32_2`.
12. Add native images, transfers, and resource-scoped synchronization.
13. Prove native same-GPU WSI.
14. Complete Vulkan semantics and conformance before default promotion.

The Gallium-backed implementation remains the differential reference and a
useful bounded acceleration path while native work proceeds.

## Evidence classes

Every result names one class:

- source proof;
- host unit proof;
- build and link proof;
- no-submit PM4 proof;
- offline kernel-parser proof;
- attended kernel-submission proof;
- silicon-output proof;
- conformance proof;
- deployment proof.

A higher class never appears by implication. A Radeon shim result is a host
model. Parser acceptance is not execution. Fence retirement is not output
correctness. An output hash from an old Mesa image is not evidence for the
current head.
