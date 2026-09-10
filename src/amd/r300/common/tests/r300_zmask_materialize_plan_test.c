/* SPDX-License-Identifier: MIT */

#include "r300_zmask_materialize_plan.h"
#include "r300_reg.h"

#include <assert.h>
#include <string.h>

int main(void)
{
   const struct r300_zmask_layout layout = {
      .stride_in_pixels = 64u, .dwords = 16u,
      .fits_zmask_ram = true, .zmask_ram_dwords = 5120u,
      .zcomp8x8 = false,
   };
   struct r300_zmask_materialize_plan plan;
   assert(r300_zmask_materialize_prefix(
             &r300_zb_depth_surface_rs485m_z24_macrotiled_logical,
             &layout, 0x123456u, 0xa5u, &plan) == 0);
   assert(plan.dword_count == 9u);
   assert(plan.begin_dword_count == 9u);
   assert(plan.words[0] == CP_PACKET0(R300_ZB_DEPTHCLEARVALUE, 0));
   assert(plan.words[2] == CP_PACKET0(R300_ZB_ZMASK_OFFSET, 1));
   assert(plan.words[5] == CP_PACKET0(R300_GB_Z_PEQ_CONFIG, 0));
   assert(plan.words[7] == CP_PACKET0(R300_ZB_BW_CNTL, 0));
   struct r300_zmask_materialize_plan before = plan;
   assert(r300_zmask_materialize_prefix(&r300_zb_depth_surface_rs485m_z24_macrotiled_logical,
                                        &layout, 0x1000000u, 0u, &plan) != 0);
   assert(memcmp(&before, &plan, sizeof(plan)) == 0);
   assert(r300_zmask_materialize_suffix(&plan) == 0);
   assert(plan.dword_count == 15u);
   assert(plan.begin_dword_count == 9u);
   assert(r300_zmask_materialize_end_words(&plan) == &plan.words[9]);
   assert(r300_zmask_materialize_end_dword_count(&plan) == 6u);
   assert(plan.words[9] == CP_PACKET0(R300_ZB_ZCACHE_CTLSTAT, 0));
   assert(plan.words[11] == CP_PACKET0(R300_ZB_BW_CNTL, 0));
   assert(plan.words[13] == CP_PACKET0(R300_GB_Z_PEQ_CONFIG, 0));
   return 0;
}
