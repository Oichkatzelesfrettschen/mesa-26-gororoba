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
#include "pipe/p_defines.h"
#include "pipe/p_state.h"
#include "util/u_inlines.h"

#include <string.h>

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
      if (mem->resource && mem->resource->target == PIPE_BUFFER)
         device->pipe->buffer_unmap(device->pipe, mem->transfer);
      else
         device->pipe->texture_unmap(device->pipe, mem->transfer);
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

   /* Map-before-bind: Vulkan permits mapping HOST_VISIBLE VkDeviceMemory before
    * (or without) binding it to a buffer or image, and deqp's default allocator
    * does exactly this.  The resource-backed model has no storage until a bind,
    * so lazily create the memory's own host-visible pipe_buffer here.  A later
    * BindBufferMemory2 makes the VkBuffer reference this same resource, so writes
    * made through the map reach the bound buffer (single shared storage, no
    * copy).  Image binds install their tiled resource and never reach this lazy
    * path once bound. */
   if (!mem->resource) {
      struct pipe_resource tmpl = {
         .target     = PIPE_BUFFER,
         .format     = PIPE_FORMAT_R8_UNORM,
         .bind       = PIPE_BIND_VERTEX_BUFFER,
         .usage      = PIPE_USAGE_DYNAMIC,
         .width0     = (unsigned)mem->size,
         .height0    = 1,
         .depth0     = 1,
         .array_size = 1,
      };
      mem->resource = device->screen->resource_create(device->screen, &tmpl);
      if (!mem->resource)
         return vk_error(device, VK_ERROR_MEMORY_MAP_FAILED);
      mem->owns_buffer = true;
   }

   /* Unmap any stale mapping from a previous vkMapMemory call.  Vulkan
    * spec forbids double-mapping without an intervening vkUnmapMemory,
    * but guard here to avoid leaking the old pipe_transfer. */
   if (mem->transfer) {
      if (mem->resource->target == PIPE_BUFFER)
         device->pipe->buffer_unmap(device->pipe, mem->transfer);
      else
         device->pipe->texture_unmap(device->pipe, mem->transfer);
      mem->transfer = NULL;
   }

   struct pipe_box box;
   void *ptr;

   if (mem->resource->target == PIPE_BUFFER) {
      /* resource_offset: byte offset into the pipe_resource.  MapMemory's
       * offset is relative to the VkDeviceMemory object; memory_offset is
       * where the buffer starts within that object. */
      if (offset < mem->memory_offset)
         return vk_error(device, VK_ERROR_MEMORY_MAP_FAILED);
      unsigned resource_offset = (unsigned)(offset - mem->memory_offset);
      unsigned map_size = size == VK_WHOLE_SIZE
                          ? mem->resource->width0 - resource_offset
                          : (unsigned)size;
      if (resource_offset + map_size > mem->resource->width0)
         return vk_error(device, VK_ERROR_MEMORY_MAP_FAILED);
      u_box_1d(resource_offset, map_size, &box);
      ptr = device->pipe->buffer_map(device->pipe, mem->resource, 0,
                                      PIPE_MAP_READ_WRITE, &box,
                                      &mem->transfer);
   } else {
      /* Texture resource: map mip level 0, full surface.  Non-zero offsets
       * and sub-range sizes are not meaningful for driver-tiled textures;
       * callers that need linear-addressable sub-range access should bind to
       * a PIPE_BUFFER readback resource instead.
       * Reject a non-zero sub-offset (the map returns the whole surface) and
       * any explicit size that exceeds the allocation bytes remaining from
       * the map offset.  The bound is mem->size - offset, not mem->size: an
       * image bound at a non-zero memoryOffset has a shorter valid tail, so
       * bounding by the full allocation size would accept ranges whose
       * offset + size runs past the VkDeviceMemory end.  Compare bytes, not
       * width0 which counts texels.  The right clause is evaluated only when
       * effective_offset == 0 (offset == memory_offset <= mem->size), so the
       * subtraction cannot underflow. */
      VkDeviceSize effective_offset = (offset >= mem->memory_offset)
                                      ? offset - mem->memory_offset : 1;
      if (effective_offset != 0 ||
          (size != VK_WHOLE_SIZE && size > mem->size - offset))
         return vk_error(device, VK_ERROR_MEMORY_MAP_FAILED);
      u_box_origin_2d(mem->resource->width0, mem->resource->height0, &box);
      ptr = device->pipe->texture_map(device->pipe, mem->resource, 0,
                                       PIPE_MAP_READ_WRITE, &box,
                                       &mem->transfer);
   }

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

   if (mem->resource && mem->resource->target == PIPE_BUFFER)
      device->pipe->buffer_unmap(device->pipe, mem->transfer);
   else
      device->pipe->texture_unmap(device->pipe, mem->transfer);
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
      VK_FROM_HANDLE(r300vk_buffer, buf, pBindInfos[i].buffer);
      VK_FROM_HANDLE(r300vk_device_memory, mem, pBindInfos[i].memory);

      if (mem->owns_buffer) {
         /* Map-before-bind created the memory's own host buffer and the app wrote
          * the buffer's data through the map.  CreateBuffer already gave the
          * VkBuffer its own resource starting at byte 0, and every consumer
          * (vertex bind, descriptors, the identity-map compute replay) reads
          * buf->resource from byte 0 -- so copy the buffer's slice of the mapped
          * allocation (at memoryOffset) into buf->resource.  Every consumer stays
          * offset-free: no draw/descriptor/compute path changes, so the shipping
          * dispatch path is byte-for-byte untouched.  The cost is CPU-write
          * coherency for the unusual map-AFTER-bind rewrite, which deqp's
          * allocate->map->write->bind allocator (and the sub-allocating default
          * allocator that binds at a non-zero offset) does not use. */
         if (mem->transfer) {
            /* Commit any live mapping so the copy sees the written bytes. */
            device->pipe->buffer_unmap(device->pipe, mem->transfer);
            mem->transfer = NULL;
            mem->map_ptr = NULL;
         }
         VkDeviceSize avail = mem->size - pBindInfos[i].memoryOffset;
         unsigned copy_bytes = (unsigned)(buf->size < avail ? buf->size : avail);
         struct pipe_box src_box;
         struct pipe_transfer *st = NULL, *dt = NULL;
         u_box_1d((unsigned)pBindInfos[i].memoryOffset, copy_bytes, &src_box);
         const void *src = device->pipe->buffer_map(device->pipe, mem->resource, 0,
                                                    PIPE_MAP_READ, &src_box, &st);
         struct pipe_box dst_box;
         u_box_1d(0, copy_bytes, &dst_box);
         void *dst = device->pipe->buffer_map(device->pipe, buf->resource, 0,
                                              PIPE_MAP_WRITE, &dst_box, &dt);
         if (src && dst)
            memcpy(dst, src, copy_bytes);
         if (dt)
            device->pipe->buffer_unmap(device->pipe, dt);
         if (st)
            device->pipe->buffer_unmap(device->pipe, st);
         if (!src || !dst)
            return vk_error(device, VK_ERROR_OUT_OF_DEVICE_MEMORY);
         mem->memory_offset = pBindInfos[i].memoryOffset;
      } else {
         /* No prior map: keep the resource-backed model unchanged -- the memory
          * borrows the buffer's create-time resource.  Byte-for-byte the path
          * the shipping compute/dispatch (identity-map replay) relies on. */
         mem->memory_offset = pBindInfos[i].memoryOffset;
         pipe_resource_reference(&mem->resource, buf->resource);
      }
   }
   return VK_SUCCESS;
}

VkResult
r300vk_BindImageMemory2(VkDevice _device,
                         uint32_t bindInfoCount,
                         const VkBindImageMemoryInfo *pBindInfos)
{
   (void)_device;
   for (uint32_t i = 0; i < bindInfoCount; i++) {
      VK_FROM_HANDLE(r300vk_image, img, pBindInfos[i].image);
      VK_FROM_HANDLE(r300vk_device_memory, mem, pBindInfos[i].memory);
      mem->memory_offset = pBindInfos[i].memoryOffset;
      pipe_resource_reference(&mem->resource, img->resource);
   }
   return VK_SUCCESS;
}
