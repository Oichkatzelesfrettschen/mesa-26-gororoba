/*
 * SPDX-License-Identifier: MIT
 *
 * Identity control for R2VB F32_4 carrier delivery: the delivery model,
 * the portable CPU baseline, and the dispatched CPU gather produce one
 * byte-identical carrier on the admitted FP24 fixed-point domain, and
 * the delivery refuses every input class the silicon producer would not
 * reproduce byte-exact.
 */

/* The asserts carry this test's verdicts, so they stay live under NDEBUG. */
#undef NDEBUG

#include "r300_r2vb_carrier_delivery.h"

#include "amd/r300/common/r300_vertex_format.h"

#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define CANARY 0xdeadbeefu

/* FP24 fixed points: +-0, +-Inf, exact small constants, the window
 * coordinates the viewport transform produces for the reference
 * triangle, and the s1e7m16 range corners -- low 7 mantissa bits zero,
 * unbiased exponent -62 and 63.
 */
static const uint32_t admitted_bits[] = {
   0x00000000u, /* +0 */
   0x80000000u, /* -0 */
   0x3f800000u, /* 1.0 */
   0xbf400000u, /* -0.75 */
   0x3f400000u, /* 0.75 */
   0x41000000u, /* 8.0 */
   0x42600000u, /* 56.0 */
   0x7f800000u, /* +Inf */
   0xff800000u, /* -Inf */
   0x20800000u, /* 2^-62, the smallest FP24 normal */
   0x5f7fff80u, /* largest binary32 with the FP24 mantissa grid at 2^63 */
};

/* Encodings the FP24 round trip does not reproduce: NaN payloads
 * truncate, binary32 denormals underflow the FP24 normal range, a set
 * low mantissa bit falls off the 16-bit grid, and exponents outside
 * [-62, 63] leave the s1e7m16 field.
 */
static const uint32_t refused_bits[] = {
   0x7fa00001u, /* signaling NaN with payload */
   0x7fc00123u, /* quiet NaN with payload */
   0x00000001u, /* smallest binary32 denormal */
   0x3dcccccdu, /* 0.1: low mantissa bits set */
   0x3f800001u, /* 1.0 + one binary32 ulp */
   0x20000000u, /* 2^-63, below the FP24 normal range */
   0x60000000u, /* 2^65, above the FP24 normal range */
};

static void
test_admission_predicate(void)
{
   for (unsigned i = 0; i < sizeof(admitted_bits) / 4; i++)
      assert(r300_r2vb_f32_4_identity_admits(admitted_bits[i]));
   for (unsigned i = 0; i < sizeof(refused_bits) / 4; i++)
      assert(!r300_r2vb_f32_4_identity_admits(refused_bits[i]));
}

/* Records over the admitted vocabulary with a stride gap, so the
 * three-way identity covers non-contiguous streams and a nonzero
 * first_vertex.
 */
static void
test_three_way_identity(void)
{
   enum { VERTS = 8, STRIDE = 24, FIRST = 2 };
   uint8_t data[(FIRST + VERTS) * STRIDE];
   memset(data, 0x5a, sizeof(data));
   for (uint32_t v = 0; v < FIRST + VERTS; v++) {
      for (unsigned lane = 0; lane < 4; lane++) {
         const uint32_t bits =
            admitted_bits[(v * 4 + lane) % (sizeof(admitted_bits) / 4)];
         memcpy(data + v * STRIDE + lane * 4, &bits, 4);
      }
   }
   const struct r300_cpu_vertex_stream stream = {
      .data = data,
      .stride = STRIDE,
      .size_bytes = sizeof(data),
   };

   uint32_t delivered[VERTS * 4 + 1];
   uint32_t baseline[VERTS * 4 + 1];
   uint32_t dispatched[VERTS * 4 + 1];
   for (unsigned i = 0; i < VERTS * 4 + 1; i++)
      delivered[i] = baseline[i] = dispatched[i] = CANARY;

   assert(r300_r2vb_f32_4_identity_deliver(R300_VERTEX_FORMAT_F32_4,
                                           &stream, FIRST, VERTS, delivered,
                                           VERTS * 4) == 0);
   assert(r300_cpu_vertex_gather_baseline(R300_VERTEX_FORMAT_F32_4,
                                          &stream, FIRST, VERTS, baseline,
                                          VERTS * 4) == 0);
   assert(r300_cpu_vertex_gather(R300_VERTEX_FORMAT_F32_4, &stream, FIRST,
                                 VERTS, dispatched, VERTS * 4) == 0);

   assert(memcmp(delivered, baseline, VERTS * 16) == 0);
   assert(memcmp(delivered, dispatched, VERTS * 16) == 0);
   assert(delivered[VERTS * 4] == CANARY && baseline[VERTS * 4] == CANARY &&
          dispatched[VERTS * 4] == CANARY);
}

/* Refusals: format, bounds, capacity, and -- the R2VB-specific class --
 * the FP24 domain.  A domain refusal writes nothing, so the CPU route
 * receives an untouched carrier; the CPU gather itself still carries
 * the same records, which pins the deliberate domain narrowing between
 * the two routes.
 */
static void
test_refusals(void)
{
   uint8_t data[4 * 16];
   memset(data, 0, sizeof(data));
   const struct r300_cpu_vertex_stream stream = {
      .data = data,
      .stride = 16,
      .size_bytes = sizeof(data),
   };
   uint32_t carrier[16];

   assert(r300_r2vb_f32_4_identity_deliver(R300_VERTEX_FORMAT_F32_3,
                                           &stream, 0, 3, carrier,
                                           16) == -EINVAL);
   assert(r300_r2vb_f32_4_identity_deliver(R300_VERTEX_FORMAT_F32_4,
                                           &stream, 0, 5, carrier,
                                           16) == -ENOSPC);
   assert(r300_r2vb_f32_4_identity_deliver(R300_VERTEX_FORMAT_F32_4,
                                           &stream, 2, 3, carrier,
                                           16) == -EINVAL);

   for (unsigned i = 0; i < sizeof(refused_bits) / 4; i++) {
      memcpy(data + 16 + 8, &refused_bits[i], 4);
      for (unsigned j = 0; j < 16; j++)
         carrier[j] = CANARY;
      assert(r300_r2vb_f32_4_identity_deliver(R300_VERTEX_FORMAT_F32_4,
                                              &stream, 0, 4, carrier,
                                              16) == -EDOM);
      for (unsigned j = 0; j < 16; j++)
         assert(carrier[j] == CANARY);
      uint32_t gathered[16];
      assert(r300_cpu_vertex_gather(R300_VERTEX_FORMAT_F32_4, &stream, 0,
                                    4, gathered, 16) == 0);
      uint32_t bits;
      memcpy(&bits, data + 16 + 8, 4);
      assert(gathered[4 + 2] == bits);
      memset(data + 16 + 8, 0, 4);
   }
}

int
main(void)
{
   test_admission_predicate();
   test_three_way_identity();
   test_refusals();
   printf("r300_r2vb_carrier_delivery_test: all checks passed\n");
   return 0;
}
