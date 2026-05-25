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

#define R300VK_HYBRID_COMPUTE_ENV "R300VK_HYBRID_COMPUTE_EXPERIMENTAL"
#define R300VK_HYBRID_COMPUTE_ENV_VALUE "1"

#define R300VK_R3XX_MAX_TEXTURE_DIMENSION 2048u
#define R300VK_R3XX_MAX_TEXTURE_3D_DIMENSION 256u
#define R300VK_R3XX_MAX_RENDER_DIMENSION 2560u
#define R300VK_R3XX_MAX_ARRAY_LAYERS 1u
#define R300VK_R3XX_MAX_MIP_LEVELS 1u
#define R300VK_R3XX_SUPPORTED_SAMPLE_COUNTS VK_SAMPLE_COUNT_1_BIT
#define R300VK_MAX_VERTEX_BINDINGS 16

#define R300VK_VK10_MIN_IMAGE_DIMENSION_1D 4096u
#define R300VK_VK10_MIN_IMAGE_DIMENSION_2D 4096u
#define R300VK_VK10_MIN_IMAGE_DIMENSION_CUBE 4096u
#define R300VK_VK10_MIN_STORAGE_BUFFER_RANGE (128u * 1024u * 1024u)
#define R300VK_VK10_MIN_COMPUTE_SHARED_MEMORY_SIZE (16u * 1024u)
#define R300VK_VK10_MIN_COMPUTE_WORKGROUP_INVOCATIONS 128u
#define R300VK_VK10_MIN_COMPUTE_WORKGROUP_SIZE_X 128u
#define R300VK_VK10_MIN_COMPUTE_WORKGROUP_SIZE_Y 128u
#define R300VK_VK10_MIN_COMPUTE_WORKGROUP_SIZE_Z 64u
#define R300VK_VK10_MIN_VIEWPORT_DIMENSION 4096u
#define R300VK_VK10_MIN_FRAMEBUFFER_DIMENSION 4096u
#define R300VK_VK10_REQUIRED_SAMPLE_COUNTS \
   (VK_SAMPLE_COUNT_1_BIT | VK_SAMPLE_COUNT_4_BIT)

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
#define R300VK_HYBRID_COMPUTE_STATUS "experimental_nonconformant_hybrid_compute_queue"

/* Backend identity labels for the Gallium-mediated submit path.
 * RS482/RS485 has no hardware vertex processor (num_vert_fpus == 0 for
 * the RS480 family per r300_parse_chipset()); Gallium Draw handles TCL in
 * software.  The fragment stage runs through the Radeon Compiler RC path
 * via r300_nir_to_rc_direct inside r300g. */
#define R300VK_BACKEND_LABEL       "r300g_gallium_mediated"
#define R300VK_VERTEX_EXEC_LOCUS   "gallium_draw_sw_tcl"
#define R300VK_FRAGMENT_EXEC_LOCUS "r300_rc_hardware_program"
#define R300VK_MEMORY_MODEL_LABEL  "experimental_resource_backed"

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
