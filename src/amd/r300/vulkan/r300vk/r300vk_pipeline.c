/*
 * Copyright (c) 2026 Terascale Functionalists
 * SPDX-License-Identifier: MIT
 */

#include "r300vk_pipeline.h"
#include "util/u_simple_shaders.h"
#include "pipe/p_shader_tokens.h"
#include "tgsi/tgsi_from_mesa.h"
#include "tgsi/tgsi_ureg.h"
#include "compiler/nir/nir_opcodes.h"
#include "r300vk_device.h"
#include "r300vk_shader_module.h"

#include "vk_alloc.h"
#include "vk_format.h"
#include "vk_log.h"
#include "vk_nir.h"
#include "vk_object.h"
#include "vk_util.h"

#include "compiler/nir/nir.h"
#include "compiler/spirv/nir_spirv.h"
#include "r300/r300_compute_admission.h"
#include "pipe/p_context.h"
#include "pipe/p_defines.h"
#include "pipe/p_state.h"
#include "util/format/u_format.h"
#include "util/macros.h"

#include <string.h>

/* Defined in the r300 driver (compiler/r300_nir_lower_vs_system_values.c).
 * Declared locally because its header r300_nir.h includes r300_screen.h, which
 * is not on the r300vk include path. */
extern bool r300_nir_lower_vs_system_values_to_inputs(nir_shader *s,
                                                      int vertex_id_slot,
                                                      int instance_id_slot);
extern void r300_nir_vs_reads_system_values(nir_shader *s,
                                            bool *reads_vertex_id,
                                            bool *reads_instance_id);

static const VkVertexInputBindingDescription *
r300vk_find_vertex_binding_desc(const VkPipelineVertexInputStateCreateInfo *vi,
                                uint32_t binding)
{
   if (!vi)
      return NULL;

   for (uint32_t i = 0; i < vi->vertexBindingDescriptionCount; i++) {
      const VkVertexInputBindingDescription *desc =
         &vi->pVertexBindingDescriptions[i];
      if (desc->binding == binding)
         return desc;
   }

   return NULL;
}

static uint32_t
r300vk_vertex_fetch_size(enum pipe_format format)
{
   /* r300_create_vertex_elements_state stores r300_vertex_element_state
    * format_size as a dword-aligned byte count, and r300_emit_vertex_arrays
    * emits it through R300_VBPNTR_SIZE*.  Clamp robust vertex counts against
    * that same fetch span so tightly packed 8/16/24-bit attributes cannot
    * expose a final vertex whose r300 hardware fetch crosses the binding end. */
   return align(util_format_get_blocksize(format), 4);
}

static VkResult
r300vk_validate_vertex_input(struct r300vk_device *device,
                              const VkPipelineVertexInputStateCreateInfo *vi,
                              uint32_t *used_binding_mask)
{
   if (!vi) return VK_SUCCESS;

   for (uint32_t i = 0; i < vi->vertexBindingDescriptionCount; i++) {
      const VkVertexInputBindingDescription *desc =
         &vi->pVertexBindingDescriptions[i];
      if (desc->binding >= R300VK_MAX_VERTEX_BINDINGS)
         return vk_errorf(device, VK_ERROR_FEATURE_NOT_PRESENT,
                          "r300vk: vertex binding %u exceeds %u",
                          desc->binding, R300VK_MAX_VERTEX_BINDINGS - 1);
   }

   for (uint32_t i = 0; i < vi->vertexAttributeDescriptionCount; i++) {
      const VkVertexInputAttributeDescription *attr =
         &vi->pVertexAttributeDescriptions[i];
      if (attr->binding >= R300VK_MAX_VERTEX_BINDINGS)
         return vk_errorf(device, VK_ERROR_FEATURE_NOT_PRESENT,
                          "r300vk: vertex attribute binding %u exceeds %u",
                          attr->binding, R300VK_MAX_VERTEX_BINDINGS - 1);
      if (!r300vk_find_vertex_binding_desc(vi, attr->binding))
         return vk_errorf(device, VK_ERROR_FEATURE_NOT_PRESENT,
                          "r300vk: vertex attribute binding %u has no "
                          "matching binding description", attr->binding);
      *used_binding_mask |= BITFIELD_BIT(attr->binding);
   }
   return VK_SUCCESS;
}

static VkResult
r300vk_reserve_vs_system_value_streams(
   struct r300vk_device *device,
   struct r300vk_pipeline *pl,
   const VkPipelineVertexInputStateCreateInfo *vi,
   bool needs_vertex_id,
   bool needs_instance_id,
   int *vertex_id_slot,
   int *instance_id_slot)
{
   *vertex_id_slot = -1;
   *instance_id_slot = -1;

   if (!needs_vertex_id && !needs_instance_id)
      return VK_SUCCESS;

   const uint32_t synth_count = (uint32_t)needs_vertex_id + (uint32_t)needs_instance_id;
   const uint32_t app_attr_count =
      vi ? vi->vertexAttributeDescriptionCount : 0;
   uint32_t used_binding_mask = 0;

   if (app_attr_count > PIPE_MAX_ATTRIBS)
      return vk_errorf(device, VK_ERROR_FEATURE_NOT_PRESENT,
                       "r300vk: vertex attribute count %u exceeds %u",
                       app_attr_count, PIPE_MAX_ATTRIBS);

   VkResult val_res = r300vk_validate_vertex_input(device, vi, &used_binding_mask);
   if (val_res != VK_SUCCESS)
      return val_res;

   if (app_attr_count > PIPE_MAX_ATTRIBS - synth_count)
      return vk_errorf(device, VK_ERROR_FEATURE_NOT_PRESENT,
                       "r300vk: no vertex input slot available for "
                       "the synthetic VS system-value stream");

   uint8_t synth_bindings[2];
   uint32_t reserved_count = 0;
   for (uint32_t b = 0; b < R300VK_MAX_VERTEX_BINDINGS; b++) {
      if (used_binding_mask & BITFIELD_BIT(b))
         continue;
      synth_bindings[reserved_count++] = (uint8_t)b;
      if (reserved_count == synth_count)
         break;
   }

   if (reserved_count < synth_count)
      return vk_errorf(device, VK_ERROR_FEATURE_NOT_PRESENT,
                       "r300vk: no vertex buffer binding available for "
                       "the synthetic VS system-value stream");

   uint32_t current_slot = app_attr_count;
   uint32_t synth_index = 0;

   pl->needs_vertex_id_stream = needs_vertex_id;
   if (needs_vertex_id) {
      *vertex_id_slot = (int)current_slot;
      pl->vertex_id_slot = (uint8_t)current_slot;
      pl->vertex_id_vb_binding = synth_bindings[synth_index++];
      current_slot++;
   } else {
      pl->vertex_id_slot = 0;
      pl->vertex_id_vb_binding = 0;
   }

   pl->needs_instance_id_stream = needs_instance_id;
   if (needs_instance_id) {
      *instance_id_slot = (int)current_slot;
      pl->instance_id_slot = (uint8_t)current_slot;
      pl->instance_id_vb_binding = synth_bindings[synth_index++];
   } else {
      pl->instance_id_slot = 0;
      pl->instance_id_vb_binding = 0;
   }

   return VK_SUCCESS;
}

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
                       const VkPipelineVertexInputStateCreateInfo *vi)
{
   /* r300g exposes VS and FS only; geometry, tessellation, and compute are
    * unsupported on R300-class hardware. */
   if (stage_info->stage != VK_SHADER_STAGE_VERTEX_BIT &&
       stage_info->stage != VK_SHADER_STAGE_FRAGMENT_BIT)
      return vk_errorf(device, VK_ERROR_FEATURE_NOT_PRESENT,
                       "r300vk: unsupported shader stage 0x%x",
                       stage_info->stage);

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

   /* vk_spirv_to_nir sets data.location for VS inputs (VERT_ATTRIB_GENERIC0+n)
    * but leaves data.driver_location at zero for all variables.  nir_lower_io
    * (called inside r300g's nir_to_rc via r300_nir_lower_for_rc) uses
    * driver_location as the TGSI IN[]/OUT[] base, so all inputs would collapse
    * to IN[0] without this assignment.  Terakan applies the same pattern in
    * terakan_shader.c before handing NIR to r600g. */
   if (stage_info->stage == VK_SHADER_STAGE_VERTEX_BIT) {
      /* RS480-family has no PVS, so gl_VertexIndex / gl_InstanceIndex cannot run
       * on hardware T&L and r300_nir_to_rc_direct would reject them.  When the
       * VS reads them, reserve a vertex-input slot past the application's
       * attributes and lower the intrinsic to a read of it; the draw path fills
       * that slot per draw (firstVertex + i / firstInstance + i). */
      /* vk_spirv_to_nir leaves gl_VertexIndex / gl_InstanceIndex as a load_deref
       * of a nir_var_system_value variable and never gathers system_values_read,
       * so nir->info cannot be trusted to flag the read here.  Scan the NIR for
       * both the deref and the lowered-intrinsic form instead. */
      bool needs_vid = false, needs_iid = false;
      r300_nir_vs_reads_system_values(nir, &needs_vid, &needs_iid);
      if (needs_vid || needs_iid) {
         int vid_slot = -1;
         int iid_slot = -1;
         VkResult r = r300vk_reserve_vs_system_value_streams(
            device, pl, vi, needs_vid, needs_iid, &vid_slot, &iid_slot);
         if (r != VK_SUCCESS) {
            ralloc_free(nir);
            return r;
         }

         if (!r300_nir_lower_vs_system_values_to_inputs(nir, vid_slot,
                                                        iid_slot)) {
            ralloc_free(nir);
            return vk_errorf(device, VK_ERROR_INITIALIZATION_FAILED,
                             "r300vk: failed to lower VS system values");
         }

         bool still_needs_vid = false, still_needs_iid = false;
         r300_nir_vs_reads_system_values(nir, &still_needs_vid,
                                         &still_needs_iid);
         if (still_needs_vid || still_needs_iid) {
            ralloc_free(nir);
            return vk_errorf(device, VK_ERROR_INITIALIZATION_FAILED,
                             "r300vk: VS system-value lowering left an "
                             "unsupported read");
         }
      }

      nir_foreach_shader_in_variable(var, nir) {
         assert(var->data.location >= VERT_ATTRIB_GENERIC0);
         var->data.driver_location = var->data.location - VERT_ATTRIB_GENERIC0;
      }
      nir_assign_io_var_locations(nir, nir_var_shader_out);
   } else {
      nir_assign_io_var_locations(nir, nir_var_shader_in);
   }

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
      pl->vs_hw_valid = r300_vs_get_hw_code(pl->vs_cso, &pl->vs_hw);
   } else {
      pl->fs_cso = device->pipe->create_fs_state(device->pipe, &ss);
      if (!pl->fs_cso) {
         ralloc_free(nir);
         return vk_error(device, VK_ERROR_INITIALIZATION_FAILED);
      }
      pl->fs_hw_valid = r300_fs_get_hw_code(pl->fs_cso, &pl->fs_hw);
   }
   return VK_SUCCESS;
}

/* Build and create the vertex elements CSO.  Extracted to keep
 * r300vk_create_one_pipeline within the CCN budget. */
static VkResult
r300vk_populate_vertex_element(struct r300vk_device *device,
                                struct r300vk_pipeline *pl,
                                const VkPipelineVertexInputStateCreateInfo *vi,
                                uint32_t attr_index,
                                struct pipe_vertex_element *ve)
{
   const VkVertexInputAttributeDescription *attr =
      &vi->pVertexAttributeDescriptions[attr_index];
   if (attr->binding >= R300VK_MAX_VERTEX_BINDINGS)
      return vk_errorf(device, VK_ERROR_INITIALIZATION_FAILED,
                       "r300vk: vertex attribute binding %u exceeds %u",
                       attr->binding, R300VK_MAX_VERTEX_BINDINGS - 1);

   enum pipe_format elem_fmt = vk_format_to_pipe_format(attr->format);
   if (elem_fmt == PIPE_FORMAT_NONE)
      return vk_errorf(device, VK_ERROR_FORMAT_NOT_SUPPORTED,
                       "r300vk: unsupported vertex attribute format %d "
                       "at location %u", attr->format, attr->location);
   const uint32_t attr_size = r300vk_vertex_fetch_size(elem_fmt);
   if (attr->offset > UINT32_MAX - attr_size)
      return vk_errorf(device, VK_ERROR_INITIALIZATION_FAILED,
                       "r300vk: vertex attribute offset %u exceeds "
                       "representable binding extent", attr->offset);

   ve->src_offset          = (uint16_t)attr->offset;
   ve->vertex_buffer_index = (uint8_t)attr->binding;
   ve->src_format          = (uint8_t)elem_fmt;
   const VkVertexInputBindingDescription *binding_desc =
      r300vk_find_vertex_binding_desc(vi, attr->binding);
   if (!binding_desc)
      return vk_errorf(device, VK_ERROR_FEATURE_NOT_PRESENT,
                       "r300vk: vertex attribute binding %u has no "
                       "matching binding description", attr->binding);

   ve->src_stride = binding_desc->stride;
   pl->vertex_stride[attr->binding] = binding_desc->stride;
   pl->vertex_binding_extent[attr->binding] =
      MAX2(pl->vertex_binding_extent[attr->binding],
           attr->offset + attr_size);
   pl->vertex_binding_mask |= BITFIELD_BIT(attr->binding);

   return VK_SUCCESS;
}

static VkResult
r300vk_build_velems_cso(struct r300vk_device *device,
                         struct r300vk_pipeline *pl,
                         const VkPipelineVertexInputStateCreateInfo *vi)
{
   struct pipe_vertex_element ve[PIPE_MAX_ATTRIBS];
   /* vi may be NULL for a VertexIndex-only shader with no application vertex
    * inputs; the synthetic system-value element is still appended below. */
   uint32_t n = vi ? vi->vertexAttributeDescriptionCount : 0;
   if (n > PIPE_MAX_ATTRIBS)
      return vk_errorf(device, VK_ERROR_FEATURE_NOT_PRESENT,
                       "r300vk: vertex attribute count %u exceeds %u",
                       n, PIPE_MAX_ATTRIBS);

   if (vi) {
      for (uint32_t b = 0; b < vi->vertexBindingDescriptionCount; b++) {
         const VkVertexInputBindingDescription *desc =
            &vi->pVertexBindingDescriptions[b];
         if (desc->binding >= R300VK_MAX_VERTEX_BINDINGS)
            return vk_errorf(device, VK_ERROR_FEATURE_NOT_PRESENT,
                             "r300vk: vertex binding %u exceeds %u",
                             desc->binding, R300VK_MAX_VERTEX_BINDINGS - 1);
      }
   }

   memset(ve, 0, sizeof(ve));
   for (uint32_t i = 0; i < n; i++) {
      VkResult res = r300vk_populate_vertex_element(device, pl, vi, i, &ve[i]);
      if (res != VK_SUCCESS)
         return res;
   }

   /* Append synthetic VS-system-value elements (R32_SINT, one int per element)
    * the VS reads via the lowering in r300vk_compile_shader.  instance_divisor
    * 0 steps the vertex-id element per vertex; 1 steps the instance-id element
    * per instance. */
   uint32_t velem_count = n;
   if (pl->needs_vertex_id_stream && velem_count < PIPE_MAX_ATTRIBS) {
      ve[velem_count].src_offset          = 0;
      ve[velem_count].vertex_buffer_index = pl->vertex_id_vb_binding;
      ve[velem_count].src_format          = (uint8_t)PIPE_FORMAT_R32_SINT;
      ve[velem_count].src_stride          = sizeof(int32_t);
      ve[velem_count].instance_divisor    = 0;
      velem_count++;
   }
   if (pl->needs_instance_id_stream && velem_count < PIPE_MAX_ATTRIBS) {
      ve[velem_count].src_offset          = 0;
      ve[velem_count].vertex_buffer_index = pl->instance_id_vb_binding;
      ve[velem_count].src_format          = (uint8_t)PIPE_FORMAT_R32_SINT;
      ve[velem_count].src_stride          = sizeof(int32_t);
      ve[velem_count].instance_divisor    = 1;
      velem_count++;
   }

   pl->velems_cso =
      device->pipe->create_vertex_elements_state(device->pipe, velem_count, ve);
   if (!pl->velems_cso)
      return vk_error(device, VK_ERROR_INITIALIZATION_FAILED);
   return VK_SUCCESS;
}

static VkResult
r300vk_init_graphics_pipeline_cso_state(struct r300vk_device *device,
                                        struct r300vk_pipeline *pl)
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
   if (!pl->blend_cso)
      return vk_error(device, VK_ERROR_INITIALIZATION_FAILED);

   struct pipe_rasterizer_state rs = {0};
   rs.fill_front  = PIPE_POLYGON_MODE_FILL;
   rs.fill_back   = PIPE_POLYGON_MODE_FILL;
   rs.cull_face   = PIPE_FACE_NONE;
   rs.front_ccw   = true;
   rs.depth_clip_near = true;
   rs.depth_clip_far  = true;
   pl->rasterizer_cso = device->pipe->create_rasterizer_state(device->pipe, &rs);
   if (!pl->rasterizer_cso)
      return vk_error(device, VK_ERROR_INITIALIZATION_FAILED);

   struct pipe_depth_stencil_alpha_state dsa = {0};
   pl->dsa_cso = device->pipe->create_depth_stencil_alpha_state(device->pipe, &dsa);
   if (!pl->dsa_cso)
      return vk_error(device, VK_ERROR_INITIALIZATION_FAILED);

   return VK_SUCCESS;
}

static void
r300vk_capture_dynamic_state(struct r300vk_pipeline *pl,
                             const VkGraphicsPipelineCreateInfo *info)
{
   bool dynamic_viewport = false;
   bool dynamic_scissor = false;
   if (info->pDynamicState) {
      for (uint32_t d = 0; d < info->pDynamicState->dynamicStateCount; d++) {
         switch (info->pDynamicState->pDynamicStates[d]) {
         case VK_DYNAMIC_STATE_VIEWPORT:
            dynamic_viewport = true;
            break;
         case VK_DYNAMIC_STATE_SCISSOR:
            dynamic_scissor = true;
            break;
         default:
            break;
         }
      }
   }
   const VkPipelineViewportStateCreateInfo *vp_state = info->pViewportState;
   if (vp_state && !dynamic_viewport &&
       vp_state->pViewports && vp_state->viewportCount > 0) {
      pl->static_viewport = vp_state->pViewports[0];
      pl->has_static_viewport = true;
   }
   if (vp_state && !dynamic_scissor &&
       vp_state->pScissors && vp_state->scissorCount > 0) {
      pl->static_scissor = vp_state->pScissors[0];
      pl->has_static_scissor = true;
   }
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

#define FAIL_PIPELINE(r) \
   do { \
      r300vk_DestroyPipeline(r300vk_device_to_handle(device), \
                             r300vk_pipeline_to_handle(pl), pAllocator); \
      return (r); \
   } while (0)

   for (uint32_t i = 0; i < info->stageCount; i++) {
      VkResult r = r300vk_compile_shader(device, &info->pStages[i], pl,
                                         info->pVertexInputState);
      if (r != VK_SUCCESS)
         FAIL_PIPELINE(r);
   }

   VkResult cso_res = r300vk_init_graphics_pipeline_cso_state(device, pl);
   if (cso_res != VK_SUCCESS)
      FAIL_PIPELINE(cso_res);

   if (info->pVertexInputState || pl->needs_vertex_id_stream ||
       pl->needs_instance_id_stream) {
      VkResult r = r300vk_build_velems_cso(device, pl, info->pVertexInputState);
      if (r != VK_SUCCESS)
         FAIL_PIPELINE(r);
   }

#undef FAIL_PIPELINE

   pl->topology = info->pInputAssemblyState
                  ? info->pInputAssemblyState->topology
                  : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

   r300vk_capture_dynamic_state(pl, info);

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
         if (result == VK_SUCCESS)
            result = r;
      }
   }
   return result;
}

/* Classify one compute kernel against the RS482 compute-as-raster substrate
 * without lowering or executing it.  r300g sets nir_options for VERTEX and
 * FRAGMENT only, so there is no compute entry to translate with; the kernel is
 * translated with the fragment-stage options purely to obtain a well-formed
 * nir_shader to walk.  That is sound for classification because the substrate
 * verbs (FP24 ALU compute, texture-load, RB3D export, blend/stencil/ZPASS
 * reductions) are the fragment pipeline's, and the kernel is never handed to
 * the RC backend.  r300_nir_classify_compute reads the shader and mutates
 * nothing.  Returns false only when SPIR-V translation itself failed. */
static bool
r300vk_classify_compute_kernel(struct r300vk_device *device,
                               const VkPipelineShaderStageCreateInfo *stage_info,
                               struct r300_compute_admission *adm,
                               struct r300_compute_identity_pattern *ident,
                               struct r300_compute_binary_map_pattern *binmap,
                               struct r300_compute_blend_acc_reduction_pattern *blendacc,
                               struct r300_compute_zpass_reduction_pattern *zpass,
                               struct r300_compute_multipass_scan_pattern *multiscan,
                               struct r300_compute_predicated_store_pattern *predstore,
                               struct r300_compute_multitap_gather_pattern *gather,
                               uint32_t local_size[3])
{
   VK_FROM_HANDLE(r300vk_shader_module, mod, stage_info->module);
   if (!mod)
      return false;

   nir_shader *nir = vk_spirv_to_nir(&device->vk, mod->code, mod->code_size,
                                     MESA_SHADER_COMPUTE, stage_info->pName,
                                     stage_info->pSpecializationInfo,
                                     &r300vk_spirv_opts,
                                     device->screen->nir_options[MESA_SHADER_FRAGMENT],
                                     false, NULL);
   if (!nir)
      return false;

   local_size[0] = nir->info.workgroup_size[0];
   local_size[1] = nir->info.workgroup_size[1];
   local_size[2] = nir->info.workgroup_size[2];

   NIR_PASS(_, nir, nir_lower_explicit_io,
            nir_var_mem_ubo | nir_var_mem_ssbo,
            nir_address_format_32bit_index_offset);

   bool progress;
   do {
      progress = false;
      NIR_PASS(progress, nir, nir_opt_dce);
      NIR_PASS(progress, nir, nir_opt_cse);
   } while (progress);

   r300_nir_classify_compute(nir, adm);
   r300_nir_detect_identity_map(nir, ident);
   r300_nir_detect_binary_map(nir, binmap);
   r300_nir_detect_blend_acc_reduction(nir, blendacc);
   r300_nir_detect_zpass_reduction(nir, zpass);
   r300_nir_detect_multipass_scan_pattern(nir, multiscan);
   r300_nir_detect_predicated_store_pattern(nir, predstore);
   r300_nir_detect_multitap_gather_pattern(nir, gather);

   ralloc_free(nir);
   return true;
}

/* Lazily create the gallium state CSOs every identity-map dispatch reuses:
 * blend = passthrough (write color unmodified, all four channels), rasterizer
 * = no cull / fill solid / no scissor / depth clip both planes, dsa = depth
 * test off + stencil off + alpha test off, sampler = NEAREST + CLAMP_TO_EDGE
 * (only NEAREST returns the stored texel unmodified, which the identity-map
 * bit-exact readback requires).
 *
 * Idempotent: subsequent identity-map pipelines find the CSOs populated and
 * skip recreation.  The matching delete_*_state runs in r300vk_DestroyDevice
 * before the pipe_context itself is destroyed. */
static bool
r300vk_device_init_identity_map_state(struct r300vk_device *device)
{
   struct pipe_context *pipe = device->pipe;
   if (!pipe)
      return false;

   if (!device->identity_map_blend_cso) {
      struct pipe_blend_state blend = {0};
      blend.rt[0].colormask = PIPE_MASK_RGBA;
      device->identity_map_blend_cso =
         pipe->create_blend_state(pipe, &blend);
      if (!device->identity_map_blend_cso)
         return false;
   }

   if (!device->identity_map_rasterizer_cso) {
      struct pipe_rasterizer_state raster = {0};
      raster.cull_face       = PIPE_FACE_NONE;
      raster.fill_front      = PIPE_POLYGON_MODE_FILL;
      raster.fill_back       = PIPE_POLYGON_MODE_FILL;
      raster.point_size      = 1.0f;
      raster.line_width      = 1.0f;
      raster.depth_clip_near = 1;
      raster.depth_clip_far  = 1;
      raster.half_pixel_center = 1;
      raster.bottom_edge_rule  = 1;
      device->identity_map_rasterizer_cso =
         pipe->create_rasterizer_state(pipe, &raster);
      if (!device->identity_map_rasterizer_cso)
         return false;
   }

   if (!device->identity_map_dsa_cso) {
      struct pipe_depth_stencil_alpha_state dsa = {0};
      /* All zero: depth test off, stencil off, alpha test off. */
      device->identity_map_dsa_cso =
         pipe->create_depth_stencil_alpha_state(pipe, &dsa);
      if (!device->identity_map_dsa_cso)
         return false;
   }

   if (!device->identity_map_sampler_cso) {
      struct pipe_sampler_state samp = {0};
      samp.wrap_s = PIPE_TEX_WRAP_CLAMP_TO_EDGE;
      samp.wrap_t = PIPE_TEX_WRAP_CLAMP_TO_EDGE;
      samp.wrap_r = PIPE_TEX_WRAP_CLAMP_TO_EDGE;
      samp.min_img_filter = PIPE_TEX_FILTER_NEAREST;
      samp.mag_img_filter = PIPE_TEX_FILTER_NEAREST;
      samp.min_mip_filter = PIPE_TEX_MIPFILTER_NONE;
      samp.max_lod  = 0.0f;
      /* unnormalized_coords stays 0 (default = normalized [0,1] coords);
       * the fullscreen-quad texcoords land in that range exactly. */
      device->identity_map_sampler_cso =
         pipe->create_sampler_state(pipe, &samp);
      if (!device->identity_map_sampler_cso)
         return false;
   }

   return true;
}

/* Emit the binary ALU op into a ureg fragment program.  Maps the detected
 * NIR opcode to its TGSI counterpart via the tgsi_ureg helpers.  The
 * admitted op set mirrors r300_compute_admission.c
 * binary_map_op_admitted().  TGSI ADD / SUB / MUL / MIN / MAX are float;
 * integer NIR opcodes fold into the same float ALU because the texture
 * sampling normalises UNORM8 bytes to [0,1] floats anyway -- the byte
 * round-trip stays bit-exact when the operator obeys the FP24 integer-exact
 * envelope. */
static bool
emit_binary_op(struct ureg_program *ureg, uint16_t nir_op,
               struct ureg_dst dst,
               struct ureg_src a, struct ureg_src b)
{
   switch (nir_op) {
   case nir_op_fadd: case nir_op_iadd:
      ureg_ADD(ureg, dst, a, b); return true;
   case nir_op_fsub: case nir_op_isub:
      /* TGSI has no SUB opcode; ureg_ADD with the second operand negated
       * is the canonical lowering (and what gallium drivers expect). */
      ureg_ADD(ureg, dst, a, ureg_negate(b)); return true;
   case nir_op_fmul: case nir_op_imul:
      ureg_MUL(ureg, dst, a, b); return true;
   case nir_op_fmin: case nir_op_imin: case nir_op_umin:
      ureg_MIN(ureg, dst, a, b); return true;
   case nir_op_fmax: case nir_op_imax: case nir_op_umax:
      ureg_MAX(ureg, dst, a, b); return true;
   default:
      return false;
   }
}

/* Synthesise the 2-sampler fragment program for the binary-map lowering:
 *   TEX  tmp0, IN[0], SAMP[0]   (sample in_a)
 *   TEX  tmp1, IN[0], SAMP[1]   (sample in_b)
 *   <op> tmp0, tmp0, tmp1       (the binary ALU op)
 *   MOV  OUT[0], tmp0
 *   END
 *
 * Costs: 2 TEX + 2 ALU = 4/96 of the R300 PFS budget
 * (R300_PFS_MAX_ALU_INST=64 / R300_PFS_MAX_TEX_INST=32). */
static void *
r300vk_synthesize_binary_map_fs(struct pipe_context *pipe, uint16_t alu_op)
{
   struct ureg_program *ureg = ureg_create(MESA_SHADER_FRAGMENT);
   if (!ureg)
      return NULL;

   struct ureg_src samp_a = ureg_DECL_sampler(ureg, 0);
   struct ureg_src samp_b = ureg_DECL_sampler(ureg, 1);
   ureg_DECL_sampler_view(ureg, 0, TGSI_TEXTURE_2D,
                          TGSI_RETURN_TYPE_FLOAT, TGSI_RETURN_TYPE_FLOAT,
                          TGSI_RETURN_TYPE_FLOAT, TGSI_RETURN_TYPE_FLOAT);
   ureg_DECL_sampler_view(ureg, 1, TGSI_TEXTURE_2D,
                          TGSI_RETURN_TYPE_FLOAT, TGSI_RETURN_TYPE_FLOAT,
                          TGSI_RETURN_TYPE_FLOAT, TGSI_RETURN_TYPE_FLOAT);

   struct ureg_src tex = ureg_DECL_fs_input(ureg, TGSI_SEMANTIC_GENERIC, 0,
                                            TGSI_INTERPOLATE_PERSPECTIVE);
   struct ureg_dst out = ureg_DECL_output(ureg, TGSI_SEMANTIC_COLOR, 0);
   struct ureg_dst tmp_a = ureg_DECL_temporary(ureg);
   struct ureg_dst tmp_b = ureg_DECL_temporary(ureg);

   /* ureg_load_tex (u_simple_shaders.c) is file-static and not exported;
    * call ureg_TEX directly with TGSI_TEXTURE_2D + the (coord, sampler)
    * pair, which is the same opcode the helper emits for use_txf=false. */
   ureg_TEX(ureg, ureg_writemask(tmp_a, TGSI_WRITEMASK_XYZW),
            TGSI_TEXTURE_2D, tex, samp_a);
   ureg_TEX(ureg, ureg_writemask(tmp_b, TGSI_WRITEMASK_XYZW),
            TGSI_TEXTURE_2D, tex, samp_b);

   if (!emit_binary_op(ureg, alu_op,
                       ureg_writemask(tmp_a, TGSI_WRITEMASK_XYZW),
                       ureg_src(tmp_a), ureg_src(tmp_b))) {
      ureg_destroy(ureg);
      return NULL;
   }

   ureg_MOV(ureg, out, ureg_src(tmp_a));
   ureg_END(ureg);
   return ureg_create_shader_and_destroy(ureg, pipe);
}

/* Synthesise the binary-map VS + FS pair on the pipeline.  Reuses the
 * device-cached state CSOs (blend / raster / dsa / sampler) the identity-map
 * synthesis populates -- the binary-map and identity-map paths share every
 * per-draw state object; only the FS differs. */
/* Fullscreen-quad vertex shader synthesis: 2 attributes (POSITION + GENERIC).
 * Identity-map coordinate interpolation and per-vertex reduction values use
 * this passthrough shape.  Cached on the pipeline object; the existing
 * destroy path frees it. */
static void *
r300vk_synthesize_passthrough_vs(struct pipe_context *pipe)
{
   const enum tgsi_semantic names[]   = { TGSI_SEMANTIC_POSITION,
                                          TGSI_SEMANTIC_GENERIC };
   const unsigned          indices[] = { 0, 0 };
   return util_make_vertex_passthrough_shader(pipe, 2, names, indices, false);
}

static bool
r300vk_binary_map_synthesize_shaders(struct r300vk_device *device,
                                      struct r300vk_pipeline *pl)
{
   struct pipe_context *pipe = device->pipe;
   if (!pipe)
      return false;
   if (!r300vk_device_init_identity_map_state(device))
      return false;

   pl->vs_cso = r300vk_synthesize_passthrough_vs(pipe);
   if (!pl->vs_cso)
      return false;

   pl->fs_cso = r300vk_synthesize_binary_map_fs(pipe, pl->binary_map.alu_op);
   if (!pl->fs_cso) {
      pipe->delete_vs_state(pipe, pl->vs_cso);
      pl->vs_cso = NULL;
      return false;
   }
   return true;
}

/* Synthesize the fullscreen-quad VS + texture-sampling FS pair that lowers an
 * identity-map compute kernel onto the compute-as-raster substrate.  The VS
 * passes through a POSITION attribute and one GENERIC varying (texture
 * coordinates); the FS samples PIPE_TEXTURE_2D (NEAREST configured at the
 * sampler-state binding point at dispatch replay time) and writes the texel
 * to the bound color RT.  Both CSOs are cached on the pipeline; the existing
 * destroy path frees vs_cso / fs_cso conditionally.
 *
 * util_make_fragment_tex_shader is the Mesa-canonical helper in
 * src/gallium/auxiliary/util/u_simple_shaders.c (TGSI-based).  Returning false
 * signals a synthesis failure that demotes the pipeline back to a no-op
 * compute object so vkCreateComputePipelines still succeeds with the kernel
 * admitted, just without the identity-map lowering. */
static bool
r300vk_identity_map_synthesize_shaders(struct r300vk_device *device,
                                        struct r300vk_pipeline *pl)
{
   struct pipe_context *pipe = device->pipe;
   if (!pipe)
      return false;

   /* Cached gallium state CSOs live on the device so every identity-map
    * pipeline reuses them.  Initialize on demand from the first identity-map
    * synthesis; subsequent calls find them populated. */
   if (!r300vk_device_init_identity_map_state(device))
      return false;

   pl->vs_cso = r300vk_synthesize_passthrough_vs(pipe);
   if (!pl->vs_cso)
      return false;

   pl->fs_cso = util_make_fragment_tex_shader(
                    pipe, TGSI_TEXTURE_2D,
                    TGSI_RETURN_TYPE_FLOAT, TGSI_RETURN_TYPE_FLOAT,
                    false /* use_txf: NEAREST sample, not integer fetch */,
                    true  /* use_persp: perspective-correct interpolation */);
   if (!pl->fs_cso) {
      pipe->delete_vs_state(pipe, pl->vs_cso);
      pl->vs_cso = NULL;
      return false;
   }
   return true;
}

/* Synthesise the VS + FS pair for the blend-add reduction lowering.  The
 * shaders are structurally identical to r300vk_identity_map_synthesize_shaders
 * -- a vertex_passthrough VS feeding a single-sampler FS that samples the
 * bound 2D texture and writes the texel to OUT[0] COLOR.  The semantic
 * difference between identity-map and blend-acc reduction lives ENTIRELY in
 * the orchestrator at dispatch time:
 *
 *   - identity-map orchestrator binds the output buffer as a W x H RT
 *     matching the input texture extent and draws a fullscreen quad with
 *     blending DISABLED.  Each fragment writes one texel exactly.
 *
 *   - blend-acc orchestrator binds the output buffer as a 1 x M RT (M =
 *     histogram bin count, derived from the output buffer's element count
 *     at orchestrator time) and draws N point primitives at positions
 *     (gid & MASK, 0) with blending ENABLED in COMB_FCN_ADD /
 *     blend_func = (ONE, ONE).  The blend hardware accumulates N writes
 *     into M bins.
 *
 * Sharing the synthesis lets the blend-acc orchestrator reuse the
 * device-cached sampler / raster CSOs the identity-map lowering already
 * populates and adds only the blend-state difference, the same reuse pattern
 * the binary-map synthesis uses for its own state CSOs. */
/* Initialise the device-cached blend-acc-reduction blend state CSO on
 * demand (the only state difference from the identity-map CSO set).
 * Configures the RB3D blend hardware path the compute-as-raster substrate
 * confirmed: COMB_FCN_ADD with blend_func = (ONE, ONE) accumulates dest+src
 * into the RT cell.  The other CSOs (rasterizer / dsa / sampler) come from
 * r300vk_device_init_identity_map_state -- the blend-acc orchestrator binds
 * those unchanged from the identity-map set. */
bool
r300vk_device_init_blend_acc_reduction_state(struct r300vk_device *device);

bool
r300vk_device_init_blend_acc_reduction_state(struct r300vk_device *device)
{
   struct pipe_context *pipe = device->pipe;
   if (!pipe)
      return false;
   if (!r300vk_device_init_identity_map_state(device))
      return false;
   if (device->blend_acc_reduction_blend_cso)
      return true;

   struct pipe_blend_state blend = {0};
   blend.rt[0].blend_enable     = 1;
   blend.rt[0].rgb_func         = PIPE_BLEND_ADD;
   blend.rt[0].alpha_func       = PIPE_BLEND_ADD;
   blend.rt[0].rgb_src_factor   = PIPE_BLENDFACTOR_ONE;
   blend.rt[0].rgb_dst_factor   = PIPE_BLENDFACTOR_ONE;
   blend.rt[0].alpha_src_factor = PIPE_BLENDFACTOR_ONE;
   blend.rt[0].alpha_dst_factor = PIPE_BLENDFACTOR_ONE;
   blend.rt[0].colormask        = PIPE_MASK_RGBA;
   device->blend_acc_reduction_blend_cso =
      pipe->create_blend_state(pipe, &blend);
   return device->blend_acc_reduction_blend_cso != NULL;
}

/* Synthesise the blend-acc-reduction fragment program: a single-MOV
 * passthrough from the GENERIC varying (carrying the per-fragment value
 * the orchestrator baked into the per-point VBO entry) to OUT[0] COLOR.
 * The RB3D blend hardware sums the per-fragment color into the bin cell
 * of the 1xM output RT.  Cost: 1 MOV ALU / 64-slot R300 PFS budget.
 *
 * This is structurally distinct from the identity-map FS, which does
 * TEX + MOV (samples the input texture); the blend-acc FS doesn't sample
 * because the value rides on the per-vertex attribute through the
 * rasterizer interpolator. */
static void *
r300vk_synthesize_blend_acc_reduction_fs(struct pipe_context *pipe)
{
   struct ureg_program *ureg = ureg_create(MESA_SHADER_FRAGMENT);
   if (!ureg)
      return NULL;
   struct ureg_src in_color = ureg_DECL_fs_input(
      ureg, TGSI_SEMANTIC_GENERIC, 0, TGSI_INTERPOLATE_PERSPECTIVE);
   struct ureg_dst out_color = ureg_DECL_output(
      ureg, TGSI_SEMANTIC_COLOR, 0);
   ureg_MOV(ureg, out_color, in_color);
   ureg_END(ureg);
   return ureg_create_shader_and_destroy(ureg, pipe);
}

static bool
r300vk_blend_acc_reduction_synthesize_shaders(struct r300vk_device *device,
                                              struct r300vk_pipeline *pl)
{
   struct pipe_context *pipe = device->pipe;
   if (!pipe)
      return false;
   if (!r300vk_device_init_identity_map_state(device))
      return false;
   if (!r300vk_device_init_blend_acc_reduction_state(device))
      return false;

   /* The VS is the same vertex-passthrough shape as the identity-map and
    * binary-map paths: 2 attributes (POSITION + GENERIC) feed the
    * rasterizer.  The GENERIC attribute carries the per-vertex color the
    * orchestrator stages into the VBO (a packed RGBA8 of the kernel's per-gid
    * input value). */
    pl->vs_cso = r300vk_synthesize_passthrough_vs(pipe);
    if (!pl->vs_cso)
      return false;

    pl->fs_cso = r300vk_synthesize_blend_acc_reduction_fs(pipe);
   if (!pl->fs_cso) {
      pipe->delete_vs_state(pipe, pl->vs_cso);
      pl->vs_cso = NULL;
      return false;
   }
   return true;
}

/* Synthesise the ZPASS-reduction fragment program: one MOV from the
 * GENERIC predicate varying (carrying a per-vertex predicate value the
 * orchestrator baked into the VBO: 1.0 = survive, 0.0 = discard) to a
 * temp, then KILL_IF on (predicate <= 0).  Surviving fragments write a
 * constant white color to OUT[0] (the color is irrelevant -- only the
 * ZPASS counter matters).  Cost: 1 MOV + 1 KILL_IF + 1 MOV out = 3
 * ALU / 64-slot R300 PFS budget.
 *
 * Mesa's tgsi_ureg has no ureg_KILL_IF helper, so we emit through the
 * TGSI macro path: ureg_insn with TGSI_OPCODE_KILL_IF takes one source
 * and discards the fragment when src.x < 0.  We negate the predicate
 * before emitting so KILL_IF(-predicate) discards when predicate < 0 --
 * we want discard when predicate == 0, but the canonical r300 predicate
 * convention here is "1.0 = pass, 0.0 = kill"; KILL_IF(-1.0) does NOT
 * trigger discard (negative-of-positive is negative, KILL_IF discards
 * on negative, so KILL_IF(-1.0) discards), so KILL_IF(predicate-0.5)
 * gives the right shape: discard when predicate < 0.5 (i.e. 0.0 baked
 * value), pass when predicate >= 0.5 (i.e. 1.0). */
static void *
r300vk_synthesize_zpass_reduction_fs(struct pipe_context *pipe)
{
   struct ureg_program *ureg = ureg_create(MESA_SHADER_FRAGMENT);
   if (!ureg)
      return NULL;
   struct ureg_src in_pred = ureg_DECL_fs_input(
      ureg, TGSI_SEMANTIC_GENERIC, 0, TGSI_INTERPOLATE_PERSPECTIVE);
   struct ureg_dst tmp = ureg_DECL_temporary(ureg);
   struct ureg_dst out_color = ureg_DECL_output(
      ureg, TGSI_SEMANTIC_COLOR, 0);
   /* TGSI KILL_IF discards when ANY of src.x/y/z/w is negative.  The
    * baked predicate only lives in the GENERIC varying's x channel;
    * the y/z/w channels follow the GL/D3D convention (0, 0, 1) for an
    * unwritten varying.  A naive `tmp = in_pred - 0.5; KILL_IF tmp`
    * would compute tmp.y = -0.5 and kill EVERY fragment regardless of
    * predicate, returning ZPASS counter = 0.  Broadcasting the predicate
    * to all four channels before the subtract gives KILL_IF
    * (predicate-0.5, predicate-0.5, predicate-0.5, predicate-0.5):
    * discard when predicate < 0.5 (the 0.0-baked discard case), pass when
    * predicate >= 0.5. */
   struct ureg_src half = ureg_imm1f(ureg, 0.5f);
   struct ureg_src pred_xxxx =
      ureg_scalar(in_pred, TGSI_SWIZZLE_X);
   ureg_ADD(ureg, tmp, pred_xxxx, ureg_negate(half));
   ureg_KILL_IF(ureg, ureg_src(tmp));
   /* Surviving fragments write white -- color content doesn't matter for
    * the ZPASS count, just that A fragment lands and the depth/stencil
    * unit increments the counter.  Reusing the predicate varying (which
    * is 1.0 for survivors anyway) keeps the program minimal. */
   ureg_MOV(ureg, out_color, pred_xxxx);
   ureg_END(ureg);
   return ureg_create_shader_and_destroy(ureg, pipe);
}

static bool
r300vk_zpass_reduction_synthesize_shaders(struct r300vk_device *device,
                                          struct r300vk_pipeline *pl)
{
   struct pipe_context *pipe = device->pipe;
   if (!pipe)
      return false;
   if (!r300vk_device_init_identity_map_state(device))
      return false;

   /* Same vertex-passthrough as the other compute-as-raster lowerings:
    * 2 attributes (POSITION + GENERIC predicate-value). */
   pl->vs_cso = r300vk_synthesize_passthrough_vs(pipe);
   if (!pl->vs_cso)
      return false;

   pl->fs_cso = r300vk_synthesize_zpass_reduction_fs(pipe);
   if (!pl->fs_cso) {
      pipe->delete_vs_state(pipe, pl->vs_cso);
      pl->vs_cso = NULL;
      return false;
   }
   return true;
}

/* Synthesise the multipass ping-pong scan fragment program: one per-pass FS
 * the orchestrator binds for every dependent FBO pass.  Each pass samples the
 * prior pass's render target (NEAREST) at the fragment's GENERIC texcoord and
 * writes the texel doubled -- a 1-TEX + 1-MUL shape.  The orchestrator runs
 * this FS pass_count times, swapping the sampler/target RT pair each pass, so
 * the texel doubles once per pass and lands at in * 2^pass_count.
 *
 * First-cut limitation: the FS hard-codes the doubling step, matching the
 * probe kernel `x = x * 2u`.  The detector records step_op so a future cut can
 * generalise to other per-iteration scales; the doubling-only synthesis
 * mirrors the iadd-first scoping of the blend-acc reduction. */
static void *
r300vk_synthesize_multipass_scan_fs(struct pipe_context *pipe)
{
   struct ureg_program *ureg = ureg_create(MESA_SHADER_FRAGMENT);
   if (!ureg)
      return NULL;

   struct ureg_src samp = ureg_DECL_sampler(ureg, 0);
   ureg_DECL_sampler_view(ureg, 0, TGSI_TEXTURE_2D,
                          TGSI_RETURN_TYPE_FLOAT, TGSI_RETURN_TYPE_FLOAT,
                          TGSI_RETURN_TYPE_FLOAT, TGSI_RETURN_TYPE_FLOAT);

   struct ureg_src tex = ureg_DECL_fs_input(ureg, TGSI_SEMANTIC_GENERIC, 0,
                                            TGSI_INTERPOLATE_PERSPECTIVE);
   struct ureg_dst out = ureg_DECL_output(ureg, TGSI_SEMANTIC_COLOR, 0);
   struct ureg_dst tmp = ureg_DECL_temporary(ureg);

   /* Sample the prior pass's RT, then double every channel.  The per-byte
    * UNORM8 doubling matches the kernel's uint *2 only while each byte stays
    * below 256 / 2^pass_count (the probe seeds inputs within that bound); a
    * channel that would exceed 1.0 clamps, which the readback oracle catches
    * as a mismatch rather than silently passing. */
   ureg_TEX(ureg, ureg_writemask(tmp, TGSI_WRITEMASK_XYZW),
            TGSI_TEXTURE_2D, tex, samp);
   struct ureg_src two = ureg_imm4f(ureg, 2.0f, 2.0f, 2.0f, 2.0f);
   ureg_MUL(ureg, out, ureg_src(tmp), two);
   ureg_END(ureg);
   return ureg_create_shader_and_destroy(ureg, pipe);
}

/* Synthesise the multipass-scan VS + per-pass FS.  Same vertex-passthrough as
 * the other compute-as-raster lowerings (POSITION + GENERIC texcoord); the FS
 * is the doubling sampler program the orchestrator rebinds for each ping-pong
 * pass. */
static bool
r300vk_multipass_scan_synthesize_shaders(struct r300vk_device *device,
                                         struct r300vk_pipeline *pl)
{
   struct pipe_context *pipe = device->pipe;
   if (!pipe)
      return false;
   if (!r300vk_device_init_identity_map_state(device))
      return false;

   pl->vs_cso = r300vk_synthesize_passthrough_vs(pipe);
   if (!pl->vs_cso)
      return false;

   pl->fs_cso = r300vk_synthesize_multipass_scan_fs(pipe);
   if (!pl->fs_cso) {
      pipe->delete_vs_state(pipe, pl->vs_cso);
      pl->vs_cso = NULL;
      return false;
   }
   return true;
}

/* Synthesise the predicated masked-store fragment program.  Two samplers:
 * sampler 0 is the predicate texture, sampler 1 is the value texture; both
 * carry the per-element SSBO contents wrapped as PIPE_TEXTURE_2D (NEAREST).
 * The FS samples the predicate, discards the fragment when the predicate is
 * false, samples the value, and writes it.  A discarded fragment performs no
 * ROP write, so the render target keeps whatever the orchestrator seeded into
 * it from out_data -- that is how a masked cell stays at its baseline.
 *
 * The predicate arrives as a UNORM8 texel, so a true predicate (kernel encodes
 * any non-zero low byte) samples to >= 1/255, and a false one to 0.  TGSI
 * KILL_IF discards when ANY of src.x/y/z/w is negative, so the threshold is
 * pred_x - 1/512: 0 -> -1/512 < 0 -> discard (masked); 1/255 -> positive ->
 * pass.  1/512 sits below the smallest non-zero UNORM8 step (1/255), so any
 * non-zero predicate byte passes -- matching the kernel's `!= 0u`.  The
 * predicate is broadcast to all four channels before the subtract (the
 * unwritten y/z/w of a GENERIC varying default to (0,0,1), which would make a
 * naive per-vector subtract discard every fragment).
 *
 * Cost: 2 TEX + 1 ADD + 1 KILL_IF + 1 MOV. */
static void *
r300vk_synthesize_predicated_store_fs(struct pipe_context *pipe)
{
   struct ureg_program *ureg = ureg_create(MESA_SHADER_FRAGMENT);
   if (!ureg)
      return NULL;

   struct ureg_src samp_pred = ureg_DECL_sampler(ureg, 0);
   struct ureg_src samp_val  = ureg_DECL_sampler(ureg, 1);
   ureg_DECL_sampler_view(ureg, 0, TGSI_TEXTURE_2D,
                          TGSI_RETURN_TYPE_FLOAT, TGSI_RETURN_TYPE_FLOAT,
                          TGSI_RETURN_TYPE_FLOAT, TGSI_RETURN_TYPE_FLOAT);
   ureg_DECL_sampler_view(ureg, 1, TGSI_TEXTURE_2D,
                          TGSI_RETURN_TYPE_FLOAT, TGSI_RETURN_TYPE_FLOAT,
                          TGSI_RETURN_TYPE_FLOAT, TGSI_RETURN_TYPE_FLOAT);

   struct ureg_src tex = ureg_DECL_fs_input(ureg, TGSI_SEMANTIC_GENERIC, 0,
                                            TGSI_INTERPOLATE_PERSPECTIVE);
   struct ureg_dst out      = ureg_DECL_output(ureg, TGSI_SEMANTIC_COLOR, 0);
   struct ureg_dst tmp_pred = ureg_DECL_temporary(ureg);
   struct ureg_dst tmp_val  = ureg_DECL_temporary(ureg);
   struct ureg_dst kill     = ureg_DECL_temporary(ureg);

   ureg_TEX(ureg, ureg_writemask(tmp_pred, TGSI_WRITEMASK_XYZW),
            TGSI_TEXTURE_2D, tex, samp_pred);
   struct ureg_src thresh = ureg_imm1f(ureg, 1.0f / 512.0f);
   struct ureg_src pred_xxxx =
      ureg_scalar(ureg_src(tmp_pred), TGSI_SWIZZLE_X);
   ureg_ADD(ureg, kill, pred_xxxx, ureg_negate(thresh));
   ureg_KILL_IF(ureg, ureg_src(kill));

   ureg_TEX(ureg, ureg_writemask(tmp_val, TGSI_WRITEMASK_XYZW),
            TGSI_TEXTURE_2D, tex, samp_val);
   ureg_MOV(ureg, out, ureg_src(tmp_val));
   ureg_END(ureg);
   return ureg_create_shader_and_destroy(ureg, pipe);
}

/* Synthesise the predicated masked-store VS + FS pair.  Same fullscreen-quad
 * vertex passthrough and device-cached state CSOs as the identity / binary
 * lowerings; only the KILL_IF FS differs. */
static bool
r300vk_predicated_store_synthesize_shaders(struct r300vk_device *device,
                                           struct r300vk_pipeline *pl)
{
   struct pipe_context *pipe = device->pipe;
   if (!pipe)
      return false;
   if (!r300vk_device_init_identity_map_state(device))
      return false;

   pl->vs_cso = r300vk_synthesize_passthrough_vs(pipe);
   if (!pl->vs_cso)
      return false;

   pl->fs_cso = r300vk_synthesize_predicated_store_fs(pipe);
   if (!pl->fs_cso) {
      pipe->delete_vs_state(pipe, pl->vs_cso);
      pl->vs_cso = NULL;
      return false;
   }
   return true;
}

/* Synthesise the box-3 multi-tap gather fragment program:
 *   ADD  coord_l, IN[0], -CONST[0]   (texcoord one texel left)
 *   ADD  coord_r, IN[0],  CONST[0]   (texcoord one texel right)
 *   TEX  t_c, IN[0],     SAMP[0]     (center tap, in[gid])
 *   TEX  t_l, coord_l,   SAMP[0]     (left   tap, in[gid-1])
 *   TEX  t_r, coord_r,   SAMP[0]     (right  tap, in[gid+1])
 *   ADD  acc, t_c, t_l
 *   ADD  OUT[0], acc, t_r
 *   END
 *
 * One sampler sampled at three neighborhood offsets, summed in the FP24 ALU.
 * CONST[0] carries the neighbor texel displacement (1/width, 0, 0, 0): the
 * element count <= 2048 lays out as a single texture row (height == 1, per
 * derive_raster_extent in r300vk_identity_map.c), so the displacement is
 * purely in normalized texcoord X and CONST[0].y stays 0 to keep the offset
 * taps in row 0.  width is a dispatch-time quantity (the grid size arrives at
 * CmdDispatch, not pipeline-create), so the orchestrator uploads CONST[0] per
 * dispatch via set_constant_buffer; the FS adds it to the interpolated
 * texcoord.  CLAMP_TO_EDGE on the device sampler defines the boundary taps:
 * gid 0's left tap and gid (N-1)'s right tap clamp to the edge texel, matching
 * a 1D edge-clamped analytic convolution.
 *
 * The FS emits a FIXED box-3 regardless of the detected tap_count -- the
 * canonical-kernel contract (the detector recognizes the N-tap shape; the
 * orchestrator and the probe agree on box-3).  The three TEX read distinct
 * offset coordinates, so CSE does not collapse them to one fetch.
 *
 * Bit-exactness: explicitly performs the 32-bit integer addition carry chain
 * in FP24.  The 0.0-1.0 UNORM8 input samples are scaled to 0-255, summed
 * per-channel with carry extraction via TRUNC and remainders via FRC, then
 * output as exact UNORM8 values.  FP24 mantissa (16-bit) exactly represents
 * the 0..767 intermediate channel sums.
 *
 * Cost: 3 TEX + ~26 ALU = ~30/96 of the R300 PFS budget. */
static void *
r300vk_synthesize_multitap_gather_fs(struct pipe_context *pipe)
{
   struct ureg_program *ureg = ureg_create(MESA_SHADER_FRAGMENT);
   if (!ureg)
      return NULL;

   struct ureg_src samp = ureg_DECL_sampler(ureg, 0);
   ureg_DECL_sampler_view(ureg, 0, TGSI_TEXTURE_2D,
                          TGSI_RETURN_TYPE_FLOAT, TGSI_RETURN_TYPE_FLOAT,
                          TGSI_RETURN_TYPE_FLOAT, TGSI_RETURN_TYPE_FLOAT);

   struct ureg_src delta = ureg_DECL_constant(ureg, 0);
   struct ureg_src tex = ureg_DECL_fs_input(ureg, TGSI_SEMANTIC_GENERIC, 0,
                                            TGSI_INTERPOLATE_PERSPECTIVE);
   struct ureg_dst out     = ureg_DECL_output(ureg, TGSI_SEMANTIC_COLOR, 0);
   struct ureg_dst coord_l = ureg_DECL_temporary(ureg);
   struct ureg_dst coord_r = ureg_DECL_temporary(ureg);
   struct ureg_dst t_c     = ureg_DECL_temporary(ureg);
   struct ureg_dst t_l     = ureg_DECL_temporary(ureg);
   struct ureg_dst t_r     = ureg_DECL_temporary(ureg);
   struct ureg_dst s0      = ureg_DECL_temporary(ureg);
   struct ureg_dst s1      = ureg_DECL_temporary(ureg);
   struct ureg_dst s2      = ureg_DECL_temporary(ureg);
   struct ureg_dst carry   = ureg_DECL_temporary(ureg);

   ureg_ADD(ureg, coord_l, tex, ureg_negate(delta));
   ureg_ADD(ureg, coord_r, tex, delta);

   ureg_TEX(ureg, ureg_writemask(t_c, TGSI_WRITEMASK_XYZW),
            TGSI_TEXTURE_2D, tex, samp);
   ureg_TEX(ureg, ureg_writemask(t_l, TGSI_WRITEMASK_XYZW),
            TGSI_TEXTURE_2D, ureg_src(coord_l), samp);
   ureg_TEX(ureg, ureg_writemask(t_r, TGSI_WRITEMASK_XYZW),
            TGSI_TEXTURE_2D, ureg_src(coord_r), samp);

   /* Scale 0.0-1.0 UNORM8 to 0-255. */
   struct ureg_src scale255 = ureg_imm1f(ureg, 255.0f);
   ureg_MUL(ureg, t_c, ureg_src(t_c), scale255);
   ureg_MUL(ureg, t_l, ureg_src(t_l), scale255);
   ureg_MUL(ureg, t_r, ureg_src(t_r), scale255);

   /* s0 = t_c + t_l + t_r */
   ureg_ADD(ureg, s0, ureg_src(t_c), ureg_src(t_l));
   ureg_ADD(ureg, s0, ureg_src(s0), ureg_src(t_r));

   struct ureg_src inv256 = ureg_imm1f(ureg, 1.0f / 256.0f);
   struct ureg_src scale_out = ureg_imm1f(ureg, 256.0f / 255.0f);

   /* Carry chain: X -> Y -> Z -> W. */
   /* X channel: remainder s0.x % 256, carry s0.x / 256. */
   ureg_MUL(ureg, ureg_writemask(s1, TGSI_WRITEMASK_X),
            ureg_scalar(ureg_src(s0), TGSI_SWIZZLE_X), inv256);
   ureg_TRUNC(ureg, ureg_writemask(carry, TGSI_WRITEMASK_X), ureg_src(s1));
   ureg_FRC(ureg, ureg_writemask(s2, TGSI_WRITEMASK_X), ureg_src(s1));
   ureg_MUL(ureg, ureg_writemask(out, TGSI_WRITEMASK_X), ureg_src(s2), scale_out);

   /* Y channel: s0.y + carry.x */
   ureg_ADD(ureg, ureg_writemask(s0, TGSI_WRITEMASK_Y),
            ureg_scalar(ureg_src(s0), TGSI_SWIZZLE_Y),
            ureg_scalar(ureg_src(carry), TGSI_SWIZZLE_X));
   ureg_MUL(ureg, ureg_writemask(s1, TGSI_WRITEMASK_Y),
            ureg_scalar(ureg_src(s0), TGSI_SWIZZLE_Y), inv256);
   ureg_TRUNC(ureg, ureg_writemask(carry, TGSI_WRITEMASK_Y), ureg_src(s1));
   ureg_FRC(ureg, ureg_writemask(s2, TGSI_WRITEMASK_Y), ureg_src(s1));
   ureg_MUL(ureg, ureg_writemask(out, TGSI_WRITEMASK_Y), ureg_src(s2), scale_out);

   /* Z channel: s0.z + carry.y */
   ureg_ADD(ureg, ureg_writemask(s0, TGSI_WRITEMASK_Z),
            ureg_scalar(ureg_src(s0), TGSI_SWIZZLE_Z),
            ureg_scalar(ureg_src(carry), TGSI_SWIZZLE_Y));
   ureg_MUL(ureg, ureg_writemask(s1, TGSI_WRITEMASK_Z),
            ureg_scalar(ureg_src(s0), TGSI_SWIZZLE_Z), inv256);
   ureg_TRUNC(ureg, ureg_writemask(carry, TGSI_WRITEMASK_Z), ureg_src(s1));
   ureg_FRC(ureg, ureg_writemask(s2, TGSI_WRITEMASK_Z), ureg_src(s1));
   ureg_MUL(ureg, ureg_writemask(out, TGSI_WRITEMASK_Z), ureg_src(s2), scale_out);

   /* W channel: s0.w + carry.z (no carry-out needed). */
   ureg_ADD(ureg, ureg_writemask(s0, TGSI_WRITEMASK_W),
            ureg_scalar(ureg_src(s0), TGSI_SWIZZLE_W),
            ureg_scalar(ureg_src(carry), TGSI_SWIZZLE_Z));
   ureg_MUL(ureg, ureg_writemask(s1, TGSI_WRITEMASK_W),
            ureg_scalar(ureg_src(s0), TGSI_SWIZZLE_W), inv256);
   ureg_FRC(ureg, ureg_writemask(s2, TGSI_WRITEMASK_W), ureg_src(s1));
   ureg_MUL(ureg, ureg_writemask(out, TGSI_WRITEMASK_W), ureg_src(s2), scale_out);

   ureg_END(ureg);
   return ureg_create_shader_and_destroy(ureg, pipe);
}

/* Synthesise the multi-tap gather VS + FS pair.  Same fullscreen-quad vertex
 * passthrough (POSITION + one GENERIC texcoord) and device-cached state CSOs
 * as the identity / binary lowerings; only the box-3 FS differs.  The FS reads
 * CONST[0] (the neighbor texel delta) which the orchestrator uploads per
 * dispatch. */
static bool
r300vk_multitap_gather_synthesize_shaders(struct r300vk_device *device,
                                          struct r300vk_pipeline *pl)
{
   struct pipe_context *pipe = device->pipe;
   if (!pipe)
      return false;
   if (!r300vk_device_init_identity_map_state(device))
      return false;

   pl->vs_cso = r300vk_synthesize_passthrough_vs(pipe);
   if (!pl->vs_cso)
      return false;

   pl->fs_cso = r300vk_synthesize_multitap_gather_fs(pipe);
   if (!pl->fs_cso) {
      pipe->delete_vs_state(pipe, pl->vs_cso);
      pl->vs_cso = NULL;
      return false;
   }
   return true;
}



static bool
r300vk_synthesize_compute_shaders(struct r300vk_device *device,
                                  struct r300vk_pipeline *pl)
{
   if (pl->identity_map.is_identity_map) {
      if (!r300vk_identity_map_synthesize_shaders(device, pl))
         pl->identity_map.is_identity_map = false;
      return true;
   }
   if (pl->binary_map.is_binary_map) {
      if (!r300vk_binary_map_synthesize_shaders(device, pl))
         pl->binary_map.is_binary_map = false;
      return true;
   }
   if (pl->blend_acc_reduction.is_blend_acc_reduction) {
      if (!r300vk_blend_acc_reduction_synthesize_shaders(device, pl))
         pl->blend_acc_reduction.is_blend_acc_reduction = false;
      return true;
   }
   if (pl->zpass_reduction.is_zpass_reduction) {
      if (!r300vk_zpass_reduction_synthesize_shaders(device, pl))
         pl->zpass_reduction.is_zpass_reduction = false;
      return true;
   }
   if (pl->multipass_scan.is_multipass_scan) {
      if (!r300vk_multipass_scan_synthesize_shaders(device, pl))
         pl->multipass_scan.is_multipass_scan = false;
      return true;
   }
   if (pl->predicated_store.is_predicated_store) {
      if (!r300vk_predicated_store_synthesize_shaders(device, pl))
         pl->predicated_store.is_predicated_store = false;
      return true;
   }
   if (pl->multitap_gather.is_multitap_gather) {
      if (!r300vk_multitap_gather_synthesize_shaders(device, pl))
         pl->multitap_gather.is_multitap_gather = false;
      return true;
   }

   return true;
}

static VkResult
r300vk_create_one_compute_pipeline(struct r300vk_device *device,
                                    const VkComputePipelineCreateInfo *pCreateInfo,
                                    const VkAllocationCallbacks *pAllocator,
                                    struct r300vk_pipeline **out_pipeline,
                                    uint32_t i)
{
   struct r300_compute_admission adm;
   struct r300_compute_identity_pattern ident = {0};
   struct r300_compute_binary_map_pattern binmap = {0};
   struct r300_compute_blend_acc_reduction_pattern blendacc = {0};
   struct r300_compute_zpass_reduction_pattern zpass = {0};
   struct r300_compute_multipass_scan_pattern multiscan = {0};
   struct r300_compute_predicated_store_pattern predstore = {0};
   struct r300_compute_multitap_gather_pattern gather = {0};
   uint32_t local_size[3];

   if (!r300vk_classify_compute_kernel(device, &pCreateInfo->stage,
                                       &adm, &ident, &binmap, &blendacc, &zpass,
                                       &multiscan, &predstore, &gather,
                                       local_size))
      return vk_errorf(device, VK_ERROR_INITIALIZATION_FAILED,
                       "r300vk: SPIR-V to NIR failed for compute kernel %u",
                       i);

   if (!adm.admissible)
      return vk_errorf(device, VK_ERROR_FEATURE_NOT_PRESENT,
                       "r300vk: compute kernel %u rejected by the RS482 "
                       "substrate classifier (%s: %s)", i,
                       r300_compute_reject_name(adm.reason),
                       adm.detail ? adm.detail : "unsupported construct");

   struct r300vk_pipeline *pl =
      vk_zalloc2(&device->vk.alloc, pAllocator, sizeof(*pl), 8,
                 VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (!pl)
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);

   vk_object_base_init(&device->vk, &pl->base, VK_OBJECT_TYPE_PIPELINE);
   pl->is_compute = true;
   pl->admission = adm;
   pl->identity_map = ident;
   pl->binary_map = binmap;
   pl->blend_acc_reduction = blendacc;
   pl->zpass_reduction = zpass;
   pl->multipass_scan = multiscan;
   pl->predicated_store = predstore;
   pl->multitap_gather = gather;
   pl->local_size_x = local_size[0];
   pl->local_size_y = local_size[1];
   pl->local_size_z = local_size[2];

   r300vk_synthesize_compute_shaders(device, pl);

   *out_pipeline = pl;
   return VK_SUCCESS;
}


VkResult
r300vk_CreateComputePipelines(VkDevice _device,
                              VkPipelineCache pipelineCache,
                              uint32_t createInfoCount,
                              const VkComputePipelineCreateInfo *pCreateInfos,
                              const VkAllocationCallbacks *pAllocator,
                              VkPipeline *pPipelines)
{
   VK_FROM_HANDLE(r300vk_device, device, _device);
   (void)pipelineCache;

   if (createInfoCount == 0)
      return VK_SUCCESS;

   for (uint32_t i = 0; i < createInfoCount; i++)
      pPipelines[i] = VK_NULL_HANDLE;

   if (!device->hybrid_compute_enabled)
      return vk_errorf(device, VK_ERROR_FEATURE_NOT_PRESENT,
                       "r300vk: compute is not exposed (set "
                       R300VK_HYBRID_COMPUTE_ENV "=1 for the experimental "
                       "hybrid-compute path)");

   for (uint32_t i = 0; i < createInfoCount; i++) {
      struct r300vk_pipeline *pl = NULL;
      VkResult result = r300vk_create_one_compute_pipeline(device, &pCreateInfos[i],
                                                            pAllocator, &pl, i);
      if (result != VK_SUCCESS)
         return result;
      pPipelines[i] = r300vk_pipeline_to_handle(pl);
   }

   return VK_SUCCESS;
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
