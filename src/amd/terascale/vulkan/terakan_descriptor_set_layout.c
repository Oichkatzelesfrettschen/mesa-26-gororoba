/*
 * Copyright © 2024 Vitaliy Triang3l Kuzmin
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include "terakan_descriptor_set_layout.h"

#include "terakan_descriptor.h"
#include "terakan_descriptor_set.h"
#include "terakan_device.h"
#include "terakan_entrypoints.h"
#include "terakan_limits.h"
#include "terakan_sampler.h"

#include "util/bitscan.h"
#include "util/macros.h"
#include "util/u_math.h"
#include "vk_alloc.h"
#include "vk_log.h"
#include "vk_util.h"

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static int
terakan_descriptor_set_layout_compare_binding_create_infos(void const * const a,
                                                           void const * const b)
{
   VkDescriptorSetLayoutBinding const * const a_binding = (VkDescriptorSetLayoutBinding const *)a;
   VkDescriptorSetLayoutBinding const * const b_binding = (VkDescriptorSetLayoutBinding const *)b;
   /* Move all empty bindings to the end to easily ignore them. */
   if (a_binding->descriptorCount != 0 && b_binding->descriptorCount == 0) {
      return -1;
   }
   if (b_binding->descriptorCount != 0 && a_binding->descriptorCount == 0) {
      return 1;
   }
   if (a_binding->binding < b_binding->binding) {
      return -1;
   }
   if (b_binding->binding < a_binding->binding) {
      return 1;
   }
   return 0;
}

struct terakan_descriptor_set_layout_support_info {
   uint32_t total_descriptors;
   uint32_t max_variable_descriptor_count;
};

/* Under Option B, per-stage and per-pipeline hardware-bank limits are
 * the concern of vkCreatePipelineLayout, not vkCreateDescriptorSet-
 * Layout.  See Vulkan 1.4 specification section 14.2.3:
 * vkGetDescriptorSetLayoutSupport may report platform-specific reasons
 * for failure but is not required to, and per-stage / per-pipeline
 * limits are reported separately via maxPerStageDescriptor* and
 * maxDescriptorSet*.
 *
 * Both call sites of this helper -- the support query and Create-
 * DescriptorSetLayout -- run identical semantics: storage-type safety
 * only.  The helper protects:
 *
 *   - the uint8_t per-stage offset fields on each layout binding
 *     (cap TERAKAN_DESCRIPTOR_SET_PER_STAGE_STORAGE_MAX);
 *   - the uint16_t per-set accumulators
 *     (cap TERAKAN_DESCRIPTOR_SET_PER_SET_STORAGE_MAX);
 *   - the uint32_t per-stage sampler-occupancy bitfields
 *     (cap TERAKAN_DESCRIPTOR_SET_PER_STAGE_SAMPLER_MASK_BITS = 32);
 *   - the gather-safety constraint
 *     (TERAKAN_MAX_GATHER_SAFE_SAMPLED_IMAGES), a single-set single-
 *     stage invariant the pipeline-layout cannot see.
 *
 * Consequences worth naming explicitly:
 *
 *   - Returning supported = VK_FALSE is still possible at the support
 *     query: any layout that exceeds the storage-type caps above is
 *     rejected here.  These are not maxPerSet* / maxPerStageDescriptor*
 *     reports; they are driver-internal representational limits that
 *     are wider than the HW caps but still finite.
 *   - The per-stage hardware-bank caps
 *     (TERAKAN_RESOURCE_RANGE_MUTABLE_MAX_COUNT_PIXEL,
 *      TERAKAN_RESOURCE_RANGE_MUTABLE_MAX_COUNT_NON_PIXEL,
 *      TERAKAN_SAMPLER_HW_COUNT_PER_STAGE) are NOT enforced here.
 *     They are enforced at terakan_pipeline_layout_create time.
 *   - The uniform-buffer cap (TERAKAN_KCACHE_MAX_UNIFORM_BUFFERS) is
 *     enforced ONLY at pipeline-layout time; there is no descriptor-
 *     set-layout counterpart -- the set-layout never tracks uniform-
 *     buffer counts for cap purposes, only for offset assignment.
 */
static bool
terakan_descriptor_set_layout_is_supported(
   struct terakan_device const * const device,
   VkDescriptorSetLayoutCreateInfo const * const create_info,
   struct terakan_descriptor_set_layout_support_info * const support_info)
{
   support_info->total_descriptors = 0;
   support_info->max_variable_descriptor_count = 0;

   if (create_info == NULL ||
       (create_info->bindingCount != 0 && create_info->pBindings == NULL)) {
      return false;
   }

   VkDescriptorSetLayoutBindingFlagsCreateInfo const * const binding_flags_info =
      vk_find_struct_const(create_info->pNext,
                           DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO);

   VkShaderStageFlags stage_mask =
      VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT;
   if (device->vk.enabled_features.geometryShader) {
      stage_mask |= VK_SHADER_STAGE_GEOMETRY_BIT;
   }
   if (device->vk.enabled_features.tessellationShader) {
      stage_mask |=
         VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT | VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT;
   }
   VkShaderStageFlags uav_supported_stage_mask = VK_SHADER_STAGE_COMPUTE_BIT;
   if (device->vk.enabled_features.fragmentStoresAndAtomics) {
      uav_supported_stage_mask |= VK_SHADER_STAGE_FRAGMENT_BIT;
   }

   uint32_t set_resource_count = 0;
   uint32_t set_uav_count = 0;
   uint32_t set_sampler_count = 0;
   uint32_t immutable_sampler_count = 0;
   uint32_t stage_sampled_image_count[MESA_SHADER_STAGES] = {0};
   uint32_t stage_other_resource_count[MESA_SHADER_STAGES] = {0};
   uint32_t stage_sampler_count[MESA_SHADER_STAGES] = {0};
   uint32_t variable_descriptor_count = 0;
   uint32_t variable_binding = 0;
   bool has_variable_binding = false;

   for (uint32_t binding_index = 0; binding_index < create_info->bindingCount; ++binding_index) {
      VkDescriptorSetLayoutBinding const * const binding =
         &create_info->pBindings[binding_index];
      uint32_t const descriptor_count = binding->descriptorCount;

      if (TERAKAN_MAX_PER_SET_DESCRIPTORS - support_info->total_descriptors <
          descriptor_count) {
         return false;
      }
      support_info->total_descriptors += descriptor_count;

      if (binding_flags_info != NULL && binding_index < binding_flags_info->bindingCount &&
          (binding_flags_info->pBindingFlags[binding_index] &
           VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT)) {
         if (!has_variable_binding || binding->binding >= variable_binding) {
            has_variable_binding = true;
            variable_binding = binding->binding;
            variable_descriptor_count = descriptor_count;
         }
      }

      if (descriptor_count == 0) {
         continue;
      }

      for (uint32_t previous_index = 0; previous_index < binding_index; ++previous_index) {
         VkDescriptorSetLayoutBinding const * const previous_binding =
            &create_info->pBindings[previous_index];
         if (previous_binding->descriptorCount != 0 &&
             previous_binding->binding == binding->binding) {
            return false;
         }
      }

      if (descriptor_count > TERAKAN_DESCRIPTOR_SET_PER_STAGE_STORAGE_MAX) {
         return false;
      }

      VkDescriptorType const descriptor_type = binding->descriptorType;
      VkShaderStageFlags const binding_stages = binding->stageFlags & stage_mask;

      if (terakan_descriptor_type_has_resource(descriptor_type)) {
         if (TERAKAN_DESCRIPTOR_SET_PER_SET_STORAGE_MAX - set_resource_count <
             descriptor_count) {
            return false;
         }
         set_resource_count += descriptor_count;

         {
            unsigned remaining_stages = (unsigned)binding_stages;
            while (remaining_stages) {
               int const stage_index = u_bit_scan(&remaining_stages);

               if (terakan_descriptor_type_has_gather_resource(descriptor_type)) {
                  if (TERAKAN_MAX_GATHER_SAFE_SAMPLED_IMAGES -
                         stage_sampled_image_count[stage_index] <
                      descriptor_count) {
                     return false;
                  }
                  stage_sampled_image_count[stage_index] += descriptor_count;
               } else {
                  stage_other_resource_count[stage_index] += descriptor_count;
               }

               uint32_t const stage_resource_count =
                  stage_sampled_image_count[stage_index] != 0
                     ? TERAKAN_GATHER_DESCRIPTOR_SLOT_OFFSET +
                          stage_sampled_image_count[stage_index] +
                          stage_other_resource_count[stage_index]
                     : stage_other_resource_count[stage_index];
               if (stage_resource_count > TERAKAN_DESCRIPTOR_SET_PER_STAGE_STORAGE_MAX) {
                  return false;
               }
            }
         }

         if ((binding_stages & uav_supported_stage_mask) &&
             terakan_descriptor_type_has_uav(descriptor_type)) {
            if (TERAKAN_DESCRIPTOR_SET_PER_SET_STORAGE_MAX - set_uav_count <
                descriptor_count) {
               return false;
            }
            set_uav_count += descriptor_count;
         }
      }

      if (terakan_descriptor_type_has_sampler(descriptor_type)) {
         /* Sampler occupancy is encoded in 32-bit bitfields on the
          * per-stage layout-shader.  A single sampler binding therefore
          * cannot exceed the mask-bit width (32). */
         if (descriptor_count > TERAKAN_DESCRIPTOR_SET_PER_STAGE_SAMPLER_MASK_BITS) {
            return false;
         }
         if (binding->pImmutableSamplers != NULL) {
            if (TERAKAN_DESCRIPTOR_SET_PER_STAGE_STORAGE_MAX -
                   immutable_sampler_count <
                descriptor_count) {
               return false;
            }
            immutable_sampler_count += descriptor_count;
         }
         if (TERAKAN_DESCRIPTOR_SET_PER_STAGE_STORAGE_MAX - set_sampler_count <
             descriptor_count) {
            return false;
         }
         set_sampler_count += descriptor_count;

         unsigned remaining_stages = (unsigned)binding_stages;
         while (remaining_stages) {
            int const stage_index = u_bit_scan(&remaining_stages);
            if (TERAKAN_DESCRIPTOR_SET_PER_STAGE_SAMPLER_MASK_BITS -
                   stage_sampler_count[stage_index] <
                descriptor_count) {
               return false;
            }
            stage_sampler_count[stage_index] += descriptor_count;
         }
      }
   }

   if (has_variable_binding) {
      uint32_t const fixed_descriptor_count =
         support_info->total_descriptors - variable_descriptor_count;
      support_info->max_variable_descriptor_count =
         MIN2(variable_descriptor_count,
              TERAKAN_MAX_PER_SET_DESCRIPTORS - fixed_descriptor_count);
   }

   return true;
}

/* Try to combine the previous range and the new one to make binding slightly faster. */
static bool
terakan_descriptor_set_layout_shader_range_try_extend(
   struct terakan_descriptor_set_layout_shader_range * const range,
   struct terakan_descriptor_set_layout_shader_range const * const extension)
{
   if (range->first_set_descriptor + range->descriptor_count != extension->first_set_descriptor ||
       range->first_shader_descriptor + range->descriptor_count !=
          extension->first_shader_descriptor ||
       ((range->first_dynamic_offset != UINT16_MAX) !=
        (extension->first_dynamic_offset != UINT16_MAX)) ||
       (range->first_dynamic_offset != UINT16_MAX &&
        range->first_dynamic_offset + range->descriptor_count != extension->first_dynamic_offset)) {
      return false;
   }
   range->descriptor_count += extension->descriptor_count;
   return true;
}

VKAPI_ATTR VkResult VKAPI_CALL
terakan_CreateDescriptorSetLayout(VkDevice const deviceHandle,
                                  VkDescriptorSetLayoutCreateInfo const * const pCreateInfo,
                                  UNUSED VkAllocationCallbacks const * const pAllocator,
                                  VkDescriptorSetLayout * const pSetLayout)
{
   struct terakan_device * const device = terakan_device_from_handle(deviceHandle);

   struct terakan_descriptor_set_layout_support_info support_info;
   if (!terakan_descriptor_set_layout_is_supported(device, pCreateInfo, &support_info)) {
      return vk_errorf(
         device, VK_ERROR_VALIDATION_FAILED_EXT,
         "The application creates a descriptor set layout that exceeds Terakan's "
         "internal storage representation; the hardware binding register caps are "
         "enforced separately at vkCreatePipelineLayout time");
   }

   /* Sort bindings by their numbers for pipeline layout compatibility and dynamic offset indexing
    * purposes, and also use the sorting to move empty bindings to the end.
    */
   VkDescriptorSetLayoutBinding * sorted_create_info_bindings = NULL;
   if (pCreateInfo->bindingCount != 0) {
      size_t const sorted_create_info_bindings_size =
         sizeof(VkDescriptorSetLayoutBinding) * pCreateInfo->bindingCount;
      sorted_create_info_bindings =
         vk_alloc2(&device->vk.alloc, pAllocator, sorted_create_info_bindings_size,
                   alignof(VkDescriptorSetLayoutBinding), VK_SYSTEM_ALLOCATION_SCOPE_COMMAND);
      if (sorted_create_info_bindings == NULL) {
         return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);
      }
      memcpy(sorted_create_info_bindings, pCreateInfo->pBindings, sorted_create_info_bindings_size);
      qsort(sorted_create_info_bindings, pCreateInfo->bindingCount,
            sizeof(VkDescriptorSetLayoutBinding),
            terakan_descriptor_set_layout_compare_binding_create_infos);
   }

   /* Skip empty bindings. */
   uint32_t non_empty_create_info_binding_count = pCreateInfo->bindingCount;
   while (non_empty_create_info_binding_count != 0 &&
          sorted_create_info_bindings[non_empty_create_info_binding_count - 1].descriptorCount ==
             0) {
      --non_empty_create_info_binding_count;
   }

   /* VK_SHADER_STAGE_ALL includes bits other than the actually supported stages, mask them out.
    * Also skip binding logic for stages never needed by the application if it uses
    * VK_SHADER_STAGE_ALL.
    */
   VkShaderStageFlags stage_mask =
      VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT;
   if (device->vk.enabled_features.geometryShader) {
      stage_mask |= VK_SHADER_STAGE_GEOMETRY_BIT;
   }
   if (device->vk.enabled_features.tessellationShader) {
      stage_mask |=
         VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT | VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT;
   }
   VkShaderStageFlags uav_supported_stage_mask = VK_SHADER_STAGE_COMPUTE_BIT;
   if (device->vk.enabled_features.fragmentStoresAndAtomics) {
      uav_supported_stage_mask |= VK_SHADER_STAGE_FRAGMENT_BIT;
   }

   /* Calculate the sizes of the suballocations, as well as masks of shader stages requiring any
    * descriptors of a given type.
    */
   size_t binding_count = 0;
   uint8_t immutable_sampler_count = 0;
   uint16_t shader_range_count = 0;
   VkShaderStageFlags stages_with_resources = 0, stages_with_samplers = 0, stages_with_uavs = 0;
   for (uint32_t create_info_binding_index = 0;
        create_info_binding_index < non_empty_create_info_binding_count;
        ++create_info_binding_index) {
      VkDescriptorSetLayoutBinding const * const binding =
         &sorted_create_info_bindings[create_info_binding_index];
      if (create_info_binding_index != 0 &&
          binding->binding == sorted_create_info_bindings[create_info_binding_index - 1].binding) {
         vk_free2(&device->vk.alloc, pAllocator, sorted_create_info_bindings);
         return vk_errorf(
            device, VK_ERROR_VALIDATION_FAILED_EXT,
            "Descriptor set layout has multiple create infos for the same binding number");
      }
      binding_count = (size_t)binding->binding + 1;
      /* Coarsely validate the binding count against the storage-type
       * limit.  Per-stage hardware caps are enforced at pipeline-layout
       * creation, see the leading comment on
       * terakan_descriptor_set_layout_is_supported. */
      if (binding->descriptorCount > TERAKAN_DESCRIPTOR_SET_PER_STAGE_STORAGE_MAX) {
         goto too_many_descriptors;
      }
      uint8_t binding_shader_range_count = 0;
      VkDescriptorType const binding_type = binding->descriptorType;
      VkShaderStageFlagBits const binding_stages = binding->stageFlags & stage_mask;
      if (terakan_descriptor_type_has_resource(binding_type)) {
         ++binding_shader_range_count;
         stages_with_resources |= binding_stages;
         VkShaderStageFlags const binding_uav_stages = binding_stages & uav_supported_stage_mask;
         if (binding_uav_stages && terakan_descriptor_type_has_uav(binding_type)) {
            ++binding_shader_range_count;
            stages_with_uavs |= binding_uav_stages;
         }
      }
      if (terakan_descriptor_type_has_sampler(binding_type)) {
         /* A single sampler binding cannot exceed the 32-bit sampler-
          * mask width on the per-stage layout-shader; tighter than the
          * uint8_t storage MAX. */
         if (binding->descriptorCount > TERAKAN_DESCRIPTOR_SET_PER_STAGE_SAMPLER_MASK_BITS) {
            goto too_many_descriptors;
         }
         if (binding->pImmutableSamplers != NULL) {
            if (TERAKAN_DESCRIPTOR_SET_PER_STAGE_STORAGE_MAX -
                   immutable_sampler_count <
                binding->descriptorCount) {
               goto too_many_descriptors;
            }
            immutable_sampler_count += binding->descriptorCount;
         }
         ++binding_shader_range_count;
         stages_with_samplers |= binding_stages;
      }
      shader_range_count += binding_shader_range_count * util_bitcount((unsigned)binding_stages);
   }

   /* Ordered by access frequency in the allocation:
    * - Shader ranges - primarily for binding.
    * - Bindings - primarily for writing and shader compilation.
    * - Immutable samplers - primarily for allocating.
    */
   VK_MULTIALLOC(multialloc);
   VK_MULTIALLOC_DECL(&multialloc, struct terakan_descriptor_set_layout, layout, 1);
   VK_MULTIALLOC_DECL(&multialloc, struct terakan_descriptor_set_layout_shader_range, shader_ranges,
                      shader_range_count);
   VK_MULTIALLOC_DECL(&multialloc, struct terakan_descriptor_set_layout_binding, bindings,
                      binding_count);
   VK_MULTIALLOC_DECL(&multialloc, uint8_t, immutable_sampler_indices_in_set,
                      immutable_sampler_count);
   VK_MULTIALLOC_DECL(&multialloc, struct terakan_sampler const *, immutable_samplers,
                      immutable_sampler_count);
   /* Mesa descriptor set layout has a different lifetime than the corresponding
    * VkDescriptorSetLayout since other objects hold additional references to them, allocation must
    * be done in the device scope.
    */
   if (vk_descriptor_set_layout_multizalloc(&device->vk, &multialloc, pCreateInfo) == NULL) {
      vk_free2(&device->vk.alloc, pAllocator, sorted_create_info_bindings);
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);
   }
   layout->immutable_sampler_indices_in_set = immutable_sampler_indices_in_set;
   layout->immutable_samplers = immutable_samplers;
   layout->shader_ranges = shader_ranges;
   layout->binding_count = binding_count;
   layout->bindings = bindings;

   /* Set up the layout of the bindings inside the set in descriptor pools, and also immutable
    * samplers.
    */
   uint8_t next_immutable_sampler_index = 0;
   uint16_t dynamic_offset_count = 0;
   uint16_t set_resource_count = 0;
   uint16_t set_uav_count = 0;
   uint8_t set_sampler_count = 0;
   for (uint32_t create_info_binding_index = 0;
        create_info_binding_index < non_empty_create_info_binding_count;
        ++create_info_binding_index) {
      VkDescriptorSetLayoutBinding const * const create_info_binding =
         &sorted_create_info_bindings[create_info_binding_index];
      struct terakan_descriptor_set_layout_binding * const layout_binding =
         &layout->bindings[create_info_binding->binding];

      VkDescriptorType const binding_type = create_info_binding->descriptorType;
      layout_binding->descriptor_type = binding_type;

      uint32_t const binding_descriptor_count = create_info_binding->descriptorCount;
      layout_binding->descriptor_count = binding_descriptor_count;

      /* Write the offsets used in descriptor updating regardless of the descriptor type.
       * This isn't necessary due to the requirements for consecutive bindings in
       * vkUpdateDescriptorSets, but trivially allows for more graceful handling of invalid usage.
       */
      layout_binding->first_set_resource = set_resource_count;
      layout_binding->first_set_uav = set_uav_count;
      layout_binding->first_set_sampler = set_sampler_count;

      /* Add to the counts, validating against the storage-type limits.
       * Per-stage and per-pipeline hardware caps are enforced at
       * terakan_pipeline_layout_create time. */
      if (terakan_descriptor_type_has_resource(binding_type)) {
         if (TERAKAN_DESCRIPTOR_SET_PER_SET_STORAGE_MAX - set_resource_count <
             binding_descriptor_count) {
            goto too_many_descriptors_destroy;
         }
         set_resource_count += binding_descriptor_count;
         if (terakan_descriptor_type_has_uav(binding_type)) {
            if (TERAKAN_DESCRIPTOR_SET_PER_SET_STORAGE_MAX - set_uav_count <
                binding_descriptor_count) {
               goto too_many_descriptors_destroy;
            }
            set_uav_count += binding_descriptor_count;
         }
      }
      bool const binding_has_samplers = terakan_descriptor_type_has_sampler(binding_type);
      if (binding_has_samplers) {
         if (TERAKAN_DESCRIPTOR_SET_PER_STAGE_STORAGE_MAX - set_sampler_count <
             binding_descriptor_count) {
            goto too_many_descriptors_destroy;
         }
         set_sampler_count += binding_descriptor_count;
      }

      layout_binding->first_immutable_sampler_or_dynamic_offset = UINT16_MAX;

      if (binding_has_samplers) {
         if (create_info_binding->pImmutableSamplers != NULL) {
            assert(immutable_sampler_count - next_immutable_sampler_index >=
                   binding_descriptor_count);
            layout_binding->first_immutable_sampler_or_dynamic_offset =
               next_immutable_sampler_index;
            next_immutable_sampler_index += binding_descriptor_count;
            for (uint32_t immutable_sampler_index = 0;
                 immutable_sampler_index < binding_descriptor_count; ++immutable_sampler_index) {
               struct terakan_sampler const * const immutable_sampler = terakan_sampler_from_handle(
                  create_info_binding->pImmutableSamplers[immutable_sampler_index]);
               uint32_t const layout_immutable_sampler_index =
                  layout_binding->first_immutable_sampler_or_dynamic_offset +
                  immutable_sampler_index;
               layout->immutable_sampler_indices_in_set[layout_immutable_sampler_index] =
                  layout_binding->first_set_sampler + immutable_sampler_index;
               layout->immutable_samplers[layout_immutable_sampler_index] = immutable_sampler;
               if (immutable_sampler->unnormalized_coordinates) {
                  layout_binding->immutable_samplers_unnormalized_coordinates |=
                     (uint32_t)1 << immutable_sampler_index;
               }
            }
         }
      } else if (binding_type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC ||
                 binding_type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC) {
         layout_binding->first_immutable_sampler_or_dynamic_offset = dynamic_offset_count;
         dynamic_offset_count += binding_descriptor_count;
      }

      layout_binding->stage_flags = (uint8_t)(create_info_binding->stageFlags & stage_mask);
   }
   assert(next_immutable_sampler_index == immutable_sampler_count);

   layout->pool_first_sampler_offset_bytes =
      sizeof(struct terakan_descriptor_set_resource) * set_resource_count;
   layout->pool_first_uav_offset_bytes =
      layout->pool_first_sampler_offset_bytes +
      sizeof(struct terakan_descriptor_set_sampler) * set_sampler_count;
   layout->pool_size_bytes = layout->pool_first_uav_offset_bytes +
                             sizeof(struct terakan_descriptor_set_uav) * set_uav_count;

   layout->dynamic_offset_count = dynamic_offset_count;
   layout->immutable_sampler_count = immutable_sampler_count;

   /* Set up the layout for each shader stage. */

   uint16_t next_shader_range_index = 0;

   /* Resource ranges. */
   unsigned remaining_stages = (unsigned)stages_with_resources;
   while (remaining_stages) {
      int const stage_index = u_bit_scan(&remaining_stages);

      struct terakan_descriptor_set_layout_shader * const layout_shader =
         &layout->shaders[stage_index];
      layout_shader->first_resource_range = next_shader_range_index;

      uint8_t stage_resource_count = 0;
      uint8_t stage_uniform_buffer_count = 0;
      uint8_t stage_sampled_image_count = 0;

      uint8_t const stage_flag = (uint8_t)1 << stage_index;

      /* Sampled images need a parallel gather-safe descriptor at
       * texture_index + TERAKAN_GATHER_DESCRIPTOR_SLOT_OFFSET.  Assign
       * sampled images first, reserve their sibling range, then place every
       * other resource descriptor after that range so sparse mixed-resource
       * layouts cannot alias a gather sibling with a regular descriptor. */
      for (uint32_t resource_pass = 0; resource_pass < 2; ++resource_pass) {
         bool const sampled_image_pass = resource_pass == 0;

         if (!sampled_image_pass && stage_sampled_image_count != 0) {
            stage_resource_count =
               TERAKAN_GATHER_DESCRIPTOR_SLOT_OFFSET + stage_sampled_image_count;
         }

         for (uint32_t create_info_binding_index = 0;
              create_info_binding_index < non_empty_create_info_binding_count;
              ++create_info_binding_index) {
            struct terakan_descriptor_set_layout_binding * const binding =
               &layout->bindings[sorted_create_info_bindings[create_info_binding_index].binding];

            if (!(binding->stage_flags & stage_flag) ||
                !terakan_descriptor_type_has_resource(binding->descriptor_type)) {
               continue;
            }

            if (terakan_descriptor_type_has_gather_resource(binding->descriptor_type) !=
                sampled_image_pass) {
               continue;
            }

            if (sampled_image_pass &&
                TERAKAN_MAX_GATHER_SAFE_SAMPLED_IMAGES - stage_sampled_image_count <
                   binding->descriptor_count) {
               goto too_many_descriptors_destroy;
            }

            /* Per-stage hardware cap (PIXEL / NON_PIXEL) is enforced
             * at terakan_pipeline_layout_create.  The set-layout
             * guards uint8_t storage of first_shader_resources[]. */
            if (TERAKAN_DESCRIPTOR_SET_PER_STAGE_STORAGE_MAX -
                   stage_resource_count <
                binding->descriptor_count) {
               goto too_many_descriptors_destroy;
            }

            binding->first_shader_resources[stage_index] = stage_resource_count;

            assert(next_shader_range_index < shader_range_count);
            struct terakan_descriptor_set_layout_shader_range * const shader_range =
               &layout->shader_ranges[next_shader_range_index];
            shader_range->first_set_descriptor = binding->first_set_resource;
            shader_range->first_dynamic_offset =
               binding->descriptor_type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC ||
                     binding->descriptor_type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC
                  ? binding->first_immutable_sampler_or_dynamic_offset
                  : UINT16_MAX;
            shader_range->first_shader_descriptor = stage_resource_count;
            shader_range->descriptor_count = binding->descriptor_count;
            if (next_shader_range_index == layout_shader->first_resource_range ||
                !terakan_descriptor_set_layout_shader_range_try_extend(
                   &layout->shader_ranges[next_shader_range_index - 1], shader_range)) {
               ++next_shader_range_index;
            }

            stage_resource_count += binding->descriptor_count;

            if (sampled_image_pass) {
               stage_sampled_image_count += binding->descriptor_count;
            } else if (binding->descriptor_type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER ||
                       binding->descriptor_type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC) {
               binding->first_shader_uniform_buffers[stage_index] = stage_uniform_buffer_count;
               stage_uniform_buffer_count += binding->descriptor_count;
            }
         }
      }

      layout_shader->resource_range_count =
         next_shader_range_index - layout_shader->first_resource_range;

      layout_shader->resource_count = stage_resource_count;
      layout_shader->uniform_buffer_count = stage_uniform_buffer_count;
   }

   /* Sampler ranges. */
   remaining_stages = (unsigned)stages_with_samplers;
   while (remaining_stages) {
      int const stage_index = u_bit_scan(&remaining_stages);

      struct terakan_descriptor_set_layout_shader * const layout_shader =
         &layout->shaders[stage_index];
      layout_shader->first_sampler_range = next_shader_range_index;

      uint8_t stage_sampler_count = 0;

      uint8_t const stage_flag = (uint8_t)1 << stage_index;

      for (uint32_t create_info_binding_index = 0;
           create_info_binding_index < non_empty_create_info_binding_count;
           ++create_info_binding_index) {
         struct terakan_descriptor_set_layout_binding * const binding =
            &layout->bindings[sorted_create_info_bindings[create_info_binding_index].binding];

         if (!(binding->stage_flags & stage_flag) ||
             !terakan_descriptor_type_has_sampler(binding->descriptor_type)) {
            continue;
         }

         /* Per-stage hardware sampler cap (SAMPLER_HW_COUNT_PER_STAGE)
          * is enforced at terakan_pipeline_layout_create.  Here the
          * tighter cap is the 32-bit width of the per-stage sampler-
          * occupancy bitfields (non_immutable_samplers /
          * immutable_samplers_unnormalized_coordinates).  The OR-into-
          * shifted-mask immediately below would invoke shift UB if
          * stage_sampler_count + binding->descriptor_count exceeded the
          * mask width. */
         if (TERAKAN_DESCRIPTOR_SET_PER_STAGE_SAMPLER_MASK_BITS - stage_sampler_count <
             binding->descriptor_count) {
            goto too_many_descriptors_destroy;
         }

         binding->first_shader_samplers[stage_index] = stage_sampler_count;

         assert(next_shader_range_index < shader_range_count);
         struct terakan_descriptor_set_layout_shader_range * const shader_range =
            &layout->shader_ranges[next_shader_range_index];
         shader_range->first_set_descriptor = binding->first_set_sampler;
         shader_range->first_dynamic_offset = UINT16_MAX;
         shader_range->first_shader_descriptor = stage_sampler_count;
         shader_range->descriptor_count = binding->descriptor_count;
         if (next_shader_range_index == layout_shader->first_sampler_range ||
             !terakan_descriptor_set_layout_shader_range_try_extend(
                &layout->shader_ranges[next_shader_range_index - 1], shader_range)) {
            ++next_shader_range_index;
         }

         if (binding->first_immutable_sampler_or_dynamic_offset != UINT16_MAX) {
            layout_shader->immutable_samplers_unnormalized_coordinates |=
               binding->immutable_samplers_unnormalized_coordinates << stage_sampler_count;
         } else {
            layout_shader->non_immutable_samplers |=
               (((uint32_t)1 << binding->descriptor_count) - 1) << stage_sampler_count;
         }

         stage_sampler_count += binding->descriptor_count;
      }

      layout_shader->sampler_range_count =
         next_shader_range_index - layout_shader->first_sampler_range;

      layout_shader->sampler_count = stage_sampler_count;
   }

   /* UAV ranges, after setting up resource ranges because they take shader indices from the
    * resources corresponding to them.
    */
   remaining_stages = (unsigned)stages_with_uavs;
   while (remaining_stages) {
      int const stage_index = u_bit_scan(&remaining_stages);

      struct terakan_descriptor_set_layout_shader * const layout_shader =
         &layout->shaders[stage_index];
      layout_shader->first_uav_range = next_shader_range_index;

      uint8_t const stage_flag = (uint8_t)1 << stage_index;

      for (uint32_t create_info_binding_index = 0;
           create_info_binding_index < non_empty_create_info_binding_count;
           ++create_info_binding_index) {
         struct terakan_descriptor_set_layout_binding * const binding =
            &layout->bindings[sorted_create_info_bindings[create_info_binding_index].binding];

         if (!(binding->stage_flags & stage_flag) ||
             !terakan_descriptor_type_has_uav(binding->descriptor_type)) {
            continue;
         }
         assert(terakan_descriptor_type_has_resource(binding->descriptor_type));

         assert(next_shader_range_index < shader_range_count);
         struct terakan_descriptor_set_layout_shader_range * const shader_range =
            &layout->shader_ranges[next_shader_range_index];
         shader_range->first_set_descriptor = binding->first_set_uav;
         shader_range->first_dynamic_offset =
            binding->descriptor_type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC
               ? binding->first_immutable_sampler_or_dynamic_offset
               : UINT16_MAX;
         shader_range->first_shader_descriptor = binding->first_shader_resources[stage_index];
         shader_range->descriptor_count = binding->descriptor_count;
         if (next_shader_range_index == layout_shader->first_uav_range ||
             !terakan_descriptor_set_layout_shader_range_try_extend(
                &layout->shader_ranges[next_shader_range_index - 1], shader_range)) {
            ++next_shader_range_index;
         }
      }

      layout_shader->uav_range_count = next_shader_range_index - layout_shader->first_uav_range;
   }

   /* Not == because contiguous ranges may be merged. */
   assert(next_shader_range_index <= shader_range_count);

   vk_free2(&device->vk.alloc, pAllocator, sorted_create_info_bindings);

   *pSetLayout = terakan_descriptor_set_layout_to_handle(layout);
   return VK_SUCCESS;

   /* While Vulkan implementations generally shouldn't perform validation, TeraScale has very low
    * binding count limits, while modern games demand many more. If they're launched on Terakan,
    * catch that early and report that instead of proceeding with invalid state.
    */
too_many_descriptors_destroy:
   vk_descriptor_set_layout_unref(&device->vk, &layout->vk);
too_many_descriptors:
   vk_free2(&device->vk.alloc, pAllocator, sorted_create_info_bindings);
   return vk_errorf(
      device, VK_ERROR_VALIDATION_FAILED_EXT,
      "The application creates a descriptor set layout that is too large to fit into the hardware "
      "binding register spaces");
}

/* VK_KHR_maintenance3 / Vulkan 1.1 core: query whether a descriptor set
 * layout would be creatable without actually creating it.
 *
 * Implementation strategy: the support query and CreateDescriptor-
 * SetLayout call the same helper with identical semantics (storage-
 * type safety only).  Per-stage hardware bank caps (PS=176, VS/ES=160,
 * GS=160, HS=160, LS=160, CS=176, FS=32 slots, per AMD Evergreen 3D
 * Registers v2 section 5 "Shader Vertex Resource Constants") are
 * enforced at terakan_pipeline_layout_create, where the full set of
 * bound VkDescriptorSetLayout objects and the target stage mask are
 * known.  This is the spec-conformant Option B model -- a layout that
 * fits maxPerSetDescriptors but overflows a per-stage bank now
 * succeeds at vkCreateDescriptorSetLayout and is rejected at the
 * pipeline-layout that would actually bind it to those banks.
 */
VKAPI_ATTR void VKAPI_CALL
terakan_GetDescriptorSetLayoutSupport(VkDevice const deviceHandle,
                                      VkDescriptorSetLayoutCreateInfo const * const pCreateInfo,
                                      VkDescriptorSetLayoutSupport * const pSupport)
{
   struct terakan_device * const device = terakan_device_from_handle(deviceHandle);

   struct terakan_descriptor_set_layout_support_info support_info;
   pSupport->supported =
      terakan_descriptor_set_layout_is_supported(device, pCreateInfo, &support_info);

   VkDescriptorSetVariableDescriptorCountLayoutSupport * const variable_count =
      vk_find_struct(pSupport->pNext, DESCRIPTOR_SET_VARIABLE_DESCRIPTOR_COUNT_LAYOUT_SUPPORT);
   if (variable_count != NULL) {
      variable_count->maxVariableDescriptorCount =
         pSupport->supported ? support_info.max_variable_descriptor_count : 0;
   }
}
