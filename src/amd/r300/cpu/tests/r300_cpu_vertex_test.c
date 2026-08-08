/*
 * SPDX-License-Identifier: MIT
 *
 * Qualification oracle for the CPU vertex executor: an independent
 * reference model, bit-pattern preservation, scalar/specialization
 * identity, refusal legs, and known-bad calibration.
 */

/* The asserts carry this test's verdicts, so they stay live under NDEBUG. */
#undef NDEBUG

#include "r300_cpu_vertex.h"

#include "amd/r300/common/r300_vertex_format.h"

#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define MAX_VERTICES 64u
#define CARRIER_DWORDS (MAX_VERTICES * 4u)
#define CANARY 0xdeadbeefu

/* Deterministic record bytes with planted special float encodings, so
 * the identity checks below cover signaling and quiet NaN payloads, a
 * denormal, negative zero, and infinity rather than only round
 * numbers.  A gather is loads and stores alone, so every pattern must
 * survive bit-exact.
 */
static const uint32_t special_bits[] = {
   0x7fa00001u, /* signaling NaN with payload */
   0x7fc00123u, /* quiet NaN with payload */
   0x00000001u, /* smallest denormal */
   0x80000000u, /* negative zero */
   0x7f800000u, /* infinity */
};

static void
fill_records(uint8_t *data, uint32_t bytes, uint32_t seed)
{
   uint32_t state = seed;
   for (uint32_t i = 0; i < bytes; i++) {
      state = state * 1664525u + 1013904223u;
      data[i] = (uint8_t)(state >> 24);
   }
   for (uint32_t i = 0; i * 4 + 3 < bytes && i < 5; i++) {
      uint32_t bits = special_bits[i];
      memcpy(data + i * 4, &bits, 4);
   }
}

/* The test's own model of the PSC lane semantics, written against the
 * vocabulary table alone, so an executor defect cannot hide by
 * agreeing with itself.
 */
static void
reference_gather(int format_id, const uint8_t *base, uint32_t stride,
                 uint32_t first_vertex, uint32_t vertex_count,
                 uint32_t *out)
{
   const struct r300_vertex_format_semantics *format =
      r300_vertex_format_semantics((enum r300_vertex_format_id)format_id);
   assert(format != NULL);
   for (uint32_t v = 0; v < vertex_count; v++) {
      const uint8_t *record =
         base + (uint64_t)(first_vertex + v) * stride;
      for (unsigned lane = 0; lane < 4; lane++) {
         uint32_t bits;
         if (format->select[lane] == R300_VERTEX_SELECT_ZERO)
            bits = 0x00000000u;
         else if (format->select[lane] == R300_VERTEX_SELECT_ONE)
            bits = 0x3f800000u;
         else
            memcpy(&bits, record + (unsigned)format->select[lane] * 4, 4);
         out[(uint64_t)v * 4 + lane] = bits;
      }
   }
}

static void
check_format(int format_id, uint32_t stride_extra, uint32_t first_vertex,
             uint32_t vertex_count)
{
   const struct r300_vertex_format_semantics *format =
      r300_vertex_format_semantics((enum r300_vertex_format_id)format_id);
   uint32_t stride = format->semantic_record_bytes + stride_extra;
   uint8_t data[MAX_VERTICES * 32];
   assert((uint64_t)(first_vertex + vertex_count) * stride <= sizeof(data));
   fill_records(data, sizeof(data), 0x5eed0000u + (uint32_t)format_id);

   const struct r300_cpu_vertex_stream stream = {
      .data = data,
      .stride = stride,
   };

   uint32_t expected[CARRIER_DWORDS];
   reference_gather(format_id, data, stride, first_vertex, vertex_count,
                    expected);

   /* Both implementations reproduce the reference bit-exactly, and
    * neither writes past the gathered range: the carrier tail keeps
    * its canary.
    */
   uint32_t scalar_out[CARRIER_DWORDS + 1];
   uint32_t dispatch_out[CARRIER_DWORDS + 1];
   for (uint32_t i = 0; i < CARRIER_DWORDS + 1; i++)
      scalar_out[i] = dispatch_out[i] = CANARY;
   assert(r300_cpu_vertex_gather_scalar(format_id, &stream, first_vertex,
                                        vertex_count, scalar_out,
                                        CARRIER_DWORDS) == 0);
   assert(r300_cpu_vertex_gather(format_id, &stream, first_vertex,
                                 vertex_count, dispatch_out,
                                 CARRIER_DWORDS) == 0);
   assert(memcmp(scalar_out, expected, vertex_count * 16) == 0);
   assert(memcmp(dispatch_out, expected, vertex_count * 16) == 0);
   for (uint32_t i = vertex_count * 4; i < CARRIER_DWORDS + 1; i++) {
      assert(scalar_out[i] == CANARY);
      assert(dispatch_out[i] == CANARY);
   }
}

int
main(void)
{
   /* Every F32 format, packed and padded strides, zero and nonzero
    * first_vertex, including the fixed cell's exact shape (F32_4,
    * three vertices, packed).
    */
   const int formats[] = {
      R300_VERTEX_FORMAT_F32_1,
      R300_VERTEX_FORMAT_F32_2,
      R300_VERTEX_FORMAT_F32_3,
      R300_VERTEX_FORMAT_F32_4,
   };
   for (unsigned f = 0; f < 4; f++) {
      check_format(formats[f], 0, 0, 3);
      check_format(formats[f], 0, 0, MAX_VERTICES);
      check_format(formats[f], 12, 0, 17);
      check_format(formats[f], 4, 5, 29);
   }

   /* Zero vertices writes nothing. */
   check_format(R300_VERTEX_FORMAT_F32_4, 0, 0, 0);

   /* Refusals: unknown format, NULL stream data, overlapping stride,
    * and a carrier too small for the count.
    */
   uint8_t data[64];
   fill_records(data, sizeof(data), 1u);
   struct r300_cpu_vertex_stream stream = { .data = data, .stride = 16 };
   uint32_t carrier[8];
   assert(r300_cpu_vertex_gather(R300_VERTEX_FORMAT_INVALID, &stream, 0, 1,
                                 carrier, 8) == -EINVAL);
   assert(r300_cpu_vertex_gather(99, &stream, 0, 1, carrier, 8) == -EINVAL);
   struct r300_cpu_vertex_stream null_stream = { .data = NULL, .stride = 16 };
   assert(r300_cpu_vertex_gather(R300_VERTEX_FORMAT_F32_4, &null_stream, 0,
                                 1, carrier, 8) == -EINVAL);
   struct r300_cpu_vertex_stream narrow = { .data = data, .stride = 12 };
   assert(r300_cpu_vertex_gather(R300_VERTEX_FORMAT_F32_4, &narrow, 0, 1,
                                 carrier, 8) == -EINVAL);
   assert(r300_cpu_vertex_gather(R300_VERTEX_FORMAT_F32_4, &stream, 0, 3,
                                 carrier, 8) == -ENOSPC);

   /* Known-bad calibration: the comparison detects a single corrupted
    * lane, and a wrong stride produces a different carrier, so the
    * identity checks above are non-vacuous.
    */
   uint32_t good[12];
   uint32_t bad[12];
   reference_gather(R300_VERTEX_FORMAT_F32_4, data, 16, 0, 3, good);
   memcpy(bad, good, sizeof(bad));
   bad[5] ^= 1u;
   assert(memcmp(good, bad, sizeof(good)) != 0);
   uint32_t wrong_stride[12];
   struct r300_cpu_vertex_stream wide = { .data = data, .stride = 20 };
   assert(r300_cpu_vertex_gather(R300_VERTEX_FORMAT_F32_4, &wide, 0, 3,
                                 wrong_stride, 12) == 0);
   assert(memcmp(good, wrong_stride, sizeof(good)) != 0);

   /* The special encodings landed in the records and survived: vertex 0
    * of the packed F32_4 gather carries them verbatim.
    */
   uint32_t first_vertex_lanes[4];
   reference_gather(R300_VERTEX_FORMAT_F32_4, data, 16, 0, 1,
                    first_vertex_lanes);
   for (unsigned lane = 0; lane < 4; lane++)
      assert(first_vertex_lanes[lane] == special_bits[lane]);

   printf("r300_cpu_vertex: %s implementation matches the reference "
          "bit-exactly\n",
          r300_cpu_vertex_implementation());
   return 0;
}
