/*
 * Copyright (c) 2026 Terascale Functionalists
 * SPDX-License-Identifier: MIT
 */

#include "r300vk_pipeline.h"
#include "r300vk_device.h"
#include "r300vk_shader_module.h"

#include "vk_alloc.h"
#include "vk_format.h"
#include "vk_log.h"
#include "vk_nir.h"
#include "vk_object.h"
#include "vk_util.h"

#include "compiler/spirv/nir_spirv.h"
#include "pipe/p_context.h"
#include "pipe/p_defines.h"
#include "pipe/p_state.h"

#include <string.h>

static const struct spirv_to_nir_options r300vk_spirv_opts = {
   .environment            = NIR_SPIRV_VULKAN,
   .ubo_addr_format        = nir_address_format_32bit_index_offset,
   .ssbo_addr_format       = nir_address_format_32bit_index_offset,
   .push_const_addr_format = nir_address_format_32bit_offset,
   .shared_addr_format     = nir_address_format_32bit_offset,
};

static VkResult
r300vk_compile_shader(struct r300vk_device *device,
                       const VkPipelineShaderStageCreateInfo *stage_info,
                       struct r300vk_pipeline *pl,
                       VkResult *out_result)
{
   VK_FROM_HANDLE(r300vk_shader_module, mod, stage_info->module);
   mesa_shader_stage stage = vk_to_mesa_shader_stage(stage_info->stage);

   const struct nir_shader_compiler_options *nir_opts =
      device->screen->nir_options[stage];

   nir_shader *nir = vk_spirv_to_nir(&device->vk,
                                      mod->code, mod->code_size,
                                      stage, stage_info->pName,
                                      stage_info->pSpecializationInfo,
                                      &r300vk_spirv_opts, nir_opts,
                                      false, NULL);
   if (!nir)
      return vk_errorf(device, VK_ERROR_INITIALIZATION_FAILED,
                       "r300vk: vk_spirv_to_nir failed for %s shader",
                       stage_info->stage == VK_SHADER_STAGE_VERTEX_BIT
                       ? "vertex" : "fragment");

   struct pipe_shader_state ss = {
      .type   = PIPE_SHADER_IR_NIR,
      .ir.nir = nir,
   };

   if (stage_info->stage == VK_SHADER_STAGE_VERTEX_BIT) {
      pl->vs_cso = device->pipe->create_vs_state(device->pipe, &ss);
      if (!pl->vs_cso) {
         ralloc_free(nir);
         return vk_error(device, VK_ERROR_INITIALIZATION_FAILED);
      }
   } else {
      pl->fs_cso = device->pipe->create_fs_state(device->pipe, &ss);
      if (!pl->fs_cso) {
         ralloc_free(nir);
         return vk_error(device, VK_ERROR_INITIALIZATION_FAILED);
      }
   }
   return VK_SUCCESS;
}

static VkResult
r300vk_create_one_pipeline(struct r300vk_device *device,
                             const VkGraphicsPipelineCreateInfo *info,
                             const VkAllocationCallbacks *pAllocator,
                             VkPipeline *pPipeline)
{
   struct r300vk_pipeline *pl;

   pl = vk_zalloc2(&device->vk.alloc, pAllocator,
                   sizeof(*pl), 8,
                   VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (!pl)
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);

   vk_object_base_init(&device->vk, &pl->base, VK_OBJECT_TYPE_PIPELINE);

   /* Compile each shader stage. */
   for (uint32_t i = 0; i < info->stageCount; i++) {
      VkResult r = r300vk_compile_shader(device, &info->pStages[i],
                                          pl, NULL);
      if (r != VK_SUCCESS) {
         r300vk_DestroyPipeline(r300vk_device_to_handle(device),
                                r300vk_pipeline_to_handle(pl), pAllocator);
         return r;
      }
   }

   /* Blend CSO: no blending, write all components. */
   {
      struct pipe_blend_state bs = {0};
      bs.rt[0].rgb_func        = PIPE_BLEND_ADD;
      bs.rt[0].rgb_src_factor  = PIPE_BLENDFACTOR_ONE;
      bs.rt[0].rgb_dst_factor  = PIPE_BLENDFACTOR_ZERO;
      bs.rt[0].alpha_func      = PIPE_BLEND_ADD;
      bs.rt[0].alpha_src_factor = PIPE_BLENDFACTOR_ONE;
      bs.rt[0].alpha_dst_factor = PIPE_BLENDFACTOR_ZERO;
      bs.rt[0].colormask       = PIPE_MASK_RGBA;
      pl->blend_cso = device->pipe->create_blend_state(device->pipe, &bs);
   }

   /* Rasterizer CSO: fill, cull none. */
   {
      struct pipe_rasterizer_state rs = {0};
      rs.fill_front  = PIPE_POLYGON_MODE_FILL;
      rs.fill_back   = PIPE_POLYGON_MODE_FILL;
      rs.cull_face   = PIPE_FACE_NONE;
      rs.front_ccw   = true;
      rs.depth_clip_near = true;
      rs.depth_clip_far  = true;
      pl->rasterizer_cso = device->pipe->create_rasterizer_state(device->pipe, &rs);
   }

   /* Depth-stencil-alpha CSO: depth test disabled. */
   {
      struct pipe_depth_stencil_alpha_state dsa = {0};
      pl->dsa_cso = device->pipe->create_depth_stencil_alpha_state(device->pipe, &dsa);
   }

   /* Vertex elements CSO from VkVertexInputAttributeDescriptions. */
   if (info->pVertexInputState) {
      const VkPipelineVertexInputStateCreateInfo *vi = info->pVertexInputState;
      struct pipe_vertex_element ve[PIPE_MAX_ATTRIBS];
      uint32_t n = vi->vertexAttributeDescriptionCount;
      if (n > PIPE_MAX_ATTRIBS)
         n = PIPE_MAX_ATTRIBS;

      memset(ve, 0, sizeof(ve));
      for (uint32_t i = 0; i < n; i++) {
         const VkVertexInputAttributeDescription *attr =
            &vi->pVertexAttributeDescriptions[i];
         ve[i].src_offset          = (uint16_t)attr->offset;
         ve[i].vertex_buffer_index = (uint8_t)attr->binding;
         ve[i].src_format          = (uint8_t)vk_format_to_pipe_format(attr->format);
         /* Locate the stride for this binding. */
         for (uint32_t b = 0; b < vi->vertexBindingDescriptionCount; b++) {
            if (vi->pVertexBindingDescriptions[b].binding == attr->binding) {
               ve[i].src_stride = vi->pVertexBindingDescriptions[b].stride;
               pl->vertex_stride[attr->binding] =
                  vi->pVertexBindingDescriptions[b].stride;
               break;
            }
         }
      }
      pl->velems_cso =
         device->pipe->create_vertex_elements_state(device->pipe, n, ve);
   }

   pl->topology = info->pInputAssemblyState
                  ? info->pInputAssemblyState->topology
                  : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

   *pPipeline = r300vk_pipeline_to_handle(pl);
   return VK_SUCCESS;
}

VkResult
r300vk_CreateGraphicsPipelines(VkDevice _device,
                                 VkPipelineCache pipelineCache,
                                 uint32_t createInfoCount,
                                 const VkGraphicsPipelineCreateInfo *pCreateInfos,
                                 const VkAllocationCallbacks *pAllocator,
                                 VkPipeline *pPipelines)
{
   VK_FROM_HANDLE(r300vk_device, device, _device);
   VkResult result = VK_SUCCESS;

   for (uint32_t i = 0; i < createInfoCount; i++) {
      VkResult r = r300vk_create_one_pipeline(device, &pCreateInfos[i],
                                               pAllocator, &pPipelines[i]);
      if (r != VK_SUCCESS) {
         pPipelines[i] = VK_NULL_HANDLE;
         result = r;
      }
   }
   return result;
}

void
r300vk_DestroyPipeline(VkDevice _device,
                        VkPipeline _pipeline,
                        const VkAllocationCallbacks *pAllocator)
{
   VK_FROM_HANDLE(r300vk_device, device, _device);
   VK_FROM_HANDLE(r300vk_pipeline, pl, _pipeline);
   if (!pl)
      return;

   if (pl->vs_cso)
      device->pipe->delete_vs_state(device->pipe, pl->vs_cso);
   if (pl->fs_cso)
      device->pipe->delete_fs_state(device->pipe, pl->fs_cso);
   if (pl->blend_cso)
      device->pipe->delete_blend_state(device->pipe, pl->blend_cso);
   if (pl->rasterizer_cso)
      device->pipe->delete_rasterizer_state(device->pipe, pl->rasterizer_cso);
   if (pl->dsa_cso)
      device->pipe->delete_depth_stencil_alpha_state(device->pipe, pl->dsa_cso);
   if (pl->velems_cso)
      device->pipe->delete_vertex_elements_state(device->pipe, pl->velems_cso);

   vk_object_base_finish(&pl->base);
   vk_free2(&device->vk.alloc, pAllocator, pl);
}
