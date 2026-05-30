/**************************************************************************
 *
 * Copyright 2011 Christian König
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

#include "pipe/p_screen.h"
#include "pipe/p_context.h"

#include "util/u_draw.h"
#include "util/u_sampler.h"
#include "util/u_inlines.h"
#include "util/u_memory.h"
#include "util/vl_zscan_data.h"

#include "vl_defines.h"
#include "vl_types.h"

#include "vl_nir.h"
#include "vl_zscan.h"
#include "vl_vertex_buffers.h"

enum VS_OUTPUT
{
   VS_O_VPOS = 0,
   VS_O_VTEX = 0
};

/* The zigzag/alternate/H.265 scan-order tables moved to src/util/vl_zscan_data.c
 * in mesa-26 (shared with the surviving vl frontend); they are declared in
 * util/vl_zscan_data.h and linked from libmesa_util.  Defining them here too
 * would clash at link (duplicate symbol). */


static void *
create_vert_shader(struct vl_zscan *zscan)
{
   const nir_shader_compiler_options *options =
      zscan->pipe->screen->nir_options[MESA_SHADER_VERTEX];
   nir_builder b =
      nir_builder_init_simple_shader(MESA_SHADER_VERTEX, options, "vl:zscan_vs");
   unsigned i;

   /* vrect is the quad corner, vpos the block position, block_num the linear
    * block index; NIR loads expand each vertex input to vec4. */
   nir_variable *iv_rect = nir_variable_create(b.shader, nir_var_shader_in,
                                              glsl_vec4_type(), "vrect");
   iv_rect->data.location = VERT_ATTRIB_GENERIC0 + VS_I_RECT;
   nir_variable *iv_pos = nir_variable_create(b.shader, nir_var_shader_in,
                                             glsl_vec4_type(), "vpos");
   iv_pos->data.location = VERT_ATTRIB_GENERIC0 + VS_I_VPOS;
   nir_variable *iv_block = nir_variable_create(b.shader, nir_var_shader_in,
                                               glsl_vec4_type(), "block_num");
   iv_block->data.location = VERT_ATTRIB_GENERIC0 + VS_I_BLOCK_NUM;

   nir_def *vrect = nir_load_var(&b, iv_rect);
   nir_def *vpos = nir_load_var(&b, iv_pos);
   nir_def *block_num = nir_channel(&b, nir_load_var(&b, iv_block), 0);

   /* o_vpos.xy = (vpos + vrect) * scale; zw = 1. */
   nir_def *scale = nir_imm_vec2(&b,
      (float)VL_BLOCK_WIDTH / zscan->buffer_width,
      (float)VL_BLOCK_HEIGHT / zscan->buffer_height);
   nir_def *vp_xy = nir_fmul(&b,
      nir_fadd(&b, nir_trim_vector(&b, vpos, 2), nir_trim_vector(&b, vrect, 2)),
      scale);
   nir_variable *ov_pos = nir_variable_create(b.shader, nir_var_shader_out,
                                             glsl_vec4_type(), "pos_out");
   ov_pos->data.location = VARYING_SLOT_POS;
   nir_store_var(&b, ov_pos,
      nir_vec4(&b, nir_channel(&b, vp_xy, 0), nir_channel(&b, vp_xy, 1),
               nir_imm_float(&b, 1.0f), nir_imm_float(&b, 1.0f)), 0xf);

   /* block_num / blocks_per_line splits into the block column (frac) and row
    * (floor); the per-plane texcoord addresses the scan/coefficient planes. */
   nir_def *bn =
      nir_fmul(&b, block_num, nir_imm_float(&b, 1.0f / zscan->blocks_per_line));
   nir_def *frac_bn = nir_ffract(&b, bn);
   nir_def *floor_bn = nir_ffloor(&b, bn);

   for (i = 0; i < zscan->num_channels; ++i) {
      float chan_off = 1.0f / (zscan->blocks_per_line * VL_BLOCK_WIDTH)
         * ((signed)i - (signed)zscan->num_channels / 2);
      nir_def *tx = nir_fadd(&b, frac_bn, nir_imm_float(&b, chan_off));

      nir_def *ox = nir_fmad(&b, nir_channel(&b, vrect, 0),
         nir_imm_float(&b, 1.0f / zscan->blocks_per_line), tx);
      nir_def *oy = nir_channel(&b, vrect, 1);
      nir_def *oz = nir_channel(&b, vpos, 2);
      nir_def *ow = nir_fmul(&b, floor_bn,
         nir_imm_float(&b, (float)zscan->blocks_per_line / zscan->blocks_total));

      nir_variable *ov_tex = nir_variable_create(b.shader, nir_var_shader_out,
                                                glsl_vec4_type(), "tex_out");
      ov_tex->data.location = VARYING_SLOT_VAR0 + i;
      nir_store_var(&b, ov_tex, nir_vec4(&b, ox, oy, oz, ow), 0xf);
   }

   return vl_nir_vs_finish(&b, zscan->pipe);
}

static void *
create_frag_shader(struct vl_zscan *zscan)
{
   struct vl_nir_fs fs;
   nir_def *coeff[VL_NIR_MAX_TC], *quant[VL_NIR_MAX_TC];
   unsigned i;

   vl_nir_fs_begin(&fs, zscan->pipe, zscan->num_channels, "vl:zscan_fs");
   vl_nir_sampler(&fs, 0, GLSL_SAMPLER_DIM_2D);   /* coefficient plane */
   vl_nir_sampler(&fs, 1, GLSL_SAMPLER_DIM_2D);   /* zigzag scan plane */
   vl_nir_sampler(&fs, 2, GLSL_SAMPLER_DIM_3D);   /* dequant matrix */
   nir_builder *b = &fs.b;

   /* Per plane: the scan texture maps the raster column to the zigzag source
    * column; fetch the coefficient there and multiply by the dequant matrix
    * value (3D) scaled by 16.  Inverse scan + inverse quant -- the monograph
    * gather + MAC -- packed one plane per output channel. */
   for (i = 0; i < zscan->num_channels; ++i) {
      nir_def *vt = fs.texcoord[i];
      nir_def *scan =
         nir_channel(b, vl_nir_tex(&fs, 1, nir_trim_vector(b, vt, 2)), 0);
      nir_def *src_coord = nir_vec2(b, scan, nir_channel(b, vt, 3));
      coeff[i] = nir_channel(b, vl_nir_tex(&fs, 0, src_coord), 0);
      nir_def *q =
         nir_channel(b, vl_nir_tex(&fs, 2, nir_trim_vector(b, vt, 3)), 0);
      quant[i] = nir_fmul(b, q, nir_imm_float(b, 16.0f));
   }

   nir_def *chan[4];
   for (i = 0; i < 4; ++i)
      chan[i] = (i < zscan->num_channels)
         ? nir_fmul(b, coeff[i], quant[i])
         : nir_imm_float(b, 0.0f);

   return vl_nir_fs_finish(&fs, zscan->pipe,
      nir_vec4(b, chan[0], chan[1], chan[2], chan[3]));
}

static bool
init_shaders(struct vl_zscan *zscan)
{
   assert(zscan);

   zscan->vs = create_vert_shader(zscan);
   if (!zscan->vs)
      goto error_vs;

   zscan->fs = create_frag_shader(zscan);
   if (!zscan->fs)
      goto error_fs;

   return true;

error_fs:
   zscan->pipe->delete_vs_state(zscan->pipe, zscan->vs);

error_vs:
   return false;
}

static void
cleanup_shaders(struct vl_zscan *zscan)
{
   assert(zscan);

   zscan->pipe->delete_vs_state(zscan->pipe, zscan->vs);
   zscan->pipe->delete_fs_state(zscan->pipe, zscan->fs);
}

static bool
init_state(struct vl_zscan *zscan)
{
   struct pipe_blend_state blend;
   struct pipe_rasterizer_state rs_state;
   struct pipe_sampler_state sampler;
   unsigned i;

   assert(zscan);

   memset(&rs_state, 0, sizeof(rs_state));
   rs_state.half_pixel_center = true;
   rs_state.bottom_edge_rule = true;
   rs_state.depth_clip_near = 1;
   rs_state.depth_clip_far = 1;

   zscan->rs_state = zscan->pipe->create_rasterizer_state(zscan->pipe, &rs_state);
   if (!zscan->rs_state)
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
   zscan->blend = zscan->pipe->create_blend_state(zscan->pipe, &blend);
   if (!zscan->blend)
      goto error_blend;

   for (i = 0; i < 3; ++i) {
      memset(&sampler, 0, sizeof(sampler));
      sampler.wrap_s = PIPE_TEX_WRAP_REPEAT;
      sampler.wrap_t = PIPE_TEX_WRAP_REPEAT;
      sampler.wrap_r = PIPE_TEX_WRAP_CLAMP_TO_EDGE;
      sampler.min_img_filter = PIPE_TEX_FILTER_NEAREST;
      sampler.min_mip_filter = PIPE_TEX_MIPFILTER_NONE;
      sampler.mag_img_filter = PIPE_TEX_FILTER_NEAREST;
      sampler.compare_mode = PIPE_TEX_COMPARE_NONE;
      sampler.compare_func = PIPE_FUNC_ALWAYS;
      /* mesa-26: unnormalized_coords (inverted); memset leaves it 0 = normalized. */
      zscan->samplers[i] = zscan->pipe->create_sampler_state(zscan->pipe, &sampler);
      if (!zscan->samplers[i])
         goto error_samplers;
   }

   return true;

error_samplers:
   for (i = 0; i < 2; ++i)
      if (zscan->samplers[i])
         zscan->pipe->delete_sampler_state(zscan->pipe, zscan->samplers[i]);

   zscan->pipe->delete_rasterizer_state(zscan->pipe, zscan->rs_state);

error_blend:
   zscan->pipe->delete_blend_state(zscan->pipe, zscan->blend);

error_rs_state:
   return false;
}

static void
cleanup_state(struct vl_zscan *zscan)
{
   unsigned i;

   assert(zscan);

   for (i = 0; i < 3; ++i)
      zscan->pipe->delete_sampler_state(zscan->pipe, zscan->samplers[i]);

   zscan->pipe->delete_rasterizer_state(zscan->pipe, zscan->rs_state);
   zscan->pipe->delete_blend_state(zscan->pipe, zscan->blend);
}

struct pipe_sampler_view *
vl_zscan_layout(struct pipe_context *pipe, const int layout[64], unsigned blocks_per_line)
{
   const unsigned total_size = blocks_per_line * VL_BLOCK_WIDTH * VL_BLOCK_HEIGHT;

   int patched_layout[64];

   struct pipe_resource res_tmpl, *res;
   struct pipe_sampler_view sv_tmpl, *sv;
   struct pipe_transfer *buf_transfer;
   unsigned x, y, i, pitch;
   float *f;

   /* pipe_box is laid out {x, width, y, height, z, depth}; a positional
    * initializer would drop VL_BLOCK_HEIGHT into z and upload to an
    * out-of-bounds layer.  Name the fields. */
   struct pipe_box rect =
   {
      .x = 0, .y = 0, .z = 0,
      .width = VL_BLOCK_WIDTH * blocks_per_line,
      .height = VL_BLOCK_HEIGHT,
      .depth = 1,
   };

   assert(pipe && layout && blocks_per_line);

   for (i = 0; i < 64; ++i)
      patched_layout[layout[i]] = i;

   memset(&res_tmpl, 0, sizeof(res_tmpl));
   res_tmpl.target = PIPE_TEXTURE_2D;
   res_tmpl.format = PIPE_FORMAT_R32_FLOAT;
   res_tmpl.width0 = VL_BLOCK_WIDTH * blocks_per_line;
   res_tmpl.height0 = VL_BLOCK_HEIGHT;
   res_tmpl.depth0 = 1;
   res_tmpl.array_size = 1;
   res_tmpl.usage = PIPE_USAGE_IMMUTABLE;
   res_tmpl.bind = PIPE_BIND_SAMPLER_VIEW;

   res = pipe->screen->resource_create(pipe->screen, &res_tmpl);
   if (!res)
      goto error_resource;

   f = pipe->texture_map(pipe, res,
                          0, PIPE_MAP_WRITE | PIPE_MAP_DISCARD_RANGE,
                          &rect, &buf_transfer);
   if (!f)
      goto error_map;

   pitch = buf_transfer->stride / sizeof(float);

   for (i = 0; i < blocks_per_line; ++i)
      for (y = 0; y < VL_BLOCK_HEIGHT; ++y)
         for (x = 0; x < VL_BLOCK_WIDTH; ++x) {
            float addr = patched_layout[x + y * VL_BLOCK_WIDTH] +
               i * VL_BLOCK_WIDTH * VL_BLOCK_HEIGHT;

            addr /= total_size;

            f[i * VL_BLOCK_WIDTH + y * pitch + x] = addr;
         }

   pipe->texture_unmap(pipe, buf_transfer);

   memset(&sv_tmpl, 0, sizeof(sv_tmpl));
   u_sampler_view_default_template(&sv_tmpl, res, res->format);
   sv = pipe->create_sampler_view(pipe, res, &sv_tmpl);
   pipe_resource_reference(&res, NULL);
   if (!sv)
      goto error_map;

   return sv;

error_map:
   pipe_resource_reference(&res, NULL);

error_resource:
   return NULL;
}

bool
vl_zscan_init(struct vl_zscan *zscan, struct pipe_context *pipe,
              unsigned buffer_width, unsigned buffer_height,
              unsigned blocks_per_line, unsigned blocks_total,
              unsigned num_channels)
{
   assert(zscan && pipe);

   zscan->pipe = pipe;
   zscan->buffer_width = buffer_width;
   zscan->buffer_height = buffer_height;
   zscan->num_channels = num_channels;
   zscan->blocks_per_line = blocks_per_line;
   zscan->blocks_total = blocks_total;

   if(!init_shaders(zscan))
      return false;

   if(!init_state(zscan)) {
      cleanup_shaders(zscan);
      return false;
   }

   return true;
}

void
vl_zscan_cleanup(struct vl_zscan *zscan)
{
   assert(zscan);

   cleanup_shaders(zscan);
   cleanup_state(zscan);
}

bool
vl_zscan_init_buffer(struct vl_zscan *zscan, struct vl_zscan_buffer *buffer,
                     struct pipe_sampler_view *src, struct pipe_surface *dst)
{
   struct pipe_resource res_tmpl, *res;
   struct pipe_sampler_view sv_tmpl;
   unsigned dst_width, dst_height;

   assert(zscan && buffer);

   memset(buffer, 0, sizeof(struct vl_zscan_buffer));

   pipe_sampler_view_reference(&buffer->src, src);

   pipe_surface_size(dst, &dst_width, &dst_height);

   buffer->viewport.scale[0] = dst_width;
   buffer->viewport.scale[1] = dst_height;
   buffer->viewport.scale[2] = 1;
   buffer->viewport.translate[0] = 0;
   buffer->viewport.translate[1] = 0;
   buffer->viewport.translate[2] = 0;
   buffer->viewport.swizzle_x = PIPE_VIEWPORT_SWIZZLE_POSITIVE_X;
   buffer->viewport.swizzle_y = PIPE_VIEWPORT_SWIZZLE_POSITIVE_Y;
   buffer->viewport.swizzle_z = PIPE_VIEWPORT_SWIZZLE_POSITIVE_Z;
   buffer->viewport.swizzle_w = PIPE_VIEWPORT_SWIZZLE_POSITIVE_W;

   buffer->fb_state.width = dst_width;
   buffer->fb_state.height = dst_height;
   buffer->fb_state.nr_cbufs = 1;
   /* mesa-26 framebuffer cbufs are surface values; copy the render target and
    * take a ref on its resource so the stored fb outlives the caller's dst. */
   buffer->fb_state.cbufs[0] = *dst;
   buffer->fb_state.cbufs[0].texture = NULL;
   pipe_resource_reference(&buffer->fb_state.cbufs[0].texture, dst->texture);

   memset(&res_tmpl, 0, sizeof(res_tmpl));
   res_tmpl.target = PIPE_TEXTURE_3D;
   res_tmpl.format = PIPE_FORMAT_R8_UNORM;
   res_tmpl.width0 = VL_BLOCK_WIDTH * zscan->blocks_per_line;
   res_tmpl.height0 = VL_BLOCK_HEIGHT;
   res_tmpl.depth0 = 2;
   res_tmpl.array_size = 1;
   res_tmpl.usage = PIPE_USAGE_IMMUTABLE;
   res_tmpl.bind = PIPE_BIND_SAMPLER_VIEW;

   res = zscan->pipe->screen->resource_create(zscan->pipe->screen, &res_tmpl);
   if (!res)
      return false;

   memset(&sv_tmpl, 0, sizeof(sv_tmpl));
   u_sampler_view_default_template(&sv_tmpl, res, res->format);
   sv_tmpl.swizzle_r = sv_tmpl.swizzle_g = sv_tmpl.swizzle_b = sv_tmpl.swizzle_a = PIPE_SWIZZLE_X;
   buffer->quant = zscan->pipe->create_sampler_view(zscan->pipe, res, &sv_tmpl);
   pipe_resource_reference(&res, NULL);
   if (!buffer->quant)
      return false;

   return true;
}

void
vl_zscan_cleanup_buffer(struct vl_zscan_buffer *buffer)
{
   assert(buffer);

   pipe_sampler_view_reference(&buffer->src, NULL);
   pipe_sampler_view_reference(&buffer->layout, NULL);
   pipe_sampler_view_reference(&buffer->quant, NULL);
   pipe_resource_reference(&buffer->fb_state.cbufs[0].texture, NULL);
}

void
vl_zscan_set_layout(struct vl_zscan_buffer *buffer, struct pipe_sampler_view *layout)
{
   assert(buffer);
   assert(layout);

   pipe_sampler_view_reference(&buffer->layout, layout);
}

void
vl_zscan_upload_quant(struct vl_zscan *zscan, struct vl_zscan_buffer *buffer,
                      const uint8_t matrix[64], bool intra)
{
   struct pipe_context *pipe;
   struct pipe_transfer *buf_transfer;
   unsigned x, y, i, pitch;
   uint8_t *data;

   /* pipe_box is laid out {x, width, y, height, z, depth}; name the fields so
    * the intra/non-intra depth slice lands in z, not the block height. */
   struct pipe_box rect =
   {
      .x = 0, .y = 0, .z = intra ? 1 : 0,
      .width = VL_BLOCK_WIDTH,
      .height = VL_BLOCK_HEIGHT,
      .depth = 1,
   };

   assert(buffer);
   assert(matrix);

   pipe = zscan->pipe;

   rect.width *= zscan->blocks_per_line;

   data = pipe->texture_map(pipe, buffer->quant->texture,
                             0, PIPE_MAP_WRITE |
                             PIPE_MAP_DISCARD_RANGE,
                             &rect, &buf_transfer);
   if (!data)
      return;

   pitch = buf_transfer->stride;

   for (i = 0; i < zscan->blocks_per_line; ++i)
      for (y = 0; y < VL_BLOCK_HEIGHT; ++y)
         for (x = 0; x < VL_BLOCK_WIDTH; ++x)
            data[i * VL_BLOCK_WIDTH + y * pitch + x] = matrix[x + y * VL_BLOCK_WIDTH];

   pipe->texture_unmap(pipe, buf_transfer);
}

void
vl_zscan_render(struct vl_zscan *zscan, struct vl_zscan_buffer *buffer, unsigned num_instances)
{
   assert(buffer);

   zscan->pipe->bind_rasterizer_state(zscan->pipe, zscan->rs_state);
   zscan->pipe->bind_blend_state(zscan->pipe, zscan->blend);
   zscan->pipe->bind_sampler_states(zscan->pipe, MESA_SHADER_FRAGMENT,
                                    0, 3, zscan->samplers);
   zscan->pipe->set_framebuffer_state(zscan->pipe, &buffer->fb_state);
   zscan->pipe->set_viewport_states(zscan->pipe, 0, 1, &buffer->viewport);
   zscan->pipe->set_sampler_views(zscan->pipe, MESA_SHADER_FRAGMENT,
                                  0, 3, 0, &buffer->src);
   zscan->pipe->bind_vs_state(zscan->pipe, zscan->vs);
   zscan->pipe->bind_fs_state(zscan->pipe, zscan->fs);
   util_draw_arrays_instanced(zscan->pipe, MESA_PRIM_QUADS, 0, 4, 0, num_instances);
}
