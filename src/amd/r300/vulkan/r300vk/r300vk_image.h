/*
 * Copyright (c) 2026 Terascale Functionalists
 * SPDX-License-Identifier: MIT
 */

#ifndef R300VK_IMAGE_H
#define R300VK_IMAGE_H

#include "r300vk_private.h"
#include "r300vk_resource_state.h"

#include "vk_image.h"

#include "pipe/p_state.h"

#ifdef __cplusplus
extern "C" {
#endif

/* r300vk_image wraps vk_image with a Gallium pipe_resource.
 * The resource is created at CreateImage time with PIPE_TEXTURE_2D,
 * PIPE_BIND_RENDER_TARGET | PIPE_BIND_SAMPLER_VIEW, and PIPE_USAGE_DEFAULT
 * so r300g routes it to VRAM.  PIPE_USAGE_STAGING is reserved for
 * the separate readback buffer in the triangle probe path. */
struct r300vk_image {
   struct vk_image               vk;  /* must be first; contains vk_object_base */
   struct pipe_resource         *resource;
   struct pipe_resource         *tiles[4];
   uint32_t                      tile_cols;
   uint32_t                      tile_rows;
   uint32_t                      tile_width[2];
   uint32_t                      tile_height[2];
   /* Row stride r300g chose for a VK_IMAGE_TILING_LINEAR image's single tile,
    * reported verbatim by GetImageSubresourceLayout.  Zero for optimal tiling. */
   uint32_t                      linear_row_pitch;
   struct r300vk_resource_state  resource_state;
};

VK_DEFINE_NONDISP_HANDLE_CASTS(r300vk_image, vk.base, VkImage,
                                VK_OBJECT_TYPE_IMAGE)

struct r300vk_image_view {
   struct vk_image_view  vk;  /* must be first */
};

VK_DEFINE_NONDISP_HANDLE_CASTS(r300vk_image_view, vk.base, VkImageView,
                                VK_OBJECT_TYPE_IMAGE_VIEW)

VkResult r300vk_CreateImage(VkDevice device,
                             const VkImageCreateInfo *pCreateInfo,
                             const VkAllocationCallbacks *pAllocator,
                             VkImage *pImage);

void r300vk_DestroyImage(VkDevice device,
                          VkImage image,
                          const VkAllocationCallbacks *pAllocator);

/* Byte size r300vk advertises for an image in GetImageMemoryRequirements2;
 * BindImageMemory2 bounds-checks against the same value. */
VkDeviceSize r300vk_image_memory_size(const struct r300vk_image *img);

void r300vk_GetImageMemoryRequirements2(VkDevice device,
                                         const VkImageMemoryRequirementsInfo2 *pInfo,
                                         VkMemoryRequirements2 *pMemoryRequirements);

/* vkGetImageSubresourceLayout is what deqp calls to learn a linear staging
 * image's rowPitch before mapping it.  The runtime's vk_common base forwards to
 * the 2KHR slot, and the core/KHR/EXT names are distinct dispatch slots, so all
 * three are defined over one shared layout helper. */
VKAPI_ATTR void VKAPI_CALL
r300vk_GetImageSubresourceLayout2(VkDevice device, VkImage image,
                                  const VkImageSubresource2 *pSubresource,
                                  VkSubresourceLayout2 *pLayout);
VKAPI_ATTR void VKAPI_CALL
r300vk_GetImageSubresourceLayout2KHR(VkDevice device, VkImage image,
                                     const VkImageSubresource2 *pSubresource,
                                     VkSubresourceLayout2 *pLayout);
VKAPI_ATTR void VKAPI_CALL
r300vk_GetImageSubresourceLayout2EXT(VkDevice device, VkImage image,
                                     const VkImageSubresource2 *pSubresource,
                                     VkSubresourceLayout2 *pLayout);

VkResult r300vk_CreateImageView(VkDevice device,
                                 const VkImageViewCreateInfo *pCreateInfo,
                                 const VkAllocationCallbacks *pAllocator,
                                 VkImageView *pView);

void r300vk_DestroyImageView(VkDevice device,
                              VkImageView imageView,
                              const VkAllocationCallbacks *pAllocator);

#ifdef __cplusplus
}
#endif

#endif /* R300VK_IMAGE_H */
