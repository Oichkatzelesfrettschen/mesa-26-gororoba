<!--
SPDX-License-Identifier: MIT
-->

# r3v -- experimental Vulkan ICD for RS480-family Radeon

## Overview

`r3v` is an experimental Vulkan installable client driver for the AMD
RS480-family integrated graphics processors:

- Radeon Xpress 200M, RS482, PCI `1002:5974`;
- Radeon Xpress 1100/1150 mobile, RS485 marketing name, PCI `1002:5975`.

The implementation is the native Radeon DRM ICD: Vulkan objects, command
records, shader admission, R300 command-stream construction, DRM submission,
and completion are owned by r3v over `src/amd/radeon/drm_vk/`; the Gallium
r300 driver shares only the API-neutral `src/amd/r300/common/` contracts.

The implementation boundaries between the native implementation and complete
Vulkan semantic/conformance coverage are documented in
`docs/hardware/r3v-implementation-boundaries.md`.

The driver is intentionally classified as experimental and nonconformant.
R3xx silicon has no documented native compute-dispatch packet, and the current
driver does not provide complete Vulkan command, memory, synchronization, WSI,
or conformance semantics. `r3v_private.h` owns the canonical
`R3V_CONFORMANCE_STATUS` classification.

## Hardware target

| Field | Value | Primary source |
|---|---|---|
| Vendor | ATI / AMD | PCI vendor ID `0x1002` |
| RS482 device | `0x5974` | `include/pci_ids/r300_pci_ids.h` |
| RS485-marketed device | `0x5975` | `include/pci_ids/r300_pci_ids.h` |
| Mesa family | `CHIP_RS480` | `r300_parse_chipset()` |
| Generation | R3xx | AMD R3xx Register Reference Guide |
| Mesa-classified vertex FPUs | 0 | `r300_parse_chipset()` |
| Vulkan compute queue | behind the exact `R3V_HYBRID_COMPUTE_EXPERIMENTAL=1` opt-in (CPU compute route) | `r3v_native_compute.c` |
| Kernel driver | `radeon` | Linux `drivers/gpu/drm/radeon/` |
| Renderer string | `ATI RS480` | r300g `r300_get_renderer()` |

Mesa's PCI table uses the `RS482_` prefix for both device IDs. The second device
is marketed as RS485. Source paths and PCI identities control technical claims;
marketing names remain descriptive only.

The current r300g path routes RS480-family vertex-stage execution through
Gallium Draw software TCL because Mesa classifies the family with
`num_vert_fpus == 0`. This is a current driver-path fact, not proof that the
silicon can never execute a hardware vertex program. The RS, TX, US, CB, and ZB
graphics blocks remain hardware-backed.

## Execution architecture

Vulkan commands are recorded into command-buffer-owned state and executed at
queue submission over the Radeon DRM transport.

```text
Vulkan command recording (r3v_native_recording.c, r3v_native_cmd.c)
-> admitted draw or dispatch (r3v_native_draw.c, r3v_native_compute.c)
-> vertex gather through the CPU executor (src/amd/r300/cpu/) into the
   command-buffer-owned GTT carrier, one stream per attribute slot the
   job reads over its bound per-vertex or per-instance binding, linearly
   from the first vertex or through the three indices an indexed draw
   reads from the bound index buffer at execution, once per instance
   from firstInstance into the cell family's 3 * instanceCount vertex
   list (VertexIndex and InstanceIndex carry the draw's base values, the
   Vulkan semantics; a vertex job that stores
   the location-0 varying writes eight-dword records, and the draw
   records the varying triangle cell whose RS routes that second FLOAT_4
   to the pass-through fragment program), or the R2VB producer route
   under its exact opt-in (immediate producer, or the fetched producer
   reading the bound vertex BO under a further exact opt-in; both keep
   one source relocation role, so they admit the slot-0 identity job
   alone); compute kernels execute on the CPU route for the one verb the
   ledger `common/r300_compute_verb.h` marks executing (identity map),
   or on its GPU route -- the compute identity carrier, the fetched
   producer over the input and output storage buffers -- under the verb's
   exact gate with the CPU bit copy as the read-back oracle; the
   remaining verbs are precommitted rows
-> fixed-cell PM4 lowering over the common contracts
   (src/amd/r300/common/)
-> prepared submission: relocation list, completion BO, arming evaluation,
   DRM_RADEON_CS through src/amd/radeon/drm_vk/
-> finite completion (write-domain BO plus bounded GEM_WAIT_IDLE)
```

`struct r3v_native_device` owns the DRM transport, the GEM-backed
`VkDeviceMemory` objects (one BO per allocation), the descriptor, pipeline,
image, and queue objects, and the prepared submission; no Gallium screen,
context, or resource takes part.

## Native ICD status

`libvulkan_r3v.so` (manifest `r3v_icd.<cpu>.json`, `driverName` `r3v`)
owns its Radeon DRM transport through `src/amd/radeon/drm_vk/` and links no
Gallium runtime library; a separation-audit test enforces that boundary. The native library owns
GEM-backed `VkDeviceMemory` (one BO per allocation, buffer-only), a queue
whose submission path builds the three-chunk `DRM_RADEON_CS` object, and
command-carrier objects. Fragment binaries are deep-copied into R3V-owned
`r300_fragment_binary` storage with a content hash and structural validator.

Real submission sits behind a conjunction, evaluated by
`r3v_native_arming_evaluate`: the exact-value gate
`R3V_NATIVE_SUBMIT_HAZARD_ACCEPTED=1`, an operator-declared bundle
digest matching the BLAKE3 of the IB about to travel, the authorized
RS482 PCI identity, the declared kernel release, the declared radeon
module srcversion, and an evidence directory that exists and carries no
attempt token. Reaching the ioctl writes that token by exclusive
creation, so the directory admits one attempt, and an armed submit
carries one command buffer. Any closed factor fails closed with
`VK_ERROR_DEVICE_LOST` and names itself. The closed gate retains the semantic
cell under `R3V_NATIVE_MANIFEST_DIR`; an open gate additionally retains the
exact submit object after completion relocation is folded into the CS. Each
retained artifact is bound by digest in its manifest, and a retention failure
refuses before the ioctl.
`r3v_native_arming_runner` reports every factor and stops at the
authorization boundary without creating a device; the attended run it
precedes follows
`docs/hardware/r3v-native-attended-cell-procedure.md`.
The public draw surface in `r3v_native_draw.c` lowers the qualified
render-pass begin/end, pipeline and vertex-buffer binds, and draw into
the fixed TCL-bypass cell; `r3v_native_recording.c` fail-closes every
other core 1.0 `vkCmd*` entrypoint by poisoning the command buffer, so
images, descriptors, transfers, and WSI remain outside the native
surface.
The drm-shim harness and offline kernel-parser replay carry the
pre-hardware evidence; the attended-cell runner has carried one armed
`DRM_RADEON_CS` submission on RS482 that the kernel accepted and retired
clean while the color target retained its sentinel fill. The cause of
that unwritten target is underdetermined -- the run retained no
predecessor register values, and
`docs/hardware/r3v-native-attended-cell-procedure.md` carries the
canonical classification -- while a later RS482 silicon matrix proved
that an unestablished `US_OUT_FMT_0`, `RB3D_COLOR_CHANNEL_MASK`, or
`SC_SCREENDOOR` each alone suppresses every color write, and the
original cell owned none of the three. The recorded
cell therefore now opens with the neutral first-draw state contract
(`src/amd/r300/common/r300_first_draw_state.c`): the contract's clauses
are emitted in pipeline order ahead of the cell, the poison-model checker
proves the stream establishes every clause itself, and the recorder,
manifest tool, and harness reference all build the one byte-identical
successor IB. The successor cell has rendered its predicted interior on
RS482 with the exterior and canary rows clean (retained bundle
`results/rs482_native_triangle_first_correct_pixel_witness_20260808T070427Z/`
in the steinmarder-r300 evidence tree;
`docs/hardware/r3v-implementation-boundaries.md` carries the
classification).

The first native hardware witness and the ordered migration path are specified
in `docs/hardware/r3v-implementation-boundaries.md`.

## Compute status

Behind the exact `R3V_HYBRID_COMPUTE_EXPERIMENTAL=1` opt-in the one queue
family advertises `VK_QUEUE_COMPUTE_BIT`; `r3v_CreateComputePipelines` admits
the identity-map kernel from SPIR-V words directly into the common compute
job (`src/amd/r300/common/r300_compute_spirv.c`), storage-buffer
descriptors bind on set 0, one dispatch records per command buffer, and
`r300_cpu_compute_job_execute` runs it at submission.  Every module outside
the admitted subset refuses at pipeline creation, so no admitted pipeline
reaches an unmatched no-op.  With the opt-in unset, compute pipeline creation
refuses.  The exposure remains experimental and nonconformant; widening it
means more admitted kernel shapes and a GPU raster-carrier route.

## R2VB source-format work

r300g's live R2VB producer admits `R32G32B32_FLOAT` and
`R32G32B32A32_FLOAT` source records, routed through the neutral
vertex-format contract with a byte-identity test pin. Final delivery remains
FP32x4.

The `FLOAT_2` source transaction is gated behind the exact-value
`R300_R2VB_FLOAT2_SOURCE` experiment and captured without submit; every
production admission path holds it out of the automatic route. Its exact
source contract and next integration step live in:

`docs/hardware/r300-r2vb-float2-source-contract.md`

Narrow source fetch support never implies narrow final-delivery support.

## Repository layout

### Retired lane

A Gallium-backed Vulkan lane preceded the ICD and is deleted; the
retirement section of `docs/hardware/r3v-implementation-boundaries.md`
maps each of its former capabilities to the mechanism that carries it and
names the retained manifests, the common-IR parity test, and the rebound
fixtures that are the differential reference.

```text
src/amd/radeon/drm_vk/     Gallium-free Radeon DRM transport (BO, PRIME,
                           relocation, CS build/submit, finite completion)

src/amd/r300/
  common/                  R300-neutral format, source-contract, fragment-
                           binary, and triangle-cell vocabulary
  vulkan/                  the r3v ICD: Vulkan objects over the Radeon DRM
                           transport (r3v_native_*.c)

src/gallium/drivers/r300/
  r300_public.h            r300 extraction interface
  r300_r2vb.c              live Gallium R2VB implementation
  compiler/                NIR/RC lowering and R300 shader compiler

docs/hardware/
  r3v-implementation-boundaries.md
  r300-r2vb-float2-source-contract.md
```

Important r3v sources include:

- `r3v_instance.c`: instance and ICD entry points;
- `r3v_physical_device.c`: DRM probe, properties, queues, memory, and limits;
- `r3v_native_device.c`, `r3v_native_queue.c`: device and the prepared
  submission over the DRM transport;
- `r3v_native_memory.c`, `r3v_native_object.c`, `r3v_native_image.c`: GEM-backed
  memory and objects;
- `r3v_native_recording.c`, `r3v_native_cmd.c`, `r3v_native_draw.c`: command
  recording and the admitted draw surface;
- `r3v_native_pipeline.c`, `r3v_native_compute.c`: direct SPIR-V admission
  into the common job IR;
- `r3v_cpu_sync.c`: CPU-timeline fence implementation;
- `meson.build`: ICD and test build graph.

## Build

The ICD builds with no Gallium dependency:

```sh
meson setup builddir-r3v \
    -Dvulkan-drivers=ati_r300 \
    -Dgallium-drivers= \
    -Dopengl=false -Dgles1=false -Dgles2=false \
    -Dglx=disabled -Degl=disabled \
    -Dplatforms=x11 -Dllvm=disabled -Dlibunwind=disabled \
    -Dbuildtype=debug

ninja -C builddir-r3v src/amd/r300/vulkan/libvulkan_r3v.so
```

Adding `drm-shim` to `-Dtools` builds the render-node model the cell
harnesses preload; adding `r300` to `-Dgallium-drivers` builds the GL driver
beside it and registers the r300g-owned planner oracles.

To inspect the in-tree ICD:

```sh
export VK_DRIVER_FILES="$PWD/builddir-r3v/src/amd/r300/vulkan/r3v_devenv_icd.x86_64.json"
export R3V_DEBUG=startup
vulkaninfo --summary
```

## Validation boundaries

Build, loader, runtime, silicon, conformance, and deployment are separate
results.

At minimum, a loader/identity check records:

| Surface | Required observation |
|---|---|
| Instance | `vkCreateInstance` succeeds |
| Physical device | one supported RS480-family PCI identity is enumerated |
| Queue families | graphics and transfer only; no compute |
| Mapped ICD | the process maps the intended `libvulkan_r3v.so` |
| Kernel window | no unexplained CS validation, reset, hang, or lockup event |
| Conformance status | experimental/nonconformant classification retained |

Native direct execution requires R3V-owned memory and PM4 plus offline
parser acceptance before an attended silicon submission; every drawing,
R2VB, and compute result carries its own positive and negative controls.

The Radeon drm-shim is a host model. Kernel parser acceptance is not execution.
Fence retirement is not output correctness. An old image hash is not evidence
for the current source head.

## Conformance ladder

The Vulkan 1.0 conformance surface is finite and machine-checked.
`tests/r3v_vulkan10_requirement_inventory.py` generates the registry
inventory and closes the advertised extension dependencies at core 1.0;
`tests/r3v_advertised_surface_deqp_binding.tsv` binds every advertised
extension, granted feature bit, receipt-bound limit, and format-feature
row to the registered test that exercises its executing route and to the
exact dEQP-VK mustpass group that judges it, so a bit without a binding
row fails the build.  `apiVersion` stays 1.0 and advertises exactly what
those bindings prove; 1.1 and later are new campaigns with their own
evidence, never a string edit.

`tests/r3v_conformance_runner.py` runs a caselist against the ICD and
seals an identity receipt: source SHA and cleanliness, Meson options,
ICD manifest and DSO digest, dEQP binary digest and CTS release, kernel
release and radeon module srcversion, packages, GPU PCI identity, boot
id, the driver-visible environment, the case list, per-case status,
dmesg delta, timeouts, and the finite verdict.  The evidence class is
derived from the run (a preloaded drm-shim is host-model; only an
unpreloaded run on the RS4xx render node is silicon), NotSupported never
counts as a pass, and every non-pass classifies against
`tests/r3v_conformance_nonpass_ledger.tsv`, most-specific row first; a
status the ledger does not name blocks the run as unclassified.  The
runner calibrates on fake-dEQP fixtures for pass, mixed, truncated,
timeout, crash, device-loss, framework-abort, wrong-ICD, and
dmesg-hazard runs.

`tests/r3v_conformance_slices.tsv` orders the mustpass groups by hazard:
info and api slices run on the host model, and command, transfer, draw,
synchronization, robustness, and WSI slices carry a submission or display
hazard and run on silicon only.  dEQP's default context requires a
GRAPHICS|COMPUTE queue and aborts on the graphics-only family, so every
run sets the exact compute-queue opt-in and the receipt records it.

## Conformance contract

The ICD must not be advertised as conformant Vulkan until:

- every exposed command has implemented semantics;
- memory mapping, visibility, aliasing, and external-memory behavior are
  complete;
- queue ordering and synchronization are resource-correct;
- image, render-pass, dynamic-rendering, query, transfer, and WSI behavior are
  complete for advertised surfaces;
- unsupported work returns a documented error rather than succeeding as a
  no-op;
- feature and extension tables are generated from implemented behavior;
- the relevant CTS and dEQP suites pass on current source and target identities.

The current empty or bounded feature exposure does not convert the driver into a
conformant Vulkan implementation.

## Project and evidence boundary

Mesa source, build rules, tests, and driver documentation live in
`mesa-26-gororoba`.

Target probes, retained silicon bundles, findings, falsifiers, and cross-repo
manifests live in the evidence repository. Linux Radeon kernel changes live in
the kernel source repository; package and deployment policy live in their
packaging repository.

A Mesa behavior change cites external evidence but lands in Mesa.

## Primary sources

- AMD R3xx Register Reference Guide;
- Mesa r300g source under `src/gallium/drivers/r300/`;
- r3v source under `src/amd/r300/vulkan/`;
- Linux Radeon DRM source under `drivers/gpu/drm/radeon/`;
- Vulkan specification, especially devices/queues, memory, synchronization,
  command execution, WSI, limits, and conformance requirements;
- Khronos Vulkan loader ICD interface documentation and `vulkan/vk_icd.h`.
