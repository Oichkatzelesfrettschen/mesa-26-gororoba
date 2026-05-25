/*
 * Copyright (c) 2026 Terascale Functionalists
 * SPDX-License-Identifier: MIT
 */

#include "r300vk_image.h"
#include "r300vk_device.h"

#include "vk_alloc.h"
#include "vk_format.h"
#include "vk_log.h"

#include "pipe/p_context.h"
#include "pipe/p_defines.h"
#include "pipe/p_state.h"
#include "util/format/u_format.h"
#include "util/macros.h"
#include "util/u_inlines.h"

static uint32_t
r300vk_split_image_axis(uint32_t extent, uint32_t tiles[2])
{
   if (extent == 0 || extent > R300VK_VK10_MIN_IMAGE_DIMENSION_2D)
      return 0;

   if (extent <= R300VK_R3XX_MAX_RENDER_DIMENSION) {
      tiles[0] = extent;
      tiles[1] = 0;
      return 1;
   }

   tiles[0] = R300VK_R3XX_MAX_RENDER_DIMENSION;
   tiles[1] = extent - R300VK_R3XX_MAX_RENDER_DIMENSION;
   return 2;
}

static void
r300vk_image_release_resources(struct r300vk_image *img)
{
   for (uint32_t i = 0; i < ARRAY_SIZE(img->tiles); i++)
      pipe_resource_reference(&img->tiles[i], NULL);
   img->resource = NULL;
}

static VkResult
r300vk_image_create_tile_resources(struct r300vk_device *device,
                                   struct r300vk_image *img,
                                   const VkImageCreateInfo *info,
                                   enum pipe_format pipe_fmt)
{
   img->tile_cols = r300vk_split_image_axis(info->extent.width,
                                            img->tile_width);
   img->tile_rows = r300vk_split_image_axis(info->extent.height,
                                            img->tile_height);
   if (img->tile_cols == 0 || img->tile_rows == 0)
      return vk_errorf(device, VK_ERROR_INITIALIZATION_FAILED,
                       "r300vk: image extent %ux%u exceeds the 4096 floor",
                       info->extent.width, info->extent.height);

   for (uint32_t y = 0; y < img->tile_rows; y++) {
      for (uint32_t x = 0; x < img->tile_cols; x++) {
         struct pipe_resource tmpl = {
            .target     = PIPE_TEXTURE_2D,
            .format     = pipe_fmt,
            .bind       = PIPE_BIND_RENDER_TARGET | PIPE_BIND_SAMPLER_VIEW,
            .usage      = PIPE_USAGE_DEFAULT,
            .width0     = img->tile_width[x],
            .height0    = img->tile_height[y],
            .depth0     = 1,
            .array_size = 1,
            .last_level = 0,
            .nr_samples = info->samples,
         };

         const uint32_t tile_index = y * img->tile_cols + x;
         img->tiles[tile_index] =
            device->screen->resource_create(device->screen, &tmpl);
         if (!img->tiles[tile_index]) {
            r300vk_image_release_resources(img);
            return vk_error(device, VK_ERROR_OUT_OF_DEVICE_MEMORY);
         }
      }
   }

   img->resource = img->tiles[0];
   return VK_SUCCESS;
}

VkResult
r300vk_CreateImage(VkDevice _device,
                   const VkImageCreateInfo *pCreateInfo,
                   const VkAllocationCallbacks *pAllocator,
                   VkImage *pImage)
{
   VK_FROM_HANDLE(r300vk_device, device, _device);
   struct r300vk_image *img;

   img = vk_zalloc2(&device->vk.alloc, pAllocator,
                    sizeof(*img), 8,
                    VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (!img)
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);

   vk_image_init(&device->vk, &img->vk, pCreateInfo);

   /* R300-class hardware only supports flat 2D images: one layer and one mip
    * level.  The 4096 Vulkan 1.0 floor is represented as one, two, or four
    * hardware-sized 2D resources, and 4x MSAA is passed through to r300g per
    * tile.  Reject unsupported shapes so callers see a clear error rather
    * than silently incorrect behavior. */
   if (pCreateInfo->arrayLayers > 1) {
      vk_image_finish(&img->vk);
      vk_free2(&device->vk.alloc, pAllocator, img);
      return vk_errorf(device, VK_ERROR_UNKNOWN,
                       "r300vk: arrayLayers %u > 1 unsupported",
                       pCreateInfo->arrayLayers);
   }
   if (pCreateInfo->mipLevels > 1) {
      vk_image_finish(&img->vk);
      vk_free2(&device->vk.alloc, pAllocator, img);
      return vk_errorf(device, VK_ERROR_UNKNOWN,
                       "r300vk: mipLevels %u > 1 unsupported",
                       pCreateInfo->mipLevels);
   }
   if (pCreateInfo->samples != VK_SAMPLE_COUNT_1_BIT &&
       pCreateInfo->samples != VK_SAMPLE_COUNT_4_BIT) {
      vk_image_finish(&img->vk);
      vk_free2(&device->vk.alloc, pAllocator, img);
      return vk_errorf(device, VK_ERROR_UNKNOWN,
                       "r300vk: samples 0x%x unsupported",
                       pCreateInfo->samples);
   }

   enum pipe_format pipe_fmt = vk_format_to_pipe_format(pCreateInfo->format);
   if (pipe_fmt == PIPE_FORMAT_NONE) {
      vk_image_finish(&img->vk);
      vk_free2(&device->vk.alloc, pAllocator, img);
      return vk_errorf(device, VK_ERROR_FORMAT_NOT_SUPPORTED,
                       "r300vk: unsupported image format %d", pCreateInfo->format);
   }

   VkResult result =
      r300vk_image_create_tile_resources(device, img, pCreateInfo, pipe_fmt);
   if (result != VK_SUCCESS) {
      vk_image_finish(&img->vk);
      vk_free2(&device->vk.alloc, pAllocator, img);
      return result;
   }

   *pImage = r300vk_image_to_handle(img);
   return VK_SUCCESS;
}

void
r300vk_DestroyImage(VkDevice _device,
                    VkImage _image,
                    const VkAllocationCallbacks *pAllocator)
{
   VK_FROM_HANDLE(r300vk_device, device, _device);
   VK_FROM_HANDLE(r300vk_image, img, _image);
   if (!img)
      return;

   r300vk_image_release_resources(img);
   vk_image_finish(&img->vk);
   vk_free2(&device->vk.alloc, pAllocator, img);
}

void
r300vk_GetImageMemoryRequirements2(VkDevice _device,
                                    const VkImageMemoryRequirementsInfo2 *pInfo,
                                    VkMemoryRequirements2 *pMemoryRequirements)
{
   VK_FROM_HANDLE(r300vk_image, img, pInfo->image);
   const VkExtent3D *ext = &img->vk.extent;

   /* 4096-byte alignment satisfies r300g tiling requirements. */
   pMemoryRequirements->memoryRequirements = (VkMemoryRequirements){
      .size           = (VkDeviceSize)ext->width * ext->height *
                        MAX2(1u, img->vk.samples) *
                        util_format_get_blocksize(vk_format_to_pipe_format(img->vk.format)),
      .alignment      = 4096,
      /* r300g places a single-sample render-target texture in
       * RADEON_DOMAIN_VRAM | RADEON_DOMAIN_GTT (r300_texture.c domain
       * selection), so both advertised memory types back it validly: type 0
       * (host-visible GTT) and type 1 (device-local).  The bound BO domain is
       * fixed by the pipe_resource template, not by the type the app picks, so
       * a device-local color target binds correctly.  Reporting only type 0
       * rejected a conformant device-local color-image allocation. */
      .memoryTypeBits = 0x3,
   };
}

VkResult
r300vk_CreateImageView(VkDevice _device,
                       const VkImageViewCreateInfo *pCreateInfo,
                       const VkAllocationCallbacks *pAllocator,
                       VkImageView *pView)
{
   VK_FROM_HANDLE(r300vk_device, device, _device);
   struct r300vk_image_view *iv;

   iv = vk_zalloc2(&device->vk.alloc, pAllocator,
                   sizeof(*iv), 8,
                   VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (!iv)
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);

   vk_image_view_init(&device->vk, &iv->vk, pCreateInfo);

   *pView = r300vk_image_view_to_handle(iv);
   return VK_SUCCESS;
}

void
r300vk_DestroyImageView(VkDevice _device,
                         VkImageView _view,
                         const VkAllocationCallbacks *pAllocator)
{
   VK_FROM_HANDLE(r300vk_device, device, _device);
   VK_FROM_HANDLE(r300vk_image_view, iv, _view);
   if (!iv)
      return;

   vk_image_view_finish(&iv->vk);
   vk_free2(&device->vk.alloc, pAllocator, iv);
}
