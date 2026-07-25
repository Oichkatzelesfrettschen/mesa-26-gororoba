/*
 * SPDX-License-Identifier: MIT
 */

#ifndef R3V_BUFFER_H
#define R3V_BUFFER_H

#include "r3v_private.h"

#include "vk_object.h"

#include "pipe/p_state.h"

#ifdef __cplusplus
extern "C" {
#endif

/* r3v_buffer wraps a Gallium PIPE_BUFFER resource created at
 * CreateBuffer time with PIPE_BIND_VERTEX_BUFFER and PIPE_USAGE_DYNAMIC.
 * Memory binding (BindBufferMemory2) copies the resource reference into
 * the companion r3v_device_memory for MapMemory access. */
struct r3v_buffer {
   struct vk_object_base  base;
   VkDeviceSize           size;
   VkBufferUsageFlags     usage;
   struct pipe_resource  *resource;  /* created immediately at CreateBuffer */
};

VK_DEFINE_NONDISP_HANDLE_CASTS(r3v_buffer, base, VkBuffer,
                                VK_OBJECT_TYPE_BUFFER)

VkResult r3v_CreateBuffer(VkDevice device,
                              const VkBufferCreateInfo *pCreateInfo,
                              const VkAllocationCallbacks *pAllocator,
                              VkBuffer *pBuffer);

void r3v_DestroyBuffer(VkDevice device,
                           VkBuffer buffer,
                           const VkAllocationCallbacks *pAllocator);

void r3v_GetBufferMemoryRequirements2(VkDevice device,
                                          const VkBufferMemoryRequirementsInfo2 *pInfo,
                                          VkMemoryRequirements2 *pMemoryRequirements);

#ifdef __cplusplus
}
#endif

#endif /* R3V_BUFFER_H */
