/* SPDX-License-Identifier: MIT */

#include "r300_zmask_materialize_plan.h"

#include "r300_pm4_builder.h"
#include "r300_reg.h"

#include <errno.h>
#include <string.h>

static int
validate_inputs(const struct r300_zb_depth_surface *surface,
                const struct r300_zmask_layout *layout)
{
   if (surface == NULL || layout == NULL ||
       r300_zb_depth_surface_check(surface) != 0 ||
       surface->bytes_per_pixel != 4u ||
       !surface->microtile || !surface->macrotile ||
       !layout->fits_zmask_ram || layout->dwords == 0u ||
       layout->stride_in_pixels == 0u || layout->zmask_ram_dwords == 0u ||
       layout->dwords > layout->zmask_ram_dwords || layout->zcomp8x8)
      return -EINVAL;
   return 0;
}

int
r300_zmask_materialize_prefix(
   const struct r300_zb_depth_surface *surface,
   const struct r300_zmask_layout *layout, uint32_t depth_code,
   uint32_t stencil, struct r300_zmask_materialize_plan *out)
{
   if (out == NULL || validate_inputs(surface, layout) != 0)
      return -EINVAL;

   struct r300_zmask_materialize_plan plan;
   memset(&plan, 0, sizeof(plan));
   if (r300_zb_depth_pack(surface, depth_code, stencil, &plan.clear_word) != 0)
      return -EINVAL;

   struct r300_pm4_builder builder;
   r300_pm4_builder_init(&builder, plan.words,
                         R300_ZMASK_MATERIALIZE_PLAN_MAX_DWORDS);
   r300_pm4_reg(&builder, R300_ZB_DEPTHCLEARVALUE, plan.clear_word);
   r300_pm4_packet0(&builder, R300_ZB_ZMASK_OFFSET,
                    (uint32_t[]){0u, layout->stride_in_pixels}, 2u);
   r300_pm4_reg(&builder, R300_GB_Z_PEQ_CONFIG,
                R300_GB_Z_PEQ_CONFIG_Z_PEQ_SIZE_4_4);
   r300_pm4_reg(&builder, R300_ZB_BW_CNTL,
                R300_FAST_FILL_ENABLE | R300_RD_COMP_ENABLE);
   if (r300_pm4_builder_finish(&builder, &plan.dword_count) != 0)
      return -ENOSPC;
   plan.begin_dword_count = plan.dword_count;
   *out = plan;
   return 0;
}

int
r300_zmask_materialize_suffix(struct r300_zmask_materialize_plan *out)
{
   if (out == NULL)
      return -EINVAL;
   struct r300_zmask_materialize_plan plan = *out;
   if (plan.begin_dword_count == 0u ||
       plan.begin_dword_count != plan.dword_count ||
       plan.dword_count > R300_ZMASK_MATERIALIZE_PLAN_MAX_DWORDS)
      return -EINVAL;
   struct r300_pm4_builder builder;
   r300_pm4_builder_init(&builder, plan.words,
                         R300_ZMASK_MATERIALIZE_PLAN_MAX_DWORDS);
   builder.count = plan.dword_count;
   r300_pm4_reg(&builder, R300_ZB_ZCACHE_CTLSTAT,
                R300_ZB_ZCACHE_CTLSTAT_ZC_FLUSH_FLUSH_AND_FREE |
                   R300_ZB_ZCACHE_CTLSTAT_ZC_FREE_FREE);
   r300_pm4_reg(&builder, R300_ZB_BW_CNTL, 0u);
   r300_pm4_reg(&builder, R300_GB_Z_PEQ_CONFIG,
                R300_GB_Z_PEQ_CONFIG_Z_PEQ_SIZE_4_4);
   if (r300_pm4_builder_finish(&builder, &plan.dword_count) != 0)
      return -ENOSPC;
   *out = plan;
   return 0;
}
