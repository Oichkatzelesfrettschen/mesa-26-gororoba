/* SPDX-License-Identifier: MIT */

/* The four-stage ZMASK ladder: the words each stage appends, the
 * ownership each stage demands, and the admission verdict the kernel's
 * HyperZ table gives that stream with and without ownership.
 */

#include "r300_zb_depth_control_cell.h"
#include "r300_zb_hyperz_admission.h"
#include "r300_zmask_clear_plan.h"
#include "r300_zmask_layout.h"

#include "r300_chipset.h"
#include "r300_reg.h"

/* The checks are the test; a release build keeps them. */
#undef NDEBUG
#include <assert.h>
#include <errno.h>
#include <stdio.h>

/* CP_PACKET0 and CP_PACKET3 as the builder encodes them: a type-0 header
 * carrying the register dword address and the payload count minus one,
 * and a type-3 header carrying the opcode and the same count field.
 */
#define PACKET0_HEADER(reg, count) \
   ((((count) - 1u) << 16) | ((reg) >> 2))
#define PACKET3_HEADER(op, count) \
   (0xC0000000u | (op) | (((count) - 1u) << 16))

/* The 64x64 Z24 reference level on one RS480 pipe.  Macrotiling is the
 * 8x8 compression conjunct, so the two calls differ in tile size alone:
 * 8x8 covers the level in four dwords and 4x4 in sixteen.
 */
static struct r300_zmask_layout
rs480_layout(bool macrotile)
{
   const struct r300_zmask_layout_params params = {
      .stride_in_pixels = 64,
      .height = 64,
      .depth_bytes_per_pixel = 4,
      .is_depth_or_stencil = true,
      .microtile = true,
      .macrotile = macrotile,
      .num_samples = 1,
      .zcomp8x8_capable = true,
      .pipes = 1,
      .zmask_ram_dwords_per_pipe = RV3xx_ZMASK_SIZE,
   };
   struct r300_zmask_layout layout;
   assert(r300_zmask_layout_compute(&params, &layout) == 0);
   assert(layout.fits_zmask_ram);
   assert(layout.dwords > 0);
   assert(layout.zcomp8x8 == macrotile);
   return layout;
}

/* The depth control cell as r300_zb_depth_control_cell.c and
 * r300_zb_depth_state.c build it: a 16-bit integer Z surface at two
 * bytes per pixel with DEPTHMACROTILE_DISABLE and DEPTHMICROTILE_LINEAR.
 * ZMASK covers 32-bit depth on a microtiled level alone, so this cell
 * compresses nothing and no bind stage can be built from it.
 */
static void
check_z16_linear_cell_gap(void)
{
   const struct r300_zmask_layout_params params = {
      .stride_in_pixels = R300_ZB_DEPTH_CONTROL_PITCH_PIXELS,
      .height = R300_ZB_DEPTH_CONTROL_TARGET_HEIGHT,
      .depth_bytes_per_pixel = R300_ZB_DEPTH_CONTROL_DEPTH_CPP,
      .is_depth_or_stencil = true,
      .microtile = false,
      .macrotile = false,
      .num_samples = 1,
      .zcomp8x8_capable = true,
      .pipes = 1,
      .zmask_ram_dwords_per_pipe = RV3xx_ZMASK_SIZE,
   };
   struct r300_zmask_layout layout;
   assert(r300_zmask_layout_compute(&params, &layout) == 0);
   assert(layout.dwords == 0);
   assert(layout.stride_in_pixels == 0);
   assert(!layout.fits_zmask_ram);
   assert(!layout.zcomp8x8);

   struct r300_zmask_clear_plan plan;
   assert(r300_zmask_clear_plan_build(R300_ZMASK_CLEAR_STAGE_BIND_CLEAR,
                                      &layout, &plan) == -EINVAL);
   assert(r300_zmask_clear_plan_build(R300_ZMASK_CLEAR_STAGE_FAST_FILL,
                                      &layout, &plan) == -EINVAL);
   /* The two stages that append nothing build from any layout. */
   assert(r300_zmask_clear_plan_build(R300_ZMASK_CLEAR_STAGE_DEPTH_ONLY,
                                      &layout, &plan) == 0);
   assert(r300_zmask_clear_plan_build(R300_ZMASK_CLEAR_STAGE_OWNERSHIP_ONLY,
                                      &layout, &plan) == 0);
}

static enum r300_zb_hyperz_verdict
judge(const struct r300_zmask_clear_plan *plan,
      enum r300_zb_hyperz_ownership ownership,
      struct r300_zb_hyperz_site *site)
{
   return r300_zb_hyperz_admit_stream(plan->words, plan->dword_count,
                                      ownership, site);
}

static void
check_empty_stage(enum r300_zmask_clear_stage stage, bool wants_ownership,
                  const struct r300_zmask_layout *layout)
{
   struct r300_zmask_clear_plan plan;
   assert(r300_zmask_clear_plan_build(stage, layout, &plan) == 0);
   assert(plan.dword_count == 0);
   assert(!plan.writes_hyperz_registers);
   assert(plan.requires_hyperz_ownership == wants_ownership);
   struct r300_zb_hyperz_site site;
   assert(judge(&plan, R300_ZB_HYPERZ_UNOWNED, &site) ==
          R300_ZB_HYPERZ_ADMIT);
   assert(judge(&plan, R300_ZB_HYPERZ_OWNED, &site) == R300_ZB_HYPERZ_ADMIT);
}

static void
check_bind_stage(enum r300_zmask_clear_stage stage, uint32_t expected_bw_cntl,
                 const struct r300_zmask_layout *layout)
{
   struct r300_zmask_clear_plan plan;
   assert(r300_zmask_clear_plan_build(stage, layout, &plan) == 0);
   assert(plan.requires_hyperz_ownership);
   assert(plan.writes_hyperz_registers);

   /* Bind run of three, two two-dword index writes, the two-dword
    * GB_Z_PEQ_CONFIG and ZB_BW_CNTL writes, and the four-dword clear
    * packet.
    */
   assert(plan.dword_count == 15);

   uint32_t i = 0;
   assert(plan.words[i++] == PACKET0_HEADER(R300_ZB_ZMASK_OFFSET, 2));
   assert(plan.words[i++] == 0);
   assert(plan.words[i++] == layout->stride_in_pixels);
   assert(plan.words[i++] == PACKET0_HEADER(R300_ZB_ZMASK_WRINDEX, 1));
   assert(plan.words[i++] == 0);
   assert(plan.words[i++] == PACKET0_HEADER(R300_ZB_ZMASK_RDINDEX, 1));
   assert(plan.words[i++] == 0);
   assert(plan.words[i++] == PACKET0_HEADER(R300_GB_Z_PEQ_CONFIG, 1));
   assert(plan.words[i++] ==
          (layout->zcomp8x8 ? R300_GB_Z_PEQ_CONFIG_Z_PEQ_SIZE_8_8
                            : R300_GB_Z_PEQ_CONFIG_Z_PEQ_SIZE_4_4));
   assert(plan.words[i++] == PACKET0_HEADER(R300_ZB_BW_CNTL, 1));
   assert(plan.words[i++] == expected_bw_cntl);
   assert(plan.words[i++] ==
          PACKET3_HEADER(R300_PACKET3_3D_CLEAR_ZMASK, 3));
   assert(plan.words[i++] == 0);
   assert(plan.words[i] == layout->dwords);
   assert(plan.words[i + 1] == 0);

   /* HiZ, RD_COMP and WR_COMP stay off in both bind stages. */
   assert((expected_bw_cntl &
           (R300_HIZ_ENABLE | R300_RD_COMP_ENABLE | R300_WR_COMP_ENABLE)) == 0);

   struct r300_zb_hyperz_site site;
   assert(judge(&plan, R300_ZB_HYPERZ_OWNED, &site) == R300_ZB_HYPERZ_ADMIT);

   /* ZB_ZMASK_OFFSET carries zero against a full gated mask and admits,
    * so the refusal lands on the pitch write: the third dword of the
    * bind run, which the walker reports at its payload index.  It
    * precedes GB_Z_PEQ_CONFIG, whose 4x4 value of zero admits unowned
    * on its own while its 8x8 value refuses.
    */
   assert(judge(&plan, R300_ZB_HYPERZ_UNOWNED, &site) ==
          R300_ZB_HYPERZ_REFUSE_OWNERSHIP);
   assert(site.reg_or_opcode == R300_ZB_ZMASK_PITCH);
   assert(site.ib_index == 2);
   assert(site.value == layout->stride_in_pixels);

   /* The tile-size write judged on its own: the 8x8 value is nonzero and
    * refuses without ownership, the 4x4 value is zero and admits, so the
    * 4x4 stream's refusal rests on the pitch and the clear packet alone.
    */
   assert(r300_zb_hyperz_admit_register(
             R300_GB_Z_PEQ_CONFIG, R300_GB_Z_PEQ_CONFIG_Z_PEQ_SIZE_8_8,
             R300_ZB_HYPERZ_UNOWNED, NULL) ==
          R300_ZB_HYPERZ_REFUSE_OWNERSHIP);
   assert(r300_zb_hyperz_admit_register(
             R300_GB_Z_PEQ_CONFIG, R300_GB_Z_PEQ_CONFIG_Z_PEQ_SIZE_4_4,
             R300_ZB_HYPERZ_UNOWNED, NULL) == R300_ZB_HYPERZ_ADMIT);
   assert(r300_zb_hyperz_admit_register(
             R300_GB_Z_PEQ_CONFIG, R300_GB_Z_PEQ_CONFIG_Z_PEQ_SIZE_8_8,
             R300_ZB_HYPERZ_OWNED, NULL) == R300_ZB_HYPERZ_ADMIT);
}

static void
check_refusals(void)
{
   struct r300_zmask_layout layout = rs480_layout(true);
   struct r300_zmask_clear_plan plan;

   assert(r300_zmask_clear_plan_build(R300_ZMASK_CLEAR_STAGE_BIND_CLEAR,
                                      NULL, &plan) == -EINVAL);
   assert(r300_zmask_clear_plan_build(R300_ZMASK_CLEAR_STAGE_BIND_CLEAR,
                                      &layout, NULL) == -EINVAL);
   assert(r300_zmask_clear_plan_build((enum r300_zmask_clear_stage)99,
                                      &layout, &plan) == -EINVAL);

   /* A level whose ZMASK does not fit binds nothing: a zero pitch and a
    * zero clear count would describe a bind of no RAM. */
   struct r300_zmask_layout unfit = layout;
   unfit.fits_zmask_ram = false;
   unfit.dwords = 0;
   unfit.stride_in_pixels = 0;
   assert(r300_zmask_clear_plan_build(R300_ZMASK_CLEAR_STAGE_BIND_CLEAR,
                                      &unfit, &plan) == -EINVAL);
   assert(r300_zmask_clear_plan_build(R300_ZMASK_CLEAR_STAGE_FAST_FILL,
                                      &unfit, &plan) == -EINVAL);
   /* Stages A and B carry no layout dependency. */
   assert(r300_zmask_clear_plan_build(R300_ZMASK_CLEAR_STAGE_DEPTH_ONLY,
                                      &unfit, &plan) == 0);
   assert(r300_zmask_clear_plan_build(R300_ZMASK_CLEAR_STAGE_OWNERSHIP_ONLY,
                                      &unfit, &plan) == 0);
}

int
main(void)
{
   assert(r300_zb_hyperz_rows_self_check() == 0);

   const struct r300_zmask_layout layout = rs480_layout(true);
   const struct r300_zmask_layout layout_4x4 = rs480_layout(false);
   assert(layout.dwords == 4 && layout_4x4.dwords == 16);

   check_empty_stage(R300_ZMASK_CLEAR_STAGE_DEPTH_ONLY, false, &layout);
   check_empty_stage(R300_ZMASK_CLEAR_STAGE_OWNERSHIP_ONLY, true, &layout);
   for (int mode = 0; mode <= 1; mode++) {
      const struct r300_zmask_layout *l = mode == 0 ? &layout : &layout_4x4;
      check_bind_stage(R300_ZMASK_CLEAR_STAGE_BIND_CLEAR, 0u, l);
      check_bind_stage(R300_ZMASK_CLEAR_STAGE_FAST_FILL,
                       R300_FAST_FILL_ENABLE, l);
   }
   check_refusals();
   check_z16_linear_cell_gap();

   printf("r300 zmask clear plan: four stages, ZMASK %u dwords at pitch %u\n",
          layout.dwords, layout.stride_in_pixels);
   for (int s = R300_ZMASK_CLEAR_STAGE_DEPTH_ONLY;
        s <= R300_ZMASK_CLEAR_STAGE_FAST_FILL; s++) {
      struct r300_zmask_clear_plan plan;
      assert(r300_zmask_clear_plan_build((enum r300_zmask_clear_stage)s,
                                         &layout, &plan) == 0);
      printf("  %-38s %2u dwords, ownership %s\n",
             r300_zmask_clear_stage_name((enum r300_zmask_clear_stage)s),
             plan.dword_count,
             plan.requires_hyperz_ownership ? "required" : "absent");
   }
   return 0;
}
