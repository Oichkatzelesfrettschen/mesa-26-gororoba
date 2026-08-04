<!--
SPDX-License-Identifier: MIT
-->

# r3v -- experimental Vulkan ICD for RS480-family Radeon

## Overview

`r3v` is an experimental Vulkan installable client driver for the AMD
RS480-family integrated graphics processors:

- Radeon Xpress 200M, RS482, PCI `1002:5974`;
- Radeon Xpress 1100/1150 mobile, RS485 marketing name, PCI `1002:5975`.

The current functional implementation is Gallium-backed. Vulkan objects and
command records are owned by r3v, while device execution, resources, graphics
state, and completion flow through Gallium r300 and the Radeon winsys.

The implementation boundaries between the current Gallium-backed ICD, a future
native Radeon DRM implementation, and complete Vulkan semantic/conformance
coverage are documented in
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
| Vulkan compute queue | not exposed | no documented R3xx compute-dispatch surface |
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

## Current execution architecture

Vulkan commands are recorded as `r3v_cmd_entry` records and replayed at submit
time through the device's Gallium `pipe_context`.

```text
Vulkan command recording
-> r3v_cmd_entry stream
-> Gallium pipe_context replay
-> r300g state and shader objects
-> Radeon winsys command submission
-> Gallium fence completion
```

`struct r3v_device` owns:

- a `radeon_winsys`;
- a Gallium `pipe_screen`;
- a Gallium `pipe_context`;
- the r3v queue and Vulkan object registries.

`VkDeviceMemory`, `VkBuffer`, and `VkImage` currently own or borrow
`pipe_resource` objects. Queue submission flushes Gallium, waits for the Gallium
fence, and synchronizes host-shadow resources.

Pipeline barriers issue a Gallium flush and update r3v's image-layout ledger.
Gallium's dirty-atom machinery re-emits required R300 state before later draws.

## Native direct-submission status

`R3V_CS_DIRECT_BACKEND_HAZARD_ACCEPTED=1` records explicit consent for direct
submission experiments. It does not select an implemented native queue path.
Submission still falls through to the Gallium replay implementation.

The r300 public extraction API now exposes precompiled vertex and fragment
descriptors. The fragment descriptor includes the prepared US/FG PM4 block, but
its pointers still alias Gallium CSO storage. A native implementation must
deep-copy every consumed word and subordinate table into R3V-owned storage
before Gallium objects can be destroyed.

A native queue additionally requires:

- R3V-owned BO and memory objects;
- complete R300 state packs and relocation ownership;
- bounded PM4 construction;
- explicit cache and role-transition barriers;
- finite completion;
- resource-scoped synchronization;
- device-loss propagation.

The first native hardware witness and the ordered migration path are specified
in `docs/hardware/r3v-implementation-boundaries.md`.

## Compute status

The graphics-as-compute classifier recognizes a bounded raster-compute corpus.
Those paths remain graphics pipeline executions, not a Vulkan compute queue.

An unsupported or unrecognized compute shape can still reach a successful
pipeline or no-op dispatch in the current experimental implementation. That is
a semantic defect. Unsupported work must ultimately fail at a documented API
boundary; successful no-op commands never count as Vulkan support.

No queue family advertises `VK_QUEUE_COMPUTE_BIT`.

## R2VB source-format work

The live Gallium r300 R2VB producer admits `R32G32B32_FLOAT` and
`R32G32B32A32_FLOAT` source records. Final delivery remains FP32x4.

The neutral `FLOAT_2` source transaction is defined, host-tested, and still
outside the live route. Its exact source contract and next integration step live
in:

`docs/hardware/r300-r2vb-float2-source-contract.md`

Narrow source fetch support never implies narrow final-delivery support.

## Repository layout

```text
src/amd/r300/
  common/                  R300-neutral format and source-contract vocabulary
  vulkan/                  r3v Vulkan objects and Gallium-backed execution

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
- `r3v_device.c`: device creation and backend selection;
- `r3v_queue.c`: Gallium replay and queue completion;
- `r3v_memory.c`, `r3v_buffer.c`, `r3v_image.c`: current resource-backed model;
- `r3v_cmd_buffer.c`: command recording;
- `r3v_pipeline.c`: SPIR-V/NIR pipeline construction and r300 CSO integration;
- `r3v_render_pass.c`, `r3v_framebuffer.c`: render-target objects;
- `r3v_cpu_sync.c`: CPU-timeline fence implementation;
- `meson.build`: ICD and test build graph.

## Build

The default Gallium-backed build requires Gallium r300:

```sh
meson setup builddir-r3v \
    -Dvulkan-drivers=ati_r300 \
    -Dgallium-drivers=r300 \
    -Dr3v-gallium-backend=true \
    -Dopengl=false -Dgles1=false -Dgles2=false \
    -Dglx=disabled -Degl=disabled \
    -Dplatforms=x11 -Dllvm=disabled -Dlibunwind=disabled \
    -Dbuildtype=debug

ninja -C builddir-r3v src/amd/r300/vulkan/libvulkan_r3v.so
```

A loader-only configuration sets `-Dr3v-gallium-backend=false`. It is an
enumeration/build boundary, not the native functional implementation.

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

Gallium-backed drawing, R2VB, video, and graphics-as-compute results require
their own positive and negative controls. Native direct execution requires
R3V-owned memory and PM4 plus offline parser acceptance before an attended
silicon submission.

The Radeon drm-shim is a host model. Kernel parser acceptance is not execution.
Fence retirement is not output correctness. An old image hash is not evidence
for the current source head.

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
