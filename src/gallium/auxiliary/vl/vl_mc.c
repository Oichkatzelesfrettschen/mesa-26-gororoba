/**************************************************************************
 *
 * Copyright 2009 Younes Manton.
 * All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sub license, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT.
 * IN NO EVENT SHALL VMWARE AND/OR ITS SUPPLIERS BE LIABLE FOR
 * ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 **************************************************************************/

#include <assert.h>

#include "pipe/p_context.h"

#include "util/u_sampler.h"
#include "util/u_draw.h"

#include "compiler/nir/nir_builder.h"

#include "vl_defines.h"
#include "vl_vertex_buffers.h"
#include "vl_mc.h"
#include "vl_idct.h"
#include "vl_nir.h"

enum VS_OUTPUT
{
   VS_O_VPOS = 0,
   VS_O_VTOP = 0,
   VS_O_VBOTTOM,

   VS_O_FLAGS = VS_O_VTOP,
   VS_O_VTEX = VS_O_VBOTTOM
};

/* The motion-comp vertex shaders share this: build the macroblock-relative
 * clip position and emit it, returning the xy for the texcoord math.  block_scale
 * maps the macroblock grid to destination texels. */
static nir_def *
calc_position(struct vl_mc *r, nir_builder *b, nir_def *block_scale)
{
   nir_variable *iv_rect = nir_get_variable_with_location(b->shader,
      nir_var_shader_in, VERT_ATTRIB_GENERIC0 + VS_I_RECT, glsl_vec4_type());
   nir_variable *iv_pos = nir_get_variable_with_location(b->shader,
      nir_var_shader_in, VERT_ATTRIB_GENERIC0 + VS_I_VPOS, glsl_vec4_type());
   nir_def *vrect = nir_trim_vector(b, nir_load_var(b, iv_rect), 2);
   nir_def *vpos = nir_trim_vector(b, nir_load_var(b, iv_pos), 2);

   /* t_vpos = (vpos + vrect) * block_scale; o_vpos.xy = t_vpos, zw = 1. */
   nir_def *t_vpos = nir_fmul(b, nir_fadd(b, vpos, vrect), block_scale);

   nir_variable *o_vpos = nir_get_variable_with_location(b->shader,
      nir_var_shader_out, VARYING_SLOT_POS, glsl_vec4_type());
   nir_store_var(b, o_vpos,
      nir_vec4(b, nir_channel(b, t_vpos, 0), nir_channel(b, t_vpos, 1),
               nir_imm_float(b, 1.0f), nir_imm_float(b, 1.0f)), 0xf);

   return t_vpos;
}

/* Field-parity selector for the interlaced reference fetch: in mesa-26 the
 * fragment position is always a sysval, so this is the unconditional branch the
 * removed fragment-position-is-sysval capability query used to guard.  Returns the
 * scalar frac(pos.y / 2) >= 0.5 ? 1.0 : 0.0. */
static nir_def *
calc_line(nir_builder *b)
{
   nir_def *pos = nir_load_frag_coord(b);
   nir_def *frac = nir_ffract(b, nir_fmul(b, nir_channel(b, pos, 1),
                                          nir_imm_float(b, 0.5f)));
   return nir_b2f32(b, nir_fge(b, frac, nir_imm_float(b, 0.5f)));
}

/* Finalize a hand-built motion-comp fragment shader and hand it to the driver
 * (nir_to_rc runs on create_fs_state). */
static void *
mc_create_fs(struct vl_mc *r, nir_builder *b)
{
   r->pipe->screen->finalize_nir(r->pipe->screen, b->shader, true);

   struct pipe_shader_state state = {0};
   state.type = PIPE_SHADER_IR_NIR;
   state.ir.nir = b->shader;
   return r->pipe->create_fs_state(r->pipe, &state);
}

static void *
create_ref_vert_shader(struct vl_mc *r)
{
   nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_VERTEX,
      r->pipe->screen->nir_options[MESA_SHADER_VERTEX], "vl:mc_ref_vs");

   nir_variable *iv_mv0 = nir_variable_create(b.shader, nir_var_shader_in,
                                              glsl_vec4_type(), "mv_top");
   iv_mv0->data.location = VERT_ATTRIB_GENERIC0 + VS_I_MV_TOP;
   nir_variable *iv_mv1 = nir_variable_create(b.shader, nir_var_shader_in,
                                              glsl_vec4_type(), "mv_bottom");
   iv_mv1->data.location = VERT_ATTRIB_GENERIC0 + VS_I_MV_BOTTOM;
   nir_def *vmv[2] = { nir_load_var(&b, iv_mv0), nir_load_var(&b, iv_mv1) };

   nir_def *block_scale = nir_imm_vec2(&b,
      (float)VL_MACROBLOCK_WIDTH / r->buffer_width,
      (float)VL_MACROBLOCK_HEIGHT / r->buffer_height);
   nir_def *t_vpos = calc_position(r, &b, block_scale);

   /* mv_scale.xy = 0.5 / dst; z = 1/4 (quarter-pel); w = 1/MV_WEIGHT_MAX.
    * o_vmv[i].xy = mv*mv_scale + t_vpos; zw = mv*mv_scale. */
   nir_def *mv_scale = nir_imm_vec4(&b,
      0.5f / r->buffer_width, 0.5f / r->buffer_height,
      1.0f / 4.0f, 1.0f / PIPE_VIDEO_MV_WEIGHT_MAX);

   for (unsigned i = 0; i < 2; ++i) {
      nir_def *mv = vmv[i];
      nir_def *o = nir_vec4(&b,
         nir_ffma(&b, nir_channel(&b, mv_scale, 0), nir_channel(&b, mv, 0),
                  nir_channel(&b, t_vpos, 0)),
         nir_ffma(&b, nir_channel(&b, mv_scale, 1), nir_channel(&b, mv, 1),
                  nir_channel(&b, t_vpos, 1)),
         nir_fmul(&b, nir_channel(&b, mv_scale, 2), nir_channel(&b, mv, 2)),
         nir_fmul(&b, nir_channel(&b, mv_scale, 3), nir_channel(&b, mv, 3)));

      nir_variable *ov = nir_variable_create(b.shader, nir_var_shader_out,
                                             glsl_vec4_type(), "vmv_out");
      ov->data.location = VARYING_SLOT_VAR0 + VS_O_VTOP + i;
      nir_store_var(&b, ov, o, 0xf);
   }

   return vl_nir_vs_finish(&b, r->pipe);
}

static void *
create_ref_frag_shader(struct vl_mc *r)
{
   const float y_scale =
      r->buffer_height / 2 *
      r->macroblock_size / VL_MACROBLOCK_HEIGHT;

   nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_FRAGMENT,
      r->pipe->screen->nir_options[MESA_SHADER_FRAGMENT], "vl:mc_ref_fs");

   nir_def *tc[2];
   for (unsigned k = 0; k < 2; ++k) {
      nir_variable *in = nir_variable_create(b.shader, nir_var_shader_in,
                                             glsl_vec4_type(), "tc");
      in->data.location = VARYING_SLOT_VAR0 + VS_O_VTOP + k;
      tc[k] = nir_load_var(&b, in);
   }

   const struct glsl_type *st =
      glsl_sampler_type(GLSL_SAMPLER_DIM_2D, false, false, GLSL_TYPE_FLOAT);
   nir_variable *sv = nir_variable_create(b.shader, nir_var_uniform, st, "ref");
   sv->data.binding = 0;
   nir_deref_instr *samp = nir_build_deref_var(&b, sv);

   /* ref = (field set) ? bottom-field tc : top-field tc.  fragment.w follows the
    * same selection. */
   nir_def *field = calc_line(&b);
   nir_def *sel = nir_fge(&b, field, nir_imm_float(&b, 0.5f));
   nir_def *ref = nir_bcsel(&b, sel, nir_trim_vector(&b, tc[1], 3),
                            nir_trim_vector(&b, tc[0], 3));
   nir_def *frag_w = nir_bcsel(&b, sel, nir_channel(&b, tc[1], 3),
                               nir_channel(&b, tc[0], 3));

   nir_def *ref_x = nir_channel(&b, ref, 0);
   nir_def *ref_y = nir_channel(&b, ref, 1);
   nir_def *ref_z = nir_channel(&b, ref, 2);

   /* When ref.z (the field offset) is nonzero, snap ref.y onto the field grid:
    * ref.y = (floor(ref.y * y_scale) + ref.z) / y_scale.  ref.z == 0 leaves it
    * untouched -- the bcsel false arm, equivalent to the TGSI IF guard. */
   nir_def *adj = nir_fmul(&b,
      nir_fadd(&b, nir_ffloor(&b, nir_fmul(&b, ref_y, nir_imm_float(&b, y_scale))),
               ref_z),
      nir_imm_float(&b, 1.0f / y_scale));
   ref_y = nir_bcsel(&b, nir_fneu(&b, ref_z, nir_imm_float(&b, 0.0f)), adj, ref_y);

   nir_def *texel = nir_tex(&b, nir_vec2(&b, ref_x, ref_y),
                            .texture_deref = samp, .sampler_deref = samp);

   nir_variable *out = nir_variable_create(b.shader, nir_var_shader_out,
                                           glsl_vec4_type(), "color");
   out->data.location = FRAG_RESULT_COLOR;
   nir_store_var(&b, out,
      nir_vec4(&b, nir_channel(&b, texel, 0), nir_channel(&b, texel, 1),
               nir_channel(&b, texel, 2), frag_w), 0xf);

   return mc_create_fs(r, &b);
}

static void *
create_ycbcr_vert_shader(struct vl_mc *r, vl_mc_ycbcr_vert_shader vs_callback, void *callback_priv)
{
   struct vertex2f scale = {
      (float)VL_BLOCK_WIDTH / r->buffer_width * VL_MACROBLOCK_WIDTH / r->macroblock_size,
      (float)VL_BLOCK_HEIGHT / r->buffer_height * VL_MACROBLOCK_HEIGHT / r->macroblock_size
   };

   nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_VERTEX,
      r->pipe->screen->nir_options[MESA_SHADER_VERTEX], "vl:mc_ycbcr_vs");

   nir_def *block_scale = nir_imm_vec2(&b, scale.x, scale.y);
   nir_def *t_vpos = calc_position(r, &b, block_scale);

   nir_variable *iv_rect = nir_get_variable_with_location(b.shader,
      nir_var_shader_in, VERT_ATTRIB_GENERIC0 + VS_I_RECT, glsl_vec4_type());
   nir_variable *iv_pos = nir_get_variable_with_location(b.shader,
      nir_var_shader_in, VERT_ATTRIB_GENERIC0 + VS_I_VPOS, glsl_vec4_type());
   nir_def *vrect = nir_load_var(&b, iv_rect);
   nir_def *vpos = nir_load_var(&b, iv_pos);

   /* The decoder fills the source texcoord (or the IDCT addresses) at VS_O_VTEX. */
   vs_callback(callback_priv, r, &b, VS_O_VTEX, t_vpos);

   /* o_flags.z = intra * 0.5; o_flags.w defaults to -1 (no field split). */
   nir_def *o_flags_z = nir_fmul(&b, nir_channel(&b, vpos, 2), nir_imm_float(&b, 0.5f));
   nir_def *o_flags_w = nir_imm_float(&b, -1.0f);
   nir_def *o_vpos_y = nir_channel(&b, t_vpos, 1);

   if (r->macroblock_size == VL_MACROBLOCK_HEIGHT) {
      /* Interlaced macroblock: vpos.w selects whether this row takes the field
       * offset.  Every branch is a pure value select -- the TGSI IF only guarded
       * the same arithmetic. */
      nir_def *zero = nir_imm_float(&b, 0.0f);
      nir_def *do_split = nir_fneu(&b, nir_channel(&b, vpos, 3), zero);

      /* t_vtex.xy = (vrect.y > 0) ? (0, scale.y) : (-scale.y, 0) */
      nir_def *vry = nir_flt(&b, zero, nir_channel(&b, vrect, 1));
      nir_def *tvtex_x = nir_bcsel(&b, vry, zero, nir_imm_float(&b, -scale.y));
      nir_def *tvtex_y0 = nir_bcsel(&b, vry, nir_imm_float(&b, scale.y), zero);

      /* t_vtex.z = frac(vpos.y * 0.5) (the row parity); t_vtex.y picks x when set. */
      nir_def *tvtex_z = nir_ffract(&b,
         nir_fmul(&b, nir_channel(&b, vpos, 1), nir_imm_float(&b, 0.5f)));
      nir_def *tz = nir_flt(&b, zero, tvtex_z);
      nir_def *tvtex_y = nir_bcsel(&b, tz, tvtex_x, tvtex_y0);

      /* o_vpos.y += t_vtex.y; o_flags.w = (parity) ? 0 : 1 -- only when split. */
      nir_def *new_vpos_y = nir_fadd(&b, nir_channel(&b, t_vpos, 1), tvtex_y);
      o_vpos_y = nir_bcsel(&b, do_split, new_vpos_y, o_vpos_y);
      nir_def *new_flags_w = nir_bcsel(&b, tz, zero, nir_imm_float(&b, 1.0f));
      o_flags_w = nir_bcsel(&b, do_split, new_flags_w, o_flags_w);
   }

   /* Override o_vpos.y (calc_position stored the unadjusted value). */
   nir_variable *o_vpos = nir_get_variable_with_location(b.shader,
      nir_var_shader_out, VARYING_SLOT_POS, glsl_vec4_type());
   nir_store_var(&b, o_vpos, o_vpos_y, 0x2);

   nir_variable *o_flags = nir_variable_create(b.shader, nir_var_shader_out,
      glsl_vec4_type(), "flags_out");
   o_flags->data.location = VARYING_SLOT_VAR0 + VS_O_FLAGS;
   nir_store_var(&b, o_flags,
      nir_vec4(&b, nir_imm_float(&b, 0.0f), nir_imm_float(&b, 0.0f),
               o_flags_z, o_flags_w), 0xf);

   return vl_nir_vs_finish(&b, r->pipe);
}

static void *
create_ycbcr_frag_shader(struct vl_mc *r, float scale, bool invert,
                         vl_mc_ycbcr_frag_shader fs_callback, void *callback_priv)
{
   nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_FRAGMENT,
      r->pipe->screen->nir_options[MESA_SHADER_FRAGMENT], "vl:mc_ycbcr_fs");

   nir_variable *iv_flags = nir_variable_create(b.shader, nir_var_shader_in,
                                                glsl_vec4_type(), "flags");
   iv_flags->data.location = VARYING_SLOT_VAR0 + VS_O_FLAGS;
   nir_def *flags = nir_load_var(&b, iv_flags);
   nir_def *flags_z = nir_channel(&b, flags, 2);
   nir_def *flags_w = nir_channel(&b, flags, 3);

   /* Kill the fragment whose field flag matches the line parity (it belongs to
    * the other field). */
   nir_def *field = calc_line(&b);
   nir_discard_if(&b, nir_feq(&b, flags_w, field));

   /* Surviving fragments: the decoder supplies the source value (a plain
    * reference fetch, or the IDCT-reconstructed residual), scaled and biased by
    * the intra weight, negated for the reverse-subtract blender pass. */
   nir_def *src = nir_trim_vector(&b, fs_callback(callback_priv, r, &b, VS_O_VTEX), 3);
   nir_def *fz3 = nir_replicate(&b, flags_z, 3);
   nir_def *scaled = (scale != 1.0f)
      ? nir_ffma(&b, src, nir_replicate(&b, nir_imm_float(&b, scale), 3), fz3)
      : nir_fadd(&b, src, fz3);
   nir_def *rgb = nir_fmul(&b, scaled,
      nir_replicate(&b, nir_imm_float(&b, invert ? -1.0f : 1.0f), 3));

   nir_variable *out = nir_variable_create(b.shader, nir_var_shader_out,
                                           glsl_vec4_type(), "color");
   out->data.location = FRAG_RESULT_COLOR;
   nir_store_var(&b, out,
      nir_vec4(&b, nir_channel(&b, rgb, 0), nir_channel(&b, rgb, 1),
               nir_channel(&b, rgb, 2), nir_imm_float(&b, 1.0f)), 0xf);

   return mc_create_fs(r, &b);
}

static bool
init_pipe_state(struct vl_mc *r)
{
   struct pipe_sampler_state sampler;
   struct pipe_blend_state blend;
   struct pipe_rasterizer_state rs_state;
   unsigned i;

   assert(r);

   memset(&sampler, 0, sizeof(sampler));
   sampler.wrap_s = PIPE_TEX_WRAP_CLAMP_TO_EDGE;
   sampler.wrap_t = PIPE_TEX_WRAP_CLAMP_TO_EDGE;
   sampler.wrap_r = PIPE_TEX_WRAP_CLAMP_TO_BORDER;
   sampler.min_img_filter = PIPE_TEX_FILTER_LINEAR;
   sampler.min_mip_filter = PIPE_TEX_MIPFILTER_NONE;
   sampler.mag_img_filter = PIPE_TEX_FILTER_LINEAR;
   sampler.compare_mode = PIPE_TEX_COMPARE_NONE;
   sampler.compare_func = PIPE_FUNC_ALWAYS;
   /* mesa-26 inverted the sense to unnormalized_coords; the memset default 0
    * keeps normalized coordinates, as the reference fetch requires. */
   r->sampler_ref = r->pipe->create_sampler_state(r->pipe, &sampler);
   if (!r->sampler_ref)
      goto error_sampler_ref;

   for (i = 0; i < VL_MC_NUM_BLENDERS; ++i) {
      memset(&blend, 0, sizeof blend);
      blend.independent_blend_enable = 0;
      blend.rt[0].blend_enable = 1;
      blend.rt[0].rgb_func = PIPE_BLEND_ADD;
      blend.rt[0].rgb_src_factor = PIPE_BLENDFACTOR_SRC_ALPHA;
      blend.rt[0].rgb_dst_factor = PIPE_BLENDFACTOR_ZERO;
      blend.rt[0].alpha_func = PIPE_BLEND_ADD;
      blend.rt[0].alpha_src_factor = PIPE_BLENDFACTOR_SRC_ALPHA;
      blend.rt[0].alpha_dst_factor = PIPE_BLENDFACTOR_ZERO;
      blend.logicop_enable = 0;
      blend.logicop_func = PIPE_LOGICOP_CLEAR;
      blend.rt[0].colormask = i;
      blend.dither = 0;
      r->blend_clear[i] = r->pipe->create_blend_state(r->pipe, &blend);
      if (!r->blend_clear[i])
         goto error_blend;

      blend.rt[0].rgb_dst_factor = PIPE_BLENDFACTOR_ONE;
      blend.rt[0].alpha_dst_factor = PIPE_BLENDFACTOR_ONE;
      r->blend_add[i] = r->pipe->create_blend_state(r->pipe, &blend);
      if (!r->blend_add[i])
         goto error_blend;

      blend.rt[0].rgb_func = PIPE_BLEND_REVERSE_SUBTRACT;
      blend.rt[0].alpha_dst_factor = PIPE_BLEND_REVERSE_SUBTRACT;
      r->blend_sub[i] = r->pipe->create_blend_state(r->pipe, &blend);
      if (!r->blend_sub[i])
         goto error_blend;
   }

   memset(&rs_state, 0, sizeof(rs_state));
   /*rs_state.sprite_coord_enable */
   rs_state.sprite_coord_mode = PIPE_SPRITE_COORD_UPPER_LEFT;
   rs_state.point_quad_rasterization = true;
   rs_state.point_size = VL_BLOCK_WIDTH;
   rs_state.half_pixel_center = true;
   rs_state.bottom_edge_rule = true;
   rs_state.depth_clip_near = 1;
   rs_state.depth_clip_far = 1;

   r->rs_state = r->pipe->create_rasterizer_state(r->pipe, &rs_state);
   if (!r->rs_state)
      goto error_rs_state;

   return true;

error_rs_state:
error_blend:
   for (i = 0; i < VL_MC_NUM_BLENDERS; ++i) {
      if (r->blend_sub[i])
         r->pipe->delete_blend_state(r->pipe, r->blend_sub[i]);

      if (r->blend_add[i])
         r->pipe->delete_blend_state(r->pipe, r->blend_add[i]);

      if (r->blend_clear[i])
         r->pipe->delete_blend_state(r->pipe, r->blend_clear[i]);
   }

   r->pipe->delete_sampler_state(r->pipe, r->sampler_ref);

error_sampler_ref:
   return false;
}

static void
cleanup_pipe_state(struct vl_mc *r)
{
   unsigned i;

   assert(r);

   r->pipe->delete_sampler_state(r->pipe, r->sampler_ref);
   for (i = 0; i < VL_MC_NUM_BLENDERS; ++i) {
      r->pipe->delete_blend_state(r->pipe, r->blend_clear[i]);
      r->pipe->delete_blend_state(r->pipe, r->blend_add[i]);
      r->pipe->delete_blend_state(r->pipe, r->blend_sub[i]);
   }
   r->pipe->delete_rasterizer_state(r->pipe, r->rs_state);
}

bool
vl_mc_init(struct vl_mc *renderer, struct pipe_context *pipe,
           unsigned buffer_width, unsigned buffer_height,
           unsigned macroblock_size, float scale,
           vl_mc_ycbcr_vert_shader vs_callback,
           vl_mc_ycbcr_frag_shader fs_callback,
           void *callback_priv)
{
   assert(renderer);
   assert(pipe);

   memset(renderer, 0, sizeof(struct vl_mc));

   renderer->pipe = pipe;
   renderer->buffer_width = buffer_width;
   renderer->buffer_height = buffer_height;
   renderer->macroblock_size = macroblock_size;

   if (!init_pipe_state(renderer))
      goto error_pipe_state;

   renderer->vs_ref = create_ref_vert_shader(renderer);
   if (!renderer->vs_ref)
      goto error_vs_ref;

   renderer->vs_ycbcr = create_ycbcr_vert_shader(renderer, vs_callback, callback_priv);
   if (!renderer->vs_ycbcr)
      goto error_vs_ycbcr;

   renderer->fs_ref = create_ref_frag_shader(renderer);
   if (!renderer->fs_ref)
      goto error_fs_ref;

   renderer->fs_ycbcr = create_ycbcr_frag_shader(renderer, scale, false, fs_callback, callback_priv);
   if (!renderer->fs_ycbcr)
      goto error_fs_ycbcr;

   renderer->fs_ycbcr_sub = create_ycbcr_frag_shader(renderer, scale, true, fs_callback, callback_priv);
   if (!renderer->fs_ycbcr_sub)
      goto error_fs_ycbcr_sub;

   return true;
   
error_fs_ycbcr_sub:
   renderer->pipe->delete_fs_state(renderer->pipe, renderer->fs_ycbcr);

error_fs_ycbcr:
   renderer->pipe->delete_fs_state(renderer->pipe, renderer->fs_ref);

error_fs_ref:
   renderer->pipe->delete_vs_state(renderer->pipe, renderer->vs_ycbcr);

error_vs_ycbcr:
   renderer->pipe->delete_vs_state(renderer->pipe, renderer->vs_ref);

error_vs_ref:
   cleanup_pipe_state(renderer);

error_pipe_state:
   return false;
}

void
vl_mc_cleanup(struct vl_mc *renderer)
{
   assert(renderer);

   cleanup_pipe_state(renderer);

   renderer->pipe->delete_vs_state(renderer->pipe, renderer->vs_ref);
   renderer->pipe->delete_vs_state(renderer->pipe, renderer->vs_ycbcr);
   renderer->pipe->delete_fs_state(renderer->pipe, renderer->fs_ref);
   renderer->pipe->delete_fs_state(renderer->pipe, renderer->fs_ycbcr);
   renderer->pipe->delete_fs_state(renderer->pipe, renderer->fs_ycbcr_sub);
}

bool
vl_mc_init_buffer(struct vl_mc *renderer, struct vl_mc_buffer *buffer)
{
   assert(renderer && buffer);

   buffer->viewport.scale[2] = 1;
   buffer->viewport.translate[0] = 0;
   buffer->viewport.translate[1] = 0;
   buffer->viewport.translate[2] = 0;
   buffer->viewport.swizzle_x = PIPE_VIEWPORT_SWIZZLE_POSITIVE_X;
   buffer->viewport.swizzle_y = PIPE_VIEWPORT_SWIZZLE_POSITIVE_Y;
   buffer->viewport.swizzle_z = PIPE_VIEWPORT_SWIZZLE_POSITIVE_Z;
   buffer->viewport.swizzle_w = PIPE_VIEWPORT_SWIZZLE_POSITIVE_W;

   buffer->fb_state.nr_cbufs = 1;
   /* mesa-26 embeds the zsbuf by value; a zeroed surface (no texture) means
    * none. */
   memset(&buffer->fb_state.zsbuf, 0, sizeof(buffer->fb_state.zsbuf));

   return true;
}

void
vl_mc_cleanup_buffer(struct vl_mc_buffer *buffer)
{
   assert(buffer);
}

void
vl_mc_set_surface(struct vl_mc_buffer *buffer, struct pipe_surface *surface)
{
   assert(buffer && surface);

   unsigned width, height;

   buffer->surface_cleared = false;

   /* mesa-26 pipe_surface has no width/height; derive from the texture level,
    * and the framebuffer embeds the surface by value. */
   pipe_surface_size(surface, &width, &height);
   buffer->viewport.scale[0] = width;
   buffer->viewport.scale[1] = height;

   buffer->fb_state.width = width;
   buffer->fb_state.height = height;
   buffer->fb_state.cbufs[0] = *surface;
}

static void
prepare_pipe_4_rendering(struct vl_mc *renderer, struct vl_mc_buffer *buffer, unsigned mask)
{
   assert(buffer);

   renderer->pipe->bind_rasterizer_state(renderer->pipe, renderer->rs_state);

   if (buffer->surface_cleared)
      renderer->pipe->bind_blend_state(renderer->pipe, renderer->blend_add[mask]);
   else
      renderer->pipe->bind_blend_state(renderer->pipe, renderer->blend_clear[mask]);

   renderer->pipe->set_framebuffer_state(renderer->pipe, &buffer->fb_state);
   renderer->pipe->set_viewport_states(renderer->pipe, 0, 1, &buffer->viewport);
}

void
vl_mc_render_ref(struct vl_mc *renderer, struct vl_mc_buffer *buffer, struct pipe_sampler_view *ref)
{
   assert(buffer && ref);

   prepare_pipe_4_rendering(renderer, buffer, PIPE_MASK_R | PIPE_MASK_G | PIPE_MASK_B);

   renderer->pipe->bind_vs_state(renderer->pipe, renderer->vs_ref);
   renderer->pipe->bind_fs_state(renderer->pipe, renderer->fs_ref);

   renderer->pipe->set_sampler_views(renderer->pipe, MESA_SHADER_FRAGMENT,
                                     0, 1, 0, &ref);
   renderer->pipe->bind_sampler_states(renderer->pipe, MESA_SHADER_FRAGMENT,
                                       0, 1, &renderer->sampler_ref);

   util_draw_arrays_instanced(renderer->pipe, MESA_PRIM_QUADS, 0, 4, 0,
                              renderer->buffer_width / VL_MACROBLOCK_WIDTH *
                              renderer->buffer_height / VL_MACROBLOCK_HEIGHT);

   buffer->surface_cleared = true;
}

void
vl_mc_render_ycbcr(struct vl_mc *renderer, struct vl_mc_buffer *buffer, unsigned component, unsigned num_instances)
{
   unsigned mask = 1 << component;

   assert(buffer);

   if (num_instances == 0)
      return;

   prepare_pipe_4_rendering(renderer, buffer, mask);

   renderer->pipe->bind_vs_state(renderer->pipe, renderer->vs_ycbcr);
   renderer->pipe->bind_fs_state(renderer->pipe, renderer->fs_ycbcr);

   util_draw_arrays_instanced(renderer->pipe, MESA_PRIM_QUADS, 0, 4, 0, num_instances);
   
   if (buffer->surface_cleared) {
      renderer->pipe->bind_blend_state(renderer->pipe, renderer->blend_sub[mask]);
      renderer->pipe->bind_fs_state(renderer->pipe, renderer->fs_ycbcr_sub);
      util_draw_arrays_instanced(renderer->pipe, MESA_PRIM_QUADS, 0, 4, 0, num_instances);
   }
}
