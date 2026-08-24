/*
 * SPDX-License-Identifier: MIT
 */

#ifndef R3V_PRIVATE_H
#define R3V_PRIVATE_H

#include <vulkan/vulkan_core.h>
#include "amd/r300/common/r300_chip_identity.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>


#ifdef __cplusplus
extern "C" {
#endif

#define R3V_VENDOR_ID_ATI R300_PCI_VENDOR_ATI

/* RS482 (Radeon Xpress 200M, IGP) and RS485 (Radeon Xpress 1100/1150,
 * mobile IGP).  The common chip identity table maps 0x5974 to RS482_5974
 * and 0x5975 to RS482_5975 in the RS480 family; both report
 * GL_RENDERER="ATI RS480", and the RS480-class vertex transform engine is
 * absent, so every vertex route is TCL bypass over produced records. */
#define R3V_PCI_DEVICE_ID_RS482 R300_PCI_DEVICE_RS482
#define R3V_PCI_DEVICE_ID_RS485 R300_PCI_DEVICE_RS485

#define R3V_API_VERSION VK_MAKE_API_VERSION(0, 1, 0, VK_HEADER_VERSION)

#define R3V_IDENTITY_MAP_FP32X4_ENV "R3V_IDENTITY_MAP_FP32X4_EXPERIMENTAL"
#define R3V_IDENTITY_MAP_FP32X4_ENV_VALUE "1"

#define R3V_R3XX_MAX_TEXTURE_DIMENSION 2048u
#define R3V_R3XX_MAX_TEXTURE_3D_DIMENSION 256u
#define R3V_R3XX_MAX_RENDER_DIMENSION 2560u
#define R3V_R3XX_MAX_ARRAY_LAYERS 1u
#define R3V_R3XX_MAX_MIP_LEVELS 1u
#define R3V_R3XX_SUPPORTED_SAMPLE_COUNTS VK_SAMPLE_COUNT_1_BIT
#define R3V_MAX_VERTEX_BINDINGS 16u
#define R3V_MAX_PUSH_CONSTANTS_SIZE 128u

#define R3V_VK10_MIN_IMAGE_DIMENSION_1D 4096u
/* The executed 2D ceiling: the transfer image family admits extents to
 * the single-tile texture dimension, so the limit advertises what
 * creation admits rather than the Vulkan 1.0 minimum. */
#define R3V_MAX_IMAGE_DIMENSION_2D R3V_R3XX_MAX_TEXTURE_DIMENSION
#define R3V_VK10_MIN_IMAGE_DIMENSION_CUBE 4096u
#define R3V_VK10_MIN_UNIFORM_BUFFER_RANGE (16u * 1024u)
#define R3V_VK10_MIN_STORAGE_BUFFER_RANGE (128u * 1024u * 1024u)
#define R3V_VK10_MIN_COMPUTE_SHARED_MEMORY_SIZE (16u * 1024u)
#define R3V_VK10_MIN_COMPUTE_WORKGROUP_INVOCATIONS 128u
#define R3V_VK10_MIN_COMPUTE_WORKGROUP_SIZE_X 128u
#define R3V_VK10_MIN_COMPUTE_WORKGROUP_SIZE_Y 128u
#define R3V_VK10_MIN_COMPUTE_WORKGROUP_SIZE_Z 64u
/* The executed render ceiling: the render-target family and the
 * viewport/scissor admissions top out at the qualified cell's 64-pixel
 * extent, so the framebuffer and viewport limits advertise it; the
 * deviation from the Vulkan 1.0 4096 minimum rides the declared
 * nonconformance. */
#define R3V_MAX_RENDER_EXTENT 64u
/* Every image and pipeline admission executes single-sample alone, so
 * the limits advertise the one truthful bit; Vulkan 1.0's required
 * minimum includes VK_SAMPLE_COUNT_4_BIT, and this deviation is part
 * of the declared nonconformance (conformanceVersion 0.0.0.0).
 */
#define R3V_SUPPORTED_SAMPLE_COUNTS VK_SAMPLE_COUNT_1_BIT

/* Conformance classification reported to probes and external tooling.
 *
 * No documented or silicon-proven native compute dispatch surface
 * exists for this RS482/RS485 R3V target: no COMPUTE queue, no
 * workgroup shared memory, no shader atomics, no SPIR-V compute
 * capabilities, no SSBO storage semantics in the current Mesa r300
 * implementation oracle or the AMD R3xx-RRG documentation.  The
 * VkQueueFlagBits reference (Vulkan spec ch. 5 "Devices and Queues",
 * VkQueueFamilyProperties::queueFlags) requires that a conformant
 * implementation report VK_QUEUE_COMPUTE_BIT for any queue family that
 * supports VK_QUEUE_GRAPHICS_BIT.  The r3v skeleton intentionally
 * violates that rule by omitting compute, so it must NOT claim Vulkan
 * conformance for any version.  Probes and CTS runners must observe
 * this classification before treating the implementation as a graphics
 * device. */
#define R3V_CONFORMANCE_STATUS "experimental_nonconformant_graphics_without_compute"

/* The number of fragment Gallium texture units r300 exposes.  Shared between the
 * pipeline (which flattens combined-image-sampler descriptors from every
 * descriptor set into this unit space) and the replay (which binds them). */
#define R3V_MAX_FS_SAMPLER_UNITS 16

static inline bool
r3v_pci_device_id_is_supported(uint32_t pci_device_id)
{
   switch (pci_device_id) {
   case R3V_PCI_DEVICE_ID_RS482:
   case R3V_PCI_DEVICE_ID_RS485:
      return true;
   default:
      return false;
   }
}

#ifdef __cplusplus
}
#endif

#endif /* R3V_PRIVATE_H */
