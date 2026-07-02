/*
 * Copyright (c) 2026 Terascale Functionalists
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
   R300C_OP_DP3,
   R300C_OP_DP4,
   R300C_OP_MIN,
   R300C_OP_MAX,
   R300C_OP_FRC,
   R300C_OP_RCP,
   R300C_OP_RSQ,
   R300C_OP_TEX,
   R300C_OP_KIL,
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
   /* Channels this def produces (RC_MASK-style 4-bit mask).  Exports and
    * KIL produce no SSA value and keep writemask 0. */
   uint8_t writemask;
   unsigned num_srcs;
   struct r300_classic_src src[3];
   /* R300C_OP_TEX only. */
   unsigned tex_unit;
};

/* The R300 TX block exposes 16 texture units (r300_chipset.c
 * num_tex_units); the descriptor does not vary this across PFS classes. */
#define R300C_MAX_TEX_UNITS 16

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

#endif /* R300_CLASSIC_IR_H */
