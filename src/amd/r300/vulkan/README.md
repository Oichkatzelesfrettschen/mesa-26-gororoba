<!--
SPDX-License-Identifier: MIT
-->

# r3v -- experimental Vulkan ICD for RS482/RS485

## Overview

`r3v` is a Vulkan installable client driver (ICD) targeting the
AMD RS480-family integrated graphics processor: Radeon Xpress 200M
(RS482, PCI `1002:5974`) and Radeon Xpress 1100/1150 mobile
(RS485, PCI `1002:5975`).  It exposes the chip as a Vulkan
`VkPhysicalDevice` so the Vulkan loader, layers, and inspection
tooling can interrogate the hardware identity, queue family
structure, memory model, and limit envelope without lighting up
any submission path.

The driver consciously violates the Vulkan conformance contract
because R3xx silicon has no native compute dispatch surface.  The
classification reported to external tooling is therefore
`experimental_nonconformant_graphics_without_compute` -- see
`r3v_private.h` for the canonical macro `R3V_CONFORMANCE_STATUS`.

The product boundary between this Gallium-backed driver (Program L), a
future native R3V implementation (Program N), and complete Vulkan
semantics (Program P) lives in
`docs/hardware/r3v-product-denominators.md`.

## Hardware target

| Field | Value | Primary source |
|---|---|---|
| Vendor | ATI / AMD | PCI vendor ID `0x1002` (PCI-SIG) |
| RS482 device | `0x5974` | `include/pci_ids/r300_pci_ids.h`, `CHIPSET(0x5974, RS482_5974, RS480)` |
| RS485 device | `0x5975` | `include/pci_ids/r300_pci_ids.h`, `CHIPSET(0x5975, RS482_5975, RS480)` |
| Family | RS480 | Mesa `r300_chipset.c`, `r300_parse_chipset()` |
| Generation | R3xx | AMD R3xx Register Reference Guide (RRG) |
| Vertex FPUs (Mesa-classified) | 0 | `r300_parse_chipset()` `num_vert_fpus` |
| Compute queue | not surfaced | R3xx-RRG -- no documented compute dispatch packet |
| Kernel driver | `radeon` | `drivers/gpu/drm/radeon/radeon_drv.c` |
| Renderer string | `ATI RS480` | Mesa r300g, `r300_get_renderer()` |

Both PCI IDs are tagged with the `RS482_` prefix in Mesa's canonical
PCI table even though `0x5975` is marketed as RS485 (Radeon Xpress
1100/1150 mobile).  The row name reflects the closer codename, not the
product marketing name; cite the source path, not the marketing label.

Current Mesa r300g routes RS482/RS485 vertex-stage execution through
Gallium Draw SW TCL because the RS480 family is classified with
`num_vert_fpus == 0`.  This is a source-supported driver-path fact,
not a final silicon impossibility claim.  Hardware PVS execution on
this exact target remains `silicon_unproven_not_disproven`.  The R300
RS/TX/US/CB/ZB blocks remain hardware-backed.

## Repository layout

```
src/amd/r300/vulkan/
  r3v_private.h            PCI IDs, API version, conformance macro
  r3v_instance.h           struct r3v_instance
  r3v_instance.c           vkCreateInstance / vk_icdGetInstanceProcAddr
  r3v_physical_device.h    struct r3v_physical_device
  r3v_physical_device.c    DRM probe + properties + queues + memory
  r3v_device.h             struct r3v_device / r3v_queue
  r3v_device.c             CreateDevice / DestroyDevice / gate check
  r3v_queue.c              submit replay -- Backend A (pipe_context)
  r3v_memory.h/.c          VkDeviceMemory resource-backed model
  r3v_buffer.h/.c          VkBuffer backed by PIPE_BUFFER pipe_resource
  r3v_image.h/.c           VkImage backed by PIPE_TEXTURE_2D pipe_resource
  r3v_resource_state.h     per-image layout-tracking struct
  r3v_cmd_buffer.h/.c      command recording into r3v_cmd_entry stream
  r3v_pipeline.h/.c        CreateGraphicsPipelines -- SPIR-V -> NIR -> r300g CSOs
  r3v_render_pass.h/.c     VkRenderPass local object
  r3v_framebuffer.h/.c     VkFramebuffer local object
  r3v_shader_module.h/.c   VkShaderModule SPIR-V storage
  r3v_cpu_sync.h/.c        vkCreateFence / vkWaitForFences (CPU timeline)
  meson.build                 libvulkan_r3v + ICD JSON
  README.md                   this file
```

The pattern mirrors `src/amd/terascale/vulkan/` (Terakan, the
production sibling Vulkan driver for the Evergreen / VLIW5 PALM
chip) and uses the Mesa Vulkan runtime base structures
(`vk_instance`, `vk_physical_device`) directly through
`src/vulkan/runtime/`.

## Submit architecture: semantic IR and backend dispatch

The Vulkan command stream is lowered to a device-independent
`r3v_cmd_entry` array (see `r3v_cmd_buffer.h`) at record time.
At submit time, `r3v_queue_driver_submit` replays this array through
a backend selected at `CreateDevice` time.

```
vkCmd* recording
  -> r3v_cmd_entry stream  (semantic IR, device-independent)
     -> Backend A (default): r3v_replay_gpu()
        -> Gallium pipe_context calls
        -> r300g atom-dirty machinery
        -> radeon_winsys DRM_RADEON_CS submit
     -> Backend B (planned): r3v_replay_backend_b()
        -> radeon_winsys cs_emit directly
        -> bypasses Gallium pipe_context
        -> NOT YET IMPLEMENTED (see below)
```

**Backend A** (current, working): `r3v_replay_gpu()` in `r3v_queue.c`
lowers the cmd_entry stream through Gallium's `pipe_context` call layer.
Gallium's atom-dirty machinery emits all required register state
(viewport, blend, DSA, rasterizer, vertex elements, PVS flush, guardband,
VAP invariant state) automatically before each draw.  Pipeline barriers
issue a `pipe->flush()` mid-stream; r300g re-emits all dirty atoms before
the next `draw_vbo`, so state remains coherent across the flush boundary.

**Backend B** (planned, not implemented): direct `radeon_winsys cs_emit`
bypassing `pipe_context`.  Two prerequisites must be resolved first:

1. **Shader code access (FATAL)**: `vs_cso` and `fs_cso` in
   `r3v_pipeline` are `void *` (opaque Gallium CSO handles).  Backend B
   cannot cast them to `r300_vertex_shader` / `r300_fragment_shader`
   without a dedicated extraction API (e.g. `r300_vs_get_hw_code()`).
   That function does not yet exist in r300g's public headers.

2. **IR completeness (SERIOUS)**: the `r3v_cmd_entry` stream carries
   Vulkan-semantic operations (bind pipeline as CSO handle, set viewport
   as `VkViewport`).  It does not carry the 15+ r300g register atoms
   (guardband, VAP invariant state, blend equation bytes, DSA bits, etc.)
   that Gallium's atom-dirty mechanism provides implicitly.  Backend B
   needs either baked-PM4 extensions to the IR or a parallel r300g-state-
   to-PM4 translation path.

The `use_cs_backend` device flag (set by
`R3V_CS_DIRECT_BACKEND_HAZARD_ACCEPTED=1` at `CreateDevice` time) records
hazard acceptance for a future cs-direct path.  The cs-direct backend body
is not implemented: the gate only records hazard acceptance and the submit
path still falls through to `r3v_replay_gpu`.

## Resource-state ledger

`r3v_resource_state` (in `r3v_resource_state.h`) tracks the current
`VkImageLayout` per image.  On RS482/RS485 (UMA, no aux compression
surfaces) layout transitions have no aux decompression step; they reduce
to a `pipe->flush()` at the barrier boundary plus a bookkeeping update to
`image->resource_state.layout`.  The field is updated at replay time in
the `R3V_CMD_PIPELINE_BARRIER` case of `r3v_replay_gpu()`.

## Conformance contract

No documented or silicon-proven native compute dispatch surface exists
for this RS482/RS485 R3V target.  The r3v skeleton MUST NOT be
advertised as conformant Vulkan for any version.  The contract is
enforced by:

1. The `R3V_CONFORMANCE_STATUS` macro in `r3v_private.h`.
2. The empty `vk_features` table in `r3v_physical_device_init_features`.
3. The queue family declaration in
   `r3v_GetPhysicalDeviceQueueFamilyProperties2`: one queue family,
   `VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_TRANSFER_BIT`, no compute.
4. Probes that observe physical-device properties must record the
   conformance classification before any benchmark or workload claim.

The classification holds until either:

- AMD or a Khronos-allocated `VkDriverId` is assigned for r3v and
  the driver passes the relevant CTS suites; or
- the project explicitly decides to keep the skeleton experimental
  and documents that decision in `docs/`.

## Build

Default `-Dr3v-gallium-backend=true` requires Gallium `r300` in the same
build.  Loader-only skeleton builds set `-Dr3v-gallium-backend=false`
instead of omitting r300 while leaving the default backend on.

```sh
# Normal r3v ICD with Gallium backend (default r3v-gallium-backend=true)
meson setup builddir-r3v \
    -Dvulkan-drivers=ati_r300 \
    -Dgallium-drivers=r300 \
    -Dopengl=false -Dgles1=false -Dgles2=false \
    -Dglx=disabled -Degl=disabled \
    -Dplatforms=x11 -Dllvm=disabled -Dlibunwind=disabled \
    -Dbuildtype=debug
ninja -C builddir-r3v src/amd/r300/vulkan/libvulkan_r3v.so
```

To exercise the ICD in-tree without installing:

```sh
export VK_ICD_FILENAMES=$PWD/builddir-r3v/src/amd/r300/vulkan/r3v_devenv_icd.x86_64.json
export R3V_DEBUG=startup
vulkaninfo --summary
```

## Probe ladder

The Steinmarder evidence repository drives the verification ladder
that gates each subsequent milestone.  Each step yields a retained
result bundle under `src/re/r300/results/`.

| Step | Mechanism | Success criterion |
|---|---|---|
| Loader visibility | `vkCreateInstance` + `vkEnumeratePhysicalDevices` | `r3v_loader_r300_device_found`, `physical_device_count >= 1` |
| Identity | `vkGetPhysicalDeviceProperties` | `vendorID=0x1002`, `deviceID` in `{0x5974, 0x5975}`, deviceName matches RS480 chip family |
| Queue families | `vkGetPhysicalDeviceQueueFamilyProperties2` | exactly one family, GRAPHICS+TRANSFER, no COMPUTE |
| Memory model | `vkGetPhysicalDeviceMemoryProperties2` | two heaps, two types, `memory_properties_placeholder=true` until DRM_RADEON_GEM_INFO query lands |
| Limits envelope | `vkGetPhysicalDeviceProperties` `limits` | R3xx-grounded values match `r3v_physical_device_init_limits` |

Higher-tier milestones (device creation, command buffer recording,
shader compilation, framebuffer attach, draw submission) remain
unimplemented and out of scope for the loader-visible skeleton.

## Falsification criteria

The skeleton would be falsified by any of:

- `vkCreateInstance` returning `VK_ERROR_INITIALIZATION_FAILED` on a
  host with the RS482/RS485 chip and a working radeon DRM driver.
- `vkEnumeratePhysicalDevices` returning zero devices when the
  loader has the r3v ICD JSON registered and the host carries an
  RS482/RS485 chip.
- `vkGetPhysicalDeviceProperties` reporting a vendor or device ID
  outside the supported set for an enumerated r3v physical device.
- A `VK_QUEUE_COMPUTE_BIT` appearing in any queue family the driver
  reports.
- A hazard match in the radeon kernel log (gpu lockup, ring stall,
  cs reject, drm reset) produced by an r3v probe that does not
  submit any IB.

## Primary-source citations

- AMD R3xx Register Reference Guide (R3xx-RRG):
  Color Buffer, Geometry Assembly, Graphics Backend, Rasterizer,
  Clipping, Setup, Texture Engine, Fragment Shader, Vertex Assembly
  and Processor, Z-Buffer chapters.
- AMD R5xx Programming Guide -- comparative reference; explicitly
  notes that R5xx builds on R3xx and much applies to R3xx/R4xx
  with caveats.
- Mesa source:
  - `src/gallium/drivers/r300/r300_chipset.c` -- PCI table
    `r300_chipset_descs[]` and family parsing.
  - `src/gallium/drivers/r300/r300_screen.c` -- caps surface.
  - `src/gallium/drivers/r300/r300_state.c` -- pipeline state.
- Linux kernel `drivers/gpu/drm/radeon/` -- radeon DRM driver,
  `radeon_drv.c`, `radeon_gem.c`, `radeon_cs.c`.
- Vulkan 1.4 specification chapters:
  - ch. 5 "Devices and Queues" -- `VkPhysicalDevice`,
    `vkEnumeratePhysicalDevices`, queue family rules.
  - ch. 36 "Layers and Extensions" -- enumeration semantics.
  - ch. 49.1 "Limit Requirements" -- minimum limit table.
- Khronos Vulkan loader specification, `LoaderInterfaceArchitecture.md`,
  "ICD interface version" section -- `vk_icdGetInstanceProcAddr`,
  `vk_icdNegotiateLoaderICDInterfaceVersion`, ICD JSON manifest
  format, `ICD_LOADER_MAGIC 0x01CDC0DE` semantics.
- Vulkan ICD ABI header `vulkan/vk_icd.h`.

## Project boundary

Per `AGENTS.md` "Canonical Mesa workspace boundary":

- Driver code lives here in `mesa-26-gororoba`.
- Evidence bundles, runners, findings, and probe results live in the
  sibling `steinmarder` repository.

Cross-repo references in this README cite paths under
`src/re/r300/...` in the steinmarder tree.  See
`steinmarder/src/re/r300/docs/R3V_ICD_GL_BACKED_SKELETON_DESIGN.md`
for the longer-form architecture scope.
