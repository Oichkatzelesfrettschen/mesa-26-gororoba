/*
 * Copyright (c) 2026 Terascale Functionalists
 * SPDX-License-Identifier: MIT
 */

/*
 * Render-to-texture verification of the H.264 normal luma deblock fragment
 * program (vl_h264_deblock.c) on a software (softpipe) screen.
 *
 * The program filters the six edge samples p2..q2 to the four modified samples
 * p1', p0', q0', q1' (ITU-T H.264 sec 8.7.2.3) with predicated activity gates and
 * saturating clips.  This harness uploads a 6-wide strip of edge samples (one
 * edge per row), renders the four modified samples into the RGBA output, and
 * checks each against an independent integer implementation of the filter.
 * Softpipe computes in f32; the largest intermediate is |((q0-p0)<<2) +
 * (p1-q1) + 4| <= 1279 << 2^24, so the comparison is exact.
 *
 * The threshold parameters alpha, beta, tc0 are the CPU-side metadata the
 * fragment program reads as a varying; the oracle sweeps the spec's threshold
 * grid (so both the filter-on and the inner p1/q1 gates fire and stay off across
 * the run) over random edges, an input set whose correct output changes if a
 * gate, clip, or shift is wrong.
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

#include "vl_h264_deblock.h"

#define N 16     /* edges per render */
#define SAMPLES 6 /* p2,p1,p0,q0,q1,q2 */

static int
clampi(int v, int lo, int hi)
{
   return v < lo ? lo : (v > hi ? hi : v);
}

/* Integer reference, ITU-T H.264 sec 8.7.2.3.  *4 / *2 avoid the negative-left-
 * shift the spec writes as <<2 / <<1; the >> floor-divisions are arithmetic on
 * the toolchains this builds with, matching the spec and the FP24 FRC-floor. */
static void
deblock_ref(const int e[SAMPLES], int alpha, int beta, int tc0, int out[4])
{
   int p2 = e[0], p1 = e[1], p0 = e[2], q0 = e[3], q1 = e[4], q2 = e[5];
   int on = (abs(p0 - q0) < alpha) && (abs(p1 - p0) < beta)
            && (abs(q1 - q0) < beta);
   int ap = abs(p2 - p0) < beta;
   int aq = abs(q2 - q0) < beta;
   int tc = tc0 + ap + aq;
   int delta = clampi((((q0 - p0) * 4) + (p1 - q1) + 4) >> 3, -tc, tc);
   int p0n = clampi(p0 + delta, 0, 255);
   int q0n = clampi(q0 - delta, 0, 255);
   int avg = (p0 + q0 + 1) >> 1;
   int dp1 = clampi((p2 + avg - (p1 * 2)) >> 1, -tc0, tc0);
   int dq1 = clampi((q2 + avg - (q1 * 2)) >> 1, -tc0, tc0);
   int p1n = ap ? p1 + dp1 : p1;
   int q1n = aq ? q1 + dq1 : q1;
   out[0] = on ? p1n : p1;
   out[1] = on ? p0n : p0;
   out[2] = on ? q0n : q0;
   out[3] = on ? q1n : q1;
}

static struct pipe_resource *
make_tex(struct pipe_screen *screen, unsigned w, unsigned h,
         enum pipe_format format)
{
   struct pipe_resource templ = {0};
   templ.target = PIPE_TEXTURE_2D;
   templ.width0 = w;
   templ.height0 = h;
   templ.depth0 = 1;
   templ.array_size = 1;
   templ.format = format;
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

/* Upload the 6-wide x N strip: row e holds edge e's p2..q2. */
static void
upload_edges(struct pipe_context *ctx, struct pipe_resource *tex,
             const int edges[N][SAMPLES])
{
   struct pipe_transfer *xfer;
   void *map = pipe_texture_map(ctx, tex, 0, 0, PIPE_MAP_WRITE,
                               0, 0, SAMPLES, N, &xfer);
   for (int e = 0; e < N; ++e) {
      float *dst = (float *)((char *)map + e * xfer->stride);
      for (int k = 0; k < SAMPLES; ++k)
         dst[k] = (float)edges[e][k];
   }
   pipe_texture_unmap(ctx, xfer);
}

static void
readback_out(struct pipe_context *ctx, struct pipe_resource *tex,
             float out[N][4])
{
   struct pipe_transfer *xfer;
   void *map = pipe_texture_map(ctx, tex, 0, 0, PIPE_MAP_READ,
                               0, 0, 1, N, &xfer);
   for (int e = 0; e < N; ++e) {
      const float *src = (const float *)((const char *)map + e * xfer->stride);
      for (int c = 0; c < 4; ++c)
         out[e][c] = src[c];
   }
   pipe_texture_unmap(ctx, xfer);
}

/* Full-screen quad over the 1-wide x N target.  VAR0 places the p0 sample at the
 * 6-wide strip's column 2 (.x = 2.5/6) with the edge-normal step .zw = (1/6, 0)
 * and the edge row in .y (interpolated 0..1, so window row == strip row); VAR1
 * carries alpha, beta, tc0. */
static void
draw_edges_quad(struct cso_context *cso, float alpha, float beta, float tc0)
{
   const float bx = 2.5f / (float)SAMPLES;
   const float sx = 1.0f / (float)SAMPLES;
   const float verts[] = {
      -1, -1, 0, 1,   bx, 0, sx, 0,   alpha, beta, tc0, 0,
      -1,  1, 0, 1,   bx, 1, sx, 0,   alpha, beta, tc0, 0,
       1,  1, 0, 1,   bx, 1, sx, 0,   alpha, beta, tc0, 0,
       1, -1, 0, 1,   bx, 0, sx, 0,   alpha, beta, tc0, 0,
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
set_framebuffer(struct cso_context *cso, struct pipe_resource *tex)
{
   struct pipe_surface surf = {{0}};
   struct pipe_framebuffer_state fb = {0};
   surf.format = tex->format;
   surf.texture = tex;
   fb.width = 1;
   fb.height = N;
   fb.nr_cbufs = 1;
   fb.cbufs[0] = surf;
   cso_set_framebuffer(cso, &fb);
}

struct deblock_pipeline {
   struct pipe_resource *in, *out;
   struct pipe_sampler_view *view_in;
   void *vs, *fs, *sampler;
};

static bool
run_params(struct pipe_context *ctx, struct cso_context *cso,
           struct deblock_pipeline *p, const int edges[N][SAMPLES],
           int alpha, int beta, int tc0)
{
   upload_edges(ctx, p->in, edges);
   cso_set_vertex_shader_handle(cso, p->vs);
   cso_set_samplers(cso, MESA_SHADER_FRAGMENT, 1,
                    (const struct pipe_sampler_state **)&p->sampler);
   set_framebuffer(cso, p->out);
   ctx->set_sampler_views(ctx, MESA_SHADER_FRAGMENT, 0, 1, 0, &p->view_in);
   cso_set_fragment_shader_handle(cso, p->fs);
   draw_edges_quad(cso, (float)alpha, (float)beta, (float)tc0);
   ctx->flush(ctx, NULL, 0);

   float got[N][4];
   readback_out(ctx, p->out, got);
   for (int e = 0; e < N; ++e) {
      int want[4];
      deblock_ref(edges[e], alpha, beta, tc0, want);
      for (int c = 0; c < 4; ++c) {
         if ((int)lroundf(got[e][c]) != want[c]) {
            printf("FAIL a=%d b=%d tc0=%d edge=%d ch=%d got %.3f want %d\n",
                   alpha, beta, tc0, e, c, got[e][c], want[c]);
            return false;
         }
      }
   }
   return true;
}

static uint32_t lcg_state;

static uint8_t
lcg_sample(void)
{
   lcg_state = lcg_state * 1664525u + 1013904223u;
   return (uint8_t)(lcg_state >> 16);
}

int
main(void)
{
   struct sw_winsys *winsys = null_sw_create();
   if (!winsys) {
      fprintf(stderr, "vl-h264-deblock: no software winsys; skipping\n");
      return 77;
   }
   struct pipe_screen *screen = softpipe_create_screen(winsys);
   if (!screen) {
      fprintf(stderr, "vl-h264-deblock: no software screen; skipping\n");
      winsys->destroy(winsys);
      return 77;
   }
   if (!screen->is_format_supported(screen, PIPE_FORMAT_R32G32B32A32_FLOAT,
                                    PIPE_TEXTURE_2D, 0, 0,
                                    PIPE_BIND_RENDER_TARGET |
                                    PIPE_BIND_SAMPLER_VIEW)) {
      fprintf(stderr, "vl-h264-deblock: RGBA32F not renderable; skipping\n");
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
   vp.scale[0] = 0.5f;
   vp.scale[1] = N / 2.0f;
   vp.scale[2] = 1.0f;
   vp.translate[0] = 0.5f;
   vp.translate[1] = N / 2.0f;
   cso_set_viewport(cso, &vp);

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

   struct deblock_pipeline p = {0};
   p.in = make_tex(screen, SAMPLES, N, PIPE_FORMAT_R32_FLOAT);
   p.out = make_tex(screen, 1, N, PIPE_FORMAT_R32G32B32A32_FLOAT);
   p.view_in = make_view(ctx, p.in);
   p.sampler = ctx->create_sampler_state(ctx, &samp);
   p.vs = util_make_vertex_passthrough_shader(ctx, 3, vs_sem, vs_idx, false);
   p.fs = vl_h264_deblock_create_luma_fs(ctx);

   /* The threshold grid crosses both the filter-on gate (small alpha/beta turn
    * it off) and the inner p1/q1 gate, over random edges. */
   static const int alphas[] = {4, 20, 80, 255};
   static const int betas[] = {2, 8, 18};
   static const int tc0s[] = {0, 1, 2, 3, 5};

   lcg_state = 0x48323634u;
   bool pass = true;
   int total = 0, ok = 0;
   for (unsigned ai = 0; ai < ARRAY_SIZE(alphas); ++ai) {
      for (unsigned bi = 0; bi < ARRAY_SIZE(betas); ++bi) {
         for (unsigned ti = 0; ti < ARRAY_SIZE(tc0s); ++ti) {
            int edges[N][SAMPLES];
            for (int e = 0; e < N; ++e)
               for (int k = 0; k < SAMPLES; ++k)
                  edges[e][k] = lcg_sample();
            ++total;
            if (run_params(ctx, cso, &p, edges, alphas[ai], betas[bi], tc0s[ti]))
               ++ok;
            else
               pass = false;
         }
      }
   }
   printf("Test(vl-h264-deblock: threshold-grid x random edges) = %s (%d/%d)\n",
          pass ? "pass" : "fail", ok, total);

   cso_unbind_context(cso);
   cso_destroy_context(cso);
   ctx->delete_sampler_state(ctx, p.sampler);
   ctx->delete_fs_state(ctx, p.fs);
   ctx->delete_vs_state(ctx, p.vs);
   pipe_sampler_view_reference(&p.view_in, NULL);
   pipe_resource_reference(&p.in, NULL);
   pipe_resource_reference(&p.out, NULL);
   ctx->destroy(ctx);
   screen->destroy(screen);
   winsys->destroy(winsys);

   printf("vl-h264-deblock: %s\n", pass ? "PASS" : "FAIL");
   return pass ? 0 : 1;
}
