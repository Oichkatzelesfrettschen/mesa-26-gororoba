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
      /* Free a still-mapped allocation: unmap against the transfer's own
       * resource, not mem->resource (see r300vk_UnmapMemory) -- a rebind can
       * leave mem->resource a texture while the live transfer is a buffer. */
      if (mem->transfer->resource->target == PIPE_BUFFER)
         device->pipe->buffer_unmap(device->pipe, mem->transfer);
      else
         device->pipe->texture_unmap(device->pipe, mem->transfer);
      mem->transfer = NULL;
   }
   pipe_resource_reference(&mem->mapped_resource, NULL);
   pipe_resource_reference(&mem->resource, NULL);
   pipe_resource_reference(&mem->bound_resource, NULL);

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
   if (!mem->resource) {
      /* Map-before-bind: Vulkan permits mapping HOST_VISIBLE VkDeviceMemory before
       * (or without) binding it to a buffer or image, and deqp's default allocator
       * does exactly this.  The resource-backed model has no storage until a bind,
       * so lazily create the memory's own host-visible pipe_buffer here.  A later
       * BindBufferMemory2 will copy this memory's slice into the VkBuffer's own
       * GPU-private resource (maintaining offset-freedom for replay). */
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
      /* Match the resource the prior transfer was created from, like
       * r300vk_UnmapMemory: keying on mem->resource can mis-route a buffer
       * transfer into texture_unmap after a rebind changed mem->resource. */
      if (mem->transfer->resource->target == PIPE_BUFFER)
         device->pipe->buffer_unmap(device->pipe, mem->transfer);
      else
         device->pipe->texture_unmap(device->pipe, mem->transfer);
      mem->transfer = NULL;
   }

   struct pipe_box box;
   void *ptr;

   if (mem->resource->target == PIPE_BUFFER) {
      /* resource_offset: byte offset into the pipe_resource.  If the memory
       * was mapped BEFORE bind (owns_buffer), mem->resource is the full
       * VkDeviceMemory and we map relative to its start.  Otherwise,
       * mem->resource is borrowed from a bound VkBuffer and we map relative
       * to the buffer's start (offset - memory_offset). */
      unsigned resource_offset;
      if (mem->owns_buffer) {
         resource_offset = (unsigned)offset;
      } else {
         if (offset < mem->memory_offset)
            return vk_error(device, VK_ERROR_MEMORY_MAP_FAILED);
         resource_offset = (unsigned)(offset - mem->memory_offset);
      }
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

   /* Acquire a reference to the resource actually mapped by the transfer,
    * so it stays alive even if mem->resource is rebound. */
   pipe_resource_reference(&mem->mapped_resource, mem->transfer->resource);

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

   /* Unmap against the resource the transfer was created from, not mem->resource.
    * A map-before-bind map lazily creates a PIPE_BUFFER and a buffer transfer; a
    * later BindImageMemory2 replaces mem->resource with the image's tiled
    * texture.  Keying on mem->resource then routes that buffer transfer into
    * texture_unmap, whose r300_transfer cast reads a garbage linear_texture and
    * faults in r300_resource_copy_region.  pipe_transfer->resource is fixed at
    * map time, so it is the correct buffer-vs-texture discriminator. */
   if (mem->transfer->resource->target == PIPE_BUFFER)
      device->pipe->buffer_unmap(device->pipe, mem->transfer);
   else
      device->pipe->texture_unmap(device->pipe, mem->transfer);
   mem->transfer = NULL;
   mem->map_ptr = NULL;
   pipe_resource_reference(&mem->mapped_resource, NULL);
}

/* Sync the live host map and the bound VkBuffer's GPU resource for an
 * owns_buffer (map-before-bind) allocation.  host_to_buffer copies the map's
 * slice at memory_offset into bound_resource (Flush and the bind-time seed);
 * !host_to_buffer copies bound_resource back into the map (Invalidate).  The
 * copy reads/writes the live map_ptr directly, so the map and its transfer stay
 * valid -- the lazy buffer is host-only, so there is no second transfer on it
 * and no concurrent GPU access. */
static VkResult
r300vk_sync_owns_buffer(struct r300vk_device *device,
                        struct r300vk_device_memory *mem,
                        bool host_to_buffer)
{
   if (!mem->owns_buffer || !mem->bound_resource || !mem->map_ptr)
      return VK_SUCCESS;

   /* Range check: verify the bound buffer's slice fits within the
    * VkDeviceMemory allocation before reading/writing hp. */
   if (mem->memory_offset + (VkDeviceSize)mem->bound_resource->width0 > mem->size)
      return vk_error(device, VK_ERROR_OUT_OF_DEVICE_MEMORY);

   unsigned bytes = (unsigned)mem->bound_resource->width0;
   if (!bytes)
      return VK_SUCCESS;

   struct pipe_box box;
   u_box_1d(0, bytes, &box);
   struct pipe_transfer *bt = NULL;
   void *bp = device->pipe->buffer_map(device->pipe, mem->bound_resource, 0,
                                       host_to_buffer ? PIPE_MAP_WRITE : PIPE_MAP_READ,
                                       &box, &bt);
   if (!bp)
      return vk_error(device, VK_ERROR_MEMORY_MAP_FAILED);

   char *hp = (char *)mem->map_ptr + mem->memory_offset;
   if (host_to_buffer)
      memcpy(bp, hp, bytes);
   else
      memcpy(hp, bp, bytes);
   device->pipe->buffer_unmap(device->pipe, bt);
   return VK_SUCCESS;
}

VkResult
r300vk_FlushMappedMemoryRanges(VkDevice _device,
                                uint32_t memoryRangeCount,
                                const VkMappedMemoryRange *pMemoryRanges)
{
   VK_FROM_HANDLE(r300vk_device, device, _device);
   /* Push post-bind host writes (e.g. the deqp vertex memcpy) across to the
    * bound VkBuffer's GPU resource. */
   for (uint32_t i = 0; i < memoryRangeCount; i++) {
      VK_FROM_HANDLE(r300vk_device_memory, mem, pMemoryRanges[i].memory);
      VkResult result = r300vk_sync_owns_buffer(device, mem, true /* host -> buffer */);
      if (result != VK_SUCCESS)
         return result;
   }
   return VK_SUCCESS;
}

VkResult
r300vk_InvalidateMappedMemoryRanges(VkDevice _device,
                                     uint32_t memoryRangeCount,
                                     const VkMappedMemoryRange *pMemoryRanges)
{
   VK_FROM_HANDLE(r300vk_device, device, _device);
   /* Pull GPU writes (e.g. copyImageToBuffer into a readback buffer) back into
    * the live host map before the app reads through the host ptr. */
   for (uint32_t i = 0; i < memoryRangeCount; i++) {
      VK_FROM_HANDLE(r300vk_device_memory, mem, pMemoryRanges[i].memory);
      VkResult result = r300vk_sync_owns_buffer(device, mem, false /* buffer -> host */);
      if (result != VK_SUCCESS)
         return result;
   }
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
         /* Map-before-bind: the memory has its own host pipe_buffer
          * (mem->resource) and a live map.  Vulkan keeps that map valid across
          * this bind.  Seed the VkBuffer's resource with whatever the map
          * already holds (covers a write-BEFORE-bind app).  The slice copy at
          * memory_offset keeps every consumer reading buf->resource from byte 0,
          * so the dispatch path stays offset-free and untouched. */
         if (pBindInfos[i].memoryOffset + buf->size > mem->size)
            return vk_error(device, VK_ERROR_OUT_OF_DEVICE_MEMORY);

         mem->memory_offset = pBindInfos[i].memoryOffset;
         pipe_resource_reference(&mem->bound_resource, buf->resource);
         VkResult result = r300vk_sync_owns_buffer(device, mem, true /* host -> buffer (seed) */);
         if (result != VK_SUCCESS)
            return result;
      } else {
         /* No prior owned map: the memory borrows the buffer's create-time
          * resource.  Byte-for-byte the path the shipping compute/dispatch
          * (identity-map replay) relies on.  If the memory was mapped and is
          * now aliased onto a second buffer, the live transfer stays valid;
          * mapped_resource holds a reference on the old resource until
          * UnmapMemory (preventing use-after-free when mem->resource is
          * overwritten below). */
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
   VK_FROM_HANDLE(r300vk_device, device, _device);
   for (uint32_t i = 0; i < bindInfoCount; i++) {
      VK_FROM_HANDLE(r300vk_image, img, pBindInfos[i].image);
      VK_FROM_HANDLE(r300vk_device_memory, mem, pBindInfos[i].memory);

      if (pBindInfos[i].memoryOffset + r300vk_image_memory_size(img) > mem->size)
         return vk_error(device, VK_ERROR_OUT_OF_DEVICE_MEMORY);

      /* If the memory was mapped before bind, vkMapMemory created its own lazy
       * HOST_VISIBLE pipe_buffer and a transfer that borrows it.  Vulkan
       * permits keeping that map alive across this bind; mapped_resource
       * holds a reference on the lazy buffer so the still-live transfer stays
       * valid even after mem->resource is replaced by the image's tiled
       * texture below. */
      mem->memory_offset = pBindInfos[i].memoryOffset;
      pipe_resource_reference(&mem->resource, img->resource);
      mem->owns_buffer = false;
   }
   return VK_SUCCESS;
}
