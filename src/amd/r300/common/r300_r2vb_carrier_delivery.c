/*
 * SPDX-License-Identifier: MIT
 */

#include "r300_r2vb_carrier_delivery.h"

#include "amd/r300/common/r300_vertex_format.h"

#include <errno.h>
#include <string.h>

bool
r300_r2vb_f32_4_identity_admits(uint32_t bits)
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

int
r300_r2vb_f32_4_identity_deliver(
   int format_id, const struct r300_cpu_vertex_stream *stream,
   uint32_t first_vertex, uint32_t vertex_count, uint32_t *carrier,
   uint32_t carrier_dwords)
{
   if (format_id != R300_VERTEX_FORMAT_F32_4)
      return -EINVAL;
   const struct r300_vertex_format_semantics *format =
      r300_vertex_format_semantics((enum r300_vertex_format_id)format_id);
   if (format == NULL || stream == NULL || stream->data == NULL ||
       carrier == NULL)
      return -EINVAL;
   if ((uint64_t)vertex_count * 4 > carrier_dwords)
      return -ENOSPC;
   if (vertex_count == 0)
      return 0;

   /* Bounds prove in 64-bit arithmetic before any read: the last record
    * must end inside the caller's declared readable range.
    */
   const uint64_t last_record_end =
      ((uint64_t)first_vertex + vertex_count - 1) * stream->stride +
      format->semantic_record_bytes;
   if (last_record_end > stream->size_bytes)
      return -EINVAL;

   /* Domain check before any carrier write: delivery is all-or-nothing,
    * so a refusal leaves the carrier bytes untouched for the CPU route.
    */
   for (uint32_t v = 0; v < vertex_count; v++) {
      const uint8_t *record =
         stream->data + ((uint64_t)first_vertex + v) * stream->stride;
      for (unsigned lane = 0; lane < 4; lane++) {
         uint32_t bits;
         memcpy(&bits, record + lane * 4, 4);
         if (!r300_r2vb_f32_4_identity_admits(bits))
            return -EDOM;
      }
   }

   for (uint32_t v = 0; v < vertex_count; v++) {
      const uint8_t *record =
         stream->data + ((uint64_t)first_vertex + v) * stream->stride;
      memcpy(carrier + (uint64_t)v * 4, record, 16);
   }
   return 0;
}
