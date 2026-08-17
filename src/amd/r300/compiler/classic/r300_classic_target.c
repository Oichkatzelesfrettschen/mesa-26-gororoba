/*
 * SPDX-License-Identifier: MIT
 */

#include "r300_classic_target.h"

#include "../radeon_code.h"

/* The R300 and R400 classes run the node-structured r300_fragprog_emit path,
 * which begins a new node per dependent texture read and errors past four
 * (emit_tex "Too many texture indirections" at current_node == 3).  The R500
 * unified instruction stream has no node concept, so its indirection budget
 * is the TEX budget itself. */

static const struct r300_classic_target r300_classic_target_r300 = {
   .pfs_class            = R300_CLASSIC_PFS_R300,
   .max_alu_insts        = R300_PFS_MAX_ALU_INST,
   .max_tex_insts        = R300_PFS_MAX_TEX_INST,
   .max_tex_indirections = R300_PFS_MAX_TEX_INDIRECT,
   .max_temp_regs        = R300_PFS_NUM_TEMP_REGS,
   .max_const_regs       = R300_PFS_NUM_CONST_REGS,
   .has_flow_control     = false,
   .has_half_swizzles    = true,
   .has_presub           = true,
   .has_omod             = true,
   .exact_int_bits       = 17,
};

/* The R400 US envelope widens code memory and doubles the temp file while
 * keeping the R300 ISA, constant file, and node-structured emitter. */
static const struct r300_classic_target r300_classic_target_r400 = {
   .pfs_class            = R300_CLASSIC_PFS_R400,
   .max_alu_insts        = R400_PFS_MAX_ALU_INST,
   .max_tex_insts        = R400_PFS_MAX_TEX_INST,
   .max_tex_indirections = R300_PFS_MAX_TEX_INDIRECT,
   .max_temp_regs        = 2 * R300_PFS_NUM_TEMP_REGS,
   .max_const_regs       = R300_PFS_NUM_CONST_REGS,
   .has_flow_control     = false,
   .has_half_swizzles    = true,
   .has_presub           = true,
   .has_omod             = true,
   .exact_int_bits       = 17,
};

static const struct r300_classic_target r300_classic_target_r500 = {
   .pfs_class            = R300_CLASSIC_PFS_R500,
   .max_alu_insts        = R500_PFS_MAX_INST,
   .max_tex_insts        = R500_PFS_MAX_INST,
   .max_tex_indirections = R500_PFS_MAX_INST,
   .max_temp_regs        = R500_PFS_NUM_TEMP_REGS,
   .max_const_regs       = R500_PFS_NUM_CONST_REGS,
   .has_flow_control     = true,
   .has_half_swizzles    = true,
   .has_presub           = true,
   .has_omod             = true,
   .exact_int_bits       = 17,
};

const struct r300_classic_target *
r300_classic_target_get(bool is_r400, bool is_r500)
{
   if (is_r500)
      return &r300_classic_target_r500;
   if (is_r400)
      return &r300_classic_target_r400;
   return &r300_classic_target_r300;
}
