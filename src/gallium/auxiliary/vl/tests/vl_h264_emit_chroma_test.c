/*
 * SPDX-License-Identifier: MIT
 */

/*
 * Verification of the H.264 chroma inter-reconstruction core
 * (vl_h264_emit_chroma_inter) on a software (softpipe) screen.
 *
 * The chroma core mirrors the luma one over half-resolution 8x8-per-macroblock
 * planes: scatter one component's 4x4 blocks, motion-compensate each
 * macroblock's 8x8 prediction with the eighth-pel bilinear kernel, inverse-
 * transform the residual, and write Clip1(prediction + residual).  This harness
 * builds a reference chroma plane and an inter slice contract, reconstructs the
 * Cb component, and checks every sample against an independent integer
 * implementation of the same pipeline (ITU-T H.264 sec 8.4.2.2.2 chroma
 * interpolation, sec 8.5.12.2 inverse transform, the Clip1 reconstruction).
 *
 * The chroma motion vector is the luma list-0 vector read in eighth-chroma-sample
 * units: integer part >> 3 shifts the read origin, fraction & 7 selects the
 * weights.  Softpipe is f32, so this proves the orchestration -- the block
 * scatter, the chroma vector derivation, the per-component placement -- not FP24
 * truncation.  The fixtures exercise integer and fractional motion (including a
 * diagonal fraction and a negative vector whose integer part floors) and a
 * four-macroblock frame that exercises the scatter offset and placement.
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

#define CHROMA_MB 8
#define CB_BLOCK_BASE 16

static int
clampi(int v, int lo, int hi)
{
   return v < lo ? lo : (v > hi ? hi : v);
}

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

/* Integer eighth-pel chroma bilinear, ITU-T H.264 sec 8.4.2.2.2, with
 * CLAMP_TO_EDGE matching the sampler.  The chroma vector is the luma vector in
 * eighth-chroma units: integer offset >> 3, fraction & 7. */
static int
predict_chroma(const uint8_t *ref, int rw, int rh, int mb_x, int mb_y, int mvx,
               int mvy, int lx, int ly)
{
   const int xF = mvx & 7, yF = mvy & 7;
   const int x = mb_x * CHROMA_MB + lx + (mvx >> 3);
   const int y = mb_y * CHROMA_MB + ly + (mvy >> 3);
   const int a = ref[clampi(y, 0, rh - 1) * rw + clampi(x, 0, rw - 1)];
   const int b = ref[clampi(y, 0, rh - 1) * rw + clampi(x + 1, 0, rw - 1)];
   const int c = ref[clampi(y + 1, 0, rh - 1) * rw + clampi(x, 0, rw - 1)];
   const int d = ref[clampi(y + 1, 0, rh - 1) * rw + clampi(x + 1, 0, rw - 1)];
   const int acc = (8 - xF) * (8 - yF) * a + xF * (8 - yF) * b
                 + (8 - xF) * yF * c + xF * yF * d;
   return (acc + 32) >> 6;
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

/* Distinct asymmetric per-block content for the Cb blocks (contract indices
 * 16..19), bounded so the residual exercises the Clip1 without saturating. */
static void
fill_cb_coeffs(struct vl_h264_mb_contract *mb, bool with_residual, int seed)
{
   memset(mb->coeff4x4, 0, sizeof(mb->coeff4x4));
   if (!with_residual)
      return;
   for (int blk = 0; blk < 4; ++blk)
      for (int k = 0; k < 16; ++k)
         mb->coeff4x4[CB_BLOCK_BASE + blk][k] =
            (int16_t)(((blk * 7 + k * 13 + seed * 5) % 61) - 30);
}

struct mb_spec {
   int mb_x, mb_y;
   int16_t mvx, mvy;
   bool residual;
   int seed;
   bool submb;   /* give each 4x4 luma block a distinct vector */
};

/* A distinct per-block luma vector for the sub-macroblock case; its eighth-pel
 * chroma derivation differs block to block. */
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
   const unsigned fw = fcols * CHROMA_MB, fh = frows * CHROMA_MB;

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

   struct vl_h264_mb_contract *contract = calloc(n_mbs, sizeof(*contract));
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
      fill_cb_coeffs(&contract[m], mbs[m].residual, mbs[m].seed);
   }

   struct vl_h264_slice_contract slice = {0};
   slice.version = VL_H264_MB_CONTRACT_VERSION;
   slice.width = fcols * 16;
   slice.height = frows * 16;
   slice.slice_type = VL_H264_SLICE_P;
   slice.num_macroblocks = n_mbs;
   slice.macroblocks = contract;

   vl_h264_emit_chroma_inter(emit, &dst_surf, fw, fh, ref_view, rw, rh, &slice,
                             CB_BLOCK_BASE);

   float *out = malloc(fw * fh * sizeof(float));
   readback(ctx, dst, out, fw, fh);

   bool ok = true;
   for (unsigned m = 0; m < n_mbs && ok; ++m) {
      const struct mb_spec *s = &mbs[m];
      for (int ly = 0; ly < CHROMA_MB && ok; ++ly) {
         for (int lx = 0; lx < CHROMA_MB; ++lx) {
            int px = s->mb_x * CHROMA_MB + lx;
            int py = s->mb_y * CHROMA_MB + ly;
            /* The chroma sample at (lx,ly) is co-located with luma block
             * (ly/2)*4 + (lx/2) and uses that block's vector. */
            int mv_blk = (ly / 2) * 4 + (lx / 2);
            int pred = predict_chroma(ref, rw, rh, s->mb_x, s->mb_y,
                                      contract[m].mv_l0[mv_blk][0],
                                      contract[m].mv_l0[mv_blk][1], lx, ly);
            int blk = (ly / 4) * 2 + (lx / 4);
            int64_t res[16];
            idct4_int(contract[m].coeff4x4[CB_BLOCK_BASE + blk], res);
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

   printf("Test(vl-h264-emit-chroma: %s) = %s\n", name, ok ? "pass" : "fail");
   return ok;
}

int
main(void)
{
   struct sw_winsys *winsys = null_sw_create();
   if (!winsys) {
      fprintf(stderr, "vl-h264-emit-chroma: no software winsys; skipping\n");
      return 77;
   }
   struct pipe_screen *screen = softpipe_create_screen(winsys);
   if (!screen) {
      fprintf(stderr, "vl-h264-emit-chroma: no software screen; skipping\n");
      winsys->destroy(winsys);
      return 77;
   }
   if (!screen->is_format_supported(screen, PIPE_FORMAT_R32_FLOAT,
                                    PIPE_TEXTURE_2D, 0, 0,
                                    PIPE_BIND_RENDER_TARGET |
                                    PIPE_BIND_SAMPLER_VIEW)) {
      fprintf(stderr,
              "vl-h264-emit-chroma: R32_FLOAT not renderable; skipping\n");
      screen->destroy(screen);
      winsys->destroy(winsys);
      return 77;
   }

   struct pipe_context *ctx = screen->context_create(screen, NULL, 0);
   struct vl_h264_emit *emit = vl_h264_emit_create(ctx);

   const unsigned rw = 32, rh = 32;
   uint8_t *ref = malloc(rw * rh);
   for (unsigned y = 0; y < rh; ++y)
      for (unsigned x = 0; x < rw; ++x)
         ref[y * rw + x] = (uint8_t)((x * 11 + y * 7 + 40) % 256);

   bool pass = true;

   /* Single-macroblock fixtures.  The luma vector is in quarter-pel; the chroma
    * vector is that value in eighth-chroma units (integer >> 3, fraction & 7). */
   const struct {
      const char *name;
      int16_t mvx, mvy;
      bool residual;
   } single[] = {
      { "zero_mv_no_resid",   0,  0, false },
      { "int_mv_resid",      16,  8, true  },   /* chroma int (2,1), frac (0,0) */
      { "frac_x_resid",       3,  0, true  },   /* chroma frac (3,0) */
      { "frac_diag_resid",    5,  3, true  },   /* chroma frac (5,3) */
      { "neg_mv_resid",      -5, -8, true  },   /* floor: int (-1,-1) frac (3,0) */
   };
   for (unsigned f = 0; f < ARRAY_SIZE(single); ++f) {
      struct mb_spec mb = { 0, 0, single[f].mvx, single[f].mvy,
                            single[f].residual, (int)f + 1 };
      pass = run_frame(emit, ctx, screen, single[f].name, ref, rw, rh, &mb, 1, 1,
                       1) && pass;
   }

   /* Four-macroblock frame: distinct chroma motion and residual per macroblock
    * at four origins, exercising the scatter offset and per-macroblock
    * placement. */
   const struct mb_spec quad[] = {
      { 0, 0,   8,  0, true, 11 },
      { 1, 0,   3,  5, true, 22 },
      { 0, 1,   0,  6, true, 33 },
      { 1, 1,  -8, -3, true, 44 },
   };
   pass = run_frame(emit, ctx, screen, "quad_mb_frame", ref, rw, rh, quad,
                    ARRAY_SIZE(quad), 2, 2) && pass;

   /* Sub-macroblock partition: each luma 4x4 block's co-located 2x2 chroma
    * region is motion-compensated with that block's vector. */
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

   printf("vl-h264-emit-chroma: %s\n", pass ? "PASS" : "FAIL");
   return pass ? 0 : 1;
}
