/*
 * SPDX-License-Identifier: MIT
 */

#include "r300_r2vb_carrier_delivery.h"

#include "amd/r300/common/r300_vertex_format.h"

#include <errno.h>
#include <string.h>

bool
r300_r2vb_fp24_identity_admits(uint32_t bits)
{
   const uint32_t exponent = (bits >> 23) & 0xffu;
   const uint32_t mantissa = bits & 0x7fffffu;

   if (exponent == 0)
      return mantissa == 0; /* +-0 admits; binary32 denormals refuse. */
   if (exponent == 0xffu)
      return mantissa == 0; /* +-Inf admits; NaN payloads truncate. */

   /* s1e7m16 holds 16 mantissa bits against binary32's 23, so the low 7
    * bits must be zero, and its normal exponents under bias 63 span
    * unbiased [-62, 63] -- biased binary32 [65, 190].
    */
   if ((mantissa & 0x7fu) != 0)
      return false;
   return exponent >= 65 && exponent <= 190;
}

/* The synthesized lane constants as explicit little-endian carrier
 * bytes, the same encoding r300_cpu_vertex_gather stores, so every
 * host writes identical carrier bytes: Z as +0.0 and W as 1.0, both
 * FP24 fixed points by construction.
 */
static const uint8_t lane_zero_bytes[4] = { 0x00, 0x00, 0x00, 0x00 };
static const uint8_t lane_one_bytes[4] = { 0x00, 0x00, 0x80, 0x3f };

int
r300_r2vb_identity_deliver(
   int format_id, const struct r300_cpu_vertex_stream *stream,
   uint32_t first_vertex, uint32_t vertex_count, uint32_t *carrier,
   uint32_t carrier_dwords)
{
   if (format_id != R300_VERTEX_FORMAT_F32_4 &&
       format_id != R300_VERTEX_FORMAT_F32_3 &&
       format_id != R300_VERTEX_FORMAT_F32_2)
      return -EINVAL;
   const struct r300_vertex_format_semantics *format =
      r300_vertex_format_semantics((enum r300_vertex_format_id)format_id);
   if (format == NULL || stream == NULL || stream->data == NULL)
      return -EINVAL;
   /* A stride below the record size makes successive records overlap,
    * which no API binding describes; the gather refuses it and the
    * delivery holds the same contract.
    */
   if (stream->stride < format->semantic_record_bytes)
      return -EINVAL;
   /* An empty delivery needs no carrier, the gather's own contract. */
   if (vertex_count == 0)
      return 0;
   if (carrier == NULL)
      return -EINVAL;

   /* Bounds prove before any read, in the gather's own form: the
    * comparison divides rather than multiplies -- the stride is at
    * least the record size, so no intermediate can wrap into an
    * in-range value, where a last_index * stride product wraps mod
    * 2^64 at large first_vertex and stride.
    */
   if (stream->size_bytes < format->semantic_record_bytes)
      return -EINVAL;
   const uint64_t last_index = (uint64_t)first_vertex + vertex_count - 1;
   if (last_index > (stream->size_bytes -
                     format->semantic_record_bytes) / stream->stride)
      return -EINVAL;
   if ((uint64_t)vertex_count * 4 > carrier_dwords)
      return -ENOSPC;

   const unsigned source_lanes = format->semantic_record_bytes / 4;

   /* Domain check over the source components before any carrier write:
    * delivery is all-or-nothing, so a refusal leaves the carrier bytes
    * untouched for the CPU route.  The synthesized lanes never scan --
    * their constants are FP24 fixed points by construction.
    */
   for (uint32_t v = 0; v < vertex_count; v++) {
      const uint8_t *record =
         stream->data + ((uint64_t)first_vertex + v) * stream->stride;
      for (unsigned lane = 0; lane < source_lanes; lane++) {
         uint32_t bits;
         memcpy(&bits, record + lane * 4, 4);
         if (!r300_r2vb_fp24_identity_admits(bits))
            return -EDOM;
      }
   }

   for (uint32_t v = 0; v < vertex_count; v++) {
      const uint8_t *record =
         stream->data + ((uint64_t)first_vertex + v) * stream->stride;
      uint32_t *out = carrier + (uint64_t)v * 4;
      memcpy(out, record, format->semantic_record_bytes);
      if (source_lanes < 3)
         memcpy(&out[2], lane_zero_bytes, 4);
      if (source_lanes < 4)
         memcpy(&out[3], lane_one_bytes, 4);
   }
   return 0;
}
