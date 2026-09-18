/* SPDX-License-Identifier: MIT */

#include "r300_zmask_materialize_plan.h"
#include "r300_reg.h"

#include <assert.h>
#include <string.h>

int main(void)
{
   /* A level whose own block is 4x4, so the prefix programs the smaller
    * plane equations and the metadata covers sixteen dwords. */
   const struct r300_zmask_layout layout = {
      .stride_in_pixels = 64u, .dwords = 16u,
      .fits_zmask_ram = true, .zmask_ram_dwords = 5120u,
      .zcomp8x8 = false, .admits_zcomp8x8 = false,
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

   struct r300_zmask_materialize_plan read_plan;
   assert(r300_zmask_fast_clear_read_plan(
             &r300_zb_depth_surface_rs485m_z24_macrotiled_logical,
             &layout, 0x800000u, 0x5au, &read_plan) == 0);
   assert(read_plan.begin_dword_count == 9u);
   assert(read_plan.dword_count == 15u);
   assert(read_plan.clear_word == 0x8000005au);
   assert(read_plan.words[5] == CP_PACKET0(R300_GB_Z_PEQ_CONFIG, 0));
   assert(read_plan.words[6] == R300_GB_Z_PEQ_CONFIG_Z_PEQ_SIZE_4_4);
   assert(read_plan.words[7] == CP_PACKET0(R300_ZB_BW_CNTL, 0));
   assert(read_plan.words[8] ==
          (R300_FAST_FILL_ENABLE | R300_RD_COMP_ENABLE));
   assert(read_plan.words[11] == CP_PACKET0(R300_ZB_BW_CNTL, 0));
   assert(read_plan.words[12] == 0u);

   /* The macrotiled single-sample level the RS485M lifecycle binds: its
    * own block is 8x8, four metadata dwords cover the 64x64 surface, and
    * the prefix programs the equations the pipe decodes there.  The
    * suffix restores 4x4 with ZB_BW_CNTL cleared, which is the
    * compression-disabled configuration the R5xx acceleration guide
    * pins. */
   const struct r300_zmask_layout admitted = {
      .stride_in_pixels = 64u, .dwords = 4u,
      .fits_zmask_ram = true, .zmask_ram_dwords = 5120u,
      .zcomp8x8 = true, .admits_zcomp8x8 = true,
   };
   struct r300_zmask_materialize_plan admitted_plan;
   assert(r300_zmask_fast_clear_read_plan(
             &r300_zb_depth_surface_rs485m_z24_macrotiled_logical,
             &admitted, 0x800000u, 0x3cu, &admitted_plan) == 0);
   assert(admitted_plan.clear_word == 0x8000003cu);
   assert(admitted_plan.words[5] == CP_PACKET0(R300_GB_Z_PEQ_CONFIG, 0));
   assert(admitted_plan.words[6] == R300_GB_Z_PEQ_CONFIG_Z_PEQ_SIZE_8_8);
   assert(admitted_plan.words[8] ==
          (R300_FAST_FILL_ENABLE | R300_RD_COMP_ENABLE));
   assert(admitted_plan.words[13] == CP_PACKET0(R300_GB_Z_PEQ_CONFIG, 0));
   assert(admitted_plan.words[14] == R300_GB_Z_PEQ_CONFIG_Z_PEQ_SIZE_4_4);

   /* Known-bad: the level admits 8x8 and the layout was pinned to 4x4.
    * The prefix sets RD_COMP_ENABLE, so the pipe decodes the level's own
    * block while the metadata covers a quarter of the tiles and the rest
    * answer out of depth memory -- the pairing RS485M executed.  Every
    * refusal leaves the caller's output untouched. */
   const struct r300_zmask_layout pinned_below = {
      .stride_in_pixels = 64u, .dwords = 16u,
      .fits_zmask_ram = true, .zmask_ram_dwords = 5120u,
      .zcomp8x8 = false, .admits_zcomp8x8 = true,
   };
   assert(r300_zmask_layout_below_admitted_block(&pinned_below));
   assert(!r300_zmask_layout_below_admitted_block(&admitted));
   assert(!r300_zmask_layout_below_admitted_block(&layout));
   struct r300_zmask_materialize_plan refused = admitted_plan;
   assert(r300_zmask_materialize_prefix(
             &r300_zb_depth_surface_rs485m_z24_macrotiled_logical,
             &pinned_below, 0x800000u, 0x3cu, &refused) != 0);
   assert(memcmp(&refused, &admitted_plan, sizeof(refused)) == 0);
   assert(r300_zmask_fast_clear_read_plan(
             &r300_zb_depth_surface_rs485m_z24_macrotiled_logical,
             &pinned_below, 0x800000u, 0x3cu, &refused) != 0);
   assert(memcmp(&refused, &admitted_plan, sizeof(refused)) == 0);
   return 0;
}
