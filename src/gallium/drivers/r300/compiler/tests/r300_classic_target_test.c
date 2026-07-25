/*
 * SPDX-License-Identifier: MIT
 */

#include "classic/r300_classic_target.h"
#include "radeon_code.h"

#include <stdio.h>
#include <stdbool.h>

/* Diff the classic target descriptor field by field against the radeon_code.h
 * authority and against the limits r300_fs.c installs into
 * radeon_compiler.Base for the same (is_r400, is_r500) selection:
 * max_temp_regs = is_r500 ? 128 : (r400_full_temps ? 64 : 32),
 * max_constants = is_r500 ? 256 : 32,
 * max_alu_insts = (is_r500 || r400_envelope) ? 512 : 64,
 * max_tex_insts = (is_r500 || r400_envelope) ? 512 : 32.
 * A descriptor drifting from either source fails here before any classic
 * compiler pass can consume the wrong budget. */

static int failures;

#define CHECK_EQ(desc, field, expect)                                        \
   do {                                                                      \
      if ((desc)->field != (expect)) {                                       \
         fprintf(stderr, "FAIL: %s.%s = %u, expected %u\n", #desc, #field,   \
                 (unsigned)(desc)->field, (unsigned)(expect));               \
         failures++;                                                         \
      }                                                                      \
   } while (0)

static void
check_r300(void)
{
   const struct r300_classic_target *t = r300_classic_target_get(false, false);
   CHECK_EQ(t, pfs_class, R300_CLASSIC_PFS_R300);
   CHECK_EQ(t, max_alu_insts, R300_PFS_MAX_ALU_INST);
   CHECK_EQ(t, max_alu_insts, 64);
   CHECK_EQ(t, max_tex_insts, R300_PFS_MAX_TEX_INST);
   CHECK_EQ(t, max_tex_insts, 32);
   CHECK_EQ(t, max_tex_indirections, R300_PFS_MAX_TEX_INDIRECT);
   CHECK_EQ(t, max_temp_regs, R300_PFS_NUM_TEMP_REGS);
   CHECK_EQ(t, max_temp_regs, 32);
   CHECK_EQ(t, max_const_regs, R300_PFS_NUM_CONST_REGS);
   CHECK_EQ(t, max_const_regs, 32);
   CHECK_EQ(t, has_flow_control, false);
   CHECK_EQ(t, has_half_swizzles, true);
   CHECK_EQ(t, has_presub, true);
   CHECK_EQ(t, has_omod, true);
   CHECK_EQ(t, exact_int_bits, 17);
}

static void
check_r400(void)
{
   const struct r300_classic_target *t = r300_classic_target_get(true, false);
   CHECK_EQ(t, pfs_class, R300_CLASSIC_PFS_R400);
   CHECK_EQ(t, max_alu_insts, R400_PFS_MAX_ALU_INST);
   CHECK_EQ(t, max_alu_insts, 512);
   CHECK_EQ(t, max_tex_insts, R400_PFS_MAX_TEX_INST);
   CHECK_EQ(t, max_tex_insts, 512);
   CHECK_EQ(t, max_tex_indirections, R300_PFS_MAX_TEX_INDIRECT);
   CHECK_EQ(t, max_temp_regs, 64);
   CHECK_EQ(t, max_const_regs, R300_PFS_NUM_CONST_REGS);
   CHECK_EQ(t, has_flow_control, false);
   CHECK_EQ(t, exact_int_bits, 17);
}

static void
check_r500(void)
{
   const struct r300_classic_target *t = r300_classic_target_get(false, true);
   CHECK_EQ(t, pfs_class, R300_CLASSIC_PFS_R500);
   CHECK_EQ(t, max_alu_insts, R500_PFS_MAX_INST);
   CHECK_EQ(t, max_alu_insts, 512);
   CHECK_EQ(t, max_tex_insts, R500_PFS_MAX_INST);
   CHECK_EQ(t, max_temp_regs, R500_PFS_NUM_TEMP_REGS);
   CHECK_EQ(t, max_temp_regs, 128);
   CHECK_EQ(t, max_const_regs, R500_PFS_NUM_CONST_REGS);
   CHECK_EQ(t, max_const_regs, 256);
   CHECK_EQ(t, has_flow_control, true);
   CHECK_EQ(t, exact_int_bits, 17);
}

static void
check_selection_precedence(void)
{
   /* is_r500 wins when both flags are set, matching the is_r500-first
    * ternaries in r300_fs.c. */
   const struct r300_classic_target *t = r300_classic_target_get(true, true);
   CHECK_EQ(t, pfs_class, R300_CLASSIC_PFS_R500);
}

int
main(void)
{
   check_r300();
   check_r400();
   check_r500();
   check_selection_precedence();
   if (failures) {
      fprintf(stderr, "r300_classic_target_test: %d failures\n", failures);
      return 1;
   }
   printf("r300_classic_target_test: all checks passed\n");
   return 0;
}
