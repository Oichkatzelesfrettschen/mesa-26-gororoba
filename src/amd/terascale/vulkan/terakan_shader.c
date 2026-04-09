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

#include "terakan_shader.h"

#include "nir/terakan_nir.h"
#include "terakan_bo.h"
#include "terakan_command_buffer.h"
#include "terakan_descriptor.h"
#include "terakan_device.h"
#include "terakan_physical_device.h"

#include "amd/terascale/common/terascale_wddm.h"
#include "compiler/glsl_types.h"
#include "amd/terascale/common/terascale_evergreend.h"
#include "gallium/drivers/r600/sfn/sfn_nir_lower_tex.h"
#include "spirv/nir_spirv.h"
#include "util/bitscan.h"
#include "util/macros.h"
#include "vk_nir.h"

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

struct terakan_shader_ring const terakan_shader_rings[TERAKAN_SHADER_RING_INDEX_COUNT] = {
   [TERAKAN_SHADER_RING_INDEX_LSTMP] =
      {
         .base_wddm_patch_ids = TERASCALE_WDDM_PATCH_IDS_SQ_LSTMP_RING_BASE,
         .base_size_config_reg_offset = TERAKAN_CONFIG_REG_OFFSET(R_008E10_SQ_LSTMP_RING_BASE),
         .item_size_context_reg_offset =
            TERAKAN_CONTEXT_REG_OFFSET(R_028830_SQ_LSTMP_RING_ITEMSIZE),
         .stages = VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
         .sx_surface_sync_mask = (uint32_t)1 << 4,
      },
   [TERAKAN_SHADER_RING_INDEX_HSTMP] =
      {
         .base_wddm_patch_ids = TERASCALE_WDDM_PATCH_IDS_SQ_HSTMP_RING_BASE,
         .base_size_config_reg_offset = TERAKAN_CONFIG_REG_OFFSET(R_008E18_SQ_HSTMP_RING_BASE),
         .item_size_context_reg_offset =
            TERAKAN_CONTEXT_REG_OFFSET(R_028834_SQ_HSTMP_RING_ITEMSIZE),
         .stages = VK_PIPELINE_STAGE_2_TESSELLATION_CONTROL_SHADER_BIT,
         .sx_surface_sync_mask = (uint32_t)1 << 4,
      },
   [TERAKAN_SHADER_RING_INDEX_ESTMP] =
      {
         .base_wddm_patch_ids = TERASCALE_WDDM_PATCH_IDS_SQ_ESTMP_RING_BASE,
         .base_size_config_reg_offset = TERAKAN_CONFIG_REG_OFFSET(R_008C50_SQ_ESTMP_RING_BASE),
         .item_size_context_reg_offset =
            TERAKAN_CONTEXT_REG_OFFSET(R_028908_SQ_ESTMP_RING_ITEMSIZE),
         .stages = VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT |
                   VK_PIPELINE_STAGE_2_TESSELLATION_EVALUATION_SHADER_BIT,
         .sx_surface_sync_mask = (uint32_t)1 << 4,
      },
   [TERAKAN_SHADER_RING_INDEX_GSTMP] =
      {
         .base_wddm_patch_ids = TERASCALE_WDDM_PATCH_IDS_SQ_GSTMP_RING_BASE,
         .base_size_config_reg_offset = TERAKAN_CONFIG_REG_OFFSET(R_008C58_SQ_GSTMP_RING_BASE),
         .item_size_context_reg_offset =
            TERAKAN_CONTEXT_REG_OFFSET(R_02890C_SQ_GSTMP_RING_ITEMSIZE),
         .stages = VK_PIPELINE_STAGE_2_GEOMETRY_SHADER_BIT,
         .sx_surface_sync_mask = (uint32_t)1 << 4,
      },
   [TERAKAN_SHADER_RING_INDEX_VSTMP] =
      {
         .base_wddm_patch_ids = TERASCALE_WDDM_PATCH_IDS_SQ_VSTMP_RING_BASE,
         .base_size_config_reg_offset = TERAKAN_CONFIG_REG_OFFSET(R_008C60_SQ_VSTMP_RING_BASE),
         .item_size_context_reg_offset =
            TERAKAN_CONTEXT_REG_OFFSET(R_028910_SQ_VSTMP_RING_ITEMSIZE),
         .stages = VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT |
                   VK_PIPELINE_STAGE_2_TESSELLATION_EVALUATION_SHADER_BIT,
         .sx_surface_sync_mask = (uint32_t)1 << 4,
      },
   [TERAKAN_SHADER_RING_INDEX_PSTMP] =
      {
         .base_wddm_patch_ids = TERASCALE_WDDM_PATCH_IDS_SQ_PSTMP_RING_BASE,
         .base_size_config_reg_offset = TERAKAN_CONFIG_REG_OFFSET(R_008C68_SQ_PSTMP_RING_BASE),
         .item_size_context_reg_offset =
            TERAKAN_CONTEXT_REG_OFFSET(R_028914_SQ_PSTMP_RING_ITEMSIZE),
         .stages = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
         .sx_surface_sync_mask = (uint32_t)1 << 4,
      },
};

/* From gl_nir_linker.c. */
static void
terakan_nir_shared_type_info(struct glsl_type const * const type, unsigned * const size,
                             unsigned * const align)
{
   assert(glsl_type_is_vector_or_scalar(type));
   uint32_t const comp_size = glsl_type_is_boolean(type) ? 4 : glsl_get_bit_size(type) / 8;
   unsigned const length = glsl_get_vector_elements(type);
   *size = comp_size * length;
   *align = comp_size * (length == 3 ? 4 : length);
}

nir_shader *
terakan_shader_spirv_to_nir(struct terakan_device * const device, size_t const spirv_size_bytes,
                            uint32_t const * const spirv, mesa_shader_stage const stage,
                            char const * const entrypoint,
                            VkSpecializationInfo const * const specialization_info)
{
   struct terakan_physical_device const * const physical_device =
      terakan_device_physical_device(device);

   static struct spirv_to_nir_options const spirv_options = {
      .environment = NIR_SPIRV_VULKAN,

      /* TODO(Triang3l): Possibly the subgroup size is properly
       * exposed.
       */

      /* TODO(Triang3l): Capabilities when supported and tested. */

      .ubo_addr_format = nir_address_format_32bit_index_offset,
      .ssbo_addr_format = nir_address_format_32bit_index_offset,
      .push_const_addr_format = nir_address_format_32bit_offset,
      .shared_addr_format = nir_address_format_32bit_offset,

      .min_ubo_alignment = TERAKAN_KCACHE_HW_LINE_BYTES,
      .min_ssbo_alignment = sizeof(uint32_t),
   };

   nir_shader * nir =
      vk_spirv_to_nir(&device->vk, spirv, spirv_size_bytes, stage, entrypoint,
                      specialization_info, &spirv_options,
                      stage == MESA_SHADER_FRAGMENT ? &physical_device->nir_options_fs
                                                    : &physical_device->nir_options_non_fs,
                      false, NULL);

   /* SFN expects certain fragment shader system values to be accessed via load_input rather than
    * the system value load intrinsics, make sure that's the case before nir_lower_system_values is
    * done that would otherwise generate system value load intrinsics.
    */

   if (nir->info.stage == MESA_SHADER_FRAGMENT) {
      struct nir_lower_sysvals_to_varyings_options const lower_sysvals_to_varyings_options = {
         .frag_coord = true,
         .front_face = true,
         .point_coord = true,
      };
      NIR_PASS(_, nir, nir_lower_sysvals_to_varyings, &lower_sysvals_to_varyings_options);
   }

   /* Assign meanings and indices to variables in cases that don't depend on the actual executable
    * code once all variables are set up (including via nir_lower_sysvals_to_varyings).
    */

   if (nir->info.stage != MESA_SHADER_COMPUTE) {
      if (nir->info.stage == MESA_SHADER_VERTEX) {
         nir_foreach_shader_in_variable (var, nir) {
            assert(var->data.location >= VERT_ATTRIB_GENERIC0);
            var->data.driver_location = var->data.location - VERT_ATTRIB_GENERIC0;
         }
      } else {
         nir_assign_io_var_locations(nir, nir_var_shader_in);
      }
      /* Fragment shader outputs are compacted in the end, not assigning locations here. */
      if (nir->info.stage != MESA_SHADER_FRAGMENT) {
         nir_assign_io_var_locations(nir, nir_var_shader_out);
      }
   }

   /* Make sure output writes are done only once, so they can be treated as exports, and also make
    * fragment inputs interpolated once.
    */

   if (nir->info.stage != MESA_SHADER_COMPUTE) {
      nir_lower_io_vars_to_temporaries(nir, nir_shader_get_entrypoint(nir), nir_var_shader_out);
   }

   /* Lower interface and binding derefs. */

   NIR_PASS(_, nir, nir_lower_system_values);
   if (nir->info.stage == MESA_SHADER_COMPUTE) {
      nir_lower_compute_system_values_options const lower_compute_system_values_options = {
         .global_id_is_32bit = true,
      };
      NIR_PASS(_, nir, nir_lower_compute_system_values, &lower_compute_system_values_options);
   }

   if (nir->info.stage == MESA_SHADER_COMPUTE) {
      NIR_PASS(_, nir, nir_lower_vars_to_explicit_types, nir_var_mem_shared,
               terakan_nir_shared_type_info);
   }

   assert(spirv_options.ubo_addr_format == nir_address_format_32bit_index_offset);
   assert(spirv_options.ssbo_addr_format == nir_address_format_32bit_index_offset);
   NIR_PASS(_, nir, nir_lower_explicit_io, nir_var_mem_ubo | nir_var_mem_ssbo,
            nir_address_format_32bit_index_offset);

   assert(spirv_options.push_const_addr_format == nir_address_format_32bit_offset);
   assert(spirv_options.shared_addr_format == nir_address_format_32bit_offset);
   NIR_PASS(
      _, nir, nir_lower_explicit_io,
      nir_var_mem_push_const | (nir->info.stage == MESA_SHADER_COMPUTE ? nir_var_mem_shared : 0),
      nir_address_format_32bit_offset);

   /* Lower basic instructions that won't be generated by other lowerings. */

   NIR_PASS(_, nir, terakan_nir_lower_sin_cos);

   return nir;
}

/* `data` points to `nir_variable_mode robust_modes`. */
static bool
terakan_nir_should_vectorize_load_store(unsigned const align_mul, unsigned const align_offset,
                                        unsigned const bit_size, unsigned const num_components,
                                        int64_t hole_size, nir_intrinsic_instr * const low,
                                        UNUSED nir_intrinsic_instr * const high, void * const data)
{
   /* TODO(Triang3l): Don't vectorize kcache loads, and also don't combine kcache and resource loads
    * (such as when the constant address of `high` is above the maximum kcache buffer size).
    */

   if (low->intrinsic == nir_intrinsic_store_ssbo) {
      /* Storage buffer UAVs always use TERASCALE_FORMAT_INDEX_32 or STORE_BYTE / STORE_SHORT. */
      return false;
   }

   /* Vectorization of buffer resource fetches. */

   if (num_components > 4 || hole_size != 0) {
      return false;
   }

   unsigned const vector_bytes = bit_size / 8u * num_components;

   if (bit_size < 32) {
      /* According to testing on Barts, 8_8_8 and 16_16_16 buffer fetches return completely invalid
       * values.
       */
      if (num_components == 3) {
         return false;
      }

      /* According to testing on Barts, the hardware implicitly rounds the address down to the
       * alignment requirement, which is min(bytes per element, 4), matching the alignment
       * restriction described in "4.4.6 Element Alignment" of the Direct3D 11.3 Functional
       * Specification.
       */
      if (nir_combined_align(align_mul, align_offset) < MIN2(vector_bytes, 4u)) {
         return false;
      }
   }

   /* According to testing on Barts, for elements larger than 4 bytes, bounds checking only
    * considers the first 4 bytes, and if they are within the buffer size, the entire element is
    * fetched. This may result in data outside the memory range bound to the buffer being loaded,
    * which is not allowed with robustBufferAccess, so vectorize beyond 4 bytes only if it can be
    * assumed that out-of-bounds access must not happen.
    */
   if (vector_bytes > 4) {
      nir_variable_mode const robust_modes = *(nir_variable_mode const *)data;
      switch (low->intrinsic) {
      case nir_intrinsic_load_ubo:
         if (robust_modes & nir_var_mem_ubo) {
            return false;
         }
         break;
      case nir_intrinsic_load_ssbo:
         if (robust_modes & nir_var_mem_ssbo) {
            return false;
         }
         break;
      default:
         break;
      }
   }

   return true;
}

void
terakan_shader_lower_and_optimize_post_link(
   nir_shader * const nir, struct terakan_pipeline_layout const * const pipeline_layout,
   BITSET_WORD * const resources_needed, uint32_t * const samplers_needed,
   BITSET_WORD * const uavs_for_mutable_resources_needed,
   uint32_t * const driver_push_constants_used,
   uint16_t * const kcache_needed,
   uint8_t * const fragment_data_uncompacted_locations_out,
   bool const robust_buffer_access)
{
   bool progress;

   /* Finally eliminate all dead code that may have effect on lowerings below and on analysis within
    * SFN, so that the demands of the shader can be estimated as accurately as possible.
    *
    * SFN also needs SSA, local variables need to be lowered to SSA, and the stores left after the
    * lowering need to be cleaned up, at some point.
    * Do that as part of the DCE loop, so that DCE works accurately through variable access, and can
    * provide feedback to dead variable removal.
    * Note that while the shader has functions inlined as part of SPIR-V to NIR conversion,
    * nir_var_shader_temp may be generated by passes like nir_lower_io_vars_to_temporaries.
    * They must be lowered to nir_var_function_temp for this cleanup to work.
    */

   NIR_PASS(_, nir, nir_lower_global_vars_to_local);
   NIR_PASS(_, nir, nir_lower_vars_to_ssa);
   do {
      progress = false;
      NIR_PASS(progress, nir, nir_remove_dead_variables, nir_var_function_temp, NULL);
      NIR_PASS(progress, nir, nir_opt_dce);
      NIR_PASS(progress, nir, nir_opt_dead_cf);
   } while (progress);

   if (nir->info.stage == MESA_SHADER_FRAGMENT) {
      /* For fragment data location compaction. */
      NIR_PASS(_, nir, nir_remove_dead_variables, nir_var_shader_in, NULL);
   }

   /* Compact fragment shader output locations.
    * See the description of terakan_nir_compact_fragment_data_locations.
    */

   uint8_t fragment_data_uncompacted_locations = 0b0;
   NIR_PASS(_, nir, terakan_nir_compact_fragment_data_locations,
            &fragment_data_uncompacted_locations);
   if (fragment_data_uncompacted_locations_out != NULL) {
      *fragment_data_uncompacted_locations_out = fragment_data_uncompacted_locations;
   }

   /* Assign gl_frag_result values to variables after the fragment data location compaction has
    * remapped the locations to the hardware values.
    */

   if (nir->info.stage == MESA_SHADER_FRAGMENT) {
      nir_assign_io_var_locations(nir, nir_var_shader_out);
   }

   /* Lower texture operations. */

   NIR_PASS(_, nir, r600_nir_lower_cube_to_2darray);

   /* Vectorize loads that will be lowered to typed buffer load (vertex fetch) instructions, but
    * first lower all loads to scalar to make sure hardware constraints for vectorizing are taken
    * into account. Also, lower SSBO stores to scalar because the hardware instruction stores one
    * element.
    */

   nir_load_store_vectorize_options load_store_vectorize_options = {
      .callback = terakan_nir_should_vectorize_load_store,
      .modes = nir_var_mem_ubo | nir_var_mem_push_const | nir_var_mem_ssbo,
   };
   NIR_PASS(_, nir, nir_lower_io_to_scalar, load_store_vectorize_options.modes, NULL, NULL);
   /* Use the effective per-stage robustness flag computed by the pipeline
    * compiler (device feature OR VK_EXT_pipeline_robustness per-stage state).
    * See terakan_nir_buffer_uav_coord for the hardware rationale. */
   if (robust_buffer_access) {
      load_store_vectorize_options.robust_modes |= nir_var_mem_ubo | nir_var_mem_ssbo;
   }
   load_store_vectorize_options.cb_data = &load_store_vectorize_options.robust_modes;
   NIR_PASS(_, nir, nir_opt_load_store_vectorize, &load_store_vectorize_options);

   /* Lower bindings according to the pipeline layout.
    * In fragment shaders, this is done after compacting the fragment data output locations as UAVs
    * must be placed above color attachments.
    * robust_buffer_access is threaded here so that the UAV coord clamp in
    * terakan_nir_buffer_uav_coord honours per-pipeline robustness state. */
   NIR_PASS(_, nir, terakan_nir_lower_bindings, pipeline_layout, resources_needed, samplers_needed,
            nir->info.stage == MESA_SHADER_FRAGMENT
               ? util_bitcount(fragment_data_uncompacted_locations)
               : 0,
            uavs_for_mutable_resources_needed, driver_push_constants_used,
            kcache_needed, robust_buffer_access);

   /* Perform lowerings on the level of basic building blocks after the interface has been set up.
    */

   /* TODO(Triang3l): Invoke nir_lower_fragcoord_wtrans when r600_lower_and_optimize_nir is removed.
    */

   assert(nir->options->lower_to_scalar);
   NIR_PASS(_, nir, nir_lower_alu_to_scalar, nir->options->lower_to_scalar_filter, NULL);
   NIR_PASS(_, nir, nir_lower_phis_to_scalar, NULL, NULL);

   /* Everything lowered by nir_lower_alu is supported natively as of this writing. */

   NIR_PASS(_, nir, nir_lower_pack);

   nir_lower_idiv_options lower_idiv_options = {};
   NIR_PASS(_, nir, nir_lower_idiv, &lower_idiv_options);

   /* Includes both mandatory lowerings and optimizations. */
   NIR_PASS(_, nir, nir_opt_algebraic);

   if (!nir->info.flrp_lowered) {
      assert(nir->options->lower_flrp16 && nir->options->lower_flrp32 &&
             nir->options->lower_flrp64);
      bool lower_flrp_progress = false;
      NIR_PASS(lower_flrp_progress, nir, nir_lower_flrp, 16 | 32 | 64, false);
      if (lower_flrp_progress) {
         NIR_PASS(_, nir, nir_opt_constant_folding);
      }
      /* Nothing should rematerialize any flrps, so we only need to do this lowering once. */
      nir->info.flrp_lowered = true;
   }
}

/*
 * Filter callback for nir_remove_dead_variables: returns true for VS output
 * variables whose location is in the dead_varyings bitmask.
 */
static bool
terakan_nir_is_dead_vs_output(nir_variable *var, void *data)
{
   uint64_t dead_mask = *(const uint64_t *)data;
   return (dead_mask & BITFIELD64_BIT(var->data.location)) != 0;
}

void
terakan_postprocess_nir(nir_shader *nir,
                        mesa_shader_stage stage,
                        bool remove_point_size,
                        uint64_t fs_inputs_read)
{
   if (stage != MESA_SHADER_VERTEX)
      return;

   /* Compute the set of dead VS outputs to prune. */
   uint64_t dead_varyings = 0;

   /* --- Point size removal ---
    * If topology is definitively not POINT_LIST AND VS is the true last
    * vertex stage, the PA ignores gl_PointSize.  Strip it to save one
    * PARAM export + the ALU that computes it.
    * The caller guarantees remove_point_size is only set when safe
    * (static non-point topology, not as_es, not as_ls). */
   if (remove_point_size && (nir->info.outputs_written & VARYING_BIT_PSIZ))
      dead_varyings |= VARYING_BIT_PSIZ;

   /* --- Varying pruning (FS→VS feedback) ---
    * Only prune PARAM exports — POS/CLIP/CULL/LAYER/VIEWPORT/EDGE are
    * consumed by the PA/clipper hardware, not routed through SPI to FS.
    * PSIZ is PA-consumed and handled separately above.
    *
    * ISA basis: SPI_VS_OUT_ID uses semantic ID matching against
    * SPI_PS_INPUT_CNTL.  Dense-packing surviving PARAM exports after
    * pruning is safe — the SFN backend assigns consecutive export_param
    * indices to surviving outputs, and SPI finds them by semantic ID
    * regardless of physical slot.  (Evergreen_3D_Registers_v2 §SPI) */
   uint64_t const hw_consumed_outputs =
      VARYING_BIT_POS | VARYING_BIT_PSIZ |
      VARYING_BIT_CLIP_DIST0 | VARYING_BIT_CLIP_DIST1 |
      VARYING_BIT_CULL_DIST0 | VARYING_BIT_CULL_DIST1 |
      VARYING_BIT_LAYER | VARYING_BIT_VIEWPORT | VARYING_BIT_EDGE;

   uint64_t const param_outputs =
      nir->info.outputs_written & ~hw_consumed_outputs;
   uint64_t const dead_params = param_outputs & ~fs_inputs_read;
   dead_varyings |= dead_params;

   if (dead_varyings == 0)
      return;

   /* Strip store_output intrinsics targeting dead varyings.
    * This is the actual codegen change — SFN will not see these outputs
    * and will not emit PARAM exports for them. */
   nir_function_impl *impl = nir_shader_get_entrypoint(nir);
   nir_foreach_block(block, impl) {
      nir_foreach_instr_safe(instr, block) {
         if (instr->type != nir_instr_type_intrinsic)
            continue;
         nir_intrinsic_instr *intrin = nir_instr_as_intrinsic(instr);
         if (intrin->intrinsic != nir_intrinsic_store_output)
            continue;
         nir_io_semantics sem = nir_intrinsic_io_semantics(intrin);
         if (dead_varyings & BITFIELD64_BIT(sem.location))
            nir_instr_remove(instr);
      }
   }

   /* Manual nir_instr_remove invalidates all derived metadata (dominance,
    * liveness, block indices).  Explicitly invalidate so subsequent passes
    * (DCE, gather_info) rebuild from scratch rather than operating on
    * stale cached data. */
   impl->valid_metadata = nir_metadata_none;

   /* Remove dead output variables via standard NIR infrastructure.
    * Using nir_remove_dead_variables ensures all bookkeeping (variable
    * lists, counts) is updated correctly. */
   nir_remove_dead_variables_options dead_var_opts = {
      .can_remove_var = terakan_nir_is_dead_vs_output,
      .can_remove_var_data = &dead_varyings,
   };
   NIR_PASS(_, nir, nir_remove_dead_variables, nir_var_shader_out,
            &dead_var_opts);

   /* Clear outputs_written bits for pruned varyings. */
   nir->info.outputs_written &= ~dead_varyings;

   /* DCE: remove ALU chains computing dead outputs.  Then re-gather
    * shader info to ensure all metadata (inputs_read, outputs_written,
    * system_values_read) is consistent with the modified NIR.
    *
    * Note: TeraScale-2 does not support transform feedback (no XFB
    * extension advertised), so pruning based on fs_inputs_read alone
    * is safe.  If XFB support is ever added, pruning must also check
    * xfb_info to avoid stripping feedback-captured outputs. */
   NIR_PASS(_, nir, nir_opt_dce);
   nir_shader_gather_info(nir, impl);

   /* Debug: verify the surgery was complete — no dead varyings should
    * survive in outputs_written after gather_info rebuilds it from the
    * actual instruction stream. */
   assert((nir->info.outputs_written & dead_varyings) == 0);
}

void
terakan_shader_impl_finish(struct terakan_shader_impl * const shader,
                           VkAllocationCallbacks const * const allocator)
{
   if (shader->shader.arrays != NULL) {
      free(shader->shader.arrays);
   }

   terakan_bo_free(shader->static_state.program_bo, allocator);
}
