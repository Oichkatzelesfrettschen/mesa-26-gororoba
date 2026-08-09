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
      assert(r300_r2vb_fp24_identity_admits(admitted_bits[i]));
   for (unsigned i = 0; i < sizeof(refused_bits) / 4; i++)
      assert(!r300_r2vb_fp24_identity_admits(refused_bits[i]));
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

   assert(r300_r2vb_identity_deliver(R300_VERTEX_FORMAT_F32_4,
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

   assert(r300_r2vb_identity_deliver(R300_VERTEX_FORMAT_F32_1,
                                           &stream, 0, 3, carrier,
                                           16) == -EINVAL);
   /* A stride below the record size describes overlapping records, the
    * binding the gather refuses; delivery holds the same contract.
    */
   const struct r300_cpu_vertex_stream overlapping = {
      .data = data,
      .stride = 12,
      .size_bytes = sizeof(data),
   };
   assert(r300_r2vb_identity_deliver(R300_VERTEX_FORMAT_F32_4,
                                           &overlapping, 0, 3, carrier,
                                           16) == -EINVAL);
   assert(r300_r2vb_identity_deliver(R300_VERTEX_FORMAT_F32_4,
                                           &stream, 0, 4, carrier,
                                           8) == -ENOSPC);
   assert(r300_r2vb_identity_deliver(R300_VERTEX_FORMAT_F32_4,
                                           &stream, 2, 3, carrier,
                                           16) == -EINVAL);
   /* The bounds proof holds where a last_index * stride product wraps
    * mod 2^64: the gather's divide form refuses this input, and the
    * delivery holds the same contract instead of reading past the
    * stream.
    */
   const struct r300_cpu_vertex_stream wrap = {
      .data = data,
      .stride = 0xffffffffu,
      .size_bytes = 16,
   };
   assert(r300_r2vb_identity_deliver(R300_VERTEX_FORMAT_F32_4, &wrap,
                                     0xffffffffu, 3, carrier,
                                     16) == -EINVAL);
   assert(r300_cpu_vertex_gather(R300_VERTEX_FORMAT_F32_4, &wrap,
                                 0xffffffffu, 3, carrier, 16) != 0);

   for (unsigned i = 0; i < sizeof(refused_bits) / 4; i++) {
      memcpy(data + 16 + 8, &refused_bits[i], 4);
      for (unsigned j = 0; j < 16; j++)
         carrier[j] = CANARY;
      assert(r300_r2vb_identity_deliver(R300_VERTEX_FORMAT_F32_4,
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

/* F32_3 and F32_2 delivery synthesizes the lanes past the source
 * record exactly as the gather does -- Z as +0.0, W as 1.0 -- so the
 * three-way byte identity extends to the synthesized shapes.  The
 * domain scan covers the source lanes alone: bytes between the record
 * end and the stride are binding padding the route never reads, so a
 * refused bit pattern there delivers clean, while the same pattern
 * inside a source lane refuses with the carrier untouched.
 */
static void
test_synthesized_identity(void)
{
   static const int formats[2] = { R300_VERTEX_FORMAT_F32_3,
                                   R300_VERTEX_FORMAT_F32_2 };
   static const uint32_t record_bytes[2] = { 12, 8 };
   const uint32_t stride = 20;
   const uint32_t verts = 4;
   const uint32_t first = 1;

   for (unsigned f = 0; f < 2; f++) {
      const unsigned lanes = record_bytes[f] / 4;
      uint8_t data[(1 + 4) * 20];
      memset(data, 0, sizeof(data));
      for (uint32_t v = 0; v < first + verts; v++) {
         for (unsigned lane = 0; lane < lanes; lane++) {
            const uint32_t bits = admitted_bits[(v * lanes + lane) %
                                                (sizeof(admitted_bits) / 4)];
            memcpy(data + v * stride + lane * 4, &bits, 4);
         }
         /* Padding bytes carry a refused pattern (0.1's bits): the
          * route reads record_bytes per record, so the pattern never
          * scans.
          */
         const uint32_t off_grid = 0x3dcccccdu;
         for (uint32_t pad = record_bytes[f]; pad + 4 <= stride; pad += 4)
            memcpy(data + v * stride + pad, &off_grid, 4);
      }
      /* The record before first_vertex carries a refused pattern in its
       * source lanes: the delivery scans the requested records alone,
       * so it never reads it.
       */
      const uint32_t leading_off_grid = 0x3dcccccdu;
      memcpy(data, &leading_off_grid, 4);

      const struct r300_cpu_vertex_stream stream = {
         .data = data,
         .stride = stride,
         .size_bytes = sizeof(data),
      };

      uint32_t delivered[4 * 4 + 1], baseline[4 * 4 + 1],
         dispatched[4 * 4 + 1];
      delivered[verts * 4] = CANARY;
      baseline[verts * 4] = CANARY;
      dispatched[verts * 4] = CANARY;
      assert(r300_r2vb_identity_deliver(formats[f], &stream, first, verts,
                                        delivered, verts * 4) == 0);
      assert(r300_cpu_vertex_gather_baseline(formats[f], &stream, first,
                                             verts, baseline,
                                             verts * 4) == 0);
      assert(r300_cpu_vertex_gather(formats[f], &stream, first, verts,
                                    dispatched, verts * 4) == 0);
      assert(memcmp(delivered, baseline, verts * 16) == 0);
      assert(memcmp(delivered, dispatched, verts * 16) == 0);
      assert(delivered[verts * 4] == CANARY &&
             baseline[verts * 4] == CANARY &&
             dispatched[verts * 4] == CANARY);
      for (uint32_t v = 0; v < verts; v++) {
         if (lanes < 3)
            assert(delivered[v * 4 + 2] == 0);
         assert(delivered[v * 4 + 3] == 0x3f800000u);
      }

      /* The same refused pattern inside a source lane refuses -EDOM
       * with the carrier untouched, while the gather still carries it.
       */
      uint8_t bad[sizeof(data)];
      memcpy(bad, data, sizeof(data));
      const uint32_t off_grid = 0x3dcccccdu;
      memcpy(bad + (first + 1) * stride + (lanes - 1) * 4, &off_grid, 4);
      const struct r300_cpu_vertex_stream bad_stream = {
         .data = bad,
         .stride = stride,
         .size_bytes = sizeof(bad),
      };
      for (unsigned j = 0; j < verts * 4 + 1; j++)
         delivered[j] = CANARY;
      assert(r300_r2vb_identity_deliver(formats[f], &bad_stream, first,
                                        verts, delivered,
                                        verts * 4) == -EDOM);
      for (unsigned j = 0; j < verts * 4 + 1; j++)
         assert(delivered[j] == CANARY);
      assert(r300_cpu_vertex_gather(formats[f], &bad_stream, first, verts,
                                    dispatched, verts * 4) == 0);
      assert(dispatched[1 * 4 + (lanes - 1)] == off_grid);
   }
}

int
main(void)
{
   test_admission_predicate();
   test_three_way_identity();
   test_synthesized_identity();
   test_refusals();
   printf("r300_r2vb_carrier_delivery_test: all checks passed\n");
   return 0;
}
