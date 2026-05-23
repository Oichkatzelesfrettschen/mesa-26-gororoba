/*
 * Copyright (c) 2026 Terascale Functionalists
 * SPDX-License-Identifier: MIT
 */

#include "r300vk_buffer.h"
#include "r300vk_device.h"

#include "vk_alloc.h"
#include "vk_log.h"
#include "vk_object.h"

#include "pipe/p_context.h"
#include "pipe/p_defines.h"
#include "pipe/p_state.h"
#include "util/u_inlines.h"

VkResult
r300vk_CreateBuffer(VkDevice _device,
                    const VkBufferCreateInfo *pCreateInfo,
                    const VkAllocationCallbacks *pAllocator,
                    VkBuffer *pBuffer)
{
   VK_FROM_HANDLE(r300vk_device, device, _device);
   struct r300vk_buffer *buf;

   buf = vk_zalloc2(&device->vk.alloc, pAllocator,
                    sizeof(*buf), 8,
                    VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (!buf)
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);

   vk_object_base_init(&device->vk, &buf->base, VK_OBJECT_TYPE_BUFFER);
   buf->size  = pCreateInfo->size;
   buf->usage = pCreateInfo->usage;

   struct pipe_resource tmpl = {
      .target    = PIPE_BUFFER,
      .format    = PIPE_FORMAT_R8_UNORM,
      .bind      = PIPE_BIND_VERTEX_BUFFER,
      .usage     = PIPE_USAGE_DYNAMIC,
      .width0    = (unsigned)pCreateInfo->size,
      .height0   = 1,
      .depth0    = 1,
      .array_size = 1,
   };

   buf->resource = device->screen->resource_create(device->screen, &tmpl);
   if (!buf->resource) {
      vk_object_base_finish(&buf->base);
      vk_free2(&device->vk.alloc, pAllocator, buf);
      return vk_error(device, VK_ERROR_OUT_OF_DEVICE_MEMORY);
   }

   *pBuffer = r300vk_buffer_to_handle(buf);
   return VK_SUCCESS;
}

void
r300vk_DestroyBuffer(VkDevice _device,
                     VkBuffer _buffer,
                     const VkAllocationCallbacks *pAllocator)
{
   VK_FROM_HANDLE(r300vk_device, device, _device);
   VK_FROM_HANDLE(r300vk_buffer, buf, _buffer);
   if (!buf)
      return;

   pipe_resource_reference(&buf->resource, NULL);
   vk_object_base_finish(&buf->base);
   vk_free2(&device->vk.alloc, pAllocator, buf);
}

void
r300vk_GetBufferMemoryRequirements2(VkDevice _device,
                                     const VkBufferMemoryRequirementsInfo2 *pInfo,
                                     VkMemoryRequirements2 *pMemoryRequirements)
{
   VK_FROM_HANDLE(r300vk_buffer, buf, pInfo->buffer);

   pMemoryRequirements->memoryRequirements = (VkMemoryRequirements){
      .size           = buf->size,
      .alignment      = 4,
      .memoryTypeBits = 1,  /* GTT heap index 0 */
   };
}
