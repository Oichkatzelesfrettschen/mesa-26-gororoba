/*
 * SPDX-License-Identifier: MIT
 *
 * K8-safe CPU vertex executor: scalar reference and SSE2 specialization.
 */

#include "r300_cpu_vertex.h"

#include "amd/r300/common/r300_vertex_format.h"

#include <errno.h>
#include <stdbool.h>
#include <string.h>

#if defined(__SSE2__)
#include <emmintrin.h>
#endif

/* The synthesized lane constants, by bit pattern rather than float
 * literal, so the carrier encoding is host-independent.
 */
#define R300_CPU_VERTEX_ZERO_BITS 0x00000000u
#define R300_CPU_VERTEX_ONE_BITS 0x3f800000u

static int
validate(const struct r300_vertex_format_semantics **format_out,
         int format_id, const struct r300_cpu_vertex_stream *stream,
         uint32_t vertex_count, uint32_t carrier_dwords)
{
   const struct r300_vertex_format_semantics *format =
      r300_vertex_format_semantics((enum r300_vertex_format_id)format_id);
   if (format == NULL || stream == NULL || stream->data == NULL)
      return -EINVAL;
   /* A stride below the record size makes successive records overlap,
    * which no API binding describes.
    */
   if (stream->stride < format->semantic_record_bytes)
      return -EINVAL;
   if (vertex_count > carrier_dwords / 4)
      return -ENOSPC;
   *format_out = format;
   return 0;
}

int
r300_cpu_vertex_gather_scalar(
   int format_id, const struct r300_cpu_vertex_stream *stream,
   uint32_t first_vertex, uint32_t vertex_count, uint32_t *carrier,
   uint32_t carrier_dwords)
{
   const struct r300_vertex_format_semantics *format;
   int result = validate(&format, format_id, stream, vertex_count,
                         carrier_dwords);
   if (result != 0)
      return result;

   const uint8_t *record =
      stream->data + (uint64_t)first_vertex * stream->stride;
   for (uint32_t v = 0; v < vertex_count; v++) {
      for (unsigned lane = 0; lane < 4; lane++) {
         uint32_t bits;
         switch (format->select[lane]) {
         case R300_VERTEX_SELECT_ZERO:
            bits = R300_CPU_VERTEX_ZERO_BITS;
            break;
         case R300_VERTEX_SELECT_ONE:
            bits = R300_CPU_VERTEX_ONE_BITS;
            break;
         default: {
            /* X..W name physical components; the vocabulary orders the
             * selector values so the component index is the selector.
             */
            unsigned component = (unsigned)format->select[lane];
            const uint8_t *src = record + component * 4;
            bits = (uint32_t)src[0] | (uint32_t)src[1] << 8 |
                   (uint32_t)src[2] << 16 | (uint32_t)src[3] << 24;
            break;
         }
         }
         carrier[(uint64_t)v * 4 + lane] = bits;
      }
      record += stream->stride;
   }
   return 0;
}

#if defined(__SSE2__)

/* SSE2 gathers for the F32 family.  Every path is loads, shuffles, and
 * stores; the lanes never enter arithmetic, so the bit-exactness
 * contract holds.  K8 implements SSE2/SSE3, so this specialization is
 * the measured x86-64-v1 path.
 */
static int
gather_sse2(const struct r300_vertex_format_semantics *format,
            const struct r300_cpu_vertex_stream *stream,
            uint32_t first_vertex, uint32_t vertex_count, uint32_t *carrier)
{
   const uint8_t *record =
      stream->data + (uint64_t)first_vertex * stream->stride;
   const __m128i one_w = _mm_set_epi32((int)R300_CPU_VERTEX_ONE_BITS, 0, 0, 0);

   switch (format->id) {
   case R300_VERTEX_FORMAT_F32_4:
      for (uint32_t v = 0; v < vertex_count; v++) {
         _mm_storeu_si128((__m128i *)(carrier + (uint64_t)v * 4),
                          _mm_loadu_si128((const __m128i *)record));
         record += stream->stride;
      }
      return 0;
   case R300_VERTEX_FORMAT_F32_3:
      /* XYZ1: 8-byte and 4-byte loads stay inside the 12-byte record. */
      for (uint32_t v = 0; v < vertex_count; v++) {
         __m128i xy = _mm_loadl_epi64((const __m128i *)record);
         int z_bits;
         memcpy(&z_bits, record + 8, sizeof(z_bits));
         __m128i xyz = _mm_unpacklo_epi64(xy, _mm_cvtsi32_si128(z_bits));
         _mm_storeu_si128((__m128i *)(carrier + (uint64_t)v * 4),
                          _mm_or_si128(xyz, one_w));
         record += stream->stride;
      }
      return 0;
   case R300_VERTEX_FORMAT_F32_2:
      /* XY01: the 8-byte load covers the record. */
      for (uint32_t v = 0; v < vertex_count; v++) {
         __m128i xy = _mm_loadl_epi64((const __m128i *)record);
         _mm_storeu_si128((__m128i *)(carrier + (uint64_t)v * 4),
                          _mm_or_si128(xy, one_w));
         record += stream->stride;
      }
      return 0;
   case R300_VERTEX_FORMAT_F32_1:
      /* X001: the 4-byte load covers the record. */
      for (uint32_t v = 0; v < vertex_count; v++) {
         int x_bits;
         memcpy(&x_bits, record, sizeof(x_bits));
         _mm_storeu_si128((__m128i *)(carrier + (uint64_t)v * 4),
                          _mm_or_si128(_mm_cvtsi32_si128(x_bits), one_w));
         record += stream->stride;
      }
      return 0;
   default:
      return -EINVAL;
   }
}

/* The specializations above encode the F32 family's identity selector
 * patterns; a vocabulary row that deviates routes to the scalar
 * reference, so a table change degrades to the slower correct path.
 */
static bool
sse2_pattern_matches(const struct r300_vertex_format_semantics *format)
{
   static const enum r300_vertex_component_select expected
      [R300_VERTEX_FORMAT_COUNT][4] = {
      [R300_VERTEX_FORMAT_F32_1] = { R300_VERTEX_SELECT_X,
                                     R300_VERTEX_SELECT_ZERO,
                                     R300_VERTEX_SELECT_ZERO,
                                     R300_VERTEX_SELECT_ONE },
      [R300_VERTEX_FORMAT_F32_2] = { R300_VERTEX_SELECT_X,
                                     R300_VERTEX_SELECT_Y,
                                     R300_VERTEX_SELECT_ZERO,
                                     R300_VERTEX_SELECT_ONE },
      [R300_VERTEX_FORMAT_F32_3] = { R300_VERTEX_SELECT_X,
                                     R300_VERTEX_SELECT_Y,
                                     R300_VERTEX_SELECT_Z,
                                     R300_VERTEX_SELECT_ONE },
      [R300_VERTEX_FORMAT_F32_4] = { R300_VERTEX_SELECT_X,
                                     R300_VERTEX_SELECT_Y,
                                     R300_VERTEX_SELECT_Z,
                                     R300_VERTEX_SELECT_W },
   };
   for (unsigned lane = 0; lane < 4; lane++) {
      if (format->select[lane] != expected[format->id][lane])
         return false;
   }
   return true;
}

#endif /* __SSE2__ */

int
r300_cpu_vertex_gather(int format_id,
                       const struct r300_cpu_vertex_stream *stream,
                       uint32_t first_vertex, uint32_t vertex_count,
                       uint32_t *carrier, uint32_t carrier_dwords)
{
#if defined(__SSE2__)
   const struct r300_vertex_format_semantics *format;
   int result = validate(&format, format_id, stream, vertex_count,
                         carrier_dwords);
   if (result != 0)
      return result;
   if (sse2_pattern_matches(format))
      return gather_sse2(format, stream, first_vertex, vertex_count,
                         carrier);
#endif
   return r300_cpu_vertex_gather_scalar(format_id, stream, first_vertex,
                                        vertex_count, carrier,
                                        carrier_dwords);
}

const char *
r300_cpu_vertex_implementation(void)
{
#if defined(__SSE2__)
   return "sse2";
#else
   return "scalar";
#endif
}
