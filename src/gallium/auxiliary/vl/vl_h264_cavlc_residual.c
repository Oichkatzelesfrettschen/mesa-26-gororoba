/*
 * Copyright (c) 2026 Terascale Functionalists
 * SPDX-License-Identifier: MIT
 */

#include <string.h>

#include "vl_h264_cavlc.h"
#include "vl_h264_cavlc_residual.h"

/*
 * Clean-room from ITU-T H.264 sec 7.3.5.3 (residual traversal) and 9.2.1 (the nC
 * neighbour derivation).  No third-party decoder source was consulted.
 */

/* 4x4 luma block scan to position in 4-sample units, and the inverse (sec
 * 6.4.3); the same mapping the intra mode prediction uses. */
static const uint8_t blk_x[16] = { 0, 1, 0, 1, 2, 3, 2, 3, 0, 1, 0, 1, 2, 3, 2, 3 };
static const uint8_t blk_y[16] = { 0, 0, 1, 1, 0, 0, 1, 1, 2, 2, 3, 3, 2, 2, 3, 3 };

static unsigned
xy_to_blk(unsigned bx, unsigned by)
{
   for (unsigned i = 0; i < 16; i++)
      if (blk_x[i] == bx && blk_y[i] == by)
         return i;
   return 0;
}

/* A neighbour macroblock is available when it is inside the frame, in the same
 * slice, and already decoded (raster order). */
static bool
mb_available(const struct vl_h264_mb_decoder *dec, int mb_x, int mb_y,
             unsigned cur_mb_addr)
{
   if (mb_x < 0 || mb_y < 0 || (unsigned)mb_x >= dec->width_in_mbs ||
       (unsigned)mb_y >= dec->height_in_mbs)
      return false;
   unsigned addr = mb_y * dec->width_in_mbs + mb_x;
   return addr >= dec->slice->first_mb_in_slice && addr < cur_mb_addr;
}

/*
 * nC for a luma 4x4 block (sec 9.2.1): the average of the left and top neighbour
 * blocks' TotalCoeff when both are available, one of them when only one is, or
 * zero.  Within-macroblock neighbours are always already decoded in scan order.
 */
static int
luma_nc(const struct vl_h264_mb_decoder *dec, unsigned mb_x, unsigned mb_y,
        unsigned blk)
{
   unsigned cur = mb_y * dec->width_in_mbs + mb_x;
   unsigned x4 = blk_x[blk], y4 = blk_y[blk];
   bool avail_a, avail_b;
   int na = 0, nb = 0;

   if (x4 > 0) {
      avail_a = true;
      na = dec->nz_luma[cur * 16 + xy_to_blk(x4 - 1, y4)];
   } else {
      avail_a = mb_available(dec, (int)mb_x - 1, mb_y, cur);
      if (avail_a)
         na = dec->nz_luma[(cur - 1) * 16 + xy_to_blk(3, y4)];
   }

   if (y4 > 0) {
      avail_b = true;
      nb = dec->nz_luma[cur * 16 + xy_to_blk(x4, y4 - 1)];
   } else {
      avail_b = mb_available(dec, mb_x, (int)mb_y - 1, cur);
      if (avail_b)
         nb = dec->nz_luma[(cur - dec->width_in_mbs) * 16 + xy_to_blk(x4, 3)];
   }

   if (avail_a && avail_b)
      return (na + nb + 1) >> 1;
   if (avail_a)
      return na;
   if (avail_b)
      return nb;
   return 0;
}

bool
vl_h264_decode_mb_luma_residual(struct vl_h264_mb_decoder *dec,
                                struct vl_h264_reader *reader, unsigned mb_x,
                                unsigned mb_y,
                                const struct vl_h264_mb_contract *mb,
                                struct vl_h264_mb_residual *res)
{
   unsigned cur = mb_y * dec->width_in_mbs + mb_x;

   memset(res, 0, sizeof(*res));

   for (unsigned blk = 0; blk < 16; blk++) {
      /* cbp_luma has one bit per 8x8 luma group; the four 4x4 blocks of an
       * uncoded group are all zero and contribute zero to a later nC. */
      unsigned group = blk / 4;
      if (!(mb->cbp_luma & (1u << group))) {
         dec->nz_luma[cur * 16 + blk] = 0;
         res->nz_luma[blk] = 0;
         continue;
      }

      struct vl_h264_cavlc_block block;
      int nc = luma_nc(dec, mb_x, mb_y, blk);
      if (!vl_h264_cavlc_residual_block(reader, 16, nc, &block))
         return false;

      dec->nz_luma[cur * 16 + blk] = (uint8_t)block.total_coeff;
      res->nz_luma[blk] = (uint8_t)block.total_coeff;
      memcpy(res->luma4x4[blk], block.coeff, sizeof(block.coeff));
   }
   return true;
}
