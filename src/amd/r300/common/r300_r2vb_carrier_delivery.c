/*
 * SPDX-License-Identifier: MIT
 */

#include "r300_r2vb_carrier_delivery.h"

#include "amd/r300/common/r300_vertex_format.h"
#include "r300_us_source_read.h"

#include <errno.h>
#include <string.h>

static uint32_t
load_le32(const uint8_t *bytes)
{
   return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
          ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

bool
r300_r2vb_fp24_identity_admits(uint32_t bits)
{
   /* The R2VB identity path reads the value through the US source-operand
    * path.  The r300_us_source_read.h RS48x source-read model canonicalizes
    * negative zero and steps every negative nonzero value toward zero, so
    * no negative source encoding is byte-identical on this route.
    */
   if ((bits & 0x80000000u) != 0)
      return false;

   /* The shared FP24 store quantizer is authoritative for the lattice
    * boundary, saturation, and low-mantissa policy.  Equality rejects
    * infinities, NaNs, denormals, off-grid values, and values outside the
    * measured normal range without duplicating its constants here.
    */
   return r300_fp24_quantize_bits(bits) == bits;
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
         const uint32_t bits = load_le32(record + lane * 4);
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
