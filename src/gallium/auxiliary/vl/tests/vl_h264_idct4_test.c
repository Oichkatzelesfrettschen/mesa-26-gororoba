/*
 * SPDX-License-Identifier: MIT
 */

/*
 * Unit test for the 4x4 inverse integer transform: the expected residuals come
 * from an independent Python implementation of the spec transform (sec
 * 8.5.12.2), so a match confirms the row/column ordering, the half-weight
 * shifts, and the (h + 32) >> 6 normalization.  The vectors include a DC-only
 * block, a low-frequency pattern, and random blocks spanning the dequantized
 * coefficient range.
 */

#include <stdint.h>
#include <stdio.h>

#include "vl_h264_intra_reconstruct.h"

struct vector {
   int16_t coeff[16];
   int16_t residual[16];
};

static const struct vector vectors[] = {
   { {224,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
     {4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4} },
   { {0,288,0,0,288,0,0,0,0,0,0,0,0,0,0,0},
     {9,7,2,0,7,5,0,-2,2,0,-4,-7,0,-2,-7,-9} },
   { {579,-511,175,882,-167,-47,-1461,352,14,1779,-1457,-210,-1557,-124,1291,-1570},
     {-18,33,22,-58,14,-24,99,38,0,-80,-63,85,47,25,38,-13} },
   { {-383,1650,-972,1267,1567,1318,-1898,1840,-731,-372,-58,-1673,-1629,-1493,819,-1317},
     {-10,36,31,-80,107,37,133,-104,-15,-58,-34,-22,-23,-6,-67,-21} },
   { {729,108,1112,-1002,-1963,585,-291,-1246,-981,-1544,813,-153,1242,749,-511,525},
     {-28,-17,-76,21,5,22,-59,-1,96,74,51,25,17,-38,-6,94} },
   { {-1295,1335,1084,-1424,63,145,1975,1144,1835,1068,1779,235,605,974,-1886,931},
     {126,-30,-64,24,-49,-65,-118,2,-88,45,-14,-104,37,32,-83,26} },
   { {329,-542,30,-1446,1555,-1831,-899,-1095,-739,709,-547,378,654,-1200,482,-1062},
     {-52,66,17,61,-12,37,11,38,-4,16,-33,81,11,-26,-50,-79} },
};

int
main(void)
{
   for (unsigned v = 0; v < sizeof(vectors) / sizeof(vectors[0]); v++) {
      int16_t residual[16];
      vl_h264_idct4(vectors[v].coeff, residual);
      for (unsigned i = 0; i < 16; i++) {
         if (residual[i] != vectors[v].residual[i]) {
            fprintf(stderr, "FAIL vector %u [%u]: %d != %d\n", v, i, residual[i],
                    vectors[v].residual[i]);
            return 1;
         }
      }
   }
   printf("vl_h264_idct4: 4x4 inverse transform matches the spec reference PASS\n");
   return 0;
}
