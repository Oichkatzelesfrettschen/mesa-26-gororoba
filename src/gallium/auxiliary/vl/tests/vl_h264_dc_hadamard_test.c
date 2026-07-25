/*
 * SPDX-License-Identifier: MIT
 */

/*
 * Unit test for the Intra_16x16 luma DC (sec 8.5.6) and 4:2:0 chroma DC
 * (sec 8.5.7) Hadamard dequant.  The expected values come from an independent
 * Python implementation of the same spec formulas, so a match confirms the
 * matrix product, the scale, and the qP-branch shifts are coded correctly; the
 * end-to-end YUV comparison against ffmpeg is the value check against an outside
 * decoder, since the per-macroblock oracle dump captures the Intra_16x16 AC only.
 * The vectors span both qP branches for each transform.
 */

#include <stdint.h>
#include <stdio.h>

#include "vl_h264_dequant.h"

#define CHECK(cond) do {                                                     \
   if (!(cond)) {                                                            \
      fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);        \
      return 1;                                                              \
   }                                                                         \
} while (0)

struct luma_vector {
   const char *name;
   int16_t scan[16];
   int qp;
   int16_t expect[16];
};

struct chroma_vector {
   const char *name;
   int16_t level[4];
   int qp;
   int16_t expect[4];
};

static const struct luma_vector luma_vectors[] = {
   { "ldc_qp16", {3,-2,1,0,1,0,0,0,0,0,0,0,0,0,0,0}, 16,
     {48,48,80,80,48,48,80,80,-16,-16,80,80,-16,-16,80,80} },
   { "ldc_qp27", {2,1,-1,0,0,1,0,0,0,0,0,0,0,0,0,0}, 27,
     {168,56,-56,56,168,56,-56,56,280,168,56,168,280,168,56,168} },
   { "ldc_qp6", {1,0,1,0,0,0,0,0,0,0,0,0,0,0,0,0}, 6,
     {10,10,10,10,10,10,10,10,0,0,0,0,0,0,0,0} },
   { "ldc_qp40", {2,-1,0,0,1,0,0,0,0,0,0,0,0,0,0,0}, 40,
     {512,512,512,512,512,512,512,512,0,0,1024,1024,0,0,1024,1024} },
};

static const struct chroma_vector chroma_vectors[] = {
   { "cdc_qp29", {3,-1,1,0}, 29, {432,720,144,432} },
   { "cdc_qp3", {2,1,0,-1}, 3, {14,14,28,0} },
   { "cdc_qp22", {1,0,0,1}, 22, {128,0,0,128} },
};

int
main(void)
{
   for (unsigned v = 0; v < sizeof(luma_vectors) / sizeof(luma_vectors[0]); v++) {
      const struct luma_vector *vec = &luma_vectors[v];
      int16_t dc[16];
      vl_h264_dequant_luma_dc(vec->scan, vec->qp, dc);
      for (unsigned i = 0; i < 16; i++) {
         if (dc[i] != vec->expect[i]) {
            fprintf(stderr, "FAIL luma %s[%u]: %d != %d\n", vec->name, i, dc[i],
                    vec->expect[i]);
            return 1;
         }
      }
   }

   for (unsigned v = 0; v < sizeof(chroma_vectors) / sizeof(chroma_vectors[0]);
        v++) {
      const struct chroma_vector *vec = &chroma_vectors[v];
      int16_t dc[4];
      vl_h264_dequant_chroma_dc(vec->level, vec->qp, dc);
      for (unsigned i = 0; i < 4; i++) {
         if (dc[i] != vec->expect[i]) {
            fprintf(stderr, "FAIL chroma %s[%u]: %d != %d\n", vec->name, i, dc[i],
                    vec->expect[i]);
            return 1;
         }
      }
   }

   printf("vl_h264_dc_hadamard: luma and chroma DC Hadamard match the spec "
          "reference across both qP branches PASS\n");
   return 0;
}
