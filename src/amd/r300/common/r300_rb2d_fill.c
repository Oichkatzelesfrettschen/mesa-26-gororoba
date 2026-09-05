/* SPDX-License-Identifier: MIT */

#include "r300_rb2d_fill.h"
#include "r300_pm4_builder.h"
#include "radeon_legacy_2d_reg.h"

#include <errno.h>
#include <string.h>

/* DST_Y_X and DST_WIDTH_HEIGHT each pack two 16-bit fields, so a plan
 * value wider than its field has no representation in the word.  The pitch
 * and offset field bounds live in the header beside the grids they count,
 * because a caller sizes a surface against them. */
#define RB2D_COORD_FIELD_MAX 0xffffu

/* The 2D scissor is established at its own field maximum rather than at the
 * surface extent, so a predecessor scissor cannot clip the plan's
 * rectangles.  That maximum, and why it also bounds a rectangle's far
 * edge, is stated once at R300_RB2D_SAFE_EXCLUSIVE_END in r300_rb2d_fill.h. */
#define RB2D_SCISSOR_MAX R300_RB2D_SAFE_EXCLUSIVE_END

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
   [R300_RB2D_FORMAT_RGB565] = { RADEON_COLOR_FORMAT_RGB565, 2u },
};

/* NULL outside the table, so a format the plan never admitted resolves to no
 * descriptor rather than to the first row's stride. */
static const struct rb2d_format_info *
format_info(enum r300_rb2d_format format)
{
   return (unsigned)format < R300_RB2D_FORMAT_COUNT ? &format_table[format]
                                                    : NULL;
}

uint32_t
r300_rb2d_format_bytes_per_pixel(enum r300_rb2d_format format)
{
   const struct rb2d_format_info *info = format_info(format);
   return info != NULL ? info->bytes_per_pixel : 0u;
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
   if (s->pitch_bytes / R300_RB2D_PITCH_GRANULARITY > R300_RB2D_MAX_PITCH_UNITS)
      return R300_RB2D_FILL_REFUSE_PITCH_FIELD;
   if (s->base_offset_bytes % R300_RB2D_OFFSET_GRANULARITY != 0)
      return R300_RB2D_FILL_REFUSE_OFFSET_GRID;
   if (s->base_offset_bytes / R300_RB2D_OFFSET_GRANULARITY >
       R300_RB2D_MAX_OFFSET_UNITS)
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
      /* The rectangle stays inside the scissor the emitter opens; the
       * bound and its cost live at R300_RB2D_SAFE_EXCLUSIVE_END. */
      if (r->x + r->width > R300_RB2D_SAFE_EXCLUSIVE_END ||
          r->y + r->height > R300_RB2D_SAFE_EXCLUSIVE_END)
         return R300_RB2D_FILL_REFUSE_RECT_BEYOND_SCISSOR;
      if (r->x + r->width > s->width_pixels ||
          r->y + r->height > s->height_pixels)
         return R300_RB2D_FILL_REFUSE_RECT_OUTSIDE;
   }

   return R300_RB2D_FILL_OK;
}

void
r300_rb2d_emitter_init(struct r300_rb2d_emitter *e, uint32_t *words,
                       uint32_t capacity, struct r300_rb2d_fill_ib *out)
{
   memset(e, 0, sizeof(*e));
   e->out = out;
   if (out != NULL) {
      memset(out, 0, sizeof(*out));
      out->ib = words;
   }
   r300_pm4_builder_init(&e->builder, words, capacity);
   if (out == NULL)
      e->builder.error = -EINVAL;
}

void
r300_rb2d_emit_surface_state(struct r300_rb2d_emitter *e,
                             const struct r300_rb2d_surface *surface,
                             struct r300_rb2d_relocation relocation)
{
   struct r300_pm4_builder *b = &e->builder;

   if (b->error != 0)
      return;
   if (surface == NULL || format_info(surface->format) == NULL ||
       surface->pitch_bytes == 0u ||
       surface->pitch_bytes % R300_RB2D_PITCH_GRANULARITY != 0u ||
       surface->pitch_bytes / R300_RB2D_PITCH_GRANULARITY >
          R300_RB2D_MAX_PITCH_UNITS ||
       surface->base_offset_bytes % R300_RB2D_OFFSET_GRANULARITY != 0u ||
       relocation.slot >= R300_RB2D_FILL_SLOT_COUNT ||
       e->out->reloc_site_count >= R300_RB2D_FILL_SLOT_COUNT) {
      b->error = -EINVAL;
      return;
   }

   r300_pm4_reg(b, RADEON_DST_PITCH_OFFSET,
                ((surface->pitch_bytes / R300_RB2D_PITCH_GRANULARITY) << 22) |
                   (surface->base_offset_bytes / R300_RB2D_OFFSET_GRANULARITY));

   const uint32_t index =
      r300_pm4_reloc_nop(b, RB2D_RELOC_PAYLOAD(relocation.slot));
   if (index == R300_PM4_NO_INDEX)
      return;
   e->out->reloc_sites[e->out->reloc_site_count++] =
      (struct r300_rb2d_fill_reloc_site){
         .ib_index = index,
         .slot = relocation.slot,
      };

   /* A new destination invalidates the format and origin established on
    * the previous one; a launch needs both re-established here. */
   e->dst_epoch++;
   e->format_epoch = 0u;
   e->origin_epoch = 0u;
}

void
r300_rb2d_emit_common_state(struct r300_rb2d_emitter *e,
                            const struct r300_rb2d_surface *surface,
                            uint32_t write_mask)
{
   struct r300_pm4_builder *b = &e->builder;

   if (b->error != 0)
      return;
   const struct rb2d_format_info *format =
      surface != NULL ? format_info(surface->format) : NULL;
   if (format == NULL || e->dst_epoch == 0u) {
      b->error = -EINVAL;
      return;
   }

   r300_pm4_reg(b, RADEON_SC_TOP_LEFT, 0);
   r300_pm4_reg(b, RADEON_SC_BOTTOM_RIGHT,
                RB2D_SCISSOR_MAX | (RB2D_SCISSOR_MAX << 16));
   r300_pm4_reg(b, RADEON_DEFAULT_SC_BOTTOM_RIGHT,
                RB2D_SCISSOR_MAX | (RB2D_SCISSOR_MAX << 16));

   r300_pm4_reg(b, RADEON_DP_GUI_MASTER_CNTL,
                RADEON_GMC_DST_PITCH_OFFSET_CNTL |
                   RADEON_GMC_BRUSH_SOLID_COLOR |
                   (format->code << 8) | RADEON_ROP3_P |
                   RADEON_GMC_CLR_CMP_CNTL_DIS | RADEON_GMC_WR_MSK_DIS);
   r300_pm4_reg(b, RADEON_DP_CNTL,
                RADEON_DST_X_LEFT_TO_RIGHT | RADEON_DST_Y_TOP_TO_BOTTOM);
   r300_pm4_reg(b, RADEON_DP_WRITE_MSK, write_mask);
   e->format_epoch++;
}

void
r300_rb2d_emit_rect(struct r300_rb2d_emitter *e,
                    const struct r300_rb2d_fill_rect *r)
{
   struct r300_pm4_builder *b = &e->builder;

   if (b->error != 0)
      return;
   if (r == NULL || r->width == 0u || r->height == 0u ||
       r->x > RB2D_COORD_FIELD_MAX || r->y > RB2D_COORD_FIELD_MAX ||
       r->width > RB2D_COORD_FIELD_MAX || r->height > RB2D_COORD_FIELD_MAX ||
       e->dst_epoch == 0u || e->format_epoch == 0u) {
      b->error = -EINVAL;
      return;
   }

   r300_pm4_reg(b, RADEON_DP_BRUSH_FRGD_CLR, r->value);
   r300_pm4_reg(b, RADEON_DST_Y_X, (r->y << 16) | r->x);
   e->origin_epoch++;
   /* Writing DST_WIDTH_HEIGHT launches the fill.  Width occupies the
    * high half and height the low half: r100_copy_blit ends its blit
    * block with cur_pages | (stride_pixels << 16), row count low and
    * pixel stride high.  The retained cell's 1x1 rectangles carry the
    * same value either way, so the kernel's packing is what fixes the
    * order for a rectangle that is not square.
    */
   r300_pm4_reg(b, RADEON_DST_WIDTH_HEIGHT, (r->width << 16) | r->height);
}

void
r300_rb2d_emit_epilogue(struct r300_rb2d_emitter *e)
{
   struct r300_pm4_builder *b = &e->builder;

   if (b->error != 0)
      return;
   /* The publication r100_copy_blit ends with: flush the 2D destination
    * cache, then hold the stream until the 2D engine and host path drain.
    */
   r300_pm4_reg(b, RADEON_DSTCACHE_CTLSTAT, RADEON_RB2D_DC_FLUSH_ALL);
   r300_pm4_reg(b, RADEON_WAIT_UNTIL,
                RADEON_WAIT_2D_IDLECLEAN | RADEON_WAIT_HOST_IDLECLEAN |
                   RADEON_WAIT_DMA_GUI_IDLE);
}

int
r300_rb2d_emitter_finish(struct r300_rb2d_emitter *e)
{
   if (e->out == NULL)
      return -EINVAL;
   const int r =
      r300_pm4_builder_finish(&e->builder, &e->out->ib_size_dwords);
   if (r != 0)
      memset(e->out, 0, sizeof(*e->out));
   return r;
}

int
r300_rb2d_fill_emit_into(const struct r300_rb2d_fill_plan *plan,
                         uint32_t *words, uint32_t capacity,
                         struct r300_rb2d_fill_ib *out)
{
   struct r300_rb2d_emitter e;

   if (words == NULL || out == NULL)
      return -EINVAL;
   if (r300_rb2d_fill_plan_check(plan) != R300_RB2D_FILL_OK)
      return -EINVAL;

   r300_rb2d_emitter_init(&e, words, capacity, out);
   r300_rb2d_emit_surface_state(
      &e, &plan->surface,
      (struct r300_rb2d_relocation){ .slot = R300_RB2D_FILL_SLOT_DST });
   r300_rb2d_emit_common_state(&e, &plan->surface, plan->write_mask);
   for (uint32_t i = 0; i < plan->rect_count; i++)
      r300_rb2d_emit_rect(&e, &plan->rects[i]);
   r300_rb2d_emit_epilogue(&e);
   return r300_rb2d_emitter_finish(&e);
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
