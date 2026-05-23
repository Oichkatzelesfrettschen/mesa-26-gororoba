/*
 * Copyright (c) 2026 Terascale Functionalists
 * SPDX-License-Identifier: MIT
 */

#ifndef R300VK_BUFFER_H
#define R300VK_BUFFER_H

#include "r300vk_private.h"

#include "vk_object.h"

#include "pipe/p_state.h"

#ifdef __cplusplus
extern "C" {
#endif

/* r300vk_buffer wraps a Gallium PIPE_BUFFER resource created at
 * CreateBuffer time with PIPE_BIND_VERTEX_BUFFER and PIPE_USAGE_DYNAMIC.
 * Memory binding (BindBufferMemory2) copies the resource reference into
 * the companion r300vk_device_memory for MapMemory access. */
struct r300vk_buffer {
   struct vk_object_base  base;
   VkDeviceSize           size;
   VkBufferUsageFlags     usage;
   struct pipe_resource  *resource;  /* created immediately at CreateBuffer */
};

VK_DEFINE_NONDISP_HANDLE_CASTS(r300vk_buffer, base, VkBuffer,
                                VK_OBJECT_TYPE_BUFFER)

VkResult r300vk_CreateBuffer(VkDevice device,
                              const VkBufferCreateInfo *pCreateInfo,
                              const VkAllocationCallbacks *pAllocator,
                              VkBuffer *pBuffer);

void r300vk_DestroyBuffer(VkDevice device,
                           VkBuffer buffer,
                           const VkAllocationCallbacks *pAllocator);

void r300vk_GetBufferMemoryRequirements2(VkDevice device,
                                          const VkBufferMemoryRequirementsInfo2 *pInfo,
                                          VkMemoryRequirements2 *pMemoryRequirements);

#ifdef __cplusplus
}
#endif

#endif /* R300VK_BUFFER_H */
