/*
 * SPDX-License-Identifier: MIT
 */

#ifndef R3V_PRIVATE_H
#define R3V_PRIVATE_H

#include <vulkan/vulkan_core.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#include "util/u_debug.h"

#ifdef __cplusplus
extern "C" {
#endif

#define R3V_VENDOR_ID_ATI 0x1002

/* RS482 (Radeon Xpress 200M, IGP) and RS485 (Radeon Xpress 1100/1150,
 * mobile IGP).  The PCI table in src/gallium/drivers/r300/r300_chipset.c
 * maps 0x5974 to RS482_5974 and 0x5975 to RS482_5975 in the RS480 family.
 * Both report GL_RENDERER="ATI RS480"; both route the vertex stage
 * through Gallium Draw SW TCL because num_vert_fpus == 0 for the RS480
 * family in r300_parse_chipset(). */
#define R3V_PCI_DEVICE_ID_RS482 0x5974
#define R3V_PCI_DEVICE_ID_RS485 0x5975

#define R3V_API_VERSION VK_MAKE_API_VERSION(0, 1, 0, VK_HEADER_VERSION)

#define R3V_HYBRID_COMPUTE_ENV "R3V_HYBRID_COMPUTE_EXPERIMENTAL"
#define R3V_HYBRID_COMPUTE_ENV_VALUE "1"
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
#define R3V_VK10_MIN_IMAGE_DIMENSION_2D 4096u
#define R3V_VK10_MIN_IMAGE_DIMENSION_CUBE 4096u
#define R3V_VK10_MIN_UNIFORM_BUFFER_RANGE (16u * 1024u)
#define R3V_VK10_MIN_STORAGE_BUFFER_RANGE (128u * 1024u * 1024u)
#define R3V_VK10_MIN_COMPUTE_SHARED_MEMORY_SIZE (16u * 1024u)
#define R3V_VK10_MIN_COMPUTE_WORKGROUP_INVOCATIONS 128u
#define R3V_VK10_MIN_COMPUTE_WORKGROUP_SIZE_X 128u
#define R3V_VK10_MIN_COMPUTE_WORKGROUP_SIZE_Y 128u
#define R3V_VK10_MIN_COMPUTE_WORKGROUP_SIZE_Z 64u
#define R3V_VK10_MIN_VIEWPORT_DIMENSION 4096u
#define R3V_VK10_MIN_FRAMEBUFFER_DIMENSION 4096u
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
#define R3V_HYBRID_COMPUTE_STATUS "experimental_nonconformant_hybrid_compute_queue"

/* The number of fragment Gallium texture units r300 exposes.  Shared between the
 * pipeline (which flattens combined-image-sampler descriptors from every
 * descriptor set into this unit space) and the replay (which binds them). */
#define R3V_MAX_FS_SAMPLER_UNITS 16

/* Experimental NEAREST tile-stitching.  r300 cannot address a texture wider or
 * taller than the sampler cap (2048 on r300-class), so r3v stores a larger
 * logical image as a grid of up to 2x2 hardware tiles and normally refuses to
 * bind such a split image as a sampled view.  Under this gate a logical sampled
 * image is compiled into a static family of per-tile samplers plus a piecewise-
 * affine coordinate transform: one texture() becomes up to four hardware fetches
 * and a branchless tile select.  Exact ONLY for NEAREST / CLAMP_TO_EDGE /
 * normalized-coordinate / 2D / single-mip / single-layer sampling; it makes no
 * conformance claim and does not solve LINEAR seams, mip chains, wrap modes, or
 * the texture-fetch precision the deqp texture-lookup tests check.  Off by
 * default. */
#define R3V_NEAREST_STITCH_TILE_UNITS  4u  /* up to a 2x2 tile grid */
#define R3V_NEAREST_STITCH_CONST_VEC4S 2u  /* per-tile affine + split thresholds */

/* Environment gates read the canonical R3V_ name first and fall back to
 * the pre-rename R300VK_ spelling so retained runbooks and probe scripts
 * keep working; the legacy arm retires with the r300vk compatibility
 * surface. */
static inline const char *
r3v_getenv_compat(const char *r3v_name, const char *legacy_name)
{
   const char *v = getenv(r3v_name);
   return v ? v : getenv(legacy_name);
}

static inline bool
r3v_debug_option_enabled(const char *options, const char *option)
{
   return options && comma_separated_list_contains(options, option);
}

static inline bool
r3v_experimental_nearest_stitch_enabled(void)
{
   const char *e = r3v_getenv_compat("R3V_EXPERIMENTAL_NEAREST_STITCH",
                                     "R300VK_EXPERIMENTAL_NEAREST_STITCH");
   return e && e[0] == '1' && e[1] == '\0';
}

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
