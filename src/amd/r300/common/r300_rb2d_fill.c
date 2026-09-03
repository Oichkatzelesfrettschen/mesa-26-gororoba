/* SPDX-License-Identifier: MIT */

#include "r300_rb2d_fill.h"
#include "r300_pm4_builder.h"

#include <errno.h>
#include <string.h>

/* 2D engine registers, spelled as drivers/gpu/drm/radeon/radeon_reg.h
 * spells them.  Every register except DST_PITCH_OFFSET sits on the
 * reg_srcs/r300 safe list and passes the CS parser unchecked;
 * DST_PITCH_OFFSET is the r300_packet0_check case r100_reloc_pitch_offset
 * handles, which consumes the plan's one relocation.
 * (safe-list membership: rg -e 0x1438 -e 0x146C -e 0x147C -e 0x1598
 * -e 0x16C0 -e 0x16CC -e 0x16E8 -e 0x16EC -e 0x16F0 -e 0x1714
 * -e 0x1720 drivers/gpu/drm/radeon/reg_srcs/r300, one line per
 * emitted register; case dispatch: rg --fixed-strings
 * r100_reloc_pitch_offset drivers/gpu/drm/radeon/r300.c)
 */
#define RADEON_DST_PITCH_OFFSET 0x142C
#define RADEON_DST_Y_X 0x1438
#define RADEON_DP_GUI_MASTER_CNTL 0x146C
#define RADEON_DP_BRUSH_FRGD_CLR 0x147C
#define RADEON_DST_WIDTH_HEIGHT 0x1598
#define RADEON_DP_CNTL 0x16C0
#define RADEON_DP_WRITE_MSK 0x16CC
#define RADEON_DEFAULT_SC_BOTTOM_RIGHT 0x16E8
#define RADEON_SC_TOP_LEFT 0x16EC
#define RADEON_SC_BOTTOM_RIGHT 0x16F0
#define RADEON_DSTCACHE_CTLSTAT 0x1714
#define RADEON_WAIT_UNTIL 0x1720

#define RADEON_GMC_DST_PITCH_OFFSET_CNTL (1u << 1)
#define RADEON_GMC_BRUSH_SOLID_COLOR (13u << 4)
#define RADEON_COLOR_FORMAT_ARGB8888 6u
#define RADEON_ROP3_P 0x00f00000u
#define RADEON_GMC_CLR_CMP_CNTL_DIS (1u << 28)
#define RADEON_GMC_WR_MSK_DIS (1u << 30)
#define RADEON_DST_X_LEFT_TO_RIGHT (1u << 0)
#define RADEON_DST_Y_TOP_TO_BOTTOM (1u << 1)
#define RADEON_RB2D_DC_FLUSH_ALL 0xfu
#define RADEON_WAIT_DMA_GUI_IDLE (1u << 9)
#define RADEON_WAIT_2D_IDLECLEAN (1u << 16)
#define RADEON_WAIT_HOST_IDLECLEAN (1u << 18)

/* DST_PITCH_OFFSET splits into a 10-bit pitch field above a 22-bit offset
 * field, and DST_Y_X and DST_WIDTH_HEIGHT each pack two 16-bit fields, so a
 * plan value wider than its field has no representation in the word. */
#define RB2D_PITCH_FIELD_MAX 0x3ffu
#define RB2D_OFFSET_FIELD_MAX 0x3fffffu
#define RB2D_COORD_FIELD_MAX 0xffffu

/* The 2D scissor is established at its own field maximum rather than at the
 * surface extent, so a predecessor scissor cannot clip the plan's
 * rectangles.  That maximum is 13 bits, narrower than the 16-bit
 * coordinate fields, and radeon_reg.h names it: RADEON_DEFAULT_SC_RIGHT_MAX
 * (0x1fff << 0) and RADEON_DEFAULT_SC_BOTTOM_MAX (0x1fff << 16).  The
 * coordinate reach the plan admits is therefore the scissor's, exported as
 * R300_RB2D_MAX_COORD_REACH so a caller decomposing a larger region reads
 * the bound rather than deriving one from the wider field. */
#define RB2D_SCISSOR_MAX R300_RB2D_MAX_COORD_REACH

/* Four dwords per drm_radeon_cs_reloc entry, so a slot's payload indexes
 * the relocation chunk at four times the slot. */
#define RB2D_RELOC_PAYLOAD(slot) ((slot) * 4)

/* One descriptor per emitted format: the code DP_GUI_MASTER_CNTL's format
 * field carries, and the bytes one pixel occupies, which the surface rules
 * measure a row against.  A format lands here with both, so neither can be
 * inferred from the other or inherited from a neighbor.
 */
struct rb2d_format_info {
   uint32_t code;
   uint32_t bytes_per_pixel;
};

static const struct rb2d_format_info format_table[R300_RB2D_FORMAT_COUNT] = {
   [R300_RB2D_FORMAT_ARGB8888] = { RADEON_COLOR_FORMAT_ARGB8888, 4u },
};

/* NULL outside the table, so a format the plan never admitted resolves to no
 * descriptor rather than to the first row's stride. */
static const struct rb2d_format_info *
format_info(enum r300_rb2d_format format)
{
   return (unsigned)format < R300_RB2D_FORMAT_COUNT ? &format_table[format]
                                                    : NULL;
}

const char *
r300_rb2d_fill_refusal_name(enum r300_rb2d_fill_refusal r)
{
   static const char *const names[R300_RB2D_FILL_REFUSAL_COUNT] = {
      "ok",
      "plan names no rectangle",
      "plan names more rectangles than the emitter carries",
      "surface format outside the emitted formats",
      "surface pitch outside the 64-byte grid",
      "surface pitch is zero",
      "surface pitch above the pitch field",
      "surface offset outside the 1 KiB grid",
      "surface offset above the offset field",
      "surface extent is zero",
      "surface pitch is narrower than its width",
      "rectangle has a zero extent",
      "rectangle coordinate above its field",
      "rectangle reaches past the 2D scissor",
      "rectangle reaches outside the surface",
   };
   return (unsigned)r < R300_RB2D_FILL_REFUSAL_COUNT ? names[r] : NULL;
}

enum r300_rb2d_fill_refusal
r300_rb2d_fill_plan_check(const struct r300_rb2d_fill_plan *plan)
{
   if (plan == NULL || plan->rects == NULL || plan->rect_count == 0)
      return R300_RB2D_FILL_REFUSE_NO_RECTS;
   if (plan->rect_count > R300_RB2D_FILL_MAX_RECTS)
      return R300_RB2D_FILL_REFUSE_TOO_MANY_RECTS;

   const struct r300_rb2d_surface *s = &plan->surface;

   const struct rb2d_format_info *format = format_info(s->format);

   if (format == NULL)
      return R300_RB2D_FILL_REFUSE_FORMAT;
   if (s->pitch_bytes == 0)
      return R300_RB2D_FILL_REFUSE_PITCH_ZERO;
   if (s->pitch_bytes % R300_RB2D_PITCH_GRANULARITY != 0)
      return R300_RB2D_FILL_REFUSE_PITCH_GRID;
   if (s->pitch_bytes / R300_RB2D_PITCH_GRANULARITY > RB2D_PITCH_FIELD_MAX)
      return R300_RB2D_FILL_REFUSE_PITCH_FIELD;
   if (s->base_offset_bytes % R300_RB2D_OFFSET_GRANULARITY != 0)
      return R300_RB2D_FILL_REFUSE_OFFSET_GRID;
   if (s->base_offset_bytes / R300_RB2D_OFFSET_GRANULARITY >
       RB2D_OFFSET_FIELD_MAX)
      return R300_RB2D_FILL_REFUSE_OFFSET_FIELD;
   if (s->width_pixels == 0 || s->height_pixels == 0)
      return R300_RB2D_FILL_REFUSE_EXTENT_ZERO;
   /* A row narrower than its own width would make a rectangle inside the
    * surface land in the next row's bytes.  The stride comes from the
    * format's own descriptor, so a format added to the table states its
    * bytes per pixel rather than inheriting ARGB8888's four. */
   if (s->pitch_bytes / format->bytes_per_pixel < s->width_pixels)
      return R300_RB2D_FILL_REFUSE_PITCH_BELOW_WIDTH;

   for (uint32_t i = 0; i < plan->rect_count; i++) {
      const struct r300_rb2d_fill_rect *r = &plan->rects[i];
      if (r->width == 0 || r->height == 0)
         return R300_RB2D_FILL_REFUSE_RECT_EMPTY;
      if (r->x > RB2D_COORD_FIELD_MAX || r->y > RB2D_COORD_FIELD_MAX ||
          r->width > RB2D_COORD_FIELD_MAX || r->height > RB2D_COORD_FIELD_MAX)
         return R300_RB2D_FILL_REFUSE_RECT_FIELD;
      /* Both sums are bounded by twice the field maximum, so neither
       * overflows the 32-bit comparisons below. */
      /* The emitter opens the scissor to its own maximum, and a rectangle
       * reaching past it is clipped by that same register: the fill lands
       * short and the stream reports success, so the plan refuses the
       * rectangle here where the shortfall is still nameable. */
      if (r->x + r->width > R300_RB2D_MAX_COORD_REACH ||
          r->y + r->height > R300_RB2D_MAX_COORD_REACH)
         return R300_RB2D_FILL_REFUSE_RECT_BEYOND_SCISSOR;
      if (r->x + r->width > s->width_pixels ||
          r->y + r->height > s->height_pixels)
         return R300_RB2D_FILL_REFUSE_RECT_OUTSIDE;
   }

   return R300_RB2D_FILL_OK;
}

static void
write_reloc(struct r300_pm4_builder *b, struct r300_rb2d_fill_ib *out)
{
   if (b->error != 0)
      return;
   if (out->reloc_site_count >= R300_RB2D_FILL_SLOT_COUNT) {
      b->error = -EINVAL;
      return;
   }

   const uint32_t index =
      r300_pm4_reloc_nop(b, RB2D_RELOC_PAYLOAD(R300_RB2D_FILL_SLOT_DST));
   if (index == R300_PM4_NO_INDEX)
      return;

   out->reloc_sites[out->reloc_site_count++] =
      (struct r300_rb2d_fill_reloc_site){
         .ib_index = index,
         .slot = R300_RB2D_FILL_SLOT_DST,
      };
}

int
r300_rb2d_fill_emit_into(const struct r300_rb2d_fill_plan *plan,
                         uint32_t *words, uint32_t capacity,
                         struct r300_rb2d_fill_ib *out)
{
   struct r300_pm4_builder b;

   if (words == NULL || out == NULL)
      return -EINVAL;
   if (r300_rb2d_fill_plan_check(plan) != R300_RB2D_FILL_OK)
      return -EINVAL;

   memset(out, 0, sizeof(*out));
   out->ib = words;
   r300_pm4_builder_init(&b, words, capacity);

   /* The plan check above admitted the format, so its descriptor resolves. */
   const struct r300_rb2d_surface *s = &plan->surface;
   const struct rb2d_format_info *format = format_info(s->format);

   r300_pm4_reg(&b, RADEON_DST_PITCH_OFFSET,
                ((s->pitch_bytes / R300_RB2D_PITCH_GRANULARITY) << 22) |
                   (s->base_offset_bytes / R300_RB2D_OFFSET_GRANULARITY));
   write_reloc(&b, out);

   r300_pm4_reg(&b, RADEON_SC_TOP_LEFT, 0);
   r300_pm4_reg(&b, RADEON_SC_BOTTOM_RIGHT,
                RB2D_SCISSOR_MAX | (RB2D_SCISSOR_MAX << 16));
   r300_pm4_reg(&b, RADEON_DEFAULT_SC_BOTTOM_RIGHT,
                RB2D_SCISSOR_MAX | (RB2D_SCISSOR_MAX << 16));

   r300_pm4_reg(&b, RADEON_DP_GUI_MASTER_CNTL,
                RADEON_GMC_DST_PITCH_OFFSET_CNTL |
                   RADEON_GMC_BRUSH_SOLID_COLOR |
                   (format->code << 8) | RADEON_ROP3_P |
                   RADEON_GMC_CLR_CMP_CNTL_DIS | RADEON_GMC_WR_MSK_DIS);
   r300_pm4_reg(&b, RADEON_DP_CNTL,
                RADEON_DST_X_LEFT_TO_RIGHT | RADEON_DST_Y_TOP_TO_BOTTOM);
   r300_pm4_reg(&b, RADEON_DP_WRITE_MSK, plan->write_mask);

   for (uint32_t i = 0; i < plan->rect_count; i++) {
      const struct r300_rb2d_fill_rect *r = &plan->rects[i];
      r300_pm4_reg(&b, RADEON_DP_BRUSH_FRGD_CLR, r->value);
      r300_pm4_reg(&b, RADEON_DST_Y_X, (r->y << 16) | r->x);
      /* Writing DST_WIDTH_HEIGHT launches the fill.  Width occupies the
       * high half and height the low half: r100_copy_blit ends its blit
       * block with cur_pages | (stride_pixels << 16), row count low and
       * pixel stride high.  The retained cell's 1x1 rectangles carry the
       * same value either way, so the kernel's packing is what fixes the
       * order for a rectangle that is not square.
       */
      r300_pm4_reg(&b, RADEON_DST_WIDTH_HEIGHT,
                   (r->width << 16) | r->height);
   }

   /* The publication r100_copy_blit ends with: flush the 2D destination
    * cache, then hold the stream until the 2D engine and host path drain.
    */
   r300_pm4_reg(&b, RADEON_DSTCACHE_CTLSTAT, RADEON_RB2D_DC_FLUSH_ALL);
   r300_pm4_reg(&b, RADEON_WAIT_UNTIL,
                RADEON_WAIT_2D_IDLECLEAN | RADEON_WAIT_HOST_IDLECLEAN |
                   RADEON_WAIT_DMA_GUI_IDLE);

   const int r = r300_pm4_builder_finish(&b, &out->ib_size_dwords);
   if (r != 0) {
      memset(out, 0, sizeof(*out));
      return r;
   }
   return 0;
}

int
r300_rb2d_fill_validate_reloc_sites(const struct r300_rb2d_fill_ib *ib)
{
   if (ib == NULL || ib->ib == NULL)
      return -EINVAL;
   if (ib->reloc_site_count != R300_RB2D_FILL_SLOT_COUNT)
      return -EINVAL;

   const struct r300_rb2d_fill_reloc_site *site = &ib->reloc_sites[0];

   if (site->slot != R300_RB2D_FILL_SLOT_DST)
      return -EINVAL;
   if (site->ib_index >= ib->ib_size_dwords)
      return -EINVAL;
   if (ib->ib[site->ib_index] != RB2D_RELOC_PAYLOAD(site->slot))
      return -EINVAL;
   /* The site indexes the NOP's payload, so the header sits one dword
    * before it.
    */
   if (site->ib_index == 0 ||
       ib->ib[site->ib_index - 1] != (0xC0000000u | R300_PM4_PACKET3_NOP))
      return -EINVAL;
   return 0;
}
