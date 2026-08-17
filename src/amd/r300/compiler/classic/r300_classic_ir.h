/*
 * SPDX-License-Identifier: MIT
 */

#ifndef R300_CLASSIC_IR_H
#define R300_CLASSIC_IR_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "util/list.h"

#include "r300_classic_target.h"

/* The classic-R300 IR: an SSA-formed, hardware-mapped instruction list for
 * the R300 PFS.  Each opcode corresponds one-to-one to an RC_OPCODE the
 * existing rc_program backend consumes, sources reference their defining
 * instruction by pointer (the ir3 def/use discipline rather than virtual
 * register numbers), and the program is a single straight-line list because
 * the non-R500 US has no flow control to model.  Swizzles use RC's own
 * 12-bit encoding (RC_MAKE_SWIZZLE/GET_SWZ, radeon_program_constants.h) so
 * emission into rc_src_register.Swizzle is a copy, not a translation. */

enum r300_classic_op {
   R300C_OP_MOV,
   R300C_OP_ADD,
   R300C_OP_MUL,
   R300C_OP_MAD,
   R300C_OP_DP2,
   R300C_OP_DP3,
   R300C_OP_DP4,
   R300C_OP_MIN,
   R300C_OP_MAX,
   R300C_OP_FRC,
   R300C_OP_ROUND,
   R300C_OP_RCP,
   R300C_OP_RSQ,
   R300C_OP_EX2,
   R300C_OP_LG2,
   R300C_OP_SIN,
   R300C_OP_COS,
   R300C_OP_POW,
   /* No set-compare opcodes: the R300 fragment US has none
    * (radeonTransformALU asserts on SLT/SGE/SEQ/SNE); comparisons arrive
    * pre-lowered to CMP-carried fcsel_ge shapes. */
   R300C_OP_CMP,
   R300C_OP_DDX,
   R300C_OP_DDY,
   /* Channel collect: source s supplies destination channel s, each read
    * through its own descriptor's channel-s select.  num_srcs is the vector
    * width (2-4) and the writemask covers exactly the low num_srcs channels.
    * Emission expands the collect into per-channel-group MOVs; register
    * allocation keeps its destination disjoint from every source because the
    * expansion is a MOV sequence, not one read-all-then-write instruction. */
   R300C_OP_VEC,
   R300C_OP_TEX,
   /* Biased sample: the packed source carries the LOD bias in .w. */
   R300C_OP_TXB,
   /* Projective sample: the TX block divides the coordinate by the
    * packed source's projector lane. */
   R300C_OP_TXP,
   R300C_OP_KIL,
   R300C_OP_KILP,
   R300C_OP_EXPORT_COLOR,
   R300C_OP_EXPORT_DEPTH,

   R300C_OP_COUNT,
};

enum r300_classic_file {
   /* The def of an earlier instruction in the same program. */
   R300C_FILE_SSA,
   /* A hardware fragment input (interpolated varying / texcoord slot).
    * Slot assignment is the front end's AllocateHwInputs contract; the
    * validator bounds only what the descriptor knows. */
   R300C_FILE_INPUT,
   /* A constant-file vec4, bounded by the target's max_const_regs. */
   R300C_FILE_CONST,
   /* A driver-updated RC_CONSTANT_STATE vec4; index addresses the
    * selection's state table and emission remaps it into the constant
    * file. */
   R300C_FILE_STATE,
};

struct r300_classic_instr;

struct r300_classic_src {
   enum r300_classic_file file;
   /* Valid for R300C_FILE_SSA; the defining instruction, which must precede
    * the user in program order. */
   struct r300_classic_instr *def;
   /* Valid for R300C_FILE_INPUT / R300C_FILE_CONST. */
   unsigned index;
   /* RC 12-bit swizzle: 4 x 3-bit selects, RC_SWIZZLE_X..W/ZERO/ONE. */
   unsigned swizzle;
   bool negate;
   bool abs;
};

struct r300_classic_instr {
   struct list_head link;
   enum r300_classic_op op;
   /* Dense SSA id, assigned at append time; the printer's t<id> name. */
   unsigned ssa_id;
   /* Channels this def produces (RC_MASK-style 4-bit mask).  KIL produces no
    * SSA value and keeps writemask 0.  R300C_OP_EXPORT_COLOR repurposes this
    * as the destination write mask instead of an SSA-def mask: the store's
    * nir_intrinsic_write_mask, so two differently-masked stores to the same
    * color attachment accumulate into the shared output register rather than
    * each clobbering the other's channels.  R300C_OP_EXPORT_DEPTH keeps
    * writemask 0; emission always targets the output register's .w lane. */
   uint8_t writemask;
   /* Clamp the result to [0, 1] (RC SaturateMode ZERO_ONE on the dst). */
   bool saturate;
   unsigned num_srcs;
   struct r300_classic_src src[4];
   /* R300C_OP_TEX only. */
   unsigned tex_unit;
   /* R300C_OP_EXPORT_COLOR only: the color attachment (0-3) the export
    * feeds; emission allocates one output register per used attachment
    * and populates the compiler's OutputColor[] the way nir_to_rc's
    * ntr_fs_output_index does. */
   unsigned export_index;
   /* R300C_OP_TEX only: the rc_texture_target the TX block samples
    * (RC_TEXTURE_1D/2D/3D/CUBE/RECT), recorded from the NIR sampler dim the
    * way rc_texture_target_from_sampler_dim maps it. */
   unsigned tex_target;
};

/* The R300 TX block exposes 16 texture units (r300_chipset.c
 * num_tex_units); the descriptor does not vary this across PFS classes. */
#define R300C_MAX_TEX_UNITS 16

/* Immediates a shader can materialize into constant-file slots; bounded by
 * the smallest constant file (R300_PFS_NUM_CONST_REGS). */
#define R300_CLASSIC_MAX_IMMEDIATES 32

struct r300_classic_program {
   const struct r300_classic_target *target;
   /* Straight-line instruction list in execution order. */
   struct list_head instrs;
   unsigned next_ssa_id;
};

struct r300_classic_program *
r300_classic_program_create(void *mem_ctx,
                            const struct r300_classic_target *target);

/* Append a new instruction; the IR owns operand validation at validate time,
 * not at construction, so selection can build then check. */
struct r300_classic_instr *
r300_classic_instr_append(struct r300_classic_program *p,
                          enum r300_classic_op op);

/* Straight-line SSA validation: op arity, def-before-use in program order,
 * writemask presence for value-producing ops and absence for sinks,
 * constant-file bounds from the target descriptor, TEX unit bounds.
 * The first violation is reported through err/err_size and fails the
 * program; a NULL err validates silently. */
bool
r300_classic_program_validate(const struct r300_classic_program *p,
                              char *err, size_t err_size);

void
r300_classic_program_print(const struct r300_classic_program *p, FILE *f);

const char *
r300_classic_op_name(enum r300_classic_op op);

unsigned
r300_classic_op_num_srcs(enum r300_classic_op op);

/* True when op allocates a temp register for its result -- an SSA def a
 * later instruction can read by ssa_id.  False for KIL/KILP (no result) and
 * for R300C_OP_EXPORT_COLOR/R300C_OP_EXPORT_DEPTH, whose writemask instead
 * names the destination output-register channels: an export's ssa_id is
 * never itself a temp register, so regalloc must not reserve a slot for
 * it. */
bool
r300_classic_op_has_def(enum r300_classic_op op);

#endif /* R300_CLASSIC_IR_H */
