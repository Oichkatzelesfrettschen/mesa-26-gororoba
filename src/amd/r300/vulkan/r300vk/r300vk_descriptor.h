/*
 * Copyright (c) 2026 Terascale Functionalists
 * SPDX-License-Identifier: MIT
 */

#ifndef R300VK_DESCRIPTOR_H
#define R300VK_DESCRIPTOR_H

#include "r300vk_private.h"

#include "vk_descriptor_set_layout.h"
#include "vk_object.h"

#ifdef __cplusplus
extern "C" {
#endif

/* One binding declaration in a descriptor-set layout: which type, how many
 * descriptors in the array, which shader stages may read it, and the linear
 * offset where this binding's descriptors start within the set's
 * descriptors[] array.  Sorted by descriptorBinding index. */
struct r300vk_dsl_binding {
   uint32_t           binding;
   VkDescriptorType   type;
   uint32_t           count;
   VkShaderStageFlags stage_flags;
   uint32_t           offset;
};

/* The driver's descriptor-set layout extends the runtime's vk_descriptor_set_layout
 * base (ref-counted, freed by vk_common_DestroyDescriptorSetLayout via
 * vk_descriptor_set_layout_unref). */
struct r300vk_descriptor_set_layout {
   struct vk_descriptor_set_layout base;
   uint32_t                        binding_count;
   uint32_t                        total_descriptors;
   struct r300vk_dsl_binding       bindings[];
};

/* One bound resource slot in a descriptor set.  The union mirrors the
 * VkDescriptorType space; the type field records which arm is live. */
struct r300vk_descriptor {
   VkDescriptorType type;
   union {
      struct {
         VkBuffer     buffer;
         VkDeviceSize offset;
         VkDeviceSize range;
      } buf;
      struct {
         VkImageView   image_view;
         VkImageLayout layout;
         VkSampler     sampler;
      } img;
   };
};

/* A descriptor set: layout reference + the flat descriptors[] array sized by
 * the layout's total_descriptors. */
struct r300vk_descriptor_set {
   struct vk_object_base                base;
   struct r300vk_descriptor_set_layout *layout;
   struct r300vk_descriptor            *descriptors;
   bool                                 allocated;
};

VK_DEFINE_NONDISP_HANDLE_CASTS(r300vk_descriptor_set, base, VkDescriptorSet,
                               VK_OBJECT_TYPE_DESCRIPTOR_SET)

/* A descriptor pool: pre-allocates max_sets slots and lends them out via
 * AllocateDescriptorSets.  Reset marks every slot free.  Per-type descriptor
 * accounting is not tracked because the simple bump-allocator does not need
 * it; the spec's "ran out of descriptors of type X" failure mode is reported
 * as OUT_OF_POOL_MEMORY when max_sets is exhausted. */
struct r300vk_descriptor_pool {
   struct vk_object_base         base;
   uint32_t                      max_sets;
   uint32_t                      allocated_sets;
   struct r300vk_descriptor_set *sets;
};

VK_DEFINE_NONDISP_HANDLE_CASTS(r300vk_descriptor_pool, base, VkDescriptorPool,
                               VK_OBJECT_TYPE_DESCRIPTOR_POOL)

VK_DEFINE_NONDISP_HANDLE_CASTS(r300vk_descriptor_set_layout, base.base,
                               VkDescriptorSetLayout,
                               VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT)

#ifdef __cplusplus
}
#endif

#endif /* R300VK_DESCRIPTOR_H */
