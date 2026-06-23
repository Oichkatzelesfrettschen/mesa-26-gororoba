/*
 * Copyright (c) 2026 Terascale Functionalists
 * SPDX-License-Identifier: MIT
 */

#include "vl_h264_dequant.h"

/*
 * Clean-room from ITU-T H.264 sec 8.5.6 (inverse scan) and 8.5.9 (dequant).
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

void
vl_h264_dequant_4x4(const int16_t scan[16], int qp, int16_t out[16])
{
   int16_t raster[16] = { 0 };
   for (unsigned k = 0; k < 16; k++)
      raster[zigzag_to_raster[k]] = scan[k];

   /* LevelScale = 16 * normAdjust (flat weightScale).  For qP >= 24 the shift is
    * a left shift, otherwise a rounded right shift (sec 8.5.9). */
   for (unsigned p = 0; p < 16; p++) {
      int scale = 16 * norm_adjust[qp % 6][class_4x4[p]];
      int level = raster[p];
      int coeff;
      if (qp >= 24) {
         coeff = (level * scale) << (qp / 6 - 4);
      } else {
         int shift = 4 - qp / 6;
         coeff = (level * scale + (1 << (shift - 1))) >> shift;
      }
      out[p] = (int16_t)coeff;
   }
}
