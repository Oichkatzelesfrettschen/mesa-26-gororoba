/*
 * Copyright (c) 2026 Terascale Functionalists
 * SPDX-License-Identifier: MIT
 */

#ifndef vl_h264_deblock_ref_h
#define vl_h264_deblock_ref_h

/*
 * Integer reference for the H.264 luma in-loop deblock, internal block edges
 * only (ITU-T H.264 sec 8.7), shared by the back-half tests that need to predict
 * the deblocked luma: the orchestrator's own softpipe harness and the end-to-end
 * decoder harness.  It mirrors vl_h264_emit_deblock_luma exactly -- the same
 * boundary-strength derivation, the same Table 8-16/8-17 thresholds, and the
 * same six-pass ping-pong (vertical r=4,8,12 then horizontal r=4,8,12, each pass
 * reading the prior pass's whole output) -- so a test comparing GPU output to
 * deblock_reference proves the shader matches this integer model.  Arithmetic
 * right shift floors toward minus infinity, matching the kernel's FRC-floor; the
 * C signed shift is arithmetic on every target Mesa builds.
 */

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "vl_h264_mb_contract.h"

/* H.264 luma macroblock edge length. */
#define DEBLOCK_REF_MB 16

/* Table 8-16 (alpha, beta) by clipped QP index and Table 8-17 (tc0) by
 * [boundary strength - 1][QP index]. */
static const int deblock_ref_alpha[52] = {
   0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 4, 4, 5, 6, 7, 8, 9, 10, 12,
   13, 15, 17, 20, 22, 25, 28, 32, 36, 40, 45, 50, 56, 63, 71, 80, 90, 101, 113,
   127, 144, 162, 182, 203, 226, 255, 255};
static const int deblock_ref_beta[52] = {
   0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4,
   6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13, 14, 14, 15, 15, 16,
   16, 17, 17, 18, 18};
static const int deblock_ref_tc0[3][52] = {
   {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 4, 4, 4, 5, 6, 6, 7, 8, 9, 10, 11,
    13},
   {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 4, 4, 5, 5, 6, 7, 8, 8, 10, 11, 12, 13,
    15, 17},
   {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 2, 2, 2, 2, 3, 3, 3, 4, 4, 4, 5, 6, 6, 7, 8, 9, 10, 11, 13, 14, 16, 18,
    20, 23, 25},
};

static inline int
deblock_ref_clamp(int v, int lo, int hi)
{
   return v < lo ? lo : (v > hi ? hi : v);
}

static inline int
deblock_ref_qp_index(int qp, int offset)
{
   return deblock_ref_clamp(qp + offset, 0, 51);
}

/* Whether a 4x4 luma block carries a nonzero coefficient (an 8x8-transformed
 * macroblock keeps its levels in coeff8x8); mirrors vl_h264_emit.c block_coded. */
static inline bool
deblock_ref_block_coded(const struct vl_h264_mb_contract *mb, int blk)
{
   if (mb->transform_8x8) {
      int by = blk / 4, bx = blk % 4;
      int quad = (by / 2) * 2 + (bx / 2);
      for (int k = 0; k < 64; ++k)
         if (mb->coeff8x8[quad][k] != 0)
            return true;
      return false;
   }
   for (int k = 0; k < 16; ++k)
      if (mb->coeff4x4[blk][k] != 0)
         return true;
   return false;
}

/* Internal-edge boundary strength; mirrors vl_h264_emit.c boundary_strength. */
static inline int
deblock_ref_strength(const struct vl_h264_mb_contract *mb, int a, int bk)
{
   if (mb->ref_l0[a] < 0 && mb->ref_l1[a] < 0)
      return 3;
   if (deblock_ref_block_coded(mb, a) || deblock_ref_block_coded(mb, bk))
      return 2;
   if (mb->ref_l0[a] != mb->ref_l0[bk])
      return 1;
   int dx = mb->mv_l0[a][0] - mb->mv_l0[bk][0];
   int dy = mb->mv_l0[a][1] - mb->mv_l0[bk][1];
   if (dx <= -4 || dx >= 4 || dy <= -4 || dy >= 4)
      return 1;
   return 0;
}

/* The normal luma filter, ITU-T H.264 sec 8.7.2.3, over six samples
 * s = p2,p1,p0,q0,q1,q2, writing p1',p0',q0',q1' into out. */
static inline void
deblock_ref_filter(const int s[6], int alpha, int beta, int tc0, int out[4])
{
   int p2 = s[0], p1 = s[1], p0 = s[2], q0 = s[3], q1 = s[4], q2 = s[5];

   out[0] = p1;
   out[1] = p0;
   out[2] = q0;
   out[3] = q1;

   bool on = abs(p0 - q0) < alpha && abs(p1 - p0) < beta && abs(q1 - q0) < beta;
   if (!on)
      return;

   bool ap = abs(p2 - p0) < beta;
   bool aq = abs(q2 - q0) < beta;
   int tc = tc0 + (ap ? 1 : 0) + (aq ? 1 : 0);

   int raw = (((q0 - p0) << 2) + (p1 - q1) + 4) >> 3;
   int delta = deblock_ref_clamp(raw, -tc, tc);
   int avg = (p0 + q0 + 1) >> 1;

   out[1] = deblock_ref_clamp(p0 + delta, 0, 255);
   out[2] = deblock_ref_clamp(q0 - delta, 0, 255);
   if (ap)
      out[0] = p1 + deblock_ref_clamp((p2 + avg - 2 * p1) >> 1, -tc0, tc0);
   if (aq)
      out[3] = q1 + deblock_ref_clamp((q2 + avg - 2 * q1) >> 1, -tc0, tc0);
}

/* Apply the internal-edge deblock to an integer luma plane in place, the same
 * six-pass ping-pong vl_h264_emit_deblock_luma runs.  Returns the number of
 * samples the filter changed, so a caller can reject a vacuous fixture. */
static inline unsigned
deblock_reference(int *pic, int w, int h,
                  const struct vl_h264_slice_contract *slice)
{
   const int MBSZ = DEBLOCK_REF_MB;
   int *orig = malloc((size_t)w * h * sizeof(int));
   memcpy(orig, pic, (size_t)w * h * sizeof(int));

   int *cur = pic;
   int *nxt = malloc((size_t)w * h * sizeof(int));

   for (int pass = 0; pass < 6; ++pass) {
      bool vertical = pass < 3;
      int r = 4 + 4 * (vertical ? pass : pass - 3);

      memcpy(nxt, cur, (size_t)w * h * sizeof(int));

      for (unsigned m = 0; m < slice->num_macroblocks; ++m) {
         const struct vl_h264_mb_contract *mb = &slice->macroblocks[m];
         if (mb->disable_deblock_idc == 1)
            continue;
         int ia = deblock_ref_qp_index(mb->qp_y, mb->slice_alpha_c0_offset_div2 * 2);
         int ib = deblock_ref_qp_index(mb->qp_y, mb->slice_beta_offset_div2 * 2);
         int alpha = deblock_ref_alpha[ia];
         int beta = deblock_ref_beta[ib];

         for (int seg = 0; seg < 4; ++seg) {
            int a, bk;
            if (vertical) {
               a = seg * 4 + (r / 4 - 1);
               bk = seg * 4 + r / 4;
            } else {
               a = (r / 4 - 1) * 4 + seg;
               bk = (r / 4) * 4 + seg;
            }
            int bs = deblock_ref_strength(mb, a, bk);
            if (bs == 0)
               continue;
            int tc0 = deblock_ref_tc0[bs - 1][ia];

            for (int t = 0; t < 4; ++t) {
               int s[6], out[4];
               int ex, ey, sx, sy;
               if (vertical) {
                  ex = mb->mb_x * MBSZ + r;
                  ey = mb->mb_y * MBSZ + seg * 4 + t;
                  sx = 1;
                  sy = 0;
               } else {
                  ex = mb->mb_x * MBSZ + seg * 4 + t;
                  ey = mb->mb_y * MBSZ + r;
                  sx = 0;
                  sy = 1;
               }
               for (int k = 0; k < 6; ++k) {
                  int xx = deblock_ref_clamp(ex + sx * (k - 3), 0, w - 1);
                  int yy = deblock_ref_clamp(ey + sy * (k - 3), 0, h - 1);
                  s[k] = cur[yy * w + xx];
               }
               deblock_ref_filter(s, alpha, beta, tc0, out);
               for (int lane = 0; lane < 4; ++lane)
                  nxt[(ey + sy * (lane - 2)) * w + ex + sx * (lane - 2)] = out[lane];
            }
         }
      }

      int *tmp = cur;
      cur = nxt;
      nxt = tmp;
   }

   if (cur != pic)
      memcpy(pic, cur, (size_t)w * h * sizeof(int));

   unsigned changed = 0;
   for (int i = 0; i < w * h; ++i)
      if (pic[i] != orig[i])
         ++changed;

   free(orig);
   free(cur == pic ? nxt : cur);
   return changed;
}

#endif /* vl_h264_deblock_ref_h */
