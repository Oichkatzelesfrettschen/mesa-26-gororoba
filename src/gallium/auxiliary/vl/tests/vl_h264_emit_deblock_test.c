/*
 * SPDX-License-Identifier: MIT
 */

/*
 * End-to-end verification of the H.264 luma in-loop deblock orchestrator
 * (vl_h264_emit_deblock_luma) on a software (softpipe) screen.
 *
 * The orchestrator filters every macroblock completely in raster order (ITU-T
 * H.264 sec 8.7) -- the four vertical edges left to right, then the four
 * horizontal edges top to bottom, the macroblock-boundary edges included --
 * deriving the boundary strength per edge from the contract's coding, motion, and
 * intra state and the alpha/beta/tc0 thresholds from the slice QP.  The edges
 * sweep on an anti-diagonal wavefront so each reads the prior edge's whole output,
 * with the normal filter (strength 1..3, sec 8.7.2.3) on internal and inter
 * boundary edges and the strong filter (strength 4, sec 8.7.2.4) on intra
 * macroblock-boundary edges.
 *
 * This harness builds an integer-domain luma plane and a slice contract, runs the
 * orchestrator, and checks every sample against an independent integer
 * implementation of the same per-macroblock order: the same boundary-strength
 * derivation, the same Table 8-16/8-17 thresholds, and the normal and strong
 * filters.  Softpipe computes in f32, but every deblock intermediate is an exact
 * integer well under 2^24 (the largest, the strong filter's 2*p3+3*p2+p1+p0+q0+4,
 * is at most 2044), so the f32 result is bit-identical to the integer spec and an
 * exact comparison is valid -- the FP24 s1e7m16 truncation the r300 silicon
 * applies is the per-stage budget gate's concern, not this orchestration rung's.
 *
 * Coverage is non-vacuous: a coded-macroblock fixture filters every internal edge
 * over smooth content so the normal filter fires broadly and the sequential
 * ordering is observable (the r=8 edge reads the column the r=4 edge rewrote); a
 * sharp internal step gates the activity test off at one edge; a motion-only
 * fixture exercises boundary strength 1 (vector delta) and 0 (skip); an all-intra
 * multi-macroblock fixture exercises the strength-4 strong filter on the
 * macroblock-boundary edges (and the cross-macroblock dependency, where one
 * macroblock's boundary edge reads its already-filtered neighbour); and a
 * multi-macroblock frame exercises raster placement and the disable_deblock_idc
 * skip.  Each fixture asserts the filter changed at least one sample, so a
 * silently-disabled filter would fail rather than trivially pass.
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

#include "vl_h264_deblock_ref.h"

#define MB 16

static int
clampi(int v, int lo, int hi)
{
   return v < lo ? lo : (v > hi ? hi : v);
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

static struct pipe_sampler_view *
make_view(struct pipe_context *ctx, struct pipe_resource *tex)
{
   struct pipe_sampler_view templ;
   u_sampler_view_default_template(&templ, tex, tex->format);
   return ctx->create_sampler_view(ctx, tex, &templ);
}

static void
upload_pic(struct pipe_context *ctx, struct pipe_resource *tex, const int *pic,
           unsigned w, unsigned h)
{
   struct pipe_transfer *xfer;
   void *map = pipe_texture_map(ctx, tex, 0, 0, PIPE_MAP_WRITE, 0, 0, w, h,
                                &xfer);
   for (unsigned row = 0; row < h; ++row) {
      float *dst = (float *)((char *)map + row * xfer->stride);
      for (unsigned col = 0; col < w; ++col)
         dst[col] = (float)pic[row * w + col];
   }
   pipe_texture_unmap(ctx, xfer);
}

static void
readback(struct pipe_context *ctx, struct pipe_resource *tex, float *out,
         unsigned w, unsigned h)
{
   struct pipe_transfer *xfer;
   void *map = pipe_texture_map(ctx, tex, 0, 0, PIPE_MAP_READ, 0, 0, w, h, &xfer);
   for (unsigned row = 0; row < h; ++row) {
      const float *src = (const float *)((const char *)map + row * xfer->stride);
      for (unsigned col = 0; col < w; ++col)
         out[row * w + col] = src[col];
   }
   pipe_texture_unmap(ctx, xfer);
}

struct mb_spec {
   int mb_x, mb_y;
   int qp;
   bool coded;             /* every block coded: internal edges strength 2 */
   bool per_block_mv;      /* distinct vector per block: strength 1 vs 0 edges */
   bool disable;           /* disable_deblock_idc == 1: skip the macroblock */
   bool intra;             /* no list reference: internal edges 3, boundary 4 */
   bool transform8x8;      /* 8x8 transform: interior edges 4 and 12 not filtered */
};

static bool
run_frame(struct vl_h264_emit *emit, struct pipe_context *ctx,
          struct pipe_screen *screen, const char *name, const int *pic_in,
          unsigned fcols, unsigned frows, const struct mb_spec *mbs,
          unsigned n_mbs)
{
   const unsigned w = fcols * MB, h = frows * MB;

   struct pipe_resource *recon = make_plane(screen, w, h);
   struct pipe_resource *scratch = make_plane(screen, w, h);
   struct pipe_sampler_view *recon_view = make_view(ctx, recon);
   struct pipe_sampler_view *scratch_view = make_view(ctx, scratch);
   upload_pic(ctx, recon, pic_in, w, h);

   struct vl_h264_mb_contract *contract = calloc(n_mbs, sizeof(*contract));
   for (unsigned m = 0; m < n_mbs; ++m) {
      contract[m].mb_x = mbs[m].mb_x;
      contract[m].mb_y = mbs[m].mb_y;
      contract[m].slice_type = VL_H264_SLICE_P;
      contract[m].qp_y = mbs[m].qp;
      contract[m].disable_deblock_idc = mbs[m].disable ? 1 : 0;
      contract[m].transform_8x8 = mbs[m].transform8x8 ? 1 : 0;
      for (int i = 0; i < 16; ++i) {
         /* A per-block walk that crosses the four-quarter-pel full-sample
          * threshold so adjacent blocks alternate between strength-1 and
          * strength-0 internal edges; a single shared vector when not. */
         contract[m].mv_l0[i][0] = mbs[m].per_block_mv ? (int16_t)((i % 2) * 6) : 0;
         contract[m].mv_l0[i][1] = 0;
         /* An intra macroblock leaves every list reference unused, so its
          * internal edges are strength 3 and its macroblock boundaries 4. */
         contract[m].ref_l0[i] = mbs[m].intra ? -1 : 0;
         contract[m].ref_l1[i] = -1;
      }
      if (mbs[m].coded)
         for (int blk = 0; blk < 16; ++blk)
            for (int k = 0; k < 16; ++k)
               contract[m].coeff4x4[blk][k] = (int16_t)(((blk + k) % 3) - 1);
   }

   struct vl_h264_slice_contract slice = {0};
   slice.version = VL_H264_MB_CONTRACT_VERSION;
   slice.width = w;
   slice.height = h;
   slice.slice_type = VL_H264_SLICE_P;
   slice.num_macroblocks = n_mbs;
   slice.macroblocks = contract;

   vl_h264_emit_deblock_luma(emit, recon, recon_view, scratch, scratch_view, w, h,
                             &slice);

   float *out = malloc((size_t)w * h * sizeof(float));
   readback(ctx, recon, out, w, h);

   int *want = malloc((size_t)w * h * sizeof(int));
   memcpy(want, pic_in, (size_t)w * h * sizeof(int));
   unsigned changed = deblock_reference(want, w, h, &slice);

   bool ok = true;
   for (unsigned y = 0; y < h && ok; ++y) {
      for (unsigned x = 0; x < w; ++x) {
         if ((int)lroundf(out[y * w + x]) != want[y * w + x]) {
            printf("FAIL %s: (%u,%u) got %.3f want %d (in %d)\n", name, x, y,
                   out[y * w + x], want[y * w + x], pic_in[y * w + x]);
            ok = false;
            break;
         }
      }
   }
   /* A vacuous pass -- the filter never fired -- is a test failure: it would
    * mask a silently disabled deblock. */
   if (ok && changed == 0) {
      printf("FAIL %s: filter changed no samples (vacuous)\n", name);
      ok = false;
   }

   free(out);
   free(want);
   free(contract);
   pipe_sampler_view_reference(&recon_view, NULL);
   pipe_sampler_view_reference(&scratch_view, NULL);
   pipe_resource_reference(&recon, NULL);
   pipe_resource_reference(&scratch, NULL);

   printf("Test(vl-h264-emit-deblock: %s) = %s (%u samples filtered)\n", name,
          ok ? "pass" : "fail", changed);
   return ok;
}

#define CHROMA_MB 8

/* Run the chroma deblock orchestrator on one component plane (Cb or Cr per use_cr)
 * and check every sample against the integer chroma reference.  The plane is the
 * half-resolution 8x8-per-macroblock chroma grid; the contract sets qp_cb and
 * qp_cr to the fixture QP and the luma block fields decide the inherited boundary
 * strength. */
static bool
run_chroma_frame(struct vl_h264_emit *emit, struct pipe_context *ctx,
                 struct pipe_screen *screen, const char *name, const int *pic_in,
                 unsigned fcols, unsigned frows, const struct mb_spec *mbs,
                 unsigned n_mbs, bool use_cr)
{
   const unsigned w = fcols * CHROMA_MB, h = frows * CHROMA_MB;

   struct pipe_resource *recon = make_plane(screen, w, h);
   struct pipe_resource *scratch = make_plane(screen, w, h);
   struct pipe_sampler_view *recon_view = make_view(ctx, recon);
   struct pipe_sampler_view *scratch_view = make_view(ctx, scratch);
   upload_pic(ctx, recon, pic_in, w, h);

   struct vl_h264_mb_contract *contract = calloc(n_mbs, sizeof(*contract));
   for (unsigned m = 0; m < n_mbs; ++m) {
      contract[m].mb_x = mbs[m].mb_x;
      contract[m].mb_y = mbs[m].mb_y;
      contract[m].slice_type = VL_H264_SLICE_P;
      contract[m].qp_cb = mbs[m].qp;
      contract[m].qp_cr = mbs[m].qp;
      contract[m].disable_deblock_idc = mbs[m].disable ? 1 : 0;
      for (int i = 0; i < 16; ++i) {
         contract[m].mv_l0[i][0] = mbs[m].per_block_mv ? (int16_t)((i % 2) * 6) : 0;
         contract[m].mv_l0[i][1] = 0;
         contract[m].ref_l0[i] = mbs[m].intra ? -1 : 0;
         contract[m].ref_l1[i] = -1;
      }
      if (mbs[m].coded)
         for (int blk = 0; blk < 16; ++blk)
            for (int k = 0; k < 16; ++k)
               contract[m].coeff4x4[blk][k] = (int16_t)(((blk + k) % 3) - 1);
   }

   struct vl_h264_slice_contract slice = {0};
   slice.version = VL_H264_MB_CONTRACT_VERSION;
   slice.width = w * 2;
   slice.height = h * 2;
   slice.slice_type = VL_H264_SLICE_P;
   slice.num_macroblocks = n_mbs;
   slice.macroblocks = contract;

   vl_h264_emit_deblock_chroma(emit, recon, recon_view, scratch, scratch_view, w, h,
                               &slice, use_cr);

   float *out = malloc((size_t)w * h * sizeof(float));
   readback(ctx, recon, out, w, h);

   int *want = malloc((size_t)w * h * sizeof(int));
   memcpy(want, pic_in, (size_t)w * h * sizeof(int));
   unsigned changed = deblock_chroma_reference(want, w, h, &slice, use_cr);

   bool ok = true;
   for (unsigned y = 0; y < h && ok; ++y) {
      for (unsigned x = 0; x < w; ++x) {
         if ((int)lroundf(out[y * w + x]) != want[y * w + x]) {
            printf("FAIL %s: (%u,%u) got %.3f want %d (in %d)\n", name, x, y,
                   out[y * w + x], want[y * w + x], pic_in[y * w + x]);
            ok = false;
            break;
         }
      }
   }
   if (ok && changed == 0) {
      printf("FAIL %s: filter changed no samples (vacuous)\n", name);
      ok = false;
   }

   free(out);
   free(want);
   free(contract);
   pipe_sampler_view_reference(&recon_view, NULL);
   pipe_sampler_view_reference(&scratch_view, NULL);
   pipe_resource_reference(&recon, NULL);
   pipe_resource_reference(&scratch, NULL);

   printf("Test(vl-h264-emit-deblock: %s) = %s (%u samples filtered)\n", name,
          ok ? "pass" : "fail", changed);
   return ok;
}

/* Block-structured content: each 4x4 block carries a small distinct DC level so
 * adjacent blocks meet at a step -- a blocking artifact the deblock filter is
 * meant to smooth -- plus a gentle intra-block gradient.  The steps stay under
 * alpha and beta so the activity gate holds and the filter fires; a pure ramp
 * (no step) would leave delta zero, which is exactly the gradient the filter
 * preserves.  A big_col_step injected at one column models a real edge the
 * activity gate must reject.  Values are kept mid-range to avoid Clip1
 * saturation masking a mismatch. */
static void
fill_blocky(int *pic, int w, int h, int big_col_step, int big_col)
{
   for (int y = 0; y < h; ++y)
      for (int x = 0; x < w; ++x) {
         int bx = (x % MB) / 4, by = (y % MB) / 4;
         int dc = 6 * ((bx * 3 + by * 2) % 5);   /* 0,6,12,18,24 inter-block step */
         int intra = (x % 4) + (y % 4);          /* 0..6 gentle gradient */
         int step = (big_col_step && x >= big_col) ? big_col_step : 0;
         pic[y * w + x] = clampi(70 + dc + intra + step, 0, 255);
      }
}

int
main(void)
{
   struct sw_winsys *winsys = null_sw_create();
   if (!winsys) {
      fprintf(stderr, "vl-h264-emit-deblock: no software winsys; skipping\n");
      return 77;
   }
   struct pipe_screen *screen = softpipe_create_screen(winsys);
   if (!screen) {
      fprintf(stderr, "vl-h264-emit-deblock: no software screen; skipping\n");
      winsys->destroy(winsys);
      return 77;
   }
   if (!screen->is_format_supported(screen, PIPE_FORMAT_R32_FLOAT,
                                    PIPE_TEXTURE_2D, 0, 0,
                                    PIPE_BIND_RENDER_TARGET |
                                    PIPE_BIND_SAMPLER_VIEW)) {
      fprintf(stderr, "vl-h264-emit-deblock: R32_FLOAT not renderable; skip\n");
      screen->destroy(screen);
      winsys->destroy(winsys);
      return 77;
   }

   struct pipe_context *ctx = screen->context_create(screen, NULL, 0);
   struct vl_h264_emit *emit = vl_h264_emit_create(ctx);

   bool pass = true;

   /* Block-structured content: inter-block DC steps are the blocking artifacts
    * the filter smooths, so it fires across the internal edges. */
   const unsigned sw = MB, sh = MB;
   int *blocky = malloc((size_t)sw * sh * sizeof(int));
   fill_blocky(blocky, sw, sh, 0, 0);

   /* Coded single macroblock: every internal edge is strength 2 and fires; the
    * r=8 edge reads the column the r=4 edge rewrote, so a broken pass order or a
    * missing inter-pass barrier diverges from the reference. */
   const struct mb_spec coded[] = {{0, 0, 36, true, false, false}};
   pass = run_frame(emit, ctx, screen, "coded_mb_strength2", blocky, 1, 1, coded,
                    1) && pass;

   /* The same content with a sharp +120 step from the x=8 column: the activity
    * gate |p0-q0| < alpha turns the r=8 vertical edge off while r=4 and r=12
    * still fire, exercising both branches of the gate. */
   int *stepped = malloc((size_t)sw * sh * sizeof(int));
   fill_blocky(stepped, sw, sh, 120, 8);
   const struct mb_spec coded_step[] = {{0, 0, 36, true, false, false}};
   pass = run_frame(emit, ctx, screen, "coded_mb_activity_gate", stepped, 1, 1,
                    coded_step, 1) && pass;

   /* Motion-only macroblock: no coding, one reference, per-block vectors that
    * alternate across the full-sample threshold, so internal edges alternate
    * between strength 1 (vector delta >= 4) and strength 0 (skipped). */
   const struct mb_spec motion[] = {{0, 0, 40, false, true, false}};
   pass = run_frame(emit, ctx, screen, "motion_strength1_and_0", blocky, 1, 1,
                    motion, 1) && pass;

   /* Four-macroblock frame at four origins: raster placement plus a disabled
    * macroblock (disable_deblock_idc == 1) that must be left untouched. */
   const unsigned qw = 2 * MB, qh = 2 * MB;
   int *quad_pic = malloc((size_t)qw * qh * sizeof(int));
   fill_blocky(quad_pic, qw, qh, 0, 0);
   const struct mb_spec quad[] = {
      {0, 0, 36, true, false, false},
      {1, 0, 34, false, true, false},
      {0, 1, 38, true, false, false},
      {1, 1, 36, true, false, true},   /* disabled: untouched */
   };
   pass = run_frame(emit, ctx, screen, "quad_mb_frame_with_disable", quad_pic, 2,
                    2, quad, 4) && pass;

   /* All-intra 2x2 frame: every internal luma edge is strength 3 and every
    * macroblock-boundary edge strength 4, so the strong filter (sec 8.7.2.4)
    * fires on the shared vertical and horizontal boundaries, and a macroblock's
    * boundary edge reads its already-filtered neighbour (the cross-macroblock
    * dependency the wavefront preserves). */
   int *intra_pic = malloc((size_t)qw * qh * sizeof(int));
   fill_blocky(intra_pic, qw, qh, 0, 0);
   const struct mb_spec intra_quad[] = {
      {0, 0, 40, true, false, false, true},
      {1, 0, 40, true, false, false, true},
      {0, 1, 40, true, false, false, true},
      {1, 1, 40, true, false, false, true},
   };
   pass = run_frame(emit, ctx, screen, "intra_quad_strong_boundary", intra_pic, 2,
                    2, intra_quad, 4) && pass;

   /* All-intra 2x2 frame on the 8x8 transform: the interior 4x4 edges at offsets
    * 4 and 12 lie inside an 8x8 transform block and are skipped, so only the 8x8
    * block edge at 8 (strength 3) and the macroblock boundaries (strength 4,
    * strong) are filtered.  A model that filtered 4 and 12 would diverge here. */
   int *intra8x8_pic = malloc((size_t)qw * qh * sizeof(int));
   fill_blocky(intra8x8_pic, qw, qh, 0, 0);
   const struct mb_spec intra8x8[] = {
      {0, 0, 40, false, false, false, true, true},
      {1, 0, 40, false, false, false, true, true},
      {0, 1, 40, false, false, false, true, true},
      {1, 1, 40, false, false, false, true, true},
   };
   pass = run_frame(emit, ctx, screen, "intra_quad_8x8_edge_skip", intra8x8_pic, 2,
                    2, intra8x8, 4) && pass;

   /* Two intra macroblocks with a large QP difference across the shared vertical
    * boundary: the boundary thresholds index by qPav = (28 + 44 + 1) >> 1 = 36,
    * different from either side, so a model that used only the q-side QP would
    * filter the boundary with the wrong alpha/beta and diverge here. */
   const unsigned dw = 2 * MB, dh = MB;
   int *qpav_pic = malloc((size_t)dw * dh * sizeof(int));
   fill_blocky(qpav_pic, dw, dh, 0, 0);
   const struct mb_spec qpav[] = {
      {0, 0, 28, true, false, false, true},
      {1, 0, 44, true, false, false, true},
   };
   pass = run_frame(emit, ctx, screen, "qpav_boundary_average", qpav_pic, 2, 1,
                    qpav, 2) && pass;

   /* All-intra 2x2 chroma frame: each 8x8 chroma macroblock filters the edges
    * co-located with luma offsets 0 and 8 (chroma offsets 0 and 4) -- the bS=4
    * two-tap on the macroblock boundary and the normal tC0+1 filter on the
    * internal edge -- inheriting the geometric intra strength from the luma blocks.
    * Run both components so the Cb (qp_cb) and Cr (qp_cr) plumbing is exercised. */
   int *chroma_pic = malloc((size_t)(2 * CHROMA_MB) * (2 * CHROMA_MB) * sizeof(int));
   fill_blocky(chroma_pic, 2 * CHROMA_MB, 2 * CHROMA_MB, 0, 0);
   const struct mb_spec chroma_quad[] = {
      {0, 0, 30, true, false, false, true},
      {1, 0, 30, true, false, false, true},
      {0, 1, 30, true, false, false, true},
      {1, 1, 30, true, false, false, true},
   };
   pass = run_chroma_frame(emit, ctx, screen, "chroma_quad_cb", chroma_pic, 2, 2,
                           chroma_quad, 4, false) && pass;
   pass = run_chroma_frame(emit, ctx, screen, "chroma_quad_cr", chroma_pic, 2, 2,
                           chroma_quad, 4, true) && pass;

   vl_h264_emit_destroy(emit);
   free(blocky);
   free(stepped);
   free(quad_pic);
   free(intra_pic);
   free(intra8x8_pic);
   free(qpav_pic);
   free(chroma_pic);
   ctx->destroy(ctx);
   screen->destroy(screen);
   winsys->destroy(winsys);

   printf("vl-h264-emit-deblock: %s\n", pass ? "PASS" : "FAIL");
   return pass ? 0 : 1;
}
