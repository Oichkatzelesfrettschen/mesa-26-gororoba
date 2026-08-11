/*
 * Copyright 2026 Mesa3D authors
 * SPDX-License-Identifier: MIT
 */

#ifndef R3V_UBO_BINDING_H
#define R3V_UBO_BINDING_H

#include "r3v_descriptor.h"

#include <limits.h>
#include <stdbool.h>
#include <stdint.h>

/* Vulkan 1.4 specification, descriptor sets chapter, dynamic uniform and
 * storage buffers: the dynamic offset shifts the descriptor base while the
 * descriptor range stays unchanged. Gallium's pipe_constant_buffer declaration
 * (rg --fixed-strings "struct pipe_constant_buffer"
 * src/gallium/include/pipe/p_state.h) stores buffer_offset and buffer_size as
 * unsigned fields, so the backing buffer can shorten that range and effective
 * offsets above UINT_MAX are rejected. */
struct r3v_ubo_effective_range {
   VkDeviceSize offset;
   VkDeviceSize size;
};

static inline bool
r3v_descriptor_type_is_dynamic(VkDescriptorType type)
{
   return type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC ||
          type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC;
}

static inline bool
r3v_ubo_descriptor_type_is_dynamic(VkDescriptorType type)
{
   return type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
}

/* Return the pDynamicOffsets index for one descriptor in a sorted set layout.
 * UINT32_MAX identifies static bindings, missing bindings, and an index past
 * a descriptor array. */
static inline uint32_t
r3v_ubo_dynamic_offset_index(const struct r3v_descriptor_set_layout *layout,
                             uint32_t binding, uint32_t descriptor_index)
{
   uint32_t dynamic_index = 0;

   if (!layout)
      return UINT32_MAX;

   for (uint32_t i = 0; i < layout->binding_count; i++) {
      const struct r3v_dsl_binding *bnd = &layout->bindings[i];
      if (!r3v_descriptor_type_is_dynamic(bnd->type))
         continue;

      if (bnd->binding == binding) {
         if (descriptor_index >= bnd->count ||
             dynamic_index > UINT32_MAX - descriptor_index)
            return UINT32_MAX;
         return dynamic_index + descriptor_index;
      }

      if (dynamic_index > UINT32_MAX - bnd->count)
         return UINT32_MAX;
      dynamic_index += bnd->count;
   }

   return UINT32_MAX;
}

/* Calculate the range passed to Gallium after applying one dynamic offset.
 * The descriptor update path validates the first two bounds, but keeping the
 * checks here makes replay fail closed when a malformed or stale descriptor
 * reaches the queue. Gallium's pipe_constant_buffer declaration
 * (rg --fixed-strings "struct pipe_constant_buffer"
 * src/gallium/include/pipe/p_state.h) stores buffer_offset and buffer_size in
 * unsigned fields, so an effective offset above UINT_MAX cannot be represented
 * safely. */
static inline bool
r3v_ubo_effective_range(VkDeviceSize descriptor_offset,
                         VkDeviceSize descriptor_range,
                         uint32_t dynamic_offset,
                         VkDeviceSize buffer_size,
                         struct r3v_ubo_effective_range *out)
{
   if (!out || descriptor_offset > buffer_size ||
       descriptor_range > buffer_size - descriptor_offset)
      return false;

   if ((VkDeviceSize)dynamic_offset > buffer_size - descriptor_offset)
      return false;

   const VkDeviceSize effective_offset =
      descriptor_offset + (VkDeviceSize)dynamic_offset;
   const VkDeviceSize buffer_remaining = buffer_size - effective_offset;

   out->offset = effective_offset;
   out->size = descriptor_range < buffer_remaining
               ? descriptor_range : buffer_remaining;
   return effective_offset <= UINT_MAX;
}

static inline VkDeviceSize
r3v_ubo_scratch_copy_size(VkDeviceSize range, uint32_t required_size)
{
   VkDeviceSize copy_size = range < (VkDeviceSize)required_size
                            ? range : (VkDeviceSize)required_size;
   return copy_size & ~(VkDeviceSize)3;
}

static inline uint32_t
r3v_ubo_constant_span(uint32_t interface_size)
{
   return (interface_size + 15u) & ~15u;
}

#endif /* R3V_UBO_BINDING_H */
