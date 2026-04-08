/*
 * Copyright (c) 2024 Vitaliy Triang3l Kuzmin
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

/*
 * terakan_nir_apply_pipeline_layout.c — Descriptor resolution (WHERE)
 *
 * Resolves abstract Vulkan bindings to physical hardware indices.
 * Does NOT emit load/store instructions.
 *
 * Phase 0 mechanical file split of the former monolith
 * terakan_nir_lower_bindings.c.  See TERAKAN_SHADER_ABI_CONTRACT.md.
 */

#include "terakan_nir_lower_bindings_internal.h"

bool
terakan_nir_lower_load_vulkan_descriptor_filter(nir_instr const * const instr,
                                                UNUSED void const * const cb_data)
{
   return instr->type == nir_instr_type_intrinsic &&
          nir_instr_as_intrinsic(instr)->intrinsic == nir_intrinsic_load_vulkan_descriptor;
}

nir_def *
terakan_nir_lower_load_vulkan_descriptor_impl(nir_builder * const b, nir_instr * const instr,
                                              UNUSED void * const cb_data)
{
   nir_intrinsic_instr * const resource_intrin =
      nir_src_as_intrinsic(nir_instr_as_intrinsic(instr)->src[0]);
   if (resource_intrin == NULL ||
       (resource_intrin->intrinsic != nir_intrinsic_vulkan_resource_index &&
        resource_intrin->intrinsic != nir_intrinsic_vulkan_resource_reindex)) {
      assert(!"load_vulkan_descriptor must accept a Vulkan resource index instruction");
      return NULL;
   }
   return &resource_intrin->def;
}

static nir_def *
terakan_nir_get_vulkan_resource_array_index(nir_builder * const b,
                                            nir_intrinsic_instr * const intrin)
{
   if (intrin->intrinsic == nir_intrinsic_vulkan_resource_index) {
      return intrin->src[0].ssa;
   }

   if (intrin->intrinsic != nir_intrinsic_vulkan_resource_reindex) {
      assert(!"vulkan_resource_reindex chains must consist only of vulkan_resource_reindex and "
              "vulkan_resource_index intrinsics");
      return NULL;
   }

   /* Accumulate in a way friendly to common subexpression elimination:
    * (index + reindex1) + reindex2...
    */
   nir_intrinsic_instr * const previous_resource_intrin = nir_src_as_intrinsic(intrin->src[0]);
   if (previous_resource_intrin == NULL) {
      assert(!"vulkan_resource_reindex chains must consist only of vulkan_resource_reindex and "
              "vulkan_resource_index intrinsics");
      return NULL;
   }
   nir_def * const previous_array_index =
      terakan_nir_get_vulkan_resource_array_index(b, previous_resource_intrin);
   b->cursor = nir_before_instr(&intrin->instr);
   return nir_iadd_nuw(b, previous_array_index, intrin->src[1].ssa);
}

bool
terakan_nir_lower_vulkan_resource_reindex_instr(nir_builder * const b, nir_instr * const instr,
                                                UNUSED void * const cb_data)
{
   if (instr->type != nir_instr_type_intrinsic) {
      return false;
   }
   nir_intrinsic_instr * const resource_reindex_intrin = nir_instr_as_intrinsic(instr);
   if (resource_reindex_intrin->intrinsic != nir_intrinsic_vulkan_resource_reindex) {
      return false;
   }

   /* Go to the initial vulkan_resource_index to obtain the set and the binding. */
   nir_intrinsic_instr const * initial_resource_intrin = resource_reindex_intrin;
   while (initial_resource_intrin != NULL &&
          initial_resource_intrin->intrinsic == nir_intrinsic_vulkan_resource_reindex) {
      initial_resource_intrin = nir_src_as_intrinsic(initial_resource_intrin->src[0]);
   }
   if (initial_resource_intrin == NULL ||
       initial_resource_intrin->intrinsic != nir_intrinsic_vulkan_resource_index) {
      assert(!"vulkan_resource_reindex chains must consist only of vulkan_resource_reindex and "
              "vulkan_resource_index intrinsics");
      return false;
   }

   nir_def * const array_index =
      terakan_nir_get_vulkan_resource_array_index(b, resource_reindex_intrin);
   assert(array_index != NULL);
   if (array_index == NULL) {
      return false;
   }

   b->cursor = nir_before_instr(&resource_reindex_intrin->instr);
   nir_def_rewrite_uses(
      &resource_reindex_intrin->def,
      nir_vulkan_resource_index(b, initial_resource_intrin->num_components,
                                initial_resource_intrin->def.bit_size, array_index,
                                .desc_set = nir_intrinsic_desc_set(initial_resource_intrin),
                                .binding = nir_intrinsic_binding(initial_resource_intrin),
                                .desc_type = nir_intrinsic_desc_type(initial_resource_intrin)));
   nir_instr_remove(&resource_reindex_intrin->instr);
   return true;
}

bool
terakan_nir_zero_vulkan_resource_offset_filter(nir_instr const * const instr,
                                               UNUSED void const * const cb_data)
{
   if (instr->type != nir_instr_type_intrinsic) {
      return false;
   }
   nir_intrinsic_instr const * const intrin = nir_instr_as_intrinsic(instr);
   return intrin->intrinsic == nir_intrinsic_vulkan_resource_index && intrin->num_components == 2;
}

nir_def *
terakan_nir_zero_vulkan_resource_offset_impl(nir_builder * const b, nir_instr * const instr,
                                             UNUSED void * const cb_data)
{
   nir_intrinsic_instr const * const intrin = nir_instr_as_intrinsic(instr);
   return nir_vec2(b,
                   nir_vulkan_resource_index(b, 1, intrin->def.bit_size, intrin->src[0].ssa,
                                             .desc_set = nir_intrinsic_desc_set(intrin),
                                             .binding = nir_intrinsic_binding(intrin),
                                             .desc_type = nir_intrinsic_desc_type(intrin)),
                   nir_imm_int(b, 0));
}

VkDescriptorType
terakan_nir_image_descriptor_type(enum glsl_sampler_dim const dim)
{
   return dim == GLSL_SAMPLER_DIM_BUF ? VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER
                                      : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
}


bool
terakan_nir_get_binding(nir_src const src, VkDescriptorType const expected_type,
                        struct terakan_pipeline_layout const * const layout,
                        nir_shader * const shader, struct terakan_nir_binding * const binding_out)
{
   nir_binding const binding = nir_chase_binding(src);
   assert(binding.success);
   if (unlikely(!binding.success)) {
      return false;
   }

   if (unlikely(binding.desc_set >= layout->vk.set_count)) {
      vk_loge(VK_LOG_OBJS(&layout->vk.base),
              "Descriptor set %u doesn't exist in the pipeline layout (contains %" PRIu32 " "
              "descriptor sets)",
              binding.desc_set, layout->vk.set_count);
      return false;
   }
   struct terakan_descriptor_set_layout const * const set_layout = container_of(
      layout->vk.set_layouts[binding.desc_set], struct terakan_descriptor_set_layout const, vk);

   if (unlikely(binding.binding >= set_layout->binding_count)) {
      vk_loge(VK_LOG_OBJS(&layout->vk.base),
              "Descriptor set %u binding %u doesn't exist in the descriptor set layout (contains "
              "%zu bindings)",
              binding.desc_set, binding.binding, set_layout->binding_count);
      return false;
   }
   struct terakan_descriptor_set_layout_binding const * const set_binding =
      &set_layout->bindings[binding.binding];

   /* If a terakan_descriptor_set_layout_binding has 0 descriptors, its fields may be uninitialized.
    * The inclusive array index range would also be impossible to calculate.
    */
   if (unlikely(set_binding->descriptor_count == 0)) {
      vk_loge(VK_LOG_OBJS(&layout->vk.base),
              "Descriptor set %u binding %u doesn't contain any descriptors", binding.desc_set,
              binding.binding);
      return false;
   }

   bool type_compatible = set_binding->descriptor_type == expected_type;
   if (!type_compatible) {
      if (expected_type == VK_DESCRIPTOR_TYPE_SAMPLER) {
         type_compatible =
            set_binding->descriptor_type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
      } else if (expected_type == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE) {
         type_compatible =
            set_binding->descriptor_type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER ||
            set_binding->descriptor_type == VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT;
      } else if (expected_type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER) {
         type_compatible =
            set_binding->descriptor_type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
      } else if (expected_type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER) {
         type_compatible =
            set_binding->descriptor_type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC;
      }
   }
   if (unlikely(!type_compatible)) {
      vk_loge(
         VK_LOG_OBJS(&layout->vk.base),
         "Descriptor set %u binding %u is expected to contain descriptors compatible with %s, but "
         "the descriptor set layout specifies them as %s instead",
         binding.desc_set, binding.binding, vk_DescriptorType_to_str(expected_type),
         vk_DescriptorType_to_str(set_binding->descriptor_type));
      return false;
   }

   assert(binding.num_indices <= 1);
   nir_def * array_index = binding.num_indices >= 1 ? binding.indices[0].ssa : NULL;
   unsigned array_index_range_first, array_index_range_last;
   if (array_index != NULL) {
      if (nir_def_instr(array_index)->type == nir_instr_type_load_const) {
         nir_load_const_instr const * const array_index_load_const =
            nir_instr_as_load_const(nir_def_instr(array_index));
         array_index_range_first = array_index_load_const->value[0].u32;
         if (unlikely(array_index_range_first >= set_binding->descriptor_count)) {
            vk_loge(VK_LOG_OBJS(&layout->vk.base),
                    "Descriptor %u doesn't exist in the descriptor set %u binding %u (contains %u "
                    "descriptors)",
                    array_index_range_first, binding.desc_set, binding.binding,
                    set_binding->descriptor_count);
            return false;
         }
         array_index_range_last = array_index_range_first;
         if (array_index_range_first == 0) {
            /* For consistency between buffer and texture instructions (the latter may have no array
             * source), don't pass an array index of 0 to the caller.
             */
            array_index = NULL;
         }
      } else {
         array_index_range_first = 0;
         array_index_range_last = set_binding->descriptor_count - 1;
         /* Limit to the array size specified in the shader (if not unbounded) for a more precise
          * descriptor demand.
          */
         nir_variable const * const binding_variable = nir_get_binding_variable(shader, binding);
         if (binding_variable != NULL && glsl_type_is_array(binding_variable->type) &&
             binding_variable->type->length != 0) {
            array_index_range_last =
               MIN2(binding_variable->type->length - 1u, array_index_range_last);
         }
      }
   } else {
      array_index_range_first = 0;
      array_index_range_last = 0;
   }

   binding_out->set = &layout->sets[binding.desc_set];
   binding_out->set_binding = set_binding;
   binding_out->array_index = array_index;
   binding_out->array_index_range_first = array_index_range_first;
   binding_out->array_index_range_last = array_index_range_last;
   return true;
}

static bool
terakan_nir_gather_uavs_needed_instr(nir_builder * const b, nir_instr * const instr,
                                     void * const cb_data)
{
   struct terakan_nir_lower_bindings_state * const state =
      (struct terakan_nir_lower_bindings_state *)cb_data;

   nir_src src = NIR_SRC_INIT;
   VkDescriptorType expected_type = VK_DESCRIPTOR_TYPE_MAX_ENUM;

   if (instr->type == nir_instr_type_intrinsic) {
      nir_intrinsic_instr * const intrin = nir_instr_as_intrinsic(instr);
      switch (intrin->intrinsic) {
      case nir_intrinsic_store_ssbo:
         src = intrin->src[1];
         expected_type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
         break;
      case nir_intrinsic_ssbo_atomic:
      case nir_intrinsic_ssbo_atomic_swap:
         src = intrin->src[0];
         expected_type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
         break;
      case nir_intrinsic_image_deref_load:
         if (nir_intrinsic_image_dim(intrin) != GLSL_SAMPLER_DIM_BUF) {
            /* TODO(Triang3l): Detect more precisely whether the image load actually needs a UAV. */
            src = intrin->src[0];
            expected_type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
         }
         break;
      case nir_intrinsic_image_deref_store:
      case nir_intrinsic_image_deref_atomic:
      case nir_intrinsic_image_deref_atomic_swap:
         src = intrin->src[0];
         expected_type = terakan_nir_image_descriptor_type(nir_intrinsic_image_dim(intrin));
         break;
      default:
         break;
      }
   }

   if (src.ssa == NULL) {
      return false;
   }
   assert(expected_type != VK_DESCRIPTOR_TYPE_MAX_ENUM);
   struct terakan_nir_binding binding;
   if (unlikely(!terakan_nir_get_binding(src, expected_type, state->layout, b->shader, &binding))) {
      return false;
   }
   uint8_t mutable_resource_first =
      binding.set->first_shader_resources[b->shader->info.stage] +
      binding.set_binding->first_shader_resources[b->shader->info.stage] -
      TERAKAN_RESOURCE_RANGE_MUTABLE_BASE;
   /* The array index logic must be consistent with the assumptions in terakan_nir_get_binding_uav.
    */
   if (binding.array_index == NULL ||
       nir_def_instr(binding.array_index)->type == nir_instr_type_load_const) {
      /* Constant index - mark only one resource as needing a UAV, and expect this constant index to
       * be added before retrieving the UAV index for the resource index.
       */
      if (binding.array_index != NULL) {
         mutable_resource_first +=
            nir_instr_as_load_const(nir_def_instr(binding.array_index))->value[0].u32;
      }
      BITSET_SET(state->uavs_for_mutable_resources_needed, mutable_resource_first);
   } else {
      /* Non-constant index - demand the whole array, starting from index 0 (disregarding
       * array_index_range_first even if at some point more precise estimation is added to avoid
       * offsetting the index at runtime).
       */
      BITSET_SET_RANGE(state->uavs_for_mutable_resources_needed, mutable_resource_first,
                       mutable_resource_first + binding.array_index_range_last);
   }

   return false;
}

/* Returns UINT_MAX if there are no UAVs exceeding the limit. */
static unsigned
terakan_nir_get_first_out_of_bounds_uav_mutable_resource(
   struct terakan_nir_lower_bindings_state const * const state,
   unsigned const mutable_resource_count)
{
   unsigned const max_uav_count = TERAKAN_COLOR_HW_RTV_AND_UAV_COUNT - state->uav_base;
   unsigned uav_count = 0;
   unsigned mutable_resource_index;
   BITSET_FOREACH_SET (mutable_resource_index, state->uavs_for_mutable_resources_needed,
                       mutable_resource_count) {
      if (uav_count >= max_uav_count) {
         return mutable_resource_index;
      }
      ++uav_count;
   }
   return UINT_MAX;
}

void
terakan_nir_gather_uavs_needed(nir_shader * const shader,
                               struct terakan_nir_lower_bindings_state * const state)
{
   if (state->uavs_for_mutable_resources_needed == NULL) {
      return;
   }

   /* TODO(Triang3l): Research detection of which storage image bindings can skip the UAV path for
    * reads, based on things like ACCESS_RESTRICT, ACCESS_COHERENT (or its Vulkan memory model
    * equivalents).
    */

   nir_shader_instructions_pass(shader, terakan_nir_gather_uavs_needed_instr, nir_metadata_none,
                                state);

   /* Disable UAVs that would be beyond the limit for safety. */
   unsigned const mutable_resource_count = shader->info.stage == MESA_SHADER_FRAGMENT
                                              ? TERAKAN_RESOURCE_RANGE_MUTABLE_MAX_COUNT_PIXEL
                                              : TERAKAN_RESOURCE_RANGE_MUTABLE_MAX_COUNT_NON_PIXEL;
   unsigned const first_out_of_bounds_uav_mutable_resource =
      terakan_nir_get_first_out_of_bounds_uav_mutable_resource(state, mutable_resource_count);
   if (unlikely(first_out_of_bounds_uav_mutable_resource != UINT_MAX)) {
      BITSET_CLEAR_RANGE(state->uavs_for_mutable_resources_needed,
                         first_out_of_bounds_uav_mutable_resource, mutable_resource_count - 1);
   }
}

/* 0-based index (UAV base not applied).
 *
 * If immed_needed is true, also marks that the IMMED resources corresponding to the UAVs are
 * needed.
 *
 * If apply_array_index_out is false as a result, the array index has already been applied to the
 * UAV index, and the caller must use 0 instead of the array index from the binding when accessing
 * the UAV.
 * If apply_array_index_out is true as a result, the array index in the binding is also sure not to
 * be NULL.
 *
 * Returns UINT_MAX if not supported, not found, or exceeding the limit.
 */

unsigned
terakan_nir_get_binding_uav(struct terakan_nir_binding const * const binding,
                            bool const immed_needed,
                            struct terakan_nir_lower_bindings_state const * const state,
                            mesa_shader_stage const stage, bool * const apply_array_index_out)
{
   if (state->uavs_for_mutable_resources_needed == NULL) {
      /* UAVs are not supported at this stage. */
      return UINT_MAX;
   }

   uint8_t mutable_resource_first = binding->set->first_shader_resources[stage] +
                                    binding->set_binding->first_shader_resources[stage] -
                                    TERAKAN_RESOURCE_RANGE_MUTABLE_BASE;
   uint8_t mutable_resource_last;
   /* The array index logic must be consistent with the assumptions in
    * terakan_nir_gather_uavs_needed_instr.
    */
   bool apply_array_index;
   if (binding->array_index == NULL ||
       nir_def_instr(binding->array_index)->type == nir_instr_type_load_const) {
      /* Constant index - don't require the entire array to be bound, pre-apply the array index. */
      if (binding->array_index != NULL) {
         mutable_resource_first +=
            nir_instr_as_load_const(nir_def_instr(binding->array_index))->value[0].u32;
      }
      mutable_resource_last = mutable_resource_first;
      apply_array_index = false;
   } else {
      /* Non-constant index - demand the whole array, starting from index 0 (disregarding
       * array_index_range_first even if at some point more precise estimation is added to avoid
       * offsetting the index at runtime).
       */
      mutable_resource_last = mutable_resource_first + binding->array_index_range_last;
      apply_array_index = true;
   }

   for (uint8_t mutable_resource_index = mutable_resource_first;
        mutable_resource_index <= mutable_resource_last; ++mutable_resource_index) {
      if (!BITSET_TEST(state->uavs_for_mutable_resources_needed, mutable_resource_index)) {
         return UINT_MAX;
      }
   }

   unsigned uav_index = 0;
   unsigned const first_uav_bit_word_index = BITSET_BITWORD(mutable_resource_first);
   for (unsigned word_index = 0; word_index < first_uav_bit_word_index; ++word_index) {
      uav_index += util_bitcount(state->uavs_for_mutable_resources_needed[word_index]);
   }
   uav_index += util_bitcount(state->uavs_for_mutable_resources_needed[first_uav_bit_word_index] &
                              (BITSET_BIT(mutable_resource_first) - 1));

   if (immed_needed) {
      /* IMMED resource indices don't have the color attachment count offset. */
      uint8_t const immed_resource_index_base =
         (stage == MESA_SHADER_FRAGMENT ? TERAKAN_RESOURCE_RANGE_UAV_IMMEDIATE_BASE_PIXEL
                                        : TERAKAN_RESOURCE_RANGE_UAV_IMMEDIATE_BASE_COMPUTE) +
         uav_index;
      if (apply_array_index) {
         BITSET_SET_RANGE(state->resources_needed, immed_resource_index_base,
                          immed_resource_index_base + binding->array_index_range_last);
      } else {
         BITSET_SET(state->resources_needed, immed_resource_index_base);
      }
   }

   *apply_array_index_out = apply_array_index;
   return uav_index;
}

/* Returns 0 in case of an error. */
unsigned
terakan_nir_atomic_uav_op(nir_atomic_op const atomic_op, bool const result_used)
{
   unsigned uav_op;
   switch (atomic_op) {
   case nir_atomic_op_iadd:
      uav_op = V_RAT_INST_ADD;
      break;
   case nir_atomic_op_imin:
      uav_op = V_RAT_INST_MIN_INT;
      break;
   case nir_atomic_op_umin:
      uav_op = V_RAT_INST_MIN_UINT;
      break;
   case nir_atomic_op_imax:
      uav_op = V_RAT_INST_MAX_INT;
      break;
   case nir_atomic_op_umax:
      uav_op = V_RAT_INST_MAX_UINT;
      break;
   case nir_atomic_op_iand:
      uav_op = V_RAT_INST_AND;
      break;
   case nir_atomic_op_ior:
      uav_op = V_RAT_INST_OR;
      break;
   case nir_atomic_op_ixor:
      uav_op = V_RAT_INST_XOR;
      break;
   case nir_atomic_op_xchg:
      /* XCHG_RTN & 0x1F is STORE_RAW, but it's not available on R9xx. */
      return result_used ? V_RAT_INST_XCHG_RTN : V_RAT_INST_STORE_TYPED;
   case nir_atomic_op_cmpxchg:
      uav_op = V_RAT_INST_CMPXCHG_INT;
      break;
   case nir_atomic_op_inc_wrap:
      /* The source is the maximum value. */
      uav_op = V_RAT_INST_INC_UINT;
      break;
   case nir_atomic_op_dec_wrap:
      /* The source is the maximum value. */
      uav_op = V_RAT_INST_DEC_UINT;
      break;
   default:
      assert(!"Unsupported atomic operation");
      return 0;
   }
   if (result_used) {
      uav_op |= 0x20;
   }
   return uav_op;
}
