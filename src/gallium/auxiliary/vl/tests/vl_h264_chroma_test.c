/*
 * SPDX-License-Identifier: MIT
 */

/*
 * Render-to-texture verification of the H.264 chroma eighth-pel bilinear
 * motion-compensation fragment program (vl_h264_chroma.c) on a software
 * (softpipe) screen.
 *
 * The program blends the 2x2 integer-sample neighborhood with the eighth-pel
 * weights ((8-xF)(8-yF), xF(8-yF), (8-xF)yF, xF*yF), sums, and normalizes
 * (acc + 32) >> 6 (ITU-T H.264 sec 8.4.2.2.2).  This harness uploads a reference
 * chroma plane, renders the interpolated plane for a given fraction, and checks
 * it against an independent integer implementation with the same CLAMP_TO_EDGE
 * extension.  Softpipe computes in f32, not FP24; the accumulator peaks at
 * 64*255 = 16320 << 2^24, so the comparison is exact.
 *
 * The oracle is discriminating: every fraction is swept over an impulse plane
 * (a single bright sample reads out which weight pairs with which neighbor and
 * where the neighbors sit) and an asymmetric plane, plus a random sweep over
 * plane and fraction -- inputs whose correct output changes if a weight is
 * misassigned or a neighbor offset is wrong.
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

#include "vl_h264_chroma.h"

#define W 16
#define H 16

static int
clampi(int v, int lo, int hi)
{
   return v < lo ? lo : (v > hi ? hi : v);
}

/* Integer reference, ITU-T H.264 sec 8.4.2.2.2, with CLAMP_TO_EDGE picture
 * extension matching the sampler.  A is the integer sample at (x,y); B, C, D are
 * one texel right, down, and diagonal. */
static int
chroma_ref(const uint8_t *ref, int x, int y, int xf, int yf)
{
   int a = ref[clampi(y, 0, H - 1) * W + clampi(x, 0, W - 1)];
   int b = ref[clampi(y, 0, H - 1) * W + clampi(x + 1, 0, W - 1)];
   int c = ref[clampi(y + 1, 0, H - 1) * W + clampi(x, 0, W - 1)];
   int d = ref[clampi(y + 1, 0, H - 1) * W + clampi(x + 1, 0, W - 1)];
   int acc = (8 - xf) * (8 - yf) * a + xf * (8 - yf) * b
             + (8 - xf) * yf * c + xf * yf * d;
   return (acc + 32) >> 6;
}

static struct pipe_resource *
make_plane(struct pipe_screen *screen)
{
   struct pipe_resource templ = {0};
   templ.target = PIPE_TEXTURE_2D;
   templ.width0 = W;
   templ.height0 = H;
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
             const uint8_t *ref)
{
   struct pipe_transfer *xfer;
   void *map = pipe_texture_map(ctx, tex, 0, 0, PIPE_MAP_WRITE,
                               0, 0, W, H, &xfer);
   for (int row = 0; row < H; ++row) {
      float *dst = (float *)((char *)map + row * xfer->stride);
      for (int col = 0; col < W; ++col)
         dst[col] = (float)ref[row * W + col];
   }
   pipe_texture_unmap(ctx, xfer);
}

static void
readback_plane(struct pipe_context *ctx, struct pipe_resource *tex, float *out)
{
   struct pipe_transfer *xfer;
   void *map = pipe_texture_map(ctx, tex, 0, 0, PIPE_MAP_READ,
                               0, 0, W, H, &xfer);
   for (int row = 0; row < H; ++row) {
      const float *src = (const float *)((const char *)map + row * xfer->stride);
      for (int col = 0; col < W; ++col)
         out[row * W + col] = src[col];
   }
   pipe_texture_unmap(ctx, xfer);
}

/* Full-screen quad over the WxH target.  Three interleaved attributes: position,
 * a texcoord (sample position .xy, reference texel step .zw), and the four
 * eighth-pel weights, constant across the quad for the current fraction. */
static void
draw_chroma_quad(struct cso_context *cso, const float w[4])
{
   const float sx = 1.0f / (float)W;
   const float sy = 1.0f / (float)H;
   const float verts[] = {
      -1, -1, 0, 1,   0, 0, sx, sy,   w[0], w[1], w[2], w[3],
      -1,  1, 0, 1,   0, 1, sx, sy,   w[0], w[1], w[2], w[3],
       1,  1, 0, 1,   1, 1, sx, sy,   w[0], w[1], w[2], w[3],
       1, -1, 0, 1,   1, 0, sx, sy,   w[0], w[1], w[2], w[3],
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
set_plane_framebuffer(struct cso_context *cso, struct pipe_resource *tex)
{
   struct pipe_surface surf = {{0}};
   struct pipe_framebuffer_state fb = {0};
   surf.format = tex->format;
   surf.texture = tex;
   fb.width = W;
   fb.height = H;
   fb.nr_cbufs = 1;
   fb.cbufs[0] = surf;
   cso_set_framebuffer(cso, &fb);
}

struct chroma_pipeline {
   struct pipe_resource *ref, *out;
   struct pipe_sampler_view *view_ref;
   void *vs, *fs, *sampler;
};

static bool
run_fraction(struct pipe_context *ctx, struct cso_context *cso,
             struct chroma_pipeline *p, const uint8_t *ref, int xf, int yf)
{
   const float w[4] = {
      (float)((8 - xf) * (8 - yf)), (float)(xf * (8 - yf)),
      (float)((8 - xf) * yf), (float)(xf * yf),
   };
   upload_plane(ctx, p->ref, ref);
   cso_set_vertex_shader_handle(cso, p->vs);
   cso_set_samplers(cso, MESA_SHADER_FRAGMENT, 1,
                    (const struct pipe_sampler_state **)&p->sampler);
   set_plane_framebuffer(cso, p->out);
   ctx->set_sampler_views(ctx, MESA_SHADER_FRAGMENT, 0, 1, 0, &p->view_ref);
   cso_set_fragment_shader_handle(cso, p->fs);
   draw_chroma_quad(cso, w);
   ctx->flush(ctx, NULL, 0);

   float out[W * H];
   readback_plane(ctx, p->out, out);
   for (int y = 0; y < H; ++y) {
      for (int x = 0; x < W; ++x) {
         int want = chroma_ref(ref, x, y, xf, yf);
         if ((int)lroundf(out[y * W + x]) != want) {
            printf("FAIL xf=%d yf=%d (%d,%d) got %.3f want %d\n",
                   xf, yf, x, y, out[y * W + x], want);
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

static void
fill_impulse(uint8_t *ref)
{
   memset(ref, 0, W * H);
   ref[(H / 2) * W + (W / 2)] = 255;
}

static void
fill_asymmetric(uint8_t *ref)
{
   for (int y = 0; y < H; ++y)
      for (int x = 0; x < W; ++x)
         ref[y * W + x] = (uint8_t)((x * 17 + y * 53 + 11) & 0xff);
}

int
main(void)
{
   struct sw_winsys *winsys = null_sw_create();
   if (!winsys) {
      fprintf(stderr, "vl-h264-chroma: no software winsys; skipping\n");
      return 77;
   }
   struct pipe_screen *screen = softpipe_create_screen(winsys);
   if (!screen) {
      fprintf(stderr, "vl-h264-chroma: no software screen; skipping\n");
      winsys->destroy(winsys);
      return 77;
   }
   if (!screen->is_format_supported(screen, PIPE_FORMAT_R32_FLOAT,
                                    PIPE_TEXTURE_2D, 0, 0,
                                    PIPE_BIND_RENDER_TARGET |
                                    PIPE_BIND_SAMPLER_VIEW)) {
      fprintf(stderr, "vl-h264-chroma: R32_FLOAT not renderable; skipping\n");
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
   vp.scale[0] = W / 2.0f;
   vp.scale[1] = H / 2.0f;
   vp.scale[2] = 1.0f;
   vp.translate[0] = W / 2.0f;
   vp.translate[1] = H / 2.0f;
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

   struct chroma_pipeline p = {0};
   p.ref = make_plane(screen);
   p.out = make_plane(screen);
   p.view_ref = make_view(ctx, p.ref);
   p.sampler = ctx->create_sampler_state(ctx, &samp);
   p.vs = util_make_vertex_passthrough_shader(ctx, 3, vs_sem, vs_idx, false);
   p.fs = vl_h264_chroma_create_bilinear_fs(ctx);

   bool pass = true;
   uint8_t ref[W * H];

   /* Both named patterns swept over all 64 eighth-pel fractions. */
   const struct {
      const char *name;
      void (*fill)(uint8_t *);
   } fixtures[] = {
      { "impulse", fill_impulse },
      { "asymmetric", fill_asymmetric },
   };
   for (unsigned f = 0; f < ARRAY_SIZE(fixtures); ++f) {
      fixtures[f].fill(ref);
      bool ok = true;
      for (int xf = 0; xf < 8; ++xf)
         for (int yf = 0; yf < 8; ++yf)
            ok = run_fraction(ctx, cso, &p, ref, xf, yf) && ok;
      pass = pass && ok;
      printf("Test(vl-h264-chroma: %s all-fractions) = %s\n",
             fixtures[f].name, ok ? "pass" : "fail");
   }

   /* Random sweep over plane and fraction. */
   lcg_state = 0x48323634u;
   const int sweep = 256;
   int sweep_ok = 0;
   for (int s = 0; s < sweep; ++s) {
      for (int i = 0; i < W * H; ++i)
         ref[i] = lcg_sample();
      int xf = (int)(lcg_sample() & 7);
      int yf = (int)(lcg_sample() & 7);
      if (run_fraction(ctx, cso, &p, ref, xf, yf))
         ++sweep_ok;
   }
   pass = pass && (sweep_ok == sweep);
   printf("Test(vl-h264-chroma: random sweep) = %s (%d/%d)\n",
          sweep_ok == sweep ? "pass" : "fail", sweep_ok, sweep);

   cso_unbind_context(cso);
   cso_destroy_context(cso);
   ctx->delete_sampler_state(ctx, p.sampler);
   ctx->delete_fs_state(ctx, p.fs);
   ctx->delete_vs_state(ctx, p.vs);
   pipe_sampler_view_reference(&p.view_ref, NULL);
   pipe_resource_reference(&p.ref, NULL);
   pipe_resource_reference(&p.out, NULL);
   ctx->destroy(ctx);
   screen->destroy(screen);
   winsys->destroy(winsys);

   printf("vl-h264-chroma: %s\n", pass ? "PASS" : "FAIL");
   return pass ? 0 : 1;
}
