/*
 * Copyright (c) 2026 Terascale Functionalists
 * SPDX-License-Identifier: MIT
 */

/*
 * Render-to-texture verification of the H.264 luma half-pel motion-compensation
 * fragment programs (vl_h264_mc.c) on a software (softpipe) screen.
 *
 * The programs run the six-tap FIR (1, -5, 20, 20, -5, 1) over six integer luma
 * samples along one axis, normalize (sum + 16) >> 5, and clip to [0, 255].  This
 * harness uploads a reference luma plane, renders the horizontal and vertical
 * half-pel planes, and checks each against an independent integer implementation
 * of the spec (ITU-T H.264 sec 8.4.2.2.1) with the same CLAMP_TO_EDGE extension
 * the sampler applies.  Softpipe computes in f32, not the r300 s1e7m16 FP24, so
 * this rung proves the NIR transcription and the filter geometry; the half-pel
 * accumulator peaks at 10710, well under 2^24, so the f32 comparison is exact.
 *
 * The oracle is discriminating: a flat plane filters to itself for any tap set
 * that sums to 32, so the fixtures include an impulse (a single bright sample
 * that reads out the tap pattern and its sign/order), a per-axis ramp (which
 * distinguishes the horizontal from the vertical program), an asymmetric plane,
 * and a random sweep -- inputs whose correct output changes if a tap is wrong or
 * an axis is swapped.
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

#include "vl_h264_mc.h"

#define W 16
#define H 16

static const int luma_6tap[6] = { 1, -5, 20, 20, -5, 1 };

static int
clampi(int v, int lo, int hi)
{
   return v < lo ? lo : (v > hi ? hi : v);
}

/* Integer reference, ITU-T H.264 sec 8.4.2.2.1, with CLAMP_TO_EDGE picture
 * extension matching the sampler.  Horizontal half-pel at column x sits between
 * integer columns x and x+1; vertical at row y between rows y and y+1. */
static int
halfpel_h_ref(const uint8_t *ref, int x, int y)
{
   int acc = 0;
   for (int tap = 0; tap < 6; ++tap)
      acc += ref[y * W + clampi(x - 2 + tap, 0, W - 1)] * luma_6tap[tap];
   return clampi((acc + 16) >> 5, 0, 255);
}

static int
halfpel_v_ref(const uint8_t *ref, int x, int y)
{
   int acc = 0;
   for (int tap = 0; tap < 6; ++tap)
      acc += ref[clampi(y - 2 + tap, 0, H - 1) * W + x] * luma_6tap[tap];
   return clampi((acc + 16) >> 5, 0, 255);
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

/* Full-screen quad over the WxH target.  The texcoord carries the sample
 * position in .xy ([0,1]) and the reference texel step (1/W, 1/H) in .zw, which
 * the fragment program walks to reach the six integer-sample centers. */
static void
draw_plane_quad(struct cso_context *cso)
{
   const float sx = 1.0f / (float)W;
   const float sy = 1.0f / (float)H;
   const float verts[] = {
      -1, -1, 0, 1,   0, 0, sx, sy,
      -1,  1, 0, 1,   0, 1, sx, sy,
       1,  1, 0, 1,   1, 1, sx, sy,
       1, -1, 0, 1,   1, 0, sx, sy,
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

struct mc_pipeline {
   struct pipe_resource *ref, *out;
   struct pipe_sampler_view *view_ref;
   void *vs, *fs_h, *fs_v, *sampler;
};

static void
run_axis(struct pipe_context *ctx, struct cso_context *cso,
         struct mc_pipeline *p, void *fs, const uint8_t *ref, float *out)
{
   upload_plane(ctx, p->ref, ref);
   cso_set_vertex_shader_handle(cso, p->vs);
   cso_set_samplers(cso, MESA_SHADER_FRAGMENT, 1,
                    (const struct pipe_sampler_state **)&p->sampler);
   set_plane_framebuffer(cso, p->out);
   ctx->set_sampler_views(ctx, MESA_SHADER_FRAGMENT, 0, 1, 0, &p->view_ref);
   cso_set_fragment_shader_handle(cso, fs);
   draw_plane_quad(cso);
   ctx->flush(ctx, NULL, 0);
   readback_plane(ctx, p->out, out);
}

static bool
check_axis(const char *name, const uint8_t *ref, const float *got,
           int (*reference)(const uint8_t *, int, int))
{
   for (int y = 0; y < H; ++y) {
      for (int x = 0; x < W; ++x) {
         int want = reference(ref, x, y);
         if ((int)lroundf(got[y * W + x]) != want) {
            printf("FAIL %s: (%d,%d) got %.3f want %d\n",
                   name, x, y, got[y * W + x], want);
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
fill_ramp_h(uint8_t *ref)
{
   for (int y = 0; y < H; ++y)
      for (int x = 0; x < W; ++x)
         ref[y * W + x] = (uint8_t)((x * 255) / (W - 1));
}

static void
fill_ramp_v(uint8_t *ref)
{
   for (int y = 0; y < H; ++y)
      for (int x = 0; x < W; ++x)
         ref[y * W + x] = (uint8_t)((y * 255) / (H - 1));
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
      fprintf(stderr, "vl-h264-mc: no software winsys; skipping\n");
      return 77;
   }
   struct pipe_screen *screen = softpipe_create_screen(winsys);
   if (!screen) {
      fprintf(stderr, "vl-h264-mc: no software screen; skipping\n");
      winsys->destroy(winsys);
      return 77;
   }
   if (!screen->is_format_supported(screen, PIPE_FORMAT_R32_FLOAT,
                                    PIPE_TEXTURE_2D, 0, 0,
                                    PIPE_BIND_RENDER_TARGET |
                                    PIPE_BIND_SAMPLER_VIEW)) {
      fprintf(stderr, "vl-h264-mc: R32_FLOAT not renderable; skipping\n");
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

   /* CLAMP_TO_EDGE matches the H.264 reference-picture edge extension. */
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

   struct mc_pipeline p = {0};
   p.ref = make_plane(screen);
   p.out = make_plane(screen);
   p.view_ref = make_view(ctx, p.ref);
   p.sampler = ctx->create_sampler_state(ctx, &samp);
   p.vs = util_make_vertex_passthrough_shader(ctx, 2, vs_sem, vs_idx, false);
   p.fs_h = vl_h264_mc_create_halfpel_h_fs(ctx);
   p.fs_v = vl_h264_mc_create_halfpel_v_fs(ctx);

   bool pass = true;
   float out[W * H];
   uint8_t ref[W * H];

   const struct {
      const char *name;
      void (*fill)(uint8_t *);
   } fixtures[] = {
      { "impulse", fill_impulse },
      { "ramp_h", fill_ramp_h },
      { "ramp_v", fill_ramp_v },
      { "asymmetric", fill_asymmetric },
   };

   for (unsigned f = 0; f < ARRAY_SIZE(fixtures); ++f) {
      fixtures[f].fill(ref);

      run_axis(ctx, cso, &p, p.fs_h, ref, out);
      bool ok_h = check_axis(fixtures[f].name, ref, out, halfpel_h_ref);
      printf("Test(vl-h264-mc: %s horizontal) = %s\n", fixtures[f].name,
             ok_h ? "pass" : "fail");

      run_axis(ctx, cso, &p, p.fs_v, ref, out);
      bool ok_v = check_axis(fixtures[f].name, ref, out, halfpel_v_ref);
      printf("Test(vl-h264-mc: %s vertical) = %s\n", fixtures[f].name,
             ok_v ? "pass" : "fail");

      pass = pass && ok_h && ok_v;
   }

   /* Random sweep over full-range 8-bit reference planes, both axes. */
   lcg_state = 0x48323634u;
   const int sweep = 64;
   int sweep_ok = 0;
   for (int s = 0; s < sweep; ++s) {
      for (int i = 0; i < W * H; ++i)
         ref[i] = lcg_sample();
      run_axis(ctx, cso, &p, p.fs_h, ref, out);
      bool ok = check_axis("random_h", ref, out, halfpel_h_ref);
      run_axis(ctx, cso, &p, p.fs_v, ref, out);
      ok = ok && check_axis("random_v", ref, out, halfpel_v_ref);
      if (ok)
         ++sweep_ok;
   }
   pass = pass && (sweep_ok == sweep);
   printf("Test(vl-h264-mc: random sweep) = %s (%d/%d)\n",
          sweep_ok == sweep ? "pass" : "fail", sweep_ok, sweep);

   cso_unbind_context(cso);
   cso_destroy_context(cso);
   ctx->delete_sampler_state(ctx, p.sampler);
   ctx->delete_fs_state(ctx, p.fs_h);
   ctx->delete_fs_state(ctx, p.fs_v);
   ctx->delete_vs_state(ctx, p.vs);
   pipe_sampler_view_reference(&p.view_ref, NULL);
   pipe_resource_reference(&p.ref, NULL);
   pipe_resource_reference(&p.out, NULL);
   ctx->destroy(ctx);
   screen->destroy(screen);
   winsys->destroy(winsys);

   printf("vl-h264-mc: %s\n", pass ? "PASS" : "FAIL");
   return pass ? 0 : 1;
}
