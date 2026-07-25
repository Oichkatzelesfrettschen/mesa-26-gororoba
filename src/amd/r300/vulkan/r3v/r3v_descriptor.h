/*
 * SPDX-License-Identifier: MIT
 */

#ifndef R3V_DESCRIPTOR_H
#define R3V_DESCRIPTOR_H

#include "r3v_private.h"

#include "vk_descriptor_set_layout.h"
#include "vk_object.h"

#ifdef __cplusplus
extern "C" {
#endif

/* One binding declaration in a descriptor-set layout: which type, how many
 * descriptors in the array, which shader stages may read it, the immutable
 * samplers owned by the layout allocation, and the linear offset where this
 * binding's descriptors start within the set's descriptors[] array.  Sorted by
 * descriptorBinding index. */
struct r3v_dsl_binding {
   uint32_t           binding;
   VkDescriptorType   type;
   uint32_t           count;
   VkShaderStageFlags stage_flags;
   uint32_t           offset;
   const VkSampler   *immutable_samplers;
};

/* The driver's descriptor-set layout extends the runtime's vk_descriptor_set_layout
 * base (ref-counted, freed by vk_common_DestroyDescriptorSetLayout via
 * vk_descriptor_set_layout_unref). */
struct r3v_descriptor_set_layout {
   struct vk_descriptor_set_layout base;
   uint32_t                        binding_count;
   uint32_t                        total_descriptors;
   struct r3v_dsl_binding       bindings[];
};

/* One bound resource slot in a descriptor set.  The union mirrors the
 * VkDescriptorType space; the type field records which arm is live. */
struct r3v_descriptor {
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
struct r3v_descriptor_set {
   struct vk_object_base                base;
   struct r3v_descriptor_set_layout *layout;
   struct r3v_descriptor            *descriptors;
   bool                                 allocated;
};

VK_DEFINE_NONDISP_HANDLE_CASTS(r3v_descriptor_set, base, VkDescriptorSet,
                               VK_OBJECT_TYPE_DESCRIPTOR_SET)

/* A descriptor pool: pre-allocates max_sets slots and lends them out via
 * AllocateDescriptorSets.  Reset marks every slot free.  Per-type descriptor
 * accounting is not tracked because the simple bump-allocator does not need
 * it; the spec's "ran out of descriptors of type X" failure mode is reported
 * as OUT_OF_POOL_MEMORY when max_sets is exhausted. */
struct r3v_descriptor_pool {
   struct vk_object_base         base;
   uint32_t                      max_sets;
   uint32_t                      allocated_sets;
   struct r3v_descriptor_set *sets;
};

VK_DEFINE_NONDISP_HANDLE_CASTS(r3v_descriptor_pool, base, VkDescriptorPool,
                               VK_OBJECT_TYPE_DESCRIPTOR_POOL)

VK_DEFINE_NONDISP_HANDLE_CASTS(r3v_descriptor_set_layout, base.base,
                               VkDescriptorSetLayout,
                               VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT)

bool r3v_descriptor_type_supports_immutable_samplers(VkDescriptorType type);
VkSampler r3v_descriptor_slot_sampler(
   const struct r3v_descriptor_set_layout *layout, uint32_t slot,
   VkSampler written_sampler);
void r3v_initialize_immutable_sampler_descriptors(
   struct r3v_descriptor_set *set);
void r3v_copy_descriptors_preserving_immutable_samplers(
   struct r3v_descriptor_set *destination_set, uint32_t destination_base,
   const struct r3v_descriptor_set *source_set, uint32_t source_base,
   uint32_t count);

#ifdef __cplusplus
}
#endif

#endif /* R3V_DESCRIPTOR_H */
