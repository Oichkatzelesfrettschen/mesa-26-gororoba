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
#include "vl_h264_chroma.h"
#include "vl_h264_reconstruct.h"
#include "vl_h264_emit.h"

#define MB_SIZE 16
#define BLOCK 4
#define LUMA_BLOCKS_PER_ROW (MB_SIZE / BLOCK)   /* 4 */

/* 4:2:0 chroma: each component plane is half resolution, so a macroblock covers
 * an 8x8 region of two by two 4x4 blocks per component. */
#define CHROMA_MB_SIZE 8
#define CHROMA_BLOCKS_PER_ROW (CHROMA_MB_SIZE / BLOCK)   /* 2 */
#define CHROMA_BLOCKS_PER_COMPONENT \
   (CHROMA_BLOCKS_PER_ROW * CHROMA_BLOCKS_PER_ROW)       /* 4 */
/* The 2x2 chroma region co-located with one 4x4 luma block (the 16 luma blocks
 * tile the 8x8 chroma macroblock four by four). */
#define CHROMA_SUBBLOCK (CHROMA_MB_SIZE / LUMA_BLOCKS_PER_ROW)   /* 2 */

struct vl_h264_emit {
   struct pipe_context *pipe;
   struct cso_context *cso;
   void *vs;
   void *fs_copy;       /* integer-pel prediction: sample the reference as is */
   void *fs_mc_h;       /* half-pel horizontal six-tap */
   void *fs_mc_v;       /* half-pel vertical six-tap */
   void *fs_qpel_axis;  /* quarter-pel: half-pel along one axis plus an integer */
   void *fs_qpel_diag;  /* quarter-pel: half-pel-horizontal plus -vertical */
   void *fs_chroma;     /* eighth-pel chroma bilinear prediction */
   void *fs_idct_row;   /* whole-plane inverse-transform row pass */
   void *fs_idct_col;   /* whole-plane inverse-transform column pass */
   void *fs_recon;      /* Clip1(prediction + residual) */
   void *fs_scale;      /* UNORM <-> integer domain scale, channel 0 (luma, Cb) */
   void *fs_scale_cr;   /* UNORM scale, channel 1 (Cr lane of NV12 chroma) */
   void *fs_interleave; /* recombine Cb + Cr into an NV12 interleaved plane */
   void *sampler;       /* NEAREST, clamp-to-edge, shared by every pass */
};

/* Integer-pel prediction kernel: sample the reference at the fragment's
 * texcoord and pass it through.  The half-pel and quarter-pel positions use the
 * six-tap kernels; this covers the integer position, where the prediction is the
 * reference sample unchanged. */
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

/* Format-boundary scale kernel: sample one channel of the source and multiply by
 * the scale the draw passes in the second varying's first lane.  The orchestrator
 * works in the integer 0..255 sample domain; a video surface plane is UNORM in
 * [0,1], so values are scaled up by 255 on the way in and down by 1/255 on the
 * way out, both exact since the values are integers in 0..255.  channel 0 reads
 * the luma plane or the Cb lane of an interleaved NV12 chroma plane; channel 1
 * reads the Cr lane. */
static nir_def *
build_scale_channel(struct vl_nir_fs *fs, unsigned channel)
{
   nir_builder *b = &fs->b;
   nir_def *tc = fs->texcoord[0];
   nir_def *uv = nir_vec2(b, nir_channel(b, tc, 0), nir_channel(b, tc, 1));
   nir_def *sample = nir_channel(b, vl_nir_tex(fs, 0, uv), channel);
   nir_def *scaled = nir_fmul(b, sample, nir_channel(b, fs->texcoord[1], 0));
   return nir_replicate(b, scaled, 4);
}

static void *
create_scale_fs(struct pipe_context *pipe, unsigned channel)
{
   struct vl_nir_fs fs;
   vl_nir_fs_begin(&fs, pipe, 2, "vl:h264_scale_fs");
   vl_nir_sampler(&fs, 0, GLSL_SAMPLER_DIM_2D);
   return vl_nir_fs_finish(&fs, pipe, build_scale_channel(&fs, channel));
}

/* Re-interleave kernel: sample the reconstructed Cb (binding 0) and Cr (binding
 * 1), scale each by the second varying's first lane, and write them to the R and
 * G lanes of an NV12 interleaved chroma plane. */
static nir_def *
build_interleave_color(struct vl_nir_fs *fs)
{
   nir_builder *b = &fs->b;
   nir_def *tc = fs->texcoord[0];
   nir_def *uv = nir_vec2(b, nir_channel(b, tc, 0), nir_channel(b, tc, 1));
   nir_def *scale = nir_channel(b, fs->texcoord[1], 0);
   nir_def *cb = nir_fmul(b, nir_channel(b, vl_nir_tex(fs, 0, uv), 0), scale);
   nir_def *cr = nir_fmul(b, nir_channel(b, vl_nir_tex(fs, 1, uv), 0), scale);
   nir_def *zero = nir_imm_float(b, 0.0f);
   return nir_vec4(b, cb, cr, zero, zero);
}

static void *
create_interleave_fs(struct pipe_context *pipe)
{
   struct vl_nir_fs fs;
   vl_nir_fs_begin(&fs, pipe, 2, "vl:h264_chroma_interleave_fs");
   vl_nir_sampler(&fs, 0, GLSL_SAMPLER_DIM_2D);
   vl_nir_sampler(&fs, 1, GLSL_SAMPLER_DIM_2D);
   return vl_nir_fs_finish(&fs, pipe, build_interleave_color(&fs));
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
   emit->fs_qpel_axis = vl_h264_mc_create_qpel_axis_fs(pipe);
   emit->fs_qpel_diag = vl_h264_mc_create_qpel_diag_fs(pipe);
   emit->fs_chroma = vl_h264_chroma_create_bilinear_fs(pipe);
   emit->fs_idct_row = vl_h264_idct_create_plane_row_fs(pipe);
   emit->fs_idct_col = vl_h264_idct_create_plane_col_fs(pipe);
   emit->fs_recon = vl_h264_reconstruct_create_fs(pipe);
   emit->fs_scale = create_scale_fs(pipe, 0);
   emit->fs_scale_cr = create_scale_fs(pipe, 1);
   emit->fs_interleave = create_interleave_fs(pipe);

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
   if (emit->fs_interleave)
      pipe->delete_fs_state(pipe, emit->fs_interleave);
   if (emit->fs_scale_cr)
      pipe->delete_fs_state(pipe, emit->fs_scale_cr);
   if (emit->fs_scale)
      pipe->delete_fs_state(pipe, emit->fs_scale);
   if (emit->fs_recon)
      pipe->delete_fs_state(pipe, emit->fs_recon);
   if (emit->fs_idct_col)
      pipe->delete_fs_state(pipe, emit->fs_idct_col);
   if (emit->fs_idct_row)
      pipe->delete_fs_state(pipe, emit->fs_idct_row);
   if (emit->fs_chroma)
      pipe->delete_fs_state(pipe, emit->fs_chroma);
   if (emit->fs_qpel_diag)
      pipe->delete_fs_state(pipe, emit->fs_qpel_diag);
   if (emit->fs_qpel_axis)
      pipe->delete_fs_state(pipe, emit->fs_qpel_axis);
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
 * [u0,u1]x[v0,v1] with step.zw, carry the four-lane second varying (the transform
 * passes read the plane dims from it, the chroma prediction reads the bilinear
 * weights), sample the given views, and draw.  The viewport places the output;
 * the texcoord drives the sampling, so the two coordinate systems stay
 * independent. */
static void
draw_pass(struct vl_h264_emit *emit, void *fs, struct pipe_surface *dst,
          unsigned dst_w, unsigned dst_h, float vp_x, float vp_y,
          float vp_w, float vp_h, float u0, float v0, float u1, float v1,
          float step_x, float step_y, float var1_x, float var1_y, float var1_z,
          float var1_w, struct pipe_sampler_view **views, unsigned nviews)
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
    * [v0,v1]); the third attribute is the four-lane second varying, constant
    * across the quad. */
   const float verts[] = {
      -1, -1, 0, 1,  u0, v0, step_x, step_y,  var1_x, var1_y, var1_z, var1_w,
      -1,  1, 0, 1,  u0, v1, step_x, step_y,  var1_x, var1_y, var1_z, var1_w,
       1,  1, 0, 1,  u1, v1, step_x, step_y,  var1_x, var1_y, var1_z, var1_w,
       1, -1, 0, 1,  u1, v0, step_x, step_y,  var1_x, var1_y, var1_z, var1_w,
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

/* Which kernel and second-varying parameters a luma fraction needs, indexed
 * [yFrac][xFrac] (ITU-T H.264 sec 8.4.2.2.1).  The integer (copy), half-pel
 * (mc_h, mc_v), and quarter-pel (axis, diag) kernels each read the reference
 * inline so their six-taps clamp at the picture edge.  The four parameter lanes
 * are: for the axis kernel, the tap direction (.xy) and the integer offset (.zw);
 * for the diagonal kernel, the half-pel-horizontal offset (.xy) and the half-pel-
 * vertical offset (.zw); the integer and half-pel kernels ignore them.  The five
 * positions whose value needs the 2D half-pel-diagonal (position j) overflow FP24
 * at their achievable peak of 475320, far above the integer-exact ceiling 2^17,
 * so they have no GPU kernel and fall back to the integer position pending a CPU
 * motion-compensation path. */
enum qpel_kernel { K_COPY, K_MC_H, K_MC_V, K_AXIS, K_DIAG };

struct qpel_dispatch {
   uint8_t kernel;
   int8_t v0, v1, v2, v3;
};

static const struct qpel_dispatch qpel_dispatch[4][4] = {
   /* yFrac = 0 */
   { {K_COPY, 0, 0, 0, 0},    /* (0,0) integer */
     {K_AXIS, 1, 0, 0, 0},    /* (1,0) a: H tap, integer at (0,0) */
     {K_MC_H, 0, 0, 0, 0},    /* (2,0) half-pel b */
     {K_AXIS, 1, 0, 1, 0} },  /* (3,0) c: H tap, integer at (1,0) */
   /* yFrac = 1 */
   { {K_AXIS, 0, 1, 0, 0},    /* (0,1) d: V tap, integer at (0,0) */
     {K_DIAG, 0, 0, 0, 0},    /* (1,1) e: H at (0,0), V at (0,0) */
     {K_COPY, 0, 0, 0, 0},    /* (2,1) f -> needs j */
     {K_DIAG, 0, 0, 1, 0} },  /* (3,1) g: H at (0,0), V at (1,0) */
   /* yFrac = 2 */
   { {K_MC_V, 0, 0, 0, 0},    /* (0,2) half-pel h */
     {K_COPY, 0, 0, 0, 0},    /* (1,2) i -> needs j */
     {K_COPY, 0, 0, 0, 0},    /* (2,2) j */
     {K_COPY, 0, 0, 0, 0} },  /* (3,2) k -> needs j */
   /* yFrac = 3 */
   { {K_AXIS, 0, 1, 0, 1},    /* (0,3) n: V tap, integer at (0,1) */
     {K_DIAG, 0, 1, 0, 0},    /* (1,3) p: H at (0,1), V at (0,0) */
     {K_COPY, 0, 0, 0, 0},    /* (2,3) q -> needs j */
     {K_DIAG, 0, 1, 1, 0} },  /* (3,3) r: H at (0,1), V at (1,0) */
};

/* Whether every 4x4 luma block of the macroblock shares one list-0 vector, i.e.
 * the macroblock is a single 16x16 partition.  Sub-macroblock partitions
 * replicate each partition's vector across the 4x4 blocks it covers, so a
 * per-block walk reconstructs every shape; this fast path coalesces the common
 * single-partition case back into one draw. */
static bool
mb_single_partition(const struct vl_h264_mb_contract *mb)
{
   for (unsigned i = 1; i < VL_H264_LUMA_4X4_BLOCKS; ++i)
      if (mb->mv_l0[i][0] != mb->mv_l0[0][0] ||
          mb->mv_l0[i][1] != mb->mv_l0[0][1])
         return false;
   return true;
}

/* Motion-compensate one luma region -- the whole macroblock or one 4x4
 * sub-block -- from ref into the pred plane.  out_x/out_y is the region's
 * top-left luma sample, size its edge; the region's quarter-pel vector shifts
 * the reference read origin and selects the kernel.  Every kernel samples the
 * reference at binding 0; the quarter-pel kernels build their half-pels inline. */
static void
mc_luma_region(struct vl_h264_emit *emit, struct pipe_surface *pred,
               unsigned width, unsigned height, struct pipe_sampler_view *ref,
               unsigned ref_w, unsigned ref_h, int mvx, int mvy, int out_x,
               int out_y, int size)
{
   const int frac_x = mvx & 3;
   const int frac_y = mvy & 3;
   const float ox = (float)(out_x + (mvx >> 2));   /* arithmetic shift: floor */
   const float oy = (float)(out_y + (mvy >> 2));
   const float inv_rw = 1.0f / (float)ref_w;
   const float inv_rh = 1.0f / (float)ref_h;

   const struct qpel_dispatch *d = &qpel_dispatch[frac_y][frac_x];
   void *kernels[5] = {emit->fs_copy, emit->fs_mc_h, emit->fs_mc_v,
                       emit->fs_qpel_axis, emit->fs_qpel_diag};

   draw_pass(emit, kernels[d->kernel], pred, width, height, (float)out_x,
             (float)out_y, (float)size, (float)size, ox * inv_rw, oy * inv_rh,
             (ox + (float)size) * inv_rw, (oy + (float)size) * inv_rh, inv_rw,
             inv_rh, (float)d->v0, (float)d->v1, (float)d->v2, (float)d->v3,
             &ref, 1);
}

/* Motion-compensate a macroblock's 16x16 luma prediction.  A single-partition
 * macroblock is one draw; otherwise each 4x4 luma block (raster index
 * by*4 + bx) is motion-compensated with its own list-0 vector, which
 * reconstructs every H.264 partition shape since the provider replicates a
 * partition's vector across the 4x4 blocks it covers. */
static void
mc_predict_mb(struct vl_h264_emit *emit, struct pipe_surface *pred,
              unsigned width, unsigned height, struct pipe_sampler_view *ref,
              unsigned ref_w, unsigned ref_h,
              const struct vl_h264_mb_contract *mb)
{
   const int origin_x = (int)mb->mb_x * MB_SIZE;
   const int origin_y = (int)mb->mb_y * MB_SIZE;

   if (mb_single_partition(mb)) {
      mc_luma_region(emit, pred, width, height, ref, ref_w, ref_h,
                     mb->mv_l0[0][0], mb->mv_l0[0][1], origin_x, origin_y,
                     MB_SIZE);
      return;
   }

   for (unsigned blk = 0; blk < VL_H264_LUMA_4X4_BLOCKS; ++blk) {
      const int bx = blk % LUMA_BLOCKS_PER_ROW;
      const int by = blk / LUMA_BLOCKS_PER_ROW;
      mc_luma_region(emit, pred, width, height, ref, ref_w, ref_h,
                     mb->mv_l0[blk][0], mb->mv_l0[blk][1], origin_x + bx * BLOCK,
                     origin_y + by * BLOCK, BLOCK);
   }
}

/* Scatter one chroma component's coefficients into a width x height plane.  The
 * contract stores the chroma 4x4 blocks after the sixteen luma blocks, in raster
 * block order: block_base is 16 for Cb and 20 for Cr, and within a component the
 * four blocks tile the 8x8 region (block i at bx = i % 2, by = i / 2).  Block i's
 * coefficient (r,c) lands at plane texel (mb_y*8 + by*4 + r, mb_x*8 + bx*4 + c). */
static void
scatter_chroma_coeff(struct pipe_context *ctx, struct pipe_resource *coeff,
                     unsigned width, unsigned height,
                     const struct vl_h264_slice_contract *slice,
                     unsigned block_base)
{
   struct pipe_transfer *xfer;
   float *map = pipe_texture_map(ctx, coeff, 0, 0, PIPE_MAP_WRITE, 0, 0, width,
                                 height, &xfer);
   const unsigned row_floats = xfer->stride / sizeof(float);

   for (unsigned y = 0; y < height; ++y)
      memset(map + y * row_floats, 0, width * sizeof(float));

   for (unsigned m = 0; m < slice->num_macroblocks; ++m) {
      const struct vl_h264_mb_contract *mb = &slice->macroblocks[m];
      const unsigned ox = (unsigned)mb->mb_x * CHROMA_MB_SIZE;
      const unsigned oy = (unsigned)mb->mb_y * CHROMA_MB_SIZE;
      if (ox + CHROMA_MB_SIZE > width || oy + CHROMA_MB_SIZE > height)
         continue;

      for (unsigned blk = 0; blk < CHROMA_BLOCKS_PER_COMPONENT; ++blk) {
         const unsigned bx = blk % CHROMA_BLOCKS_PER_ROW;
         const unsigned by = blk / CHROMA_BLOCKS_PER_ROW;
         const int16_t *coeffs = mb->coeff4x4[block_base + blk];
         for (unsigned r = 0; r < BLOCK; ++r) {
            float *dst = map + (oy + by * BLOCK + r) * row_floats
                             + (ox + bx * BLOCK);
            for (unsigned c = 0; c < BLOCK; ++c)
               dst[c] = (float)coeffs[r * BLOCK + c];
         }
      }
   }
   pipe_texture_unmap(ctx, xfer);
}

/* Motion-compensate one chroma region -- the whole 8x8 macroblock chroma or one
 * 2x2 sub-block co-located with a luma 4x4 block -- from ref into the pred plane.
 * For 4:2:0 the chroma vector equals the luma list-0 vector in eighth-chroma-
 * sample units (the quarter-luma vector over the 2x subsampling): the integer
 * part >> 3 shifts the read origin, the fraction & 7 selects the bilinear weights
 * (8-xF)(8-yF), xF(8-yF), (8-xF)yF, xF*yF. */
static void
mc_chroma_region(struct vl_h264_emit *emit, struct pipe_surface *pred,
                 unsigned width, unsigned height, struct pipe_sampler_view *ref,
                 unsigned ref_w, unsigned ref_h, int mvx, int mvy, int out_x,
                 int out_y, int size)
{
   const int frac_x = mvx & 7;
   const int frac_y = mvy & 7;
   const float w0 = (float)((8 - frac_x) * (8 - frac_y));
   const float w1 = (float)(frac_x * (8 - frac_y));
   const float w2 = (float)((8 - frac_x) * frac_y);
   const float w3 = (float)(frac_x * frac_y);

   const float ox = (float)(out_x + (mvx >> 3));   /* arithmetic shift: floor */
   const float oy = (float)(out_y + (mvy >> 3));
   const float inv_rw = 1.0f / (float)ref_w;
   const float inv_rh = 1.0f / (float)ref_h;

   draw_pass(emit, emit->fs_chroma, pred, width, height, (float)out_x,
             (float)out_y, (float)size, (float)size, ox * inv_rw, oy * inv_rh,
             (ox + (float)size) * inv_rw, (oy + (float)size) * inv_rh, inv_rw,
             inv_rh, w0, w1, w2, w3, &ref, 1);
}

/* Motion-compensate a macroblock's 8x8 chroma prediction.  A single-partition
 * macroblock is one draw; otherwise each luma 4x4 block's co-located 2x2 chroma
 * region is motion-compensated with that block's list-0 vector. */
static void
mc_predict_chroma_mb(struct vl_h264_emit *emit, struct pipe_surface *pred,
                     unsigned width, unsigned height,
                     struct pipe_sampler_view *ref, unsigned ref_w,
                     unsigned ref_h, const struct vl_h264_mb_contract *mb)
{
   const int origin_x = (int)mb->mb_x * CHROMA_MB_SIZE;
   const int origin_y = (int)mb->mb_y * CHROMA_MB_SIZE;

   if (mb_single_partition(mb)) {
      mc_chroma_region(emit, pred, width, height, ref, ref_w, ref_h,
                       mb->mv_l0[0][0], mb->mv_l0[0][1], origin_x, origin_y,
                       CHROMA_MB_SIZE);
      return;
   }

   for (unsigned blk = 0; blk < VL_H264_LUMA_4X4_BLOCKS; ++blk) {
      const int bx = blk % LUMA_BLOCKS_PER_ROW;
      const int by = blk / LUMA_BLOCKS_PER_ROW;
      mc_chroma_region(emit, pred, width, height, ref, ref_w, ref_h,
                       mb->mv_l0[blk][0], mb->mv_l0[blk][1],
                       origin_x + bx * CHROMA_SUBBLOCK,
                       origin_y + by * CHROMA_SUBBLOCK, CHROMA_SUBBLOCK);
   }
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

/* The shared residual-and-combine tail, identical for luma and chroma: inverse-
 * transform the already-scattered coefficient plane (row pass then column pass
 * through an R32_FLOAT intermediate) and write Clip1(prediction + residual) into
 * dst.  A sampler barrier orders each render before the next pass samples it;
 * the barrier after the column pass also orders the caller's prediction draws
 * before reconstruction reads them. */
static void
transform_and_reconstruct(struct vl_h264_emit *emit, struct pipe_surface *dst,
                          unsigned width, unsigned height,
                          struct pipe_sampler_view *coeff_view,
                          struct pipe_sampler_view *pred_view)
{
   struct pipe_context *pipe = emit->pipe;
   struct pipe_screen *screen = pipe->screen;

   struct pipe_resource *inter = make_plane(screen, width, height);
   struct pipe_resource *residual = make_plane(screen, width, height);
   struct pipe_sampler_view *inter_view = make_view(pipe, inter);
   struct pipe_sampler_view *residual_view = make_view(pipe, residual);

   struct pipe_surface inter_surf = {{0}};
   inter_surf.format = inter->format;
   inter_surf.texture = inter;
   struct pipe_surface residual_surf = {{0}};
   residual_surf.format = residual->format;
   residual_surf.texture = residual;

   const float inv_w = 1.0f / (float)width;
   const float inv_h = 1.0f / (float)height;

   draw_pass(emit, emit->fs_idct_row, &inter_surf, width, height, 0, 0, width,
             height, 0, 0, 1, 1, inv_w, inv_h, (float)width, (float)height, 0, 0,
             &coeff_view, 1);
   pipe->texture_barrier(pipe, PIPE_TEXTURE_BARRIER_SAMPLER);
   draw_pass(emit, emit->fs_idct_col, &residual_surf, width, height, 0, 0, width,
             height, 0, 0, 1, 1, inv_w, inv_h, (float)width, (float)height, 0, 0,
             &inter_view, 1);
   pipe->texture_barrier(pipe, PIPE_TEXTURE_BARRIER_SAMPLER);

   struct pipe_sampler_view *recon_views[2] = {pred_view, residual_view};
   draw_pass(emit, emit->fs_recon, dst, width, height, 0, 0, width, height, 0, 0,
             1, 1, inv_w, inv_h, (float)width, (float)height, 0, 0, recon_views,
             2);

   pipe_sampler_view_reference(&inter_view, NULL);
   pipe_sampler_view_reference(&residual_view, NULL);
   pipe_resource_reference(&inter, NULL);
   pipe_resource_reference(&residual, NULL);
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
    *       rung simple; sizing is the frame, not the macroblock.  tracking --
    *       vl_h264_emit scratch-plane cache. */
   struct pipe_resource *coeff = make_plane(screen, width, height);
   struct pipe_resource *pred = make_plane(screen, width, height);
   struct pipe_sampler_view *coeff_view = make_view(pipe, coeff);
   struct pipe_sampler_view *pred_view = make_view(pipe, pred);

   struct pipe_surface pred_surf = {{0}};
   pred_surf.format = pred->format;
   pred_surf.texture = pred;

   set_pipeline_state(emit);

   scatter_luma_coeff(pipe, coeff, width, height, slice);

   /* Prediction: one motion-compensated draw per macroblock into its region.
    * The quarter-pel kernels compute their half-pels inline from the reference,
    * so the six-taps clamp at the picture edge for any motion vector. */
   for (unsigned m = 0; m < slice->num_macroblocks; ++m)
      mc_predict_mb(emit, &pred_surf, width, height, ref_luma, ref_width,
                    ref_height, &slice->macroblocks[m]);

   transform_and_reconstruct(emit, dst_luma, width, height, coeff_view,
                             pred_view);
   pipe->flush(pipe, NULL, 0);

   pipe_sampler_view_reference(&coeff_view, NULL);
   pipe_sampler_view_reference(&pred_view, NULL);
   pipe_resource_reference(&coeff, NULL);
   pipe_resource_reference(&pred, NULL);
}

void
vl_h264_emit_chroma_inter(struct vl_h264_emit *emit,
                          struct pipe_surface *dst_chroma, unsigned width,
                          unsigned height, struct pipe_sampler_view *ref_chroma,
                          unsigned ref_width, unsigned ref_height,
                          const struct vl_h264_slice_contract *slice,
                          unsigned block_base)
{
   struct pipe_context *pipe = emit->pipe;
   struct pipe_screen *screen = pipe->screen;

   struct pipe_resource *coeff = make_plane(screen, width, height);
   struct pipe_resource *pred = make_plane(screen, width, height);
   struct pipe_sampler_view *coeff_view = make_view(pipe, coeff);
   struct pipe_sampler_view *pred_view = make_view(pipe, pred);

   struct pipe_surface pred_surf = {{0}};
   pred_surf.format = pred->format;
   pred_surf.texture = pred;

   set_pipeline_state(emit);

   scatter_chroma_coeff(pipe, coeff, width, height, slice, block_base);

   for (unsigned m = 0; m < slice->num_macroblocks; ++m)
      mc_predict_chroma_mb(emit, &pred_surf, width, height, ref_chroma,
                           ref_width, ref_height, &slice->macroblocks[m]);

   transform_and_reconstruct(emit, dst_chroma, width, height, coeff_view,
                             pred_view);
   pipe->flush(pipe, NULL, 0);

   pipe_sampler_view_reference(&coeff_view, NULL);
   pipe_sampler_view_reference(&pred_view, NULL);
   pipe_resource_reference(&coeff, NULL);
   pipe_resource_reference(&pred, NULL);
}

/* One full-target scale copy with the given scale kernel: sample src across the
 * whole dst and multiply by scale, which rides in the second varying's first
 * lane.  fs selects which source channel is read (luma/Cb on channel 0, Cr on
 * channel 1). */
static void
scale_copy(struct vl_h264_emit *emit, void *fs, struct pipe_surface *dst,
           unsigned width, unsigned height, struct pipe_sampler_view *src,
           float scale)
{
   draw_pass(emit, fs, dst, width, height, 0, 0, width, height, 0, 0, 1, 1,
             1.0f / (float)width, 1.0f / (float)height, scale, 0, 0, 0, &src, 1);
}

/* Recombine the reconstructed Cb and Cr planes into an interleaved NV12 chroma
 * surface, scaling each by scale on the way to the UNORM target. */
static void
interleave_copy(struct vl_h264_emit *emit, struct pipe_surface *dst,
                unsigned width, unsigned height, struct pipe_sampler_view *cb,
                struct pipe_sampler_view *cr, float scale)
{
   struct pipe_sampler_view *views[2] = {cb, cr};
   draw_pass(emit, emit->fs_interleave, dst, width, height, 0, 0, width, height,
             0, 0, 1, 1, 1.0f / (float)width, 1.0f / (float)height, scale, 0, 0,
             0, views, 2);
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

   scale_copy(emit, emit->fs_scale, &ref_scaled_surf, ref_width, ref_height,
              ref_luma, 255.0f);
   pipe->texture_barrier(pipe, PIPE_TEXTURE_BARRIER_SAMPLER);

   vl_h264_emit_luma_inter(emit, &recon_surf, width, height, ref_scaled_view,
                           ref_width, ref_height, slice);
   pipe->texture_barrier(pipe, PIPE_TEXTURE_BARRIER_SAMPLER);

   scale_copy(emit, emit->fs_scale, dst_luma, width, height, recon_view,
              1.0f / 255.0f);
   pipe->flush(pipe, NULL, 0);

   pipe_sampler_view_reference(&ref_scaled_view, NULL);
   pipe_sampler_view_reference(&recon_view, NULL);
   pipe_resource_reference(&ref_scaled, NULL);
   pipe_resource_reference(&recon, NULL);
}

void
vl_h264_emit_chroma_inter_unorm(struct vl_h264_emit *emit,
                                struct pipe_surface *dst_chroma, unsigned width,
                                unsigned height,
                                struct pipe_sampler_view *ref_chroma,
                                unsigned ref_width, unsigned ref_height,
                                const struct vl_h264_slice_contract *slice)
{
   struct pipe_context *pipe = emit->pipe;
   struct pipe_screen *screen = pipe->screen;

   set_pipeline_state(emit);

   /* NV12 chroma is one R8G8 plane with Cb in the R lane and Cr in the G lane.
    * De-interleave the reference into two R32_FLOAT working planes scaled to the
    * integer 0..255 domain, reconstruct each component, then recombine the two
    * results into the interleaved target. */
   struct pipe_resource *ref_cb = make_plane(screen, ref_width, ref_height);
   struct pipe_resource *ref_cr = make_plane(screen, ref_width, ref_height);
   struct pipe_resource *recon_cb = make_plane(screen, width, height);
   struct pipe_resource *recon_cr = make_plane(screen, width, height);
   struct pipe_sampler_view *ref_cb_view = make_view(pipe, ref_cb);
   struct pipe_sampler_view *ref_cr_view = make_view(pipe, ref_cr);
   struct pipe_sampler_view *recon_cb_view = make_view(pipe, recon_cb);
   struct pipe_sampler_view *recon_cr_view = make_view(pipe, recon_cr);

   struct pipe_surface ref_cb_surf = {{0}};
   ref_cb_surf.format = ref_cb->format;
   ref_cb_surf.texture = ref_cb;
   struct pipe_surface ref_cr_surf = {{0}};
   ref_cr_surf.format = ref_cr->format;
   ref_cr_surf.texture = ref_cr;
   struct pipe_surface recon_cb_surf = {{0}};
   recon_cb_surf.format = recon_cb->format;
   recon_cb_surf.texture = recon_cb;
   struct pipe_surface recon_cr_surf = {{0}};
   recon_cr_surf.format = recon_cr->format;
   recon_cr_surf.texture = recon_cr;

   scale_copy(emit, emit->fs_scale, &ref_cb_surf, ref_width, ref_height,
              ref_chroma, 255.0f);
   scale_copy(emit, emit->fs_scale_cr, &ref_cr_surf, ref_width, ref_height,
              ref_chroma, 255.0f);
   pipe->texture_barrier(pipe, PIPE_TEXTURE_BARRIER_SAMPLER);

   vl_h264_emit_chroma_inter(emit, &recon_cb_surf, width, height, ref_cb_view,
                             ref_width, ref_height, slice,
                             VL_H264_LUMA_4X4_BLOCKS);
   vl_h264_emit_chroma_inter(emit, &recon_cr_surf, width, height, ref_cr_view,
                             ref_width, ref_height, slice,
                             VL_H264_LUMA_4X4_BLOCKS + CHROMA_BLOCKS_PER_COMPONENT);
   pipe->texture_barrier(pipe, PIPE_TEXTURE_BARRIER_SAMPLER);

   interleave_copy(emit, dst_chroma, width, height, recon_cb_view, recon_cr_view,
                   1.0f / 255.0f);
   pipe->flush(pipe, NULL, 0);

   pipe_sampler_view_reference(&ref_cb_view, NULL);
   pipe_sampler_view_reference(&ref_cr_view, NULL);
   pipe_sampler_view_reference(&recon_cb_view, NULL);
   pipe_sampler_view_reference(&recon_cr_view, NULL);
   pipe_resource_reference(&ref_cb, NULL);
   pipe_resource_reference(&ref_cr, NULL);
   pipe_resource_reference(&recon_cb, NULL);
   pipe_resource_reference(&recon_cr, NULL);
}
