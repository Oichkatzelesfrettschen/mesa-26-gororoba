/*
 * Copyright (c) 2026 Terascale Functionalists
 * SPDX-License-Identifier: MIT
 */

#ifndef R300_CLASSIC_SELECT_H
#define R300_CLASSIC_SELECT_H

#include "nir.h"
#include "../radeon_code.h"

#include "r300_classic_ir.h"

/* NIR instruction selection for the classic-R300 IR.  The selector owns a
 * narrow entry contract -- a fragment shader, one function, one block (the
 * non-R500 US has no flow control), IO still on variables -- and either
 * covers the whole shader with the admitted opcode subset or rejects it with
 * a named reason.  Rejection is the supported result for everything outside
 * the subset; the selector never silently drops an instruction. */

/* Immediates the selection materialized into constant-file slots.  Slot
 * numbering starts at first_index inside the target's constant file; the
 * emission phase installs the values at those indices (the same contract
 * rc_constants_add_immediate serves for nir_to_rc). */
struct r300_classic_immediates {
   unsigned first_index;
   unsigned count;
   float values[R300_CLASSIC_MAX_IMMEDIATES][4];
};

struct r300_shader_semantics;
struct r300_fragment_program_external_state;

/* Driver-updated state constants the selection materialized (viewport
 * scale/offset for the frag-coord reconstruction); emission adds them as
 * RC_CONSTANT_STATE entries the same way nir_to_rc's state table lands. */
#define R300_CLASSIC_MAX_STATE_CONSTANTS 8
struct r300_classic_state_constants {
   unsigned count;
   struct {
      unsigned rc_state;
      unsigned sampler;
   } entries[R300_CLASSIC_MAX_STATE_CONSTANTS];
};

struct r300_classic_select_result {
   struct r300_classic_program *program;
   struct r300_classic_immediates immediates;
   struct r300_classic_state_constants states;
   /* NULL on success; the named rejection reason otherwise (ralloc'd on
    * mem_ctx). */
   const char *reject_reason;
};

/* Select nir into a classic program.  nir is consumed (lowering passes run
 * on it in place); run it on a clone.  num_driver_consts reserves the front
 * of the constant file for driver-owned constants, so immediates land after
 * them.  semantics, when non-NULL, records each selected input's varying
 * slot at its RC input index -- the r300_shader_semantics contract
 * AllocateHwInputs consumes at pair-regalloc time, recorded the way
 * ntr_read_input_output records it.  Returns false only on out-of-memory;
 * a rejected shader returns true with result->program NULL and
 * reject_reason set. */
bool
r300_classic_select(void *mem_ctx, nir_shader *nir,
                    const struct r300_classic_target *target,
                    const struct r300_fragment_program_external_state *ext,
                    unsigned num_driver_consts,
                    enum r300_fs_input_semantics input_semantics,
                    struct r300_shader_semantics *semantics,
                    struct r300_classic_select_result *result);

#endif /* R300_CLASSIC_SELECT_H */
