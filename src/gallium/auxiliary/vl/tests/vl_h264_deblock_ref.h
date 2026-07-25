/*
 * SPDX-License-Identifier: MIT
 */

#ifndef vl_h264_deblock_ref_h
#define vl_h264_deblock_ref_h

/*
 * Integer reference for the H.264 luma in-loop deblock (ITU-T H.264 sec 8.7),
 * shared by the back-half tests that need to predict the deblocked luma: the
 * orchestrator's own softpipe harness and the end-to-end decoder harness.  It
 * mirrors vl_h264_emit_deblock_luma exactly -- the same boundary-strength
 * derivation including the macroblock-boundary strength 4, the same Table
 * 8-16/8-17 thresholds, the same normal (bS 1..3) and strong (bS 4) filters, and
 * the same per-macroblock raster order over all edges (the four vertical edges
 * left to right, then the four horizontal edges top to bottom, boundary edges
 * included).  The reference applies that order in place, which the wavefront
 * proof shows equals the GPU's anti-diagonal schedule, so a test comparing GPU
 * output to deblock_reference proves the shader matches this integer model.
 * Arithmetic right shift floors toward minus infinity, matching the kernel's
 * FRC-floor; the C signed shift is arithmetic on every target Mesa builds.
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

/* Boundary strength across a luma edge; mirrors vl_h264_emit.c deblock_strength. */
static inline int
deblock_ref_strength(const struct vl_h264_mb_contract *mb_p, int blk_p,
                     const struct vl_h264_mb_contract *mb_q, int blk_q,
                     bool mb_boundary)
{
   bool p_intra = mb_p->ref_l0[blk_p] < 0 && mb_p->ref_l1[blk_p] < 0;
   bool q_intra = mb_q->ref_l0[blk_q] < 0 && mb_q->ref_l1[blk_q] < 0;
   if (p_intra || q_intra)
      return mb_boundary ? 4 : 3;
   if (deblock_ref_block_coded(mb_p, blk_p) || deblock_ref_block_coded(mb_q, blk_q))
      return 2;
   if (mb_p->ref_l0[blk_p] != mb_q->ref_l0[blk_q])
      return 1;
   int dx = mb_p->mv_l0[blk_p][0] - mb_q->mv_l0[blk_q][0];
   int dy = mb_p->mv_l0[blk_p][1] - mb_q->mv_l0[blk_q][1];
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

/* The bS=4 strong luma filter, ITU-T H.264 sec 8.7.2.4, over eight samples
 * s = p3,p2,p1,p0,q0,q1,q2,q3, writing p2',p1',p0',q0',q1',q2' into out; mirrors
 * vl_h264_deblock.c deblock_strong_filter.  strong_thr is alpha/4 + 2. */
static inline void
deblock_ref_strong(const int s[8], int alpha, int beta, int strong_thr, int out[6])
{
   int p3 = s[0], p2 = s[1], p1 = s[2], p0 = s[3];
   int q0 = s[4], q1 = s[5], q2 = s[6], q3 = s[7];

   out[0] = p2; out[1] = p1; out[2] = p0;
   out[3] = q0; out[4] = q1; out[5] = q2;

   bool on = abs(p0 - q0) < alpha && abs(p1 - p0) < beta && abs(q1 - q0) < beta;
   if (!on)
      return;

   bool ap = abs(p2 - p0) < beta;
   bool aq = abs(q2 - q0) < beta;
   bool strong = abs(p0 - q0) < strong_thr;

   if (strong && ap) {
      out[2] = (p2 + 2 * p1 + 2 * p0 + 2 * q0 + q1 + 4) >> 3;
      out[1] = (p2 + p1 + p0 + q0 + 2) >> 2;
      out[0] = (2 * p3 + 3 * p2 + p1 + p0 + q0 + 4) >> 3;
   } else {
      out[2] = (2 * p1 + p0 + q1 + 2) >> 2;
   }
   if (strong && aq) {
      out[3] = (q2 + 2 * q1 + 2 * q0 + 2 * p0 + p1 + 4) >> 3;
      out[4] = (q2 + q1 + q0 + p0 + 2) >> 2;
      out[5] = (2 * q3 + 3 * q2 + q1 + q0 + p0 + 4) >> 3;
   } else {
      out[3] = (2 * q1 + q0 + p1 + 2) >> 2;
   }
}

/* Filter one luma edge segment of four samples in place at (ex0, ey0), stepping
 * (sx, sy) along the edge normal: bS=4 takes the strong filter (eight inputs
 * p3..q3, six outputs p2..q2), bS 1..3 the normal filter (six inputs, four
 * outputs p1..q1). */
static inline void
deblock_ref_segment(int *pic, int w, int h, int bs, int alpha, int beta, int tc0,
                    int strong_thr, int ex0, int ey0, int sx, int sy)
{
   for (int t = 0; t < 4; ++t) {
      int ex = ex0 + (sy ? t : 0);   /* march the segment along the edge */
      int ey = ey0 + (sx ? t : 0);
      if (bs == 4) {
         int s[8], out[6];
         for (int k = 0; k < 8; ++k) {
            int xx = deblock_ref_clamp(ex + sx * (k - 4), 0, w - 1);
            int yy = deblock_ref_clamp(ey + sy * (k - 4), 0, h - 1);
            s[k] = pic[yy * w + xx];
         }
         deblock_ref_strong(s, alpha, beta, strong_thr, out);
         for (int lane = 0; lane < 6; ++lane)
            pic[(ey + sy * (lane - 3)) * w + ex + sx * (lane - 3)] = out[lane];
      } else {
         int s[6], out[4];
         for (int k = 0; k < 6; ++k) {
            int xx = deblock_ref_clamp(ex + sx * (k - 3), 0, w - 1);
            int yy = deblock_ref_clamp(ey + sy * (k - 3), 0, h - 1);
            s[k] = pic[yy * w + xx];
         }
         deblock_ref_filter(s, alpha, beta, tc0, out);
         for (int lane = 0; lane < 4; ++lane)
            pic[(ey + sy * (lane - 2)) * w + ex + sx * (lane - 2)] = out[lane];
      }
   }
}

/* Apply the full luma deblock to an integer plane in place: each macroblock in
 * raster order is filtered completely (the four vertical edges left to right, then
 * the four horizontal edges top to bottom, including the macroblock-boundary edges
 * at offset 0), the per-macroblock spec order proven bit-exact against ffmpeg and
 * equal to the GPU's anti-diagonal wavefront.  Returns the number of samples the
 * filter changed, so a caller can reject a vacuous fixture. */
static inline unsigned
deblock_reference(int *pic, int w, int h,
                  const struct vl_h264_slice_contract *slice)
{
   const int MBSZ = DEBLOCK_REF_MB;
   int mbw = (w + MBSZ - 1) / MBSZ, mbh = (h + MBSZ - 1) / MBSZ;
   const struct vl_h264_mb_contract **grid =
      calloc((size_t)mbw * mbh, sizeof(*grid));
   for (unsigned m = 0; m < slice->num_macroblocks; ++m) {
      const struct vl_h264_mb_contract *mb = &slice->macroblocks[m];
      if (mb->mb_x >= 0 && mb->mb_x < mbw && mb->mb_y >= 0 && mb->mb_y < mbh)
         grid[mb->mb_y * mbw + mb->mb_x] = mb;
   }

   int *orig = malloc((size_t)w * h * sizeof(int));
   memcpy(orig, pic, (size_t)w * h * sizeof(int));

   for (int mby = 0; mby < mbh; ++mby) {
      for (int mbx = 0; mbx < mbw; ++mbx) {
         const struct vl_h264_mb_contract *mb = grid[mby * mbw + mbx];
         if (!mb || mb->disable_deblock_idc == 1)
            continue;

         for (int pass = 0; pass < 8; ++pass) {
            bool vertical = pass < 4;
            int r = 4 * (vertical ? pass : pass - 4);   /* 0, 4, 8, 12 */
            /* An 8x8-transform macroblock skips the interior 4x4 edges at 4 and
             * 12 (ITU-T H.264 sec 8.7.2); only the boundary at 0 and the 8x8-block
             * edge at 8 are filtered. */
            if (mb->transform_8x8 && (r == 4 || r == 12))
               continue;
            bool mb_boundary = (r == 0);
            if (mb_boundary && vertical && mbx == 0)
               continue;
            if (mb_boundary && !vertical && mby == 0)
               continue;
            const struct vl_h264_mb_contract *mb_p = mb;
            if (mb_boundary)
               mb_p = vertical ? grid[mby * mbw + (mbx - 1)]
                               : grid[(mby - 1) * mbw + mbx];
            if (!mb_p)
               continue;
            /* Thresholds index by qPav = (qPp + qPq + 1) >> 1 (sec 8.7.2.2); for an
             * internal edge mb_p == mb so qPav is the macroblock QP. */
            int qp_av = (mb_p->qp_y + mb->qp_y + 1) >> 1;
            int ia = deblock_ref_qp_index(qp_av, mb->slice_alpha_c0_offset_div2 * 2);
            int ib = deblock_ref_qp_index(qp_av, mb->slice_beta_offset_div2 * 2);
            int alpha = deblock_ref_alpha[ia];
            int beta = deblock_ref_beta[ib];
            int strong_thr = (alpha >> 2) + 2;
            int col = r / 4;

            for (int seg = 0; seg < 4; ++seg) {
               int blk_q, blk_p;
               if (vertical) {
                  blk_q = seg * 4 + col;
                  blk_p = mb_boundary ? seg * 4 + 3 : seg * 4 + (col - 1);
               } else {
                  blk_q = col * 4 + seg;
                  blk_p = mb_boundary ? 12 + seg : (col - 1) * 4 + seg;
               }
               int bs = deblock_ref_strength(mb_p, blk_p, mb, blk_q, mb_boundary);
               if (bs == 0)
                  continue;
               int tc0 = bs <= 3 ? deblock_ref_tc0[bs - 1][ia] : 0;

               int ed = (vertical ? mbx : mby) * MBSZ + r;
               int basec = (vertical ? mby : mbx) * MBSZ + seg * 4;
               int ex0 = vertical ? ed : basec;
               int ey0 = vertical ? basec : ed;
               deblock_ref_segment(pic, w, h, bs, alpha, beta, tc0, strong_thr,
                                   ex0, ey0, vertical ? 1 : 0, vertical ? 0 : 1);
            }
         }
      }
   }

   unsigned changed = 0;
   for (int i = 0; i < w * h; ++i)
      if (pic[i] != orig[i])
         ++changed;

   free(orig);
   free(grid);
   return changed;
}

/* Chroma edge filter (sec 8.7.2.3/4, chromaEdgeFlag=1); mirrors
 * build_chroma_deblock_apply.  Only p0 and q0 change: bS=4 is the unconditional
 * two-tap, bS 1..3 the normal delta with tC = tC0 + 1. */
static inline void
deblock_chroma_ref_filter(int p1, int p0, int q0, int q1, int alpha, int beta,
                          int tc0, int bs, int *p0n, int *q0n)
{
   if (!(abs(p0 - q0) < alpha && abs(p1 - p0) < beta && abs(q1 - q0) < beta)) {
      *p0n = p0;
      *q0n = q0;
      return;
   }
   if (bs == 4) {
      *p0n = (2 * p1 + p0 + q1 + 2) >> 2;
      *q0n = (2 * q1 + q0 + p1 + 2) >> 2;
   } else {
      int tc = tc0 + 1;
      int delta = deblock_ref_clamp((((q0 - p0) << 2) + (p1 - q1) + 4) >> 3, -tc, tc);
      *p0n = deblock_ref_clamp(p0 + delta, 0, 255);
      *q0n = deblock_ref_clamp(q0 - delta, 0, 255);
   }
}

/* Apply the in-loop chroma deblock to one integer component plane in place, the
 * same per-macroblock order vl_h264_emit_deblock_chroma runs: each 8x8 chroma
 * macroblock filters the two vertical edges (chroma offsets 0,4) then the two
 * horizontal, boundary strength inherited from the co-located luma edge, two-row
 * sub-segments, thresholds at the component's chroma QP averaged across the edge.
 * use_cr picks qp_cr over qp_cb.  Returns the number of samples changed. */
static inline unsigned
deblock_chroma_reference(int *pic, int w, int h,
                         const struct vl_h264_slice_contract *slice, bool use_cr)
{
   const int MBSZ = 8;
   int mbw = (w + MBSZ - 1) / MBSZ, mbh = (h + MBSZ - 1) / MBSZ;
   const struct vl_h264_mb_contract **grid =
      calloc((size_t)mbw * mbh, sizeof(*grid));
   for (unsigned m = 0; m < slice->num_macroblocks; ++m) {
      const struct vl_h264_mb_contract *mb = &slice->macroblocks[m];
      if (mb->mb_x >= 0 && mb->mb_x < mbw && mb->mb_y >= 0 && mb->mb_y < mbh)
         grid[mb->mb_y * mbw + mb->mb_x] = mb;
   }
   int *orig = malloc((size_t)w * h * sizeof(int));
   memcpy(orig, pic, (size_t)w * h * sizeof(int));

   for (int mby = 0; mby < mbh; ++mby) {
      for (int mbx = 0; mbx < mbw; ++mbx) {
         const struct vl_h264_mb_contract *mb = grid[mby * mbw + mbx];
         if (!mb || mb->disable_deblock_idc == 1)
            continue;

         for (int pass = 0; pass < 4; ++pass) {
            bool vertical = pass < 2;
            int co = 4 * (vertical ? pass : pass - 2);
            bool mb_boundary = (co == 0);
            if (mb_boundary && vertical && mbx == 0)
               continue;
            if (mb_boundary && !vertical && mby == 0)
               continue;
            const struct vl_h264_mb_contract *mb_p = mb;
            if (mb_boundary)
               mb_p = vertical ? grid[mby * mbw + (mbx - 1)]
                               : grid[(mby - 1) * mbw + mbx];
            if (!mb_p)
               continue;

            int qp_q = use_cr ? mb->qp_cr : mb->qp_cb;
            int qp_p = use_cr ? mb_p->qp_cr : mb_p->qp_cb;
            int qp_av = (qp_p + qp_q + 1) >> 1;
            int ia = deblock_ref_qp_index(qp_av, mb->slice_alpha_c0_offset_div2 * 2);
            int ib = deblock_ref_qp_index(qp_av, mb->slice_beta_offset_div2 * 2);
            int alpha = deblock_ref_alpha[ia];
            int beta = deblock_ref_beta[ib];
            int luma_col = co / 2;

            for (int seg = 0; seg < 4; ++seg) {
               int blk_q, blk_p;
               if (vertical) {
                  blk_q = seg * 4 + luma_col;
                  blk_p = mb_boundary ? seg * 4 + 3 : seg * 4 + (luma_col - 1);
               } else {
                  blk_q = luma_col * 4 + seg;
                  blk_p = mb_boundary ? 12 + seg : (luma_col - 1) * 4 + seg;
               }
               int bs = deblock_ref_strength(mb_p, blk_p, mb, blk_q, mb_boundary);
               if (bs == 0)
                  continue;
               int tc0 = bs <= 3 ? deblock_ref_tc0[bs - 1][ia] : 0;

               int ed = (vertical ? mbx : mby) * MBSZ + co;
               int base = (vertical ? mby : mbx) * MBSZ + seg * 2;
               for (int t = 0; t < 2; ++t) {
                  int along = base + t;
                  int p0n, q0n;
                  if (vertical) {
                     deblock_chroma_ref_filter(pic[along * w + ed - 2],
                        pic[along * w + ed - 1], pic[along * w + ed],
                        pic[along * w + ed + 1], alpha, beta, tc0, bs, &p0n, &q0n);
                     pic[along * w + ed - 1] = p0n;
                     pic[along * w + ed] = q0n;
                  } else {
                     deblock_chroma_ref_filter(pic[(ed - 2) * w + along],
                        pic[(ed - 1) * w + along], pic[ed * w + along],
                        pic[(ed + 1) * w + along], alpha, beta, tc0, bs, &p0n, &q0n);
                     pic[(ed - 1) * w + along] = p0n;
                     pic[ed * w + along] = q0n;
                  }
               }
            }
         }
      }
   }

   unsigned changed = 0;
   for (int i = 0; i < w * h; ++i)
      if (pic[i] != orig[i])
         ++changed;
   free(orig);
   free(grid);
   return changed;
}

#endif /* vl_h264_deblock_ref_h */
