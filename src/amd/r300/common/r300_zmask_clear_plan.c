/* SPDX-License-Identifier: MIT */

#include "r300_zmask_clear_plan.h"

#include "r300_pm4_builder.h"
#include "r300_reg.h"

#include <errno.h>
#include <string.h>

/* r300_emit_zmask_clear writes OUT_CS_PKT3(3D_CLEAR_ZMASK, 2) followed
 * by zero, the level's ZMASK dword count, and zero: a start index, the
 * dwords to clear, and the value written into each.
 */
#define ZMASK_CLEAR_PAYLOAD_DWORDS 3u

static void
emit_bind_and_clear(struct r300_pm4_builder *b,
                    const struct r300_zmask_layout *layout,
                    uint32_t zb_bw_cntl)
{
   /* ZB_ZMASK_OFFSET and ZB_ZMASK_PITCH are adjacent, so the bind is one
    * register run.  r300_emit_fb_state writes offset zero and the
    * surface's zmask_stride_in_pixels as the pitch, which places the
    * level at the base of the ZMASK RAM.
    */
   const uint32_t bind[] = {0u, layout->stride_in_pixels};
   r300_pm4_packet0(b, R300_ZB_ZMASK_OFFSET, bind, 2u);

   /* The autoincrementing ZMASK RAM access indices.  The kernel's HyperZ
    * table carries no row for either, so both admit on their own; the
    * plan writes them so the RAM window the clear fills starts where the
    * bind placed it rather than where a predecessor left the index.
    */
   r300_pm4_reg(b, R300_ZB_ZMASK_WRINDEX, 0u);
   r300_pm4_reg(b, R300_ZB_ZMASK_RDINDEX, 0u);

   /* The ZMASK tile size the layout decided.  r300_update_hyperz sets
    * Z_PEQ_SIZE_8_8 from the level's zcomp8x8 flag before the ZB_BW_CNTL
    * enables, and the tile size scales the RAM the surface consumes: the
    * 64x64 reference level clears four dwords at 8x8 and sixteen at 4x4,
    * so a retained setting from a predecessor describes a different
    * surface than the one the clear covers.  The 4x4 case writes its
    * value explicitly rather than leaving the register alone.
    */
   r300_pm4_reg(b, R300_GB_Z_PEQ_CONFIG,
                layout->zcomp8x8 ? R300_GB_Z_PEQ_CONFIG_Z_PEQ_SIZE_8_8
                                 : R300_GB_Z_PEQ_CONFIG_Z_PEQ_SIZE_4_4);

   r300_pm4_reg(b, R300_ZB_BW_CNTL, zb_bw_cntl);

   const uint32_t clear[ZMASK_CLEAR_PAYLOAD_DWORDS] = {0u, layout->dwords,
                                                       0u};
   r300_pm4_packet3(b, R300_PACKET3_3D_CLEAR_ZMASK, clear,
                    ZMASK_CLEAR_PAYLOAD_DWORDS);
}

int
r300_zmask_clear_plan_build(enum r300_zmask_clear_stage stage,
                            const struct r300_zmask_layout *layout,
                            struct r300_zmask_clear_plan *out)
{
   if (layout == NULL || out == NULL)
      return -EINVAL;

   memset(out, 0, sizeof(*out));

   struct r300_pm4_builder b;
   r300_pm4_builder_init(&b, out->words, R300_ZMASK_CLEAR_PLAN_MAX_DWORDS);

   switch (stage) {
   case R300_ZMASK_CLEAR_STAGE_DEPTH_ONLY:
      break;
   case R300_ZMASK_CLEAR_STAGE_OWNERSHIP_ONLY:
      out->requires_hyperz_ownership = true;
      break;
   case R300_ZMASK_CLEAR_STAGE_BIND_CLEAR:
   case R300_ZMASK_CLEAR_STAGE_FAST_FILL:
      if (!layout->fits_zmask_ram || layout->dwords == 0u ||
          layout->stride_in_pixels == 0u)
         return -EINVAL;
      out->requires_hyperz_ownership = true;
      out->writes_hyperz_registers = true;
      /* SC_HYPERZ stays unwritten: the scan converter's HiZ bit belongs
       * to the HiZ stage past this ladder.
       */
      emit_bind_and_clear(&b, layout,
                          stage == R300_ZMASK_CLEAR_STAGE_FAST_FILL
                             ? R300_FAST_FILL_ENABLE
                             : 0u);
      break;
   default:
      return -EINVAL;
   }

   const int err = r300_pm4_builder_finish(&b, &out->dword_count);
   if (err != 0) {
      memset(out, 0, sizeof(*out));
      return err;
   }
   return 0;
}

const char *
r300_zmask_clear_stage_name(enum r300_zmask_clear_stage stage)
{
   switch (stage) {
   case R300_ZMASK_CLEAR_STAGE_DEPTH_ONLY:
      return "depth only";
   case R300_ZMASK_CLEAR_STAGE_OWNERSHIP_ONLY:
      return "ownership only";
   case R300_ZMASK_CLEAR_STAGE_BIND_CLEAR:
      return "ZMASK bind and clear";
   case R300_ZMASK_CLEAR_STAGE_FAST_FILL:
      return "ZMASK bind and clear with fast fill";
   }
   return NULL;
}
