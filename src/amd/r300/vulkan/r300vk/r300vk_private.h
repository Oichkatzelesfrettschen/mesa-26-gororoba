/*
 * Copyright (c) 2026 Terascale Functionalists
 * SPDX-License-Identifier: MIT
 */

#ifndef R300VK_PRIVATE_H
#define R300VK_PRIVATE_H

#include <vulkan/vulkan_core.h>

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define R300VK_VENDOR_ID_ATI 0x1002

/* RS482 (Radeon Xpress 200M, IGP) and RS485 (Radeon Xpress 1100/1150,
 * mobile IGP).  The PCI table in src/gallium/drivers/r300/r300_chipset.c
 * maps 0x5974 to RS482_5974 and 0x5975 to RS482_5975 in the RS480 family.
 * Both report GL_RENDERER="ATI RS480"; both route the vertex stage
 * through Gallium Draw SW TCL because num_vert_fpus == 0 for the RS480
 * family in r300_parse_chipset(). */
#define R300VK_PCI_DEVICE_ID_RS482 0x5974
#define R300VK_PCI_DEVICE_ID_RS485 0x5975

#define R300VK_API_VERSION VK_MAKE_API_VERSION(0, 1, 0, VK_HEADER_VERSION)

/* Conformance classification reported to probes and external tooling.
 *
 * No documented or silicon-proven native compute dispatch surface
 * exists for this RS482/RS485 R300VK target: no COMPUTE queue, no
 * workgroup shared memory, no shader atomics, no SPIR-V compute
 * capabilities, no SSBO storage semantics in the current Mesa r300
 * implementation oracle or the AMD R3xx-RRG documentation.  The
 * VkQueueFlagBits reference (Vulkan spec ch. 5 "Devices and Queues",
 * VkQueueFamilyProperties::queueFlags) requires that a conformant
 * implementation report VK_QUEUE_COMPUTE_BIT for any queue family that
 * supports VK_QUEUE_GRAPHICS_BIT.  The r300vk skeleton intentionally
 * violates that rule by omitting compute, so it must NOT claim Vulkan
 * conformance for any version.  Probes and CTS runners must observe
 * this classification before treating the implementation as a graphics
 * device. */
#define R300VK_CONFORMANCE_STATUS "experimental_nonconformant_graphics_without_compute"

static inline bool
r300vk_pci_device_id_is_supported(uint32_t pci_device_id)
{
   switch (pci_device_id) {
   case R300VK_PCI_DEVICE_ID_RS482:
   case R300VK_PCI_DEVICE_ID_RS485:
      return true;
   default:
      return false;
   }
}

#ifdef __cplusplus
}
#endif

#endif /* R300VK_PRIVATE_H */
