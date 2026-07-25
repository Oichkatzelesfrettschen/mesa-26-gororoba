/*
 * SPDX-License-Identifier: MIT
 */

/* CPU in-loop deblocking filter (ITU-T H.264 sec 8.7) over the fully
 * reconstructed frame.  The decode path reconstructs inter macroblocks on the
 * GPU back half and intra macroblocks on the CPU, both un-deblocked; this pass
 * runs once over the final luma and chroma planes so every edge -- intra,
 * inter, and the boundaries between them -- is filtered in the spec's order.
 * It replaces the per-inter-emit GPU deblock, which ran before the intra
 * macroblocks existed and so could not filter them or their boundaries.
 *
 * The thresholds (Table 8-16 alpha/beta, Table 8-17 tc0), the QP averaging
 * (sec 8.7.2.2), and the boundary strength (sec 8.7.2.1) mirror the GPU emit
 * path in vl_h264_emit.c; both implement the same normative tables, and the
 * filter arithmetic mirrors the FP24 NIR kernels in vl_h264_deblock.c.
 */

#include "vl_h264_deblock_cpu.h"

#include "util/u_math.h"
#include "util/u_memory.h"

#include <stdbool.h>

#define MB 16
#define LUMA_BLOCKS_PER_ROW 4

static const int deblock_alpha[52] = {
   0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 4, 4, 5, 6, 7, 8, 9, 10, 12,
   13, 15, 17, 20, 22, 25, 28, 32, 36, 40, 45, 50, 56, 63, 71, 80, 90, 101, 113,
   127, 144, 162, 182, 203, 226, 255, 255};
static const int deblock_beta[52] = {
   0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4,
   6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13, 14, 14, 15, 15, 16,
   16, 17, 17, 18, 18};
static const int deblock_tc0[3][52] = {
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

static inline int iabs(int v) { return v < 0 ? -v : v; }
static inline int clip3(int lo, int hi, int v)
{
   return v < lo ? lo : (v > hi ? hi : v);
}
static inline int clip1(int v) { return clip3(0, 255, v); }

static int
qp_index(int qp, int offset)
{
   int i = qp + offset;
   return i < 0 ? 0 : (i > 51 ? 51 : i);
}

static int
qp_avg(int qp_p, int qp_q)
{
   return (qp_p + qp_q + 1) >> 1;
}

/* Whether a 4x4 luma block carries a nonzero coefficient (raises bS to 2). */
static bool
block_coded(const struct vl_h264_mb_contract *mb, unsigned blk)
{
   if (mb->transform_8x8) {
      unsigned by = blk / LUMA_BLOCKS_PER_ROW, bx = blk % LUMA_BLOCKS_PER_ROW;
      unsigned quad = (by / 2) * 2 + (bx / 2);
      for (unsigned k = 0; k < 64; ++k)
         if (mb->coeff8x8[quad][k] != 0)
            return true;
      return false;
   }
   for (unsigned k = 0; k < 16; ++k)
      if (mb->coeff4x4[blk][k] != 0)
         return true;
   return false;
}

/* Boundary strength across a luma block edge (ITU-T H.264 sec 8.7.2.1). */
static int
boundary_strength(const struct vl_h264_mb_contract *mb_p, unsigned blk_p,
                  const struct vl_h264_mb_contract *mb_q, unsigned blk_q,
                  bool mb_boundary)
{
   bool p_intra = mb_p->ref_l0[blk_p] < 0 && mb_p->ref_l1[blk_p] < 0;
   bool q_intra = mb_q->ref_l0[blk_q] < 0 && mb_q->ref_l1[blk_q] < 0;
   if (p_intra || q_intra)
      return mb_boundary ? 4 : 3;
   if (block_coded(mb_p, blk_p) || block_coded(mb_q, blk_q))
      return 2;
   if (mb_p->ref_l0[blk_p] != mb_q->ref_l0[blk_q])
      return 1;
   int dx = mb_p->mv_l0[blk_p][0] - mb_q->mv_l0[blk_q][0];
   int dy = mb_p->mv_l0[blk_p][1] - mb_q->mv_l0[blk_q][1];
   if (dx <= -4 || dx >= 4 || dy <= -4 || dy >= 4)
      return 1;
   return 0;
}

/* Filter one luma line across an edge in place.  q0p addresses the q0 sample;
 * step is the across-edge stride (1 for a vertical edge, the row stride for a
 * horizontal edge).  The normal filter (bS 1..3, sec 8.7.2.3) updates p0/q0 and
 * conditionally p1/q1; the strong filter (bS 4, sec 8.7.2.4) updates up to
 * p2..q2 per side.  All eight inputs are read before any write. */
static void
filter_luma_line(uint8_t *q0p, int step, int bs, int ia, int alpha, int beta)
{
   int p3 = q0p[-4 * step], p2 = q0p[-3 * step], p1 = q0p[-2 * step];
   int p0 = q0p[-step], q0 = q0p[0], q1 = q0p[step];
   int q2 = q0p[2 * step], q3 = q0p[3 * step];

   if (!(iabs(p0 - q0) < alpha && iabs(p1 - p0) < beta && iabs(q1 - q0) < beta))
      return;

   bool ap = iabs(p2 - p0) < beta;
   bool aq = iabs(q2 - q0) < beta;

   if (bs < 4) {
      int tc0 = deblock_tc0[bs - 1][ia];
      int tc = tc0 + ap + aq;
      int delta = clip3(-tc, tc, (((q0 - p0) << 2) + (p1 - q1) + 4) >> 3);
      q0p[-step] = clip1(p0 + delta);
      q0p[0] = clip1(q0 - delta);
      if (ap)
         q0p[-2 * step] =
            p1 + clip3(-tc0, tc0, (p2 + ((p0 + q0 + 1) >> 1) - 2 * p1) >> 1);
      if (aq)
         q0p[step] =
            q1 + clip3(-tc0, tc0, (q2 + ((p0 + q0 + 1) >> 1) - 2 * q1) >> 1);
      return;
   }

   bool strong = iabs(p0 - q0) < ((alpha >> 2) + 2);
   if (strong && ap) {
      q0p[-step] = (p2 + 2 * p1 + 2 * p0 + 2 * q0 + q1 + 4) >> 3;
      q0p[-2 * step] = (p2 + p1 + p0 + q0 + 2) >> 2;
      q0p[-3 * step] = (2 * p3 + 3 * p2 + p1 + p0 + q0 + 4) >> 3;
   } else {
      q0p[-step] = (2 * p1 + p0 + q1 + 2) >> 2;
   }
   if (strong && aq) {
      q0p[0] = (q2 + 2 * q1 + 2 * q0 + 2 * p0 + p1 + 4) >> 3;
      q0p[step] = (q2 + q1 + q0 + p0 + 2) >> 2;
      q0p[2 * step] = (2 * q3 + 3 * q2 + q1 + q0 + p0 + 4) >> 3;
   } else {
      q0p[0] = (2 * q1 + q0 + p1 + 2) >> 2;
   }
}

/* Filter one chroma line across an edge in place (chromaEdgeFlag = 1): the
 * normal filter (bS 1..3) updates only p0/q0 with tC = tC0 + 1; the bS=4 filter
 * is the unconditional two-tap average. */
static void
filter_chroma_line(uint8_t *q0p, int step, int bs, int ia, int alpha, int beta)
{
   int p1 = q0p[-2 * step], p0 = q0p[-step], q0 = q0p[0], q1 = q0p[step];

   if (!(iabs(p0 - q0) < alpha && iabs(p1 - p0) < beta && iabs(q1 - q0) < beta))
      return;

   if (bs < 4) {
      int tc = deblock_tc0[bs - 1][ia] + 1;
      int delta = clip3(-tc, tc, (((q0 - p0) << 2) + (p1 - q1) + 4) >> 3);
      q0p[-step] = clip1(p0 + delta);
      q0p[0] = clip1(q0 - delta);
   } else {
      q0p[-step] = (2 * p1 + p0 + q1 + 2) >> 2;
      q0p[0] = (2 * q1 + q0 + p1 + 2) >> 2;
   }
}

/* Luma plane: macroblocks in raster order, each one's four vertical edges
 * (offsets 0,4,8,12) left to right then its four horizontal edges top to
 * bottom, each edge filtered per four-row/column segment with its own boundary
 * strength, in place. */
static void
deblock_luma(const struct vl_h264_mb_contract **grid, unsigned mbw,
             unsigned mbh, uint8_t *y, int stride)
{
   for (unsigned mby = 0; mby < mbh; ++mby) {
      for (unsigned mbx = 0; mbx < mbw; ++mbx) {
         const struct vl_h264_mb_contract *mb = grid[mby * mbw + mbx];
         if (!mb || mb->disable_deblock_idc == 1)
            continue;
         const int alpha_off = (int)mb->slice_alpha_c0_offset_div2 * 2;
         const int beta_off = (int)mb->slice_beta_offset_div2 * 2;

         for (unsigned dir = 0; dir < 2; ++dir) {
            const bool vertical = dir == 0;
            for (unsigned e = 0; e < 4; ++e) {
               const int r = (int)e * 4;
               if (mb->transform_8x8 && (r == 4 || r == 12))
                  continue;
               const bool mb_boundary = (r == 0);
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

               const int qpav = qp_avg(mb_p->qp_y, mb->qp_y);
               const int ia = qp_index(qpav, alpha_off);
               const int ib = qp_index(qpav, beta_off);
               const int alpha = deblock_alpha[ia];
               const int beta = deblock_beta[ib];
               const int col = r / 4;

               for (int seg = 0; seg < LUMA_BLOCKS_PER_ROW; ++seg) {
                  unsigned blk_q, blk_p;
                  if (vertical) {
                     blk_q = seg * LUMA_BLOCKS_PER_ROW + col;
                     blk_p = mb_boundary
                        ? seg * LUMA_BLOCKS_PER_ROW + (LUMA_BLOCKS_PER_ROW - 1)
                        : seg * LUMA_BLOCKS_PER_ROW + (col - 1);
                  } else {
                     blk_q = col * LUMA_BLOCKS_PER_ROW + seg;
                     blk_p = mb_boundary
                        ? (LUMA_BLOCKS_PER_ROW - 1) * LUMA_BLOCKS_PER_ROW + seg
                        : (col - 1) * LUMA_BLOCKS_PER_ROW + seg;
                  }
                  int bs = boundary_strength(mb_p, blk_p, mb, blk_q, mb_boundary);
                  if (bs == 0)
                     continue;

                  const int edge = (int)(vertical ? mbx : mby) * MB + r;
                  const int base = (int)(vertical ? mby : mbx) * MB + seg * 4;
                  for (int line = 0; line < 4; ++line) {
                     uint8_t *q0p = vertical
                        ? y + (base + line) * stride + edge
                        : y + edge * stride + (base + line);
                     int step = vertical ? 1 : stride;
                     filter_luma_line(q0p, step, bs, ia, alpha, beta);
                  }
               }
            }
         }
      }
   }
}

/* Chroma plane (4:2:0): edges at chroma offsets 0 and 4 (co-located with luma
 * offsets 0 and 8); each chroma sample inherits the boundary strength of the
 * luma block edge at twice its position, and Cb/Cr use their own QP. */
static void
deblock_chroma(const struct vl_h264_mb_contract **grid, unsigned mbw,
               unsigned mbh, uint8_t *c, int stride, bool is_cr)
{
   for (unsigned mby = 0; mby < mbh; ++mby) {
      for (unsigned mbx = 0; mbx < mbw; ++mbx) {
         const struct vl_h264_mb_contract *mb = grid[mby * mbw + mbx];
         if (!mb || mb->disable_deblock_idc == 1)
            continue;
         const int alpha_off = (int)mb->slice_alpha_c0_offset_div2 * 2;
         const int beta_off = (int)mb->slice_beta_offset_div2 * 2;

         for (unsigned dir = 0; dir < 2; ++dir) {
            const bool vertical = dir == 0;
            for (unsigned e = 0; e < 2; ++e) {
               const int cr = (int)e * 4;          /* chroma edge offset 0 or 4 */
               const int luma_r = cr * 2;          /* co-located luma 0 or 8 */
               const bool mb_boundary = (cr == 0);
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

               const int qp_p = is_cr ? mb_p->qp_cr : mb_p->qp_cb;
               const int qp_q = is_cr ? mb->qp_cr : mb->qp_cb;
               const int qpav = qp_avg(qp_p, qp_q);
               const int ia = qp_index(qpav, alpha_off);
               const int ib = qp_index(qpav, beta_off);
               const int alpha = deblock_alpha[ia];
               const int beta = deblock_beta[ib];
               const int col = luma_r / 4;          /* luma block column 0 or 2 */

               /* Eight chroma samples along the edge; sample yc inherits the
                * luma bS at block-row yc/2. */
               const int edge = (int)(vertical ? mbx : mby) * 8 + cr;
               const int cbase = (int)(vertical ? mby : mbx) * 8;
               for (int s = 0; s < 8; ++s) {
                  const int brow = s / 2;           /* luma block row 0..3 */
                  unsigned blk_q, blk_p;
                  if (vertical) {
                     blk_q = brow * LUMA_BLOCKS_PER_ROW + col;
                     blk_p = mb_boundary
                        ? brow * LUMA_BLOCKS_PER_ROW + (LUMA_BLOCKS_PER_ROW - 1)
                        : brow * LUMA_BLOCKS_PER_ROW + (col - 1);
                  } else {
                     blk_q = col * LUMA_BLOCKS_PER_ROW + brow;
                     blk_p = mb_boundary
                        ? (LUMA_BLOCKS_PER_ROW - 1) * LUMA_BLOCKS_PER_ROW + brow
                        : (col - 1) * LUMA_BLOCKS_PER_ROW + brow;
                  }
                  int bs = boundary_strength(mb_p, blk_p, mb, blk_q, mb_boundary);
                  if (bs == 0)
                     continue;

                  uint8_t *q0p = vertical ? c + (cbase + s) * stride + edge
                                          : c + edge * stride + (cbase + s);
                  int step = vertical ? 1 : stride;
                  filter_chroma_line(q0p, step, bs, ia, alpha, beta);
               }
            }
         }
      }
   }
}

void
vl_h264_deblock_cpu(const struct vl_h264_slice_contract *slice, unsigned mbw,
                    unsigned mbh, uint8_t *y, int y_stride, uint8_t *cb,
                    uint8_t *cr, int c_stride)
{
   const struct vl_h264_mb_contract **grid =
      CALLOC((size_t)mbw * mbh, sizeof(*grid));
   if (!grid)
      return;
   for (unsigned m = 0; m < slice->num_macroblocks; ++m) {
      const struct vl_h264_mb_contract *mb = &slice->macroblocks[m];
      if ((unsigned)mb->mb_x < mbw && (unsigned)mb->mb_y < mbh)
         grid[(unsigned)mb->mb_y * mbw + (unsigned)mb->mb_x] = mb;
   }

   if (y)
      deblock_luma(grid, mbw, mbh, y, y_stride);
   if (cb)
      deblock_chroma(grid, mbw, mbh, cb, c_stride, false);
   if (cr)
      deblock_chroma(grid, mbw, mbh, cr, c_stride, true);

   FREE(grid);
}
