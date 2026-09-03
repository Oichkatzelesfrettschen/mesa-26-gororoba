/*
 * SPDX-License-Identifier: MIT
 *
 * Linear-span decomposition above the RB2D rectangle plan.  A transfer
 * command names a byte interval; the 2D engine fills rectangles on a
 * surface, so something has to turn one into the other.  This is that
 * conversion, and it owns no field width of its own: every bound it
 * enforces is read from r300_rb2d_fill.h, so a rule that moves there moves
 * here with it.
 *
 * The carrier is a 64-byte row of sixteen 32-bit words.  That pitch is the
 * smallest DST_PITCH_OFFSET can name (R300_RB2D_PITCH_GRANULARITY), which
 * makes the row the tightest grid a byte interval can be cut on, and the
 * ARGB8888 descriptor gives each word one pixel so a dword pattern lands
 * bit-exactly.
 *
 * An interval maps onto that carrier through the base the offset field can
 * name:
 *
 *    base      = byte_offset rounded down to R300_RB2D_OFFSET_GRANULARITY
 *    relative  = byte_offset - base            0 .. 1023
 *    y         = relative / 64                 0 .. 15
 *    x         = (relative % 64) / 4           0 .. 15
 *
 * and the interval becomes at most three rectangles: the remainder of the
 * first row, a block of whole rows, and the remainder of the last row.
 * Whole rows are one rectangle of height N rather than N rectangles,
 * because DST_WIDTH_HEIGHT carries both extents.
 *
 * One surface reaches R300_RB2D_MAX_COORD_REACH rows, so a segment covers
 * about 512 KiB and a longer interval becomes several segments in order.
 * A segment is never silently dropped: an interval that cannot be
 * represented refuses here, before any rectangle exists, rather than
 * producing one the fill plan would reject.
 */

#ifndef R300_RB2D_LINEAR_SPAN_H
#define R300_RB2D_LINEAR_SPAN_H

#include "r300_rb2d_fill.h"

#include <stdbool.h>
#include <stdint.h>

/* The carrier row: R300_RB2D_PITCH_GRANULARITY bytes of ARGB8888, so one
 * pixel per 32-bit word and sixteen words per row. */
#define R300_RB2D_SPAN_DWORDS_PER_ROW                                         \
   (R300_RB2D_PITCH_GRANULARITY / (uint32_t)sizeof(uint32_t))

/* Rectangles one segment can need: the first row's remainder, the whole
 * rows, and the last row's remainder. */
#define R300_RB2D_SPAN_MAX_RECTS_PER_SEGMENT 3u

/* A dword-aligned byte interval in one buffer, filled with one pattern. */
struct r300_rb2d_span {
   uint64_t byte_offset;
   uint64_t byte_size;
   /* DP_BRUSH_FRGD_CLR's payload, already in the order the destination
    * bytes take. */
   uint32_t value;
};

/* Every rule the decomposition holds a span to, in the order it tests
 * them, so a refusal names one fact. */
enum r300_rb2d_span_refusal {
   R300_RB2D_SPAN_OK = 0,
   R300_RB2D_SPAN_REFUSE_NULL,
   R300_RB2D_SPAN_REFUSE_SIZE_ZERO,
   R300_RB2D_SPAN_REFUSE_OFFSET_ALIGNMENT,
   R300_RB2D_SPAN_REFUSE_SIZE_ALIGNMENT,
   R300_RB2D_SPAN_REFUSE_RANGE_OVERFLOW,
   R300_RB2D_SPAN_REFUSE_OUTSIDE_BUFFER,
   R300_RB2D_SPAN_REFUSE_BASE_FIELD,
   R300_RB2D_SPAN_REFUSE_FOOTPRINT_OUTSIDE_BUFFER,
   R300_RB2D_SPAN_REFUSE_SEGMENT_STORAGE,
   R300_RB2D_SPAN_REFUSE_PLAN_REJECTED,
   R300_RB2D_SPAN_REFUSAL_COUNT,
};

const char *r300_rb2d_span_refusal_name(enum r300_rb2d_span_refusal r);

/* The segments a span decomposes into, written in order into plans[] and
 * their rectangles into rects[].
 *
 * rects[] holds R300_RB2D_SPAN_MAX_RECTS_PER_SEGMENT entries per segment,
 * so a caller sizes it at that multiple of max_segments; each plan points
 * into its own run.  Returns the number of segments written, zero on
 * refusal, and sets *refusal in both cases.
 *
 * Storage is exact rather than advisory: a span needing more segments than
 * max_segments refuses with R300_RB2D_SPAN_REFUSE_SEGMENT_STORAGE and
 * writes nothing, because a partly decomposed interval names a fill nobody
 * asked for.  r300_rb2d_linear_span_segments() sizes the storage first.
 *
 * Every plan written has passed r300_rb2d_fill_plan_check(); a plan that
 * would not refuses the whole decomposition with
 * R300_RB2D_SPAN_REFUSE_PLAN_REJECTED, so the two never disagree.
 */
uint32_t r300_rb2d_linear_span_plan(const struct r300_rb2d_span *span,
                                    uint64_t buffer_bytes,
                                    struct r300_rb2d_fill_plan *plans,
                                    struct r300_rb2d_fill_rect *rects,
                                    uint32_t max_segments,
                                    enum r300_rb2d_span_refusal *refusal);

/* How many segments a span needs, or zero with *refusal naming the rule it
 * violates.  It runs the same decomposition and counts, so a span this
 * accepts is one r300_rb2d_linear_span_plan() accepts at that size. */
uint32_t r300_rb2d_linear_span_segments(const struct r300_rb2d_span *span,
                                        uint64_t buffer_bytes,
                                        enum r300_rb2d_span_refusal *refusal);

/* Dwords the emitted stream costs for a decomposition, summing each
 * segment's own plan cost. */
uint64_t r300_rb2d_linear_span_dwords(const struct r300_rb2d_fill_plan *plans,
                                      uint32_t segment_count);

#endif /* R300_RB2D_LINEAR_SPAN_H */
