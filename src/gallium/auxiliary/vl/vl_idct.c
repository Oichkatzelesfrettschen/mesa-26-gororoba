/**************************************************************************
 *
 * Copyright 2010 Christian König
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
#include "pipe/p_screen.h"

#include "util/u_draw.h"
#include "util/u_sampler.h"
#include "util/u_memory.h"

#include "compiler/nir/nir_builder.h"

#include "vl_defines.h"
#include "vl_types.h"
#include "vl_vertex_buffers.h"
#include "vl_idct.h"
#include "vl_nir.h"

enum VS_OUTPUT
{
   VS_O_VPOS = 0,
   VS_O_L_ADDR0 = 0,
   VS_O_L_ADDR1,
   VS_O_R_ADDR0,
   VS_O_R_ADDR1
};

/**
 * The DCT matrix stored as hex representation of floats. Equal to the following equation:
 * for (i = 0; i < 8; ++i)
 *    for (j = 0; j < 8; ++j)
 *       if (i == 0) const_matrix[i][j] = 1.0f / sqrtf(8.0f);
 *       else const_matrix[i][j] = sqrtf(2.0f / 8.0f) * cosf((2 * j + 1) * i * M_PI / (2.0f * 8.0f));
 */
static const uint32_t const_matrix[8][8] = {
   { 0x3eb504f3, 0x3eb504f3, 0x3eb504f3, 0x3eb504f3, 0x3eb504f3, 0x3eb504f3, 0x3eb504f3, 0x3eb504f3 },
   { 0x3efb14be, 0x3ed4db31, 0x3e8e39da, 0x3dc7c5c4, 0xbdc7c5c2, 0xbe8e39d9, 0xbed4db32, 0xbefb14bf },
   { 0x3eec835f, 0x3e43ef15, 0xbe43ef14, 0xbeec835e, 0xbeec835f, 0xbe43ef1a, 0x3e43ef1b, 0x3eec835f },
   { 0x3ed4db31, 0xbdc7c5c2, 0xbefb14bf, 0xbe8e39dd, 0x3e8e39d7, 0x3efb14bf, 0x3dc7c5d0, 0xbed4db34 },
   { 0x3eb504f3, 0xbeb504f3, 0xbeb504f4, 0x3eb504f1, 0x3eb504f3, 0xbeb504f0, 0xbeb504ef, 0x3eb504f4 },
   { 0x3e8e39da, 0xbefb14bf, 0x3dc7c5c8, 0x3ed4db32, 0xbed4db34, 0xbdc7c5bb, 0x3efb14bf, 0xbe8e39d7 },
   { 0x3e43ef15, 0xbeec835f, 0x3eec835f, 0xbe43ef07, 0xbe43ef23, 0x3eec8361, 0xbeec835c, 0x3e43ef25 },
   { 0x3dc7c5c4, 0xbe8e39dd, 0x3ed4db32, 0xbefb14c0, 0x3efb14be, 0xbed4db31, 0x3e8e39ce, 0xbdc7c596 },
};

/* The IDCT runs as two separable matrix-vector passes over 8x8 blocks packed
 * four coefficients per texel (VL_BLOCK_WIDTH/4 == 2 texels wide).  These
 * helpers build the sample addresses and the per-row dot products the passes
 * share.  The arithmetic is the proven shader IDCT, transliterated lane for
 * lane from TGSI writemasks to NIR component builds. */

/* Even/odd address pair for one column.  Each address is a vec2: one lane
 * carries the row "start" coordinate, the other the column "tc".  right_side
 * and transposed pick which lane is which, since the second pass samples the
 * matrix transposed.  addr[1] is addr[0] nudged one texel (1/size) along the
 * start lane so fetch_four reads the coefficient pair in one TEX each. */
static void
calc_addr(nir_builder *b, nir_def *addr[2], nir_def *tc, nir_def *start,
          bool right_side, bool transposed, float size)
{
   bool start_in_x = (right_side == transposed);
   unsigned sw_start = right_side ? 1 : 0;
   unsigned sw_tc = right_side ? 0 : 1;

   nir_def *s0 = nir_channel(b, start, sw_start);
   nir_def *t0 = nir_channel(b, tc, sw_tc);
   nir_def *s1 = nir_fadd(b, s0, nir_imm_float(b, 1.0f / size));

   addr[0] = start_in_x ? nir_vec2(b, s0, t0) : nir_vec2(b, t0, s0);
   addr[1] = start_in_x ? nir_vec2(b, s1, t0) : nir_vec2(b, t0, s1);
}

/* Step both addresses by pos texels along the tc lane, walking the eight matrix
 * rows (mismatch) or the +-2 coefficient neighborhood (stage 1).  pos is
 * signed; the start lane is copied through unchanged. */
static void
increment_addr(nir_builder *b, nir_def *daddr[2], nir_def *saddr[2],
               bool right_side, bool transposed, int pos, float size)
{
   unsigned tc_lane = (right_side == transposed) ? 1 : 0;
   nir_def *delta = nir_imm_float(b, pos / size);

   for (unsigned k = 0; k < 2; ++k) {
      nir_def *x = nir_channel(b, saddr[k], 0);
      nir_def *y = nir_channel(b, saddr[k], 1);
      if (tc_lane == 0)
         x = nir_fadd(b, x, delta);
      else
         y = nir_fadd(b, y, delta);
      daddr[k] = nir_vec2(b, x, y);
   }
}

/* Sample the even/odd texel pair.  The caller sizes the coordinate to the
 * sampler: vec2 for the 2D source/matrix planes, vec3 for the 3D intermediate
 * the second pass reads. */
static void
fetch_four(nir_builder *b, nir_def *m[2], nir_def *addr[2],
           nir_deref_instr *sampler)
{
   m[0] = nir_tex(b, addr[0], .texture_deref = sampler, .sampler_deref = sampler);
   m[1] = nir_tex(b, addr[1], .texture_deref = sampler, .sampler_deref = sampler);
}

static nir_deref_instr *
idct_combined_sampler(nir_builder *b, enum glsl_sampler_dim dim,
                      unsigned binding, const char *name)
{
   const struct glsl_type *sampler_type =
      glsl_sampler_type(dim, false, false, GLSL_TYPE_FLOAT);
   nir_variable *sampler =
      nir_variable_create(b->shader, nir_var_uniform, sampler_type, name);
   sampler->data.binding = binding;
   BITSET_SET(b->shader->info.textures_used, binding);
   BITSET_SET(b->shader->info.samplers_used, binding);
   return nir_build_deref_var(b, sampler);
}

/* One output coefficient: dot4 of the matrix row with the even coefficients
 * plus dot4 with the odd coefficients -- the separable DCT-III row sum. */
static nir_def *
matrix_mul(nir_builder *b, nir_def *l[2], nir_def *r[2])
{
   return nir_fadd(b, nir_fdot4(b, l[0], r[0]), nir_fdot4(b, l[1], r[1]));
}

/* Widen a vec2/vec3 address to the vec4 a generic varying carries, zero-filling
 * the unused lanes (the TGSI form wrote only the live lanes; zero is inert). */
static nir_def *
idct_pad4(nir_builder *b, nir_def *v)
{
   nir_def *zero = nir_imm_float(b, 0.0f);
   unsigned n = v->num_components;
   return nir_vec4(b, nir_channel(b, v, 0),
                   n > 1 ? nir_channel(b, v, 1) : zero,
                   n > 2 ? nir_channel(b, v, 2) : zero, zero);
}

/* Finalize a hand-built IDCT fragment shader.  The stage-1 pass writes several
 * render targets, so the single-output vl_nir_fs_finish does not fit; the
 * driver finalizes (nir_to_rc) on create_fs_state. */
static void *
idct_create_fs(struct vl_idct *idct, nir_builder *b)
{
   /* Lower deref-combined samplers to indexed tex.  The IDCT builders mark
    * textures_used/samplers_used when declaring each sampler; nir_lower_samplers
    * does not update those SoA backend masks. */
   NIR_PASS(_, b->shader, nir_lower_samplers);
   nir_shader_gather_info(b->shader, nir_shader_get_entrypoint(b->shader));
   nir_assign_io_var_locations(b->shader, nir_var_shader_in);
   nir_assign_io_var_locations(b->shader, nir_var_shader_out);
   if (idct->pipe->screen->finalize_nir)
      idct->pipe->screen->finalize_nir(idct->pipe->screen, b->shader, true);

   struct pipe_shader_state state = {0};
   state.type = PIPE_SHADER_IR_NIR;
   state.ir.nir = b->shader;
   return idct->pipe->create_fs_state(idct->pipe, &state);
}

static void *
create_mismatch_vert_shader(struct vl_idct *idct)
{
   nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_VERTEX,
      idct->pipe->screen->nir_options[MESA_SHADER_VERTEX],
      "vl:idct_mismatch_vs");

   nir_variable *iv_pos = nir_variable_create(b.shader, nir_var_shader_in,
                                              glsl_vec4_type(), "vpos");
   iv_pos->data.location = VERT_ATTRIB_GENERIC0 + VS_I_VPOS;
   nir_def *vpos = nir_trim_vector(&b, nir_load_var(&b, iv_pos), 2);

   nir_def *scale = nir_imm_vec2(&b,
      (float)VL_BLOCK_WIDTH / idct->buffer_width,
      (float)VL_BLOCK_HEIGHT / idct->buffer_height);

   /* The mismatch point must cover the block's last source texel, whose .w
    * carries coefficient 63: texel column 2*vpos.x+1 (the second of the
    * block's texel pair), row 8*vpos.y+7.  Center the point on that texel:
    * x = vpos*scale + (1.5/2)*scale reaches the second texel's center and
    * y = vpos*scale + (7.5/8)*scale the last row's center.  A whole-scale
    * offset instead puts the point on the block's far corner, an exact texel
    * boundary tie: the rasterizer then lands the point one texel low/right,
    * overwriting the NEIGHBOR block's first texel (its DC among the four
    * coefficients) with this block's mismatch output instead of correcting
    * this block's coefficient 63. */
   nir_def *pos_center = nir_imm_vec2(&b,
      0.75f * (float)VL_BLOCK_WIDTH / idct->buffer_width,
      0.9375f * (float)VL_BLOCK_HEIGHT / idct->buffer_height);
   nir_def *pos_xy = nir_fmad(&b, vpos, scale, pos_center);
   nir_variable *ov_pos = nir_variable_create(b.shader, nir_var_shader_out,
                                              glsl_vec4_type(), "pos_out");
   ov_pos->data.location = VARYING_SLOT_POS;
   nir_store_var(&b, ov_pos,
      nir_vec4(&b, nir_channel(&b, pos_xy, 0), nir_channel(&b, pos_xy, 1),
               nir_imm_float(&b, 1.0f), nir_imm_float(&b, 1.0f)), 0xf);

   /* o_l_addr = calc_addr(t_tex, t_tex), t_tex = vpos*scale.  A point's
    * varyings are not interpolated toward fragment centers the way a quad's
    * are, so the read address arrives at the exact block-corner texel edge;
    * center both lanes (half a four-coefficient texel in x, half a row in y),
    * the same NEAREST-tie discipline as the first-pass l_center bias. */
   nir_def *t_tex = nir_fmul(&b, vpos, scale);
   nir_def *addr[2];
   calc_addr(&b, addr, t_tex, t_tex, false, false, idct->buffer_width / 4);
   nir_def *read_center = nir_imm_vec2(&b,
      2.0f / idct->buffer_width, 0.5f / idct->buffer_height);
   addr[0] = nir_fadd(&b, addr[0], read_center);
   addr[1] = nir_fadd(&b, addr[1], read_center);

   for (unsigned k = 0; k < 2; ++k) {
      nir_variable *ov = nir_variable_create(b.shader, nir_var_shader_out,
                                             glsl_vec4_type(), "l_addr_out");
      ov->data.location = VARYING_SLOT_VAR0 + VS_O_L_ADDR0 + k;
      nir_store_var(&b, ov, idct_pad4(&b, addr[k]), 0xf);
   }

   return vl_nir_vs_finish(&b, idct->pipe);
}

static void *
create_mismatch_frag_shader(struct vl_idct *idct)
{
   const float K14 = (float)(1 << 14);

   nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_FRAGMENT,
      idct->pipe->screen->nir_options[MESA_SHADER_FRAGMENT],
      "vl:idct_mismatch_fs");

   nir_def *addr[2];
   for (unsigned k = 0; k < 2; ++k) {
      nir_variable *in = nir_variable_create(b.shader, nir_var_shader_in,
                                             glsl_vec4_type(), "l_addr");
      in->data.location = VARYING_SLOT_VAR0 + VS_O_L_ADDR0 + k;
      addr[k] = nir_trim_vector(&b, nir_load_var(&b, in), 2);
   }

   nir_deref_instr *samp =
      idct_combined_sampler(&b, GLSL_SAMPLER_DIM_2D, 0, "src");

   /* Fetch all eight matrix rows (even/odd texel each, 64 coefficients). */
   nir_def *m[8][2];
   for (unsigned i = 0; i < 8; ++i) {
      nir_def *a[2];
      increment_addr(&b, a, addr, false, false, i, idct->buffer_height);
      fetch_four(&b, m[i], a, samp);
   }

   /* Accumulate the even and odd texels, then fold to one vec4 of partial sums. */
   nir_def *sum0 = m[0][0], *sum1 = m[0][1];
   for (unsigned i = 1; i < 8; ++i) {
      sum0 = nir_fadd(&b, sum0, m[i][0]);
      sum1 = nir_fadd(&b, sum1, m[i][1]);
   }
   nir_def *sum = nir_fadd(&b, sum0, sum1);

   /* IEEE-1180 mismatch control on coefficient 63 (the .w of the last texel).
    * total parity = frac(sum(|partial sums|) * 16384) < 0.5; coeff-63 parity is
    * the same test on |coeff63| * 16384; nudge coeff63 by +-1/32768 only when
    * both are odd. */
   nir_def *sum_acc = nir_fdot4(&b, nir_fabs(&b, sum),
                                nir_imm_vec4(&b, K14, K14, K14, K14));
   nir_def *c63 = nir_channel(&b, m[7][1], 3);
   nir_def *c63_acc = nir_fmul(&b, nir_fabs(&b, c63), nir_imm_float(&b, K14));

   nir_def *sum_odd = nir_flt(&b, nir_fabs(&b, nir_ffract(&b, sum_acc)),
                              nir_imm_float(&b, 0.5f));
   nir_def *c63_odd = nir_flt(&b, nir_fabs(&b, nir_ffract(&b, c63_acc)),
                              nir_imm_float(&b, 0.5f));

   nir_def *corr = nir_bcsel(&b, c63_odd,
                             nir_imm_float(&b, 1.0f / (1 << 15)),
                             nir_imm_float(&b, -1.0f / (1 << 15)));
   corr = nir_fmul(&b, corr, nir_b2f32(&b, sum_odd));

   nir_def *color = nir_vec4(&b,
      nir_channel(&b, m[7][1], 0), nir_channel(&b, m[7][1], 1),
      nir_channel(&b, m[7][1], 2), nir_fadd(&b, corr, c63));

   nir_variable *out = nir_variable_create(b.shader, nir_var_shader_out,
                                           glsl_vec4_type(), "color");
   out->data.location = FRAG_RESULT_COLOR;
   nir_store_var(&b, out, color, 0xf);

   return idct_create_fs(idct, &b);
}

static void *
create_stage1_vert_shader(struct vl_idct *idct)
{
   nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_VERTEX,
      idct->pipe->screen->nir_options[MESA_SHADER_VERTEX],
      "vl:idct_stage1_vs");

   nir_variable *iv_rect = nir_variable_create(b.shader, nir_var_shader_in,
                                               glsl_vec4_type(), "vrect");
   iv_rect->data.location = VERT_ATTRIB_GENERIC0 + VS_I_RECT;
   nir_variable *iv_pos = nir_variable_create(b.shader, nir_var_shader_in,
                                              glsl_vec4_type(), "vpos");
   iv_pos->data.location = VERT_ATTRIB_GENERIC0 + VS_I_VPOS;
   nir_def *vrect = nir_trim_vector(&b, nir_load_var(&b, iv_rect), 2);
   nir_def *vpos = nir_trim_vector(&b, nir_load_var(&b, iv_pos), 2);

   nir_def *scale = nir_imm_vec2(&b,
      (float)VL_BLOCK_WIDTH / idct->buffer_width,
      (float)VL_BLOCK_HEIGHT / idct->buffer_height);

   /* o_vpos.xy = (vpos + vrect) * scale; zw = 1. */
   nir_def *t_tex = nir_fmul(&b, nir_fadd(&b, vpos, vrect), scale);
   nir_variable *ov_pos = nir_variable_create(b.shader, nir_var_shader_out,
                                              glsl_vec4_type(), "pos_out");
   ov_pos->data.location = VARYING_SLOT_POS;
   nir_store_var(&b, ov_pos,
      nir_vec4(&b, nir_channel(&b, t_tex, 0), nir_channel(&b, t_tex, 1),
               nir_imm_float(&b, 1.0f), nir_imm_float(&b, 1.0f)), 0xf);

   nir_def *t_start = nir_fmul(&b, vpos, scale);

   /* Left = the source coefficients walked by the matrix rows; right = the DCT
    * matrix sampled transposed (the 0.0 start lane is the matrix origin). */
   nir_def *l_addr[2], *r_addr[2];
   calc_addr(&b, l_addr, t_tex, t_start, false, false, idct->buffer_width / 4);
   /* The source coefficient plane is fetched NEAREST on both lanes at non-power-of-
    * two sizes: the stepped start/x lane uses calc_addr's buffer_width/4 size,
    * while the row lane uses buffer_height via the interpolated
    * (vpos+vrect)*scale varying.  NEAREST resolves the
    * leftmost columns of a block a hair below the intended texel edge, so floor()
    * reads one texel low on each lane.  The column drop puts the horizontal DC into
    * slot 4 (a flat block reconstructs as horizontal frequency 4); the row drop
    * reads the source row one high, which on the single-render-target collapse
    * misplaces a block's first two output columns by one vertical-frequency
    * channel.  Bias the start/x lane by half one packed coefficient texel,
    * 0.5/(buffer_width/4) = 2/buffer_width, and the row lane by
    * 0.5/buffer_height.  floor(R+0.5)=R keeps round-to-nearest exact and is
    * inert where the coordinate already sits on the center. */
   nir_def *l_center = nir_imm_vec2(&b, 2.0f / idct->buffer_width,
                                    0.5f / idct->buffer_height);
   l_addr[0] = nir_fadd(&b, l_addr[0], l_center);
   l_addr[1] = nir_fadd(&b, l_addr[1], l_center);
   calc_addr(&b, r_addr, vrect, nir_imm_vec2(&b, 0.0f, 0.0f), true, true,
             VL_BLOCK_WIDTH / 4);

   nir_def *addrs[4] = { l_addr[0], l_addr[1], r_addr[0], r_addr[1] };
   for (unsigned k = 0; k < 4; ++k) {
      nir_variable *ov = nir_variable_create(b.shader, nir_var_shader_out,
                                             glsl_vec4_type(), "addr_out");
      ov->data.location = VARYING_SLOT_VAR0 + VS_O_L_ADDR0 + k;
      nir_store_var(&b, ov, idct_pad4(&b, addrs[k]), 0xf);
   }

   return vl_nir_vs_finish(&b, idct->pipe);
}

static void *
create_stage1_frag_shader(struct vl_idct *idct)
{
   nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_FRAGMENT,
      idct->pipe->screen->nir_options[MESA_SHADER_FRAGMENT],
      "vl:idct_stage1_fs");

   nir_def *l_addr[2], *r_addr[2];
   for (unsigned k = 0; k < 2; ++k) {
      nir_variable *in = nir_variable_create(b.shader, nir_var_shader_in,
                                             glsl_vec4_type(), "l_addr");
      in->data.location = VARYING_SLOT_VAR0 + VS_O_L_ADDR0 + k;
      l_addr[k] = nir_trim_vector(&b, nir_load_var(&b, in), 2);
   }
   for (unsigned k = 0; k < 2; ++k) {
      nir_variable *in = nir_variable_create(b.shader, nir_var_shader_in,
                                             glsl_vec4_type(), "r_addr");
      in->data.location = VARYING_SLOT_VAR0 + VS_O_R_ADDR0 + k;
      r_addr[k] = nir_trim_vector(&b, nir_load_var(&b, in), 2);
   }

   nir_deref_instr *samp_src =
      idct_combined_sampler(&b, GLSL_SAMPLER_DIM_2D, 0, "src");
   nir_deref_instr *samp_mat =
      idct_combined_sampler(&b, GLSL_SAMPLER_DIM_2D, 1, "matrix");

   /* Four source rows around the output row (offsets -2..+1). */
   nir_def *l[4][2];
   for (unsigned i = 0; i < 4; ++i) {
      nir_def *a[2];
      increment_addr(&b, a, l_addr, false, false, (int)i - 2, idct->buffer_height);
      fetch_four(&b, l[i], a, samp_src);
   }

   /* One render target per output column: the matrix column dotted with each of
    * the four source rows fills the four channels of that target. */
   for (unsigned i = 0; i < idct->nr_of_render_targets; ++i) {
      nir_def *r[2], *a[2];
      increment_addr(&b, a, r_addr, true, true,
                     (int)i - (int)idct->nr_of_render_targets / 2, VL_BLOCK_HEIGHT);
      fetch_four(&b, r, a, samp_mat);

      nir_def *chan[4];
      for (unsigned j = 0; j < 4; ++j)
         chan[j] = matrix_mul(&b, l[j], r);

      nir_variable *out = nir_variable_create(b.shader, nir_var_shader_out,
                                              glsl_vec4_type(), "frag");
      out->data.location = FRAG_RESULT_DATA0 + i;
      nir_store_var(&b, out,
                    nir_vec4(&b, chan[0], chan[1], chan[2], chan[3]), 0xf);
   }

   return idct_create_fs(idct, &b);
}

void
vl_idct_stage2_vert_shader(struct vl_idct *idct, nir_builder *b,
                           unsigned first_output, nir_def *tex)
{
   /* This runs inside the motion-comp ycbcr vertex shader and shares its
    * vrect/vpos inputs; fetch the existing variables rather than redeclaring at
    * the same attribute location. */
   nir_variable *iv_rect = nir_get_variable_with_location(b->shader,
      nir_var_shader_in, VERT_ATTRIB_GENERIC0 + VS_I_RECT, glsl_vec4_type());
   nir_variable *iv_pos = nir_get_variable_with_location(b->shader,
      nir_var_shader_in, VERT_ATTRIB_GENERIC0 + VS_I_VPOS, glsl_vec4_type());
   nir_def *vrect = nir_trim_vector(b, nir_load_var(b, iv_rect), 2);
   nir_def *vpos = nir_trim_vector(b, nir_load_var(b, iv_pos), 2);

   nir_def *scale = nir_imm_vec2(b,
      (float)VL_BLOCK_WIDTH / idct->buffer_width,
      (float)VL_BLOCK_HEIGHT / idct->buffer_height);

   /* tex.z selects the intermediate depth slice this render target reads
    * (integer block-width / render-target division, as in the original). */
   nir_def *tex_z = nir_fmul(b, nir_channel(b, vrect, 0),
      nir_imm_float(b, (float)(VL_BLOCK_WIDTH / idct->nr_of_render_targets)));
   nir_def *t_start = nir_fmul(b, vpos, scale);

   /* Left = transpose matrix from the origin; right = the intermediate sampled
    * at the macroblock position, carrying the depth slice in z. */
   nir_def *l_addr[2], *r_addr[2];
   calc_addr(b, l_addr, vrect, nir_imm_vec2(b, 0.0f, 0.0f), false, false,
             VL_BLOCK_WIDTH / 4);
   calc_addr(b, r_addr, nir_trim_vector(b, tex, 2), t_start, true, false,
             idct->buffer_height / 4);

   /* The intermediate carries each block-row as a row-lane texel pair: texel 2k
    * holds vertical frequencies 0..3, texel 2k+1 holds 4..7.  t_start.y puts the
    * pair's first texel on the row boundary 2k exactly in exact arithmetic, but
    * the intermediate height buffer_height/4 is non-power-of-two (72 for a
    * 288-tall source), so FP24 resolves vpos.y * VL_BLOCK_HEIGHT/buffer_height
    * just below the boundary whenever the fraction is not exactly representable;
    * NEAREST then fetches row 2k-1, the previous block's high-frequency texel,
    * and the reconstruction loses vertical frequencies 0..3 for that block-row.
    * Measured on RS480: the uncentered pair lands (2k-1, 2k+1) for 32 of 36
    * block-rows of a 288-tall plane; only rows whose fraction is dyadic
    * (k = 0, 9, 18, 27) resolve exactly.  Bias BOTH texels' row lanes to the
    * texel center (+0.5/(buffer_height/4)): floor of a centered coordinate is
    * exact, the 1/size step between the texels keeps the pair (2k, 2k+1), and
    * the bias is inert on hardware whose NEAREST is already exact.  This mirrors
    * the l_center bias the first pass applies to its source reads for the same
    * NPOT reason.  Centering only the second texel leaves the pair (2k-1, 2k+1)
    * and is NOT sufficient; centering only the first regresses against an
    * uncentered first-pass write -- the two passes' biases are co-dependent. */
   nir_def *r_center = nir_imm_vec2(b, 0.0f, 0.5f / (idct->buffer_height / 4.0f));
   r_addr[0] = nir_fadd(b, r_addr[0], r_center);
   r_addr[1] = nir_fadd(b, r_addr[1], r_center);

   nir_def *outs[4] = {
      l_addr[0], l_addr[1],
      nir_vec3(b, nir_channel(b, r_addr[0], 0), nir_channel(b, r_addr[0], 1), tex_z),
      nir_vec3(b, nir_channel(b, r_addr[1], 0), nir_channel(b, r_addr[1], 1), tex_z),
   };
   for (unsigned k = 0; k < 4; ++k) {
      nir_variable *ov = nir_variable_create(b->shader, nir_var_shader_out,
                                             glsl_vec4_type(), "idct_addr_out");
      ov->data.location = VARYING_SLOT_VAR0 + first_output + k;
      nir_store_var(b, ov, idct_pad4(b, outs[k]), 0xf);
   }
}

nir_def *
vl_idct_stage2_frag_shader(struct vl_idct *idct, nir_builder *b,
                           unsigned first_input)
{
   nir_def *l_addr[2], *r_addr[2];
   for (unsigned k = 0; k < 2; ++k) {
      nir_variable *in = nir_variable_create(b->shader, nir_var_shader_in,
                                             glsl_vec4_type(), "idct_l_addr");
      in->data.location = VARYING_SLOT_VAR0 + first_input + k;
      l_addr[k] = nir_trim_vector(b, nir_load_var(b, in), 2);
   }
   for (unsigned k = 0; k < 2; ++k) {
      nir_variable *in = nir_variable_create(b->shader, nir_var_shader_in,
                                             glsl_vec4_type(), "idct_r_addr");
      in->data.location = VARYING_SLOT_VAR0 + first_input + 2 + k;
      r_addr[k] = nir_trim_vector(b, nir_load_var(b, in), 3);
   }

   /* sampler 0 = the first-pass intermediate (3D, sliced by the address z),
    * sampler 1 = the transpose DCT matrix (2D). */
   nir_deref_instr *samp0 =
      idct_combined_sampler(b, GLSL_SAMPLER_DIM_3D, 0, "intermediate");
   nir_deref_instr *samp1 =
      idct_combined_sampler(b, GLSL_SAMPLER_DIM_2D, 1, "transpose");

   nir_def *l[2], *r[2];
   fetch_four(b, l, l_addr, samp1);   /* 2D transpose */
   fetch_four(b, r, r_addr, samp0);   /* 3D intermediate */

   /* The reconstructed residual, broadcast like the original full-vec4 write. */
   return nir_replicate(b, matrix_mul(b, l, r), 4);
}

static bool
init_shaders(struct vl_idct *idct)
{
   idct->vs_mismatch = create_mismatch_vert_shader(idct);
   if (!idct->vs_mismatch)
      goto error_vs_mismatch;

   idct->fs_mismatch = create_mismatch_frag_shader(idct);
   if (!idct->fs_mismatch)
      goto error_fs_mismatch;

   idct->vs = create_stage1_vert_shader(idct);
   if (!idct->vs)
      goto error_vs;

   idct->fs = create_stage1_frag_shader(idct);
   if (!idct->fs)
      goto error_fs;

   return true;

error_fs:
   idct->pipe->delete_vs_state(idct->pipe, idct->vs);

error_vs:
   idct->pipe->delete_vs_state(idct->pipe, idct->vs_mismatch);

error_fs_mismatch:
   idct->pipe->delete_vs_state(idct->pipe, idct->fs);

error_vs_mismatch:
   return false;
}

static void
cleanup_shaders(struct vl_idct *idct)
{
   idct->pipe->delete_vs_state(idct->pipe, idct->vs_mismatch);
   idct->pipe->delete_fs_state(idct->pipe, idct->fs_mismatch);
   idct->pipe->delete_vs_state(idct->pipe, idct->vs);
   idct->pipe->delete_fs_state(idct->pipe, idct->fs);
}

static bool
init_state(struct vl_idct *idct)
{
   struct pipe_blend_state blend;
   struct pipe_rasterizer_state rs_state;
   struct pipe_sampler_state sampler;
   unsigned i;

   assert(idct);

   memset(&rs_state, 0, sizeof(rs_state));
   rs_state.point_size = 1;
   rs_state.half_pixel_center = true;
   rs_state.bottom_edge_rule = true;
   rs_state.depth_clip_near = 1;
   rs_state.depth_clip_far = 1;

   idct->rs_state = idct->pipe->create_rasterizer_state(idct->pipe, &rs_state);
   if (!idct->rs_state)
      goto error_rs_state;

   memset(&blend, 0, sizeof blend);

   blend.independent_blend_enable = 0;
   blend.rt[0].blend_enable = 0;
   blend.rt[0].rgb_func = PIPE_BLEND_ADD;
   blend.rt[0].rgb_src_factor = PIPE_BLENDFACTOR_ONE;
   blend.rt[0].rgb_dst_factor = PIPE_BLENDFACTOR_ONE;
   blend.rt[0].alpha_func = PIPE_BLEND_ADD;
   blend.rt[0].alpha_src_factor = PIPE_BLENDFACTOR_ONE;
   blend.rt[0].alpha_dst_factor = PIPE_BLENDFACTOR_ONE;
   blend.logicop_enable = 0;
   blend.logicop_func = PIPE_LOGICOP_CLEAR;
   /* Needed to allow color writes to FB, even if blending disabled */
   blend.rt[0].colormask = PIPE_MASK_RGBA;
   blend.dither = 0;
   idct->blend = idct->pipe->create_blend_state(idct->pipe, &blend);
   if (!idct->blend)
      goto error_blend;

   for (i = 0; i < 2; ++i) {
      memset(&sampler, 0, sizeof(sampler));
      sampler.wrap_s = PIPE_TEX_WRAP_REPEAT;
      sampler.wrap_t = PIPE_TEX_WRAP_REPEAT;
      sampler.wrap_r = PIPE_TEX_WRAP_REPEAT;
      sampler.min_img_filter = PIPE_TEX_FILTER_NEAREST;
      sampler.min_mip_filter = PIPE_TEX_MIPFILTER_NONE;
      sampler.mag_img_filter = PIPE_TEX_FILTER_NEAREST;
      sampler.compare_mode = PIPE_TEX_COMPARE_NONE;
      sampler.compare_func = PIPE_FUNC_ALWAYS;
      /* mesa-26 inverted the sense to unnormalized_coords; the memset leaves it
       * 0, i.e. normalized coordinates, as this path requires. */
      idct->samplers[i] = idct->pipe->create_sampler_state(idct->pipe, &sampler);
      if (!idct->samplers[i])
         goto error_samplers;
   }

   return true;

error_samplers:
   for (i = 0; i < 2; ++i)
      if (idct->samplers[i])
         idct->pipe->delete_sampler_state(idct->pipe, idct->samplers[i]);

   idct->pipe->delete_rasterizer_state(idct->pipe, idct->rs_state);

error_blend:
   idct->pipe->delete_blend_state(idct->pipe, idct->blend);

error_rs_state:
   return false;
}

static void
cleanup_state(struct vl_idct *idct)
{
   unsigned i;

   for (i = 0; i < 2; ++i)
      idct->pipe->delete_sampler_state(idct->pipe, idct->samplers[i]);

   idct->pipe->delete_rasterizer_state(idct->pipe, idct->rs_state);
   idct->pipe->delete_blend_state(idct->pipe, idct->blend);
}

static bool
init_source(struct vl_idct *idct, struct vl_idct_buffer *buffer)
{
   struct pipe_resource *tex;

   assert(idct && buffer);

   tex = buffer->sampler_views.individual.source->texture;

   buffer->fb_state_mismatch.width = tex->width0;
   buffer->fb_state_mismatch.height = tex->height0;
   buffer->fb_state_mismatch.nr_cbufs = 1;

   /* mesa-26 framebuffer state embeds pipe_surface by value; build the single
    * mismatch-pass target in place (level 0, layer 0). */
   pipe_surface_init(idct->pipe, &buffer->fb_state_mismatch.cbufs[0], tex, 0, 0);

   buffer->viewport_mismatch.scale[0] = tex->width0;
   buffer->viewport_mismatch.scale[1] = tex->height0;
   buffer->viewport_mismatch.scale[2] = 1;
   buffer->viewport_mismatch.swizzle_x = PIPE_VIEWPORT_SWIZZLE_POSITIVE_X;
   buffer->viewport_mismatch.swizzle_y = PIPE_VIEWPORT_SWIZZLE_POSITIVE_Y;
   buffer->viewport_mismatch.swizzle_z = PIPE_VIEWPORT_SWIZZLE_POSITIVE_Z;
   buffer->viewport_mismatch.swizzle_w = PIPE_VIEWPORT_SWIZZLE_POSITIVE_W;

   return true;
}

static void
cleanup_source(struct vl_idct_buffer *buffer)
{
   assert(buffer);

   pipe_resource_reference(&buffer->fb_state_mismatch.cbufs[0].texture, NULL);

   pipe_sampler_view_reference(&buffer->sampler_views.individual.source, NULL);
}

static bool
init_intermediate(struct vl_idct *idct, struct vl_idct_buffer *buffer)
{
   struct pipe_resource *tex;
   unsigned i;

   assert(idct && buffer);

   tex = buffer->sampler_views.individual.intermediate->texture;

   buffer->fb_state.width = tex->width0;
   buffer->fb_state.height = tex->height0;
   buffer->fb_state.nr_cbufs = idct->nr_of_render_targets;
   /* One value-embedded surface per render target, each a single layer of the
    * intermediate (the first IDCT pass scatters the columns across layers). */
   for(i = 0; i < idct->nr_of_render_targets; ++i)
      pipe_surface_init(idct->pipe, &buffer->fb_state.cbufs[i], tex, 0, i);

   buffer->viewport.scale[0] = tex->width0;
   buffer->viewport.scale[1] = tex->height0;
   buffer->viewport.scale[2] = 1;
   buffer->viewport.swizzle_x = PIPE_VIEWPORT_SWIZZLE_POSITIVE_X;
   buffer->viewport.swizzle_y = PIPE_VIEWPORT_SWIZZLE_POSITIVE_Y;
   buffer->viewport.swizzle_z = PIPE_VIEWPORT_SWIZZLE_POSITIVE_Z;
   buffer->viewport.swizzle_w = PIPE_VIEWPORT_SWIZZLE_POSITIVE_W;

   return true;
}

static void
cleanup_intermediate(struct vl_idct_buffer *buffer)
{
   unsigned i;

   assert(buffer);

   for(i = 0; i < PIPE_MAX_COLOR_BUFS; ++i)
      pipe_resource_reference(&buffer->fb_state.cbufs[i].texture, NULL);

   pipe_sampler_view_reference(&buffer->sampler_views.individual.intermediate, NULL);
}

struct pipe_sampler_view *
vl_idct_upload_matrix(struct pipe_context *pipe, float scale)
{
   struct pipe_resource tex_templ, *matrix;
   struct pipe_sampler_view sv_templ, *sv;
   struct pipe_transfer *buf_transfer;
   unsigned i, j, pitch;
   float *f;

   /* pipe_box is laid out {x, width, y, height, z, depth}; name the fields so
    * VL_BLOCK_HEIGHT does not land in z and force an out-of-bounds layer. */
   struct pipe_box rect =
   {
      .x = 0, .y = 0, .z = 0,
      .width = VL_BLOCK_WIDTH / 4,
      .height = VL_BLOCK_HEIGHT,
      .depth = 1,
   };

   assert(pipe);

   memset(&tex_templ, 0, sizeof(tex_templ));
   tex_templ.target = PIPE_TEXTURE_2D;
   tex_templ.format = PIPE_FORMAT_R32G32B32A32_FLOAT;
   tex_templ.last_level = 0;
   tex_templ.width0 = 2;
   tex_templ.height0 = 8;
   tex_templ.depth0 = 1;
   tex_templ.array_size = 1;
   tex_templ.usage = PIPE_USAGE_IMMUTABLE;
   tex_templ.bind = PIPE_BIND_SAMPLER_VIEW;
   tex_templ.flags = 0;

   matrix = pipe->screen->resource_create(pipe->screen, &tex_templ);
   if (!matrix)
      goto error_matrix;

   f = pipe->texture_map(pipe, matrix, 0,
                                     PIPE_MAP_WRITE |
                                     PIPE_MAP_DISCARD_RANGE,
                                     &rect, &buf_transfer);
   if (!f)
      goto error_map;

   pitch = buf_transfer->stride / sizeof(float);

   for(i = 0; i < VL_BLOCK_HEIGHT; ++i)
      for(j = 0; j < VL_BLOCK_WIDTH; ++j)
         // transpose and scale
         f[i * pitch + j] = ((const float (*)[8])const_matrix)[j][i] * scale;

   pipe->texture_unmap(pipe, buf_transfer);

   memset(&sv_templ, 0, sizeof(sv_templ));
   u_sampler_view_default_template(&sv_templ, matrix, matrix->format);
   sv = pipe->create_sampler_view(pipe, matrix, &sv_templ);
   pipe_resource_reference(&matrix, NULL);
   if (!sv)
      goto error_map;

   return sv;

error_map:
   pipe_resource_reference(&matrix, NULL);

error_matrix:
   return NULL;
}

bool vl_idct_init(struct vl_idct *idct, struct pipe_context *pipe,
                  unsigned buffer_width, unsigned buffer_height,
                  unsigned nr_of_render_targets,
                  struct pipe_sampler_view *matrix,
                  struct pipe_sampler_view *transpose)
{
   assert(idct && pipe);
   assert(matrix && transpose);

   idct->pipe = pipe;
   idct->buffer_width = buffer_width;
   idct->buffer_height = buffer_height;
   idct->nr_of_render_targets = nr_of_render_targets;

   pipe_sampler_view_reference(&idct->matrix, matrix);
   pipe_sampler_view_reference(&idct->transpose, transpose);

   if(!init_shaders(idct))
      return false;

   if(!init_state(idct)) {
      cleanup_shaders(idct);
      return false;
   }

   return true;
}

void
vl_idct_cleanup(struct vl_idct *idct)
{
   cleanup_shaders(idct);
   cleanup_state(idct);

   pipe_sampler_view_reference(&idct->matrix, NULL);
   pipe_sampler_view_reference(&idct->transpose, NULL);
}

bool
vl_idct_init_buffer(struct vl_idct *idct, struct vl_idct_buffer *buffer,
                    struct pipe_sampler_view *source,
                    struct pipe_sampler_view *intermediate)
{
   assert(buffer && idct);
   assert(source && intermediate);

   memset(buffer, 0, sizeof(struct vl_idct_buffer));

   pipe_sampler_view_reference(&buffer->sampler_views.individual.matrix, idct->matrix);
   pipe_sampler_view_reference(&buffer->sampler_views.individual.source, source);
   pipe_sampler_view_reference(&buffer->sampler_views.individual.transpose, idct->transpose);
   pipe_sampler_view_reference(&buffer->sampler_views.individual.intermediate, intermediate);

   if (!init_source(idct, buffer))
      return false;

   if (!init_intermediate(idct, buffer))
      return false;

   return true;
}

void
vl_idct_cleanup_buffer(struct vl_idct_buffer *buffer)
{
   assert(buffer);

   cleanup_source(buffer);
   cleanup_intermediate(buffer);

   pipe_sampler_view_reference(&buffer->sampler_views.individual.matrix, NULL);
   pipe_sampler_view_reference(&buffer->sampler_views.individual.transpose, NULL);
}

void
vl_idct_flush(struct vl_idct *idct, struct vl_idct_buffer *buffer, unsigned num_instances)
{
   assert(buffer);

   idct->pipe->bind_rasterizer_state(idct->pipe, idct->rs_state);
   idct->pipe->bind_blend_state(idct->pipe, idct->blend);

   idct->pipe->bind_sampler_states(idct->pipe, MESA_SHADER_FRAGMENT,
                                   0, 2, idct->samplers);

   idct->pipe->set_sampler_views(idct->pipe, MESA_SHADER_FRAGMENT, 0, 2, 0,
                                 buffer->sampler_views.stage[0]);

   /* mismatch control */
   idct->pipe->set_framebuffer_state(idct->pipe, &buffer->fb_state_mismatch);
   idct->pipe->set_viewport_states(idct->pipe, 0, 1, &buffer->viewport_mismatch);
   idct->pipe->bind_vs_state(idct->pipe, idct->vs_mismatch);
   idct->pipe->bind_fs_state(idct->pipe, idct->fs_mismatch);
   util_draw_arrays_instanced(idct->pipe, MESA_PRIM_POINTS, 0, 1, 0, num_instances);

   /* first stage */
   idct->pipe->set_framebuffer_state(idct->pipe, &buffer->fb_state);
   idct->pipe->set_viewport_states(idct->pipe, 0, 1, &buffer->viewport);
   idct->pipe->bind_vs_state(idct->pipe, idct->vs);
   idct->pipe->bind_fs_state(idct->pipe, idct->fs);
   util_draw_arrays_instanced(idct->pipe, MESA_PRIM_QUADS, 0, 4, 0, num_instances);
}

void
vl_idct_prepare_stage2(struct vl_idct *idct, struct vl_idct_buffer *buffer)
{
   assert(buffer);

   /* second stage */
   idct->pipe->bind_rasterizer_state(idct->pipe, idct->rs_state);
   idct->pipe->bind_sampler_states(idct->pipe, MESA_SHADER_FRAGMENT,
                                   0, 2, idct->samplers);
   idct->pipe->set_sampler_views(idct->pipe, MESA_SHADER_FRAGMENT,
                                 0, 2, 0, buffer->sampler_views.stage[1]);
}
