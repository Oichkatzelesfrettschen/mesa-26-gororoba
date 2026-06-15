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
 * TODO: depth/stencil is excluded from TRANSFER_DST below because the depth
 *       aspect needs an 8-bit repack.  Vulkan's depth-aspect buffer layout for
 *       D24_UNORM_S8_UINT / X8_D24_UNORM_PACK32 puts D24 in bits [23:0]; r300's
 *       S8_UINT_Z24_UNORM / X8Z24_UNORM store depth in bits [31:8] and
 *       stencil/pad in [7:0] (util_pack_z shifts depth << 8, u_pack_color.h).
 *       Per-aspect read-modify-write at the two r300vk_queue.c copy choke points
 *       closes it (verified layout, steinmarder finding
 *       rs482-r300vk-depth-transfer-repack-first-principles):
 *         depth   buf->img: img = (img & 0x000000FF) | ((buf & 0x00FFFFFF) << 8)
 *         depth   img->buf: buf = (img >> 8) & 0x00FFFFFF
 *         stencil buf->img: img = (img & 0xFFFFFF00) | (s8 & 0xFF)
 *         stencil img->buf: s8  = img & 0xFF
 *       in r300vk_copy_buffer_region_to_image and
 *       r300vk_copy_image_region_to_buffer; Z16_UNORM needs no repack.  Admit
 *       Z16/X8Z24/S8_UINT_Z24 in r300vk_format_supports_transfer_dst only once
 *       the repack lands and a depth-transfer probe verifies it byte-exact
 *       against the NVIDIA oracle, so it can never regress to silent wrong
 *       depth. */
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

/* The single authority on the transfer-destination contract, shared by the
 * format-properties advertise, the image-format-properties usage gate, and
 * vkCreateImage's usage accept so all three agree.  The replay's
 * buffer<->image and image<->image copies move raw bytes through pipe
 * texture_map with no per-texel conversion, so any plain format with a
 * defined byte layout transfers losslessly -- snorm and 32-bit-float
 * semantics never enter a memcpy.  Block-compressed formats transfer in
 * block units (the tile walks and the buffer-image span run their rect
 * arithmetic in block space).  The one real constraint remaining excluded:
 * combined depth/stencil formats need the per-aspect repacking described in
 * the depth-aspect TODO above. */
static inline bool
r300vk_format_supports_transfer_dst(enum pipe_format pipe_format)
{
   if (util_format_is_depth_or_stencil(pipe_format))
      return false;

   const struct util_format_description *desc =
      util_format_description(pipe_format);
   return desc && desc->nr_channels > 0;
}

#ifdef __cplusplus
}
#endif

#endif /* R300VK_FORMAT_H */
