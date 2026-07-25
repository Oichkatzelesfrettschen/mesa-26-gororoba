/*
 * SPDX-License-Identifier: MIT
 */

/*
 * Render-to-texture verification of the H.264 sample-reconstruction fragment
 * program (vl_h264_reconstruct.c) on a software (softpipe) screen.
 *
 * The program reads a prediction plane and a signed residual plane and writes
 * Clip1(prediction + residual) (ITU-T H.264 sec 8.5.x).  This harness uploads
 * both planes, renders the reconstructed plane, and checks it against an
 * independent integer implementation, sweeping inputs that exercise the clip on
 * both ends (a residual driving the sum below 0 and above 255).
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

#include "vl_h264_reconstruct.h"

#define W 16
#define H 16

static int
clampi(int v, int lo, int hi)
{
   return v < lo ? lo : (v > hi ? hi : v);
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
             const int *vals)
{
   struct pipe_transfer *xfer;
   void *map = pipe_texture_map(ctx, tex, 0, 0, PIPE_MAP_WRITE,
                               0, 0, W, H, &xfer);
   for (int row = 0; row < H; ++row) {
      float *dst = (float *)((char *)map + row * xfer->stride);
      for (int col = 0; col < W; ++col)
         dst[col] = (float)vals[row * W + col];
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

static void
draw_quad(struct cso_context *cso)
{
   const float verts[] = {
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
set_framebuffer(struct cso_context *cso, struct pipe_resource *tex)
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

static uint32_t lcg_state;

static int
lcg_next(void)
{
   lcg_state = lcg_state * 1664525u + 1013904223u;
   return (int)(lcg_state >> 13);
}

int
main(void)
{
   struct sw_winsys *winsys = null_sw_create();
   if (!winsys) {
      fprintf(stderr, "vl-h264-reconstruct: no software winsys; skipping\n");
      return 77;
   }
   struct pipe_screen *screen = softpipe_create_screen(winsys);
   if (!screen) {
      fprintf(stderr, "vl-h264-reconstruct: no software screen; skipping\n");
      winsys->destroy(winsys);
      return 77;
   }
   if (!screen->is_format_supported(screen, PIPE_FORMAT_R32_FLOAT,
                                    PIPE_TEXTURE_2D, 0, 0,
                                    PIPE_BIND_RENDER_TARGET |
                                    PIPE_BIND_SAMPLER_VIEW)) {
      fprintf(stderr, "vl-h264-reconstruct: R32_FLOAT not renderable; skip\n");
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
   void *sampler = ctx->create_sampler_state(ctx, &samp);
   void *samplers[2] = { sampler, sampler };

   static const enum tgsi_semantic vs_sem[] = {
      TGSI_SEMANTIC_POSITION, TGSI_SEMANTIC_GENERIC,
   };
   static const unsigned vs_idx[] = {0, 0};

   struct pipe_resource *pred = make_plane(screen);
   struct pipe_resource *resid = make_plane(screen);
   struct pipe_resource *out = make_plane(screen);
   struct pipe_sampler_view *views[2] = {
      make_view(ctx, pred), make_view(ctx, resid),
   };
   void *vs = util_make_vertex_passthrough_shader(ctx, 2, vs_sem, vs_idx, false);
   void *fs = vl_h264_reconstruct_create_fs(ctx);

   cso_set_vertex_shader_handle(cso, vs);
   cso_set_samplers(cso, MESA_SHADER_FRAGMENT, 2,
                    (const struct pipe_sampler_state **)samplers);
   cso_set_fragment_shader_handle(cso, fs);

   lcg_state = 0x48323634u;
   bool pass = true;
   int p[W * H], r[W * H];
   float got[W * H];

   const int sweep = 256;
   int ok = 0;
   for (int s = 0; s < sweep; ++s) {
      for (int i = 0; i < W * H; ++i) {
         p[i] = lcg_next() & 0xff;          /* prediction 0..255 */
         r[i] = (lcg_next() % 511) - 255;   /* residual -255..255 */
      }
      upload_plane(ctx, pred, p);
      upload_plane(ctx, resid, r);
      set_framebuffer(cso, out);
      ctx->set_sampler_views(ctx, MESA_SHADER_FRAGMENT, 0, 2, 0, views);
      draw_quad(cso);
      ctx->flush(ctx, NULL, 0);
      readback_plane(ctx, out, got);

      bool block_ok = true;
      for (int i = 0; i < W * H && block_ok; ++i) {
         int want = clampi(p[i] + r[i], 0, 255);
         if ((int)lroundf(got[i]) != want) {
            printf("FAIL sweep %d idx %d: pred %d resid %d got %.3f want %d\n",
                   s, i, p[i], r[i], got[i], want);
            block_ok = false;
         }
      }
      if (block_ok)
         ++ok;
      else
         pass = false;
   }
   printf("Test(vl-h264-reconstruct: clip sweep) = %s (%d/%d)\n",
          pass ? "pass" : "fail", ok, sweep);

   cso_unbind_context(cso);
   cso_destroy_context(cso);
   ctx->delete_sampler_state(ctx, sampler);
   ctx->delete_fs_state(ctx, fs);
   ctx->delete_vs_state(ctx, vs);
   pipe_sampler_view_reference(&views[0], NULL);
   pipe_sampler_view_reference(&views[1], NULL);
   pipe_resource_reference(&pred, NULL);
   pipe_resource_reference(&resid, NULL);
   pipe_resource_reference(&out, NULL);
   ctx->destroy(ctx);
   screen->destroy(screen);
   winsys->destroy(winsys);

   printf("vl-h264-reconstruct: %s\n", pass ? "PASS" : "FAIL");
   return pass ? 0 : 1;
}
