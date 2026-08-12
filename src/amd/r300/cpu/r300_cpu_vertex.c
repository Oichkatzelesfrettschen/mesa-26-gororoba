/*
 * SPDX-License-Identifier: MIT
 *
 * Portable CPU vertex executor: byte-copy baseline and the SSE2/SSE3
 * SIMD candidates.
 */

#include "r300_cpu_vertex.h"

#include "amd/r300/common/r300_vertex_format.h"

#include <errno.h>
#include <stdbool.h>
#include <string.h>

#if defined(__SSE2__)
#include <emmintrin.h>
#endif
#if defined(__SSE3__)
#include <pmmintrin.h>
#endif

/* The synthesized lane constants as explicit little-endian carrier
 * bytes, so every host writes the same encoding.
 */
static const uint8_t lane_zero_bytes[4] = { 0x00, 0x00, 0x00, 0x00 };
static const uint8_t lane_one_bytes[4] = { 0x00, 0x00, 0x80, 0x3f };

static int
validate(const struct r300_vertex_format_semantics **format_out,
         int format_id, const struct r300_cpu_vertex_stream *stream,
         uint32_t first_vertex, uint32_t vertex_count,
         const uint32_t *carrier, uint32_t carrier_dwords)
{
   const struct r300_vertex_format_semantics *format =
      r300_vertex_format_semantics((enum r300_vertex_format_id)format_id);
   if (format == NULL || stream == NULL || stream->data == NULL)
      return -EINVAL;
   /* A nonzero stride below the record size makes successive records
    * overlap.  Zero stride is the Vulkan constant-binding form: every
    * gathered vertex reads the same record.
    */
   if (stream->stride != 0 &&
       stream->stride < format->semantic_record_bytes)
      return -EINVAL;
   if (vertex_count != 0) {
      if (carrier == NULL)
         return -EINVAL;
      /* Every requested record lies inside the stream bound: the last
       * record's start plus its record bytes fits size_bytes.  The
       * comparison divides rather than multiplies -- the last index is
       * at most 2^33 and the stride is at least the record size, so no
       * intermediate can wrap into an in-range value.
       */
      if (stream->size_bytes < format->semantic_record_bytes)
         return -EINVAL;
      if (stream->stride != 0) {
         uint64_t last_index = (uint64_t)first_vertex + vertex_count - 1;
         if (last_index > (stream->size_bytes -
                           format->semantic_record_bytes) / stream->stride)
            return -EINVAL;
      }
   }
   if (vertex_count > carrier_dwords / 4)
      return -ENOSPC;
   *format_out = format;
   return 0;
}

int
r300_cpu_vertex_gather_baseline(
   int format_id, const struct r300_cpu_vertex_stream *stream,
   uint32_t first_vertex, uint32_t vertex_count, uint32_t *carrier,
   uint32_t carrier_dwords)
{
   const struct r300_vertex_format_semantics *format;
   int result = validate(&format, format_id, stream, first_vertex,
                         vertex_count, carrier, carrier_dwords);
   if (result != 0)
      return result;

   const uint8_t *record =
      stream->data + (uint64_t)first_vertex * stream->stride;
   uint8_t *out = (uint8_t *)carrier;
   for (uint32_t v = 0; v < vertex_count; v++) {
      for (unsigned lane = 0; lane < 4; lane++) {
         const uint8_t *src;
         switch (format->select[lane]) {
         case R300_VERTEX_SELECT_ZERO:
            src = lane_zero_bytes;
            break;
         case R300_VERTEX_SELECT_ONE:
            src = lane_one_bytes;
            break;
         default:
            /* X..W name physical components; the vocabulary orders the
             * selector values so the component index is the selector.
             */
            src = record + (unsigned)format->select[lane] * 4;
            break;
         }
         memcpy(out + lane * 4, src, 4);
      }
      record += stream->stride;
      out += 16;
   }
   return 0;
}

#if defined(__SSE2__)

/* SSE2 gathers for the F32 family: loads, shuffles, and stores alone,
 * so the byte contract holds.  The vector ONE constant is expressed as
 * a lane value, which equals the carrier's byte encoding because SSE2
 * implies a little-endian host.  The K8 primary target implements
 * SSE2/SSE3; this path is correctness-qualified by the oracle and the
 * target timing measurement decides which candidate stays.
 */
static int
gather_sse2(const struct r300_vertex_format_semantics *format,
            const struct r300_cpu_vertex_stream *stream,
            uint32_t first_vertex, uint32_t vertex_count, uint32_t *carrier)
{
   const uint8_t *record =
      stream->data + (uint64_t)first_vertex * stream->stride;
   const __m128i one_w = _mm_set_epi32(0x3f800000, 0, 0, 0);

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
 * patterns; a vocabulary row that deviates routes to the baseline, so
 * a table change degrades to the slower correct path.
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

#if defined(__SSE3__)

/* The SSE3 candidate: _mm_lddqu_si128 replaces _mm_loadu_si128 on the
 * F32_4 pass-through load, the one kernel wide enough for the
 * unaligned-load form to matter.  _mm_lddqu_si128 compiles to LDDQU;
 * the SSE2 intrinsic's opcode is the compiler's choice (MOVDQU or
 * MOVUPS), so the bench's SSE2-versus-SSE3 delta compares the two
 * compiled kernels as shipped, and the retained disassembly in the
 * evidence bundle names the exact instruction pair the measurement
 * timed.  SSE3 adds no form for the 8- and 4-byte partial loads, so
 * the remaining formats share the SSE2 kernels and the delta isolates
 * the load path alone.
 */
static int
gather_sse3(const struct r300_vertex_format_semantics *format,
            const struct r300_cpu_vertex_stream *stream,
            uint32_t first_vertex, uint32_t vertex_count, uint32_t *carrier)
{
   if (format->id == R300_VERTEX_FORMAT_F32_4) {
      const uint8_t *record =
         stream->data + (uint64_t)first_vertex * stream->stride;
      for (uint32_t v = 0; v < vertex_count; v++) {
         _mm_storeu_si128((__m128i *)(carrier + (uint64_t)v * 4),
                          _mm_lddqu_si128((const __m128i *)record));
         record += stream->stride;
      }
      return 0;
   }
   return gather_sse2(format, stream, first_vertex, vertex_count, carrier);
}

#endif /* __SSE3__ */

/* The named SIMD entry points refuse rather than substitute: a build
 * without the instruction set reports -ENOSYS, and a vocabulary row
 * outside the encoded identity patterns reports -EINVAL, so a bench
 * lane's label always names the kernels it timed.
 */
int
r300_cpu_vertex_gather_sse2(int format_id,
                            const struct r300_cpu_vertex_stream *stream,
                            uint32_t first_vertex, uint32_t vertex_count,
                            uint32_t *carrier, uint32_t carrier_dwords)
{
#if defined(__SSE2__)
   const struct r300_vertex_format_semantics *format;
   int result = validate(&format, format_id, stream, first_vertex,
                         vertex_count, carrier, carrier_dwords);
   if (result != 0)
      return result;
   if (!sse2_pattern_matches(format))
      return -EINVAL;
   return gather_sse2(format, stream, first_vertex, vertex_count, carrier);
#else
   (void)format_id;
   (void)stream;
   (void)first_vertex;
   (void)vertex_count;
   (void)carrier;
   (void)carrier_dwords;
   return -ENOSYS;
#endif
}

int
r300_cpu_vertex_gather_sse3(int format_id,
                            const struct r300_cpu_vertex_stream *stream,
                            uint32_t first_vertex, uint32_t vertex_count,
                            uint32_t *carrier, uint32_t carrier_dwords)
{
#if defined(__SSE3__)
   const struct r300_vertex_format_semantics *format;
   int result = validate(&format, format_id, stream, first_vertex,
                         vertex_count, carrier, carrier_dwords);
   if (result != 0)
      return result;
   if (!sse2_pattern_matches(format))
      return -EINVAL;
   return gather_sse3(format, stream, first_vertex, vertex_count, carrier);
#else
   (void)format_id;
   (void)stream;
   (void)first_vertex;
   (void)vertex_count;
   (void)carrier;
   (void)carrier_dwords;
   return -ENOSYS;
#endif
}

int
r300_cpu_vertex_gather(int format_id,
                       const struct r300_cpu_vertex_stream *stream,
                       uint32_t first_vertex, uint32_t vertex_count,
                       uint32_t *carrier, uint32_t carrier_dwords)
{
#if defined(__SSE2__)
   const struct r300_vertex_format_semantics *format;
   int result = validate(&format, format_id, stream, first_vertex,
                         vertex_count, carrier, carrier_dwords);
   if (result != 0)
      return result;
   if (sse2_pattern_matches(format))
      return gather_sse2(format, stream, first_vertex, vertex_count,
                         carrier);
#endif
   return r300_cpu_vertex_gather_baseline(format_id, stream, first_vertex,
                                          vertex_count, carrier,
                                          carrier_dwords);
}

const char *
r300_cpu_vertex_implementation(void)
{
#if defined(__SSE2__)
   return "sse2";
#else
   return "baseline";
#endif
}
