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
#include "frontend/winsys_handle.h"
#include "vk_util.h"

#include <string.h>

static VkResult r300vk_sync_owns_buffer(struct r300vk_device *device,
                                        struct r300vk_device_memory *mem,
                                        bool host_to_buffer,
                                        VkDeviceSize range_offset,
                                        VkDeviceSize range_size);

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

   /* Dedicated-image allocations carry the image whose BO vkGetMemoryFdKHR
    * exports (the wsi-drm swapchain pattern: one image, one allocation, one
    * PRIME fd).  The image outlives the allocation per valid usage, so a raw
    * pointer suffices. */
   const VkMemoryDedicatedAllocateInfo *dedicated =
      vk_find_struct_const(pAllocateInfo->pNext, MEMORY_DEDICATED_ALLOCATE_INFO);
   if (dedicated && dedicated->image != VK_NULL_HANDLE) {
      VK_FROM_HANDLE(r300vk_image, dimg, dedicated->image);
      mem->dedicated_image = dimg;
   }

   simple_mtx_lock(&device->memory_list_lock);
   list_addtail(&mem->device_link, &device->memory_list);
   simple_mtx_unlock(&device->memory_list_lock);

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

   simple_mtx_lock(&device->memory_list_lock);
   list_del(&mem->device_link);
   simple_mtx_unlock(&device->memory_list_lock);

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
   pipe_resource_reference(&mem->bound_image_tile, NULL);

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

   /* Promote a borrowed buffer binding to owned host storage when the map
    * range exceeds the bound slice.  Mapping the full VkDeviceMemory is always
    * legal for host-visible memory, and zink's allocator depends on it: it
    * over-allocates memory relative to the buffer (e.g. a 2 MB allocation
    * backing a 1 MB slab buffer bound at offset 0) and maps the whole
    * allocation once.  A map served from the borrowed bound resource can cover
    * only that buffer's bytes, so give the memory its own allocation-sized
    * host buffer, remember the borrowed resource as bound_resource, and seed
    * the host view from it after the map below succeeds.  From then on the
    * allocation behaves exactly like a map-before-bind owns_buffer memory:
    * Flush/Invalidate and the submit-boundary sync keep the two in step. */
   bool promoted = false;
   if (!mem->owns_buffer && mem->resource->target == PIPE_BUFFER) {
      VkDeviceSize slice_end = mem->memory_offset + mem->resource->width0;
      VkDeviceSize req_end = (size == VK_WHOLE_SIZE) ? mem->size : offset + size;
      if (offset < mem->memory_offset || req_end > slice_end) {
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
         struct pipe_resource *owned =
            device->screen->resource_create(device->screen, &tmpl);
         if (!owned)
            return vk_error(device, VK_ERROR_MEMORY_MAP_FAILED);
         pipe_resource_reference(&mem->bound_resource, mem->resource);
         pipe_resource_reference(&mem->resource, NULL);
         mem->resource = owned;
         mem->owns_buffer = true;
         promoted = true;
      }
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

   /* A promotion left the host view zero-filled; pull the bound buffer's
    * current bytes into it so the map shows what the buffer already holds
    * (a bind-time seed in the opposite direction). */
   if (promoted) {
      VkResult seed = r300vk_sync_owns_buffer(device, mem, false /* buffer -> host */,
                                              0, VK_WHOLE_SIZE);
      if (seed != VK_SUCCESS)
         return seed;
   }

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
 * owns_buffer (map-before-bind) allocation.  host_to_buffer copies the map into
 * bound_resource (Flush and the bind-time seed); !host_to_buffer copies
 * bound_resource back into the map (Invalidate).  The copy reads/writes the live
 * map_ptr directly, so the map and its transfer stay valid -- the lazy buffer is
 * host-only, so there is no second transfer on it and no concurrent GPU access.
 *
 * range_offset and range_size name the touched window in VkDeviceMemory space,
 * exactly like VkMappedMemoryRange::offset/size; range_size == VK_WHOLE_SIZE
 * means to the end of the allocation.  Only the part of the bound slice
 * [memory_offset, memory_offset + width0) that overlaps that window is mapped
 * and copied, so a Flush/Invalidate of one sub-range never touches bytes the
 * caller did not name.  An empty intersection copies nothing and succeeds.  The
 * bind-time seed passes the whole allocation (offset 0, VK_WHOLE_SIZE) so the
 * intersection yields the full slice. */
static VkResult
r300vk_sync_owns_buffer(struct r300vk_device *device,
                        struct r300vk_device_memory *mem,
                        bool host_to_buffer,
                        VkDeviceSize range_offset,
                        VkDeviceSize range_size)
{
   if (!mem->owns_buffer || !mem->bound_resource || !mem->map_ptr)
      return VK_SUCCESS;

   /* Range check: verify the bound buffer's slice fits within the
    * VkDeviceMemory allocation before reading/writing hp. */
   if (mem->memory_offset + (VkDeviceSize)mem->bound_resource->width0 > mem->size)
      return vk_error(device, VK_ERROR_OUT_OF_DEVICE_MEMORY);

   /* Intersect the requested window with the bound slice, all in 64-bit memory
    * space.  range_size == VK_WHOLE_SIZE, a range_offset past the allocation, or
    * a range_offset + range_size that overflows or runs past the end all clamp
    * range_end to mem->size; the allocation-bounds check above already proved the
    * slice end fits in mem->size.  Test the addend against the remaining space
    * (mem->size - range_offset) so the sum itself never wraps. */
   VkDeviceSize range_end;
   if (range_size == VK_WHOLE_SIZE || range_offset >= mem->size ||
       range_size > mem->size - range_offset)
      range_end = mem->size;
   else
      range_end = range_offset + range_size;
   VkDeviceSize slice_begin = mem->memory_offset;
   VkDeviceSize slice_end = slice_begin + (VkDeviceSize)mem->bound_resource->width0;
   VkDeviceSize intersect_begin = range_offset > slice_begin ? range_offset : slice_begin;
   VkDeviceSize intersect_end = range_end < slice_end ? range_end : slice_end;
   if (intersect_begin >= intersect_end)
      return VK_SUCCESS;

   unsigned buffer_offset = (unsigned)(intersect_begin - slice_begin);
   unsigned bytes = (unsigned)(intersect_end - intersect_begin);

   struct pipe_box box;
   u_box_1d(buffer_offset, bytes, &box);
   struct pipe_transfer *bt = NULL;
   void *bp = device->pipe->buffer_map(device->pipe, mem->bound_resource, 0,
                                       host_to_buffer ? PIPE_MAP_WRITE : PIPE_MAP_READ,
                                       &box, &bt);
   if (!bp)
      return vk_error(device, VK_ERROR_MEMORY_MAP_FAILED);

   char *hp = (char *)mem->map_ptr + intersect_begin;
   if (host_to_buffer)
      memcpy(bp, hp, bytes);
   else
      memcpy(hp, bp, bytes);
   device->pipe->buffer_unmap(device->pipe, bt);
   return VK_SUCCESS;
}

/* Pull a map-before-bind linear image's tile into the live host map.  The
 * companion to r300vk_sync_owns_buffer for the image case: an owns_buffer
 * allocation later bound to a linear image keeps its host map on the lazy
 * buffer, while the image's pixels live in a separate row-major r300g tile that
 * a GPU copy fills.  The tile stride equals bound_image_row_pitch, which is the
 * pitch GetImageSubresourceLayout reported and r300vk_image_memory_size sized,
 * so copy every tile row into the map at memory_offset.  Only image -> host
 * (Invalidate) is implemented; a host -> image upload path is not yet exercised
 * and is intentionally absent rather than shipped untested. */
static VkResult
r300vk_sync_owns_image(struct r300vk_device *device,
                       struct r300vk_device_memory *mem)
{
   if (!mem->owns_buffer || !mem->bound_image_tile ||
       mem->bound_image_row_pitch == 0 || !mem->map_ptr)
      return VK_SUCCESS;

   struct pipe_resource *tile = mem->bound_image_tile;
   const unsigned pitch = mem->bound_image_row_pitch;
   const unsigned height = tile->height0;

   if (mem->memory_offset + (VkDeviceSize)pitch * height > mem->size)
      return vk_error(device, VK_ERROR_OUT_OF_DEVICE_MEMORY);

   struct pipe_box box;
   u_box_origin_2d(tile->width0, height, &box);
   struct pipe_transfer *xfer = NULL;
   const uint8_t *src = device->pipe->texture_map(device->pipe, tile, 0,
                                                  PIPE_MAP_READ, &box, &xfer);
   if (!src)
      return vk_error(device, VK_ERROR_MEMORY_MAP_FAILED);

   uint8_t *dst = (uint8_t *)mem->map_ptr + mem->memory_offset;
   const unsigned row_bytes =
      pitch < (unsigned)xfer->stride ? pitch : (unsigned)xfer->stride;
   for (unsigned row = 0; row < height; row++)
      memcpy(dst + (size_t)row * pitch, src + (size_t)row * xfer->stride, row_bytes);

   device->pipe->texture_unmap(device->pipe, xfer);
   return VK_SUCCESS;
}

void
r300vk_device_memory_sync_bound(struct r300vk_device *device,
                                bool host_to_buffer)
{
   simple_mtx_lock(&device->memory_list_lock);
   list_for_each_entry(struct r300vk_device_memory, mem,
                       &device->memory_list, device_link) {
      if (!mem->owns_buffer || !mem->map_ptr)
         continue;
      if (mem->bound_resource)
         r300vk_sync_owns_buffer(device, mem, host_to_buffer, 0, VK_WHOLE_SIZE);
      /* Image tiles have only the image -> host direction (the host -> image
       * upload path is intentionally absent until exercised); pull them on the
       * submit-exit sync so a rendered linear image reaches the live map. */
      if (!host_to_buffer && mem->bound_image_tile)
         r300vk_sync_owns_image(device, mem);
   }
   simple_mtx_unlock(&device->memory_list_lock);
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
      VkResult result = r300vk_sync_owns_buffer(device, mem, true /* host -> buffer */,
                                                pMemoryRanges[i].offset,
                                                pMemoryRanges[i].size);
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
      VkResult result = r300vk_sync_owns_buffer(device, mem, false /* buffer -> host */,
                                                pMemoryRanges[i].offset,
                                                pMemoryRanges[i].size);
      if (result != VK_SUCCESS)
         return result;
      /* A linear image bound to an owns_buffer allocation needs the image -> host
       * pull instead; one of the two syncs is a no-op for any given binding. */
      result = r300vk_sync_owns_image(device, mem);
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
         VkResult result = r300vk_sync_owns_buffer(device, mem, true /* host -> buffer (seed) */,
                                                   0, VK_WHOLE_SIZE);
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

      mem->memory_offset = pBindInfos[i].memoryOffset;

      if (mem->owns_buffer && img->vk.tiling == VK_IMAGE_TILING_LINEAR) {
         /* Map-before-bind linear image: vkMapMemory already created the
          * memory's own host pipe_buffer and a live map (the deqp default
          * allocator maps before bind).  The image's pixels live in its own
          * r300g tile, written by a GPU copy, so the host map and the tile are
          * separate storage -- mirror the owns_buffer VkBuffer path: keep the
          * live map, record the tile, and re-sync at Invalidate.  HOST_COHERENT
          * memory means the app may not call Invalidate, but deqp's image
          * readback does (verified on RS482), and that is the only point a
          * stale host map can be refreshed from a row-major tile. */
         pipe_resource_reference(&mem->bound_image_tile, img->resource);
         mem->bound_image_row_pitch = img->linear_row_pitch;
      } else {
         /* Map-after-bind (or optimal tiling): borrow the image's tiled
          * resource as mem->resource so a later vkMapMemory maps the texture
          * directly through the texture branch. */
         pipe_resource_reference(&mem->resource, img->resource);
         mem->owns_buffer = false;
      }
   }
   return VK_SUCCESS;
}

VkResult
r300vk_GetMemoryFdKHR(VkDevice _device,
                      const VkMemoryGetFdInfoKHR *pGetFdInfo,
                      int *pFd)
{
   VK_FROM_HANDLE(r300vk_device, device, _device);
   VK_FROM_HANDLE(r300vk_device_memory, mem, pGetFdInfo->memory);

   if (pGetFdInfo->handleType != VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT &&
       pGetFdInfo->handleType != VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT)
      return vk_error(device, VK_ERROR_INVALID_EXTERNAL_HANDLE);

   /* Export the dedicated image's BO through the winsys PRIME path -- the
    * same drmPrimeHandleToFD route r300g/GL presents through (the DRI3
    * oracle: two such exports per swapchain, then only CS per frame). */
   if (!mem->dedicated_image || !mem->dedicated_image->resource)
      return vk_error(device, VK_ERROR_OUT_OF_DEVICE_MEMORY);

   struct winsys_handle wh = { .type = WINSYS_HANDLE_TYPE_FD };
   if (!device->screen->resource_get_handle(device->screen, device->pipe,
                                            mem->dedicated_image->resource,
                                            &wh,
                                            PIPE_HANDLE_USAGE_FRAMEBUFFER_WRITE))
      return vk_error(device, VK_ERROR_OUT_OF_DEVICE_MEMORY);

   *pFd = (int)wh.handle;
   return VK_SUCCESS;
}

VkResult
r300vk_GetMemoryFdPropertiesKHR(VkDevice _device,
                                VkExternalMemoryHandleTypeFlagBits handleType,
                                int fd,
                                VkMemoryFdPropertiesKHR *pMemoryFdProperties)
{
   /* Both advertised memory types back an imported dma-buf on the UMA pool. */
   pMemoryFdProperties->memoryTypeBits = 0x3;
   return VK_SUCCESS;
}
