/*
 * SPDX-License-Identifier: MIT
 */

#include "terakan_pipeline_key.h"

#include "terakan_device.h"
#include "terakan_physical_device.h"

#include "gallium/drivers/r600/r600_shader_common.h"
#include "vk_pipeline.h"

#include <string.h>
#include "util/u_debug.h"

void
terakan_shader_stage_key_fill(struct terakan_shader_stage_key *key,
                              struct terakan_device const *device,
                              VkPipelineShaderStageCreateInfo const *stage_info,
                              VkPipelineCreateFlags2KHR pipeline_flags)
{
   memset(key, 0, sizeof(*key));

   if (pipeline_flags & VK_PIPELINE_CREATE_2_DISABLE_OPTIMIZATION_BIT_KHR)
      key->optimisations_disabled = 1;

   if (pipeline_flags & VK_PIPELINE_CREATE_2_CAPTURE_STATISTICS_BIT_KHR)
      key->keep_statistic_info = 1;

   struct vk_pipeline_robustness_state rs;
   vk_pipeline_robustness_state_fill(&device->vk.robustness_state, &rs, NULL,
                                     stage_info->pNext);

   if (rs.storage_buffers == VK_PIPELINE_ROBUSTNESS_BUFFER_BEHAVIOR_ROBUST_BUFFER_ACCESS_2_EXT)
      key->storage_robustness2 = 1;
   if (rs.uniform_buffers == VK_PIPELINE_ROBUSTNESS_BUFFER_BEHAVIOR_ROBUST_BUFFER_ACCESS_2_EXT)
      key->uniform_robustness2 = 1;
   if (rs.vertex_inputs == VK_PIPELINE_ROBUSTNESS_BUFFER_BEHAVIOR_ROBUST_BUFFER_ACCESS_2_EXT)
      key->vertex_robustness1 = 1;

   if (device->vk.enabled_features.robustBufferAccess)
      key->robust_buffer_access = 1;

   /* NIR-lowering gates that alter the compiled binary must participate in
    * the cache key.
    */
   if (debug_get_bool_option("TERAKAN_STORAGE_IMAGE_BASE_ARRAY_LAYER", true))
      key->fix_k_base_array_layer = 1;
   if (debug_get_bool_option("TERAKAN_FIX_Z_UINT_FORMAT_COMP", true))
      key->fix_z_uint_format_comp = 1;
}

void
terakan_graphics_state_key_fill(struct terakan_graphics_state_key *key,
                                VkGraphicsPipelineCreateInfo const *create_info,
                                VkShaderStageFlags shader_stages,
                                uint8_t ps_nr_cbufs)
{
   memset(key, 0, sizeof(*key));

   key->ps_nr_cbufs = ps_nr_cbufs;

   /* Input assembly topology */
   if (create_info->pInputAssemblyState != NULL)
      key->ia_topology = (uint8_t)create_info->pInputAssemblyState->topology;

   /* Tessellation / geometry stage presence */
   if (shader_stages & VK_SHADER_STAGE_GEOMETRY_BIT)
      key->vs_as_es = 1;
   if (shader_stages & VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT)
      key->vs_as_ls = 1;

   /* Rasterization state */
   if (create_info->pRasterizationState != NULL)
      key->rs_cull_mode = (uint8_t)create_info->pRasterizationState->cullMode;

   /* Multisample state */
   if (create_info->pMultisampleState != NULL) {
      VkPipelineMultisampleStateCreateInfo const *ms = create_info->pMultisampleState;
      if (ms->sampleShadingEnable)
         key->sample_shading_enable = 1;
      switch (ms->rasterizationSamples) {
      case VK_SAMPLE_COUNT_1_BIT:  key->ms_rasterization_samples = 0; break;
      case VK_SAMPLE_COUNT_2_BIT:  key->ms_rasterization_samples = 1; break;
      case VK_SAMPLE_COUNT_4_BIT:  key->ms_rasterization_samples = 2; break;
      case VK_SAMPLE_COUNT_8_BIT:  key->ms_rasterization_samples = 3; break;
      default:                     key->ms_rasterization_samples = 0; break;
      }
      if (ms->alphaToOneEnable)
         key->alpha_to_one = 1;
   }

   /* Color blend state — detect dual-source blending */
   if (create_info->pColorBlendState != NULL) {
      VkPipelineColorBlendStateCreateInfo const *cb = create_info->pColorBlendState;
      for (uint32_t i = 0; i < cb->attachmentCount; i++) {
         VkPipelineColorBlendAttachmentState const *att = &cb->pAttachments[i];
         if (!att->blendEnable)
            continue;
         if (att->srcColorBlendFactor == VK_BLEND_FACTOR_SRC1_COLOR ||
             att->srcColorBlendFactor == VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR ||
             att->srcColorBlendFactor == VK_BLEND_FACTOR_SRC1_ALPHA ||
             att->srcColorBlendFactor == VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA ||
             att->dstColorBlendFactor == VK_BLEND_FACTOR_SRC1_COLOR ||
             att->dstColorBlendFactor == VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR ||
             att->dstColorBlendFactor == VK_BLEND_FACTOR_SRC1_ALPHA ||
             att->dstColorBlendFactor == VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA ||
             att->srcAlphaBlendFactor == VK_BLEND_FACTOR_SRC1_COLOR ||
             att->srcAlphaBlendFactor == VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR ||
             att->srcAlphaBlendFactor == VK_BLEND_FACTOR_SRC1_ALPHA ||
             att->srcAlphaBlendFactor == VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA ||
             att->dstAlphaBlendFactor == VK_BLEND_FACTOR_SRC1_COLOR ||
             att->dstAlphaBlendFactor == VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR ||
             att->dstAlphaBlendFactor == VK_BLEND_FACTOR_SRC1_ALPHA ||
             att->dstAlphaBlendFactor == VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA) {
            key->dual_source_blend = 1;
            break;
         }
      }
   }

   /* Point size removal — safe ONLY when ALL conditions hold:
    * 1. Topology is statically known (not VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY)
    * 2. Topology is not POINT_LIST
    * 3. VS is the true last vertex stage (not feeding GS as ES, or TES as LS)
    *
    * If VS feeds GS/TES, the intermediate stage determines final point size.
    * If topology is dynamic, it may become POINT_LIST at draw time.
    * (Rubber-duck finding: dynamic topology + last-vertex-stage safety.) */
   bool topology_is_dynamic = false;
   if (create_info->pDynamicState != NULL) {
      for (uint32_t i = 0; i < create_info->pDynamicState->dynamicStateCount; ++i) {
         if (create_info->pDynamicState->pDynamicStates[i] ==
             VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY) {
            topology_is_dynamic = true;
            break;
         }
      }
   }
   if (!topology_is_dynamic &&
       key->ia_topology != VK_PRIMITIVE_TOPOLOGY_POINT_LIST &&
       !key->vs_as_es && !key->vs_as_ls)
      key->enable_remove_point_size = 1;
}

void
terakan_r600_shader_key_from_state(union r600_shader_key *r600_key,
                                   struct terakan_graphics_state_key const *state_key,
                                   mesa_shader_stage stage)
{
   memset(r600_key, 0, sizeof(*r600_key));

   switch (stage) {
   case MESA_SHADER_FRAGMENT:
      r600_key->ps.nr_cbufs = state_key->ps_nr_cbufs;
      r600_key->ps.color_two_side = state_key->color_two_side;
      r600_key->ps.alpha_to_one = state_key->alpha_to_one;
      r600_key->ps.apply_sample_id_mask = state_key->apply_sample_id_mask;
      r600_key->ps.dual_source_blend = state_key->dual_source_blend;
      break;
   case MESA_SHADER_VERTEX:
      r600_key->vs.as_es = state_key->vs_as_es;
      r600_key->vs.as_ls = state_key->vs_as_ls;
      r600_key->vs.as_gs_a = state_key->vs_as_gs_a;
      break;
   default:
      break;
   }
}
