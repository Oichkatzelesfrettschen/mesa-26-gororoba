/*
 * SPDX-License-Identifier: MIT
 */

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

/* The eighth-pel chroma bilinear prediction for one sample (ITU-T H.264 sec
 * 8.4.2.2.2).  The luma vector doubles as the chroma vector: in 4:2:0 a
 * quarter-luma-sample step is an eighth-chroma-sample step, so mvx and mvy index
 * the chroma plane directly in eighth-sample units -- the integer part is the
 * arithmetic-shift floor, the fraction the low three bits.  The four weights sum
 * to 64 and the sum rounds with +32 before the >>6, matching the FP24 back
 * half's bilinear blend in vl_h264_chroma.c. */
static int
predict_chroma(const uint8_t *ref, unsigned stride, int rw, int rh, int px,
               int py, int mvx, int mvy)
{
   int xi = px + (mvx >> 3), yi = py + (mvy >> 3);
   int xf = mvx & 7, yf = mvy & 7;
   int a = ref_at(ref, stride, rw, rh, xi, yi);
   int b = ref_at(ref, stride, rw, rh, xi + 1, yi);
   int c = ref_at(ref, stride, rw, rh, xi, yi + 1);
   int d = ref_at(ref, stride, rw, rh, xi + 1, yi + 1);
   return ((8 - xf) * (8 - yf) * a + xf * (8 - yf) * b +
           (8 - xf) * yf * c + xf * yf * d + 32) >> 6;
}

void
vl_h264_cpu_chroma_mc_multiref(const struct vl_h264_mb_contract *mbs,
                               unsigned num_mbs, unsigned width_in_mbs,
                               unsigned height_in_mbs,
                               const struct vl_h264_ref_plane *refs_cb,
                               const struct vl_h264_ref_plane *refs_cr,
                               unsigned num_refs, uint8_t *cb, uint8_t *cr,
                               unsigned stride)
{
   (void) height_in_mbs;
   if (num_refs == 0)
      return;
   uint8_t *plane[2] = { cb, cr };
   const struct vl_h264_ref_plane *refs[2] = { refs_cb, refs_cr };

   for (unsigned a = 0; a < num_mbs; a++) {
      const struct vl_h264_mb_contract *mb = &mbs[a];
      if (mb->ref_l0[0] < 0)
         continue; /* intra macroblock: the CPU intra pass owns its chroma */
      unsigned mb_x = a % width_in_mbs, mb_y = a / width_in_mbs;

      /* Each 4x4 chroma block co-locates with an 8x8 luma region -- a 2x2 group
       * of luma 4x4 blocks, each with its own vector and reference.  The back
       * half built every chroma sample from refs[0]; recompute a block only when
       * one of its luma blocks selects a later reference, and only when every
       * selected reference is in the built list. */
      for (unsigned blk = 0; blk < 4; blk++) {
         int qx = (int)(blk % 2) * 4, qy = (int)(blk / 2) * 4;
         bool needs_fix = false, resolvable = true;
         for (int sy = 0; sy < 2; sy++)
            for (int sx = 0; sx < 2; sx++) {
               int lblk = (qy / 2 + sy) * 4 + (qx / 2 + sx);
               int ri = mb->ref_l0[lblk];
               if (ri > 0)
                  needs_fix = true;
               if (ri < 0 || (unsigned) ri >= num_refs)
                  resolvable = false;
            }
         if (!needs_fix || !resolvable)
            continue;

         for (unsigned comp = 0; comp < 2; comp++) {
            int16_t res[16];
            vl_h264_idct4(mb->coeff4x4[VL_H264_LUMA_4X4_BLOCKS + comp * 4 + blk],
                          res);
            for (int y = 0; y < 4; y++)
               for (int x = 0; x < 4; x++) {
                  int cx = qx + x, cy = qy + y;       /* chroma sample in the MB */
                  int lblk = (cy / 2) * 4 + (cx / 2); /* its luma 4x4 block */
                  int ref_idx = mb->ref_l0[lblk];
                  const struct vl_h264_ref_plane *r = &refs[comp][ref_idx];
                  if (!r->pixels)
                     continue; /* unmapped reference: leave the back half's sample */
                  int px = (int) mb_x * 8 + cx, py = (int) mb_y * 8 + cy;
                  int pred = predict_chroma(r->pixels, r->stride, r->w, r->h, px,
                                            py, mb->mv_l0[lblk][0],
                                            mb->mv_l0[lblk][1]);
                  plane[comp][py * (int) stride + px] =
                     (uint8_t) clip1(pred + res[y * 4 + x]);
               }
         }
      }
   }
}
