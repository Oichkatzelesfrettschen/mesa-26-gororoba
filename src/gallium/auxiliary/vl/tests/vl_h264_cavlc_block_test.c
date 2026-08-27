/*
 * SPDX-License-Identifier: MIT
 */

/*
 * Round-trip test for the CAVLC residual block decoder.  The bitstreams are
 * produced by an independent Python CAVLC encoder (the forward direction of the
 * spec), so a decode that reproduces the original coefficients validates the
 * level state machine, the total_zeros/run_before parse, and the scan-order
 * combine against a separate implementation.  The vectors cover an empty block,
 * a single coefficient, two adjacent levels, a level with a zero run, a dense
 * block on the nC>=2 table, and a chroma DC 2x2 block.  For every vector, the
 * oracle requires block.total_coeff == vec->total_coeff and every
 * block.coeff[i] == vec->coeff[i].  One negative vector pins the capacity
 * contract: a max_num_coeff past the block's array capacity is rejected with
 * the caller's block left untouched.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "vl_h264_cavlc.h"

#define CHECK(cond) do {                                                     \
   if (!(cond)) {                                                            \
      fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);        \
      return 1;                                                              \
   }                                                                         \
} while (0)

struct vector {
   const char *name;
   int nc;
   unsigned max_num_coeff;
   unsigned total_coeff;
   uint8_t bytes[8];
   unsigned num_bytes;
   int16_t coeff[16];
};

static const struct vector vectors[] = {
   { "empty", 0, 16, 0, {192}, 1, {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0} },
   { "single_pos0", 0, 16, 1, {88}, 1, {1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0} },
   { "two_2_1", 0, 16, 2, {17,240}, 2, {2,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0} },
   { "run_1_gap_n2", 0, 16, 2, {7,106,64}, 3, {1,0,0,-2,0,0,0,0,0,0,0,0,0,0,0,0} },
   { "dense_nc2", 2, 16, 5, {13,217,117}, 3, {3,-1,1,0,2,0,0,-1,0,0,0,0,0,0,0,0} },
   { "chroma_dc", -1, 4, 2, {54}, 1, {1,-1,0,0,0,0,0,0,0,0,0,0,0,0,0,0} },
};

/* A max_num_coeff above the block's own array capacity is a caller error the
 * decoder rejects before it reads a bit or writes a byte.  The sentinel fill
 * survives the call, which is the observable form of "out is untouched": the
 * capacity check runs ahead of the memset that opens a real decode. */
static int
test_capacity_rejected(void)
{
   struct vl_h264_reader reader;
   struct vl_h264_cavlc_block block, before;
   const uint8_t bytes[] = {88};

   memset(&block, 0xa5, sizeof(block));
   before = block;

   CHECK(vl_h264_reader_init(&reader, bytes, sizeof(bytes)));
   CHECK(!vl_h264_cavlc_residual_block(&reader, VL_H264_CAVLC_MAX_COEFF + 1, 0,
                                       &block));
   CHECK(memcmp(&block, &before, sizeof(block)) == 0);
   vl_h264_reader_fini(&reader);
   return 0;
}

int
main(void)
{
   if (test_capacity_rejected() != 0)
      return 1;

   for (unsigned v = 0; v < sizeof(vectors) / sizeof(vectors[0]); v++) {
      const struct vector *vec = &vectors[v];
      struct vl_h264_reader reader;
      struct vl_h264_cavlc_block block;

      CHECK(vl_h264_reader_init(&reader, vec->bytes, vec->num_bytes));
      if (!vl_h264_cavlc_residual_block(&reader, vec->max_num_coeff, vec->nc,
                                        &block)) {
         fprintf(stderr, "FAIL %s: decode returned false\n", vec->name);
         return 1;
      }
      if (block.total_coeff != vec->total_coeff) {
         fprintf(stderr, "FAIL %s: total_coeff %u != %u\n", vec->name,
                 block.total_coeff, vec->total_coeff);
         return 1;
      }
      for (unsigned i = 0; i < vec->max_num_coeff; i++) {
         if (block.coeff[i] != vec->coeff[i]) {
            fprintf(stderr, "FAIL %s: coeff[%u] %d != %d\n", vec->name, i,
                    block.coeff[i], vec->coeff[i]);
            return 1;
         }
      }
      vl_h264_reader_fini(&reader);
   }

   printf("vl_h264_cavlc_block: residual blocks round-trip an independent "
          "encoder, an over-capacity max_num_coeff is rejected PASS\n");
   return 0;
}
