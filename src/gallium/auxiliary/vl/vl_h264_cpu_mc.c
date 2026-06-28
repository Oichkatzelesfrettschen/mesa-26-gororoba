/*
 * Copyright (c) 2026 Terascale Functionalists
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <stdlib.h>

#include "vl_h264_cpu_mc.h"

#include "vl_h264_intra_reconstruct.h"

/* The six-tap luma interpolation filter (ITU-T H.264 sec 8.4.2.2.1). */
static const int luma_tap[6] = { 1, -5, 20, 20, -5, 1 };

static int
clampi(int v, int lo, int hi)
{
   return v < lo ? lo : (v > hi ? hi : v);
}

static int
clip1(int v)
{
   return clampi(v, 0, 255);
}

/* A reference sample with clamp-to-edge, matching the sampler's edge behavior. */
static int
ref_at(const uint8_t *ref, unsigned stride, int rw, int rh, int x, int y)
{
   return ref[clampi(y, 0, rh - 1) * (int)stride + clampi(x, 0, rw - 1)];
}

/* The horizontal six-tap sum at (x, y), before the round and shift -- the
 * intermediate the half-pel b and the 2D half-pel j are built from. */
static int
hsum(const uint8_t *ref, unsigned stride, int rw, int rh, int x, int y)
{
   int acc = 0;
   for (int t = 0; t < 6; t++)
      acc += ref_at(ref, stride, rw, rh, x - 2 + t, y) * luma_tap[t];
   return acc;
}

/* The vertical six-tap sum at (x, y), before the round and shift. */
static int
vsum(const uint8_t *ref, unsigned stride, int rw, int rh, int x, int y)
{
   int acc = 0;
   for (int t = 0; t < 6; t++)
      acc += ref_at(ref, stride, rw, rh, x, y - 2 + t) * luma_tap[t];
   return acc;
}

bool
vl_h264_luma_mv_needs_j(int mvx, int mvy)
{
   int pos = (mvy & 3) * 4 + (mvx & 3);
   return pos == 6 || pos == 9 || pos == 10 || pos == 11 || pos == 14;
}

/* The full quarter-pel luma prediction for one sample (sec 8.4.2.2.1), including
 * the diagonal-center positions that need the 2D half-pel j. */
static int
predict_luma(const uint8_t *ref, unsigned stride, int rw, int rh, int px, int py,
             int mvx, int mvy)
{
   int xx = px + (mvx >> 2), yy = py + (mvy >> 2);
   int pos = (mvy & 3) * 4 + (mvx & 3);
   if (pos == 0)
      return ref_at(ref, stride, rw, rh, xx, yy);

   int b = clip1((hsum(ref, stride, rw, rh, xx, yy) + 16) >> 5);
   int h = clip1((vsum(ref, stride, rw, rh, xx, yy) + 16) >> 5);
   int m = clip1((vsum(ref, stride, rw, rh, xx + 1, yy) + 16) >> 5);
   int s = clip1((hsum(ref, stride, rw, rh, xx, yy + 1) + 16) >> 5);
   /* The 2D half-pel: the six-tap vertical sum over the horizontal intermediate,
    * which is why it overflows the FP24 range the GPU kernel is bounded by. */
   int j1 = 0;
   for (int t = 0; t < 6; t++)
      j1 += hsum(ref, stride, rw, rh, xx, yy - 2 + t) * luma_tap[t];
   int j = clip1((j1 + 512) >> 10);
   int g = ref_at(ref, stride, rw, rh, xx, yy);
   int g_right = ref_at(ref, stride, rw, rh, xx + 1, yy);
   int g_down = ref_at(ref, stride, rw, rh, xx, yy + 1);

   switch (pos) {
   case 1:  return (g + b + 1) >> 1;
   case 2:  return b;
   case 3:  return (b + g_right + 1) >> 1;
   case 4:  return (g + h + 1) >> 1;
   case 5:  return (b + h + 1) >> 1;
   case 6:  return (b + j + 1) >> 1;
   case 7:  return (b + m + 1) >> 1;
   case 8:  return h;
   case 9:  return (h + j + 1) >> 1;
   case 10: return j;
   case 11: return (j + m + 1) >> 1;
   case 12: return (g_down + h + 1) >> 1;
   case 13: return (h + s + 1) >> 1;
   case 14: return (j + s + 1) >> 1;
   case 15: return (m + s + 1) >> 1;
   default: return g;
   }
}

void
vl_h264_cpu_luma_diag_fallback(const struct vl_h264_mb_contract *mbs,
                               unsigned num_mbs, unsigned width_in_mbs,
                               unsigned height_in_mbs, const uint8_t *reference,
                               int ref_w, int ref_h, unsigned ref_stride,
                               uint8_t *luma, unsigned stride)
{
   (void) height_in_mbs;
   for (unsigned a = 0; a < num_mbs; a++) {
      const struct vl_h264_mb_contract *mb = &mbs[a];
      if (mb->ref_l0[0] < 0)
         continue; /* an intra macroblock has no motion */
      unsigned mb_x = a % width_in_mbs, mb_y = a / width_in_mbs;

      /* mv_l0 and the dequantized coeff4x4 are canonical raster, so the block at
       * raster index by*4 + bx covers the 4x4 region at (bx, by). */
      for (int by = 0; by < 4; by++)
         for (int bx = 0; bx < 4; bx++) {
            int blk = by * 4 + bx;
            int mvx = mb->mv_l0[blk][0], mvy = mb->mv_l0[blk][1];
            if (mb->ref_l0[blk] < 0 || !vl_h264_luma_mv_needs_j(mvx, mvy))
               continue;
            int16_t res[16];
            vl_h264_idct4(mb->coeff4x4[blk], res);
            for (int ly = 0; ly < 4; ly++)
               for (int lx = 0; lx < 4; lx++) {
                  int px = (int)mb_x * 16 + bx * 4 + lx;
                  int py = (int)mb_y * 16 + by * 4 + ly;
                  int pred = predict_luma(reference, ref_stride, ref_w, ref_h, px,
                                          py, mvx, mvy);
                  luma[py * stride + px] = (uint8_t)clip1(pred + res[ly * 4 + lx]);
               }
         }
   }
}

void
vl_h264_cpu_luma_mc_multiref(const struct vl_h264_mb_contract *mbs,
                             unsigned num_mbs, unsigned width_in_mbs,
                             unsigned height_in_mbs,
                             const struct vl_h264_ref_plane *refs,
                             unsigned num_refs, uint8_t *luma, unsigned stride)
{
   (void) height_in_mbs;
   if (num_refs == 0)
      return;
   if (getenv("R300_H264_REF_DUMP")) {
      int hi = 0, n_hi = 0, n_inter = 0;
      for (unsigned a = 0; a < num_mbs; a++)
         for (int blk = 0; blk < 16; blk++) {
            int ri = mbs[a].ref_l0[blk];
            if (ri >= 0) n_inter++;
            if (ri > 0) { n_hi++; if (ri > hi) hi = ri; }
         }
      fprintf(stderr, "h264ref num_refs=%u inter_blocks=%d ref_idx>0_blocks=%d "
              "max_ref_idx=%d\n", num_refs, n_inter, n_hi, hi);
   }
   for (unsigned a = 0; a < num_mbs; a++) {
      const struct vl_h264_mb_contract *mb = &mbs[a];
      if (mb->ref_l0[0] < 0)
         continue; /* intra macroblock */
      unsigned mb_x = a % width_in_mbs, mb_y = a / width_in_mbs;

      for (int by = 0; by < 4; by++)
         for (int bx = 0; bx < 4; bx++) {
            int blk = by * 4 + bx;
            int ref_idx = mb->ref_l0[blk];
            if (ref_idx < 0)
               continue; /* intra block in an inter macroblock */
            int mvx = mb->mv_l0[blk][0], mvy = mb->mv_l0[blk][1];
            /* The back half already produced refs[0] at the non-j positions. */
            if (ref_idx == 0 && !vl_h264_luma_mv_needs_j(mvx, mvy))
               continue;
            /* A reference past the built list cannot be resolved (an unsupported
             * reordering or a frame-number gap); leave the back half's block. */
            if ((unsigned)ref_idx >= num_refs)
               continue;
            const struct vl_h264_ref_plane *r = &refs[ref_idx];

            int16_t res[16];
            vl_h264_idct4(mb->coeff4x4[blk], res);
            for (int ly = 0; ly < 4; ly++)
               for (int lx = 0; lx < 4; lx++) {
                  int px = (int)mb_x * 16 + bx * 4 + lx;
                  int py = (int)mb_y * 16 + by * 4 + ly;
                  int pred = predict_luma(r->pixels, r->stride, r->w, r->h, px, py,
                                          mvx, mvy);
                  luma[py * stride + px] = (uint8_t)clip1(pred + res[ly * 4 + lx]);
               }
         }
   }
}
