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
#include <string.h>

/* CP_PACKET0 and CP_PACKET3 as the builder encodes them: a type-0 header
 * carrying the register dword address and the payload count minus one,
 * and a type-3 header carrying the opcode and the same count field.
 */
#define PACKET0_HEADER(reg, count) \
   ((((count) - 1u) << 16) | ((reg) >> 2))
#define PACKET3_HEADER(op, count) \
   (0xC0000000u | (op) | (((count) - 1u) << 16))

/* The 64x64 Z24 reference level on one RS480 pipe at a requested
 * compression block.  Macrotiling is the 8x8 conjunct, so a macrotiled
 * level asked for the larger block takes it and covers the level in four
 * dwords; every other combination lands at 4x4 and sixteen.
 */
static struct r300_zmask_layout
rs480_layout_at(bool macrotile, enum r300_zmask_compression block)
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
   assert(r300_zmask_layout_compute_at_block(&params, block, &layout) == 0);
   assert(layout.fits_zmask_ram);
   assert(layout.dwords > 0);
   assert(layout.zcomp8x8 == (macrotile && block == R300_ZCOMP_8X8));
   return layout;
}

/* The level at whatever block it would take on its own, which is what
 * r300_zmask_layout_compute answers. */
static struct r300_zmask_layout
rs480_layout(bool macrotile)
{
   struct r300_zmask_layout layout = rs480_layout_at(macrotile, R300_ZCOMP_8X8);
   struct r300_zmask_layout unrequested;
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
   assert(r300_zmask_layout_compute(&params, &unrequested) == 0);
   assert(memcmp(&layout, &unrequested, sizeof(layout)) == 0);
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
check_next_word(const struct r300_zmask_clear_plan *plan, uint32_t *index,
                uint32_t expected)
{
   assert(*index < plan->dword_count);
   assert(plan->words[*index] == expected);
   (*index)++;
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
   check_next_word(&plan, &i, PACKET0_HEADER(R300_ZB_ZMASK_OFFSET, 2));
   check_next_word(&plan, &i, 0u);
   check_next_word(&plan, &i, layout->stride_in_pixels);
   check_next_word(&plan, &i, PACKET0_HEADER(R300_ZB_ZMASK_WRINDEX, 1));
   check_next_word(&plan, &i, 0u);
   check_next_word(&plan, &i, PACKET0_HEADER(R300_ZB_ZMASK_RDINDEX, 1));
   check_next_word(&plan, &i, 0u);
   check_next_word(&plan, &i, PACKET0_HEADER(R300_GB_Z_PEQ_CONFIG, 1));
   check_next_word(
      &plan, &i,
      layout->zcomp8x8 ? R300_GB_Z_PEQ_CONFIG_Z_PEQ_SIZE_8_8
                       : R300_GB_Z_PEQ_CONFIG_Z_PEQ_SIZE_4_4);
   check_next_word(&plan, &i, PACKET0_HEADER(R300_ZB_BW_CNTL, 1));
   check_next_word(&plan, &i, expected_bw_cntl);
   check_next_word(&plan, &i,
                   PACKET3_HEADER(R300_PACKET3_3D_CLEAR_ZMASK, 3));
   check_next_word(&plan, &i, 0u);
   check_next_word(&plan, &i, layout->dwords);
   check_next_word(&plan, &i, 0u);
   assert(i == plan.dword_count);

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
check_binding_refusal_preserves_output(
   const struct r300_zmask_layout *layout,
   const struct r300_zmask_clear_plan *sentinel)
{
   for (int stage = R300_ZMASK_CLEAR_STAGE_BIND_CLEAR;
        stage <= R300_ZMASK_CLEAR_STAGE_FAST_FILL; stage++) {
      struct r300_zmask_clear_plan plan = *sentinel;
      assert(r300_zmask_clear_plan_build(
                (enum r300_zmask_clear_stage)stage, layout, &plan) == -EINVAL);
      assert(memcmp(&plan, sentinel, sizeof(plan)) == 0);
   }
}

static void
check_refusals(void)
{
   struct r300_zmask_layout layout = rs480_layout(true);
   const struct r300_zmask_clear_plan sentinel = {
      .words = {0x12345678u, 0x87654321u},
      .dword_count = 0xabcdef01u,
      .requires_hyperz_ownership = true,
      .writes_hyperz_registers = true,
   };
   struct r300_zmask_clear_plan plan = sentinel;

   assert(r300_zmask_clear_plan_build(R300_ZMASK_CLEAR_STAGE_BIND_CLEAR,
                                      NULL, &plan) == -EINVAL);
   assert(memcmp(&plan, &sentinel, sizeof(plan)) == 0);
   assert(r300_zmask_clear_plan_build(R300_ZMASK_CLEAR_STAGE_BIND_CLEAR,
                                      &layout, NULL) == -EINVAL);
   assert(r300_zmask_clear_plan_build((enum r300_zmask_clear_stage)99,
                                      &layout, &plan) == -EINVAL);
   assert(memcmp(&plan, &sentinel, sizeof(plan)) == 0);

   /* A level whose ZMASK does not fit binds nothing: a zero pitch and a
    * zero clear count would describe a bind of no RAM. */
   struct r300_zmask_layout unfit = layout;
   unfit.fits_zmask_ram = false;
   check_binding_refusal_preserves_output(&unfit, &sentinel);

   unfit = layout;
   unfit.dwords = 0;
   check_binding_refusal_preserves_output(&unfit, &sentinel);

   unfit = layout;
   unfit.stride_in_pixels = 0;
   check_binding_refusal_preserves_output(&unfit, &sentinel);

   /* Stages A and B carry no layout dependency. */
   assert(r300_zmask_clear_plan_build(R300_ZMASK_CLEAR_STAGE_DEPTH_ONLY,
                                      &unfit, &plan) == 0);
   assert(r300_zmask_clear_plan_build(R300_ZMASK_CLEAR_STAGE_OWNERSHIP_ONLY,
                                      &unfit, &plan) == 0);

   const struct r300_zmask_layout valid = rs480_layout_at(
      true, r300_zmask_clear_stage_block(R300_ZMASK_CLEAR_STAGE_BIND_CLEAR));
   struct r300_zmask_layout forged = valid;
   forged.zmask_ram_dwords = 0u;
   check_binding_refusal_preserves_output(&forged, &sentinel);

   forged = valid;
   forged.zmask_ram_dwords = forged.dwords - 1u;
   check_binding_refusal_preserves_output(&forged, &sentinel);

   forged = valid;
   forged.zmask_ram_dwords = forged.dwords;
   assert(r300_zmask_clear_plan_build(R300_ZMASK_CLEAR_STAGE_BIND_CLEAR,
                                      &forged, &plan) == 0);
}


/* GB_Z_PEQ_CONFIG and the 3D_CLEAR_ZMASK count both follow the block
 * size, so a layout resolved at one block and a stage that programs the
 * other describe different surfaces: a 4x4 plane-equation register over
 * a clear sized for 8x8 leaves three quarters of the level's metadata
 * untouched.  The builder refuses the pair rather than emitting it.
 */
static void
check_block_disagreement_refused(void)
{
   struct r300_zmask_clear_plan plan;
   const struct r300_zmask_layout eight = rs480_layout_at(true, R300_ZCOMP_8X8);
   const struct r300_zmask_layout four = rs480_layout_at(true, R300_ZCOMP_4X4);
   assert(eight.zcomp8x8 && !four.zcomp8x8);
   assert(eight.dwords == 4 && four.dwords == 16);

   /* Every stage in this ladder programs 4x4, so the 8x8 layout is the
    * known-bad at both binding stages. */
   assert(r300_zmask_clear_plan_build(R300_ZMASK_CLEAR_STAGE_BIND_CLEAR,
                                      &eight, &plan) == -EINVAL);
   assert(r300_zmask_clear_plan_build(R300_ZMASK_CLEAR_STAGE_FAST_FILL,
                                      &eight, &plan) == -EINVAL);
   assert(r300_zmask_clear_plan_build(R300_ZMASK_CLEAR_STAGE_BIND_CLEAR,
                                      &four, &plan) == 0);
   assert(r300_zmask_clear_plan_build(R300_ZMASK_CLEAR_STAGE_FAST_FILL,
                                      &four, &plan) == 0);

   /* The refusal reads the layout's block, not its dword count: an 8x8
    * layout carrying a 4x4 count is still an 8x8 plane-equation
    * register, and a 4x4 layout carrying an 8x8 count still programs
    * 4x4 over a coverage that names four dwords.  Both are refused or
    * admitted by the block alone, which is what makes the register and
    * the count travel together. */
   struct r300_zmask_layout mismatched = eight;
   mismatched.dwords = four.dwords;
   const struct r300_zmask_clear_plan sentinel = {
      .words = {0x12345678u, 0x87654321u},
      .dword_count = 0xabcdef01u,
      .requires_hyperz_ownership = true,
      .writes_hyperz_registers = true,
   };
   check_binding_refusal_preserves_output(&mismatched, &sentinel);
   assert(r300_zmask_clear_plan_build(R300_ZMASK_CLEAR_STAGE_BIND_CLEAR,
                                      &mismatched, &plan) == -EINVAL);

   /* A block outside the enumeration refuses at the layout. */
   struct r300_zmask_layout layout;
   const struct r300_zmask_layout_params params = {
      .stride_in_pixels = 64,
      .height = 64,
      .depth_bytes_per_pixel = 4,
      .is_depth_or_stencil = true,
      .microtile = true,
      .macrotile = true,
      .num_samples = 1,
      .zcomp8x8_capable = true,
      .pipes = 1,
      .zmask_ram_dwords_per_pipe = RV3xx_ZMASK_SIZE,
   };
   assert(r300_zmask_layout_compute_at_block(
             &params, (enum r300_zmask_compression)6, &layout) == -EINVAL);
}

static void
check_fast_clear_value_plan(const struct r300_zmask_layout *layout)
{
   struct r300_zmask_clear_plan bind_plan;
   assert(r300_zmask_clear_plan_build(R300_ZMASK_CLEAR_STAGE_FAST_FILL, layout,
                                      &bind_plan) == 0);
   struct r300_zmask_clear_plan plan;
   assert(r300_zmask_fast_clear_plan_build(
             &r300_zb_depth_surface_rs485m_z24_macrotiled_logical, layout,
             0x400000u, 0xa5u, &plan) == 0);
   assert(plan.dword_count == bind_plan.dword_count + 2u);
   assert(plan.words[0] == PACKET0_HEADER(R300_ZB_DEPTHCLEARVALUE, 1u));
   assert(plan.words[1] == 0x400000a5u);
   assert(memcmp(plan.words + 2u, bind_plan.words,
                 bind_plan.dword_count * sizeof(bind_plan.words[0])) == 0);
   assert(plan.requires_hyperz_ownership);
   assert(plan.writes_hyperz_registers);
   struct r300_zb_hyperz_site site;
   assert(judge(&plan, R300_ZB_HYPERZ_UNOWNED, &site) ==
          R300_ZB_HYPERZ_REFUSE_OWNERSHIP);
   assert(judge(&plan, R300_ZB_HYPERZ_OWNED, &site) == R300_ZB_HYPERZ_ADMIT);

   const struct r300_zmask_clear_plan sentinel = {
      .words = {0x12345678u},
      .dword_count = 0x87654321u,
      .requires_hyperz_ownership = false,
      .writes_hyperz_registers = false,
   };
   plan = sentinel;
   assert(r300_zmask_fast_clear_plan_build(
             &r300_zb_depth_surface_rs485m_z24_macrotiled_logical, layout,
             0x1000000u, 0xa5u, &plan) == -EINVAL);
   assert(memcmp(&plan, &sentinel, sizeof(plan)) == 0);
}

int
main(void)
{
   assert(r300_zb_hyperz_rows_self_check() == 0);

   /* The level's own decision, which the 8x8 conjunct macrotiling
    * satisfies: four dwords at 8x8 against sixteen at 4x4. */
   const struct r300_zmask_layout layout = rs480_layout(true);
   const struct r300_zmask_layout layout_4x4 = rs480_layout(false);
   assert(layout.dwords == 4 && layout_4x4.dwords == 16);

   /* Every stage in the ladder leaves read and write compression off, so
    * every stage programs 4x4 plane equations and takes a layout
    * resolved at that block.  The macrotiled level asked for its
    * stage's block reports the 4x4 coverage, which is the same sixteen
    * dwords the untiled level takes and not the four its own 8x8
    * decision would have named. */
   for (int s = R300_ZMASK_CLEAR_STAGE_DEPTH_ONLY;
        s <= R300_ZMASK_CLEAR_STAGE_FAST_FILL; s++)
      assert(r300_zmask_clear_stage_block((enum r300_zmask_clear_stage)s) ==
             R300_ZCOMP_4X4);
   const struct r300_zmask_layout bound = rs480_layout_at(
      true, r300_zmask_clear_stage_block(R300_ZMASK_CLEAR_STAGE_BIND_CLEAR));
   assert(!bound.zcomp8x8);
   assert(bound.dwords == 16);
   assert(bound.dwords == layout_4x4.dwords);
   assert(bound.stride_in_pixels == layout_4x4.stride_in_pixels);

   check_empty_stage(R300_ZMASK_CLEAR_STAGE_DEPTH_ONLY, false, &layout);
   check_empty_stage(R300_ZMASK_CLEAR_STAGE_OWNERSHIP_ONLY, true, &layout);
   for (int mode = 0; mode <= 1; mode++) {
      const struct r300_zmask_layout *l = mode == 0 ? &bound : &layout_4x4;
      check_bind_stage(R300_ZMASK_CLEAR_STAGE_BIND_CLEAR, 0u, l);
      check_bind_stage(R300_ZMASK_CLEAR_STAGE_FAST_FILL,
                       R300_FAST_FILL_ENABLE, l);
   }
   check_refusals();
   check_block_disagreement_refused();
   check_z16_linear_cell_gap();
   check_fast_clear_value_plan(&bound);

   printf("r300 zmask clear plan: four stages, ZMASK %u dwords at pitch %u\n",
          bound.dwords, bound.stride_in_pixels);
   for (int s = R300_ZMASK_CLEAR_STAGE_DEPTH_ONLY;
        s <= R300_ZMASK_CLEAR_STAGE_FAST_FILL; s++) {
      struct r300_zmask_clear_plan plan;
      assert(r300_zmask_clear_plan_build((enum r300_zmask_clear_stage)s,
                                         &bound, &plan) == 0);
      printf("  %-38s %2u dwords, ownership %s\n",
             r300_zmask_clear_stage_name((enum r300_zmask_clear_stage)s),
             plan.dword_count,
             plan.requires_hyperz_ownership ? "required" : "absent");
   }
   return 0;
}
