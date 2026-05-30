/**************************************************************************
 *
 * Copyright 2012 Christian König.
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

#include <stdio.h>

#include "pipe/p_context.h"

#include "util/u_draw.h"
#include "util/u_inlines.h"
#include "util/u_memory.h"
#include "util/u_math.h"

#include "vl_types.h"
#include "vl_vertex_buffers.h"
#include "vl_matrix_filter.h"
#include "vl_nir.h"

static void *
create_vert_shader(struct vl_matrix_filter *filter)
{
   return vl_nir_vs_passthrough(filter->pipe, 1, "vl:matrix_filter_vs");
}

static inline bool
is_vec_zero(struct vertex2f v)
{
   return v.x == 0.0f && v.y == 0.0f;
}

static void *
create_frag_shader(struct vl_matrix_filter *filter, unsigned num_offsets,
                   struct vertex2f *offsets, const float *matrix_values)
{
   struct vl_nir_fs fs;
   unsigned i;

   vl_nir_fs_begin(&fs, filter->pipe, 1, "vl:matrix_filter_fs");
   vl_nir_sampler(&fs, 0, GLSL_SAMPLER_DIM_2D);
   nir_builder *b = &fs.b;

   /* Separable neighbourhood convolution: sum_i matrix[i] * tex(tc + offset[i]),
    * accumulated in-shader (one FP24 MAC per non-zero tap, the monograph P3
    * primitive).  Zero-weight taps and the zero offset are folded as in the
    * fixed grid; the offsets already land on the A3 sample grid. */
   nir_def *vtex = nir_trim_vector(b, fs.texcoord[0], 2);
   nir_def *sum = nir_imm_vec4(b, 0.0f, 0.0f, 0.0f, 0.0f);

   for (i = 0; i < num_offsets; ++i) {
      if (matrix_values[i] == 0.0f)
         continue;

      nir_def *coord = is_vec_zero(offsets[i])
         ? vtex
         : nir_fadd(b, vtex, nir_imm_vec2(b, offsets[i].x, offsets[i].y));
      nir_def *texel = vl_nir_tex(&fs, 0, coord);
      sum = nir_ffma(b, texel,
                     nir_imm_vec4(b, matrix_values[i], matrix_values[i],
                                  matrix_values[i], matrix_values[i]),
                     sum);
   }

   return vl_nir_fs_finish(&fs, filter->pipe, sum);
}

bool
vl_matrix_filter_init(struct vl_matrix_filter *filter, struct pipe_context *pipe,
                      unsigned video_width, unsigned video_height,
                      unsigned matrix_width, unsigned matrix_height,
                      const float *matrix_values)
{
   struct pipe_rasterizer_state rs_state;
   struct pipe_blend_state blend;
   struct pipe_sampler_state sampler;
   struct pipe_vertex_element ve;
   struct vertex2f *offsets, v, sizes;
   unsigned i, num_offsets = matrix_width * matrix_height;

   assert(filter && pipe);
   assert(video_width && video_height);
   assert(matrix_width && matrix_height);

   memset(filter, 0, sizeof(*filter));
   filter->pipe = pipe;

   memset(&rs_state, 0, sizeof(rs_state));
   rs_state.half_pixel_center = true;
   rs_state.bottom_edge_rule = true;
   rs_state.depth_clip_near = 1;
   rs_state.depth_clip_far = 1;

   filter->rs_state = pipe->create_rasterizer_state(pipe, &rs_state);
   if (!filter->rs_state)
      goto error_rs_state;

   memset(&blend, 0, sizeof blend);
   blend.rt[0].rgb_func = PIPE_BLEND_ADD;
   blend.rt[0].rgb_src_factor = PIPE_BLENDFACTOR_ONE;
   blend.rt[0].rgb_dst_factor = PIPE_BLENDFACTOR_ONE;
   blend.rt[0].alpha_func = PIPE_BLEND_ADD;
   blend.rt[0].alpha_src_factor = PIPE_BLENDFACTOR_ONE;
   blend.rt[0].alpha_dst_factor = PIPE_BLENDFACTOR_ONE;
   blend.logicop_func = PIPE_LOGICOP_CLEAR;
   blend.rt[0].colormask = PIPE_MASK_RGBA;
   filter->blend = pipe->create_blend_state(pipe, &blend);
   if (!filter->blend)
      goto error_blend;

   memset(&sampler, 0, sizeof(sampler));
   sampler.wrap_s = PIPE_TEX_WRAP_CLAMP_TO_EDGE;
   sampler.wrap_t = PIPE_TEX_WRAP_CLAMP_TO_EDGE;
   sampler.wrap_r = PIPE_TEX_WRAP_CLAMP_TO_EDGE;
   sampler.min_img_filter = PIPE_TEX_FILTER_NEAREST;
   sampler.min_mip_filter = PIPE_TEX_MIPFILTER_NONE;
   sampler.mag_img_filter = PIPE_TEX_FILTER_NEAREST;
   sampler.compare_mode = PIPE_TEX_COMPARE_NONE;
   sampler.compare_func = PIPE_FUNC_ALWAYS;
   /* mesa-26 pipe_sampler_state uses unnormalized_coords (inverted); the
    * memset leaves it 0, i.e. normalized coordinates. */
   filter->sampler = pipe->create_sampler_state(pipe, &sampler);
   if (!filter->sampler)
      goto error_sampler;

   filter->quad = vl_vb_upload_quads(pipe);
   if(!filter->quad.buffer.resource)
      goto error_quad;

   memset(&ve, 0, sizeof(ve));
   ve.src_offset = 0;
   ve.instance_divisor = 0;
   ve.vertex_buffer_index = 0;
   ve.src_format = PIPE_FORMAT_R32G32_FLOAT;
   filter->ves = pipe->create_vertex_elements_state(pipe, 1, &ve);
   if (!filter->ves)
      goto error_ves;

   offsets = MALLOC(sizeof(struct vertex2f) * num_offsets);
   if (!offsets)
      goto error_offsets;

   sizes.x = (float)(matrix_width - 1) / 2.0f;
   sizes.y = (float)(matrix_height - 1) / 2.0f;

   for (v.x = -sizes.x, i = 0; v.x <= sizes.x; v.x += 1.0f)
      for (v.y = -sizes.y; v.y <= sizes.y; v.y += 1.0f)
         offsets[i++] = v;

   for (i = 0; i < num_offsets; ++i) {
      offsets[i].x /= video_width;
      offsets[i].y /= video_height;
   }

   filter->vs = create_vert_shader(filter);
   if (!filter->vs)
      goto error_vs;

   filter->fs = create_frag_shader(filter, num_offsets, offsets, matrix_values);
   if (!filter->fs)
      goto error_fs;

   FREE(offsets);
   return true;

error_fs:
   pipe->delete_vs_state(pipe, filter->vs);

error_vs:
   FREE(offsets);

error_offsets:
   pipe->delete_vertex_elements_state(pipe, filter->ves);

error_ves:
   pipe_resource_reference(&filter->quad.buffer.resource, NULL);

error_quad:
   pipe->delete_sampler_state(pipe, filter->sampler);

error_sampler:
   pipe->delete_blend_state(pipe, filter->blend);

error_blend:
   pipe->delete_rasterizer_state(pipe, filter->rs_state);

error_rs_state:
   return false;
}

void
vl_matrix_filter_cleanup(struct vl_matrix_filter *filter)
{
   assert(filter);

   filter->pipe->delete_sampler_state(filter->pipe, filter->sampler);
   filter->pipe->delete_blend_state(filter->pipe, filter->blend);
   filter->pipe->delete_rasterizer_state(filter->pipe, filter->rs_state);
   filter->pipe->delete_vertex_elements_state(filter->pipe, filter->ves);
   pipe_resource_reference(&filter->quad.buffer.resource, NULL);

   filter->pipe->delete_vs_state(filter->pipe, filter->vs);
   filter->pipe->delete_fs_state(filter->pipe, filter->fs);
}

void
vl_matrix_filter_render(struct vl_matrix_filter *filter,
                        struct pipe_sampler_view *src,
                        struct pipe_surface *dst)
{
   struct pipe_viewport_state viewport;
   struct pipe_framebuffer_state fb_state;
   unsigned dst_width, dst_height;

   assert(filter && src && dst);

   pipe_surface_size(dst, &dst_width, &dst_height);

   memset(&viewport, 0, sizeof(viewport));
   viewport.scale[0] = dst_width;
   viewport.scale[1] = dst_height;
   viewport.scale[2] = 1;
   viewport.swizzle_x = PIPE_VIEWPORT_SWIZZLE_POSITIVE_X;
   viewport.swizzle_y = PIPE_VIEWPORT_SWIZZLE_POSITIVE_Y;
   viewport.swizzle_z = PIPE_VIEWPORT_SWIZZLE_POSITIVE_Z;
   viewport.swizzle_w = PIPE_VIEWPORT_SWIZZLE_POSITIVE_W;

   memset(&fb_state, 0, sizeof(fb_state));
   fb_state.width = dst_width;
   fb_state.height = dst_height;
   fb_state.nr_cbufs = 1;
   fb_state.cbufs[0] = *dst;

   filter->pipe->bind_rasterizer_state(filter->pipe, filter->rs_state);
   filter->pipe->bind_blend_state(filter->pipe, filter->blend);
   filter->pipe->bind_sampler_states(filter->pipe, MESA_SHADER_FRAGMENT,
                                     0, 1, &filter->sampler);
   filter->pipe->set_sampler_views(filter->pipe, MESA_SHADER_FRAGMENT,
                                   0, 1, 0, &src);
   filter->pipe->bind_vs_state(filter->pipe, filter->vs);
   filter->pipe->bind_fs_state(filter->pipe, filter->fs);
   filter->pipe->set_framebuffer_state(filter->pipe, &fb_state);
   filter->pipe->set_viewport_states(filter->pipe, 0, 1, &viewport);
   filter->pipe->set_vertex_buffers(filter->pipe, 1, &filter->quad);
   filter->pipe->bind_vertex_elements_state(filter->pipe, filter->ves);

   util_draw_arrays(filter->pipe, MESA_PRIM_QUADS, 0, 4);
}
