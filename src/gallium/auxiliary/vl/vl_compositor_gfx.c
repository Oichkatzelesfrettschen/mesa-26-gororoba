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

#include "util/compiler.h"
#include "pipe/p_context.h"

#include "util/u_memory.h"
#include "util/u_draw.h"
#include "util/u_surface.h"
#include "util/u_upload_mgr.h"

#include "vl_nir.h"

#include "vl_csc.h"
#include "vl_types.h"

#include "vl_compositor_gfx.h"

enum VS_OUTPUT
{
   VS_O_VPOS = 0,
   VS_O_COLOR = 0,
   VS_O_VTEX = 0,
   VS_O_VTOP,
   VS_O_VBOTTOM,
};

void *
create_vert_shader(struct vl_compositor *c)
{
   const nir_shader_compiler_options *options =
      c->pipe->screen->nir_options[MESA_SHADER_VERTEX];
   nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_VERTEX, options,
                                                  "vl:compositor_vs");

   nir_variable *in_vpos = nir_variable_create(b.shader, nir_var_shader_in,
                                               glsl_vec4_type(), "vpos");
   in_vpos->data.location = VERT_ATTRIB_GENERIC0;
   nir_variable *in_vtex = nir_variable_create(b.shader, nir_var_shader_in,
                                               glsl_vec4_type(), "vtex");
   in_vtex->data.location = VERT_ATTRIB_GENERIC0 + 1;
   nir_variable *in_color = nir_variable_create(b.shader, nir_var_shader_in,
                                                glsl_vec4_type(), "color");
   in_color->data.location = VERT_ATTRIB_GENERIC0 + 2;

   nir_def *vpos = nir_load_var(&b, in_vpos);
   nir_def *vtex = nir_load_var(&b, in_vtex);
   nir_def *color = nir_load_var(&b, in_color);

   struct {
      const char *name;
      gl_varying_slot slot;
      nir_def *value;
   } outs[5];
   outs[0].name = "o_vpos";   outs[0].slot = VARYING_SLOT_POS;
   outs[1].name = "o_color";  outs[1].slot = VARYING_SLOT_COL0;
   outs[2].name = "o_vtex";   outs[2].slot = VARYING_SLOT_VAR0 + VS_O_VTEX;
   outs[3].name = "o_vtop";   outs[3].slot = VARYING_SLOT_VAR0 + VS_O_VTOP;
   outs[4].name = "o_vbottom"; outs[4].slot = VARYING_SLOT_VAR0 + VS_O_VBOTTOM;

   /* tmp.x = vtex.w / 2; tmp.y = vtex.w / 4.
    * o_vtop    = (vtex.x, vtex.y * tmp.x + 0.25, vtex.y * tmp.y + 0.25, 1/tmp.x)
    * o_vbottom = (vtex.x, vtex.y * tmp.x - 0.25, vtex.y * tmp.y - 0.25, 1/tmp.y) */
   nir_def *vtex_x = nir_channel(&b, vtex, 0);
   nir_def *vtex_y = nir_channel(&b, vtex, 1);
   nir_def *half_w = nir_fmul_imm(&b, nir_channel(&b, vtex, 3), 0.5);
   nir_def *quarter_w = nir_fmul_imm(&b, nir_channel(&b, vtex, 3), 0.25);

   outs[0].value = vpos;
   outs[1].value = color;
   outs[2].value = vtex;
   outs[3].value = nir_vec4(
      &b, vtex_x,
      nir_fadd_imm(&b, nir_fmul(&b, vtex_y, half_w), 0.25),
      nir_fadd_imm(&b, nir_fmul(&b, vtex_y, quarter_w), 0.25),
      nir_frcp(&b, half_w));
   outs[4].value = nir_vec4(
      &b, vtex_x,
      nir_fadd_imm(&b, nir_fmul(&b, vtex_y, half_w), -0.25),
      nir_fadd_imm(&b, nir_fmul(&b, vtex_y, quarter_w), -0.25),
      nir_frcp(&b, quarter_w));

   for (unsigned i = 0; i < 5; i++) {
      nir_variable *out = nir_variable_create(b.shader, nir_var_shader_out,
                                              glsl_vec4_type(), outs[i].name);
      out->data.location = outs[i].slot;
      nir_store_var(&b, out, outs[i].value, 0xf);
   }

   return vl_nir_vs_finish(&b, c->pipe);
}

/* CONST[i] of the fs constant buffer as a vec4: the gfx kernels read the
 * CSC matrix rows the compositor uploads to fs constant slot 0, the same
 * window the compute path reads. */
static nir_def *
frag_shader_const(nir_builder *b, unsigned index)
{
   return nir_load_ubo(b, 4, 32, nir_imm_int(b, 0),
                       nir_imm_int(b, index * 16),
                       .align_mul = 4, .range = ~0);
}

/* Sample plane s at (x, sel) with the field layer for array targets.
 * The weave coordinate vector carries x, the per-plane y in y/z, and the
 * field layer in w. */
static nir_def *
frag_shader_plane_tex(struct vl_nir_fs *fs, unsigned s, nir_def *coord,
                      unsigned sel_chan, bool use_array)
{
   nir_builder *b = &fs->b;
   nir_def *x = nir_channel(b, coord, 0);
   nir_def *sel = nir_channel(b, coord, sel_chan);
   nir_def *c = use_array
      ? nir_vec3(b, x, sel, nir_channel(b, coord, 3))
      : nir_vec2(b, x, sel);
   return vl_nir_tex(fs, s, c);
}

/* Weave the top and bottom field planes back into one frame: snap each
 * field's y to its texel row, fetch the three planes per field, and
 * linearly blend the two fields by the fractional distance of the output
 * row from the field rows. */
static nir_def *
frag_shader_weave(struct vl_nir_fs *fs, bool use_array)
{
   nir_builder *b = &fs->b;
   /* i_tc[0] = VTOP (texcoord[1]), i_tc[1] = VBOTTOM (texcoord[2]). */
   nir_def *i_tc[2] = { fs->texcoord[1], fs->texcoord[2] };
   nir_def *t_tc[2], *t_texel[2];

   /* t_tc.x = i_tc.x; t_tc.yz = (round(i_tc.yz - 0.5) + 0.5) scaled by the
    * per-plane inverse heights in VTOP.w / VBOTTOM.w; t_tc.w = the field
    * layer. */
   for (unsigned i = 0; i < 2; i++) {
      nir_def *yz = nir_fround_even(
         b, nir_fadd_imm(b, nir_trim_vector(b, i_tc[i], 3), -0.5));
      nir_def *y = nir_fmul(b, nir_fadd_imm(b, nir_channel(b, yz, 1), 0.5),
                            nir_channel(b, i_tc[0], 3));
      nir_def *z = nir_fmul(b, nir_fadd_imm(b, nir_channel(b, yz, 2), 0.5),
                            nir_channel(b, i_tc[1], 3));
      t_tc[i] = nir_vec4(b, nir_channel(b, i_tc[i], 0), y, z,
                         nir_imm_float(b, i ? 1.0 : 0.0));
   }

   /* texel[i] channel j comes from plane j fetched at that field's
    * coordinate (luma from y, chroma from z). */
   for (unsigned i = 0; i < 2; i++) {
      nir_def *chan[4];
      for (unsigned j = 0; j < 3; j++) {
         nir_def *t = frag_shader_plane_tex(fs, j, t_tc[i], j ? 2 : 1,
                                            use_array);
         chan[j] = nir_channel(b, t, j);
      }
      chan[3] = nir_imm_float(b, 0.0);
      t_texel[i] = nir_vec4(b, chan[0], chan[1], chan[2], chan[3]);
   }

   /* factor = |round(i_tc.y) - i_tc.y| * 2 per plane, broadcast (y,z,z,z). */
   nir_def *r = nir_fround_even(b, i_tc[0]);
   nir_def *f = nir_fmul_imm(
      b, nir_fabs(b, nir_fadd(b, r, nir_fneg(b, i_tc[0]))), 2.0);
   nir_def *factor = nir_swizzle(b, f, (unsigned[]){1, 2, 2, 2}, 4);

   /* TGSI LRP(s, a, b) = s*a + (1-s)*b. */
   return nir_flrp(b, t_texel[1], t_texel[0], factor);
}

/* Color-space conversion: force texel.w to 1 for the three CSC rows and
 * carry the incoming texel.w through as the fragment alpha. */
static nir_def *
frag_shader_csc(struct vl_nir_fs *fs, nir_def *texel)
{
   nir_builder *b = &fs->b;
   nir_def *csc[3];
   for (unsigned i = 0; i < 3; i++)
      csc[i] = frag_shader_const(b, i);

   nir_def *alpha = nir_channel(b, texel, 3);
   nir_def *t = nir_vector_insert_imm(b, texel, nir_imm_float(b, 1.0), 3);
   return nir_vec4(b, nir_fdot4(b, csc[0], t), nir_fdot4(b, csc[1], t),
                   nir_fdot4(b, csc[2], t), alpha);
}

/* Progressive fetch: one texel per plane at the shared VTEX coordinate,
 * luma/chroma landing in x/y/z and alpha from plane 0's w channel. */
static nir_def *
frag_shader_yuv(struct vl_nir_fs *fs, bool use_array)
{
   nir_builder *b = &fs->b;
   nir_def *tc = fs->texcoord[0];
   nir_def *chan[4];
   for (unsigned j = 0; j < 3; j++) {
      nir_def *c = use_array ? nir_trim_vector(b, tc, 3)
                             : nir_trim_vector(b, tc, 2);
      chan[j] = nir_channel(b, vl_nir_tex(fs, j, c), j);
   }
   nir_def *c0 = use_array ? nir_trim_vector(b, tc, 3)
                           : nir_trim_vector(b, tc, 2);
   chan[3] = nir_channel(b, vl_nir_tex(fs, 0, c0), 3);
   return nir_vec4(b, chan[0], chan[1], chan[2], chan[3]);
}

static bool
compositor_sampler_is_array(struct vl_compositor *c)
{
   return c->pipe->screen->caps.max_texture_array_layers > 1;
}





void *
create_frag_shader_video_buffer(struct vl_compositor *c)
{
   const bool use_array = compositor_sampler_is_array(c);
   struct vl_nir_fs fs;
   vl_nir_fs_begin(&fs, c->pipe, 1, "vl:compositor_video_buffer");
   for (unsigned i = 0; i < 3; i++)
      vl_nir_sampler_array(&fs, i, GLSL_SAMPLER_DIM_2D, use_array);
   nir_def *texel = frag_shader_yuv(&fs, use_array);
   return vl_nir_fs_finish(&fs, c->pipe, frag_shader_csc(&fs, texel));
}

void *
create_frag_shader_weave_rgb(struct vl_compositor *c)
{
   const bool use_array = compositor_sampler_is_array(c);
   struct vl_nir_fs fs;
   vl_nir_fs_begin(&fs, c->pipe, 3, "vl:compositor_weave_rgb");
   for (unsigned i = 0; i < 3; i++)
      vl_nir_sampler_array(&fs, i, GLSL_SAMPLER_DIM_2D, use_array);
   nir_def *texel = frag_shader_weave(&fs, use_array);
   return vl_nir_fs_finish(&fs, c->pipe, frag_shader_csc(&fs, texel));
}

void *
create_frag_shader_deint_yuv(struct vl_compositor *c, bool y, bool w)
{
   const bool use_array = compositor_sampler_is_array(c);
   struct vl_nir_fs fs;
   vl_nir_fs_begin(&fs, c->pipe, w ? 3 : 1, "vl:compositor_deint_yuv");
   for (unsigned i = 0; i < 3; i++)
      vl_nir_sampler_array(&fs, i, GLSL_SAMPLER_DIM_2D, use_array);
   nir_def *texel = w ? frag_shader_weave(&fs, use_array)
                      : frag_shader_yuv(&fs, use_array);
   nir_builder *b = &fs.b;
   nir_def *color = y
      ? texel
      : nir_swizzle(b, texel, (unsigned[]){1, 2, 3, 3}, 4);
   return vl_nir_fs_finish(&fs, c->pipe, color);
}

void *
create_frag_shader_palette(struct vl_compositor *c, bool include_cc)
{
   struct vl_nir_fs fs;
   vl_nir_fs_begin(&fs, c->pipe, 1, "vl:compositor_palette");
   vl_nir_sampler(&fs, 0, GLSL_SAMPLER_DIM_2D);
   vl_nir_sampler(&fs, 1, GLSL_SAMPLER_DIM_1D);
   nir_builder *b = &fs.b;

   /* texel = tex(tc, indices); fragment.rgb = tex(texel.x, palette),
    * through the CSC rows when the palette holds YUV; fragment.a =
    * texel.a. */
   nir_def *texel =
      vl_nir_tex(&fs, 0, nir_trim_vector(b, fs.texcoord[0], 2));
   nir_def *pal = vl_nir_tex(&fs, 1, nir_channel(b, texel, 0));
   nir_def *rgb[3];
   if (include_cc) {
      for (unsigned i = 0; i < 3; i++)
         rgb[i] = nir_fdot4(b, frag_shader_const(b, i), pal);
   } else {
      for (unsigned i = 0; i < 3; i++)
         rgb[i] = nir_channel(b, pal, i);
   }
   nir_def *color =
      nir_vec4(b, rgb[0], rgb[1], rgb[2], nir_channel(b, texel, 3));
   return vl_nir_fs_finish(&fs, c->pipe, color);
}

void *
create_frag_shader_rgba(struct vl_compositor *c)
{
   struct vl_nir_fs fs;
   vl_nir_fs_begin(&fs, c->pipe, 1, "vl:compositor_rgba");
   vl_nir_sampler(&fs, 0, GLSL_SAMPLER_DIM_2D);
   nir_builder *b = &fs.b;

   nir_variable *in_color = nir_variable_create(
      b->shader, nir_var_shader_in, glsl_vec4_type(), "v_color");
   in_color->data.location = VARYING_SLOT_COL0;
   nir_def *color = nir_load_var(b, in_color);

   nir_def *texel =
      vl_nir_tex(&fs, 0, nir_trim_vector(b, fs.texcoord[0], 2));
   return vl_nir_fs_finish(&fs, c->pipe, nir_fmul(b, texel, color));
}

void *
create_frag_shader_rgb_yuv(struct vl_compositor *c, bool y)
{
   struct vl_nir_fs fs;
   vl_nir_fs_begin(&fs, c->pipe, 1, "vl:compositor_rgb_yuv");
   vl_nir_sampler(&fs, 0, GLSL_SAMPLER_DIM_2D);
   nir_builder *b = &fs.b;

   nir_def *texel =
      vl_nir_tex(&fs, 0, nir_trim_vector(b, fs.texcoord[0], 2));
   nir_def *color;
   if (y) {
      nir_def *l = nir_fdot4(b, frag_shader_const(b, 0), texel);
      color = nir_swizzle(b, l, (unsigned[]){0, 0, 0, 0}, 4);
   } else {
      nir_def *u = nir_fdot4(b, frag_shader_const(b, 1), texel);
      nir_def *v = nir_fdot4(b, frag_shader_const(b, 2), texel);
      color = nir_vec4(b, u, v, v, v);
   }
   return vl_nir_fs_finish(&fs, c->pipe, color);
}

static void
gen_rect_verts(struct vertex2f *vb, struct vl_compositor_layer *layer)
{
   struct vertex2f tl, tr, br, bl;

   assert(vb && layer);

   switch (layer->rotate) {
   default:
   case VL_COMPOSITOR_ROTATE_0:
      tl = layer->dst.tl;
      tr.x = layer->dst.br.x;
      tr.y = layer->dst.tl.y;
      br = layer->dst.br;
      bl.x = layer->dst.tl.x;
      bl.y = layer->dst.br.y;
      break;
   case VL_COMPOSITOR_ROTATE_90:
      tl.x = layer->dst.br.x;
      tl.y = layer->dst.tl.y;
      tr = layer->dst.br;
      br.x = layer->dst.tl.x;
      br.y = layer->dst.br.y;
      bl = layer->dst.tl;
      break;
   case VL_COMPOSITOR_ROTATE_180:
      tl = layer->dst.br;
      tr.x = layer->dst.tl.x;
      tr.y = layer->dst.br.y;
      br = layer->dst.tl;
      bl.x = layer->dst.br.x;
      bl.y = layer->dst.tl.y;
      break;
   case VL_COMPOSITOR_ROTATE_270:
      tl.x = layer->dst.tl.x;
      tl.y = layer->dst.br.y;
      tr = layer->dst.tl;
      br.x = layer->dst.br.x;
      br.y = layer->dst.tl.y;
      bl = layer->dst.br;
      break;
   }

   vb[ 0].x = tl.x;
   vb[ 0].y = tl.y;
   vb[ 1].x = layer->src.tl.x;
   vb[ 1].y = layer->src.tl.y;
   vb[ 2] = layer->zw;
   vb[ 3].x = layer->colors[0].x;
   vb[ 3].y = layer->colors[0].y;
   vb[ 4].x = layer->colors[0].z;
   vb[ 4].y = layer->colors[0].w;

   vb[ 5].x = tr.x;
   vb[ 5].y = tr.y;
   vb[ 6].x = layer->src.br.x;
   vb[ 6].y = layer->src.tl.y;
   vb[ 7] = layer->zw;
   vb[ 8].x = layer->colors[1].x;
   vb[ 8].y = layer->colors[1].y;
   vb[ 9].x = layer->colors[1].z;
   vb[ 9].y = layer->colors[1].w;

   vb[10].x = br.x;
   vb[10].y = br.y;
   vb[11].x = layer->src.br.x;
   vb[11].y = layer->src.br.y;
   vb[12] = layer->zw;
   vb[13].x = layer->colors[2].x;
   vb[13].y = layer->colors[2].y;
   vb[14].x = layer->colors[2].z;
   vb[14].y = layer->colors[2].w;

   vb[15].x = bl.x;
   vb[15].y = bl.y;
   vb[16].x = layer->src.tl.x;
   vb[16].y = layer->src.br.y;
   vb[17] = layer->zw;
   vb[18].x = layer->colors[3].x;
   vb[18].y = layer->colors[3].y;
   vb[19].x = layer->colors[3].z;
   vb[19].y = layer->colors[3].w;
}

static inline struct u_rect
calc_drawn_area(struct vl_compositor_state *s, struct vl_compositor_layer *layer)
{
   struct vertex2f tl, br;
   struct u_rect result;

   assert(s && layer);

   // rotate
   switch (layer->rotate) {
   default:
   case VL_COMPOSITOR_ROTATE_0:
      tl = layer->dst.tl;
      br = layer->dst.br;
      break;
   case VL_COMPOSITOR_ROTATE_90:
      tl.x = layer->dst.br.x;
      tl.y = layer->dst.tl.y;
      br.x = layer->dst.tl.x;
      br.y = layer->dst.br.y;
      break;
   case VL_COMPOSITOR_ROTATE_180:
      tl = layer->dst.br;
      br = layer->dst.tl;
      break;
   case VL_COMPOSITOR_ROTATE_270:
      tl.x = layer->dst.tl.x;
      tl.y = layer->dst.br.y;
      br.x = layer->dst.br.x;
      br.y = layer->dst.tl.y;
      break;
   }

   // scale
   result.x0 = tl.x * layer->viewport.scale[0] + layer->viewport.translate[0];
   result.y0 = tl.y * layer->viewport.scale[1] + layer->viewport.translate[1];
   result.x1 = br.x * layer->viewport.scale[0] + layer->viewport.translate[0];
   result.y1 = br.y * layer->viewport.scale[1] + layer->viewport.translate[1];

   // and clip
   result.x0 = MAX2(result.x0, s->scissor.minx);
   result.y0 = MAX2(result.y0, s->scissor.miny);
   result.x1 = MIN2(result.x1, s->scissor.maxx);
   result.y1 = MIN2(result.y1, s->scissor.maxy);
   return result;
}

static void
gen_vertex_data(struct vl_compositor *c, struct vl_compositor_state *s, struct u_rect *dirty, struct pipe_resource **releasebuf)
{
   struct vertex2f *vb;
   unsigned i;

   assert(c);

   /* Allocate new memory for vertices. */
   u_upload_alloc(c->pipe->stream_uploader, 0,
                  VL_COMPOSITOR_VB_STRIDE * VL_COMPOSITOR_MAX_LAYERS * 4, /* size */
                  4, /* alignment */
                  &c->vertex_buf.buffer_offset, &c->vertex_buf.buffer.resource,
                  releasebuf,
                  (void **)&vb);

   for (i = 0; i < VL_COMPOSITOR_MAX_LAYERS; i++) {
      if (s->used_layers & (1 << i)) {
         struct vl_compositor_layer *layer = &s->layers[i];
         gen_rect_verts(vb, layer);
         vb += 20;

         if (!layer->viewport_valid) {
            layer->viewport.scale[0] = c->fb_state.width;
            layer->viewport.scale[1] = c->fb_state.height;
            layer->viewport.translate[0] = 0;
            layer->viewport.translate[1] = 0;
         }

         if (dirty && layer->clearing) {
            struct u_rect drawn = calc_drawn_area(s, layer);
            if (
             dirty->x0 >= drawn.x0 &&
             dirty->y0 >= drawn.y0 &&
             dirty->x1 <= drawn.x1 &&
             dirty->y1 <= drawn.y1) {

               // We clear the dirty area anyway, no need for clear_render_target
               dirty->x0 = dirty->y0 = VL_COMPOSITOR_MAX_DIRTY;
               dirty->x1 = dirty->y1 = VL_COMPOSITOR_MIN_DIRTY;
            }
         }
      }
   }

   u_upload_unmap(c->pipe->stream_uploader);
}

static void
set_csc_matrix(struct vl_compositor_state *s)
{
   struct pipe_transfer *buf_transfer;

   float *ptr = pipe_buffer_map(s->pipe, s->shader_params,
                                PIPE_MAP_WRITE | PIPE_MAP_DISCARD_WHOLE_RESOURCE,
                                &buf_transfer);

   if (!ptr)
     return;

   memcpy(ptr, &s->csc_matrix, sizeof(vl_csc_matrix));

   ptr += sizeof(vl_csc_matrix) / sizeof(float);
   *ptr++ = 0.0f; /* luma_min */
   *ptr++ = 1.0f; /* luma_max */

   pipe_buffer_unmap(s->pipe, buf_transfer);
}

static void
draw_layers(struct vl_compositor *c, struct vl_compositor_state *s, struct u_rect *dirty)
{
   unsigned vb_index, i;

   assert(c);

   for (i = 0, vb_index = 0; i < VL_COMPOSITOR_MAX_LAYERS; ++i) {
      if (s->used_layers & (1 << i)) {
         struct vl_compositor_layer *layer = &s->layers[i];
         struct pipe_sampler_view **samplers = &layer->sampler_views[0];
         unsigned num_sampler_views = !samplers[1] ? 1 : !samplers[2] ? 2 : 3;
         void *blend = layer->blend_enabled ? c->blend_add : c->blend_clear;

         c->pipe->bind_blend_state(c->pipe, blend);
         c->pipe->set_viewport_states(c->pipe, 0, 1, &layer->viewport);
         c->pipe->bind_fs_state(c->pipe, layer->fs);
         c->pipe->bind_sampler_states(c->pipe, MESA_SHADER_FRAGMENT, 0,
                                      num_sampler_views, layer->samplers);
         c->pipe->set_sampler_views(c->pipe, MESA_SHADER_FRAGMENT, 0,
                                    num_sampler_views, 0, samplers);

         util_draw_arrays(c->pipe, MESA_PRIM_QUADS, vb_index * 4, 4);
         vb_index++;

         if (dirty) {
            // Remember the currently drawn area as dirty for the next draw command
            struct u_rect drawn = calc_drawn_area(s, layer);
            dirty->x0 = MIN2(drawn.x0, dirty->x0);
            dirty->y0 = MIN2(drawn.y0, dirty->y0);
            dirty->x1 = MAX2(drawn.x1, dirty->x1);
            dirty->y1 = MAX2(drawn.y1, dirty->y1);
         }
      }
   }
}

void
vl_compositor_gfx_render(struct vl_compositor_state *s,
                     struct vl_compositor           *c,
                     struct pipe_surface            *dst_surface,
                     struct u_rect                  *dirty_area,
                     bool                            clear_dirty)
{
   assert(c);
   assert(dst_surface);

   pipe_surface_size(dst_surface, &c->fb_state.width, &c->fb_state.height);
   c->fb_state.cbufs[0] = *dst_surface;

   if (!s->scissor_valid) {
      s->scissor.minx = 0;
      s->scissor.miny = 0;
      s->scissor.maxx = c->fb_state.width;
      s->scissor.maxy = c->fb_state.height;
   }
   c->pipe->set_scissor_states(c->pipe, 0, 1, &s->scissor);

   struct pipe_resource *releasebuf = NULL;
   gen_vertex_data(c, s, dirty_area, &releasebuf);
   set_csc_matrix(s);

   if (clear_dirty && dirty_area &&
       (dirty_area->x0 < dirty_area->x1 || dirty_area->y0 < dirty_area->y1)) {

      c->pipe->clear_render_target(c->pipe, dst_surface, &s->clear_color,
                                   0, 0, c->fb_state.width, c->fb_state.height, false);
      dirty_area->x0 = dirty_area->y0 = VL_COMPOSITOR_MAX_DIRTY;
      dirty_area->x1 = dirty_area->y1 = VL_COMPOSITOR_MIN_DIRTY;
   }

   c->pipe->set_framebuffer_state(c->pipe, &c->fb_state);
   c->pipe->bind_vs_state(c->pipe, c->vs);
   c->pipe->bind_vertex_elements_state(c->pipe, c->vertex_elems_state);
   c->pipe->set_vertex_buffers(c->pipe, 1, &c->vertex_buf);
   pipe_set_constant_buffer(c->pipe, MESA_SHADER_FRAGMENT, 0, s->shader_params);
   c->pipe->bind_rasterizer_state(c->pipe, c->rast);

   draw_layers(c, s, dirty_area);
   pipe_resource_release(c->pipe, releasebuf);
}
