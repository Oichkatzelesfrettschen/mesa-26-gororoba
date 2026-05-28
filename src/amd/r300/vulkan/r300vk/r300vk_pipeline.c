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

   const uint32_t synth_count =
      (needs_vertex_id ? 1u : 0u) + (needs_instance_id ? 1u : 0u);
   const uint32_t app_attr_count =
      vi ? vi->vertexAttributeDescriptionCount : 0;
   uint32_t used_binding_mask = 0;

   if (app_attr_count > PIPE_MAX_ATTRIBS)
      return vk_errorf(device, VK_ERROR_FEATURE_NOT_PRESENT,
                       "r300vk: vertex attribute count %u exceeds %u",
                       app_attr_count, PIPE_MAX_ATTRIBS);

   if (vi) {
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
         used_binding_mask |= BITFIELD_BIT(attr->binding);
      }
   }

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

   *vertex_id_slot = needs_vertex_id ? (int)app_attr_count : -1;
   *instance_id_slot =
      needs_instance_id ? (int)(app_attr_count + (needs_vertex_id ? 1 : 0)) : -1;

   pl->needs_vertex_id_stream = needs_vertex_id;
   pl->needs_instance_id_stream = needs_instance_id;
   pl->vertex_id_slot = (uint8_t)app_attr_count;
   pl->instance_id_slot = (uint8_t)(app_attr_count + (needs_vertex_id ? 1 : 0));
   uint32_t synth_index = 0;
   pl->vertex_id_vb_binding =
      needs_vertex_id ? synth_bindings[synth_index++] : 0;
   pl->instance_id_vb_binding =
      needs_instance_id ? synth_bindings[synth_index++] : 0;

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
      const VkVertexInputAttributeDescription *attr =
         &vi->pVertexAttributeDescriptions[i];
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

      ve[i].src_offset          = (uint16_t)attr->offset;
      ve[i].vertex_buffer_index = (uint8_t)attr->binding;
      ve[i].src_format          = (uint8_t)elem_fmt;
      const VkVertexInputBindingDescription *binding_desc =
         r300vk_find_vertex_binding_desc(vi, attr->binding);
      if (!binding_desc)
         return vk_errorf(device, VK_ERROR_FEATURE_NOT_PRESENT,
                          "r300vk: vertex attribute binding %u has no "
                          "matching binding description", attr->binding);

      ve[i].src_stride = binding_desc->stride;
      pl->vertex_stride[attr->binding] = binding_desc->stride;
      pl->vertex_binding_extent[attr->binding] =
         MAX2(pl->vertex_binding_extent[attr->binding],
              attr->offset + attr_size);
      pl->vertex_binding_mask |= BITFIELD_BIT(attr->binding);
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
         FAIL_PIPELINE(vk_error(device, VK_ERROR_INITIALIZATION_FAILED));
   }

   {
      struct pipe_rasterizer_state rs = {0};
      rs.fill_front  = PIPE_POLYGON_MODE_FILL;
      rs.fill_back   = PIPE_POLYGON_MODE_FILL;
      rs.cull_face   = PIPE_FACE_NONE;
      rs.front_ccw   = true;
      rs.depth_clip_near = true;
      rs.depth_clip_far  = true;
      pl->rasterizer_cso = device->pipe->create_rasterizer_state(device->pipe, &rs);
      if (!pl->rasterizer_cso)
         FAIL_PIPELINE(vk_error(device, VK_ERROR_INITIALIZATION_FAILED));
   }

   {
      struct pipe_depth_stencil_alpha_state dsa = {0};
      pl->dsa_cso = device->pipe->create_depth_stencil_alpha_state(device->pipe, &dsa);
      if (!pl->dsa_cso)
         FAIL_PIPELINE(vk_error(device, VK_ERROR_INITIALIZATION_FAILED));
   }

   /* Build the velems CSO when there is application vertex input OR a synthetic
    * system-value stream to append (a VertexIndex-only VS has no app input). */
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
                               struct r300_compute_binary_map_pattern *binmap)
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

   /* vk_spirv_to_nir emits storage-buffer access in deref form
    * (store_deref / load_deref through nir_var_mem_ssbo), with
    * ssbo_addr_format set as the LOWERING TARGET for a later
    * nir_lower_explicit_io pass.  The classifier and the identity-map
    * detector both pattern-match on load_ssbo / store_ssbo intrinsics, so
    * the lowering must run before they walk the NIR -- otherwise the
    * detector misses the identity shape and r300vk_identity_map_dispatch_replay
    * never runs (an empirical bundle 20260528T034632Z showed 256/256
    * readback mismatches with the dispatch lifecycle otherwise clean). */
   NIR_PASS(_, nir, nir_lower_explicit_io,
            nir_var_mem_ubo | nir_var_mem_ssbo,
            nir_address_format_32bit_index_offset);

   /* CSE + DCE between explicit_io and the detectors.  nir_lower_explicit_io
    * emits independent address computations for each load_ssbo / store_ssbo
    * intrinsic even when the source GLSL uses the same index for both --
    * `out[gid] = in[gid]` produces two separate SSA defs for the gid*stride
    * offset, one feeding the load and one feeding the store.  The M-E.6
    * adversarial-review fix added the `store->src[2].ssa == load->src[1].ssa`
    * gate to reject scatter shapes `out[g(i)] = in[i]`, but absent CSE the
    * gate also rejects the legitimate identity-map shape on Vostro RS482
    * (empirical bundle 20260528T143353Z showed both M-E and M-F cases
    * regressed with sentinel_survivors=256/256 because is_identity_map=0
    * and is_binary_map=0 fell through to the M-D no-op path).
    * nir_opt_cse folds the duplicate offset computations to a single SSA
    * def shared between the load and the store, restoring the offset_eq
    * gate to TRUE on the identity-map and binary-map shapes.  The loop is
    * standard mesa pattern (progressing while either DCE or CSE finds
    * work) -- bounded by the NIR size, terminates in O(1) for the small
    * single-statement kernels the detectors actually pattern-match. */
   bool progress;
   do {
      progress = false;
      NIR_PASS(progress, nir, nir_opt_dce);
      NIR_PASS(progress, nir, nir_opt_cse);
   } while (progress);

   r300_nir_classify_compute(nir, adm);
   /* Identity-map + binary-map detection are independent of admission so
    * the orchestrator can use them to select the appropriate compute-as-
    * raster lowering shape for an admitted kernel.  The two detectors are
    * mutually exclusive in their match conditions (identity-map requires
    * the store value to be a load_ssbo def; binary-map requires the store
    * value to be a binary ALU op def) so at most one fires. */
   r300_nir_detect_identity_map(nir, ident);
   r300_nir_detect_binary_map(nir, binmap);

   /* R300VK_DEBUG=identity_map -- log what the classifier + detector
    * decided, so the M-E.4.4 triage can pin whether identity_map was
    * recognised at all and which bindings it captured. */
   {
      const char *flags = getenv("R300VK_DEBUG");
      if (flags && strstr(flags, "identity_map")) {
         unsigned ssbo_loads = 0, ssbo_stores = 0, image_stores = 0;
         nir_foreach_function_impl (impl, nir) {
            nir_foreach_block (block, impl) {
               nir_foreach_instr (instr, block) {
                  if (instr->type != nir_instr_type_intrinsic)
                     continue;
                  const nir_intrinsic_instr *in =
                     nir_instr_as_intrinsic(instr);
                  if (in->intrinsic == nir_intrinsic_load_ssbo)
                     ssbo_loads++;
                  else if (in->intrinsic == nir_intrinsic_store_ssbo)
                     ssbo_stores++;
                  else if (in->intrinsic == nir_intrinsic_image_deref_store ||
                           in->intrinsic == nir_intrinsic_image_store)
                     image_stores++;
               }
            }
         }
         fprintf(stderr,
                 "ident_map: classify admit=%d reason=%d detect "
                 "is_identity_map=%d in_binding=%u out_binding=%u "
                 "is_binary_map=%d binmap_in_a=%u binmap_in_b=%u "
                 "binmap_out=%u binmap_op=%u "
                 "(ssbo_loads=%u ssbo_stores=%u image_stores=%u)\n",
                 (int)adm->admissible, (int)adm->reason,
                 (int)ident->is_identity_map,
                 ident->input_ssbo_binding, ident->output_ssbo_binding,
                 (int)binmap->is_binary_map,
                 binmap->input_a_ssbo_binding, binmap->input_b_ssbo_binding,
                 binmap->output_ssbo_binding, (unsigned)binmap->alu_op,
                 ssbo_loads, ssbo_stores, image_stores);
      }
   }

   ralloc_free(nir);
   return true;
}

/* Lazily create the gallium state CSOs every identity-map dispatch reuses:
 * blend = passthrough (write color unmodified, all four channels), rasterizer
 * = no cull / fill solid / no scissor / depth clip both planes, dsa = depth
 * test off + stencil off + alpha test off, sampler = NEAREST + CLAMP_TO_EDGE
 * (per the M-E exactness theorem: only NEAREST returns the stored texel
 * unmodified, the M-E.identity-map readback requires bit-exactness).
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
 * round-trip stays bit-exact when the operator obeys the FP24-budget
 * envelope (per
 * src/re/r300/docs/rs482-r300vk-compute-texture-pair-binary-map-derivation.md). */
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

/* Synthesise the 2-sampler fragment program for the M-F binary-map lowering:
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

/* Synthesise the M-F (binary-map) VS + FS pair on the pipeline.  Reuses the
 * device-cached state CSOs (blend / raster / dsa / sampler) the identity-map
 * synthesis populates -- M-F and M-E share every per-draw state object;
 * only the FS differs. */
static bool
r300vk_binary_map_synthesize_shaders(struct r300vk_device *device,
                                     struct r300vk_pipeline *pl)
{
   struct pipe_context *pipe = device->pipe;
   if (!pipe)
      return false;
   if (!r300vk_device_init_identity_map_state(device))
      return false;

   const enum tgsi_semantic vs_semantic_names[]   = { TGSI_SEMANTIC_POSITION,
                                                      TGSI_SEMANTIC_GENERIC };
   const unsigned          vs_semantic_indices[] = { 0, 0 };

   pl->vs_cso = util_make_vertex_passthrough_shader(
                   pipe, 2, vs_semantic_names, vs_semantic_indices, false);
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
 * util_make_vertex_passthrough_shader and util_make_fragment_tex_shader are
 * the Mesa-canonical helpers in src/gallium/auxiliary/util/u_simple_shaders.c
 * (TGSI-based; r300g's create_vs_state / create_fs_state accept TGSI directly
 * via PIPE_SHADER_IR_TGSI -- the same path other r300/r600 driver-internal
 * shader synthesis uses, e.g. r600_blit.c).  Returning false signals a
 * synthesis failure that demotes the pipeline back to a no-op compute object
 * so vkCreateComputePipelines still succeeds with the kernel admitted, just
 * without the identity-map lowering. */
static bool
r300vk_identity_map_synthesize_shaders(struct r300vk_device *device,
                                       struct r300vk_pipeline *pl)
{
   struct pipe_context *pipe = device->pipe;
   if (!pipe)
      return false;

   const enum tgsi_semantic vs_semantic_names[]   = { TGSI_SEMANTIC_POSITION,
                                                      TGSI_SEMANTIC_GENERIC };
   const unsigned          vs_semantic_indices[] = { 0, 0 };

   /* Cached gallium state CSOs live on the device so every identity-map
    * pipeline reuses them.  Initialize on demand from the first identity-map
    * synthesis; subsequent calls find them populated. */
   if (!r300vk_device_init_identity_map_state(device))
      return false;

   pl->vs_cso = util_make_vertex_passthrough_shader(
                   pipe, 2, vs_semantic_names, vs_semantic_indices, false);
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
   (void)pAllocator;

   /* createInfoCount == 0 is a no-op that succeeds: there is no pPipelines
    * entry to write and nothing to reject, matching vk_common_CreateComputePipelines. */
   if (createInfoCount == 0)
      return VK_SUCCESS;

   /* The COMPUTE queue is exposed only under the experimental hybrid-compute
    * gate, so a conformant run never reaches this entry point.  Pre-NULL every
    * slot: an inadmissible kernel or an allocation failure leaves its slot
    * VK_NULL_HANDLE per the spec contract, and the diagnostic names WHY a
    * kernel cannot lower to the compute-as-raster substrate. */
   for (uint32_t i = 0; i < createInfoCount; i++)
      pPipelines[i] = VK_NULL_HANDLE;

   if (!device->hybrid_compute_enabled)
      return vk_errorf(device, VK_ERROR_FEATURE_NOT_PRESENT,
                       "r300vk: compute is not exposed (set "
                       R300VK_HYBRID_COMPUTE_ENV "=1 for the experimental "
                       "hybrid-compute path)");

   for (uint32_t i = 0; i < createInfoCount; i++) {
      struct r300_compute_admission adm;
      struct r300_compute_identity_pattern ident = {0};
      struct r300_compute_binary_map_pattern binmap = {0};
      if (!r300vk_classify_compute_kernel(device, &pCreateInfos[i].stage,
                                          &adm, &ident, &binmap))
         return vk_errorf(device, VK_ERROR_INITIALIZATION_FAILED,
                          "r300vk: SPIR-V to NIR failed for compute kernel %u",
                          i);

      if (!adm.admissible)
         return vk_errorf(device, VK_ERROR_FEATURE_NOT_PRESENT,
                          "r300vk: compute kernel %u rejected by the RS482 "
                          "substrate classifier (%s: %s)", i,
                          r300_compute_reject_name(adm.reason),
                          adm.detail ? adm.detail : "unsupported construct");

      /* The kernel is admissible against the substrate.  Create a compute
       * pipeline object: a valid VkPipeline that CmdDispatch can bind and
       * submit.  When the kernel matches the identity-map pattern, the
       * dispatch replay lowers it onto the compute-as-raster substrate as a
       * fullscreen-quad fragment draw (VS+FS synthesis lands in the next
       * stage); otherwise the dispatch remains the no-op object lifecycle.
       * vk_zalloc2 leaves every CSO pointer NULL, which r300vk_DestroyPipeline
       * frees conditionally. */
      struct r300vk_pipeline *pl =
         vk_zalloc2(&device->vk.alloc, pAllocator, sizeof(*pl), 8,
                    VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
      if (!pl)
         return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);
      vk_object_base_init(&device->vk, &pl->base, VK_OBJECT_TYPE_PIPELINE);
      pl->is_compute = true;
      pl->identity_map = ident;
      pl->binary_map = binmap;
      /* When the kernel matches the identity-map pattern, synthesize the
       * fullscreen-quad VS + texture-sampling FS pair the dispatch replay
       * will bind.  A synthesis failure demotes the pipeline back to a no-op
       * compute object: vkCreateComputePipelines still succeeds (the kernel
       * is admitted; the lowering is the optimization) and the dispatch path
       * falls back to the M-D no-op semantics.  M-F binary-map synthesis
       * lands in M-F.2; for now an admitted binary-map kernel uses the M-D
       * no-op dispatch path until M-F.2's two-sampler FS lands. */
      if (pl->identity_map.is_identity_map &&
          !r300vk_identity_map_synthesize_shaders(device, pl))
         pl->identity_map.is_identity_map = false;
      else if (pl->binary_map.is_binary_map &&
               !r300vk_binary_map_synthesize_shaders(device, pl))
         pl->binary_map.is_binary_map = false;
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
