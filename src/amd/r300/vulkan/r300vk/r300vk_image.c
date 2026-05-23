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
#include "util/u_inlines.h"

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

   struct pipe_resource tmpl = {
      .target     = PIPE_TEXTURE_2D,
      .format     = vk_format_to_pipe_format(pCreateInfo->format),
      .bind       = PIPE_BIND_RENDER_TARGET | PIPE_BIND_SAMPLER_VIEW,
      .usage      = PIPE_USAGE_DEFAULT,
      .width0     = pCreateInfo->extent.width,
      .height0    = pCreateInfo->extent.height,
      .depth0     = 1,
      .array_size = 1,
      .last_level = 0,
      .nr_samples = 1,
   };

   img->resource = device->screen->resource_create(device->screen, &tmpl);
   if (!img->resource) {
      vk_image_finish(&img->vk);
      vk_free2(&device->vk.alloc, pAllocator, img);
      return vk_error(device, VK_ERROR_OUT_OF_DEVICE_MEMORY);
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

   pipe_resource_reference(&img->resource, NULL);
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
                        util_format_get_blocksize(vk_format_to_pipe_format(img->vk.format)),
      .alignment      = 4096,
      .memoryTypeBits = 1,  /* GTT heap index 0 */
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
