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
 *   - BindBufferMemory2, when owns_buffer is set, points the VkBuffer at that
 *     same buffer so map writes and the bound buffer share one allocation;
 *     otherwise the memory borrows the buffer's create-time resource.
 *   - BindImageMemory2 installs the image's tiled resource.
 * FlushMappedMemoryRanges and InvalidateMappedMemoryRanges are no-ops because
 * there is no CPU-GPU cache separation on this target.
 *
 * memory_offset records the VkBindBufferMemoryInfo/VkBindImageMemoryInfo
 * memoryOffset.  For a borrowed bound resource (owns_buffer == false) the
 * pipe_resource starts at the buffer, so MapMemory uses
 * resource_offset = MapMemory.offset - memory_offset.  An owns_buffer
 * allocation's resource IS the whole VkDeviceMemory, so memoryOffset must be
 * zero (BindBufferMemory2 rejects non-zero) and offset - 0 reduces correctly. */
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
