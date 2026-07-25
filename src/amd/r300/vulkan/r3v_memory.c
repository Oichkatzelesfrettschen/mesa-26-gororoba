/*
 * SPDX-License-Identifier: MIT
 */

#include "r3v_memory.h"
#include "r3v_buffer.h"
#include "r3v_image.h"
#include "r3v_device.h"

#include "vk_alloc.h"
#include "vk_log.h"
#include "vk_object.h"

#include "pipe/p_context.h"
#include "pipe/p_defines.h"
#include "pipe/p_state.h"
#include "util/u_inlines.h"
#include "util/u_math.h"
#include "util/os_misc.h"
#include "frontend/winsys_handle.h"
#include "vk_util.h"

#include <limits.h>
#include <string.h>

static VkResult r3v_sync_owns_buffer(struct r3v_device *device,
                                        struct r3v_device_memory *mem,
                                        bool host_to_buffer,
                                        VkDeviceSize range_offset,
                                        VkDeviceSize range_size);

static VkDeviceSize
r3v_min_memory_offset(VkDeviceSize a, VkDeviceSize b)
{
   return a < b ? a : b;
}

static VkDeviceSize
r3v_max_memory_offset(VkDeviceSize a, VkDeviceSize b)
{
   return a > b ? a : b;
}

static VkDeviceSize
r3v_memory_range_end(const struct r3v_device_memory *mem,
                        VkDeviceSize range_offset,
                        VkDeviceSize range_size)
{
   if (range_size == VK_WHOLE_SIZE || range_offset >= mem->size ||
       range_size > mem->size - range_offset)
      return mem->size;

   return range_offset + range_size;
}

static void
r3v_clear_bound_image(struct r3v_device_memory *mem)
{
   pipe_resource_reference(&mem->bound_image_tile, NULL);
   mem->bound_image_offset = 0;
   mem->bound_image_size = 0;
   mem->bound_image_row_pitch = 0;
}

VkResult
r3v_AllocateMemory(VkDevice _device,
                      const VkMemoryAllocateInfo *pAllocateInfo,
                      const VkAllocationCallbacks *pAllocator,
                      VkDeviceMemory *pMemory)
{
   VK_FROM_HANDLE(r3v_device, device, _device);
   struct r3v_device_memory *mem;

   mem = vk_zalloc2(&device->vk.alloc, pAllocator,
                    sizeof(*mem), 8,
                    VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (!mem)
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);

   vk_object_base_init(&device->vk, &mem->base,
                       VK_OBJECT_TYPE_DEVICE_MEMORY);
   mem->size = pAllocateInfo->allocationSize;
   util_dynarray_init(&mem->bound_buffers, NULL);

   /* Dedicated-image allocations carry the image whose BO vkGetMemoryFdKHR
    * exports (the wsi-drm swapchain pattern: one image, one allocation, one
    * PRIME fd).  The image outlives the allocation per valid usage, so a raw
    * pointer suffices. */
   const VkMemoryDedicatedAllocateInfo *dedicated =
      vk_find_struct_const(pAllocateInfo->pNext, MEMORY_DEDICATED_ALLOCATE_INFO);
   if (dedicated && dedicated->image != VK_NULL_HANDLE) {
      VK_FROM_HANDLE(r3v_image, dimg, dedicated->image);
      mem->dedicated_image = dimg;
   }

   simple_mtx_lock(&device->memory_list_lock);
   list_addtail(&mem->device_link, &device->memory_list);
   simple_mtx_unlock(&device->memory_list_lock);

   *pMemory = r3v_device_memory_to_handle(mem);
   return VK_SUCCESS;
}

void
r3v_FreeMemory(VkDevice _device,
                  VkDeviceMemory _memory,
                  const VkAllocationCallbacks *pAllocator)
{
   VK_FROM_HANDLE(r3v_device, device, _device);
   VK_FROM_HANDLE(r3v_device_memory, mem, _memory);
   if (!mem)
      return;

   simple_mtx_lock(&device->memory_list_lock);
   list_del(&mem->device_link);
   simple_mtx_unlock(&device->memory_list_lock);

   if (mem->transfer) {
      /* Free a still-mapped allocation: unmap against the transfer's own
       * resource, not mem->resource (see r3v_UnmapMemory) -- a rebind can
       * leave mem->resource a texture while the live transfer is a buffer. */
      if (mem->transfer->resource->target == PIPE_BUFFER)
         device->pipe->buffer_unmap(device->pipe, mem->transfer);
      else
         device->pipe->texture_unmap(device->pipe, mem->transfer);
      mem->transfer = NULL;
   }
   pipe_resource_reference(&mem->mapped_resource, NULL);
   pipe_resource_reference(&mem->resource, NULL);
   util_dynarray_foreach(&mem->bound_buffers,
                         struct r3v_bound_buffer_slice, slice)
      pipe_resource_reference(&slice->resource, NULL);
   util_dynarray_fini(&mem->bound_buffers);
   pipe_resource_reference(&mem->bound_image_tile, NULL);

   vk_object_base_finish(&mem->base);
   vk_free2(&device->vk.alloc, pAllocator, mem);
}

void
r3v_device_memory_drop_buffer_slices(struct r3v_device *device,
                                        struct pipe_resource *resource)
{
   if (!resource)
      return;
   simple_mtx_lock(&device->memory_list_lock);
   list_for_each_entry(struct r3v_device_memory, mem,
                       &device->memory_list, device_link) {
      unsigned kept = 0;
      util_dynarray_foreach(&mem->bound_buffers,
                            struct r3v_bound_buffer_slice, slice) {
         if (slice->resource == resource) {
            pipe_resource_reference(&slice->resource, NULL);
         } else {
            *util_dynarray_element(&mem->bound_buffers,
                                   struct r3v_bound_buffer_slice, kept) =
               *slice;
            kept++;
         }
      }
      mem->bound_buffers.size =
         kept * sizeof(struct r3v_bound_buffer_slice);
   }
   simple_mtx_unlock(&device->memory_list_lock);
}

static VkResult
r3v_memory_get_map_range_size(struct r3v_device *device,
                                 struct r3v_device_memory *mem,
                                 VkDeviceSize offset,
                                 VkDeviceSize size,
                                 VkDeviceSize *map_range_size)
{
   *map_range_size = 0;

   if (offset > mem->size)
      return vk_error(device, VK_ERROR_MEMORY_MAP_FAILED);

   *map_range_size = size == VK_WHOLE_SIZE ? mem->size - offset : size;
   if (*map_range_size > mem->size - offset)
      return vk_error(device, VK_ERROR_MEMORY_MAP_FAILED);

   return VK_SUCCESS;
}

static VkResult
r3v_memory_create_owned_map_buffer(struct r3v_device *device,
                                      struct r3v_device_memory *mem)
{
   if (mem->resource)
      return VK_SUCCESS;

   /* Map-before-bind: Vulkan permits mapping HOST_VISIBLE VkDeviceMemory before
    * a buffer or image bind.  The resource-backed model has no storage until a
    * bind, so create an allocation-sized host pipe_buffer and let the later bind
    * seed the real GPU resource from the live map. */
   if (mem->size > UINT_MAX)
      return vk_error(device, VK_ERROR_MEMORY_MAP_FAILED);

   struct pipe_resource tmpl = {
      .target = PIPE_BUFFER,
      .format = PIPE_FORMAT_R8_UNORM,
      .bind = PIPE_BIND_VERTEX_BUFFER,
      .usage = PIPE_USAGE_DYNAMIC,
      .width0 = (unsigned)mem->size,
      .height0 = 1,
      .depth0 = 1,
      .array_size = 1,
   };
   mem->resource = device->screen->resource_create(device->screen, &tmpl);
   if (!mem->resource)
      return vk_error(device, VK_ERROR_MEMORY_MAP_FAILED);

   mem->owns_buffer = true;
   return VK_SUCCESS;
}

static VkResult
r3v_memory_promote_borrowed_buffer(struct r3v_device *device,
                                      struct r3v_device_memory *mem,
                                      VkDeviceSize offset,
                                      VkDeviceSize map_range_size,
                                      bool *promoted)
{
   *promoted = false;
   if (mem->owns_buffer || mem->resource->target != PIPE_BUFFER)
      return VK_SUCCESS;

   if (mem->size > UINT_MAX || mem->memory_offset > mem->size ||
       (VkDeviceSize)mem->resource->width0 > mem->size - mem->memory_offset)
      return vk_error(device, VK_ERROR_MEMORY_MAP_FAILED);

   VkDeviceSize slice_end = mem->memory_offset + mem->resource->width0;
   VkDeviceSize req_end = offset + map_range_size;
   if (offset >= mem->memory_offset && req_end <= slice_end)
      return VK_SUCCESS;

   /* A borrowed buffer binding can cover only that buffer's bytes.  Promote to
    * allocation-sized host storage when the map range crosses the bound slice,
    * then keep the borrowed buffer syncing through bound_buffers like a
    * map-before-bind allocation. */
   struct pipe_resource tmpl = {
      .target = PIPE_BUFFER,
      .format = PIPE_FORMAT_R8_UNORM,
      .bind = PIPE_BIND_VERTEX_BUFFER,
      .usage = PIPE_USAGE_DYNAMIC,
      .width0 = (unsigned)mem->size,
      .height0 = 1,
      .depth0 = 1,
      .array_size = 1,
   };
   struct pipe_resource *owned =
      device->screen->resource_create(device->screen, &tmpl);
   if (!owned)
      return vk_error(device, VK_ERROR_MEMORY_MAP_FAILED);

   struct r3v_bound_buffer_slice slice = {
      .resource = NULL,
      .offset = mem->memory_offset,
   };
   pipe_resource_reference(&slice.resource, mem->resource);
   simple_mtx_lock(&device->memory_list_lock);
   util_dynarray_append_typed(&mem->bound_buffers,
                              struct r3v_bound_buffer_slice, slice);
   simple_mtx_unlock(&device->memory_list_lock);

   pipe_resource_reference(&mem->resource, NULL);
   mem->resource = owned;
   mem->owns_buffer = true;
   *promoted = true;
   return VK_SUCCESS;
}

static void
r3v_memory_unmap_live_transfer(struct r3v_device *device,
                                  struct r3v_device_memory *mem)
{
   if (!mem->transfer)
      return;

   /* Match the resource the transfer was created from.  A rebind can change
    * mem->resource from buffer to texture while the live transfer still names
    * the old buffer resource. */
   if (mem->transfer->resource->target == PIPE_BUFFER)
      device->pipe->buffer_unmap(device->pipe, mem->transfer);
   else
      device->pipe->texture_unmap(device->pipe, mem->transfer);

   mem->transfer = NULL;
   mem->map_ptr = NULL;
   mem->map_offset = 0;
   mem->map_size = 0;
   pipe_resource_reference(&mem->mapped_resource, NULL);
}

static VkResult
r3v_memory_map_buffer_resource(struct r3v_device *device,
                                  struct r3v_device_memory *mem,
                                  VkDeviceSize offset,
                                  VkDeviceSize map_range_size,
                                  void **ptr)
{
   /* If owns_buffer is true, mem->resource covers the whole VkDeviceMemory.
    * Otherwise the borrowed pipe_buffer starts at the bound buffer's byte zero,
    * so vkMapMemory.offset is relative to memory_offset. */
   VkDeviceSize resource_offset_bytes;
   if (mem->owns_buffer) {
      resource_offset_bytes = offset;
   } else {
      if (offset < mem->memory_offset)
         return vk_error(device, VK_ERROR_MEMORY_MAP_FAILED);
      resource_offset_bytes = offset - mem->memory_offset;
   }

   if (resource_offset_bytes > UINT_MAX || map_range_size > UINT_MAX)
      return vk_error(device, VK_ERROR_MEMORY_MAP_FAILED);
   unsigned resource_offset = (unsigned)resource_offset_bytes;
   unsigned map_size = (unsigned)map_range_size;
   if (resource_offset > mem->resource->width0 ||
       map_size > mem->resource->width0 - resource_offset)
      return vk_error(device, VK_ERROR_MEMORY_MAP_FAILED);

   struct pipe_box box;
   u_box_1d(resource_offset, map_size, &box);
   *ptr = device->pipe->buffer_map(device->pipe, mem->resource, 0,
                                   PIPE_MAP_READ_WRITE, &box, &mem->transfer);
   return *ptr ? VK_SUCCESS : vk_error(device, VK_ERROR_MEMORY_MAP_FAILED);
}

static VkResult
r3v_memory_map_texture_resource(struct r3v_device *device,
                                   struct r3v_device_memory *mem,
                                   VkDeviceSize offset,
                                   VkDeviceSize map_range_size,
                                   void **ptr)
{
   /* Texture resources map as full mip-level surfaces.  The only valid Vulkan
    * byte offset is the image's bind offset; sub-ranges that need byte-granular
    * access must use a buffer resource instead.  The map range may exceed the
    * image's tight byte span up to the page-rounded capacity of the backing
    * BO: radeon_winsys_bo_create aligns every allocation to gart_page_size,
    * which the winsys itself derives from os_get_page_size because TTM rounds
    * BO sizes to the CPU page.  Bytes between the image span and the page
    * boundary are allocated storage, so a host map covering them cannot fault;
    * the whole range stays clamped to the VkDeviceMemory allocation size. */
   if (offset < mem->bound_image_offset)
      return vk_error(device, VK_ERROR_MEMORY_MAP_FAILED);

   uint64_t page_size = 0;
   if (!os_get_page_size(&page_size))
      return vk_error(device, VK_ERROR_MEMORY_MAP_FAILED);

   const VkDeviceSize mappable_size =
      MIN2(mem->size, align64(mem->bound_image_size, page_size));
   const VkDeviceSize image_offset = offset - mem->bound_image_offset;
   if (image_offset != 0 || map_range_size > mappable_size)
      return vk_error(device, VK_ERROR_MEMORY_MAP_FAILED);

   struct pipe_box box;
   u_box_origin_2d(mem->resource->width0, mem->resource->height0, &box);
   *ptr = device->pipe->texture_map(device->pipe, mem->resource, 0,
                                    PIPE_MAP_READ_WRITE, &box, &mem->transfer);
   return *ptr ? VK_SUCCESS : vk_error(device, VK_ERROR_MEMORY_MAP_FAILED);
}

static VkResult
r3v_memory_map_resource(struct r3v_device *device,
                           struct r3v_device_memory *mem,
                           VkDeviceSize offset,
                           VkDeviceSize map_range_size,
                           void **ptr)
{
   if (mem->resource->target == PIPE_BUFFER)
      return r3v_memory_map_buffer_resource(device, mem, offset,
                                               map_range_size, ptr);

   return r3v_memory_map_texture_resource(device, mem, offset,
                                             map_range_size, ptr);
}

VkResult
r3v_MapMemory(VkDevice _device,
                 VkDeviceMemory _memory,
                 VkDeviceSize offset,
                 VkDeviceSize size,
                 VkMemoryMapFlags flags,
                 void **ppData)
{
   VK_FROM_HANDLE(r3v_device, device, _device);
   VK_FROM_HANDLE(r3v_device_memory, mem, _memory);

   VkDeviceSize map_range_size;
   VkResult result = r3v_memory_get_map_range_size(device, mem, offset,
                                                      size, &map_range_size);
   if (result != VK_SUCCESS)
      return result;

   result = r3v_memory_create_owned_map_buffer(device, mem);
   if (result != VK_SUCCESS)
      return result;

   bool promoted = false;
   result = r3v_memory_promote_borrowed_buffer(device, mem, offset,
                                                  map_range_size, &promoted);
   if (result != VK_SUCCESS)
      return result;

   /* Unmap any stale mapping from a previous vkMapMemory call.  Vulkan
    * spec forbids double-mapping without an intervening vkUnmapMemory,
    * but guard here to avoid leaking the old pipe_transfer. */
   r3v_memory_unmap_live_transfer(device, mem);

   void *ptr;
   result = r3v_memory_map_resource(device, mem, offset, map_range_size, &ptr);
   if (result != VK_SUCCESS)
      return result;

   /* Acquire a reference to the resource actually mapped by the transfer,
    * so it stays alive even if mem->resource is rebound. */
   pipe_resource_reference(&mem->mapped_resource, mem->transfer->resource);

   mem->map_ptr = ptr;
   mem->map_offset = offset;
   mem->map_size = map_range_size;

   /* A promotion left the host view zero-filled; pull the bound buffer's
    * current bytes into it so the map shows what the buffer already holds
    * (a bind-time seed in the opposite direction). */
   if (promoted) {
      VkResult seed = r3v_sync_owns_buffer(device, mem, false /* buffer -> host */,
                                              0, VK_WHOLE_SIZE);
      if (seed != VK_SUCCESS)
         return seed;
   }

   if (device->dbg_log_draws)
      fprintf(stderr, "r3v map: mem=%p off=%llu size=%llu owns=%d prom=%d "
              "res=%p slices=%u moff=%llu\n",
              (void *)mem, (unsigned long long)mem->map_offset,
              (unsigned long long)mem->map_size, mem->owns_buffer, promoted,
              (void *)mem->resource,
              (unsigned)util_dynarray_num_elements(
                 &mem->bound_buffers, struct r3v_bound_buffer_slice),
              (unsigned long long)mem->memory_offset);

   *ppData = ptr;
   return VK_SUCCESS;
}

void
r3v_UnmapMemory(VkDevice _device,
                   VkDeviceMemory _memory)
{
   VK_FROM_HANDLE(r3v_device, device, _device);
   VK_FROM_HANDLE(r3v_device_memory, mem, _memory);

   if (device->dbg_log_draws)
      fprintf(stderr, "r3v unmap: mem=%p owns=%d res=%p slices=%u\n",
              (void *)mem, mem->owns_buffer, (void *)mem->resource,
              (unsigned)util_dynarray_num_elements(
                 &mem->bound_buffers, struct r3v_bound_buffer_slice));

   /* Flush the host map into every bound buffer before tearing the map down.  An
    * owns_buffer (map-before-bind, or a memory promoted on aliasing) holds the
    * app's writes only in mem->resource's map; the submit-boundary host->buffer
    * sync skips a memory whose map_ptr is already NULL, so an app that writes then
    * unmaps before queue submit would never propagate those writes to the bound
    * VkBuffers and the deferred draw would read the stale per-buffer BO. */
   if (mem->owns_buffer && mem->map_ptr)
      r3v_sync_owns_buffer(device, mem, true /* host -> buffer */, 0,
                              VK_WHOLE_SIZE);

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
   mem->map_offset = 0;
   mem->map_size = 0;
   pipe_resource_reference(&mem->mapped_resource, NULL);
}

/* Sync the live host map with one bound VkBuffer's GPU resource for an
 * owns_buffer (map-before-bind) allocation.  host_to_buffer copies the map into
 * the slice's resource (Flush and the bind-time seed); !host_to_buffer copies
 * the resource back into the map (Invalidate).  The copy reads/writes the live
 * map_ptr directly, so the map and its transfer stay valid -- the lazy buffer is
 * host-only, so there is no second transfer on it and no concurrent GPU access.
 *
 * range_offset and range_size name the touched window in VkDeviceMemory space,
 * exactly like VkMappedMemoryRange::offset/size; range_size == VK_WHOLE_SIZE
 * means to the end of the allocation.  Only the intersection of the requested
 * window, the bound slice, and the live map is copied, so the host pointer is
 * addressed relative to the vkMapMemory offset instead of the allocation base.
 * An empty intersection copies nothing and succeeds. */
static VkResult
r3v_sync_one_buffer_slice(struct r3v_device *device,
                             struct r3v_device_memory *mem,
                             const struct r3v_bound_buffer_slice *slice,
                             bool host_to_buffer,
                             VkDeviceSize range_offset,
                             VkDeviceSize range_size)
{
   if (!mem->map_ptr || !slice->resource)
      return VK_SUCCESS;

   /* Range check: verify the bound buffer's slice fits within the
    * VkDeviceMemory allocation before reading/writing hp. */
   if (slice->offset > mem->size ||
       (VkDeviceSize)slice->resource->width0 > mem->size - slice->offset)
      return vk_error(device, VK_ERROR_OUT_OF_DEVICE_MEMORY);

   VkDeviceSize range_end = r3v_memory_range_end(mem, range_offset, range_size);
   VkDeviceSize slice_begin = slice->offset;
   VkDeviceSize slice_end = slice_begin + (VkDeviceSize)slice->resource->width0;
   VkDeviceSize map_begin = mem->map_offset;
   VkDeviceSize map_end = mem->map_offset + mem->map_size;
   VkDeviceSize intersect_begin =
      r3v_max_memory_offset(r3v_max_memory_offset(range_offset, slice_begin),
                               map_begin);
   VkDeviceSize intersect_end =
      r3v_min_memory_offset(r3v_min_memory_offset(range_end, slice_end),
                               map_end);
   if (intersect_begin >= intersect_end)
      return VK_SUCCESS;

   unsigned buffer_offset = (unsigned)(intersect_begin - slice_begin);
   unsigned bytes = (unsigned)(intersect_end - intersect_begin);

   struct pipe_box box;
   u_box_1d(buffer_offset, bytes, &box);
   struct pipe_transfer *bt = NULL;
   void *bp = device->pipe->buffer_map(device->pipe, slice->resource, 0,
                                       host_to_buffer ? PIPE_MAP_WRITE : PIPE_MAP_READ,
                                       &box, &bt);
   if (!bp)
      return vk_error(device, VK_ERROR_MEMORY_MAP_FAILED);

   char *hp = (char *)mem->map_ptr + (intersect_begin - map_begin);
   if (host_to_buffer)
      memcpy(bp, hp, bytes);
   else
      memcpy(hp, bp, bytes);
   device->pipe->buffer_unmap(device->pipe, bt);
   return VK_SUCCESS;
}

/* Sync the requested window against every bound buffer slice.  A suballocating
 * client binds several VkBuffers at distinct offsets in one allocation; each
 * keeps its own GPU resource, so each slice that intersects the window gets its
 * own copy.  Slices never overlap under suballocation; an app that deliberately
 * aliases overlapping buffer binds gets whichever slice syncs last, matching
 * the no-stronger guarantee aliased memory has under Vulkan without barriers. */
static VkResult
r3v_sync_owns_buffer(struct r3v_device *device,
                        struct r3v_device_memory *mem,
                        bool host_to_buffer,
                        VkDeviceSize range_offset,
                        VkDeviceSize range_size)
{
   if (!mem->owns_buffer || !mem->map_ptr)
      return VK_SUCCESS;

   util_dynarray_foreach(&mem->bound_buffers,
                         struct r3v_bound_buffer_slice, slice) {
      VkResult result = r3v_sync_one_buffer_slice(device, mem, slice,
                                                     host_to_buffer,
                                                     range_offset, range_size);
      if (result != VK_SUCCESS)
         return result;
   }
   return VK_SUCCESS;
}

/* Pull a map-before-bind linear image's tile into the live host map.  The
 * companion to r3v_sync_owns_buffer for the image case: an owns_buffer
 * allocation later bound to a linear image keeps its host map on the lazy
 * buffer, while the image's pixels live in a separate row-major r300g tile that
 * a GPU copy fills.  Copy only the intersection of the requested memory range,
 * the bound image slice, and the live map.  map_ptr is the start of the
 * vkMapMemory window, so the host copy address is relative to map_offset, not
 * VkDeviceMemory byte zero.  Only image -> host sync is implemented; a host ->
 * image upload path is intentionally absent until a caller exercises it. */
static VkResult
r3v_sync_owns_image(struct r3v_device *device,
                       struct r3v_device_memory *mem,
                       VkDeviceSize range_offset,
                       VkDeviceSize range_size)
{
   if (!mem->owns_buffer || !mem->bound_image_tile ||
       mem->bound_image_row_pitch == 0 || !mem->map_ptr)
      return VK_SUCCESS;

   struct pipe_resource *tile = mem->bound_image_tile;
   const unsigned pitch = mem->bound_image_row_pitch;
   const unsigned height = tile->height0;
   const VkDeviceSize image_bytes = (VkDeviceSize)pitch * height;

   if (height != 0 && image_bytes / height != pitch)
      return vk_error(device, VK_ERROR_OUT_OF_DEVICE_MEMORY);
   if (image_bytes > mem->bound_image_size ||
       mem->bound_image_offset > mem->size ||
       image_bytes > mem->size - mem->bound_image_offset)
      return vk_error(device, VK_ERROR_OUT_OF_DEVICE_MEMORY);

   const VkDeviceSize image_begin = mem->bound_image_offset;
   const VkDeviceSize image_end = image_begin + image_bytes;
   const VkDeviceSize map_begin = mem->map_offset;
   const VkDeviceSize map_end = mem->map_offset + mem->map_size;
   const VkDeviceSize range_end =
      r3v_memory_range_end(mem, range_offset, range_size);
   const VkDeviceSize copy_begin =
      r3v_max_memory_offset(r3v_max_memory_offset(range_offset, image_begin),
                               map_begin);
   const VkDeviceSize copy_end =
      r3v_min_memory_offset(r3v_min_memory_offset(range_end, image_end),
                               map_end);

   if (copy_begin >= copy_end)
      return VK_SUCCESS;

   struct pipe_box box;
   u_box_origin_2d(tile->width0, height, &box);
   struct pipe_transfer *xfer = NULL;
   const uint8_t *src = device->pipe->texture_map(device->pipe, tile, 0,
                                                  PIPE_MAP_READ, &box, &xfer);
   if (!src)
      return vk_error(device, VK_ERROR_MEMORY_MAP_FAILED);

   const unsigned row_bytes =
      pitch < (unsigned)xfer->stride ? pitch : (unsigned)xfer->stride;
   const size_t src_stride = (size_t)xfer->stride;
   uint8_t *dst = mem->map_ptr;
   for (unsigned row = 0; row < height; row++) {
      const VkDeviceSize row_begin = image_begin + (VkDeviceSize)row * pitch;
      const VkDeviceSize row_end = row_begin + row_bytes;
      const VkDeviceSize row_copy_begin =
         r3v_max_memory_offset(copy_begin, row_begin);
      const VkDeviceSize row_copy_end =
         r3v_min_memory_offset(copy_end, row_end);

      if (row_copy_begin >= row_copy_end)
         continue;

      const size_t row_src_offset = (size_t)(row_copy_begin - row_begin);
      const size_t row_dst_offset = (size_t)(row_copy_begin - map_begin);
      const size_t row_copy_bytes = (size_t)(row_copy_end - row_copy_begin);

      memcpy(dst + row_dst_offset,
             src + (size_t)row * src_stride + row_src_offset,
             row_copy_bytes);
   }

   device->pipe->texture_unmap(device->pipe, xfer);
   return VK_SUCCESS;
}

void
r3v_device_memory_sync_bound(struct r3v_device *device,
                                bool host_to_buffer)
{
   simple_mtx_lock(&device->memory_list_lock);
   list_for_each_entry(struct r3v_device_memory, mem,
                       &device->memory_list, device_link) {
      if (!mem->owns_buffer || !mem->map_ptr)
         continue;
      if (device->dbg_log_draws) {
         util_dynarray_foreach(&mem->bound_buffers,
                               struct r3v_bound_buffer_slice, slice)
            fprintf(stderr, "r3v sync: mem=%p dir=%s bound=%p moff=%llu "
                    "w0=%u\n",
                    (void *)mem, host_to_buffer ? "h2b" : "b2h",
                    (void *)slice->resource,
                    (unsigned long long)slice->offset,
                    slice->resource ? slice->resource->width0 : 0);
      }
      r3v_sync_owns_buffer(device, mem, host_to_buffer, 0, VK_WHOLE_SIZE);
      /* Image tiles have only the image -> host direction (the host -> image
       * upload path is intentionally absent until exercised); pull them on the
       * submit-exit sync so a rendered linear image reaches the live map. */
      if (!host_to_buffer && mem->bound_image_tile)
         r3v_sync_owns_image(device, mem, 0, VK_WHOLE_SIZE);
   }
   simple_mtx_unlock(&device->memory_list_lock);
}

VkResult
r3v_FlushMappedMemoryRanges(VkDevice _device,
                                uint32_t memoryRangeCount,
                                const VkMappedMemoryRange *pMemoryRanges)
{
   VK_FROM_HANDLE(r3v_device, device, _device);
   /* Push post-bind host writes (e.g. the deqp vertex memcpy) across to the
    * bound VkBuffer's GPU resource. */
   for (uint32_t i = 0; i < memoryRangeCount; i++) {
      VK_FROM_HANDLE(r3v_device_memory, mem, pMemoryRanges[i].memory);
      VkResult result = r3v_sync_owns_buffer(device, mem, true /* host -> buffer */,
                                                pMemoryRanges[i].offset,
                                                pMemoryRanges[i].size);
      if (result != VK_SUCCESS)
         return result;
   }
   return VK_SUCCESS;
}

VkResult
r3v_InvalidateMappedMemoryRanges(VkDevice _device,
                                     uint32_t memoryRangeCount,
                                     const VkMappedMemoryRange *pMemoryRanges)
{
   VK_FROM_HANDLE(r3v_device, device, _device);
   /* Pull GPU writes (e.g. copyImageToBuffer into a readback buffer) back into
    * the live host map before the app reads through the host ptr. */
   for (uint32_t i = 0; i < memoryRangeCount; i++) {
      VK_FROM_HANDLE(r3v_device_memory, mem, pMemoryRanges[i].memory);
      VkResult result = r3v_sync_owns_buffer(device, mem, false /* buffer -> host */,
                                                pMemoryRanges[i].offset,
                                                pMemoryRanges[i].size);
      if (result != VK_SUCCESS)
         return result;
      /* A linear image bound to an owns_buffer allocation needs the image -> host
       * pull instead; one of the two syncs is a no-op for any given binding. */
      result = r3v_sync_owns_image(device, mem,
                                      pMemoryRanges[i].offset,
                                      pMemoryRanges[i].size);
      if (result != VK_SUCCESS)
         return result;
   }
   return VK_SUCCESS;
}

VkResult
r3v_BindBufferMemory2(VkDevice _device,
                          uint32_t bindInfoCount,
                          const VkBindBufferMemoryInfo *pBindInfos)
{
   VK_FROM_HANDLE(r3v_device, device, _device);
   for (uint32_t i = 0; i < bindInfoCount; i++) {
      VK_FROM_HANDLE(r3v_buffer, buf, pBindInfos[i].buffer);
      VK_FROM_HANDLE(r3v_device_memory, mem, pBindInfos[i].memory);

      /* Memory aliasing: a second DISTINCT VkBuffer binding a memory already bound
       * to another buffer.  r3v backs every VkBuffer with its own BO, so the
       * host map -- which follows mem->resource -- would populate only the
       * last-bound buffer and leave earlier-bound aliased buffers reading
       * uninitialised BOs (the dEQP host_write_index_buffer wrong-pixels case: the
       * index buffer's BO never receives the CPU writes that land in the
       * later-bound buffer).  Promote to allocation-sized owns storage and record
       * the evicted buffer as a bound_buffers slice; the new buffer is appended by
       * the owns branch below, and the submit/unmap host->buffer sync then
       * propagates the host map to every aliased buffer.  Guarded to !map_ptr so a
       * live map is never orphaned by the mem->resource swap, and to a distinct
       * resource so the single-buffer borrow path (the byte-exact dispatch/compute
       * route) is never promoted. */
      if (!mem->owns_buffer && !mem->map_ptr && mem->resource &&
          mem->resource != buf->resource && mem->size <= UINT_MAX) {
         struct pipe_resource tmpl = {
            .target = PIPE_BUFFER,
            .format = PIPE_FORMAT_R8_UNORM,
            .bind = PIPE_BIND_VERTEX_BUFFER,
            .usage = PIPE_USAGE_DYNAMIC,
            .width0 = (unsigned)mem->size,
            .height0 = 1,
            .depth0 = 1,
            .array_size = 1,
         };
         struct pipe_resource *owned =
            device->screen->resource_create(device->screen, &tmpl);
         if (!owned)
            return vk_error(device, VK_ERROR_OUT_OF_DEVICE_MEMORY);

         struct r3v_bound_buffer_slice evicted = {
            .resource = NULL,
            .offset = mem->memory_offset,
         };
         pipe_resource_reference(&evicted.resource, mem->resource);
         simple_mtx_lock(&device->memory_list_lock);
         util_dynarray_append_typed(&mem->bound_buffers,
                                    struct r3v_bound_buffer_slice, evicted);
         simple_mtx_unlock(&device->memory_list_lock);

         pipe_resource_reference(&mem->resource, NULL);
         mem->resource = owned;
         mem->owns_buffer = true;
      }

      if (mem->owns_buffer) {
         /* Map-before-bind: the memory has its own host pipe_buffer
          * (mem->resource) and a live map.  Vulkan keeps that map valid across
          * this bind.  Seed the VkBuffer's resource with whatever the map
          * already holds (covers a write-BEFORE-bind app).  The slice copy at
          * memory_offset keeps every consumer reading buf->resource from byte 0,
          * so the dispatch path stays offset-free and untouched. */
         if (pBindInfos[i].memoryOffset > mem->size ||
             buf->size > mem->size - pBindInfos[i].memoryOffset)
            return vk_error(device, VK_ERROR_OUT_OF_DEVICE_MEMORY);

         if (device->dbg_log_draws)
            fprintf(stderr, "r3v bind-buf: mem=%p buf=%p res=%p moff=%llu "
                    "bufsize=%llu OWNS slices=%u\n",
                    (void *)mem, (void *)buf, (void *)buf->resource,
                    (unsigned long long)pBindInfos[i].memoryOffset,
                    (unsigned long long)buf->size,
                    (unsigned)util_dynarray_num_elements(
                       &mem->bound_buffers,
                       struct r3v_bound_buffer_slice));
         mem->memory_offset = pBindInfos[i].memoryOffset;
         struct r3v_bound_buffer_slice slice = {
            .resource = NULL,
            .offset = pBindInfos[i].memoryOffset,
         };
         pipe_resource_reference(&slice.resource, buf->resource);
         /* memory_list_lock covers every bound_buffers mutation: the drop
          * helper and the submit-boundary sync walk the array under it. */
         simple_mtx_lock(&device->memory_list_lock);
         util_dynarray_append_typed(&mem->bound_buffers,
                                    struct r3v_bound_buffer_slice, slice);
         simple_mtx_unlock(&device->memory_list_lock);
         /* Seed from the local slice value: an append from another bind can
          * grow-realloc the array, so a pointer into it would dangle. */
         VkResult result = r3v_sync_one_buffer_slice(
            device, mem, &slice,
            true /* host -> buffer (seed) */, 0, VK_WHOLE_SIZE);
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
         if (device->dbg_log_draws)
            fprintf(stderr, "r3v bind-buf: mem=%p buf=%p res=%p moff=%llu "
                    "bufsize=%llu BORROW prev_res=%p prev_moff=%llu\n",
                    (void *)mem, (void *)buf, (void *)buf->resource,
                    (unsigned long long)pBindInfos[i].memoryOffset,
                    (unsigned long long)buf->size, (void *)mem->resource,
                    (unsigned long long)mem->memory_offset);
         mem->memory_offset = pBindInfos[i].memoryOffset;
         pipe_resource_reference(&mem->resource, buf->resource);
      }
   }
   return VK_SUCCESS;
}

VkResult
r3v_BindImageMemory2(VkDevice _device,
                         uint32_t bindInfoCount,
                         const VkBindImageMemoryInfo *pBindInfos)
{
   VK_FROM_HANDLE(r3v_device, device, _device);
   for (uint32_t i = 0; i < bindInfoCount; i++) {
      VK_FROM_HANDLE(r3v_image, img, pBindInfos[i].image);
      VK_FROM_HANDLE(r3v_device_memory, mem, pBindInfos[i].memory);
      const VkDeviceSize image_size = r3v_image_memory_size(img);

      if (pBindInfos[i].memoryOffset > mem->size ||
          image_size > mem->size - pBindInfos[i].memoryOffset)
         return vk_error(device, VK_ERROR_OUT_OF_DEVICE_MEMORY);

      /* Memory aliasing, image-over-buffer: binding an image to a memory
       * whose live view is a borrowed VkBuffer resource would evict that
       * buffer from the memory's host-map path -- vkMapMemory would map the
       * image's storage while GPU consumers of the still-bound buffer read
       * its own never-written BO.  Promote to allocation-sized owns storage
       * exactly like the buffer-over-buffer alias path: the evicted buffer
       * becomes a bound_buffers slice, so the Flush/Unmap/submit
       * host -> buffer sync propagates host writes to it, and the image half
       * rides the bound_image_tile pull below.  The slice keeps the OLD
       * memory_offset (the buffer bind's offset), so the promote runs before
       * this bind overwrites it.  Guarded to !map_ptr so a live transfer is
       * never orphaned, to a distinct resource so re-binding the same image
       * never promotes, and to a PIPE_BUFFER view so an image-over-image
       * rebind keeps the single-active-view model. */
      if (!mem->owns_buffer && !mem->map_ptr && mem->resource &&
          mem->resource != img->resource &&
          mem->resource->target == PIPE_BUFFER && mem->size <= UINT_MAX) {
         struct pipe_resource tmpl = {
            .target = PIPE_BUFFER,
            .format = PIPE_FORMAT_R8_UNORM,
            .bind = PIPE_BIND_VERTEX_BUFFER,
            .usage = PIPE_USAGE_DYNAMIC,
            .width0 = (unsigned)mem->size,
            .height0 = 1,
            .depth0 = 1,
            .array_size = 1,
         };
         struct pipe_resource *owned =
            device->screen->resource_create(device->screen, &tmpl);
         if (!owned)
            return vk_error(device, VK_ERROR_OUT_OF_DEVICE_MEMORY);

         struct r3v_bound_buffer_slice evicted = {
            .resource = NULL,
            .offset = mem->memory_offset,
         };
         pipe_resource_reference(&evicted.resource, mem->resource);
         simple_mtx_lock(&device->memory_list_lock);
         util_dynarray_append_typed(&mem->bound_buffers,
                                    struct r3v_bound_buffer_slice, evicted);
         simple_mtx_unlock(&device->memory_list_lock);

         pipe_resource_reference(&mem->resource, NULL);
         mem->resource = owned;
         mem->owns_buffer = true;
      }

      mem->memory_offset = pBindInfos[i].memoryOffset;
      r3v_clear_bound_image(mem);
      mem->bound_image_offset = pBindInfos[i].memoryOffset;
      mem->bound_image_size = image_size;

      if (mem->owns_buffer) {
         /* Owns storage (map-before-bind, or an alias promote above): the
          * memory keeps its own host pipe_buffer and any live map.  A linear
          * image's pixels live in a separate row-major r300g tile that a GPU
          * copy fills -- record the tile so the submit-exit and Invalidate
          * syncs pull it into the host map, mirroring the owns_buffer
          * VkBuffer slices.  An optimal-tiling image has no host-linear
          * layout to pull; its content is reachable only through GPU image
          * ops, which read img->resource directly, so the owns storage stays
          * the memory's host view. */
         if (img->vk.tiling == VK_IMAGE_TILING_LINEAR) {
            pipe_resource_reference(&mem->bound_image_tile, img->resource);
            mem->bound_image_row_pitch = img->linear_row_pitch;
         }
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
r3v_GetMemoryFdKHR(VkDevice _device,
                      const VkMemoryGetFdInfoKHR *pGetFdInfo,
                      int *pFd)
{
   VK_FROM_HANDLE(r3v_device, device, _device);
   VK_FROM_HANDLE(r3v_device_memory, mem, pGetFdInfo->memory);

   if (pGetFdInfo->handleType != VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT &&
       pGetFdInfo->handleType != VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT)
      return vk_error(device, VK_ERROR_INVALID_EXTERNAL_HANDLE);

   /* Export the dedicated image's BO through the winsys PRIME path -- the
    * same drmPrimeHandleToFD route r300g/GL presents through (the DRI3
    * oracle: two such exports per swapchain, then only CS per frame). */
   struct pipe_resource *bo = NULL;
   if (mem->dedicated_image)
      bo = mem->dedicated_image->tiles[0] ? mem->dedicated_image->tiles[0]
                                          : mem->dedicated_image->resource;
   if (!bo)
      return vk_error(device, VK_ERROR_OUT_OF_DEVICE_MEMORY);

   struct winsys_handle wh = { .type = WINSYS_HANDLE_TYPE_FD };
   if (!device->screen->resource_get_handle(device->screen, device->pipe,
                                            bo,
                                            &wh,
                                            PIPE_HANDLE_USAGE_FRAMEBUFFER_WRITE))
      return vk_error(device, VK_ERROR_OUT_OF_DEVICE_MEMORY);

   *pFd = (int)wh.handle;
   return VK_SUCCESS;
}

VkResult
r3v_GetMemoryFdPropertiesKHR(VkDevice _device,
                                VkExternalMemoryHandleTypeFlagBits handleType,
                                int fd,
                                VkMemoryFdPropertiesKHR *pMemoryFdProperties)
{
   /* Both advertised memory types back an imported dma-buf on the UMA pool. */
   pMemoryFdProperties->memoryTypeBits = 0x3;
   return VK_SUCCESS;
}
