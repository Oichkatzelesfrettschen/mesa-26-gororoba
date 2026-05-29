/*
 * Copyright (c) 2026 Terascale Functionalists
 * SPDX-License-Identifier: MIT
 */

#ifndef R300VK_MEMORY_H
#define R300VK_MEMORY_H

#include "r300vk_private.h"

#include "vk_object.h"

#include "pipe/p_state.h"

#ifdef __cplusplus
extern "C" {
#endif

/* r300vk_device_memory: resource-backed model with lazy map-before-bind storage.
 *
 * RS482/RS485 is UMA; VRAM and GTT share the same physical memory.
 * AllocateMemory allocates the object and defers pipe_resource creation.  The
 * resource is filled lazily by whichever path needs it first:
 *   - MapMemory, when the memory is mapped before any bind, creates the
 *     memory's own HOST_VISIBLE pipe_buffer and sets owns_buffer.  Vulkan
 *     permits mapping VkDeviceMemory before (or without) binding a resource.
 *   - BindBufferMemory2, when owns_buffer is set, copies the buffer's slice of
 *     the mapped allocation (at memoryOffset) into the VkBuffer's own
 *     create-time resource, so every consumer reads buf->resource offset-free
 *     and no draw/descriptor/compute path needs the bind offset; otherwise the
 *     memory borrows the buffer's create-time resource (the prior model).
 *   - BindImageMemory2 installs the image's tiled resource.
 * FlushMappedMemoryRanges and InvalidateMappedMemoryRanges are no-ops because
 * there is no CPU-GPU cache separation on this target.
 *
 * memory_offset records the VkBindBufferMemoryInfo/VkBindImageMemoryInfo
 * memoryOffset.  For a borrowed bound resource (owns_buffer == false) the
 * pipe_resource starts at the buffer, so MapMemory uses
 * resource_offset = MapMemory.offset - memory_offset.  For an owns_buffer
 * allocation the resource IS the whole VkDeviceMemory and memory_offset stays
 * the bind offset for record; the buffer's bytes were already copied out at
 * bind, so the draw reads buf->resource directly.  A subsequent map sees the
 * whole allocation (resource_offset = offset, memory_offset 0 before any bind).
 *
 * Coherency caveat: the bind-time copy is one-way (map -> buffer).  A map that
 * REWRITES the bytes after BindBufferMemory2 will not propagate to the buffer;
 * the deqp allocate->map->write->bind pattern does not do this. */
struct r300vk_device_memory {
   struct vk_object_base  base;
   VkDeviceSize           size;
   VkDeviceSize           memory_offset;  /* from BindBufferMemory2/BindImageMemory2 */
   struct pipe_resource  *resource;  /* NULL until first map or BindBufferMemory2/BindImageMemory2 */
   struct pipe_transfer  *transfer;  /* non-NULL while mapped */
   void                  *map_ptr;
   bool                   owns_buffer;  /* true when MapMemory lazily created the
                                         * memory's own host pipe_buffer for the
                                         * map-before-bind case (vs borrowing a
                                         * bound buffer/image resource) */
};

VK_DEFINE_NONDISP_HANDLE_CASTS(r300vk_device_memory, base,
                                VkDeviceMemory, VK_OBJECT_TYPE_DEVICE_MEMORY)

VkResult r300vk_AllocateMemory(VkDevice device,
                                const VkMemoryAllocateInfo *pAllocateInfo,
                                const VkAllocationCallbacks *pAllocator,
                                VkDeviceMemory *pMemory);

void r300vk_FreeMemory(VkDevice device,
                       VkDeviceMemory memory,
                       const VkAllocationCallbacks *pAllocator);

VkResult r300vk_MapMemory(VkDevice device,
                          VkDeviceMemory memory,
                          VkDeviceSize offset,
                          VkDeviceSize size,
                          VkMemoryMapFlags flags,
                          void **ppData);

void r300vk_UnmapMemory(VkDevice device,
                        VkDeviceMemory memory);

VkResult r300vk_FlushMappedMemoryRanges(VkDevice device,
                                         uint32_t memoryRangeCount,
                                         const VkMappedMemoryRange *pMemoryRanges);

VkResult r300vk_InvalidateMappedMemoryRanges(VkDevice device,
                                              uint32_t memoryRangeCount,
                                              const VkMappedMemoryRange *pMemoryRanges);

VkResult r300vk_BindBufferMemory2(VkDevice device,
                                   uint32_t bindInfoCount,
                                   const VkBindBufferMemoryInfo *pBindInfos);

VkResult r300vk_BindImageMemory2(VkDevice device,
                                  uint32_t bindInfoCount,
                                  const VkBindImageMemoryInfo *pBindInfos);

#ifdef __cplusplus
}
#endif

#endif /* R300VK_MEMORY_H */
