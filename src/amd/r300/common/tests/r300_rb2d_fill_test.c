/*
 * SPDX-License-Identifier: MIT
 *
 * The RB2D solid-fill plan: every surface and rectangle rule refused on its
 * own mutation, the field packing the retained 1x1 cell cannot show, and
 * the emission the fixed direct-write control reproduces byte for byte.
 */

/* The asserts carry this test's verdicts, so they stay live under NDEBUG. */
#undef NDEBUG

#include "r300_direct_write.h"
#include "r300_rb2d_fill.h"
#include "r300_tcl_bypass_triangle.h"

#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#define WORDS 128u

static const struct r300_rb2d_fill_rect one_rect[] = {
   { 4u, 8u, 2u, 3u, 0xdeadbeefu },
};

static struct r300_rb2d_fill_plan
base_plan(void)
{
   return (struct r300_rb2d_fill_plan){
      .surface = {
         .base_offset_bytes = 0u,
         .pitch_bytes = 256u,
         .width_pixels = 64u,
         .height_pixels = 64u,
         .format = R300_RB2D_FORMAT_ARGB8888,
      },
      .write_mask = 0xffffffffu,
      .rects = one_rect,
      .rect_count = 1u,
   };
}

#define REFUSES(plan, code)                                                  \
   do {                                                                      \
      const enum r300_rb2d_fill_refusal got = r300_rb2d_fill_plan_check(&plan); \
      assert(got == (code));                                                  \
      assert(r300_rb2d_fill_refusal_name(got) != NULL);                       \
   } while (0)

/* Each rule owns one arm, so a rule that stops holding fails by name rather
 * than passing inside another rule's refusal. */
static void
test_plan_rules(void)
{
   struct r300_rb2d_fill_plan p = base_plan();
   assert(r300_rb2d_fill_plan_check(&p) == R300_RB2D_FILL_OK);
   assert(r300_rb2d_fill_plan_check(NULL) == R300_RB2D_FILL_REFUSE_NO_RECTS);

   p = base_plan();
   p.rect_count = 0;
   REFUSES(p, R300_RB2D_FILL_REFUSE_NO_RECTS);

   p = base_plan();
   p.rects = NULL;
   REFUSES(p, R300_RB2D_FILL_REFUSE_NO_RECTS);

   p = base_plan();
   p.rect_count = R300_RB2D_FILL_MAX_RECTS + 1u;
   REFUSES(p, R300_RB2D_FILL_REFUSE_TOO_MANY_RECTS);

   p = base_plan();
   p.surface.format = R300_RB2D_FORMAT_COUNT;
   REFUSES(p, R300_RB2D_FILL_REFUSE_FORMAT);

   /* DST_PITCH_OFFSET counts pitch in 64-byte units, so a pitch off that
    * grid has no representation and refuses rather than truncating. */
   p = base_plan();
   p.surface.pitch_bytes = 0;
   REFUSES(p, R300_RB2D_FILL_REFUSE_PITCH_ZERO);
   p.surface.pitch_bytes = 256u + 4u;
   REFUSES(p, R300_RB2D_FILL_REFUSE_PITCH_GRID);
   p.surface.pitch_bytes = (0x3ffu + 1u) * R300_RB2D_PITCH_GRANULARITY;
   REFUSES(p, R300_RB2D_FILL_REFUSE_PITCH_FIELD);

   /* The offset field counts 1 KiB units for the same reason. */
   p = base_plan();
   p.surface.base_offset_bytes = 512u;
   REFUSES(p, R300_RB2D_FILL_REFUSE_OFFSET_GRID);
   p.surface.base_offset_bytes = 1024u;
   assert(r300_rb2d_fill_plan_check(&p) == R300_RB2D_FILL_OK);

   p = base_plan();
   p.surface.width_pixels = 0;
   REFUSES(p, R300_RB2D_FILL_REFUSE_EXTENT_ZERO);
   p = base_plan();
   p.surface.height_pixels = 0;
   REFUSES(p, R300_RB2D_FILL_REFUSE_EXTENT_ZERO);

   /* A row narrower than its own width would put a rectangle inside the
    * surface into the next row's bytes. */
   p = base_plan();
   p.surface.width_pixels = 65u;
   REFUSES(p, R300_RB2D_FILL_REFUSE_PITCH_BELOW_WIDTH);

   struct r300_rb2d_fill_rect r = one_rect[0];

   p = base_plan();
   r = one_rect[0];
   r.width = 0;
   p.rects = &r;
   REFUSES(p, R300_RB2D_FILL_REFUSE_RECT_EMPTY);
   r = one_rect[0];
   r.height = 0;
   REFUSES(p, R300_RB2D_FILL_REFUSE_RECT_EMPTY);

   r = one_rect[0];
   r.x = 0x10000u;
   REFUSES(p, R300_RB2D_FILL_REFUSE_RECT_FIELD);

   /* The emitter opens the 2D scissor to R300_RB2D_MAX_COORD_REACH, which
    * is narrower than the coordinate field, so a rectangle inside the field
    * and past the scissor would be clipped by the stream's own register and
    * report success.  The refusal lands before the surface-bounds rule, so
    * a surface large enough to hold such a rectangle still refuses it: the
    * scissor, not the surface, is what clips.
    */
   {
      /* The widest surface the pitch field can name is 0x3ff 64-byte units,
       * 65472 bytes, which is 16368 ARGB8888 pixels -- wider than the
       * scissor reach, so the scissor is what bounds a rectangle on both
       * axes.  This surface is one pixel past the reach on each axis and
       * its pitch stays inside the field. */
      struct r300_rb2d_surface wide = p.surface;
      wide.width_pixels = R300_RB2D_MAX_COORD_REACH + 1u;
      wide.height_pixels = R300_RB2D_MAX_COORD_REACH + 1u;
      wide.pitch_bytes = (R300_RB2D_MAX_COORD_REACH + 1u) * 4u;
      struct r300_rb2d_fill_plan big = p;
      big.surface = wide;
      struct r300_rb2d_fill_rect far = { 0u, 0u, R300_RB2D_MAX_COORD_REACH,
                                         1u, 1u };
      big.rects = &far;
      big.rect_count = 1u;
      /* Reaching exactly the scissor value is admitted; one past refuses,
       * so the bound is exact rather than generous. */
      assert(r300_rb2d_fill_plan_check(&big) == R300_RB2D_FILL_OK);
      far.width = R300_RB2D_MAX_COORD_REACH + 1u;
      assert(r300_rb2d_fill_plan_check(&big) ==
             R300_RB2D_FILL_REFUSE_RECT_BEYOND_SCISSOR);
      far = (struct r300_rb2d_fill_rect){ 0u, 0u, 1u,
                                          R300_RB2D_MAX_COORD_REACH + 1u,
                                          1u };
      assert(r300_rb2d_fill_plan_check(&big) ==
             R300_RB2D_FILL_REFUSE_RECT_BEYOND_SCISSOR);
      /* An origin at the reach with any extent also passes: the far edge
       * is what the scissor clips. */
      far = (struct r300_rb2d_fill_rect){ 0u, R300_RB2D_MAX_COORD_REACH, 1u,
                                          1u, 1u };
      assert(r300_rb2d_fill_plan_check(&big) ==
             R300_RB2D_FILL_REFUSE_RECT_BEYOND_SCISSOR);
   }

   /* The surface extent bounds the rectangle, and the boundary case one
    * pixel inside is admitted so the rule is exact rather than generous. */
   r = (struct r300_rb2d_fill_rect){ 62u, 62u, 2u, 2u, 1u };
   assert(r300_rb2d_fill_plan_check(&p) == R300_RB2D_FILL_OK);
   r.width = 3u;
   REFUSES(p, R300_RB2D_FILL_REFUSE_RECT_OUTSIDE);
   r = (struct r300_rb2d_fill_rect){ 62u, 62u, 2u, 3u, 1u };
   REFUSES(p, R300_RB2D_FILL_REFUSE_RECT_OUTSIDE);

   for (unsigned i = 0; i < R300_RB2D_FILL_REFUSAL_COUNT; i++)
      assert(r300_rb2d_fill_refusal_name((enum r300_rb2d_fill_refusal)i) !=
             NULL);
   assert(r300_rb2d_fill_refusal_name(R300_RB2D_FILL_REFUSAL_COUNT) == NULL);
}

/* The emitter refuses an invalid plan and a short buffer before writing a
 * stream it would then have to report. */
static void
test_emit_refusals(void)
{
   struct r300_rb2d_fill_plan p = base_plan();
   struct r300_rb2d_fill_ib ib;
   uint32_t words[WORDS];

   assert(r300_rb2d_fill_emit_into(&p, NULL, WORDS, &ib) == -EINVAL);
   assert(r300_rb2d_fill_emit_into(&p, words, WORDS, NULL) == -EINVAL);

   struct r300_rb2d_fill_plan bad = base_plan();
   bad.surface.pitch_bytes = 100u;
   assert(r300_rb2d_fill_emit_into(&bad, words, WORDS, &ib) == -EINVAL);

   assert(r300_rb2d_fill_emit_into(&p, words, WORDS, &ib) == 0);
   const uint32_t exact = ib.ib_size_dwords;
   assert(exact == R300_RB2D_FILL_DWORDS(1));
   assert(r300_rb2d_fill_emit_into(&p, words, exact - 1u, &ib) == -ENOSPC);
   assert(ib.ib_size_dwords == 0);
   assert(r300_rb2d_fill_emit_into(&p, words, exact, &ib) == 0);
   assert(r300_rb2d_fill_validate_reloc_sites(&ib) == 0);
}

/* The retained cell's two 1x1 rectangles carry the same dword whichever
 * half holds width, so the packing is checked on a rectangle that is not
 * square: r100_copy_blit writes cur_pages | (stride_pixels << 16), width
 * high and height low.
 */
static void
test_field_packing(void)
{
   static const struct r300_rb2d_fill_rect wide[] = {
      { 1u, 2u, 7u, 3u, 0x01020304u },
   };
   struct r300_rb2d_fill_plan p = base_plan();
   struct r300_rb2d_fill_ib ib;
   uint32_t words[WORDS];

   p.rects = wide;
   assert(r300_rb2d_fill_emit_into(&p, words, WORDS, &ib) == 0);

   bool saw_y_x = false, saw_width_height = false;
   for (uint32_t i = 0; i + 1 < ib.ib_size_dwords; i++) {
      /* PACKET0 header for one register: the register's dword address in
       * the low bits, count zero. */
      if (ib.ib[i] == (0x1438u >> 2)) {
         assert(ib.ib[i + 1] == ((2u << 16) | 1u));
         saw_y_x = true;
      }
      if (ib.ib[i] == (0x1598u >> 2)) {
         assert(ib.ib[i + 1] == ((7u << 16) | 3u));
         saw_width_height = true;
      }
   }
   assert(saw_y_x && saw_width_height);
}

/* The fixed direct-write control is one instance of this plan, so its
 * stream is the plan's stream: same length, same bytes, same relocation
 * site.  This is the property that lets the retained RB2D witness carry
 * forward across the generalization. */
static void
test_control_cell_parity(void)
{
   static const struct r300_rb2d_fill_rect probes[] = {
      { R300_DIRECT_WRITE_A_X, R300_DIRECT_WRITE_A_Y, 1u, 1u,
        R300_DIRECT_WRITE_A_VALUE },
      { R300_DIRECT_WRITE_B_X, R300_DIRECT_WRITE_B_Y, 1u, 1u,
        R300_DIRECT_WRITE_B_VALUE },
   };
   struct r300_rb2d_fill_plan p = {
      .surface = {
         .base_offset_bytes = 0u,
         .pitch_bytes = R300_TRIANGLE_TARGET_PITCH_PIXELS * 4u,
         .width_pixels = R300_TRIANGLE_TARGET_WIDTH,
         .height_pixels = R300_TRIANGLE_TARGET_HEIGHT,
         .format = R300_RB2D_FORMAT_ARGB8888,
      },
      .write_mask = 0xffffffffu,
      .rects = probes,
      .rect_count = 2u,
   };
   struct r300_rb2d_fill_ib plan_ib;
   struct r300_direct_write_ib cell;
   uint32_t words[WORDS];

   assert(r300_rb2d_fill_emit_into(&p, words, WORDS, &plan_ib) == 0);
   assert(r300_direct_write_emit(&cell) == 0);
   assert(plan_ib.ib_size_dwords == cell.ib_size_dwords);
   assert(plan_ib.ib_size_dwords == R300_RB2D_FILL_DWORDS(2));
   assert(memcmp(plan_ib.ib, cell.ib, cell.ib_size_dwords * 4u) == 0);
   assert(plan_ib.reloc_site_count == cell.reloc_site_count);
   assert(plan_ib.reloc_sites[0].ib_index == cell.reloc_sites[0].ib_index);
   r300_direct_write_release(&cell);
}

int
main(void)
{
   test_plan_rules();
   test_emit_refusals();
   test_field_packing();
   test_control_cell_parity();
   printf("r300_rb2d_fill_test: all checks passed\n");
   return 0;
}
