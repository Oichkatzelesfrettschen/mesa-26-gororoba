# R3V product denominators: Gallium, native, and complete Vulkan

## Purpose

R3V work has three independent product denominators.  They share compiler,
hardware, and evidence inputs, but one denominator never closes another.

- Program L is the explicit Gallium legacy product.
- Program N is a separate native R3V product.
- Program P is the complete Vulkan product.

A change belongs to the product that owns the live object and execution path.
Silicon reachability, source architecture, attended hardware evidence, and API
semantic completeness remain separate closure axes.

The Linux Radeon driver owns memory placement, command validation, relocation,
submission, completion, and hazard containment.  Userspace owns Vulkan objects,
vertex semantics, R300 state construction, route selection, and execution
planning.  The kernel proves a submitted tuple safe; it does not invent missing
vertex work.

PALM and Terakan provide reusable direct-DRM interface patterns.  Their
Evergreen registers, packet semantics, cache rules, shader ISA, and silicon
measurements never serve as RS480 authority.

## Current implementation boundary

The functional R3V build is the Gallium product.  Its Meson arm adds the device,
queue, memory, buffer, image, command, descriptor, and pipeline objects only
when `r3v-gallium-backend` and Gallium r300 are enabled.  That arm links
`driver_r300`, `libgallium`, and `libgalliumvl` into one
`libvulkan_r3v.so`.

`struct r3v_device` owns `radeon_winsys`, `pipe_screen`, and `pipe_context`.
`VkBuffer`, `VkImage`, and `VkDeviceMemory` own or borrow `pipe_resource`
objects.  Queue submission waits without a finite driver timeout, replays
through `pipe_context`, flushes Gallium, waits for the Gallium fence, and
synchronizes host shadow resources.

`R3V_CS_DIRECT_BACKEND_HAZARD_ACCEPTED=1` records consent and prints that the
direct backend is absent.  The submit still executes the Gallium replay.

The r300 public extraction API now exposes precompiled vertex and fragment
descriptors.  The fragment descriptor includes the pre-baked US/FG PM4 block,
but its pointers alias Gallium CSO storage.  Extraction is therefore a bridge
input, not native binary ownership.

The hybrid compute classifier recognizes a large graphics-as-compute corpus.
A rejected or unrecognized kernel can still produce a successful pipeline and
a successful no-op dispatch.  That behavior is an explicit semantic defect and
never counts toward Program P.

## Program L: Gallium RS482 product

### Product definition

Program L is every functional RS482 path whose live ownership reaches Gallium
Draw, r300g, Gallium resources, Gallium state objects, Gallium VL, or the
Gallium Radeon winsys.  It is a real product and a differential authority.  It
is not native R3V progress.

Program L includes the following surfaces.

| Surface | Program L mechanism |
|---|---|
| OpenGL and GLES | Gallium state trackers over r300g |
| Experimental R3V Vulkan | SPIR-V and Vulkan commands lowered into Gallium CSOs and `pipe_context` replay |
| NIR ingress | r300 NIR lowering, `nir_to_rc`, and the direct Draw NIR executor where admitted |
| NIR compatibility bridge | `nir_to_tgsi` for Draw shapes outside the direct executor |
| CPU vertex execution | Gallium Draw SW TCL, including the direct NIR interpreter and the existing TGSI or LLVM execution lanes |
| R300 fragment execution | RC compilation, US/PFS programs, textures, CB, ZB, ROP, and raster state |
| R2VB | fragment ALU producer, CB export to GTT, cache publication, and TCL_BYPASS re-ingest |
| Graphics-as-compute | admitted raster kernels, texture gathers, ROP operations, reductions, and multipass carriers |
| Video acceleration | Gallium VL MPEG-1/MPEG-2 shader decode and the separately gated H.264 experiment |
| Transfers and clears | Gallium maps, blits, uploads, copies, clears, and resource transitions |
| Memory | `pipe_resource`, Gallium winsys BOs, CPU shadows, uploads, and explicit copies |
| Queue | synchronous Gallium replay, global submit-boundary memory synchronization, and Gallium fences |
| WSI | common WSI over Gallium-exported resources or the explicit software fallback |
| X11 display | Xserver, glamor, Radeon DDX, KMS, scanout, and package identity |
| Evidence | drm-shim host models, no-submit captures, Steinmarder result bundles, kernel parser replay, and target runs |

### NIR-only direction

The direct Draw NIR executor is a substantive CPU vertex lane.  It lowers the
shader to executable input and output spans, evaluates ALU through
`nir_eval_const_opcode`, implements selected I/O and system-value intrinsics,
and executes phi nodes, conditionals, loops, and supported jumps.  Textures,
calls, unsupported intrinsics, and unsupported instruction classes remain on
the compatibility bridge.

The next NIR-only work is additive and measured:

1. Keep `DRAW_NIR_FORCE_BRIDGE=1` as the differential control.
2. Retain a named decline reason for every bridge conversion.
3. Neutralize the remaining output-allocation semantic-pair entry points where
   they obstruct removal of the bridge.
4. Widen the direct executor only with a corpus specimen, a known-bad control,
   and numerical parity against the bridge.
5. Keep a bridge for shapes whose semantics are not implemented.
6. Treat removal of `nir_to_tgsi` as a result of coverage, never as a source
   deletion target by itself.

A NIR-only Draw path remains Program L because Draw owns the execution object
and vertex marshaling.

### R300 compiler and state direction

Program L keeps one authoritative R300 lowering and state vocabulary.  The
important reusable mechanisms are:

- NIR optimization and lowering into RC;
- FP24 constant packing and exact-integer admission;
- shader resource measurement;
- vertex format, DATA_TYPE, and component-selector semantics;
- VAP, PSC, RS, CB, ZB, texture, viewport, and raster register construction;
- cache publication and role-transition barriers;
- PM4 and relocation decoding;
- complete state-tuple captures.

The Gallium dirty-atom scheduler remains the legacy state owner.  Pure packing
and validation functions should move behind value-type interfaces where that
reduces duplication, but Program L continues to consume them through the
existing atoms.

### R2VB direction

The standing RS482 route is:

```text
application vertex source
-> fragment-ALU producer
-> CB export into a GTT carrier
-> cache and VAP publication
-> TCL_BYPASS vertex fetch
-> ordinary raster and fragment delivery
```

Its current automatic product contract remains bounded by source format,
single-position source identity, constant source, topology, count, layout,
query state, clip state, fragment-state interference, output streams, and
final delivery format.  A named decline returns the draw to the CPU path.

The 2048-wide grid is the large-count representation.  The one-row raster stop
is a measured geometry law, not a reason to erase the exact grid route.

The R2VB planner, typed transport work, computed-varying route, and multipass
research remain Program L while they execute through `r300_context`,
`pipe_resource`, Gallium state, or Gallium command streams.

### Graphics-as-compute and VA direction

The graphics-as-compute corpus demonstrates useful R300 mechanisms, but it is
not a Vulkan compute queue.  Every admitted pattern needs a complete semantic
plan, precision contract, binding contract, output oracle, and synchronization
contract.  A successful no-op is prohibited even in the experimental product;
an unsupported shape must fail at a documented API boundary.

The VA lane is a separate Program L workload.  RS480 has no UVD or VCE block,
so Gallium VL reconstructs supported video through the 3D pipeline.  MPEG-1 and
MPEG-2 are the established surface.  H.264 remains an exact-gated experiment
whose CPU entropy provider and fragment back half need their own denominator.
Video results inform fragment, texture, blend, and render-target state, but they
do not prove Vulkan ownership.

### Program L closure

Program L closes a capability only when:

- the build graph includes its tests;
- every admitted command executes its documented semantics;
- every unsupported command declines or returns a documented failure;
- source, build, runtime, and silicon evidence are labeled separately;
- the current Mesa image, kernel, package, and target identities are retained;
- positive and negative controls calibrate every verdict producer;
- no result is promoted from a PALM or Terakan hardware observation.

Program L may remain intentionally nonconformant Vulkan.  It must still be
semantically honest inside every capability it advertises.

## Program N: native R3V product

### Product definition

Program N is a separate functional ICD whose Vulkan objects, memory, queues,
command buffers, execution graph, and PM4 are owned by R3V.  Its complete
functional build omits `driver_r300`, `libgallium`, `libgalliumvl`,
`pipe_context`, `pipe_screen`, `pipe_resource`, and Gallium CSO ownership.

A loader-only skeleton is not the native product.  A direct selector that falls
back to Gallium is not the native product.  Extracted PM4 that still aliases a
Gallium CSO is not native ownership.

### Source-layer split

The native implementation uses four mechanism layers.

| Layer | Authority |
|---|---|
| `src/amd/radeon/drm_vk/` | Radeon DRM BO, map, PRIME, relocation, submission, and completion transport shared by Radeon Vulkan drivers |
| `src/amd/r300/common/` | R300 and RS480 device facts, formats, packet fields, state packers, barriers, and validators |
| `src/amd/r300/cpu/` | scalar reference and K8-safe vertex execution |
| `src/amd/r300/vulkan/` | Vulkan objects, execution graph, command lowering, queue policy, images, WSI, and product entry points |

The shared Radeon DRM layer contains no Evergreen or R300 graphics state.  It
abstracts the stable DRM Radeon interface shape demonstrated by Terakan:

- GEM creation and close;
- mmap and unmap;
- PRIME import and export;
- BO domain and CPU-access policy;
- relocation references with read and write domains plus priority;
- `DRM_RADEON_CS` chunk construction and submission;
- finite completion using a driver-owned completion BO;
- explicit host write publication before submission;
- deterministic error translation and device-loss escalation.

R300 packet values, tiling, cache operations, vertex tuples, and shader state
stay in the R300 layer.

### Native object graph

The native object graph is:

```text
r3v_instance
  -> r3v_physical_device
       -> render-node fd
       -> RS480 device information
       -> one shared UMA budget model
  -> r3v_device
       -> radeon_drm_vk_device
       -> completion service
       -> native queues
       -> native object registries
  -> r3v_device_memory
       -> exactly one owned r3v_bo
  -> r3v_buffer
       -> a bound byte-range view of one memory BO
  -> r3v_image
       -> a bound layout view of one memory BO
  -> r3v_pipeline
       -> owned shader binaries
       -> owned immutable R300 state packs
  -> r3v_cmd_buffer
       -> Vulkan-semantic command records
  -> r3v_exec_graph
       -> resource-scoped execution nodes
```

`VkBuffer` and `VkImage` create metadata only.  Binding installs a range and
layout view into an already allocated `VkDeviceMemory` BO.  No resource is
created before memory binding.  Host mapping maps the owned BO directly under
an explicit unsnooped-UMA visibility contract.

### Owned pipeline binaries

The existing r300 extraction API is the bridge from Program L.  Program N must
deep-copy every consumed word and every subordinate table into R3V-owned
storage before the Gallium CSO can be destroyed.

The first owned fragment binary contains:

- the US and FG PM4 block;
- immutable shader metadata needed by state validation;
- external and state-constant layout;
- a content hash;
- the source compiler identity.

The native product must not retain a pointer into an r300g object.  A temporary
build-time compiler bridge may create the owned binary, but the resulting
runtime artifact and queue path must remain valid after all Gallium objects are
released.  Native build purity ultimately replaces that bridge with a common
R300 compiler library or an R3V-owned compiler path.

### Native execution graph

A command buffer lowers to explicit execution nodes rather than replaying a
global `pipe_context`.

Initial node classes are:

- `CPU_VERTEX`;
- `R2VB_PRODUCER`;
- `PM4_DRAW`;
- `COPY`;
- `CLEAR`;
- `BARRIER`;
- `PRESENT`.

Each node names its BO reads, BO writes, byte ranges, required domains,
coordinate space, precision contract, and predecessor set.  Queue submission
builds one bounded IB or a finite chain of IBs from ready nodes.

The queue is asynchronous with respect to the application.  It never copies all
mapped memory at every submit, never waits for the entire device without a
finite policy, and never uses host synchronization as a substitute for a
resource dependency.  BO references and completion objects keep resources
alive until retirement.

### First native acceptance witness

The first native hardware cell is not PVS and not R2VB.  It is one fixed
pretransformed TCL_BYPASS triangle with a complete same-IB state witness.

The cell uses:

- one real GTT vertex BO;
- one real color BO;
- one FLOAT_4 position stream;
- identity XYZW PSC selectors;
- `VAP_VTX_SIZE = 4`;
- position-only VAP output;
- no index buffer;
- no instancing;
- no user clip planes;
- no query;
- no texture;
- no external shader constants;
- one owned fragment binary;
- an explicit cache and VAP publication sequence;
- one complete relocation list;
- one completion BO.

The no-submit form first proves exact PM4, relocation identity, state coverage,
and fixed command size.  The Radeon shim proves BO and domain behavior but
never counts as execution.  Offline kernel replay proves parser acceptance for
the known-good tuple and rejection for a calibrated malformed underfeed tuple.
Only then does an attended RS482 run submit the known-good cell.

The current RAD-06 census makes complete-witness emission load-bearing.  Real
legacy draws inherit state across submissions and use nonidentity PSC, so the
identity-only kernel check is naturally dormant.  The first native writer must
emit the entire identity tuple in one IB instead of relying on inherited state.

### Native CPU vertex route

The first general vertex executor is CPU-owned:

```text
Vulkan vertex and index state
-> scalar semantic reference
-> K8 SSE2 or SSE3 specialization selected by measured object code
-> direct writes into the final mapped GTT carrier
-> TCL_BYPASS delivery
```

The executor is NIR-driven but independent of Gallium Draw object ownership.
The direct Draw NIR executor supplies a differential semantic reference and
corpus, not the native runtime object.

Every specialization records CPUID, compiler flags, final-object ISA, cache
state, thermal state, vertex layout, and end-to-end time.  SSE4, AVX, and FMA
never enter the target build.

### Native R2VB route

R2VB migration follows the fixed triangle and CPU route.

1. Migrate the exact FLOAT_4 identity source control.
2. Migrate the qualified FLOAT_3 producer source with XYZ1 reconstruction.
3. Add FLOAT_2 only after XY01 user and kernel validators agree.
4. Migrate the qualified grid count and topology cells.
5. Add computed varyings one measured shape at a time.
6. Add CPU/GPU hybrid carriers only with an explicit final VAP join.

The native route owns its BOs, PM4, packers, barriers, and completion.  Calling
the Gallium R2VB function from a native queue would remain Program L.

### Native WSI

Native WSI begins after native BO ownership and export identity close.

The acceptance chain is:

```text
native image memory BO
-> PRIME dma-buf export
-> same BO identity at common WSI
-> same-GPU presentation
-> retirement and reuse
```

X11 and Wayland are independent platform cells.  The current X11-only extension
table and Gallium resource export do not close either native cell.  The
software presentation mode stays a separate fallback product path.

### Program N closure

Program N closes only when:

- native and legacy products have distinct build and ICD identities;
- the complete native functional target links without Gallium;
- dependency, symbol, and include audits prove link purity;
- memory owns real BOs and bound objects are views;
- the queue submits direct bounded PM4 and completes finitely;
- the first valid PM4 cell retires and a malformed control fails closed;
- CPU vertex and migrated R2VB cells carry retained output oracles;
- every native capability has a current kernel and target evidence chain.

## Program P: complete Vulkan product

Program P begins from a working Program N product and closes API semantics.

It requires:

- one authoritative image-format and creation contract;
- complete memory aliasing, mapping, flush, invalidate, and external-memory
  semantics;
- resource-scoped queue ordering, fences, semaphores, events, and barriers;
- native transfers, clears, render passes, dynamic rendering, queries, and
  presentation;
- complete graphics pipeline state and vertex interface semantics;
- compute only when workgroups, descriptors, storage access, atomics, barriers,
  and dispatch semantics exist;
- no successful no-op command;
- X11 and Wayland WSI cells as advertised;
- feature and extension tables generated from implemented behavior;
- CTS, dEQP, Piglit, and workload evidence appropriate to every advertised
  surface;
- default promotion only after native image, queue, memory, execution, and WSI
  identities are current and replayable.

Program P never inherits a closure merely because Program L emulates the
operation through Gallium or because Program N can submit one direct draw.

## R300 state extraction map

### Extract as value-type mechanisms

These mechanisms are suitable for common R300 code:

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
- texture and image layout arithmetic whose inputs are value types.

### Keep R300-specific

These mechanisms belong under R300 common or Vulkan code, not the generic DRM
substrate:

- RS480 family capabilities and quirks;
- invariant and VAP-invariant register values;
- R300 shader ISA and RC metadata;
- VAP, PSC, RS, GA, US, TX, CB, ZB, and ROP registers;
- R300 texture layout and tiling rules;
- R300 cache publication and flush sequences;
- R300 draw packet construction;
- R2VB producer and delivery semantics.

### Keep legacy-only

These objects remain in Program L:

- `pipe_context`, `pipe_screen`, and `pipe_resource`;
- Gallium dirty atoms and state binding;
- Gallium CSO lifetime;
- Draw ownership and vbuf stages;
- `u_upload`, `u_blitter`, and Gallium transfer helpers;
- Gallium VL decoder ownership;
- Gallium winsys command buffers and fences.

A pure helper extracted from a legacy file becomes common code only after its
inputs and outputs are independent value types and its tests run without a
Gallium object.

## FLOAT_2 and FLOAT_3 transition

The neutral source contract defines:

```text
F32_2 -> 2 physical dwords -> XY01 logical vec4
F32_3 -> 3 physical dwords -> XYZ1 logical vec4
F32_4 -> 4 physical dwords -> XYZW logical vec4
```

The live automatic R2VB producer still admits F32_3 and F32_4.  Final delivery
still admits FP32x4 only.  F32_2 remains outside the live route.

Program L advances in this order:

1. Register the neutral source and Gallium adapter tests in Meson.
2. Route existing F32_3 and F32_4 construction through the neutral contract
   with byte-identical PM4 controls.
3. Add an exact F32_2 source gate that never rides
   `R300_R2VB_STANDING`.
4. Capture the six-dword FLOAT_4-plus-FLOAT_2 producer tuple without submit.
5. Extend user and kernel validators from identity-only PSC to explicit
   synthesized-lane contracts.
6. Run the bounded FLOAT_2 silicon ladder.
7. Decide standing promotion in a separate change.

Program N migrates F32_3 before F32_2.  Physical fetch width and logical
component availability remain distinct fields at every layer.  F32_2 or F32_3
producer support never implies F32_2 or F32_3 final delivery support.

## Cross-repository authority

| Repository | Authority |
|---|---|
| `mesa-26-gororoba` | Program L and Program N userspace implementation, compilers, state packs, R2VB, R3V, WSI, and tests |
| `steinmarder-r300` | RS482 frontier, findings, probes, manifests, falsifiers, and target result bundles |
| `steinmarder` | shared evidence engine, retained cross-lane mechanisms, and extracted-lane registry |
| `vostro1000-re` | K8 semantics, CPUID, platform behavior, watchdog and crash evidence, and CPU-executor qualification |
| `linux-radeon-gororoba` | Radeon kernel parser, GEM, GART, faults, completion, recovery, and hazard containment |
| `radeon-custom` | package, source pin, deployment transition, rollback, and installed runtime authority |
| `xserver-rs48x` | Xserver source-series and glamor authority |
| `xf86-video-ati-rs482` | Radeon DDX source and exact payload authority |
| `PKGBUILD_xf86-video-ati-rs482` | DDX package source-lock and installation authority |
| `umr-gororoba` | read-only register and command-stream inspection |
| `radeontop-gororoba` | read-only utilization and sampling evidence |
| `radeontool-gororoba` | bounded read-only register observation |
| `steinmarder-r600-terakan` | PALM and Terakan evidence only; transferable process patterns, never RS482 hardware facts |

A Mesa behavior patch lands in Mesa.  Kernel code lands in the kernel source
repository.  Package policy lands in the package repository.  Target evidence
and findings land in Steinmarder-r300.  Cross-repository claims cite every
source head and retain one manifest joining them.

## Ordered implementation

The ordered execution is:

1. Keep Program L current and semantically honest.
2. Wire the Program L format-contract tests into the normal build.
3. Refactor F32_3 and F32_4 through the neutral source contract.
4. Land the gated F32_2 no-submit producer contract.
5. Extract the generic Radeon direct-DRM transport with host tests.
6. Deep-copy the R300 fragment binary into owned storage.
7. Create distinct native and legacy product identities.
8. Build the native BO and memory object graph.
9. Emit and offline-validate the fixed identity-bypass triangle.
10. Run the attended native triangle cell.
11. Build and qualify the native K8 vertex executor.
12. Migrate native R2VB F32_3, then F32_2.
13. Add native images, transfers, and resource-scoped queue synchronization.
14. Prove native same-GPU WSI.
15. Close Program P semantics and conformance before default promotion.

Program L development continues in parallel.  Its evidence is the differential
reference for Program N, and its bounded acceleration remains useful after the
native product exists.

## Evidence classes

Every result names one class:

- source proof;
- host unit proof;
- build and link proof;
- no-submit PM4 proof;
- offline kernel-parser proof;
- attended kernel submission proof;
- silicon output proof;
- conformance proof;
- deployment proof.

A higher class never appears by implication.  The Radeon shim is a host model.
A parser PASS is not execution.  Fence retirement is not output correctness.
An output hash from an old Mesa image is not evidence for the current head.
