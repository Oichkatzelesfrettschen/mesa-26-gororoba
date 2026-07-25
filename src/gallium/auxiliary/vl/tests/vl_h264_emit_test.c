/*
 * SPDX-License-Identifier: MIT
 */

/*
 * End-to-end verification of the H.264 luma inter-reconstruction orchestrator
 * (vl_h264_emit.c) on a software (softpipe) screen.
 *
 * The orchestrator drives the per-stage fragment programs over a whole frame:
 * scatter the macroblock coefficients, motion-compensate each macroblock's
 * prediction from the reference, inverse-transform the residual, and write
 * Clip1(prediction + residual).  This harness builds a reference plane and an
 * inter slice contract, runs the orchestrator into a target plane, and checks
 * every luma sample against an independent integer implementation of the same
 * pipeline (ITU-T H.264 sec 8.4.2.2.1 motion compensation, sec 8.5.12.2 inverse
 * transform, the Clip1 reconstruction).
 *
 * Softpipe computes in f32, not the r300 s1e7m16 FP24, so this rung proves the
 * orchestration -- the coefficient scatter, the reference sampling at the motion
 * vector, the plane addressing, the texture-barrier ordering, and the
 * prediction-plus-residual combination -- not FP24 truncation, which the
 * per-stage models own.  Single-macroblock fixtures exercise the integer-pel and
 * the two axis-aligned half-pel motion positions, each with a non-trivial
 * residual; a four-macroblock fixture exercises the scatter offset and the
 * per-macroblock prediction placement at non-zero macroblock origins, with a
 * distinct motion vector and residual per macroblock.
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
#include "pipe/p_state.h"
#include "util/format/u_formats.h"

#include "frontend/sw_winsys.h"
#include "softpipe/sp_public.h"
#include "sw/null/null_sw_winsys.h"

#include "util/u_inlines.h"
#include "util/u_sampler.h"

#include "vl_h264_emit.h"
#include "vl_h264_mb_contract.h"

#define MB 16

static const int luma_6tap[6] = { 1, -5, 20, 20, -5, 1 };

static int
clampi(int v, int lo, int hi)
{
   return v < lo ? lo : (v > hi ? hi : v);
}

/* Integer inverse transform, ITU-T H.264 sec 8.5.12.2, one 16-coefficient
 * block; matches the steinmarder idct4_int oracle. */
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
idct4_int(const int16_t coeff[16], int64_t residual[16])
{
   int64_t rows[16];
   for (int r = 0; r < 4; ++r) {
      int64_t z[4], o[4];
      for (int c = 0; c < 4; ++c)
         z[c] = coeff[r * 4 + c];
      idct4_1d_int(z, o);
      for (int c = 0; c < 4; ++c)
         rows[r * 4 + c] = o[c];
   }
   for (int c = 0; c < 4; ++c) {
      int64_t z[4], o[4];
      for (int r = 0; r < 4; ++r)
         z[r] = rows[r * 4 + c];
      idct4_1d_int(z, o);
      for (int i = 0; i < 4; ++i)
         residual[i * 4 + c] = (o[i] + 32) >> 6;
   }
}

/* Integer six-tap half-pel, ITU-T H.264 sec 8.4.2.2.1, with CLAMP_TO_EDGE
 * matching the sampler.  The half-sample sits between integer position p and
 * p+1 along the filtered axis. */
static int
halfpel_h_ref(const uint8_t *ref, int rw, int rh, int x, int y)
{
   int acc = 0;
   for (int tap = 0; tap < 6; ++tap)
      acc += ref[clampi(y, 0, rh - 1) * rw + clampi(x - 2 + tap, 0, rw - 1)]
             * luma_6tap[tap];
   return clampi((acc + 16) >> 5, 0, 255);
}

static int
halfpel_v_ref(const uint8_t *ref, int rw, int rh, int x, int y)
{
   int acc = 0;
   for (int tap = 0; tap < 6; ++tap)
      acc += ref[clampi(y - 2 + tap, 0, rh - 1) * rw + clampi(x, 0, rw - 1)]
             * luma_6tap[tap];
   return clampi((acc + 16) >> 5, 0, 255);
}

static int
integer_ref(const uint8_t *ref, int rw, int rh, int x, int y)
{
   return ref[clampi(y, 0, rh - 1) * rw + clampi(x, 0, rw - 1)];
}

/* Full H.264 luma prediction (ITU-T sec 8.4.2.2.1) for the FP24-feasible
 * positions: the integer position, the two half-pel positions, and the eight
 * quarter positions that average an integer or half-pel sample with an adjacent
 * one.  The five positions whose value needs the 2D half-pel-diagonal (j)
 * overflow FP24 and are not exercised here.  The vector's integer part shifts
 * the reference origin; the fraction selects the position. */
static int
predict_qpel(const uint8_t *ref, int rw, int rh, int mb_x, int mb_y, int mvx,
             int mvy, int lx, int ly)
{
   const int xF = mvx & 3, yF = mvy & 3;
   const int X = mb_x * MB + (mvx >> 2) + lx;
   const int Y = mb_y * MB + (mvy >> 2) + ly;
   const int g = integer_ref(ref, rw, rh, X, Y);
   const int g_right = integer_ref(ref, rw, rh, X + 1, Y);
   const int g_down = integer_ref(ref, rw, rh, X, Y + 1);
   const int b = halfpel_h_ref(ref, rw, rh, X, Y);
   const int h = halfpel_v_ref(ref, rw, rh, X, Y);
   const int m = halfpel_v_ref(ref, rw, rh, X + 1, Y);
   const int s = halfpel_h_ref(ref, rw, rh, X, Y + 1);

   switch (yF * 4 + xF) {
   case 0:  return g;                      /* (0,0) integer */
   case 1:  return (g + b + 1) >> 1;       /* (1,0) a */
   case 2:  return b;                      /* (2,0) half-pel b */
   case 3:  return (b + g_right + 1) >> 1; /* (3,0) c */
   case 4:  return (g + h + 1) >> 1;       /* (0,1) d */
   case 5:  return (b + h + 1) >> 1;       /* (1,1) e */
   case 7:  return (b + m + 1) >> 1;       /* (3,1) g */
   case 8:  return h;                      /* (0,2) half-pel h */
   case 12: return (h + g_down + 1) >> 1;  /* (0,3) n */
   case 13: return (h + s + 1) >> 1;       /* (1,3) p */
   case 15: return (m + s + 1) >> 1;       /* (3,3) r */
   default: return g;                      /* j-dependent positions: unsupported */
   }
}

static struct pipe_resource *
make_plane(struct pipe_screen *screen, unsigned w, unsigned h)
{
   struct pipe_resource templ = {0};
   templ.target = PIPE_TEXTURE_2D;
   templ.width0 = w;
   templ.height0 = h;
   templ.depth0 = 1;
   templ.array_size = 1;
   templ.format = PIPE_FORMAT_R32_FLOAT;
   templ.usage = PIPE_USAGE_DEFAULT;
   templ.bind = PIPE_BIND_SAMPLER_VIEW | PIPE_BIND_RENDER_TARGET;
   return screen->resource_create(screen, &templ);
}

static void
upload_ref(struct pipe_context *ctx, struct pipe_resource *tex,
           const uint8_t *ref, unsigned rw, unsigned rh)
{
   struct pipe_transfer *xfer;
   void *map = pipe_texture_map(ctx, tex, 0, 0, PIPE_MAP_WRITE, 0, 0, rw, rh,
                               &xfer);
   for (unsigned row = 0; row < rh; ++row) {
      float *dst = (float *)((char *)map + row * xfer->stride);
      for (unsigned col = 0; col < rw; ++col)
         dst[col] = (float)ref[row * rw + col];
   }
   pipe_texture_unmap(ctx, xfer);
}

static void
readback(struct pipe_context *ctx, struct pipe_resource *tex, float *out,
         unsigned w, unsigned h)
{
   struct pipe_transfer *xfer;
   void *map = pipe_texture_map(ctx, tex, 0, 0, PIPE_MAP_READ, 0, 0, w, h,
                               &xfer);
   for (unsigned row = 0; row < h; ++row) {
      const float *src = (const float *)((const char *)map + row * xfer->stride);
      for (unsigned col = 0; col < w; ++col)
         out[row * w + col] = src[col];
   }
   pipe_texture_unmap(ctx, xfer);
}

/* Distinct asymmetric per-block content keyed by a seed, bounded so the residual
 * lands in a range that exercises the Clip1 on both ends without saturating
 * everywhere. */
static void
fill_block_coeffs(struct vl_h264_mb_contract *mb, bool with_residual, int seed)
{
   memset(mb->coeff4x4, 0, sizeof(mb->coeff4x4));
   if (!with_residual)
      return;
   for (int blk = 0; blk < VL_H264_LUMA_4X4_BLOCKS; ++blk)
      for (int k = 0; k < 16; ++k)
         mb->coeff4x4[blk][k] =
            (int16_t)(((blk * 7 + k * 13 + seed * 5) % 61) - 30);
}

struct mb_spec {
   int mb_x, mb_y;
   int16_t mvx, mvy;
   bool residual;
   int seed;
   bool submb;   /* give each 4x4 luma block a distinct vector */
};

/* A distinct per-block vector for the sub-macroblock case, varied enough that
 * adjacent blocks differ in both integer and fractional parts; a per-block walk
 * that mistakenly reused one vector for the whole macroblock would diverge. */
static void
submb_vector(int blk, int16_t *mvx, int16_t *mvy)
{
   *mvx = (int16_t)(blk * 3 - 8);
   *mvy = (int16_t)(blk * 2 - 6);
}

static bool
run_frame(struct vl_h264_emit *emit, struct pipe_context *ctx,
          struct pipe_screen *screen, const char *name, const uint8_t *ref,
          unsigned rw, unsigned rh, const struct mb_spec *mbs, unsigned n_mbs,
          unsigned fcols, unsigned frows)
{
   const unsigned fw = fcols * MB, fh = frows * MB;

   struct pipe_resource *ref_tex = make_plane(screen, rw, rh);
   struct pipe_resource *dst = make_plane(screen, fw, fh);
   upload_ref(ctx, ref_tex, ref, rw, rh);

   struct pipe_sampler_view *ref_view;
   {
      struct pipe_sampler_view templ;
      u_sampler_view_default_template(&templ, ref_tex, ref_tex->format);
      ref_view = ctx->create_sampler_view(ctx, ref_tex, &templ);
   }
   struct pipe_surface dst_surf = {{0}};
   dst_surf.format = dst->format;
   dst_surf.texture = dst;

   struct vl_h264_mb_contract *contract =
      calloc(n_mbs, sizeof(*contract));
   for (unsigned m = 0; m < n_mbs; ++m) {
      contract[m].mb_x = mbs[m].mb_x;
      contract[m].mb_y = mbs[m].mb_y;
      contract[m].slice_type = VL_H264_SLICE_P;
      for (int i = 0; i < 16; ++i) {
         if (mbs[m].submb)
            submb_vector(i, &contract[m].mv_l0[i][0], &contract[m].mv_l0[i][1]);
         else {
            contract[m].mv_l0[i][0] = mbs[m].mvx;
            contract[m].mv_l0[i][1] = mbs[m].mvy;
         }
         contract[m].ref_l0[i] = 0;
         contract[m].ref_l1[i] = -1;
      }
      fill_block_coeffs(&contract[m], mbs[m].residual, mbs[m].seed);
   }

   struct vl_h264_slice_contract slice = {0};
   slice.version = VL_H264_MB_CONTRACT_VERSION;
   slice.width = fw;
   slice.height = fh;
   slice.slice_type = VL_H264_SLICE_P;
   slice.num_macroblocks = n_mbs;
   slice.macroblocks = contract;

   vl_h264_emit_luma_inter(emit, &dst_surf, fw, fh, ref_view, rw, rh, &slice);

   float *out = malloc(fw * fh * sizeof(float));
   readback(ctx, dst, out, fw, fh);

   bool ok = true;
   for (unsigned m = 0; m < n_mbs && ok; ++m) {
      const struct mb_spec *s = &mbs[m];
      for (int ly = 0; ly < MB && ok; ++ly) {
         for (int lx = 0; lx < MB; ++lx) {
            int px = s->mb_x * MB + lx;
            int py = s->mb_y * MB + ly;
            int blk = (ly / 4) * 4 + (lx / 4);
            int pred = predict_qpel(ref, rw, rh, s->mb_x, s->mb_y,
                                    contract[m].mv_l0[blk][0],
                                    contract[m].mv_l0[blk][1], lx, ly);
            int64_t res[16];
            idct4_int(contract[m].coeff4x4[blk], res);
            int want = clampi(pred + (int)res[(ly % 4) * 4 + (lx % 4)], 0, 255);
            if ((int)lroundf(out[py * fw + px]) != want) {
               printf("FAIL %s: mb(%d,%d) (%d,%d) got %.3f want %d\n", name,
                      s->mb_x, s->mb_y, lx, ly, out[py * fw + px], want);
               ok = false;
               break;
            }
         }
      }
   }

   free(out);
   free(contract);
   pipe_sampler_view_reference(&ref_view, NULL);
   pipe_resource_reference(&ref_tex, NULL);
   pipe_resource_reference(&dst, NULL);

   printf("Test(vl-h264-emit: %s) = %s\n", name, ok ? "pass" : "fail");
   return ok;
}

int
main(void)
{
   struct sw_winsys *winsys = null_sw_create();
   if (!winsys) {
      fprintf(stderr, "vl-h264-emit: no software winsys; skipping\n");
      return 77;
   }
   struct pipe_screen *screen = softpipe_create_screen(winsys);
   if (!screen) {
      fprintf(stderr, "vl-h264-emit: no software screen; skipping\n");
      winsys->destroy(winsys);
      return 77;
   }
   if (!screen->is_format_supported(screen, PIPE_FORMAT_R32_FLOAT,
                                    PIPE_TEXTURE_2D, 0, 0,
                                    PIPE_BIND_RENDER_TARGET |
                                    PIPE_BIND_SAMPLER_VIEW)) {
      fprintf(stderr, "vl-h264-emit: R32_FLOAT not renderable; skipping\n");
      screen->destroy(screen);
      winsys->destroy(winsys);
      return 77;
   }

   struct pipe_context *ctx = screen->context_create(screen, NULL, 0);
   struct vl_h264_emit *emit = vl_h264_emit_create(ctx);

   /* A reference plane large enough for a 2x2-macroblock frame plus motion;
    * both-axis structure makes a horizontal-vs-vertical half-pel swap or an axis
    * transpose observable. */
   const unsigned rw = 64, rh = 64;
   uint8_t *ref = malloc(rw * rh);
   for (unsigned y = 0; y < rh; ++y)
      for (unsigned x = 0; x < rw; ++x)
         ref[y * rw + x] = (uint8_t)((x * 9 + y * 5 + 30) % 256);

   bool pass = true;

   /* Single-macroblock fixtures: each motion position, with and without a
    * residual.  The macroblock is at the origin so this isolates the motion and
    * residual paths from the placement. */
   const struct {
      const char *name;
      int16_t mvx, mvy;
      bool residual;
   } single[] = {
      { "copy_zero_mv_no_resid",  0,  0, false },
      { "copy_int_mv_resid",     16,  8, true  },
      { "halfpel_h_resid",        2,  0, true  },
      { "halfpel_h_shift_resid", 10,  0, true  },
      { "halfpel_v_resid",        0,  2, true  },
      { "neg_mv_resid",          -8, -4, true  },
      /* The eight FP24-feasible quarter-pel positions: the axis quarters and the
       * four corner (diagonal) quarters, with an integer offset on a few. */
      { "qpel_a_resid",           5,  0, true  },   /* (1,0) a, +1 int x */
      { "qpel_c_resid",           3,  0, true  },   /* (3,0) c */
      { "qpel_d_resid",           0,  5, true  },   /* (0,1) d, +1 int y */
      { "qpel_e_resid",           1,  1, true  },   /* (1,1) e corner */
      { "qpel_g_resid",           3,  1, true  },   /* (3,1) g corner */
      { "qpel_n_resid",           0,  3, true  },   /* (0,3) n */
      { "qpel_p_resid",           1,  3, true  },   /* (1,3) p corner */
      { "qpel_r_resid",           3,  3, true  },   /* (3,3) r corner */
      { "qpel_neg_corner_resid", -7, -7, true  },   /* (1,1) e, negative floor */
   };
   for (unsigned f = 0; f < ARRAY_SIZE(single); ++f) {
      struct mb_spec mb = { 0, 0, single[f].mvx, single[f].mvy,
                            single[f].residual, (int)f + 1 };
      pass = run_frame(emit, ctx, screen, single[f].name, ref, rw, rh, &mb, 1,
                       1, 1) && pass;
   }

   /* Four-macroblock frame: distinct motion and residual per macroblock at four
    * different origins, so the scatter offset and the per-macroblock prediction
    * placement are exercised, not just the (0,0) macroblock. */
   const struct mb_spec quad[] = {
      { 0, 0,   5,  1, true, 11 },   /* (1,1) e corner quarter-pel, +1 int x */
      { 1, 0,   3,  0, true, 22 },   /* (3,0) c quarter-pel */
      { 0, 1,   1,  3, true, 33 },   /* (1,3) p corner quarter-pel */
      { 1, 1,  -7, -5, true, 44 },   /* (1,3) p, negative floor */
   };
   pass = run_frame(emit, ctx, screen, "quad_mb_frame", ref, rw, rh, quad,
                    ARRAY_SIZE(quad), 2, 2) && pass;

   /* Sub-macroblock partition: every 4x4 luma block carries its own vector, so
    * the prediction is motion-compensated block by block rather than as one
    * 16x16 partition.  A single-MB frame and a 2x2 frame, each with a residual. */
   const struct mb_spec submb_single[] = {
      { 0, 0, 0, 0, true, 55, true },
   };
   pass = run_frame(emit, ctx, screen, "submb_per_block_mv", ref, rw, rh,
                    submb_single, 1, 1, 1) && pass;

   const struct mb_spec submb_quad[] = {
      { 0, 0, 0, 0, true, 61, true },
      { 1, 0, 0, 0, true, 62, true },
      { 0, 1, 0, 0, true, 63, true },
      { 1, 1, 0, 0, true, 64, true },
   };
   pass = run_frame(emit, ctx, screen, "submb_quad_frame", ref, rw, rh,
                    submb_quad, ARRAY_SIZE(submb_quad), 2, 2) && pass;

   vl_h264_emit_destroy(emit);
   free(ref);
   ctx->destroy(ctx);
   screen->destroy(screen);
   winsys->destroy(winsys);

   printf("vl-h264-emit: %s\n", pass ? "PASS" : "FAIL");
   return pass ? 0 : 1;
}
