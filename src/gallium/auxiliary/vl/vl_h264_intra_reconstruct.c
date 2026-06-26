/*
 * Copyright (c) 2026 Terascale Functionalists
 * SPDX-License-Identifier: MIT
 */

#include "vl_h264_intra_reconstruct.h"

/*
 * Clean-room from ITU-T H.264 sec 8.3 (intra prediction) and 8.5.12.2 (the 4x4
 * inverse transform).  No third-party decoder source was consulted.
 */

/* One-dimensional inverse core transform (sec 8.5.12.2): the H.264 inverse basis
 * with the two half-weight entries as arithmetic right shifts. */
static void
idct4_1d(const int z[4], int out[4])
{
   int a = z[0] + z[2];
   int b = z[0] - z[2];
   int c = (z[1] >> 1) - z[3];
   int d = z[1] + (z[3] >> 1);
   out[0] = a + d;
   out[1] = b + c;
   out[2] = b - c;
   out[3] = a - d;
}

void
vl_h264_idct4(const int16_t coeff[16], int16_t residual[16])
{
   int rows[16], cols[16];

   for (int r = 0; r < 4; r++) {
      int z[4] = { coeff[r * 4], coeff[r * 4 + 1], coeff[r * 4 + 2],
                   coeff[r * 4 + 3] };
      int o[4];
      idct4_1d(z, o);
      for (int c = 0; c < 4; c++)
         rows[r * 4 + c] = o[c];
   }

   for (int c = 0; c < 4; c++) {
      int z[4] = { rows[c], rows[4 + c], rows[8 + c], rows[12 + c] };
      int o[4];
      idct4_1d(z, o);
      for (int r = 0; r < 4; r++)
         cols[r * 4 + c] = o[r];
   }

   for (int i = 0; i < 16; i++)
      residual[i] = (int16_t)((cols[i] + 32) >> 6);
}
