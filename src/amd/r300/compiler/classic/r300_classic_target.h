/*
 * SPDX-License-Identifier: MIT
 */

#ifndef R300_CLASSIC_TARGET_H
#define R300_CLASSIC_TARGET_H

#include <stdbool.h>

/* The R300 PFS machine model as data.  The classic fragment-compiler front
 * end reads its budgets and feature bits from this descriptor instead of
 * scattering radeon_code.h constants and is_r400/is_r500 conditionals through
 * its passes; the descriptor values equal, field by field, the limits
 * r300_fs.c installs into radeon_compiler.Base for the same chip selection,
 * and the unit test diffs them against the radeon_code.h authority. */

enum r300_classic_pfs_class {
   /* R300/RS485M-class unified shader: 64 paired ALU, 32 TEX, 32 vec4 temps,
    * 32 vec4 constants, no flow control (r300_fragprog_emit has no
    * RC_OPCODE_IF or RC_OPCODE_BGNLOOP case). */
   R300_CLASSIC_PFS_R300,
   /* R400 US envelope: the R300 ISA with 512-instruction code memory and the
    * full 64-temp file.  Selected when the chip is R400 or the kernel-gated
    * R400_US route resolves the envelope on an RS48x part. */
   R300_CLASSIC_PFS_R400,
   /* R500 US: 512 unified instructions, 128 temps, 256 constants, and real
    * flow control (r500_fragprog_emit carries the IF/LOOP cases). */
   R300_CLASSIC_PFS_R500,
};

struct r300_classic_target {
   enum r300_classic_pfs_class pfs_class;

   /* Instruction budgets, vec4-paired ALU and TEX counts (radeon_code.h
    * R300_PFS_MAX_ALU_INST / R300_PFS_MAX_TEX_INST families). */
   unsigned max_alu_insts;
   unsigned max_tex_insts;
   /* Dependent-read depth: a TEX whose coordinate comes from a prior TEX
    * result burns one indirection level (R300_PFS_MAX_TEX_INDIRECT). */
   unsigned max_tex_indirections;

   /* Register files, vec4-indexed (R300_PFS_NUM_TEMP_REGS /
    * R300_PFS_NUM_CONST_REGS families).  rc_src_register.Index and
    * rc_dst_register.Index address these directly; there are no register
    * classes and no contiguity constraints. */
   unsigned max_temp_regs;
   unsigned max_const_regs;

   /* Only the R500 emitter lowers RC_OPCODE_IF/BGNLOOP; on every other class
    * a shader with control flow is rejected before selection. */
   bool has_flow_control;

   /* ALU capabilities mirrored from the radeon_compiler.Base bits r300_fs.c
    * sets for every PFS class. */
   bool has_half_swizzles;
   bool has_presub;
   bool has_omod;

   /* FP24 numerics: the fragment ALU carries a 16-bit mantissa plus hidden
    * bit, so consecutive integers are exact through 2^exact_int_bits and the
    * ALU has no integer or dest-relative addressing path.  exact_int_bits is
    * 16 for R300/R400 PFS (FP24); R500 uses FP32 and a larger window. */
   unsigned exact_int_bits;
};

/* Select the descriptor for a resolved chip class.  is_r400 means the R400
 * US envelope is live for code-memory purposes (chip R400, or the RS48x
 * kernel-gated route), matching the meaning radeon_compiler.Base.is_r400
 * carries at fragment-compile time; is_r500 wins when both are set. */
const struct r300_classic_target *
r300_classic_target_get(bool is_r400, bool is_r500);

#endif /* R300_CLASSIC_TARGET_H */
