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

/* r300vk_device_memory: resource-backed model.
 *
 * RS482/RS485 is UMA; VRAM and GTT share the same physical memory.
 * AllocateMemory allocates the object but defers pipe_resource creation
 * until BindBufferMemory2 or BindImageMemory2.  MapMemory requires a
 * prior bind; without a resource it returns VK_ERROR_MEMORY_MAP_FAILED.
 * FlushMappedMemoryRanges and InvalidateMappedMemoryRanges are no-ops
 * because there is no CPU-GPU cache separation on this target. */
struct r300vk_device_memory {
   struct vk_object_base  base;
   VkDeviceSize           size;
   struct pipe_resource  *resource;  /* NULL until BindBufferMemory2/BindImageMemory2 */
   struct pipe_transfer  *transfer;  /* non-NULL while mapped via transfer_map */
   void                  *map_ptr;
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
