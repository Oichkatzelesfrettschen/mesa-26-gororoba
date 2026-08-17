/*
 * SPDX-License-Identifier: MIT
 */

#ifndef R300_CLASSIC_REGALLOC_H
#define R300_CLASSIC_REGALLOC_H

#include "r300_classic_ir.h"

/* Temp-file fit gate on the SSA program.  Emission gives every SSA def its
 * own RC temporary (the RC optimizer requires SSA-like temp usage) and
 * rc_pair_regalloc owns the hardware register packing, so this linear scan
 * exists to answer one question early: does the program's peak vec4
 * liveness fit the target's flat temp file?  A program it rejects falls
 * back to nir_to_rc cleanly instead of failing deep inside the backend.
 * Each value-producing instruction takes the lowest free slot and a slot
 * returns to the pool at its value's last use; one SSA value per slot, no
 * coalescing and no channel packing, so the estimate is conservative. */

struct r300_classic_regalloc_result {
   /* Temp slot per ssa_id; R300_CLASSIC_NO_TEMP for sinks. */
   unsigned *temp_of_ssa;
   /* High-water slot count actually used. */
   unsigned num_temps;
   /* NULL when allocation fits the target's temp file; the named rejection
    * otherwise (ralloc'd on mem_ctx). */
   const char *reject_reason;
};

#define R300_CLASSIC_NO_TEMP (~0u)

/* Returns false only on out-of-memory; an over-budget program returns true
 * with temp_of_ssa NULL and reject_reason set. */
bool
r300_classic_regalloc(void *mem_ctx, const struct r300_classic_program *p,
                      struct r300_classic_regalloc_result *result);

#endif /* R300_CLASSIC_REGALLOC_H */
