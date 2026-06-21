/*
 * Copyright (c) 2026 Terascale Functionalists
 * SPDX-License-Identifier: MIT
 */

#include "pipe/p_context.h"
#include "pipe/p_screen.h"
#include "pipe/p_shader_tokens.h"
#include "pipe/p_state.h"

#include "compiler/nir/nir_builder.h"

#include "cso_cache/cso_context.h"
#include "util/u_draw_quad.h"
#include "util/u_inlines.h"
#include "util/u_memory.h"
#include "util/u_sampler.h"
#include "util/u_simple_shaders.h"

#include "vl_nir.h"
#include "vl_h264_idct.h"
#include "vl_h264_mc.h"
#include "vl_h264_reconstruct.h"
#include "vl_h264_emit.h"

#define MB_SIZE 16
#define BLOCK 4
#define LUMA_BLOCKS_PER_ROW (MB_SIZE / BLOCK)   /* 4 */

struct vl_h264_emit {
   struct pipe_context *pipe;
   struct cso_context *cso;
   void *vs;
   void *fs_copy;       /* integer-pel prediction: sample the reference as is */
   void *fs_mc_h;       /* half-pel horizontal six-tap */
   void *fs_mc_v;       /* half-pel vertical six-tap */
   void *fs_idct_row;   /* whole-plane inverse-transform row pass */
   void *fs_idct_col;   /* whole-plane inverse-transform column pass */
   void *fs_recon;      /* Clip1(prediction + residual) */
   void *fs_scale;      /* UNORM <-> integer luma domain scale at the boundary */
   void *sampler;       /* NEAREST, clamp-to-edge, shared by every pass */
};

/* Integer-pel prediction kernel: sample the reference at the fragment's
 * texcoord and pass it through.  The half-pel positions use the six-tap kernels;
 * this covers the integer position, where the prediction is the reference
 * sample unchanged. */
static nir_def *
build_copy_color(struct vl_nir_fs *fs)
{
   nir_builder *b = &fs->b;
   nir_def *tc = fs->texcoord[0];
   nir_def *uv = nir_vec2(b, nir_channel(b, tc, 0), nir_channel(b, tc, 1));
   nir_def *sample = nir_channel(b, vl_nir_tex(fs, 0, uv), 0);
   return nir_replicate(b, sample, 4);
}

static void *
create_copy_fs(struct pipe_context *pipe)
{
   struct vl_nir_fs fs;
   vl_nir_fs_begin(&fs, pipe, 1, "vl:h264_mc_copy_fs");
   vl_nir_sampler(&fs, 0, GLSL_SAMPLER_DIM_2D);
   return vl_nir_fs_finish(&fs, pipe, build_copy_color(&fs));
}

/* Format-boundary scale kernel: sample the source and multiply by the scale the
 * draw passes in the second varying's first lane.  The orchestrator works in the
 * integer 0..255 luma domain; a video surface plane is R8_UNORM in [0,1], so the
 * reference is scaled up by 255 on the way in and the reconstructed luma down by
 * 1/255 on the way out, both exact since the values are integers in 0..255. */
static nir_def *
build_scale_color(struct vl_nir_fs *fs)
{
   nir_builder *b = &fs->b;
   nir_def *tc = fs->texcoord[0];
   nir_def *uv = nir_vec2(b, nir_channel(b, tc, 0), nir_channel(b, tc, 1));
   nir_def *sample = nir_channel(b, vl_nir_tex(fs, 0, uv), 0);
   nir_def *scaled = nir_fmul(b, sample, nir_channel(b, fs->texcoord[1], 0));
   return nir_replicate(b, scaled, 4);
}

static void *
create_scale_fs(struct pipe_context *pipe)
{
   struct vl_nir_fs fs;
   vl_nir_fs_begin(&fs, pipe, 2, "vl:h264_scale_fs");
   vl_nir_sampler(&fs, 0, GLSL_SAMPLER_DIM_2D);
   return vl_nir_fs_finish(&fs, pipe, build_scale_color(&fs));
}

struct vl_h264_emit *
vl_h264_emit_create(struct pipe_context *pipe)
{
   struct vl_h264_emit *emit = CALLOC_STRUCT(vl_h264_emit);
   if (!emit)
      return NULL;

   emit->pipe = pipe;
   emit->cso = cso_create_context(pipe, 0);
   if (!emit->cso) {
      FREE(emit);
      return NULL;
   }

   static const enum tgsi_semantic vs_sem[] = {
      TGSI_SEMANTIC_POSITION, TGSI_SEMANTIC_GENERIC, TGSI_SEMANTIC_GENERIC,
   };
   static const unsigned vs_idx[] = {0, 0, 1};
   emit->vs = util_make_vertex_passthrough_shader(pipe, 3, vs_sem, vs_idx,
                                                  false);

   emit->fs_copy = create_copy_fs(pipe);
   emit->fs_mc_h = vl_h264_mc_create_halfpel_h_fs(pipe);
   emit->fs_mc_v = vl_h264_mc_create_halfpel_v_fs(pipe);
   emit->fs_idct_row = vl_h264_idct_create_plane_row_fs(pipe);
   emit->fs_idct_col = vl_h264_idct_create_plane_col_fs(pipe);
   emit->fs_recon = vl_h264_reconstruct_create_fs(pipe);
   emit->fs_scale = create_scale_fs(pipe);

   /* NEAREST with clamp-to-edge matches the H.264 reference-picture edge
    * extension and the integer-sample reads every kernel makes. */
   struct pipe_sampler_state samp = {0};
   samp.min_img_filter = PIPE_TEX_FILTER_NEAREST;
   samp.mag_img_filter = PIPE_TEX_FILTER_NEAREST;
   samp.min_mip_filter = PIPE_TEX_MIPFILTER_NONE;
   samp.wrap_s = PIPE_TEX_WRAP_CLAMP_TO_EDGE;
   samp.wrap_t = PIPE_TEX_WRAP_CLAMP_TO_EDGE;
   samp.wrap_r = PIPE_TEX_WRAP_CLAMP_TO_EDGE;
   emit->sampler = pipe->create_sampler_state(pipe, &samp);

   return emit;
}

void
vl_h264_emit_destroy(struct vl_h264_emit *emit)
{
   if (!emit)
      return;
   struct pipe_context *pipe = emit->pipe;

   /* cso unbinds the bound shaders and sampler before any delete_*_state. */
   if (emit->cso)
      cso_destroy_context(emit->cso);
   if (emit->sampler)
      pipe->delete_sampler_state(pipe, emit->sampler);
   if (emit->fs_scale)
      pipe->delete_fs_state(pipe, emit->fs_scale);
   if (emit->fs_recon)
      pipe->delete_fs_state(pipe, emit->fs_recon);
   if (emit->fs_idct_col)
      pipe->delete_fs_state(pipe, emit->fs_idct_col);
   if (emit->fs_idct_row)
      pipe->delete_fs_state(pipe, emit->fs_idct_row);
   if (emit->fs_mc_v)
      pipe->delete_fs_state(pipe, emit->fs_mc_v);
   if (emit->fs_mc_h)
      pipe->delete_fs_state(pipe, emit->fs_mc_h);
   if (emit->fs_copy)
      pipe->delete_fs_state(pipe, emit->fs_copy);
   if (emit->vs)
      pipe->delete_vs_state(pipe, emit->vs);
   FREE(emit);
}

static struct pipe_resource *
make_plane(struct pipe_screen *screen, unsigned width, unsigned height)
{
   struct pipe_resource templ = {0};
   templ.target = PIPE_TEXTURE_2D;
   templ.width0 = width;
   templ.height0 = height;
   templ.depth0 = 1;
   templ.array_size = 1;
   templ.format = PIPE_FORMAT_R32_FLOAT;
   templ.usage = PIPE_USAGE_DEFAULT;
   templ.bind = PIPE_BIND_SAMPLER_VIEW | PIPE_BIND_RENDER_TARGET;
   return screen->resource_create(screen, &templ);
}

static struct pipe_sampler_view *
make_view(struct pipe_context *ctx, struct pipe_resource *tex)
{
   struct pipe_sampler_view templ;
   u_sampler_view_default_template(&templ, tex, tex->format);
   return ctx->create_sampler_view(ctx, tex, &templ);
}

/* One textured pass: bind fs and the dst surface, map the clip quad to the pixel
 * rectangle (vp_x, vp_y, vp_w, vp_h) of dst, interpolate texcoord.xy across
 * [u0,u1]x[v0,v1] with step.zw and plane dims in the second varying, sample the
 * given views, and draw.  The viewport places the output; the texcoord drives
 * the sampling, so the two coordinate systems stay independent. */
static void
draw_pass(struct vl_h264_emit *emit, void *fs, struct pipe_surface *dst,
          unsigned dst_w, unsigned dst_h, float vp_x, float vp_y,
          float vp_w, float vp_h, float u0, float v0, float u1, float v1,
          float step_x, float step_y, float dim_x, float dim_y,
          struct pipe_sampler_view **views, unsigned nviews)
{
   struct cso_context *cso = emit->cso;
   const void *samplers[2] = {emit->sampler, emit->sampler};

   struct pipe_framebuffer_state fb = {0};
   fb.width = dst_w;
   fb.height = dst_h;
   fb.nr_cbufs = 1;
   fb.cbufs[0] = *dst;
   cso_set_framebuffer(cso, &fb);

   struct pipe_viewport_state vp = {0};
   vp.scale[0] = vp_w / 2.0f;
   vp.scale[1] = vp_h / 2.0f;
   vp.scale[2] = 1.0f;
   vp.translate[0] = vp_x + vp_w / 2.0f;
   vp.translate[1] = vp_y + vp_h / 2.0f;
   cso_set_viewport(cso, &vp);

   cso_set_vertex_shader_handle(cso, emit->vs);
   cso_set_fragment_shader_handle(cso, fs);
   cso_set_samplers(cso, MESA_SHADER_FRAGMENT, nviews,
                    (const struct pipe_sampler_state **)samplers);
   emit->pipe->set_sampler_views(emit->pipe, MESA_SHADER_FRAGMENT, 0, nviews, 0,
                                 views);

   /* Clip corner (x,y) maps to texcoord ((x+1)/2 of [u0,u1], (y+1)/2 of
    * [v0,v1]); the third attribute carries the plane dims the transform passes
    * read and is unused by the other kernels. */
   const float verts[] = {
      -1, -1, 0, 1,  u0, v0, step_x, step_y,  dim_x, dim_y, 0, 0,
      -1,  1, 0, 1,  u0, v1, step_x, step_y,  dim_x, dim_y, 0, 0,
       1,  1, 0, 1,  u1, v1, step_x, step_y,  dim_x, dim_y, 0, 0,
       1, -1, 0, 1,  u1, v0, step_x, step_y,  dim_x, dim_y, 0, 0,
   };
   struct cso_velems_state ve;
   memset(&ve, 0, sizeof(ve));
   ve.count = 3;
   for (unsigned i = 0; i < 3; ++i) {
      ve.velems[i].src_format = PIPE_FORMAT_R32G32B32A32_FLOAT;
      ve.velems[i].src_offset = i * 16;
      ve.velems[i].src_stride = 3 * 4 * sizeof(float);
   }
   util_draw_user_vertices(cso, &ve, (void *)verts, MESA_PRIM_QUADS, 4);
}

/* Scatter the slice's per-macroblock luma coefficients into a width x height
 * coefficient plane.  The contract stores the sixteen luma 4x4 blocks in raster
 * block order (block i at in-macroblock position bx = i % 4, by = i / 4), each
 * in raster coefficient order, so block i's coefficient (r,c) lands at plane
 * texel (mb_y*16 + by*4 + r, mb_x*16 + bx*4 + c).  Macroblocks the slice did not
 * fill stay zero, so their residual is zero. */
static void
scatter_luma_coeff(struct pipe_context *ctx, struct pipe_resource *coeff,
                   unsigned width, unsigned height,
                   const struct vl_h264_slice_contract *slice)
{
   struct pipe_transfer *xfer;
   float *map = pipe_texture_map(ctx, coeff, 0, 0, PIPE_MAP_WRITE, 0, 0, width,
                                 height, &xfer);
   const unsigned row_floats = xfer->stride / sizeof(float);

   for (unsigned y = 0; y < height; ++y)
      memset(map + y * row_floats, 0, width * sizeof(float));

   for (unsigned m = 0; m < slice->num_macroblocks; ++m) {
      const struct vl_h264_mb_contract *mb = &slice->macroblocks[m];
      const unsigned ox = (unsigned)mb->mb_x * MB_SIZE;
      const unsigned oy = (unsigned)mb->mb_y * MB_SIZE;
      if (ox + MB_SIZE > width || oy + MB_SIZE > height)
         continue;

      for (unsigned blk = 0; blk < VL_H264_LUMA_4X4_BLOCKS; ++blk) {
         const unsigned bx = blk % LUMA_BLOCKS_PER_ROW;
         const unsigned by = blk / LUMA_BLOCKS_PER_ROW;
         for (unsigned r = 0; r < BLOCK; ++r) {
            float *dst = map + (oy + by * BLOCK + r) * row_floats
                             + (ox + bx * BLOCK);
            for (unsigned c = 0; c < BLOCK; ++c)
               dst[c] = (float)mb->coeff4x4[blk][r * BLOCK + c];
         }
      }
   }
   pipe_texture_unmap(ctx, xfer);
}

/* Motion-compensate one macroblock's 16x16 luma prediction from ref into the
 * macroblock's region of the pred plane.  The list-0 vector is quarter-pel; its
 * integer part shifts the reference read origin and its fractional part selects
 * the kernel.  This rung handles the integer position and the axis-aligned
 * half-pel positions; other fractions fall back to the integer position until
 * the quarter-pel rung lands. */
static void
mc_predict_mb(struct vl_h264_emit *emit, struct pipe_surface *pred,
              unsigned width, unsigned height, struct pipe_sampler_view *ref,
              unsigned ref_w, unsigned ref_h,
              const struct vl_h264_mb_contract *mb)
{
   const int mvx = mb->mv_l0[0][0];
   const int mvy = mb->mv_l0[0][1];
   const int frac_x = mvx & 3;
   const int frac_y = mvy & 3;
   const int int_x = mvx >> 2;   /* arithmetic shift: floor for negatives */
   const int int_y = mvy >> 2;

   const float ox = (float)((int)mb->mb_x * MB_SIZE + int_x);
   const float oy = (float)((int)mb->mb_y * MB_SIZE + int_y);
   const float inv_rw = 1.0f / (float)ref_w;
   const float inv_rh = 1.0f / (float)ref_h;

   void *fs = emit->fs_copy;
   if (frac_x == 2 && frac_y == 0)
      fs = emit->fs_mc_h;
   else if (frac_x == 0 && frac_y == 2)
      fs = emit->fs_mc_v;

   draw_pass(emit, fs, pred, width, height,
             (float)((int)mb->mb_x * MB_SIZE), (float)((int)mb->mb_y * MB_SIZE),
             MB_SIZE, MB_SIZE,
             ox * inv_rw, oy * inv_rh,
             (ox + MB_SIZE) * inv_rw, (oy + MB_SIZE) * inv_rh,
             inv_rw, inv_rh, (float)ref_w, (float)ref_h, &ref, 1);
}

/* The fixed pipeline state every pass shares: opaque write, no depth/stencil,
 * and the pixel-center and edge rules the per-stage rungs validated under. */
static void
set_pipeline_state(struct vl_h264_emit *emit)
{
   struct pipe_blend_state blend = {0};
   blend.rt[0].colormask = PIPE_MASK_RGBA;
   cso_set_blend(emit->cso, &blend);
   struct pipe_depth_stencil_alpha_state dsa = {{{0}}};
   cso_set_depth_stencil_alpha(emit->cso, &dsa);
   struct pipe_rasterizer_state rs = {0};
   rs.half_pixel_center = 1;
   rs.bottom_edge_rule = 1;
   rs.depth_clip_near = 1;
   rs.depth_clip_far = 1;
   cso_set_rasterizer(emit->cso, &rs);
}

void
vl_h264_emit_luma_inter(struct vl_h264_emit *emit, struct pipe_surface *dst_luma,
                        unsigned width, unsigned height,
                        struct pipe_sampler_view *ref_luma, unsigned ref_width,
                        unsigned ref_height,
                        const struct vl_h264_slice_contract *slice)
{
   struct pipe_context *pipe = emit->pipe;
   struct pipe_screen *screen = pipe->screen;

   /* TODO: cache the scratch planes on the emit across frames keyed by frame
    *       size instead of allocating per call -- four R32_FLOAT frame planes
    *       per frame is wasteful.  reason -- the per-frame lifetime keeps this
    *       first reconstruction rung simple; sizing is the frame, not the
    *       macroblock.  tracking -- vl_h264_emit scratch-plane cache. */
   struct pipe_resource *coeff = make_plane(screen, width, height);
   struct pipe_resource *pred = make_plane(screen, width, height);
   struct pipe_resource *inter = make_plane(screen, width, height);
   struct pipe_resource *residual = make_plane(screen, width, height);
   struct pipe_sampler_view *coeff_view = make_view(pipe, coeff);
   struct pipe_sampler_view *pred_view = make_view(pipe, pred);
   struct pipe_sampler_view *inter_view = make_view(pipe, inter);
   struct pipe_sampler_view *residual_view = make_view(pipe, residual);

   struct pipe_surface pred_surf = {{0}};
   pred_surf.format = pred->format;
   pred_surf.texture = pred;
   struct pipe_surface inter_surf = {{0}};
   inter_surf.format = inter->format;
   inter_surf.texture = inter;
   struct pipe_surface residual_surf = {{0}};
   residual_surf.format = residual->format;
   residual_surf.texture = residual;

   set_pipeline_state(emit);

   scatter_luma_coeff(pipe, coeff, width, height, slice);

   /* Prediction: one motion-compensated draw per macroblock into its region. */
   for (unsigned m = 0; m < slice->num_macroblocks; ++m)
      mc_predict_mb(emit, &pred_surf, width, height, ref_luma, ref_width,
                    ref_height, &slice->macroblocks[m]);

   const float inv_w = 1.0f / (float)width;
   const float inv_h = 1.0f / (float)height;

   /* Residual: the whole-plane inverse transform, row pass then column pass,
    * through the R32_FLOAT intermediate.  A sampler barrier orders each plane's
    * render before the next pass samples it. */
   draw_pass(emit, emit->fs_idct_row, &inter_surf, width, height, 0, 0, width,
             height, 0, 0, 1, 1, inv_w, inv_h, (float)width, (float)height,
             &coeff_view, 1);
   pipe->texture_barrier(pipe, PIPE_TEXTURE_BARRIER_SAMPLER);
   draw_pass(emit, emit->fs_idct_col, &residual_surf, width, height, 0, 0,
             width, height, 0, 0, 1, 1, inv_w, inv_h, (float)width,
             (float)height, &inter_view, 1);
   pipe->texture_barrier(pipe, PIPE_TEXTURE_BARRIER_SAMPLER);

   /* Reconstruct: Clip1(prediction + residual) into the target.  The barrier
    * above also orders the prediction draws before this samples them. */
   struct pipe_sampler_view *recon_views[2] = {pred_view, residual_view};
   draw_pass(emit, emit->fs_recon, dst_luma, width, height, 0, 0, width, height,
             0, 0, 1, 1, inv_w, inv_h, (float)width, (float)height, recon_views,
             2);
   pipe->flush(pipe, NULL, 0);

   pipe_sampler_view_reference(&coeff_view, NULL);
   pipe_sampler_view_reference(&pred_view, NULL);
   pipe_sampler_view_reference(&inter_view, NULL);
   pipe_sampler_view_reference(&residual_view, NULL);
   pipe_resource_reference(&coeff, NULL);
   pipe_resource_reference(&pred, NULL);
   pipe_resource_reference(&inter, NULL);
   pipe_resource_reference(&residual, NULL);
}

/* One full-target scale copy: sample src across the whole dst and multiply by
 * scale.  The scale rides in the second varying's first lane, which the scale
 * kernel reads. */
static void
scale_copy(struct vl_h264_emit *emit, struct pipe_surface *dst, unsigned width,
           unsigned height, struct pipe_sampler_view *src, float scale)
{
   draw_pass(emit, emit->fs_scale, dst, width, height, 0, 0, width, height, 0, 0,
             1, 1, 1.0f / (float)width, 1.0f / (float)height, scale, 0, &src, 1);
}

void
vl_h264_emit_luma_inter_unorm(struct vl_h264_emit *emit,
                              struct pipe_surface *dst_luma, unsigned width,
                              unsigned height,
                              struct pipe_sampler_view *ref_luma,
                              unsigned ref_width, unsigned ref_height,
                              const struct vl_h264_slice_contract *slice)
{
   struct pipe_context *pipe = emit->pipe;
   struct pipe_screen *screen = pipe->screen;

   set_pipeline_state(emit);

   /* The reference plane and the target are R8_UNORM in [0,1]; the orchestrator
    * works in the integer 0..255 luma domain.  Scale the reference up into an
    * R32_FLOAT working plane, reconstruct into another, then scale the result
    * down into the target. */
   struct pipe_resource *ref_scaled = make_plane(screen, ref_width, ref_height);
   struct pipe_resource *recon = make_plane(screen, width, height);
   struct pipe_sampler_view *ref_scaled_view = make_view(pipe, ref_scaled);
   struct pipe_sampler_view *recon_view = make_view(pipe, recon);

   struct pipe_surface ref_scaled_surf = {{0}};
   ref_scaled_surf.format = ref_scaled->format;
   ref_scaled_surf.texture = ref_scaled;
   struct pipe_surface recon_surf = {{0}};
   recon_surf.format = recon->format;
   recon_surf.texture = recon;

   scale_copy(emit, &ref_scaled_surf, ref_width, ref_height, ref_luma, 255.0f);
   pipe->texture_barrier(pipe, PIPE_TEXTURE_BARRIER_SAMPLER);

   vl_h264_emit_luma_inter(emit, &recon_surf, width, height, ref_scaled_view,
                           ref_width, ref_height, slice);
   pipe->texture_barrier(pipe, PIPE_TEXTURE_BARRIER_SAMPLER);

   scale_copy(emit, dst_luma, width, height, recon_view, 1.0f / 255.0f);
   pipe->flush(pipe, NULL, 0);

   pipe_sampler_view_reference(&ref_scaled_view, NULL);
   pipe_sampler_view_reference(&recon_view, NULL);
   pipe_resource_reference(&ref_scaled, NULL);
   pipe_resource_reference(&recon, NULL);
}
