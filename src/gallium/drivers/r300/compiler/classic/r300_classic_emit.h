/*
 * Copyright (c) 2026 Terascale Functionalists
 * SPDX-License-Identifier: MIT
 */

#ifndef R300_CLASSIC_EMIT_H
#define R300_CLASSIC_EMIT_H

#include "r300_classic_regalloc.h"
#include "r300_classic_select.h"

struct r300_fragment_program_compiler;

/* Emit the allocated classic program into the fragment compiler's rc_program
 * through the same rc_insert_new_instruction/RC_OPCODE_* surface nir_to_rc
 * uses, so r300_fragprog_emit.c, r500_fragprog_emit.c, and r300_fs.c consume
 * the result unchanged.  Immediates install through
 * rc_constants_add_immediate_vec4 and constant-file references remap to the
 * indices it returns; exports emit as MOVs into RC_FILE_OUTPUT and the
 * compiler's OutputColor/OutputDepth indices are populated the way nir_to_rc
 * populates them.  Returns false when an instruction cannot be expressed;
 * the emitted-so-far program is then invalid and must be discarded. */
bool
r300_classic_emit(const struct r300_classic_program *p,
                  const struct r300_classic_regalloc_result *ra,
                  const struct r300_classic_immediates *imm,
                  struct r300_fragment_program_compiler *fc);

#endif /* R300_CLASSIC_EMIT_H */
