/*
 * Copyright (c) 2026 Terascale Functionalists
 * SPDX-License-Identifier: MIT
 */

#include "r300vk_memory.h"
#include "r300vk_buffer.h"
#include "r300vk_image.h"
#include "r300vk_device.h"

#include "vk_alloc.h"
#include "vk_log.h"
#include "vk_object.h"

#include "pipe/p_context.h"
#include "pipe/p_state.h"
#include "util/u_inlines.h"

VkResult
r300vk_AllocateMemory(VkDevice _device,
                      const VkMemoryAllocateInfo *pAllocateInfo,
                      const VkAllocationCallbacks *pAllocator,
                      VkDeviceMemory *pMemory)
{
   VK_FROM_HANDLE(r300vk_device, device, _device);
   struct r300vk_device_memory *mem;

   mem = vk_zalloc2(&device->vk.alloc, pAllocator,
                    sizeof(*mem), 8,
                    VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (!mem)
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);

   vk_object_base_init(&device->vk, &mem->base,
                       VK_OBJECT_TYPE_DEVICE_MEMORY);
   mem->size = pAllocateInfo->allocationSize;

   *pMemory = r300vk_device_memory_to_handle(mem);
   return VK_SUCCESS;
}

void
r300vk_FreeMemory(VkDevice _device,
                  VkDeviceMemory _memory,
                  const VkAllocationCallbacks *pAllocator)
{
   VK_FROM_HANDLE(r300vk_device, device, _device);
   VK_FROM_HANDLE(r300vk_device_memory, mem, _memory);
   if (!mem)
      return;

   if (mem->transfer) {
      device->pipe->buffer_unmap(device->pipe, mem->transfer);
      mem->transfer = NULL;
   }
   pipe_resource_reference(&mem->resource, NULL);

   vk_object_base_finish(&mem->base);
   vk_free2(&device->vk.alloc, pAllocator, mem);
}

VkResult
r300vk_MapMemory(VkDevice _device,
                 VkDeviceMemory _memory,
                 VkDeviceSize offset,
                 VkDeviceSize size,
                 VkMemoryMapFlags flags,
                 void **ppData)
{
   VK_FROM_HANDLE(r300vk_device, device, _device);
   VK_FROM_HANDLE(r300vk_device_memory, mem, _memory);

   if (!mem->resource)
      return vk_errorf(device, VK_ERROR_MEMORY_MAP_FAILED,
                       "r300vk: MapMemory called before BindBufferMemory2 "
                       "or BindImageMemory2 (resource-backed memory model "
                       "requires a prior bind)");

   if (mem->resource->target != PIPE_BUFFER)
      return vk_errorf(device, VK_ERROR_MEMORY_MAP_FAILED,
                       "r300vk: image-backed device memory is not host-mappable");

   /* Unmap any stale mapping from a previous vkMapMemory call.  Vulkan
    * spec forbids double-mapping without an intervening vkUnmapMemory,
    * but guard here to avoid leaking the old pipe_transfer. */
   if (mem->transfer) {
      device->pipe->buffer_unmap(device->pipe, mem->transfer);
      mem->transfer = NULL;
   }

   struct pipe_box box;
   u_box_1d((unsigned)offset,
            size == VK_WHOLE_SIZE ? mem->resource->width0 - (unsigned)offset
                                  : (unsigned)size,
            &box);

   void *ptr = device->pipe->buffer_map(device->pipe, mem->resource, 0,
                                         PIPE_MAP_READ_WRITE, &box,
                                         &mem->transfer);
   if (!ptr)
      return vk_error(device, VK_ERROR_MEMORY_MAP_FAILED);

   mem->map_ptr = ptr;
   *ppData = ptr;
   return VK_SUCCESS;
}

void
r300vk_UnmapMemory(VkDevice _device,
                   VkDeviceMemory _memory)
{
   VK_FROM_HANDLE(r300vk_device, device, _device);
   VK_FROM_HANDLE(r300vk_device_memory, mem, _memory);

   if (!mem->transfer)
      return;

   device->pipe->buffer_unmap(device->pipe, mem->transfer);
   mem->transfer = NULL;
   mem->map_ptr = NULL;
}

VkResult
r300vk_FlushMappedMemoryRanges(VkDevice _device,
                                uint32_t memoryRangeCount,
                                const VkMappedMemoryRange *pMemoryRanges)
{
   /* RS482/RS485 is UMA; no CPU-GPU cache separation on this target. */
   (void)_device;
   (void)memoryRangeCount;
   (void)pMemoryRanges;
   return VK_SUCCESS;
}

VkResult
r300vk_InvalidateMappedMemoryRanges(VkDevice _device,
                                     uint32_t memoryRangeCount,
                                     const VkMappedMemoryRange *pMemoryRanges)
{
   /* RS482/RS485 is UMA; no CPU-GPU cache separation on this target. */
   (void)_device;
   (void)memoryRangeCount;
   (void)pMemoryRanges;
   return VK_SUCCESS;
}

VkResult
r300vk_BindBufferMemory2(VkDevice _device,
                          uint32_t bindInfoCount,
                          const VkBindBufferMemoryInfo *pBindInfos)
{
   VK_FROM_HANDLE(r300vk_device, device, _device);
   for (uint32_t i = 0; i < bindInfoCount; i++) {
      if (pBindInfos[i].memoryOffset != 0)
         return vk_errorf(device, VK_ERROR_INITIALIZATION_FAILED,
                          "r300vk: non-zero memoryOffset is unsupported "
                          "in the resource-backed memory model");
      VK_FROM_HANDLE(r300vk_buffer, buf, pBindInfos[i].buffer);
      VK_FROM_HANDLE(r300vk_device_memory, mem, pBindInfos[i].memory);
      pipe_resource_reference(&mem->resource, buf->resource);
   }
   return VK_SUCCESS;
}

VkResult
r300vk_BindImageMemory2(VkDevice _device,
                         uint32_t bindInfoCount,
                         const VkBindImageMemoryInfo *pBindInfos)
{
   VK_FROM_HANDLE(r300vk_device, device, _device);
   for (uint32_t i = 0; i < bindInfoCount; i++) {
      if (pBindInfos[i].memoryOffset != 0)
         return vk_errorf(device, VK_ERROR_INITIALIZATION_FAILED,
                          "r300vk: non-zero memoryOffset is unsupported "
                          "in the resource-backed memory model");
      VK_FROM_HANDLE(r300vk_image, img, pBindInfos[i].image);
      VK_FROM_HANDLE(r300vk_device_memory, mem, pBindInfos[i].memory);
      pipe_resource_reference(&mem->resource, img->resource);
   }
   return VK_SUCCESS;
}
