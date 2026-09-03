/*
 * SPDX-License-Identifier: MIT
 */

#include "r300_rb2d_linear_span.h"

#include "util/macros.h"

#include <assert.h>
#include <string.h>

const char *
r300_rb2d_span_refusal_name(enum r300_rb2d_span_refusal r)
{
   static const char *const names[R300_RB2D_SPAN_REFUSAL_COUNT] = {
      "ok",
      "span pointer is null",
      "span size is zero",
      "span offset is not dword aligned",
      "span size is not a dword multiple",
      "span range overflows",
      "span reaches outside the buffer",
      "span base is outside the offset field",
      "segment footprint reaches outside the buffer",
      "segment storage is too small",
      "segment plan rejected by the fill plan",
   };
   return (unsigned)r < R300_RB2D_SPAN_REFUSAL_COUNT ? names[r] : NULL;
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
 * zero.  Rows stop at R300_RB2D_MAX_COORD_REACH because the emitter opens
 * the 2D scissor there; a rectangle past it would be clipped by the stream
 * that established it.
 */
static void
cut_segment(uint64_t byte_offset, uint64_t dwords_left, struct segment *seg)
{
   const uint32_t per_row = R300_RB2D_SPAN_DWORDS_PER_ROW;

   memset(seg, 0, sizeof(*seg));
   seg->base = byte_offset & ~(uint64_t)(R300_RB2D_OFFSET_GRANULARITY - 1u);

   const uint32_t relative = (uint32_t)(byte_offset - seg->base);
   const uint32_t y0 = relative / R300_RB2D_PITCH_GRANULARITY;
   const uint32_t x0 = (relative % R300_RB2D_PITCH_GRANULARITY) / 4u;
   uint32_t row = y0;

   /* The far edge of every rectangle stays inside the scissor, so the rows
    * this segment may touch run from y0 up to the reach. */
   const uint32_t row_limit = R300_RB2D_MAX_COORD_REACH;

   if (x0 != 0u && row < row_limit) {
      const uint64_t want = per_row - x0;
      const uint32_t width =
         (uint32_t)(dwords_left < want ? dwords_left : want);
      seg->rects[seg->rect_count++] = (struct r300_rb2d_fill_rect){
         .x = x0, .y = row, .width = width, .height = 1u
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
         .x = 0u, .y = row, .width = per_row, .height = (uint32_t)rows
      };
      dwords_left -= rows * per_row;
      row += (uint32_t)rows;
   }

   if (dwords_left > 0u && dwords_left < per_row && row < row_limit) {
      seg->rects[seg->rect_count++] = (struct r300_rb2d_fill_rect){
         .x = 0u, .y = row, .width = (uint32_t)dwords_left, .height = 1u
      };
      dwords_left -= dwords_left;
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
 * non-NULL and counting otherwise.  Both paths walk the same loop, so the
 * count and the write agree by construction. */
static uint32_t
decompose(const struct r300_rb2d_span *span, uint64_t buffer_bytes,
          struct r300_rb2d_fill_plan *plans, struct r300_rb2d_fill_rect *rects,
          uint32_t max_segments, enum r300_rb2d_span_refusal *refusal)
{
   enum r300_rb2d_span_refusal ignored;
   if (refusal == NULL)
      refusal = &ignored;
   *refusal = R300_RB2D_SPAN_OK;

   if (span == NULL) {
      *refusal = R300_RB2D_SPAN_REFUSE_NULL;
      return 0;
   }
   if (span->byte_size == 0u) {
      *refusal = R300_RB2D_SPAN_REFUSE_SIZE_ZERO;
      return 0;
   }
   if (span->byte_offset % 4u != 0u) {
      *refusal = R300_RB2D_SPAN_REFUSE_OFFSET_ALIGNMENT;
      return 0;
   }
   if (span->byte_size % 4u != 0u) {
      *refusal = R300_RB2D_SPAN_REFUSE_SIZE_ALIGNMENT;
      return 0;
   }
   if (span->byte_offset > UINT64_MAX - span->byte_size) {
      *refusal = R300_RB2D_SPAN_REFUSE_RANGE_OVERFLOW;
      return 0;
   }
   if (span->byte_offset + span->byte_size > buffer_bytes) {
      *refusal = R300_RB2D_SPAN_REFUSE_OUTSIDE_BUFFER;
      return 0;
   }

   uint64_t offset = span->byte_offset;
   uint64_t dwords_left = span->byte_size / 4u;
   uint32_t written = 0;

   while (dwords_left > 0u) {
      struct segment seg;
      cut_segment(offset, dwords_left, &seg);

      /* A segment that consumes nothing would loop, and the only way to
       * reach it is a base whose first row already sits at the scissor
       * reach, which the base grid makes impossible: y0 is under sixteen.
       * The check states the invariant the loop depends on. */
      assert(seg.dwords_consumed > 0u && seg.rect_count > 0u);

      const uint64_t base_units = seg.base / R300_RB2D_OFFSET_GRANULARITY;
      if (base_units > UINT32_MAX || seg.base > UINT32_MAX) {
         *refusal = R300_RB2D_SPAN_REFUSE_BASE_FIELD;
         return 0;
      }
      /* The surface the plan declares is rows_used rows of one carrier
       * pitch, and those bytes must be the buffer's. */
      const uint64_t footprint =
         (uint64_t)seg.rows_used * R300_RB2D_PITCH_GRANULARITY;
      if (seg.base + footprint > buffer_bytes) {
         *refusal = R300_RB2D_SPAN_REFUSE_FOOTPRINT_OUTSIDE_BUFFER;
         return 0;
      }

      if (plans != NULL) {
         if (written >= max_segments) {
            *refusal = R300_RB2D_SPAN_REFUSE_SEGMENT_STORAGE;
            return 0;
         }
         struct r300_rb2d_fill_rect *run =
            &rects[(size_t)written * R300_RB2D_SPAN_MAX_RECTS_PER_SEGMENT];
         for (uint32_t i = 0; i < seg.rect_count; i++) {
            run[i] = seg.rects[i];
            run[i].value = span->value;
         }
         plans[written] = (struct r300_rb2d_fill_plan){
            .surface = {
               .base_offset_bytes = (uint32_t)seg.base,
               .pitch_bytes = R300_RB2D_PITCH_GRANULARITY,
               .width_pixels = R300_RB2D_SPAN_DWORDS_PER_ROW,
               .height_pixels = seg.rows_used,
               .format = R300_RB2D_FORMAT_ARGB8888,
            },
            /* Every lane of the pattern reaches the destination; the
             * brush colour is the whole dword. */
            .write_mask = 0xffffffffu,
            .rects = run,
            .rect_count = seg.rect_count,
         };
         /* The plan is the authority on its own admissibility, so a
          * segment it would reject ends the decomposition here rather
          * than reaching an emitter. */
         if (r300_rb2d_fill_plan_check(&plans[written]) !=
             R300_RB2D_FILL_OK) {
            *refusal = R300_RB2D_SPAN_REFUSE_PLAN_REJECTED;
            return 0;
         }
      }

      written++;
      offset += seg.dwords_consumed * 4u;
      dwords_left -= seg.dwords_consumed;
   }

   return written;
}

uint32_t
r300_rb2d_linear_span_plan(const struct r300_rb2d_span *span,
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
   /* Size before writing: a span needing more segments than the caller
    * sized for refuses whole rather than leaving a prefix of the interval
    * filled and the rest silently absent. */
   const uint32_t needed =
      decompose(span, buffer_bytes, NULL, NULL, 0u, refusal);
   if (needed == 0u)
      return 0;
   if (needed > max_segments) {
      *refusal = R300_RB2D_SPAN_REFUSE_SEGMENT_STORAGE;
      return 0;
   }
   return decompose(span, buffer_bytes, plans, rects, max_segments, refusal);
}

uint32_t
r300_rb2d_linear_span_segments(const struct r300_rb2d_span *span,
                               uint64_t buffer_bytes,
                               enum r300_rb2d_span_refusal *refusal)
{
   return decompose(span, buffer_bytes, NULL, NULL, 0u, refusal);
}

uint64_t
r300_rb2d_linear_span_dwords(const struct r300_rb2d_fill_plan *plans,
                             uint32_t segment_count)
{
   uint64_t n = 0;
   if (plans == NULL)
      return 0;
   for (uint32_t i = 0; i < segment_count; i++)
      n += R300_RB2D_FILL_DWORDS(plans[i].rect_count);
   return n;
}
