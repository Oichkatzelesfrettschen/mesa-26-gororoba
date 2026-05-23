<!--
Copyright (c) 2026 Terascale Functionalists
SPDX-License-Identifier: MIT
-->

# r300vk -- experimental Vulkan ICD for RS482/RS485

## Overview

`r300vk` is a Vulkan installable client driver (ICD) targeting the
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
`r300vk_private.h` for the canonical macro `R300VK_CONFORMANCE_STATUS`.

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
PCI table even though `0x5975` markets as RS485 (Radeon Xpress
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
src/amd/r300/vulkan/r300vk/
  r300vk_private.h            PCI IDs, API version, conformance macro
  r300vk_instance.h           struct r300vk_instance
  r300vk_instance.c           vkCreateInstance / vk_icdGetInstanceProcAddr
  r300vk_physical_device.h    struct r300vk_physical_device
  r300vk_physical_device.c    DRM probe + properties + queues + memory
  meson.build                 libvulkan_r300 + ICD JSON
  README.md                   this file
```

The pattern mirrors `src/amd/terascale/vulkan/` (Terakan, the
production sibling Vulkan driver for the Evergreen / VLIW5 PALM
chip) and uses the Mesa Vulkan runtime base structures
(`vk_instance`, `vk_physical_device`) directly through
`src/vulkan/runtime/`.

## Conformance contract

No documented or Vostro-proven native compute dispatch surface exists
for this RS482/RS485 R300VK target.  The r300vk skeleton MUST NOT be
advertised as conformant Vulkan for any version.  The contract is
enforced by:

1. The `R300VK_CONFORMANCE_STATUS` macro in `r300vk_private.h`.
2. The empty `vk_features` table in `r300vk_physical_device_init_features`.
3. The queue family declaration in
   `r300vk_GetPhysicalDeviceQueueFamilyProperties2`: one queue family,
   `VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_TRANSFER_BIT`, no compute.
4. Probes that observe physical-device properties must record the
   conformance classification before any benchmark or workload claim.

The classification holds until either:

- AMD or a Khronos-allocated `VkDriverId` is assigned for r300vk and
  the driver passes the relevant CTS suites; or
- the project explicitly decides to keep the skeleton experimental
  and documents that decision in `docs/`.

## Build

```sh
meson setup builddir-r300vk \
    -Dvulkan-drivers=amd_r300 \
    -Dgallium-drivers= \
    -Dopengl=false -Dgles1=false -Dgles2=false \
    -Dglx=disabled -Degl=disabled \
    -Dplatforms=x11 -Dllvm=disabled -Dlibunwind=disabled \
    -Dbuildtype=debug
ninja -C builddir-r300vk src/amd/r300/vulkan/r300vk/libvulkan_r300.so
```

To exercise the ICD in-tree without installing:

```sh
export VK_ICD_FILENAMES=$PWD/builddir-r300vk/src/amd/r300/vulkan/r300vk/r300_devenv_icd.x86_64.json
export R300VK_DEBUG=startup
vulkaninfo --summary
```

## Probe ladder

The Steinmarder evidence repository drives the verification ladder
that gates each subsequent milestone.  Each step yields a retained
result bundle under `src/re/r300/results/`.

| Step | Mechanism | Success criterion |
|---|---|---|
| Loader visibility | `vkCreateInstance` + `vkEnumeratePhysicalDevices` | `r300vk_loader_r300_device_found`, `physical_device_count >= 1` |
| Identity | `vkGetPhysicalDeviceProperties` | `vendorID=0x1002`, `deviceID` in `{0x5974, 0x5975}`, deviceName matches RS480 chip family |
| Queue families | `vkGetPhysicalDeviceQueueFamilyProperties2` | exactly one family, GRAPHICS+TRANSFER, no COMPUTE |
| Memory model | `vkGetPhysicalDeviceMemoryProperties2` | two heaps, two types, `memory_properties_placeholder=true` until DRM_RADEON_GEM_INFO query lands |
| Limits envelope | `vkGetPhysicalDeviceProperties` `limits` | R3xx-grounded values match `r300vk_physical_device_init_limits` |

Higher-tier milestones (device creation, command buffer recording,
shader compilation, framebuffer attach, draw submission) remain
unimplemented and out of scope for the loader-visible skeleton.

## Falsification criteria

The skeleton would be falsified by any of:

- `vkCreateInstance` returning `VK_ERROR_INITIALIZATION_FAILED` on a
  host with the RS482/RS485 chip and a working radeon DRM driver.
- `vkEnumeratePhysicalDevices` returning zero devices when the
  loader has the r300vk ICD JSON registered and the host carries an
  RS482/RS485 chip.
- `vkGetPhysicalDeviceProperties` reporting a vendor or device ID
  outside the supported set for an enumerated r300vk physical device.
- A `VK_QUEUE_COMPUTE_BIT` appearing in any queue family the driver
  reports.
- A hazard match in the radeon kernel log (gpu lockup, ring stall,
  cs reject, drm reset) produced by an r300vk probe that does not
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
`steinmarder/src/re/r300/docs/R300VK_ICD_GL_BACKED_SKELETON_DESIGN.md`
for the longer-form architecture scope.
