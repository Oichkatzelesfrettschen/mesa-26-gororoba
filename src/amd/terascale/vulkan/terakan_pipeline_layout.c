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

#include "terakan_pipeline_layout.h"

#include "terakan_command_buffer.h"
#include "terakan_descriptor.h"
#include "terakan_descriptor_set.h"
#include "terakan_descriptor_set_layout.h"
#include "terakan_device.h"
#include "terakan_entrypoints.h"
#include "terakan_hw_state.h"

/* FIX-G (C-2026-04-19-03): forward declaration of the compute LS-register
 * KCACHE bank emitter in terakan_dispatch.c.  No dedicated header for dispatch;
 * declared here where needed. */
void terakan_emit_compute_kcache_bank(struct terakan_gfx_command_writer *command_writer,
                                      uint32_t bank,
                                      struct terakan_bo const *bo,
                                      uint32_t va_kcache_lines,
                                      uint32_t size_lines);

#include "amd/terascale/common/terascale_format.h"
#include "compiler/shader_enums.h"
#include "amd/terascale/common/terascale_evergreend.h"
#include "util/bitscan.h"
#include "util/bitset.h"
#include "util/macros.h"
#include "util/u_debug.h"
#include "util/u_math.h"
#include "vk_alloc.h"
#include "vk_log.h"

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* Update robustness write-guard metadata for a UAV slot in KCACHE bank 14.
 * Routes SSBO bounds to uav_byte_sizes[] and texel buffer element counts to
 * texel_buffer_element_counts[]; zeros the complementary array for
 * defense-in-depth. Pass bound=0 for unbound UAVs.
 *
 * FIX-K (C-2026-04-19-06): base_array_layer is routed to
 * uav_base_array_layers[] (bank 14, dwords 28..39) only when
 * inject_base_array_layer is true (view-is-non-array-over-array-image);
 * otherwise zero is written so the NIR-side nir_iadd folds away.  This
 * respects guardrail #1: plain 2D images over single-layer backing must
 * not have baseArrayLayer added, else MEM_RAT targets a slice beyond
 * the allocation boundary and corrupts adjacent suballocations. */
static inline void
update_uav_robustness_metadata(struct terakan_gfx_command_writer * const cw,
                               uint8_t const uav_idx, bool const is_texel,
                               uint32_t const bound,
                               uint32_t const base_array_layer,
                               bool const inject_base_array_layer)
{
   if (uav_idx >= TERAKAN_COLOR_HW_RTV_AND_UAV_COUNT)
      return;
   cw->robustness_metadata.uav_byte_sizes[uav_idx] = is_texel ? 0 : bound;
   cw->robustness_metadata.texel_buffer_element_counts[uav_idx] = is_texel ? bound : 0;
   cw->robustness_metadata.uav_base_array_layers[uav_idx] =
      inject_base_array_layer ? base_array_layer : 0u;
   cw->robustness_metadata.dirty = true;
}

static VkDescriptorType
terakan_set_resource_descriptor_type(struct terakan_descriptor_set_layout const * const sl,
                                     uint16_t const set_resource_index)
{
   for (uint32_t bi = 0; bi < sl->binding_count; ++bi) {
      struct terakan_descriptor_set_layout_binding const * const b = &sl->bindings[bi];
      if (b->descriptor_count == 0 || b->first_set_resource == UINT16_MAX)
         continue;
      if (set_resource_index >= b->first_set_resource &&
          set_resource_index < b->first_set_resource + b->descriptor_count)
         return b->descriptor_type;
   }
   return VK_DESCRIPTOR_TYPE_MAX_ENUM;
}

VKAPI_ATTR void VKAPI_CALL
terakan_CmdBindDescriptorSets(VkCommandBuffer const commandBuffer,
                              VkPipelineBindPoint const pipelineBindPoint,
                              VkPipelineLayout const layoutHandle, uint32_t const firstSet,
                              uint32_t const descriptorSetCount,
                              VkDescriptorSet const * const pDescriptorSets,
                              UNUSED uint32_t const dynamicOffsetCount,
                              uint32_t const * const pDynamicOffsets)
{
   struct terakan_gfx_command_writer * const command_writer =
      terakan_command_buffer_from_handle(commandBuffer)->command_writer.gfx;
   struct terakan_pipeline_layout const * const layout =
      terakan_pipeline_layout_from_handle(layoutHandle);

   bool const is_compute = pipelineBindPoint == VK_PIPELINE_BIND_POINT_COMPUTE;
   mesa_shader_stage const stage_first = is_compute ? MESA_SHADER_COMPUTE : MESA_SHADER_VERTEX;
   mesa_shader_stage const stage_last = is_compute ? MESA_SHADER_COMPUTE : MESA_SHADER_FRAGMENT;

   mesa_shader_stage const uav_stage = is_compute ? MESA_SHADER_COMPUTE : MESA_SHADER_FRAGMENT;
   BITSET_WORD const * const uavs_used =
      command_writer->state_draw.cb_color_uav.from_apply_sq_pgm_ps.fs_uavs_used;
   BITSET_WORD * const uavs_not_null =
      command_writer->state_draw.cb_color_uav.fs_uavs_not_null;
   struct terakan_state_draw_cb_color_uav * const uavs =
      command_writer->state_draw.cb_color_uav.fs_uavs;
   bool const debug_uav_bind = debug_get_bool_option("TERAKAN_DEBUG_UAV_BIND", false);

   uint32_t const * set_dyn_off = pDynamicOffsets;

   for (uint32_t si = 0; si < descriptorSetCount; ++si) {
      struct terakan_pipeline_layout_set const * const ls = &layout->sets[firstSet + si];
      struct terakan_descriptor_set const * const set =
         terakan_descriptor_set_from_handle(pDescriptorSets[si]);
      if (set == NULL)
         continue;
      struct terakan_descriptor_set_layout const * const sl = set->layout;
      struct terakan_descriptor_set_resource const * const set_res =
         (struct terakan_descriptor_set_resource const *)set->descriptors;
      struct terakan_descriptor_set_sampler const * const set_samp =
         (struct terakan_descriptor_set_sampler const *)(set->descriptors +
                                                         sl->pool_first_sampler_offset_bytes);

      for (mesa_shader_stage stage = stage_first; stage <= stage_last; ++stage) {
         struct terakan_descriptor_set_layout_shader const * const sls = &sl->shaders[stage];
         /* Compute shares the fragment SQC block. */
         mesa_shader_stage const sqc = stage == MESA_SHADER_COMPUTE ? MESA_SHADER_FRAGMENT : stage;

         /* --- Resources (unified VTX/TEX, single pass with inline dynamic offsets) --- */
         {
            terakan_hw_state_sqc_set_resource_function const setter =
               terakan_hw_state_sqc_set_resource_for_stage[sqc];
            uint8_t const set_base = ls->first_shader_resources[stage];
            struct terakan_descriptor_set_layout_shader_range const * const ranges =
               sl->shader_ranges + sls->first_resource_range;
            for (uint8_t ri = 0; ri < sls->resource_range_count; ++ri) {
               struct terakan_descriptor_set_layout_shader_range const * const r = &ranges[ri];
               struct terakan_descriptor_set_resource const * const rr = set_res + r->first_set_descriptor;
               uint8_t const base = set_base + r->first_shader_descriptor;
               bool const has_dyn = r->first_dynamic_offset != UINT16_MAX;
               for (uint8_t di = 0; di < r->descriptor_count; ++di) {
                  struct terakan_descriptor_set_resource desc = rr[di];
                  if (has_dyn && desc.bo != NULL) {
                     assert(G_03001C_TYPE(desc.resource[7]) == V_03001C_SQ_TEX_VTX_VALID_BUFFER);
                     uint64_t const a =
                        (desc.resource[0] |
                         ((uint64_t)G_030008_BASE_ADDRESS_HI(desc.resource[2]) << 32)) +
                        set_dyn_off[r->first_dynamic_offset + di];
                     desc.resource[0] = (uint32_t)a;
                     desc.resource[2] = (desc.resource[2] & C_030008_BASE_ADDRESS_HI) |
                                        S_030008_BASE_ADDRESS_HI(a >> 32);
                  }
                  uint8_t resource_index = base + di;
                  VkDescriptorType const desc_type =
                     terakan_set_resource_descriptor_type(sl, r->first_set_descriptor + di);
                  (void)desc_type;
                  /* Shader-side resource IDs consume the namespace shifted by
                   * R600_MAX_CONST_BUFFERS. Shift all descriptor-backed
                   * resources uniformly to keep every descriptor class in one
                   * collision-free physical RID space.
                   */
                  resource_index += TERAKAN_SAMPLER_HW_COUNT_PER_STAGE;
                  setter(&command_writer->hw_state_sqc, resource_index, desc.bo, desc.resource);
               }
            }
         }

         /* --- KCACHE UBOs (binding-based; banks mirror layout UBO assignment) --- */
         {
            terakan_hw_state_sqc_set_kcache_function const setter =
               terakan_hw_state_sqc_set_kcache_for_stage[sqc];
            uint8_t const bank_base = ls->first_shader_uniform_buffers[stage];
            for (uint32_t bi = 0; bi < sl->binding_count; ++bi) {
               struct terakan_descriptor_set_layout_binding const * const b = &sl->bindings[bi];
               if ((b->descriptor_type != VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER &&
                    b->descriptor_type != VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC) ||
                   b->descriptor_count == 0 || !(b->stage_flags & (1u << stage)))
                  continue;
               struct terakan_descriptor_set_resource const * const br =
                  set_res + b->first_set_resource;
               uint8_t const bk_base = b->first_shader_uniform_buffers[stage];
               for (uint16_t di = 0; di < b->descriptor_count; ++di) {
                  if (br[di].bo == NULL)
                     continue;
                  uint32_t const sz = br[di].resource[1] + 1;
                  if (sz > TERAKAN_KCACHE_HW_MAX_BUFFER_SIZE_BYTES)
                     continue;
                  uint8_t const bank = bank_base + bk_base + di;
                  if (bank >= TERAKAN_KCACHE_MAX_UNIFORM_BUFFERS)
                     continue;
                  uint64_t va = br[di].resource[0] |
                     ((uint64_t)G_030008_BASE_ADDRESS_HI(br[di].resource[2]) << 32);
                  if (b->descriptor_type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC &&
                      b->first_immutable_sampler_or_dynamic_offset != UINT16_MAX)
                     va += set_dyn_off[b->first_immutable_sampler_or_dynamic_offset + di];
                  uint32_t const size_lines =
                     (sz + TERAKAN_KCACHE_HW_LINE_BYTES - 1) >> TERAKAN_KCACHE_HW_LINE_BYTES_LOG2;
                  uint32_t const va_lines =
                     (uint32_t)(va >> TERAKAN_KCACHE_HW_LINE_BYTES_LOG2);
                  setter(&command_writer->hw_state_sqc, bank, size_lines,
                         br[di].bo, va_lines);

                  /* FIX-G (C-2026-04-19-03): the setter above stages the
                   * UBO into hw_state_sqc under the FS-stage bits (compute
                   * shares the FS SQC slot).  That writes PS-side ALU_CONST_
                   * CACHE_PS_* registers later, which the COMPUTE shader
                   * does NOT read -- compute reads LS-side ALU_CONST_CACHE_
                   * LS_*.  Without this additional direct emission, app
                   * UBOs at compute bindings never reach the shader's
                   * KCACHE, the shader's load_ubo reads whatever is at
                   * ALU_CONST_CACHE_LS_<bank> (zero for banks > 0), and
                   * dEQP-VK.image.store.with_format.*.*._single_layer
                   * tests write to image slice 0 regardless of the
                   * u_layerNdx the test provides.
                   *
                   * Emit LS-register KCACHE bank binding directly here for
                   * compute.  Gated behind TERAKAN_FIX_G_UBO_WIRING=1
                   * during validation; promote to default once the 78-test
                   * single_layer sweep goes green.
                   */
                  if (is_compute) {
                     static int fix_g_cached = -1;
                     if (fix_g_cached < 0) {
                        fix_g_cached = debug_get_bool_option(
                           "TERAKAN_FIX_G_UBO_WIRING", false) ? 1 : 0;
                     }
                     if (fix_g_cached) {
                        terakan_emit_compute_kcache_bank(
                           command_writer, bank, br[di].bo, va_lines, size_lines);
                        if (unlikely(debug_uav_bind)) {
                           fprintf(stderr,
                                   "terakan: FIX-G compute-ubo-bind: bank=%u "
                                   "bo=%p va=0x%llX size=%u lines=%u (LS-direct)\n",
                                   bank, (void const *)br[di].bo,
                                   (unsigned long long)va, sz, size_lines);
                        }
                     }
                  }

                  if (unlikely(debug_uav_bind)) {
                     fprintf(stderr,
                             "terakan: ubo-bind: stage=%u binding=%u idx=%u bank=%u bo=%p va=0x%llX size=%u lines=%u\n",
                             stage, bi, di, bank, (void const *)br[di].bo,
                             (unsigned long long)va, sz, size_lines);
                  }
               }
            }
         }

         /* --- Samplers --- */
         {
            terakan_hw_state_sqc_set_sampler_function const setter =
               terakan_hw_state_sqc_set_sampler_for_stage[sqc];
            uint8_t const set_base = ls->first_shader_samplers[stage];
            struct terakan_descriptor_set_layout_shader_range const * const ranges =
               sl->shader_ranges + sls->first_sampler_range;
            for (uint8_t ri = 0; ri < sls->sampler_range_count; ++ri) {
               struct terakan_descriptor_set_sampler const * const rs =
                  set_samp + ranges[ri].first_set_descriptor;
               uint8_t const base = set_base + ranges[ri].first_shader_descriptor;
               for (uint8_t si2 = 0; si2 < ranges[ri].descriptor_count; ++si2)
                  if (likely(G_03C008_TYPE(rs[si2].sampler[2])))
                     setter(&command_writer->hw_state_sqc, base + si2,
                            rs[si2].sampler, rs[si2].border_color);
            }
         }
      }

      /* --- Unordered access views (UAVs with robustness metadata) --- */
      struct terakan_descriptor_set_layout_shader const * const sls_uav = &sl->shaders[uav_stage];
      if (sls_uav->uav_range_count != 0) {
         struct terakan_descriptor_set_uav const * const set_uavs =
            (struct terakan_descriptor_set_uav const *)(set->descriptors +
                                                        sl->pool_first_uav_offset_bytes);
         assert(ls->first_shader_resources[uav_stage] >= TERAKAN_RESOURCE_RANGE_MUTABLE_BASE);
         uint8_t const uav_set_base =
            ls->first_shader_resources[uav_stage] - TERAKAN_RESOURCE_RANGE_MUTABLE_BASE;
         struct terakan_descriptor_set_layout_shader_range const * const uav_ranges =
            sl->shader_ranges + sls_uav->first_uav_range;
         for (uint8_t ri = 0; ri < sls_uav->uav_range_count; ++ri) {
            struct terakan_descriptor_set_layout_shader_range const * const r = &uav_ranges[ri];
            struct terakan_descriptor_set_uav const * const ru = set_uavs + r->first_set_descriptor;
            uint8_t const rbase = uav_set_base + r->first_shader_descriptor;
            for (uint8_t ui = 0; ui < r->descriptor_count; ++ui) {
               uint8_t const idx = rbase + ui;
               assert(idx < (is_compute ? TERAKAN_RESOURCE_RANGE_MUTABLE_MAX_COUNT_NON_PIXEL
                                        : TERAKAN_RESOURCE_RANGE_MUTABLE_MAX_COUNT_PIXEL));
               bool const used = BITSET_TEST(uavs_used, idx);
               struct terakan_descriptor_set_uav const * const uav = &ru[ui];
               if (uav->bo != NULL) {
                  struct terakan_color_descriptor const * color = &uav->color;
                  struct terakan_color_descriptor dyn_color;
                  if (r->first_dynamic_offset != UINT16_MAX) {
                     assert(G_028C70_RESOURCE_TYPE(uav->color.info) == V_028C70_BUFFER);
                     unsigned const bpt =
                        terascale_format_bytes_per_block[G_028C70_FORMAT(uav->color.info)];
                     uint64_t const va =
                        ((uint64_t)uav->color.base << 8) + uav->color.view * bpt +
                        set_dyn_off[r->first_dynamic_offset + ui];
                     unsigned const tip_log2 =
                        terakan_gfx_command_writer_physical_device(command_writer)
                           ->tiling_info.pipe_interleave_bytes_log2;
                     uint64_t const va_al = va >> tip_log2 << tip_log2;
                     dyn_color = uav->color;
                     dyn_color.base = (uint32_t)(va_al >> 8);
                     dyn_color.view = (uint32_t)(va - va_al) * bpt;
                     color = &dyn_color;
                  }
                  struct terakan_state_draw_cb_color_uav * const su = &uavs[idx];
                  if (used) {
                     if (!BITSET_TEST(uavs_not_null, idx) || su->bo != uav->bo ||
                         memcmp(&su->color, color, sizeof(*color)) != 0 ||
                         memcmp(su->real_resource, uav->real_resource,
                                sizeof(su->real_resource)) != 0) {
                        su->bo = uav->bo;
                        su->color = *color;
                        memcpy(su->real_resource, uav->real_resource,
                               sizeof(su->real_resource));
                        terakan_state_draw_set_pending(&command_writer->state_draw,
                                                       TERAKAN_STATE_DRAW_INDEX_CB_COLOR_UAV);
                     }
                  } else {
                     su->bo = uav->bo;
                     su->color = *color;
                     memcpy(su->real_resource, uav->real_resource,
                            sizeof(su->real_resource));
                  }
                  BITSET_SET(uavs_not_null, idx);
                  /* For STORAGE_BUFFER_DYNAMIC, shrink bound by the dynamic offset. */
                  uint32_t bound = uav->buffer_byte_size;
                  if (bound > 0 && r->first_dynamic_offset != UINT16_MAX) {
                     uint32_t const doff = set_dyn_off[r->first_dynamic_offset + ui];
                     bound = (doff < bound) ? (bound - doff) : 0;
                  }
                  bool const inject_layer =
                     (uav->view_flags &
                      TERAKAN_DESCRIPTOR_SET_UAV_VIEW_FLAG_NONARRAY_VIEW_OF_ARRAY_IMAGE) != 0;
                  update_uav_robustness_metadata(command_writer, idx,
                                                 uav->is_texel_buffer, bound,
                                                 uav->base_array_layer, inject_layer);
               } else {
                  if (used && BITSET_TEST(uavs_not_null, idx))
                     terakan_state_draw_set_pending(&command_writer->state_draw,
                                                    TERAKAN_STATE_DRAW_INDEX_CB_COLOR_UAV);
                  BITSET_CLEAR(uavs_not_null, idx);
                  update_uav_robustness_metadata(command_writer, idx, false, 0,
                                                 0, false);
               }
            }
         }
      }

      set_dyn_off += sl->dynamic_offset_count;
   }
}

VkResult
terakan_pipeline_layout_create(struct terakan_device * const device,
                               VkPipelineLayoutCreateInfo const * const create_info,
                               VkShaderStageFlags const stage_mask,
                               struct terakan_pipeline_layout ** const pipeline_layout_out)
{
   assert(!(stage_mask & ~(VK_SHADER_STAGE_ALL_GRAPHICS | VK_SHADER_STAGE_COMPUTE_BIT)));

   VK_MULTIALLOC(multialloc);
   VK_MULTIALLOC_DECL(&multialloc, struct terakan_pipeline_layout, layout, 1);
   VK_MULTIALLOC_DECL(&multialloc, struct terakan_pipeline_layout_set, sets,
                      create_info->setLayoutCount);
   /* Pipeline layouts may outlive VkPipelineLayout; allocate in device scope. */
   if (vk_pipeline_layout_multizalloc(&device->vk, &multialloc, create_info) == NULL)
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);
   layout->sets = sets;

   uint8_t next_res[MESA_SHADER_STAGES] = {};
   uint8_t next_ubo[MESA_SHADER_STAGES] = {};
   uint8_t next_samp[MESA_SHADER_STAGES] = {};

   for (uint32_t set_idx = 0; set_idx < layout->vk.set_count; ++set_idx) {
      struct vk_descriptor_set_layout const * const slb = layout->vk.set_layouts[set_idx];
      if (slb == NULL)
         continue;
      struct terakan_descriptor_set_layout const * const sl =
         container_of(slb, struct terakan_descriptor_set_layout const, vk);
      struct terakan_pipeline_layout_set * const s = &layout->sets[set_idx];

      unsigned remaining = (unsigned)stage_mask;
      while (remaining) {
         unsigned const st = u_bit_scan(&remaining);
         struct terakan_descriptor_set_layout_shader const * const sls = &sl->shaders[st];
         uint8_t const max_res =
            ((VkShaderStageFlags)1 << st == VK_SHADER_STAGE_FRAGMENT_BIT)
               ? TERAKAN_RESOURCE_RANGE_MUTABLE_MAX_COUNT_PIXEL
               : TERAKAN_RESOURCE_RANGE_MUTABLE_MAX_COUNT_NON_PIXEL;
         if (max_res - next_res[st] < sls->resource_count)
            goto too_many_descriptors;
         s->first_shader_resources[st] = TERAKAN_RESOURCE_RANGE_MUTABLE_BASE + next_res[st];
         next_res[st] += sls->resource_count;

         if (next_ubo[st] + sls->uniform_buffer_count > TERAKAN_KCACHE_MAX_UNIFORM_BUFFERS)
            goto too_many_descriptors;
         s->first_shader_uniform_buffers[st] = next_ubo[st];
         next_ubo[st] += sls->uniform_buffer_count;

         if (TERAKAN_SAMPLER_HW_COUNT_PER_STAGE - next_samp[st] < sls->sampler_count)
            goto too_many_descriptors;
         s->first_shader_samplers[st] = next_samp[st];
         layout->shader_non_immutable_samplers[st] |=
            sls->non_immutable_samplers << next_samp[st];
         layout->shader_immutable_samplers_unnormalized_coordinates[st] |=
            sls->immutable_samplers_unnormalized_coordinates << next_samp[st];
         next_samp[st] += sls->sampler_count;
      }
   }

   for (uint32_t pi = 0; pi < create_info->pushConstantRangeCount; ++pi) {
      VkPushConstantRange const * const pc = &create_info->pPushConstantRanges[pi];
      uint32_t const extent = pc->offset + pc->size;
      unsigned remaining = (unsigned)(pc->stageFlags & stage_mask);
      while (remaining) {
         uint32_t * const e =
            &layout->shader_app_push_constants_extents_bytes[u_bit_scan(&remaining)];
         *e = MAX2(extent, *e);
      }
   }

   *pipeline_layout_out = layout;
   return VK_SUCCESS;

too_many_descriptors:
   vk_pipeline_layout_unref(&device->vk, &layout->vk);
   return vk_errorf(
      device, VK_ERROR_VALIDATION_FAILED_EXT,
      "The application creates a pipeline layout that is too large to fit into the hardware "
      "binding register spaces");
}

VKAPI_ATTR VkResult VKAPI_CALL
terakan_CreatePipelineLayout(VkDevice const deviceHandle,
                             VkPipelineLayoutCreateInfo const * const pCreateInfo,
                             UNUSED VkAllocationCallbacks const * const pAllocator,
                             VkPipelineLayout * const pPipelineLayout)
{
   struct terakan_pipeline_layout * layout;
   VkResult const result = terakan_pipeline_layout_create(
      terakan_device_from_handle(deviceHandle), pCreateInfo,
      VK_SHADER_STAGE_ALL_GRAPHICS | VK_SHADER_STAGE_COMPUTE_BIT, &layout);
   if (result != VK_SUCCESS)
      return result;
   *pPipelineLayout = terakan_pipeline_layout_to_handle(layout);
   return VK_SUCCESS;
}
