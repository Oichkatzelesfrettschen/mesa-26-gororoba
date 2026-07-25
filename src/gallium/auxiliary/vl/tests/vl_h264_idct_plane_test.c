/*
 * SPDX-License-Identifier: MIT
 */

/*
 * Render-to-texture verification of the whole-plane H.264 inverse-transform
 * fragment programs (vl_h264_idct_create_plane_row_fs / _plane_col_fs) on a
 * software (softpipe) screen.
 *
 * The single-block kernels read a fixed four-texel window; the plane kernels
 * derive each fragment's 4x4 block and in-block position from its plane
 * coordinate, so one draw transforms a whole plane of tiled blocks -- the
 * 16x16 luma macroblock (four by four blocks) and the 8x8 chroma plane (two by
 * two) the back half needs.  This harness uploads a coefficient plane, runs the
 * row then column pass through same-sized R32_FLOAT planes, reads back the
 * residual, and checks every block against the independent integer transform
 * (ITU-T H.264 sec 8.5.12.2).  Softpipe computes in f32, not the r300 s1e7m16
 * FP24, so this rung proves the NIR transcription, the block addressing, and the
 * transform orientation; every intermediate stays well under 2^24, so the f32
 * comparison is exact at zero tolerance.
 *
 * The oracle is discriminating in two independent ways.  Filling each block with
 * distinct asymmetric content means a block-addressing error (a fragment reading
 * a neighbouring block, or selecting the wrong output texel) changes the result;
 * the per-block content is itself asymmetric and transpose-sensitive, so a
 * mis-oriented row or column pass changes the result.  A random sweep over every
 * block compounds both.
 */

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "compiler/shader_enums.h"
#include "pipe/p_context.h"
#include "pipe/p_defines.h"
#include "pipe/p_screen.h"
#include "pipe/p_shader_tokens.h"
#include "pipe/p_state.h"
#include "util/format/u_formats.h"

#include "frontend/sw_winsys.h"
#include "softpipe/sp_public.h"
#include "sw/null/null_sw_winsys.h"

#include "cso_cache/cso_context.h"
#include "util/u_draw_quad.h"
#include "util/u_inlines.h"
#include "util/u_sampler.h"
#include "util/u_simple_shaders.h"

#include "vl_h264_idct.h"

#define BLOCK 4
#define N (BLOCK * BLOCK)
/* Largest plane the harness drives: a 16x16 luma macroblock. */
#define MAX_DIM 16

/* Integer reference: ITU-T H.264 sec 8.5.12.2, mirroring the steinmarder
 * idct4_int oracle and the single-block harness.  C arithmetic right shift of a
 * negative int is arithmetic on the clang/gcc toolchains this builds with,
 * matching the spec and the FP24 model's FRC-floor. */
static void
idct4_1d_int(const int64_t z[4], int64_t out[4])
{
   int64_t a = z[0] + z[2];
   int64_t b = z[0] - z[2];
   int64_t c = (z[1] >> 1) - z[3];
   int64_t d = z[1] + (z[3] >> 1);
   out[0] = a + d;
   out[1] = b + c;
   out[2] = b - c;
   out[3] = a - d;
}

static void
idct4_int_ref(const int16_t coeff[N], int64_t residual[N])
{
   int64_t rows[N];
   for (int r = 0; r < BLOCK; ++r) {
      int64_t z[4], o[4];
      for (int c = 0; c < BLOCK; ++c)
         z[c] = coeff[r * BLOCK + c];
      idct4_1d_int(z, o);
      for (int c = 0; c < BLOCK; ++c)
         rows[r * BLOCK + c] = o[c];
   }
   for (int c = 0; c < BLOCK; ++c) {
      int64_t z[4], o[4];
      for (int r = 0; r < BLOCK; ++r)
         z[r] = rows[r * BLOCK + c];
      idct4_1d_int(z, o);
      for (int i = 0; i < BLOCK; ++i)
         residual[i * BLOCK + c] = (o[i] + 32) >> 6;
   }
}

static struct pipe_resource *
make_plane(struct pipe_screen *screen, int width, int height)
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

static void
upload_plane(struct pipe_context *ctx, struct pipe_resource *tex,
             const float *src, int width, int height)
{
   struct pipe_transfer *xfer;
   void *map = pipe_texture_map(ctx, tex, 0, 0, PIPE_MAP_WRITE,
                               0, 0, width, height, &xfer);
   for (int row = 0; row < height; ++row) {
      float *dst = (float *)((char *)map + row * xfer->stride);
      for (int col = 0; col < width; ++col)
         dst[col] = src[row * width + col];
   }
   pipe_texture_unmap(ctx, xfer);
}

static void
readback_plane(struct pipe_context *ctx, struct pipe_resource *tex,
               float *out, int width, int height)
{
   struct pipe_transfer *xfer;
   void *map = pipe_texture_map(ctx, tex, 0, 0, PIPE_MAP_READ,
                               0, 0, width, height, &xfer);
   for (int row = 0; row < height; ++row) {
      const float *s = (const float *)((const char *)map + row * xfer->stride);
      for (int col = 0; col < width; ++col)
         out[row * width + col] = s[col];
   }
   pipe_texture_unmap(ctx, xfer);
}

/* Full-screen quad over the WxH target.  Three interleaved attributes: position,
 * a texcoord (sample position .xy, reference texel step .zw), and the plane
 * width/height in .xy, constant across the quad.  The fragment derives its block
 * from the position and width/height; the step reaches the texel centers. */
static void
draw_plane_quad(struct cso_context *cso, int width, int height)
{
   const float sx = 1.0f / (float)width;
   const float sy = 1.0f / (float)height;
   const float fw = (float)width;
   const float fh = (float)height;
   const float verts[] = {
      -1, -1, 0, 1,   0, 0, sx, sy,   fw, fh, 0, 0,
      -1,  1, 0, 1,   0, 1, sx, sy,   fw, fh, 0, 0,
       1,  1, 0, 1,   1, 1, sx, sy,   fw, fh, 0, 0,
       1, -1, 0, 1,   1, 0, sx, sy,   fw, fh, 0, 0,
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

static void
set_plane_framebuffer(struct cso_context *cso, struct pipe_resource *tex,
                      int width, int height)
{
   struct pipe_surface surf = {{0}};
   struct pipe_framebuffer_state fb = {0};
   surf.format = tex->format;
   surf.texture = tex;
   fb.width = width;
   fb.height = height;
   fb.nr_cbufs = 1;
   fb.cbufs[0] = surf;
   cso_set_framebuffer(cso, &fb);
}

struct plane_pipeline {
   struct pipe_context *ctx;
   struct cso_context *cso;
   void *vs, *fs_row, *fs_col, *sampler;
};

static void
set_viewport(struct cso_context *cso, int width, int height)
{
   struct pipe_viewport_state vp = {0};
   vp.scale[0] = width / 2.0f;
   vp.scale[1] = height / 2.0f;
   vp.scale[2] = 1.0f;
   vp.translate[0] = width / 2.0f;
   vp.translate[1] = height / 2.0f;
   cso_set_viewport(cso, &vp);
}

/* Run both passes over a width x height coefficient plane and read back the
 * residual plane.  input, inter, and final are caller-owned same-sized planes. */
static void
run_plane(struct plane_pipeline *p, struct pipe_resource *input,
          struct pipe_resource *inter, struct pipe_resource *final,
          struct pipe_sampler_view *view_input,
          struct pipe_sampler_view *view_inter,
          const float *coeff, float *out, int width, int height)
{
   struct pipe_context *ctx = p->ctx;
   struct cso_context *cso = p->cso;

   upload_plane(ctx, input, coeff, width, height);
   set_viewport(cso, width, height);
   cso_set_vertex_shader_handle(cso, p->vs);
   cso_set_samplers(cso, MESA_SHADER_FRAGMENT, 1,
                    (const struct pipe_sampler_state **)&p->sampler);

   set_plane_framebuffer(cso, inter, width, height);
   ctx->set_sampler_views(ctx, MESA_SHADER_FRAGMENT, 0, 1, 0, &view_input);
   cso_set_fragment_shader_handle(cso, p->fs_row);
   draw_plane_quad(cso, width, height);

   set_plane_framebuffer(cso, final, width, height);
   ctx->set_sampler_views(ctx, MESA_SHADER_FRAGMENT, 0, 1, 0, &view_inter);
   cso_set_fragment_shader_handle(cso, p->fs_col);
   draw_plane_quad(cso, width, height);

   ctx->flush(ctx, NULL, 0);
   readback_plane(ctx, final, out, width, height);
}

/* Compare the residual plane against the per-block integer transform.  Block
 * (bx,by) lives at plane texels [4*bx, 4*bx+4) x [4*by, 4*by+4); coefficients
 * are stored the same way. */
static bool
check_plane(const char *name, const float *coeff, const float *got,
            int blocks_x, int blocks_y)
{
   const int width = blocks_x * BLOCK;
   for (int by = 0; by < blocks_y; ++by) {
      for (int bx = 0; bx < blocks_x; ++bx) {
         int16_t block[N];
         for (int r = 0; r < BLOCK; ++r)
            for (int c = 0; c < BLOCK; ++c)
               block[r * BLOCK + c] =
                  (int16_t)coeff[(by * BLOCK + r) * width + bx * BLOCK + c];

         int64_t ref[N];
         idct4_int_ref(block, ref);

         for (int r = 0; r < BLOCK; ++r) {
            for (int c = 0; c < BLOCK; ++c) {
               float g = got[(by * BLOCK + r) * width + bx * BLOCK + c];
               if ((int64_t)lroundf(g) != ref[r * BLOCK + c]) {
                  printf("FAIL %s: block(%d,%d) [%d][%d] got %.3f want %lld\n",
                         name, bx, by, r, c, g,
                         (long long)ref[r * BLOCK + c]);
                  return false;
               }
            }
         }
      }
   }
   return true;
}

/* Deterministic LCG so the random sweep is reproducible; seed is "H264". */
static uint32_t lcg_state;

static int16_t
lcg_coeff(int bound)
{
   lcg_state = lcg_state * 1664525u + 1013904223u;
   int span = 2 * bound + 1;
   return (int16_t)((int)((lcg_state >> 9) % (uint32_t)span) - bound);
}

/* Fill each block with distinct asymmetric content keyed by block index, so a
 * fragment that reads the wrong block or selects the wrong output texel fails. */
static void
fill_distinct(float *coeff, int blocks_x, int blocks_y)
{
   const int width = blocks_x * BLOCK;
   for (int by = 0; by < blocks_y; ++by) {
      for (int bx = 0; bx < blocks_x; ++bx) {
         int seed = (by * blocks_x + bx) * 37 + 1;
         for (int r = 0; r < BLOCK; ++r) {
            for (int c = 0; c < BLOCK; ++c) {
               int v = ((r * 4 + c) * 11 + seed * 7) % 401 - 200;
               coeff[(by * BLOCK + r) * width + bx * BLOCK + c] = (float)v;
            }
         }
      }
   }
}

static bool
run_size(struct plane_pipeline *p, struct pipe_screen *screen, int blocks_x,
         int blocks_y)
{
   const int width = blocks_x * BLOCK;
   const int height = blocks_y * BLOCK;

   struct pipe_resource *input = make_plane(screen, width, height);
   struct pipe_resource *inter = make_plane(screen, width, height);
   struct pipe_resource *final = make_plane(screen, width, height);
   struct pipe_sampler_view *view_input = make_view(p->ctx, input);
   struct pipe_sampler_view *view_inter = make_view(p->ctx, inter);

   float coeff[MAX_DIM * MAX_DIM];
   float out[MAX_DIM * MAX_DIM];
   bool pass = true;

   fill_distinct(coeff, blocks_x, blocks_y);
   run_plane(p, input, inter, final, view_input, view_inter, coeff, out,
             width, height);
   bool ok = check_plane("distinct", coeff, out, blocks_x, blocks_y);
   printf("Test(vl-h264-idct-plane: %dx%d distinct) = %s\n", width, height,
          ok ? "pass" : "fail");
   pass = pass && ok;

   lcg_state = 0x48323634u;
   const int sweep = 64;
   int sweep_ok = 0;
   for (int s = 0; s < sweep; ++s) {
      for (int i = 0; i < width * height; ++i)
         coeff[i] = (float)lcg_coeff(8000);
      run_plane(p, input, inter, final, view_input, view_inter, coeff, out,
                width, height);
      if (check_plane("random", coeff, out, blocks_x, blocks_y))
         ++sweep_ok;
   }
   printf("Test(vl-h264-idct-plane: %dx%d random sweep) = %s (%d/%d)\n",
          width, height, sweep_ok == sweep ? "pass" : "fail", sweep_ok, sweep);
   pass = pass && (sweep_ok == sweep);

   pipe_sampler_view_reference(&view_input, NULL);
   pipe_sampler_view_reference(&view_inter, NULL);
   pipe_resource_reference(&input, NULL);
   pipe_resource_reference(&inter, NULL);
   pipe_resource_reference(&final, NULL);
   return pass;
}

int
main(void)
{
   struct sw_winsys *winsys = null_sw_create();
   if (!winsys) {
      fprintf(stderr, "vl-h264-idct-plane: no software winsys; skipping\n");
      return 77;
   }
   struct pipe_screen *screen = softpipe_create_screen(winsys);
   if (!screen) {
      fprintf(stderr, "vl-h264-idct-plane: no software screen; skipping\n");
      winsys->destroy(winsys);
      return 77;
   }
   if (!screen->is_format_supported(screen, PIPE_FORMAT_R32_FLOAT,
                                    PIPE_TEXTURE_2D, 0, 0,
                                    PIPE_BIND_RENDER_TARGET |
                                    PIPE_BIND_SAMPLER_VIEW)) {
      fprintf(stderr, "vl-h264-idct-plane: R32_FLOAT not renderable; skipping\n");
      screen->destroy(screen);
      winsys->destroy(winsys);
      return 77;
   }

   struct pipe_context *ctx = screen->context_create(screen, NULL, 0);
   struct cso_context *cso = cso_create_context(ctx, 0);

   struct pipe_blend_state blend = {0};
   blend.rt[0].colormask = PIPE_MASK_RGBA;
   cso_set_blend(cso, &blend);
   struct pipe_depth_stencil_alpha_state dsa = {{{0}}};
   cso_set_depth_stencil_alpha(cso, &dsa);
   struct pipe_rasterizer_state rs = {0};
   rs.half_pixel_center = 1;
   rs.bottom_edge_rule = 1;
   rs.depth_clip_near = 1;
   rs.depth_clip_far = 1;
   cso_set_rasterizer(cso, &rs);

   struct pipe_sampler_state samp = {0};
   samp.min_img_filter = PIPE_TEX_FILTER_NEAREST;
   samp.mag_img_filter = PIPE_TEX_FILTER_NEAREST;
   samp.min_mip_filter = PIPE_TEX_MIPFILTER_NONE;
   samp.wrap_s = PIPE_TEX_WRAP_CLAMP_TO_EDGE;
   samp.wrap_t = PIPE_TEX_WRAP_CLAMP_TO_EDGE;
   samp.wrap_r = PIPE_TEX_WRAP_CLAMP_TO_EDGE;

   static const enum tgsi_semantic vs_sem[] = {
      TGSI_SEMANTIC_POSITION, TGSI_SEMANTIC_GENERIC, TGSI_SEMANTIC_GENERIC,
   };
   static const unsigned vs_idx[] = {0, 0, 1};

   struct plane_pipeline p = {0};
   p.ctx = ctx;
   p.cso = cso;
   p.sampler = ctx->create_sampler_state(ctx, &samp);
   p.vs = util_make_vertex_passthrough_shader(ctx, 3, vs_sem, vs_idx, false);
   p.fs_row = vl_h264_idct_create_plane_row_fs(ctx);
   p.fs_col = vl_h264_idct_create_plane_col_fs(ctx);

   bool pass = true;
   /* 16x16 luma macroblock (four by four blocks) and 8x8 chroma (two by two):
    * the same shader pair, proving the addressing is size-agnostic. */
   pass = run_size(&p, screen, 4, 4) && pass;
   pass = run_size(&p, screen, 2, 2) && pass;

   cso_unbind_context(cso);
   cso_destroy_context(cso);
   ctx->delete_sampler_state(ctx, p.sampler);
   ctx->delete_fs_state(ctx, p.fs_row);
   ctx->delete_fs_state(ctx, p.fs_col);
   ctx->delete_vs_state(ctx, p.vs);
   ctx->destroy(ctx);
   screen->destroy(screen);
   winsys->destroy(winsys);

   printf("vl-h264-idct-plane: %s\n", pass ? "PASS" : "FAIL");
   return pass ? 0 : 1;
}
