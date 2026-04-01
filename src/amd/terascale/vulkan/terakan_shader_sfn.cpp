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
#include "terakan_device.h"
#include "terakan_physical_device.h"
#include "terakan_vertex_input.h"

#include "compiler/shader_enums.h"
#include "amd/terascale/common/terascale_evergreend.h"
#include "gallium/drivers/r600/r600_asm.h"
#include "gallium/drivers/r600/r600_isa.h"
#include "terakan_instance.h"
#include "gallium/drivers/r600/sfn/sfn_assembler.h"
#include "gallium/drivers/r600/sfn/sfn_memorypool.h"
#include "gallium/drivers/r600/sfn/sfn_nir.h"
#include "gallium/include/pipe/p_shader_tokens.h"
#include "gallium/include/pipe/p_state.h"
#include "util/u_math.h"
#include "amd_family.h"
#include "nir.h"
#include "vk_log.h"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>


/* ---------------------------------------------------------------------------
 * TERAKAN_DEBUG=shaders / TERAKAN_DEBUG=perf
 *
 * After r600_bytecode_build the linked-list CF/ALU structure is still intact.
 * Walk it to produce:
 *   - Per-clause bundle count and slot-fill histogram
 *   - Overall VLIW utilisation (actual_slots / theoretical_max_slots)
 *   - GPR / ndw summary
 *
 * Activated via TERAKAN_DEBUG_SHADERS (always) or TERAKAN_DEBUG_PERF
 * (only when utilisation falls below TERAKAN_VLIW_WARN_THRESHOLD).
 * ---------------------------------------------------------------------------
 */

#define TERAKAN_VLIW_SLOTS_MAX 5
/* Warn when fewer than this fraction of available slots are filled. */
#define TERAKAN_VLIW_WARN_THRESHOLD 0.70f

static void
terakan_shader_debug_vliw_stats(struct r600_bytecode const * const bc,
                                mesa_shader_stage const stage,
                                uint64_t const debug_flags)
{
   unsigned total_alu = 0;
   unsigned n_bundles = 0;
   /* histogram: [0] unused, [1..5] = bundles with 1..5 slots filled */
   unsigned bundle_hist[TERAKAN_VLIW_SLOTS_MAX + 1] = {0};
   unsigned cur_bundle_size = 0;
   unsigned n_cf_alu = 0;   /* number of ALU CF clauses */

   struct r600_bytecode_cf *cf;
   LIST_FOR_EACH_ENTRY(cf, &bc->cf, list) {
      const struct cf_op_info *cfop = r600_isa_cf(cf->op);
      if (!(cfop->flags & CF_ALU))
         continue;
      ++n_cf_alu;
      cur_bundle_size = 0;
      struct r600_bytecode_alu *alu;
      LIST_FOR_EACH_ENTRY(alu, &cf->alu, list) {
         ++total_alu;
         ++cur_bundle_size;
         if (alu->last) {
            ++n_bundles;
            unsigned const clamped =
               cur_bundle_size <= TERAKAN_VLIW_SLOTS_MAX
                  ? cur_bundle_size
                  : TERAKAN_VLIW_SLOTS_MAX;
            bundle_hist[clamped]++;
            cur_bundle_size = 0;
         }
      }
   }

   unsigned const max_slots = n_bundles * TERAKAN_VLIW_SLOTS_MAX;
   float const utilization = max_slots
      ? (float)total_alu / (float)max_slots
      : 0.0f;

   /* Always print when TERAKAN_DEBUG_SHADERS; only print on low utilization
    * when TERAKAN_DEBUG_PERF. */
   bool const always_print = (debug_flags & TERAKAN_DEBUG_SHADERS) != 0;
   bool const perf_warn = (debug_flags & TERAKAN_DEBUG_PERF) &&
                           utilization < TERAKAN_VLIW_WARN_THRESHOLD;

   if (!always_print && !perf_warn)
      return;

   const char *stage_name = mesa_shader_stage_name(stage);

   if (perf_warn && !always_print) {
      fprintf(stderr,
              "TERAKAN_PERF [%s]: VLIW utilisation %.1f%% < %.0f%% threshold\n",
              stage_name, utilization * 100.0f,
              TERAKAN_VLIW_WARN_THRESHOLD * 100.0f);
   }

   fprintf(stderr,
           "TERAKAN_VLIW [%s]: GPR=%-3u ndw=%-5u "
           "cf_alu_clauses=%-3u bundles=%-5u alu_instr=%-5u util=%.1f%%\n",
           stage_name, bc->ngpr, bc->ndw,
           n_cf_alu, n_bundles, total_alu, utilization * 100.0f);
   fprintf(stderr,
           "  Bundle histogram: 1-slot=%-4u 2-slot=%-4u 3-slot=%-4u "
           "4-slot=%-4u 5-slot=%-4u\n",
           bundle_hist[1], bundle_hist[2], bundle_hist[3],
           bundle_hist[4], bundle_hist[5]);
   if (n_bundles) {
      fprintf(stderr,
              "  5-slot%%=%.1f%%  4-slot%%=%.1f%%  avg_fill=%.2f\n",
              100.0f * bundle_hist[5] / n_bundles,
              100.0f * bundle_hist[4] / n_bundles,
              (float)total_alu / n_bundles);
   }
}

VkResult
terakan_shader_impl_compile(terakan_shader_impl * const shader, terakan_device * const device,
                            r600_shader_key const * const key, nir_shader * const nir,
                            VkAllocationCallbacks const * const allocator)
{
   VkResult result;

   terakan_physical_device const & physical_device = *terakan_device_physical_device(device);
   terakan_physical_device_chip_info const & chip_info = physical_device.chip_info;
   amd_gfx_level const gfx_level = chip_info.is_r9xx ? CAYMAN : EVERGREEN;

   /* TODO(Triang3l): Fill stream output info from NIR. */
   pipe_stream_output_info so_info = {};

   r600::init_pool();

#if 0
   r600_finalize_nir_common(nir, gfx_level);
#endif
   /* For r600_lower_and_optimize_nir, for fields like number bit sizes, and also for
    * DB_SHADER_CONTROL in fragment shaders.
    */
   nir_shader_gather_info(nir, nir_shader_get_entrypoint(nir));
   r600_lower_and_optimize_nir(nir, key, gfx_level, &so_info);

   r600::ShaderBindingLayout binding_layout;
   binding_layout.texture_resource_offset = 0;

   r600::Shader * const unscheduled_sfn_shader = r600::Shader::translate_from_nir(
      nir, (const pipe_stream_output_info *)&so_info, static_cast<r600_shader*>(nullptr), *key, chip_info.is_r9xx ? ISA_CC_CAYMAN : ISA_CC_EVERGREEN,
      chip_info.chip_family);
   unscheduled_sfn_shader->set_binding_layout(binding_layout);
   if (unscheduled_sfn_shader == nullptr) {
      r600::release_pool();
      return vk_errorf(device, VK_ERROR_UNKNOWN, "Failed to translate the shader from NIR");
   }
   r600_finalize_and_optimize_shader(unscheduled_sfn_shader);
   r600::Shader * const sfn_shader = r600_schedule_shader(unscheduled_sfn_shader);
   if (sfn_shader != unscheduled_sfn_shader) {
      delete unscheduled_sfn_shader;
   }
   if (sfn_shader == nullptr) {
      r600::release_pool();
      return vk_errorf(device, VK_ERROR_UNKNOWN, "Failed to schedule the shader");
   }

   /* With the current size calculation, nir->scratch_size is in vec4 units. */
   shader->scratch_item_size_dwords = 4 * nir->scratch_size;

   sfn_shader->get_shader_info(&shader->shader);
   /* Pre-applied during binding lowering. */
   shader->shader.rat_base = 0;

   /* TODO(Triang3l): has_compressed_msaa_texturing. */
   r600_bytecode_init(&shader->shader.bc, gfx_level, chip_info.chip_family, false);

   /* We already schedule the code with this in mind, no need to handle this in the backend
    * assembler.
    */
   shader->shader.bc.ar_handling = AR_HANDLE_NORMAL;
   shader->shader.bc.r6xx_nop_after_rel_dst = 0;

   shader->shader.bc.type = shader->shader.processor_type;
   shader->shader.bc.isa = physical_device.isa;
   shader->shader.bc.ngpr = sfn_shader->required_registers();

   r600::Assembler assembler(&shader->shader, *key);
   if (!assembler.lower(sfn_shader)) {
      delete sfn_shader;

      r600::release_pool();

      r600_bytecode_clear(&shader->shader.bc);
      if (shader->shader.arrays != nullptr) {
         std::free(shader->shader.arrays);
      }

      return vk_errorf(device, VK_ERROR_UNKNOWN, "Failed to lower the shader to assembly");
   }

   delete sfn_shader;

   r600::release_pool();

   if (r600_bytecode_build(&shader->shader.bc) != 0) {
      r600_bytecode_clear(&shader->shader.bc);
      if (shader->shader.arrays != nullptr) {
         std::free(shader->shader.arrays);
      }
      return vk_errorf(device, VK_ERROR_UNKNOWN, "Failed to build the shader bytecode");
   }

   /* --- TERAKAN_DEBUG: ISA + VLIW analysis (linked list still valid here) --- */
   {
      struct terakan_physical_device const * const phys_dev =
         terakan_device_physical_device(device);
      struct terakan_instance const * const instance =
         container_of(phys_dev->vk.base.instance, struct terakan_instance const, vk);
      uint64_t const dbg = instance->debug_flags;
      if (dbg & (TERAKAN_DEBUG_SHADERS | TERAKAN_DEBUG_PERF)) {
         if (dbg & TERAKAN_DEBUG_SHADERS) {
            /* Full ISA disassembly via r600_bytecode_disasm. */
            fprintf(stderr, "TERAKAN_SHADERS: --- %s shader ISA ---\n",
                    mesa_shader_stage_name(nir->info.stage));
            r600_bytecode_disasm(&shader->shader.bc);
         }
         terakan_shader_debug_vliw_stats(&shader->shader.bc, nir->info.stage, dbg);
      }
      if (dbg & TERAKAN_DEBUG_PIPELINE) {
         fprintf(stderr,
                 "TERAKAN_PIPELINE: %s GPR=%u ndw=%u nstack=%u\n",
                 mesa_shader_stage_name(nir->info.stage),
                 shader->shader.bc.ngpr, shader->shader.bc.ndw,
                 shader->shader.bc.nstack);
      }
   }

   /* Fill shader registers and other info. */

   shader->static_state.sq_pgm_resources[0] = S_028844_NUM_GPRS(shader->shader.bc.ngpr) |
                                              S_028844_STACK_SIZE(shader->shader.bc.nstack) |
                                              S_028844_DX10_CLAMP(1);
   /* TODO(Triang3l): Rounding modes from shader float controls. */
   shader->static_state.sq_pgm_resources[1] = S_028848_SINGLE_ROUND(V_SQ_ROUND_NEAREST_EVEN) |
                                              S_028848_DOUBLE_ROUND(V_SQ_ROUND_NEAREST_EVEN);

   /* TODO(Triang3l): Correct vertex pipeline stages. */
   switch (nir->info.stage) {
   case MESA_SHADER_VERTEX: {
      std::memset(shader->static_state.stage.vs.spi_vs_out_id, 0,
                  sizeof(shader->static_state.stage.vs.spi_vs_out_id));
      for (unsigned output_index = 0; output_index < shader->shader.noutput; ++output_index) {
         r600_shader_io const & output = shader->shader.output[output_index];
         if (output.export_param >= 0) {
            unsigned & parameter_spi_vs_out_id =
               shader->static_state.stage.vs.spi_vs_out_id[output.export_param / 4];
            unsigned const parameter_shift = (output.export_param & 3) * 8;
            assert(!(parameter_spi_vs_out_id & ((uint32_t)0xFF << parameter_shift)));
            parameter_spi_vs_out_id |= (uint32_t)output.spi_sid << parameter_shift;
         }
      }

      shader->static_state.stage.vs.spi_vs_out_config =
         S_0286C4_VS_EXPORT_COUNT(shader->shader.highest_export_param);

      uint32_t const clip_distances_enabled =
         (((uint32_t)1 << nir->info.clip_distance_array_size) - 1);
      uint32_t const cull_distances_enabled =
         (((uint32_t)1 << nir->info.cull_distance_array_size) - 1)
         << nir->info.clip_distance_array_size;
      uint32_t const clip_cull_distances_enabled = clip_distances_enabled | cull_distances_enabled;
      shader->static_state.stage.vs.pa_cl_vs_out_cntl =
         clip_distances_enabled | (cull_distances_enabled << 8) |
         S_02881C_USE_VTX_POINT_SIZE(shader->shader.vs_out_point_size) |
         S_02881C_USE_VTX_RENDER_TARGET_INDX(shader->shader.vs_out_layer) |
         S_02881C_USE_VTX_VIEWPORT_INDX(shader->shader.vs_out_viewport) |
         S_02881C_VS_OUT_MISC_VEC_ENA(shader->shader.vs_out_misc_write) |
         S_02881C_VS_OUT_CCDIST0_VEC_ENA((clip_cull_distances_enabled & 0b00001111) != 0) |
         S_02881C_VS_OUT_CCDIST1_VEC_ENA((clip_cull_distances_enabled & 0b11110000) != 0);
   } break;

   case MESA_SHADER_FRAGMENT: {
      uint32_t db_shader_control =
         S_02880C_KILL_ENABLE(shader->shader.uses_kill) |
         S_02880C_CONSERVATIVE_Z_EXPORT(nir->info.fs.depth_layout == FRAG_DEPTH_LAYOUT_GREATER
                                           ? V_02880C_EXPORT_GREATER_THAN_Z
                                           : (nir->info.fs.depth_layout == FRAG_DEPTH_LAYOUT_LESS
                                                 ? V_02880C_EXPORT_LESS_THAN_Z
                                                 : V_02880C_EXPORT_ANY_Z));
      for (unsigned output_index = 0; output_index < shader->shader.noutput; ++output_index) {
         switch (shader->shader.output[output_index].frag_result) {
         case FRAG_RESULT_DEPTH:
            db_shader_control |= S_02880C_Z_EXPORT_ENABLE(1);
            break;
         case FRAG_RESULT_STENCIL:
            db_shader_control |= S_02880C_STENCIL_EXPORT_ENABLE(1);
            break;
         case FRAG_RESULT_SAMPLE_MASK:
            db_shader_control |= S_02880C_MASK_EXPORT_ENABLE(1);
            break;
         default:
            break;
         }
      }
      db_shader_control |= S_02880C_DB_SOURCE_FORMAT(
         db_shader_control & S_02880C_MASK_EXPORT_ENABLE(1)
            ? (db_shader_control & S_02880C_Z_EXPORT_ENABLE(1) ? V_02880C_EXPORT_DB_FULL
                                                               : V_02880C_EXPORT_DB_FOUR16)
            : V_02880C_EXPORT_DB_TWO);
      db_shader_control |= S_02880C_DUAL_EXPORT_ENABLE(
         G_02880C_DB_SOURCE_FORMAT(db_shader_control) != V_02880C_EXPORT_DB_FULL);
      /* See RadeonSI DB_SHADER_CONTROL setup for more details about the possible Z order and
       * EXEC_ON_* cases.
       * Not using ReZ currently due to unknown performance impact.
       */
      if (nir->info.fs.early_fragment_tests) {
         db_shader_control |= S_02880C_DEPTH_BEFORE_SHADER(1) |
                              S_02880C_Z_ORDER(V_02880C_EARLY_Z_THEN_LATE_Z) |
                              S_02880C_EXEC_ON_NOOP(nir->info.writes_memory);
      } else if (nir->info.writes_memory) {
         db_shader_control |= S_02880C_Z_ORDER(V_02880C_LATE_Z) | S_02880C_EXEC_ON_HIER_FAIL(1);
      } else {
         db_shader_control |= S_02880C_Z_ORDER(V_02880C_EARLY_Z_THEN_LATE_Z);
      }
      shader->fs.db_shader_control = db_shader_control;

      bool export_z = (db_shader_control &
                       ~(uint32_t)(C_02880C_Z_EXPORT_ENABLE & C_02880C_STENCIL_EXPORT_ENABLE &
                                   C_02880C_MASK_EXPORT_ENABLE)) != 0;
      /* Something must be exported, either at least one color or at least the DB export.
       * Explicitly ensuring that is not necessary in this code due to the + 1.
       */
      shader->static_state.stage.ps.sq_pgm_exports_ps =
         S_02884C_EXPORT_COLORS(shader->shader.ps_export_highest + 1) | S_02884C_EXPORT_Z(export_z);

      std::memset(shader->static_state.stage.ps.spi_ps_input_cntl, 0,
                  sizeof(shader->static_state.stage.ps.spi_ps_input_cntl));
      /* TODO(Triang3l): Build SPI_BARYC_CNTL in the shader compiler rather than independently here
       * because GPR allocation in the shader depends on it.
       */
      shader->static_state.stage.ps.spi_baryc_cntl = 0;
      uint32_t interpolator_count = 0;
      r600_shader_io const * position_input = nullptr;
      uint32_t face_and_sample_mask_gpr = UINT32_MAX;
      uint32_t sample_id_gpr = UINT32_MAX;
      for (unsigned input_index = 0; input_index < shader->shader.ninput; ++input_index) {
         r600_shader_io const & input = shader->shader.input[input_index];
         if (input.varying_slot == VARYING_SLOT_POS) {
            assert(position_input == nullptr);
            position_input = &input;
         } else if (input.varying_slot == VARYING_SLOT_FACE ||
                    input.system_value == SYSTEM_VALUE_SAMPLE_MASK_IN) {
            assert(face_and_sample_mask_gpr == UINT32_MAX || face_and_sample_mask_gpr == input.gpr);
            face_and_sample_mask_gpr = input.gpr;
         } else if (input.system_value == SYSTEM_VALUE_SAMPLE_ID) {
            assert(sample_id_gpr == UINT32_MAX);
            sample_id_gpr = input.gpr;
         } else if (input.spi_sid != 0) {
            interpolator_count = MAX2(input.lds_pos + 1, interpolator_count);
            shader->static_state.stage.ps.spi_ps_input_cntl[input.lds_pos] =
               S_028644_SEMANTIC(input.spi_sid) |
               S_028644_FLAT_SHADE(input.interpolate == TGSI_INTERPOLATE_CONSTANT) |
               S_028644_PT_SPRITE_TEX(input.varying_slot == VARYING_SLOT_PNTC);
            bool const interpolator_is_linear = input.interpolate == TGSI_INTERPOLATE_LINEAR;
            if (interpolator_is_linear || input.interpolate == TGSI_INTERPOLATE_PERSPECTIVE ||
                input.interpolate == TGSI_INTERPOLATE_COLOR) {
               switch (input.interpolate_location) {
               case TGSI_INTERPOLATE_LOC_CENTER:
                  shader->static_state.stage.ps.spi_baryc_cntl |= interpolator_is_linear
                                                                     ? S_0286E0_LINEAR_CENTER_ENA(1)
                                                                     : S_0286E0_PERSP_CENTER_ENA(1);
                  break;
               case TGSI_INTERPOLATE_LOC_CENTROID:
                  shader->static_state.stage.ps.spi_baryc_cntl |=
                     interpolator_is_linear ? S_0286E0_LINEAR_CENTROID_ENA(1)
                                            : S_0286E0_PERSP_CENTROID_ENA(1);
                  break;
               case TGSI_INTERPOLATE_LOC_SAMPLE:
                  shader->static_state.stage.ps.spi_baryc_cntl |= interpolator_is_linear
                                                                     ? S_0286E0_LINEAR_SAMPLE_ENA(1)
                                                                     : S_0286E0_PERSP_SAMPLE_ENA(1);
                  break;
               default:
                  break;
               }
            }
         }
      }
      if (!shader->static_state.stage.ps.spi_baryc_cntl) {
         shader->static_state.stage.ps.spi_baryc_cntl = S_0286E0_PERSP_CENTER_ENA(1);
      }
      constexpr uint32_t spi_baryc_cntl_persp_clear =
         C_0286E0_PERSP_CENTER_ENA & C_0286E0_PERSP_CENTROID_ENA & C_0286E0_PERSP_SAMPLE_ENA &
         C_0286E0_PERSP_PULL_MODEL_ENA;
      constexpr uint32_t spi_baryc_cntl_linear_clear =
         C_0286E0_LINEAR_CENTER_ENA & C_0286E0_LINEAR_CENTROID_ENA & C_0286E0_LINEAR_SAMPLE_ENA;
      shader->static_state.stage.ps.spi_ps_in_control[0] =
         S_0286CC_NUM_INTERP(MAX2(interpolator_count, 1)) |
         S_0286CC_PERSP_GRADIENT_ENA(
            (shader->static_state.stage.ps.spi_baryc_cntl & ~spi_baryc_cntl_persp_clear) != 0) |
         S_0286CC_LINEAR_GRADIENT_ENA(
            (shader->static_state.stage.ps.spi_baryc_cntl & ~spi_baryc_cntl_linear_clear) != 0);
      if (position_input != nullptr) {
         shader->static_state.stage.ps.spi_ps_in_control[0] |=
            S_0286CC_POSITION_ENA(1) |
            S_0286CC_POSITION_CENTROID(position_input->interpolate_location ==
                                       TGSI_INTERPOLATE_LOC_CENTROID) |
            S_0286CC_POSITION_SAMPLE(position_input->interpolate_location ==
                                     TGSI_INTERPOLATE_LOC_SAMPLE) |
            S_0286CC_POSITION_ADDR(position_input->gpr);
      }
      shader->static_state.stage.ps.spi_ps_in_control[1] = 0;
      if (face_and_sample_mask_gpr != UINT32_MAX) {
         shader->static_state.stage.ps.spi_ps_in_control[1] |=
            S_0286D0_FRONT_FACE_ENA(1) | S_0286D0_FRONT_FACE_ADDR(face_and_sample_mask_gpr);
      }
      if (sample_id_gpr != UINT32_MAX) {
         shader->static_state.stage.ps.spi_ps_in_control[1] |=
            S_0286D0_FIXED_PT_POSITION_ENA(1) | S_0286D0_FIXED_PT_POSITION_ADDR(sample_id_gpr);
      }
      shader->static_state.stage.ps.spi_input_z =
         S_0286D8_PROVIDE_Z_TO_SPI(position_input != nullptr);

      shader->static_state.stage.ps.cb_shader_mask = shader->shader.ps_color_export_mask;
   } break;

   default:
      break;
   }

   if (nir->info.stage == MESA_SHADER_VERTEX) {
      for (unsigned input_index = 0; input_index < shader->shader.ninput; ++input_index) {
         struct r600_shader_io const * const input = &shader->shader.input[input_index];
         assert(input->gpr > 0);
         assert(input->gpr - 1 < TERAKAN_VERTEX_INPUT_MAX_ATTRIBUTES);
         if (input->gpr > 0 && input->gpr - 1 < TERAKAN_VERTEX_INPUT_MAX_ATTRIBUTES) {
            BITSET_SET(shader->vs.vertex_attributes_needed, input->gpr - 1);
         }
      }
   }

   /* Write the program to the BO. */
   size_t const program_size_bytes = sizeof(uint32_t) * shader->shader.bc.ndw;
   result = device->winsys_fn->bo->allocate_device_memory(
      device, program_size_bytes, TERAKAN_SHADER_PROGRAM_ALIGNMENT,
      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
         VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
      0, allocator, VK_SYSTEM_ALLOCATION_SCOPE_OBJECT, &shader->static_state.program_bo);
   if (result != VK_SUCCESS) {
      r600_bytecode_clear(&shader->shader.bc);
      if (shader->shader.arrays != nullptr) {
         std::free(shader->shader.arrays);
      }
      return vk_error(device, result);
   }
   shader->static_state.program_va_shr8 = 0;
   {
      void * const program_bo_mapping = terakan_bo_map(shader->static_state.program_bo);
      if (program_bo_mapping == nullptr) {
         terakan_bo_free(shader->static_state.program_bo, allocator);
         r600_bytecode_clear(&shader->shader.bc);
         if (shader->shader.arrays != nullptr) {
            std::free(shader->shader.arrays);
         }
         return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);
      }
      util_memcpy_cpu_to_le32(program_bo_mapping, shader->shader.bc.bytecode, program_size_bytes);
      terakan_bo_unmap(device->meta_shaders_bo);
   }

   /* Don't need the bytecode structure after writing the binary. */
   r600_bytecode_clear(&shader->shader.bc);

   return VK_SUCCESS;
}
