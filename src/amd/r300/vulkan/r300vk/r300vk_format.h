/*
 * Copyright (c) 2026 Terascale Functionalists
 * SPDX-License-Identifier: MIT
 */

#ifndef R300VK_FORMAT_H
#define R300VK_FORMAT_H

#include <vulkan/vulkan_core.h>

#include "util/format/u_format.h"
#include "vk_format.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The single VkFormat -> pipe_format authority for r300vk.
 *
 * util's vk_format_to_pipe_format maps the two packed depth-stencil VkFormats
 * to the depth-low component order (VK_FORMAT_D24_UNORM_S8_UINT ->
 * PIPE_FORMAT_Z24_UNORM_S8_UINT, VK_FORMAT_X8_D24_UNORM_PACK32 ->
 * PIPE_FORMAT_Z24X8_UNORM), but the r300 ZS tile hardware implements only the
 * stencil-low twins (r300_texture.c is_format_supported accepts Z16_UNORM,
 * X8Z24_UNORM, and S8_UINT_Z24_UNORM).  Vulkan prescribes no combined-aspect
 * packing for these formats -- host access is defined per aspect only -- so
 * backing them with the swapped twin is a valid implementation, and it is the
 * only one this silicon offers.  Every r300vk conversion of an image or
 * attachment VkFormat goes through this helper so the format-properties
 * advertise, vkCreateImage's pipe_resource template, and the replay's sampler
 * views all agree on the same backing layout.
 *
 * TODO: the depth-aspect buffer copy layout (Vulkan "Copying Data Between
 *       Buffers and Images": D24 in bits 0..23 of a 32-bit word) does not
 *       match S8_UINT_Z24_UNORM's depth-in-bits-8..31 storage, so
 *       r300vk_replay_cpu_readback and the CopyBufferToImage path must repack
 *       the depth aspect when depth-aspect transfers start being exercised;
 *       the replay binds no depth attachment yet, so no current consumer hits
 *       this. */
static inline enum pipe_format
r300vk_vk_format_to_pipe_format(VkFormat vk_format)
{
   switch (vk_format) {
   case VK_FORMAT_D24_UNORM_S8_UINT:
      return PIPE_FORMAT_S8_UINT_Z24_UNORM;
   case VK_FORMAT_X8_D24_UNORM_PACK32:
      return PIPE_FORMAT_X8Z24_UNORM;
   default:
      return vk_format_to_pipe_format(vk_format);
   }
}

#ifdef __cplusplus
}
#endif

#endif /* R300VK_FORMAT_H */
