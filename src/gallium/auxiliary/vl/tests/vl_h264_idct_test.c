/*
 * Copyright (c) 2026 Terascale Functionalists
 * SPDX-License-Identifier: MIT
 */

/*
 * Render-to-texture verification of the H.264 4x4 inverse-transform fragment
 * programs (vl_h264_idct.c) on a software (llvmpipe) screen.
 *
 * The shaders run two separable butterfly passes through 4x4 R32_FLOAT targets;
 * this harness uploads a coefficient block, runs both passes, reads back the
 * residual, and checks it against an independent integer implementation of the
 * spec (ITU-T H.264 sec 8.5.12.2).  A software rasterizer computes in f32, not
 * the r300 s1e7m16 FP24, so this rung proves the NIR transcription and the
 * transform orientation -- not FP24 truncation, which a separate FP24-precision
 * model owns.  Every intermediate here stays well under 2^24, so the f32
 * comparison is exact at zero tolerance.
 *
 * A separable transform is invariant under swapping its two passes for a
 * symmetric input, so a single DC or symmetric block would pass even with the
 * row and column passes transposed.  The oracle is therefore deliberately
 * discriminating: a [0][1]-vs-[1][0] transpose canary pair, an asymmetric full
 * block, and a random sweep -- inputs whose correct output changes if either
 * pass is mis-oriented.
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

/* Integer reference: ITU-T H.264 sec 8.5.12.2, mirroring the steinmarder
 * idct4_int oracle.  C arithmetic right shift of a negative int is arithmetic
 * (floor toward minus infinity) on the clang/gcc toolchains this builds with,
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
make_block_texture(struct pipe_screen *screen)
{
   struct pipe_resource templ = {0};
   templ.target = PIPE_TEXTURE_2D;
   templ.width0 = BLOCK;
   templ.height0 = BLOCK;
   templ.depth0 = 1;
   templ.array_size = 1;
   templ.format = PIPE_FORMAT_R32_FLOAT;
   templ.usage = PIPE_USAGE_DEFAULT;
   templ.bind = PIPE_BIND_SAMPLER_VIEW | PIPE_BIND_RENDER_TARGET;
   return screen->resource_create(screen, &templ);
}

static struct pipe_sampler_view *
make_block_view(struct pipe_context *ctx, struct pipe_resource *tex)
{
   struct pipe_sampler_view templ;
   u_sampler_view_default_template(&templ, tex, tex->format);
   return ctx->create_sampler_view(ctx, tex, &templ);
}

static void
upload_coeff(struct pipe_context *ctx, struct pipe_resource *tex,
             const int16_t coeff[N])
{
   struct pipe_transfer *xfer;
   void *map = pipe_texture_map(ctx, tex, 0, 0, PIPE_MAP_WRITE,
                               0, 0, BLOCK, BLOCK, &xfer);
   for (int row = 0; row < BLOCK; ++row) {
      float *dst = (float *)((char *)map + row * xfer->stride);
      for (int col = 0; col < BLOCK; ++col)
         dst[col] = (float)coeff[row * BLOCK + col];
   }
   pipe_texture_unmap(ctx, xfer);
}

static void
readback_residual(struct pipe_context *ctx, struct pipe_resource *tex,
                  float out[N])
{
   struct pipe_transfer *xfer;
   void *map = pipe_texture_map(ctx, tex, 0, 0, PIPE_MAP_READ,
                               0, 0, BLOCK, BLOCK, &xfer);
   for (int row = 0; row < BLOCK; ++row) {
      const float *src = (const float *)((const char *)map + row * xfer->stride);
      for (int col = 0; col < BLOCK; ++col)
         out[row * BLOCK + col] = src[col];
   }
   pipe_texture_unmap(ctx, xfer);
}

/* Full-screen quad over the 4x4 target with the standard interleaved
 * position/texcoord layout.  With this viewport (positive y scale) softpipe
 * resolves a fragment at window (col,row) to texcoord ((col+0.5)/4,
 * (row+0.5)/4), so window row == matrix row and window col == matrix col for
 * both the texture reads and the readback. */
static void
draw_block_quad(struct cso_context *cso)
{
   static const float verts[] = {
      -1, -1, 0, 1,   0, 0, 0, 0,
      -1,  1, 0, 1,   0, 1, 0, 0,
       1,  1, 0, 1,   1, 1, 0, 0,
       1, -1, 0, 1,   1, 0, 0, 0,
   };
   struct cso_velems_state ve;
   memset(&ve, 0, sizeof(ve));
   ve.count = 2;
   for (unsigned i = 0; i < 2; ++i) {
      ve.velems[i].src_format = PIPE_FORMAT_R32G32B32A32_FLOAT;
      ve.velems[i].src_offset = i * 16;
      ve.velems[i].src_stride = 2 * 4 * sizeof(float);
   }
   util_draw_user_vertices(cso, &ve, (void *)verts, MESA_PRIM_QUADS, 4);
}

static void
set_block_framebuffer(struct cso_context *cso, struct pipe_resource *tex)
{
   struct pipe_surface surf = {{0}};
   struct pipe_framebuffer_state fb = {0};
   surf.format = tex->format;
   surf.texture = tex;
   fb.width = BLOCK;
   fb.height = BLOCK;
   fb.nr_cbufs = 1;
   fb.cbufs[0] = surf;
   cso_set_framebuffer(cso, &fb);
}

struct idct_pipeline {
   struct pipe_resource *input, *inter, *final;
   struct pipe_sampler_view *view_input, *view_inter;
   void *vs, *fs_row, *fs_col, *sampler;
};

static bool g_debug;

static void
dump_block(struct pipe_context *ctx, const char *label,
           struct pipe_resource *tex)
{
   float v[N];
   readback_residual(ctx, tex, v);
   printf("  %s:\n", label);
   for (int row = 0; row < BLOCK; ++row)
      printf("    [%7.1f %7.1f %7.1f %7.1f]\n",
             v[row * BLOCK + 0], v[row * BLOCK + 1],
             v[row * BLOCK + 2], v[row * BLOCK + 3]);
}

static void
run_idct(struct pipe_context *ctx, struct cso_context *cso,
         struct idct_pipeline *p, const int16_t coeff[N], float out[N])
{
   upload_coeff(ctx, p->input, coeff);

   cso_set_vertex_shader_handle(cso, p->vs);
   cso_set_samplers(cso, MESA_SHADER_FRAGMENT, 1,
                    (const struct pipe_sampler_state **)&p->sampler);

   if (g_debug)
      dump_block(ctx, "input readback", p->input);

   /* Pass 1: row butterfly, input -> intermediate. */
   set_block_framebuffer(cso, p->inter);
   ctx->set_sampler_views(ctx, MESA_SHADER_FRAGMENT, 0, 1, 0, &p->view_input);
   cso_set_fragment_shader_handle(cso, p->fs_row);
   draw_block_quad(cso);

   if (g_debug) {
      ctx->flush(ctx, NULL, 0);
      dump_block(ctx, "pass1 intermediate", p->inter);
   }

   /* Pass 2: column butterfly + normalize, intermediate -> final. */
   set_block_framebuffer(cso, p->final);
   ctx->set_sampler_views(ctx, MESA_SHADER_FRAGMENT, 0, 1, 0, &p->view_inter);
   cso_set_fragment_shader_handle(cso, p->fs_col);
   draw_block_quad(cso);

   ctx->flush(ctx, NULL, 0);
   readback_residual(ctx, p->final, out);
   if (g_debug)
      dump_block(ctx, "pass2 final", p->final);
}

static bool
check_block(const char *name, const int16_t coeff[N], const float got[N])
{
   int64_t ref[N];
   idct4_int_ref(coeff, ref);
   for (int i = 0; i < N; ++i) {
      if ((int64_t)lroundf(got[i]) != ref[i]) {
         printf("FAIL %s: residual[%d] got %.3f want %lld\n",
                name, i, got[i], (long long)ref[i]);
         return false;
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

int
main(void)
{
   g_debug = getenv("VL_H264_IDCT_DEBUG") != NULL;

   struct sw_winsys *winsys = null_sw_create();
   if (!winsys) {
      fprintf(stderr, "vl-h264-idct: no software winsys; skipping\n");
      return 77;
   }
   struct pipe_screen *screen = softpipe_create_screen(winsys);
   if (!screen) {
      fprintf(stderr, "vl-h264-idct: no software screen; skipping\n");
      winsys->destroy(winsys);
      return 77;
   }
   if (!screen->is_format_supported(screen, PIPE_FORMAT_R32_FLOAT,
                                    PIPE_TEXTURE_2D, 0, 0,
                                    PIPE_BIND_RENDER_TARGET |
                                    PIPE_BIND_SAMPLER_VIEW)) {
      fprintf(stderr, "vl-h264-idct: R32_FLOAT not renderable; skipping\n");
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
   struct pipe_viewport_state vp = {0};
   vp.scale[0] = BLOCK / 2.0f;
   vp.scale[1] = BLOCK / 2.0f;
   vp.scale[2] = 1.0f;
   vp.translate[0] = BLOCK / 2.0f;
   vp.translate[1] = BLOCK / 2.0f;
   cso_set_viewport(cso, &vp);

   struct pipe_sampler_state samp = {0};
   samp.min_img_filter = PIPE_TEX_FILTER_NEAREST;
   samp.mag_img_filter = PIPE_TEX_FILTER_NEAREST;
   samp.min_mip_filter = PIPE_TEX_MIPFILTER_NONE;
   samp.wrap_s = PIPE_TEX_WRAP_CLAMP_TO_EDGE;
   samp.wrap_t = PIPE_TEX_WRAP_CLAMP_TO_EDGE;
   samp.wrap_r = PIPE_TEX_WRAP_CLAMP_TO_EDGE;

   static const enum tgsi_semantic vs_sem[] = {
      TGSI_SEMANTIC_POSITION, TGSI_SEMANTIC_GENERIC,
   };
   static const unsigned vs_idx[] = {0, 0};

   struct idct_pipeline p = {0};
   p.input = make_block_texture(screen);
   p.inter = make_block_texture(screen);
   p.final = make_block_texture(screen);
   p.view_input = make_block_view(ctx, p.input);
   p.view_inter = make_block_view(ctx, p.inter);
   p.sampler = ctx->create_sampler_state(ctx, &samp);
   p.vs = util_make_vertex_passthrough_shader(ctx, 2, vs_sem, vs_idx, false);
   p.fs_row = vl_h264_idct_create_row_fs(ctx);
   p.fs_col = vl_h264_idct_create_col_fs(ctx);

   bool pass = true;
   float out[N];

   const struct {
      const char *name;
      int16_t coeff[N];
   } fixtures[] = {
      { "dc_only",       { 64 } },
      { "ac_0_1_canary", { 0, 80, 0, 0 } },
      { "ac_1_0_canary", { 0, 0, 0, 0,  80 } },
      { "asymmetric",    { 10, -40, 5, 0,  -8, 0, 0, 3,
                           0, 12, -2, 0,  7, 0, 0, -1 } },
      { "ramp",          { -50, -43, -36, -29, -22, -15, -8, -1,
                           6, 13, 20, 27, 34, 41, 48, 55 } },
      { "envelope_edge", { 8000, -7000, 6000, -5000,  4000, -3000, 2000, -1000,
                           1500, -2500, 3500, -4500,  5500, -6500, 7500, -8000 } },
   };

   for (unsigned f = 0; f < ARRAY_SIZE(fixtures); ++f) {
      if (g_debug)
         printf("=== fixture %s ===\n", fixtures[f].name);
      run_idct(ctx, cso, &p, fixtures[f].coeff, out);
      if (g_debug) {
         int64_t ref[N];
         idct4_int_ref(fixtures[f].coeff, ref);
         printf("  reference:\n");
         for (int row = 0; row < BLOCK; ++row)
            printf("    [%7lld %7lld %7lld %7lld]\n",
                   (long long)ref[row * BLOCK + 0], (long long)ref[row * BLOCK + 1],
                   (long long)ref[row * BLOCK + 2], (long long)ref[row * BLOCK + 3]);
      }
      bool ok = check_block(fixtures[f].name, fixtures[f].coeff, out);
      pass = pass && ok;
      printf("Test(vl-h264-idct: %s) = %s\n", fixtures[f].name,
             ok ? "pass" : "fail");
   }

   /* Random sweep: blocks bounded inside the QP<=35 in-envelope coefficient
    * range, so every block is also within the FP24 integer-exact envelope.  The
    * per-block dumps would be unreadable under the debug gate, so skip it. */
   lcg_state = 0x48323634u;
   const bool run_sweep = !g_debug;
   const int sweep = run_sweep ? 256 : 0;
   int sweep_ok = 0;
   for (int s = 0; s < sweep; ++s) {
      int16_t coeff[N];
      for (int i = 0; i < N; ++i)
         coeff[i] = lcg_coeff(8000);
      run_idct(ctx, cso, &p, coeff, out);
      if (check_block("random", coeff, out))
         ++sweep_ok;
   }
   pass = pass && (sweep_ok == sweep);
   printf("Test(vl-h264-idct: random sweep) = %s (%d/%d)\n",
          sweep_ok == sweep ? "pass" : "fail", sweep_ok, sweep);

   /* Destroy the cso context first: it unbinds the shaders and sampler from the
    * pipe, which softpipe asserts before any delete_*_state. */
   cso_unbind_context(cso);
   cso_destroy_context(cso);
   ctx->delete_sampler_state(ctx, p.sampler);
   ctx->delete_fs_state(ctx, p.fs_row);
   ctx->delete_fs_state(ctx, p.fs_col);
   ctx->delete_vs_state(ctx, p.vs);
   pipe_sampler_view_reference(&p.view_input, NULL);
   pipe_sampler_view_reference(&p.view_inter, NULL);
   pipe_resource_reference(&p.input, NULL);
   pipe_resource_reference(&p.inter, NULL);
   pipe_resource_reference(&p.final, NULL);
   ctx->destroy(ctx);
   screen->destroy(screen);
   winsys->destroy(winsys);

   printf("vl-h264-idct: %s\n", pass ? "PASS" : "FAIL");
   return pass ? 0 : 1;
}
