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

#include "terakan_pipeline_graphics.h"

#include "terakan_bo.h"
#include "terakan_command_buffer.h"
#include "terakan_device.h"
#include "terakan_entrypoints.h"
#include "terakan_physical_device.h"
#include "terakan_pipeline_layout.h"
#include "terakan_shader.h"
#include "terakan_state.h"
#include "terakan_state_color.h"
#include "terakan_state_input_assembly.h"
#include "terakan_state_rasterization.h"

#include "terakan_pipeline_cache.h"
#include "terakan_pipeline_key.h"

#include "amd/terascale/common/terascale_evergreend.h"
#include "gallium/drivers/r600/r600_shader_common.h"
#include "pipe/p_shader_tokens.h"
#include "util/bitscan.h"
#include "util/macros.h"
#include "util/ralloc.h"
#include "util/u_math.h"
#include "vk_alloc.h"
#include "vk_enum_to_str.h"
#include "vk_graphics_state.h"
#include "vk_pipeline.h"
#include "vk_pipeline_cache.h"
#include "vk_log.h"
#include "vk_util.h"

#include <assert.h>
#include <inttypes.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef void (*terakan_pipeline_graphics_apply_state_function)(
   struct terakan_gfx_command_writer * command_writer,
   struct terakan_pipeline_graphics const * pipeline);

static void
terakan_pipeline_graphics_apply_vgt_primitive_type(
   struct terakan_gfx_command_writer * const command_writer,
   struct terakan_pipeline_graphics const * const pipeline)
{
   command_writer->state_draw.vgt_primitive_type = pipeline->vertex_input.vgt_primitive_type;
   terakan_state_draw_set_pending(&command_writer->state_draw,
                                  TERAKAN_STATE_DRAW_INDEX_VGT_PRIMITIVE_TYPE);
}

static void
terakan_pipeline_graphics_apply_sq_pgm_fs(struct terakan_gfx_command_writer * const command_writer,
                                          struct terakan_pipeline_graphics const * const pipeline)
{
   struct terakan_vertex_input_static_state const * const sq_pgm_fs =
      pipeline->vertex_input.sq_pgm_fs.program_bo != NULL
         ? &pipeline->vertex_input.sq_pgm_fs
         : &container_of(pipeline->base.base.device, struct terakan_device const, vk)
               ->empty_vertex_input;
   if (command_writer->state_draw.sq_pgm_fs.static_state != sq_pgm_fs) {
      command_writer->state_draw.sq_pgm_fs.static_state = sq_pgm_fs;
      terakan_state_draw_set_pending(&command_writer->state_draw,
                                     TERAKAN_STATE_DRAW_INDEX_SQ_PGM_FS);
   }
}

static void
terakan_pipeline_graphics_apply_sq_resources_fs_stride(
   struct terakan_gfx_command_writer * const command_writer,
   struct terakan_pipeline_graphics const * const pipeline)
{
   {
      unsigned bindings_remaining =
         pipeline->vertex_input.sq_resources_fs_stride.bindings_with_static_stride;
      while (bindings_remaining) {
         int const binding_index = u_bit_scan(&bindings_remaining);
         command_writer->state_draw.sq_resources_fs[binding_index].stride =
            pipeline->vertex_input.sq_resources_fs_stride.binding_strides[binding_index];
      }
   }
   command_writer->state_draw.sq_resources_fs_pending |=
      pipeline->vertex_input.sq_resources_fs_stride.bindings_with_static_stride;
   terakan_state_draw_set_pending(&command_writer->state_draw,
                                  TERAKAN_STATE_DRAW_INDEX_SQ_RESOURCES_FS);
}

static void
terakan_pipeline_graphics_apply_sq_pgm_fs_2048_stride_workaround(
   struct terakan_gfx_command_writer * const command_writer,
   struct terakan_pipeline_graphics const * const pipeline)
{
   assert(BITSET_TEST(pipeline->static_state, TERAKAN_PIPELINE_GRAPHICS_STATE_SQ_PGM_FS));
   assert(
      BITSET_TEST(pipeline->static_state, TERAKAN_PIPELINE_GRAPHICS_STATE_SQ_RESOURCES_FS_STRIDE));
   assert(!(pipeline->vertex_input.sq_pgm_fs.bindings_with_2048_stride_workaround &
            ~pipeline->vertex_input.sq_resources_fs_stride.bindings_with_static_stride));
   uint32_t const new_bindings_with_2048_stride_workaround =
      (command_writer->state_draw.sq_pgm_fs.bindings_with_2048_stride_workaround &
       ~pipeline->vertex_input.sq_resources_fs_stride.bindings_with_static_stride) |
      pipeline->vertex_input.sq_pgm_fs.bindings_with_2048_stride_workaround;
   /* If whether the workaround needs to be applied to any currently needed bindings is changed,
    * update the fetch shader.
    */
   if ((command_writer->state_draw.sq_pgm_fs.bindings_with_2048_stride_workaround ^
        new_bindings_with_2048_stride_workaround) &
       pipeline->vertex_input.sq_pgm_fs.bindings_needed_by_attributes_and_provided) {
      terakan_state_draw_set_pending(&command_writer->state_draw,
                                     TERAKAN_STATE_DRAW_INDEX_SQ_PGM_FS);
   }
   command_writer->state_draw.sq_pgm_fs.bindings_with_2048_stride_workaround =
      new_bindings_with_2048_stride_workaround;
}

static void
terakan_pipeline_graphics_apply_pa_sc_vport_z_min_0_max_1(
   struct terakan_gfx_command_writer * const command_writer,
   struct terakan_pipeline_graphics const * const pipeline)
{
   if (command_writer->state_draw.viewport.pa_sc_vport_z_min_0_max_1 !=
       pipeline->pre_rasterization.pa_sc_vport_z_min_0_max_1) {
      command_writer->state_draw.viewport.pa_sc_vport_z_min_0_max_1 =
         pipeline->pre_rasterization.pa_sc_vport_z_min_0_max_1;
      command_writer->state_draw.viewport.viewports_pending.pa_sc_vport_z_min_max =
         BITFIELD_MASK(ARRAY_SIZE(command_writer->state_draw.viewport.viewports));
      terakan_state_draw_set_pending(&command_writer->state_draw,
                                     TERAKAN_STATE_DRAW_INDEX_VIEWPORT);
   }
}

static void
terakan_pipeline_graphics_apply_viewport_count(
   struct terakan_gfx_command_writer * const command_writer,
   struct terakan_pipeline_graphics const * const pipeline)
{
   terakan_state_draw_set_viewport_count(&command_writer->state_draw,
                                         pipeline->pre_rasterization.viewport_count);
}

static void
terakan_pipeline_graphics_apply_viewport(struct terakan_gfx_command_writer * const command_writer,
                                         struct terakan_pipeline_graphics const * const pipeline)
{
   struct terakan_state_draw * const state = &command_writer->state_draw;
   bool any_pending = false;
   for (uint8_t viewport_index = 0; viewport_index < pipeline->pre_rasterization.viewport_count;
        ++viewport_index) {
      struct terakan_state_draw_viewport * const state_viewport =
         &state->viewport.viewports[viewport_index];
      struct terakan_state_draw_viewport const * const pipeline_viewport =
         &pipeline->pre_rasterization.viewports[viewport_index];
      uint16_t const viewport_bit = (uint16_t)BITFIELD_BIT(viewport_index);
      if (memcmp(state_viewport->pa_cl_vport_xy_scale_offset,
                 pipeline_viewport->pa_cl_vport_xy_scale_offset,
                 sizeof(state_viewport->pa_cl_vport_xy_scale_offset)) != 0) {
         any_pending = true;
         state->viewport.viewports_pending.pa_cl_vport_xy_scale_offset |= viewport_bit;
      }
      if (memcmp(state_viewport->pa_cl_vport_z_gl_dx_scale_offset,
                 pipeline_viewport->pa_cl_vport_z_gl_dx_scale_offset,
                 sizeof(state_viewport->pa_cl_vport_z_gl_dx_scale_offset)) != 0) {
         any_pending = true;
         state->viewport.viewports_pending.pa_cl_vport_z_scale_offset |= viewport_bit;
      }
      if (memcmp(state_viewport->pa_cl_gb_vert_horz_clip_adj,
                 pipeline_viewport->pa_cl_gb_vert_horz_clip_adj,
                 sizeof(state_viewport->pa_cl_gb_vert_horz_clip_adj)) != 0) {
         any_pending = true;
         state->viewport.pa_cl_gb_pending = true;
      }
      if (memcmp(state_viewport->pa_sc_vport_scissor_tl_br_xy,
                 pipeline_viewport->pa_sc_vport_scissor_tl_br_xy,
                 sizeof(state_viewport->pa_sc_vport_scissor_tl_br_xy)) != 0) {
         any_pending = true;
         state->viewport.viewports_pending.pa_sc_vport_scissor |= viewport_bit;
      }
      if (memcmp(state_viewport->pa_sc_vport_z_min_max, pipeline_viewport->pa_sc_vport_z_min_max,
                 sizeof(state_viewport->pa_sc_vport_z_min_max)) != 0 &&
          !state->viewport.pa_sc_vport_z_min_0_max_1) {
         any_pending = true;
         state->viewport.viewports_pending.pa_sc_vport_z_min_max |= viewport_bit;
      }
      *state_viewport = *pipeline_viewport;
   }
   if (any_pending) {
      terakan_state_draw_set_pending(state, TERAKAN_STATE_DRAW_INDEX_VIEWPORT);
   }
}

static void
terakan_pipeline_graphics_apply_pa_sc_vport_generic_scissor(
   struct terakan_gfx_command_writer * const command_writer,
   struct terakan_pipeline_graphics const * const pipeline)
{
   for (uint32_t scissor_index = 0;
        scissor_index < pipeline->pre_rasterization.pa_sc_vport_generic_scissor_count;
        ++scissor_index) {
      uint16_t * const viewport_scissor =
         command_writer->state_draw.viewport.pa_sc_vport_generic_scissor_tl_br_xy[scissor_index][0];
      uint16_t const * const pipeline_scissor =
         pipeline->pre_rasterization.pa_sc_vport_generic_scissor_tl_br_xy[scissor_index][0];
      if (memcmp(viewport_scissor, pipeline_scissor, sizeof(uint16_t) * 4) != 0) {
         memcpy(viewport_scissor, pipeline_scissor, sizeof(uint16_t) * 4);
         command_writer->state_draw.viewport.viewports_pending.pa_sc_vport_scissor |=
            (uint16_t)BITFIELD_BIT(scissor_index);
         terakan_state_draw_set_pending(&command_writer->state_draw,
                                        TERAKAN_STATE_DRAW_INDEX_VIEWPORT);
      }
   }
}

static void
terakan_pipeline_graphics_apply_pa_cl_clip_cntl(
   struct terakan_gfx_command_writer * const command_writer,
   struct terakan_pipeline_graphics const * const pipeline)
{
   terakan_state_draw_replace_fields(&command_writer->state_draw,
                                     TERAKAN_STATE_DRAW_INDEX_PA_CL_CLIP_CNTL,
                                     &command_writer->state_draw.pa_cl_clip_cntl,
                                     pipeline->pre_rasterization.pa_cl_clip_cntl_clear,
                                     pipeline->pre_rasterization.pa_cl_clip_cntl);
}

static void
terakan_pipeline_graphics_apply_pa_su_sc_mode_cntl(
   struct terakan_gfx_command_writer * const command_writer,
   struct terakan_pipeline_graphics const * const pipeline)
{
   terakan_state_draw_replace_fields(&command_writer->state_draw,
                                     TERAKAN_STATE_DRAW_INDEX_PA_SU_SC_MODE_CNTL,
                                     &command_writer->state_draw.pa_su_sc_mode_cntl,
                                     pipeline->pre_rasterization.pa_su_sc_mode_cntl_clear,
                                     pipeline->pre_rasterization.pa_su_sc_mode_cntl);
}

static void
terakan_pipeline_graphics_apply_pa_su_poly_offset(
   struct terakan_gfx_command_writer * const command_writer,
   struct terakan_pipeline_graphics const * const pipeline)
{
   /* These values are not needed by internal draws, modify hw_state_draw directly. */
   bool const clamp_scale_offset_modified =
      memcmp(&command_writer->hw_state_draw.pa_su_poly_offset_clamp,
             &pipeline->pre_rasterization.pa_su_poly_offset.clamp, sizeof(float)) != 0 ||
      memcmp(&command_writer->hw_state_draw.pa_su_poly_offset_subpixel_slope_scale,
             &pipeline->pre_rasterization.pa_su_poly_offset.subpixel_slope_scale,
             sizeof(float)) != 0 ||
      memcmp(&command_writer->hw_state_draw.pa_su_poly_offset_offset,
             &pipeline->pre_rasterization.pa_su_poly_offset.offset, sizeof(float)) != 0;
   command_writer->hw_state_draw.pa_su_poly_offset_clamp =
      pipeline->pre_rasterization.pa_su_poly_offset.clamp;
   command_writer->hw_state_draw.pa_su_poly_offset_subpixel_slope_scale =
      pipeline->pre_rasterization.pa_su_poly_offset.subpixel_slope_scale;
   command_writer->hw_state_draw.pa_su_poly_offset_offset =
      pipeline->pre_rasterization.pa_su_poly_offset.offset;
   terakan_hw_state_draw_written(&command_writer->hw_state_draw,
                                 TERAKAN_HW_STATE_DRAW_INDEX_PA_SU_POLY_OFFSET_CLAMP_SCALE_OFFSET,
                                 clamp_scale_offset_modified);

   if (command_writer->state_draw.pa_su_poly_offset_db_fmt_cntl.representation !=
          pipeline->pre_rasterization.pa_su_poly_offset.representation ||
       command_writer->state_draw.pa_su_poly_offset_db_fmt_cntl.representation_exact !=
          pipeline->pre_rasterization.pa_su_poly_offset.representation_exact) {
      command_writer->state_draw.pa_su_poly_offset_db_fmt_cntl.representation =
         pipeline->pre_rasterization.pa_su_poly_offset.representation;
      command_writer->state_draw.pa_su_poly_offset_db_fmt_cntl.representation_exact =
         pipeline->pre_rasterization.pa_su_poly_offset.representation_exact;
      terakan_state_draw_set_pending(&command_writer->state_draw,
                                     TERAKAN_STATE_DRAW_INDEX_PA_SU_POLY_OFFSET_DB_FMT_CNTL);
   }
}

static void
terakan_pipeline_graphics_apply_db_render_override_pre_rasterization(
   struct terakan_gfx_command_writer * const command_writer,
   struct terakan_pipeline_graphics const * const pipeline)
{
   terakan_state_draw_replace_fields(&command_writer->state_draw,
                                     TERAKAN_STATE_DRAW_INDEX_DB_RENDER_OVERRIDE,
                                     &command_writer->state_draw.db_render_override,
                                     pipeline->pre_rasterization.db_render_override_clear,
                                     pipeline->pre_rasterization.db_render_override);
}

static void
terakan_pipeline_graphics_apply_pa_sc_aa_mask(
   struct terakan_gfx_command_writer * const command_writer,
   struct terakan_pipeline_graphics const * const pipeline)
{
   command_writer->state_draw.pa_sc_aa_mask = pipeline->multisample.pa_sc_aa_mask;
   terakan_state_draw_set_pending(&command_writer->state_draw,
                                  TERAKAN_STATE_DRAW_INDEX_PA_SC_AA_MASK);
}

static void
terakan_pipeline_graphics_apply_db_stencilrefmask(
   struct terakan_gfx_command_writer * const command_writer,
   struct terakan_pipeline_graphics const * const pipeline)
{
   for (unsigned face_index = 0; face_index < 2; ++face_index) {
      terakan_state_draw_replace_fields(
         &command_writer->state_draw, TERAKAN_STATE_DRAW_INDEX_DB_STENCILREFMASK,
         &command_writer->state_draw.db_stencilrefmask_front_back[face_index],
         pipeline->fragment_shader.db_stencilrefmask_clear,
         pipeline->fragment_shader.db_stencilrefmask_front_back[face_index]);
   }
}

static void
terakan_pipeline_graphics_apply_db_depth_control(
   struct terakan_gfx_command_writer * const command_writer,
   struct terakan_pipeline_graphics const * const pipeline)
{
   terakan_state_draw_replace_fields(
      &command_writer->state_draw, TERAKAN_STATE_DRAW_INDEX_DB_DEPTH_CONTROL,
      &command_writer->state_draw.db_depth_control,
      pipeline->fragment_shader.db_depth_control_clear, pipeline->fragment_shader.db_depth_control);
}

static void
terakan_pipeline_graphics_apply_logic_op_enable(
   struct terakan_gfx_command_writer * const command_writer,
   struct terakan_pipeline_graphics const * const pipeline)
{
   /* Make TERAKAN_STATE_DRAW_INDEX_LOGIC_OP pending only if needed due to the complexity of
    * applying it.
    */
   if (command_writer->state_draw.logic_op.enable != pipeline->fragment_output.logic_op_enable) {
      command_writer->state_draw.logic_op.enable = pipeline->fragment_output.logic_op_enable;
      terakan_state_draw_set_pending(&command_writer->state_draw,
                                     TERAKAN_STATE_DRAW_INDEX_LOGIC_OP);
   }
}

static void
terakan_pipeline_graphics_apply_logic_op_rop3(
   struct terakan_gfx_command_writer * const command_writer,
   struct terakan_pipeline_graphics const * const pipeline)
{
   /* Make TERAKAN_STATE_DRAW_INDEX_LOGIC_OP pending only if needed due to the complexity of
    * applying it.
    */
   if (command_writer->state_draw.logic_op.enable &&
       command_writer->state_draw.logic_op.rop3 != pipeline->fragment_output.logic_op_rop3) {
      terakan_state_draw_set_pending(&command_writer->state_draw,
                                     TERAKAN_STATE_DRAW_INDEX_LOGIC_OP);
   }
   command_writer->state_draw.logic_op.rop3 = pipeline->fragment_output.logic_op_rop3;
}

static void
terakan_pipeline_graphics_apply_cb_blend_rgba(
   struct terakan_gfx_command_writer * const command_writer,
   struct terakan_pipeline_graphics const * const pipeline)
{
   /* The blend constant is not needed by internal draws, modify hw_state_draw directly. */
   bool const modified = memcmp(command_writer->hw_state_draw.cb_blend_rgba,
                                pipeline->fragment_output.cb_blend_rgba, sizeof(float) * 4) != 0;
   memcpy(command_writer->hw_state_draw.cb_blend_rgba, pipeline->fragment_output.cb_blend_rgba,
          sizeof(float) * 4);
   terakan_hw_state_draw_written(&command_writer->hw_state_draw,
                                 TERAKAN_HW_STATE_DRAW_INDEX_CB_BLEND_RGBA, modified);
}

static void
terakan_pipeline_graphics_apply_cb_blend_control_enable(
   struct terakan_gfx_command_writer * const command_writer,
   struct terakan_pipeline_graphics const * const pipeline)
{
   for (uint32_t attachment_index = 0;
        attachment_index < pipeline->fragment_output.color_blend_attachment_count;
        ++attachment_index) {
      uint32_t * const cb_blend_control_ptr =
         &command_writer->state_draw.cb_blend_control.attachments[attachment_index];
      bool const attachment_enable = G_028780_BLEND_CONTROL_ENABLE(
         pipeline->fragment_output.cb_blend_control[attachment_index]);
      if (G_028780_BLEND_CONTROL_ENABLE(*cb_blend_control_ptr) != attachment_enable) {
         *cb_blend_control_ptr = (*cb_blend_control_ptr & C_028780_BLEND_CONTROL_ENABLE) |
                                 S_028780_BLEND_CONTROL_ENABLE(attachment_enable);
         terakan_state_draw_set_pending(&command_writer->state_draw,
                                        TERAKAN_STATE_DRAW_INDEX_CB_BLEND_CONTROL);
      }
   }
}

static void
terakan_pipeline_graphics_apply_cb_blend_control_equation(
   struct terakan_gfx_command_writer * const command_writer,
   struct terakan_pipeline_graphics const * const pipeline)
{
   for (uint32_t attachment_index = 0;
        attachment_index < pipeline->fragment_output.color_blend_attachment_count;
        ++attachment_index) {
      uint32_t * const cb_blend_control_ptr =
         &command_writer->state_draw.cb_blend_control.attachments[attachment_index];
      bool const attachment_enable = G_028780_BLEND_CONTROL_ENABLE(*cb_blend_control_ptr);
      uint32_t const cb_blend_control =
         S_028780_BLEND_CONTROL_ENABLE(attachment_enable) |
         (pipeline->fragment_output.cb_blend_control[attachment_index] &
          C_028780_BLEND_CONTROL_ENABLE);
      if (*cb_blend_control_ptr != cb_blend_control) {
         *cb_blend_control_ptr = cb_blend_control;
         if (attachment_enable) {
            terakan_state_draw_set_pending(&command_writer->state_draw,
                                           TERAKAN_STATE_DRAW_INDEX_CB_BLEND_CONTROL);
         }
      }
   }
}

static void
terakan_pipeline_graphics_apply_color_attachment_write_mask(
   struct terakan_gfx_command_writer * const command_writer,
   struct terakan_pipeline_graphics const * const pipeline)
{
   for (uint32_t attachment_index = 0;
        attachment_index < pipeline->fragment_output.color_blend_attachment_count;
        ++attachment_index) {
      uint8_t * const attachment_write_mask_ptr =
         &command_writer->state_draw.cb_target_mask.attachment_write_masks[attachment_index];
      uint8_t const attachment_write_mask =
         pipeline->fragment_output.color_attachment_write_masks[attachment_index];
      if (*attachment_write_mask_ptr != attachment_write_mask) {
         *attachment_write_mask_ptr = attachment_write_mask;
         terakan_state_draw_set_pending(&command_writer->state_draw,
                                        TERAKAN_STATE_DRAW_INDEX_CB_TARGET_MASK);
      }
   }
}

static void
terakan_pipeline_graphics_apply_color_attachment_write_enable(
   struct terakan_gfx_command_writer * const command_writer,
   struct terakan_pipeline_graphics const * const pipeline)
{
   if (command_writer->state_draw.cb_target_mask.attachment_write_enable !=
       pipeline->fragment_output.color_attachment_write_enable) {
      command_writer->state_draw.cb_target_mask.attachment_write_enable =
         pipeline->fragment_output.color_attachment_write_enable;
      terakan_state_draw_set_pending(&command_writer->state_draw,
                                     TERAKAN_STATE_DRAW_INDEX_CB_TARGET_MASK);
   }
}

static terakan_pipeline_graphics_apply_state_function const
   terakan_pipeline_graphics_apply_state_functions[TERAKAN_PIPELINE_GRAPHICS_STATE_COUNT] = {
      [TERAKAN_PIPELINE_GRAPHICS_STATE_PA_SC_VPORT_Z_MIN_0_MAX_1] =
         terakan_pipeline_graphics_apply_pa_sc_vport_z_min_0_max_1,
      [TERAKAN_PIPELINE_GRAPHICS_STATE_VIEWPORT_COUNT] =
         terakan_pipeline_graphics_apply_viewport_count,
      [TERAKAN_PIPELINE_GRAPHICS_STATE_VIEWPORT] = terakan_pipeline_graphics_apply_viewport,
      [TERAKAN_PIPELINE_GRAPHICS_STATE_PA_SC_VPORT_GENERIC_SCISSOR] =
         terakan_pipeline_graphics_apply_pa_sc_vport_generic_scissor,
      [TERAKAN_PIPELINE_GRAPHICS_STATE_VGT_PRIMITIVE_TYPE] =
         terakan_pipeline_graphics_apply_vgt_primitive_type,
      [TERAKAN_PIPELINE_GRAPHICS_STATE_SQ_PGM_FS] = terakan_pipeline_graphics_apply_sq_pgm_fs,
      [TERAKAN_PIPELINE_GRAPHICS_STATE_SQ_RESOURCES_FS_STRIDE] =
         terakan_pipeline_graphics_apply_sq_resources_fs_stride,
      [TERAKAN_PIPELINE_GRAPHICS_STATE_SQ_PGM_FS_2048_STRIDE_WORKAROUND] =
         terakan_pipeline_graphics_apply_sq_pgm_fs_2048_stride_workaround,
      [TERAKAN_PIPELINE_GRAPHICS_STATE_PA_CL_CLIP_CNTL] =
         terakan_pipeline_graphics_apply_pa_cl_clip_cntl,
      [TERAKAN_PIPELINE_GRAPHICS_STATE_PA_SU_SC_MODE_CNTL] =
         terakan_pipeline_graphics_apply_pa_su_sc_mode_cntl,
      [TERAKAN_PIPELINE_GRAPHICS_STATE_PA_SU_POLY_OFFSET] =
         terakan_pipeline_graphics_apply_pa_su_poly_offset,
      [TERAKAN_PIPELINE_GRAPHICS_STATE_DB_RENDER_OVERRIDE_PRE_RASTERIZATION] =
         terakan_pipeline_graphics_apply_db_render_override_pre_rasterization,
      [TERAKAN_PIPELINE_GRAPHICS_STATE_PA_SC_AA_MASK] =
         terakan_pipeline_graphics_apply_pa_sc_aa_mask,
      [TERAKAN_PIPELINE_GRAPHICS_STATE_DB_STENCILREFMASK] =
         terakan_pipeline_graphics_apply_db_stencilrefmask,
      [TERAKAN_PIPELINE_GRAPHICS_STATE_DB_DEPTH_CONTROL] =
         terakan_pipeline_graphics_apply_db_depth_control,
      [TERAKAN_PIPELINE_GRAPHICS_STATE_LOGIC_OP_ENABLE] =
         terakan_pipeline_graphics_apply_logic_op_enable,
      [TERAKAN_PIPELINE_GRAPHICS_STATE_LOGIC_OP_ROP3] =
         terakan_pipeline_graphics_apply_logic_op_rop3,
      [TERAKAN_PIPELINE_GRAPHICS_STATE_CB_BLEND_RGBA] =
         terakan_pipeline_graphics_apply_cb_blend_rgba,
      [TERAKAN_PIPELINE_GRAPHICS_STATE_CB_BLEND_CONTROL_ENABLE] =
         terakan_pipeline_graphics_apply_cb_blend_control_enable,
      [TERAKAN_PIPELINE_GRAPHICS_STATE_CB_BLEND_CONTROL_EQUATION] =
         terakan_pipeline_graphics_apply_cb_blend_control_equation,
      [TERAKAN_PIPELINE_GRAPHICS_STATE_COLOR_ATTACHMENT_WRITE_MASK] =
         terakan_pipeline_graphics_apply_color_attachment_write_mask,
      [TERAKAN_PIPELINE_GRAPHICS_STATE_COLOR_ATTACHMENT_WRITE_ENABLE] =
         terakan_pipeline_graphics_apply_color_attachment_write_enable,
};

void
terakan_pipeline_graphics_bind(struct terakan_gfx_command_writer * const command_writer,
                               struct terakan_pipeline_graphics const * const pipeline)
{
   /* Vertex shader. */

   struct terakan_shader_impl const * const vs = &pipeline->shaders[MESA_SHADER_VERTEX];
   if (command_writer->state_draw.sq_pgm_ls_hs_es_gs_vs.vs_as_vs != vs) {
      command_writer->state_draw.sq_pgm_ls_hs_es_gs_vs.vs_as_vs = vs;
      terakan_state_draw_set_pending(&command_writer->state_draw,
                                     TERAKAN_STATE_DRAW_INDEX_SQ_PGM_LS_HS_ES_GS_VS);
   }

   /* Fragment shader. */

   struct terakan_shader_impl const * const fs =
      pipeline->shader_stages & VK_SHADER_STAGE_FRAGMENT_BIT
         ? &pipeline->shaders[MESA_SHADER_FRAGMENT]
         : NULL;
   if (command_writer->state_draw.sq_pgm_ps.fs != fs) {
      command_writer->state_draw.sq_pgm_ps.fs = fs;
      terakan_state_draw_set_pending(&command_writer->state_draw,
                                     TERAKAN_STATE_DRAW_INDEX_SQ_PGM_PS);
   }

   /* Static state. */

   command_writer->state_draw.cmd_set_depth_clamp_enable_sets_depth_clip_enable =
      pipeline->pre_rasterization.cmd_set_depth_clamp_enable_sets_depth_clip_enable;

   /* Cache the pipeline-wide kcache_needed mask for draw-time bank 14 binding. */
   command_writer->graphics_kcache_needed = pipeline->kcache_needed_merged;

   unsigned state_index;
   BITSET_FOREACH_SET (state_index, pipeline->static_state, TERAKAN_PIPELINE_GRAPHICS_STATE_COUNT) {
      terakan_pipeline_graphics_apply_state_functions[state_index](command_writer, pipeline);
   }

   /* Update SQ_THREAD_RESOURCE_MGMT_1/2 and SQ_STACK_RESOURCE_MGMT_1/2/3 when the pipeline's
    * active stage set differs from the current hardware configuration.
    *
    * WHY: command buffer initialization sets these config registers to the VS+PS-only allocation
    * (sq_thread_resource_mgmt_ts_gs_r8xx[0][0]).  A pipeline with tessellation or geometry shader
    * stages needs the SQ thread budget redistributed across LS/HS/ES/GS; without this the SQ
    * deadlocks on wave launches to slots with zero thread allocation.  Even though tessellation
    * and geometry shaders are not yet exposed by Terakan, implementing the re-emit now keeps the
    * invariant tight and prevents silent breakage when those features are added.
    *
    * HOW: compare new_thread_mgmt against state_draw.sq_tmp.sq_thread_resource_mgmt, which tracks
    * the value currently in hardware (both are set to [0][0] at command buffer reset, both are
    * updated together here on mismatch).  On mismatch: emit PS_PARTIAL_FLUSH followed by a single
    * PKT3_SET_CONFIG_REG covering SQ_THREAD_RESOURCE_MGMT_1/2 and SQ_STACK_RESOURCE_MGMT_1/2/3,
    * then sync sq_tmp.sq_thread_resource_mgmt to match hardware.
    */
   {
      struct terakan_physical_device_chip_info const * const chip_info =
         &terakan_gfx_command_writer_physical_device(command_writer)->chip_info;
      if (!chip_info->is_r9xx) {
         /* Restore graphics-biased SQ_CONFIG if a compute dispatch ran since the
          * last graphics pipeline bind.
          *
          * WHY: terakan_compute_multiplex_begin() programs SQ_CONFIG to
          * LS_PRIO=3/CS_PRIO=3 with VS_PRIO=PS_PRIO=0 so the SQ dedicates
          * all thread-scheduling priority to the compute (LS) stage.  It also
          * zeros SQ_THREAD_RESOURCE_MGMT_1 (PS/VS/GS/ES slots = 0) and sets the
          * sq_config_is_compute_mode flag.  Without a restore here, the first draw
          * after a dispatch has VS and PS wavefronts fighting for zero-priority
          * slots against an LS stage that holds all priority -- a silent deadlock.
          *
          * HOW: emit one PS_PARTIAL_FLUSH (stall until all shader wavefronts retire
          * so the SQ is quiescent) followed by a single SET_CONFIG_REG that writes
          * R_008C00_SQ_CONFIG with the graphics priority layout.  Clear the flag
          * so subsequent draws skip this path.  The sq_tmp mismatch check below
          * fires independently to restore SQ_THREAD_RESOURCE_MGMT_1/2 thread
          * slots; that path also emits its own PS_PARTIAL_FLUSH which is harmless
          * (the pipeline is already stalled from this one). */
         if (command_writer->sq_config_is_compute_mode) {
            uint32_t const sq_config =
               S_008C00_VC_ENABLE(chip_info->has_vertex_cache) |
               S_008C00_EXPORT_SRC_C(1) |
               S_008C00_LS_PRIO(3) | S_008C00_HS_PRIO(3) |
               S_008C00_ES_PRIO(3) | S_008C00_GS_PRIO(2) |
               S_008C00_VS_PRIO(1) | S_008C00_PS_PRIO(0) |
               S_008C00_CS_PRIO(0);
            /* 2 (PS_PARTIAL_FLUSH EVENT_WRITE) + 2 (SET_CONFIG_REG header) + 1 (SQ_CONFIG data) */
            uint32_t *p = terakan_gfx_command_writer_emit(
               command_writer, TERAKAN_GFX_COMMAND_WRITER_EMIT_CONTENTS_STATE, 2 + 2 + 1);
            if (unlikely(p == NULL)) {
               return;
            }
            *p++ = PKT3(PKT3_EVENT_WRITE, 0, 0);
            *p++ = EVENT_TYPE(EVENT_TYPE_PS_PARTIAL_FLUSH) | EVENT_INDEX(4);
            *p++ = PKT3(PKT3_SET_CONFIG_REG, 1, 0);
            *p++ = TERAKAN_CONFIG_REG_OFFSET(R_008C00_SQ_CONFIG);
            *p++ = sq_config;
            terakan_gfx_command_writer_emit_done(command_writer, p);
            command_writer->sq_config_is_compute_mode = false;
         }

         bool const ts_enabled =
            !!(pipeline->shader_stages & (VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT |
                                          VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT));
         bool const gs_enabled =
            !!(pipeline->shader_stages & VK_SHADER_STAGE_GEOMETRY_BIT);
         uint32_t const * const new_thread_mgmt =
            chip_info->sq_thread_resource_mgmt_ts_gs_r8xx[ts_enabled ? 1 : 0][gs_enabled ? 1 : 0];

         if (memcmp(command_writer->state_draw.sq_tmp.sq_thread_resource_mgmt,
                    new_thread_mgmt, sizeof(uint32_t) * 2) != 0) {
            unsigned const sq_vertex_stage_count =
               terakan_shader_hw_vertex_stage_count(ts_enabled, gs_enabled);
            uint32_t const sq_stage_stack_entries =
               chip_info->sq_max_stack_entries / (sq_vertex_stage_count + 1);
            /* sq_thread_stack_register_count: compile-time constant 5 covering
             * SQ_THREAD_RESOURCE_MGMT_1, _2, SQ_STACK_RESOURCE_MGMT_1, _2, _3. */
            uint32_t const sq_thread_stack_register_count =
               (R_008C28_SQ_STACK_RESOURCE_MGMT_3 - R_008C18_SQ_THREAD_RESOURCE_MGMT_1) /
                  sizeof(uint32_t) + 1;

            /* Emit PS_PARTIAL_FLUSH + SET_CONFIG_REG in one reservation.
             * Total DWORDs: 2 (EVENT_WRITE) + 2 (PKT3 header + reg offset) +
             *               sq_thread_stack_register_count (5 data). */
            uint32_t *p = terakan_gfx_command_writer_emit(
               command_writer, TERAKAN_GFX_COMMAND_WRITER_EMIT_CONTENTS_STATE,
               2 + 2 + sq_thread_stack_register_count);
            if (unlikely(p == NULL)) {
               return;
            }
            *p++ = PKT3(PKT3_EVENT_WRITE, 0, 0);
            *p++ = EVENT_TYPE(EVENT_TYPE_PS_PARTIAL_FLUSH) | EVENT_INDEX(4);
            *p++ = PKT3(PKT3_SET_CONFIG_REG, sq_thread_stack_register_count, 0);
            *p++ = TERAKAN_CONFIG_REG_OFFSET(R_008C18_SQ_THREAD_RESOURCE_MGMT_1);
            /* R_008C18_SQ_THREAD_RESOURCE_MGMT_1, R_008C1C_SQ_THREAD_RESOURCE_MGMT_2 */
            memcpy(p, new_thread_mgmt, sizeof(uint32_t) * 2);
            p += 2;
            /* R_008C20_SQ_STACK_RESOURCE_MGMT_1 */
            *p++ = S_008C20_NUM_PS_STACK_ENTRIES(sq_stage_stack_entries) |
                   S_008C20_NUM_VS_STACK_ENTRIES(sq_stage_stack_entries);
            /* R_008C24_SQ_STACK_RESOURCE_MGMT_2 */
            *p++ = gs_enabled
                      ? S_008C24_NUM_GS_STACK_ENTRIES(sq_stage_stack_entries) |
                           S_008C24_NUM_ES_STACK_ENTRIES(sq_stage_stack_entries)
                      : 0;
            /* R_008C28_SQ_STACK_RESOURCE_MGMT_3 */
            *p++ = ts_enabled
                      ? S_008C28_NUM_HS_STACK_ENTRIES(sq_stage_stack_entries) |
                           S_008C28_NUM_LS_STACK_ENTRIES(sq_stage_stack_entries)
                      : 0;
            terakan_gfx_command_writer_emit_done(command_writer, p);

            /* Keep sq_tmp consistent with hardware.  terakan_state_draw_apply_sq_tmp reads
             * sq_tmp.sq_thread_resource_mgmt to size the per-stage ring buffer segments. */
            memcpy(command_writer->state_draw.sq_tmp.sq_thread_resource_mgmt,
                   new_thread_mgmt, sizeof(uint32_t) * 2);
         }
      }
   }
}

void
terakan_pipeline_graphics_destroy(struct terakan_pipeline_graphics * const pipeline,
                                  VkAllocationCallbacks const * allocator)
{
   if (allocator == NULL) {
      allocator = &pipeline->base.base.device->alloc;
   }

   if (BITSET_TEST(pipeline->static_state, TERAKAN_PIPELINE_GRAPHICS_STATE_SQ_PGM_FS) &&
       pipeline->vertex_input.sq_pgm_fs.program_bo != NULL) {
      terakan_bo_free(pipeline->vertex_input.sq_pgm_fs.program_bo, allocator);
   }

   unsigned remaining_shader_stages = (unsigned)pipeline->shader_stages;
   while (remaining_shader_stages) {
      terakan_shader_impl_finish(
         &pipeline->shaders[vk_to_mesa_shader_stage((
            VkShaderStageFlagBits)((VkShaderStageFlags)1 << u_bit_scan(&remaining_shader_stages)))],
         allocator);
   }

   terakan_pipeline_finish(&pipeline->base);

   vk_free(allocator, pipeline);
}

/* If attributes_needed_by_vs is NULL (in case of a library with vertex input state, but no
 * pre-rasterization state), all provided inputs are assumed to be needed, but the fetch shader
 * isn't created.
 */
static VkResult
terakan_pipeline_graphics_vertex_input_init(struct terakan_pipeline_graphics * const pipeline,
                                            struct vk_graphics_pipeline_state const * const state,
                                            struct terakan_device * const device,
                                            BITSET_WORD const * const attributes_needed_by_vs,
                                            VkAllocationCallbacks const * const allocator)
{
   VkResult result;

   if (state->ia != NULL) {
      /* TERAKAN_PIPELINE_GRAPHICS_STATE_VGT_PRIMITIVE_TYPE */
      if (!BITSET_TEST(state->dynamic, MESA_VK_DYNAMIC_IA_PRIMITIVE_TOPOLOGY)) {
         pipeline->vertex_input.vgt_primitive_type =
            terakan_state_draw_primitive_topology_vgt_primitive_type(state->ia->primitive_topology);
         BITSET_SET(pipeline->static_state, TERAKAN_PIPELINE_GRAPHICS_STATE_VGT_PRIMITIVE_TYPE);
      }
   }

   /* TERAKAN_PIPELINE_GRAPHICS_STATE_SQ_PGM_FS,
    * TERAKAN_PIPELINE_GRAPHICS_STATE_SQ_RESOURCES_FS_STRIDE,
    * TERAKAN_PIPELINE_GRAPHICS_STATE_SQ_PGM_FS_2048_STRIDE_WORKAROUND
    */
   if (state->vi != NULL && !BITSET_TEST(state->dynamic, MESA_VK_DYNAMIC_VI)) {
      struct terakan_vertex_input_static_state * const fs_state = &pipeline->vertex_input.sq_pgm_fs;

      bool const is_r9xx = terakan_device_physical_device(device)->chip_info.is_r9xx;

      uint32_t const bindings_provided = (uint32_t)state->vi->bindings_valid;

      BITSET_ZERO(fs_state->attributes_needed_and_provided);
      static_assert(
         sizeof(BITSET_WORD) >= sizeof(uint32_t),
         "Assuming that vk_vertex_input_state::attributes_valid can fit into one bitset word, as "
         "the maximum attribute count inside Terakan is more flexible than in the Mesa Vulkan "
         "runtime, with the possibility to expose more than 32 reserved for future.");
      fs_state->attributes_needed_and_provided[0] = state->vi->attributes_valid;
      if (attributes_needed_by_vs != NULL) {
         fs_state->attributes_needed_and_provided[0] = attributes_needed_by_vs[0];
      }
      fs_state->bindings_needed_by_attributes_and_provided = 0b0;
      unsigned attribute_index;
      BITSET_FOREACH_SET (attribute_index, fs_state->attributes_needed_and_provided,
                          TERAKAN_VERTEX_INPUT_MAX_ATTRIBUTES) {
         struct vk_vertex_attribute_state const * const attribute =
            &state->vi->attributes[attribute_index];
         uint32_t const attribute_binding_bit = BITFIELD_BIT(attribute->binding);
         if (unlikely(!terakan_vertex_input_attribute_translate(
                attribute_index, attribute->binding, attribute->format, attribute->offset,
                &fs_state->attributes[attribute_index]))) {
            return vk_errorf(
               device, VK_ERROR_VALIDATION_FAILED_EXT,
               "Failed to translate vertex attribute %d: binding %" PRIu32 ", format %s "
               "(%" PRIu32 "), offset %" PRIu32,
               attribute_index, attribute->binding, vk_Format_to_str(attribute->format),
               (uint32_t)attribute->format, attribute->offset);
         }
         assert(bindings_provided & attribute_binding_bit);
         if (unlikely(!(bindings_provided & attribute_binding_bit))) {
            return vk_errorf(device, VK_ERROR_VALIDATION_FAILED_EXT,
                             "Vertex attribute %d uses binding %" PRIu32 ", which is not "
                             "provided",
                             attribute_index, attribute->binding);
         }
         fs_state->bindings_needed_by_attributes_and_provided |= attribute_binding_bit;
      }

      fs_state->instance_bindings = 0b0;
      {
         unsigned bindings_remaining = fs_state->bindings_needed_by_attributes_and_provided;
         while (bindings_remaining) {
            int const binding_index = u_bit_scan(&bindings_remaining);
            struct vk_vertex_binding_state const * const binding =
               &state->vi->bindings[binding_index];
            if (binding->input_rate == VK_VERTEX_INPUT_RATE_INSTANCE) {
               fs_state->instance_bindings |= BITFIELD_BIT(binding_index);
               fs_state->instance_binding_divisors[binding_index] = binding->divisor;
            }
         }
      }

      fs_state->bindings_with_2048_stride_workaround = 0b0;
      if (bindings_provided && !BITSET_TEST(state->dynamic, MESA_VK_DYNAMIC_VI_BINDING_STRIDES)) {
         pipeline->vertex_input.sq_resources_fs_stride.bindings_with_static_stride =
            bindings_provided;
         {
            unsigned bindings_remaining =
               pipeline->vertex_input.sq_resources_fs_stride.bindings_with_static_stride;
            while (bindings_remaining) {
               int const binding_index = u_bit_scan(&bindings_remaining);
               uint16_t const binding_stride = state->vi->bindings[binding_index].stride;
               pipeline->vertex_input.sq_resources_fs_stride.binding_strides[binding_index] =
                  binding_stride;
               if (!is_r9xx && binding_stride >= 2048) {
                  fs_state->bindings_with_2048_stride_workaround |= BITFIELD_BIT(binding_index);
               }
            }
         }
         BITSET_SET(pipeline->static_state, TERAKAN_PIPELINE_GRAPHICS_STATE_SQ_RESOURCES_FS_STRIDE);
         if (!is_r9xx) {
            BITSET_SET(pipeline->static_state,
                       TERAKAN_PIPELINE_GRAPHICS_STATE_SQ_PGM_FS_2048_STRIDE_WORKAROUND);
         }
      }

      /* If attributes_needed_by_vs is NULL, this is a pipeline library with a vertex input state,
       * but no pre-rasterization state (no vertex shader). The fetch shader will be created when
       * they're linked for only the needed attributes.
       * If there are no valid needed attributes (thus no used bindings either), don't create a
       * fetch shader, instead use the empty one.
       */
      if (attributes_needed_by_vs == NULL ||
          !fs_state->bindings_needed_by_attributes_and_provided) {
         fs_state->program_bo = NULL;
         fs_state->program_va_shr8 = 0;
      } else {
         uint32_t fs_alu_qword_count, fs_alu_clause_count, fs_fetch_count;
         uint32_t fs_alu[2 * TERAKAN_VERTEX_INPUT_FS_MAX_ALU_QWORDS];
         uint8_t fs_alu_clause_qwords[TERAKAN_VERTEX_INPUT_FS_MAX_ALU_CLAUSES];
         uint32_t fs_fetch[4 * TERAKAN_VERTEX_INPUT_MAX_ATTRIBUTES];
         terakan_vertex_input_create_fs_alu_and_fetches(
            is_r9xx, fs_state->attributes_needed_and_provided, fs_state->attributes,
            fs_state->instance_bindings, fs_state->instance_binding_divisors,
            fs_state->bindings_with_2048_stride_workaround, &fs_alu_qword_count, fs_alu,
            &fs_alu_clause_count, fs_alu_clause_qwords, &fs_fetch_count, fs_fetch);
         /* TODO(Triang3l): Suballocate the fetch shader as well as other shaders. */
         result = device->winsys_fn->bo->allocate_device_memory(
            device,
            terakan_vertex_input_fs_byte_count(fs_alu_qword_count, fs_alu_clause_count,
                                               fs_fetch_count),
            TERAKAN_SHADER_PROGRAM_ALIGNMENT,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
               VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            0, allocator, VK_SYSTEM_ALLOCATION_SCOPE_OBJECT, &fs_state->program_bo);
         if (result != VK_SUCCESS) {
            return vk_error(device, result);
         }
         fs_state->program_va_shr8 = 0;
         void * const fs_mapping = terakan_bo_map(fs_state->program_bo);
         if (fs_mapping == NULL) {
            terakan_bo_free(fs_state->program_bo, allocator);
            return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);
         }
         terakan_vertex_input_create_fs_program(is_r9xx, fs_alu_qword_count, fs_alu,
                                                fs_alu_clause_count, fs_alu_clause_qwords,
                                                fs_fetch_count, fs_fetch, fs_mapping);
         terakan_bo_unmap(fs_state->program_bo);
      }

      BITSET_SET(pipeline->static_state, TERAKAN_PIPELINE_GRAPHICS_STATE_SQ_PGM_FS);
   }

   return VK_SUCCESS;
}

static void
terakan_pipeline_graphics_pre_rasterization_init(
   struct terakan_pipeline_graphics * const pipeline,
   struct vk_graphics_pipeline_state const * const state, bool const depth_range_unrestricted)
{
   if (state->rs != NULL) {
      /* TERAKAN_PIPELINE_GRAPHICS_STATE_PA_SC_VPORT_Z_MIN_0_MAX_1,
       * TERAKAN_PIPELINE_GRAPHICS_STATE_DB_RENDER_OVERRIDE_PRE_RASTERIZATION
       */
      pipeline->pre_rasterization.db_render_override_clear = UINT32_MAX;
      pipeline->pre_rasterization.db_render_override = 0;
      if (!BITSET_TEST(state->dynamic, MESA_VK_DYNAMIC_RS_DEPTH_CLAMP_ENABLE)) {
         if (depth_range_unrestricted) {
            pipeline->pre_rasterization.db_render_override_clear &= C_02800C_DISABLE_VIEWPORT_CLAMP;
            pipeline->pre_rasterization.db_render_override |=
               S_02800C_DISABLE_VIEWPORT_CLAMP(!state->rs->depth_clamp_enable);
         } else {
            pipeline->pre_rasterization.pa_sc_vport_z_min_0_max_1 = !state->rs->depth_clamp_enable;
            BITSET_SET(pipeline->static_state,
                       TERAKAN_PIPELINE_GRAPHICS_STATE_PA_SC_VPORT_Z_MIN_0_MAX_1);
         }
      }
      assert(!(pipeline->pre_rasterization.db_render_override &
               pipeline->pre_rasterization.db_render_override_clear));
      if (pipeline->pre_rasterization.db_render_override_clear != UINT32_MAX) {
         BITSET_SET(pipeline->static_state,
                    TERAKAN_PIPELINE_GRAPHICS_STATE_DB_RENDER_OVERRIDE_PRE_RASTERIZATION);
      }
   }

   if (state->vp != NULL) {
      /* TERAKAN_PIPELINE_GRAPHICS_STATE_VIEWPORT_COUNT */
      if (!BITSET_TEST(state->dynamic, MESA_VK_DYNAMIC_VP_VIEWPORT_COUNT)) {
         assert(state->vp->viewport_count <= ARRAY_SIZE(pipeline->pre_rasterization.viewports));
         pipeline->pre_rasterization.viewport_count = (uint8_t)state->vp->viewport_count;
         BITSET_SET(pipeline->static_state, TERAKAN_PIPELINE_GRAPHICS_STATE_VIEWPORT_COUNT);
         /* TERAKAN_PIPELINE_GRAPHICS_STATE_VIEWPORT */
         if (!BITSET_TEST(state->dynamic, MESA_VK_DYNAMIC_VP_VIEWPORTS)) {
            for (uint8_t viewport_index = 0;
                 viewport_index < pipeline->pre_rasterization.viewport_count; ++viewport_index) {
               terakan_state_draw_viewport_translate(
                  &state->vp->viewports[viewport_index],
                  &pipeline->pre_rasterization.viewports[viewport_index]);
            }
            BITSET_SET(pipeline->static_state, TERAKAN_PIPELINE_GRAPHICS_STATE_VIEWPORT);
         }
      }

      /* TERAKAN_PIPELINE_GRAPHICS_STATE_PA_SC_VPORT_GENERIC_SCISSOR */
      if (!BITSET_TEST(state->dynamic, MESA_VK_DYNAMIC_VP_SCISSORS)) {
         assert(state->vp->scissor_count <=
                ARRAY_SIZE(pipeline->pre_rasterization.pa_sc_vport_generic_scissor_tl_br_xy));
         pipeline->pre_rasterization.pa_sc_vport_generic_scissor_count = state->vp->scissor_count;
         for (uint32_t scissor_index = 0; scissor_index < state->vp->scissor_count;
              ++scissor_index) {
            terakan_state_translate_window_rect_unpacked(
               &state->vp->scissors[scissor_index],
               pipeline->pre_rasterization.pa_sc_vport_generic_scissor_tl_br_xy[scissor_index][0]);
         }
         BITSET_SET(pipeline->static_state,
                    TERAKAN_PIPELINE_GRAPHICS_STATE_PA_SC_VPORT_GENERIC_SCISSOR);
      }
   }

   /* TERAKAN_PIPELINE_GRAPHICS_STATE_PA_CL_CLIP_CNTL */
   pipeline->pre_rasterization.pa_cl_clip_cntl_clear = UINT32_MAX;
   pipeline->pre_rasterization.pa_cl_clip_cntl = 0;
   if (state->vp != NULL) {
      if (!BITSET_TEST(state->dynamic, MESA_VK_DYNAMIC_VP_DEPTH_CLIP_NEGATIVE_ONE_TO_ONE)) {
         pipeline->pre_rasterization.pa_cl_clip_cntl_clear &= C_028810_DX_CLIP_SPACE_DEF;
         pipeline->pre_rasterization.pa_cl_clip_cntl |=
            S_028810_DX_CLIP_SPACE_DEF(!state->vp->depth_clip_negative_one_to_one);
      }
   }
   if (state->rs != NULL) {
      if (!BITSET_TEST(state->dynamic, MESA_VK_DYNAMIC_RS_RASTERIZER_DISCARD_ENABLE)) {
         pipeline->pre_rasterization.pa_cl_clip_cntl_clear &=
            TERAKAN_STATE_DRAW_RASTERIZER_DISCARD_ENABLE_PA_CL_CLIP_CNTL_CLEAR;
         pipeline->pre_rasterization.pa_cl_clip_cntl |=
            terakan_state_draw_rasterizer_discard_enable_pa_cl_clip_cntl(
               state->rs->rasterizer_discard_enable);
      }
   }
   pipeline->pre_rasterization.cmd_set_depth_clamp_enable_sets_depth_clip_enable = false;
   if (state->rs != NULL) {
      if (!BITSET_TEST(state->dynamic, MESA_VK_DYNAMIC_RS_DEPTH_CLIP_ENABLE)) {
         if (state->rs->depth_clip_enable == VK_MESA_DEPTH_CLIP_ENABLE_NOT_CLAMP) {
            pipeline->pre_rasterization.cmd_set_depth_clamp_enable_sets_depth_clip_enable = true;
            if (!BITSET_TEST(state->dynamic, MESA_VK_DYNAMIC_RS_DEPTH_CLAMP_ENABLE)) {
               pipeline->pre_rasterization.pa_cl_clip_cntl_clear &=
                  TERAKAN_STATE_DRAW_DEPTH_CLIP_ENABLE_PA_CL_CLIP_CNTL_CLEAR;
               pipeline->pre_rasterization.pa_cl_clip_cntl |=
                  terakan_state_draw_depth_clip_enable_pa_cl_clip_cntl(
                     !state->rs->depth_clamp_enable);
            }
         } else {
            pipeline->pre_rasterization.pa_cl_clip_cntl_clear &=
               TERAKAN_STATE_DRAW_DEPTH_CLIP_ENABLE_PA_CL_CLIP_CNTL_CLEAR;
            pipeline->pre_rasterization.pa_cl_clip_cntl |=
               terakan_state_draw_depth_clip_enable_pa_cl_clip_cntl(state->rs->depth_clip_enable ==
                                                                    VK_MESA_DEPTH_CLIP_ENABLE_TRUE);
         }
      }
   }
   assert(!(pipeline->pre_rasterization.pa_cl_clip_cntl &
            pipeline->pre_rasterization.pa_cl_clip_cntl_clear));
   if (pipeline->pre_rasterization.pa_cl_clip_cntl_clear != UINT32_MAX) {
      BITSET_SET(pipeline->static_state, TERAKAN_PIPELINE_GRAPHICS_STATE_PA_CL_CLIP_CNTL);
   }

   if (state->rs != NULL) {
      /* TERAKAN_PIPELINE_GRAPHICS_STATE_PA_SU_SC_MODE_CNTL */
      pipeline->pre_rasterization.pa_su_sc_mode_cntl_clear = UINT32_MAX;
      pipeline->pre_rasterization.pa_su_sc_mode_cntl = 0;
      if (!BITSET_TEST(state->dynamic, MESA_VK_DYNAMIC_RS_POLYGON_MODE)) {
         pipeline->pre_rasterization.pa_su_sc_mode_cntl_clear &=
            TERAKAN_STATE_DRAW_POLYGON_MODE_PA_SU_SC_MODE_CNTL_CLEAR;
         pipeline->pre_rasterization.pa_su_sc_mode_cntl |=
            terakan_state_draw_polygon_mode_pa_su_sc_mode_cntl(state->rs->polygon_mode);
      }
      if (!BITSET_TEST(state->dynamic, MESA_VK_DYNAMIC_RS_CULL_MODE)) {
         pipeline->pre_rasterization.pa_su_sc_mode_cntl_clear &=
            TERAKAN_STATE_DRAW_CULL_MODE_PA_SU_SC_MODE_CNTL_CLEAR;
         pipeline->pre_rasterization.pa_su_sc_mode_cntl |=
            terakan_state_draw_cull_mode_pa_su_sc_mode_cntl(state->rs->cull_mode);
      }
      if (!BITSET_TEST(state->dynamic, MESA_VK_DYNAMIC_RS_FRONT_FACE)) {
         pipeline->pre_rasterization.pa_su_sc_mode_cntl_clear &=
            TERAKAN_STATE_DRAW_FRONT_FACE_PA_SU_SC_MODE_CNTL_CLEAR;
         pipeline->pre_rasterization.pa_su_sc_mode_cntl |=
            terakan_state_draw_front_face_pa_su_sc_mode_cntl(state->rs->front_face);
      }
      if (!BITSET_TEST(state->dynamic, MESA_VK_DYNAMIC_RS_PROVOKING_VERTEX)) {
         pipeline->pre_rasterization.pa_su_sc_mode_cntl_clear &=
            TERAKAN_STATE_DRAW_PROVOKING_VERTEX_MODE_PA_SU_SC_MODE_CNTL_CLEAR;
         pipeline->pre_rasterization.pa_su_sc_mode_cntl |=
            terakan_state_draw_provoking_vertex_mode_pa_su_sc_mode_cntl(
               state->rs->provoking_vertex);
      }
      if (!BITSET_TEST(state->dynamic, MESA_VK_DYNAMIC_RS_DEPTH_BIAS_ENABLE)) {
         pipeline->pre_rasterization.pa_su_sc_mode_cntl_clear &=
            TERAKAN_STATE_DRAW_DEPTH_BIAS_ENABLE_PA_SU_SC_MODE_CNTL_CLEAR;
         pipeline->pre_rasterization.pa_su_sc_mode_cntl |=
            terakan_state_draw_depth_bias_enable_pa_su_sc_mode_cntl(state->rs->depth_bias.enable);
      }
      assert(!(pipeline->pre_rasterization.pa_su_sc_mode_cntl &
               pipeline->pre_rasterization.pa_su_sc_mode_cntl_clear));
      if (pipeline->pre_rasterization.pa_su_sc_mode_cntl_clear != UINT32_MAX) {
         BITSET_SET(pipeline->static_state, TERAKAN_PIPELINE_GRAPHICS_STATE_PA_SU_SC_MODE_CNTL);
      }

      /* TERAKAN_PIPELINE_GRAPHICS_STATE_PA_SU_POLY_OFFSET */
      if (!BITSET_TEST(state->dynamic, MESA_VK_DYNAMIC_RS_DEPTH_BIAS_FACTORS)) {
         pipeline->pre_rasterization.pa_su_poly_offset.clamp = state->rs->depth_bias.clamp;
         pipeline->pre_rasterization.pa_su_poly_offset.subpixel_slope_scale =
            TERAKAN_HW_STATE_DRAW_POLY_OFFSET_SLOPE_SUBPIXELS_IN_PIXEL *
            state->rs->depth_bias.slope_factor;
         pipeline->pre_rasterization.pa_su_poly_offset.offset =
            state->rs->depth_bias.constant_factor;
         pipeline->pre_rasterization.pa_su_poly_offset.representation =
            state->rs->depth_bias.representation;
         pipeline->pre_rasterization.pa_su_poly_offset.representation_exact =
            state->rs->depth_bias.exact;
         BITSET_SET(pipeline->static_state, TERAKAN_PIPELINE_GRAPHICS_STATE_PA_SU_POLY_OFFSET);
      }
   }
}

static void
terakan_pipeline_graphics_multisample_init(struct terakan_pipeline_graphics * const pipeline,
                                           struct vk_graphics_pipeline_state const * const state)
{
   if (state->ms != NULL) {
      /* TERAKAN_PIPELINE_GRAPHICS_STATE_PA_SC_AA_MASK */
      if (!BITSET_TEST(state->dynamic, MESA_VK_DYNAMIC_MS_SAMPLE_MASK)) {
         pipeline->multisample.pa_sc_aa_mask = (uint16_t)state->ms->sample_mask;
         BITSET_SET(pipeline->static_state, TERAKAN_PIPELINE_GRAPHICS_STATE_PA_SC_AA_MASK);
      }
   }
}

static void
terakan_pipeline_graphics_fragment_shader_state_init(
   struct terakan_pipeline_graphics * const pipeline,
   struct vk_graphics_pipeline_state const * const state)
{
   if (state->ds != NULL) {
      /* TERAKAN_PIPELINE_GRAPHICS_STATE_DB_STENCILREFMASK */
      if (BITSET_TEST(state->dynamic, MESA_VK_DYNAMIC_DS_STENCIL_TEST_ENABLE) ||
          state->ds->stencil.test_enable) {
         pipeline->fragment_shader.db_stencilrefmask_clear = UINT32_MAX;
         memset(pipeline->fragment_shader.db_stencilrefmask_front_back, 0,
                sizeof(pipeline->fragment_shader.db_stencilrefmask_front_back));
         if (!BITSET_TEST(state->dynamic, MESA_VK_DYNAMIC_DS_STENCIL_COMPARE_MASK)) {
            pipeline->fragment_shader.db_stencilrefmask_clear &= C_028430_STENCILMASK;
            pipeline->fragment_shader.db_stencilrefmask_front_back[0] |=
               S_028430_STENCILMASK(state->ds->stencil.front.compare_mask);
            pipeline->fragment_shader.db_stencilrefmask_front_back[1] |=
               S_028430_STENCILMASK(state->ds->stencil.back.compare_mask);
         }
         if (!BITSET_TEST(state->dynamic, MESA_VK_DYNAMIC_DS_STENCIL_WRITE_MASK)) {
            pipeline->fragment_shader.db_stencilrefmask_clear &= C_028430_STENCILWRITEMASK;
            pipeline->fragment_shader.db_stencilrefmask_front_back[0] |=
               S_028430_STENCILWRITEMASK(state->ds->stencil.front.write_mask);
            pipeline->fragment_shader.db_stencilrefmask_front_back[1] |=
               S_028430_STENCILWRITEMASK(state->ds->stencil.back.write_mask);
         }
         if (!BITSET_TEST(state->dynamic, MESA_VK_DYNAMIC_DS_STENCIL_REFERENCE)) {
            pipeline->fragment_shader.db_stencilrefmask_clear &= C_028430_STENCILREF;
            pipeline->fragment_shader.db_stencilrefmask_front_back[0] |=
               S_028430_STENCILREF(state->ds->stencil.front.reference);
            pipeline->fragment_shader.db_stencilrefmask_front_back[1] |=
               S_028430_STENCILREF(state->ds->stencil.back.reference);
         }
         assert(!((pipeline->fragment_shader.db_stencilrefmask_front_back[0] |
                   pipeline->fragment_shader.db_stencilrefmask_front_back[1]) &
                  pipeline->fragment_shader.db_stencilrefmask_clear));
         if (pipeline->fragment_shader.db_stencilrefmask_clear != UINT32_MAX) {
            BITSET_SET(pipeline->static_state, TERAKAN_PIPELINE_GRAPHICS_STATE_DB_STENCILREFMASK);
         }
      }

      /* TERAKAN_PIPELINE_GRAPHICS_STATE_DB_DEPTH_CONTROL */
      pipeline->fragment_shader.db_depth_control_clear = UINT32_MAX;
      pipeline->fragment_shader.db_depth_control = 0;
      if (!BITSET_TEST(state->dynamic, MESA_VK_DYNAMIC_DS_DEPTH_TEST_ENABLE)) {
         pipeline->fragment_shader.db_depth_control_clear &= C_028800_Z_ENABLE;
         pipeline->fragment_shader.db_depth_control |=
            S_028800_Z_ENABLE(state->ds->depth.test_enable);
      }
      if (!BITSET_TEST(state->dynamic, MESA_VK_DYNAMIC_DS_DEPTH_WRITE_ENABLE)) {
         pipeline->fragment_shader.db_depth_control_clear &= C_028800_Z_WRITE_ENABLE;
         pipeline->fragment_shader.db_depth_control |=
            S_028800_Z_WRITE_ENABLE(state->ds->depth.write_enable);
      }
      if (!BITSET_TEST(state->dynamic, MESA_VK_DYNAMIC_DS_DEPTH_COMPARE_OP)) {
         pipeline->fragment_shader.db_depth_control_clear &= C_028800_ZFUNC;
         pipeline->fragment_shader.db_depth_control |=
            S_028800_ZFUNC((uint32_t)state->ds->depth.compare_op);
      }
      if (!BITSET_TEST(state->dynamic, MESA_VK_DYNAMIC_DS_STENCIL_TEST_ENABLE)) {
         pipeline->fragment_shader.db_depth_control_clear &= C_028800_STENCIL_ENABLE;
         pipeline->fragment_shader.db_depth_control |=
            S_028800_STENCIL_ENABLE(state->ds->stencil.test_enable);
      }
      if (!BITSET_TEST(state->dynamic, MESA_VK_DYNAMIC_DS_STENCIL_OP)) {
         pipeline->fragment_shader.db_depth_control_clear &=
            C_028800_STENCILFAIL & C_028800_STENCILZPASS & C_028800_STENCILZFAIL &
            C_028800_STENCILFUNC & C_028800_STENCILFAIL_BF & C_028800_STENCILZPASS_BF &
            C_028800_STENCILZFAIL_BF & C_028800_STENCILFUNC_BF;
         pipeline->fragment_shader.db_depth_control |=
            S_028800_STENCILFAIL((uint32_t)state->ds->stencil.front.op.fail) |
            S_028800_STENCILZPASS((uint32_t)state->ds->stencil.front.op.pass) |
            S_028800_STENCILZFAIL((uint32_t)state->ds->stencil.front.op.depth_fail) |
            S_028800_STENCILFUNC((uint32_t)state->ds->stencil.front.op.compare) |
            S_028800_STENCILFAIL_BF((uint32_t)state->ds->stencil.back.op.fail) |
            S_028800_STENCILZPASS_BF((uint32_t)state->ds->stencil.back.op.pass) |
            S_028800_STENCILZFAIL_BF((uint32_t)state->ds->stencil.back.op.depth_fail) |
            S_028800_STENCILFUNC_BF((uint32_t)state->ds->stencil.back.op.compare);
      }
      assert(!(pipeline->fragment_shader.db_depth_control &
               pipeline->fragment_shader.db_depth_control_clear));
      if (pipeline->fragment_shader.db_depth_control_clear != UINT32_MAX) {
         BITSET_SET(pipeline->static_state, TERAKAN_PIPELINE_GRAPHICS_STATE_DB_DEPTH_CONTROL);
      }
   }
}

static void
terakan_pipeline_graphics_fragment_output_init(struct terakan_pipeline_graphics * const pipeline,
                                               struct vk_graphics_pipeline_state const * const state)
{
   if (state->cb != NULL) {
      /* TERAKAN_PIPELINE_GRAPHICS_STATE_LOGIC_OP_ENABLE */
      bool logic_op_potentially_enabled = true;
      if (!BITSET_TEST(state->dynamic, MESA_VK_DYNAMIC_CB_LOGIC_OP_ENABLE)) {
         bool const logic_op_enable = state->cb->logic_op_enable;
         pipeline->fragment_output.logic_op_enable = logic_op_enable;
         BITSET_SET(pipeline->static_state, TERAKAN_PIPELINE_GRAPHICS_STATE_LOGIC_OP_ENABLE);
         logic_op_potentially_enabled = logic_op_enable;
      }

      /* TERAKAN_PIPELINE_GRAPHICS_STATE_LOGIC_OP_ROP3
       * Optimize out if the logical operation is known to be disabled.
       */
      if (logic_op_potentially_enabled &&
          !BITSET_TEST(state->dynamic, MESA_VK_DYNAMIC_CB_LOGIC_OP)) {
         pipeline->fragment_output.logic_op_rop3 =
            terakan_state_draw_logic_op_rop3((VkLogicOp)state->cb->logic_op);
         BITSET_SET(pipeline->static_state, TERAKAN_PIPELINE_GRAPHICS_STATE_LOGIC_OP_ROP3);
      }

      /* TERAKAN_PIPELINE_GRAPHICS_STATE_CB_BLEND_RGBA */
      if (!BITSET_TEST(state->dynamic, MESA_VK_DYNAMIC_CB_BLEND_CONSTANTS)) {
         memcpy(pipeline->fragment_output.cb_blend_rgba, state->cb->blend_constants,
                sizeof(float) * 4);
         BITSET_SET(pipeline->static_state, TERAKAN_PIPELINE_GRAPHICS_STATE_CB_BLEND_RGBA);
      }

      /* Must be ignored if all states using it are dynamic, don't insert assertions unless it's
       * actually used.
       */
      pipeline->fragment_output.color_blend_attachment_count = state->cb->attachment_count;

      memset(pipeline->fragment_output.cb_blend_control, 0,
             sizeof(pipeline->fragment_output.cb_blend_control));

      /* TERAKAN_PIPELINE_GRAPHICS_STATE_CB_BLEND_CONTROL_ENABLE */
      if (!BITSET_TEST(state->dynamic, MESA_VK_DYNAMIC_CB_BLEND_ENABLES)) {
         for (uint32_t attachment_index = 0;
              attachment_index < pipeline->fragment_output.color_blend_attachment_count;
              ++attachment_index) {
            pipeline->fragment_output.cb_blend_control[attachment_index] |=
               S_028780_BLEND_CONTROL_ENABLE(state->cb->attachments[attachment_index].blend_enable);
         }
         BITSET_SET(pipeline->static_state,
                    TERAKAN_PIPELINE_GRAPHICS_STATE_CB_BLEND_CONTROL_ENABLE);
      }

      /* TERAKAN_PIPELINE_GRAPHICS_STATE_CB_BLEND_CONTROL_EQUATION */
      if (!BITSET_TEST(state->dynamic, MESA_VK_DYNAMIC_CB_BLEND_EQUATIONS)) {
         for (uint32_t attachment_index = 0;
              attachment_index < pipeline->fragment_output.color_blend_attachment_count;
              ++attachment_index) {
            struct vk_color_blend_attachment_state const * const attachment =
               &state->cb->attachments[attachment_index];
            bool const allow_dual_source = attachment_index == 0;
            /* According to VkPipelineColorBlendAttachmentState VUIDs, all factors and operations
             * must be valid values regardless of blendEnable, so there's no need to handle
             * blendEnable being static here.
             */
            pipeline->fragment_output.cb_blend_control[attachment_index] |=
               S_028780_COLOR_SRCBLEND(terakan_state_draw_blend_factor_translate(
                  attachment->src_color_blend_factor, allow_dual_source)) |
               S_028780_COLOR_DESTBLEND(terakan_state_draw_blend_factor_translate(
                  attachment->dst_color_blend_factor, allow_dual_source)) |
               S_028780_COLOR_COMB_FCN(
                  terakan_state_draw_blend_op_translate(attachment->color_blend_op));
            if (attachment->src_alpha_blend_factor != attachment->src_color_blend_factor ||
                attachment->dst_alpha_blend_factor != attachment->dst_color_blend_factor ||
                attachment->alpha_blend_op != attachment->color_blend_op) {
               pipeline->fragment_output.cb_blend_control[attachment_index] |=
                  S_028780_SEPARATE_ALPHA_BLEND(1) |
                  S_028780_ALPHA_SRCBLEND(terakan_state_draw_blend_factor_translate(
                     attachment->src_alpha_blend_factor, allow_dual_source)) |
                  S_028780_ALPHA_DESTBLEND(terakan_state_draw_blend_factor_translate(
                     attachment->dst_alpha_blend_factor, allow_dual_source)) |
                  S_028780_ALPHA_COMB_FCN(
                     terakan_state_draw_blend_op_translate(attachment->alpha_blend_op));
            }
         }
         BITSET_SET(pipeline->static_state,
                    TERAKAN_PIPELINE_GRAPHICS_STATE_CB_BLEND_CONTROL_EQUATION);
      }

      /* TERAKAN_PIPELINE_GRAPHICS_STATE_COLOR_ATTACHMENT_WRITE_MASK */
      if (!BITSET_TEST(state->dynamic, MESA_VK_DYNAMIC_CB_WRITE_MASKS)) {
         for (uint32_t attachment_index = 0;
              attachment_index < pipeline->fragment_output.color_blend_attachment_count;
              ++attachment_index) {
            pipeline->fragment_output.color_attachment_write_masks[attachment_index] =
               state->cb->attachments[attachment_index].write_mask & 0b1111;
         }
         BITSET_SET(pipeline->static_state,
                    TERAKAN_PIPELINE_GRAPHICS_STATE_COLOR_ATTACHMENT_WRITE_MASK);
      }

      /* TERAKAN_PIPELINE_GRAPHICS_STATE_COLOR_ATTACHMENT_WRITE_ENABLE */
      if (!BITSET_TEST(state->dynamic, MESA_VK_DYNAMIC_CB_COLOR_WRITE_ENABLES)) {
         pipeline->fragment_output.color_attachment_write_enable =
            state->cb->color_write_enables & BITFIELD_MASK(TERAKAN_COLOR_HW_RTV_COUNT);
         BITSET_SET(pipeline->static_state,
                    TERAKAN_PIPELINE_GRAPHICS_STATE_COLOR_ATTACHMENT_WRITE_ENABLE);
      }
   }
}


/*
 * Phase A: Allocate and initialize base pipeline object.
 * Only allocation and base-object init — no compilation, no state parsing.
 */
static VkResult
terakan_pipeline_graphics_init(struct terakan_device * const device,
                               VkAllocationCallbacks const * const allocator,
                               struct terakan_pipeline_graphics ** const pipeline_out)
{
   /* vk_zalloc2 atomically allocates and zero-initialises the whole pipeline
    * struct.  Zeroing is an intrinsic guarantee of object creation, not a
    * bandage applied by the caller — the teardown path relies on all pointer
    * fields being NULL and all stage bitmasks being 0 until explicitly set
    * by the compilation phase.  Do NOT replace with vk_alloc2 + memset. */
   struct terakan_pipeline_graphics * const pipeline =
      vk_zalloc2(&device->vk.alloc, allocator, sizeof(struct terakan_pipeline_graphics),
                 alignof(struct terakan_pipeline_graphics), VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (pipeline == NULL)
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);

   terakan_pipeline_init(&pipeline->base, device, false);

   *pipeline_out = pipeline;
   return VK_SUCCESS;
}

static char const *
terakan_pipeline_graphics_interp_mode_name(unsigned const mode)
{
   switch (mode) {
   case TGSI_INTERPOLATE_CONSTANT:
      return "flat";
   case TGSI_INTERPOLATE_LINEAR:
      return "noperspective";
   case TGSI_INTERPOLATE_PERSPECTIVE:
      return "smooth";
   case TGSI_INTERPOLATE_COLOR:
      return "color";
   default:
      return "unknown";
   }
}

static char const *
terakan_pipeline_graphics_interp_loc_name(unsigned const location)
{
   switch (location) {
   case TGSI_INTERPOLATE_LOC_CENTER:
      return "center";
   case TGSI_INTERPOLATE_LOC_CENTROID:
      return "centroid";
   case TGSI_INTERPOLATE_LOC_SAMPLE:
      return "sample";
   default:
      return "unknown";
   }
}

static bool
terakan_pipeline_graphics_spi_baryc_enabled(uint32_t const spi_baryc_cntl,
                                             bool const noperspective,
                                             unsigned const location)
{
   switch (location) {
   case TGSI_INTERPOLATE_LOC_CENTER:
      return noperspective ? G_0286E0_LINEAR_CENTER_ENA(spi_baryc_cntl) != 0
                           : G_0286E0_PERSP_CENTER_ENA(spi_baryc_cntl) != 0;
   case TGSI_INTERPOLATE_LOC_CENTROID:
      return noperspective ? G_0286E0_LINEAR_CENTROID_ENA(spi_baryc_cntl) != 0
                           : G_0286E0_PERSP_CENTROID_ENA(spi_baryc_cntl) != 0;
   case TGSI_INTERPOLATE_LOC_SAMPLE:
      return noperspective ? G_0286E0_LINEAR_SAMPLE_ENA(spi_baryc_cntl) != 0
                           : G_0286E0_PERSP_SAMPLE_ENA(spi_baryc_cntl) != 0;
   default:
      return false;
   }
}

static VkResult
terakan_pipeline_graphics_validate_vs_fs_spi_routing(
   struct terakan_device * const device,
   struct terakan_shader_impl const * const vs,
   struct terakan_shader_impl const * const fs)
{
   assert(vs != NULL);
   assert(fs != NULL);

   uint32_t const vs_export_count =
      G_0286C4_VS_EXPORT_COUNT(vs->static_state.stage.vs.spi_vs_out_config);
   uint32_t const vs_out_id_count = vs_export_count / 4 + 1;
   if (unlikely(vs_out_id_count > ARRAY_SIZE(vs->static_state.stage.vs.spi_vs_out_id))) {
      return vk_errorf(device, VK_ERROR_VALIDATION_FAILED_EXT,
                       "VS SPI export vector count out of bounds: VS_EXPORT_COUNT=%" PRIu32
                       ", vectors=%" PRIu32 ", spi_vs_out_id_capacity=%zu",
                       vs_export_count, vs_out_id_count,
                       ARRAY_SIZE(vs->static_state.stage.vs.spi_vs_out_id));
   }

   if (unlikely(vs->shader.highest_export_param != vs_export_count)) {
      return vk_errorf(device, VK_ERROR_VALIDATION_FAILED_EXT,
                       "VS SPI export count mismatch: SPI_VS_OUT_CONFIG=%" PRIu32
                       " but shader.highest_export_param=%u",
                       vs_export_count, vs->shader.highest_export_param);
   }

   int16_t vs_param_for_sid[UINT8_MAX + 1];
   memset(vs_param_for_sid, -1, sizeof(vs_param_for_sid));

   uint8_t vs_sid_by_param[ARRAY_SIZE(vs->static_state.stage.vs.spi_vs_out_id) * 4] = {0};
   uint32_t vs_semantic_count = 0;
   for (uint32_t param = 0; param <= vs_export_count; ++param) {
      uint32_t const sid =
         (vs->static_state.stage.vs.spi_vs_out_id[param / 4] >> ((param & 3) * 8)) & 0xFFu;
      vs_sid_by_param[param] = (uint8_t)sid;
      if (sid != 0 && vs_param_for_sid[sid] < 0) {
         vs_param_for_sid[sid] = (int16_t)param;
         ++vs_semantic_count;
      }
   }

   for (uint32_t output_index = 0; output_index < vs->shader.noutput; ++output_index) {
      struct r600_shader_io const * const output = &vs->shader.output[output_index];
      if (output->export_param < 0)
         continue;

      if (unlikely((uint32_t)output->export_param > vs_export_count)) {
         return vk_errorf(device, VK_ERROR_VALIDATION_FAILED_EXT,
                          "VS export index out of range: output[%" PRIu32
                          "].export_param=%d exceeds VS_EXPORT_COUNT=%" PRIu32,
                          output_index, output->export_param, vs_export_count);
      }

      uint32_t const sid_from_spi = vs_sid_by_param[output->export_param];
      if (unlikely(output->spi_sid < 0 || (uint32_t)output->spi_sid != sid_from_spi)) {
         return vk_errorf(device, VK_ERROR_VALIDATION_FAILED_EXT,
                          "VS export semantic mismatch at param %d: shader spi_sid=%d"
                          " but SPI_VS_OUT_ID sid=%" PRIu32,
                          output->export_param, output->spi_sid, sid_from_spi);
      }

      if (sid_from_spi != 0 &&
          unlikely(vs_param_for_sid[sid_from_spi] >= 0 &&
                   vs_param_for_sid[sid_from_spi] != output->export_param)) {
         return vk_errorf(device, VK_ERROR_VALIDATION_FAILED_EXT,
                          "VS semantic SID=%" PRIu32
                          " exported by multiple params (%d and %d)",
                          sid_from_spi, vs_param_for_sid[sid_from_spi], output->export_param);
      }
   }

   uint32_t const fs_interp_count =
      G_0286CC_NUM_INTERP(fs->static_state.stage.ps.spi_ps_in_control[0]);
   if (unlikely(fs_interp_count > ARRAY_SIZE(fs->static_state.stage.ps.spi_ps_input_cntl))) {
      return vk_errorf(device, VK_ERROR_VALIDATION_FAILED_EXT,
                       "FS SPI interpolator count out of bounds: NUM_INTERP=%" PRIu32
                       ", spi_ps_input_cntl_capacity=%zu",
                       fs_interp_count,
                       ARRAY_SIZE(fs->static_state.stage.ps.spi_ps_input_cntl));
   }

   uint32_t fs_expected_interp_count = 0;
   uint32_t fs_semantic_count = 0;
   bool fs_interp_slot_used[ARRAY_SIZE(fs->static_state.stage.ps.spi_ps_input_cntl)] = {false};

   struct r600_shader_io const *position_input = NULL;
   uint32_t face_or_sample_mask_gpr = UINT32_MAX;
   uint32_t sample_id_gpr = UINT32_MAX;

   struct terakan_instance const * const instance =
      container_of(terakan_device_physical_device(device)->vk.base.instance,
                   struct terakan_instance const, vk);
   bool const debug_pipeline = (instance->debug_flags & TERAKAN_DEBUG_PIPELINE) != 0;

   for (uint32_t input_index = 0; input_index < fs->shader.ninput; ++input_index) {
      struct r600_shader_io const * const input = &fs->shader.input[input_index];

      if (input->varying_slot == VARYING_SLOT_POS) {
         if (unlikely(position_input != NULL)) {
            return vk_errorf(device, VK_ERROR_VALIDATION_FAILED_EXT,
                             "FS has multiple position inputs (indices %u and %" PRIu32 ")",
                             (unsigned)(position_input - fs->shader.input), input_index);
         }
         position_input = input;
         continue;
      }

      if (input->varying_slot == VARYING_SLOT_FACE ||
          input->system_value == SYSTEM_VALUE_SAMPLE_MASK_IN) {
         if (unlikely(face_or_sample_mask_gpr != UINT32_MAX && face_or_sample_mask_gpr != input->gpr)) {
            return vk_errorf(device, VK_ERROR_VALIDATION_FAILED_EXT,
                             "FS front-face/sample-mask GPR mismatch: expected %" PRIu32
                             ", got %u at input %" PRIu32,
                             face_or_sample_mask_gpr, input->gpr, input_index);
         }
         face_or_sample_mask_gpr = input->gpr;
         continue;
      }

      if (input->system_value == SYSTEM_VALUE_SAMPLE_ID) {
         if (unlikely(sample_id_gpr != UINT32_MAX && sample_id_gpr != input->gpr)) {
            return vk_errorf(device, VK_ERROR_VALIDATION_FAILED_EXT,
                             "FS sample-id GPR mismatch: expected %" PRIu32
                             ", got %u at input %" PRIu32,
                             sample_id_gpr, input->gpr, input_index);
         }
         sample_id_gpr = input->gpr;
         continue;
      }

      if (input->spi_sid == 0)
         continue;

      if (unlikely(input->spi_sid < 0 || input->spi_sid > UINT8_MAX)) {
         return vk_errorf(device, VK_ERROR_VALIDATION_FAILED_EXT,
                          "FS input %" PRIu32 " has invalid spi_sid=%d",
                          input_index, input->spi_sid);
      }

      if (unlikely(input->lds_pos >= ARRAY_SIZE(fs->static_state.stage.ps.spi_ps_input_cntl))) {
         return vk_errorf(device, VK_ERROR_VALIDATION_FAILED_EXT,
                          "FS input %" PRIu32 " has invalid lds_pos=%u",
                          input_index, input->lds_pos);
      }

      fs_expected_interp_count = MAX2(fs_expected_interp_count, input->lds_pos + 1);
      if (unlikely(input->lds_pos >= fs_interp_count)) {
         return vk_errorf(device, VK_ERROR_VALIDATION_FAILED_EXT,
                          "FS input %" PRIu32 " requires SPI_PS_INPUT_CNTL[%u],"
                          " but SPI_PS_IN_CONTROL.NUM_INTERP=%" PRIu32,
                          input_index, input->lds_pos, fs_interp_count);
      }

      if (unlikely(fs_interp_slot_used[input->lds_pos])) {
         return vk_errorf(device, VK_ERROR_VALIDATION_FAILED_EXT,
                          "FS interpolation slot collision: multiple inputs mapped to lds_pos=%u",
                          input->lds_pos);
      }
      fs_interp_slot_used[input->lds_pos] = true;

      uint32_t const spi_input_cntl = fs->static_state.stage.ps.spi_ps_input_cntl[input->lds_pos];
      uint32_t const sid = G_028644_SEMANTIC(spi_input_cntl);
      if (unlikely(sid != (uint32_t)input->spi_sid)) {
         return vk_errorf(device, VK_ERROR_VALIDATION_FAILED_EXT,
                          "FS SPI semantic mismatch at input %" PRIu32
                          ": shader spi_sid=%d, SPI_PS_INPUT_CNTL[%u].SEMANTIC=%" PRIu32,
                          input_index, input->spi_sid, input->lds_pos, sid);
      }

      int16_t const vs_param_index = vs_param_for_sid[sid];
      if (unlikely(vs_param_index < 0)) {
         return vk_errorf(device, VK_ERROR_VALIDATION_FAILED_EXT,
                          "FS input %" PRIu32 " requests SID=%" PRIu32
                          " but VS exports no matching semantic",
                          input_index, sid);
      }

      bool const expected_flat = input->interpolate == TGSI_INTERPOLATE_CONSTANT;
      bool const expected_noperspective = input->interpolate == TGSI_INTERPOLATE_LINEAR;
      bool const expected_perspective =
         input->interpolate == TGSI_INTERPOLATE_PERSPECTIVE ||
         input->interpolate == TGSI_INTERPOLATE_COLOR;

      bool const reg_flat = G_028644_FLAT_SHADE(spi_input_cntl) != 0;
      bool const reg_noperspective = G_028644_SEL_LINEAR(spi_input_cntl) != 0;
      bool const reg_centroid = G_028644_SEL_CENTROID(spi_input_cntl) != 0;
      bool const reg_sample = G_028644_SEL_SAMPLE(spi_input_cntl) != 0;

      if (unlikely(reg_centroid && reg_sample)) {
         return vk_errorf(device, VK_ERROR_VALIDATION_FAILED_EXT,
                          "FS interpolation ambiguity at input %" PRIu32
                          ": SPI_PS_INPUT_CNTL[%u] has both centroid and sample selectors",
                          input_index, input->lds_pos);
      }

      if (unlikely(reg_flat != expected_flat)) {
         return vk_errorf(device, VK_ERROR_VALIDATION_FAILED_EXT,
                          "FS interpolation mode mismatch at input %" PRIu32
                          ": expected %s but SPI_PS_INPUT_CNTL[%u].FLAT_SHADE=%u",
                          input_index,
                          terakan_pipeline_graphics_interp_mode_name(input->interpolate),
                          input->lds_pos, (unsigned)reg_flat);
      }

      if (unlikely(expected_flat && reg_noperspective)) {
         return vk_errorf(device, VK_ERROR_VALIDATION_FAILED_EXT,
                          "FS interpolation ambiguity at input %" PRIu32
                          ": flat input has SEL_LINEAR set in SPI_PS_INPUT_CNTL[%u]",
                          input_index, input->lds_pos);
      }

      if (unlikely(!expected_flat && expected_noperspective != reg_noperspective)) {
         return vk_errorf(device, VK_ERROR_VALIDATION_FAILED_EXT,
                          "FS interpolation mode mismatch at input %" PRIu32
                          ": expected %s but SPI_PS_INPUT_CNTL[%u].SEL_LINEAR=%u",
                          input_index,
                          terakan_pipeline_graphics_interp_mode_name(input->interpolate),
                          input->lds_pos, (unsigned)reg_noperspective);
      }

      bool expected_centroid;
      bool expected_sample;
      switch (input->interpolate_location) {
      case TGSI_INTERPOLATE_LOC_CENTER:
         expected_centroid = false;
         expected_sample = false;
         break;
      case TGSI_INTERPOLATE_LOC_CENTROID:
         expected_centroid = true;
         expected_sample = false;
         break;
      case TGSI_INTERPOLATE_LOC_SAMPLE:
         expected_centroid = false;
         expected_sample = true;
         break;
      default:
         return vk_errorf(device, VK_ERROR_VALIDATION_FAILED_EXT,
                          "FS interpolation location ambiguity at input %" PRIu32
                          ": unsupported interpolate_location=%u",
                          input_index, input->interpolate_location);
      }

      if (unlikely(reg_centroid != expected_centroid || reg_sample != expected_sample)) {
         return vk_errorf(device, VK_ERROR_VALIDATION_FAILED_EXT,
                          "FS interpolation location mismatch at input %" PRIu32
                          ": expected %s but SPI_PS_INPUT_CNTL[%u] has centroid=%u sample=%u",
                          input_index,
                          terakan_pipeline_graphics_interp_loc_name(input->interpolate_location),
                          input->lds_pos, (unsigned)reg_centroid, (unsigned)reg_sample);
      }

      if (unlikely(!expected_flat && !expected_noperspective && !expected_perspective)) {
         return vk_errorf(device, VK_ERROR_VALIDATION_FAILED_EXT,
                          "FS interpolation ambiguity at input %" PRIu32
                          ": unsupported interpolate mode=%u",
                          input_index, input->interpolate);
      }

      if (!expected_flat &&
          unlikely(!terakan_pipeline_graphics_spi_baryc_enabled(
             fs->static_state.stage.ps.spi_baryc_cntl, expected_noperspective,
             input->interpolate_location))) {
         return vk_errorf(device, VK_ERROR_VALIDATION_FAILED_EXT,
                          "FS barycentric mode mismatch at input %" PRIu32
                          ": mode=%s location=%s not enabled in SPI_BARYC_CNTL=0x%08" PRIx32,
                          input_index,
                          terakan_pipeline_graphics_interp_mode_name(input->interpolate),
                          terakan_pipeline_graphics_interp_loc_name(input->interpolate_location),
                          fs->static_state.stage.ps.spi_baryc_cntl);
      }

      ++fs_semantic_count;

      if (debug_pipeline) {
         fprintf(stderr,
                 "TERAKAN_SPI_ROUTE_MAP: vs_param=%d sid=%" PRIu32
                 " fs_input=%" PRIu32 " fs_lds=%u fs_gpr=%u interp=%s loc=%s"
                 " cntl=0x%08" PRIx32 "\n",
                 vs_param_index, sid, input_index, input->lds_pos, input->gpr,
                 terakan_pipeline_graphics_interp_mode_name(input->interpolate),
                 terakan_pipeline_graphics_interp_loc_name(input->interpolate_location),
                 spi_input_cntl);
      }
   }

   fs_expected_interp_count = MAX2(fs_expected_interp_count, 1u);
   if (unlikely(fs_interp_count != fs_expected_interp_count)) {
      return vk_errorf(device, VK_ERROR_VALIDATION_FAILED_EXT,
                       "SPI_PS_IN_CONTROL.NUM_INTERP mismatch: programmed=%" PRIu32
                       " expected=%" PRIu32,
                       fs_interp_count, fs_expected_interp_count);
   }

   for (uint32_t interp = 0; interp < fs_interp_count; ++interp) {
      uint32_t const sid = G_028644_SEMANTIC(fs->static_state.stage.ps.spi_ps_input_cntl[interp]);
      if (unlikely(fs_interp_slot_used[interp] && sid == 0)) {
         return vk_errorf(device, VK_ERROR_VALIDATION_FAILED_EXT,
                          "FS interpolation slot %" PRIu32
                          " is used by shader input but SPI semantic is 0",
                          interp);
      }
      if (unlikely(!fs_interp_slot_used[interp] && sid != 0)) {
         return vk_errorf(device, VK_ERROR_VALIDATION_FAILED_EXT,
                          "FS interpolation slot %" PRIu32
                          " is programmed with SID=%" PRIu32
                          " but no FS input maps to that lds_pos",
                          interp, sid);
      }
   }

   bool const position_expected = position_input != NULL;
   bool const position_enabled =
      G_0286CC_POSITION_ENA(fs->static_state.stage.ps.spi_ps_in_control[0]) != 0;
   if (unlikely(position_expected != position_enabled)) {
      return vk_errorf(device, VK_ERROR_VALIDATION_FAILED_EXT,
                       "FS position mapping mismatch: expected POSITION_ENA=%u got %u",
                       (unsigned)position_expected, (unsigned)position_enabled);
   }

   if (position_expected) {
      uint32_t const position_addr =
         G_0286CC_POSITION_ADDR(fs->static_state.stage.ps.spi_ps_in_control[0]);
      if (unlikely(position_addr != position_input->gpr)) {
         return vk_errorf(device, VK_ERROR_VALIDATION_FAILED_EXT,
                          "FS position GPR mismatch: SPI_PS_IN_CONTROL_0.POSITION_ADDR=%" PRIu32
                          " expected=%u",
                          position_addr, position_input->gpr);
      }

      bool const position_centroid =
         G_0286CC_POSITION_CENTROID(fs->static_state.stage.ps.spi_ps_in_control[0]) != 0;
      bool const position_sample =
         G_0286CC_POSITION_SAMPLE(fs->static_state.stage.ps.spi_ps_in_control[0]) != 0;
      bool const expected_position_centroid =
         position_input->interpolate_location == TGSI_INTERPOLATE_LOC_CENTROID;
      bool const expected_position_sample =
         position_input->interpolate_location == TGSI_INTERPOLATE_LOC_SAMPLE;
      if (unlikely(position_centroid != expected_position_centroid ||
                   position_sample != expected_position_sample)) {
         return vk_errorf(device, VK_ERROR_VALIDATION_FAILED_EXT,
                          "FS position interpolation mismatch: expected loc=%s but"
                          " POSITION_CENTROID=%u POSITION_SAMPLE=%u",
                          terakan_pipeline_graphics_interp_loc_name(
                             position_input->interpolate_location),
                          (unsigned)position_centroid, (unsigned)position_sample);
      }
   }

   bool const provide_z_to_spi =
      (fs->static_state.stage.ps.spi_input_z & S_0286D8_PROVIDE_Z_TO_SPI(1)) != 0;
   if (unlikely(provide_z_to_spi != position_expected)) {
      return vk_errorf(device, VK_ERROR_VALIDATION_FAILED_EXT,
                       "SPI_INPUT_Z.PROVIDE_Z_TO_SPI mismatch: expected=%u got=%u",
                       (unsigned)position_expected, (unsigned)provide_z_to_spi);
   }

   bool const front_face_expected = face_or_sample_mask_gpr != UINT32_MAX;
   bool const front_face_enabled =
      G_0286D0_FRONT_FACE_ENA(fs->static_state.stage.ps.spi_ps_in_control[1]) != 0;
   if (unlikely(front_face_expected != front_face_enabled)) {
      return vk_errorf(device, VK_ERROR_VALIDATION_FAILED_EXT,
                       "FS front-face mapping mismatch: expected FRONT_FACE_ENA=%u got=%u",
                       (unsigned)front_face_expected, (unsigned)front_face_enabled);
   }
   if (front_face_expected) {
      uint32_t const front_face_addr =
         G_0286D0_FRONT_FACE_ADDR(fs->static_state.stage.ps.spi_ps_in_control[1]);
      if (unlikely(front_face_addr != face_or_sample_mask_gpr)) {
         return vk_errorf(device, VK_ERROR_VALIDATION_FAILED_EXT,
                          "FS front-face/sample-mask GPR mismatch: SPI_PS_IN_CONTROL_1"
                          ".FRONT_FACE_ADDR=%" PRIu32 " expected=%" PRIu32,
                          front_face_addr, face_or_sample_mask_gpr);
      }
   }

   bool const sample_id_expected = sample_id_gpr != UINT32_MAX;
   bool const sample_id_enabled =
      G_0286D0_FIXED_PT_POSITION_ENA(fs->static_state.stage.ps.spi_ps_in_control[1]) != 0;
   if (unlikely(sample_id_expected != sample_id_enabled)) {
      return vk_errorf(device, VK_ERROR_VALIDATION_FAILED_EXT,
                       "FS sample-id mapping mismatch: expected FIXED_PT_POSITION_ENA=%u got=%u",
                       (unsigned)sample_id_expected, (unsigned)sample_id_enabled);
   }
   if (sample_id_expected) {
      uint32_t const sample_id_addr =
         G_0286D0_FIXED_PT_POSITION_ADDR(fs->static_state.stage.ps.spi_ps_in_control[1]);
      if (unlikely(sample_id_addr != sample_id_gpr)) {
         return vk_errorf(device, VK_ERROR_VALIDATION_FAILED_EXT,
                          "FS sample-id GPR mismatch: SPI_PS_IN_CONTROL_1"
                          ".FIXED_PT_POSITION_ADDR=%" PRIu32 " expected=%" PRIu32,
                          sample_id_addr, sample_id_gpr);
      }
   }

   if (debug_pipeline) {
      int32_t const position_gpr = position_input ? (int32_t)position_input->gpr : -1;
      int32_t const front_face_gpr =
         face_or_sample_mask_gpr != UINT32_MAX ? (int32_t)face_or_sample_mask_gpr : -1;
      int32_t const sample_id_gpr_log = sample_id_gpr != UINT32_MAX ? (int32_t)sample_id_gpr : -1;
      fprintf(stderr,
              "TERAKAN_SPI_ROUTE: vs_export_count=%" PRIu32 " vs_vectors=%" PRIu32
              " vs_semantics=%" PRIu32 " fs_num_interp=%" PRIu32
              " fs_semantics=%" PRIu32 "\n",
              vs_export_count, vs_export_count + 1, vs_semantic_count,
              fs_interp_count, fs_semantic_count);
      fprintf(stderr,
              "TERAKAN_SPI_GPR_MAP: position_gpr=%" PRId32
              " front_face_gpr=%" PRId32 " sample_id_gpr=%" PRId32
              " in_control0=0x%08" PRIx32 " in_control1=0x%08" PRIx32
              " input_z=0x%08" PRIx32 " baryc=0x%08" PRIx32 "\n",
              position_gpr, front_face_gpr, sample_id_gpr_log,
              fs->static_state.stage.ps.spi_ps_in_control[0],
              fs->static_state.stage.ps.spi_ps_in_control[1],
              fs->static_state.stage.ps.spi_input_z,
              fs->static_state.stage.ps.spi_baryc_cntl);
   }

   return VK_SUCCESS;
}

static VkResult
terakan_pipeline_graphics_validate_fs_color_export_parity(
   struct terakan_device * const device,
   struct terakan_shader_impl const * const fs,
   struct terakan_graphics_state_key const * const gfx_state_key)
{
   assert(fs != NULL);
   assert(gfx_state_key != NULL);

   uint8_t const fs_uncompacted_locations = fs->fs.fragment_data_uncompacted_locations;
   uint32_t const expected_export_width = util_bitcount(fs_uncompacted_locations);
   uint32_t const cb_shader_mask = fs->static_state.stage.ps.cb_shader_mask;

   uint32_t exported_rtv_count = 0;
   uint32_t exported_channel_count = 0;
   bool seen_zero_after_exports = false;

   for (uint32_t rtv = 0; rtv < TERAKAN_COLOR_HW_RTV_COUNT; ++rtv) {
      uint32_t const nibble = (cb_shader_mask >> (rtv * 4u)) & 0xFu;
      if (nibble == 0) {
         if (exported_rtv_count != 0)
            seen_zero_after_exports = true;
         continue;
      }

      if (unlikely(seen_zero_after_exports)) {
         return vk_errorf(device, VK_ERROR_VALIDATION_FAILED_EXT,
                          "FS color export width mismatch: cb_shader_mask has a hole before RTV %"
                          PRIu32 " (cb_shader_mask=0x%08" PRIx32 ")",
                          rtv, cb_shader_mask);
      }

      ++exported_rtv_count;
      exported_channel_count += util_bitcount(nibble);
   }

   if (unlikely(exported_rtv_count != expected_export_width)) {
      return vk_errorf(device, VK_ERROR_VALIDATION_FAILED_EXT,
                       "FS color export width mismatch: compacted exports=%" PRIu32
                       " (cb_shader_mask=0x%08" PRIx32 ") but uncompacted locations=%"
                       PRIu32 " (mask=0x%02x)",
                       exported_rtv_count, cb_shader_mask, expected_export_width,
                       fs_uncompacted_locations);
   }

   if (unlikely(expected_export_width == 0 && cb_shader_mask != 0)) {
      return vk_errorf(device, VK_ERROR_VALIDATION_FAILED_EXT,
                       "FS color export channel mismatch: no fragment outputs expected, but"
                       " cb_shader_mask=0x%08" PRIx32,
                       cb_shader_mask);
   }

   if (unlikely(expected_export_width != 0 && exported_channel_count == 0)) {
      return vk_errorf(device, VK_ERROR_VALIDATION_FAILED_EXT,
                       "FS color export channel mismatch: %" PRIu32
                       " RTV exports present but cb_shader_mask has no channels",
                       expected_export_width);
   }

   if (gfx_state_key->dual_source_blend) {
      bool const has_rtv0 = (cb_shader_mask & 0xFu) != 0;
      bool const has_rtv1 = (cb_shader_mask & 0xF0u) != 0;
      bool const dual_export_possible =
         G_02880C_DUAL_EXPORT_ENABLE(fs->fs.db_shader_control) != 0;
      if (unlikely(!has_rtv0 || !has_rtv1 || expected_export_width < 2u ||
                   !dual_export_possible)) {
         return vk_errorf(device, VK_ERROR_VALIDATION_FAILED_EXT,
                          "FS dual-source export mismatch: dual-source blending requires"
                          " compacted RTV0+RTV1 exports and DUAL_EXPORT_ENABLE"
                          " (cb_shader_mask=0x%08" PRIx32
                          ", expected_exports=%" PRIu32
                          ", db_shader_control=0x%08" PRIx32 ")",
                          cb_shader_mask, expected_export_width,
                          fs->fs.db_shader_control);
      }
   }

   return VK_SUCCESS;
}


/*
 * Phase B: Per-stage SPIR-V → NIR → post-link lower → cache lookup → compile.
 *
 * Invariant 1 (Dual stage mask): declared_stages is precomputed from
 * create_info and is loop-invariant.  pipeline->shader_stages accumulates
 * only on successful commit and is the cleanup mask used by _destroy().
 *
 * Invariant 2 (Ownership isolation): Each iteration compiles into a
 * stack-local terakan_shader_impl.  On success, the local is committed to
 * pipeline->shaders[stage_index] via struct copy and
 * pipeline->shader_stages is updated in the same tick.  On failure, the
 * local is finished() in-place and the pipeline slot is left untouched
 * (still zeroed from vk_zalloc2 in _init()).  The orchestrator's _destroy()
 * path only ever sees fully committed, validated state.  There is no
 * window in which pipeline->shaders[stage_index] contains half-constructed
 * allocations, so no "defensive NULL" of internal pointers is required.
 *
 * Invariant 3 (Cache hit ≡ miss): Pre-compile fields (resources_needed,
 * samplers_needed, kcache_needed, push_constants_usage, fragment_data,
 * uavs_for_mutable) are populated by post-link lowering BEFORE cache
 * lookup, so they exist identically on both paths.
 *
 * Invariant 4 (Post-link ordering barrier): Cache key is computed only
 * AFTER terakan_shader_lower_and_optimize_post_link() completes.
 *
 * Invariant 5 (Cache key per-stage purity): Although declared_stages is a
 * cross-pipeline bitmask, it flows into gfx_state_key and then through
 * terakan_r600_shader_key_from_state() which projects down to the
 * per-stage r600_shader_key by dropping all fields not relevant to the
 * current stage (see terakan_pipeline_key.c).  The cache key hash
 * (terakan_pipeline_cache_hash_shader) ingests only the projected
 * shader_key plus the per-stage stage_key plus the per-stage SPIR-V hash.
 * It does NOT ingest gfx_state_key directly.  Consequently, two pipelines
 * with the same VS SPIR-V but different FS configurations produce the
 * same VS cache key; two pipelines where one adds a GS produce different
 * VS cache keys (because vs_as_es flips, which is a legitimate
 * recompilation trigger).  This preserves per-stage caching while
 * honoring cross-stage linkage requirements.  Do NOT hash gfx_state_key
 * directly into the cache key — doing so would destroy per-stage
 * hit rates across pipelines that reuse stages.
 */
static VkResult
terakan_pipeline_graphics_compile_shaders(
   struct terakan_pipeline_graphics * const pipeline,
   struct terakan_device * const device,
   VkGraphicsPipelineCreateInfo const * const create_info,
   struct vk_pipeline_cache * const cache,
   VkAllocationCallbacks const * const allocator)
{
   VkResult result;

   struct terakan_pipeline_layout const * const pipeline_layout =
      terakan_pipeline_layout_from_handle(create_info->layout);

   /* --- Loop-invariant cross-stage state (computed once, Invariant 5) ------
    * declared_stages, pipeline_flags, and the cross-stage portion of
    * gfx_state_key (everything except ps_nr_cbufs, which depends on the
    * current fragment shader's output count) are loop-invariant. */
   VkShaderStageFlags declared_stages = 0;
   for (uint32_t i = 0; i < create_info->stageCount; ++i)
      declared_stages |= create_info->pStages[i].stage;

   VkPipelineCreateFlags2KHR const pipeline_flags =
      terakan_pipeline_create_flags(create_info->flags, create_info->pNext);

   /* Per-stage context for the 4-pass Multi-Pass Post-Link Barrier.
    *
    * Unlike the original single-loop design, the barrier requires that
    * FS is lowered first (to extract inputs_read), then VS is lowered
    * with FS feedback (to prune dead outputs).  The application's
    * pStages array order is irrelevant — the driver controls the
    * compilation sequence.
    *
    * Pass 1 (Discovery):  SPIR-V → NIR for all stages.  No compilation.
    * Pass 2 (Fragment):   Post-link lower FS, extract inputs_read.
    * Pass 3 (Vertex+):    Post-link lower VS (with FS feedback) + others.
    * Pass 4 (Compile):    Cache key → lookup → compile → commit.
    */
   struct {
      bool present;
      nir_shader *nir;
      struct terakan_shader_impl local_shader;
      bool robust_buffer_access;
      VkPipelineShaderStageCreateInfo const *stage_info;
   } stages[MESA_SHADER_STAGES];
   memset(stages, 0, sizeof(stages));

   /* =================================================================
    * PASS 1 — Discovery: Convert all SPIR-V to NIR.  Determine per-stage
    * robustness.  Do NOT compile or lower.
    * ================================================================= */
   for (uint32_t i = 0; i < create_info->stageCount; ++i) {
      VkPipelineShaderStageCreateInfo const * const stage_info =
         &create_info->pStages[i];

      mesa_shader_stage const si = vk_to_mesa_shader_stage(stage_info->stage);

      size_t spirv_size_bytes;
      uint32_t const *spirv =
         terakan_pipeline_stage_spirv(stage_info, &spirv_size_bytes);
      assert(spirv != NULL);

      nir_shader *nir =
         terakan_shader_spirv_to_nir(device, spirv_size_bytes, spirv, si,
                                     stage_info->pName,
                                     stage_info->pSpecializationInfo);
      if (nir == NULL) {
         result = vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);
         goto cleanup;
      }

      stages[si].present = true;
      stages[si].nir = nir;
      stages[si].stage_info = stage_info;

      /* Compute effective robustness for this stage.  Honour
       * VK_EXT_pipeline_robustness per-pipeline and per-stage overrides;
       * otherwise fall back to device-level robustBufferAccess. */
      struct vk_pipeline_robustness_state stage_rs;
      vk_pipeline_robustness_state_fill(&device->vk.robustness_state, &stage_rs,
                                        create_info->pNext, stage_info->pNext);
      stages[si].robust_buffer_access =
         device->vk.enabled_features.robustBufferAccess ||
         stage_rs.storage_buffers ==
            VK_PIPELINE_ROBUSTNESS_BUFFER_BEHAVIOR_ROBUST_BUFFER_ACCESS_EXT ||
         stage_rs.storage_buffers ==
            VK_PIPELINE_ROBUSTNESS_BUFFER_BEHAVIOR_ROBUST_BUFFER_ACCESS_2_EXT ||
         stage_rs.uniform_buffers ==
            VK_PIPELINE_ROBUSTNESS_BUFFER_BEHAVIOR_ROBUST_BUFFER_ACCESS_EXT ||
         stage_rs.uniform_buffers ==
            VK_PIPELINE_ROBUSTNESS_BUFFER_BEHAVIOR_ROBUST_BUFFER_ACCESS_2_EXT;

      /* Invariant 2: compile into an isolated stack-local.  The pipeline's
       * stage slot is never touched until we commit on success in Pass 4. */
      memset(&stages[si].local_shader, 0, sizeof(stages[si].local_shader));
      stages[si].local_shader.push_constants_usage.app_extent_bytes =
         pipeline_layout->shader_app_push_constants_extents_bytes[si];
   }

   /* =================================================================
    * PASS 2 — Fragment Pass: Post-link lower the FS, run DCE, extract
    * inputs_read for cross-stage feedback to the VS.
    *
    * This is the foundation of the Multi-Pass Post-Link Barrier.
    * After post-link DCE, inputs_read reflects the TRUE set of
    * FS-consumed varyings (DCE-dead varyings excluded), which the VS
    * can use to prune dead outputs in Pass 3.
    * ================================================================= */
   uint64_t fs_inputs_read = ~0ULL; /* Conservative: keep all VS outputs */

   if (stages[MESA_SHADER_FRAGMENT].present) {
      struct terakan_shader_impl *fs_local =
         &stages[MESA_SHADER_FRAGMENT].local_shader;
      nir_shader *fs_nir = stages[MESA_SHADER_FRAGMENT].nir;

      /* Post-link lowering populates pre-compile metadata (Invariant 3).
       * This runs BEFORE cache key construction (Invariant 4).
       * strict_reject_fs is set inside before lower_bindings removes the
       * ssbo_atomic_swap intrinsics.
       * image_atomic_reject_fs is set for any image atomic on CHIP_PALM;
       * those wedge the CP ring and are rejected at pipeline-creation time. */
      bool strict_reject_fs = false;
      bool image_atomic_reject_fs = false;
      terakan_shader_lower_and_optimize_post_link(
         fs_nir, pipeline_layout, fs_local->resources_needed,
         &fs_local->samplers_needed,
         fs_local->uavs_for_mutable_resources_needed,
         &fs_local->push_constants_usage.driver_constants,
         &fs_local->kcache_needed,
         &fs_local->fs.fragment_data_uncompacted_locations,
         stages[MESA_SHADER_FRAGMENT].robust_buffer_access,
         terakan_device_physical_device(device)->chip_info.chip_family,
         &strict_reject_fs,
         &image_atomic_reject_fs);

      if (strict_reject_fs || image_atomic_reject_fs) {
         result = VK_ERROR_FEATURE_NOT_PRESENT;
         goto cleanup;
      }

      fs_inputs_read = fs_nir->info.inputs_read;
   }

   /* Compute the graphics state key ONCE — used by both Pass 3 (postprocess
    * decisions) and Pass 4 (cache key projection).  All inputs are from
    * immutable create_info or from FS post-link results (ps_nr_cbufs).
    * Computing once guarantees byte-identical keys across passes. */
   uint8_t ps_nr_cbufs = 0;
   if (stages[MESA_SHADER_FRAGMENT].present) {
      ps_nr_cbufs = util_bitcount(
         stages[MESA_SHADER_FRAGMENT].local_shader.fs
            .fragment_data_uncompacted_locations);
   }

   struct terakan_graphics_state_key gfx_state_key;
   terakan_graphics_state_key_fill(&gfx_state_key, create_info,
                                   declared_stages, ps_nr_cbufs);

   /* =================================================================
    * PASS 3 — Vertex + Others: Post-link lower all non-FS stages.
    * For VS: apply terakan_postprocess_nir with FS feedback (varying
    * pruning, point size removal).  This is the core cross-stage
    * optimization enabled by the Multi-Pass Barrier.
    * ================================================================= */
   {
      for (mesa_shader_stage si = 0; si < MESA_SHADER_STAGES; si++) {
         if (!stages[si].present || si == MESA_SHADER_FRAGMENT)
            continue; /* FS already lowered in Pass 2 */

         struct terakan_shader_impl *local = &stages[si].local_shader;
         nir_shader *nir = stages[si].nir;

         /* Post-link lowering populates pre-compile metadata (Invariant 3). */
         bool strict_reject_stage = false;
         bool image_atomic_reject_stage = false;
         terakan_shader_lower_and_optimize_post_link(
            nir, pipeline_layout, local->resources_needed,
            &local->samplers_needed,
            local->uavs_for_mutable_resources_needed,
            &local->push_constants_usage.driver_constants,
            &local->kcache_needed,
            NULL, /* Not FS: no fragment data locations */
            stages[si].robust_buffer_access,
            terakan_device_physical_device(device)->chip_info.chip_family,
            &strict_reject_stage,
            &image_atomic_reject_stage);

         if (strict_reject_stage || image_atomic_reject_stage) {
            result = VK_ERROR_FEATURE_NOT_PRESENT;
            goto cleanup;
         }

         /* Cross-stage post-process: varying pruning + point size removal.
          *
          * For VS: if VS feeds GS/TES, enable_remove_point_size is already
          * 0 (set in gfx_state_key_fill) and fs_inputs_read is conservative
          * (~0ULL when not the last vertex stage).
          *
          * When VS IS the last vertex stage, fs_inputs_read from Pass 2
          * drives varying pruning, and enable_remove_point_size from the
          * static topology check drives point size removal.
          *
          * Must run AFTER post-link (for valid lowered NIR) and BEFORE
          * cache key construction (so the hash reflects the pruned program).
          */
         uint64_t effective_fs_inputs =
            (si == MESA_SHADER_VERTEX &&
             !gfx_state_key.vs_as_es && !gfx_state_key.vs_as_ls)
               ? fs_inputs_read : ~0ULL;

         terakan_postprocess_nir(nir, si,
                                 gfx_state_key.enable_remove_point_size,
                                 effective_fs_inputs);
      }
   }

   /* =================================================================
    * PASS 4 — Compilation: Build cache keys, lookup or compile, commit.
    *
    * For VS, the cache key includes cross-stage postprocess context
    * (fs_inputs_read + remove_point_size) to prevent aliasing between
    * pruned and unpruned shader variants.  This avoids mutating the
    * shared r600_shader_key union.
    * ================================================================= */
   pipeline->shader_stages = 0;

   for (mesa_shader_stage si = 0; si < MESA_SHADER_STAGES; si++) {
      if (!stages[si].present)
         continue;

      struct terakan_shader_impl *local = &stages[si].local_shader;
      nir_shader *nir = stages[si].nir;

      /* --- Cache key construction (Invariant 5: per-stage purity) ------
       * stage_key: per-stage compilation flags (opt, statistics, robustness)
       * gfx_state_key: computed once before Pass 3, projected to shader_key
       * shader_key: per-stage r600 projection, stage-irrelevant fields zero
       *
       * The cache hash ingests stage_key + shader_key + spirv_hash +
       * optional postprocess_ctx.  gfx_state_key itself is never hashed
       * directly.  See Invariant 5 above. */
      struct terakan_shader_stage_key stage_key;
      terakan_shader_stage_key_fill(&stage_key, device,
                                    stages[si].stage_info, pipeline_flags);

      union r600_shader_key shader_key;
      terakan_r600_shader_key_from_state(&shader_key, &gfx_state_key, si);

      blake3_hash spirv_hash;
      struct vk_pipeline_robustness_state rs;
      vk_pipeline_robustness_state_fill(&device->vk.robustness_state, &rs,
                                        create_info->pNext,
                                        stages[si].stage_info->pNext);
      vk_pipeline_hash_shader_stage(pipeline_flags,
                                    stages[si].stage_info, &rs, spirv_hash);

      /* For VS, include cross-stage postprocess decisions in the cache key
       * to prevent aliasing between pruned and unpruned variants.
       * Zero-initialized struct ensures deterministic BLAKE3 input. */
      struct {
         uint64_t fs_inputs_read;
         uint32_t remove_point_size;
         uint32_t pad0;
      } vs_postprocess_ctx;

      void const *postprocess_ctx = NULL;
      size_t postprocess_ctx_size = 0;

      if (si == MESA_SHADER_VERTEX) {
         memset(&vs_postprocess_ctx, 0, sizeof(vs_postprocess_ctx));
         vs_postprocess_ctx.fs_inputs_read =
            (!gfx_state_key.vs_as_es && !gfx_state_key.vs_as_ls)
               ? fs_inputs_read : ~0ULL;
         vs_postprocess_ctx.remove_point_size =
            gfx_state_key.enable_remove_point_size;
         postprocess_ctx = &vs_postprocess_ctx;
         postprocess_ctx_size = sizeof(vs_postprocess_ctx);
      }

      blake3_hash cache_key;
      terakan_pipeline_cache_hash_shader(cache_key, device, &stage_key,
                                         &shader_key, spirv_hash,
                                         postprocess_ctx,
                                         postprocess_ctx_size);

      /* Cache lookup — skip compilation on hit (Invariant 3). */
      struct terakan_cached_shader *cached =
         terakan_pipeline_cache_lookup(cache, cache_key);
      if (cached != NULL) {
         result = terakan_cached_shader_restore(cached, local, device,
                                                allocator);
         vk_pipeline_cache_object_unref(&device->vk, &cached->base);
         ralloc_free(nir);
         stages[si].nir = NULL;
         if (result != VK_SUCCESS)
            goto cleanup;
      } else {
         /* Cache miss — compilation required.  If the app set
          * FAIL_ON_PIPELINE_COMPILE_REQUIRED, bail out immediately so the
          * app can schedule its own background compile and retry later.
          * VK_PIPELINE_COMPILE_REQUIRED is a non-error success code (>0)
          * but counts as non-VK_SUCCESS for EARLY_RETURN_ON_FAILURE. */
         if (pipeline_flags &
             VK_PIPELINE_CREATE_2_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT_KHR) {
            result = VK_PIPELINE_COMPILE_REQUIRED;
            goto cleanup;
         }

         result = terakan_shader_impl_compile(local, device, &shader_key,
                                              nir, allocator);
         size_t const program_size_bytes =
            sizeof(uint32_t) * local->shader.bc.ndw;
         ralloc_free(nir);
         stages[si].nir = NULL;
         if (result != VK_SUCCESS)
            goto cleanup;

         terakan_pipeline_cache_insert(cache, cache_key, local, si,
                                       program_size_bytes, device);
      }

      /* Invariant 2 commit: transfer ownership of all locally allocated
       * resources (BO, shader.arrays, etc.) to the pipeline via struct copy.
       * After this assignment, local_shader is "moved-from"; do NOT call
       * terakan_shader_impl_finish() on it. */
      pipeline->shaders[si] = *local;
      pipeline->shader_stages |= mesa_to_vk_shader_stage(si);
   }

   if ((pipeline->shader_stages & (VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT)) ==
       (VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT)) {
      result = terakan_pipeline_graphics_validate_vs_fs_spi_routing(
         device, &pipeline->shaders[MESA_SHADER_VERTEX],
         &pipeline->shaders[MESA_SHADER_FRAGMENT]);
      if (result != VK_SUCCESS)
         goto cleanup;
   }

   if (pipeline->shader_stages & VK_SHADER_STAGE_FRAGMENT_BIT) {
      result = terakan_pipeline_graphics_validate_fs_color_export_parity(
         device, &pipeline->shaders[MESA_SHADER_FRAGMENT], &gfx_state_key);
      if (result != VK_SUCCESS)
         goto cleanup;
   }

   /* Merge per-stage kcache_needed masks into a single pipeline-wide mask.
    * Draw-time KCACHE bank 14 binding checks this instead of always binding.
    * ISA basis: bank 14 is NOT dynamically indexed (Evergreen ISA §4.6.4,
    * banks >= 14 ignore BANK_INDEX_MODE), so a single binding serves all
    * stages reading from the same robustness metadata buffer. */
   pipeline->kcache_needed_merged = 0;
   u_foreach_bit(s, pipeline->shader_stages) {
      mesa_shader_stage const stage =
         vk_to_mesa_shader_stage((VkShaderStageFlagBits)(1u << s));
      if (stage <= MESA_SHADER_FRAGMENT)
         pipeline->kcache_needed_merged |=
            pipeline->shaders[stage].kcache_needed;
   }

   /* Vertex shader is mandatory if the pre-rasterization part is present.
    * TODO(Triang3l): Skip for graphics pipeline libraries without
    * pre-rasterization. */
   assert(declared_stages & VK_SHADER_STAGE_VERTEX_BIT);
   if (unlikely(!(pipeline->shader_stages & VK_SHADER_STAGE_VERTEX_BIT))) {
      return vk_errorf(device, VK_ERROR_VALIDATION_FAILED_EXT,
                       "No vertex shader in the graphics pipeline");
   }

   return VK_SUCCESS;

cleanup:
   /* Free remaining NIR allocations (ralloc_free(NULL) is safe).
    * For stages committed to pipeline->shaders, the orchestrator's
    * _destroy() handles cleanup via pipeline->shader_stages.
    * For stages present but not committed, finish() cleans up any
    * partial allocations (BO, arrays) from failed compile/restore.
    * For stages with only post-link metadata (no heap resources),
    * finish() is a safe no-op. */
   for (int i = 0; i < MESA_SHADER_STAGES; i++) {
      ralloc_free(stages[i].nir);
      if (stages[i].present &&
          !(pipeline->shader_stages &
            mesa_to_vk_shader_stage((mesa_shader_stage)i))) {
         terakan_shader_impl_finish(&stages[i].local_shader, allocator);
      }
   }
   return result;
}

/*
 * Phase C: Parse VkGraphicsPipelineCreateInfo into hardware state registers.
 *
 * Invariant 5: Runs AFTER compile_shaders — depends on compiled VS data
 * (vertex_attributes_needed) and FS data (fragment_data_locations).
 * BITSET_ZERO is inside this function for self-containment (RD-5).
 */
static VkResult
terakan_pipeline_graphics_fill_state(
   struct terakan_pipeline_graphics * const pipeline,
   struct terakan_device * const device,
   VkGraphicsPipelineCreateInfo const * const create_info,
   VkAllocationCallbacks const * const allocator)
{
   BITSET_ZERO(pipeline->static_state);

   struct vk_graphics_pipeline_all_state all_state;
   struct vk_graphics_pipeline_state state = {};
   VkResult result = vk_graphics_pipeline_state_fill(
      &device->vk, &state, create_info, NULL, NULL, 0, &all_state,
      NULL, VK_SYSTEM_ALLOCATION_SCOPE_OBJECT, NULL);
   if (result != VK_SUCCESS)
      return result;

   result = terakan_pipeline_graphics_vertex_input_init(
      pipeline, &state, device,
      pipeline->shader_stages & VK_SHADER_STAGE_VERTEX_BIT
         ? pipeline->shaders[MESA_SHADER_VERTEX].vs.vertex_attributes_needed
         : NULL,
      allocator);
   if (result != VK_SUCCESS)
      return result;

   terakan_pipeline_graphics_pre_rasterization_init(
      pipeline, &state, device->vk.enabled_extensions.EXT_depth_range_unrestricted);
   terakan_pipeline_graphics_multisample_init(pipeline, &state);
   terakan_pipeline_graphics_fragment_shader_state_init(pipeline, &state);
   terakan_pipeline_graphics_fragment_output_init(pipeline, &state);

   return VK_SUCCESS;
}

/*
 * Orchestrator — thin caller with single destroy error path (Invariant 2).
 * All sub-functions are static (Invariant 6).
 */
static VkResult
terakan_pipeline_graphics_create(struct terakan_device * const device,
                                 VkGraphicsPipelineCreateInfo const * const create_info,
                                 struct vk_pipeline_cache * const cache,
                                 VkAllocationCallbacks const * const allocator,
                                 struct terakan_pipeline_graphics ** const pipeline_out)
{
   struct terakan_pipeline_graphics *pipeline;
   VkResult result;

   result = terakan_pipeline_graphics_init(device, allocator, &pipeline);
   if (result != VK_SUCCESS)
      return result;

   result = terakan_pipeline_graphics_compile_shaders(
      pipeline, device, create_info, cache, allocator);
   if (result != VK_SUCCESS)
      goto fail;

   result = terakan_pipeline_graphics_fill_state(
      pipeline, device, create_info, allocator);
   if (result != VK_SUCCESS)
      goto fail;

   *pipeline_out = pipeline;
   return VK_SUCCESS;

fail:
   terakan_pipeline_graphics_destroy(pipeline, allocator);
   return result;
}


VKAPI_ATTR VkResult VKAPI_CALL
terakan_CreateGraphicsPipelines(VkDevice const deviceHandle, VkPipelineCache const pipelineCache,
                                uint32_t const createInfoCount,
                                VkGraphicsPipelineCreateInfo const * const pCreateInfos,
                                VkAllocationCallbacks const * const pAllocator,
                                VkPipeline * const pPipelines)
{
   VkResult result = VK_SUCCESS;

   struct terakan_device * const device = terakan_device_from_handle(deviceHandle);

   uint32_t pipeline_index;

   for (pipeline_index = 0; pipeline_index < createInfoCount; ++pipeline_index) {
      struct terakan_pipeline_graphics * pipeline = NULL;
      VkGraphicsPipelineCreateInfo const * const create_info = &pCreateInfos[pipeline_index];
      struct vk_pipeline_cache *cache =
         pipelineCache != VK_NULL_HANDLE
            ? vk_pipeline_cache_from_handle(pipelineCache) : device->vk.mem_cache;
      VkResult const pipeline_result =
         terakan_pipeline_graphics_create(device, create_info, cache, pAllocator, &pipeline);
      if (pipeline_result != VK_SUCCESS) {
         result = pipeline_result;
         if (terakan_pipeline_create_flags(create_info->flags, create_info->pNext) &
             VK_PIPELINE_CREATE_2_EARLY_RETURN_ON_FAILURE_BIT_KHR) {
            break;
         }
         pPipelines[pipeline_index] = VK_NULL_HANDLE;
         continue;
      }
      pPipelines[pipeline_index] = terakan_pipeline_to_handle(&pipeline->base);
   }

   for (; pipeline_index < createInfoCount; ++pipeline_index) {
      pPipelines[pipeline_index] = VK_NULL_HANDLE;
   }

   return result;
}
