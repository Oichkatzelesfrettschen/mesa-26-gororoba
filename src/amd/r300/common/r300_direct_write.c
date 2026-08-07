/* SPDX-License-Identifier: MIT */

#include "r300_direct_write.h"
#include "r300_pm4_builder.h"
#include "r300_tcl_bypass_triangle.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* 2D engine registers, spelled as drivers/gpu/drm/radeon/radeon_reg.h
 * spells them.  Every register except DST_PITCH_OFFSET sits on the
 * reg_srcs/r300 safe list and passes the CS parser unchecked;
 * DST_PITCH_OFFSET is the r300_packet0_check case r100_reloc_pitch_offset
 * handles, which consumes the cell's one relocation.
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

/* DST_PITCH_OFFSET packs the pitch in 64-byte units above a 1 KiB-granular
 * offset (r100_copy_blit: (pitch << 22) | (offset >> 10)), so the 256-byte
 * row carries pitch 4 and the BO-base destination carries offset 0;
 * per-pixel addressing rides in DST_Y_X.
 */
#define R300_DIRECT_WRITE_PITCH_64B \
   (R300_TRIANGLE_TARGET_PITCH_PIXELS * 4u / 64u)

/* Four dwords per drm_radeon_cs_reloc entry, so a slot's payload indexes
 * the relocation chunk at four times the slot.
 */
#define R300_DIRECT_WRITE_RELOC_PAYLOAD(slot) ((slot) * 4)

#define R300_DIRECT_WRITE_MAX_DWORDS 64

static void
write_reloc(struct r300_pm4_builder *b, struct r300_direct_write_ib *out,
            uint32_t slot)
{
   if (b->error != 0)
      return;
   if (slot >= R300_DIRECT_WRITE_SLOT_COUNT ||
       out->reloc_site_count >= R300_DIRECT_WRITE_SLOT_COUNT) {
      b->error = -EINVAL;
      return;
   }

   const uint32_t index =
      r300_pm4_reloc_nop(b, R300_DIRECT_WRITE_RELOC_PAYLOAD(slot));
   if (index == R300_PM4_NO_INDEX)
      return;

   out->reloc_sites[out->reloc_site_count++] =
      (struct r300_direct_write_reloc_site){
         .ib_index = index,
         .slot = slot,
      };
}

static void
emit_fill(struct r300_pm4_builder *b, uint32_t value, uint32_t x, uint32_t y)
{
   r300_pm4_reg(b, RADEON_DP_BRUSH_FRGD_CLR, value);
   r300_pm4_reg(b, RADEON_DST_Y_X, (y << 16) | x);
   /* Writing DST_WIDTH_HEIGHT launches the fill. */
   r300_pm4_reg(b, RADEON_DST_WIDTH_HEIGHT, (1u << 16) | 1u);
}

static int
emit_cell(struct r300_pm4_builder *b, struct r300_direct_write_ib *out)
{
   r300_pm4_reg(b, RADEON_DST_PITCH_OFFSET,
                R300_DIRECT_WRITE_PITCH_64B << 22);
   write_reloc(b, out, R300_DIRECT_WRITE_SLOT_COLOR);

   /* The 2D scissor is established rather than inherited, so a predecessor
    * scissor cannot clip the probe pixels; 0x1fff is each field's maximum.
    */
   r300_pm4_reg(b, RADEON_SC_TOP_LEFT, 0);
   r300_pm4_reg(b, RADEON_SC_BOTTOM_RIGHT, 0x1fffu | (0x1fffu << 16));
   r300_pm4_reg(b, RADEON_DEFAULT_SC_BOTTOM_RIGHT,
                0x1fffu | (0x1fffu << 16));

   r300_pm4_reg(b, RADEON_DP_GUI_MASTER_CNTL,
                RADEON_GMC_DST_PITCH_OFFSET_CNTL |
                RADEON_GMC_BRUSH_SOLID_COLOR |
                (RADEON_COLOR_FORMAT_ARGB8888 << 8) |
                RADEON_ROP3_P |
                RADEON_GMC_CLR_CMP_CNTL_DIS |
                RADEON_GMC_WR_MSK_DIS);
   r300_pm4_reg(b, RADEON_DP_CNTL,
                RADEON_DST_X_LEFT_TO_RIGHT | RADEON_DST_Y_TOP_TO_BOTTOM);
   r300_pm4_reg(b, RADEON_DP_WRITE_MSK, 0xffffffffu);

   emit_fill(b, R300_DIRECT_WRITE_A_VALUE,
             R300_DIRECT_WRITE_A_X, R300_DIRECT_WRITE_A_Y);
   emit_fill(b, R300_DIRECT_WRITE_B_VALUE,
             R300_DIRECT_WRITE_B_X, R300_DIRECT_WRITE_B_Y);

   /* The publication r100_copy_blit ends with: flush the 2D destination
    * cache, then hold the stream until the 2D engine and host path drain.
    */
   r300_pm4_reg(b, RADEON_DSTCACHE_CTLSTAT, RADEON_RB2D_DC_FLUSH_ALL);
   r300_pm4_reg(b, RADEON_WAIT_UNTIL,
                RADEON_WAIT_2D_IDLECLEAN |
                RADEON_WAIT_HOST_IDLECLEAN |
                RADEON_WAIT_DMA_GUI_IDLE);

   return r300_pm4_builder_finish(b, &out->ib_size_dwords);
}

int
r300_direct_write_emit_into(uint32_t *words, uint32_t capacity,
                            struct r300_direct_write_ib *out)
{
   struct r300_pm4_builder b;
   int r;

   if (!words || !out)
      return -EINVAL;

   memset(out, 0, sizeof(*out));
   out->ib = words;
   r300_pm4_builder_init(&b, words, capacity);
   r = emit_cell(&b, out);
   if (r != 0) {
      memset(out, 0, sizeof(*out));
      return r;
   }
   return 0;
}

int
r300_direct_write_emit(struct r300_direct_write_ib *out)
{
   uint32_t *words;
   int r;

   if (!out)
      return -EINVAL;
   words = calloc(R300_DIRECT_WRITE_MAX_DWORDS, sizeof(*words));
   if (!words)
      return -ENOMEM;
   r = r300_direct_write_emit_into(words, R300_DIRECT_WRITE_MAX_DWORDS, out);
   if (r != 0) {
      free(words);
      return r;
   }
   out->owns_ib = true;
   return 0;
}

void
r300_direct_write_release(struct r300_direct_write_ib *ib)
{
   if (!ib)
      return;
   if (ib->owns_ib)
      free(ib->ib);
   memset(ib, 0, sizeof(*ib));
}

int
r300_direct_write_validate_reloc_sites(const struct r300_direct_write_ib *ib)
{
   if (!ib || !ib->ib)
      return -EINVAL;
   if (ib->reloc_site_count != R300_DIRECT_WRITE_SLOT_COUNT)
      return -EINVAL;

   const struct r300_direct_write_reloc_site *site = &ib->reloc_sites[0];

   if (site->slot != R300_DIRECT_WRITE_SLOT_COLOR)
      return -EINVAL;
   if (site->ib_index >= ib->ib_size_dwords)
      return -EINVAL;
   if (ib->ib[site->ib_index] !=
       R300_DIRECT_WRITE_RELOC_PAYLOAD(site->slot))
      return -EINVAL;
   /* The site indexes the NOP's payload, so the header sits one dword
    * before it.
    */
   if (site->ib_index == 0 ||
       ib->ib[site->ib_index - 1] != (0xC0000000u | R300_PM4_PACKET3_NOP))
      return -EINVAL;
   return 0;
}

void
r300_direct_write_oracle(const uint32_t *pixels, uint32_t size_bytes,
                         struct r300_direct_write_verdict *verdict)
{
   const uint32_t pitch = R300_TRIANGLE_TARGET_PITCH_PIXELS;
   /* The oracle covers target plus canary row (allocation rows) only;
    * an allocation tail past the canary row carries no expectation and
    * stays unscanned.
    */
   const uint32_t covered = R300_TRIANGLE_ALLOCATION_ROWS * pitch;
   const uint32_t total_avail = size_bytes / 4;
   const uint32_t total = total_avail < covered ? total_avail : covered;
   const uint32_t target = R300_TRIANGLE_TARGET_HEIGHT * pitch;
   const uint32_t a = R300_DIRECT_WRITE_A_Y * pitch + R300_DIRECT_WRITE_A_X;
   const uint32_t b = R300_DIRECT_WRITE_B_Y * pitch + R300_DIRECT_WRITE_B_X;

   memset(verdict, 0, sizeof(*verdict));
   if (!pixels || total < R300_TRIANGLE_ALLOCATION_ROWS * pitch)
      return;

   verdict->value_a_pass = pixels[a] == R300_DIRECT_WRITE_A_VALUE;
   verdict->value_b_pass = pixels[b] == R300_DIRECT_WRITE_B_VALUE;
   verdict->sentinel_pass = true;
   verdict->canary_pass = true;
   for (uint32_t i = 0; i < total; i++) {
      if (pixels[i] != R300_TRIANGLE_COLOR_SENTINEL) {
         verdict->executed = true;
         if (i >= target)
            verdict->canary_pass = false;
         else if (i != a && i != b)
            verdict->sentinel_pass = false;
      }
   }
}
