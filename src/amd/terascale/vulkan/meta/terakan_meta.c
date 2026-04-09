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

#include "terakan_meta.h"

#include "terakan_command_buffer.h"
#include "terakan_device.h"

#include "util/bitscan.h"

#include <assert.h>

static uint32_t terakan_meta_empty_opaque_ps_r8xx[] = {
   /* 0: Export the color with an alpha of 1 and end the program. */

   S_SQ_CF_ALLOC_EXPORT_WORD0_TYPE(V_SQ_CF_ALLOC_EXPORT_WORD0_SQ_EXPORT_PIXEL) |
      S_SQ_CF_ALLOC_EXPORT_WORD0_ARRAY_BASE(0),
   S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_X(TERASCALE_SWIZZLE_0) |
      S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_Y(TERASCALE_SWIZZLE_0) |
      S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_Z(TERASCALE_SWIZZLE_0) |
      S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_W(TERASCALE_SWIZZLE_1) |
      S_SQ_CF_ALLOC_EXPORT_WORD1_END_OF_PROGRAM(true) |
      EG_V_SQ_CF_ALLOC_EXPORT_WORD1_SQ_CF_INST_EXPORT_DONE,
};

static uint32_t terakan_meta_empty_opaque_ps_r9xx[] = {
   /* 0: Export the color with an alpha of 1. */

   S_SQ_CF_ALLOC_EXPORT_WORD0_TYPE(V_SQ_CF_ALLOC_EXPORT_WORD0_SQ_EXPORT_PIXEL) |
      S_SQ_CF_ALLOC_EXPORT_WORD0_ARRAY_BASE(0),
   S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_X(TERASCALE_SWIZZLE_0) |
      S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_Y(TERASCALE_SWIZZLE_0) |
      S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_Z(TERASCALE_SWIZZLE_0) |
      S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_W(TERASCALE_SWIZZLE_1) |
      EG_V_SQ_CF_ALLOC_EXPORT_WORD1_SQ_CF_INST_EXPORT_DONE,

   /* 1: End the program. */

   TERAKAN_SHADER_CF_END_R9XX,
};

static struct terakan_meta_shader const terakan_meta_empty_opaque_ps = {
   .r8xx =
      {
         .program = terakan_meta_empty_opaque_ps_r8xx,
         .program_size_bytes = sizeof(terakan_meta_empty_opaque_ps_r8xx),
         .static_registers =
            {
               .sq_pgm_resources =
                  {
                     TERAKAN_META_SQ_PGM_RESOURCES_COMMON,
                     TERAKAN_META_SQ_PGM_RESOURCES_2_COMMON,
                  },
               .stage =
                  {
                     .ps =
                        {
                           .sq_pgm_exports_ps = S_02884C_EXPORT_COLORS(1),
                           .spi_ps_in_control =
                              {
                                 S_0286CC_NUM_INTERP(1) | S_0286CC_LINEAR_GRADIENT_ENA(1),
                                 0,
                              },
                           .spi_baryc_cntl = S_0286E0_LINEAR_CENTER_ENA(1),
                           .cb_shader_mask = 0xF,
                        },
                  },
            },
      },
   .r9xx =
      {
         .program = terakan_meta_empty_opaque_ps_r9xx,
         .program_size_bytes = sizeof(terakan_meta_empty_opaque_ps_r9xx),
         .static_registers =
            {
               .sq_pgm_resources =
                  {
                     TERAKAN_META_SQ_PGM_RESOURCES_COMMON,
                     TERAKAN_META_SQ_PGM_RESOURCES_2_COMMON,
                  },
               .stage =
                  {
                     .ps =
                        {
                           .sq_pgm_exports_ps = S_02884C_EXPORT_COLORS(1),
                           .spi_ps_in_control =
                              {
                                 S_0286CC_NUM_INTERP(1) | S_0286CC_LINEAR_GRADIENT_ENA(1),
                                 0,
                              },
                           .spi_baryc_cntl = S_0286E0_LINEAR_CENTER_ENA(1),
                           .cb_shader_mask = 0xF,
                        },
                  },
            },
      },
   .stage =
      {
         .ps =
            {
               .db_shader_control = TERAKAN_META_DB_SHADER_CONTROL_DEFAULT,
            },
      },
};

struct terakan_meta_shader const * const terakan_meta_shaders[TERAKAN_META_SHADER_COUNT] = {
   [TERAKAN_META_SHADER_EMPTY_OPAQUE_PS] = &terakan_meta_empty_opaque_ps,
   [TERAKAN_META_SHADER_POSITION_FROM_INDEX_VS] = &terakan_meta_position_from_index_vs,
   [TERAKAN_META_SHADER_POSITION_AND_LAYER_FROM_INDEX_VS] =
      &terakan_meta_position_and_layer_from_index_vs,
   [TERAKAN_META_SHADER_CLEAR_COLOR_PS] = &terakan_meta_clear_color_ps,
   [TERAKAN_META_SHADER_COPY_BUFFER_TO_IMAGE_PS] = &terakan_meta_copy_buffer_to_image_ps,
   [TERAKAN_META_SHADER_COPY_IMAGE_TO_BUFFER_PS] = &terakan_meta_copy_image_to_buffer_ps,
   [TERAKAN_META_SHADER_COPY_IMAGE_PS] = &terakan_meta_copy_image_ps,
   [TERAKAN_META_SHADER_COPY_EXPAND_3X_PS] = &terakan_meta_copy_expand_3x_ps,
   [TERAKAN_META_SHADER_QUERY_ACCUM_ZPASS_1_RB_VS] = &terakan_meta_query_accum_zpass_1_rb_vs,
   [TERAKAN_META_SHADER_QUERY_ACCUM_ZPASS_2_RB_VS] = &terakan_meta_query_accum_zpass_2_rb_vs,
   [TERAKAN_META_SHADER_QUERY_ACCUM_ZPASS_4_RB_VS] = &terakan_meta_query_accum_zpass_4_rb_vs,
   [TERAKAN_META_SHADER_QUERY_ACCUM_ZPASS_8_RB_VS] = &terakan_meta_query_accum_zpass_8_rb_vs,
   [TERAKAN_META_SHADER_QUERY_ACCUM_PIPELINESTAT_VS] = &terakan_meta_query_accum_pipelinestat_vs,
   [TERAKAN_META_SHADER_QUERY_ACCUM_STREAMOUTSTATS_VS] =
      &terakan_meta_query_accum_streamoutstats_vs,
   [TERAKAN_META_SHADER_QUERY_COPY_ZPASS_32_BIT_1_RB_VS] =
      &terakan_meta_query_copy_zpass_32_bit_1_rb_vs,
   [TERAKAN_META_SHADER_QUERY_COPY_ZPASS_32_BIT_2_RB_VS] =
      &terakan_meta_query_copy_zpass_32_bit_2_rb_vs,
   [TERAKAN_META_SHADER_QUERY_COPY_ZPASS_32_BIT_4_RB_VS] =
      &terakan_meta_query_copy_zpass_32_bit_4_rb_vs,
   [TERAKAN_META_SHADER_QUERY_COPY_ZPASS_32_BIT_8_RB_VS] =
      &terakan_meta_query_copy_zpass_32_bit_8_rb_vs,
   [TERAKAN_META_SHADER_QUERY_COPY_ZPASS_64_BIT_1_RB_VS] =
      &terakan_meta_query_copy_zpass_64_bit_1_rb_vs,
   [TERAKAN_META_SHADER_QUERY_COPY_ZPASS_64_BIT_2_RB_VS] =
      &terakan_meta_query_copy_zpass_64_bit_2_rb_vs,
   [TERAKAN_META_SHADER_QUERY_COPY_ZPASS_64_BIT_4_RB_VS] =
      &terakan_meta_query_copy_zpass_64_bit_4_rb_vs,
   [TERAKAN_META_SHADER_QUERY_COPY_ZPASS_64_BIT_8_RB_VS] =
      &terakan_meta_query_copy_zpass_64_bit_8_rb_vs,
   [TERAKAN_META_SHADER_QUERY_COPY_PIPELINESTAT_32_BIT_VS] =
      &terakan_meta_query_copy_pipelinestat_32_bit_vs,
   [TERAKAN_META_SHADER_QUERY_COPY_PIPELINESTAT_64_BIT_VS] =
      &terakan_meta_query_copy_pipelinestat_64_bit_vs,
   [TERAKAN_META_SHADER_QUERY_COPY_TIMESTAMP_32_BIT_VS] =
      &terakan_meta_query_copy_timestamp_32_bit_vs,
   [TERAKAN_META_SHADER_QUERY_COPY_TIMESTAMP_64_BIT_VS] =
      &terakan_meta_query_copy_timestamp_64_bit_vs,
   [TERAKAN_META_SHADER_QUERY_COPY_STREAMOUTSTATS_32_BIT_VS] =
      &terakan_meta_query_copy_streamoutstats_32_bit_vs,
   [TERAKAN_META_SHADER_QUERY_COPY_STREAMOUTSTATS_64_BIT_VS] =
      &terakan_meta_query_copy_streamoutstats_64_bit_vs,
};

void
terakan_meta_modify_state_draw_dword(struct terakan_gfx_command_writer * const command_writer,
                                     enum terakan_state_draw_index const invalidate_state_index,
                                     enum terakan_hw_state_draw_index const hw_state_index,
                                     uint32_t * const hw_state_item, uint32_t const value)
{
   /* Making the state pending unconditionally even if the value in the hardware state ends up
    * unchanged for simplicity of assumptions about other terakan_hw_state_draw fields covered by
    * the same terakan_state_draw item on the caller's side.
    */
   terakan_state_draw_set_pending(&command_writer->state_draw, invalidate_state_index);
   bool const modified = *hw_state_item != value;
   *hw_state_item = value;
   terakan_hw_state_draw_written(&command_writer->hw_state_draw, hw_state_index, modified);
}

void
terakan_meta_set_pa_cl_vte_cntl(struct terakan_gfx_command_writer * const command_writer,
                                uint32_t const pa_cl_vte_cntl)
{
   terakan_meta_modify_state_draw_dword(command_writer, TERAKAN_STATE_DRAW_INDEX_PA_CL_VTE_CNTL,
                                        TERAKAN_HW_STATE_DRAW_INDEX_PA_CL_VTE_CNTL,
                                        &command_writer->hw_state_draw.pa_cl_vte_cntl,
                                        pa_cl_vte_cntl);
}

void
terakan_meta_set_db_render_override(struct terakan_gfx_command_writer * const command_writer,
                                    uint32_t const db_render_override)
{
   terakan_meta_modify_state_draw_dword(command_writer, TERAKAN_STATE_DRAW_INDEX_DB_RENDER_OVERRIDE,
                                        TERAKAN_HW_STATE_DRAW_INDEX_DB_RENDER_OVERRIDE,
                                        &command_writer->hw_state_draw.db_render_override,
                                        db_render_override);
}

void
terakan_meta_set_db_depth_control(struct terakan_gfx_command_writer * const command_writer,
                                  uint32_t const db_depth_control)
{
   terakan_meta_modify_state_draw_dword(command_writer, TERAKAN_STATE_DRAW_INDEX_DB_DEPTH_CONTROL,
                                        TERAKAN_HW_STATE_DRAW_INDEX_DB_DEPTH_CONTROL,
                                        &command_writer->hw_state_draw.db_depth_control,
                                        db_depth_control);
}

void
terakan_meta_set_db_shader_control(struct terakan_gfx_command_writer * const command_writer,
                                   uint32_t const db_shader_control)
{
   terakan_meta_modify_state_draw_dword(command_writer, TERAKAN_STATE_DRAW_INDEX_DB_SHADER_CONTROL,
                                        TERAKAN_HW_STATE_DRAW_INDEX_DB_SHADER_CONTROL,
                                        &command_writer->hw_state_draw.db_shader_control,
                                        db_shader_control);
}

void
terakan_meta_set_vs(struct terakan_gfx_command_writer * const command_writer,
                    enum terakan_meta_shader_index const shader_index)
{
   terakan_state_draw_set_pending(&command_writer->state_draw,
                                  TERAKAN_STATE_DRAW_INDEX_SQ_PGM_LS_HS_ES_GS_VS);

   struct terakan_device const * const device = terakan_gfx_command_writer_device(command_writer);

   struct terakan_shader_static const * const shader_static = &device->meta_shaders[shader_index];

   if (BITSET_TEST(command_writer->hw_state_draw.state_ever_written,
                   TERAKAN_HW_STATE_DRAW_INDEX_SQ_PGM_VS) &&
       command_writer->hw_state_draw.sq_pgm_vs == shader_static) {
      /* If this shader was set via this function previously, everything else set by this function
       * must still be up to date.
       */
      return;
   }

   command_writer->hw_state_draw.sq_pgm_vs = shader_static;
   terakan_hw_state_draw_written(&command_writer->hw_state_draw,
                                 TERAKAN_HW_STATE_DRAW_INDEX_SQ_PGM_VS, true);

   terakan_state_draw_set_pending(&command_writer->state_draw,
                                  TERAKAN_STATE_DRAW_INDEX_SQ_TMP_LS_HS_ES_GS_VS);
   terakan_hw_state_draw_set_sq_ring(command_writer, TERAKAN_SHADER_RING_INDEX_LSTMP, 0, 0);
   terakan_hw_state_draw_set_sq_ring(command_writer, TERAKAN_SHADER_RING_INDEX_HSTMP, 0, 0);
   terakan_hw_state_draw_set_sq_ring(command_writer, TERAKAN_SHADER_RING_INDEX_ESTMP, 0, 0);
   terakan_hw_state_draw_set_sq_ring(command_writer, TERAKAN_SHADER_RING_INDEX_GSTMP, 0, 0);
   terakan_hw_state_draw_set_sq_ring(command_writer, TERAKAN_SHADER_RING_INDEX_VSTMP, 0, 0);

   struct terakan_meta_shader const * const shader = terakan_meta_shaders[shader_index];

   terakan_hw_state_sqc_set_needed_by_vs(&command_writer->hw_state_sqc, shader->kcache_needed,
                                         shader->resources_needed, shader->samplers_needed,
                                         VK_SHADER_STAGE_FRAGMENT_BIT);
}

void
terakan_meta_set_ps(struct terakan_gfx_command_writer * const command_writer,
                    enum terakan_meta_shader_index const shader_index,
                    bool const set_db_shader_control)
{
   struct terakan_meta_shader const * const shader = terakan_meta_shaders[shader_index];

   if (set_db_shader_control) {
      terakan_meta_set_db_shader_control(command_writer, shader->stage.ps.db_shader_control);
   }

   terakan_state_draw_set_pending(&command_writer->state_draw, TERAKAN_STATE_DRAW_INDEX_SQ_PGM_PS);

   struct terakan_device const * const device = terakan_gfx_command_writer_device(command_writer);

   struct terakan_shader_static const * const shader_static = &device->meta_shaders[shader_index];

   bool const shader_static_modified =
      !BITSET_TEST(command_writer->hw_state_draw.state_ever_written,
                   TERAKAN_HW_STATE_DRAW_INDEX_SQ_PGM_PS) ||
      command_writer->hw_state_draw.sq_pgm_ps != shader_static;
   if (!shader_static_modified && shader_index != TERAKAN_META_SHADER_EMPTY_OPAQUE_PS) {
      /* If this shader was set via this function previously, everything else set by this function
       * must still be up to date.
       * TERAKAN_META_SHADER_EMPTY_OPAQUE_PS, however, is also used for application's draws - ensure
       * everything is set up consistently.
       */
      return;
   }

   command_writer->hw_state_draw.sq_pgm_ps = shader_static;
   terakan_hw_state_draw_written(&command_writer->hw_state_draw,
                                 TERAKAN_HW_STATE_DRAW_INDEX_SQ_PGM_PS, shader_static_modified);

   terakan_state_draw_set_pending(&command_writer->state_draw, TERAKAN_STATE_DRAW_INDEX_SQ_TMP_PS);
   terakan_hw_state_draw_set_sq_ring(command_writer, TERAKAN_SHADER_RING_INDEX_PSTMP, 0, 0);

   terakan_hw_state_sqc_set_needed_by_fs(&command_writer->hw_state_sqc, shader->kcache_needed,
                                         shader->resources_needed, shader->samplers_needed);
}

void
terakan_meta_begin(struct terakan_gfx_command_writer * const command_writer,
                   bool const rasterization_used)
{
   terakan_state_draw_set_pending(&command_writer->state_draw,
                                  TERAKAN_STATE_DRAW_INDEX_PIPELINESTAT);
   bool const pipelinestat_modified = command_writer->hw_state_draw.pipelinestat != false;
   command_writer->hw_state_draw.pipelinestat = false;
   terakan_hw_state_draw_written(&command_writer->hw_state_draw,
                                 TERAKAN_HW_STATE_DRAW_INDEX_PIPELINESTAT, pipelinestat_modified);

   if (rasterization_used) {
      terakan_meta_modify_state_draw_dword(
         command_writer, TERAKAN_STATE_DRAW_INDEX_DB_COUNT_CONTROL,
         TERAKAN_HW_STATE_DRAW_INDEX_DB_COUNT_CONTROL,
         &command_writer->hw_state_draw.db_count_control, S_028004_ZPASS_INCREMENT_DISABLE(1));
   }
}

void
terakan_meta_begin_cb(struct terakan_gfx_command_writer * const command_writer,
                      uint32_t const cb_target_mask, uint8_t const disable_blend_for_targets)
{
   terakan_meta_modify_state_draw_dword(
      command_writer, TERAKAN_STATE_DRAW_INDEX_CB_COLOR_CONTROL,
      TERAKAN_HW_STATE_DRAW_INDEX_CB_COLOR_CONTROL, &command_writer->hw_state_draw.cb_color_control,
      S_028808_MODE(cb_target_mask ? V_028808_CB_NORMAL : V_028808_CB_DISABLE) |
         S_028808_ROP3(0xCC));

   terakan_meta_modify_state_draw_dword(command_writer, TERAKAN_STATE_DRAW_INDEX_CB_TARGET_MASK,
                                        TERAKAN_HW_STATE_DRAW_INDEX_CB_TARGET_MASK,
                                        &command_writer->hw_state_draw.cb_target_mask,
                                        cb_target_mask);
   if (cb_target_mask) {
      /* Going to bind color targets for this meta draw. */
      terakan_state_draw_set_pending(&command_writer->state_draw,
                                     TERAKAN_STATE_DRAW_INDEX_CB_COLOR_RTV);
      terakan_state_draw_set_pending(&command_writer->state_draw,
                                     TERAKAN_STATE_DRAW_INDEX_CB_COLOR_UAV);
   }

   if (disable_blend_for_targets) {
      terakan_state_draw_set_pending(&command_writer->state_draw,
                                     TERAKAN_STATE_DRAW_INDEX_CB_BLEND_CONTROL);
      unsigned remaining_targets_to_disable_blend = (unsigned)disable_blend_for_targets;
      while (remaining_targets_to_disable_blend) {
         terakan_hw_state_draw_set_cb_blend_control(
            &command_writer->hw_state_draw, u_bit_scan(&remaining_targets_to_disable_blend), 0);
      }
   }
}

/* ---- Meta state save / restore (B2 infrastructure) ---- */

void
terakan_meta_save_state(struct terakan_gfx_command_writer * const command_writer,
                        uint32_t const flags,
                        struct terakan_meta_saved_state * const state_out)
{
   state_out->flags = flags;

   if (flags & TERAKAN_META_SAVE_GRAPHICS_PIPELINE) {
      /* Save Mesa dynamic graphics state.
       *
       * Terakan does not currently populate vk_dynamic_graphics_state from its
       * CmdSet* paths (state_draw is the authoritative source), so this is
       * mostly saving defaults.  Struct-copy is safe here because the pointer
       * members (vi, ms.sample_locations) are NULL -- Terakan does not support
       * the extensions that allocate them.
       */
      struct vk_command_buffer * const vk_cmd =
         &command_writer->base.command_buffer->vk;
      assert(vk_cmd->dynamic_graphics_state.vi == NULL);
      assert(vk_cmd->dynamic_graphics_state.ms.sample_locations == NULL);
      state_out->vk_dynamic = vk_cmd->dynamic_graphics_state;

      state_out->graphics_kcache_needed = command_writer->graphics_kcache_needed;
      state_out->robustness_metadata_bound_to_stages =
         command_writer->robustness_metadata.bound_to_stages;
   }

   if (flags & TERAKAN_META_SAVE_COMPUTE_PIPELINE) {
      state_out->compute_pipeline = command_writer->bound_compute_pipeline;
      state_out->compute_pipeline_dirty = command_writer->compute_pipeline_dirty;
   }

   if (flags & TERAKAN_META_SAVE_PUSH_CONSTANTS) {
      state_out->push_constants_bound_to_stages =
         command_writer->push_constants_state.up_to_date_push_constants_bound_to_stages;
   }
}

void
terakan_meta_restore_state(struct terakan_gfx_command_writer * const command_writer,
                           struct terakan_meta_saved_state const * const saved)
{
   if (saved->flags & TERAKAN_META_SAVE_GRAPHICS_PIPELINE) {
      /* Restore Mesa dynamic graphics state and mark it fully dirty.
       *
       * The struct-copy restores the exact pre-meta snapshot.  dirty_all()
       * ensures any driver code consuming vk_dynamic_graphics_state will
       * re-validate everything on the next draw.  Terakan currently drives
       * re-emission through state_draw.state_pending (set by the meta
       * helpers), so this is a forward-looking measure for when the CmdSet*
       * paths are migrated.
       */
      struct vk_command_buffer * const vk_cmd =
         &command_writer->base.command_buffer->vk;
      vk_cmd->dynamic_graphics_state = saved->vk_dynamic;
      vk_dynamic_graphics_state_dirty_all(&vk_cmd->dynamic_graphics_state);

      command_writer->graphics_kcache_needed = saved->graphics_kcache_needed;
      command_writer->robustness_metadata.bound_to_stages =
         saved->robustness_metadata_bound_to_stages;
   }

   if (saved->flags & TERAKAN_META_SAVE_COMPUTE_PIPELINE) {
      command_writer->bound_compute_pipeline = saved->compute_pipeline;
      command_writer->compute_pipeline_dirty = saved->compute_pipeline_dirty;
   }

   if (saved->flags & TERAKAN_META_SAVE_PUSH_CONSTANTS) {
      /* Invalidate the cached allocation so push constants are re-uploaded
       * and re-bound on the next draw or dispatch.
       */
      command_writer->push_constants_state.allocation.mapping_if_up_to_date = NULL;
      command_writer->push_constants_state.up_to_date_push_constants_bound_to_stages = 0;
   }
}
