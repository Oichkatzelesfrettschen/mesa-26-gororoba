/* SPDX-License-Identifier: MIT */

#include "r300_direct_write.h"
#include "r300_pm4_builder.h"
#include "r300_rb2d_fill.h"
#include "r300_tcl_bypass_triangle.h"

#include "util/macros.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* The cell is one instance of the general RB2D solid-fill plan: a 64x64
 * linear ARGB8888 surface at the triangle target's pitch, filled at two
 * probe pixels.  The plan owns the register contract and the surface rules;
 * this file owns the fixed geometry and the output oracle that reads it,
 * so the retained stream is reproduced rather than re-derived.
 */
static const struct r300_rb2d_fill_rect probe_rects[] = {
   { R300_DIRECT_WRITE_A_X, R300_DIRECT_WRITE_A_Y, 1u, 1u,
     R300_DIRECT_WRITE_A_VALUE },
   { R300_DIRECT_WRITE_B_X, R300_DIRECT_WRITE_B_Y, 1u, 1u,
     R300_DIRECT_WRITE_B_VALUE },
};

static const struct r300_rb2d_fill_plan cell_plan = {
   .surface = {
      .base_offset_bytes = 0u,
      .pitch_bytes = R300_TRIANGLE_TARGET_PITCH_PIXELS * 4u,
      .width_pixels = R300_TRIANGLE_TARGET_WIDTH,
      .height_pixels = R300_TRIANGLE_TARGET_HEIGHT,
      .format = R300_RB2D_FORMAT_ARGB8888,
   },
   .write_mask = 0xffffffffu,
   .rects = probe_rects,
   .rect_count = ARRAY_SIZE(probe_rects),
};

#define R300_DIRECT_WRITE_MAX_DWORDS 64

int
r300_direct_write_emit_into(uint32_t *words, uint32_t capacity,
                            struct r300_direct_write_ib *out)
{
   struct r300_rb2d_fill_ib fill;
   int r;

   if (!words || !out)
      return -EINVAL;

   memset(out, 0, sizeof(*out));
   r = r300_rb2d_fill_emit_into(&cell_plan, words, capacity, &fill);
   if (r != 0)
      return r;

   out->ib = fill.ib;
   out->ib_size_dwords = fill.ib_size_dwords;
   out->reloc_site_count = fill.reloc_site_count;
   for (uint32_t i = 0; i < fill.reloc_site_count; i++) {
      out->reloc_sites[i] = (struct r300_direct_write_reloc_site){
         .ib_index = fill.reloc_sites[i].ib_index,
         .slot = fill.reloc_sites[i].slot,
      };
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
   if (!ib)
      return -EINVAL;

   struct r300_rb2d_fill_ib fill = {
      .ib = ib->ib,
      .ib_size_dwords = ib->ib_size_dwords,
      .reloc_site_count = ib->reloc_site_count,
   };
   for (uint32_t i = 0;
        i < ib->reloc_site_count && i < R300_RB2D_FILL_SLOT_COUNT; i++) {
      fill.reloc_sites[i] = (struct r300_rb2d_fill_reloc_site){
         .ib_index = ib->reloc_sites[i].ib_index,
         .slot = ib->reloc_sites[i].slot,
      };
   }
   return r300_rb2d_fill_validate_reloc_sites(&fill);
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
