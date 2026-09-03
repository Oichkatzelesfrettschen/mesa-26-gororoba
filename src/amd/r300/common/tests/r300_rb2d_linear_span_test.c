/*
 * SPDX-License-Identifier: MIT
 *
 * The decomposition's primary obligation is agreement with the plan it
 * feeds: every rectangle it produces is one r300_rb2d_fill_plan_check
 * admits, every dword of the requested interval is covered exactly once,
 * and nothing outside the interval is touched.  The coverage arms below
 * prove that by replaying each decomposition onto a shadow of the buffer,
 * so a rectangle placed one row or one column wrong is a miscount rather
 * than a reader's judgement.
 */

#include "r300_rb2d_linear_span.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))
#define MAX_SEGMENTS 64u
#define RECT_STORAGE (MAX_SEGMENTS * R300_RB2D_SPAN_MAX_RECTS_PER_SEGMENT)

static struct r300_rb2d_fill_plan plans[MAX_SEGMENTS];
static struct r300_rb2d_fill_rect rects[RECT_STORAGE];

/* Replays a decomposition onto a per-dword touch count of the buffer and
 * holds it to the interval: every dword inside touched once, every dword
 * outside untouched, and every plan admitted by the plan checker. */
static void
covers_exactly(const struct r300_rb2d_span *span, uint64_t buffer_bytes,
               uint32_t expect_segments)
{
   enum r300_rb2d_span_refusal refusal = R300_RB2D_SPAN_REFUSAL_COUNT;
   const uint32_t sized =
      r300_rb2d_linear_span_segments(span, buffer_bytes, &refusal);
   assert(refusal == R300_RB2D_SPAN_OK);
   assert(sized == expect_segments);

   const uint32_t n = r300_rb2d_linear_span_plan(
      span, buffer_bytes, plans, rects, MAX_SEGMENTS, &refusal);
   assert(refusal == R300_RB2D_SPAN_OK);
   assert(n == sized);

   const size_t dwords = (size_t)(buffer_bytes / 4u);
   uint8_t *touched = calloc(dwords, 1);
   assert(touched != NULL);

   for (uint32_t s = 0; s < n; s++) {
      const struct r300_rb2d_fill_plan *p = &plans[s];
      /* The plan is the authority on its own admissibility. */
      assert(r300_rb2d_fill_plan_check(p) == R300_RB2D_FILL_OK);
      assert(p->surface.pitch_bytes == R300_RB2D_PITCH_GRANULARITY);
      assert(p->surface.base_offset_bytes % R300_RB2D_OFFSET_GRANULARITY == 0);

      for (uint32_t r = 0; r < p->rect_count; r++) {
         const struct r300_rb2d_fill_rect *rc = &p->rects[r];
         assert(rc->value == span->value);
         /* Every rectangle stays inside the scissor the emitter opens. */
         assert(rc->x + rc->width <= R300_RB2D_MAX_COORD_REACH);
         assert(rc->y + rc->height <= R300_RB2D_MAX_COORD_REACH);
         for (uint32_t row = 0; row < rc->height; row++) {
            for (uint32_t col = 0; col < rc->width; col++) {
               const uint64_t byte =
                  p->surface.base_offset_bytes +
                  (uint64_t)(rc->y + row) * R300_RB2D_PITCH_GRANULARITY +
                  (uint64_t)(rc->x + col) * 4u;
               assert(byte % 4u == 0u);
               const size_t index = (size_t)(byte / 4u);
               assert(index < dwords);
               touched[index]++;
            }
         }
      }
   }

   const size_t first = (size_t)(span->byte_offset / 4u);
   const size_t last = (size_t)((span->byte_offset + span->byte_size) / 4u);
   for (size_t i = 0; i < dwords; i++) {
      const uint8_t want = (i >= first && i < last) ? 1u : 0u;
      assert(touched[i] == want);
   }
   free(touched);
}

static void
test_refusals(void)
{
   const uint64_t bo = 1u << 20;
   enum r300_rb2d_span_refusal r = R300_RB2D_SPAN_REFUSAL_COUNT;
   struct r300_rb2d_span s = { .byte_offset = 0, .byte_size = 64,
                               .value = 0x11223344u };

   assert(r300_rb2d_linear_span_segments(NULL, bo, &r) == 0);
   assert(r == R300_RB2D_SPAN_REFUSE_NULL);

   s.byte_size = 0;
   assert(r300_rb2d_linear_span_segments(&s, bo, &r) == 0);
   assert(r == R300_RB2D_SPAN_REFUSE_SIZE_ZERO);

   s = (struct r300_rb2d_span){ 2u, 64u, 1u };
   assert(r300_rb2d_linear_span_segments(&s, bo, &r) == 0);
   assert(r == R300_RB2D_SPAN_REFUSE_OFFSET_ALIGNMENT);

   s = (struct r300_rb2d_span){ 0u, 66u, 1u };
   assert(r300_rb2d_linear_span_segments(&s, bo, &r) == 0);
   assert(r == R300_RB2D_SPAN_REFUSE_SIZE_ALIGNMENT);

   s = (struct r300_rb2d_span){ UINT64_MAX - 3u, 8u, 1u };
   assert(r300_rb2d_linear_span_segments(&s, bo, &r) == 0);
   assert(r == R300_RB2D_SPAN_REFUSE_RANGE_OVERFLOW);

   s = (struct r300_rb2d_span){ bo - 4u, 8u, 1u };
   assert(r300_rb2d_linear_span_segments(&s, bo, &r) == 0);
   assert(r == R300_RB2D_SPAN_REFUSE_OUTSIDE_BUFFER);

   /* A buffer whose tail stops inside a carrier row: the interval fits,
    * the surface the segment declares does not, and the decomposition
    * refuses rather than naming bytes past the allocation. */
   s = (struct r300_rb2d_span){ 0u, 68u, 1u };
   assert(r300_rb2d_linear_span_segments(&s, 100u, &r) == 0);
   assert(r == R300_RB2D_SPAN_REFUSE_FOOTPRINT_OUTSIDE_BUFFER);

   /* Storage smaller than the decomposition needs refuses whole, writing
    * nothing, so a caller never fills a prefix and drops the rest. */
   memset(plans, 0, sizeof(plans));
   s = (struct r300_rb2d_span){ 0u, 4u * 1024u * 1024u, 1u };
   const uint64_t big = 8u * 1024u * 1024u;
   assert(r300_rb2d_linear_span_segments(&s, big, &r) > 1u);
   assert(r300_rb2d_linear_span_plan(&s, big, plans, rects, 1u, &r) == 0);
   assert(r == R300_RB2D_SPAN_REFUSE_SEGMENT_STORAGE);
   assert(plans[0].rect_count == 0u);

   assert(r300_rb2d_linear_span_plan(&s, big, NULL, rects, MAX_SEGMENTS,
                                     &r) == 0);
   assert(r == R300_RB2D_SPAN_REFUSE_SEGMENT_STORAGE);

   for (unsigned i = 0; i < R300_RB2D_SPAN_REFUSAL_COUNT; i++)
      assert(r300_rb2d_span_refusal_name((enum r300_rb2d_span_refusal)i) !=
             NULL);
   assert(r300_rb2d_span_refusal_name(R300_RB2D_SPAN_REFUSAL_COUNT) == NULL);
}

static void
test_shapes(void)
{
   const uint64_t bo = 1u << 20;

   /* One dword at the origin: one rectangle, one row. */
   struct r300_rb2d_span s = { 0u, 4u, 0xdeadbeefu };
   covers_exactly(&s, bo, 1u);
   assert(plans[0].rect_count == 1u);
   assert(plans[0].rects[0].x == 0u && plans[0].rects[0].y == 0u);
   assert(plans[0].rects[0].width == 1u && plans[0].rects[0].height == 1u);

   /* One whole row from the origin: still one rectangle, because
    * DST_WIDTH_HEIGHT carries the width and the row count together. */
   s = (struct r300_rb2d_span){ 0u, 64u, 1u };
   covers_exactly(&s, bo, 1u);
   assert(plans[0].rect_count == 1u);
   assert(plans[0].rects[0].width == R300_RB2D_SPAN_DWORDS_PER_ROW);
   assert(plans[0].rects[0].height == 1u);

   /* Sixteen whole rows: one rectangle of height sixteen. */
   s = (struct r300_rb2d_span){ 0u, 16u * 64u, 1u };
   covers_exactly(&s, bo, 1u);
   assert(plans[0].rect_count == 1u);
   assert(plans[0].rects[0].height == 16u);

   /* The three-rectangle shape: a partial first row, whole rows, and a
    * partial last row.  Offset 12 puts the origin at column three. */
   s = (struct r300_rb2d_span){ 12u, 5000u & ~3u, 0x11223344u };
   covers_exactly(&s, bo, 1u);
   assert(plans[0].rect_count == 3u);
   assert(plans[0].rects[0].x == 3u && plans[0].rects[0].y == 0u);
   assert(plans[0].rects[0].width == R300_RB2D_SPAN_DWORDS_PER_ROW - 3u);
   assert(plans[0].rects[1].x == 0u && plans[0].rects[1].y == 1u);
   assert(plans[0].rects[1].width == R300_RB2D_SPAN_DWORDS_PER_ROW);
   assert(plans[0].rects[2].x == 0u);
   assert(plans[0].rects[2].height == 1u);

   /* An offset past the first kibibyte moves the base, not the column:
    * 1024 + 12 lands at column three of row zero of the next base. */
   s = (struct r300_rb2d_span){ 1024u + 12u, 128u, 1u };
   covers_exactly(&s, bo, 1u);
   assert(plans[0].surface.base_offset_bytes == 1024u);
   assert(plans[0].rects[0].x == 3u && plans[0].rects[0].y == 0u);

   /* An offset inside the first kibibyte but past its first row places the
    * origin at a nonzero row, which is the only way y is not zero. */
   s = (struct r300_rb2d_span){ 64u * 5u + 8u, 256u, 1u };
   covers_exactly(&s, bo, 1u);
   assert(plans[0].surface.base_offset_bytes == 0u);
   assert(plans[0].rects[0].y == 5u && plans[0].rects[0].x == 2u);

   /* A whole-row start needs no partial first rectangle. */
   s = (struct r300_rb2d_span){ 64u, 192u, 1u };
   covers_exactly(&s, bo, 1u);
   assert(plans[0].rect_count == 1u);
   assert(plans[0].rects[0].x == 0u && plans[0].rects[0].y == 1u);
   assert(plans[0].rects[0].height == 3u);

   /* A sub-row interval that does not reach the row end is one rectangle
    * narrower than the row. */
   s = (struct r300_rb2d_span){ 8u, 16u, 1u };
   covers_exactly(&s, bo, 1u);
   assert(plans[0].rect_count == 1u);
   assert(plans[0].rects[0].x == 2u && plans[0].rects[0].width == 4u);
}

/* The scissor reach caps a segment, so an interval longer than one window
 * becomes several segments in order with no dword lost between them. */
static void
test_segmentation(void)
{
   const uint64_t window =
      (uint64_t)R300_RB2D_MAX_COORD_REACH * R300_RB2D_PITCH_GRANULARITY;
   const uint64_t bo = 4u * window;
   enum r300_rb2d_span_refusal r;

   struct r300_rb2d_span s = { 0u, window, 0x5a5a5a5au };
   assert(r300_rb2d_linear_span_segments(&s, bo, &r) == 1u);
   covers_exactly(&s, bo, 1u);
   assert(plans[0].rects[plans[0].rect_count - 1u].y +
             plans[0].rects[plans[0].rect_count - 1u].height ==
          R300_RB2D_MAX_COORD_REACH);

   /* One dword past the window opens a second segment. */
   s = (struct r300_rb2d_span){ 0u, window + 4u, 0x5a5a5a5au };
   covers_exactly(&s, bo, 2u);
   assert(plans[1].surface.base_offset_bytes > 0u);

   /* Three windows and a remainder, from a nonzero unaligned start. */
   s = (struct r300_rb2d_span){ 12u, 3u * window + 4096u, 0x0f0f0f0fu };
   const uint32_t n = r300_rb2d_linear_span_segments(&s, bo, &r);
   assert(r == R300_RB2D_SPAN_OK && n >= 4u);
   covers_exactly(&s, bo, n);

   /* The emitted cost is the sum of the segments' own plan costs, so a
    * caller sizes one IB for the whole decomposition. */
   uint64_t expect = 0;
   for (uint32_t i = 0; i < n; i++)
      expect += R300_RB2D_FILL_DWORDS(plans[i].rect_count);
   assert(r300_rb2d_linear_span_dwords(plans, n) == expect);
   assert(r300_rb2d_linear_span_dwords(NULL, n) == 0u);
}

/* A sweep over offsets and sizes: the coverage replay is the oracle, so
 * every shape the arithmetic can take is checked rather than the handful a
 * reader thought of. */
static void
test_sweep(void)
{
   const uint64_t bo = 1u << 16;
   static const uint64_t offsets[] = { 0,  4,   8,   12,  60,  64,
                                       68, 124, 128, 512, 1020, 1024,
                                       1028, 4096, 4100 };
   static const uint64_t sizes[] = { 4,   8,    12,   32,   60,   64,
                                     68,  128,  252,  256,  1024, 4096,
                                     4100 };

   for (unsigned i = 0; i < ARRAY_LEN(offsets); i++) {
      for (unsigned j = 0; j < ARRAY_LEN(sizes); j++) {
         if (offsets[i] + sizes[j] > bo)
            continue;
         /* The segment's declared surface must fit the buffer, which the
          * decomposition refuses when it does not; that arm is covered in
          * test_refusals, so the sweep skips the shapes that reach it. */
         const uint64_t base =
            offsets[i] & ~(uint64_t)(R300_RB2D_OFFSET_GRANULARITY - 1u);
         const uint64_t end = offsets[i] + sizes[j];
         const uint64_t rows_end =
            (end - base + R300_RB2D_PITCH_GRANULARITY - 1u) /
            R300_RB2D_PITCH_GRANULARITY;
         if (base + rows_end * R300_RB2D_PITCH_GRANULARITY > bo)
            continue;

         struct r300_rb2d_span s = { offsets[i], sizes[j], 0xa5a5a5a5u };
         enum r300_rb2d_span_refusal r;
         const uint32_t n = r300_rb2d_linear_span_segments(&s, bo, &r);
         assert(r == R300_RB2D_SPAN_OK && n >= 1u);
         covers_exactly(&s, bo, n);
      }
   }
}

/* The attended cell's oracle shape, pinned here so the prediction the
 * procedure document carries is the decomposition the planner produces
 * rather than one a reader worked out.  A change to either that separates
 * them fails before a submission is armed against the wrong bytes. */
static void
test_attended_cell_shape(void)
{
   const uint64_t bo = 64u * 1024u;
   struct r300_rb2d_span s = { 12u, 4992u, 0x11223344u };

   covers_exactly(&s, bo, 1u);
   assert(plans[0].surface.base_offset_bytes == 0u);
   assert(plans[0].surface.height_pixels == 79u);
   assert(plans[0].rect_count == 3u);
   assert(plans[0].rects[0].x == 3u && plans[0].rects[0].y == 0u &&
          plans[0].rects[0].width == 13u && plans[0].rects[0].height == 1u);
   assert(plans[0].rects[1].x == 0u && plans[0].rects[1].y == 1u &&
          plans[0].rects[1].width == 16u && plans[0].rects[1].height == 77u);
   assert(plans[0].rects[2].x == 0u && plans[0].rects[2].y == 78u &&
          plans[0].rects[2].width == 3u && plans[0].rects[2].height == 1u);
   /* The declared surface stays inside the allocation, and the canary the
    * oracle places in the last 64 bytes lies past it. */
   assert(79u * R300_RB2D_PITCH_GRANULARITY == 5056u);
   assert(5056u < bo - 64u);
}

int
main(void)
{
   test_refusals();
   test_shapes();
   test_segmentation();
   test_sweep();
   test_attended_cell_shape();
   printf("r300_rb2d_linear_span_test: all checks passed\n");
   return 0;
}
