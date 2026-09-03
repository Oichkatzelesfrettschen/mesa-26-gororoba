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
 * The carrier is a row of ARGB8888 pixels whose pitch the caller names.
 * Two pitches matter.  R300_RB2D_SPAN_PITCH_DIRECT_WRITE is the 256-byte
 * row the retained direct-write control stream exercises, so a plan built
 * on it differs from that stream in the rectangle list alone.
 * R300_RB2D_PITCH_GRANULARITY is the 64-byte row, the smallest
 * DST_PITCH_OFFSET can name and therefore the tightest grid a byte
 * interval can be cut on; it reaches a smaller window per segment and no
 * stream has yet exercised it.
 *
 * An interval maps onto the carrier through the base the offset field can
 * name:
 *
 *    base      = byte_offset rounded down to R300_RB2D_OFFSET_GRANULARITY
 *    relative  = byte_offset - base            0 .. 1023
 *    y         = relative / pitch_bytes
 *    x         = (relative % pitch_bytes) / 4
 *
 * and the interval becomes at most three rectangles: the remainder of the
 * first row, a block of whole rows, and the remainder of the last row.
 * Whole rows are one rectangle of height N rather than N rectangles,
 * because DST_WIDTH_HEIGHT carries both extents.
 *
 * One surface reaches R300_RB2D_SAFE_EXCLUSIVE_END rows, so a segment covers
 * that many pitches -- 512 KiB at 64 bytes, 2 MiB at 256 -- and a longer
 * interval becomes several segments in order.  A segment is never silently
 * dropped: an interval that cannot be represented refuses here, before any
 * rectangle exists, rather than producing one the fill plan would reject.
 *
 * The destination offset rides DST_PITCH_OFFSET's 22-bit field of 1 KiB
 * units, which addresses 32 bits from the relocated base, so the interval
 * a span may name lives inside R300_RB2D_ADDRESS_SPACE_BYTES.
 */

#ifndef R300_RB2D_LINEAR_SPAN_H
#define R300_RB2D_LINEAR_SPAN_H

#include "r300_rb2d_fill.h"

#include <stdbool.h>
#include <stdint.h>

/* The byte interval DST_PITCH_OFFSET's offset field reaches from the
 * relocated destination base: 22 bits of 1 KiB units is 32 bits of
 * address, so [0, 2^32) is representable and nothing above it is. */
#define R300_RB2D_ADDRESS_SPACE_BYTES (UINT64_C(1) << 32)

/* The pitch of the retained direct-write control stream: 256 bytes is
 * pitch 4 in DST_PITCH_OFFSET's 64-byte units.  A plan on this carrier
 * differs from the witnessed stream in its rectangle list alone. */
#define R300_RB2D_SPAN_PITCH_DIRECT_WRITE 256u

/* Rectangles one segment can need: the first row's remainder, the whole
 * rows, and the last row's remainder. */
#define R300_RB2D_SPAN_MAX_RECTS_PER_SEGMENT 3u

/* The carrier a span is cut on.  Both fields reach the fill plan, so both
 * are held to the plan's own grids before any rectangle exists. */
struct r300_rb2d_span_layout {
   uint32_t pitch_bytes;
   enum r300_rb2d_format format;
};

/* A dword-aligned byte interval in one buffer, filled with one pattern. */
struct r300_rb2d_span {
   uint64_t byte_offset;
   uint64_t byte_size;
   /* DP_BRUSH_FRGD_CLR's payload, already in the order the destination
    * bytes take. */
   uint32_t value;
};

/* Every rule the decomposition holds a layout and a span to, in the order
 * it tests them, so a refusal names one fact. */
enum r300_rb2d_span_refusal {
   R300_RB2D_SPAN_OK = 0,
   R300_RB2D_SPAN_REFUSE_LAYOUT_NULL,
   R300_RB2D_SPAN_REFUSE_LAYOUT_FORMAT,
   R300_RB2D_SPAN_REFUSE_LAYOUT_PITCH_ZERO,
   R300_RB2D_SPAN_REFUSE_LAYOUT_PITCH_GRID,
   R300_RB2D_SPAN_REFUSE_LAYOUT_PITCH_STRIDE,
   R300_RB2D_SPAN_REFUSE_LAYOUT_PITCH_FIELD,
   R300_RB2D_SPAN_REFUSE_LAYOUT_ROW_BEYOND_SCISSOR,
   R300_RB2D_SPAN_REFUSE_NULL,
   R300_RB2D_SPAN_REFUSE_SIZE_ZERO,
   R300_RB2D_SPAN_REFUSE_OFFSET_ALIGNMENT,
   R300_RB2D_SPAN_REFUSE_SIZE_ALIGNMENT,
   R300_RB2D_SPAN_REFUSE_RANGE_OVERFLOW,
   R300_RB2D_SPAN_REFUSE_ADDRESS_WIDTH,
   R300_RB2D_SPAN_REFUSE_OUTSIDE_BUFFER,
   R300_RB2D_SPAN_REFUSE_BASE_FIELD,
   R300_RB2D_SPAN_REFUSE_FOOTPRINT_OUTSIDE_BUFFER,
   R300_RB2D_SPAN_REFUSE_SEGMENT_EMPTY,
   R300_RB2D_SPAN_REFUSE_SEGMENT_STORAGE,
   R300_RB2D_SPAN_REFUSE_PLAN_REJECTED,
   R300_RB2D_SPAN_REFUSAL_COUNT,
};

const char *r300_rb2d_span_refusal_name(enum r300_rb2d_span_refusal r);

/* Holds a carrier to the grids DST_PITCH_OFFSET packs, to the pattern
 * width a span carries, and to the scissor the emitter opens.  Returns
 * R300_RB2D_SPAN_OK, or the first rule the layout violates. */
enum r300_rb2d_span_refusal
r300_rb2d_span_layout_check(const struct r300_rb2d_span_layout *layout);

/* Pixels one carrier row holds, or zero for a layout that does not pass
 * r300_rb2d_span_layout_check. */
uint32_t
r300_rb2d_span_layout_pixels_per_row(const struct r300_rb2d_span_layout *l);

/* The segments a span decomposes into on a carrier, written in order into
 * plans[] and their rectangles into rects[].
 *
 * rects[] holds R300_RB2D_SPAN_MAX_RECTS_PER_SEGMENT entries per segment,
 * so a caller sizes it at that multiple of max_segments; each plan points
 * into its own run.  Returns the number of segments written, zero on
 * refusal, and sets *refusal in both cases.
 *
 * Storage is exact rather than advisory: a span needing more segments than
 * max_segments refuses with R300_RB2D_SPAN_REFUSE_SEGMENT_STORAGE and
 * writes nothing, because a partly decomposed interval names a fill nobody
 * asked for.  The whole interval is decomposed and validated before the
 * first rectangle reaches caller storage, so a refusal at any segment
 * leaves plans[] and rects[] as the caller left them.
 */
uint32_t r300_rb2d_linear_span_plan(const struct r300_rb2d_span *span,
                                    const struct r300_rb2d_span_layout *layout,
                                    uint64_t buffer_bytes,
                                    struct r300_rb2d_fill_plan *plans,
                                    struct r300_rb2d_fill_rect *rects,
                                    uint32_t max_segments,
                                    enum r300_rb2d_span_refusal *refusal);

/* How many segments a span needs on a carrier, or zero with *refusal
 * naming the rule it violates.  It runs the same decomposition and applies
 * the same r300_rb2d_fill_plan_check to every segment, so a span this
 * accepts is one r300_rb2d_linear_span_plan() accepts at that size. */
uint32_t
r300_rb2d_linear_span_segments(const struct r300_rb2d_span *span,
                               const struct r300_rb2d_span_layout *layout,
                               uint64_t buffer_bytes,
                               enum r300_rb2d_span_refusal *refusal);

/* Dwords the emitted stream costs for a decomposition, summing each
 * segment's own plan cost.  Returns false and writes nothing for absent
 * storage, an empty decomposition, a rectangle count the fill plan does
 * not admit, or a total the IB length field cannot carry. */
bool r300_rb2d_linear_span_dwords(const struct r300_rb2d_fill_plan *plans,
                                  uint32_t segment_count,
                                  uint32_t *dwords_out);

#endif /* R300_RB2D_LINEAR_SPAN_H */
