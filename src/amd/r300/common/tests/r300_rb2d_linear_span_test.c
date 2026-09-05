/*
 * SPDX-License-Identifier: MIT
 *
 * The decomposition's primary obligation is agreement with the plan it
 * feeds: every rectangle it produces is one r300_rb2d_fill_plan_check
 * admits, every dword of the requested interval is covered exactly once,
 * and nothing outside the interval is touched.  The coverage arms below
 * prove that by replaying each decomposition onto a shadow of the buffer,
 * so a rectangle placed one row or one column wrong is a miscount rather
 * than a reader's judgment.
 *
 * Every shape arm runs on both carriers: the 256-byte row the retained
 * direct-write stream exercises and the 64-byte row that is the tightest
 * DST_PITCH_OFFSET can name.  A rule that holds on one pitch and not the
 * other is a pitch-specific defect rather than a decomposition rule, and
 * running both is what separates them.
 */

/* The asserts carry this test's verdicts, so they stay live under NDEBUG. */
#undef NDEBUG

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

/* The two carriers every shape arm runs on. */
static const struct r300_rb2d_span_layout tight = {
   .pitch_bytes = R300_RB2D_PITCH_GRANULARITY,
   .format = R300_RB2D_FORMAT_ARGB8888,
};
static const struct r300_rb2d_span_layout witnessed = {
   .pitch_bytes = R300_RB2D_SPAN_PITCH_DIRECT_WRITE,
   .format = R300_RB2D_FORMAT_ARGB8888,
};

/* Replays a decomposition onto a per-dword touch count of the buffer and
 * holds it to the interval: every dword inside touched once, every dword
 * outside untouched, and every plan admitted by the plan checker. */
static void
covers_exactly(const struct r300_rb2d_span *span,
               const struct r300_rb2d_span_layout *layout,
               uint64_t buffer_bytes, uint32_t expect_segments)
{
   const uint32_t pitch = layout->pitch_bytes;
   const uint32_t per_row = r300_rb2d_span_layout_pixels_per_row(layout);
   enum r300_rb2d_span_refusal refusal = R300_RB2D_SPAN_REFUSAL_COUNT;
   assert(per_row > 0u);

   const uint32_t sized =
      r300_rb2d_linear_span_segments(span, layout, buffer_bytes, &refusal);
   assert(refusal == R300_RB2D_SPAN_OK);
   assert(sized == expect_segments);

   const uint32_t n = r300_rb2d_linear_span_plan(
      span, layout, buffer_bytes, plans, rects, MAX_SEGMENTS, &refusal);
   assert(refusal == R300_RB2D_SPAN_OK);
   assert(n == sized);

   const size_t dwords = (size_t)(buffer_bytes / 4u);
   uint8_t *touched = calloc(dwords, 1);
   assert(touched != NULL);

   for (uint32_t s = 0; s < n; s++) {
      const struct r300_rb2d_fill_plan *p = &plans[s];
      /* The plan is the authority on its own admissibility. */
      assert(r300_rb2d_fill_plan_check(p) == R300_RB2D_FILL_OK);
      assert(p->surface.pitch_bytes == pitch);
      assert(p->surface.width_pixels == per_row);
      assert(p->surface.base_offset_bytes % R300_RB2D_OFFSET_GRANULARITY == 0);

      for (uint32_t r = 0; r < p->rect_count; r++) {
         const struct r300_rb2d_fill_rect *rc = &p->rects[r];
         assert(rc->value == span->value);
         /* Every rectangle stays inside the scissor the emitter opens. */
         assert(rc->x + rc->width <= R300_RB2D_SAFE_EXCLUSIVE_END);
         assert(rc->y + rc->height <= R300_RB2D_SAFE_EXCLUSIVE_END);
         for (uint32_t row = 0; row < rc->height; row++) {
            for (uint32_t col = 0; col < rc->width; col++) {
               const uint64_t byte = p->surface.base_offset_bytes +
                                     (uint64_t)(rc->y + row) * pitch +
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

/* The carrier is checked before a rectangle is cut against it, so every
 * grid DST_PITCH_OFFSET packs refuses by its own name. */
static void
test_layouts(void)
{
   struct r300_rb2d_span_layout l;

   assert(r300_rb2d_span_layout_check(NULL) ==
          R300_RB2D_SPAN_REFUSE_LAYOUT_NULL);
   assert(r300_rb2d_span_layout_pixels_per_row(NULL) == 0u);

   assert(r300_rb2d_span_layout_check(&tight) == R300_RB2D_SPAN_OK);
   assert(r300_rb2d_span_layout_pixels_per_row(&tight) == 16u);
   assert(r300_rb2d_span_layout_check(&witnessed) == R300_RB2D_SPAN_OK);
   assert(r300_rb2d_span_layout_pixels_per_row(&witnessed) == 64u);

   l = tight;
   l.pitch_bytes = 0u;
   assert(r300_rb2d_span_layout_check(&l) ==
          R300_RB2D_SPAN_REFUSE_LAYOUT_PITCH_ZERO);

   /* Off the 64-byte grid: DST_PITCH_OFFSET counts 64-byte units, so 100
    * bytes has no representation in the word. */
   l.pitch_bytes = 100u;
   assert(r300_rb2d_span_layout_check(&l) ==
          R300_RB2D_SPAN_REFUSE_LAYOUT_PITCH_GRID);

   /* One unit past the 10-bit pitch field. */
   l.pitch_bytes = (R300_RB2D_MAX_PITCH_UNITS + 1u) *
                   R300_RB2D_PITCH_GRANULARITY;
   assert(r300_rb2d_span_layout_check(&l) ==
          R300_RB2D_SPAN_REFUSE_LAYOUT_PITCH_FIELD);

   /* The widest carrier the scissor admits is the largest 64-byte
    * multiple whose row of dwords stays inside R300_RB2D_SAFE_EXCLUSIVE_END,
    * and one grid step past it refuses. */
   const uint32_t widest =
      (R300_RB2D_SAFE_EXCLUSIVE_END / (R300_RB2D_PITCH_GRANULARITY / 4u)) *
      R300_RB2D_PITCH_GRANULARITY;
   l.pitch_bytes = widest;
   assert(r300_rb2d_span_layout_check(&l) == R300_RB2D_SPAN_OK);
   assert(r300_rb2d_span_layout_pixels_per_row(&l) == widest / 4u);
   assert(widest / 4u <= R300_RB2D_SAFE_EXCLUSIVE_END);
   l.pitch_bytes = widest + R300_RB2D_PITCH_GRANULARITY;
   assert(r300_rb2d_span_layout_check(&l) ==
          R300_RB2D_SPAN_REFUSE_LAYOUT_ROW_BEYOND_SCISSOR);

   /* A format whose pixel is not the span's pattern width. */
   l = tight;
   l.format = R300_RB2D_FORMAT_COUNT;
   assert(r300_rb2d_span_layout_check(&l) ==
          R300_RB2D_SPAN_REFUSE_LAYOUT_FORMAT);

   /* A layout refusal reaches the decomposition unchanged, so a caller
    * never cuts rectangles against a carrier the plan would reject. */
   enum r300_rb2d_span_refusal r = R300_RB2D_SPAN_REFUSAL_COUNT;
   const struct r300_rb2d_span s = { 0u, 64u, 1u };
   assert(r300_rb2d_linear_span_segments(&s, &l, 1u << 20, &r) == 0u);
   assert(r == R300_RB2D_SPAN_REFUSE_LAYOUT_FORMAT);
   assert(r300_rb2d_linear_span_segments(&s, NULL, 1u << 20, &r) == 0u);
   assert(r == R300_RB2D_SPAN_REFUSE_LAYOUT_NULL);
}

/* DST_PITCH_OFFSET's offset field addresses 32 bits from the relocated
 * base, so the representable interval is [0, 2^32) and an interval whose
 * last byte sits above it refuses by its own name.  The buffer is wider
 * than that space in every arm, so the refusal cannot be the
 * outside-the-buffer rule wearing another name.
 */
static void
test_address_width(void)
{
   const uint64_t bo = R300_RB2D_ADDRESS_SPACE_BYTES + (1u << 16);
   enum r300_rb2d_span_refusal r = R300_RB2D_SPAN_REFUSAL_COUNT;
   struct r300_rb2d_span s;

   /* An interval ending exactly at 2^32 is the widest representable one. */
   s = (struct r300_rb2d_span){ R300_RB2D_ADDRESS_SPACE_BYTES - 8u, 8u, 1u };
   assert(r300_rb2d_linear_span_segments(&s, &tight, bo, &r) == 1u);
   assert(r == R300_RB2D_SPAN_OK);

   /* Its last byte is 0xfffffffb, one dword inside the space. */
   s = (struct r300_rb2d_span){ R300_RB2D_ADDRESS_SPACE_BYTES - 8u, 4u, 1u };
   assert(r300_rb2d_linear_span_segments(&s, &tight, bo, &r) == 1u);
   assert(r == R300_RB2D_SPAN_OK);

   /* One dword past: the base fits 32 bits, the second dword does not. */
   s = (struct r300_rb2d_span){ (uint64_t)UINT32_MAX - 3u, 8u, 1u };
   assert(r300_rb2d_linear_span_segments(&s, &tight, bo, &r) == 0u);
   assert(r == R300_RB2D_SPAN_REFUSE_ADDRESS_WIDTH);

   /* An offset at the exclusive end names no representable byte. */
   s = (struct r300_rb2d_span){ R300_RB2D_ADDRESS_SPACE_BYTES, 4u, 1u };
   assert(r300_rb2d_linear_span_segments(&s, &tight, bo, &r) == 0u);
   assert(r == R300_RB2D_SPAN_REFUSE_ADDRESS_WIDTH);

   /* A range that overflows 64 bits is caught before the address rule, so
    * the two refusals stay distinct. */
   s = (struct r300_rb2d_span){ UINT64_MAX - 3u, 8u, 1u };
   assert(r300_rb2d_linear_span_segments(&s, &tight, bo, &r) == 0u);
   assert(r == R300_RB2D_SPAN_REFUSE_RANGE_OVERFLOW);
}

static void
test_refusals(void)
{
   const uint64_t bo = 1u << 20;
   enum r300_rb2d_span_refusal r = R300_RB2D_SPAN_REFUSAL_COUNT;
   struct r300_rb2d_span s = { .byte_offset = 0, .byte_size = 64,
                               .value = 0x11223344u };

   assert(r300_rb2d_linear_span_segments(NULL, &tight, bo, &r) == 0);
   assert(r == R300_RB2D_SPAN_REFUSE_NULL);

   s.byte_size = 0;
   assert(r300_rb2d_linear_span_segments(&s, &tight, bo, &r) == 0);
   assert(r == R300_RB2D_SPAN_REFUSE_SIZE_ZERO);

   s = (struct r300_rb2d_span){ 2u, 64u, 1u };
   assert(r300_rb2d_linear_span_segments(&s, &tight, bo, &r) == 0);
   assert(r == R300_RB2D_SPAN_REFUSE_OFFSET_ALIGNMENT);

   s = (struct r300_rb2d_span){ 0u, 66u, 1u };
   assert(r300_rb2d_linear_span_segments(&s, &tight, bo, &r) == 0);
   assert(r == R300_RB2D_SPAN_REFUSE_SIZE_ALIGNMENT);

   s = (struct r300_rb2d_span){ bo - 4u, 8u, 1u };
   assert(r300_rb2d_linear_span_segments(&s, &tight, bo, &r) == 0);
   assert(r == R300_RB2D_SPAN_REFUSE_OUTSIDE_BUFFER);

   /* The footprint bound is the kernel's end_byte: the last rectangle's
    * row start plus (x + width) * cpp.  Sixty-eight bytes on the 64-byte
    * carrier write one whole row and four bytes of the next, so the
    * footprint is 68: a 100-byte buffer admits it although the second
    * carrier row runs past the buffer, and a 66-byte buffer refuses it. */
   s = (struct r300_rb2d_span){ 0u, 68u, 1u };
   assert(r300_rb2d_linear_span_segments(&s, &tight, 100u, &r) == 1u);
   assert(r == R300_RB2D_SPAN_OK);
   s = (struct r300_rb2d_span){ 0u, 68u, 1u };
   assert(r300_rb2d_linear_span_segments(&s, &tight, 68u, &r) == 1u);
   /* Sixty-six bytes is off the dword grid for the span size rule, so the
    * refusal under test is reached through a dword-aligned buffer that is
    * still short: the interval [0, 68) against 64 bytes. */
   assert(r300_rb2d_linear_span_segments(&s, &tight, 64u, &r) == 0);
   assert(r == R300_RB2D_SPAN_REFUSE_OUTSIDE_BUFFER);
   /* A fill's written bytes are exactly its interval, so an interval inside
    * the buffer has its last written byte inside it too: the footprint
    * refusal is the guard that keeps that true if the cut ever writes past
    * the interval, and a span ending on the buffer's last byte passes. */
   s = (struct r300_rb2d_span){ 1024u + 4u, 4u, 1u };
   assert(r300_rb2d_linear_span_segments(&s, &tight, 1032u, &r) == 1u);
   assert(r == R300_RB2D_SPAN_OK);

   /* Storage smaller than the decomposition needs refuses whole, writing
    * nothing, so a caller never fills a prefix and drops the rest. */
   memset(plans, 0, sizeof(plans));
   s = (struct r300_rb2d_span){ 0u, 4u * 1024u * 1024u, 1u };
   const uint64_t big = 8u * 1024u * 1024u;
   assert(r300_rb2d_linear_span_segments(&s, &tight, big, &r) > 1u);
   assert(r300_rb2d_linear_span_plan(&s, &tight, big, plans, rects, 1u, &r) ==
          0);
   assert(r == R300_RB2D_SPAN_REFUSE_SEGMENT_STORAGE);
   assert(plans[0].rect_count == 0u);

   assert(r300_rb2d_linear_span_plan(&s, &tight, big, NULL, rects,
                                     MAX_SEGMENTS, &r) == 0);
   assert(r == R300_RB2D_SPAN_REFUSE_SEGMENT_STORAGE);

   for (unsigned i = 0; i < R300_RB2D_SPAN_REFUSAL_COUNT; i++)
      assert(r300_rb2d_span_refusal_name((enum r300_rb2d_span_refusal)i) !=
             NULL);
   assert(r300_rb2d_span_refusal_name(R300_RB2D_SPAN_REFUSAL_COUNT) == NULL);
}

/* Sizing and writing judge the same plan, so an interval whose refusal
 * falls on a later segment leaves caller storage exactly as it was.  The
 * shape below places the refusal on the second segment: the first fills
 * one whole scissor window, and the second declares a surface whose last
 * row reaches past the allocation.
 */
static void
test_all_or_nothing(void)
{
   const uint32_t per_row = r300_rb2d_span_layout_pixels_per_row(&tight);
   const uint64_t window =
      (uint64_t)R300_RB2D_SAFE_EXCLUSIVE_END * tight.pitch_bytes;
   /* The tail is a whole number of dwords that is not a whole number of
    * rows, so the span needs a second segment, and the write is offered
    * storage for one: the refusal lands after the first segment was cut
    * and before anything reaches caller storage. */
   const uint64_t tail_dwords = 78u * per_row + 1u;
   const uint64_t bo = window + tail_dwords * 4u;
   enum r300_rb2d_span_refusal r = R300_RB2D_SPAN_REFUSAL_COUNT;

   const struct r300_rb2d_span s = { 0u, window + tail_dwords * 4u,
                                     0x11223344u };
   assert(s.byte_offset + s.byte_size <= bo);

   /* A sentinel the decomposition must not disturb. */
   memset(plans, 0xa5, sizeof(plans));
   memset(rects, 0xa5, sizeof(rects));

   assert(r300_rb2d_linear_span_segments(&s, &tight, bo, &r) == 2u);
   assert(r == R300_RB2D_SPAN_OK);

   r = R300_RB2D_SPAN_REFUSAL_COUNT;
   assert(r300_rb2d_linear_span_plan(&s, &tight, bo, plans, rects, 1u,
                                     &r) == 0u);
   assert(r == R300_RB2D_SPAN_REFUSE_SEGMENT_STORAGE);

   const uint8_t *bytes = (const uint8_t *)plans;
   for (size_t i = 0; i < sizeof(plans); i++)
      assert(bytes[i] == 0xa5u);
   bytes = (const uint8_t *)rects;
   for (size_t i = 0; i < sizeof(rects); i++)
      assert(bytes[i] == 0xa5u);

   memset(plans, 0, sizeof(plans));
   memset(rects, 0, sizeof(rects));
}

static void
test_shapes(const struct r300_rb2d_span_layout *layout)
{
   const uint64_t bo = 1u << 20;
   const uint32_t per_row = r300_rb2d_span_layout_pixels_per_row(layout);
   const uint32_t pitch = layout->pitch_bytes;

   /* One dword at the origin: one rectangle, one row. */
   struct r300_rb2d_span s = { 0u, 4u, 0xdeadbeefu };
   covers_exactly(&s, layout, bo, 1u);
   assert(plans[0].rect_count == 1u);
   assert(plans[0].rects[0].x == 0u && plans[0].rects[0].y == 0u);
   assert(plans[0].rects[0].width == 1u && plans[0].rects[0].height == 1u);

   /* One whole row from the origin: still one rectangle, because
    * DST_WIDTH_HEIGHT carries the width and the row count together. */
   s = (struct r300_rb2d_span){ 0u, pitch, 1u };
   covers_exactly(&s, layout, bo, 1u);
   assert(plans[0].rect_count == 1u);
   assert(plans[0].rects[0].width == per_row);
   assert(plans[0].rects[0].height == 1u);

   /* Sixteen whole rows: one rectangle of height sixteen. */
   s = (struct r300_rb2d_span){ 0u, 16u * (uint64_t)pitch, 1u };
   covers_exactly(&s, layout, bo, 1u);
   assert(plans[0].rect_count == 1u);
   assert(plans[0].rects[0].height == 16u);

   /* The three-rectangle shape: a partial first row, whole rows, and a
    * partial last row.  Offset 12 puts the origin at column three. */
   s = (struct r300_rb2d_span){ 12u, 5000u & ~3u, 0x11223344u };
   covers_exactly(&s, layout, bo, 1u);
   assert(plans[0].rect_count == 3u);
   assert(plans[0].rects[0].x == 3u && plans[0].rects[0].y == 0u);
   assert(plans[0].rects[0].width == per_row - 3u);
   assert(plans[0].rects[1].x == 0u && plans[0].rects[1].y == 1u);
   assert(plans[0].rects[1].width == per_row);
   assert(plans[0].rects[2].x == 0u);
   assert(plans[0].rects[2].height == 1u);

   /* An offset past the first kibibyte moves the base, not the column:
    * 1024 + 12 lands at column three of row zero of the next base. */
   s = (struct r300_rb2d_span){ 1024u + 12u, 128u, 1u };
   covers_exactly(&s, layout, bo, 1u);
   assert(plans[0].surface.base_offset_bytes == 1024u);
   assert(plans[0].rects[0].x == 3u && plans[0].rects[0].y == 0u);

   /* An offset inside the first kibibyte but past its first row places the
    * origin at a nonzero row, which is the only way y is not zero. */
   s = (struct r300_rb2d_span){ 5u * (uint64_t)pitch + 8u, 256u, 1u };
   covers_exactly(&s, layout, bo, 1u);
   assert(plans[0].surface.base_offset_bytes ==
          (5u * pitch + 8u) / R300_RB2D_OFFSET_GRANULARITY *
             R300_RB2D_OFFSET_GRANULARITY);
   assert(plans[0].rects[0].x == 2u);

   /* A whole-row start needs no partial first rectangle. */
   s = (struct r300_rb2d_span){ pitch, 3u * (uint64_t)pitch, 1u };
   covers_exactly(&s, layout, bo, 1u);
   assert(plans[0].rect_count == 1u);
   assert(plans[0].rects[0].x == 0u && plans[0].rects[0].y == 1u);
   assert(plans[0].rects[0].height == 3u);

   /* A sub-row interval that does not reach the row end is one rectangle
    * narrower than the row. */
   s = (struct r300_rb2d_span){ 8u, 16u, 1u };
   covers_exactly(&s, layout, bo, 1u);
   assert(plans[0].rect_count == 1u);
   assert(plans[0].rects[0].x == 2u && plans[0].rects[0].width == 4u);
}

/* The scissor reach caps a segment, so an interval longer than one window
 * becomes several segments in order with no dword lost between them. */
static void
test_segmentation(const struct r300_rb2d_span_layout *layout)
{
   const uint64_t window =
      (uint64_t)R300_RB2D_SAFE_EXCLUSIVE_END * layout->pitch_bytes;
   const uint64_t bo = 4u * window;
   enum r300_rb2d_span_refusal r;

   struct r300_rb2d_span s = { 0u, window, 0x5a5a5a5au };
   assert(r300_rb2d_linear_span_segments(&s, layout, bo, &r) == 1u);
   covers_exactly(&s, layout, bo, 1u);
   assert(plans[0].rects[plans[0].rect_count - 1u].y +
             plans[0].rects[plans[0].rect_count - 1u].height ==
          R300_RB2D_SAFE_EXCLUSIVE_END);

   /* One dword past the window opens a second segment. */
   s = (struct r300_rb2d_span){ 0u, window + 4u, 0x5a5a5a5au };
   covers_exactly(&s, layout, bo, 2u);
   assert(plans[1].surface.base_offset_bytes > 0u);

   /* Three windows and a remainder, from a nonzero unaligned start. */
   s = (struct r300_rb2d_span){ 12u, 3u * window + 4096u, 0x0f0f0f0fu };
   const uint32_t n = r300_rb2d_linear_span_segments(&s, layout, bo, &r);
   assert(r == R300_RB2D_SPAN_OK && n >= 4u);
   covers_exactly(&s, layout, bo, n);

   /* The emitted cost is the sum of the segments' own plan costs, so a
    * caller sizes one IB for the whole decomposition. */
   uint64_t expect = 0;
   for (uint32_t i = 0; i < n; i++)
      expect += R300_RB2D_FILL_DWORDS(plans[i].rect_count);
   uint32_t got = 0;
   assert(r300_rb2d_linear_span_dwords(plans, n, &got));
   assert((uint64_t)got == expect);
}

/* The dword cost is a checked interface: absent storage, an empty
 * decomposition, and a rectangle count the fill plan does not admit each
 * refuse rather than returning a number a caller would size an IB from. */
static void
test_dword_cost_refusals(void)
{
   uint32_t got = 0xffffffffu;
   struct r300_rb2d_fill_plan one = { .rect_count = 1u };

   assert(!r300_rb2d_linear_span_dwords(&one, 1u, NULL));
   assert(!r300_rb2d_linear_span_dwords(NULL, 1u, &got));
   assert(got == 0u);
   assert(!r300_rb2d_linear_span_dwords(&one, 0u, &got));
   assert(got == 0u);

   assert(r300_rb2d_linear_span_dwords(&one, 1u, &got));
   assert(got == R300_RB2D_FILL_DWORDS(1u));

   one.rect_count = 0u;
   assert(!r300_rb2d_linear_span_dwords(&one, 1u, &got));
   one.rect_count = R300_RB2D_FILL_MAX_RECTS + 1u;
   assert(!r300_rb2d_linear_span_dwords(&one, 1u, &got));
}

/* Every carrier the layout admits produces a surface the fill plan admits,
 * at both extremes of the height a segment can declare.  That subsumption
 * is why the plan check inside the decomposition has no admitted input to
 * reject: it stands as the guard that keeps the counting and the writing
 * pass judging one object, and this arm fails the moment a loosened layout
 * rule lets a rejectable surface through.
 */
static void
test_layout_subsumes_plan_check(void)
{
   const uint32_t widest =
      (R300_RB2D_SAFE_EXCLUSIVE_END / (R300_RB2D_PITCH_GRANULARITY / 4u)) *
      R300_RB2D_PITCH_GRANULARITY;
   uint32_t admitted = 0;

   for (uint32_t pitch = R300_RB2D_PITCH_GRANULARITY;
        pitch <= widest + R300_RB2D_PITCH_GRANULARITY;
        pitch += R300_RB2D_PITCH_GRANULARITY) {
      const struct r300_rb2d_span_layout l = {
         .pitch_bytes = pitch, .format = R300_RB2D_FORMAT_ARGB8888
      };
      const uint32_t per_row = r300_rb2d_span_layout_pixels_per_row(&l);
      if (r300_rb2d_span_layout_check(&l) != R300_RB2D_SPAN_OK) {
         /* One step past the widest admitted carrier, and nothing
          * below it. */
         assert(pitch == widest + R300_RB2D_PITCH_GRANULARITY);
         assert(per_row == 0u);
         continue;
      }
      admitted++;

      static const uint32_t heights[] = { 1u, R300_RB2D_SAFE_EXCLUSIVE_END };
      for (unsigned h = 0; h < ARRAY_LEN(heights); h++) {
         const struct r300_rb2d_fill_rect rect = {
            .x = 0u, .y = 0u, .width = per_row, .height = heights[h],
            .value = 0x11223344u
         };
         const struct r300_rb2d_fill_plan plan = {
            .surface = { .base_offset_bytes = 0u,
                         .pitch_bytes = pitch,
                         .width_pixels = per_row,
                         .height_pixels = heights[h],
                         .format = R300_RB2D_FORMAT_ARGB8888 },
            .write_mask = 0xffffffffu,
            .rects = &rect,
            .rect_count = 1u,
         };
         assert(r300_rb2d_fill_plan_check(&plan) == R300_RB2D_FILL_OK);
      }
   }

   assert(admitted == widest / R300_RB2D_PITCH_GRANULARITY);
}

/* The attended cell's decomposition, pinned so the prediction a submission
 * is armed against is the one the planner produces rather than one a
 * reader worked out.  A change that separates them fails here, before any
 * arming identity is bound to the wrong bytes. */
static void
test_attended_cell_shape(void)
{
   const uint64_t bo = 64u * 1024u;
   const struct r300_rb2d_span s = { 12u, 4992u, 0x11223344u };

   /* The witnessed 256-byte carrier: 64 pixels per row. */
   covers_exactly(&s, &witnessed, bo, 1u);
   assert(plans[0].surface.pitch_bytes == 256u);
   assert(plans[0].surface.base_offset_bytes == 0u);
   assert(plans[0].rect_count == 3u);
   assert(plans[0].rects[0].x == 3u && plans[0].rects[0].y == 0u &&
          plans[0].rects[0].width == 61u && plans[0].rects[0].height == 1u);
   assert(plans[0].rects[1].x == 0u && plans[0].rects[1].y == 1u &&
          plans[0].rects[1].width == 64u && plans[0].rects[1].height == 18u);
   assert(plans[0].rects[2].x == 0u && plans[0].rects[2].y == 19u &&
          plans[0].rects[2].width == 35u && plans[0].rects[2].height == 1u);
   assert(plans[0].surface.height_pixels == 20u);
   /* The declared surface footprint leaves the allocation tail outside it,
    * so a canary past row 19 proves the fill wrote no further. */
   assert(plans[0].surface.height_pixels * 256u == 5120u);

   /* The tightest 64-byte carrier reaches the same interval through a
    * taller surface, which is the pitch-specific difference between the
    * two routes. */
   covers_exactly(&s, &tight, bo, 1u);
   assert(plans[0].surface.pitch_bytes == 64u);
   assert(plans[0].rect_count == 3u);
   assert(plans[0].rects[0].x == 3u && plans[0].rects[0].y == 0u &&
          plans[0].rects[0].width == 13u && plans[0].rects[0].height == 1u);
   assert(plans[0].rects[1].x == 0u && plans[0].rects[1].y == 1u &&
          plans[0].rects[1].width == 16u && plans[0].rects[1].height == 77u);
   assert(plans[0].rects[2].x == 0u && plans[0].rects[2].y == 78u &&
          plans[0].rects[2].width == 3u && plans[0].rects[2].height == 1u);
   assert(plans[0].surface.height_pixels == 79u);
   assert(plans[0].surface.height_pixels * 64u == 5056u);
}

/* A sweep over offsets and sizes: the coverage replay is the oracle, so
 * every shape the arithmetic can take is checked rather than the handful a
 * reader thought of. */
static void
test_sweep(const struct r300_rb2d_span_layout *layout)
{
   const uint64_t bo = 1u << 16;
   const uint32_t pitch = layout->pitch_bytes;
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
         const uint64_t rows_end = (end - base + pitch - 1u) / pitch;
         if (base + rows_end * pitch > bo)
            continue;

         struct r300_rb2d_span s = { offsets[i], sizes[j], 0xa5a5a5a5u };
         enum r300_rb2d_span_refusal r;
         const uint32_t n =
            r300_rb2d_linear_span_segments(&s, layout, bo, &r);
         assert(r == R300_RB2D_SPAN_OK && n >= 1u);
         covers_exactly(&s, layout, bo, n);
      }
   }
}

int
main(void)
{
   const struct r300_rb2d_span_layout *const carriers[] = { &witnessed,
                                                            &tight };

   test_layouts();
   test_layout_subsumes_plan_check();
   test_address_width();
   test_refusals();
   test_all_or_nothing();
   test_dword_cost_refusals();
   test_attended_cell_shape();

   for (unsigned i = 0; i < ARRAY_LEN(carriers); i++) {
      test_shapes(carriers[i]);
      test_segmentation(carriers[i]);
      test_sweep(carriers[i]);
   }

   printf("r300_rb2d_linear_span_test: all checks passed\n");
   return 0;
}
