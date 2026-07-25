/*
 * SPDX-License-Identifier: MIT
 */

#include <stdbool.h>

#include "vl_h264_cavlc_residual.h"
#include "vl_h264_dequant.h"
#include "vl_h264_mb_contract.h"

/*
 * Clean-room from ITU-T H.264 sec 8.5.6/8.5.7 (DC Hadamard), 8.5.8 (inverse
 * scan), and 8.5.9 (residual dequant).  No third-party decoder source was
 * consulted.
 */

/* Inverse 4x4 zig-zag scan (sec 8.5.6): scan position k maps to the raster index
 * row * 4 + col. */
static const uint8_t zigzag_to_raster[16] = {
   0, 1, 4, 8, 5, 2, 3, 6, 9, 12, 13, 10, 7, 11, 14, 15,
};

/* Position class per raster 4x4 coefficient (sec 8.5.9): 0 = both even, 1 = both
 * odd, 2 = otherwise.  Symmetric, so dequant commutes with a transpose. */
static const uint8_t class_4x4[16] = {
   0, 2, 0, 2, 2, 1, 2, 1, 0, 2, 0, 2, 2, 1, 2, 1,
};

/* normAdjust V(m, class) indexed by qP % 6 then class (sec 8.5.9, Table 8-13). */
static const uint8_t norm_adjust[6][3] = {
   { 10, 16, 13 }, { 11, 18, 14 }, { 13, 20, 16 },
   { 14, 23, 18 }, { 16, 25, 20 }, { 18, 29, 23 },
};

/* Dequantize one residual coefficient (sec 8.5.9): LevelScale = 16 * normAdjust
 * (flat weightScale), then a left shift for qP >= 24 or a rounded right shift
 * below it. */
static int
dequant_ac_coeff(int level, int qp, unsigned cls)
{
   int scale = 16 * norm_adjust[qp % 6][cls];
   /* Scale by a power of two with a multiply rather than a left shift: the value
    * is signed and a left shift of a negative is undefined, and a malformed
    * stream can drive the product past 32 bits, so compute in 64-bit. */
   if (qp >= 24)
      return (int)((int64_t)level * scale * ((int64_t)1 << (qp / 6 - 4)));
   int shift = 4 - qp / 6;
   return (int)(((int64_t)level * scale + (1 << (shift - 1))) >> shift);
}

void
vl_h264_dequant_4x4(const int16_t scan[16], int qp, int16_t out[16])
{
   int16_t raster[16] = { 0 };
   for (unsigned k = 0; k < 16; k++)
      raster[zigzag_to_raster[k]] = scan[k];
   for (unsigned p = 0; p < 16; p++)
      out[p] = (int16_t)dequant_ac_coeff(raster[p], qp, class_4x4[p]);
}

void
vl_h264_dequant_4x4_ac(const int16_t ac_scan[15], int qp, int16_t out[16])
{
   int16_t raster[16] = { 0 };
   /* The fifteen AC levels occupy scan positions 1..15; position 0 is the DC,
    * coded separately and filled by the caller. */
   for (unsigned k = 0; k < 15; k++)
      raster[zigzag_to_raster[k + 1]] = ac_scan[k];
   out[0] = 0;
   for (unsigned p = 1; p < 16; p++)
      out[p] = (int16_t)dequant_ac_coeff(raster[p], qp, class_4x4[p]);
}

/* The 4x4 Hadamard basis for the Intra_16x16 luma DC transform (sec 8.5.6); it
 * is symmetric, so f = H c H. */
static const int8_t hadamard_4x4[4][4] = {
   { 1, 1, 1, 1 }, { 1, 1, -1, -1 }, { 1, -1, -1, 1 }, { 1, -1, 1, -1 },
};

void
vl_h264_dequant_luma_dc(const int16_t scan[16], int qp, int16_t dc[16])
{
   int c[4][4];
   for (unsigned k = 0; k < 16; k++) {
      unsigned p = zigzag_to_raster[k];
      c[p / 4][p % 4] = scan[k];
   }

   /* f = H c H, in two passes. */
   int tmp[4][4], f[4][4];
   for (unsigned i = 0; i < 4; i++)
      for (unsigned n = 0; n < 4; n++)
         tmp[i][n] = hadamard_4x4[i][0] * c[0][n] + hadamard_4x4[i][1] * c[1][n]
                   + hadamard_4x4[i][2] * c[2][n] + hadamard_4x4[i][3] * c[3][n];
   for (unsigned i = 0; i < 4; i++)
      for (unsigned j = 0; j < 4; j++)
         f[i][j] = tmp[i][0] * hadamard_4x4[j][0] + tmp[i][1] * hadamard_4x4[j][1]
                 + tmp[i][2] * hadamard_4x4[j][2] + tmp[i][3] * hadamard_4x4[j][3];

   /* Scale with the DC LevelScale at position (0,0), class 0.  This is the
    * final-edition scaling that conforming decoders use; the 2002 draft's
    * << (qP/6 - 2) overflows the dynamic range and is not what ffmpeg applies. */
   int scale = 16 * norm_adjust[qp % 6][0];
   for (unsigned i = 0; i < 4; i++) {
      for (unsigned j = 0; j < 4; j++) {
         int64_t val;
         if (qp >= 36)
            val = (int64_t)f[i][j] * scale * ((int64_t)1 << (qp / 6 - 6));
         else
            val = ((int64_t)f[i][j] * scale + (1 << (5 - qp / 6))) >> (6 - qp / 6);
         dc[i * 4 + j] = (int16_t)val;
      }
   }
}

void
vl_h264_dequant_chroma_dc(const int16_t level[4], int qp, int16_t dc[4])
{
   /* 2x2 inverse Hadamard f = H2 c H2, c in raster order. */
   int c00 = level[0], c01 = level[1], c10 = level[2], c11 = level[3];
   const int f[4] = {
      c00 + c01 + c10 + c11,
      c00 - c01 + c10 - c11,
      c00 + c01 - c10 - c11,
      c00 - c01 - c10 + c11,
   };

   /* Final-edition 4:2:0 chroma DC scaling, the form conforming decoders use.
    * The scale is a signed multiply in 64-bit, not a left shift, so a negative f
    * or an out-of-range fuzzed coefficient stays defined. */
   int scale = 16 * norm_adjust[qp % 6][0];
   for (unsigned i = 0; i < 4; i++)
      dc[i] = (int16_t)(((int64_t)f[i] * scale * ((int64_t)1 << (qp / 6))) >> 5);
}

/* 4x4 luma block scan to raster grid position (sec 6.4.3); the residual stores
 * blocks in this 8x8-major scan order, the contract in raster order. */
static const uint8_t luma_blk_x[16] = { 0, 1, 0, 1, 2, 3, 2, 3,
                                        0, 1, 0, 1, 2, 3, 2, 3 };
static const uint8_t luma_blk_y[16] = { 0, 0, 1, 1, 0, 0, 1, 1,
                                        2, 2, 3, 3, 2, 2, 3, 3 };

void
vl_h264_dequant_fill_contract(const struct vl_h264_mb_residual *res,
                              struct vl_h264_mb_contract *mb)
{
   bool intra_16x16 = mb->mb_type != 0;

   int16_t luma_dc[16] = { 0 };
   if (intra_16x16)
      vl_h264_dequant_luma_dc(res->luma_dc, mb->qp_y, luma_dc);

   for (unsigned s = 0; s < 16; s++) {
      unsigned raster_blk = luma_blk_y[s] * 4 + luma_blk_x[s];
      int16_t *out = mb->coeff4x4[raster_blk];
      if (intra_16x16) {
         vl_h264_dequant_4x4_ac(res->luma4x4[s], mb->qp_y, out);
         out[0] = luma_dc[raster_blk];
      } else {
         vl_h264_dequant_4x4(res->luma4x4[s], mb->qp_y, out);
      }
   }

   /* Chroma: four 4x4 blocks per component, Cb then Cr, after the luma blocks
    * in the contract.  The DC of each block comes from the 2x2 Hadamard. */
   const int qp_chroma[2] = { mb->qp_cb, mb->qp_cr };
   for (unsigned comp = 0; comp < 2; comp++) {
      int16_t chroma_dc[4];
      vl_h264_dequant_chroma_dc(res->chroma_dc[comp], qp_chroma[comp], chroma_dc);
      for (unsigned blk = 0; blk < 4; blk++) {
         int16_t *out = mb->coeff4x4[VL_H264_LUMA_4X4_BLOCKS + comp * 4 + blk];
         vl_h264_dequant_4x4_ac(res->chroma_ac[comp][blk], qp_chroma[comp], out);
         out[0] = chroma_dc[blk];
      }
   }
}
