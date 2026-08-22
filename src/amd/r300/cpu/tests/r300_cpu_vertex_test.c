/*
 * SPDX-License-Identifier: MIT
 *
 * Qualification oracle for the CPU vertex executor: an independent
 * reference model, bit-pattern preservation, baseline/SIMD-path
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
         /* The expected model is byte-defined like the contract: a
          * physical lane copies its four record bytes and a synthesized
          * lane writes the little-endian encoding, whatever the host's
          * own byte order.
          */
         static const uint8_t zero_bytes[4] = { 0x00, 0x00, 0x00, 0x00 };
         static const uint8_t one_bytes[4] = { 0x00, 0x00, 0x80, 0x3f };
         uint8_t *lane_out = (uint8_t *)out + (uint64_t)v * 16 + lane * 4;
         if (format->select[lane] == R300_VERTEX_SELECT_ZERO)
            memcpy(lane_out, zero_bytes, 4);
         else if (format->select[lane] == R300_VERTEX_SELECT_ONE)
            memcpy(lane_out, one_bytes, 4);
         else
            memcpy(lane_out,
                   record + (unsigned)format->select[lane] * 4, 4);
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

   const struct r300_vertex_stream stream = {
      .data = data,
      .stride = stride,
      .size_bytes = sizeof(data),
   };

   uint32_t expected[CARRIER_DWORDS];
   reference_gather(format_id, data, stride, first_vertex, vertex_count,
                    expected);

   /* Both implementations reproduce the reference bit-exactly, and
    * neither writes past the gathered range: the carrier tail keeps
    * its canary.
    */
   uint32_t baseline_out[CARRIER_DWORDS + 1];
   uint32_t dispatch_out[CARRIER_DWORDS + 1];
   for (uint32_t i = 0; i < CARRIER_DWORDS + 1; i++)
      baseline_out[i] = dispatch_out[i] = CANARY;
   assert(r300_cpu_vertex_gather_baseline(format_id, &stream, first_vertex,
                                        vertex_count, baseline_out,
                                        CARRIER_DWORDS) == 0);
   assert(r300_cpu_vertex_gather(format_id, &stream, first_vertex,
                                 vertex_count, dispatch_out,
                                 CARRIER_DWORDS) == 0);
   assert(memcmp(baseline_out, expected, vertex_count * 16) == 0);
   assert(memcmp(dispatch_out, expected, vertex_count * 16) == 0);
   for (uint32_t i = vertex_count * 4; i < CARRIER_DWORDS + 1; i++) {
      assert(baseline_out[i] == CANARY);
      assert(dispatch_out[i] == CANARY);
   }

   /* The named SIMD candidates qualify against the same reference on
    * every build that carries their instruction set; a build without
    * one reports -ENOSYS and the identity claim stays scoped to the
    * builds that ran it.
    */
   int (*const simd[])(int, const struct r300_vertex_stream *,
                       uint32_t, uint32_t, uint32_t *, uint32_t) = {
      r300_cpu_vertex_gather_sse2,
      r300_cpu_vertex_gather_sse3,
   };
   for (unsigned t = 0; t < 2; t++) {
      uint32_t simd_out[CARRIER_DWORDS + 1];
      for (uint32_t i = 0; i < CARRIER_DWORDS + 1; i++)
         simd_out[i] = CANARY;
      int rc = simd[t](format_id, &stream, first_vertex, vertex_count,
                       simd_out, CARRIER_DWORDS);
      if (rc == -ENOSYS)
         continue;
      assert(rc == 0);
      assert(memcmp(simd_out, expected, vertex_count * 16) == 0);
      for (uint32_t i = vertex_count * 4; i < CARRIER_DWORDS + 1; i++)
         assert(simd_out[i] == CANARY);
   }
}

static void
check_zero_stride(void)
{
   uint8_t data[16];
   uint32_t expected[12];
   uint32_t output[12];
   fill_records(data, sizeof(data), 0x5eed0001u);
   const struct r300_vertex_stream stream = {
      .data = data,
      .stride = 0,
      .size_bytes = sizeof(data),
   };

   reference_gather(R300_VERTEX_FORMAT_F32_4, data, 0, 7, 3, expected);
   assert(r300_cpu_vertex_gather_baseline(
             R300_VERTEX_FORMAT_F32_4, &stream, 7, 3, output, 12) == 0);
   assert(memcmp(output, expected, sizeof(expected)) == 0);
   assert(r300_cpu_vertex_gather(R300_VERTEX_FORMAT_F32_4, &stream, 7, 3,
                                 output, 12) == 0);
   assert(memcmp(output, expected, sizeof(expected)) == 0);

   const struct r300_vertex_stream short_record = {
      .data = data,
      .stride = 0,
      .size_bytes = sizeof(data) - 1,
   };
   assert(r300_cpu_vertex_gather(R300_VERTEX_FORMAT_F32_4, &short_record,
                                 0, 1, output, 4) == -EINVAL);
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
   check_zero_stride();

   /* Refusals: unknown format, NULL stream data, overlapping stride,
    * and a carrier too small for the count.
    */
   uint8_t data[64];
   fill_records(data, sizeof(data), 1u);
   struct r300_vertex_stream stream = { .data = data, .stride = 16, .size_bytes = sizeof(data) };
   uint32_t carrier[8];
   assert(r300_cpu_vertex_gather(R300_VERTEX_FORMAT_INVALID, &stream, 0, 1,
                                 carrier, 8) == -EINVAL);
   assert(r300_cpu_vertex_gather(99, &stream, 0, 1, carrier, 8) == -EINVAL);
   struct r300_vertex_stream null_stream = { .data = NULL, .stride = 16, .size_bytes = 64 };
   assert(r300_cpu_vertex_gather(R300_VERTEX_FORMAT_F32_4, &null_stream, 0,
                                 1, carrier, 8) == -EINVAL);
   struct r300_vertex_stream narrow = { .data = data, .stride = 12, .size_bytes = sizeof(data) };
   assert(r300_cpu_vertex_gather(R300_VERTEX_FORMAT_F32_4, &narrow, 0, 1,
                                 carrier, 8) == -EINVAL);
   assert(r300_cpu_vertex_gather(R300_VERTEX_FORMAT_F32_4, &stream, 0, 3,
                                 carrier, 8) == -ENOSPC);
   /* The stream bound rejects a NULL carrier for a nonzero gather, an
    * out-of-range first_vertex, an incomplete final record, and a
    * first_vertex/count pair whose product would wrap 32-bit math.
    */
   assert(r300_cpu_vertex_gather(R300_VERTEX_FORMAT_F32_4, &stream, 0, 1,
                                 NULL, 8) == -EINVAL);
   assert(r300_cpu_vertex_gather(R300_VERTEX_FORMAT_F32_4, &stream, 4, 1,
                                 carrier, 8) == -EINVAL);
   struct r300_vertex_stream short_tail = { .data = data, .stride = 16,
                                                .size_bytes = 60 };
   assert(r300_cpu_vertex_gather(R300_VERTEX_FORMAT_F32_4, &short_tail, 3,
                                 1, carrier, 8) == -EINVAL);
   assert(r300_cpu_vertex_gather(R300_VERTEX_FORMAT_F32_4, &stream,
                                 0xffffffffu, 1, carrier, 8) == -EINVAL);

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
   struct r300_vertex_stream wide = { .data = data, .stride = 20, .size_bytes = sizeof(data) };
   assert(r300_cpu_vertex_gather(R300_VERTEX_FORMAT_F32_4, &wide, 0, 3,
                                 wrong_stride, 12) == 0);
   assert(memcmp(good, wrong_stride, sizeof(good)) != 0);

   /* The special encodings landed in the records and survived: vertex 0
    * of the packed F32_4 gather carries their bytes verbatim.
    */
   uint32_t first_vertex_lanes[4];
   assert(r300_cpu_vertex_gather(R300_VERTEX_FORMAT_F32_4, &stream, 0, 1,
                                 first_vertex_lanes, 4) == 0);
   for (unsigned lane = 0; lane < 4; lane++)
      assert(memcmp((uint8_t *)first_vertex_lanes + lane * 4,
                    data + lane * 4, 4) == 0);

   /* The synthesized ONE lane carries the little-endian carrier bytes
    * 00 00 80 3f on every host; the assertion reads bytes, not a host
    * dword, so a byte-order defect cannot cancel out of it.
    */
   uint32_t one_lane[4];
   struct r300_vertex_stream f1_stream = { .data = data, .stride = 4, .size_bytes = sizeof(data) };
   assert(r300_cpu_vertex_gather(R300_VERTEX_FORMAT_F32_1, &f1_stream, 0, 1,
                                 one_lane, 4) == 0);
   const uint8_t *w_bytes = (const uint8_t *)one_lane + 12;
   assert(w_bytes[0] == 0x00 && w_bytes[1] == 0x00 && w_bytes[2] == 0x80 &&
          w_bytes[3] == 0x3f);

   printf("r300_cpu_vertex: %s implementation matches the byte-defined reference "
          "bit-exactly\n",
          r300_cpu_vertex_implementation());
   return 0;
}
