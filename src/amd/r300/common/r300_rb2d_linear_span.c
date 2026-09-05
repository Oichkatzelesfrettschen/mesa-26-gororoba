/*
 * SPDX-License-Identifier: MIT
 */

#include "r300_rb2d_linear_span.h"

#include "util/macros.h"

#include <string.h>

/* The span carries one 32-bit pattern, so its offset and size sit on the
 * dword grid whatever the carrier's pixel width. */
#define RB2D_SPAN_PATTERN_BYTES 4u

const char *
r300_rb2d_span_refusal_name(enum r300_rb2d_span_refusal r)
{
   static const char *const names[R300_RB2D_SPAN_REFUSAL_COUNT] = {
      "ok",
      "layout pointer is null",
      "layout format is outside the carrier table",
      "layout pitch is zero",
      "layout pitch is off the 64-byte grid",
      "layout pitch is not a whole number of pixels",
      "layout pitch is outside the pitch field",
      "layout row is wider than the scissor reaches",
      "span pointer is null",
      "span size is zero",
      "span offset is not dword aligned",
      "span size is not a dword multiple",
      "span pattern halves differ on a two-byte carrier",
      "span range overflows",
      "span reaches outside the 32-bit destination address space",
      "span reaches outside the buffer",
      "span base is outside the offset field",
      "segment footprint reaches outside the buffer",
      "segment covers no dword",
      "segment storage is too small",
      "segment plan rejected by the fill plan",
   };
   return (unsigned)r < R300_RB2D_SPAN_REFUSAL_COUNT ? names[r] : NULL;
}

enum r300_rb2d_span_refusal
r300_rb2d_span_layout_check(const struct r300_rb2d_span_layout *layout)
{
   if (layout == NULL)
      return R300_RB2D_SPAN_REFUSE_LAYOUT_NULL;
   const uint32_t cpp = r300_rb2d_format_bytes_per_pixel(layout->format);
   if (cpp == 0u)
      return R300_RB2D_SPAN_REFUSE_LAYOUT_FORMAT;
   if (layout->pitch_bytes == 0u)
      return R300_RB2D_SPAN_REFUSE_LAYOUT_PITCH_ZERO;
   if (layout->pitch_bytes % R300_RB2D_PITCH_GRANULARITY != 0u)
      return R300_RB2D_SPAN_REFUSE_LAYOUT_PITCH_GRID;
   /* The pattern is a dword, so a row holds a whole number of patterns
    * only when its byte count is a dword multiple; on the two-byte carrier
    * that is also what keeps the pattern phase across rows. */
   if (layout->pitch_bytes % RB2D_SPAN_PATTERN_BYTES != 0u)
      return R300_RB2D_SPAN_REFUSE_LAYOUT_PITCH_STRIDE;
   /* DST_PITCH_OFFSET counts the pitch in 64-byte units through a 10-bit
    * field, so the plan checker refuses a wider carrier; the layout
    * refuses it first, before a rectangle is cut against it. */
   if (layout->pitch_bytes / R300_RB2D_PITCH_GRANULARITY >
       R300_RB2D_MAX_PITCH_UNITS)
      return R300_RB2D_SPAN_REFUSE_LAYOUT_PITCH_FIELD;
   /* A whole-row rectangle spans the carrier, so the row itself has to
    * fit inside the scissor the emitter opens. */
   if (layout->pitch_bytes / cpp > R300_RB2D_SAFE_EXCLUSIVE_END)
      return R300_RB2D_SPAN_REFUSE_LAYOUT_ROW_BEYOND_SCISSOR;
   return R300_RB2D_SPAN_OK;
}

uint32_t
r300_rb2d_span_layout_pixels_per_row(const struct r300_rb2d_span_layout *l)
{
   if (r300_rb2d_span_layout_check(l) != R300_RB2D_SPAN_OK)
      return 0u;
   return l->pitch_bytes / r300_rb2d_format_bytes_per_pixel(l->format);
}

/* One segment's working state: the base its DST_PITCH_OFFSET names, the
 * rectangles it carries, and the row past its last, which is both the
 * surface height and what the next segment starts after. */
struct segment {
   uint64_t base;
   struct r300_rb2d_fill_rect rects[R300_RB2D_SPAN_MAX_RECTS_PER_SEGMENT];
   uint32_t rect_count;
   uint32_t rows_used;
   uint64_t dwords_consumed;
};

/* Cuts one segment out of the interval remaining at byte_offset.
 *
 * The base is the offset field's own grid, so the first row starts at
 * (x, y) inside the first kibibyte and every later row starts at column
 * zero.  Rows stop at R300_RB2D_SAFE_EXCLUSIVE_END because the emitter opens
 * the 2D scissor there; a rectangle past it would be clipped by the stream
 * that established it.
 */
static void
cut_segment(uint64_t byte_offset, uint64_t dwords_left, uint32_t pitch_bytes,
            uint32_t cpp, uint32_t value, struct segment *seg)
{
   const uint32_t per_row = pitch_bytes / cpp;

   memset(seg, 0, sizeof(*seg));
   seg->base = byte_offset & ~(uint64_t)(R300_RB2D_OFFSET_GRANULARITY - 1u);

   const uint32_t relative = (uint32_t)(byte_offset - seg->base);
   const uint32_t y0 = relative / pitch_bytes;
   const uint32_t x0 = (relative % pitch_bytes) / cpp;
   uint32_t row = y0;

   /* The far edge of every rectangle stays inside the scissor, so the rows
    * this segment may touch run from y0 up to the reach. */
   const uint32_t row_limit = R300_RB2D_SAFE_EXCLUSIVE_END;

   if (x0 != 0u && row < row_limit) {
      const uint64_t want = per_row - x0;
      const uint32_t width =
         (uint32_t)(dwords_left < want ? dwords_left : want);
      seg->rects[seg->rect_count++] = (struct r300_rb2d_fill_rect){
         .x = x0, .y = row, .width = width, .height = 1u, .value = value
      };
      dwords_left -= width;
      row++;
   }

   if (dwords_left >= per_row && row < row_limit) {
      uint64_t rows = dwords_left / per_row;
      const uint64_t room = row_limit - row;
      if (rows > room)
         rows = room;
      seg->rects[seg->rect_count++] = (struct r300_rb2d_fill_rect){
         .x = 0u, .y = row, .width = per_row, .height = (uint32_t)rows,
         .value = value
      };
      dwords_left -= rows * per_row;
      row += (uint32_t)rows;
   }

   if (dwords_left > 0u && dwords_left < per_row && row < row_limit) {
      seg->rects[seg->rect_count++] = (struct r300_rb2d_fill_rect){
         .x = 0u, .y = row, .width = (uint32_t)dwords_left, .height = 1u,
         .value = value
      };
      row++;
   }

   seg->rows_used = row;
   /* What the segment consumes is the area its rectangles cover, not the
    * rows they span: a partial first or last row carries fewer than a
    * whole one, and the caller advances the interval by this. */
   for (uint32_t i = 0; i < seg->rect_count; i++)
      seg->dwords_consumed +=
         (uint64_t)seg->rects[i].width * seg->rects[i].height;
}

/* Runs the decomposition, writing into caller storage when plans is
 * non-NULL and counting otherwise.  Both paths cut the same segments and
 * submit each to r300_rb2d_fill_plan_check, so a span one path accepts is
 * one the other accepts and the count and the write agree by
 * construction. */
static uint32_t
decompose(const struct r300_rb2d_span *span,
          const struct r300_rb2d_span_layout *layout, uint64_t buffer_bytes,
          struct r300_rb2d_fill_plan *plans, struct r300_rb2d_fill_rect *rects,
          uint32_t max_segments, enum r300_rb2d_span_refusal *refusal)
{
   enum r300_rb2d_span_refusal ignored;
   if (refusal == NULL)
      refusal = &ignored;

   *refusal = r300_rb2d_span_layout_check(layout);
   if (*refusal != R300_RB2D_SPAN_OK)
      return 0;

   if (span == NULL) {
      *refusal = R300_RB2D_SPAN_REFUSE_NULL;
      return 0;
   }
   if (span->byte_size == 0u) {
      *refusal = R300_RB2D_SPAN_REFUSE_SIZE_ZERO;
      return 0;
   }
   if (span->byte_offset % RB2D_SPAN_PATTERN_BYTES != 0u) {
      *refusal = R300_RB2D_SPAN_REFUSE_OFFSET_ALIGNMENT;
      return 0;
   }
   if (span->byte_size % RB2D_SPAN_PATTERN_BYTES != 0u) {
      *refusal = R300_RB2D_SPAN_REFUSE_SIZE_ALIGNMENT;
      return 0;
   }
   /* On a two-byte carrier the brush writes the pattern's low half per
    * pixel, so the bytes reproduce the dword pattern only when both halves
    * are equal. */
   const uint32_t cpp = r300_rb2d_format_bytes_per_pixel(layout->format);
   if (cpp == 2u && (span->value >> 16) != (span->value & 0xffffu)) {
      *refusal = R300_RB2D_SPAN_REFUSE_PATTERN_WIDTH;
      return 0;
   }
   if (span->byte_offset > UINT64_MAX - span->byte_size) {
      *refusal = R300_RB2D_SPAN_REFUSE_RANGE_OVERFLOW;
      return 0;
   }
   /* The whole interval, last byte included, is addressed through the
    * offset field's 32 bits.  Stated as a subtraction so the sum that
    * would overflow is never formed: [0, 2^32) is representable and an
    * interval ending exactly at 2^32 is the widest one that is. */
   if (span->byte_offset >= R300_RB2D_ADDRESS_SPACE_BYTES ||
       span->byte_size >
          R300_RB2D_ADDRESS_SPACE_BYTES - span->byte_offset) {
      *refusal = R300_RB2D_SPAN_REFUSE_ADDRESS_WIDTH;
      return 0;
   }
   if (span->byte_offset + span->byte_size > buffer_bytes) {
      *refusal = R300_RB2D_SPAN_REFUSE_OUTSIDE_BUFFER;
      return 0;
   }

   uint64_t offset = span->byte_offset;
   /* Pixels of the carrier, the unit rectangles are cut in. */
   uint64_t dwords_left = span->byte_size / cpp;
   const uint32_t brush = cpp == 2u ? (span->value & 0xffffu) : span->value;
   uint32_t written = 0;

   while (dwords_left > 0u) {
      struct segment seg;
      cut_segment(offset, dwords_left, layout->pitch_bytes, cpp, brush,
                  &seg);

      /* A segment that consumes nothing would loop.  The base grid makes
       * it unreachable -- y0 is under R300_RB2D_OFFSET_GRANULARITY divided
       * by the pitch, far below the scissor reach -- and the refusal keeps
       * that true in a build with assertions compiled out. */
      if (seg.dwords_consumed == 0u || seg.rect_count == 0u) {
         *refusal = R300_RB2D_SPAN_REFUSE_SEGMENT_EMPTY;
         return 0;
      }

      const uint64_t base_units = seg.base / R300_RB2D_OFFSET_GRANULARITY;
      if (base_units > UINT32_MAX || seg.base > UINT32_MAX) {
         *refusal = R300_RB2D_SPAN_REFUSE_BASE_FIELD;
         return 0;
      }
      /* The bytes the segment writes must be the buffer's.  The bound is
       * the kernel's own, r100_cs_track_2d_dst_check's end_byte: the last
       * rectangle row's start plus (x + width) * cpp, so a partial last
       * row is charged to its last written byte rather than to the whole
       * carrier row.  A dense carrier can therefore end inside a buffer
       * narrower than its pitch, which is what makes a wide virtual pitch
       * usable on a small buffer at all. */
      uint64_t footprint = 0u;
      for (uint32_t i = 0; i < seg.rect_count; i++) {
         const struct r300_rb2d_fill_rect *r = &seg.rects[i];
         const uint64_t end =
            (uint64_t)(r->y + r->height - 1u) * layout->pitch_bytes +
            ((uint64_t)r->x + r->width) * cpp;
         if (end > footprint)
            footprint = end;
      }
      if (seg.base + footprint > buffer_bytes) {
         *refusal = R300_RB2D_SPAN_REFUSE_FOOTPRINT_OUTSIDE_BUFFER;
         return 0;
      }

      /* One plan is built and checked per segment whichever mode this
       * runs in, against the segment's own rectangles.  The write mode
       * then copies those exact rectangles into caller storage and points
       * the stored plan at the copy, so what the caller receives is the
       * object the checker admitted.
       *
       * The layout and span rules above subsume every surface rule the
       * plan checks, so this rejection has no admitted input to fire on
       * today.  It is the guard that keeps the counting and the writing
       * pass judging one object when either set of rules moves, and
       * test_layout_subsumes_plan_check pins the subsumption it rests
       * on. */
      struct r300_rb2d_fill_plan plan = {
         .surface = {
            .base_offset_bytes = (uint32_t)seg.base,
            .pitch_bytes = layout->pitch_bytes,
            .width_pixels = layout->pitch_bytes / cpp,
            .height_pixels = seg.rows_used,
            .format = layout->format,
         },
         /* Every lane of the pattern reaches the destination; the brush
          * color is the whole dword. */
         .write_mask = 0xffffffffu,
         .rects = seg.rects,
         .rect_count = seg.rect_count,
      };
      if (r300_rb2d_fill_plan_check(&plan) != R300_RB2D_FILL_OK) {
         *refusal = R300_RB2D_SPAN_REFUSE_PLAN_REJECTED;
         return 0;
      }

      if (plans != NULL) {
         if (written >= max_segments) {
            *refusal = R300_RB2D_SPAN_REFUSE_SEGMENT_STORAGE;
            return 0;
         }
         struct r300_rb2d_fill_rect *run =
            &rects[(size_t)written * R300_RB2D_SPAN_MAX_RECTS_PER_SEGMENT];
         memcpy(run, seg.rects, seg.rect_count * sizeof(seg.rects[0]));
         plan.rects = run;
         plans[written] = plan;
      }

      written++;
      offset += seg.dwords_consumed * cpp;
      dwords_left -= seg.dwords_consumed;
   }

   return written;
}

uint32_t
r300_rb2d_linear_span_plan(const struct r300_rb2d_span *span,
                           const struct r300_rb2d_span_layout *layout,
                           uint64_t buffer_bytes,
                           struct r300_rb2d_fill_plan *plans,
                           struct r300_rb2d_fill_rect *rects,
                           uint32_t max_segments,
                           enum r300_rb2d_span_refusal *refusal)
{
   enum r300_rb2d_span_refusal ignored;
   if (refusal == NULL)
      refusal = &ignored;
   if (plans == NULL || rects == NULL || max_segments == 0u) {
      *refusal = R300_RB2D_SPAN_REFUSE_SEGMENT_STORAGE;
      return 0;
   }
   /* Size and validate the whole interval before writing: a span that
    * refuses at any segment refuses whole rather than leaving a prefix of
    * the interval planned and the rest silently absent.  The sizing pass
    * applies every rule the write pass applies, so the write cannot then
    * refuse partway. */
   const uint32_t needed =
      decompose(span, layout, buffer_bytes, NULL, NULL, 0u, refusal);
   if (needed == 0u)
      return 0;
   if (needed > max_segments) {
      *refusal = R300_RB2D_SPAN_REFUSE_SEGMENT_STORAGE;
      return 0;
   }
   return decompose(span, layout, buffer_bytes, plans, rects, max_segments,
                    refusal);
}

uint32_t
r300_rb2d_linear_span_segments(const struct r300_rb2d_span *span,
                               const struct r300_rb2d_span_layout *layout,
                               uint64_t buffer_bytes,
                               enum r300_rb2d_span_refusal *refusal)
{
   return decompose(span, layout, buffer_bytes, NULL, NULL, 0u, refusal);
}

bool
r300_rb2d_linear_span_dwords(const struct r300_rb2d_fill_plan *plans,
                             uint32_t segment_count, uint32_t *dwords_out)
{
   if (dwords_out == NULL)
      return false;
   *dwords_out = 0u;
   if (plans == NULL || segment_count == 0u)
      return false;

   uint64_t n = 0;
   for (uint32_t i = 0; i < segment_count; i++) {
      /* A rectangle count the fill plan does not admit has no emitted
       * cost, so the sum would name a stream nobody emits. */
      if (plans[i].rect_count == 0u ||
          plans[i].rect_count > R300_RB2D_FILL_MAX_RECTS)
         return false;
      n += R300_RB2D_FILL_DWORDS(plans[i].rect_count);
      if (n > UINT32_MAX)
         return false;
   }
   *dwords_out = (uint32_t)n;
   return true;
}
