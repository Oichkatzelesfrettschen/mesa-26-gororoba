/*
 * SPDX-License-Identifier: MIT
 */

#ifndef R3V_IMAGE_H
#define R3V_IMAGE_H

#include "r3v_private.h"
#include "r3v_resource_state.h"

#include "vk_image.h"

#include "pipe/p_state.h"

#ifdef __cplusplus
extern "C" {
#endif

/* r3v_image wraps vk_image with a Gallium pipe_resource.
 * The resource is created at CreateImage time with PIPE_TEXTURE_2D,
 * PIPE_BIND_RENDER_TARGET | PIPE_BIND_SAMPLER_VIEW, and PIPE_USAGE_DEFAULT
 * so r300g routes it to VRAM.  PIPE_USAGE_STAGING is reserved for
 * the separate readback buffer in the triangle probe path. */
struct r3v_image {
   struct vk_image               vk;  /* must be first; contains vk_object_base */

   /* dma-buf export (the wsi-drm substrate): created with
    * VkExternalMemoryImageCreateInfo, backed by a single SHARED|SCANOUT
    * linear pipe resource so PRIME export hands X a KMS-displayable BO.
    * external_stride is the winsys-reported row pitch of that BO. */
   bool     external;
   uint32_t external_stride;
   struct pipe_resource         *resource;
   struct pipe_resource         *tiles[4];
   uint32_t                      tile_cols;
   uint32_t                      tile_rows;
   uint32_t                      tile_width[2];
   uint32_t                      tile_height[2];
   /* Row stride r300g chose for a VK_IMAGE_TILING_LINEAR image's single tile,
    * reported verbatim by GetImageSubresourceLayout.  Zero for optimal tiling. */
   uint32_t                      linear_row_pitch;
   struct r3v_resource_state  resource_state;

   /* Tier-2 LINEAR tile-stitch: a lazy sampler-only atlas of up to four
    * OVERLAPPING charts, each <= the 2048 sampler cap, with seam texels
    * duplicated so a bilinear footprint stays inside one chart (the render/copy
    * tiles[] above stay a disjoint partition).  content_serial advances on every
    * write into the image (render/clear/copy/transfer); the atlas is (re)built
    * when sampler_atlas.serial trails it.  origin/width are logical-x charts,
    * origin/height logical-y. */
   uint64_t                      content_serial;
   struct {
      struct pipe_resource      *tiles[4];
      uint32_t                   cols, rows;
      uint32_t                   origin_x[2], origin_y[2];
      uint32_t                   width[2], height[2];
      uint64_t                   serial;
      bool                       valid;
   }                             sampler_atlas;
};

VK_DEFINE_NONDISP_HANDLE_CASTS(r3v_image, vk.base, VkImage,
                                VK_OBJECT_TYPE_IMAGE)

struct r3v_device;
bool r3v_image_ensure_sampler_atlas(struct r3v_device *device,
                                       struct r3v_image *img);

/* Mark an image's content changed so a stale sampler atlas is rebuilt before the
 * next stitched sample.  content_serial is CPU-side invalidation bookkeeping, not
 * image content, so the const image the write replays carry can advance it. */
static inline void
r3v_image_mark_written(const struct r3v_image *img)
{
   ((struct r3v_image *)img)->content_serial++;
}

struct r3v_image_view {
   struct vk_image_view  vk;  /* must be first */
};

VK_DEFINE_NONDISP_HANDLE_CASTS(r3v_image_view, vk.base, VkImageView,
                                VK_OBJECT_TYPE_IMAGE_VIEW)

VkResult r3v_CreateImage(VkDevice device,
                             const VkImageCreateInfo *pCreateInfo,
                             const VkAllocationCallbacks *pAllocator,
                             VkImage *pImage);

void r3v_DestroyImage(VkDevice device,
                          VkImage image,
                          const VkAllocationCallbacks *pAllocator);

/* Byte size r3v advertises for an image in GetImageMemoryRequirements2;
 * BindImageMemory2 bounds-checks against the same value. */
VkDeviceSize r3v_image_memory_size(const struct r3v_image *img);

void r3v_GetImageMemoryRequirements2(VkDevice device,
                                         const VkImageMemoryRequirementsInfo2 *pInfo,
                                         VkMemoryRequirements2 *pMemoryRequirements);

void r3v_GetImageSparseMemoryRequirements2(VkDevice device,
   const VkImageSparseMemoryRequirementsInfo2 *pInfo,
   uint32_t *pSparseMemoryRequirementCount,
   VkSparseImageMemoryRequirements2 *pSparseMemoryRequirements);

/* vkGetImageSubresourceLayout is what deqp calls to learn a linear staging
 * image's rowPitch before mapping it.  The runtime vk_common base forwards the
 * v1 call to the dispatch table's GetImageSubresourceLayout2KHR slot.  That
 * name, ...2EXT (VK_EXT_host_image_copy), and the core ...2 are aliases that
 * share one device dispatch slot, so the driver defines only the core form and
 * the generated entrypoint table dispatches all three names to it. */
VKAPI_ATTR void VKAPI_CALL
r3v_GetImageSubresourceLayout2(VkDevice device, VkImage image,
                                  const VkImageSubresource2 *pSubresource,
                                  VkSubresourceLayout2 *pLayout);

/* VK_KHR_maintenance5: subresource layout of an image described by a
 * VkImageCreateInfo that has not been created.  Like GetImageSubresourceLayout2,
 * only the core form is defined; the ...KHR alias shares its dispatch slot. */
VKAPI_ATTR void VKAPI_CALL
r3v_GetDeviceImageSubresourceLayout(VkDevice device,
                                       const VkDeviceImageSubresourceInfo *pInfo,
                                       VkSubresourceLayout2 *pLayout);

VkResult r3v_CreateImageView(VkDevice device,
                                 const VkImageViewCreateInfo *pCreateInfo,
                                 const VkAllocationCallbacks *pAllocator,
                                 VkImageView *pView);

void r3v_DestroyImageView(VkDevice device,
                              VkImageView imageView,
                              const VkAllocationCallbacks *pAllocator);

#ifdef __cplusplus
}
#endif

#endif /* R3V_IMAGE_H */
