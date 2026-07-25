/*
 * SPDX-License-Identifier: MIT
 */

#ifndef R3V_MEMORY_H
#define R3V_MEMORY_H

#include "r3v_private.h"

#include "vk_object.h"

#include "pipe/p_state.h"
#include "util/list.h"
#include "util/u_dynarray.h"

#ifdef __cplusplus
extern "C" {
#endif

/* r3v_device_memory: resource-backed model with lazy map-before-bind storage.
 *
 * RS482/RS485 is UMA; VRAM and GTT share the same physical memory.
 * AllocateMemory allocates the object and defers pipe_resource creation.  The
 * resource is filled lazily by whichever path needs it first:
 *   - MapMemory, when the memory is mapped before any bind, creates the
 *     memory's own HOST_VISIBLE pipe_buffer and sets owns_buffer.  Vulkan
 *     permits mapping VkDeviceMemory before (or without) binding a resource.
 *   - BindBufferMemory2, when owns_buffer is set, appends the bound VkBuffer's
 *     create-time resource to bound_buffers and seeds it with whatever the map
 *     already holds.  The host map stays live across the bind, and consumers
 *     read buf->resource offset-free (the slice copy carries memoryOffset), so
 *     no draw/descriptor/compute path needs the bind offset; otherwise the
 *     memory borrows the buffer's create-time resource (the prior model).
 *   - BindImageMemory2 installs the image's tiled resource.
 *
 * Map persistence and coherency: Vulkan keeps a map valid across bind, and the
 * standard deqp pattern is map -> bind -> WRITE-via-host-ptr -> flush (and
 * render -> invalidate -> READ-via-host-ptr).  The host map lives on the lazy
 * pipe_buffer (mem->resource); each bound VkBuffer has its own GPU-side
 * resource, recorded as a bound_buffers slice.  They are kept in sync only at
 * the explicit barriers: FlushMappedMemoryRanges copies the live map (host) ->
 * each slice's resource (so a post-bind host write reaches the GPU buffer), and
 * InvalidateMappedMemoryRanges copies each slice's resource (GPU) -> the live
 * map (so a copyImageToBuffer result reaches the host read).  The lazy buffer is
 * host-only -- the GPU never touches it -- so the map needs no
 * PIPE_MAP_PERSISTENT/COHERENT.
 *
 * One allocation, many buffers: a suballocating client (zink slabs upload
 * buffers this way) binds several VkBuffers at distinct memoryOffsets within
 * one VkDeviceMemory.  Every binding must keep receiving host writes for the
 * allocation's lifetime, so the bindings live in the bound_buffers array --
 * tracking only the most recent bind would orphan every earlier buffer's
 * resource from the sync (its vertex data would read back as zeros after the
 * next buffer binds). */
struct r3v_bound_buffer_slice {
   struct pipe_resource *resource;  /* the bound VkBuffer's resource (referenced) */
   VkDeviceSize          offset;    /* VkBindBufferMemoryInfo::memoryOffset */
};

/*
 *
 * memory_offset records the most recent VkBindBufferMemoryInfo or
 * VkBindImageMemoryInfo memoryOffset.  Image bind metadata keeps its own byte
 * slice because later buffer binds can legitimately change memory_offset.
 * bound_image_tile is present only for owns_buffer linear-image sync; the
 * offset and size still constrain texture-backed maps to the image byte span.
 * For a borrowed bound resource (owns_buffer == false) the
 * pipe_resource starts at the buffer, so MapMemory uses
 * resource_offset = MapMemory.offset - memory_offset.  For an owns_buffer
 * allocation the resource IS the whole VkDeviceMemory; the sync copies the
 * buffer's slice at its bind offset, so consumers read each bound buffer's
 * resource from byte 0. */
struct r3v_device_memory {
   struct vk_object_base  base;
   VkDeviceSize           size;
   VkDeviceSize           memory_offset;  /* most recent bind's memoryOffset: the borrow-map
                                           * base for buffers, the slice base for images */
   struct pipe_resource  *resource;  /* NULL until first map or BindBufferMemory2/BindImageMemory2 */
   struct util_dynarray   bound_buffers;  /* owns_buffer: r3v_bound_buffer_slice per bound
                                           * VkBuffer, each synced with the host map at
                                           * Flush/Invalidate and the submit boundary */
   struct pipe_resource  *bound_image_tile; /* owns_buffer + linear image: the bound image's single
                                             * row-major tile, pulled into the host map at
                                             * Invalidate and the submit boundary */
   VkDeviceSize           bound_image_offset; /* VkBindImageMemoryInfo::memoryOffset for the image */
   VkDeviceSize           bound_image_size; /* byte span occupied by the image binding */
   uint32_t               bound_image_row_pitch; /* linear-image row stride; 0 when the binding is a
                                                  * buffer or an optimal image (no host-linear layout) */
   struct pipe_resource  *mapped_resource; /* holds a ref on the resource currently mapped,
                                            * keeping it alive if mem->resource is rebound */
   struct pipe_transfer  *transfer;  /* non-NULL while mapped */
   void                  *map_ptr;
   VkDeviceSize           map_offset;  /* byte offset of map_ptr inside VkDeviceMemory */
   VkDeviceSize           map_size;    /* live vkMapMemory byte range */
   bool                   owns_buffer;  /* true when MapMemory lazily created the
                                         * memory's own host pipe_buffer for the
                                         * map-before-bind case (vs borrowing a
                                         * bound buffer/image resource) */
   struct r3v_image   *dedicated_image; /* VkMemoryDedicatedAllocateInfo image:
                                            * the BO vkGetMemoryFdKHR exports */
   struct list_head       device_link;  /* in r3v_device::memory_list, for the
                                         * submit-boundary coherence sync */
};

VK_DEFINE_NONDISP_HANDLE_CASTS(r3v_device_memory, base,
                                VkDeviceMemory, VK_OBJECT_TYPE_DEVICE_MEMORY)

struct r3v_device;

/* Sync every owns_buffer allocation's live host map with its bound resource:
 * host_to_buffer pushes app writes into the bound VkBuffer's GPU resource
 * (submit entry), !host_to_buffer pulls GPU results back into the map (submit
 * exit, after the fence retires) -- the submit-boundary realization of
 * HOST_COHERENT memory on this synchronous-submit device. */
void r3v_device_memory_sync_bound(struct r3v_device *device,
                                     bool host_to_buffer);

/* Drop every bound_buffers slice whose resource is the given one, across all
 * live allocations.  DestroyBuffer must call this: a suballocating client
 * recycles (destroys and re-creates) buffers within a live allocation, and a
 * dead buffer's slice left in the array would keep copying its stale resource
 * back over the host map at the next buffer -> host sync. */
void r3v_device_memory_drop_buffer_slices(struct r3v_device *device,
                                             struct pipe_resource *resource);

VkResult r3v_AllocateMemory(VkDevice device,
                                const VkMemoryAllocateInfo *pAllocateInfo,
                                const VkAllocationCallbacks *pAllocator,
                                VkDeviceMemory *pMemory);

void r3v_FreeMemory(VkDevice device,
                       VkDeviceMemory memory,
                       const VkAllocationCallbacks *pAllocator);

VkResult r3v_MapMemory(VkDevice device,
                          VkDeviceMemory memory,
                          VkDeviceSize offset,
                          VkDeviceSize size,
                          VkMemoryMapFlags flags,
                          void **ppData);

void r3v_UnmapMemory(VkDevice device,
                        VkDeviceMemory memory);

VkResult r3v_FlushMappedMemoryRanges(VkDevice device,
                                         uint32_t memoryRangeCount,
                                         const VkMappedMemoryRange *pMemoryRanges);

VkResult r3v_InvalidateMappedMemoryRanges(VkDevice device,
                                              uint32_t memoryRangeCount,
                                              const VkMappedMemoryRange *pMemoryRanges);

VkResult r3v_BindBufferMemory2(VkDevice device,
                                   uint32_t bindInfoCount,
                                   const VkBindBufferMemoryInfo *pBindInfos);

VkResult r3v_GetMemoryFdKHR(VkDevice device,
                               const VkMemoryGetFdInfoKHR *pGetFdInfo,
                               int *pFd);

VkResult r3v_GetMemoryFdPropertiesKHR(VkDevice device,
                                         VkExternalMemoryHandleTypeFlagBits handleType,
                                         int fd,
                                         VkMemoryFdPropertiesKHR *pMemoryFdProperties);

VkResult r3v_BindImageMemory2(VkDevice device,
                                  uint32_t bindInfoCount,
                                  const VkBindImageMemoryInfo *pBindInfos);

#ifdef __cplusplus
}
#endif

#endif /* R3V_MEMORY_H */
